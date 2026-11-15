#ifndef SOFTLINE_WWW_H
#define SOFTLINE_WWW_H

#include "softline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The host owns HTTP, WebSocket connections, authentication, the event loop,
 * conversation history and replay. These operations never open sockets or
 * start threads. All callbacks run synchronously on the caller's thread. */
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
/** Immutable HTTP asset view, borrowed until receiver destruction. */
typedef struct sl_www_asset {
  /** Route-relative path beginning with '/'. */
  const char *path;
  /** HTTP media type; the host sends the response headers. */
  const char *content_type;
  /** Immutable body bytes, also used as HEAD response metadata. */
  const unsigned char *data;
  /** Body byte count. */
  size_t length;
} sl_www_asset_t;
/** One complete, bounded UTF-8 JSON WebSocket text frame. SL_OK consumes the
 * frame. NOT_READY consumes none; the host must retry or disconnect/replay.
 * The bytes are borrowed for this call only. A frame is not a whole response:
 * publish response deltas as the producer emits them. */
typedef int (*sl_www_write_t)(void *, const void *, size_t);
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
/** Handle a decoded browser action on the caller's thread. */
typedef int (*sl_www_action_t)(void *, const sl_www_request_t *);
/** Handle a bounded binary frame for the peer's active image source. */
typedef int (*sl_www_binary_t)(void *, const sl_www_request_t *, const void *,
                               size_t);
/** Register host-owned replay work after an optional sequence cursor. */
typedef int (*sl_www_replay_t)(void *, void *peer, const char *after);
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
  /** Optional borrowed asset descriptors and bytes. */
  const sl_www_asset_t *assets;
  /** Number of custom assets. */
  size_t asset_count;
  /** Presentation only; default 500, zero immediate. */
  int queue_display_delay_ms;
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
  /** Mount under any prefix. path is relative to that route, starting at '/'.
   * Handle HTTP HEAD with the same metadata and omit the body in the host. */
  int (*asset)(sl_www_t *, const char *path, sl_www_asset_t *);
  /** Invoke on WebSocket open; replay only registers host-owned replay work. */
  int (*connect)(sl_www_t *, void *peer, const char *after);
  /** Decode a URL-encoded action into bounded callback-scoped fields. */
  int (*message)(sl_www_t *, void *peer, const void *, size_t, sl_www_write_t,
                 void *reply_context);
  /** Forward at most SL_WWW_CHUNK_MAX image bytes to the host. */
  int (*binary)(sl_www_t *, void *peer, const void *, size_t, sl_www_write_t,
                void *reply_context);
  /** Free the receiver after host-owned connections/replay have retired. */
  void (*destroy)(sl_www_t *);
  /** Private implementation; applications must not inspect it. */
  void *impl;
};

/** Set documented defaults; enabled remains zero. */
void sl_www_config_init(sl_www_config_t *);
/** Configuration strings are copied; custom asset bytes and context are
 * borrowed until destroy. No terminal editor or libmdf dependency is needed. */
sl_www_t *sl_www_create(const sl_www_config_t *);
/** Free a receiver; ignored during its own callback. NULL is harmless. */
void sl_www_destroy(sl_www_t *);
/** Look up an immutable route-relative asset or return SL_ERROR_INVALID. */
int sl_www_asset(sl_www_t *, const char *, sl_www_asset_t *);
/** Notify the host of a peer and optional last-seen replay cursor. */
int sl_www_connect(sl_www_t *, void *, const char *after);
/** Decode and deliver a bounded UTF-8 action; callback reentry is rejected. */
int sl_www_message(sl_www_t *, void *, const void *, size_t, sl_www_write_t,
                   void *);
/** Deliver one bounded binary image chunk; callback reentry is rejected. */
int sl_www_binary(sl_www_t *, void *, const void *, size_t, sl_www_write_t,
                  void *);
/** Encode one bounded event and call write once. The output is JSON, without
 * HTTP/WebSocket framing. Field names are ASCII identifiers. type and seq are
 * reserved. seq may be NULL for transient replies. See docs/web-frontend.md. */
int sl_www_emit(sl_www_write_t, void *, const char *type, const char *seq,
                const sl_www_field_t *, size_t);
/** Look up a borrowed decoded field; NULL means absent. */
const char *sl_www_field(const sl_www_request_t *, const char *name);

#ifdef __cplusplus
}
#endif
#endif
