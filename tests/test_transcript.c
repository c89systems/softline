#define _DEFAULT_SOURCE
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include "softline/transcript.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

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

enum { CHAT_BACKLOG_LIMIT = 256 * 1024 };

typedef struct fixture {
  sl_transcript_id_t log, worker;
  int master, phase, handoff, viewer_idle_ticks;
  double queue_started;
  size_t length;
  char output[16384];
} fixture_t;

static volatile sig_atomic_t restored;

static void returning_handler(int signal_number) {
  (void)signal_number;
  restored = 1;
}

static void keys(fixture_t *f, const char *keys) {
  CHECK(write(f->master, keys, strlen(keys)) == (ssize_t)strlen(keys));
}

static void capture(fixture_t *f) {
  ssize_t n;
  while ((n = read(f->master, f->output + f->length,
                   sizeof(f->output) - f->length - 1)) > 0)
    f->length += (size_t)n;
  CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  f->output[f->length] = 0;
}

static void advance(fixture_t *f, const char *input) {
  ++f->phase;
  f->length = 0;
  keys(f, input);
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
    CHECK(strstr(f->output, "\033[?1049h") && strstr(f->output, "1/2 Log"));
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
    CHECK(strstr(f->output, "2/2 Worker") && strstr(f->output, "ALT frame"));
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
  CHECK(sl_transcript_add(sl, "Log", 6, 42, &f.log) == SL_OK);
  CHECK(sl_transcript_add(sl, "Worker", 6, 42, &f.worker) == SL_OK);
  for (i = 0; i < 6; ++i)
    CHECK(sl_transcript_add(sl, "extra", 2, 5, &extra[i]) == SL_OK);
  CHECK(sl_transcript_add(sl, "extra", 2, 5, &rejected) == SL_ERROR_FULL);
  for (i = 0; i < 6; ++i)
    CHECK(sl_transcript_remove(sl, extra[i]) == SL_OK);
  CHECK(sl_transcript_remove(sl, extra[0]) == SL_ERROR_INVALID);
  CHECK(TEXT(sl, f.log, "plain\ttext\n") == SL_OK);
  CHECK(sl_transcript_resize(sl, f.log, 7, 44) == SL_OK);
  /* Text and VT chunks share one screen. Split a cursor command and a UTF-8
   * character to exercise the producer's actual chunk boundary. */
  CHECK(VT(sl, f.worker, "\033[2;") == SL_OK);
  CHECK(VT(sl, f.worker, "1Hnormal frame \303") == SL_OK);
  CHECK(VT(sl, f.worker, "\251") == SL_OK);
  CHECK(VT(sl, f.worker, "\033[?1049h\033[H\033[36mALT frame\033[0m") == SL_OK);
  CHECK(VT(sl, f.worker, "\033]2;external title\007") == SL_OK);
  CHECK(sl_transcript_reset(sl, f.log) == SL_OK);
  CHECK(TEXT(sl, f.log, "plain\ttext\n") == SL_OK);
  CHECK(!sl_transcript_visible(sl));
  CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', shell_key, &f) == SL_OK);
  CHECK(sl_set_idle_callback(sl, drive, &f) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  line = sl_readline(sl, "test> ");
  CHECK(line && !strcmp(line, "draft") && f.phase == 11 && f.handoff == 1);
  sl_free_string(sl, line);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  CHECK(!tcgetattr(slave, &after) && before.c_lflag == after.c_lflag);
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
    CHECK(!tcgetattr(slave, &after) && before.c_lflag == after.c_lflag);
  }
  close(slave);
  close(f.master);
  return 0;
}
