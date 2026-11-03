#ifndef SOFTLINE_PTY_H
#define SOFTLINE_PTY_H
#include "softline/softline.h"
#include <sys/types.h>
typedef struct sl_pty {
  int fd, status, write_blocked, exec_fd, ready, exec_failure;
  double eof_deadline;
  size_t drain_remaining;
  pid_t pid;
  const char *shell;
  struct sl_pty *next;
} sl_pty_t;
int sl_pty_callback(void *context, sl_terminal_event_t *event);
int sl_pty_reap(sl_pty_t *pty);
#endif
