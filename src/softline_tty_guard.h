#ifndef SOFTLINE_TTY_GUARD_H
#define SOFTLINE_TTY_GUARD_H
#include <termios.h>
typedef struct sl_tty_guard sl_tty_guard_t;
sl_tty_guard_t *sl_tty_guard_begin(int, int, const sl_tty_guard_t *);
int sl_tty_guard_restore_modes(sl_tty_guard_t *);
int sl_tty_guard_end(sl_tty_guard_t *, int restore_modes, int restore_flags);
int sl_tty_setattr(int, int, const struct termios *);
#endif
