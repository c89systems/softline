#ifndef SOFTLINE_TEST_TERMIOS_SNAPSHOT_H
#define SOFTLINE_TEST_TERMIOS_SNAPSHOT_H

#include <fcntl.h>
#include <stddef.h>
#include <termios.h>

#if defined(__APPLE__)
/* XNU also reports its internal "has been written" bit in F_GETFL.
 * Compare the access mode and all flags that F_SETFL can restore. */
#define TEST_TTY_FILE_FLAGS                                                    \
  (O_ACCMODE | O_APPEND | O_ASYNC | O_SYNC | O_DSYNC | O_NONBLOCK)
#else
#define TEST_TTY_FILE_FLAGS (~0)
#endif

static int test_termios_equal(const struct termios *a,
                              const struct termios *b) {
  tcflag_t local_changes = a->c_lflag ^ b->c_lflag;
  size_t i;
#if defined(__APPLE__) && defined(PENDIN)
  /* XNU sets PENDIN when ICANON is restored without flushing. It consumes
   * that bit on the next input operation to reprocess pending bytes. */
  local_changes &= ~PENDIN;
#endif
  if (a->c_iflag != b->c_iflag || a->c_oflag != b->c_oflag ||
      a->c_cflag != b->c_cflag || local_changes ||
      cfgetispeed(a) != cfgetispeed(b) || cfgetospeed(a) != cfgetospeed(b))
    return 0;
  for (i = 0; i < NCCS; ++i)
    if (a->c_cc[i] != b->c_cc[i])
      return 0;
  return 1;
}

#endif
