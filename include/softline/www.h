#ifndef SOFTLINE_WWW_H
#define SOFTLINE_WWW_H

/** @file
 * Framework-independent web assets, action decoding and event encoding.
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
 * Publish response deltas as the producer emits them.
 * @param context Host-owned writer state.
 * @param frame JSON bytes borrowed for this call, without transport framing.
 * @param length Payload size in bytes.
 * @return SL_OK if the host consumed the payload. SL_ERROR_NOT_READY means
 * it consumed none. Retry the event or disconnect and replay. */
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
 * @return Status to return to the host's sl_www_message() call. */
typedef int (*sl_www_action_t)(void *context, const sl_www_request_t *request);
/** Accept one bounded image frame for the peer's active source. The request
 * and data are borrowed for the call.
 * @return Status to return to the host's sl_www_binary() call. */
typedef int (*sl_www_binary_t)(void *context, const sl_www_request_t *request,
                               const void *bytes, size_t length);
/** Register host-owned replay work. The cursor is borrowed for the call.
 * @return Status to return to the host's sl_www_connect() call. */
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
  /** Look up a route-relative asset. @see sl_www_asset */
  int (*asset)(sl_www_t *self, const char *path, sl_www_asset_t *out);
  /** Notify replay on WebSocket open. @see sl_www_connect */
  int (*connect)(sl_www_t *self, void *peer, const char *after);
  /** Deliver an action message. @see sl_www_message */
  int (*message)(sl_www_t *self, void *peer, const void *bytes, size_t length,
                 sl_www_write_t reply, void *reply_context);
  /** Deliver a binary image frame. @see sl_www_binary */
  int (*binary)(sl_www_t *self, void *peer, const void *bytes, size_t length,
                sl_www_write_t reply, void *reply_context);
  /** Free the receiver after host-owned connections/replay have retired. */
  void (*destroy)(sl_www_t *self);
  /** Private implementation; applications must not inspect it. */
  void *impl;
  /** Deliver a simulated prompt to one browser. @see sl_www_simulate_prompt */
  int (*simulate_prompt)(sl_www_t *self, sl_www_write_t write, void *target,
                         const char *text,
                         const sl_prompt_simulation_options_t *options);
};

/** Initialize documented defaults, including enabled=0. NULL is ignored. */
void sl_www_config_init(sl_www_config_t *config);
/** Create a receiver with serialized page settings and borrowed assets.
 * @param config Initialized settings, or NULL for defaults. Custom asset
 * descriptors and the callback context must outlive the receiver.
 * @return Caller-owned receiver, or NULL on invalid input or allocation
 * failure. Release a successful result with sl_www_destroy(). */
sl_www_t *sl_www_create(const sl_www_config_t *config);
/** Free a receiver after the host retires connections and replay work.
 * NULL and calls during the receiver's own callbacks are ignored. */
void sl_www_destroy(sl_www_t *self);
/** Look up the asset at a mounted route's relative path. For HEAD, use the
 * same metadata and omit the body in the host.
 * @param self Enabled receiver.
 * @param path Borrowed route-relative path starting with '/'.
 * @param out Receives the borrowed asset view on success.
 * @return SL_OK or SL_ERROR_INVALID for an unknown or unavailable asset. */
int sl_www_asset(sl_www_t *self, const char *path, sl_www_asset_t *out);
/** Notify the host of a peer and optional last-seen replay cursor.
 * @param self Enabled receiver outside its callbacks.
 * @param peer Host-owned identity for the connection.
 * @param after Last-seen cursor, or NULL to provide an empty cursor.
 * @return Replay callback status, SL_OK with no handler, or SL_ERROR_INVALID
 * for an unavailable receiver or callback reentry. */
int sl_www_connect(sl_www_t *self, void *peer, const char *after);
/** Decode and deliver a bounded URL-encoded action with an action field.
 * @param self Enabled receiver with an action handler, outside its callbacks.
 * @param peer Host-owned identity for the connection.
 * @param bytes Borrowed request body containing unique field names.
 * @param length Body byte count, between 1 and SL_WWW_REQUEST_MAX.
 * @param reply Required writer for transient response events.
 * @param reply_context Borrowed writer context.
 * @return Action status, or an SL_ERROR_* status if decoding or dispatch fails.
 */
int sl_www_message(sl_www_t *self, void *peer, const void *bytes, size_t length,
                   sl_www_write_t reply, void *reply_context);
/** Deliver a binary image chunk of 1 to SL_WWW_CHUNK_MAX bytes. The
 * receiver requires a binary handler and a reply writer. The peer, byte
 * buffer and reply context are borrowed for this call.
 * @return Binary handler status or SL_ERROR_INVALID for an invalid frame,
 * unavailable receiver or callback reentry. */
int sl_www_binary(sl_www_t *self, void *peer, const void *bytes, size_t length,
                  sl_www_write_t reply, void *reply_context);
/** Send one transient prompt simulation to the selected chat connection.
 * The browser applies the timing and submits through its usual send/steer
 * action. Text is borrowed for this call. A failed writer leaves no
 * committed simulation. Leave options NULL for immediate queued submission.
 * Call outside receiver callbacks, on the receiver's owner thread. */
int sl_www_simulate_prompt(sl_www_t *self, sl_www_write_t write, void *target,
                           const char *text,
                           const sl_prompt_simulation_options_t *options);
/** Opt in to best-effort simulation_status actions on the same connection.
 * Use a fresh id of 1 to 64 ASCII letters, digits, underscores or hyphens for
 * each simulation on a peer. SL_OK means the writer consumed the frames. The
 * normal action callback receives any browser reports. Match them by peer
 * and id. Call outside receiver callbacks, on the receiver's owner thread. */
int sl_www_simulate_prompt_tracked(
    sl_www_t *self, sl_www_write_t write, void *target, const char *id,
    const char *text, const sl_prompt_simulation_options_t *options);
/** Request cancellation on the same peer and id as a tracked simulation.
 * The browser stops a matching pending or active job and keeps inserted text.
 * SL_OK means the writer consumed the request. Call on the receiver's owner
 * thread, outside receiver callbacks. */
int sl_www_cancel_prompt_simulation(sl_www_t *self, sl_www_write_t write,
                                    void *target, const char *id);
/** Encode one bounded browser event and call the writer once on success.
 * The host owns event order and replay. Open a feed message with begin (id,
 * role, optional format, copy, label, tone and timestamp), stream delta events
 * with that id, and finish with end. Supply label and timestamp as display
 * text for assistant or note messages. Start a new message id when output
 * resumes after an intervening feed entry. See docs/web-frontend.md for the
 * event fields and their defaults.
 * @param write Required writer for a JSON payload without transport framing.
 * @param context Borrowed writer context.
 * @param type Event name. It and field names use 1 to 64 ASCII letters,
 * digits, underscores or hyphens. Field names must be unique and must exclude
 * the reserved names type and seq.
 * @param seq Optional host replay cursor of at most 64 UTF-8 bytes. NULL
 * omits the field.
 * @param fields Borrowed field array with up to SL_WWW_CHUNK_MAX bytes of
 * UTF-8 values in total, before JSON escaping.
 * @param count Field count from zero to SL_WWW_FIELDS_MAX. Zero allows a NULL
 * fields pointer.
 * @return Writer status, SL_ERROR_INVALID for invalid input, SL_ERROR_NOMEM
 * on allocation failure, or SL_ERROR_FULL if encoding exceeds capacity.
 * @see sl_www_write_t */
int sl_www_emit(sl_www_write_t write, void *context, const char *type,
                const char *seq, const sl_www_field_t *fields, size_t count);
/** Look up a borrowed decoded field; NULL means absent. */
const char *sl_www_field(const sl_www_request_t *request, const char *name);

#ifdef __cplusplus
}
#endif
#endif
