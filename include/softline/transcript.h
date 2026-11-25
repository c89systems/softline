#ifndef SOFTLINE_TRANSCRIPT_H
#define SOFTLINE_TRANSCRIPT_H

/** Read-only terminal views for host-owned logs and PTY output.
 *
 * The free functions match the transcript methods on `sl_t`. Call either
 * form on the editor's owner thread, including from watch and idle callbacks.
 * The host owns the output source and its lifetime. */

#include "softline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register a viewer in Ctrl+T cycle order.
 *
 * `title` is a required label borrowed for the call. The native header shows
 * the position in the cycle. `rows` accepts 1–200 and `columns` accepts 1–320.
 * The editor owns the bounded screen and scrollback.
 *
 * Returns `SL_OK` with the new ID in `*id`. Invalid arguments return
 * `SL_ERROR_INVALID`. Exhausting the eight slots or all IDs returns
 * `SL_ERROR_FULL`. Allocation failure returns `SL_ERROR_NOMEM`. */
int sl_transcript_add(sl_t *self, const char *title, int rows, int columns,
                      sl_transcript_id_t *id);
/** Feed borrowed `bytes` to viewer `id` as `SL_TRANSCRIPT_TEXT` or
 * `SL_TRANSCRIPT_VT`. A zero `length` permits a null pointer.
 *
 * Change formats at complete UTF-8 and VT boundaries. New output preserves
 * the active search or scroll position as far as retained rows allow.
 *
 * Returns `SL_OK` on success, `SL_ERROR_INVALID` for invalid arguments or
 * an unknown viewer, `SL_ERROR_NOT_READY` if the viewer needs reset, or
 * `SL_ERROR_NOMEM` if processing failed and the viewer needs reset. */
int sl_transcript_write(sl_t *self, sl_transcript_id_t id,
                        sl_transcript_format_t format, const void *bytes,
                        size_t length);
/** Update the source grid for viewer `id`. The viewport is sized separately.
 *
 * `rows` accepts 1–200 and `columns` accepts 1–320. Returns `SL_OK` on
 * success, `SL_ERROR_INVALID` for invalid arguments or an unknown viewer,
 * `SL_ERROR_NOT_READY` if the viewer needs reset, or `SL_ERROR_NOMEM` if
 * resizing failed and the viewer needs reset. */
int sl_transcript_resize(sl_t *self, sl_transcript_id_t id, int rows,
                         int columns);
/** Clear the screen, parser, scrollback and search for `id`. Resume following
 * output at the current source geometry, keeping the same ID.
 *
 * Returns `SL_OK` on success, `SL_ERROR_INVALID` for an unknown viewer, or
 * `SL_ERROR_NOMEM` if the viewer could not be reinitialized. */
int sl_transcript_reset(sl_t *self, sl_transcript_id_t id);
/** Remove viewer `id`. If it was visible, restore chat and display deferred
 * chat output. Returns `SL_OK` on success, `SL_ERROR_INVALID` for an unknown
 * viewer, or `SL_ERROR_IO` if restoring chat or displaying its output failed.
 */
int sl_transcript_remove(sl_t *self, sl_transcript_id_t id);
/** Return nonzero when a read-only viewer is active. Chat output goes to a
 * bounded backlog while the viewer is open. A null `self` returns zero. */
int sl_transcript_visible(const sl_t *self);

#ifdef __cplusplus
}
#endif
#endif
