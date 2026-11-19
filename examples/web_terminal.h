#ifndef SOFTLINE_WEB_TERMINAL_H
#define SOFTLINE_WEB_TERMINAL_H
#include "softline/www.h"
#include <poll.h>
typedef struct demo_web_terminal demo_web_terminal_t;
typedef int (*demo_web_terminal_write_t)(void *, int binary, const void *,
                                         size_t);
demo_web_terminal_t *demo_web_terminal_create(demo_web_terminal_write_t,
                                              void *);
int demo_web_terminal_message(demo_web_terminal_t *, int binary, const void *,
                              size_t);
/* Service nonblocking PTY I/O. One means EOF, negative means protocol/I/O
 * error. */
int demo_web_terminal_pump(demo_web_terminal_t *);
void demo_web_terminal_descriptor(demo_web_terminal_t *, struct pollfd *);
void demo_web_terminal_close(demo_web_terminal_t *);
/* NOT_READY retains ownership until the host loop has reaped the child. */
int demo_web_terminal_destroy(demo_web_terminal_t *);
#endif
