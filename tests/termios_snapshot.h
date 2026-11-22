#ifndef SOFTLINE_TEST_TERMIOS_SNAPSHOT_H
#define SOFTLINE_TEST_TERMIOS_SNAPSHOT_H

#include <stddef.h>
#include <termios.h>

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
