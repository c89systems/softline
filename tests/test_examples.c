#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "softline/softline.h"

#include <errno.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
  MAX_ROWS = 40,
  MAX_COLS = 140,
  RAW_CAP = 262144,
  CHAT_READY_TIMEOUT_MS = 15000
};
static const char *const observed[] = {
    "A short answer", "Next step",           "A longer answer", "> draft",
    "> queued",       "Operation cancelled", "Q 1. queued"};

struct terminal {
  int fd, rows, cols, row, col, escape;
  int saved_row, saved_col, scroll_top, scroll_bottom;
  int cursor_visible;
  int farewell_above_status;
  unsigned int region_updates;
  const char *guard[5];
  size_t frames, frame_failure, frame_offset;
  char csi[64];
  size_t csi_len, raw_len;
  size_t read_chunk;
  unsigned int seen;
  char cells[MAX_ROWS][MAX_COLS + 1];
  char raw[RAW_CAP];
  unsigned int scrolls, region_scrolls;
  char history[128][MAX_COLS + 1];
};

static int tests_run, tests_passed;
#define TEST(name)                                                             \
  do {                                                                         \
    tests_run++;                                                               \
    printf("  %-58s", name);                                                   \
  } while (0)
#define PASS()                                                                 \
  do {                                                                         \
    tests_passed++;                                                            \
    printf("PASS\n");                                                          \
  } while (0)
#define FAIL(msg)                                                              \
  do {                                                                         \
    printf("FAIL: %s\n", msg);                                                 \
    return;                                                                    \
  } while (0)
#define ASSERT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    if (!(cond))                                                               \
      FAIL(msg);                                                               \
  } while (0)

static void term_clear(struct terminal *t) {
  int row;
  for (row = 0; row < t->rows; row++) {
    memset(t->cells[row], ' ', (size_t)t->cols);
    t->cells[row][t->cols] = 0;
  }
  t->row = t->col = 0;
}

static void term_init(struct terminal *t, int fd, int cols, int rows) {
  memset(t, 0, sizeof(*t));
  t->fd = fd;
  t->cols = cols;
  t->rows = rows;
  t->scroll_bottom = rows - 1;
  t->cursor_visible = 1;
  term_clear(t);
}

static void term_scroll(struct terminal *t) {
  int row;
  t->region_scrolls++;
  if (t->scroll_top == 0 && t->scroll_bottom == t->rows - 1) {
    memcpy(t->history[t->scrolls % 128u], t->cells[0], (size_t)t->cols + 1);
    t->scrolls++;
  }
  for (row = t->scroll_top + 1; row <= t->scroll_bottom; row++)
    memcpy(t->cells[row - 1], t->cells[row], (size_t)t->cols + 1);
  memset(t->cells[t->scroll_bottom], ' ', (size_t)t->cols);
  t->cells[t->scroll_bottom][t->cols] = 0;
  t->row = t->scroll_bottom;
}

/* Main-screen resize preserves cells, scrolling only enough to retain the
 * live input cursor. Real width reflow is exercised separately in tmux. */
static void term_resize(struct terminal *t, int cols, int rows) {
  int shift = t->row >= rows ? t->row - rows + 1 : 0;
  int row, old_row = t->row, old_rows = t->rows, old_cols = t->cols;
  t->scroll_top = 0;
  t->scroll_bottom = old_rows - 1;
  for (row = 0; row < shift; row++)
    term_scroll(t);
  t->row = old_row - shift;
  t->saved_row -= shift;
  if (t->saved_row < 0)
    t->saved_row = 0;
  for (row = 0; row < rows; row++) {
    if (row >= old_rows)
      memset(t->cells[row], ' ', (size_t)cols);
    else if (cols > old_cols)
      memset(t->cells[row] + old_cols, ' ', (size_t)(cols - old_cols));
    t->cells[row][cols] = 0;
  }
  t->cols = cols;
  t->rows = rows;
  t->scroll_bottom = rows - 1;
  if (t->col >= cols)
    t->col = cols - 1;
  if (t->saved_col >= cols)
    t->saved_col = cols - 1;
}

static void term_csi(struct terminal *t, char final) {
  int a, b;
  char *part;
  t->csi[t->csi_len] = 0;
  if (t->csi[0] == '?') {
    if (strcmp(t->csi, "?25") == 0)
      t->cursor_visible = final == 'h';
    return;
  }
  a = t->csi_len ? atoi(t->csi) : 0;
  part = strchr(t->csi, ';');
  b = part ? atoi(part + 1) : 0;
  switch (final) {
  case 'n':
    if (a == 6) {
      char reply[48];
      int count = snprintf(reply, sizeof(reply), "\033[%d;%dR", t->row + 1,
                           t->col < t->cols ? t->col + 1 : t->cols);
      if (count > 0 && count < (int)sizeof(reply))
        (void)write(t->fd, reply, (size_t)count);
    }
    break;
  case 'r':
    t->region_updates++;
    t->scroll_top = a > 0 ? a - 1 : 0;
    t->scroll_bottom = b > 0 && b <= t->rows ? b - 1 : t->rows - 1;
    t->row = t->col = 0;
    break;
  case 'L': {
    int count = a > 0 ? a : 1;
    int row;
    if (t->row < t->scroll_top || t->row > t->scroll_bottom)
      break;
    if (count > t->scroll_bottom - t->row + 1)
      count = t->scroll_bottom - t->row + 1;
    for (row = t->scroll_bottom; row >= t->row + count; row--)
      memcpy(t->cells[row], t->cells[row - count], (size_t)t->cols + 1);
    for (row = t->row; row < t->row + count; row++) {
      memset(t->cells[row], ' ', (size_t)t->cols);
      t->cells[row][t->cols] = 0;
    }
    break;
  }
  case 'S': {
    int count = a > 0 ? a : 1;
    int old_row = t->row;
    while (count-- > 0)
      term_scroll(t);
    t->row = old_row;
    break;
  }
  case 'H':
  case 'f':
    t->row = a > 0 ? a - 1 : 0;
    t->col = b > 0 ? b - 1 : 0;
    break;
  case 'A':
    t->row -= a > 0 ? a : 1;
    break;
  case 'B':
    t->row += a > 0 ? a : 1;
    break;
  case 'C':
    t->col += a > 0 ? a : 1;
    break;
  case 'D':
    t->col -= a > 0 ? a : 1;
    break;
  case 'J':
    if (a == 2)
      term_clear(t);
    break;
  case 'K':
    memset(t->cells[t->row] + t->col, ' ', (size_t)(t->cols - t->col));
    break;
  case 'X': {
    int count = a > 0 ? a : 1;
    if (count > t->cols - t->col)
      count = t->cols - t->col;
    memset(t->cells[t->row] + t->col, ' ', (size_t)count);
    break;
  }
  default:
    break;
  }
  if (t->row < 0)
    t->row = 0;
  if (t->row >= t->rows)
    t->row = t->rows - 1;
  if (t->col < 0)
    t->col = 0;
  if (t->col >= t->cols &&
      (final == 'H' || final == 'f' || final == 'C' || final == 'D'))
    t->col = t->cols - 1;
}

/* Inspect every byte prefix, including each completed terminal control,
 * rather than only the screen after a feed returns. */
static void term_check_frame(struct terminal *t) {
  size_t k;
  int message_row;
  for (message_row = 0; message_row + 1 < t->rows; message_row++)
    if (strstr(t->cells[message_row], "! Goodbye.") &&
        strstr(t->cells[message_row + 1], "streaming demo"))
      t->farewell_above_status = 1;
  for (k = 0; k < sizeof(t->guard) / sizeof(t->guard[0]); k++) {
    int row, matches = 0;
    if (!t->guard[k])
      continue;
    for (row = 0; row < t->rows; row++)
      if (strstr(t->cells[row], t->guard[k]))
        matches++;
    if (matches != 1 && !t->frame_failure)
      t->frame_failure = t->frame_offset;
  }
  if (t->guard[0])
    t->frames++;
}

static void term_feed(struct terminal *t, const char *bytes, size_t len) {
  size_t i;
  if (len > RAW_CAP - 1 - t->raw_len)
    len = RAW_CAP - 1 - t->raw_len;
  memcpy(t->raw + t->raw_len, bytes, len);
  t->raw_len += len;
  t->raw[t->raw_len] = 0;
  for (i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)bytes[i];
    t->frame_offset = t->raw_len - len + i + 1;
    if (t->escape == 1) {
      if (ch == '7') {
        t->saved_row = t->row;
        t->saved_col = t->col;
      } else if (ch == '8') {
        t->row = t->saved_row < t->rows ? t->saved_row : t->rows - 1;
        t->col = t->saved_col <= t->cols ? t->saved_col : t->cols - 1;
      }
      t->escape = ch == '[' ? 2 : 0;
      if (t->escape == 2)
        t->csi_len = 0;
      term_check_frame(t);
      continue;
    }
    if (t->escape == 2) {
      if (ch >= '@' && ch <= '~') {
        term_csi(t, (char)ch);
        t->escape = 0;
      } else if (t->csi_len < sizeof(t->csi) - 1) {
        t->csi[t->csi_len++] = (char)ch;
      }
      term_check_frame(t);
      continue;
    }
    if (ch == 27)
      t->escape = 1;
    else if (ch == '\r')
      t->col = 0;
    else if (ch == '\n') {
      if (t->row == t->scroll_bottom)
        term_scroll(t);
      else
        t->row++;
    } else if (ch >= 32) {
      size_t k;
      if (t->col >= t->cols) {
        t->col = 0;
        if (t->row == t->scroll_bottom)
          term_scroll(t);
        else
          t->row++;
      }
      t->cells[t->row][t->col++] = (char)ch;
      for (k = 0; k < sizeof(observed) / sizeof(observed[0]); k++) {
        if (t->seen & (1u << k))
          continue;
        if (strstr(t->cells[t->row], observed[k]))
          t->seen |= 1u << k;
      }
    }
    term_check_frame(t);
  }
}

static int term_read(struct terminal *t) {
  fd_set fds;
  struct timeval tv;
  char bytes[2048];
  ssize_t n;
  FD_ZERO(&fds);
  FD_SET(t->fd, &fds);
  tv.tv_sec = 0;
  tv.tv_usec = 50000;
  if (select(t->fd + 1, &fds, NULL, NULL, &tv) <= 0)
    return 0;
  n = read(t->fd, bytes, t->read_chunk ? t->read_chunk : sizeof(bytes));
  if (n < 0 && (errno == EINTR || errno == EAGAIN))
    return 0;
  if (n <= 0) {
    if (n != 0 && errno != EIO)
      fprintf(stderr, "terminal read ended: n=%ld errno=%d\n", (long)n, errno);
    return -1;
  }
  term_feed(t, bytes, (size_t)n);
  return 1;
}

static int term_contains(const struct terminal *t, const char *needle) {
  int row;
  for (row = 0; row < t->rows; row++)
    if (strstr(t->cells[row], needle))
      return 1;
  return 0;
}

static int term_row_of(const struct terminal *t, const char *needle) {
  int row;
  for (row = 0; row < t->rows; row++)
    if (strstr(t->cells[row], needle))
      return row;
  return -1;
}

static struct timespec deadline_after(int milliseconds) {
  struct timespec deadline;
  (void)clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += milliseconds / 1000;
  deadline.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  return deadline;
}

static int before_deadline(const struct timespec *deadline) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec < deadline->tv_sec ||
         (now.tv_sec == deadline->tv_sec && now.tv_nsec < deadline->tv_nsec);
}

static int wait_screen(struct terminal *t, const char *needle, int ms) {
  struct timespec deadline = deadline_after(ms);
  size_t k;
  unsigned int bit = 0;
  for (k = 0; k < sizeof(observed) / sizeof(observed[0]); k++)
    if (strcmp(needle, observed[k]) == 0)
      bit = 1u << k;
  while (!term_contains(t, needle) && !(t->seen & bit) &&
         before_deadline(&deadline))
    if (term_read(t) < 0)
      return -1;
  return term_contains(t, needle) || (t->seen & bit) ? 0 : -1;
}

static int wait_raw(struct terminal *t, const char *needle, int ms) {
  struct timespec deadline = deadline_after(ms);
  while (!strstr(t->raw, needle) && before_deadline(&deadline))
    if (term_read(t) < 0)
      return -1;
  return strstr(t->raw, needle) ? 0 : -1;
}

static int wait_raw_since(struct terminal *t, size_t mark, const char *needle,
                          int ms) {
  struct timespec deadline = deadline_after(ms);
  while (mark <= t->raw_len && !strstr(t->raw + mark, needle) &&
         before_deadline(&deadline))
    if (term_read(t) < 0)
      return -1;
  return mark <= t->raw_len && strstr(t->raw + mark, needle) ? 0 : -1;
}

static int wait_raw_after(struct terminal *t, const char *first,
                          const char *second, int ms) {
  struct timespec deadline = deadline_after(ms);
  const char *at;
  while (before_deadline(&deadline)) {
    at = strstr(t->raw, first);
    if (at && strstr(at + strlen(first), second))
      return 0;
    if (term_read(t) < 0)
      return -1;
  }
  return -1;
}

static void term_dump(const struct terminal *t) {
  int row;
  size_t i;
  const char *failure;
  fprintf(stderr, "\nterminal snapshot (%lu bytes):\n",
          (unsigned long)t->raw_len);
  for (row = 0; row < t->rows; row++)
    fprintf(stderr, "%2d |%s|\n", row, t->cells[row]);
  failure = strstr(t->raw, "chat ");
  if (failure)
    fprintf(stderr, "chat diagnostic: %.200s\n", failure);
  fprintf(stderr, "tail: ");
  for (i = t->raw_len > 240 ? t->raw_len - 240 : 0; i < t->raw_len; i++) {
    unsigned char ch = (unsigned char)t->raw[i];
    fputc(ch >= 32 && ch < 127 ? ch : '.', stderr);
  }
  fputc('\n', stderr);
}

static pid_t spawn(const char *path, int *fd, int cols, int rows) {
  struct winsize ws;
  int slave;
  pid_t pid;
  char *args[2];
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)cols;
  ws.ws_row = (unsigned short)rows;
  if (openpty(fd, &slave, NULL, NULL, &ws) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    close(*fd);
    (void)setsid();
    (void)ioctl(slave, TIOCSCTTY, 0);
    (void)dup2(slave, STDIN_FILENO);
    (void)dup2(slave, STDOUT_FILENO);
    (void)dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO)
      close(slave);
    args[0] = (char *)path;
    args[1] = NULL;
    execv(path, args);
    _exit(127);
  }
  close(slave);
  return pid;
}

static int finish(pid_t pid, int fd) {
  int status, i;
  char tail[1024];
  size_t tail_len = 0;
  struct timespec deadline = deadline_after(10000);
  while (before_deadline(&deadline)) {
    fd_set fds;
    struct timeval tv;
    char discard[2048];
    int ready;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      close(fd);
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
      fprintf(stderr, "example child exited with status=%d\n", status);
      fwrite(tail, 1, tail_len, stderr);
      fputc('\n', stderr);
      return -1;
    }
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    ready = select(fd + 1, &fds, NULL, NULL, &tv);
    if (ready > 0) {
      ssize_t amount = read(fd, discard, sizeof(discard));
      if (amount > 0) {
        size_t keep = (size_t)amount;
        if (keep > sizeof(tail)) {
          memcpy(tail, discard + keep - sizeof(tail), sizeof(tail));
          tail_len = sizeof(tail);
        } else {
          if (tail_len + keep > sizeof(tail)) {
            size_t shift = tail_len + keep - sizeof(tail);
            memmove(tail, tail + shift, tail_len - shift);
            tail_len -= shift;
          }
          memcpy(tail + tail_len, discard, keep);
          tail_len += keep;
        }
      } else {
        usleep(10000);
      }
    }
  }
  (void)kill(-pid, SIGTERM);
  fprintf(stderr, "example child did not exit before timeout\n");
  for (i = 0; i < (int)tail_len; i++) {
    unsigned char ch = (unsigned char)tail[i];
    fputc(ch >= 32 && ch < 127 ? ch : '.', stderr);
  }
  fputc('\n', stderr);
  (void)kill(pid, SIGTERM);
  for (i = 0; i < 100; i++) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      close(fd);
      return -1;
    }
    usleep(10000);
  }
  (void)kill(-pid, SIGKILL);
  (void)kill(pid, SIGKILL);
  (void)waitpid(pid, &status, 0);
  close(fd);
  return -1;
}

static int cancel_and_quit(struct terminal *t, int fd, pid_t pid) {
  size_t mark = t->raw_len;
  if (write(fd, "\003", 1) != 1 ||
      wait_raw_since(t, mark, "\033[?2004h", 3000) != 0 ||
      write(fd, "/quit\r", 6) != 6) {
    term_dump(t);
    return -1;
  }
  return finish(pid, fd);
}

static void test_simple(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  TEST("simple example accepts wrapped input");
  pid = spawn(path, &fd, 40, 6);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 40, 6);
  ASSERT_TRUE(wait_screen(&t, "softline> ", 3000) == 0, "prompt missing");
  ASSERT_TRUE(write(fd, "a long example input nextword", 29) == 29,
              "input failed");
  ASSERT_TRUE(wait_screen(&t, "nextword", 3000) == 0, "editing failed");
  ASSERT_TRUE(write(fd, "\r", 1) == 1, "submit failed");
  ASSERT_TRUE(wait_raw(&t, "submitted: a long", 3000) == 0,
              "submission missing");
  ASSERT_TRUE(wait_raw_after(&t, "submitted: a long", "\033[?2004h", 3000) == 0,
              "next prompt missing");
  ASSERT_TRUE(write(fd, "/quit\r", 6) == 6, "quit failed");
  ASSERT_TRUE(finish(pid, fd) == 0, "child failed");
  PASS();
}

static void test_chat_live_queue(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  struct timespec deadline;
  TEST("chat streams Markdown while editing and dispatches FIFO");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  if (wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) != 0) {
    int status = 0;
    term_dump(&t);
    fprintf(stderr, "child status: waitpid=%ld status=%d\n",
            (long)waitpid(pid, &status, WNOHANG), status);
    FAIL("editor missing");
  }
  ASSERT_TRUE(strstr(t.raw, "\033[?1049h") == NULL, "alternate screen used");
  ASSERT_TRUE(write(fd, "first\r", 6) == 6, "first send failed");
  ASSERT_TRUE(wait_screen(&t, "> first", 3000) == 0, "quote missing");
  ASSERT_TRUE(wait_screen(&t, "! Thinking...", 3000) == 0,
              "status message missing");
  ASSERT_TRUE(wait_screen(&t, "streaming demo", 3000) == 0,
              "status line missing while thinking");
  ASSERT_TRUE(wait_screen(&t, "demo/project", 3000) == 0,
              "neutral demo location missing from status line");
  ASSERT_TRUE(term_row_of(&t, "Thinking...") <
                  term_row_of(&t, "streaming demo"),
              "status message is not above the status line");
  ASSERT_TRUE(strstr(t.raw, "\033[3;") != NULL,
              "italic status or prompt treatment missing");
  ASSERT_TRUE(wait_screen(&t, "A short answer", 3000) == 0,
              "heading not streamed");
  ASSERT_TRUE(strstr(t.raw, "\033[3;96mfirst") != NULL,
              "quoted prompt did not use its italic accent style");
  ASSERT_TRUE(wait_screen(&t, "! Reasoning...", 3000) == 0,
              "status message did not update mid-stream");
  ASSERT_TRUE(write(fd, "draft", 5) == 5, "draft failed");
  ASSERT_TRUE(wait_screen(&t, "> draft", 3000) == 0,
              "editing blocked during stream");
  ASSERT_TRUE(write(fd, "\rsecond\r", 8) == 8, "queue input failed");
  ASSERT_TRUE(wait_screen(&t, "Q 1. draft", 3000) == 0,
              "first queue item invisible");
  ASSERT_TRUE(wait_screen(&t, "2. second", 3000) == 0,
              "second queue item invisible");
  t.seen &= ~(1u << 3);
  if (wait_screen(&t, "Next step", 6000) != 0) {
    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    fprintf(stderr, "chat child wait=%ld status=%d\n", (long)waited, status);
    term_dump(&t);
    FAIL("stream stopped before completion");
  }
  ASSERT_TRUE(wait_screen(&t, "> draft", 6000) == 0,
              "first queued turn not dispatched");
  ASSERT_TRUE(wait_screen(&t, "A longer answer", 6000) == 0,
              "queued turn did not start next operation");
  ASSERT_TRUE(wait_screen(&t, "+ streaming demo", 10000) == 0,
              "queued turn did not finish");
  deadline = deadline_after(2000);
  while (before_deadline(&deadline) &&
         (t.row != t.rows - 1 || t.col != 2 || !t.cursor_visible))
    if (term_read(&t) < 0)
      break;
  ASSERT_TRUE(t.scroll_top == 0 && t.scroll_bottom < t.rows - 1 &&
                  t.row > t.scroll_bottom && strstr(t.raw, "\033[2J") == NULL,
              "stream did not keep its scroll region above the prompt");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_long_unbroken_editor_word(const char *path) {
  int fd;
  int first_row;
  int quote_row;
  pid_t pid;
  size_t quote_mark;
  char word[44];
  struct terminal t;
  TEST("chat editor and quote fill rows inside a long unbroken word");
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "0", 1) == 0,
              "failed to set fast stream delay");
  pid = spawn(path, &fd, 20, 40);
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1) == 0,
              "failed to restore stream delay");
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 20, 40);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  memset(word, 'a', 18);
  memset(word + 18, 'b', 18);
  memset(word + 36, 'c', 7);
  word[43] = '\0';
  ASSERT_TRUE(write(fd, word, 43) == 43, "long word input failed");
  ASSERT_TRUE(wait_screen(&t, "ccccccc", 3000) == 0,
              "long word was not fully rendered");
  first_row = term_row_of(&t, "> aaaaaaaaaaaaaaaaaa");
  if (first_row < 0 || first_row + 2 >= t.rows ||
      strstr(t.cells[first_row + 1], "  bbbbbbbbbbbbbbbbbb") == NULL ||
      strstr(t.cells[first_row + 2], "  ccccccc") == NULL)
    term_dump(&t);
  ASSERT_TRUE(first_row >= 0 && first_row + 2 < t.rows &&
                  strstr(t.cells[first_row + 1], "  bbbbbbbbbbbbbbbbbb") &&
                  strstr(t.cells[first_row + 2], "  ccccccc"),
              "editor left a short or blank row in an unbroken word");
  quote_mark = t.raw_len;
  ASSERT_TRUE(write(fd, "\r", 1) == 1, "long word submit failed");
  ASSERT_TRUE(wait_raw_since(&t, quote_mark, "\033[3;96m", 3000) == 0 &&
                  wait_screen(&t, "> ccccccc", 3000) == 0,
              "quoted long word was not rendered");
  quote_row = term_row_of(&t, "> aaaaaaaaaaaaaaaaaa");
  if (quote_row < 0 || quote_row + 2 >= t.rows ||
      strstr(t.cells[quote_row + 1], "> bbbbbbbbbbbbbbbbbb") == NULL ||
      strstr(t.cells[quote_row + 2], "> ccccccc") == NULL)
    term_dump(&t);
  ASSERT_TRUE(quote_row >= 0 && quote_row + 2 < t.rows &&
                  strstr(t.cells[quote_row + 1], "> bbbbbbbbbbbbbbbbbb") &&
                  strstr(t.cells[quote_row + 2], "> ccccccc"),
              "quoted long word left a short or blank row");
  quote_mark = t.raw_len;
  ASSERT_TRUE(wait_raw_since(&t, quote_mark, "\033[32m+ ", 6000) == 0,
              "long word response did not finish");
  ASSERT_TRUE(write(fd, "/quit\r", 6) == 6, "quit failed");
  ASSERT_TRUE(finish(pid, fd) == 0, "child failed");
  PASS();
}

static void test_chat_queued_quit(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  TEST("queued /quit runs after earlier FIFO turns");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "first\r", 6) == 6, "first send failed");
  ASSERT_TRUE(wait_screen(&t, "! Thinking...", 3000) == 0,
              "response did not start");
  ASSERT_TRUE(write(fd, "followup\r/quit\r", 15) == 15, "queued turns failed");
  ASSERT_TRUE(wait_screen(&t, "Q 2. /quit", 3000) == 0,
              "/quit was not queued behind the follow-up");
  ASSERT_TRUE(wait_screen(&t, "> followup", 6000) == 0,
              "earlier queued turn was skipped");
  ASSERT_TRUE(wait_screen(&t, "A longer answer", 6000) == 0,
              "earlier queued turn did not receive a response");
  ASSERT_TRUE(wait_raw(&t, "Goodbye.", 8000) == 0 && t.farewell_above_status,
              "exit farewell did not render");
  ASSERT_TRUE(finish(pid, fd) == 0, "queued /quit did not terminate chat");
  PASS();
}

static void test_chat_promoted_quit(const char *path) {
  int fd;
  pid_t pid;
  size_t cancel_mark;
  struct terminal t;
  TEST("idle promotion delivers queued /quit immediately");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "first\r", 6) == 6, "first send failed");
  ASSERT_TRUE(wait_screen(&t, "! Thinking...", 3000) == 0,
              "response did not start");
  ASSERT_TRUE(write(fd, "/quit\r", 6) == 6, "queued quit failed");
  ASSERT_TRUE(wait_screen(&t, "Q 1. /quit", 3000) == 0,
              "quit was not queued while busy");
  cancel_mark = t.raw_len;
  ASSERT_TRUE(write(fd, "\003", 1) == 1, "cancel failed");
  ASSERT_TRUE(wait_screen(&t, "Operation cancelled", 3000) == 0,
              "operation did not cancel");
  ASSERT_TRUE(wait_raw_since(&t, cancel_mark, "\033[?2004h", 3000) == 0,
              "editor did not reopen after cancellation");
  ASSERT_TRUE(write(fd, "\033\r", 2) == 2, "promotion failed");
  ASSERT_TRUE(wait_raw(&t, "Goodbye.", 3000) == 0 && t.farewell_above_status,
              "exit farewell did not render");
  ASSERT_TRUE(finish(pid, fd) == 0, "promoted /quit did not terminate chat");
  PASS();
}

static void test_chat_steered_quit(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  TEST("steered /quit runs at the next application response seam");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "first\r", 6) == 6, "first send failed");
  ASSERT_TRUE(wait_screen(&t, "! Thinking...", 3000) == 0,
              "response did not start");
  ASSERT_TRUE(write(fd, "/quit\033\r", 7) == 7, "steer failed");
  ASSERT_TRUE(wait_screen(&t, "S 1. /quit", 3000) == 0,
              "quit was not marked as steer");
  ASSERT_TRUE(wait_raw(&t, "Goodbye.", 3000) == 0 && t.farewell_above_status,
              "exit farewell did not render");
  ASSERT_TRUE(finish(pid, fd) == 0, "steered /quit did not terminate chat");
  PASS();
}

static void
test_chat_preserves_transcript_and_prompt_spacing(const char *path) {
  int fd;
  int note_row;
  int note_last_row;
  int quote_row;
  int answer_row;
  pid_t pid;
  size_t output_mark;
  struct terminal t;
  TEST("chat keeps visible transcript and blank rows around rendered prompt");
  pid = spawn(path, &fd, 120, 30);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 120, 30);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(term_contains(&t, "Enter sends or queues"),
              "introductory transcript missing");
  ASSERT_TRUE(term_contains(&t, "Completion demo:"),
              "completion guide missing");
  output_mark = t.raw_len;
  ASSERT_TRUE(write(fd, "hello\r", 6) == 6, "send failed");
  ASSERT_TRUE(wait_screen(&t, "A short answer", 3000) == 0,
              "response heading missing");
  note_row = term_row_of(&t, "Enter sends or queues");
  quote_row = term_row_of(&t, "> hello");
  /* Startup help can grow or wrap; locate its actual final visible row. */
  note_last_row = quote_row - 1;
  while (note_last_row >= 0 &&
         strspn(t.cells[note_last_row], " ") == (size_t)t.cols)
    --note_last_row;
  answer_row = term_row_of(&t, "A short answer");
  ASSERT_TRUE(strstr(t.raw + output_mark, "\033[1;1H") == NULL,
              "submission repainted the full terminal viewport");
  ASSERT_TRUE(note_row >= 0 && note_last_row >= note_row &&
                  quote_row == note_last_row + 2 && answer_row == quote_row + 2,
              "visible transcript or prompt spacing was lost");
  ASSERT_TRUE(t.cells[quote_row][0] == '>' && t.cells[quote_row][1] == ' ',
              "quoted prompt prefix is indented");
  ASSERT_TRUE(strstr(t.raw, "                    ") == NULL,
              "viewport clearing wrote literal blank runs to scrollback");
  ASSERT_TRUE(quote_row > 0 &&
                  strspn(t.cells[quote_row - 1], " ") == (size_t)t.cols &&
                  strspn(t.cells[quote_row + 1], " ") == (size_t)t.cols,
              "rendered prompt lacks a blank row on each side");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_spacing_across_turns(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  TEST("chat keeps one empty row across consecutive response turns");
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "0", 1) == 0,
              "failed to set fast stream delay");
  pid = spawn(path, &fd, 120, 40);
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1) == 0,
              "failed to restore stream delay");
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 120, 40);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "one\r", 4) == 4, "first send failed");
  ASSERT_TRUE(wait_screen(&t, "Try another prompt.", 3000) == 0,
              "first response missing");
  ASSERT_TRUE(wait_screen(&t, "+ streaming demo", 10000) == 0,
              "first response did not finish");
  ASSERT_TRUE(write(fd, "two\r", 4) == 4, "second send failed");
  ASSERT_TRUE(wait_screen(&t, "The stream is still live.", 3000) == 0,
              "second response missing");
  ASSERT_TRUE(wait_screen(&t, "+ streaming demo", 10000) == 0,
              "second response did not finish");
  ASSERT_TRUE(write(fd, "three\r", 6) == 6, "third send failed");
  ASSERT_TRUE(wait_screen(&t, "A single line can be", 3000) == 0,
              "third response missing");
  ASSERT_TRUE(wait_screen(&t, "+ streaming demo", 10000) == 0,
              "third response did not finish");
  ASSERT_TRUE(term_row_of(&t, "# A short answer") ==
                      term_row_of(&t, "> one") + 2 &&
                  term_row_of(&t, "> two") ==
                      term_row_of(&t, "Try another prompt.") + 2 &&
                  term_row_of(&t, "## A longer answer") ==
                      term_row_of(&t, "> two") + 2 &&
                  term_row_of(&t, "> three") ==
                      term_row_of(&t, "The stream is still live.") + 2 &&
                  term_row_of(&t, "# Notes") == term_row_of(&t, "> three") + 2,
              "consecutive turns have extra blank rows");
  ASSERT_TRUE(strstr(t.raw, "                    ") == NULL,
              "completed turns wrote literal blank runs to scrollback");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_cancel(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  TEST("chat cancel holds queue until manual promotion");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  if (wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) != 0) {
    int status = 0;
    term_dump(&t);
    fprintf(stderr, "child status: waitpid=%ld status=%d\n",
            (long)waitpid(pid, &status, WNOHANG), status);
    FAIL("editor missing");
  }
  ASSERT_TRUE(write(fd, "work\r", 5) == 5, "send failed");
  if (wait_screen(&t, "A short answer", 3000) != 0) {
    term_dump(&t);
    FAIL("operation missing");
  }
  ASSERT_TRUE(write(fd, "queued\r", 7) == 7, "enqueue failed");
  if (wait_screen(&t, "Q 1. queued", 3000) != 0) {
    int status = 0;
    pid_t waited = waitpid(pid, &status, WNOHANG);
    fprintf(stderr, "cancel chat child wait=%ld status=%d\n", (long)waited,
            status);
    term_dump(&t);
    FAIL("queue item invisible");
  }
  t.seen &= ~(1u << 6);
  ASSERT_TRUE(write(fd, "\003", 1) == 1, "cancel failed");
  ASSERT_TRUE(wait_screen(&t, "Operation cancelled", 3000) == 0,
              "operation not cancelled");
  ASSERT_TRUE(term_row_of(&t, "Reasoning...") < 0,
              "status message was not cleared after cancellation");
  if (wait_screen(&t, "Q 1. queued", 3000) != 0) {
    term_dump(&t);
    FAIL("cancel unexpectedly dequeued turn");
  }
  ASSERT_TRUE(write(fd, "\033\r", 2) == 2, "promotion failed");
  ASSERT_TRUE(wait_screen(&t, "> queued", 3000) == 0,
              "manual promotion missing");
  ASSERT_TRUE(wait_screen(&t, "A longer answer", 3000) == 0,
              "promoted operation did not start");
  {
    size_t mark = t.raw_len;
    ASSERT_TRUE(wait_raw_since(&t, mark, "\033[32m+ ", 6000) == 0,
                "promoted operation did not finish");
  }
  ASSERT_TRUE(write(fd, "/quit\r", 6) == 6, "quit failed");
  ASSERT_TRUE(finish(pid, fd) == 0, "child failed");
  PASS();
}

static void test_chat_resizes_while_streaming(const char *path) {
  struct winsize ws;
  struct terminal t;
  int fd;
  pid_t pid;
  TEST("chat resizes renderer and editor during a live stream");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "resize\r", 7) == 7, "send failed");
  ASSERT_TRUE(wait_screen(&t, "A short answer", 3000) == 0,
              "stream did not start");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 50;
  ws.ws_row = 12;
  ASSERT_TRUE(ioctl(fd, TIOCSWINSZ, &ws) == 0, "resize failed");
  term_resize(&t, 50, 12);
  ASSERT_TRUE(write(fd, "draft", 5) == 5, "draft failed");
  if (wait_screen(&t, "> draft", 3000) != 0) {
    term_dump(&t);
    FAIL("draft lost after midstream resize");
  }
  ASSERT_TRUE(wait_screen(&t, "Next step", 6000) == 0,
              "Markdown stopped after midstream resize");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_queued_steer(const char *path) {
  struct terminal t;
  int fd;
  pid_t pid;
  int first_heading_row;
  int paragraph_row;
  int steer_row;
  int heading_row;
  int valid_seam;
  TEST("chat queues Alt-Enter and inserts steer at a Markdown seam");
  pid = spawn(path, &fd, 120, 30);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 120, 30);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "work\r", 5) == 5, "initial send failed");
  ASSERT_TRUE(wait_screen(&t, "A short answer", 3000) == 0,
              "operation did not start");
  ASSERT_TRUE(write(fd, "queued\rsteer\033\r", 14) == 14,
              "queue and steer input failed");
  ASSERT_TRUE(wait_screen(&t, "S 2. steer", 3000) == 0,
              "steer mode was not shown in the queue");
  ASSERT_TRUE(wait_screen(&t, "> steer", 5000) == 0,
              "steer was not delivered at a seam");
  {
    struct timespec queue_deadline = deadline_after(1500);
    while (!term_contains(&t, "Q 1. queued") &&
           before_deadline(&queue_deadline))
      if (term_read(&t) < 0)
        break;
  }
  if (!term_contains(&t, "Q 1. queued"))
    term_dump(&t);
  ASSERT_TRUE(term_contains(&t, "Q 1. queued"),
              "ordinary queued turn was consumed at the steer seam");
  ASSERT_TRUE(wait_screen(&t, "Next step", 6000) == 0,
              "response did not continue after steer");
  paragraph_row = term_row_of(&t, "Here is italic context");
  steer_row = term_row_of(&t, "> steer");
  heading_row = term_row_of(&t, "Next step");
  first_heading_row = term_row_of(&t, "A short answer");
  valid_seam =
      first_heading_row >= 0 && paragraph_row >= 0 && steer_row >= 0 &&
      heading_row >= 0 &&
      ((steer_row == paragraph_row + 2 && heading_row == steer_row + 2) ||
       (steer_row == first_heading_row + 2 && paragraph_row == steer_row + 2 &&
        heading_row == paragraph_row + 2));
  if (!valid_seam)
    term_dump(&t);
  ASSERT_TRUE(valid_seam,
              "steer split a Markdown paragraph or lacked blank rows");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_steer_after_paragraph(const char *path) {
  int fd;
  pid_t pid;
  struct terminal t;
  int paragraph_row;
  int steer_row;
  int heading_row;
  TEST("chat inserts a late steer with one empty row at the paragraph seam");
  pid = spawn(path, &fd, 120, 30);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 120, 30);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "work\r", 5) == 5, "initial send failed");
  ASSERT_TRUE(wait_screen(&t, "A short answer", 3000) == 0,
              "operation did not start");
  ASSERT_TRUE(wait_screen(&t, "Here is italic", 6000) == 0,
              "paragraph did not start before late steer");
  ASSERT_TRUE(write(fd, "late\033\r", 6) == 6, "late steer input failed");
  ASSERT_TRUE(wait_screen(&t, "S 1. late", 3000) == 0,
              "late steer was not queued");
  ASSERT_TRUE(wait_screen(&t, "> late", 5000) == 0,
              "late steer was not delivered");
  ASSERT_TRUE(wait_screen(&t, "Next step", 6000) == 0,
              "response did not continue after late steer");
  paragraph_row = term_row_of(&t, "Here is italic context");
  steer_row = term_row_of(&t, "> late");
  heading_row = term_row_of(&t, "Next step");
  if (paragraph_row < 0 || steer_row != paragraph_row + 2 ||
      heading_row != steer_row + 2)
    term_dump(&t);
  ASSERT_TRUE(paragraph_row >= 0 && steer_row == paragraph_row + 2 &&
                  heading_row == steer_row + 2,
              "late steer has extra blank rows or split the paragraph");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_steer_after_last_seam_starts_turn(const char *path) {
  struct terminal t;
  int fd;
  pid_t pid;
  TEST("late steer after final seam starts the next response");
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "30", 1) == 0,
              "failed to set stream delay");
  pid = spawn(path, &fd, 120, 30);
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1) == 0,
              "failed to restore stream delay");
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 120, 30);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  ASSERT_TRUE(write(fd, "work\r", 5) == 5, "initial send failed");
  ASSERT_TRUE(wait_screen(&t, "Try another", 6000) == 0,
              "final paragraph did not start");
  ASSERT_TRUE(write(fd, "hello\033\r", 7) == 7, "late steer input failed");
  ASSERT_TRUE(wait_screen(&t, "> hello", 6000) == 0,
              "late steer was not rendered");
  if (wait_screen(&t, "A longer answer", 6000) != 0) {
    term_dump(&t);
    FAIL("late steer did not start a response");
  }
  ASSERT_TRUE(term_row_of(&t, "> hello") >= 0 &&
                  term_row_of(&t, "A longer answer") >
                      term_row_of(&t, "> hello"),
              "late steer response did not follow its prompt");
  ASSERT_TRUE(cancel_and_quit(&t, fd, pid) == 0, "child failed");
  PASS();
}

static void test_chat_native_lifecycle(const char *path, int initial_row) {
  struct terminal t;
  struct timespec deadline;
  char transcript[MAX_ROWS][MAX_COLS + 1];
  const char *draft =
      "abcdefghijklmnopqrstabcdefghijklmnopqrstabcdefghijklmnopqrst";
  const char *shell_output;
  int fd;
  int status;
  int protected_rows;
  unsigned int region_scrolls;
  int tries;
  int turn;
  int row;
  int reaped;
  size_t mark;
  pid_t pid;
  TEST("native chat anchors the prompt, keeps wraps stable, and restores exit");
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "1", 1) == 0,
              "delay setup failed");
  pid = spawn(path, &fd, 40, 24);
  ASSERT_TRUE(setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1) == 0,
              "delay restore failed");
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 40, 24);
  memcpy(t.cells[0], "EXISTING SHELL OUTPUT", 21);
  t.row = initial_row;
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0 &&
                  wait_screen(&t, "> ", 3000) == 0,
              "initial prompt missing");
  ASSERT_TRUE(t.row == 23,
              "initial prompt was not anchored at the terminal bottom");
  /* Longer startup notes may legitimately scroll the original shell row into
   * terminal history. Preserve it there as well as in the visible viewport. */
  shell_output = t.scrolls ? t.history[0] : t.cells[0];
  ASSERT_TRUE(initial_row != 4 ||
                  strncmp(shell_output, "EXISTING SHELL OUTPUT", 21) == 0,
              "startup lost existing terminal transcript");
  for (turn = 0; turn < 3; turn++) {
    mark = t.raw_len;
    ASSERT_TRUE(write(fd, "hello world\r", 12) == 12, "send failed");
    ASSERT_TRUE(wait_raw_since(&t, mark, "\033[32m+ ", 5000) == 0,
                "response did not finish");
    deadline = deadline_after(3000);
    while (before_deadline(&deadline) &&
           (t.row != 23 || t.col != 2 || !t.cursor_visible))
      if (term_read(&t) < 0)
        break;
  }
  ASSERT_TRUE(t.row == 23, "prompt did not park at the bottom after output");
  protected_rows = t.scroll_bottom + 1;
  memcpy(transcript, t.cells, sizeof(transcript));
  region_scrolls = t.region_scrolls;
  mark = t.raw_len;
  ASSERT_TRUE(write(fd, draft, strlen(draft)) == (ssize_t)strlen(draft),
              "draft input failed");
  for (tries = 0; tries < 100; tries++) {
    ASSERT_TRUE(term_read(&t) >= 0, "draft render failed");
    if (t.row == 23 && t.col == 24 &&
        strstr(t.cells[23], "stabcdefghijklmnopqrst"))
      break;
  }
  ASSERT_TRUE(tries < 100 && t.scroll_bottom + 1 == protected_rows - 1,
              "wrapping did not reserve exactly one additional editor row");
  for (row = 0; row <= t.scroll_bottom; row++)
    ASSERT_TRUE(memcmp(transcript[row + t.region_scrolls - region_scrolls],
                       t.cells[row], (size_t)t.cols) == 0,
                "wrapping overwrote surviving transcript cells");
  /* Width reflow is checked in VTE and tmux. This grid model clips
   * columns, so retain only its per-byte editor shrink check here. */
  t.read_chunk = 1;
  protected_rows = t.scroll_bottom + 1;
  memcpy(transcript, t.cells, sizeof(transcript));
  region_scrolls = t.region_scrolls;
  ASSERT_TRUE(write(fd, "\025", 1) == 1, "clear draft failed");
  deadline = deadline_after(3000);
  while (before_deadline(&deadline) && (t.row != 23 || t.col != 2))
    ASSERT_TRUE(term_read(&t) >= 0, "draft clear render failed");
  ASSERT_TRUE(t.row == 23 && t.col == 2 && t.region_scrolls == region_scrolls &&
                  memcmp(transcript, t.cells,
                         (size_t)protected_rows * sizeof(t.cells[0])) == 0,
              "unwrapping the prompt moved transcript rows");
  t.read_chunk = 0;
  ASSERT_TRUE(write(fd, "/quit\r", 6) == 6, "quit failed");
  deadline = deadline_after(5000);
  reaped = 0;
  while (before_deadline(&deadline)) {
    if (term_read(&t) < 0)
      break;
    if (waitpid(pid, &status, WNOHANG) == pid) {
      reaped = 1;
      while (term_read(&t) > 0)
        ;
      break;
    }
  }
  if (!reaped)
    ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "exit wait failed");
  close(fd);
  ASSERT_TRUE(t.farewell_above_status,
              "farewell did not use the message line above the status bar");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "chat exit failed");
  ASSERT_TRUE(t.cursor_visible && t.scroll_top == 0 && t.scroll_bottom == 23 &&
                  t.col == 0 && t.row == 23 && term_contains(&t, "Goodbye.") &&
                  term_contains(&t, "streaming demo") &&
                  term_row_of(&t, "! Goodbye.") <
                      term_row_of(&t, "streaming demo") &&
                  !term_contains(&t, "> /quit") &&
                  strspn(t.cells[23], " ") == (size_t)t.cols,
              "exit did not clear input and keep status on the same row");
  PASS();
}

static void test_chat_eof_goodbye(const char *path, int busy) {
  int fd;
  pid_t pid;
  struct terminal t;
  TEST(busy ? "chat renders farewell before EOF cancels a worker"
            : "chat renders farewell before idle EOF exits");
  pid = spawn(path, &fd, 80, 14);
  ASSERT_TRUE(pid > 0, "spawn failed");
  term_init(&t, fd, 80, 14);
  ASSERT_TRUE(wait_raw(&t, "\033[?2004h", CHAT_READY_TIMEOUT_MS) == 0,
              "editor missing");
  if (busy) {
    ASSERT_TRUE(write(fd, "hello\r", 6) == 6, "send failed");
    ASSERT_TRUE(wait_screen(&t, "Thinking...", 3000) == 0,
                "worker did not start");
  }
  ASSERT_TRUE(write(fd, "\004", 1) == 1, "EOF failed");
  ASSERT_TRUE(wait_raw(&t, "Goodbye.", 3000) == 0 && t.farewell_above_status,
              "EOF farewell missing");
  ASSERT_TRUE(finish(pid, fd) == 0, "EOF did not terminate chat");
  PASS();
}

static void test_chat_non_tty(const char *path) {
  int input[2], output[2], status;
  pid_t pid;
  char bytes[2048], *args[2];
  size_t used;
  ssize_t n;
  TEST("chat emits plain output when redirected");
  ASSERT_TRUE(pipe(input) == 0 && pipe(output) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    close(input[1]);
    close(output[0]);
    (void)dup2(input[0], STDIN_FILENO);
    (void)dup2(output[1], STDOUT_FILENO);
    close(input[0]);
    close(output[1]);
    args[0] = (char *)path;
    args[1] = NULL;
    execv(path, args);
    _exit(127);
  }
  close(input[0]);
  close(output[1]);
  ASSERT_TRUE(
      write(input[1], "hello\nok\n**literal** # heading\n/quit\n", 37) == 37,
      "input failed");
  close(input[1]);
  used = 0;
  while (used < sizeof(bytes) - 1) {
    n = read(output[0], bytes + used, sizeof(bytes) - 1 - used);
    if (n <= 0)
      break;
    used += (size_t)n;
  }
  bytes[used] = 0;
  close(output[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "child failed");
  ASSERT_TRUE(strstr(bytes, "> hello") != NULL, "quote missing");
  ASSERT_TRUE(
      strcmp(bytes, "\n\n> hello\n\n> ok\n\n> **literal** # heading\n\n") == 0,
      "renderer output has more or fewer than one empty prompt row");
  ASSERT_TRUE(strstr(bytes, "\033[") == NULL, "terminal controls in pipe");
  PASS();
}

static void test_chat_piped_input_terminal_output(const char *path) {
  int input[2], master_fd, slave_fd;
  pid_t pid;
  char bytes[2048], *args[2];
  size_t used;
  struct timespec deadline;

  TEST("chat with piped input keeps terminal output line-oriented");
  ASSERT_TRUE(pipe(input) == 0 &&
                  openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "pipe or pty failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    close(input[1]);
    close(master_fd);
    (void)dup2(input[0], STDIN_FILENO);
    (void)dup2(slave_fd, STDOUT_FILENO);
    close(input[0]);
    close(slave_fd);
    args[0] = (char *)path;
    args[1] = NULL;
    execv(path, args);
    _exit(127);
  }
  close(input[0]);
  close(slave_fd);
  ASSERT_TRUE(write(input[1], "hello\n/quit\n", 12) == 12, "input failed");
  close(input[1]);
  used = 0;
  deadline = deadline_after(3000);
  while (before_deadline(&deadline) && used < sizeof(bytes) - 1) {
    fd_set readfds;
    struct timeval timeout;
    ssize_t amount;
    FD_ZERO(&readfds);
    FD_SET(master_fd, &readfds);
    timeout.tv_sec = 0;
    timeout.tv_usec = 50000;
    if (select(master_fd + 1, &readfds, NULL, NULL, &timeout) <= 0)
      continue;
    amount = read(master_fd, bytes + used, sizeof(bytes) - 1 - used);
    if (amount <= 0)
      break;
    used += (size_t)amount;
  }
  bytes[used] = '\0';
  ASSERT_TRUE(finish(pid, master_fd) == 0, "child failed");
  ASSERT_TRUE(strstr(bytes, "> hello") != NULL, "quote missing");
  ASSERT_TRUE(strstr(bytes, "\033[") == NULL,
              "piped chat input activated terminal viewport");
  PASS();
}

struct frame_producer {
  int input;
  int ack;
  int output;
};

static int frame_finite_source(sl_t *sl, void *userdata, const char **bytes,
                               size_t *length) {
  int *sent = userdata;
  (void)sl;
  *bytes = "BATCH\n";
  *length = *sent ? 0 : 6;
  *sent = 1;
  return SL_OK;
}

static int frame_producer_ready(sl_t *sl, const sl_watch_event_t *event,
                                void *userdata) {
  struct frame_producer *producer = userdata;
  char byte;
  int result;
  (void)event;
  if (read(producer->input, &byte, 1) != 1)
    return SL_ERROR_IO;
  if (byte >= 1 && byte <= 4) {
    static const char *const queued[] = {"first", "second", "third", "fourth"};
    result = sl_prompt_queue_append(sl, queued[byte - 1], 0, NULL);
    if (result == SL_OK)
      result = sl_set_status_message(sl, "notice");
  } else if (byte == '-') {
    char *line = NULL;
    result = sl_prompt_queue_take(sl, 0, &line);
    sl_free_string(sl, line);
    if (result == SL_OK)
      result = sl_set_status_message(sl, "notice");
  } else if (byte == '%') {
    int sent = 0;
    result = sl_output_stream_end(sl);
    if (result == SL_OK)
      result = sl_print_above(sl, frame_finite_source, &sent);
    if (result == SL_OK)
      result = sl_output_stream_begin(sl);
  } else if (byte == '=')
    result = sl_set_screen_width(sl, 0);
  else if (byte == '#')
    result = sl_set_status_message(sl, "notice");
  else if (byte == '!')
    result = sl_set_status_message(sl, NULL);
  else if (byte == '@') {
    static const char span[] =
        "wrapped source text fills this row completely\n"
        "line\nline\nline\nline\nline\nline\nline\nline\n"
        "line\nline\nline\nline\nline\nline\nline\nline\nsecond line\nthird";
    result = sl_output_stream_write(sl, span, sizeof(span) - 1);
  } else
    result = sl_output_stream_write(sl, &byte, 1);
  /* The PTY can deliver data after a pipe ACK. Fence the visible frame with
   * an idempotent SGR so the parent observes all preceding terminal bytes. */
  if (result == SL_OK && (write(producer->output, "\033[0;0m", 6) != 6 ||
                          write(producer->ack, "a", 1) != 1))
    return SL_ERROR_IO;
  return result;
}

static int frame_exchange(struct terminal *t, int commands, int ack,
                          char byte) {
  struct timespec deadline = deadline_after(2000);
  int received = 0;
  size_t mark = t->raw_len;
  if (write(commands, &byte, 1) != 1)
    return -1;
  while ((!received || strstr(t->raw + mark, "\033[0;0m") == NULL) &&
         before_deadline(&deadline)) {
    fd_set ready;
    struct timeval timeout = {0, 50000};
    int max_fd = t->fd > ack ? t->fd : ack;
    FD_ZERO(&ready);
    FD_SET(t->fd, &ready);
    FD_SET(ack, &ready);
    if (select(max_fd + 1, &ready, NULL, NULL, &timeout) <= 0)
      continue;
    if (FD_ISSET(t->fd, &ready) && term_read(t) < 0)
      return -1;
    if (FD_ISSET(ack, &ready))
      received = read(ack, &byte, 1) == 1;
  }
  if (!received || strstr(t->raw + mark, "\033[0;0m") == NULL)
    return -1;
  /* An ACK follows the last terminal write; drain those bytes before checking
   * the screen, without relying on a sleep or a matching intermediate cursor.
   */
  for (;;) {
    fd_set ready;
    struct timeval timeout = {0, 0};
    FD_ZERO(&ready);
    FD_SET(t->fd, &ready);
    if (select(t->fd + 1, &ready, NULL, NULL, &timeout) <= 0)
      break;
    if (term_read(t) < 0)
      return -1;
  }
  return 0;
}

static void test_prompt_frames(sl_prompt_theme_t theme, int clear,
                               int destroy) {
  static const char source[] =
      "abcdefghijklmno\n\033[31mparagraph words\033[0m\n";
  struct terminal t;
  struct winsize ws;
  struct frame_producer producer;
  int fd, slave, commands[2], ack[2], status, step;
  int restored = 0, expected_row = 23;
  pid_t pid;
  TEST(theme == SL_PROMPT_THEME_PLAIN
           ? "every producer byte preserves the plain prompt frame"
           : "every producer byte preserves the styled prompt frame");
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = 24;
  ws.ws_col = 40;
  ASSERT_TRUE(openpty(&fd, &slave, NULL, NULL, &ws) == 0 &&
                  pipe(commands) == 0 && pipe(ack) == 0,
              "PTY setup failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t config;
    sl_t *sl;
    sl_watch_id_t watch;
    char *line;
    const char *elements[] = {"fixture"};
    close(fd);
    close(commands[1]);
    close(ack[0]);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = theme;
    config.clear_prompt_on_exit = clear;
    config.prompt_queue = 1;
    config.statusline = 1;
    sl = sl_create_with_config(&config);
    producer.input = commands[0];
    producer.ack = ack[1];
    producer.output = slave;
    if (!sl || sl_set_status_elements(sl, elements, 1) != SL_OK ||
        sl_set_status_message(sl, "notice") != SL_OK ||
        sl_prompt_queue_append(sl, "queued", 0, NULL) != SL_OK ||
        sl_output_stream_begin(sl) != SL_OK ||
        sl_output_stream_write(sl, "header\n", 7) != SL_OK ||
        sl_watch_add(sl, commands[0], SL_WATCH_READ, frame_producer_ready,
                     &producer, &watch) != SL_OK)
      _exit(2);
    line = sl_readline(sl, "> ");
    if (!line || strcmp(line, "draft\nmore") != 0 ||
        (!destroy && sl_output_stream_end(sl) != SL_OK))
      _exit(3);
    sl_free_string(sl, line);
    sl_destroy(sl);
    _exit(0);
  }
  close(slave);
  close(commands[0]);
  close(ack[1]);
  term_init(&t, fd, 40, 24);
  t.row = 3;
  ASSERT_TRUE(wait_screen(&t, "> ", 3000) == 0, "initial prompt missing");
  ASSERT_TRUE(write(fd, "draft\nmore", 10) == 10, "draft input failed");
  ASSERT_TRUE(wait_screen(&t, "more", 3000) == 0, "multiline draft missing");
  while (term_read(&t) > 0)
    ;
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '#') == 0,
              "initial frame did not settle before resize");
  t.guard[0] = "> draft";
  t.guard[1] = "more";
  t.guard[2] = "fixture";
  t.guard[3] = "notice";
  t.guard[4] = "queued";
  for (step = 0; step < 3; step++) {
    static const int fitted_widths[] = {38, 36, 40};
    size_t mark = t.raw_len;
    unsigned int regions = t.region_updates;
    ws.ws_col = (unsigned short)fitted_widths[step];
    ASSERT_TRUE(ioctl(fd, TIOCSWINSZ, &ws) == 0, "fitted frame resize failed");
    term_resize(&t, ws.ws_col, ws.ws_row);
    ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '#') == 0,
                "fitted frame resize did not finish");
    if (t.row != t.rows - 1 || t.col != 6 || !t.cursor_visible) {
      fprintf(stderr, "fitted step %d cursor %d,%d theme %d\n", step, t.row,
              t.col, (int)theme);
      term_dump(&t);
      FAIL("fitted resize displaced the input cursor");
    }
    ASSERT_TRUE(strstr(t.raw + mark, "\0337") == NULL &&
                    strstr(t.raw + mark, "\0338") == NULL &&
                    strstr(t.raw + mark, "\033[s") == NULL &&
                    strstr(t.raw + mark, "\033[u") == NULL &&
                    strstr(t.raw + mark, "\033[2K") == NULL &&
                    strstr(t.raw + mark, "\033[0K") == NULL &&
                    strstr(t.raw + mark, "> draft") == NULL &&
                    strstr(t.raw + mark, "fixture") == NULL &&
                    strstr(t.raw + mark, "notice") == NULL &&
                    strstr(t.raw + mark, "queued") == NULL &&
                    strstr(t.raw + mark, "more") == NULL,
                "width resize erased or repainted a fitted prompt frame");
    if (t.region_updates != regions + 1 || t.scroll_top != 0 ||
        t.scroll_bottom != t.rows - 6 ||
        strstr(t.raw + mark, "\033[65535;7H") == NULL) {
      fprintf(stderr, "fitted resize step %d, theme %d, clear %d: |%s|\n", step,
              (int)theme, clear, t.raw + mark);
      term_dump(&t);
      FAIL(
          "fitted resize did not restore the reserved margin and input cursor");
    }
  }
  for (step = 0; step < 480; step++) {
    char byte = source[(size_t)step % (sizeof(source) - 1)];
    size_t mark;
    if (step > 0 && step % (int)(sizeof(source) - 1) == 0) {
      int cycle = step / (int)(sizeof(source) - 1);
      int row, shift, transcript_rows = t.scroll_bottom + 1;
      unsigned int scrolls;
      char transcript[MAX_ROWS][MAX_COLS + 1];
      ws.ws_col = cycle % 2 ? 30 : 40;
      ws.ws_row = cycle % 3 == 0 ? 18 : cycle % 3 == 1 ? 28 : 24;
      ASSERT_TRUE(ioctl(fd, TIOCSWINSZ, &ws) == 0, "frame resize failed");
      if (t.row >= ws.ws_row)
        transcript_rows -= t.row - ws.ws_row + 1;
      term_resize(&t, ws.ws_col, ws.ws_row);
      expected_row = t.rows - 1;
      memcpy(transcript, t.cells, sizeof(transcript));
      scrolls = t.region_scrolls;
      /* Layout changes may reflow the prompt; feed guards start after that
       * frame is settled. No transcript replay is allowed during resize. */
      memset(t.guard, 0, sizeof(t.guard));
      ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '#') == 0,
                  "resize did not finish");
      ASSERT_TRUE(t.row == expected_row && t.col == 6 && t.cursor_visible,
                  "resize displaced the editor cursor");
      shift = (int)(t.region_scrolls - scrolls);
      for (row = 0; row + shift < transcript_rows && row <= t.scroll_bottom;
           row++) {
        if (memcmp(t.cells[row], transcript[row + shift], (size_t)t.cols) !=
            0) {
          fprintf(stderr, "cycle %d row %d: before |%s| after |%s|\n", cycle,
                  row, transcript[row], t.cells[row]);
          FAIL("prompt resize changed transcript cells");
        }
      }
      t.guard[0] = "> draft";
      t.guard[1] = "more";
      t.guard[2] = "fixture";
      t.guard[3] = "notice";
      t.guard[4] = "queued";
    }
    mark = t.raw_len;
    ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '=') == 0 &&
                    t.raw_len == mark + 6 &&
                    strcmp(t.raw + mark, "\033[0;0m") == 0,
                "native width setter repainted an unchanged prompt");
    if (step == 0 || step == (int)(sizeof(source) - 1) * 8)
      byte = '@';
    if (step == (int)(sizeof(source) - 1) * 3)
      byte = '%';
    ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], byte) == 0,
                "producer feed did not complete");
    if (t.frame_failure) {
      term_dump(&t);
      fprintf(stderr, "feed %d lost prompt content at byte %lu\n", step,
              (unsigned long)t.frame_failure);
      (void)kill(pid, SIGTERM);
      (void)waitpid(pid, &status, 0);
      close(fd);
      close(commands[1]);
      close(ack[0]);
      FAIL("a producer feed exposed an incomplete prompt frame");
    }
    ASSERT_TRUE(strstr(t.raw + mark, "\0337") == NULL &&
                    strstr(t.raw + mark, "\0338") == NULL &&
                    strstr(t.raw + mark, "\033[s") == NULL &&
                    strstr(t.raw + mark, "\033[u") == NULL &&
                    strstr(t.raw + mark, "\033[2K") == NULL &&
                    strstr(t.raw + mark, "\033[0K") == NULL &&
                    strstr(t.raw + mark, "\033[?25l") == NULL &&
                    strstr(t.raw + mark, "\033[?25h") == NULL &&
                    strstr(t.raw + mark, "\033[6n") == NULL &&
                    strstr(t.raw + mark, "fixture") == NULL &&
                    strstr(t.raw + mark, "notice") == NULL &&
                    strstr(t.raw + mark, "> draft") == NULL &&
                    strstr(t.raw + mark, "queued") == NULL,
                "feed erased or repainted an unchanged prompt frame");
    if (t.row == expected_row)
      restored++;
    ASSERT_TRUE(t.cursor_visible && t.col == 6,
                "feed did not restore the editor cursor");
  }
  ASSERT_TRUE(restored == 480 && t.frames > 1000,
              "producer feeds moved the terminal-positioned prompt");
  memset(t.guard, 0, sizeof(t.guard));
  {
    char before[MAX_ROWS][MAX_COLS + 1];
    size_t exit_mark = t.raw_len;
    unsigned int scrolls = t.region_scrolls;
    int row;
    struct timespec deadline = deadline_after(3000);
    int reaped = 0;
    memcpy(before, t.cells, sizeof(before));
    if (!clear) {
      t.guard[0] = "fixture";
      t.guard[1] = "notice";
      t.guard[2] = "queued";
    }
    ASSERT_TRUE(write(fd, "\r", 1) == 1, "submit failed");
    while (before_deadline(&deadline)) {
      (void)term_read(&t);
      if (waitpid(pid, &status, WNOHANG) == pid) {
        reaped = 1;
        while (term_read(&t) > 0)
          ;
        break;
      }
    }
    ASSERT_TRUE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "frame producer failed");
    ASSERT_TRUE(t.cursor_visible && t.col == 0 && t.scroll_top == 0 &&
                    t.scroll_bottom == t.rows - 1,
                "exit did not restore the terminal cursor and margins");
    ASSERT_TRUE(!term_contains(&t, "> draft") && !term_contains(&t, "more"),
                "exit left input cells visible");
    if (!clear) {
      ASSERT_TRUE(t.row == expected_row && t.region_scrolls == scrolls &&
                      strstr(t.raw + exit_mark, "\n") == NULL &&
                      !t.frame_failure,
                  "exit scrolled, advanced a row, or erased status cells");
      for (row = 0; row < expected_row - 1; row++)
        ASSERT_TRUE(memcmp(t.cells[row], before[row], (size_t)t.cols) == 0,
                    "exit changed cells above the editor");
      ASSERT_TRUE(strspn(t.cells[expected_row - 1], " ") == (size_t)t.cols &&
                      strspn(t.cells[expected_row], " ") == (size_t)t.cols,
                  "exit did not clear all wrapped editor rows");
    } else {
      ASSERT_TRUE(!term_contains(&t, "fixture") &&
                      !term_contains(&t, "notice") &&
                      !term_contains(&t, "queued"),
                  "full-clear exit left status or queue rows visible");
    }
    close(fd);
  }
  close(commands[1]);
  close(ack[0]);
  PASS();
}

static void
test_queue_after_output_fills_prompt_region(sl_prompt_theme_t theme) {
  struct terminal t;
  struct winsize ws;
  struct frame_producer producer;
  int fd, slave, commands[2], ack[2], i;
  int transcript_rows;
  unsigned int scrolls;
  size_t mark;
  char transcript[MAX_ROWS][MAX_COLS + 1];
  /* Clear the eleven actual draft lines using line-local Ctrl-U and
   * Backspace for each of the ten newline boundaries. */
  const char replace_draft[] = "\025\177\025\177\025\177\025\177\025\177"
                               "\025\177\025\177\025\177\025\177\025\177"
                               "\025draft\nmore\nshortened";
  pid_t pid;
  TEST(
      theme == SL_PROMPT_THEME_PLAIN
          ? "queue appears after native output fills the plain scroll region"
          : "queue appears after native output fills the styled scroll region");
  memset(&ws, 0, sizeof(ws));
  ws.ws_row = 14;
  ws.ws_col = 40;
  ASSERT_TRUE(openpty(&fd, &slave, NULL, NULL, &ws) == 0 &&
                  pipe(commands) == 0 && pipe(ack) == 0,
              "PTY setup failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t config;
    sl_t *sl;
    sl_watch_id_t watch;
    char *line;
    const char *elements[] = {"fixture"};
    close(fd);
    close(commands[1]);
    close(ack[0]);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = theme;
    config.prompt_queue = 1;
    config.statusline = 1;
    sl = sl_create_with_config(&config);
    producer.input = commands[0];
    producer.ack = ack[1];
    producer.output = slave;
    if (!sl || sl_set_prompt_queue(sl, 1, 4, 3) != SL_OK ||
        sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) !=
            SL_OK ||
        sl_set_status_elements(sl, elements, 1) != SL_OK ||
        sl_output_stream_begin(sl) != SL_OK ||
        sl_watch_add(sl, commands[0], SL_WATCH_READ, frame_producer_ready,
                     &producer, &watch) != SL_OK)
      _exit(2);
    line = sl_readline(sl, "> ");
    if (!line || strcmp(line, "done") != 0)
      _exit(3);
    sl_free_string(sl, line);
    sl_destroy(sl);
    _exit(0);
  }
  close(slave);
  close(commands[0]);
  close(ack[1]);
  term_init(&t, fd, 40, 14);
  ASSERT_TRUE(wait_screen(&t, "> ", 3000) == 0, "initial prompt missing");
  ASSERT_TRUE(t.scroll_bottom == 11 && strstr(t.cells[12], "fixture"),
              "empty queue or status message reserved unused rows");
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], 'p') == 0 &&
                  frame_exchange(&t, commands[1], ack[0], 1) == 0 &&
                  t.scroll_bottom == 9 && t.region_scrolls == 0 &&
                  t.cells[0][0] == 'p',
              "prompt growth scrolled output despite available space");
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '-') == 0 &&
                  frame_exchange(&t, commands[1], ack[0], '!') == 0 &&
                  t.scroll_bottom == 11 && t.region_scrolls == 0 &&
                  t.cells[0][0] == 'p',
              "sparse prompt shrink moved output or retained unused rows");
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '@') == 0,
              "output did not fill its region");
  ASSERT_TRUE(strncmp(t.cells[t.scroll_bottom], "third", 5) == 0,
              "fixture did not park the producer at its bottom margin");
  transcript_rows = t.scroll_bottom + 1;
  memcpy(transcript, t.cells, sizeof(transcript));
  scrolls = t.region_scrolls;
  mark = t.raw_len;
  for (i = 1; i <= 4; i++) {
    ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], (char)i) == 0,
                "enqueue did not render");
    ASSERT_TRUE(t.scroll_bottom == 10 - i &&
                    t.region_scrolls == scrolls + (unsigned int)i + 1,
                "queue growth did not use exactly its rendered rows");
    ASSERT_TRUE(strncmp(t.cells[t.scroll_bottom], "third", 5) == 0,
                "queue growth lost the live output row");
    ASSERT_TRUE(term_contains(&t, "Q 1. first"), "first queue entry hidden");
    if (i >= 2)
      ASSERT_TRUE(term_contains(&t, "Q 2. second"),
                  "second queue entry hidden");
    if (i >= 3)
      ASSERT_TRUE(term_contains(&t, "Q 3. third"), "third queue entry hidden");
    if (i == 4)
      ASSERT_TRUE(term_contains(&t, "... 1 more"), "queue overflow hidden");
  }
  ASSERT_TRUE(write(fd, "draft\nmore\nlast", 15) == 15, "draft input failed");
  ASSERT_TRUE(wait_screen(&t, "last", 3000) == 0, "multiline editor missing");
  ASSERT_TRUE(
      term_contains(&t, "Q 1. first") && term_contains(&t, "Q 2. second") &&
          term_contains(&t, "Q 3. third") && term_contains(&t, "... 1 more"),
      "multiline draft displaced the queue panel");
  ASSERT_TRUE(term_contains(&t, "fixture") && term_contains(&t, "notice"),
              "queue displaced status rows");
  ASSERT_TRUE(t.scroll_bottom == 4 && t.region_scrolls == scrolls + 7,
              "multiline draft did not use exactly its rendered rows");
  for (i = 0; i <= t.scroll_bottom; i++)
    ASSERT_TRUE(memcmp(t.cells[i], transcript[i + 7], (size_t)t.cols) == 0,
                "queue growth overwrote surviving transcript cells");
  ASSERT_TRUE(strstr(t.raw + mark, "second line") == NULL,
              "prompt growth replayed producer text");
  t.guard[0] = "Q 1. first";
  t.guard[1] = "Q 2. second";
  t.guard[2] = "Q 3. third";
  t.guard[3] = "... 1 more";
  t.guard[4] = "last";
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], 'x') == 0 &&
                  !t.frame_failure &&
                  strncmp(t.cells[t.scroll_bottom], "thirdx", 6) == 0,
              "live output lost its position or erased queued previews");
  memset(t.guard, 0, sizeof(t.guard));
  ASSERT_TRUE(write(fd, "\n1\n2\n3\n4\n5\n6\n7\ntail", 19) == 19 &&
                  wait_screen(&t, "tail", 3000) == 0 &&
                  frame_exchange(&t, commands[1], ack[0], '#') == 0,
              "oversized editor did not render");
  ASSERT_TRUE(t.scroll_bottom == 1 && term_contains(&t, "tail") &&
                  term_contains(&t, "Q 1. first") &&
                  term_contains(&t, "Q 3. third") &&
                  term_contains(&t, "... 1 more") &&
                  term_contains(&t, "fixture") && term_contains(&t, "notice"),
              "editor overflow hid queue or consumed required output rows");
  scrolls = t.region_scrolls;
  ASSERT_TRUE(write(fd, replace_draft, sizeof(replace_draft) - 1) ==
                      (ssize_t)(sizeof(replace_draft) - 1) &&
                  wait_screen(&t, "shortened", 3000) == 0 &&
                  frame_exchange(&t, commands[1], ack[0], '#') == 0,
              "multiline replacement input failed");
  if (t.scroll_bottom != 4 || t.region_scrolls != scrolls) {
    term_dump(&t);
    fprintf(stderr, "replacement margin=%d scrolls=%u expected=%u\n",
            t.scroll_bottom, t.region_scrolls, scrolls);
  }
  ASSERT_TRUE(t.scroll_bottom == 4 && t.region_scrolls == scrolls,
              "editor paging retained unused rows after shrink");
  transcript_rows = t.scroll_bottom + 1;
  memcpy(transcript, t.cells, sizeof(transcript));
  scrolls = t.region_scrolls;
  for (i = 0; i < 4; i++) {
    ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '-') == 0 &&
                    t.scroll_bottom == 5 + i && t.region_scrolls == scrolls,
                "dequeue did not release exactly one row without scrolling");
    ASSERT_TRUE(memcmp(transcript, t.cells,
                       (size_t)transcript_rows * sizeof(t.cells[0])) == 0,
                "shrinking the queue moved producer cells");
    ASSERT_TRUE(strspn(t.cells[t.scroll_bottom], " ") == (size_t)t.cols,
                "freed output row still contains prompt text");
  }
  ASSERT_TRUE(!term_contains(&t, "Q 1.") && !term_contains(&t, "... 1 more"),
              "empty queue left stale preview cells");
  ASSERT_TRUE(write(fd, "\025\177\025\177\025done", 9) == 9 &&
                  wait_screen(&t, "> done", 3000) == 0,
              "draft clear failed");
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '#') == 0 &&
                  t.scroll_bottom == 10 && t.region_scrolls == scrolls,
              "draft shrink retained unused editor rows");
  ASSERT_TRUE(frame_exchange(&t, commands[1], ack[0], '!') == 0 &&
                  t.scroll_bottom == 11 && t.region_scrolls == scrolls &&
                  !term_contains(&t, "notice"),
              "cleared status message retained an unused row");
  ASSERT_TRUE(write(fd, "\r", 1) == 1 && finish(pid, fd) == 0, "child failed");
  close(commands[1]);
  close(ack[0]);
  PASS();
}

/* Direct runs (including Valgrind) must not inherit a user's history store.
 * Keep a fresh private store under this build, regardless of the caller's cwd
 * or SOFTLINE_HISTORY_DIR; retain it for diagnostics until make clean. */
static int isolate_example_history(void) {
  const char suffix[] = "/example-history.XXXXXX";
  char *directory;
  int result;
  directory = (char *)malloc(strlen(SL_TEST_WORK_DIR) + sizeof(suffix));
  if (!directory)
    return -1;
  strcpy(directory, SL_TEST_WORK_DIR);
  strcat(directory, suffix);
  result =
      mkdtemp(directory) ? setenv("SOFTLINE_HISTORY_DIR", directory, 1) : -1;
  free(directory);
  return result;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc == 2 && strcmp(argv[1], "frames") == 0) {
    test_queue_after_output_fills_prompt_region(SL_PROMPT_THEME_PLAIN);
    test_queue_after_output_fills_prompt_region(SL_PROMPT_THEME_DEFAULT);
    test_prompt_frames(SL_PROMPT_THEME_PLAIN, 0, 0);
    test_prompt_frames(SL_PROMPT_THEME_DEFAULT, 0, 1);
    test_prompt_frames(SL_PROMPT_THEME_PLAIN, 1, 0);
    test_prompt_frames(SL_PROMPT_THEME_DEFAULT, 1, 1);
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc != 3 && argc != 4)
    return 2;
  if (isolate_example_history() != 0) {
    perror("could not isolate example history under the build directory");
    return 1;
  }
  if (setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1) != 0)
    return 1;
  printf("softline example integration tests\n");
  if (argc == 4) {
    if (strcmp(argv[3], "frames") == 0)
      test_prompt_frames(SL_PROMPT_THEME_PLAIN, 0, 0);
    else if (strcmp(argv[3], "resize") == 0)
      test_chat_resizes_while_streaming(argv[2]);
    else if (strcmp(argv[3], "word") == 0)
      test_chat_long_unbroken_editor_word(argv[2]);
    else if (strcmp(argv[3], "spacing") == 0)
      test_chat_preserves_transcript_and_prompt_spacing(argv[2]);
    else
      test_chat_native_lifecycle(argv[2],
                                 strcmp(argv[3], "bottom") == 0 ? 23 : 4);
    return tests_passed == tests_run ? 0 : 1;
  }
  test_chat_native_lifecycle(argv[2], 4);
  test_chat_native_lifecycle(argv[2], 23);
  test_chat_eof_goodbye(argv[2], 0);
  test_chat_eof_goodbye(argv[2], 1);
  test_simple(argv[1]);
  test_chat_live_queue(argv[2]);
  test_chat_long_unbroken_editor_word(argv[2]);
  test_chat_queued_quit(argv[2]);
  test_chat_promoted_quit(argv[2]);
  test_chat_steered_quit(argv[2]);
  test_chat_preserves_transcript_and_prompt_spacing(argv[2]);
  test_chat_spacing_across_turns(argv[2]);
  test_chat_cancel(argv[2]);
  test_chat_resizes_while_streaming(argv[2]);
  test_chat_queued_steer(argv[2]);
  test_chat_steer_after_paragraph(argv[2]);
  test_chat_steer_after_last_seam_starts_turn(argv[2]);
  test_chat_non_tty(argv[2]);
  test_chat_piped_input_terminal_output(argv[2]);
  printf("%d/%d tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
