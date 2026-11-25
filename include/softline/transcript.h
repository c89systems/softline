#ifndef SOFTLINE_TRANSCRIPT_H
#define SOFTLINE_TRANSCRIPT_H

/** @file
 * Read-only terminal views for host-owned logs and PTY output.
 * Call these functions on the editor's owner thread, including from watch
 * and idle callbacks. The host owns the output source and its lifetime. */

#include "softline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register a viewer in Ctrl+T cycle order. The editor owns its bounded
 * screen and scrollback. The host owns the producer and any replay.
 * @param self Native editor.
 * @param title Required non-NULL label, borrowed for this call. The native
 * header displays the viewer's position in the Ctrl+T cycle.
 * @param rows Source screen height, from 1 to 200.
 * @param columns Source screen width, from 1 to 320.
 * @param id Required output for the new identity on success.
 * @return SL_OK on success, SL_ERROR_INVALID for invalid arguments,
 * SL_ERROR_FULL when all eight slots or identities are exhausted, or
 * SL_ERROR_NOMEM on allocation failure. */
int sl_transcript_add(sl_t *self, const char *title, int rows, int columns,
                      sl_transcript_id_t *id);
/** Consume one chunk incrementally, including split UTF-8 and VT controls.
 * Switch formats at complete UTF-8 and VT boundaries. The viewer consumes
 * output only. Appending output preserves an active search or scroll position
 * as far as the retained rows allow.
 * @param self Native editor.
 * @param id Registered viewer identity.
 * @param format Plain text or raw terminal output.
 * @param bytes Producer bytes borrowed for this call.
 * @param length Byte count. Zero permits a NULL bytes pointer.
 * @return SL_OK on success, SL_ERROR_INVALID for invalid arguments or an
 * unknown viewer, SL_ERROR_NOT_READY if the viewer needs reset, or
 * SL_ERROR_NOMEM if processing failed and the viewer needs reset. */
int sl_transcript_write(sl_t *self, sl_transcript_id_t id,
                        sl_transcript_format_t format, const void *bytes,
                        size_t length);
/** Apply producer geometry independently of the viewer viewport.
 * @param self Native editor.
 * @param id Registered viewer identity.
 * @param rows Source screen height, from 1 to 200.
 * @param columns Source screen width, from 1 to 320.
 * @return SL_OK on success, SL_ERROR_INVALID for invalid arguments or an
 * unknown viewer, SL_ERROR_NOT_READY if the viewer needs reset, or
 * SL_ERROR_NOMEM if resizing failed and the viewer needs reset. */
int sl_transcript_resize(sl_t *self, sl_transcript_id_t id, int rows,
                         int columns);
/** Clear the screen, parser, scrollback and search before replacing or
 * replaying a source. Resume following output at the current source geometry.
 * @param self Native editor.
 * @param id Registered viewer identity to preserve across the reset.
 * @return SL_OK on success, SL_ERROR_INVALID for an unknown viewer, or
 * SL_ERROR_NOMEM if the viewer could not be reinitialized. */
int sl_transcript_reset(sl_t *self, sl_transcript_id_t id);
/** Remove a viewer. If it was visible, restore chat and replay its backlog.
 * @param self Native editor.
 * @param id Registered viewer identity.
 * @return SL_OK on success, SL_ERROR_INVALID for an unknown viewer, or
 * SL_ERROR_IO if restoring chat or replaying its output failed. */
int sl_transcript_remove(sl_t *self, sl_transcript_id_t id);
/** Report whether a transcript viewer is active.
 * Chat output continues into a bounded backlog for display on return.
 * @param self Native editor, or NULL.
 * @return Nonzero when a viewer is active and zero otherwise. */
int sl_transcript_visible(const sl_t *self);

#ifdef __cplusplus
}
#endif
#endif
