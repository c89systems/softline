#define _DEFAULT_SOURCE
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include "silent_cursor_probe.h"
#include "softline/transcript.h"
#include "termios_snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <vterm.h>

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      fprintf(stderr, "transcript line %d: %s\n", __LINE__, #expression);      \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
#define TEXT(sl, id, text)                                                     \
  sl_transcript_write(sl, id, SL_TRANSCRIPT_TEXT, text, sizeof(text) - 1)
#define VT(sl, id, text)                                                       \
  sl_transcript_write(sl, id, SL_TRANSCRIPT_VT, text, sizeof(text) - 1)
#define CHAT(sl, text) sl_output_stream_write(sl, text, sizeof(text) - 1)

enum { CHAT_BACKLOG_LIMIT = 256 * 1024, CAPTURE_LIMIT = 16384 };

typedef struct fixture {
  sl_transcript_id_t log, worker;
  int master, phase, handoff, viewer_idle_ticks;
  double queue_started;
  size_t length;
  char output[CAPTURE_LIMIT];
  pthread_mutex_t capture_lock;
  pthread_t reader;
  int stop[2];
  size_t pending_length;
  char pending[CAPTURE_LIMIT];
  VTerm *terminal;
} fixture_t;

static volatile sig_atomic_t restored;

static void returning_handler(int signal_number) {
  (void)signal_number;
  restored = 1;
}

static void keys(fixture_t *f, const char *keys) {
  CHECK(write(f->master, keys, strlen(keys)) == (ssize_t)strlen(keys));
}

static void read_output(fixture_t *f) {
  ssize_t n;
  do {
    CHECK(f->pending_length < sizeof(f->pending));
    n = read(f->master, f->pending + f->pending_length,
             sizeof(f->pending) - f->pending_length);
    if (n > 0)
      f->pending_length += (size_t)n;
  } while (n > 0 || (n < 0 && errno == EINTR));
  CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}

/* A synchronous transcript replay can exceed the PTY's output buffer.
 * Drain it even while the editor is inside the replay call. */
static void *read_output_thread(void *userdata) {
  fixture_t *f = userdata;
  struct pollfd fds[2];
  fds[0].fd = f->master;
  fds[0].events = POLLIN;
  fds[1].fd = f->stop[0];
  fds[1].events = POLLIN;
  for (;;) {
    int ready = poll(fds, 2, -1);
    if (ready < 0 && errno == EINTR)
      continue;
    CHECK(ready > 0);
    if (fds[1].revents)
      return NULL;
    CHECK(fds[0].revents & POLLIN);
    CHECK(!pthread_mutex_lock(&f->capture_lock));
    read_output(f);
    CHECK(!pthread_mutex_unlock(&f->capture_lock));
  }
}

static void capture(fixture_t *f) {
  CHECK(!pthread_mutex_lock(&f->capture_lock));
  read_output(f);
  CHECK(f->pending_length < sizeof(f->output) - f->length);
  if (f->terminal)
    vterm_input_write(f->terminal, f->pending, f->pending_length);
  memcpy(f->output + f->length, f->pending, f->pending_length);
  f->length += f->pending_length;
  f->pending_length = 0;
  f->output[f->length] = 0;
  CHECK(!pthread_mutex_unlock(&f->capture_lock));
}

static void advance(fixture_t *f, const char *input) {
  ++f->phase;
  f->length = 0;
  keys(f, input);
}

static void screen_text(const fixture_t *f, int row, int start, char *text,
                        size_t capacity) {
  VTermRect rect = {row, row + 1, start, 20};
  size_t used = vterm_screen_get_text(vterm_obtain_screen(f->terminal), text,
                                      capacity - 1, rect);
  text[used] = 0;
}

static int screen_has(const fixture_t *f, const char *needle, int highlight) {
  VTermScreen *screen = vterm_obtain_screen(f->terminal);
  int rows, columns, row, col;
  char text[128];
  vterm_get_size(f->terminal, &rows, &columns);
  for (row = 0; row < rows; ++row) {
    if (!highlight) {
      screen_text(f, row, 0, text, sizeof(text));
      if (strstr(text, needle))
        return 1;
    } else {
      for (col = 0; col < columns; ++col) {
        VTermPos pos = {row, col};
        VTermScreenCell cell;
        CHECK(vterm_screen_get_cell(screen, pos, &cell));
        if (VTERM_COLOR_IS_INDEXED(&cell.bg) &&
            cell.bg.indexed.idx == highlight) {
          screen_text(f, row, col, text, sizeof(text));
          if (!strncmp(text, needle, strlen(needle)))
            return 1;
        }
      }
    }
  }
  return 0;
}

static void top_row(const fixture_t *f, int row) {
  char expected[16], text[128];
  snprintf(expected, sizeof(expected), "row%02d", row);
  screen_text(f, 1, 0, text, sizeof(text));
  CHECK(!strncmp(text, expected, strlen(expected)));
}

static void numbered_rows(sl_t *sl, sl_transcript_id_t id) {
  int i;
  for (i = 0; i <= 24; ++i) {
    char row[64];
    const char *tail = i == 2    ? " \033[31mNeEd\033[0mLe"
                       : i == 8  ? " needle NEEDLE"
                       : i == 23 ? "                        Café 中"
                                 : "";
    snprintf(row, sizeof(row), "%srow%02d%s", i ? "\r\n" : "", i, tail);
    CHECK(sl_transcript_write(sl, id, SL_TRANSCRIPT_VT, row, strlen(row)) ==
          SL_OK);
  }
}

static void drive_navigation(sl_t *sl, void *context) {
  static const struct {
    const char *key;
    int top;
  } steps[] = {{"g", 0},      {"k", 0},      {"j", 1},       {"J", 6},
               {"K", 1},      {"d", 4},      {"u", 1},       {" ", 6},
               {"f", 11},     {"b", 6},      {"\033[5~", 1}, {"\033[6~", 6},
               {"\033[A", 5}, {"\033[B", 6}, {"\033[H", 0},  {"\033[F", 19},
               {"J", 19},     {"g", 0}};
  fixture_t *f = context;
  int count = (int)(sizeof(steps) / sizeof(*steps));
  int phase = f->phase - count;
  capture(f);
  if (!f->phase) {
    numbered_rows(sl, f->log);
    advance(f, "draft\024");
    return;
  }
  if (f->phase <= count) {
    top_row(f, f->phase == 1 ? 19 : steps[f->phase - 2].top);
    if (f->phase > 1)
      CHECK(!strstr(f->output, "\033[2J"));
    if (f->phase > 2 && steps[f->phase - 2].top == steps[f->phase - 3].top)
      CHECK(!f->length);
    advance(f, steps[f->phase - 1].key);
    return;
  }
  if (phase == 28) {
    CHECK(!sl_transcript_visible(sl) && strstr(f->output, "chat-restored"));
    advance(f, "\024");
    return;
  }
  if (phase == 30) {
    CHECK(!sl_transcript_visible(sl));
    advance(f, "\024");
    return;
  }
  if (phase == 32) {
    CHECK(!sl_transcript_visible(sl));
    advance(f, "qj/Gc\r");
    return;
  }
  CHECK(sl->transcript_visible(sl));
  switch (phase) {
  case 1: {
    struct winsize size = {9, 20, 0, 0};
    top_row(f, 0);
    CHECK(!ioctl(f->master, TIOCSWINSZ, &size));
    vterm_set_size(f->terminal, 9, 20);
    advance(f, "");
    break;
  }
  case 2:
    advance(f, "d");
    break;
  case 3:
    top_row(f, 4);
    advance(f, "u");
    break;
  case 4:
    top_row(f, 0);
    advance(f, "/needle\r");
    break;
  case 5:
    CHECK(screen_has(f, "NeEdLe", 11));
    advance(f, "Nn");
    break;
  case 6:
    top_row(f, 1);
    CHECK(!screen_has(f, "No further match", 0) &&
          screen_has(f, "needle", 11) && screen_has(f, "NEEDLE", 3));
    advance(f, "n");
    break;
  case 7:
    CHECK(screen_has(f, "needle", 3) && screen_has(f, "NEEDLE", 11));
    advance(f, "/qjdG");
    break;
  case 8:
    CHECK(screen_has(f, "/qjdG", 0) && screen_has(f, "NEEDLE", 11));
    advance(f, "\033");
    break;
  case 9:
    top_row(f, 1);
    CHECK(!screen_has(f, "/qjdG", 0) && screen_has(f, "NEEDLE", 11));
    advance(f, "N");
    break;
  case 10:
    CHECK(screen_has(f, "needle", 11));
    advance(f, "/CAFé 中\r");
    break;
  case 11:
    CHECK(screen_has(f, "Café 中", 11));
    advance(f, "n");
    break;
  case 12:
    CHECK(screen_has(f, "No further match", 0) && screen_has(f, "Café 中", 11));
    CHECK(VT(sl, f->log, "\033[5;43H!") == SL_OK);
    advance(f, "");
    break;
  case 13:
    CHECK(!screen_has(f, "", 11) && screen_has(f, "Café 中", 3));
    CHECK(sl_transcript_reset(sl, f->log) == SL_OK);
    numbered_rows(sl, f->log);
    advance(f, "/");
    break;
  case 14:
    top_row(f, 17);
    CHECK(!screen_has(f, "", 3) && !screen_has(f, "", 11));
    CHECK(TEXT(sl, f->log, "\nrow25 newneedle\nrow26") == SL_OK);
    advance(f, "");
    break;
  case 15:
    top_row(f, 17);
    CHECK(!f->length && !screen_has(f, "newneedle", 0));
    advance(f, "newneedle\r");
    break;
  case 16: {
    int i;
    CHECK(screen_has(f, "newneedle", 11));
    for (i = 0; i < 2100; ++i)
      CHECK(TEXT(sl, f->log, "\nx") == SL_OK);
    CHECK(TEXT(sl, f->log, "\nrow27 newneedle") == SL_OK);
    advance(f, "");
    break;
  }
  case 17:
    CHECK(!screen_has(f, "row25", 0) && !screen_has(f, "", 11));
    advance(f, "n");
    break;
  case 18:
    CHECK(screen_has(f, "row27 ", 0) && screen_has(f, "newneedle", 11));
    advance(f, "G");
    break;
  case 19:
    CHECK(screen_has(f, "row27 ", 0) && screen_has(f, "newneedle", 3) &&
          !screen_has(f, "", 11));
    CHECK(CHAT(sl, "chat-restored\n") == SL_OK);
    CHECK(TEXT(sl, f->log, "\nrow28 tail-follow") == SL_OK);
    advance(f, "");
    break;
  case 20:
    CHECK(screen_has(f, "row28 tail-follow", 0));
    advance(f, "c");
    break;
  case 21:
    CHECK(screen_has(f, "row28 tail-follow", 0) &&
          screen_has(f, "row27 newneedle", 0) && !screen_has(f, "", 3) &&
          !screen_has(f, "", 11));
    CHECK(TEXT(sl, f->log, "\nrow29 newneedle") == SL_OK);
    advance(f, "n");
    break;
  case 22:
    CHECK(screen_has(f, "row29 newneedle", 0) && !screen_has(f, "", 3) &&
          !screen_has(f, "", 11) && !screen_has(f, "No further match", 0));
    CHECK(VT(sl, f->log, "\033[6;1Hshort\033[K") == SL_OK);
    advance(f, "");
    break;
  case 23:
    CHECK(!strcmp(f->output, "\033[9;1H\033[0mshort\033[0m\033[K"));
    CHECK(screen_has(f, "row28 tail-follow", 0) &&
          !screen_has(f, "row29 newneedle", 0));
    CHECK(VT(sl, f->log, "\033[6;1H12345678901234567890") == SL_OK);
    advance(f, "");
    break;
  case 24:
    CHECK(!strstr(f->output, "\033[K") &&
          screen_has(f, "12345678901234567890", 0));
    advance(f, "/absent-target\r");
    break;
  case 25: {
    struct winsize size = {9, 80, 0, 0};
    CHECK(screen_has(f, "12345678901234567890", 0) &&
          screen_has(f, "No further match", 0));
    CHECK(!ioctl(f->master, TIOCSWINSZ, &size));
    vterm_set_size(f->terminal, 9, 80);
    advance(f, "");
    break;
  }
  case 26:
    CHECK(strstr(f->output, "c clear  nN match"));
    advance(f, "c");
    break;
  case 27:
    CHECK(strstr(f->output, "/ search  q quit") &&
          !strstr(f->output, "c clear") && !strstr(f->output, "nN match"));
    advance(f, "q");
    break;
  case 29:
    advance(f, "\033");
    break;
  case 31:
    advance(f, "\003");
    break;
  default:
    CHECK(0);
  }
}

static double milliseconds(void) {
  struct timespec now;
  CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}

static int shell_key(sl_t *sl, sl_key_t key, void *context,
                     sl_key_action_t *action) {
  fixture_t *f = context;
  (void)key;
  capture(f);
  CHECK(!sl_transcript_visible(sl) && strstr(f->output, "\033[?1049l"));
  CHECK(!strstr(f->output, "evicted-line") &&
        strstr(f->output, "oldest-kept-line") &&
        strstr(f->output, "newest-kept-line"));
  ++f->handoff;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int finite_note(sl_t *sl, void *context, const char **chunk,
                       size_t *length) {
  int *calls = context;
  (void)sl;
  if (!(*calls)++) {
    *chunk = "deferred-note\n";
    *length = strlen(*chunk);
  }
  return SL_OK;
}

static void long_line(sl_t *sl, size_t length) {
  char block[4096];
  memset(block, 'w', sizeof(block));
  while (length) {
    size_t count = length < sizeof(block) ? length : sizeof(block);
    CHECK(sl_output_stream_write(sl, block, count) == SL_OK);
    length -= count;
  }
}

static void drive(sl_t *sl, void *context) {
  fixture_t *f = context;
  capture(f);
  switch (f->phase) {
  case 0:
    CHECK(!sl_transcript_visible(sl));
    CHECK(CHAT(sl, "\033[3") == SL_OK);
    CHECK(sl_prompt_queue_append(sl, "queued-marker", 0, NULL) == SL_OK);
    f->queue_started = milliseconds();
    advance(f, "draft\024");
    break;
  case 1: {
    int calls = 0;
    CHECK(sl_transcript_visible(sl));
    CHECK(strstr(f->output, "\033[?1049h") &&
          strstr(f->output, "1/2  jkJKudgG scroll  / search  q quit") &&
          !strstr(f->output, "c clear"));
    CHECK(strstr(f->output, "plain   text") && !strstr(f->output, "\033[31m"));
    CHECK(!strstr(f->output, "queued-marker") && ++f->viewer_idle_ticks <= 24);
    if (milliseconds() - f->queue_started < 850)
      break;
    CHECK(sl_print_above(sl, finite_note, &calls) == SL_ERROR_INVALID &&
          calls == 0);
    CHECK(sl_output_stream_reconcile(sl) == SL_OK);
    CHECK(CHAT(sl, "1mdeferred-live-\303") == SL_OK);
    CHECK(sl_output_stream_end(sl) == SL_ERROR_INVALID);
    CHECK(CHAT(sl, "\251\033[0m\n") == SL_OK);
    CHECK(CHAT(sl, "\033[H") == SL_ERROR_INVALID);
    CHECK(sl_output_stream_end(sl) == SL_OK);
    CHECK(sl_print_above(sl, finite_note, &calls) == SL_OK && calls == 2);
    CHECK(sl_output_stream_begin(sl) == SL_OK);
    CHECK(CHAT(sl, "\033]8;;https://example.invalid/source\033\\link\n") ==
          SL_OK);
    CHECK(sl_output_stream_write_quoted_prompt(sl, "deferred-quote") == SL_OK);
    CHECK(CHAT(sl, "\033[3") == SL_OK);
    CHECK(TEXT(sl, f->log, "live chunk\n") == SL_OK);
    advance(f, "");
    break;
  }
  case 2:
    CHECK(strstr(f->output, "live chunk") && !strstr(f->output, "deferred-"));
    CHECK(!raise(SIGUSR1) && restored);
    capture(f);
    CHECK(strstr(f->output, "\033[?1049l"));
    advance(f, "");
    break;
  case 3:
    CHECK(sl_transcript_visible(sl) && strstr(f->output, "\033[?1049h"));
    advance(f, "ignored\024");
    break;
  case 4:
    CHECK(strstr(f->output, "2/2  jkJKudgG scroll") &&
          strstr(f->output, "ALT frame"));
    CHECK(!strstr(f->output, "normal frame") &&
          !strstr(f->output, "\033[?1049h"));
    CHECK(!strstr(f->output, "external title"));
    CHECK(VT(sl, f->worker, "\033[?1049l") == SL_OK);
    advance(f, "");
    break;
  case 5:
    CHECK(strstr(f->output, "normal frame") && !strstr(f->output, "ALT frame"));
    CHECK(!strstr(f->output, "\033[?1049l"));
    advance(f, "\024");
    break;
  case 6: {
    const char *leave = strstr(f->output, "\033[?1049l");
    const char *live = strstr(f->output, "\033[31mdeferred-live-\303\251");
    const char *note = strstr(f->output, "deferred-note");
    const char *quote = strstr(f->output, "deferred-quote");
    const char *link =
        strstr(f->output, "https://example.invalid/source\033\\link");
    const char *close_link = link ? strstr(link, "\033]8;;\033\\") : NULL;
    CHECK(!sl_transcript_visible(sl) && strstr(f->output, "\033[?1049l"));
    CHECK(leave && live && note && quote && leave < live && live < note &&
          note < quote);
    CHECK(link && note < link && close_link && close_link < quote);
    CHECK(strstr(f->output, "queued-marker"));
    CHECK(sl_prompt_queue_clear(sl) == SL_OK);
    CHECK(CHAT(sl, "2mafter viewer\033[0m\n") == SL_OK);
    capture(f);
    CHECK(strstr(f->output, "\033[32mafter viewer"));
    advance(f, "\024");
    break;
  }
  case 7: {
    char lines[1996];
    size_t i;
    CHECK(sl_transcript_visible(sl));
    CHECK(CHAT(sl, "evicted-line\noldest-kept-line\n") == SL_OK);
    for (i = 0; i < sizeof(lines); i += 2) {
      lines[i] = 'x';
      lines[i + 1] = '\n';
    }
    CHECK(sl_output_stream_write(sl, lines, sizeof(lines)) == SL_OK);
    CHECK(CHAT(sl, "newest-kept-line\n") == SL_OK);
    advance(f, "\033s");
    break;
  }
  case 8:
    CHECK(!sl_transcript_visible(sl) && f->handoff == 1);
    advance(f, "\024");
    break;
  case 9:
    CHECK(sl_transcript_visible(sl));
    long_line(sl, CHAT_BACKLOG_LIMIT + 4096);
    CHECK(sl_transcript_remove(sl, f->log) == SL_OK);
    CHECK(!sl_transcript_visible(sl));
    capture(f);
    CHECK(!strstr(f->output, "wwww") && strstr(f->output, "\033[0m\r"));
    advance(f, "\024");
    break;
  case 10:
    CHECK(sl_transcript_visible(sl));
    long_line(sl, CHAT_BACKLOG_LIMIT - 7);
    CHECK(CHAT(sl, "\n\033[38;5;2mkept-after-oversized-line\033[0m\n") ==
          SL_OK);
    CHECK(sl_transcript_remove(sl, f->worker) == SL_OK);
    CHECK(!sl_transcript_visible(sl));
    capture(f);
    CHECK(strstr(f->output, "kept-after-oversized-line") &&
          !strstr(f->output, "wwww"));
    advance(f, "\r");
    break;
  default:
    CHECK(0);
  }
}

static void drive_without_stream(sl_t *sl, void *context) {
  fixture_t *f = context;
  capture(f);
  switch (f->phase) {
  case 0:
    advance(f, "\024");
    break;
  case 1: {
    int calls = 0;
    CHECK(sl_transcript_visible(sl));
    CHECK(sl_print_above(sl, finite_note, &calls) == SL_OK && calls == 2);
    if (f->handoff) {
      CHECK(sl_output_stream_begin(sl) == SL_OK);
      CHECK(CHAT(sl, "\033[3") == SL_OK);
    }
    capture(f);
    CHECK(!strstr(f->output, "deferred-note"));
    advance(f, "\024");
    break;
  }
  case 2:
    CHECK(!sl_transcript_visible(sl) && strstr(f->output, "deferred-note"));
    if (f->handoff) {
      CHECK(CHAT(sl, "1mnew-stream\033[0m\n") == SL_OK);
      capture(f);
      CHECK(strstr(f->output, "\033[31mnew-stream"));
      CHECK(sl_output_stream_end(sl) == SL_OK);
    } else
      CHECK(CHAT(sl, "closed") == SL_ERROR_INVALID);
    advance(f, "done\r");
    break;
  default:
    CHECK(0);
  }
}

static int screen_rows_with(const fixture_t *f, const char *needle) {
  int rows, row, matches = 0;
  char text[128];
  vterm_get_size(f->terminal, &rows, NULL);
  for (row = 0; row < rows; ++row) {
    screen_text(f, row, 0, text, sizeof(text));
    if (strstr(text, needle))
      ++matches;
  }
  return matches;
}

static void drive_handoff_close(sl_t *sl, void *context) {
  fixture_t *f = context;
  capture(f);
  switch (f->phase) {
  case 0:
    advance(f, "draft\nsecond\033[D!\033[D");
    break;
  case 1:
  case 2:
    CHECK(!strcmp(sl_buffer(sl), "draft\nsecon!d") && sl_cursor(sl) == 11);
    CHECK(screen_rows_with(f, "kept-output") == 1 &&
          screen_rows_with(f, "queued") == 1 &&
          screen_rows_with(f, "Ready.") == 1 &&
          screen_rows_with(f, "FLAG") == 1 &&
          screen_rows_with(f, "test> draft") == 1 &&
          screen_rows_with(f, "secon!d") == 1);
    if (f->phase == 1) {
      CHECK(sl_output_stream_end_for_handoff(sl) == SL_ERROR_INVALID);
      CHECK(sl_suspend(sl) == SL_OK);
      advance(f, "");
    } else {
      CHECK(screen_rows_with(f, "Pager closed.") == 1);
      advance(f, "\037");
    }
    break;
  case 3:
    CHECK(!strcmp(sl_buffer(sl), "draft\nsecond") && sl_cursor(sl) == 11);
    CHECK(sl_set_status_message(sl, "Goodbye.") == SL_OK);
    advance(f, "");
    break;
  case 4:
    CHECK(screen_rows_with(f, "Goodbye.") == 1);
    CHECK(sl_suspend(sl) == SL_OK);
    advance(f, "");
    break;
  default:
    CHECK(0);
  }
}

static void test_handoff_close(fixture_t *f, sl_config_t *config,
                               const struct termios *before) {
  static const char pager[] = "\033[?1049h\033[2J\033[Hpager\033[?1049l";
  const char *elements[] = {"FLAG"};
  struct winsize size = {12, 20, 0, 0};
  struct termios after;
  sl_t *sl;
  char *queued = NULL;
  CHECK(!ioctl(f->master, TIOCSWINSZ, &size));
  f->terminal = vterm_new(12, 20);
  CHECK(f->terminal);
  vterm_set_utf8(f->terminal, 1);
  vterm_screen_enable_altscreen(vterm_obtain_screen(f->terminal), 1);
  vterm_screen_reset(vterm_obtain_screen(f->terminal), 1);
  f->phase = 0;
  f->length = 0;
  config->prompt_queue = 1;
  config->prompt_queue_display_delay_ms = 0;
  config->statusline = 1;
  sl = sl_create_with_config(config);
  CHECK(sl && sl_set_status_elements(sl, elements, 1) == SL_OK);
  CHECK(sl_set_status_message(sl, "Ready.") == SL_OK &&
        sl_prompt_queue_append(sl, "queued", 0, NULL) == SL_OK &&
        sl_set_idle_callback(sl, drive_handoff_close, f) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK &&
        CHAT(sl, "kept-output\n") == SL_OK);
  CHECK(!sl_readline(sl, "test> ") &&
        sl_last_readline_status(sl) == SL_READLINE_SUSPENDED && f->phase == 2);
  CHECK(sl->output_stream_end_for_handoff(sl) == SL_OK);
  CHECK(!tcgetattr(config->input_fd, &after) &&
        test_termios_equal(before, &after));
  capture(f);
  CHECK(screen_rows_with(f, "kept-output") == 1 &&
        !screen_rows_with(f, "Ready.") && !screen_rows_with(f, "FLAG") &&
        !screen_rows_with(f, "queued") && !screen_rows_with(f, "draft") &&
        !screen_rows_with(f, "secon!d"));
  CHECK(sl_prompt_queue_count(sl) == 1 &&
        sl_prompt_queue_peek(sl, 0, &queued) == SL_OK &&
        !strcmp(queued, "queued"));
  sl_free_string(sl, queued);
  CHECK(write(config->output_fd, pager, sizeof(pager) - 1) ==
        (ssize_t)(sizeof(pager) - 1));
  CHECK(sl_output_stream_begin(sl) == SL_OK &&
        CHAT(sl, "Pager closed.\n") == SL_OK);
  CHECK(!sl_readline(sl, "test> ") &&
        sl_last_readline_status(sl) == SL_READLINE_SUSPENDED && f->phase == 5);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  capture(f);
  CHECK(screen_rows_with(f, "Goodbye.") == 1 && !screen_has(f, "draft", 0));
  sl_destroy(sl);
  vterm_free(f->terminal);
}

int main(void) {
  struct winsize size = {12, 80, 0, 0};
  struct termios before, after;
  sl_config_t config;
  sl_t *sl;
  fixture_t f;
  int slave, i;
  char *line;
  sl_transcript_id_t extra[6], rejected;
  memset(&f, 0, sizeof(f));
  CHECK(signal(SIGUSR1, returning_handler) != SIG_ERR);
  CHECK(!openpty(&f.master, &slave, NULL, NULL, &size));
  CHECK(!fcntl(f.master, F_SETFL, O_NONBLOCK));
  CHECK(!pipe(f.stop));
  CHECK(!pthread_mutex_init(&f.capture_lock, NULL));
  CHECK(!pthread_create(&f.reader, NULL, read_output_thread, &f));
  CHECK(!tcgetattr(slave, &before));
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  config.osc8_mode = SL_OSC8_ON;
  config.prompt_queue = 1;
  config.prompt_queue_display_delay_ms = 500;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(sl_transcript_add(sl, "invalid", 0, 32, &rejected) == SL_ERROR_INVALID);
  CHECK(sl->transcript_add(sl, "Log", 6, 42, &f.log) == SL_OK);
  CHECK(sl_transcript_add(sl, "Worker", 6, 42, &f.worker) == SL_OK);
  for (i = 0; i < 6; ++i)
    CHECK(sl_transcript_add(sl, "extra", 2, 5, &extra[i]) == SL_OK);
  CHECK(sl_transcript_add(sl, "extra", 2, 5, &rejected) == SL_ERROR_FULL);
  for (i = 0; i < 6; ++i)
    CHECK(sl->transcript_remove(sl, extra[i]) == SL_OK);
  CHECK(sl_transcript_remove(sl, extra[0]) == SL_ERROR_INVALID);
  CHECK(TEXT(sl, f.log, "plain\ttext\n") == SL_OK);
  CHECK(sl->transcript_resize(sl, f.log, 7, 44) == SL_OK);
  /* Text and VT chunks share one screen. Split a cursor command and a UTF-8
   * character to exercise the producer's actual chunk boundary. */
  CHECK(VT(sl, f.worker, "\033[2;") == SL_OK);
  CHECK(VT(sl, f.worker, "1Hnormal frame \303") == SL_OK);
  CHECK(VT(sl, f.worker, "\251") == SL_OK);
  CHECK(VT(sl, f.worker, "\033[?1049h\033[H\033[36mALT frame\033[0m") == SL_OK);
  CHECK(VT(sl, f.worker, "\033]2;external title\007") == SL_OK);
  CHECK(sl->transcript_reset(sl, f.log) == SL_OK);
  CHECK(sl->transcript_write(sl, f.log, SL_TRANSCRIPT_TEXT, "plain\ttext\n",
                             sizeof("plain\ttext\n") - 1) == SL_OK);
  CHECK(!sl_transcript_visible(sl));
  CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', shell_key, &f) == SL_OK);
  CHECK(sl_set_idle_callback(sl, drive, &f) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  line = sl_readline(sl, "test> ");
  CHECK(line && !strcmp(line, "draft") && f.phase == 11 && f.handoff == 1);
  sl_free_string(sl, line);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  CHECK(!tcgetattr(slave, &after) && test_termios_equal(&before, &after));
  for (i = 0; i < 2; ++i) {
    f.handoff = i;
    f.phase = 0;
    f.length = 0;
    config.prompt_queue = 0;
    sl = sl_create_with_config(&config);
    CHECK(sl);
    CHECK(sl_transcript_add(sl, "Log", 6, 42, &f.log) == SL_OK);
    CHECK(sl_set_idle_callback(sl, drive_without_stream, &f) == SL_OK);
    line = sl_readline(sl, "test> ");
    CHECK(line && !strcmp(line, "done") && f.phase == 3);
    sl_free_string(sl, line);
    sl_destroy(sl);
    CHECK(!tcgetattr(slave, &after) && test_termios_equal(&before, &after));
  }
  size.ws_row = 7;
  size.ws_col = 20;
  CHECK(!ioctl(f.master, TIOCSWINSZ, &size));
  f.length = 0;
  f.phase = 0;
  f.terminal = vterm_new(7, 20);
  CHECK(f.terminal);
  vterm_set_utf8(f.terminal, 1);
  vterm_screen_enable_altscreen(vterm_obtain_screen(f.terminal), 1);
  vterm_screen_reset(vterm_obtain_screen(f.terminal), 1);
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_transcript_add(sl, "Navigation", 6, 44, &f.log) == SL_OK);
  CHECK(sl_set_idle_callback(sl, drive_navigation, &f) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  line = sl_readline(sl, "test> ");
  CHECK(line && !strcmp(line, "draftqj/Gc") && f.phase == 51);
  sl_free_string(sl, line);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  f.length = 0;
  capture(&f);
  vterm_free(f.terminal);
  test_handoff_close(&f, &config, &before);
  close(f.stop[1]);
  CHECK(!pthread_join(f.reader, NULL));
  CHECK(!pthread_mutex_destroy(&f.capture_lock));
  close(f.stop[0]);
  close(slave);
  close(f.master);
  return 0;
}
