#ifndef SOFTLINE_WWW_H
#define SOFTLINE_WWW_H

/** Framework-independent web assets, action decoding and event encoding.
 * The host owns HTTP/WebSocket transport, authorization, conversation history
 * and replay. Callbacks run synchronously on the receiver's owner thread. */

#include "softline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum binary chunk size and aggregate value bytes per emitted event. */
#define SL_WWW_CHUNK_MAX 8192u
/** Maximum encoded event size, including JSON escaping. */
#define SL_WWW_FRAME_MAX 65536u
/** Maximum URL-encoded action request size. */
#define SL_WWW_REQUEST_MAX (3u * 128u * 1024u + 4096u)
/** Maximum action/event fields per call. */
#define SL_WWW_FIELDS_MAX 16u

/** Framework-independent web receiver; no sockets, workers or history. */
typedef struct sl_www sl_www_t;
/** Borrowed UTF-8 string field. */
typedef struct sl_www_field {
  /** ASCII identifier; type and seq are reserved for emitted events. */
  const char *name;
  /** Value bytes; NULL is an empty string. */
  const char *value;
  /** Byte length; zero selects strlen(value). */
  size_t length;
} sl_www_field_t;
/** Borrowed view of an immutable HTTP response body and its metadata. */
typedef struct sl_www_asset {
  /** Route-relative path beginning with '/'; may borrow the lookup path. */
  const char *path;
  /** HTTP media type; the host sends the response headers. */
  const char *content_type;
  /** Immutable body bytes, also used as HEAD response metadata. */
  const unsigned char *data;
  /** Body byte count. */
  size_t length;
} sl_www_asset_t;
/** Consume one bounded UTF-8 JSON payload for a WebSocket text frame.
 *
 * `context` is host-owned. `frame` and `length` describe borrowed JSON
 * bytes without transport framing. Publish deltas as the producer emits them.
 * Return `SL_OK` after consuming the payload. `SL_ERROR_NOT_READY` means
 * none was consumed. Retry the event or disconnect and replay. */
typedef int (*sl_www_write_t)(void *context, const void *frame, size_t length);
/** One callback-scoped action or binary request. */
typedef struct sl_www_request {
  /** Decoded fields; empty for binary callbacks. */
  const sl_www_field_t *fields;
  /** Number of decoded fields. */
  size_t count;
  /** Host-owned connection identity; never supplied by the browser. */
  void *peer;
  /** Writer for a transient bounded reply. */
  sl_www_write_t reply;
  /** Borrowed writer context. */
  void *reply_context;
} sl_www_request_t;
/** Handle an action using fields and a reply writer borrowed for the call.
 * The result becomes the status of the host's `sl_www_message()` call. */
typedef int (*sl_www_action_t)(void *context, const sl_www_request_t *request);
/** Accept one bounded image frame for the peer's active source. The request
 * and data are borrowed for the call.
 * The result becomes the status of the host's `sl_www_binary()` call. */
typedef int (*sl_www_binary_t)(void *context, const sl_www_request_t *request,
                               const void *bytes, size_t length);
/** Register host-owned replay work. The cursor is borrowed for the call.
 * The result becomes the status of the host's `sl_www_connect()` call. */
typedef int (*sl_www_replay_t)(void *context, void *peer, const char *after);
/** Initialize with sl_www_config_init() before overriding fields. */
typedef struct sl_www_config {
  /** Nonzero enables the handler; default zero. */
  int enabled;
  /** Browser document title; default softline. */
  const char *title;
  /** Welcome Markdown; default Welcome to softline. */
  const char *welcome;
  /** Trusted host HTML/SVG only; model text is never HTML. */
  const char *banner;
  /** Optional trusted host HTML/SVG in the upper-left toolbar; default empty.
   */
  const char *header;
  /** Optional asset descriptors, strings and bytes borrowed until destroy. */
  const sl_www_asset_t *assets;
  /** Number of custom assets. */
  size_t asset_count;
  /** Presentation only; default 500, zero immediate. */
  int queue_display_delay_ms;
  /** Nonzero serves the animated chat page; default zero. */
  int animations;
  /** Client hint, default 32 MiB; the host image source enforces the limit. */
  size_t image_max_bytes;
  /** Optional action handler. */
  sl_www_action_t action;
  /** Optional image chunk handler; NULL disables browser image selection. */
  sl_www_binary_t binary;
  /** Optional host-owned history/replay handler. */
  sl_www_replay_t replay;
  /** Borrowed callback context. */
  void *context;
} sl_www_config_t;

/** Public receiver shell; methods match the free functions below. */
struct sl_www {
  /** Look up an asset by route-relative `path` starting with '/'. On
   * `SL_OK`, `out` receives the borrowed view. */
  int (*asset)(sl_www_t *self, const char *path, sl_www_asset_t *out);
  /** Notify replay of a WebSocket open for `peer`. Pass the last-seen
   * cursor in `after`, or null for an empty cursor. */
  int (*connect)(sl_www_t *self, void *peer, const char *after);
  /** Decode a bounded URL-encoded action for `peer`. Requires a reply
   * writer for transient events. Returns the action or decoding status. */
  int (*message)(sl_www_t *self, void *peer, const void *bytes, size_t length,
                 sl_www_write_t reply, void *reply_context);
  /** Deliver 1 to `SL_WWW_CHUNK_MAX` image bytes to the binary callback
   * for `peer`. Requires a reply writer. */
  int (*binary)(sl_www_t *self, void *peer, const void *bytes, size_t length,
                sl_www_write_t reply, void *reply_context);
  /** Free the receiver after host-owned connections/replay have retired. */
  void (*destroy)(sl_www_t *self);
  /** Private implementation; applications must not inspect it. */
  void *impl;
  /** Send a transient simulated prompt to the selected connection's writer.
   * Null `options` requests immediate queued submission. */
  int (*simulate_prompt)(sl_www_t *self, sl_www_write_t write, void *target,
                         const char *text,
                         const sl_prompt_simulation_options_t *options);
  /** Send a simulation with a fresh peer-local `id`. The writer's success
   * confirms frame consumption. Browser reports arrive as separate actions. */
  int (*simulate_prompt_tracked)(sl_www_t *self, sl_www_write_t write,
                                 void *target, const char *id, const char *text,
                                 const sl_prompt_simulation_options_t *options);
  /** Ask the original connection to cancel the tracked simulation with
   * this `id` and keep any inserted text. */
  int (*cancel_prompt_simulation)(sl_www_t *self, sl_www_write_t write,
                                  void *target, const char *id);
};

/** Initialize documented defaults, including enabled=0. NULL is ignored. */
void sl_www_config_init(sl_www_config_t *config);
/** Create a receiver with serialized page settings and borrowed assets.
 *
 * Pass an initialized `config` or null for defaults. Custom asset
 * descriptors and the callback context must outlive the receiver.
 * Returns a caller-owned receiver or null on invalid input or allocation
 * failure. Release it with `sl_www_destroy()`. */
sl_www_t *sl_www_create(const sl_www_config_t *config);
/** Free a receiver after the host retires connections and replay work.
 * NULL and calls during the receiver's own callbacks are ignored. */
void sl_www_destroy(sl_www_t *self);
/** Look up an asset on an enabled receiver. `path` starts with '/' and is
 * relative to the mount. The host uses the same metadata for HEAD and omits
 * the body. Returns `SL_OK` with a borrowed view in `out`, or
 * `SL_ERROR_INVALID` for an unknown or unavailable asset. */
int sl_www_asset(sl_www_t *self, const char *path, sl_www_asset_t *out);
/** Notify replay that the host-owned connection `peer` opened.
 *
 * Call on an enabled receiver outside its callbacks. `after` is the
 * last-seen cursor, or null for an empty cursor. Returns the replay callback
 * status, `SL_OK` with no handler, or `SL_ERROR_INVALID` for an unavailable
 * receiver or callback reentry. */
int sl_www_connect(sl_www_t *self, void *peer, const char *after);
/** Decode a bounded URL-encoded request and dispatch its action.
 *
 * Call outside callbacks with an enabled receiver that has an action
 * handler. `peer` is the host-owned connection identity. The borrowed
 * body `bytes` must contain unique fields, including `action`, with
 * `length` between 1 and `SL_WWW_REQUEST_MAX`.
 *
 * `reply` is the required writer for transient responses, with borrowed
 * `reply_context`. Returns the action status or a decoding/dispatch error. */
int sl_www_message(sl_www_t *self, void *peer, const void *bytes, size_t length,
                   sl_www_write_t reply, void *reply_context);
/** Deliver a binary image chunk of 1 to `SL_WWW_CHUNK_MAX` bytes. The
 * receiver requires a binary handler and a reply writer. The peer, byte
 * buffer and reply context are borrowed for this call.
 * Returns the handler status or `SL_ERROR_INVALID` for an invalid frame,
 * unavailable receiver or callback reentry. */
int sl_www_binary(sl_www_t *self, void *peer, const void *bytes, size_t length,
                  sl_www_write_t reply, void *reply_context);
/** Send a transient prompt simulation to a live chat connection.
 *
 * Call on the receiver's owner thread outside callbacks. `write` and
 * `target` select the connection. Borrowed `text` is a nonblank UTF-8
 * prompt of up to 128 KiB. Null `options` requests immediate queued
 * submission.
 *
 * Returns `SL_OK` after the writer consumes every frame. Writer errors
 * propagate. The browser starts only after the final frame, and can decline
 * if the draft already has content. */
int sl_www_simulate_prompt(sl_www_t *self, sl_www_write_t write, void *target,
                           const char *text,
                           const sl_prompt_simulation_options_t *options);
/** Send a prompt simulation with advisory `simulation_status` actions.
 *
 * Use the same arguments and result contract as `sl_www_simulate_prompt()`,
 * plus a fresh `id` per simulation on the peer. IDs contain 1–64 ASCII
 * letters, digits, underscores or hyphens. Match reports from the normal
 * action callback by peer and ID. */
int sl_www_simulate_prompt_tracked(
    sl_www_t *self, sl_www_write_t write, void *target, const char *id,
    const char *text, const sl_prompt_simulation_options_t *options);
/** Cancel a tracked simulation using the original writer and `id`.
 *
 * Call on the receiver's owner thread outside callbacks. The browser stops
 * a matching pending or active job and keeps inserted text. `SL_OK` means
 * the writer consumed the request. */
int sl_www_cancel_prompt_simulation(sl_www_t *self, sl_www_write_t write,
                                    void *target, const char *id);
/** Encode one bounded browser event and call the writer once on success.
 * The host owns event order and replay. Open a feed message with begin (id,
 * role, optional format, copy, label, tone and timestamp), stream delta events
 * with that id, and finish with end. Supply label and timestamp as display
 * text for assistant or note messages. Start a new message id when output
 * resumes after an intervening feed entry. See docs/web-frontend.md for the
 * event fields and their defaults. See docs/transcripts.md for the
 * transcript_open, transcript_data, transcript_resize, transcript_reset and
 * transcript_remove events. The host announces each view before sending data.
 *
 * - `write` receives JSON without framing, with borrowed `context`.
 * - `type` and field names use 1–64 ASCII letters, digits, underscores
 *   or hyphens. Field names are unique and exclude `type` and `seq`.
 * - `seq` is an optional replay cursor of up to 64 UTF-8 bytes.
 * - `fields` is borrowed, with up to `SL_WWW_CHUNK_MAX` value bytes
 *   before escaping. `count` is at most `SL_WWW_FIELDS_MAX`.
 *   A zero count permits a null field array.
 *
 * Returns the writer status, `SL_ERROR_INVALID` for invalid input,
 * `SL_ERROR_NOMEM` on allocation failure, or `SL_ERROR_FULL` if encoding
 * exceeds capacity. */
int sl_www_emit(sl_www_write_t write, void *context, const char *type,
                const char *seq, const sl_www_field_t *fields, size_t count);
/** Look up a borrowed decoded field; NULL means absent. */
const char *sl_www_field(const sl_www_request_t *request, const char *name);

#ifdef __cplusplus
}
#endif
#endif
