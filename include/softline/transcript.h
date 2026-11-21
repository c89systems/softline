#ifndef SOFTLINE_TRANSCRIPT_H
#define SOFTLINE_TRANSCRIPT_H

#include "softline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long sl_transcript_id_t;

typedef enum sl_transcript_format {
  /** UTF-8 text. LF starts a new line. Terminal controls are ignored. */
  SL_TRANSCRIPT_TEXT = 0,
  /** Terminal output bytes, using the source PTY's rows and columns. */
  SL_TRANSCRIPT_VT = 1
} sl_transcript_format_t;

/** Register a read-only viewer in Ctrl+T cycle order. The editor owns the
 * bounded screen and scrollback until removal. Hosts own producers and replay.
 * All calls run on the editor's owner thread. */
int sl_transcript_add(sl_t *, const char *title, int rows, int columns,
                      sl_transcript_id_t *id);
/** Consume one chunk incrementally, including split UTF-8 and VT controls.
 * Switch formats at complete UTF-8 and VT boundaries. No input or replies
 * are sent to the producer. */
int sl_transcript_write(sl_t *, sl_transcript_id_t, sl_transcript_format_t,
                        const void *, size_t);
/** Apply a producer resize. The viewer viewport never resizes the producer. */
int sl_transcript_resize(sl_t *, sl_transcript_id_t, int rows, int columns);
/** Clear a viewer before feeding a replacement source or replay. */
int sl_transcript_reset(sl_t *, sl_transcript_id_t);
/** Remove a viewer, returning to the editor if it was visible. */
int sl_transcript_remove(sl_t *, sl_transcript_id_t);
/** Nonzero while a transcript occupies the native terminal. Chat output
 * continues into a bounded backlog and is replayed when chat becomes visible.
 */
int sl_transcript_visible(const sl_t *);

#ifdef __cplusplus
}
#endif
#endif
