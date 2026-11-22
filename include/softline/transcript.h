#ifndef SOFTLINE_TRANSCRIPT_H
#define SOFTLINE_TRANSCRIPT_H

/** @file
 * Read-only terminal views for host-owned logs and PTY output.
 * Drive these operations on the editor's owner thread. */

#include "softline.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Nonzero handle-local identity assigned at registration. */
typedef unsigned long sl_transcript_id_t;

/** Encoding of a producer output chunk. */
typedef enum sl_transcript_format {
  /** UTF-8 text. LF starts a new line and Tab advances to a tab stop. */
  SL_TRANSCRIPT_TEXT = 0,
  /** Terminal output bytes, using the source PTY's rows and columns. */
  SL_TRANSCRIPT_VT = 1
} sl_transcript_format_t;

/** Register a viewer in Ctrl+T cycle order. The editor owns its bounded
 * screen and scrollback. The host owns the producer and any replay.
 * @param self Native editor.
 * @param title Copied display label. Nonprintable or non-ASCII bytes are
 * replaced by question marks, and long labels are truncated.
 * @param rows Source screen height, from 1 to 200.
 * @param columns Source screen width, from 1 to 320.
 * @param id Required output for the new identity on success.
 * @return SL_OK, SL_ERROR_FULL if all eight slots are occupied or identities
 * are exhausted, or an SL_ERROR_* status. */
int sl_transcript_add(sl_t *self, const char *title, int rows, int columns,
                      sl_transcript_id_t *id);
/** Consume one chunk incrementally, including split UTF-8 and VT controls.
 * Switch formats at complete UTF-8 and VT boundaries. The viewer consumes
 * output only.
 * @param self Native editor.
 * @param id Registered viewer identity.
 * @param format Plain text or raw terminal output.
 * @param bytes Producer bytes borrowed for this call.
 * @param length Byte count. Zero permits a NULL bytes pointer.
 * @return SL_OK, SL_ERROR_NOT_READY if a failed viewer needs reset, or an
 * SL_ERROR_* status. */
int sl_transcript_write(sl_t *self, sl_transcript_id_t id,
                        sl_transcript_format_t format, const void *bytes,
                        size_t length);
/** Apply source geometry using the same bounds as sl_transcript_add().
 * The host drives producer resizing independently of the viewer viewport.
 * @return SL_OK or an SL_ERROR_* status. */
int sl_transcript_resize(sl_t *self, sl_transcript_id_t id, int rows,
                         int columns);
/** Clear the screen, parser and scrollback before replacing a source or
 * replaying it. The viewer keeps its identity, title and source dimensions.
 * @return SL_OK or an SL_ERROR_* status. */
int sl_transcript_reset(sl_t *self, sl_transcript_id_t id);
/** Remove a viewer. If it was visible, restore chat and replay its backlog.
 * @return SL_OK, SL_ERROR_INVALID for an unknown viewer, or SL_ERROR_IO
 * when the terminal could not return to chat. */
int sl_transcript_remove(sl_t *self, sl_transcript_id_t id);
/** Return nonzero while a transcript occupies the native terminal.
 * Chat output continues into a bounded backlog for display on return. */
int sl_transcript_visible(const sl_t *self);

#ifdef __cplusplus
}
#endif
#endif
