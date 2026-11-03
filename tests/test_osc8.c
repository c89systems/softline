#define _DEFAULT_SOURCE
#include "completion_fixture.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "OSC8 line %d: %s\n", __LINE__, #c);                     \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static sl_t *redirected(int fd, sl_osc8_mode_t mode) {
  sl_config_t config;
  sl_config_init(&config);
  config.output_fd = fd;
  config.osc8_mode = mode;
  return sl_create_with_config(&config);
}

static int bytes_equal(int fd, const char *expected, size_t length) {
  char bytes[SL_OSC8_MAX_BYTES + 128];
  size_t used = 0;
  ssize_t count;
  while ((count = read(fd, bytes + used, sizeof(bytes) - used)) > 0)
    used += (size_t)count;
  if (used != length || memcmp(bytes, expected, length)) {
    fprintf(stderr, "expected %lu bytes, got %lu: ", (unsigned long)length,
            (unsigned long)used);
    (void)fwrite(bytes, 1, used, stderr);
    fputc('\n', stderr);
  }
  return used == length && !memcmp(bytes, expected, length);
}

static int split_controls(void) {
  const char *values[] = {
      "before\033]8;;https://example.org/\033\\label\033]8;;\033\\after",
      "\033]8;id=one:other=value;https://example.org/"
      "caf\303\251\007label\033]8;;\007",
      "\033[31m\033]8;;file:///example\033\\red\033[0m link\033]8;;\033\\\n"};
  size_t v, split;
  for (v = 0; v < sizeof(values) / sizeof(values[0]); ++v) {
    size_t length = strlen(values[v]);
    for (split = 0; split <= length; ++split) {
      int fds[2];
      sl_t *sl;
      CHECK(!pipe(fds));
      CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
      sl = redirected(fds[1], SL_OSC8_ON);
      CHECK(sl && sl_output_stream_begin(sl) == SL_OK);
      CHECK(sl_output_stream_write(sl, values[v], split) == SL_OK);
      CHECK(sl_set_osc8_mode(sl, SL_OSC8_OFF) == SL_ERROR_NOT_READY);
      CHECK(sl_output_stream_write(sl, values[v] + split, length - split) ==
            SL_OK);
      CHECK(sl_output_stream_end(sl) == SL_OK);
      CHECK(bytes_equal(fds[0], values[v], length));
      CHECK(sl_set_osc8_mode(sl, SL_OSC8_OFF) == SL_OK);
      sl_destroy(sl);
      close(fds[0]);
      close(fds[1]);
    }
  }
  return 0;
}

static int reject_controls(void) {
  const char *bad[] = {"\033]0;title\007",
                       "\033]52;c;secret\007",
                       "\033]9;;bad\007",
                       "\033]8x;https://example.org\007",
                       "\033]8;\007",
                       "\033]8;;bad\nurl\007",
                       "\033]8;;bad\033[31m",
                       "\033]8;;\300\257\007",
                       "\033]8;;\302\235\007",
                       "\033]8;;\355\240\200\007",
                       "\033]8;;\364\220\200\200\007",
                       "\033]8;;\303\007",
                       "\033]8;id=\001;x\007",
                       "\033[H"};
  size_t i;
  for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    int fds[2];
    sl_t *sl;
    CHECK(!pipe(fds));
    CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
    sl = redirected(fds[1], SL_OSC8_ON);
    CHECK(sl && sl_output_stream_begin(sl) == SL_OK);
    CHECK(sl_output_stream_write(sl, "ok", 2) == SL_OK);
    CHECK(sl_output_stream_write(sl, bad[i], strlen(bad[i])) ==
          SL_ERROR_INVALID);
    CHECK(sl_output_stream_write(sl, "safe", 4) == SL_OK);
    CHECK(sl_output_stream_end(sl) == SL_OK);
    CHECK(bytes_equal(fds[0], "oksafe", 6));
    sl_destroy(sl);
    close(fds[0]);
    close(fds[1]);
  }
  return 0;
}

static int limits_and_spacing(void) {
  int fds[2];
  sl_t *sl;
  char *control = malloc(SL_OSC8_MAX_BYTES + 2);
  const char *open = "\033]8;;https://example.org\033\\";
  CHECK(control && !pipe(fds));
  CHECK(fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0);
  sl = redirected(fds[1], SL_OSC8_OFF);
  CHECK(sl && sl_output_stream_begin(sl) == SL_OK);
  CHECK(sl_output_stream_write(sl, open, strlen(open)) == SL_ERROR_INVALID);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  CHECK(bytes_equal(fds[0], "", 0));
  CHECK(sl_set_osc8_mode(sl, SL_OSC8_ON) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  memcpy(control, "\033]8;;", 5);
  memset(control + 5, 'x', SL_OSC8_MAX_BYTES - 7);
  memcpy(control + SL_OSC8_MAX_BYTES - 2, "\033\\", 2);
  CHECK(sl_output_stream_write(sl, control, SL_OSC8_MAX_BYTES) == SL_OK);
  CHECK(bytes_equal(fds[0], control, SL_OSC8_MAX_BYTES));
  CHECK(sl_output_stream_write(sl, "X", 1) == SL_OK);
  CHECK(bytes_equal(fds[0], "X",
                    1)); /* Streaming: label delivered before close. */
  CHECK(sl_output_stream_end(sl) == SL_OK);
  CHECK(bytes_equal(fds[0], "\033]8;;\033\\", 7));
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  control[SL_OSC8_MAX_BYTES - 2] = 'x';
  memcpy(control + SL_OSC8_MAX_BYTES - 1, "\033\\", 2);
  CHECK(sl_output_stream_write(sl, control, SL_OSC8_MAX_BYTES + 1) ==
        SL_ERROR_INVALID);
  CHECK(bytes_equal(fds[0], "", 0));
  CHECK(sl_output_stream_write(sl, "\033]8;;https://example.org",
                               strlen("\033]8;;https://example.org")) == SL_OK);
  CHECK(sl_output_stream_end(sl) == SL_ERROR_INVALID);
  CHECK(bytes_equal(fds[0], "", 0));
  CHECK(sl_output_stream_write(sl, "\033\\", 2) == SL_OK);
  CHECK(sl_output_stream_write(sl, "x\n\n", 3) == SL_OK);
  CHECK(sl_set_quoted_prompt_prefix(sl, "> ") == SL_OK);
  CHECK(sl_output_stream_write_quoted_prompt(sl, "user") == SL_OK);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  CHECK(bytes_equal(
      fds[0], "\033]8;;https://example.org\033\\x\n\n\033]8;;\033\\> user\n\n",
      strlen(
          "\033]8;;https://example.org\033\\x\n\n\033]8;;\033\\> user\n\n")));
  sl_destroy(sl);
  close(fds[0]);
  close(fds[1]);
  free(control);
  return 0;
}

static int detection(void) {
  int master, slave, fds[2];
  sl_config_t config;
  sl_osc8_info_t info;
  sl_t *sl;
  struct winsize size = {24, 80, 0, 0};
  const char *names[] = {"TERM_PROGRAM", "VTE_VERSION", "WT_SESSION", "TMUX",
                         "STY"};
  static const struct {
    const char *term, *program, *vte, *mux, *screen, *windows;
    int detected;
  } cases[] = {
      {"xterm-256color", "iTerm.app", "", "", "", "", 1},
      {"xterm-256color", "WezTerm", "", "", "", "", 1},
      {"xterm-256color", "vscode", "", "", "", "", 1},
      {"xterm-kitty", "", "", "", "", "", 1},
      {"xterm-256color", "", "", "", "", "windows-session", 1},
      {"xterm-256color", "", "4999", "", "", "", 0},
      {"xterm-256color", "", "5000", "", "", "", 1},
      {"xterm-256color", "", "5000junk", "", "", "", 0},
      {"xterm-256color", "", "9999999999999999999999999", "", "", "", 0},
      {"dumb", "WezTerm", "8400", "", "", "", 0},
      {"screen-256color", "WezTerm", "8400", "", "", "", 0},
      {"tmux-256color", "WezTerm", "8400", "", "", "", 0},
      {"xterm-kitty", "WezTerm", "8400", "session", "", "", 0},
      {"xterm-kitty", "WezTerm", "8400", "", "session", "", 0},
      {"xterm-256color", "unknown", "", "", "", "", 0}};
  size_t i;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(fds));
  for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
    unsetenv(names[i]);
  setenv("TERM", "xterm-256color", 1);
  sl_config_init(&config);
  config.input_fd = fds[0]; /* Detection must inspect the configured output. */
  config.output_fd = slave;
  sl = sl_create_with_config(&config);
  CHECK(sl && sl->get_osc8_info(sl, &info) == SL_OK);
  CHECK(info.mode == SL_OSC8_OFF && !info.detected && !info.enabled);
  CHECK(sl_set_osc8_mode(sl, SL_OSC8_AUTO) == SL_OK);
  CHECK(sl_get_osc8_info(sl, &info) == SL_OK && !info.enabled);
  setenv("VTE_VERSION", "5000", 1);
  CHECK(sl_get_osc8_info(sl, &info) == SL_OK && !info.enabled); /* Cached. */
  CHECK(sl->set_osc8_mode(sl, SL_OSC8_AUTO) == SL_OK);
  CHECK(sl_get_osc8_info(sl, &info) == SL_OK && info.detected && info.enabled);
  setenv("TMUX", "/session", 1);
  CHECK(sl_set_osc8_mode(sl, SL_OSC8_AUTO) == SL_OK);
  CHECK(sl_get_osc8_info(sl, &info) == SL_OK && !info.detected &&
        !info.enabled);
  CHECK(sl_set_osc8_mode(sl, SL_OSC8_ON) == SL_OK);
  CHECK(sl_get_osc8_info(sl, &info) == SL_OK && !info.detected && info.enabled);
  CHECK(sl_set_osc8_mode(sl, (sl_osc8_mode_t)9) == SL_ERROR_INVALID);
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    setenv("TERM", cases[i].term, 1);
    setenv("TERM_PROGRAM", cases[i].program, 1);
    setenv("VTE_VERSION", cases[i].vte, 1);
    setenv("TMUX", cases[i].mux, 1);
    setenv("STY", cases[i].screen, 1);
    setenv("WT_SESSION", cases[i].windows, 1);
    CHECK(sl_set_osc8_mode(sl, SL_OSC8_AUTO) == SL_OK);
    CHECK(sl_get_osc8_info(sl, &info) == SL_OK &&
          info.detected == cases[i].detected &&
          info.enabled == cases[i].detected);
    CHECK(sl_set_osc8_mode(sl, SL_OSC8_OFF) == SL_OK);
    CHECK(sl_get_osc8_info(sl, &info) == SL_OK &&
          info.detected == cases[i].detected && !info.enabled);
  }
  sl_destroy(sl);
  unsetenv("TMUX");
  setenv("VTE_VERSION", "8400", 1);
  config.osc8_mode = SL_OSC8_AUTO;
  config.input_fd = slave;
  config.output_fd = fds[1];
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_get_osc8_info(sl, &info) == SL_OK && !info.detected &&
        !info.enabled);
  CHECK(sl_set_osc8_mode(sl, SL_OSC8_ON) == SL_OK);
  CHECK(sl_get_osc8_info(sl, &info) == SL_OK && info.enabled && !info.detected);
  CHECK(sl_get_osc8_info(sl, NULL) == SL_ERROR_INVALID);
  CHECK(sl_get_osc8_info(NULL, &info) == SL_ERROR_INVALID);
  CHECK(sl_set_osc8_mode(NULL, SL_OSC8_ON) == SL_ERROR_INVALID);
  sl_destroy(sl);
  config.osc8_mode = (sl_osc8_mode_t)-1;
  CHECK(!sl_create_with_config(&config));
  close(master);
  close(slave);
  close(fds[0]);
  close(fds[1]);
  return 0;
}

static int producer(sl_t *sl, int master) {
  const char *open = "\033]8;id=stable;https://example.org/\033\\";
  size_t i;
  for (i = 0; i < strlen(open); ++i)
    if (sl_output_stream_write(sl, open + i, 1) != SL_OK)
      return SL_ERROR_IO;
  if (sl_output_stream_write(sl, "FIRST", 5) != SL_OK ||
      sl_set_status_message(sl, "reasoning changed") != SL_OK ||
      sl_output_stream_write(sl, "-SECOND", 7) != SL_OK ||
      sl_output_stream_write_quoted_prompt(sl, "QUOTED") != SL_OK ||
      sl_output_stream_write(sl, "PLAIN", 5) != SL_OK ||
      write(master, "\r", 1) != 1)
    return SL_ERROR_IO;
  return SL_OK;
}
struct producer_state {
  int master, phase, status;
  struct completion_fixture *completions;
};
static void produce_when_ready(sl_t *sl, void *data) {
  struct producer_state *state = data;
  if (!state->phase) {
    state->phase = 1;
    state->status = sl_set_buffer(sl, "draft");
  } else if (state->phase == 1 && state->completions->calls) {
    state->phase = 2;
    state->status = producer(sl, state->master);
  }
  if (state->status != SL_OK)
    (void)sl_cancel(sl);
}

/* Inspect only OSC 8 scope in real PTY bytes, not terminal layout. */
static int scope_at(const char *bytes, const char *word, int expected) {
  size_t i, count = 0;
  int active = 0;
  for (i = 0; bytes[i]; ++i) {
    if (!strncmp(bytes + i, "\033]8;", 4)) {
      const char *params = strchr(bytes + i + 4, ';'), *end;
      if (!params)
        return 0;
      end = params + 1;
      while (*end && *end != 7 && *end != 27)
        ++end;
      if (!*end)
        return 0;
      active = end > params + 1;
      i = (size_t)(end - bytes) + (*end == 27 ? 1 : 0);
    } else if (!strncmp(bytes + i, word, strlen(word))) {
      if (active != expected)
        return 0;
      ++count;
    }
  }
  if (!count)
    fprintf(stderr, "missing %s in native bytes: %s\n", word, bytes);
  return count != 0;
}

static int native_scope(void) {
  int master, slave;
  struct winsize size = {24, 80, 0, 0};
  sl_config_t config;
  sl_t *sl;
  struct producer_state producer_state;
  char bytes[32768], *line;
  ssize_t count;
  size_t used = 0;
  static const struct completion_item completion = {"draft suggestion",
                                                    SL_COMPLETION_START};
  struct completion_fixture completions = {&completion, 1, 0};
  CHECK(!openpty(&master, &slave, NULL, NULL, &size));
  memset(&producer_state, 0, sizeof(producer_state));
  producer_state.master = master;
  producer_state.completions = &completions;
  CHECK(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.osc8_mode = SL_OSC8_ON;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  config.prompt_queue = 1;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(fixture_setup(sl, &completions) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "QUEUE", 0, NULL) == SL_OK);
  CHECK(sl_inject_queued_prompt(sl, "APPROVAL", NULL) == SL_OK);
  CHECK(sl_set_status_message(sl, "reasoning") == SL_OK);
  CHECK(sl_set_idle_callback(sl, produce_when_ready, &producer_state) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  line = sl_next_prompt(sl, "> ", NULL);
  if (!line || strcmp(line, "draft"))
    fprintf(stderr, "native draft=%s status=%d error=%s\n",
            line ? line : "NULL", sl_last_readline_status(sl),
            sl_last_error(sl));
  CHECK(line && !strcmp(line, "draft"));
  CHECK(producer_state.status == SL_OK && completions.calls == 1);
  sl_free_string(sl, line);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  while ((count = read(master, bytes + used, sizeof(bytes) - 1 - used)) > 0)
    used += (size_t)count;
  bytes[used] = '\0';
  CHECK(scope_at(bytes, "FIRST", 1) && scope_at(bytes, "-SECOND", 1));
  CHECK(scope_at(bytes, "QUOTED", 0) && scope_at(bytes, "PLAIN", 0));
  CHECK(scope_at(bytes, "> ", 0) && scope_at(bytes, "QUEUE", 0));
  CHECK(scope_at(bytes, "1 approval", 0));
  CHECK(scope_at(bytes, "reasoning", 0));
  CHECK(scope_at(bytes, "suggestion", 0));
  close(master);
  close(slave);
  return 0;
}

static int redirected_teardown(int partial) {
  int master, slave, input[2];
  struct winsize size = {12, 40, 0, 0};
  sl_config_t config;
  sl_t *sl;
  char bytes[1024];
  size_t used = 0;
  ssize_t count;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(input));
  CHECK(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
  sl_config_init(&config);
  config.input_fd = input[0];
  config.output_fd = slave;
  config.osc8_mode = SL_OSC8_ON;
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_output_stream_begin(sl) == SL_OK);
  CHECK(sl_output_stream_write(
            sl, "\033]8;;https://example.org\007LABEL",
            strlen("\033]8;;https://example.org\007LABEL")) == SL_OK);
  if (partial) {
    const char *fragment = partial == 1 ? "\033]8;;unfinished" : "\033[3";
    CHECK(sl_output_stream_write(sl, fragment, strlen(fragment)) == SL_OK);
    CHECK(sl_output_stream_end(sl) == SL_ERROR_INVALID);
  }
  sl_destroy(sl);
  CHECK(write(slave, "HOST_AFTER", 10) == 10);
  /* A PTY can deliver the final write after an earlier readable prefix. */
  bytes[0] = '\0';
  while (!strstr(bytes, "HOST_AFTER")) {
    struct pollfd fd;
    fd.fd = master;
    fd.events = POLLIN;
    CHECK(poll(&fd, 1, 1000) > 0);
    while ((count = read(master, bytes + used, sizeof(bytes) - 1 - used)) > 0)
      used += (size_t)count;
    bytes[used] = '\0';
    if (strstr(bytes, "HOST_AFTER"))
      break;
  }
  CHECK(scope_at(bytes, "LABEL", 1) && scope_at(bytes, "HOST_AFTER", 0));
  CHECK(!strstr(bytes, "unfinished") && !strstr(bytes, "\033[3"));
  close(master);
  close(slave);
  close(input[0]);
  close(input[1]);
  return 0;
}

int main(void) {
  return detection() || split_controls() || reject_controls() ||
         limits_and_spacing() || native_scope() || redirected_teardown(0) ||
         redirected_teardown(1) || redirected_teardown(2);
}
