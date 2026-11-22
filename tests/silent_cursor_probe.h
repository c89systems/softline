#ifndef SOFTLINE_TEST_SILENT_CURSOR_PROBE_H
#define SOFTLINE_TEST_SILENT_CURSOR_PROBE_H

#ifdef SOFTLINE_TEST_SILENT_CURSOR_PROBE
#include <sys/select.h>
#include <unistd.h>

static int cursor_probe_fd = -1;
static int real_cursor_replies;

ssize_t __real_write(int, const void *, size_t);
int __real_select(int, fd_set *, fd_set *, fd_set *, struct timeval *);

ssize_t __wrap_write(int fd, const void *bytes, size_t length) {
  const char *text = bytes;
  ssize_t result = __real_write(fd, bytes, length);
  if (!real_cursor_replies && result == 4 && length == 4 && text[0] == '\033' &&
      text[1] == '[' && text[2] == '6' && text[3] == 'n')
    cursor_probe_fd = fd;
  return result;
}

int __wrap_select(int count, fd_set *reads, fd_set *writes, fd_set *errors,
                  struct timeval *timeout) {
  if (cursor_probe_fd >= 0 && count == cursor_probe_fd + 1 && reads &&
      FD_ISSET(cursor_probe_fd, reads) && !writes && !errors && timeout &&
      timeout->tv_sec == 0 && timeout->tv_usec > 0 &&
      timeout->tv_usec <= 100000) {
    struct timeval ready_now = {0, 0};
    /* These fixtures have no emulator to answer a cursor probe. Exercise the
     * real no-reply fallback without waiting for each silent terminal. */
    cursor_probe_fd = -1;
    return __real_select(count, reads, writes, errors, &ready_now);
  }
  return __real_select(count, reads, writes, errors, timeout);
}

#define CURSOR_REPLIES_REAL(enabled)                                           \
  do {                                                                         \
    real_cursor_replies = (enabled);                                           \
    cursor_probe_fd = -1;                                                      \
  } while (0)
#else
#define CURSOR_REPLIES_REAL(enabled) ((void)(enabled))
#endif
#endif
