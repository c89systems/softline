#ifndef SOFTLINE_CLIPBOARD_H
#define SOFTLINE_CLIPBOARD_H

#include "softline/softline.h"
#include <poll.h>

typedef struct sl_clipboard sl_clipboard_t;
/* next returns 1 for BEGIN/DATA/END, 0 for pending, -1 for failure.
 * DATA remains borrowed until next is called again. No operation waits. */
sl_clipboard_t *sl_clipboard_open(size_t limit);
int sl_clipboard_next(sl_clipboard_t *, sl_image_paste_event_t *);
int sl_clipboard_prepare(sl_clipboard_t *, struct pollfd *, size_t, int *);
const char *sl_clipboard_error_text(const sl_clipboard_t *);
void sl_clipboard_close(sl_clipboard_t *);
int sl_clipboard_save_image(const char *, char **, char *, size_t);
void sl_clipboard_discard_image(const char *, const char *);
int sl_clipboard_valid_path_template(const char *);

#endif
