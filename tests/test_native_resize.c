#define _DEFAULT_SOURCE
#include "softline_surface.h"

#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int drain(int fd, char *bytes, size_t capacity) {
  ssize_t length;
  size_t used = 0;
  while (used + 1 < capacity &&
         (length = read(fd, bytes + used, capacity - used - 1)) > 0)
    used += (size_t)length;
  bytes[used] = '\0';
  return (int)used;
}

static int has_scroll(const char *bytes) {
  const unsigned char *p = (const unsigned char *)bytes;
  while (*p) {
    if (*p++ != 27)
      continue;
    if (*p != '[')
      continue;
    p++;
    while (*p >= 0x20 && *p <= 0x3f)
      p++;
    if (*p == 'S' || *p == 'T' || *p == 'L' || *p == 'M')
      return 1;
    if (*p)
      p++;
  }
  return 0;
}

static int resize_case(int columns, int rows, int producer_row) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, row, col, failed, expected_col;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(
      slave, 40, 10, producer_row < 10 ? producer_row : 9, 3, -1);
  if (!surface)
    return 1;
  expected_col =
      columns != 40 && (producer_row < 10 ? producer_row : 9) + rows - 12 < 0
          ? 0
          : 3;
  (void)drain(master, bytes, sizeof(bytes));
  size.ws_col = (unsigned short)columns;
  size.ws_row = (unsigned short)rows;
  if (ioctl(master, TIOCSWINSZ, &size) != 0)
    return 1;
  failed = sl_surface_resize(surface, columns, rows - 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  {
    char margin[32];
    (void)snprintf(margin, sizeof(margin), "\033[1;%dr", rows - 2);
    failed |= strcmp(bytes, margin) != 0;
  }
  sl_surface_native_position(surface, &row, &col);
  failed |= has_scroll(bytes) || strchr(bytes, '\n') != NULL ||
            (columns != 40 && row < 0) || row >= rows - 2 ||
            col != expected_col || strstr(bytes, "\0337") != NULL ||
            strstr(bytes, "\0338") != NULL;
  if (failed)
    fprintf(stderr, "%dx%d producer row %d: unexpected resize %s\n", columns,
            rows, producer_row, bytes);
  failed |= sl_surface_native_write(surface, "X", 1, rows - 1, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  {
    char margin[32];
    (void)snprintf(margin, sizeof(margin), "\033[1;%dr", rows - 2);
    failed |= strstr(bytes, margin) == NULL;
  }
  failed |= sl_surface_native_write(surface, "Y", 1, rows - 1, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= strstr(bytes, "Y") == NULL || has_scroll(bytes);
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  return failed;
}

static int prompt_growth_case(void) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, row;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, 10, 9, 0, -1);
  if (!surface)
    return 1;
  (void)drain(master, bytes, sizeof(bytes));
  failed = sl_surface_resize(surface, 40, 8) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  sl_surface_native_position(surface, &row, NULL);
  failed |= strstr(bytes, "\033[2S") == NULL || row != 7;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= sl_surface_resize(surface, 40, 10) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  sl_surface_native_position(surface, &row, NULL);
  failed |= has_scroll(bytes) || row != 7;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  return failed;
}

static int exact_width_case(void) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, col;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, 10, 9, 0, -1);
  if (!surface)
    return 1;
  failed = sl_surface_native_write(surface, "abcdefghijkl", 12, 11, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  size.ws_col = 12;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 12, 10) != 0;
  sl_surface_native_position(surface, NULL, &col);
  /* A filled row and a fresh empty row have different continuation behavior. */
  failed |= col != 12;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= sl_surface_native_write(surface, "m", 1, 11, 2) != 0;
  sl_surface_native_position(surface, NULL, &col);
  failed |= col != 1;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= strstr(bytes, "abcdefghijkl") != NULL ||
            strstr(bytes, "\0337") != NULL || strstr(bytes, "\0338") != NULL;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  return failed;
}

static int clipped_tail_case(int line_open, int grow_before_feed) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, row, col, failed;
  char bytes[1024];
  const char tail[] = "\033[31mTAIL\033[0m";
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, 10, 0, 0, -1);
  if (!surface)
    return 1;
  failed = sl_surface_native_write(surface, line_open ? "prefix" : "prefix\n",
                                   line_open ? 6 : 7, 11, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  size.ws_col = 16;
  size.ws_row = 5;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 16, 3) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != (line_open ? 0 : 2) || col != 0;
  (void)drain(master, bytes, sizeof(bytes));
  if (grow_before_feed) {
    size.ws_col = 80;
    size.ws_row = 12;
    failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
              sl_surface_resize(surface, 80, 10) != 0;
    sl_surface_native_position(surface, &row, &col);
    failed |= row != (line_open ? 7 : 9) || col != 0;
    (void)drain(master, bytes, sizeof(bytes));
    failed |= has_scroll(bytes) || strstr(bytes, "prefix") != NULL;
  }
  failed |= sl_surface_native_write(surface, tail, sizeof(tail) - 1,
                                    grow_before_feed ? 11 : 4, 2) != 0;
  sl_surface_native_position(surface, &row, &col);
  (void)drain(master, bytes, sizeof(bytes));
  failed |= row != (line_open ? 0 : 2) + (grow_before_feed ? 7 : 0) ||
            col != 4 || has_scroll(bytes) || strstr(bytes, tail) == NULL ||
            strchr(bytes, '\n') != NULL || strstr(bytes, "prefix") != NULL;
  if (failed)
    fprintf(stderr, "clipped %s resumed at the wrong row\n",
            line_open ? "continuation" : "new line");
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  return failed;
}

/* Geometry and SGR/CR do not consume the unfinished producer line. Only LF
 * may change that state while successive resizes clip its physical endpoint. */
static int repeated_clip_case(const char *source, int open, const char *control,
                              int shrinks) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, row, col, failed, ended, expected;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, 11, 0, 0, -1);
  if (!surface)
    return 1;
  failed = sl_surface_native_write(surface, source, strlen(source), 11, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  size.ws_col = 16;
  size.ws_row = 5;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 16, 4) != 0;
  failed |= sl_surface_native_exit_advances(surface) != open;
  (void)drain(master, bytes, sizeof(bytes));
  if (*control) {
    failed |=
        sl_surface_native_write(surface, control, strlen(control), 4, 2) != 0;
    (void)drain(master, bytes, sizeof(bytes));
    failed |= strstr(bytes, source) != NULL || strstr(bytes, "\033[6n") != NULL;
  }
  ended = !open || !strcmp(control, "\n");
  failed |= sl_surface_native_exit_advances(surface) != !ended;
  size.ws_row = 4;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 16, 3) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= has_scroll(bytes) || strchr(bytes, '\n') != NULL;
  if (shrinks == 3) {
    size.ws_col = 8;
    size.ws_row = 3;
    failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
              sl_surface_resize(surface, 8, 2) != 0;
    (void)drain(master, bytes, sizeof(bytes));
    failed |= has_scroll(bytes) || strchr(bytes, '\n') != NULL;
  }
  failed |= sl_surface_native_exit_advances(surface) != !ended;
  failed |= sl_surface_native_write(surface, "Y", 1, size.ws_row - 1, 2) != 0;
  sl_surface_native_position(surface, &row, &col);
  expected = !open ? size.ws_row - 2 : ended && shrinks == 3 ? 1 : 0;
  failed |= row != expected || col != 1;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= strstr(bytes, source) != NULL || strstr(bytes, "\033[6n") != NULL ||
            strchr(bytes, '\n') != NULL || strstr(bytes, "Y") == NULL;
  failed |= sl_surface_native_finish(surface, size.ws_row - 1) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= has_scroll(bytes) || strchr(bytes, '\n') != NULL;
  if (failed)
    fprintf(stderr,
            "repeated clip: source_bytes=%lu open=%d control=%u shrinks=%d "
            "row=%d expected=%d col=%d\n",
            (unsigned long)strlen(source), open,
            (unsigned int)(unsigned char)control[0], shrinks, row, expected,
            col);
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  return failed;
}

static int newline_case(int mode) {
  struct winsize size;
  struct termios attributes;
  sl_surface_t *surface;
  int master, slave, failed, row, col;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 20;
  size.ws_row = 8;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0 ||
      tcgetattr(slave, &attributes) != 0)
    return 1;
  attributes.c_oflag |= OPOST | ONLCR;
  if (mode == 1)
    attributes.c_oflag &= ~OPOST;
  else if (mode == 2)
    attributes.c_oflag &= ~ONLCR;
  if (tcsetattr(slave, TCSANOW, &attributes) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 20, 7, 0, 0, -1);
  if (!surface)
    return 1;
  (void)drain(master, bytes, sizeof(bytes));
  failed = sl_surface_native_write(surface, "a\nb", 3, 7, 0) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 1 || col != (mode == 0 ? 1 : 2);
  (void)drain(master, bytes, sizeof(bytes));
  failed |= strstr(bytes, mode == 0 ? "a\r\nb" : "a\nb") == NULL;
  failed |= sl_surface_native_write(surface, "c", 1, 7, 0) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 1 || col != (mode == 0 ? 2 : 3);
  (void)drain(master, bytes, sizeof(bytes));
  failed |=
      strstr(bytes, mode == 0 ? "\033[65535;2H" : "\033[65535;3H") == NULL;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "newline mode %d overwrote a native continuation\n", mode);
  return failed;
}

static int clipped_tab_case(int full_row) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, row, col;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 20;
  size.ws_row = 8;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 20, 7, 0, 0, -1);
  if (!surface)
    return 1;
  failed = sl_surface_native_write(surface,
                                   full_row ? "abcdefghijklmnopqrst\t"
                                            : "abcdefghijklmnopqr\tX",
                                   full_row ? 21 : 20, 7, 0) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  size.ws_col = 40;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0;
  failed |= sl_surface_resize(surface, 40, 7) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= sl_surface_native_write(surface, "Y", 1, 7, 0) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 0 || col != (full_row ? 20 : 21);
  (void)drain(master, bytes, sizeof(bytes));
  failed |=
      strstr(bytes, full_row ? "\033[65535;20H" : "\033[65535;21H") == NULL;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "clipped tab introduced a gap after width growth\n");
  return failed;
}

static int owned_cursor_resize_case(void) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, 12, 0, 0, -1);
  if (!surface)
    return 1;
  (void)drain(master, bytes, sizeof(bytes));
  failed = sl_surface_native_write(surface, "hello", 5, -1, 0) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  /* A cursor report may be unavailable. The terminal still retains the
   * physical producer cursor even when the stored bottom delta is clipped. */
  size.ws_row = 6;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 40, 6) != 0;
  failed |= drain(master, bytes, sizeof(bytes)) != 0;
  failed |= sl_surface_native_write(surface, "X", 1, -1, 0) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= strcmp(bytes, "\033[0mX\033[0m") != 0;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "prompt-free shrink moved the live producer cursor\n");
  return failed;
}

static int observed_unicode_case(int newline, int handoff) {
  static const char flag[] = "\360\237\207\270\360\237\207\252";
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, row, col;
  char bytes[1024];
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, handoff ? 12 : 10, 0, 0, -1);
  if (!surface)
    return 1;
  (void)drain(master, bytes, sizeof(bytes));
  failed = sl_surface_native_write(surface, flag, sizeof(flag) - 1,
                                   handoff ? -1 : 11, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  if (handoff) {
    failed |= strstr(bytes, "\033[6n") != NULL;
    sl_surface_native_observe(surface, 40, 12, 0, 2);
  } else {
    failed |= strstr(bytes, "\033[6n") != NULL;
    sl_surface_native_observe_write(surface, 0, 2);
  }
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 0 || col != 2;
  if (newline) {
    failed |=
        sl_surface_native_write(surface, "\nX", 2, handoff ? -1 : 11, 2) != 0;
    /* A previous line's width can change its observed row, but must not
     * become part of this new line's column after growth. */
    if (handoff)
      sl_surface_native_observe(surface, 40, 12, 2, 1);
    else
      sl_surface_native_observe_write(surface, 2, 1);
  }
  size.ws_col = 60;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 60, handoff ? 12 : 10) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != (newline ? 2 : 0) || col != (newline ? 1 : 2);
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "observed Unicode endpoint drifted after growth\n");
  return failed;
}

static int observed_opaque_endpoint_case(void) {
  static const char wide[] = "\344\270\255";
  static const char narrow[] = "\360\237\217\263";
  char source[29 + sizeof(wide) - 1 + sizeof(narrow) - 1 + 1];
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, row, col;
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  memset(source, 'a', 29);
  memcpy(source + 29, wide, sizeof(wide) - 1);
  memcpy(source + 29 + sizeof(wide) - 1, narrow, sizeof(narrow) - 1);
  source[sizeof(source) - 1] = 'X';
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  surface = sl_surface_create_native(slave, 40, 10, 0, 0, -1);
  if (!surface) {
    close(slave);
    close(master);
    return 1;
  }
  failed = sl_surface_native_write(surface, source, sizeof(source), 11, 2) != 0;
  /* Only the aggregate terminal endpoint is authoritative. There is no
   * glyph-width callback or span reconstruction after changing the width. */
  sl_surface_native_observe_write(surface, 0, 33);
  size.ws_col = 30;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 30, 10) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= col != 29;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "opaque endpoint was reconstructed instead of clamped\n");
  return failed;
}

static int observed_geometry_case(void) {
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, row, col;
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  surface = sl_surface_create_native(slave, 40, 12, 0, 0, -1);
  if (!surface)
    return 1;
  failed = sl_surface_native_write(surface, "ABC", 3, -1, 0) != 0;
  /* The terminal may move the viewport during physical height growth. That
   * observed row change must not add six rows to the logical text width. */
  sl_surface_native_observe(surface, 40, 18, 6, 3);
  size.ws_col = 60;
  size.ws_row = 18;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 60, 18) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 6 || col != 3;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "observed physical resize changed the logical column\n");
  return failed;
}

static int observed_pending_wrap_case(int handoff) {
  char source[40];
  struct winsize size;
  sl_surface_t *surface;
  int master, slave, failed, row, col;
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 12;
  memset(source, 'a', sizeof(source));
  if (openpty(&master, &slave, NULL, NULL, &size) != 0)
    return 1;
  surface = sl_surface_create_native(slave, 40, handoff ? 12 : 10, 0, 0, -1);
  if (!surface)
    return 1;
  failed = sl_surface_native_write(surface, source, sizeof(source),
                                   handoff ? -1 : 11, 2) != 0;
  if (handoff)
    sl_surface_native_observe(surface, 40, 12, 0, 39);
  else
    sl_surface_native_observe_write(surface, 0, 39);
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 0 || col != 40;
  size.ws_col = 80;
  failed |= ioctl(master, TIOCSWINSZ, &size) != 0 ||
            sl_surface_resize(surface, 80, handoff ? 12 : 10) != 0;
  sl_surface_native_position(surface, &row, &col);
  failed |= row != 0 || col != 40;
  sl_surface_destroy(surface);
  close(slave);
  close(master);
  if (failed)
    fprintf(stderr, "observed pending wrap kept an uncorrected row count\n");
  return failed;
}

static int hyperlink_wrap_case(const char *control, const char *tail) {
  struct winsize size = {12, 40, 0, 0};
  int master, slave, failed;
  char bytes[2048], source[256];
  sl_surface_t *surface;
  if (openpty(&master, &slave, NULL, NULL, &size))
    return 1;
  (void)fcntl(master, F_SETFL, O_NONBLOCK);
  surface = sl_surface_create_native(slave, 40, 10, 0, 0, 0);
  if (!surface)
    return 1;
  sl_surface_set_osc8(surface, 1);
  failed = sl_surface_native_write(surface,
                                   "0123456789012345678901234567890123456789",
                                   40, 10, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  snprintf(source, sizeof(source), "%s%s", control, tail);
  failed |=
      sl_surface_native_write(surface, source, strlen(source), 10, 2) != 0;
  (void)drain(master, bytes, sizeof(bytes));
  failed |= (strstr(bytes, "\r\033D") != NULL) != (*tail == 'X');
  if (failed)
    fprintf(stderr, "hyperlink changed pending wrap: %s\n", bytes);
  sl_surface_destroy(surface);
  close(master);
  close(slave);
  return failed;
}

int main(void) {
  static const int dimensions[][2] = {{16, 5},  {40, 5},  {80, 5}, {16, 12},
                                      {80, 12}, {16, 18}, {80, 18}};
  size_t i;
  size_t source, control;
  int failed = 0, shrinks;
  const struct {
    const char *text;
    int open;
  } sources[] = {{"prefix", 1},
                 {"prefix\r", 1},
                 {"café", 1},
                 {"0123456789012345678901234567890123456789", 1},
                 {"prefix\n", 0}};
  const char *controls[] = {"", "\033[31m", "\r", "\n"};
  for (i = 0; i < sizeof(dimensions) / sizeof(dimensions[0]); i++) {
    failed |= resize_case(dimensions[i][0], dimensions[i][1], 0);
    failed |= resize_case(dimensions[i][0], dimensions[i][1], 8);
    failed |=
        resize_case(dimensions[i][0], dimensions[i][1], dimensions[i][1] - 1);
  }
  failed |= prompt_growth_case();
  failed |= exact_width_case();
  failed |= clipped_tail_case(1, 0);
  failed |= clipped_tail_case(0, 0);
  failed |= clipped_tail_case(1, 1);
  failed |= clipped_tail_case(0, 1);
  for (source = 0; source < sizeof(sources) / sizeof(sources[0]); ++source)
    for (control = 0; control < sizeof(controls) / sizeof(controls[0]);
         ++control)
      for (shrinks = 2; shrinks <= 3; ++shrinks)
        failed |= repeated_clip_case(sources[source].text, sources[source].open,
                                     controls[control], shrinks);
  for (i = 0; i < 3; i++)
    failed |= newline_case((int)i);
  failed |= clipped_tab_case(0);
  failed |= clipped_tab_case(1);
  failed |= owned_cursor_resize_case();
  failed |= observed_unicode_case(0, 0);
  failed |= observed_unicode_case(1, 0);
  failed |= observed_unicode_case(0, 1);
  failed |= observed_unicode_case(1, 1);
  failed |= observed_opaque_endpoint_case();
  failed |= observed_geometry_case();
  failed |= observed_pending_wrap_case(0);
  failed |= observed_pending_wrap_case(1);
  failed |= hyperlink_wrap_case("\033]8;;https://xyz.test/\033\\", "X");
  failed |= hyperlink_wrap_case("\033]8;;https://example.org/\007", "\n");
  failed |= hyperlink_wrap_case("\033]8;;https://example.org/\033\\", "\r");
  failed |= hyperlink_wrap_case("\033]8;;https://xyz.test/\007", "");
  if (!failed)
    puts("Physical resize and repeated clipping preserve line state; "
         "prompt growth still scrolls.");
  return failed;
}
