#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "softline/softline.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#define SL_TEST_PTY 1
#include <pty.h>
#else
#define SL_TEST_PTY 0
#endif

static int tests_run = 0;
static int tests_passed = 0;
static volatile sig_atomic_t signal_seen = 0;
static int signal_ack_fd = -1;

static void remember_signal(int signo) {
  unsigned char byte;
  (void)signo;
  signal_seen = 1;
  if (signal_ack_fd >= 0) {
    byte = 's';
    (void)write(signal_ack_fd, &byte, 1);
  }
}

static void ignore_signal(int signo) { (void)signo; }

#define TEST(name)                                                             \
  do {                                                                         \
    tests_run++;                                                               \
    printf("  %-58s", name);                                                   \
    fflush(stdout);                                                            \
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

static int one_chunk_stream(sl_t *sl, void *userdata, const char **chunk,
                            size_t *len) {
  const char *text;
  (void)sl;
  text = (const char *)userdata;
  if (!text) {
    *chunk = NULL;
    *len = 0;
    return SL_OK;
  }
  *chunk = text;
  *len = strlen(text);
  return SL_OK;
}

struct one_chunk_once {
  const char *text;
  int sent;
};

static int one_chunk_once_stream(sl_t *sl, void *userdata, const char **chunk,
                                 size_t *len) {
  struct one_chunk_once *stream;
  (void)sl;
  stream = (struct one_chunk_once *)userdata;
  if (!stream || stream->sent) {
    *chunk = NULL;
    *len = 0;
    return SL_OK;
  }
  stream->sent = 1;
  *chunk = stream->text;
  *len = strlen(stream->text);
  return SL_OK;
}

static int failing_stream(sl_t *sl, void *userdata, const char **chunk,
                          size_t *len) {
  (void)sl;
  (void)userdata;
  *chunk = NULL;
  *len = 0;
  return SL_ERROR;
}

static int invalid_chunk_stream(sl_t *sl, void *userdata, const char **chunk,
                                size_t *len) {
  (void)sl;
  (void)userdata;
  *chunk = NULL;
  *len = 1;
  return SL_OK;
}

static int empty_stream(sl_t *sl, void *userdata, const char **chunk,
                        size_t *len) {
  (void)sl;
  (void)userdata;
  *chunk = NULL;
  *len = 0;
  return SL_OK;
}

static void test_config_init(void) {
  sl_config_t cfg;

  TEST("config_init sets documented defaults");
  memset(&cfg, 0x7f, sizeof(cfg));
  sl_config_init(&cfg);
  ASSERT_TRUE(cfg.input_fd == STDIN_FILENO, "input fd default");
  ASSERT_TRUE(cfg.output_fd == STDOUT_FILENO, "output fd default");
  ASSERT_TRUE(cfg.screen_width == 0, "screen width default");
  ASSERT_TRUE(cfg.live_scroll_region == 0, "live scroll region default");
  ASSERT_TRUE(cfg.clear_prompt_on_exit == 0,
              "preserve status UI on exit default");
  ASSERT_TRUE(cfg.disable_image_paste == 0, "image paste enabled by default");
  ASSERT_TRUE(cfg.image_paste_path_template == NULL,
              "image paste uses XDG cache by default");
  ASSERT_TRUE(cfg.history_max_len == 100, "history max default");
  ASSERT_TRUE(cfg.line_max_len == 131072, "line max default");
  ASSERT_TRUE(cfg.prompt_queue == 0, "prompt queue default");
  ASSERT_TRUE(cfg.prompt_queue_max_entries == 64, "prompt queue max default");
  ASSERT_TRUE(cfg.prompt_queue_preview_entries == 3,
              "prompt queue preview default");
  ASSERT_TRUE(cfg.prompt_theme == SL_PROMPT_THEME_DEFAULT,
              "prompt theme default");
  ASSERT_TRUE(cfg.statusline == 0, "status line default");
  ASSERT_TRUE(cfg.statusline_start_element == 0,
              "status element start default");
  ASSERT_TRUE(cfg.status_spinner == 0, "status spinner default");
  ASSERT_TRUE(cfg.status_busy == 0, "status busy default");
  ASSERT_TRUE(cfg.status_idle_marker == '+', "status idle marker default");
  PASS();
}

static void test_plain_config_init(sl_config_t *config) {
  sl_config_init(config);
  config->prompt_theme = SL_PROMPT_THEME_PLAIN;
}

#define sl_config_init test_plain_config_init

static void test_large_draft_limits(void) {
  sl_config_t config;
  sl_t *sl;
  sl_prompt_id_t id;
  char *text, *taken;
  size_t i;
  TEST("128 KiB drafts and queue entries round-trip at default limit");
  sl_config_init(&config);
  config.prompt_queue = 1;
  config.input_fd = config.output_fd = open("/dev/null", O_RDWR);
  ASSERT_TRUE(config.input_fd >= 0, "open null");
  sl = sl_create_with_config(&config);
  text = malloc(131074);
  ASSERT_TRUE(sl && text, "allocate draft");
  for (i = 0; i < 131072; ++i)
    text[i] = i % 80 == 79 ? '\n' : 'a';
  text[131072] = 0;
  ASSERT_TRUE(sl_set_buffer(sl, text) == SL_OK, "maximum draft accepted");
  ASSERT_TRUE(!strcmp(sl_buffer(sl), text), "maximum draft preserved");
  ASSERT_TRUE(sl_prompt_queue_append(sl, text, 1, &id) == SL_OK && id,
              "maximum held queue entry accepted");
  text[131072] = 'b';
  text[131073] = 0;
  ASSERT_TRUE(sl_set_buffer(sl, text) != SL_OK, "oversize draft rejected");
  ASSERT_TRUE(sl_insert(sl, "b") != SL_OK && strlen(sl_buffer(sl)) == 131072,
              "oversize insertion leaves draft intact");
  ASSERT_TRUE(sl_prompt_queue_replace(sl, 0, text) != SL_OK,
              "oversize replacement rejected");
  ASSERT_TRUE(sl_prompt_queue_append(sl, text, 1, NULL) != SL_OK &&
                  sl_prompt_queue_count(sl) == 1,
              "oversize insertion leaves queue intact");
  text[131072] = 0;
  ASSERT_TRUE(!strcmp(sl_buffer(sl), text), "failed mutation preserved text");
  ASSERT_TRUE(sl_prompt_queue_approve(sl, id) == SL_OK, "approve held entry");
  ASSERT_TRUE(sl_prompt_queue_take(sl, 0, &taken) == SL_OK &&
                  !strcmp(taken, text),
              "queue round-trip preserved complete text");
  sl_free_string(sl, taken);
  sl_destroy(sl);
  close(config.input_fd);
  free(text);
  PASS();
}

static void test_receiver_shell(void) {
  sl_t *sl;

  TEST("constructor returns receiver shell");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl->readline != NULL, "readline method missing");
  ASSERT_TRUE(sl->next_prompt != NULL, "next_prompt method missing");
  ASSERT_TRUE(sl->destroy != NULL, "destroy method missing");
  ASSERT_TRUE(sl->free_string != NULL, "free_string method missing");
  ASSERT_TRUE(sl->history_add != NULL, "history_add method missing");
  ASSERT_TRUE(sl->history_set_max_len != NULL,
              "history_set_max_len method missing");
  ASSERT_TRUE(sl->history_save != NULL, "history_save method missing");
  ASSERT_TRUE(sl->history_load != NULL, "history_load method missing");
  ASSERT_TRUE(sl->set_screen_width != NULL, "set_screen_width method missing");
  ASSERT_TRUE(sl->set_live_scroll_region != NULL,
              "set_live_scroll_region method missing");
  ASSERT_TRUE(sl->set_prompt_queue != NULL, "set_prompt_queue method missing");
  ASSERT_TRUE(sl->set_prompt_theme != NULL, "set_prompt_theme method missing");
  ASSERT_TRUE(sl->set_statusline != NULL, "set_statusline method missing");
  ASSERT_TRUE(sl->set_status_message != NULL,
              "set_status_message method missing");
  ASSERT_TRUE(sl->set_status_message_prefix != NULL &&
                  sl->set_status_message_colors != NULL,
              "status message styling methods missing");
  ASSERT_TRUE(sl->set_status_elements != NULL,
              "set_status_elements method missing");
  ASSERT_TRUE(sl->set_status_element != NULL,
              "set_status_element method missing");
  ASSERT_TRUE(sl->set_status_busy != NULL, "set_status_busy method missing");
  ASSERT_TRUE(sl->set_status_spinner != NULL,
              "set_status_spinner method missing");
  ASSERT_TRUE(sl->set_status_idle_marker != NULL,
              "set_status_idle_marker method missing");
  ASSERT_TRUE(sl->prompt_queue_count != NULL,
              "prompt_queue_count method missing");
  ASSERT_TRUE(sl->prompt_queue_capacity != NULL,
              "prompt_queue_capacity method missing");
  ASSERT_TRUE(sl->prompt_queue_peek != NULL,
              "prompt_queue_peek method missing");
  ASSERT_TRUE(sl->prompt_queue_insert != NULL,
              "prompt_queue_insert method missing");
  ASSERT_TRUE(sl->prompt_queue_append != NULL,
              "prompt_queue_append method missing");
  ASSERT_TRUE(sl->prompt_queue_replace != NULL,
              "prompt_queue_replace method missing");
  ASSERT_TRUE(sl->prompt_queue_take != NULL,
              "prompt_queue_take method missing");
  ASSERT_TRUE(sl->prompt_queue_clear != NULL,
              "prompt_queue_clear method missing");
  ASSERT_TRUE(sl->prompt_queue_enqueue_draft != NULL,
              "prompt_queue_enqueue_draft method missing");
  ASSERT_TRUE(sl->set_prompt_queue_delivery != NULL,
              "set_prompt_queue_delivery method missing");
  ASSERT_TRUE(sl->get_prompt_queue_delivery != NULL,
              "get_prompt_queue_delivery method missing");
  ASSERT_TRUE(sl->set_prompt_queue_profile != NULL,
              "set_prompt_queue_profile method missing");
  ASSERT_TRUE(sl->get_prompt_queue_profile != NULL,
              "get_prompt_queue_profile method missing");
  ASSERT_TRUE(sl->set_prompt_queue_keys != NULL,
              "set_prompt_queue_keys method missing");
  ASSERT_TRUE(sl->get_prompt_queue_keys != NULL,
              "get_prompt_queue_keys method missing");
  ASSERT_TRUE(sl->set_idle_callback != NULL,
              "set_idle_callback method missing");
  ASSERT_TRUE(sl->watch_add != NULL, "watch_add method missing");
  ASSERT_TRUE(sl->watch_modify != NULL, "watch_modify method missing");
  ASSERT_TRUE(sl->watch_remove != NULL, "watch_remove method missing");
  ASSERT_TRUE(sl->watch_clear != NULL, "watch_clear method missing");
  ASSERT_TRUE(sl->bind_key != NULL, "bind_key method missing");
  ASSERT_TRUE(sl->insert != NULL, "insert method missing");
  ASSERT_TRUE(sl->set_buffer != NULL, "set_buffer method missing");
  ASSERT_TRUE(sl->buffer != NULL, "buffer method missing");
  ASSERT_TRUE(sl->cursor != NULL, "cursor method missing");
  ASSERT_TRUE(sl->set_cursor != NULL, "set_cursor method missing");
  ASSERT_TRUE(sl->submit != NULL, "submit method missing");
  ASSERT_TRUE(sl->cancel != NULL, "cancel method missing");
  ASSERT_TRUE(sl->print_above != NULL, "print_above method missing");
  ASSERT_TRUE(sl->last_readline_status != NULL,
              "last_readline_status method missing");
  ASSERT_TRUE(sl->last_error != NULL, "last_error method missing");
  ASSERT_TRUE(sl->impl != NULL, "impl missing");
  ASSERT_TRUE(sl->last_readline_status(sl) == SL_READLINE_NONE,
              "initial readline status mismatch");
  sl->destroy(sl);
  PASS();
}

static void test_status_message_api(void) {
  sl_t *sl;
  TEST("status message accepts updates and clear, rejects controls");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl_set_status_message(sl, "Thinking...") == SL_OK &&
                  sl->set_status_message(sl, "Reasoning...") == SL_OK &&
                  sl_set_status_message(sl, "\xc3\xa5") == SL_OK,
              "valid status message rejected");
  ASSERT_TRUE(sl_set_status_message_prefix(sl, "? ") == SL_OK &&
                  sl_set_status_message_prefix(sl, "") == SL_OK &&
                  sl->set_status_message_prefix(sl, NULL) == SL_OK &&
                  sl_set_status_message_colors(sl, SL_THEME_COLOR_MUTED,
                                               SL_THEME_COLOR_ELEMENT_2) ==
                      SL_OK,
              "valid status styling rejected");
  ASSERT_TRUE(sl_set_status_message_prefix(sl, "bad\n") == SL_ERROR_INVALID &&
                  sl_set_status_message_prefix(sl, "\033[31m") ==
                      SL_ERROR_INVALID &&
                  sl_set_status_message_colors(sl, (sl_theme_color_t)-1,
                                               SL_THEME_COLOR_MUTED) ==
                      SL_ERROR_INVALID &&
                  sl_set_status_message_colors(sl, SL_THEME_COLOR_MUTED,
                                               (sl_theme_color_t)99) ==
                      SL_ERROR_INVALID,
              "invalid status styling accepted");
  ASSERT_TRUE(sl_set_status_message(sl, "two\nlines") == SL_ERROR_INVALID &&
                  sl_set_status_message(sl, "\033[31m") == SL_ERROR_INVALID &&
                  sl_set_status_message(sl, "\xc3") == SL_ERROR_INVALID,
              "invalid status message accepted");
  ASSERT_TRUE(sl_set_status_message(sl, NULL) == SL_OK &&
                  sl_set_status_message(sl, "") == SL_OK,
              "status message did not clear");
  sl_destroy(sl);
  PASS();
}

static void test_parser_only_stream_allows_geometry_changes(void) {
#if SL_TEST_PTY
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  int input_pipe[2];
  char output[64];
  ssize_t output_len;
  fd_set readfds;
  struct timeval timeout;
  TEST("parser-only stream accepts geometry changes with TTY output");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0 &&
                  pipe(input_pipe) == 0,
              "mixed-TTY setup failed");
  sl_config_init(&cfg);
  cfg.input_fd = input_pipe[0];
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_set_screen_width(sl, 20) == SL_OK &&
                  sl_output_stream_write(sl, "\033[2", 3) == SL_OK &&
                  sl_output_stream_write(sl, "J", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "H", 1) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "parser-only stream failed after geometry change");
  FD_ZERO(&readfds);
  FD_SET(master_fd, &readfds);
  timeout.tv_sec = 1;
  timeout.tv_usec = 0;
  ASSERT_TRUE(select(master_fd + 1, &readfds, NULL, NULL, &timeout) == 1,
              "parser-only stream emitted no output");
  output_len = read(master_fd, output, sizeof(output));
  ASSERT_TRUE(output_len == 1 && output[0] == 'H',
              "parser-only stream emitted an incomplete terminal command");
  sl_destroy(sl);
  close(input_pipe[0]);
  close(input_pipe[1]);
  close(slave_fd);
  close(master_fd);
  PASS();
#else
  TEST("parser-only stream accepts geometry changes with TTY output");
  PASS();
#endif
}

struct full_editor_stream_probe {
  int fired;
  int start_inside;
  int setup_result;
  int write_result;
  size_t draft_length;
};

#if SL_TEST_PTY
static size_t read_live_pty_output(int fd, char *bytes, size_t capacity);
#endif

static void full_editor_stream_idle(sl_t *sl, void *userdata) {
  struct full_editor_stream_probe *probe;
  char draft[301];
  probe = (struct full_editor_stream_probe *)userdata;
  if (probe->fired)
    return;
  probe->fired = 1;
  memset(draft, 'x', sizeof(draft) - 1);
  draft[sizeof(draft) - 1] = '\0';
  if (probe->start_inside)
    draft[60] = '\0';
  probe->setup_result = sl_set_buffer(sl, draft);
  if (probe->setup_result == SL_OK && probe->start_inside)
    probe->setup_result = sl_output_stream_begin(sl);
  if (probe->setup_result == SL_OK && !probe->start_inside)
    probe->setup_result = sl_set_status_message(
        sl, "Hello world, this is a long status line, that continues on "
            "multiple lines.");
  if (probe->setup_result == SL_OK) {
    probe->draft_length = strlen(sl_buffer(sl));
    probe->write_result = sl_output_stream_write(sl, "AFTER", 5);
  }
  (void)sl_cancel(sl);
}

static void test_live_output_retains_row_with_full_editor(int start_inside) {
#if SL_TEST_PTY
  struct winsize ws;
  struct full_editor_stream_probe probe;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[16384];
  char *line;
  TEST(start_inside
           ? "starting a stream pages an existing wrapped draft"
           : "live stream keeps an output row under a full editor and status");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)(start_inside ? 20 : 40);
  ws.ws_row = (unsigned short)(start_inside ? 5 : 8);
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_set_statusline(sl, !start_inside, 0) == SL_OK &&
                  (start_inside || sl_output_stream_begin(sl) == SL_OK),
              "full editor stream setup failed");
  memset(&probe, 0, sizeof(probe));
  probe.start_inside = start_inside;
  ASSERT_TRUE(sl_set_idle_callback(sl, full_editor_stream_idle, &probe) ==
                  SL_OK,
              "idle callback setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line == NULL && probe.fired && probe.setup_result == SL_OK &&
                  probe.write_result == SL_OK &&
                  probe.draft_length == (size_t)(start_inside ? 60 : 300),
              "full editor blocked live output or lost its draft");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "stream end failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(strstr(output, "AFTER") != NULL,
              "live fragment missing after full editor render");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
#else
  (void)start_inside;
  TEST("live stream keeps an output row under a full editor and status");
  PASS();
#endif
}

struct finite_recovery_probe {
  int fired;
  int setup_result;
  int invalid[3];
  int recovered[3];
};

struct finite_partial_chunk {
  const char *text;
  int sent;
  int end_result;
};

static int finite_partial_stream(sl_t *sl, void *userdata, const char **chunk,
                                 size_t *length) {
  struct finite_partial_chunk *state = (struct finite_partial_chunk *)userdata;
  (void)sl;
  *chunk = state->sent ? NULL : state->text;
  *length = state->sent ? 0 : strlen(state->text);
  if (state->sent)
    return state->end_result;
  state->sent = 1;
  return SL_OK;
}

static void finite_recovery_idle(sl_t *sl, void *userdata) {
  struct finite_recovery_probe *probe =
      (struct finite_recovery_probe *)userdata;
  static const char *const incomplete[] = {"before\033[", "before\xe2\x82",
                                           "before\033["};
  int i;
  if (probe->fired)
    return;
  probe->fired = 1;
  probe->setup_result = sl_output_stream_begin(sl);
  if (probe->setup_result == SL_OK)
    probe->setup_result = sl_output_stream_end(sl);
  for (i = 0; probe->setup_result == SL_OK && i < 3; i++) {
    struct finite_partial_chunk bad;
    struct one_chunk_once good;
    bad.text = incomplete[i];
    bad.sent = 0;
    bad.end_result = i == 2 ? SL_ERROR : SL_OK;
    probe->invalid[i] = sl_print_above(sl, finite_partial_stream, &bad);
    good.text = "RECOVERED";
    good.sent = 0;
    probe->recovered[i] = sl_print_above(sl, one_chunk_once_stream, &good);
  }
  (void)sl_cancel(sl);
}

static void test_finite_output_recovers_from_partial_sequences(void) {
#if SL_TEST_PTY
  struct winsize ws;
  struct finite_recovery_probe probe;
  sl_config_t config;
  sl_t *sl;
  int master, slave, i, count;
  char output[16384];
  const char *next;
  TEST("finite output recovers after incomplete ANSI, UTF-8, or callback "
       "errors");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master, &slave, NULL, NULL, &ws) == 0, "openpty failed");
  sl_config_init(&config);
  config.input_fd = slave;
  config.output_fd = slave;
  sl = sl_create_with_config(&config);
  memset(&probe, 0, sizeof(probe));
  ASSERT_TRUE(sl && sl_set_idle_callback(sl, finite_recovery_idle, &probe) ==
                        SL_OK,
              "finite recovery setup failed");
  ASSERT_TRUE(sl_readline(sl, "> ") == NULL && probe.fired &&
                  probe.setup_result == SL_OK,
              "native session was not retained during readline");
  for (i = 0; i < 3; i++) {
    ASSERT_TRUE(probe.invalid[i] == (i == 2 ? SL_ERROR : SL_ERROR_INVALID),
                "finite output did not report the original failure");
    ASSERT_TRUE(probe.recovered[i] == SL_OK,
                "partial sequence contaminated the next finite output");
  }
  (void)read_live_pty_output(master, output, sizeof(output));
  next = output;
  count = 0;
  while ((next = strstr(next, "RECOVERED")) != NULL) {
    count++;
    next += strlen("RECOVERED");
  }
  ASSERT_TRUE(count == 3, "recovered finite output was not emitted");
  sl_destroy(sl);
  close(slave);
  close(master);
  PASS();
#else
  TEST("finite output recovers after incomplete sequences");
  PASS();
#endif
}

static void test_free_function_wrappers_use_receiver_methods(void) {
  sl_t *sl;

  TEST("free-function wrappers use receiver methods");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl_history_set_max_len(sl, 2) == SL_OK,
              "wrapper history max failed");
  ASSERT_TRUE(sl_history_add(sl, "alpha") == SL_OK,
              "wrapper history add failed");
  ASSERT_TRUE(sl_set_screen_width(sl, 12) == SL_OK,
              "wrapper screen width failed");
  ASSERT_TRUE(sl_set_live_scroll_region(sl, 1) == SL_OK,
              "wrapper live scroll region failed");
  ASSERT_TRUE(sl_set_image_paste_path_template(
                  sl, "{{XDG_CACHE_HOME}}/softline/*") == SL_OK,
              "wrapper image paste template failed");
  ASSERT_TRUE(sl->set_image_paste_path_template(sl, NULL) == SL_OK,
              "receiver image paste template reset failed");
  ASSERT_TRUE(sl_set_prompt_queue(sl, 1, 4, 2) == SL_OK,
              "wrapper prompt queue failed");
  ASSERT_TRUE(sl_set_prompt_theme(sl, SL_PROMPT_THEME_ACCENT) == SL_OK,
              "wrapper prompt theme failed");
  ASSERT_TRUE(sl_set_statusline(sl, 1, 15) == SL_OK,
              "wrapper status line failed");
  ASSERT_TRUE(sl_set_status_element(sl, 0, "model") == SL_OK,
              "wrapper status element failed");
  ASSERT_TRUE(sl_set_status_busy(sl, 1) == SL_OK, "wrapper status busy failed");
  ASSERT_TRUE(sl_set_status_spinner(sl, 1) == SL_OK,
              "wrapper status spinner failed");
  ASSERT_TRUE(sl_set_status_idle_marker(sl, '-') == SL_OK,
              "wrapper status idle marker failed");
  ASSERT_TRUE(sl_set_status_idle_marker(sl, '\0') == SL_OK,
              "wrapper clear status idle marker failed");
  ASSERT_TRUE(sl_set_buffer(sl, "draft") == SL_OK, "wrapper set_buffer failed");
  ASSERT_TRUE(strcmp(sl_buffer(sl), "draft") == 0, "wrapper buffer mismatch");
  ASSERT_TRUE(sl_set_cursor(sl, 2) == SL_OK, "wrapper set_cursor failed");
  ASSERT_TRUE(sl_cursor(sl) == 2, "wrapper cursor mismatch");
  ASSERT_TRUE(sl_insert(sl, "X") == SL_OK, "wrapper insert failed");
  ASSERT_TRUE(strcmp(sl_buffer(sl), "drXaft") == 0,
              "wrapper insert did not mutate receiver");
  ASSERT_TRUE(sl_last_readline_status(sl) == SL_READLINE_NONE,
              "wrapper initial readline status mismatch");
  ASSERT_TRUE(sl_last_error(sl) == NULL, "wrapper last_error mismatch");
  sl_destroy(sl);
  PASS();
}

static void test_prompt_queue_control_api(void) {
  sl_prompt_queue_delivery_t delivery;
  sl_prompt_queue_keys_t keys;
  sl_prompt_queue_profile_t profile;
  sl_prompt_queue_mode_t mode;
  char *text;
  sl_t *sl;

  TEST("prompt queue control API manages bounded FIFO state");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl_prompt_queue_append(sl, "disabled", 0, NULL) ==
                  SL_ERROR_INVALID,
              "disabled queue accepted mutation");
  ASSERT_TRUE(sl_set_prompt_queue(sl, 1, 3, 2) == SL_OK, "queue enable failed");
  ASSERT_TRUE(sl_prompt_queue_count(sl) == 0, "initial queue count mismatch");
  ASSERT_TRUE(sl_prompt_queue_capacity(sl) == 3, "queue capacity mismatch");
  ASSERT_TRUE(sl_prompt_queue_append(sl, "one", 0, NULL) == SL_OK,
              "append one failed");
  ASSERT_TRUE(sl_prompt_queue_append(sl, "three", 0, NULL) == SL_OK,
              "append three failed");
  ASSERT_TRUE(sl_prompt_queue_insert(sl, 1, "two", 0, NULL) == SL_OK,
              "insert two failed");
  ASSERT_TRUE(sl_prompt_queue_append(sl, "full", 0, NULL) == SL_ERROR_FULL,
              "full queue did not report SL_ERROR_FULL");
  text = NULL;
  ASSERT_TRUE(sl_prompt_queue_peek(sl, 1, &text) == SL_OK, "peek two failed");
  ASSERT_TRUE(strcmp(text, "two") == 0, "peek returned wrong entry");
  sl_free_string(sl, text);
  ASSERT_TRUE(sl_prompt_queue_replace(sl, 1, "second") == SL_OK,
              "replace failed");
  ASSERT_TRUE(sl_prompt_queue_get_mode(sl, 1, &mode) == SL_OK &&
                  mode == SL_PROMPT_QUEUE_MODE_QUEUED,
              "new queue entry did not default to queued mode");
  ASSERT_TRUE(sl_prompt_queue_set_mode(sl, 1, SL_PROMPT_QUEUE_MODE_STEER) ==
                  SL_OK,
              "steer mode update failed");
  ASSERT_TRUE(sl_prompt_queue_get_mode(sl, 1, &mode) == SL_OK &&
                  mode == SL_PROMPT_QUEUE_MODE_STEER,
              "steer mode readback failed");
  ASSERT_TRUE(sl_prompt_queue_replace(sl, 1, "second edit") == SL_OK &&
                  sl_prompt_queue_get_mode(sl, 1, &mode) == SL_OK &&
                  mode == SL_PROMPT_QUEUE_MODE_STEER,
              "replacement lost entry mode");
  ASSERT_TRUE(sl_prompt_queue_set_mode(sl, 1, (sl_prompt_queue_mode_t)9) ==
                  SL_ERROR_INVALID,
              "invalid queue mode accepted");
  text = NULL;
  ASSERT_TRUE(sl_prompt_queue_take(sl, 0, &text) == SL_OK, "take failed");
  ASSERT_TRUE(strcmp(text, "one") == 0, "take FIFO entry mismatch");
  sl_free_string(sl, text);
  ASSERT_TRUE(sl_prompt_queue_count(sl) == 2, "take count mismatch");
  ASSERT_TRUE(sl_prompt_queue_insert(sl, 9, "bad", 0, NULL) == SL_ERROR_INVALID,
              "invalid insertion index accepted");
  ASSERT_TRUE(sl_prompt_queue_replace(sl, 0, "") == SL_ERROR_INVALID,
              "empty replacement accepted");
  ASSERT_TRUE(sl_prompt_queue_enqueue_draft(sl) == SL_ERROR_INVALID,
              "inactive draft enqueue accepted");
  ASSERT_TRUE(sl_set_prompt_queue_delivery(
                  sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK,
              "manual delivery configuration failed");
  delivery = SL_PROMPT_QUEUE_DELIVERY_AUTO;
  ASSERT_TRUE(sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
                  delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL,
              "manual delivery readback mismatch");
  ASSERT_TRUE(sl_set_prompt_queue_profile(
                  sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK,
              "queued-turns profile configuration failed");
  profile = SL_PROMPT_QUEUE_PROFILE_DEFAULT;
  ASSERT_TRUE(sl_get_prompt_queue_profile(sl, &profile) == SL_OK &&
                  profile == SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS,
              "queued-turns profile readback mismatch");
  ASSERT_TRUE(sl_get_prompt_queue_keys(sl, &keys) == SL_OK,
              "queued-turns keys readback failed");
  ASSERT_TRUE(keys.enqueue_draft == SL_KEY_ENTER &&
                  keys.edit_newest == SL_KEY_ALT_E &&
                  keys.submit_or_promote_newest == SL_KEY_ALT_ENTER,
              "queued-turns key defaults mismatch");
  ASSERT_TRUE(sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
                  delivery == SL_PROMPT_QUEUE_DELIVERY_AUTO,
              "idle queued-turns delivery must be automatic");
  ASSERT_TRUE(sl_set_status_busy(sl, 1) == SL_OK,
              "queued-turns busy transition failed");
  ASSERT_TRUE(sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
                  delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL,
              "busy queued-turns delivery must retain turns");
  ASSERT_TRUE(sl_set_status_busy(sl, 0) == SL_OK,
              "queued-turns idle transition failed");
  ASSERT_TRUE(sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
                  delivery == SL_PROMPT_QUEUE_DELIVERY_AUTO,
              "idle queued-turns delivery must release turns");
  ASSERT_TRUE(sl_set_prompt_queue_delivery(
                  sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK &&
                  sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
                  delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL,
              "queued-turns manual delivery configuration failed");
  ASSERT_TRUE(sl_set_status_busy(sl, 1) == SL_OK &&
                  sl_set_status_busy(sl, 0) == SL_OK &&
                  sl_get_prompt_queue_delivery(sl, &delivery) == SL_OK &&
                  delivery == SL_PROMPT_QUEUE_DELIVERY_MANUAL,
              "busy transition lost host-controlled delivery");
  ASSERT_TRUE(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_AUTO) ==
                  SL_OK,
              "queued-turns automatic delivery restore failed");
  keys.enqueue_draft = SL_KEY_NONE;
  ASSERT_TRUE(sl_set_prompt_queue_keys(sl, &keys) == SL_OK,
              "available-state queue key configuration failed");
  keys.edit_newest = SL_KEY_ALT_ENTER;
  ASSERT_TRUE(sl_set_prompt_queue_keys(sl, &keys) == SL_ERROR_INVALID,
              "duplicate queue keys accepted");
  ASSERT_TRUE(sl_prompt_queue_clear(sl) == SL_OK, "queue clear failed");
  ASSERT_TRUE(sl_prompt_queue_count(sl) == 0, "queue clear count mismatch");
  sl_destroy(sl);
  PASS();
}

static void test_set_cursor_clamps_to_utf8_cluster_boundary(void) {
  sl_t *sl;

  TEST("set_cursor clamps to UTF-8 cluster boundary");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");

  ASSERT_TRUE(sl_set_buffer(sl, "\303\245") == SL_OK, "set_buffer failed");
  ASSERT_TRUE(sl_set_cursor(sl, 1) == SL_OK, "set_cursor inside UTF-8 failed");
  ASSERT_TRUE(sl_cursor(sl) == 0, "cursor did not clamp to UTF-8 start");
  ASSERT_TRUE(sl_insert(sl, "x") == SL_OK, "insert failed");
  ASSERT_TRUE(strcmp(sl_buffer(sl), "x\303\245") == 0,
              "insert split UTF-8 sequence");

  ASSERT_TRUE(sl_set_buffer(sl, "ze\314\201y") == SL_OK,
              "set combining buffer failed");
  ASSERT_TRUE(sl_set_cursor(sl, 2) == SL_OK,
              "set_cursor inside combining cluster failed");
  ASSERT_TRUE(sl_cursor(sl) == 1, "cursor did not clamp to cluster start");
  ASSERT_TRUE(sl_insert(sl, "X") == SL_OK, "combining insert failed");
  ASSERT_TRUE(strcmp(sl_buffer(sl), "zXe\314\201y") == 0,
              "insert split combining cluster");

  sl_destroy(sl);
  PASS();
}

static void test_destroy_null(void) {
  TEST("sl_destroy NULL is safe");
  sl_destroy(NULL);
  PASS();
}

static void test_invalid_config_is_rejected(void) {
  sl_config_t cfg;
  sl_t *sl;

  TEST("invalid create config is rejected");
  sl_config_init(&cfg);
  cfg.history_max_len = -1;
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL, "negative history accepted");
  sl_config_init(&cfg);
  cfg.line_max_len = 0;
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL, "zero line max accepted");
  sl_config_init(&cfg);

  cfg.screen_width = -1;
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "negative screen width accepted");
  sl_config_init(&cfg);

  cfg.live_scroll_region = -1;
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "negative live scroll region accepted");

  sl_config_init(&cfg);
  cfg.clear_prompt_on_exit = -1;
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "negative exit clear option accepted");
  sl_config_init(&cfg);
  cfg.disable_image_paste = -1;
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "negative image paste disable option accepted");
  sl_config_init(&cfg);
  cfg.image_paste_path_template = "relative/*";
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "relative image paste template accepted");
  sl_config_init(&cfg);
  cfg.image_paste_path_template = "/tmp/image";
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "image paste template without xid marker accepted");
  sl_config_init(&cfg);
  cfg.image_paste_path_template = "/tmp/*/*";
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "image paste template with two markers accepted");
  sl_config_init(&cfg);
  cfg.image_paste_path_template = "{{XDG_CACHE_DIR}}/*";
  ASSERT_TRUE(sl_create_with_config(&cfg) == NULL,
              "nonstandard XDG token accepted");
  sl_config_init(&cfg);
  cfg.prompt_queue = 1;

  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "normal prompt queue rejected");
  ASSERT_TRUE(sl_set_image_paste_path_template(sl, "relative/*") ==
                  SL_ERROR_INVALID,
              "relative image paste setter accepted");
  ASSERT_TRUE(sl_set_image_paste_path_template(sl, "~/images/*") == SL_OK,
              "home image paste setter failed");
  sl->destroy(sl);
  PASS();
}

static void test_invalid_receiver_arguments(void) {
  sl_t *sl;

  TEST("invalid receiver arguments return errors");
  ASSERT_TRUE(sl_readline(NULL, "p> ") == NULL, "NULL readline accepted");
  ASSERT_TRUE(sl_history_add(NULL, "x") == SL_ERROR_INVALID,
              "NULL history_add accepted");
  ASSERT_TRUE(sl_history_set_max_len(NULL, 1) == SL_ERROR_INVALID,
              "NULL history_set_max_len accepted");
  ASSERT_TRUE(sl_history_save(NULL, "x") == SL_ERROR_INVALID,
              "NULL history_save accepted");
  ASSERT_TRUE(sl_history_load(NULL, "x") == SL_ERROR_INVALID,
              "NULL history_load accepted");
  ASSERT_TRUE(sl_set_screen_width(NULL, 1) == SL_ERROR_INVALID,
              "NULL set_screen_width accepted");
  ASSERT_TRUE(sl_output_stream_begin(NULL) == SL_ERROR_INVALID &&
                  sl_output_stream_write(NULL, "x", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_end(NULL) == SL_ERROR_INVALID,
              "NULL output stream handle accepted");
  ASSERT_TRUE(sl_set_live_scroll_region(NULL, 1) == SL_ERROR_INVALID,
              "NULL set_live_scroll_region accepted");
  ASSERT_TRUE(sl_set_idle_callback(NULL, NULL, NULL) == SL_ERROR_INVALID,
              "NULL set_idle_callback accepted");
  ASSERT_TRUE(sl_watch_add(NULL, -1, 0, NULL, NULL, NULL) == SL_ERROR_INVALID,
              "NULL watch_add accepted");
  ASSERT_TRUE(sl_watch_modify(NULL, 1, SL_WATCH_READ) == SL_ERROR_INVALID,
              "NULL watch_modify accepted");
  ASSERT_TRUE(sl_watch_remove(NULL, 1) == SL_ERROR_INVALID,
              "NULL watch_remove accepted");
  ASSERT_TRUE(sl_watch_clear(NULL) == SL_ERROR_INVALID,
              "NULL watch_clear accepted");
  ASSERT_TRUE(sl_bind_key(NULL, SL_KEY_TAB, NULL, NULL) == SL_ERROR_INVALID,
              "NULL bind_key accepted");
  ASSERT_TRUE(sl_insert(NULL, "x") == SL_ERROR_INVALID, "NULL insert accepted");
  ASSERT_TRUE(sl_set_buffer(NULL, "x") == SL_ERROR_INVALID,
              "NULL set_buffer accepted");
  ASSERT_TRUE(sl_next_prompt(NULL, "p> ", NULL) == NULL,
              "NULL next_prompt accepted");
  ASSERT_TRUE(sl_set_prompt_queue(NULL, 1, 1, 1) == SL_ERROR_INVALID,
              "NULL prompt queue accepted");
  ASSERT_TRUE(sl_set_prompt_theme(NULL, SL_PROMPT_THEME_PLAIN) ==
                  SL_ERROR_INVALID,
              "NULL prompt theme accepted");
  ASSERT_TRUE(sl_set_statusline(NULL, 1, 0) == SL_ERROR_INVALID,
              "NULL status line accepted");
  ASSERT_TRUE(sl_set_status_elements(NULL, NULL, 0) == SL_ERROR_INVALID,
              "NULL status elements accepted");
  ASSERT_TRUE(sl_set_status_element(NULL, 0, "x") == SL_ERROR_INVALID,
              "NULL status element accepted");
  ASSERT_TRUE(sl_set_status_busy(NULL, 1) == SL_ERROR_INVALID,
              "NULL status busy accepted");
  ASSERT_TRUE(sl_set_status_spinner(NULL, 1) == SL_ERROR_INVALID,
              "NULL status spinner accepted");
  ASSERT_TRUE(sl_set_status_idle_marker(NULL, '-') == SL_ERROR_INVALID,
              "NULL status idle marker accepted");
  ASSERT_TRUE(sl_buffer(NULL) == NULL, "NULL buffer returned text");
  ASSERT_TRUE(sl_cursor(NULL) == 0, "NULL cursor returned offset");
  ASSERT_TRUE(sl_set_cursor(NULL, 0) == SL_ERROR_INVALID,
              "NULL set_cursor accepted");
  ASSERT_TRUE(sl_submit(NULL) == SL_ERROR_INVALID, "NULL submit accepted");
  ASSERT_TRUE(sl_cancel(NULL) == SL_ERROR_INVALID, "NULL cancel accepted");
  ASSERT_TRUE(sl_print_above(NULL, empty_stream, NULL) == SL_ERROR_INVALID,
              "NULL print_above accepted");
  ASSERT_TRUE(sl_last_readline_status(NULL) == SL_READLINE_NONE,
              "NULL last_readline_status returned status");
  ASSERT_TRUE(sl_last_error(NULL) == NULL, "NULL last_error returned text");

  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl->history_add(sl, NULL) == SL_ERROR_INVALID,
              "NULL history entry accepted");
  ASSERT_TRUE(sl->history_set_max_len(sl, -1) == SL_ERROR_INVALID,
              "negative history max accepted");
  ASSERT_TRUE(sl->set_screen_width(sl, -1) == SL_ERROR_INVALID,
              "negative screen width accepted");
  ASSERT_TRUE(sl->set_live_scroll_region(sl, -1) == SL_ERROR_INVALID,
              "negative live scroll region accepted");
  ASSERT_TRUE(sl->set_prompt_queue(sl, 1, 1, 1) == SL_OK,
              "normal prompt queue rejected");
  ASSERT_TRUE(sl->set_prompt_theme(sl, (sl_prompt_theme_t)99) ==
                  SL_ERROR_INVALID,
              "invalid prompt theme accepted");
  ASSERT_TRUE(sl->set_statusline(sl, -1, 0) == SL_ERROR_INVALID,
              "negative status line accepted");
  ASSERT_TRUE(sl->set_status_elements(sl, NULL, 1) == SL_ERROR_INVALID,
              "NULL status elements accepted");
  ASSERT_TRUE(sl->set_status_element(sl, SL_STATUS_MAX_ELEMENTS, "x") ==
                  SL_ERROR_INVALID,
              "out-of-range status element accepted");
  ASSERT_TRUE(sl->set_status_element(sl, 0, "bad\ntext") == SL_ERROR_INVALID,
              "status control text accepted");
  ASSERT_TRUE(sl->set_status_element(sl, 0, "bad\23331m") == SL_ERROR_INVALID,
              "status raw C1 control accepted");
  ASSERT_TRUE(sl->set_status_element(sl, 0, "bad\302\23331m") ==
                  SL_ERROR_INVALID,
              "status UTF-8 C1 control accepted");
  ASSERT_TRUE(sl->set_status_element(sl, 0, "bad\302text") == SL_ERROR_INVALID,
              "status invalid UTF-8 accepted");
  ASSERT_TRUE(sl->set_status_busy(sl, -1) == SL_ERROR_INVALID,
              "negative status busy accepted");
  ASSERT_TRUE(sl->set_status_spinner(sl, -1) == SL_ERROR_INVALID,
              "negative status spinner accepted");
  ASSERT_TRUE(sl->set_status_idle_marker(sl, '\n') == SL_ERROR_INVALID,
              "status control idle marker accepted");
  ASSERT_TRUE(sl->bind_key(sl, SL_KEY_NONE, NULL, NULL) == SL_ERROR_INVALID,
              "SL_KEY_NONE binding accepted");
  ASSERT_TRUE(sl->insert(sl, NULL) == SL_ERROR_INVALID,
              "NULL insert text accepted");
  ASSERT_TRUE(sl->set_buffer(sl, NULL) == SL_ERROR_INVALID,
              "NULL set_buffer text accepted");
  ASSERT_TRUE(sl->submit(sl) == SL_ERROR_INVALID, "inactive submit accepted");
  ASSERT_TRUE(sl->cancel(sl) == SL_ERROR_INVALID, "inactive cancel accepted");
  ASSERT_TRUE(sl->print_above(sl, NULL, NULL) == SL_ERROR_INVALID,
              "NULL stream callback accepted");
  ASSERT_TRUE(sl->last_error(sl) != NULL, "last_error not set");
  sl->destroy(sl);
  PASS();
}

static void test_plain_readline(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;
  char out[64];
  ssize_t n;

  TEST("non-tty readline submits on newline");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  ASSERT_TRUE(write(in_pipe[1], "hello\n", 6) == 6, "write input failed");
  close(in_pipe[1]);

  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  line = sl->readline(sl, "p> ");
  ASSERT_TRUE(line != NULL, "readline returned NULL");
  ASSERT_TRUE(strcmp(line, "hello") == 0, "line mismatch");
  ASSERT_TRUE(sl->last_readline_status(sl) == SL_READLINE_SUBMITTED,
              "submitted readline status mismatch");
  sl->free_string(sl, line);
  sl->destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[1]);
  n = read(out_pipe[0], out, sizeof(out) - 1);
  ASSERT_TRUE(n >= 0, "read output failed");
  out[n] = '\0';
  close(out_pipe[0]);
  ASSERT_TRUE(strcmp(out, "") == 0, "non-tty readline emitted prompt output");
  PASS();
}

static void test_plain_next_prompt_is_direct(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_prompt_source_t source;
  sl_t *sl;
  char *line;

  TEST("non-tty next_prompt reports direct source");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  ASSERT_TRUE(write(in_pipe[1], "hello\n", 6) == 6, "write input failed");
  close(in_pipe[1]);

  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  source = SL_PROMPT_SOURCE_NONE;
  line = sl_next_prompt(sl, "p> ", &source);
  ASSERT_TRUE(line != NULL, "next_prompt returned NULL");
  ASSERT_TRUE(strcmp(line, "hello") == 0, "next_prompt line mismatch");
  ASSERT_TRUE(source == SL_PROMPT_SOURCE_DIRECT,
              "non-tty next_prompt source was not direct");
  ASSERT_TRUE(sl_last_readline_status(sl) == SL_READLINE_SUBMITTED,
              "non-tty next_prompt status mismatch");
  sl_free_string(sl, line);
  sl_destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[0]);
  close(out_pipe[1]);
  PASS();
}

static void test_plain_readline_uses_default_prompt(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;
  char out[64];
  ssize_t n;

  TEST("non-tty default prompt stays silent");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  ASSERT_TRUE(write(in_pipe[1], "hello\n", 6) == 6, "write input failed");
  close(in_pipe[1]);

  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  line = sl->readline(sl, NULL);
  ASSERT_TRUE(line != NULL, "readline returned NULL");
  ASSERT_TRUE(strcmp(line, "hello") == 0, "line mismatch");
  sl->free_string(sl, line);
  sl->destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[1]);
  n = read(out_pipe[0], out, sizeof(out) - 1);
  ASSERT_TRUE(n >= 0, "read output failed");
  out[n] = '\0';
  close(out_pipe[0]);
  ASSERT_TRUE(strcmp(out, "") == 0, "non-tty default prompt emitted output");
  PASS();
}

static void test_plain_readline_consumes_crlf_once(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;

  TEST("non-tty readline consumes CRLF as one line ending");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  ASSERT_TRUE(write(in_pipe[1], "one\r\ntwo\r\n", 10) == 10,
              "write input failed");
  close(in_pipe[1]);

  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");

  line = sl->readline(sl, "p> ");
  ASSERT_TRUE(line != NULL, "first readline returned NULL");
  ASSERT_TRUE(strcmp(line, "one") == 0, "first line mismatch");
  sl->free_string(sl, line);

  line = sl->readline(sl, "p> ");
  ASSERT_TRUE(line != NULL, "second readline returned NULL");
  ASSERT_TRUE(strcmp(line, "two") == 0, "second line mismatch");
  sl->free_string(sl, line);

  sl->destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[0]);
  close(out_pipe[1]);
  PASS();
}

static void test_plain_readline_empty_eof_returns_null(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;

  TEST("non-tty empty EOF returns NULL");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  close(in_pipe[1]);
  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  line = sl->readline(sl, "p> ");
  ASSERT_TRUE(line == NULL, "empty EOF returned a line");
  ASSERT_TRUE(sl_last_readline_status(sl) == SL_READLINE_EOF,
              "EOF readline status mismatch");
  sl->destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[0]);
  close(out_pipe[1]);
  PASS();
}

static void test_plain_readline_retries_eintr(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;
  pid_t pid;
  int status;
  struct sigaction sa;
  struct sigaction old_sa;

  TEST("non-tty readline retries interrupted reads");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = ignore_signal;
  sigemptyset(&sa.sa_mask);
  ASSERT_TRUE(sigaction(SIGUSR1, &sa, &old_sa) == 0, "sigaction failed");
  signal_seen = 0;

  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    close(in_pipe[0]);
    close(out_pipe[0]);
    close(out_pipe[1]);
    usleep(50000);
    (void)kill(getppid(), SIGUSR1);
    usleep(50000);
    (void)write(in_pipe[1], "hello\n", 6);
    close(in_pipe[1]);
    _exit(0);
  }

  close(in_pipe[1]);
  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  line = sl->readline(sl, "p> ");
  ASSERT_TRUE(line != NULL, "readline returned NULL after EINTR");
  ASSERT_TRUE(strcmp(line, "hello") == 0, "line mismatch after EINTR");
  ASSERT_TRUE(sl->last_readline_status(sl) == SL_READLINE_SUBMITTED,
              "submitted readline status mismatch after EINTR");
  sl->free_string(sl, line);
  sl->destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[0]);
  close(out_pipe[1]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "signal writer failed");
  ASSERT_TRUE(sigaction(SIGUSR1, &old_sa, NULL) == 0,
              "restore sigaction failed");
  PASS();
}

static void test_plain_readline_stops_at_line_max(void) {
  int in_pipe[2];
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;

  TEST("non-tty readline stops at configured line max");
  ASSERT_TRUE(pipe(in_pipe) == 0, "input pipe failed");
  ASSERT_TRUE(pipe(out_pipe) == 0, "output pipe failed");
  ASSERT_TRUE(write(in_pipe[1], "abcdef\n", 7) == 7, "write input failed");
  close(in_pipe[1]);

  sl_config_init(&cfg);
  cfg.input_fd = in_pipe[0];
  cfg.output_fd = out_pipe[1];
  cfg.line_max_len = 4;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  line = sl->readline(sl, "p> ");
  ASSERT_TRUE(line != NULL, "readline returned NULL");
  ASSERT_TRUE(strcmp(line, "abcd") == 0, "line max result mismatch");
  ASSERT_TRUE(sl->last_error(sl) != NULL, "line max error not recorded");
  sl->free_string(sl, line);
  sl->destroy(sl);
  close(in_pipe[0]);
  close(out_pipe[0]);
  close(out_pipe[1]);
  PASS();
}

static void test_readline_status_is_per_instance(void) {
  int in_pipe1[2];
  int in_pipe2[2];
  sl_config_t cfg;
  sl_t *sl1;
  sl_t *sl2;
  char *line;

  TEST("readline status is per-instance");
  ASSERT_TRUE(pipe(in_pipe1) == 0, "first input pipe failed");
  ASSERT_TRUE(pipe(in_pipe2) == 0, "second input pipe failed");
  ASSERT_TRUE(write(in_pipe1[1], "one\n", 4) == 4, "write first input failed");
  close(in_pipe1[1]);
  close(in_pipe2[1]);

  sl_config_init(&cfg);
  cfg.input_fd = in_pipe1[0];
  sl1 = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl1 != NULL, "first create failed");
  sl_config_init(&cfg);
  cfg.input_fd = in_pipe2[0];
  sl2 = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl2 != NULL, "second create failed");

  line = sl1->readline(sl1, "p> ");
  ASSERT_TRUE(line != NULL, "first readline returned NULL");
  ASSERT_TRUE(strcmp(line, "one") == 0, "first readline mismatch");
  sl1->free_string(sl1, line);
  line = sl2->readline(sl2, "p> ");
  ASSERT_TRUE(line == NULL, "second readline returned text");
  ASSERT_TRUE(sl1->last_readline_status(sl1) == SL_READLINE_SUBMITTED,
              "first instance status changed");
  ASSERT_TRUE(sl2->last_readline_status(sl2) == SL_READLINE_EOF,
              "second instance status mismatch");

  sl1->destroy(sl1);
  sl2->destroy(sl2);
  close(in_pipe1[0]);
  close(in_pipe2[0]);
  PASS();
}

static void test_history_is_per_instance(void) {
  char file1[] = "/tmp/softline-history-1-XXXXXX";
  char file2[] = "/tmp/softline-history-2-XXXXXX";
  int fd1;
  int fd2;
  sl_t *a;
  sl_t *b;
  FILE *fp;
  char buf[128];

  TEST("history is per-instance");
  fd1 = mkstemp(file1);
  fd2 = mkstemp(file2);
  ASSERT_TRUE(fd1 >= 0 && fd2 >= 0, "mkstemp failed");
  close(fd1);
  close(fd2);
  a = sl_create();
  b = sl_create();
  ASSERT_TRUE(a != NULL && b != NULL, "create failed");
  ASSERT_TRUE(a->history_add(a, "alpha") == SL_OK, "history add a failed");
  ASSERT_TRUE(b->history_add(b, "beta") == SL_OK, "history add b failed");
  ASSERT_TRUE(a->history_save(a, file1) == SL_OK, "history save a failed");
  ASSERT_TRUE(b->history_save(b, file2) == SL_OK, "history save b failed");
  fp = fopen(file1, "r");
  ASSERT_TRUE(fp != NULL, "open file1 failed");
  ASSERT_TRUE(fgets(buf, sizeof(buf), fp) != NULL, "read file1 failed");
  fclose(fp);
  ASSERT_TRUE(strcmp(buf, "alpha\n") == 0, "file1 mismatch");
  fp = fopen(file2, "r");
  ASSERT_TRUE(fp != NULL, "open file2 failed");
  ASSERT_TRUE(fgets(buf, sizeof(buf), fp) != NULL, "read file2 failed");
  fclose(fp);
  ASSERT_TRUE(strcmp(buf, "beta\n") == 0, "file2 mismatch");
  a->destroy(a);
  b->destroy(b);
  unlink(file1);
  unlink(file2);
  PASS();
}

static void test_history_io_failures_are_reported(void) {
  sl_t *sl;

  TEST("history I/O failures are reported");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl->history_save(sl, NULL) == SL_ERROR_INVALID,
              "NULL save path accepted");
  ASSERT_TRUE(sl->history_load(sl, NULL) == SL_ERROR_INVALID,
              "NULL load path accepted");
  ASSERT_TRUE(sl->history_load(sl, "/tmp/softline-no-such-file") == SL_ERROR_IO,
              "missing history load did not fail");
  ASSERT_TRUE(sl->last_error(sl) != NULL, "history error not recorded");
  sl->destroy(sl);
  PASS();
}

static void test_history_save_uses_private_permissions(void) {
  char file[] = "/tmp/softline-history-private-XXXXXX";
  int fd;
  struct stat st;
  sl_t *sl;

  TEST("history save forces owner-only permissions");
  fd = mkstemp(file);
  ASSERT_TRUE(fd >= 0, "mkstemp failed");
  close(fd);
  ASSERT_TRUE(chmod(file, 0666) == 0, "chmod fixture failed");
  sl = sl_create();
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl->history_add(sl, "secret") == SL_OK, "history add failed");
  ASSERT_TRUE(sl->history_save(sl, file) == SL_OK, "history save failed");
  ASSERT_TRUE(stat(file, &st) == 0, "stat failed");
  ASSERT_TRUE((st.st_mode & 0777) == 0600, "history file is not private");
  sl->destroy(sl);
  unlink(file);
  PASS();
}

static void test_history_round_trips_multiline_entries(void) {
  char file1[] = "/tmp/softline-history-multiline-1-XXXXXX";
  char file2[] = "/tmp/softline-history-multiline-2-XXXXXX";
  int fd1;
  int fd2;
  sl_t *a;
  sl_t *b;
  FILE *fp;
  char buf[128];

  TEST("history save/load preserves multiline entries");
  fd1 = mkstemp(file1);
  fd2 = mkstemp(file2);
  ASSERT_TRUE(fd1 >= 0 && fd2 >= 0, "mkstemp failed");
  close(fd1);
  close(fd2);
  a = sl_create();
  b = sl_create();
  ASSERT_TRUE(a != NULL && b != NULL, "create failed");
  ASSERT_TRUE(a->history_add(a, "alpha\nbeta\\gamma") == SL_OK,
              "history add failed");
  ASSERT_TRUE(a->history_save(a, file1) == SL_OK, "history save failed");
  fp = fopen(file1, "r");
  ASSERT_TRUE(fp != NULL, "open saved history failed");
  ASSERT_TRUE(fgets(buf, sizeof(buf), fp) != NULL, "read saved history failed");
  fclose(fp);
  ASSERT_TRUE(strcmp(buf, "alpha\\nbeta\\\\gamma\n") == 0,
              "encoded history mismatch");
  ASSERT_TRUE(b->history_load(b, file1) == SL_OK, "history load failed");
  ASSERT_TRUE(b->history_save(b, file2) == SL_OK, "history resave failed");
  fp = fopen(file2, "r");
  ASSERT_TRUE(fp != NULL, "open resaved history failed");
  ASSERT_TRUE(fgets(buf, sizeof(buf), fp) != NULL,
              "read resaved history failed");
  fclose(fp);
  ASSERT_TRUE(strcmp(buf, "alpha\\nbeta\\\\gamma\n") == 0,
              "round-trip history mismatch");
  a->destroy(a);
  b->destroy(b);
  unlink(file1);
  unlink(file2);
  PASS();
}

static void test_history_rejects_oversized_entries(void) {
  char file1[] = "/tmp/softline-history-oversized-1-XXXXXX";
  char file2[] = "/tmp/softline-history-oversized-2-XXXXXX";
  int fd1;
  int fd2;
  sl_config_t cfg;
  sl_t *sl;
  FILE *fp;
  char buf[128];

  TEST("history rejects oversized entries without splitting");
  fd1 = mkstemp(file1);
  fd2 = mkstemp(file2);
  ASSERT_TRUE(fd1 >= 0 && fd2 >= 0, "mkstemp failed");
  fp = fdopen(fd1, "w");
  ASSERT_TRUE(fp != NULL, "fdopen failed");
  ASSERT_TRUE(fputs("123456789\nok\n", fp) >= 0, "write fixture failed");
  ASSERT_TRUE(fclose(fp) == 0, "close fixture failed");
  close(fd2);
  sl_config_init(&cfg);
  cfg.line_max_len = 4;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl->history_add(sl, "12345") == SL_ERROR_INVALID,
              "oversized history add accepted");
  ASSERT_TRUE(sl->history_load(sl, file1) == SL_ERROR_INVALID,
              "oversized history load accepted");
  ASSERT_TRUE(sl->history_save(sl, file2) == SL_OK, "history save failed");
  fp = fopen(file2, "r");
  ASSERT_TRUE(fp != NULL, "open saved history failed");
  ASSERT_TRUE(fgets(buf, sizeof(buf), fp) != NULL, "read saved history failed");
  ASSERT_TRUE(strcmp(buf, "ok\n") == 0, "oversized record was split or kept");
  ASSERT_TRUE(fgets(buf, sizeof(buf), fp) == NULL,
              "unexpected extra history record");
  fclose(fp);
  sl->destroy(sl);
  unlink(file1);
  unlink(file2);
  PASS();
}

static void test_stream_failures_are_reported(void) {
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;

  TEST("print_above stream failures are reported");
  ASSERT_TRUE(pipe(out_pipe) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  ASSERT_TRUE(sl->print_above(sl, failing_stream, NULL) == SL_ERROR,
              "stream callback failure not propagated");
  ASSERT_TRUE(sl->print_above(sl, invalid_chunk_stream, NULL) ==
                  SL_ERROR_INVALID,
              "invalid stream chunk accepted");
  close(out_pipe[1]);
  ASSERT_TRUE(sl->print_above(sl, one_chunk_stream, "x") == SL_ERROR_IO,
              "closed output fd did not fail");
  sl->destroy(sl);
  close(out_pipe[0]);
  PASS();
}

static void test_print_above_uses_lf_for_non_tty_output(void) {
  int out_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char out[64];
  ssize_t n;
  struct one_chunk_once stream;

  TEST("print_above uses LF for non-tty output");
  ASSERT_TRUE(pipe(out_pipe) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = out_pipe[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "create failed");
  stream.text = "alpha\n";
  stream.sent = 0;
  ASSERT_TRUE(sl->print_above(sl, one_chunk_once_stream, &stream) == SL_OK,
              "print_above failed");
  sl->destroy(sl);
  close(out_pipe[1]);
  n = read(out_pipe[0], out, sizeof(out) - 1);
  ASSERT_TRUE(n >= 0, "read output failed");
  out[n] = '\0';
  close(out_pipe[0]);
  ASSERT_TRUE(strcmp(out, "alpha\n") == 0,
              "non-tty stream output did not use LF");
  PASS();
}

static int contains_bytes(const char *haystack, const char *needle) {
  return strstr(haystack, needle) != NULL;
}

static int count_bytes(const char *haystack, const char *needle) {
  int count;
  size_t needle_len;
  const char *p;
  count = 0;
  needle_len = strlen(needle);
  if (needle_len == 0)
    return 0;
  p = haystack;
  while ((p = strstr(p, needle)) != NULL) {
    count++;
    p += needle_len;
  }
  return count;
}

static int contains_after_bytes(const char *haystack, const char *first,
                                const char *second) {
  const char *p;
  p = strstr(haystack, first);
  if (!p)
    return 0;
  p += strlen(first);
  return strstr(p, second) != NULL;
}

struct vt_screen {
  int rows;
  int cols;
  int row;
  int col;
  int scroll_top;
  int scroll_bottom;
  int saved_row;
  int saved_col;
  char cells[24][120];
  unsigned int history_count;
  char history[64][120];
};

static void vt_clear(struct vt_screen *screen) {
  int r;
  int c;
  for (r = 0; r < screen->rows; r++) {
    for (c = 0; c < screen->cols; c++)
      screen->cells[r][c] = ' ';
    screen->cells[r][screen->cols] = '\0';
  }
  screen->row = 0;
  screen->col = 0;
  screen->scroll_top = 0;
  screen->scroll_bottom = screen->rows - 1;
}

static void vt_init(struct vt_screen *screen, int rows, int cols) {
  memset(screen, 0, sizeof(*screen));
  screen->rows = rows > 24 ? 24 : rows;
  screen->cols = cols > 119 ? 119 : cols;
  vt_clear(screen);
}

static void vt_scroll_region(struct vt_screen *screen, int top, int bottom) {
  int r;
  if (top < 0)
    top = 0;
  if (bottom >= screen->rows)
    bottom = screen->rows - 1;
  if (bottom < top)
    return;
  if (top == 0 && bottom == screen->rows - 1) {
    memcpy(screen->history[screen->history_count % 64u], screen->cells[0],
           (size_t)screen->cols + 1);
    screen->history_count++;
  }
  for (r = top + 1; r <= bottom; r++)
    memcpy(screen->cells[r - 1], screen->cells[r], (size_t)screen->cols + 1);
  memset(screen->cells[bottom], ' ', (size_t)screen->cols);
  screen->cells[bottom][screen->cols] = '\0';
}

static int vt_history_contains(const struct vt_screen *screen,
                               const char *needle) {
  unsigned int count;
  unsigned int i;
  count = screen->history_count < 64u ? screen->history_count : 64u;
  for (i = 0; i < count; i++)
    if (strstr(screen->history[i], needle))
      return 1;
  return 0;
}

static void vt_scroll(struct vt_screen *screen) {
  vt_scroll_region(screen, screen->scroll_top, screen->scroll_bottom);
  screen->row = screen->scroll_bottom;
}

static void vt_lf(struct vt_screen *screen) {
  if (screen->row == screen->scroll_bottom) {
    vt_scroll_region(screen, screen->scroll_top, screen->scroll_bottom);
    return;
  }
  screen->row++;
  if (screen->row >= screen->rows)
    vt_scroll(screen);
}

static void vt_put(struct vt_screen *screen, char ch) {
  if (screen->col >= screen->cols) {
    screen->col = 0;
    vt_lf(screen);
  }
  screen->cells[screen->row][screen->col] = ch;
  screen->col++;
}

static const char *vt_parse_number(const char *p, int *value) {
  int v;
  v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    p++;
  }
  *value = v;
  return p;
}

static const char *vt_csi(struct vt_screen *screen, const char *p) {
  int a;
  int b;
  int have_a;
  if (*p == '?') {
    while (*p && ((*p < '@') || (*p > '~')))
      p++;
    return *p ? p + 1 : p;
  }
  a = 0;
  b = 0;
  have_a = 0;
  if (*p >= '0' && *p <= '9') {
    have_a = 1;
    p = vt_parse_number(p, &a);
  }
  if (*p == ';') {
    p++;
    if (*p >= '0' && *p <= '9')
      p = vt_parse_number(p, &b);
  }
  while (*p && (*p < '@' || *p > '~'))
    p++;
  switch (*p) {
  case 'A':
    screen->row -= have_a && a > 0 ? a : 1;
    if (screen->row < 0)
      screen->row = 0;
    break;
  case 'B':
    screen->row += have_a && a > 0 ? a : 1;
    if (screen->row >= screen->rows)
      screen->row = screen->rows - 1;
    break;
  case 'C':
    screen->col += have_a && a > 0 ? a : 1;
    if (screen->col >= screen->cols)
      screen->col = screen->cols - 1;
    break;
  case 'D':
    screen->col -= have_a && a > 0 ? a : 1;
    if (screen->col < 0)
      screen->col = 0;
    break;
  case 'H':
  case 'f':
    screen->row = have_a && a > 0 ? a - 1 : 0;
    screen->col = b > 0 ? b - 1 : 0;
    if (screen->row < 0)
      screen->row = 0;
    if (screen->row >= screen->rows)
      screen->row = screen->rows - 1;
    if (screen->col < 0)
      screen->col = 0;
    if (screen->col >= screen->cols)
      screen->col = screen->cols - 1;
    break;
  case 'r':
    if (!have_a) {
      screen->scroll_top = 0;
      screen->scroll_bottom = screen->rows - 1;
    } else {
      screen->scroll_top = a > 0 ? a - 1 : 0;
      screen->scroll_bottom = b > 0 ? b - 1 : screen->rows - 1;
      if (screen->scroll_top < 0)
        screen->scroll_top = 0;
      if (screen->scroll_bottom >= screen->rows)
        screen->scroll_bottom = screen->rows - 1;
      if (screen->scroll_bottom < screen->scroll_top)
        screen->scroll_bottom = screen->scroll_top;
    }
    screen->row = screen->col = 0;
    break;
  case 'L': {
    int count = have_a && a > 0 ? a : 1;
    int row;
    if (screen->row < screen->scroll_top || screen->row > screen->scroll_bottom)
      break;
    if (count > screen->scroll_bottom - screen->row + 1)
      count = screen->scroll_bottom - screen->row + 1;
    for (row = screen->scroll_bottom; row >= screen->row + count; row--)
      memcpy(screen->cells[row], screen->cells[row - count],
             (size_t)screen->cols + 1);
    for (row = screen->row; row < screen->row + count; row++) {
      memset(screen->cells[row], ' ', (size_t)screen->cols);
      screen->cells[row][screen->cols] = 0;
    }
    break;
  }
  case 'S': {
    int count = have_a && a > 0 ? a : 1;
    while (count-- > 0)
      vt_scroll_region(screen, screen->scroll_top, screen->scroll_bottom);
    break;
  }
  case 'J':
    if (a == 2)
      vt_clear(screen);
    break;
  case 'K':
    memset(screen->cells[screen->row] + screen->col, ' ',
           (size_t)(screen->cols - screen->col));
    break;
  case 'X': {
    int count = have_a && a > 0 ? a : 1;
    if (count > screen->cols - screen->col)
      count = screen->cols - screen->col;
    memset(screen->cells[screen->row] + screen->col, ' ', (size_t)count);
    break;
  }
  default:
    break;
  }
  return *p ? p + 1 : p;
}

static void vt_apply(struct vt_screen *screen, const char *bytes) {
  const char *p;
  p = bytes;
  while (*p) {
    if (*p == '\033' && (p[1] == '7' || p[1] == '8')) {
      if (p[1] == '7') {
        screen->saved_row = screen->row;
        screen->saved_col = screen->col;
      } else {
        screen->row = screen->saved_row < screen->rows ? screen->saved_row
                                                       : screen->rows - 1;
        screen->col = screen->saved_col <= screen->cols ? screen->saved_col
                                                        : screen->cols - 1;
      }
      p += 2;
      continue;
    }
    if (*p == '\033' && p[1] == '[') {
      p = vt_csi(screen, p + 2);
      continue;
    }
    if (*p == '\r') {
      screen->col = 0;
    } else if (*p == '\n') {
      vt_lf(screen);
    } else if ((unsigned char)*p >= 32) {
      vt_put(screen, *p);
    }
    p++;
  }
}

static int vt_contains(struct vt_screen *screen, const char *needle) {
  int r;
  for (r = 0; r < screen->rows; r++) {
    if (strstr(screen->cells[r], needle))
      return 1;
  }
  return 0;
}

static int vt_count(struct vt_screen *screen, const char *needle) {
  int r;
  int count;
  size_t needle_len;
  count = 0;
  needle_len = strlen(needle);
  if (needle_len == 0)
    return 0;
  for (r = 0; r < screen->rows; r++) {
    const char *p;
    p = screen->cells[r];
    while ((p = strstr(p, needle)) != NULL) {
      count++;
      p += needle_len;
    }
  }
  return count;
}

static void vt_dump(struct vt_screen *screen) {
  int r;
  for (r = 0; r < screen->rows; r++)
    fprintf(stderr, "\n%02d:%s", r, screen->cells[r]);
  fprintf(stderr, "\n");
}

static ssize_t read_some_with_timeout(int fd, char *buf, size_t cap) {
  fd_set readfds;
  struct timeval tv;
  int ready;
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  do {
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    ready = select(fd + 1, &readfds, NULL, NULL, &tv);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0)
    return ready;
  do {
    ready = (int)read(fd, buf, cap);
  } while (ready < 0 && errno == EINTR);
  return ready;
}

static ssize_t read_some_with_timeout_ms(int fd, char *buf, size_t cap,
                                         long timeout_ms) {
  fd_set readfds;
  struct timeval tv;
  int ready;
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;
  do {
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);
    ready = select(fd + 1, &readfds, NULL, NULL, &tv);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0)
    return ready;
  do {
    ready = (int)read(fd, buf, cap);
  } while (ready < 0 && errno == EINTR);
  return ready;
}

static void test_redirected_live_output_validates_stream(void) {
  static const char expected[] = "\xc3\xa4\033[31mXZH";
  sl_config_t cfg;
  sl_t *sl;
  int output[2];
  char bytes[64];
  ssize_t amount;

  TEST("redirected live output validates split ANSI and UTF-8");
  ASSERT_TRUE(pipe(output) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = output[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "\xc3", 1) == SL_OK &&
                  sl_output_stream_end(sl) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\xa4\033[31", 5) == SL_OK &&
                  sl_output_stream_end(sl) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "m", 1) == SL_OK &&
                  sl_output_stream_write(sl, "\001", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "X", 1) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "redirected stream did not enforce byte protocol");
  ASSERT_TRUE(sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "\xf0\x80\x80\x80", 4) ==
                      SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\x80", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\xc2\x9b", 2) ==
                      SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "Z", 1) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "redirected stream accepted malformed UTF-8 or a C1 control");
  ASSERT_TRUE(sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "\033[2", 3) == SL_OK &&
                  sl_output_stream_write(sl, "J", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "H", 1) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "invalid ANSI sequence leaked into subsequent output");
  sl_destroy(sl);
  close(output[1]);
  amount = read(output[0], bytes, sizeof(bytes));
  close(output[0]);
  ASSERT_TRUE(amount == (ssize_t)(sizeof(expected) - 1) &&
                  memcmp(bytes, expected, sizeof(expected) - 1) == 0,
              "redirected stream did not forward its valid prefix exactly");
  PASS();
}

static void test_quoted_prompt_output_api(void) {
  static const char expected[] =
      "answer\n\n? one two\n? three four\n\n>> a\n>> b    c\n\n> *md*\n\n"
      "> \xe7\x95\x8c\xe7\x95\x8c\n> \xe7\x95\x8c\n\n";
  sl_config_t cfg;
  sl_quote_style_t style;
  sl_t *sl;
  int output[2];
  char bytes[256];
  ssize_t amount;

  TEST(
      "quoted prompt API wraps, separates, configures, and preserves Markdown");
  ASSERT_TRUE(pipe(output) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = output[1];
  cfg.screen_width = 12;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl->output_stream_write_quoted_prompt &&
                  sl->set_quoted_prompt_prefix && sl->set_quoted_prompt_style,
              "quoted prompt receiver API missing");
  ASSERT_TRUE(
      sl_output_stream_write_quoted_prompt(sl, "x") == SL_ERROR_INVALID &&
          sl_set_quoted_prompt_prefix(sl, "") == SL_ERROR_INVALID &&
          sl_set_quoted_prompt_prefix(sl, "bad\n") == SL_ERROR_INVALID &&
          sl_set_quoted_prompt_prefix(sl, "\033[31m") == SL_ERROR_INVALID,
      "quoted prompt accepted invalid state or prefix");
  style.prefix.red = 1;
  style.prefix.green = 2;
  style.prefix.blue = 3;
  style.text.red = 4;
  style.text.green = 5;
  style.text.blue = 6;
  ASSERT_TRUE(
      sl_set_quoted_prompt_style(sl, &style) == SL_OK &&
          sl_set_quoted_prompt_prefix(sl, "? ") == SL_OK &&
          sl_output_stream_begin(sl) == SL_OK &&
          sl_output_stream_write(sl, "answer\n", 7) == SL_OK &&
          sl_output_stream_write_quoted_prompt(sl, "one two three four") ==
              SL_OK &&
          sl_set_quoted_prompt_prefix(sl, ">> ") == SL_OK &&
          sl_output_stream_write_quoted_prompt(sl, "a\nb\tc") == SL_OK &&
          sl_set_quoted_prompt_prefix(sl, "abcdefghijkl") == SL_OK &&
          sl_output_stream_write_quoted_prompt(sl, "x") == SL_ERROR_INVALID &&
          sl_set_quoted_prompt_prefix(sl, NULL) == SL_OK &&
          sl_set_quoted_prompt_style(sl, NULL) == SL_OK &&
          sl_output_stream_write_quoted_prompt(sl, "bad\033[31m") ==
              SL_ERROR_INVALID &&
          sl_output_stream_write_quoted_prompt(sl, "*md*") == SL_OK &&
          sl_set_screen_width(sl, 7) == SL_OK &&
          sl_output_stream_write_quoted_prompt(
              sl, "\xe7\x95\x8c\xe7\x95\x8c\xe7\x95\x8c") == SL_OK &&
          sl_set_screen_width(sl, 3) == SL_OK &&
          sl_output_stream_write_quoted_prompt(sl, "\xe7\x95\x8c") ==
              SL_ERROR_INVALID &&
          sl_output_stream_end(sl) == SL_OK,
      "quoted prompt output or validation failed");
  sl_destroy(sl);
  close(output[1]);
  amount = read(output[0], bytes, sizeof(bytes));
  close(output[0]);
  ASSERT_TRUE(amount == (ssize_t)(sizeof(expected) - 1) &&
                  memcmp(bytes, expected, sizeof(expected) - 1) == 0,
              "quoted prompt wrapping, literal text, or blank rows differ");
  PASS();
}

static void test_quoted_prompt_long_unbroken_word(void) {
  static const char expected[] = "\n\n> abcdefghij\n> klmnopqrst\n> uvwxy\n\n";
  sl_config_t cfg;
  sl_t *sl;
  int output[2];
  char bytes[128];
  ssize_t amount;

  TEST("quoted prompt fills rows when a single word exceeds the width");
  ASSERT_TRUE(pipe(output) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = output[1];
  cfg.screen_width = 12;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(
                      sl, "abcdefghijklmnopqrstuvwxy") == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "quoted prompt write failed");
  sl_destroy(sl);
  close(output[1]);
  amount = read(output[0], bytes, sizeof(bytes));
  close(output[0]);
  ASSERT_TRUE(amount == (ssize_t)(sizeof(expected) - 1) &&
                  memcmp(bytes, expected, sizeof(expected) - 1) == 0,
              "long word wrapped before the row was full");
  PASS();
}

static void test_quoted_prompt_chunk_boundary(void) {
  sl_config_t cfg;
  sl_t *sl;
  int output[2];
  char prompt[4102];
  char bytes[4200];
  size_t used;
  size_t i;
  ssize_t amount;

  TEST("quoted prompt preserves UTF-8 across bounded output chunks");
  ASSERT_TRUE(pipe(output) == 0, "pipe failed");
  prompt[0] = 'a';
  for (i = 0; i < 2050; i++) {
    prompt[1 + 2 * i] = '\xc3';
    prompt[2 + 2 * i] = '\xa9';
  }
  prompt[4101] = '\0';
  sl_config_init(&cfg);
  cfg.output_fd = output[1];
  cfg.screen_width = 5000;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, prompt) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "quoted prompt rejected a UTF-8 chunk boundary");
  sl_destroy(sl);
  close(output[1]);
  used = 0;
  while (used < sizeof(bytes) &&
         (amount = read(output[0], bytes + used, sizeof(bytes) - used)) > 0)
    used += (size_t)amount;
  if (used == sizeof(bytes))
    amount = -1;
  close(output[0]);
  ASSERT_TRUE(amount == 0 && used == 4107 && memcmp(bytes, "\n\n> ", 4) == 0 &&
                  memcmp(bytes + 4, prompt, 4101) == 0 &&
                  memcmp(bytes + 4105, "\n\n", 2) == 0,
              "quoted prompt changed bytes across an output chunk boundary");
  PASS();
}

#if SL_TEST_PTY
static void native_submit_idle(sl_t *sl, void *userdata) {
  int *submitted;
  submitted = (int *)userdata;
  if (*submitted)
    return;
  *submitted = 1;
  (void)sl_set_buffer(sl, "draft");
  (void)sl_submit(sl);
}
#endif

static void test_native_output_preserves_source_bytes(void) {
#if SL_TEST_PTY
  static const char source[] =
      "\033[38;2;1;2;3mabcdefghijklmnopqrstuvwxyz0123456789"
      "abcdefghijklmnopqrstuvwxyz0123456789\033[0m\t"
      "\303\245\346\227\245";
  struct winsize ws;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  int submitted;
  unsigned int i;
  char cluster[258];
  struct termios original;
  struct termios current;
  char *line;
  char output[8192];
  TEST("native output preserves producer bytes through terminal wrapping");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 16;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(tcgetattr(slave_fd, &original) == 0, "termios read failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  cfg.screen_width = 5;
  cfg.clear_prompt_on_exit = 1;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
              "native stream setup failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(sl_output_stream_write(sl, source, 3) == SL_OK &&
                  sl_output_stream_write(sl, source + 3, sizeof(source) - 4) ==
                      SL_OK,
              "native source write failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0 &&
                  strstr(output, source) != NULL &&
                  strstr(output, "\n") == NULL,
              "native wrapping inserted bytes or rewrote producer styles");
  cluster[0] = 'e';
  for (i = 1; i < sizeof(cluster) - 1; i += 2) {
    cluster[i] = (char)0xcc;
    cluster[i + 1] = (char)0x81;
  }
  cluster[sizeof(cluster) - 1] = '\0';
  ASSERT_TRUE(sl_output_stream_write(sl, cluster, sizeof(cluster) - 1) ==
                      SL_OK &&
                  read_live_pty_output(master_fd, output, sizeof(output)) > 0 &&
                  strstr(output, cluster) != NULL,
              "native output imposed the retired cell cluster limit");
  submitted = 0;
  ASSERT_TRUE(sl_set_idle_callback(sl, native_submit_idle, &submitted) == SL_OK,
              "native submit callback setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line && strcmp(line, "draft") == 0 &&
                  tcgetattr(slave_fd, &current) == 0 &&
                  !(current.c_lflag & ICANON) &&
                  current.c_oflag == original.c_oflag,
              "native session lost raw input or changed output processing");
  sl_free_string(sl, line);
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "native stream end failed");
  ASSERT_TRUE(tcgetattr(slave_fd, &current) == 0 &&
                  current.c_lflag == original.c_lflag &&
                  current.c_iflag == original.c_iflag &&
                  current.c_oflag == original.c_oflag,
              "native stream end did not restore terminal attributes");
  ASSERT_TRUE(
      read_live_pty_output(master_fd, output, sizeof(output)) > 0 &&
          strstr(output, "\033[r\033[0m") != NULL &&
          strstr(output, "\0337") == NULL && strstr(output, "\0338") == NULL &&
          strstr(output, "\n") != NULL && strstr(output, "\033[?25h") != NULL,
      "native teardown did not restore the terminal below output");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
#endif
}

static void test_native_tab_expansion(void) {
#if SL_TEST_PTY && defined(TABDLY) && defined(TAB3)
  int separate, mode;
  for (separate = 0; separate < 2; ++separate) {
    for (mode = 0; mode < 3; ++mode) {
      struct winsize ws;
      struct termios before_input, before_output, current;
      sl_config_t cfg;
      sl_t *sl;
      int input_master, input_slave, output_master, output_slave;
      int submitted;
      char *line;
      char output[4096];
      TEST("native tabs bypass kernel expansion and restore both TTYs");
      memset(&ws, 0, sizeof(ws));
      ws.ws_col = 40;
      ws.ws_row = 8;
      ASSERT_TRUE(openpty(&input_master, &input_slave, NULL, NULL, &ws) == 0,
                  "input openpty failed");
      output_master = input_master;
      output_slave = input_slave;
      if (separate)
        ASSERT_TRUE(openpty(&output_master, &output_slave, NULL, NULL, &ws) ==
                        0,
                    "output openpty failed");
      ASSERT_TRUE(tcgetattr(input_slave, &before_input) == 0,
                  "input attributes unavailable");
      before_input.c_oflag = (before_input.c_oflag & ~TABDLY) | TAB3;
      ASSERT_TRUE(tcsetattr(input_slave, TCSANOW, &before_input) == 0 &&
                      tcgetattr(output_slave, &before_output) == 0,
                  "input expansion setup failed");
      before_output.c_oflag = (before_output.c_oflag & ~TABDLY) | TAB3;
      if (mode == 0)
        before_output.c_oflag |= OPOST | ONLCR;
      else if (mode == 1) {
        before_output.c_oflag |= OPOST;
        before_output.c_oflag &= ~ONLCR;
      } else
        before_output.c_oflag &= ~OPOST;
      ASSERT_TRUE(tcsetattr(output_slave, TCSANOW, &before_output) == 0 &&
                      tcgetattr(input_slave, &before_input) == 0,
                  "output expansion setup failed");
      sl_config_init(&cfg);
      cfg.input_fd = input_slave;
      cfg.output_fd = output_slave;
      sl = sl_create_with_config(&cfg);
      if (sl && mode == 0) {
        submitted = 0;
        ASSERT_TRUE(sl_set_idle_callback(sl, native_submit_idle, &submitted) ==
                        SL_OK,
                    "readline callback setup failed");
        line = sl_readline(sl, "> ");
        ASSERT_TRUE(line && strcmp(line, "draft") == 0 &&
                        tcgetattr(input_slave, &current) == 0 &&
                        current.c_oflag == before_input.c_oflag &&
                        current.c_lflag == before_input.c_lflag &&
                        tcgetattr(output_slave, &current) == 0 &&
                        current.c_oflag == before_output.c_oflag &&
                        current.c_lflag == before_output.c_lflag,
                    "ordinary readline did not restore terminal flags");
        sl_free_string(sl, line);
      }
      ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
                  "native stream setup failed");
      ASSERT_TRUE(tcgetattr(input_slave, &current) == 0 &&
                      !(current.c_lflag & ICANON) &&
                      current.c_oflag == (before_input.c_oflag & ~TABDLY) &&
                      tcgetattr(output_slave, &current) == 0 &&
                      current.c_oflag == (before_output.c_oflag & ~TABDLY),
                  "kernel tab expansion remains active or LF flags changed");
      (void)read_live_pty_output(output_master, output, sizeof(output));
      ASSERT_TRUE(
          sl_output_stream_write(sl, "\tX\nY", 4) == SL_OK &&
              read_live_pty_output(output_master, output, sizeof(output)) > 0 &&
              strstr(output, mode == 0 ? "\tX\r\nY" : "\tX\nY") != NULL,
          "tab bytes or configured LF processing changed");
      if (mode != 2)
        ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK,
                    "native stream end failed");
      sl_destroy(sl);
      ASSERT_TRUE(tcgetattr(input_slave, &current) == 0 &&
                      current.c_oflag == before_input.c_oflag &&
                      current.c_lflag == before_input.c_lflag &&
                      tcgetattr(output_slave, &current) == 0 &&
                      current.c_oflag == before_output.c_oflag &&
                      current.c_lflag == before_output.c_lflag,
                  "teardown did not restore original terminal attributes");
      if (separate) {
        close(output_slave);
        close(output_master);
      }
      close(input_slave);
      close(input_master);
      PASS();
    }
  }
#endif
}

static void test_native_output_without_reported_size(void) {
#if SL_TEST_PTY
  struct winsize ws;
  struct vt_screen screen;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[8192];
  TEST("native output uses terminal defaults when PTY reports zero size");
  memset(&ws, 0, sizeof(ws));
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "ONE", 3) == SL_OK &&
                  sl_output_stream_write(sl, "\n", 1) == SL_OK &&
                  sl_output_stream_write(sl, "TWO", 3) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "native output with fallback geometry failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "fallback geometry output missing");
  vt_init(&screen, 24, 80);
  vt_apply(&screen, output);
  ASSERT_TRUE(strncmp(screen.cells[0], "ONE", 3) == 0 &&
                  strncmp(screen.cells[1], "TWO", 3) == 0 && screen.row == 2 &&
                  screen.col == 0,
              "fallback geometry lost its output row between producer spans");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
#endif
}

static void test_quoted_prompt_terminal_style(void) {
#if SL_TEST_PTY
  sl_config_t cfg;
  sl_quote_style_t style;
  struct winsize ws;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char bytes[8192];
  size_t used;
  ssize_t amount;

  TEST("quoted prompt uses theme colours and independent style override");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 12;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  style.prefix.red = 1;
  style.prefix.green = 2;
  style.prefix.blue = 3;
  style.text.red = 4;
  style.text.green = 5;
  style.text.blue = 6;
  ASSERT_TRUE(sl && sl_set_prompt_theme(sl, SL_PROMPT_THEME_GRUVBOX) == SL_OK &&
                  sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "\033[", 2) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "incomplete") ==
                      SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "0m", 2) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "theme") == SL_OK &&
                  sl_set_quoted_prompt_prefix(sl, ">> ") == SL_OK &&
                  sl_set_quoted_prompt_style(sl, &style) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "custom") == SL_OK &&
                  sl_set_quoted_prompt_style(sl, NULL) == SL_OK &&
                  sl_set_prompt_theme(sl, SL_PROMPT_THEME_DEFAULT) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "default") ==
                      SL_OK &&
                  sl_set_prompt_theme(sl, SL_PROMPT_THEME_PLAIN) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "plain") == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "terminal quoted prompt setup failed");
  sl_destroy(sl);
  close(slave_fd);
  used = 0;
  while (used < sizeof(bytes) - 1) {
    amount = read(master_fd, bytes + used, sizeof(bytes) - 1 - used);
    if (amount <= 0)
      break;
    used += (size_t)amount;
  }
  close(master_fd);
  bytes[used] = '\0';
  ASSERT_TRUE(strstr(bytes, "\033[2m\033[38;2;102;92;84m") != NULL &&
                  strstr(bytes, "\033[3m\033[38;2;250;189;47m") != NULL &&
                  strstr(bytes, "\033[2m\033[38;2;1;2;3m") != NULL &&
                  strstr(bytes, "\033[3m\033[38;2;4;5;6m") != NULL &&
                  strstr(bytes, "\033[2;90m") != NULL &&
                  strstr(bytes, "\033[3;96m") != NULL &&
                  strstr(bytes, "\033[3;97m") != NULL,
              "theme or custom quote colour and italic treatment missing");
  PASS();
#endif
}

static void test_quoted_prompt_resets_inherited_style(void) {
#if SL_TEST_PTY
  struct winsize ws;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[8192];

  TEST("quoted prompt prefix clears preceding reverse and background style");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_set_prompt_theme(sl, SL_PROMPT_THEME_DEFAULT) == SL_OK &&
                  sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "\033[7;41mresponse\n\n",
                                         strlen("\033[7;41mresponse\n\n")) ==
                      SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "quoted") == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "styled quote setup failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "quoted output missing");
  ASSERT_TRUE(contains_bytes(output, "\033[0m\033[2;90m> ") &&
                  contains_bytes(output, "\033[0m\033[3;96mquoted") &&
                  !contains_bytes(output, "\033[0;2;7;90;41m> "),
              "first quoted prefix inherited reverse video or background");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
#else
  TEST("quoted prompt prefix clears preceding reverse and background style");
  printf("SKIP\n");
  tests_passed++;
#endif
}

#if SL_TEST_PTY
static int quoted_prompt_has_one_empty_row_after_session(const char *answer) {
  struct winsize ws;
  struct vt_screen screen;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[8192];
  int answer_row;
  int quote_row;
  int row;

  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 12;
  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0)
    return 0;
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  if (!sl || sl_output_stream_begin(sl) != SL_OK ||
      sl_output_stream_write(sl, answer, strlen(answer)) != SL_OK ||
      sl_output_stream_end(sl) != SL_OK ||
      sl_output_stream_begin(sl) != SL_OK ||
      sl_output_stream_write_quoted_prompt(sl, "question") != SL_OK ||
      sl_output_stream_end(sl) != SL_OK) {
    sl_destroy(sl);
    close(slave_fd);
    close(master_fd);
    return 0;
  }
  sl_destroy(sl);
  close(slave_fd);
  if (read_live_pty_output(master_fd, output, sizeof(output)) == 0) {
    close(master_fd);
    return 0;
  }
  close(master_fd);
  vt_init(&screen, 12, 30);
  vt_apply(&screen, output);
  answer_row = -1;
  quote_row = -1;
  for (row = 0; row < screen.rows; row++) {
    if (strstr(screen.cells[row], "answer"))
      answer_row = row;
    if (strstr(screen.cells[row], "> question"))
      quote_row = row;
  }
  return answer_row >= 0 && quote_row == answer_row + 2 &&
         screen.cells[answer_row + 1][0] == ' ';
}
#endif

static void test_quoted_prompt_spacing_across_sessions(void) {
  TEST("quoted prompts keep one empty row across output sessions");
#if SL_TEST_PTY
  ASSERT_TRUE(quoted_prompt_has_one_empty_row_after_session("answer\n\n") &&
                  quoted_prompt_has_one_empty_row_after_session("answer"),
              "retained output inserted the wrong number of quote separators");
  PASS();
#else
  printf("SKIP\n");
  tests_passed++;
#endif
}

static void test_redirected_quoted_prompt_spacing_across_sessions(void) {
  static const char expected[] = "answer\n\n> question\n\n";
  sl_config_t cfg;
  sl_t *sl;
  int output[2];
  char bytes[64];
  ssize_t amount;

  TEST("redirected quoted prompts preserve spacing across sessions");
  ASSERT_TRUE(pipe(output) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = output[1];
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "answer\n\n", 8) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK &&
                  sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "question") ==
                      SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "redirected output sessions failed");
  sl_destroy(sl);
  close(output[1]);
  amount = read(output[0], bytes, sizeof(bytes));
  close(output[0]);
  ASSERT_TRUE(amount == (ssize_t)(sizeof(expected) - 1) &&
                  memcmp(bytes, expected, sizeof(expected) - 1) == 0,
              "redirected quote added an extra empty row");
  PASS();
}

static void test_redirected_quote_spacing_after_finite_output(void) {
  static const char expected[] = "first\n\nfinite\n\n> second\n\n";
  struct one_chunk_once finite;
  sl_config_t cfg;
  sl_t *sl;
  int output[2];
  char bytes[64];
  ssize_t amount;

  TEST("redirected finite output updates the next quoted prompt spacing");
  ASSERT_TRUE(pipe(output) == 0, "pipe failed");
  sl_config_init(&cfg);
  cfg.output_fd = output[1];
  sl = sl_create_with_config(&cfg);
  finite.text = "finite";
  finite.sent = 0;
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "first\n\n", 7) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK &&
                  sl_print_above(sl, one_chunk_once_stream, &finite) == SL_OK &&
                  sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "second") == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "redirected output sequence failed");
  sl_destroy(sl);
  close(output[1]);
  amount = read(output[0], bytes, sizeof(bytes));
  close(output[0]);
  ASSERT_TRUE(amount == (ssize_t)(sizeof(expected) - 1) &&
                  memcmp(bytes, expected, sizeof(expected) - 1) == 0,
              "quote spacing ignored intervening finite output");
  PASS();
}

static void test_quoted_prompt_large_whitespace_run(void) {
  const size_t length = 128u * 1024u;
  sl_config_t cfg;
  sl_t *sl;
  char *prompt;
  clock_t started;
  clock_t finished;
  int output_fd;

  TEST("quoted prompt scans a large whitespace run in bounded CPU time");
  prompt = (char *)malloc(length + 1);
  ASSERT_TRUE(prompt != NULL, "large prompt allocation failed");
  memset(prompt, ' ', length);
  prompt[length] = '\0';
  output_fd = open("/dev/null", O_WRONLY);
  ASSERT_TRUE(output_fd >= 0, "null output setup failed");
  sl_config_init(&cfg);
  cfg.output_fd = output_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
              "large quoted prompt setup failed");
  started = clock();
  ASSERT_TRUE(sl_output_stream_write_quoted_prompt(sl, prompt) == SL_OK,
              "large quoted prompt failed");
  finished = clock();
  ASSERT_TRUE(started != (clock_t)-1 && finished != (clock_t)-1 &&
                  (double)(finished - started) / CLOCKS_PER_SEC < 10.0,
              "quoted prompt rescanned whitespace suffixes");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK,
              "large quoted prompt did not end cleanly");
  sl_destroy(sl);
  close(output_fd);
  free(prompt);
  PASS();
}

static void test_quote_spacing_after_partial_invalid_write(void) {
#if SL_TEST_PTY
  struct winsize ws;
  struct vt_screen screen;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  int abc_row;
  int x_row;
  int quote_row;
  int row;
  char output[8192];
  TEST("quote keeps blank-row separation after rejected output bytes");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 12;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "\n\n", 2) == SL_OK &&
                  sl_output_stream_write(sl, "abc\0", 4) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\n\n", 2) == SL_OK &&
                  sl_output_stream_write(sl, "\033[2J", 4) ==
                      SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "X", 1) == SL_OK &&
                  sl_output_stream_write_quoted_prompt(sl, "hello") == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "partial write recovery failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  vt_init(&screen, 12, 30);
  vt_apply(&screen, output);
  abc_row = -1;
  x_row = -1;
  quote_row = -1;
  for (row = 0; row < screen.rows; row++) {
    if (strstr(screen.cells[row], "abc"))
      abc_row = row;
    if (strstr(screen.cells[row], "X"))
      x_row = row;
    if (strstr(screen.cells[row], "> hello"))
      quote_row = row;
  }
  if (abc_row < 0 || x_row < abc_row + 2 || quote_row < x_row + 2)
    vt_dump(&screen);
  ASSERT_TRUE(abc_row >= 0 && x_row >= abc_row + 2 && quote_row >= x_row + 2 &&
                  !strstr(screen.cells[x_row], "> hello"),
              "quoted prompt joined partially accepted output");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
#else
  TEST("quote keeps blank-row separation after rejected output bytes");
  PASS();
#endif
}

static ssize_t read_until_eof_with_timeout(int fd, char *buf, size_t cap) {
  ssize_t total;
  total = 0;
  while ((size_t)total < cap) {
    ssize_t n;
    n = read_some_with_timeout(fd, buf + total, cap - (size_t)total);
    if (n < 0)
      return n;
    if (n == 0)
      return total;
    total += n;
  }
  return total;
}

#if defined(__linux__)
struct idle_print_state {
  int printed;
};

struct idle_scroll_region_state {
  int calls;
};

struct idle_stream_failure_state {
  int fd;
  int attempted;
  int status;
};

struct idle_stream_end_state {
  int fd;
  int attempted;
};

struct idle_output_boundary_state {
  int fd;
  int attempted;
};

struct idle_count_print_state {
  int calls;
  int printed;
  const char *text;
};

struct idle_finish_state {
  int calls;
  const char *text;
  int cancel;
};

struct idle_ready_state {
  int fd;
  int ready;
};

struct idle_queue_limit_state {
  const char *expected_buffer;
  int max_entries;
  int status;
  int attempted;
};

struct text_stream_state {
  const char *chunks[4];
  int index;
  int calls;
};

static volatile sig_atomic_t ctrl_c_sigint_count = 0;
static int ctrl_c_signal_marker_fd = -1;

static void count_ctrl_c_sigint(int signum) {
  if (signum == SIGINT) {
    ctrl_c_sigint_count++;
    if (ctrl_c_signal_marker_fd >= 0)
      (void)write(ctrl_c_signal_marker_fd, "SIGNAL_MARKER", 13);
  }
}

struct enter_override_state {
  int calls;
};

static int insert_text_key(sl_t *sl, sl_key_t key, void *userdata,
                           sl_key_action_t *action) {
  const char *text;
  (void)key;
  text = (const char *)userdata;
  if (sl->insert(sl, text) != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int submit_action_key(sl_t *sl, sl_key_t key, void *userdata,
                             sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  *action = SL_KEY_ACTION_SUBMIT;
  return SL_OK;
}

static int reset_action_key(sl_t *sl, sl_key_t key, void *userdata,
                            sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  *action = SL_KEY_ACTION_RESET;
  return SL_OK;
}

static int submit_method_key(sl_t *sl, sl_key_t key, void *userdata,
                             sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (sl->submit(sl) != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int edit_buffer_key(sl_t *sl, sl_key_t key, void *userdata,
                           sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (sl->set_buffer(sl, "abcd") != SL_OK)
    return SL_ERROR;
  if (sl->set_cursor(sl, 2) != SL_OK)
    return SL_ERROR;
  if (sl->insert(sl, "XX") != SL_OK)
    return SL_ERROR;
  if (strcmp(sl->buffer(sl), "abXXcd") != 0)
    return SL_ERROR;
  if (sl->cursor(sl) != 4)
    return SL_ERROR;
  *action = SL_KEY_ACTION_SUBMIT;
  return SL_OK;
}

static int compound_edit_key(sl_t *sl, sl_key_t key, void *userdata,
                             sl_key_action_t *action) {
  int status = edit_buffer_key(sl, key, userdata, action);
  *action = SL_KEY_ACTION_HANDLED;
  return status;
}

static int pass_action_key(sl_t *sl, sl_key_t key, void *userdata,
                           sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  *action = SL_KEY_ACTION_PASS;
  return SL_OK;
}

static int cancel_action_key(sl_t *sl, sl_key_t key, void *userdata,
                             sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}

static int enter_override_key(sl_t *sl, sl_key_t key, void *userdata,
                              sl_key_action_t *action) {
  struct enter_override_state *state;
  (void)key;
  state = (struct enter_override_state *)userdata;
  state->calls++;
  if (state->calls == 1) {
    if (sl->insert(sl, "\n") != SL_OK)
      return SL_ERROR;
    *action = SL_KEY_ACTION_HANDLED;
  } else {
    *action = SL_KEY_ACTION_PASS;
  }
  return SL_OK;
}

static int next_text_chunk(sl_t *sl, void *userdata, const char **chunk,
                           size_t *len) {
  struct text_stream_state *state;
  const char *text;
  (void)sl;
  state = (struct text_stream_state *)userdata;
  if (!state || state->index >= 4) {
    *chunk = NULL;
    *len = 0;
    return SL_OK;
  }
  text = state->chunks[state->index];
  state->index++;
  state->calls++;
  if (!text) {
    *chunk = NULL;
    *len = 0;
    return SL_OK;
  }
  *chunk = text;
  *len = strlen(text);
  return SL_OK;
}

static void idle_print_once(sl_t *sl, void *userdata) {
  struct idle_print_state *state;
  struct text_stream_state stream;
  state = (struct idle_print_state *)userdata;
  if (!state || state->printed)
    return;
  state->printed = 1;
  stream.chunks[0] = "idle-";
  stream.chunks[1] = "output";
  stream.chunks[2] = "\n";
  stream.chunks[3] = NULL;
  stream.index = 0;
  stream.calls = 0;
  (void)sl->print_above(sl, next_text_chunk, &stream);
}

static void idle_end_live_stream_once(sl_t *sl, void *userdata) {
  struct idle_stream_end_state *state;
  char result;
  state = (struct idle_stream_end_state *)userdata;
  if (!state || state->attempted)
    return;
  state->attempted = 1;
  result = sl_output_stream_begin(sl) == SL_OK &&
                   sl_output_stream_write(sl, "notice\n", 7) == SL_OK &&
                   sl_output_stream_end(sl) == SL_OK
               ? 'R'
               : 'E';
  (void)write(state->fd, &result, 1);
}

static void idle_output_boundary_once(sl_t *sl, void *userdata) {
  struct idle_output_boundary_state *state;
  struct one_chunk_once middle;
  char result;
  state = (struct idle_output_boundary_state *)userdata;
  if (!state || state->attempted)
    return;
  state->attempted = 1;
  middle.text = "middle\n";
  middle.sent = 0;
  result =
      sl_output_stream_begin(sl) == SL_OK &&
              sl_output_stream_write(sl, "first\n", 6) == SL_OK &&
              sl_output_stream_end(sl) == SL_OK &&
              sl_print_above(sl, one_chunk_once_stream, &middle) == SL_OK &&
              sl_output_stream_begin(sl) == SL_OK &&
              sl_output_stream_write(sl, "last\n", 5) == SL_OK &&
              sl_output_stream_end(sl) == SL_OK
          ? 'R'
          : 'E';
  (void)write(state->fd, &result, 1);
}

static void idle_print_through_scroll_region(sl_t *sl, void *userdata) {
  struct idle_scroll_region_state *state;
  struct text_stream_state stream;
  const char *text;
  state = (struct idle_scroll_region_state *)userdata;
  if (!state)
    return;
  state->calls++;
  if (state->calls != 1 && state->calls != 4)
    return;
  text = state->calls == 1 ? "first-live\n" : "second-live\n";
  stream.chunks[0] = text;
  stream.chunks[1] = NULL;
  stream.chunks[2] = NULL;
  stream.chunks[3] = NULL;
  stream.index = 0;
  stream.calls = 0;
  (void)sl->print_above(sl, next_text_chunk, &stream);
}

static void idle_print_failure_once(sl_t *sl, void *userdata) {
  struct idle_stream_failure_state *state;
  char ready;
  state = (struct idle_stream_failure_state *)userdata;
  if (!state || state->attempted)
    return;
  state->attempted = 1;
  state->status = sl->print_above(sl, failing_stream, NULL);
  ready = state->status == SL_ERROR ? 'R' : 'E';
  (void)write(state->fd, &ready, 1);
}

static void idle_finish_after_two_ticks(sl_t *sl, void *userdata) {
  struct idle_finish_state *state;
  state = (struct idle_finish_state *)userdata;
  if (!state)
    return;
  state->calls++;
  if (state->calls < 2)
    return;
  if (state->cancel) {
    (void)sl->cancel(sl);
    return;
  }
  if (state->text)
    (void)sl->insert(sl, state->text);
  (void)sl->submit(sl);
}

static void idle_release_after_steer_queued(sl_t *sl, void *userdata) {
  (void)userdata;
  if (sl_prompt_queue_count(sl) == 2)
    (void)sl_set_status_busy(sl, 0);
}

static int idle_quiet_watch_callback(sl_t *sl, const sl_watch_event_t *event,
                                     void *userdata) {
  (void)sl;
  (void)event;
  (void)userdata;
  return SL_OK;
}

static void idle_signal_ready_once(sl_t *sl, void *userdata) {
  struct idle_ready_state *state;
  (void)sl;
  state = (struct idle_ready_state *)userdata;
  if (!state || state->ready)
    return;
  state->ready = 1;
  (void)write(state->fd, "R", 1);
}

static void idle_reduce_prompt_queue_limit(sl_t *sl, void *userdata) {
  struct idle_queue_limit_state *state;
  const char *buffer;
  state = (struct idle_queue_limit_state *)userdata;
  if (!state || state->attempted)
    return;
  buffer = sl->buffer(sl);
  if (!buffer || strcmp(buffer, state->expected_buffer) != 0)
    return;
  state->attempted = 1;
  state->status = sl->set_prompt_queue(sl, 1, state->max_entries, 1);
  (void)sl->submit(sl);
}

static void append_terminal_bytes(char *terminal, size_t *terminal_len,
                                  size_t terminal_cap, const char *buf,
                                  ssize_t n) {
  if (n <= 0 || *terminal_len >= terminal_cap - 1)
    return;
  if ((size_t)n > terminal_cap - 1 - *terminal_len)
    n = (ssize_t)(terminal_cap - 1 - *terminal_len);
  memcpy(terminal + *terminal_len, buf, (size_t)n);
  *terminal_len += (size_t)n;
  terminal[*terminal_len] = '\0';
}

static int run_pty_readline_case_with_prompt_limit(
    const char *input, const char *prompt, const char *expected_prompt,
    int width, int height, size_t line_max_len, const char *print_above,
    char *terminal, size_t terminal_cap, char *result, size_t result_cap,
    int *exit_status) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  size_t terminal_len;
  ssize_t n;
  int status;
  struct winsize ws;

  memset(&ws, 0, sizeof(ws));
  if (width > 0)
    ws.ws_col = (unsigned short)width;
  if (height > 0)
    ws.ws_row = (unsigned short)height;
  if (openpty(&master_fd, &slave_fd, NULL, NULL,
              (width > 0 || height > 0) ? &ws : NULL) != 0)
    return -1;
  if (pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = width;
    if (line_max_len > 0)
      cfg.line_max_len = line_max_len;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    if (print_above) {
      struct text_stream_state stream;
      stream.chunks[0] = print_above;
      stream.chunks[1] = NULL;
      stream.chunks[2] = NULL;
      stream.chunks[3] = NULL;
      stream.index = 0;
      stream.calls = 0;
      (void)sl->print_above(sl, next_text_chunk, &stream);
    }
    line = sl->readline(sl, prompt);
    if (!line) {
      (void)write(result_pipe[1], "<NULL>", 6);
    } else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }

  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, expected_prompt)) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n <= 0)
      return -1;
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    if (terminal_len >= terminal_cap - 1)
      return -1;
  }
  if (write(master_fd, input, strlen(input)) != (ssize_t)strlen(input))
    return -1;
  n = read_some_with_timeout(result_pipe[0], result, result_cap - 1);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
  } while (n > 0 && terminal_len < terminal_cap - 1);
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int run_pty_readline_case_with_limit(const char *input, int width,
                                            int height, size_t line_max_len,
                                            const char *print_above,
                                            char *terminal, size_t terminal_cap,
                                            char *result, size_t result_cap,
                                            int *exit_status) {
  return run_pty_readline_case_with_prompt_limit(
      input, "p> ", "p> ", width, height, line_max_len, print_above, terminal,
      terminal_cap, result, result_cap, exit_status);
}

static int run_pty_readline_default_prompt_case(const char *input,
                                                char *terminal,
                                                size_t terminal_cap,
                                                char *result, size_t result_cap,
                                                int *exit_status) {
  return run_pty_readline_case_with_prompt_limit(
      input, NULL, "> ", 20, 0, 0, NULL, terminal, terminal_cap, result,
      result_cap, exit_status);
}

static int run_pty_readline_case(const char *input, int width, int height,
                                 const char *print_above, char *terminal,
                                 size_t terminal_cap, char *result,
                                 size_t result_cap, int *exit_status) {
  return run_pty_readline_case_with_limit(input, width, height, 0, print_above,
                                          terminal, terminal_cap, result,
                                          result_cap, exit_status);
}

static int run_pty_readline_completion_case(int prompt_queue, char *terminal,
                                            size_t terminal_cap, char *result,
                                            size_t result_cap,
                                            int *exit_status) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  size_t terminal_len;
  ssize_t n;
  int status;

  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 5;
  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0 ||
      pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = prompt_queue;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(3);
    if (write(slave_fd, "after\n", 6) != 6)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }

  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n <= 0)
      return -1;
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    if (terminal_len >= terminal_cap - 1)
      return -1;
  }
  if (write(master_fd, "hello\r", 6) != 6)
    return -1;
  n = read_some_with_timeout(result_pipe[0], result, result_cap - 1);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
  } while (n > 0 && terminal_len < terminal_cap - 1);
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int run_pty_history_case_with_options(
    const char *input, const char **history, int history_len,
    const char *history_file, int width, int height, int prompt_queue,
    char *terminal, size_t terminal_cap, char *result, size_t result_cap,
    int *exit_status) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  size_t terminal_len;
  ssize_t n;
  int status;
  int i;

  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0)
    return -1;
  if (pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = width;
    cfg.prompt_queue = prompt_queue;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    for (i = 0; i < history_len; i++)
      (void)sl->history_add(sl, history[i]);
    if (history_file && sl->history_load(sl, history_file) != SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line) {
      (void)write(result_pipe[1], "<NULL>", 6);
    } else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }

  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n <= 0)
      return -1;
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    if (terminal_len >= terminal_cap - 1)
      return -1;
  }
  if (write(master_fd, input, strlen(input)) != (ssize_t)strlen(input))
    return -1;
  n = read_some_with_timeout(result_pipe[0], result, result_cap - 1);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
  } while (n > 0 && terminal_len < terminal_cap - 1);
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int run_pty_history_case_with_file(const char *input,
                                          const char **history, int history_len,
                                          const char *history_file,
                                          char *terminal, size_t terminal_cap,
                                          char *result, size_t result_cap,
                                          int *exit_status) {
  return run_pty_history_case_with_options(
      input, history, history_len, history_file, 20, 0, 0, terminal,
      terminal_cap, result, result_cap, exit_status);
}

static int run_pty_history_case(const char *input, const char **history,
                                int history_len, char *terminal,
                                size_t terminal_cap, char *result,
                                size_t result_cap, int *exit_status) {
  return run_pty_history_case_with_file(input, history, history_len, NULL,
                                        terminal, terminal_cap, result,
                                        result_cap, exit_status);
}

static int run_pty_key_binding_case(const char *input, sl_key_t key,
                                    sl_key_callback_t callback, void *userdata,
                                    char *terminal, size_t terminal_cap,
                                    char *result, size_t result_cap,
                                    int *exit_status) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  size_t terminal_len;
  ssize_t n;
  int status;

  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0)
    return -1;
  if (pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 24;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    if (sl->bind_key(sl, key, callback, userdata) != SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line) {
      (void)write(result_pipe[1], "<NULL>", 6);
    } else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }

  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n <= 0)
      return -1;
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    if (terminal_len >= terminal_cap - 1)
      return -1;
  }
  if (write(master_fd, input, strlen(input)) != (ssize_t)strlen(input))
    return -1;
  n = read_some_with_timeout(result_pipe[0], result, result_cap - 1);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
  } while (n > 0 && terminal_len < terminal_cap - 1);
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int run_pty_prompt_queue_case_with_width(
    const char *input, sl_prompt_theme_t theme, int native_chat,
    int reduced_max_entries, int expected_prompts, int width,
    const char *prompt, char *terminal, size_t terminal_cap, char *result,
    size_t result_cap, int *exit_status) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  size_t terminal_len;
  ssize_t n;
  int status;

  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)width;
  ws.ws_row = 8;
  if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) != 0 ||
      pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char header[32];
    char output[512];
    struct idle_queue_limit_state limit_state;
    size_t header_len;
    size_t used;
    int i;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    cfg.prompt_queue_max_entries = 8;
    cfg.prompt_queue_preview_entries = 1;
    cfg.prompt_theme = theme;
    sl = sl_create_with_config(&cfg);
    if (!sl || (native_chat && sl_output_stream_begin(sl) != SL_OK))
      _exit(2);
    memset(&limit_state, 0, sizeof(limit_state));
    limit_state.expected_buffer = "third";
    limit_state.max_entries = reduced_max_entries;
    limit_state.status = SL_ERROR;
    if (reduced_max_entries > 0 &&
        sl->set_idle_callback(sl, idle_reduce_prompt_queue_limit,
                              &limit_state) != SL_OK)
      _exit(5);
    used = 0;
    output[0] = '\0';
    for (i = 0; i < expected_prompts; i++) {
      char *line;
      sl_prompt_source_t source;
      int written;
      source = SL_PROMPT_SOURCE_NONE;
      line = sl_next_prompt(sl, prompt, &source);
      if (!line)
        _exit(3);
      written = snprintf(output + used, sizeof(output) - used, "%s%d:%s",
                         i == 0 ? "" : "|", (int)source, line);
      sl_free_string(sl, line);
      if (written < 0 || (size_t)written >= sizeof(output) - used)
        _exit(4);
      used += (size_t)written;
    }
    if (reduced_max_entries > 0) {
      int written;
      written = snprintf(header, sizeof(header), "%d;", limit_state.status);
      if (written < 0 || written >= (int)sizeof(header))
        _exit(6);
      header_len = (size_t)written;
      if (header_len > sizeof(output) - used)
        _exit(7);
      memmove(output + header_len, output, used);
      memcpy(output, header, header_len);
      used += header_len;
    }
    (void)write(result_pipe[1], output, used);
    sl_destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, prompt)) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n <= 0)
      return -1;
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    if (terminal_len >= terminal_cap - 1)
      return -1;
  }
  if (write(master_fd, input, strlen(input)) != (ssize_t)strlen(input))
    return -1;
  n = read_some_with_timeout(result_pipe[0], result, result_cap - 1);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
  } while (n > 0 && terminal_len < terminal_cap - 1);
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int run_pty_prompt_queue_case(const char *input, sl_prompt_theme_t theme,
                                     int native_chat, int reduced_max_entries,
                                     int expected_prompts, char *terminal,
                                     size_t terminal_cap, char *result,
                                     size_t result_cap, int *exit_status) {
  return run_pty_prompt_queue_case_with_width(
      input, theme, native_chat, reduced_max_entries, expected_prompts, 40,
      "chat> ", terminal, terminal_cap, result, result_cap, exit_status);
}

static int run_pty_themed_readline_case(sl_prompt_theme_t theme, char *terminal,
                                        size_t terminal_cap, char *result,
                                        size_t result_cap, int *exit_status) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  size_t terminal_len;
  ssize_t n;
  int status;

  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0 ||
      pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 24;
    cfg.prompt_theme = theme;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl_readline(sl, "normal> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "normal> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n <= 0)
      return -1;
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    if (terminal_len >= terminal_cap - 1)
      return -1;
  }
  if (write(master_fd, "ok\r", 3) != 3)
    return -1;
  n = read_some_with_timeout_ms(result_pipe[0], result, result_cap - 1, 10000);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               terminal_cap - 1 - terminal_len);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
  } while (n > 0 && terminal_len < terminal_cap - 1);
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int set_status_idle_marker_key(sl_t *sl, sl_key_t key, void *userdata,
                                      sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (!action || sl_set_status_idle_marker(sl, '\0') != SL_OK ||
      sl_set_status_spinner(sl, 0) != SL_OK ||
      sl_set_status_busy(sl, 0) != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int set_status_dash_marker_key(sl_t *sl, sl_key_t key, void *userdata,
                                      sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (!action || sl_set_status_idle_marker(sl, '-') != SL_OK ||
      sl_set_status_spinner(sl, 0) != SL_OK ||
      sl_set_status_busy(sl, 0) != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int set_status_busy_spinner_key(sl_t *sl, sl_key_t key, void *userdata,
                                       sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (!action || sl_set_status_spinner(sl, 1) != SL_OK ||
      sl_set_status_busy(sl, 1) != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int set_status_message_style_key(sl_t *sl, sl_key_t key, void *userdata,
                                        sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (!action || sl_set_status_message_prefix(sl, "? ") != SL_OK ||
      sl_set_status_message_colors(sl, SL_THEME_COLOR_ELEMENT_2,
                                   SL_THEME_COLOR_MUTED) != SL_OK ||
      sl_set_status_message(sl, "Changed") != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int hide_status_message_prefix_key(sl_t *sl, sl_key_t key,
                                          void *userdata,
                                          sl_key_action_t *action) {
  (void)key;
  (void)userdata;
  if (!action || sl_set_status_message_prefix(sl, "") != SL_OK ||
      sl_set_status_message(sl, "Bare") != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int wait_for_status_output(int fd, char *terminal, size_t capacity,
                                  size_t *length, size_t mark,
                                  const char *marker, const char *stage) {
  struct timespec deadline;
  struct timespec now;
  ssize_t amount;
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    return -1;
  deadline.tv_sec += 10;
  while (!contains_bytes(terminal + mark, marker)) {
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
        now.tv_sec > deadline.tv_sec ||
        (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
      fprintf(stderr, "status PTY %s marker timed out after %lu bytes\n", stage,
              (unsigned long)*length);
      return -1;
    }
    if (*length >= capacity - 1)
      return -1;
    amount = read_some_with_timeout_ms(fd, terminal + *length,
                                       capacity - 1 - *length, 100);
    if (amount < 0) {
      fprintf(stderr, "status PTY %s read failed: errno=%d\n", stage, errno);
      return -1;
    }
    if (amount > 0) {
      *length += (size_t)amount;
      terminal[*length] = '\0';
    }
  }
  return 0;
}

static int run_pty_statusline_case(sl_prompt_theme_t theme, char *terminal,
                                   size_t terminal_cap, char *result,
                                   size_t result_cap, int *exit_status) {
  static const char *const elements[] = {
      "e0",  "e1",  "e2",  "e3",  "e4",  "e5",  "e6",  "e7",  "e8",
      "e9",  "e10", "e11", "e12", "e13", "e14", "e15", "e16", "e17",
      "e18", "e19", "e20", "e21", "e22", "e23", "e24", "e25", "e26",
      "e27", "e28", "e29", "e30", "e31", "e32"};
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  size_t terminal_len;
  size_t output_mark;
  ssize_t n;
  int status;
  const char *idle_style;
  const char *busy_style;
  const char *element_style;
  const char *dash_style;
  const char *changed_style;
  const char *bare_style;

  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0 ||
      pipe(result_pipe) != 0)
    return -1;
  pid = fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 24;
    cfg.prompt_theme = theme;
    cfg.statusline = 1;
    cfg.statusline_start_element = 15;
    cfg.status_spinner = 1;
    cfg.status_busy = 0;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_set_status_message(
            sl, "Hello world, this is a long status line, that continues on "
                "multiple lines.") != SL_OK ||
        sl_set_status_elements(sl, elements,
                               sizeof(elements) / sizeof(elements[0])) != SL_OK)
      _exit(2);
    if (sl_bind_key(sl, SL_KEY_ALT_M, set_status_idle_marker_key, NULL) !=
        SL_OK)
      _exit(4);
    if (sl_bind_key(sl, (sl_key_t)(SL_KEY_ALT_BASE + 'n'),
                    set_status_dash_marker_key, NULL) != SL_OK)
      _exit(5);
    if (sl_bind_key(sl, (sl_key_t)(SL_KEY_ALT_BASE + 'p'),
                    set_status_busy_spinner_key, NULL) != SL_OK)
      _exit(6);
    if (sl_bind_key(sl, (sl_key_t)(SL_KEY_ALT_BASE + 'z'),
                    set_status_message_style_key, NULL) != SL_OK)
      _exit(7);
    if (sl_bind_key(sl, (sl_key_t)(SL_KEY_ALT_BASE + 'y'),
                    hide_status_message_prefix_key, NULL) != SL_OK)
      _exit(8);
    line = sl_readline(sl, "status> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  idle_style = theme == SL_PROMPT_THEME_DEFAULT ? "\033[32m+ "
                                                : "\033[38;2;57;255;20m+ ";
  busy_style = theme == SL_PROMPT_THEME_DEFAULT ? "\033[31m- "
                                                : "\033[38;2;255;51;51m- ";
  element_style = theme == SL_PROMPT_THEME_DEFAULT
                      ? "\r  \033[97me0"
                      : "\r  \033[38;2;172;164;184me0";
  dash_style = theme == SL_PROMPT_THEME_DEFAULT ? "\033[32m- "
                                                : "\033[38;2;57;255;20m- ";
  changed_style = theme == SL_PROMPT_THEME_DEFAULT
                      ? "\033[35m? \033[0m\033[3;2;90mChanged"
                      : "\033[38;2;185;103;255m? "
                        "\033[0m\033[3;38;2;72;76;105mChanged";
  bare_style = theme == SL_PROMPT_THEME_DEFAULT ? "\033[3;2;90mBare"
                                                : "\033[3;38;2;72;76;105mBare";
  terminal_len = 0;
  terminal[0] = '\0';
  if (wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             0, "status> ", "prompt") != 0 ||
      wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             0, idle_style, "initial idle") != 0)
    return -1;
  output_mark = terminal_len;
  if (write(master_fd, "\033p", 2) != 2 ||
      wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             output_mark, busy_style, "busy spinner") != 0)
    return -1;
  output_mark = terminal_len;
  if (write(master_fd, "\033m", 2) != 2 ||
      wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             output_mark, element_style,
                             "blank idle marker") != 0)
    return -1;
  output_mark = terminal_len;
  if (write(master_fd, "\033n", 2) != 2 ||
      wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             output_mark, dash_style, "idle dash") != 0)
    return -1;
  output_mark = terminal_len;
  if (write(master_fd, "\033z", 2) != 2 ||
      wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             output_mark, changed_style,
                             "changed message") != 0)
    return -1;
  output_mark = terminal_len;
  if (write(master_fd, "\033y", 2) != 2 ||
      wait_for_status_output(master_fd, terminal, terminal_cap, &terminal_len,
                             output_mark, bare_style, "bare message") != 0)
    return -1;
  if (write(master_fd, "ok\r", 3) != 3)
    return -1;
  n = read_some_with_timeout(result_pipe[0], result, result_cap - 1);
  if (n <= 0)
    return -1;
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  if (waitpid(pid, &status, 0) != pid)
    return -1;
  *exit_status = status;
  return 0;
}

static int termios_same_observable(const struct termios *a,
                                   const struct termios *b) {
  static const int controls[] = {
      VINTR, VQUIT, VERASE, VKILL,    VEOF,     VTIME,   VMIN,   VSWTC, VSTART,
      VSTOP, VSUSP, VEOL,   VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2};
  size_t i;
  if (a->c_iflag != b->c_iflag || a->c_oflag != b->c_oflag ||
      a->c_cflag != b->c_cflag || a->c_lflag != b->c_lflag)
    return 0;
  /* Compare named kernel controls, not unused libc slots. Musl tcgetattr
   * leaves reserved c_cc entries uninitialized beyond the kernel layout. */
  for (i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
    if (a->c_cc[controls[i]] != b->c_cc[controls[i]])
      return 0;
  }
  return 1;
}

static void test_pty_enter_and_ctrl_j(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("pty readline is non-destructive and Ctrl-J inserts newline");
  ASSERT_TRUE(run_pty_readline_case("ab\ncd\r", 20, 0, NULL, terminal,
                                    sizeof(terminal), result, sizeof(result),
                                    &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ab\ncd") == 0, "result mismatch");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[2J"), "clear-screen emitted");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[?25l"), "cursor-hide emitted");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[H"), "home-position emitted");
  PASS();
}

static void test_pty_readline_uses_default_prompt(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("pty readline uses default prompt");
  ASSERT_TRUE(run_pty_readline_default_prompt_case(
                  "ok\r", terminal, sizeof(terminal), result, sizeof(result),
                  &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0, "result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "> "), "default prompt missing");
  ASSERT_TRUE(!contains_bytes(terminal, "p> "), "test prompt was rendered");
  PASS();
}

static void test_pty_readline_stops_at_line_max(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("pty readline stops at configured line max");
  ASSERT_TRUE(run_pty_readline_case_with_limit(
                  "abcdef\r", 20, 0, 4, NULL, terminal, sizeof(terminal),
                  result, sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "abcd") == 0, "line max result mismatch");
  PASS();
}

static void test_normal_prompt_proceeds_after_output(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  char terminal[4096];
  char result[256];
  size_t terminal_len;
  ssize_t n;
  int status;

  TEST("normal prompt proceeds after output");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *first;
    char *second;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 20;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    first = sl->readline(sl, "p> ");
    if (!first)
      _exit(3);
    (void)write(slave_fd, "out:", 4);
    (void)write(slave_fd, first, strlen(first));
    (void)write(slave_fd, "\r\n", 2);
    second = sl->readline(sl, "p> ");
    if (!second)
      _exit(4);
    (void)write(result_pipe[1], first, strlen(first));
    (void)write(result_pipe[1], "|", 1);
    (void)write(result_pipe[1], second, strlen(second));
    sl->free_string(sl, first);
    sl->free_string(sl, second);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "first prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "one\r", 4) == 4, "first submit failed");
  while (!contains_after_bytes(terminal, "out:one", "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "second prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "two\r", 4) == 4, "second submit failed");
  n = read_until_eof_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  close(master_fd);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "one|two") == 0, "sequential result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "out:one"),
              "output between prompts missing");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[1;"),
              "normal prompt used scroll region");
  PASS();
}

/* These two interaction cases always close their PTY and reap their child,
 * including failed assertions. A stranded child retains CTest's capture pipe.
 */
struct prompt_output_child {
  int master, slave, result[2], gate[2], ready[2];
  pid_t pid;
};

static int prompt_output_open(struct prompt_output_child *child, int columns) {
  struct winsize ws;
  child->master = child->slave = -1;
  child->result[0] = child->result[1] = -1;
  child->gate[0] = child->gate[1] = -1;
  child->ready[0] = child->ready[1] = -1;
  child->pid = -1;
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = (unsigned short)columns;
  ws.ws_row = 8;
  return openpty(&child->master, &child->slave, NULL, NULL, &ws) ||
         pipe(child->result) || pipe(child->gate) || pipe(child->ready);
}

static void prompt_output_close_fd(int *fd) {
  if (*fd >= 0)
    close(*fd);
  *fd = -1;
}

static void prompt_output_parent(struct prompt_output_child *child) {
  prompt_output_close_fd(&child->slave);
  prompt_output_close_fd(&child->result[1]);
  prompt_output_close_fd(&child->gate[0]);
  prompt_output_close_fd(&child->ready[1]);
}

static int prompt_output_cleanup(struct prompt_output_child *child) {
  int status, failed = 0;
  pid_t reaped;
  prompt_output_close_fd(&child->master);
  prompt_output_close_fd(&child->slave);
  prompt_output_close_fd(&child->result[0]);
  prompt_output_close_fd(&child->result[1]);
  prompt_output_close_fd(&child->gate[0]);
  prompt_output_close_fd(&child->gate[1]);
  prompt_output_close_fd(&child->ready[0]);
  prompt_output_close_fd(&child->ready[1]);
  if (child->pid > 0) {
    (void)kill(child->pid, SIGKILL);
    do {
      reaped = waitpid(child->pid, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    failed = reaped != child->pid;
    child->pid = -1;
  }
  return failed;
}

static int prompt_output_before_deadline(const struct timespec *deadline) {
  struct timespec current;
  return clock_gettime(CLOCK_MONOTONIC, &current) == 0 &&
         (current.tv_sec < deadline->tv_sec ||
          (current.tv_sec == deadline->tv_sec &&
           current.tv_nsec < deadline->tv_nsec));
}

#define OUTPUT_REQUIRE(condition, message)                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      failure = message;                                                       \
      goto output_cleanup;                                                     \
    }                                                                          \
  } while (0)

/* mode 1 gates the child across a quiet read; mode 2 exits before completion.
 */
static const char *run_wrapped_output_prompt(int mode) {
  struct prompt_output_child child;
  struct timespec deadline;
  const char *failure = NULL;
  char terminal[16384];
  char buf[512];
  struct vt_screen screen;
  const char *input;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  OUTPUT_REQUIRE(prompt_output_open(&child, 80) == 0, "pty or pipe failed");
  child.pid = fork();
  OUTPUT_REQUIRE(child.pid >= 0, "fork failed");
  if (child.pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(child.master);
    close(child.result[0]);
    close(child.gate[1]);
    close(child.ready[0]);
    if (mode == 2)
      _exit(0);
    if (mode == 1 && read(child.gate[0], buf, 1) != 1)
      _exit(5);
    sl_config_init(&cfg);
    cfg.input_fd = child.slave;
    cfg.output_fd = child.slave;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "softline> ");
    if (!line)
      _exit(3);
    (void)write(child.slave, "submitted: ", 11);
    (void)write(child.slave, line, strlen(line));
    (void)write(child.slave, "\r\n", 2);
    sl->free_string(sl, line);
    line = sl->readline(sl, "softline> ");
    if (!line)
      _exit(4);
    (void)write(child.result[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(child.slave);
    close(child.result[1]);
    _exit(0);
  }
  prompt_output_parent(&child);
  terminal_len = 0;
  terminal[0] = '\0';
  OUTPUT_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0,
                 "clock failed");
  deadline.tv_sec += 5;
  while (!contains_bytes(terminal, "softline> ")) {
    OUTPUT_REQUIRE(prompt_output_before_deadline(&deadline),
                   "prompt was not rendered");
    n = read_some_with_timeout_ms(child.master, buf, sizeof(buf), 20);
    if (mode == 1 && child.gate[1] >= 0 && n == 0) {
      OUTPUT_REQUIRE(write(child.gate[1], "x", 1) == 1, "gate release failed");
      prompt_output_close_fd(&child.gate[1]);
    }
    OUTPUT_REQUIRE(n >= 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  input = "hello world jspdi jsdip jfjpisd rjfsdpi fpisdmjfpisi "
          "jpdstjfjpisjdfpijj\r";
  OUTPUT_REQUIRE(write(child.master, input, strlen(input)) ==
                     (ssize_t)strlen(input),
                 "write input failed");
  tries = 0;
  while (tries < 300) {
    vt_init(&screen, 8, 80);
    vt_apply(&screen, terminal);
    if (contains_after_bytes(terminal, "submitted:", "softline> ") &&
        vt_contains(&screen, "submitted: hello world jspdi jsdip") &&
        vt_contains(&screen, "softline>"))
      break;
    n = read_some_with_timeout_ms(child.master, buf, sizeof(buf), 20);
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  vt_init(&screen, 8, 80);
  vt_apply(&screen, terminal);
  if (!vt_contains(&screen, "submitted: hello world jspdi jsdip") ||
      !vt_contains(&screen, "softline>"))
    vt_dump(&screen);
  OUTPUT_REQUIRE(vt_contains(&screen, "submitted: hello world jspdi jsdip"),
                 "submitted output missing");
  OUTPUT_REQUIRE(vt_contains(&screen, "softline>"),
                 "next prompt missing after wrapped submit");
  OUTPUT_REQUIRE(write(child.master, "ok\r", 3) == 3, "second submit failed");
  n = read_until_eof_with_timeout(child.result[0], buf, sizeof(buf));
  OUTPUT_REQUIRE(n == 2 && memcmp(buf, "ok", 2) == 0, "read result failed");
  OUTPUT_REQUIRE(waitpid(child.pid, &status, 0) == child.pid, "waitpid failed");
  child.pid = -1;
  OUTPUT_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "child editor failed");
output_cleanup:
  if (prompt_output_cleanup(&child))
    failure = "failed to reap PTY child";
  return failure;
}

static void test_normal_wrapped_prompt_proceeds_after_output(void) {
  const char *failure;
  TEST("normal wrapped prompt proceeds after output");
  failure = run_wrapped_output_prompt(0);
  ASSERT_TRUE(!failure, failure);
  PASS();
  TEST("wrapped prompt waits through a gated startup gap");
  failure = run_wrapped_output_prompt(1);
  ASSERT_TRUE(!failure, failure);
  PASS();
  TEST("missing wrapped prompt fails and reaps its PTY child");
  failure = run_wrapped_output_prompt(2);
  ASSERT_TRUE(failure && strcmp(failure, "prompt was not rendered") == 0,
              "missing output or child cleanup failure was hidden");
  PASS();
}

static void test_pty_history_navigation_restores_draft(void) {
  const char *history[2];
  char terminal[4096];
  char result[256];
  int status;

  history[0] = "first";
  history[1] = "second";
  TEST("history up recalls newest entry");
  ASSERT_TRUE(run_pty_history_case("\033[A\r", history, 2, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "history case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "second") == 0, "newest history mismatch");
  PASS();

  TEST("history down restores edited draft");
  ASSERT_TRUE(run_pty_history_case("draft\033[A\033[B\r", history, 2, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "history draft case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "draft") == 0, "draft restore mismatch");
  PASS();

  TEST("Ctrl-P and Ctrl-N navigate history entries");
  ASSERT_TRUE(run_pty_history_case("\020\r", history, 2, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "Ctrl-P history case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "second") == 0, "Ctrl-P history mismatch");
  ASSERT_TRUE(run_pty_history_case("draft\020\016\r", history, 2, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "Ctrl-N history case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "draft") == 0, "Ctrl-N draft restore mismatch");
  PASS();
}

static void test_pty_readline_does_not_auto_add_history(void) {
  char file[] = "/tmp/softline-history-no-auto-XXXXXX";
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  int fd;
  int status;
  char terminal[1024];
  char result[16];
  size_t terminal_len;
  ssize_t n;
  struct stat st;

  TEST("TTY readline does not auto-add submitted history");
  fd = mkstemp(file);
  ASSERT_TRUE(fd >= 0, "mkstemp failed");
  close(fd);
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(3);
    sl->free_string(sl, line);
    if (sl->history_save(sl, file) != SL_OK)
      _exit(4);
    (void)write(result_pipe[1], "S", 1);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
    ASSERT_TRUE(terminal_len < sizeof(terminal) - 1, "terminal buffer full");
  }
  ASSERT_TRUE(write(master_fd, "secret\r", 7) == 7, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result));
  ASSERT_TRUE(n == 1, "child did not save history");
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(stat(file, &st) == 0, "stat history file failed");
  unlink(file);
  ASSERT_TRUE(st.st_size == 0, "readline submission was auto-added to history");
  PASS();
}

static void test_ctrl_r_searches_memory_history(void) {
  const char *history[3];
  char terminal[8192];
  char result[256];
  int status;

  history[0] = "alpha deploy";
  history[1] = "beta build";
  history[2] = "gamma deploy";
  TEST("Ctrl-R searches in-memory history newest first");
  ASSERT_TRUE(run_pty_history_case("\022deploy\r edited\r", history, 3,
                                   terminal, sizeof(terminal), result,
                                   sizeof(result), &status) == 0,
              "history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "gamma deploy edited") == 0,
              "history match was submitted before it could be edited");
  ASSERT_TRUE(contains_bytes(terminal, "(r-search)`deploy': "),
              "reverse search prompt missing");
  ASSERT_TRUE(contains_bytes(terminal, "gamma deploy"),
              "matching history entry was not rendered");
  PASS();

  TEST("Ctrl-R accepts history for editing with prompt queue enabled");
  ASSERT_TRUE(run_pty_history_case_with_options(
                  "\022deploy\r!\r", history, 3, NULL, 20, 0, 1, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "queued history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  strcmp(result, "gamma deploy!") == 0,
              "queue-enabled search submitted history before editing");
  PASS();
}

static void test_ctrl_r_repeats_and_wraps_matches(void) {
  const char *history[3];
  const char *single_history[2];
  char terminal[8192];
  char result[256];
  int status;

  history[0] = "alpha deploy";
  history[1] = "beta deploy";
  history[2] = "gamma deploy";
  TEST("Ctrl-R repeats cycle older matches and wrap");
  ASSERT_TRUE(run_pty_history_case("\022deploy\022\r\r", history, 3, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "repeat history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "repeat child editor failed");
  ASSERT_TRUE(strcmp(result, "beta deploy") == 0,
              "repeat did not select older match");

  ASSERT_TRUE(run_pty_history_case("\022deploy\022\022\022\r\r", history, 3,
                                   terminal, sizeof(terminal), result,
                                   sizeof(result), &status) == 0,
              "wrapped repeat history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "wrapped repeat child editor failed");
  ASSERT_TRUE(strcmp(result, "gamma deploy") == 0,
              "repeat did not wrap to newest match");

  single_history[0] = "alpha deploy";
  single_history[1] = "beta build";
  ASSERT_TRUE(run_pty_history_case("\022deploy\022\r\r", single_history, 2,
                                   terminal, sizeof(terminal), result,
                                   sizeof(result), &status) == 0,
              "single-match repeat history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "single-match repeat child editor failed");
  ASSERT_TRUE(strcmp(result, "alpha deploy") == 0,
              "single repeated match did not remain selected");
  PASS();
}

static void test_ctrl_r_no_match_and_cancel_restore_draft(void) {
  const char *history[2];
  char terminal[8192];
  char result[256];
  int status;

  history[0] = "alpha";
  history[1] = "beta";
  TEST("Ctrl-R no-match accept leaves draft editable");
  ASSERT_TRUE(run_pty_history_case("draft\022zzz\r edited\r", history, 2,
                                   terminal, sizeof(terminal), result,
                                   sizeof(result), &status) == 0,
              "no-match history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "no-match child editor failed");
  ASSERT_TRUE(strcmp(result, "draft edited") == 0,
              "no-match search did not preserve an editable draft");
  ASSERT_TRUE(contains_bytes(terminal, "(failed)`zzz': "),
              "failed reverse search prompt missing");
  PASS();

  TEST("Ctrl-R cancel restores edited draft");
  ASSERT_TRUE(run_pty_history_case("draft\022alpha\007\r", history, 2, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "cancel history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "cancel child editor failed");
  ASSERT_TRUE(strcmp(result, "draft") == 0, "cancel did not restore draft");
  PASS();

  TEST("Ctrl-R with empty history leaves draft editable after Enter");
  ASSERT_TRUE(run_pty_history_case("draft\022\r edited\r", NULL, 0, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0,
              "empty history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  strcmp(result, "draft edited") == 0,
              "empty history search submitted the draft");
  PASS();
}

static void test_ctrl_r_searches_loaded_history_file(void) {
  char file[] = "/tmp/softline-history-search-XXXXXX";
  const char *history[1];
  char terminal[8192];
  char result[256];
  const char *data;
  int status;
  int fd;

  TEST("Ctrl-R searches loaded history file");
  fd = mkstemp(file);
  ASSERT_TRUE(fd >= 0, "mkstemp failed");
  data = "file alpha\nfile deploy\nhead\\ntail\n";
  ASSERT_TRUE(write(fd, data, strlen(data)) == (ssize_t)strlen(data),
              "write history file failed");
  close(fd);
  history[0] = "memory deploy";
  ASSERT_TRUE(run_pty_history_case_with_file(
                  "\022file\r edited\r", history, 1, file, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "loaded history search case failed");
  unlink(file);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "loaded history child editor failed");
  ASSERT_TRUE(strcmp(result, "file deploy edited") == 0,
              "loaded file history match mismatch");
  PASS();
}

static void test_ctrl_r_searches_unicode_and_multiline_history(void) {
  char file[] = "/tmp/softline-history-search-unicode-XXXXXX";
  char terminal[8192];
  char result[256];
  const char *data;
  int status;
  int fd;

  TEST("Ctrl-R searches Unicode and multiline history");
  fd = mkstemp(file);
  ASSERT_TRUE(fd >= 0, "mkstemp failed");
  data = "unicode \303\245\303\244\303\266\nhead\\ntail\n";
  ASSERT_TRUE(write(fd, data, strlen(data)) == (ssize_t)strlen(data),
              "write unicode history file failed");
  close(fd);
  ASSERT_TRUE(run_pty_history_case_with_file("\022\303\244\r!\r", NULL, 0, file,
                                             terminal, sizeof(terminal), result,
                                             sizeof(result), &status) == 0,
              "unicode history search case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "unicode history child editor failed");
  ASSERT_TRUE(strcmp(result, "unicode \303\245\303\244\303\266!") == 0,
              "unicode history match mismatch");

  ASSERT_TRUE(run_pty_history_case_with_file("\022tail\r!\r", NULL, 0, file,
                                             terminal, sizeof(terminal), result,
                                             sizeof(result), &status) == 0,
              "multiline history search case failed");
  unlink(file);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "multiline history child editor failed");
  ASSERT_TRUE(strcmp(result, "head\ntail!") == 0,
              "multiline history match mismatch");
  PASS();
}

static void test_bracketed_paste_is_literal_content(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("bracketed paste treats control bytes as content");
  ASSERT_TRUE(run_pty_readline_case("pre\033[200~\tab\025\033[D\rcd\033[201~\r",
                                    20, 0, NULL, terminal, sizeof(terminal),
                                    result, sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "pre\tab\025\033[D\ncd") == 0,
              "paste result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[?2004h"),
              "bracketed paste was not enabled");
  ASSERT_TRUE(contains_bytes(terminal, "\033[?2004l"),
              "bracketed paste was not disabled");
  PASS();
}

static void test_bracketed_paste_stops_at_line_max(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("bracketed paste stops at configured line max");
  ASSERT_TRUE(run_pty_readline_case_with_limit(
                  "\033[200~abcdef\033[201~\r", 20, 0, 4, NULL, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "abcd") == 0, "paste line max result mismatch");
  PASS();
}

static void test_tab_render_expands_beyond_line_bytes(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("tab rendering can exceed line byte count");
  ASSERT_TRUE(run_pty_readline_case_with_limit(
                  "\033[200~\t\033[201~\r", 1, 0, 4, NULL, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "\t") == 0, "tab result mismatch");
  PASS();
}

static void test_key_binding_tab_inserts_text(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("key binding TAB inserts text");
  ASSERT_TRUE(run_pty_key_binding_case("a\tb\r", SL_KEY_TAB, insert_text_key,
                                       "COMP", terminal, sizeof(terminal),
                                       result, sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "aCOMPb") == 0, "TAB binding result mismatch");
  PASS();
}

static void test_prompt_queue_dispatches_fifo_with_themes(void) {
  static const sl_prompt_theme_t stash_themes[] = {
      SL_PROMPT_THEME_DRACULA,    SL_PROMPT_THEME_GRUVBOX,
      SL_PROMPT_THEME_MONOCHROME, SL_PROMPT_THEME_MONOGREEN,
      SL_PROMPT_THEME_OUTRUN,     SL_PROMPT_THEME_SYNTHWAVE};
  static const char *const control_styles[] = {
      "\033[38;2;255;184;108mQ ", "\033[38;2;211;134;155mQ ",
      "\033[38;2;224;184;90mQ ",  "\033[38;2;51;255;51mQ ",
      "\033[38;2;255;42;109mQ ",  "\033[38;2;185;103;255mQ "};
  static const char *const text_styles[] = {
      "\033[38;2;255;184;108mfirst", "\033[38;2;211;134;155mfirst",
      "\033[38;2;224;184;90mfirst",  "\033[38;2;51;255;51mfirst",
      "\033[38;2;255;42;109mfirst",  "\033[38;2;185;103;255mfirst"};
  char terminal[16384];
  char result[512];
  int status;
  size_t i;

  TEST("prompt queue previews and dispatches FIFO across themes");
  ASSERT_TRUE(run_pty_prompt_queue_case("first\tsecond\tthird\r",
                                        SL_PROMPT_THEME_PLAIN, 1, 0, 3,
                                        terminal, sizeof(terminal), result,
                                        sizeof(result), &status) == 0,
              "plain prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "plain prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:third|2:first|2:second") == 0,
              "plain prompt queue dispatch order mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "Q 1. first"),
              "plain FIFO preview missing");
  ASSERT_TRUE(contains_bytes(terminal, "... 1 more"),
              "plain overflow count missing");

  ASSERT_TRUE(run_pty_prompt_queue_case(
                  "first\tsecond\r", SL_PROMPT_THEME_ACCENT, 1, 0, 2, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "accent prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "accent prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:second|2:first") == 0,
              "accent prompt queue dispatch mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;244;114;182mQ ") &&
                  contains_bytes(terminal, "\033[38;2;244;114;182mfirst") &&
                  contains_bytes(terminal, "\033[1;36mchat> \033[0m"),
              "accent full prompt treatment missing");

  ASSERT_TRUE(run_pty_prompt_queue_case(
                  "first\tsecond\r", SL_PROMPT_THEME_RICED, 1, 0, 2, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "riced prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "riced prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:second|2:first") == 0,
              "riced prompt queue dispatch mismatch");
  ASSERT_TRUE(
      contains_bytes(terminal, "\033[38;2;255;139;139mQ ") &&
          contains_bytes(terminal, "\033[38;2;255;139;139mfirst") &&
          contains_bytes(terminal, "\033[1;38;2;255;255;255mchat> \033[0m"),
      "riced full prompt treatment missing");
  ASSERT_TRUE(run_pty_prompt_queue_case(
                  "first\tsecond\r", SL_PROMPT_THEME_DEFAULT, 1, 0, 2, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "default prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "default prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:second|2:first") == 0,
              "default prompt queue dispatch mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[34mQ ") &&
                  contains_bytes(terminal, "\033[34mfirst") &&
                  contains_bytes(terminal, "\033[1;97mchat> \033[0m") &&
                  !contains_bytes(terminal, "\033[38;2;"),
              "default ANSI queue treatment missing");
  for (i = 0; i < sizeof(stash_themes) / sizeof(stash_themes[0]); i++) {
    ASSERT_TRUE(run_pty_prompt_queue_case("first\tsecond\r", stash_themes[i], 1,
                                          0, 2, terminal, sizeof(terminal),
                                          result, sizeof(result), &status) == 0,
                "stash theme prompt queue pty case failed");
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "stash theme prompt queue child failed");
    ASSERT_TRUE(strcmp(result, "1:second|2:first") == 0,
                "stash theme prompt queue dispatch mismatch");
    ASSERT_TRUE(contains_bytes(terminal, control_styles[i]) &&
                    contains_bytes(terminal, text_styles[i]),
                "stash theme queue palette treatment missing");
  }
  PASS();
}

static void test_prompt_queue_dispatches_fifo_in_normal_scrollback(void) {
  char terminal[16384];
  char result[512];
  int status;

  TEST("prompt queue dispatches FIFO in normal scrollback");
  ASSERT_TRUE(run_pty_prompt_queue_case(
                  "first\tsecond\r", SL_PROMPT_THEME_ACCENT, 0, 0, 2, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "normal prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "normal prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:second|2:first") == 0,
              "normal prompt queue dispatch mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;244;114;182mQ ") &&
                  contains_bytes(terminal, "\033[1;36mchat> \033[0m"),
              "normal prompt queue preview missing");
  PASS();
}

static void test_prompt_queue_clips_control_rows_on_narrow_terminals(void) {
  char terminal[16384];
  char result[512];
  int status;

  TEST("prompt queue clips control rows on narrow terminals");
  ASSERT_TRUE(run_pty_prompt_queue_case_with_width(
                  "first\tsecond\tthird\r", SL_PROMPT_THEME_PLAIN, 0, 0, 3, 4,
                  "p> ", terminal, sizeof(terminal), result, sizeof(result),
                  &status) == 0,
              "narrow prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "narrow prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:third|2:first|2:second") == 0,
              "narrow prompt queue dispatch mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "Q 1.") &&
                  !contains_bytes(terminal, "Q 1. "),
              "queue prefix exceeded narrow render width");
  ASSERT_TRUE(contains_bytes(terminal, "  ..") &&
                  !contains_bytes(terminal, "... 1 more"),
              "queue overflow row exceeded narrow render width");
  PASS();
}

static void test_prompt_queue_previews_control_bytes_safely(void) {
  static const char input[] = "\033[200~a\t\033[31m\302\23331mX\033[201~\tok\r";
  char terminal[16384];
  char result[512];
  int status;

  TEST("prompt queue previews pasted controls without terminal escapes");
  ASSERT_TRUE(run_pty_prompt_queue_case(input, SL_PROMPT_THEME_PLAIN, 1, 0, 2,
                                        terminal, sizeof(terminal), result,
                                        sizeof(result), &status) == 0,
              "control-byte prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "control-byte prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:ok|2:a\t\033[31m\302\23331mX") == 0,
              "queued control-byte prompt was changed");
  ASSERT_TRUE(contains_bytes(terminal, "Q 1. a  ^[[31m\\x9B31mX"),
              "queued control-byte preview was not safely rendered");
  PASS();
}

static void test_prompt_queue_rejects_reduced_capacity(void) {
  char terminal[16384];
  char result[512];
  int status;

  TEST("prompt queue rejects reduced capacity with pending prompts");
  ASSERT_TRUE(run_pty_prompt_queue_case("first\tsecond\tthird",
                                        SL_PROMPT_THEME_PLAIN, 0, 1, 3,
                                        terminal, sizeof(terminal), result,
                                        sizeof(result), &status) == 0,
              "queue capacity pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queue capacity child failed");
  ASSERT_TRUE(strcmp(result, "-2;1:third|2:first|2:second") == 0,
              "reduced capacity changed queued dispatch");
  PASS();
}

static void test_prompt_queue_alt_e_recalls_newest(void) {
  char terminal[16384];
  char result[512];
  int status;

  TEST("Alt-E recalls the newest queued prompt into the editor");
  ASSERT_TRUE(run_pty_prompt_queue_case("first\tsecond\t\033e\r",
                                        SL_PROMPT_THEME_PLAIN, 1, 0, 2,
                                        terminal, sizeof(terminal), result,
                                        sizeof(result), &status) == 0,
              "Alt-E prompt queue pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Alt-E prompt queue child failed");
  ASSERT_TRUE(strcmp(result, "1:second|2:first") == 0,
              "Alt-E did not restore queue tail");
  ASSERT_TRUE(contains_bytes(terminal, "Q 1. first"),
              "Alt-E did not redraw the remaining queued preview");
  PASS();
}

static void test_prompt_themes_style_normal_readline(void) {
  static const sl_prompt_theme_t themes[] = {
      SL_PROMPT_THEME_DEFAULT,    SL_PROMPT_THEME_ACCENT,
      SL_PROMPT_THEME_DRACULA,    SL_PROMPT_THEME_GRUVBOX,
      SL_PROMPT_THEME_MONOCHROME, SL_PROMPT_THEME_MONOGREEN,
      SL_PROMPT_THEME_OUTRUN,     SL_PROMPT_THEME_RICED,
      SL_PROMPT_THEME_SYNTHWAVE};
  static const char *const styles[] = {"\033[1;97mnormal> ",
                                       "\033[1;36mnormal> ",
                                       "\033[38;2;98;114;164mnormal> ",
                                       "\033[1;38;2;184;187;38mnormal> ",
                                       "\033[1;38;2;168;118;40mnormal> ",
                                       "\033[1;38;2;22;122;31mnormal> ",
                                       "\033[38;2;78;69;99mnormal> ",
                                       "\033[1;38;2;255;255;255mnormal> ",
                                       "\033[1;38;2;255;126;219mnormal> "};
  static const char *const input_styles[] = {"",
                                             "",
                                             "",
                                             "",
                                             "\033[38;2;255;224;138mok\033[0m",
                                             "\033[1;38;2;51;255;51mok\033[0m",
                                             "",
                                             "",
                                             ""};
  char terminal[4096];
  char result[64];
  char styled_prompt[128];
  int status;
  size_t i;

  TEST("prompt themes style normal readline UI");
  for (i = 0; i < sizeof(themes) / sizeof(themes[0]); i++) {
    ASSERT_TRUE(run_pty_themed_readline_case(themes[i], terminal,
                                             sizeof(terminal), result,
                                             sizeof(result), &status) == 0,
                "themed normal prompt pty case failed");
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "themed normal prompt child failed");
    ASSERT_TRUE(strcmp(result, "ok") == 0,
                "themed normal prompt result mismatch");
    ASSERT_TRUE(contains_bytes(terminal, styles[i]),
                "themed normal prompt treatment missing");
    (void)snprintf(styled_prompt, sizeof(styled_prompt), "%s\033[0m",
                   styles[i]);
    ASSERT_TRUE(contains_bytes(terminal, styled_prompt),
                "themed prompt did not reset before typed input");
    if (themes[i] == SL_PROMPT_THEME_DEFAULT)
      ASSERT_TRUE(!contains_bytes(terminal, "\033[38;2;"),
                  "default prompt used true-colour output");
    if (input_styles[i][0] != '\0')
      ASSERT_TRUE(contains_bytes(terminal, input_styles[i]),
                  "themed input treatment missing");
  }
  PASS();
}

static void test_statusline_uses_palette_offset_and_truncation(void) {
  char terminal[16384];
  char result[64];
  int status;

  TEST("status line offsets colours and truncates after 32 elements");
  ASSERT_TRUE(run_pty_statusline_case(SL_PROMPT_THEME_RICED, terminal,
                                      sizeof(terminal), result, sizeof(result),
                                      &status) == 0,
              "status line pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "status line child failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0, "status line result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;255;51;51m/ "),
              "status spinner marker missing");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;255;51;51m- "),
              "status spinner did not advance");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;57;255;20m+ "),
              "default status idle marker missing");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;57;255;20m- "),
              "status idle marker missing");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;172;164;184me0"),
              "status starting palette colour missing");
  ASSERT_TRUE(contains_bytes(terminal, "\033[38;2;54;249;246me1"),
              "status palette did not wrap after index seven");
  ASSERT_TRUE(contains_bytes(terminal, "e30") &&
                  !contains_bytes(terminal, "e31") &&
                  contains_bytes(terminal, "..."),
              "status line did not retain 31 elements plus ellipsis");
  ASSERT_TRUE(
      contains_bytes(terminal, "\033[38;2;72;76;105m! \033[0m") &&
          contains_bytes(terminal,
                         "\033[3;38;2;172;164;184mHello world, this is a") &&
          contains_bytes(terminal,
                         "  \033[3;38;2;172;164;184mlong status line, that") &&
          contains_bytes(terminal,
                         "  \033[3;38;2;172;164;184mcontinues on multiple") &&
          contains_bytes(terminal, "  \033[3;38;2;172;164;184mlines."),
      "status message styles or word wrapping failed");
  ASSERT_TRUE(contains_after_bytes(terminal, "e7", "\n"),
              "status elements did not wrap between elements");
  PASS();
}

static void test_default_statusline_uses_ansi_palette(void) {
  char terminal[16384];
  char result[64];
  int status;

  TEST("default status line uses standard ANSI palette");
  ASSERT_TRUE(run_pty_statusline_case(SL_PROMPT_THEME_DEFAULT, terminal,
                                      sizeof(terminal), result, sizeof(result),
                                      &status) == 0,
              "default status line pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "default status line child failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0, "default status line result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[31m/ ") &&
                  contains_bytes(terminal, "\033[31m- ") &&
                  contains_bytes(terminal, "\033[32m+ ") &&
                  contains_bytes(terminal, "\033[32m- "),
              "default status markers did not use ANSI red and green");
  ASSERT_TRUE(contains_bytes(terminal, "\033[97me0") &&
                  contains_bytes(terminal, "\033[36me1") &&
                  contains_bytes(terminal, "\033[90m : ") &&
                  !contains_bytes(terminal, "\033[38;2;"),
              "default status palette did not use ANSI colours");
  ASSERT_TRUE(
      contains_bytes(terminal,
                     "\033[2;90m! \033[0m\033[3;90mHello world, this is a") &&
          contains_bytes(terminal, "  \033[3;90mlong status line, that") &&
          contains_bytes(terminal, "  \033[3;90mcontinues on multiple") &&
          contains_bytes(terminal, "  \033[3;90mlines."),
      "default status colors or word wrapping failed");
  PASS();
}

static void test_queueing_resets_history_navigation(void) {
  const char *history[] = {"one", "two"};
  char terminal[4096];
  char result[256];
  int status;

  TEST("queueing a recalled prompt resets history navigation");
  ASSERT_TRUE(run_pty_history_case_with_options(
                  "\033[A\t\033[A\r", history, 2, NULL, 20, 0, 1, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "queued history case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued history child failed");
  ASSERT_TRUE(strcmp(result, "two") == 0,
              "queueing recalled history skipped newest entry");
  PASS();
}

static void test_key_binding_alt_m_inserts_text(void) {
  char terminal[4096];
  char result[256];
  int status;
  sl_key_t documented_alt_m;

  TEST("key binding Alt-M inserts text");
  documented_alt_m = (sl_key_t)(SL_KEY_ALT_BASE + (unsigned char)'m');
  ASSERT_TRUE(SL_KEY_ALT_M == documented_alt_m,
              "SL_KEY_ALT_M does not match documented Alt-letter encoding");
  ASSERT_TRUE(run_pty_key_binding_case(
                  "\033m\r", documented_alt_m, insert_text_key, "ALT", terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ALT") == 0, "Alt-M binding result mismatch");
  PASS();
}

static void test_key_binding_f1_submits(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("key binding F1 submits");
  ASSERT_TRUE(run_pty_key_binding_case(
                  "draft\033OP", SL_KEY_F1, submit_action_key, NULL, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "draft") == 0, "F1 submit result mismatch");
  PASS();
}

static void test_key_binding_can_edit_buffer_and_submit(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("key binding can edit buffer and submit");
  ASSERT_TRUE(run_pty_key_binding_case("\033OQ", SL_KEY_F2, edit_buffer_key,
                                       NULL, terminal, sizeof(terminal), result,
                                       sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "abXXcd") == 0, "edit binding result mismatch");
  PASS();
}

static void test_key_binding_can_cancel(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("key binding can cancel");
  ASSERT_TRUE(run_pty_key_binding_case(
                  "draft\033OR", SL_KEY_F3, cancel_action_key, NULL, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "<NULL>") == 0, "cancel result mismatch");
  PASS();
}

static void test_key_binding_enter_can_pass_to_default(void) {
  char terminal[4096];
  char result[256];
  int status;
  struct enter_override_state state;

  TEST("key binding Enter can pass to default");
  state.calls = 0;
  ASSERT_TRUE(run_pty_key_binding_case(
                  "a\rb\r", SL_KEY_ENTER, enter_override_key, &state, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "a\nb") == 0, "Enter binding result mismatch");
  PASS();
}

static void test_key_binding_ctrl_enter_can_call_submit(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("key binding Ctrl-Enter can call submit");
  ASSERT_TRUE(run_pty_key_binding_case("draft\033[13;5u", SL_KEY_CTRL_ENTER,
                                       submit_method_key, NULL, terminal,
                                       sizeof(terminal), result, sizeof(result),
                                       &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "draft") == 0,
              "Ctrl-Enter submit result mismatch");
  PASS();
}

static void test_next_prompt_retains_raw_input_between_turns(void) {
  int master_fd;
  int slave_fd;
  int ready_pipe[2];
  int release_pipe[2];
  int result_pipe[2];
  int status;
  pid_t pid;
  char terminal[4096];
  char result[128];
  size_t terminal_len;
  ssize_t n;

  TEST("next_prompt preserves Ctrl-Enter between turn results");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(ready_pipe) == 0 && pipe(release_pipe) == 0 &&
                  pipe(result_pipe) == 0,
              "handoff pipe setup failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_prompt_source_t source;
    sl_t *sl;
    char *first;
    char *second;
    char output[128];
    struct termios before;
    struct termios after;
    int restored;
    int written;
    char release;
    close(master_fd);
    close(ready_pipe[0]);
    close(release_pipe[1]);
    close(result_pipe[0]);
    if (tcgetattr(slave_fd, &before) != 0)
      _exit(2);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_bind_key(sl, SL_KEY_CTRL_ENTER, submit_method_key, NULL) != SL_OK)
      _exit(3);
    source = SL_PROMPT_SOURCE_NONE;
    first = sl_next_prompt(sl, "first> ", &source);
    if (!first || source != SL_PROMPT_SOURCE_DIRECT)
      _exit(4);
    if (write(ready_pipe[1], "r", 1) != 1)
      _exit(5);
    if (read(release_pipe[0], &release, 1) != 1)
      _exit(6);
    source = SL_PROMPT_SOURCE_NONE;
    second = sl_next_prompt(sl, "second> ", &source);
    if (!second || source != SL_PROMPT_SOURCE_DIRECT)
      _exit(7);
    written = snprintf(output, sizeof(output), "%s|%s", first, second);
    sl_free_string(sl, first);
    sl_free_string(sl, second);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(8);
    sl_destroy(sl);
    restored = tcgetattr(slave_fd, &after) == 0 &&
               termios_same_observable(&before, &after);
    written += snprintf(output + written, sizeof(output) - (size_t)written,
                        "|%s", restored ? "RESTORED" : "RAW");
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(9);
    (void)write(result_pipe[1], output, (size_t)written);
    close(slave_fd);
    close(ready_pipe[1]);
    close(release_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(ready_pipe[1]);
  close(release_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "first> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "first handoff prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "first\r", strlen("first\r")) ==
                  (ssize_t)strlen("first\r"),
              "first handoff input failed");
  n = read_some_with_timeout(ready_pipe[0], result, sizeof(result));
  ASSERT_TRUE(n == 1 && result[0] == 'r', "host handoff was not reached");
  /* next_prompt retains raw terminal ownership while the host handles a turn.
   */
  ASSERT_TRUE(
      write(master_fd, "second\033[13;5u", strlen("second\033[13;5u")) ==
          (ssize_t)strlen("second\033[13;5u"),
      "immediate Ctrl-Enter input failed");
  ASSERT_TRUE(write(release_pipe[1], "g", 1) == 1, "handoff release failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "immediate Ctrl-Enter result missing");
  result[n] = '\0';
  close(master_fd);
  close(ready_pipe[0]);
  close(release_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "handoff child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "handoff child failed");
  ASSERT_TRUE(strcmp(result, "first|second|RESTORED") == 0,
              "next_prompt did not preserve input or restore on destruction");
  PASS();
}

static void test_key_binding_ctrl_r_overrides_history_search(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("key binding Ctrl-R overrides history search");
  ASSERT_TRUE(run_pty_key_binding_case("\022\r", SL_KEY_CTRL_R, insert_text_key,
                                       "BOUND", terminal, sizeof(terminal),
                                       result, sizeof(result), &status) == 0,
              "key binding case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "BOUND") == 0, "Ctrl-R binding result mismatch");
  ASSERT_TRUE(!contains_bytes(terminal, "(r-search)"),
              "built-in reverse search ran despite binding");
  PASS();
}

static void test_prompt_reset_keys(void) {
  char terminal[8192], result[256];
  const char *history[] = {"first", "second"};
  int status;
  TEST("Ctrl-L resets layout while preserving text and cursor");
  ASSERT_TRUE(run_pty_readline_case("abcd\033[D\033[D\014\014X\r", 24, 0, NULL,
                                    terminal, sizeof(terminal), result,
                                    sizeof(result), &status) == 0,
              "reset case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "abXcd"),
              "reset changed draft/cursor");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[2J") &&
                  !contains_bytes(terminal, "\033[3J") &&
                  !contains_bytes(terminal, "\033c") &&
                  !contains_bytes(terminal, "\033[?1049h"),
              "reset cleared or replaced the terminal");
  ASSERT_TRUE(run_pty_key_binding_case("abcd\033[D\033[D\tX\r", SL_KEY_TAB,
                                       reset_action_key, NULL, terminal,
                                       sizeof(terminal), result, sizeof(result),
                                       &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "abXcd"),
              "reset action could not be rebound");
  ASSERT_TRUE(contains_after_bytes(terminal, "abcd", "abcd"),
              "remapped reset did not repaint unchanged text");
  ASSERT_TRUE(run_pty_key_binding_case(
                  "ab\014cd\r", SL_KEY_CTRL_L, insert_text_key, "*", terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "ab*cd"),
              "Ctrl-L override was ignored");
  ASSERT_TRUE(run_pty_history_case("\022sec\014\r!\r", history, 2, terminal,
                                   sizeof(terminal), result, sizeof(result),
                                   &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "second!"),
              "reset cancelled history search");
  PASS();
}

static void test_terminal_mode_is_restored(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  char terminal[4096];
  char result[256];
  size_t terminal_len;
  ssize_t n;
  int status;

  TEST("readline restores terminal mode before returning");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct termios before;
    struct termios after;
    int restored;
    close(master_fd);
    close(result_pipe[0]);
    if (tcgetattr(slave_fd, &before) != 0)
      _exit(2);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(3);
    line = sl->readline(sl, "p> ");
    restored = tcgetattr(slave_fd, &after) == 0 &&
               termios_same_observable(&before, &after);
    if (line) {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    if (restored)
      (void)write(result_pipe[1], "|RESTORED", 9);
    else
      (void)write(result_pipe[1], "|RAW", 4);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_until_eof_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ok|RESTORED") == 0,
              "terminal mode was not restored");
  PASS();
}

static void test_non_ctrl_c_signal_does_not_interrupt_readline(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  int ack_pipe[2];
  pid_t pid;
  char terminal[4096];
  char result[256];
  char ack;
  size_t terminal_len;
  size_t result_len;
  ssize_t n;
  int status;
  int tries;

  TEST("non-Ctrl-C signal does not interrupt readline");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  ASSERT_TRUE(pipe(ack_pipe) == 0, "ack pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct sigaction sa;
    close(master_fd);
    close(result_pipe[0]);
    close(ack_pipe[0]);
    signal_ack_fd = ack_pipe[1];
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = remember_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
      _exit(2);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line)
      (void)write(result_pipe[1], "<NULL>", 6);
    else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    if (signal_seen)
      (void)write(result_pipe[1], "|SIG", 4);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    close(ack_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  close(ack_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(kill(pid, SIGUSR1) == 0, "signal failed");
  n = read_some_with_timeout(ack_pipe[0], &ack, 1);
  ASSERT_TRUE(n == 1, "signal handler did not run");
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  result_len = 0;
  result[0] = '\0';
  tries = 0;
  while (!contains_bytes(result, "|SIG") && tries < 250) {
    n = read_some_with_timeout_ms(result_pipe[0], result + result_len,
                                  sizeof(result) - 1 - result_len, 20);
    if (n > 0) {
      result_len += (size_t)n;
      result[result_len] = '\0';
    }
    tries++;
  }
  ASSERT_TRUE(result_len > 0, "read result failed");
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  close(master_fd);
  close(ack_pipe[0]);
  close(result_pipe[0]);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ok|SIG") == 0, "signal interrupted readline");
  PASS();
}

static void test_pty_ctrl_d_exits(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("pty Ctrl-D on empty prompt returns NULL");
  ASSERT_TRUE(run_pty_readline_case("\004", 20, 0, NULL, terminal,
                                    sizeof(terminal), result, sizeof(result),
                                    &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "<NULL>") == 0, "Ctrl-D result mismatch");
  PASS();
}

struct line_key_case {
  const char *name;
  const char *draft;
  size_t cursor;
  const char *keys;
  const char *expected;
};

static int prepare_line_key_case(sl_t *sl, sl_key_t key, void *userdata,
                                 sl_key_action_t *action) {
  struct line_key_case *state = userdata;
  (void)key;
  if (sl_set_buffer(sl, state->draft) != SL_OK ||
      sl_set_cursor(sl, state->cursor) != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static void test_pty_logical_line_keys(void) {
  char terminal[4096];
  char result[256];
  char input[64];
  int status;
  size_t i;
  struct line_key_case cases[] = {
      {"Ctrl-A moves to the current line start", "ab\ncdef\ngh", 5, "\001",
       "ab\n!cdef\ngh"},
      {"Ctrl-E moves to the current line end", "ab\ncdef\ngh", 5, "\005",
       "ab\ncdef!\ngh"},
      {"Home moves to the current line start", "ab\ncdef\ngh", 5, "\033[H",
       "ab\n!cdef\ngh"},
      {"End moves to the current line end", "ab\ncdef\ngh", 5, "\033[F",
       "ab\ncdef!\ngh"},
      {"Tilde Home moves to the current line start", "ab\ncdef\ngh", 5,
       "\033[1~", "ab\n!cdef\ngh"},
      {"Tilde End moves to the current line end", "ab\ncdef\ngh", 5, "\033[4~",
       "ab\ncdef!\ngh"},
      {"Ctrl-A stays at the current line start", "ab\ncdef\ngh", 3, "\001\001",
       "ab\n!cdef\ngh"},
      {"Ctrl-E stays at the current line end", "ab\ncdef\ngh", 7, "\005\005",
       "ab\ncdef!\ngh"},
      {"Ctrl-A at a newline uses its preceding line", "ab\ncd", 2, "\001",
       "!ab\ncd"},
      {"Ctrl-E at a newline stays on its preceding line", "ab\ncd", 2, "\005",
       "ab!\ncd"},
      {"Empty line navigation stays on that line", "ab\n\ncd", 3, "\001\005",
       "ab\n!\ncd"},
      {"Trailing newline navigation stays on the empty line", "ab\n", 3,
       "\001\005", "ab\n!"},
      {"Empty buffer line keys preserve the cursor", "", 0, "\001\005\025\013",
       "!"},
      {"Ctrl-A ignores soft wraps", "abcdefghijklmnopqrstuvwxyZ\nnext", 23,
       "\001", "!abcdefghijklmnopqrstuvwxyZ\nnext"},
      {"Ctrl-E ignores soft wraps", "abcdefghijklmnopqrstuvwxyZ\nnext", 1,
       "\005", "abcdefghijklmnopqrstuvwxyZ!\nnext"},
      {"Ctrl-U deletes only the current line prefix", "ab\ncdef\ngh", 5, "\025",
       "ab\n!ef\ngh"},
      {"Ctrl-U at line start removes only the preceding newline",
       "ab\ncdef\ngh", 3, "\025", "ab!cdef\ngh"},
      {"Repeated Ctrl-U removes text then just its newline", "ab\ncdef\ngh", 5,
       "\025\025", "ab!ef\ngh"},
      {"Ctrl-U preserves the newline at line end", "ab\ncdef\ngh", 7, "\025",
       "ab\n!\ngh"},
      {"Ctrl-U removes an empty line", "ab\n\ncd", 3, "\025", "ab!\ncd"},
      {"Ctrl-U removes a trailing empty line", "ab\n", 3, "\025", "ab!"},
      {"Ctrl-U removes successive trailing empty lines", "ab\n\n\n", 5,
       "\025\025\025", "ab!"},
      {"Ctrl-U preserves an empty first line", "\ncd", 0, "\025", "!\ncd"},
      {"Ctrl-U clears whitespace before removing its newline", "ab\n \t\ncd", 5,
       "\025\025", "ab!\ncd"},
      {"Ctrl-U removes a blank line reached with Up", "ab\n\ncd", 6,
       "\033[A\025", "ab!\ncd"},
      {"Ctrl-U joins Unicode lines without splitting clusters", "é\n中文", 3,
       "\025", "é!中文"},
      {"Ctrl-U ignores soft wraps", "abcdefghijklmnopqrstuvwxyZ\nnext", 23,
       "\025", "!xyZ\nnext"},
      {"Ctrl-K preserves subsequent lines", "ab\ncd", 0, "\013", "!\ncd"},
      {"Ctrl-K preserves surrounding lines", "ab\ncdef\ngh", 5, "\013",
       "ab\ncd!\ngh"},
      {"Ctrl-K at line end joins only the following line", "ab\ncd", 2, "\013",
       "ab!cd"},
      {"Ctrl-K on an empty line removes only that newline", "ab\n\ncd", 3,
       "\013", "ab\n!cd"},
      {"Repeated Ctrl-K removes text then just its newline", "ab\ncd\nend", 0,
       "\013\013", "!cd\nend"},
      {"Ctrl-K at buffer end preserves the draft", "ab\ncd", 5, "\013",
       "ab\ncd!"},
      {"Ctrl-K ignores soft wraps", "abcdefghijklmnopqrstuvwxyZ\nnext", 1,
       "\013", "a!\nnext"},
      {"Ctrl-W removes a word within the current line", "ab\ncd ef\ngh", 8,
       "\027", "ab\ncd !\ngh"},
      {"Ctrl-W never deletes a word from the preceding line", "ab\ncd", 4,
       "\027", "ab\n!d"},
      {"Ctrl-W at line start removes only the preceding newline", "ab\ncd", 3,
       "\027", "ab!cd"},
      {"Ctrl-W on an empty line removes only one newline", "ab\n\ncd", 3,
       "\027", "ab!\ncd"},
      {"Ctrl-W clears indentation without crossing a newline", "ab\n \tcd", 5,
       "\027", "ab\n!cd"},
      {"Ctrl-W removes trailing spaces and the word on this line",
       "ab\ncd ef \ngh", 9, "\027", "ab\ncd !\ngh"},
      {"Ctrl-W preserves the start of the first line", "ab\ncd", 0, "\027",
       "!ab\ncd"},
      {"Ctrl-W removes a trailing blank line without its preceding word",
       "ab\n", 3, "\027", "ab!"},
      {"Ctrl-W clears only whitespace on a blank line", "ab\n \t\ncd", 5,
       "\027", "ab\n!\ncd"},
      {"Ctrl-W preserves Unicode on adjacent lines", "é\n中文", 9, "\027",
       "é\n!"},
      {"Ctrl-W joins Unicode lines without removing the previous word",
       "é\n中文", 3, "\027", "é!中文"},
      {"Ctrl-W removes blank lines reached with Up", "ab\n\ncd", 6,
       "\033[A\027", "ab!\ncd"},
      {"Ctrl-W removes a whole word across soft wraps",
       "ab\nabcdefghijklmnopqrstuvwxyz next", 29, "\027", "ab\n! next"},
      {"Repeated Ctrl-W removes a word then just its newline", "ab\ncd", 5,
       "\027\027", "ab!"},
      {"Ctrl-A preserves Unicode and other lines", "é中文\n世界", 6, "\001",
       "!é中文\n世界"},
      {"Ctrl-E preserves Unicode and other lines", "é中文\n世界", 6, "\005",
       "é中文!\n世界"},
      {"Ctrl-U preserves Unicode suffixes and other lines", "é中文\n世界", 6,
       "\025", "!文\n世界"},
      {"Ctrl-K preserves Unicode prefixes and other lines", "é中文\n世界", 3,
       "\013", "é!\n世界"},
      {"Single-line navigation retains its behavior", "abcd", 2, "\001\005",
       "abcd!"},
      {"Single-line Ctrl-U retains its behavior", "abcd", 2, "\025", "!cd"},
      {"Single-line Ctrl-K retains its behavior", "abcd", 2, "\013", "ab!"}};

  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    TEST(cases[i].name);
    (void)snprintf(input, sizeof(input), "\t%s!\r", cases[i].keys);
    ASSERT_TRUE(run_pty_key_binding_case(input, SL_KEY_TAB,
                                         prepare_line_key_case, &cases[i],
                                         terminal, sizeof(terminal), result,
                                         sizeof(result), &status) == 0,
                "pty case failed");
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "child editor failed");
    ASSERT_TRUE(strcmp(result, cases[i].expected) == 0,
                "line key changed text or cursor outside the current line");
    PASS();
  }

  TEST("Line keys preserve multiline pasted content");
  ASSERT_TRUE(
      run_pty_readline_case("\033[200~ab\rcd\033[201~\002\002\002\001\013Y\r",
                            20, 0, NULL, terminal, sizeof(terminal), result,
                            sizeof(result), &status) == 0 &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
          !strcmp(result, "Y\ncd"),
      "line key changed pasted content outside the current line");
  PASS();
}

static void test_readline_edit_commands(void) {
  char terminal[8192], result[512], input[512];
  int status;
  size_t i;
  const char *history[] = {"matching history"};
  struct line_key_case cases[] = {
      {"Alt-F ends at a word end", "one two", 0, "\033f", "one! two"},
      {"Alt-B recognizes punctuation", "foo.bar", 7, "\033b", "foo.!bar"},
      {"Alt-F skips punctuation before a word", "..foo_bar", 0, "\033f",
       "..foo!_bar"},
      {"Alt-B treats underscore as a word separator", "foo_bar", 7, "\033b",
       "foo_!bar"},
      {"Alt-F recognizes Unicode punctuation", "café—中文 end", 0, "\033f",
       "café!—中文 end"},
      {"Alt-B recognizes Unicode words", "café—中文", 14, "\033b",
       "café—!中文"},
      {"Alt-F skips a complete emoji cluster", "👩‍💻 café", 0, "\033f",
       "👩‍💻 café!"},
      {"Alt-B preserves a combining cluster", "é.foo", 7, "\033b\033b",
       "!é.foo"},
      {"Down retains its column across a short line",
       "abcdefghij\nx\nabcdefghij", 8, "\033[B\033[B",
       "abcdefghij\nx\nabcdefgh!ij"},
      {"Up retains its column across a short line", "abcdefghij\nx\nabcdefghij",
       21, "\033[A\033[A", "abcdefgh!ij\nx\nabcdefghij"},
      {"Down retains its column across an empty line", "abcdef\n\nabcdef", 5,
       "\033[B\033[B", "abcdef\n\nabcde!f"},
      {"Horizontal movement resets the preferred column",
       "abcdefghij\nx\nabcdefghij", 8, "\033[B\002\033[B",
       "abcdefghij\nx\n!abcdefghij"},
      {"Typing resets the preferred column", "abcdefghij\nx\nabcdefghij", 8,
       "\033[BZ\033[B", "abcdefghij\nxZ\nab!cdefghij"},
      {"Ctrl-L preserves the preferred column", "abcdefghij\nx\nabcdefghij", 8,
       "\033[B\014\033[B", "abcdefghij\nx\nabcdefgh!ij"},
      {"A tab does not replace the preferred column",
       "abcdefghij\n\t\nabcdefghij", 8, "\033[B\033[B",
       "abcdefghij\n\t\nabcdefgh!ij"},
      {"A wide cluster does not replace the preferred column",
       "abcdefghij\n中\nabcdefghij", 8, "\033[B\033[B",
       "abcdefghij\n中\nabcdefgh!ij"},
      {"Ctrl-Y restores killed line text", "abc\ndef", 1, "\013\031",
       "abc!\ndef"},
      {"Undo restores a forward kill and its cursor", "abc\ndef", 1, "\013\037",
       "a!bc\ndef"},
      {"Undo restores a backward kill and its cursor", "abc\ndef", 6,
       "\025\037", "abc\nde!f"},
      {"Consecutive forward kills append in textual order", "abc\ndef", 0,
       "\013\013\031", "abc\n!def"},
      {"Consecutive backward kills prepend in textual order", "abc\ndef", 7,
       "\025\025\031", "abc\ndef!"},
      {"Mixed direction kills preserve textual order", "abc def", 3,
       "\025\013\031", "abc def!"},
      {"Alt-Y rotates a yank to an older kill", "one two", 7,
       "\027\001\013\031\033y", "two!"},
      {"Alt-Y wraps around the kill ring", "one two", 7,
       "\027\001\013\031\033y\033y", "one !"},
      {"Editing invalidates yank rotation", "one two", 7,
       "\027\001\013\031X\033y", "one X!"},
      {"Undo removes a yank as one command", "abc", 0, "\013\031\037", "!"},
      {"Undo keeps consecutive kills individually reversible", "abc\ndef", 0,
       "\013\013\037", "!\ndef"},
      {"An empty kill ring leaves the draft intact", "abc", 1, "\031", "a!bc"},
      {"Alt-Y requires a preceding yank", "abc", 0, "\013\033y", "!"},
      {"Delete does not write to the kill ring", "abc", 1, "\004\031", "a!c"},
      {"Alt-D kills an alphanumeric word", "one.two three", 0, "\033d",
       "!.two three"},
      {"Alt-Backspace kills an alphanumeric word", "one.two three", 7,
       "\033\177", "one.! three"},
      {"Alt-Backspace accepts the BS encoding", "one.two", 7, "\033\010",
       "one.!"},
      {"Alt-D supplies the kill ring", "one.two", 0, "\033d\031", "one!.two"},
      {"Ctrl-T transposes adjacent characters", "abcd", 2, "\024", "acb!d"},
      {"Ctrl-T transposes at buffer end", "abcd", 4, "\024", "abdc!"},
      {"Ctrl-T preserves Unicode clusters", "aé中", 1, "\024", "éa!中"},
      {"Ctrl-T preserves combining clusters", "aéZ", 1, "\024", "éa!Z"},
      {"Ctrl-T preserves joined emoji clusters", "a👩‍💻Z", 1, "\024",
       "👩‍💻a!Z"},
      {"Ctrl-T at buffer start is harmless", "ab", 0, "\024", "!ab"},
      {"Ctrl-T with one cluster is harmless", "é", 2, "\024", "é!"},
      {"Undo restores transposition atomically", "abcd", 2, "\024\037",
       "ab!cd"},
      {"Undo removes a continuous typing run", "base", 4, "XYZ\037", "base!"},
      {"Movement separates undo typing groups", "", 0, "ab\002X\037", "a!b"},
      {"Repeated undo reaches the original empty draft", "", 0,
       "ab\002X\037\037\037", "!"},
      {"Undo restores forward deletion", "abc", 1, "\004\037", "a!bc"},
      {"Undo restores backspace", "abc", 2, "\177\037", "ab!c"},
      {"Undo treats bracketed paste as one edit", "ab", 1,
       "\033[200~x\rYZ\033[201~\037", "a!b"},
      {"Undo keeps Unicode typing whole", "", 0, "café中文\037", "!"},
      {"Ctrl-X Ctrl-U requests undo", "base", 4, "XYZ\030\025", "base!"},
      {"Unsupported Ctrl-X chord consumes a complete two-byte character",
       "base", 4, "\030é", "base!"},
      {"Unsupported Ctrl-X chord consumes a complete three-byte character",
       "base", 4, "\030中", "base!"},
      {"Unsupported Ctrl-X chord consumes a complete four-byte character",
       "base", 4, "\030😀", "base!"},
      {"Ctrl-X consumes only the rejected character", "base", 4, "\030é中文",
       "base中文!"},
      {"Rejected Unicode chord preserves subsequent undo", "base", 4,
       "XYZ\030é\037", "base!"},
      {"A paste cancels the Ctrl-X prefix and remains literal", "", 0,
       "\030\033[200~a\rcd\025\033[201~", "a\ncd\025!"},
      {"Ctrl-L preserves draft undo history", "base", 4, "XYZ\014\037",
       "base!"}};
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    TEST(cases[i].name);
    (void)snprintf(input, sizeof(input), "\t%s!\r", cases[i].keys);
    ASSERT_TRUE(run_pty_key_binding_case(input, SL_KEY_TAB,
                                         prepare_line_key_case, &cases[i],
                                         terminal, sizeof(terminal), result,
                                         sizeof(result), &status) == 0,
                "PTY editing case failed");
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "editor child failed");
    if (strcmp(result, cases[i].expected))
      fprintf(stderr, "\nobserved [%s], expected [%s]\n", result,
              cases[i].expected);
    ASSERT_TRUE(!strcmp(result, cases[i].expected),
                "editing result/cursor mismatch");
    PASS();
  }
  TEST("A refused yank does not partially change a limited draft");
  ASSERT_TRUE(run_pty_readline_case_with_limit(
                  "abc\001\0131234\031!\r", 24, 0, 5, NULL, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "1234!"),
              "limited yank changed the draft");
  PASS();
  TEST("A failed yank preserves the preceding undo command");
  ASSERT_TRUE(
      run_pty_readline_case_with_limit("abc\001\0131234\031\037!\r", 24, 0, 5,
                                       NULL, terminal, sizeof(terminal), result,
                                       sizeof(result), &status) == 0 &&
          WIFEXITED(status) && WEXITSTATUS(status) == 0 && !strcmp(result, "!"),
      "refused yank damaged undo history");
  PASS();
  TEST("Kill ring survives a readline boundary on the same handle");
  ASSERT_TRUE(run_pty_prompt_queue_case("saved\001\013first\r\031\r",
                                        SL_PROMPT_THEME_PLAIN, 0, 0, 2,
                                        terminal, sizeof(terminal), result,
                                        sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "1:first|1:saved"),
              "kill ring lost across prompts");
  PASS();
  TEST("One key callback is one atomic undo group");
  ASSERT_TRUE(run_pty_key_binding_case("orig\t\037!\r", SL_KEY_TAB,
                                       compound_edit_key, NULL, terminal,
                                       sizeof(terminal), result, sizeof(result),
                                       &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "orig!"),
              "callback undo was partial");
  PASS();
  TEST("A printable callback remains an independent undo group");
  ASSERT_TRUE(run_pty_key_binding_case("origq\037!\r", (sl_key_t)'q',
                                       compound_edit_key, NULL, terminal,
                                       sizeof(terminal), result, sizeof(result),
                                       &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "orig!"),
              "callback undo erased prior typing");
  PASS();
  TEST("A Ctrl-L callback remains an independent undo group");
  ASSERT_TRUE(run_pty_key_binding_case("orig\014\037!\r", SL_KEY_CTRL_L,
                                       compound_edit_key, NULL, terminal,
                                       sizeof(terminal), result, sizeof(result),
                                       &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "orig!"),
              "Ctrl-L callback undo was partial");
  PASS();
  TEST("PASS bindings preserve yank rotation");
  ASSERT_TRUE(run_pty_key_binding_case("one two\027\001\013\031\033y\r",
                                       SL_KEY_CTRL_Y, pass_action_key, NULL,
                                       terminal, sizeof(terminal), result,
                                       sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "two"),
              "PASS binding broke yank rotation");
  PASS();
  TEST("PASS bindings preserve consecutive kill order");
  ASSERT_TRUE(run_pty_key_binding_case(
                  "abc\ndef\002\002\002\002\002\002\002\013\013\031\r",
                  SL_KEY_CTRL_K, pass_action_key, NULL, terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "abc\ndef"),
              "PASS binding broke kill coalescing");
  PASS();
  TEST("Cancelling reverse search preserves draft undo");
  ASSERT_TRUE(run_pty_history_case("draft\022match\007\037!\r", history, 1,
                                   terminal, sizeof(terminal), result,
                                   sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "!"),
              "search cancellation lost undo");
  PASS();
  TEST("Accepting a history match starts a new undo context");
  ASSERT_TRUE(run_pty_history_case("draft\022match\r\037!\r", history, 1,
                                   terminal, sizeof(terminal), result,
                                   sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "matching history!"),
              "undo crossed history context");
  PASS();
  for (i = 9; i <= 10; i++) {
    size_t used = 0, j;
    TEST(i == 9 ? "Kill ring retains the ten newest independent kills"
                : "Kill ring rotation wraps after the tenth entry");
    for (j = 0; j < 12; j++)
      used += (size_t)snprintf(input + used, sizeof(input) - used,
                               "v%02lu\001\013\005", (unsigned long)j);
    input[used++] = '\031';
    for (j = 0; j < i; j++) {
      input[used++] = '\033';
      input[used++] = 'y';
    }
    memcpy(input + used, "!\r", 3);
    ASSERT_TRUE(run_pty_readline_case(input, 24, 0, NULL, terminal,
                                      sizeof(terminal), result, sizeof(result),
                                      &status) == 0 &&
                    WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                    !strcmp(result, i == 9 ? "v02!" : "v11!"),
                "kill ring eviction or rotation was wrong");
    PASS();
  }
  TEST("Ctrl-X bindings override the undo prefix");
  ASSERT_TRUE(run_pty_key_binding_case(
                  "a\030b\r", SL_KEY_CTRL_X, insert_text_key, "*", terminal,
                  sizeof(terminal), result, sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "a*b"),
              "Ctrl-X binding was bypassed");
  PASS();
  TEST("Alt-E retains priority over an unfinished Ctrl-X prefix");
  ASSERT_TRUE(run_pty_prompt_queue_case("first\tsecond\t\030\033e\r",
                                        SL_PROMPT_THEME_PLAIN, 1, 0, 2,
                                        terminal, sizeof(terminal), result,
                                        sizeof(result), &status) == 0 &&
                  WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  !strcmp(result, "1:second|2:first"),
              "prefix swallowed the reserved queue edit shortcut");
  PASS();
}

static void test_readline_keeps_normal_completion_with_queueing(void) {
  char terminal[4096];
  char result[256];
  struct vt_screen screen;
  int status;

  TEST("readline retains completion output when queueing is enabled");
  ASSERT_TRUE(run_pty_readline_completion_case(1, terminal, sizeof(terminal),
                                               result, sizeof(result),
                                               &status) == 0,
              "queued readline completion pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued readline completion child failed");
  ASSERT_TRUE(strcmp(result, "hello") == 0,
              "queued readline completion result mismatch");
  vt_init(&screen, 5, 20);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "p> hello"),
              "queued direct readline prompt was cleared");
  ASSERT_TRUE(vt_contains(&screen, "after"),
              "following output missing after queued direct readline");
  PASS();
}

struct resized_prompt_clear_state {
  int master_fd;
  int slave_fd;
  int fired;
  int status;
};

static size_t read_live_pty_output(int fd, char *bytes, size_t capacity);

static void test_print_above_pulls_stream_chunks(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  char terminal[4096];
  char result[256];
  size_t terminal_len;
  ssize_t n;
  int status;

  TEST("print_above pulls streamed chunks");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct text_stream_state stream;
    char calls[32];
    int written;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 20;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    stream.chunks[0] = "alpha";
    stream.chunks[1] = "-";
    stream.chunks[2] = "beta\n";
    stream.chunks[3] = NULL;
    stream.index = 0;
    stream.calls = 0;
    if (sl->print_above(sl, next_text_chunk, &stream) != SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (line)
      sl->free_string(sl, line);
    written = snprintf(calls, sizeof(calls), "%d", stream.calls);
    if (written > 0)
      (void)write(result_pipe[1], calls, (size_t)written);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "4") == 0, "stream callback count mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "alpha-beta"),
              "streamed chunks were not written");
  PASS();
}

static void test_word_wrap_keeps_words_intact(void) {
  char terminal[8192];
  char result[256];
  int status;

  TEST("word wrap reflows at word boundaries");
  ASSERT_TRUE(run_pty_readline_case("hello world sentence\r", 16, 0, NULL,
                                    terminal, sizeof(terminal), result,
                                    sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "hello world sentence") == 0,
              "word wrap result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "p> ") &&
                  contains_bytes(terminal, "hello world"),
              "first wrapped row missing");
  ASSERT_TRUE(contains_bytes(terminal, "   s") &&
                  contains_bytes(terminal, "entence"),
              "continuation word row missing");
  PASS();
}

static void test_word_wrap_cursor_at_skipped_space(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[8192];
  char buf[512];
  struct vt_screen screen;
  const char *input;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("word wrap preserves cursor at skipped space");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 12;
  ws.ws_row = 5;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  input = "word nextword\033[D\033[D\033[D\033[D\033[D"
          "\033[D\033[D\033[D\033[D";
  ASSERT_TRUE(write(master_fd, input, strlen(input)) == (ssize_t)strlen(input),
              "write input failed");
  tries = 0;
  while (!contains_bytes(terminal, "   nextword") && tries < 300) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  for (tries = 0; tries < 300; tries++) {
    vt_init(&screen, 5, 12);
    vt_apply(&screen, terminal);
    if (vt_contains(&screen, "p> word") &&
        vt_contains(&screen, "   nextword") && screen.row == 1 &&
        screen.col == 3)
      break;
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  vt_init(&screen, 5, 12);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "p> word"), "first wrapped row missing");
  ASSERT_TRUE(vt_contains(&screen, "   nextword"),
              "continuation wrapped row missing");
  ASSERT_TRUE(screen.row == 1, "cursor row did not stay on wrapped row");
  ASSERT_TRUE(screen.col == 3, "cursor column did not stay at wrap boundary");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], buf, sizeof(buf));
  ASSERT_TRUE(n > 0, "read result failed");
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  PASS();
}

static void test_active_word_wrap_does_not_duplicate_prompt(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[16384];
  char buf[512];
  struct vt_screen screen;
  const char *input;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("active word wrap does not duplicate prompt");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 80;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "softline> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "softline> ")) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  input = "hello world jspdi jsdip jfjpisd rjfsdpi fpisdmjfpisi "
          "jpdstjfjpisjdfpijj";
  ASSERT_TRUE(write(master_fd, input, strlen(input)) == (ssize_t)strlen(input),
              "write input failed");
  tries = 0;
  while (tries < 300) {
    vt_init(&screen, 8, 80);
    vt_apply(&screen, terminal);
    if (vt_contains(&screen, "          jpdstjfjpisjdfpijj"))
      break;
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  vt_init(&screen, 8, 80);
  vt_apply(&screen, terminal);
  if (!vt_contains(&screen, "          jpdstjfjpisjdfpijj"))
    vt_dump(&screen);
  ASSERT_TRUE(vt_count(&screen, "softline>") == 1,
              "active wrap duplicated prompt");
  ASSERT_TRUE(vt_contains(&screen, "softline> hello world jspdi jsdip"),
              "first prompt row missing");
  ASSERT_TRUE(vt_contains(&screen, "          jpdstjfjpisjdfpijj"),
              "continuation row missing prompt indent");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], buf, sizeof(buf));
  ASSERT_TRUE(n > 0, "read result failed");
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  PASS();
}

static void test_active_exact_width_row_does_not_autowrap_prompt(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[16384];
  char buf[512];
  struct vt_screen screen;
  const char *input;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("active exact-width row does not autowrap prompt");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 6;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "softline> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "softline> ")) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  input = "aaaaa bbbbb ccccc ddddd eeeeee fffff ggggg";
  ASSERT_TRUE(write(master_fd, input, strlen(input)) == (ssize_t)strlen(input),
              "write input failed");
  tries = 0;
  while (!contains_bytes(terminal, "ggggg") && tries < 300) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  vt_init(&screen, 6, 40);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_count(&screen, "softline>") == 1,
              "exact-width render duplicated prompt");
  ASSERT_TRUE(vt_contains(&screen, "softline> aaaaa bbbbb ccccc ddddd eeeeee"),
              "first exact-width row missing");
  ASSERT_TRUE(vt_contains(&screen, "          fffff ggggg"),
              "continuation after exact-width row missing");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], buf, sizeof(buf));
  ASSERT_TRUE(n > 0, "read result failed");
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  PASS();
}

static void test_normal_prompt_wraps_at_bottom_with_long_prompt(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[8192];
  char result[256];
  char buf[512];
  const char *input;
  struct vt_screen screen;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("normal prompt wraps at bottom with long prompt");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 6;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    if (write(slave_fd, "1\r\n2\r\n3\r\n4\r\n5\r\n", 15) != 15)
      _exit(2);
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(3);
    line = sl->readline(sl, "softline> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "softline> ")) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  input = "dj jpsidjfpisj sdpijfijfa pidspifjxjp nextword\r";
  ASSERT_TRUE(write(master_fd, input, strlen(input)) == (ssize_t)strlen(input),
              "write input failed");
  tries = 0;
  while (!contains_bytes(terminal, "pidspifjxjp nextword") && tries < 300) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(
      strcmp(result, "dj jpsidjfpisj sdpijfijfa pidspifjxjp nextword") == 0,
      "wrapped prompt result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "softline> dj jpsidjfpisj sdpijfijfa"),
              "first prompt row missing");
  ASSERT_TRUE(contains_bytes(terminal, "pidspifjxjp nextword"),
              "continuation word run missing");
  ASSERT_TRUE(!contains_bytes(terminal, "sdpijfijfa pidsp"),
              "word was split across wrap");
  vt_init(&screen, 6, 40);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "softline> dj jpsidjfpisj sdpijfijfa"),
              "final screen first prompt row missing");
  ASSERT_TRUE(vt_contains(&screen, "          pidspifjxjp nextword"),
              "final screen continuation row missing");
  ASSERT_TRUE(!vt_contains(&screen, "sdpijfijfa pidsp"),
              "final screen split a word across wrap");
  PASS();
}

static void test_normal_prompt_growth_scrolls_at_screen_bottom(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[8192];
  char result[256];
  char buf[512];
  struct vt_screen screen;
  const char *input;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("normal prompt growth within viewport scrolls at screen bottom");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 16;
  ws.ws_row = 6;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    if (write(slave_fd, "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n", 29) != 29)
      _exit(2);
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  input = "hello world sentence";
  ASSERT_TRUE(write(master_fd, input, strlen(input)) == (ssize_t)strlen(input),
              "write input failed");
  tries = 0;
  while (tries < 300) {
    vt_init(&screen, 6, 16);
    vt_apply(&screen, terminal);
    if (vt_contains(&screen, "p> hello world") &&
        vt_contains(&screen, "   sentence"))
      break;
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  vt_init(&screen, 6, 16);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "p> hello world"),
              "bottom growth first row missing");
  ASSERT_TRUE(vt_contains(&screen, "   sentence"),
              "bottom growth continuation row missing");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "hello world sentence") == 0,
              "bottom growth result mismatch");
  PASS();
}

static void test_resize_reflows_without_keypress(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[8192];
  char result[256];
  char buf[512];
  struct vt_screen screen;
  struct vt_screen before_resize;
  size_t terminal_len;
  size_t resize_offset;
  ssize_t n;
  int status;
  int tries;

  TEST("resize reflows while readline is idle");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 20;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 0;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    if (write(slave_fd, "\033[20;1H", 7) != 7)
      _exit(2);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  ASSERT_TRUE(write(master_fd, "hello world sentence", 20) == 20,
              "write input failed");
  tries = 0;
  while (tries < 100) {
    vt_init(&screen, 20, 40);
    vt_apply(&screen, terminal);
    if (vt_contains(&screen, "p> hello world sentence"))
      break;
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  before_resize = screen;
  ASSERT_TRUE(vt_contains(&screen, "p> hello world sentence"),
              "wide render missing");
  ws.ws_col = 16;
  ws.ws_row = 20;
  resize_offset = terminal_len;
  ASSERT_TRUE(ioctl(master_fd, TIOCSWINSZ, &ws) == 0, "resize ioctl failed");
  tries = 0;
  while (tries < 100) {
    vt_init(&screen, 20, 16);
    vt_apply(&screen, terminal + resize_offset);
    if (vt_contains(&screen, "p> hello world") &&
        vt_contains(&screen, "   sentence"))
      break;
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  n = (ssize_t)read_live_pty_output(master_fd, buf, sizeof(buf));
  if (n > 0)
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  screen = before_resize;
  screen.cols = 16;
  for (tries = 0; tries < screen.rows; tries++)
    screen.cells[tries][screen.cols] = '\0';
  vt_apply(&screen, terminal + resize_offset);
  ASSERT_TRUE(vt_contains(&screen, "p> hello world"),
              "idle resize did not reflow before input");
  ASSERT_TRUE(vt_contains(&screen, "   sentence"),
              "idle resize continuation was not rendered before input");
  /* LF between existing prompt rows is not a scroll. Assert the resulting
   * screen and history rather than rejecting the control byte itself. */
  ASSERT_TRUE(screen.history_count == 0 &&
                  strncmp(screen.cells[18], "p> hello world", 14) == 0 &&
                  strncmp(screen.cells[19], "   sentence     ", 16) == 0,
              "idle resize displaced or left stale prompt cells");
  ws.ws_col = 40;
  resize_offset = terminal_len;
  ASSERT_TRUE(ioctl(master_fd, TIOCSWINSZ, &ws) == 0, "expand ioctl failed");
  for (tries = 0; tries < 100; tries++) {
    if (contains_bytes(terminal + resize_offset, "p> hello world sentence"))
      break;
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  ASSERT_TRUE(tries < 100, "idle expansion did not redraw prompt");
  n = (ssize_t)read_live_pty_output(master_fd, buf, sizeof(buf));
  if (n > 0)
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  for (tries = 0; tries < screen.rows; tries++) {
    memset(screen.cells[tries] + 16, ' ', 24);
    screen.cells[tries][40] = '\0';
  }
  screen.cols = 40;
  vt_apply(&screen, terminal + resize_offset);
  ASSERT_TRUE(screen.history_count == 0 &&
                  strncmp(screen.cells[18], "p> hello world sentence", 23) ==
                      0 &&
                  strncmp(screen.cells[19], "                ", 16) == 0,
              "idle expansion scrolled or retained an extra prompt row");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "hello world sentence") == 0,
              "resize result mismatch");
  PASS();
}

static void test_visual_up_moves_across_wrapped_rows(void) {
  char terminal[8192];
  char result[256];
  int status;

  TEST("up arrow moves across wrapped visual rows");
  ASSERT_TRUE(run_pty_readline_case("hello world sentence\033[AX\r", 16, 0,
                                    NULL, terminal, sizeof(terminal), result,
                                    sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "hello woXrld sentence") == 0,
              "visual up insertion mismatch");
  PASS();
}

struct begin_failure_probe {
  int fd;
  int fired;
  int result;
  int retained_raw;
};

static void begin_failure_idle(sl_t *sl, void *userdata) {
  struct begin_failure_probe *probe = (struct begin_failure_probe *)userdata;
  struct termios before, after;
  struct winsize small, previous;
  if (probe->fired)
    return;
  probe->fired = 1;
  if (tcgetattr(probe->fd, &before) != 0 ||
      ioctl(probe->fd, TIOCGWINSZ, &previous) != 0)
    return;
  small = previous;
  small.ws_row = 2;
  if (ioctl(probe->fd, TIOCSWINSZ, &small) != 0)
    return;
  probe->result = sl_output_stream_begin(sl);
  probe->retained_raw = tcgetattr(probe->fd, &after) == 0 &&
                        termios_same_observable(&before, &after) &&
                        !(after.c_lflag & (ICANON | ECHO));
  (void)ioctl(probe->fd, TIOCSWINSZ, &previous);
  (void)sl_cancel(sl);
}

static void test_native_begin_failure_restores_termios(void) {
  int master, slave, readonly_fd, result, restored;
  sl_config_t config;
  sl_t *sl;
  struct termios before, after;
  struct winsize size;
  struct begin_failure_probe probe;
  TEST("failed native begin restores newly acquired raw mode");
  memset(&size, 0, sizeof(size));
  size.ws_col = 20;
  size.ws_row = 8;
  ASSERT_TRUE(openpty(&master, &slave, NULL, NULL, &size) == 0,
              "openpty failed");
  readonly_fd = open(ttyname(slave), O_RDONLY | O_NOCTTY);
  ASSERT_TRUE(readonly_fd >= 0 && tcgetattr(slave, &before) == 0,
              "read-only output setup failed");
  sl_config_init(&config);
  config.input_fd = slave;
  config.output_fd = readonly_fd;
  sl = sl_create_with_config(&config);
  ASSERT_TRUE(sl != NULL, "sl_create failed");
  result = sl_output_stream_begin(sl);
  restored =
      tcgetattr(slave, &after) == 0 && termios_same_observable(&before, &after);
  sl_destroy(sl);
  close(readonly_fd);
  close(slave);
  close(master);
  ASSERT_TRUE(result == SL_ERROR_IO && restored,
              "failed native begin left input in raw mode");
  PASS();

  TEST("failed native begin preserves raw mode owned by readline");
  ASSERT_TRUE(openpty(&master, &slave, NULL, NULL, &size) == 0,
              "openpty failed");
  sl_config_init(&config);
  config.input_fd = slave;
  config.output_fd = slave;
  sl = sl_create_with_config(&config);
  memset(&probe, 0, sizeof(probe));
  probe.fd = slave;
  ASSERT_TRUE(sl &&
                  sl_set_idle_callback(sl, begin_failure_idle, &probe) == SL_OK,
              "begin failure callback setup failed");
  ASSERT_TRUE(sl_readline(sl, "> ") == NULL && probe.fired &&
                  probe.result == SL_ERROR_IO && probe.retained_raw,
              "failed begin released the editor's raw mode");
  sl_destroy(sl);
  close(slave);
  close(master);
  PASS();
}

static void test_ctrl_c_interrupts_child(int native) {
  int master_fd;
  int slave_fd;
  pid_t pid;
  int status;
  char terminal[512];
  size_t terminal_len;
  ssize_t n;
  int tries;

  TEST(native ? "native Ctrl-C restores margins before terminating"
              : "Ctrl-C interrupts active readline");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    (void)setsid();
    (void)ioctl(slave_fd, TIOCSCTTY, 0);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl || (native && sl_output_stream_begin(sl) != SL_OK))
      _exit(2);
    line = sl->readline(sl, "p> ");
    if (line)
      sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    _exit(0);
  }
  close(slave_fd);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "\003", 1) == 1, "write Ctrl-C failed");
  tries = 0;
  do {
    n = waitpid(pid, &status, WNOHANG);
    if (n == 0)
      usleep(10000);
    tries++;
  } while (n == 0 && tries < 200);
  ASSERT_TRUE(n == pid, "child did not exit after Ctrl-C");
  ASSERT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT,
              "child was not interrupted by SIGINT");
  while (terminal_len + 1 < sizeof(terminal) &&
         (n = read_some_with_timeout(master_fd, terminal + terminal_len,
                                     sizeof(terminal) - 1 - terminal_len)) > 0)
    terminal_len += (size_t)n;
  terminal[terminal_len] = '\0';
  close(master_fd);
  if (native) {
    const char *reset = strstr(terminal, "\033[r");
    ASSERT_TRUE(reset && strstr(reset, "\033[65535;1H"),
                "native SIGINT left restricted margins or a homed cursor");
  }
  PASS();
}

static void test_ctrl_c_signal_is_not_delivered_twice(int native) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  int status;
  char terminal[512];
  char result[128];
  size_t terminal_len;
  ssize_t n;
  int tries;

  TEST(native ? "native Ctrl-C restores margins before a returning handler"
              : "Ctrl-C is raised once after raw cleanup");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct termios before;
    struct termios after;
    char msg[128];
    int restored;
    int readline_status;
    int written;

    close(master_fd);
    close(result_pipe[0]);
    (void)setsid();
    (void)ioctl(slave_fd, TIOCSCTTY, 0);
    if (tcgetattr(slave_fd, &before) != 0)
      _exit(2);
#if defined(TABDLY) && defined(TAB3)
    if (native) {
      before.c_oflag = (before.c_oflag & ~TABDLY) | TAB3 | OPOST;
      if (tcsetattr(slave_fd, TCSANOW, &before) != 0)
        _exit(2);
    }
#endif
    ctrl_c_sigint_count = 0;
    ctrl_c_signal_marker_fd = native ? slave_fd : -1;
    if (signal(SIGINT, count_ctrl_c_sigint) == SIG_ERR)
      _exit(3);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl || (native && sl_output_stream_begin(sl) != SL_OK))
      _exit(4);
    line = sl->readline(sl, "p> ");
    if (line)
      sl->free_string(sl, line);
    readline_status = (int)sl->last_readline_status(sl);
    if (tcgetattr(slave_fd, &after) != 0)
      _exit(5);
    restored = termios_same_observable(&before, &after);
    if (native &&
        (sl_output_stream_write(sl, "AFTER\tINTERRUPT\n", 16) != SL_OK ||
         tcgetattr(slave_fd, &after) != 0 ||
         !termios_same_observable(&before, &after) ||
         sl_output_stream_end(sl) != SL_OK))
      _exit(6);
    sl->destroy(sl);
    written = snprintf(msg, sizeof(msg), "%d %d %d", (int)ctrl_c_sigint_count,
                       restored, readline_status);
    if (written > 0)
      (void)write(result_pipe[1], msg, (size_t)written);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "p> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "prompt was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "\003", 1) == 1, "write Ctrl-C failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  tries = 0;
  do {
    n = waitpid(pid, &status, WNOHANG);
    if (n == 0)
      usleep(10000);
    tries++;
  } while (n == 0 && tries < 200);
  close(result_pipe[0]);
  ASSERT_TRUE(n == pid, "child did not exit after Ctrl-C");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "1 1 4") == 0,
              "Ctrl-C signal count or terminal cleanup mismatch");
  while (terminal_len + 1 < sizeof(terminal) &&
         (n = read_some_with_timeout(master_fd, terminal + terminal_len,
                                     sizeof(terminal) - 1 - terminal_len)) > 0)
    terminal_len += (size_t)n;
  terminal[terminal_len] = '\0';
  close(master_fd);
  if (native) {
    const char *marker = strstr(terminal, "SIGNAL_MARKER");
    const char *reset = strstr(terminal, "\033[r");
    ASSERT_TRUE(marker && reset && reset < marker &&
                    strstr(marker, "AFTER\tINTERRUPT"),
                "native continuation expanded tabs or preceded cleanup");
  }
  PASS();
}

static void test_idle_callback_can_submit_without_input(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  char result[128];
  ssize_t n;
  int status;

  TEST("idle callback can submit without input");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_finish_state state;

    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    state.calls = 0;
    state.text = "idle-result";
    state.cancel = 0;
    if (sl->set_idle_callback(sl, idle_finish_after_two_ticks, &state) != SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line)
      (void)write(result_pipe[1], "<NULL>", 6);
    else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "idle submit did not return");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "idle-result") == 0, "idle submit mismatch");
  PASS();
}

static void test_idle_callback_runs_with_quiet_watch(void) {
  int master_fd;
  int slave_fd;
  int quiet_pipe[2];
  int result_pipe[2];
  pid_t pid;
  char result[128];
  ssize_t n;
  int status;

  TEST("idle callback runs while an external watch is quiet");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(quiet_pipe) == 0, "quiet pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    char *line;
    struct idle_finish_state state;

    close(master_fd);
    close(quiet_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    state.calls = 0;
    state.text = "idle-watch-result";
    state.cancel = 0;
    if (sl->set_idle_callback(sl, idle_finish_after_two_ticks, &state) != SL_OK)
      _exit(3);
    watch_id = 0;
    if (sl->watch_add(sl, quiet_pipe[0], SL_WATCH_READ,
                      idle_quiet_watch_callback, NULL, &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(4);
    line = sl->readline(sl, "p> ");
    if (!line)
      (void)write(result_pipe[1], "<NULL>", 6);
    else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(quiet_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(quiet_pipe[0]);
  close(result_pipe[1]);
  n = read_some_with_timeout_ms(result_pipe[0], result, sizeof(result) - 1,
                                1000);
  ASSERT_TRUE(n > 0, "idle callback was blocked by quiet watch");
  result[n] = '\0';
  close(master_fd);
  close(quiet_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "idle-watch-result") == 0,
              "idle callback result mismatch with quiet watch");
  PASS();
}

static void test_busy_spinner_ticks_with_quiet_watch(void) {
  int master_fd;
  int slave_fd;
  int quiet_pipe[2];
  int result_pipe[2];
  int status;
  int spinner_advanced;
  pid_t pid;
  char result[128];
  char terminal[8192];
  size_t terminal_len;
  ssize_t n;
  struct timeval spinner_started;

  TEST("busy spinner ticks while an external watch is quiet");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(quiet_pipe) == 0, "quiet pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    char *line;
    close(master_fd);
    close(quiet_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.statusline = 1;
    cfg.status_spinner = 1;
    cfg.status_busy = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    watch_id = 0;
    if (sl_watch_add(sl, quiet_pipe[0], SL_WATCH_READ,
                     idle_quiet_watch_callback, NULL, &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(3);
    line = sl_readline(sl, "spin> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave_fd);
    close(quiet_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(quiet_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "spin> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "spinner prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  spinner_advanced = 0;
  ASSERT_TRUE(gettimeofday(&spinner_started, NULL) == 0,
              "spinner clock read failed");
  while (!spinner_advanced) {
    struct timeval now;
    long elapsed_ms;
    long remaining_ms;

    ASSERT_TRUE(gettimeofday(&now, NULL) == 0, "spinner clock read failed");
    elapsed_ms = (long)(now.tv_sec - spinner_started.tv_sec) * 1000L +
                 (long)(now.tv_usec - spinner_started.tv_usec) / 1000L;
    if (elapsed_ms >= 1500L)
      break;
    remaining_ms = 1500L - elapsed_ms;
    if (remaining_ms > 100L)
      remaining_ms = 100L;
    n = read_some_with_timeout_ms(master_fd, terminal + terminal_len,
                                  sizeof(terminal) - 1 - terminal_len,
                                  remaining_ms);
    if (n > 0) {
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
    if (contains_bytes(terminal, "- ")) {
      spinner_advanced = 1;
    }
  }
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "spinner submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "spinner result missing");
  result[n] = '\0';
  close(master_fd);
  close(quiet_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "spinner child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "spinner child failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0, "spinner result mismatch");
  ASSERT_TRUE(spinner_advanced, "quiet watch froze the busy spinner");
  PASS();
}

static void test_idle_callback_can_cancel_without_input(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  char result[128];
  ssize_t n;
  int status;

  TEST("idle callback can cancel without input");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_finish_state state;

    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    state.calls = 0;
    state.text = NULL;
    state.cancel = 1;
    if (sl->set_idle_callback(sl, idle_finish_after_two_ticks, &state) != SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line) {
      if (sl->last_readline_status(sl) == SL_READLINE_CANCELLED)
        (void)write(result_pipe[1], "<NULL>|CANCELLED", 16);
      else
        (void)write(result_pipe[1], "<NULL>", 6);
    } else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "idle cancel did not return");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "<NULL>|CANCELLED") == 0,
              "idle cancel status mismatch");
  PASS();
}

static void test_final_render_failure_reports_error(void) {
  int master_fd;
  int slave_fd;
  int output_master_fd;
  int output_slave_fd;
  int result_pipe[2];
  pid_t pid;
  char terminal[256];
  char result[128];
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("final render failure reports readline error");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(openpty(&output_master_fd, &output_slave_fd, NULL, NULL, NULL) ==
                  0,
              "output pty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    int readline_status;
    char msg[64];
    int written;

    close(master_fd);
    close(output_master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = output_slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    line = sl->readline(sl, "p> ");
    readline_status = (int)sl->last_readline_status(sl);
    if (line) {
      written = snprintf(msg, sizeof(msg), "%s|%d", line, readline_status);
      sl->free_string(sl, line);
    } else {
      written = snprintf(msg, sizeof(msg), "<NULL>|%d", readline_status);
    }
    if (written > 0)
      (void)write(result_pipe[1], msg, (size_t)written);
    sl->destroy(sl);
    close(slave_fd);
    close(output_slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(output_slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  tries = 0;
  while (!contains_bytes(terminal, "p> ") && tries < 100) {
    n = read_some_with_timeout(output_master_fd, result, sizeof(result));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), result,
                            n);
    tries++;
  }
  ASSERT_TRUE(contains_bytes(terminal, "p> "), "initial prompt not rendered");
  close(output_master_fd);
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "<NULL>|5") == 0,
              "readline did not report final render error");
  PASS();
}

static void test_narrow_terminal_does_not_submit_before_enter(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  int ready_pipe[2];
  pid_t pid;
  char input[121];
  char result[1024];
  ssize_t n;
  int status;
  int i;

  TEST("narrow terminal does not submit before Enter");
  for (i = 0; i < 120; i++)
    input[i] = 'x';
  input[120] = '\0';
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  ASSERT_TRUE(pipe(ready_pipe) == 0, "ready pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_ready_state ready;

    close(master_fd);
    close(result_pipe[0]);
    close(ready_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 1;
    cfg.line_max_len = 120;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(3);
    ready.fd = ready_pipe[1];
    ready.ready = 0;
    if (sl->set_idle_callback(sl, idle_signal_ready_once, &ready) != SL_OK)
      _exit(4);
    line = sl->readline(sl, "p> ");
    if (!line)
      (void)write(result_pipe[1], "<NULL>", 6);
    else {
      (void)write(result_pipe[1], line, strlen(line));
      sl->free_string(sl, line);
    }
    sl->destroy(sl);
    close(slave_fd);
    close(ready_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  close(ready_pipe[1]);
  n = read_some_with_timeout(ready_pipe[0], result, sizeof(result));
  ASSERT_TRUE(n == 1, "readline did not become idle");
  ASSERT_TRUE(write(master_fd, input, strlen(input)) == (ssize_t)strlen(input),
              "write input failed");
  for (i = 0; i < 50; i++) {
    n = read_some_with_timeout_ms(result_pipe[0], result, sizeof(result) - 1,
                                  20);
    ASSERT_TRUE(n == 0, "readline returned before Enter");
  }
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  close(ready_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strlen(result) == strlen(input), "submitted length mismatch");
  ASSERT_TRUE(strcmp(result, input) == 0, "submitted input mismatch");
  PASS();
}

static void test_live_output_end_restores_unbounded_cursor(void) {
  int master_fd;
  int slave_fd;
  int ready_pipe[2];
  int result_pipe[2];
  pid_t pid;
  char terminal[4096];
  char buf[512];
  char ready;
  const char *last_hide;
  const char *last_show;
  const char *next;
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("ending unbounded live output restores the active editor cursor");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0 &&
                  pipe(ready_pipe) == 0 && pipe(result_pipe) == 0,
              "pty or pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_stream_end_state state;
    close(master_fd);
    close(ready_pipe[0]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    state.fd = ready_pipe[1];
    state.attempted = 0;
    if (sl_set_idle_callback(sl, idle_end_live_stream_once, &state) != SL_OK)
      _exit(3);
    line = sl_readline(sl, "p> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave_fd);
    close(ready_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(ready_pipe[1]);
  close(result_pipe[1]);
  ASSERT_TRUE(read_some_with_timeout(ready_pipe[0], &ready, 1) == 1 &&
                  ready == 'R',
              "idle stream did not finish");
  terminal_len = 0;
  terminal[0] = '\0';
  for (tries = 0; tries < 100 && terminal_len < sizeof(terminal) - 1; tries++) {
    n = read_some_with_timeout_ms(master_fd, buf, sizeof(buf), 20);
    if (n <= 0)
      break;
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  }
  ASSERT_TRUE(contains_bytes(terminal, "notice") &&
                  contains_bytes(terminal, "p> "),
              "stream or prompt output missing");
  last_hide = NULL;
  last_show = NULL;
  next = terminal;
  while ((next = strstr(next, "\033[?25l")) != NULL) {
    last_hide = next;
    next++;
  }
  next = terminal;
  while ((next = strstr(next, "\033[?25h")) != NULL) {
    last_show = next;
    next++;
  }
  ASSERT_TRUE(!last_hide || (last_show && last_show > last_hide),
              "stream end left the editor cursor hidden");
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_some_with_timeout(result_pipe[0], buf, sizeof(buf) - 1);
  ASSERT_TRUE(n == 2 && memcmp(buf, "ok", 2) == 0, "editor result mismatch");
  close(master_fd);
  close(ready_pipe[0]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "child editor failed");
  PASS();
}

static void test_unbounded_finite_output_preserves_active_prompt(void) {
  struct winsize ws;
  struct vt_screen screen;
  int master_fd;
  int slave_fd;
  int ready_pipe[2];
  int result_pipe[2];
  pid_t pid;
  char terminal[8192];
  char result[16];
  char ready;
  ssize_t n;
  int status;

  TEST("unbounded finite output preserves an active editor between sessions");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0 &&
                  pipe(ready_pipe) == 0 && pipe(result_pipe) == 0,
              "pty or pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    struct idle_output_boundary_state state;
    char *line;
    close(master_fd);
    close(ready_pipe[0]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    state.fd = ready_pipe[1];
    state.attempted = 0;
    if (sl_set_idle_callback(sl, idle_output_boundary_once, &state) != SL_OK)
      _exit(3);
    line = sl_readline(sl, "p> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave_fd);
    close(ready_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(ready_pipe[1]);
  close(result_pipe[1]);
  ASSERT_TRUE(read_some_with_timeout(ready_pipe[0], &ready, 1) == 1 &&
                  ready == 'R',
              "active editor output sequence failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, terminal, sizeof(terminal)) > 0,
              "active editor output missing");
  vt_init(&screen, 8, 30);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "first") && vt_contains(&screen, "middle") &&
                  vt_contains(&screen, "last") && vt_contains(&screen, "p> "),
              "finite output erased transcript or active prompt");
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result));
  ASSERT_TRUE(n == 2 && memcmp(result, "ok", 2) == 0,
              "active editor lost its input after finite output");
  close(master_fd);
  close(ready_pipe[0]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "active editor child failed");
  PASS();
}

static void test_idle_callback_prints_above_active_prompt(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  char terminal[4096];
  char result[256];
  char buf[512];
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;
  struct winsize ws;

  TEST("unbounded prompts clear and redraw live output by default");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 1;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_print_state state;
    close(master_fd);
    close(result_pipe[0]);
    state.printed = 0;
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.screen_width = 20;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    sl->set_idle_callback(sl, idle_print_once, &state);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(3);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  tries = 0;
  while (!contains_bytes(terminal, "idle-output") && tries < 100) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  ASSERT_TRUE(contains_bytes(terminal, "idle-output"),
              "idle output was not printed");
  ASSERT_TRUE(contains_bytes(terminal, "p> "), "prompt was not redrawn");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[6n"),
              "default live output requested a cursor-position probe");
  ASSERT_TRUE(!contains_bytes(terminal, "\033[1;"),
              "default live output entered a scroll region");
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0, "idle callback result mismatch");
  PASS();
}

static void test_normal_prompt_pins_at_bottom_for_live_output(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  struct vt_screen screen;
  char terminal[8192];
  char result[64];
  char buf[512];
  size_t terminal_len;
  ssize_t n;
  int status;
  int replied;
  int tries;
  int prompts_after_second;
  int first_row;
  int second_row;
  int queue_row;
  int prompt_row;
  int i;
  size_t resize_offset;

  TEST("normal prompt pins at bottom for live output");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 5;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_scroll_region_state state;
    close(master_fd);
    close(result_pipe[0]);
    state.calls = 0;
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    cfg.live_scroll_region = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    if (sl->set_idle_callback(sl, idle_print_through_scroll_region, &state) !=
        SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  replied = 0;
  tries = 0;
  while (!contains_bytes(terminal, "first-live") && tries < 100) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    if (!replied && contains_bytes(terminal, "\033[6n")) {
      ASSERT_TRUE(write(master_fd, "\033\033[5;4R", 7) == 7,
                  "nested Escape and cursor report write failed");
      replied = 1;
    }
    tries++;
  }
  ASSERT_TRUE(replied, "normal prompt did not request cursor position");
  ASSERT_TRUE(contains_bytes(terminal, "first-live"),
              "first live message missing");
  ASSERT_TRUE(count_bytes(terminal, "p> ") == 1,
              "first live message repainted the prompt");
  ASSERT_TRUE(contains_bytes(terminal, "\033[1;4r"),
              "bottom prompt did not enter scroll region");
  usleep(150000);
  ASSERT_TRUE(write(master_fd, "queued\tok", 9) == 9,
              "queue-and-text write failed");
  tries = 0;
  while (!contains_bytes(terminal, "second-live") && tries < 100) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  ASSERT_TRUE(contains_bytes(terminal, "Q 1. queued"),
              "queued prompt did not reflow the pinned region");
  ASSERT_TRUE(contains_bytes(terminal, "\033[1;3r"),
              "scroll region did not shrink after queue reflow");
  ASSERT_TRUE(contains_bytes(terminal, "second-live"),
              "second live message missing");
  prompts_after_second = count_bytes(terminal, "p> ");
  n = read_some_with_timeout(master_fd, buf, sizeof(buf));
  if (n > 0)
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  ASSERT_TRUE(count_bytes(terminal, "p> ") == prompts_after_second,
              "live message repainted the reflowed prompt");
  resize_offset = terminal_len;
  ws.ws_row = 6;
  ASSERT_TRUE(ioctl(master_fd, TIOCSWINSZ, &ws) == 0, "terminal resize failed");
  ASSERT_TRUE(write(master_fd, "\014x\177", 3) == 3,
              "pinned reset and resize input failed");
  n = read_some_with_timeout(master_fd, buf, sizeof(buf));
  ASSERT_TRUE(n > 0, "resize did not reflow pinned prompt");
  append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  n = (ssize_t)read_live_pty_output(master_fd, buf, sizeof(buf));
  if (n > 0)
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  ASSERT_TRUE(!contains_bytes(terminal + resize_offset, "\033[1;1H\033[2K") &&
                  !contains_bytes(terminal + resize_offset, "\033[2;1H\033[2K"),
              "pinned resize cleared transcript rows");
  ASSERT_TRUE(!contains_bytes(terminal + resize_offset, "\n"),
              "pinned resize advanced the terminal scrollback");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "read result failed");
  result[n] = '\0';
  do {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
  } while (n > 0 && terminal_len < sizeof(terminal) - 1);
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0, "pinned prompt result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "\033[r"),
              "scroll region was not restored");
  vt_init(&screen, 6, 20);
  screen.row = 4;
  screen.col = 0;
  vt_apply(&screen, terminal);
  first_row = -1;
  second_row = -1;
  queue_row = -1;
  prompt_row = -1;
  for (i = 0; i < screen.rows; i++) {
    if (strstr(screen.cells[i], "first-live"))
      first_row = i;
    if (strstr(screen.cells[i], "second-live"))
      second_row = i;
    if (strstr(screen.cells[i], "Q 1. queued"))
      queue_row = i;
    if (strstr(screen.cells[i], "p> ok"))
      prompt_row = i;
  }
  ASSERT_TRUE(first_row >= 0 && second_row == first_row + 1 && queue_row >= 0 &&
                  prompt_row == queue_row + 1,
              "live transcript content was overwritten or on the wrong row");
  PASS();
}

static void test_pinned_stream_failure_restores_prompt_cursor(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  int ready_pipe[2];
  pid_t pid;
  struct winsize ws;
  struct vt_screen screen;
  char terminal[4096];
  char result[64];
  char buf[512];
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("pinned stream failure restores prompt cursor before editing");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 5;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  ASSERT_TRUE(pipe(ready_pipe) == 0, "ready pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_stream_failure_state state;
    close(master_fd);
    close(result_pipe[0]);
    close(ready_pipe[0]);
    memset(&state, 0, sizeof(state));
    state.fd = ready_pipe[1];
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.live_scroll_region = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    if (sl->set_idle_callback(sl, idle_print_failure_once, &state) != SL_OK)
      _exit(3);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(ready_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  close(ready_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  tries = 0;
  while (!contains_bytes(terminal, "\033[6n") && tries < 100) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  ASSERT_TRUE(contains_bytes(terminal, "\033[6n"),
              "pinned stream did not request cursor position");
  ASSERT_TRUE(write(master_fd, "\033[5;4R", 6) == 6,
              "cursor report write failed");
  n = read_some_with_timeout(ready_pipe[0], result, sizeof(result));
  ASSERT_TRUE(n == 1 && result[0] == 'R',
              "failing stream did not report its callback error");
  ASSERT_TRUE(write(master_fd, "x", 1) == 1, "edit write failed");
  tries = 0;
  while (!contains_bytes(terminal, "x") && tries < 100) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  ASSERT_TRUE(contains_bytes(terminal, "x"), "edited prompt was not rendered");
  vt_init(&screen, 5, 20);
  screen.row = 4;
  screen.col = 0;
  vt_apply(&screen, terminal);
  ASSERT_TRUE(strstr(screen.cells[4], "p> x") != NULL,
              "edit after failed stream was not placed on the prompt row");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n == 1, "read result failed");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  close(ready_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "pinned stream failure child failed");
  ASSERT_TRUE(strcmp(result, "x") == 0,
              "pinned stream failure result mismatch");
  PASS();
}

static void test_cursor_probe_preserves_concurrent_queue_input(void) {
  static const char input[] =
      "\033[9999999999;9999999999R"
      "queued-012345678901234567890123456789012345\tok\033[13;5u";
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  pid_t pid;
  struct winsize ws;
  char terminal[4096];
  char result[64];
  char buf[512];
  size_t terminal_len;
  ssize_t n;
  int status;
  int tries;

  TEST("cursor probe preserves queued input and Ctrl-Enter");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 5;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    struct idle_print_state state;
    close(master_fd);
    close(result_pipe[0]);
    state.printed = 0;
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    cfg.live_scroll_region = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    if (sl->bind_key(sl, SL_KEY_CTRL_ENTER, submit_method_key, NULL) != SL_OK)
      _exit(3);
    if (sl->set_idle_callback(sl, idle_print_once, &state) != SL_OK)
      _exit(4);
    line = sl->readline(sl, "p> ");
    if (!line)
      _exit(5);
    (void)write(result_pipe[1], line, strlen(line));
    sl->free_string(sl, line);
    sl->destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  tries = 0;
  while (!contains_bytes(terminal, "\033[6n") && tries < 100) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    tries++;
  }
  ASSERT_TRUE(contains_bytes(terminal, "\033[6n"),
              "cursor-position probe was not requested");
  ASSERT_TRUE(
      write(master_fd, input, sizeof(input) - 1) ==
          (ssize_t)(sizeof(input) - 1),
      "concurrent malformed cursor report and queue input write failed");
  tries = 0;
  n = 0;
  while (n == 0 && tries < 50) {
    n = read_some_with_timeout(master_fd, buf, sizeof(buf));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), buf, n);
    n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
    tries++;
  }
  ASSERT_TRUE(n > 0, "concurrent queue input did not complete");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "waitpid failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "ok") == 0,
              "cursor probe did not preserve queued Ctrl-Enter input");
  PASS();
}

static void test_utf8_input_and_backspace(void) {
  char terminal[4096];
  char result[256];
  int status;

  TEST("UTF-8 input is preserved and backspace is character-wide");
  ASSERT_TRUE(run_pty_readline_case("\303\245b\177\r", 20, 0, NULL, terminal,
                                    sizeof(terminal), result, sizeof(result),
                                    &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "\303\245") == 0, "UTF-8 result mismatch");
  PASS();
}

static void test_utf8_swedish_input_is_rendered_as_full_sequences(void) {
  char terminal[8192];
  char result[256];
  int status;

  TEST("UTF-8 Swedish input renders as full sequences");
  ASSERT_TRUE(run_pty_readline_case("\303\245\303\244\303\266\r", 20, 0, NULL,
                                    terminal, sizeof(terminal), result,
                                    sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "\303\245\303\244\303\266") == 0,
              "Swedish UTF-8 result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "p> \303\245\303\244\303\266"),
              "Swedish UTF-8 render missing");
  PASS();
}

static void test_unicode_width_wraps_japanese_and_emoji(void) {
  char terminal[8192];
  char result[256];
  int status;

  TEST("Unicode width wraps Japanese and emoji");
  ASSERT_TRUE(run_pty_readline_case("\346\227\245\346\234\254\350\252\236"
                                    "\346\227\245\346\234\254\350\252\236"
                                    "\360\237\231\202x\r",
                                    12, 0, NULL, terminal, sizeof(terminal),
                                    result, sizeof(result), &status) == 0,
              "pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "child editor failed");
  ASSERT_TRUE(strcmp(result, "\346\227\245\346\234\254\350\252\236"
                             "\346\227\245\346\234\254\350\252\236"
                             "\360\237\231\202x") == 0,
              "wide UTF-8 result mismatch");
  ASSERT_TRUE(contains_bytes(terminal, "p> \346\227\245\346\234\254"
                                       "\350\252\236\346\227\245"),
              "wide first row missing");
  ASSERT_TRUE(contains_bytes(terminal, "   \346\234\254\350\252\236"
                                       "\360\237\231\202x"),
              "wide continuation row missing");
  PASS();
}

static void test_unicode_backspace_deletes_clusters(void) {
  char terminal[8192];
  char result[256];
  int status;

  TEST("Unicode backspace deletes combining and emoji clusters");
  ASSERT_TRUE(run_pty_readline_case("ze\314\201x\177\177\r", 20, 0, NULL,
                                    terminal, sizeof(terminal), result,
                                    sizeof(result), &status) == 0,
              "combining pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "combining child editor failed");
  ASSERT_TRUE(strcmp(result, "z") == 0, "combining cluster was split");

  ASSERT_TRUE(run_pty_readline_case("z\360\237\221\251\342\200\215"
                                    "\360\237\222\273x\177\177\r",
                                    20, 0, NULL, terminal, sizeof(terminal),
                                    result, sizeof(result), &status) == 0,
              "emoji pty case failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "emoji child editor failed");
  ASSERT_TRUE(strcmp(result, "z") == 0, "emoji cluster was split");
  PASS();
}

static void test_queued_turns_profile_promotes_manually(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  int status;
  pid_t pid;
  char result[128];
  char terminal[8192];
  size_t terminal_len;
  ssize_t n;

  TEST("queued-turns profile records steer mode for host-controlled delivery");
  if (openpty(&master_fd, &slave_fd, NULL, NULL, NULL) != 0 ||
      pipe(result_pipe) != 0) {
    FAIL("pty setup failed");
  }
  pid = fork();
  if (pid < 0)
    FAIL("fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_prompt_source_t source;
    sl_t *sl;
    char *line;
    char *steer;
    sl_prompt_queue_mode_t mode;
    char output[128];
    int written;
    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) !=
            SL_OK ||
        sl_set_status_busy(sl, 1) != SL_OK ||
        sl_set_idle_callback(sl, idle_release_after_steer_queued, NULL) !=
            SL_OK)
      _exit(2);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (!line)
      _exit(3);
    written = snprintf(output, sizeof(output), "%d:%s", (int)source, line);
    sl_free_string(sl, line);
    if (written < 0 || written >= (int)sizeof(output))
      _exit(4);
    if (source != SL_PROMPT_SOURCE_QUEUED)
      _exit(5);
    if (sl_prompt_queue_count(sl) != 1 ||
        sl_prompt_queue_get_mode(sl, 0, &mode) != SL_OK ||
        mode != SL_PROMPT_QUEUE_MODE_STEER ||
        sl_prompt_queue_take(sl, 0, &steer) != SL_OK)
      _exit(6);
    written += snprintf(output + written, sizeof(output) - (size_t)written,
                        "|%s", steer);
    sl_free_string(sl, steer);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "turn> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    if (n <= 0)
      FAIL("initial queued-turns prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(
      write(master_fd, "queued\rsteer\033\r", strlen("queued\rsteer\033\r")) ==
          (ssize_t)strlen("queued\rsteer\033\r"),
      "queued-turns input write failed");
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "S 2. steer")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    if (n <= 0)
      FAIL("steer queue preview missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  n = read_some_with_timeout_ms(result_pipe[0], result, sizeof(result) - 1,
                                5000);
  ASSERT_TRUE(n > 0, "queued-turns result missing");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "queued-turns wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued-turns child failed");
  ASSERT_TRUE(strcmp(result, "2:queued|steer") == 0,
              "queued-turns source or ordering mismatch");
  PASS();
}

static void test_queued_turns_cancellation_pauses_auto_delivery(void) {
  int master_fd;
  int slave_fd;
  int result_pipe[2];
  int status;
  pid_t pid;
  char result[128];
  char terminal[8192];
  size_t terminal_len;
  ssize_t n;

  TEST("queued-turns cancellation pauses delivery until direct submit");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_prompt_source_t source;
    sl_t *sl;
    char *line;
    char output[128];
    int written;
    struct idle_finish_state idle_state;

    close(master_fd);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) !=
            SL_OK ||
        sl_set_status_busy(sl, 1) != SL_OK ||
        sl_prompt_queue_append(sl, "queued", 0, NULL) != SL_OK)
      _exit(2);
    idle_state.calls = 0;
    idle_state.text = NULL;
    idle_state.cancel = 1;
    if (sl_set_idle_callback(sl, idle_finish_after_two_ticks, &idle_state) !=
        SL_OK)
      _exit(3);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (line || sl_last_readline_status(sl) != SL_READLINE_CANCELLED)
      _exit(4);
    if (sl_set_idle_callback(sl, NULL, NULL) != SL_OK ||
        sl_set_status_busy(sl, 0) != SL_OK)
      _exit(5);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (!line)
      _exit(6);
    written = snprintf(output, sizeof(output), "%d:%s", (int)source, line);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(7);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (!line)
      _exit(8);
    written += snprintf(output + written, sizeof(output) - (size_t)written,
                        "|%d:%s", (int)source, line);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(9);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "turn> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "initial queued-turns prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "turn> ") ||
         !contains_bytes(terminal, "Q 1. queued")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "cancelled queue was not retained for direct input");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "direct\r", 7) == 7,
              "direct post-cancellation turn write failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "queued-turn cancellation result missing");
  result[n] = '\0';
  close(master_fd);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid,
              "queued-turn cancellation child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued-turn cancellation child failed");
  ASSERT_TRUE(strcmp(result, "1:direct|2:queued") == 0,
              "cancellation did not pause then resume FIFO delivery");
  PASS();
}

struct watch_print_state {
  int fd;
  int calls;
  pid_t callback_pid;
};

static int watch_print_callback(sl_t *sl, const sl_watch_event_t *event,
                                void *userdata) {
  struct watch_print_state *state;
  struct one_chunk_once stream;
  char drain[16];
  state = (struct watch_print_state *)userdata;
  if (!state || !event || (event->events & SL_WATCH_READ) == 0)
    return SL_ERROR_INVALID;
  if (read(state->fd, drain, sizeof(drain)) <= 0)
    return SL_ERROR_IO;
  state->calls++;
  state->callback_pid = getpid();
  if (sl_set_status_element(sl, 0, "streaming") != SL_OK)
    return SL_ERROR;
  stream.text = "[watch] streamed update\n";
  stream.sent = 0;
  return sl_print_above(sl, one_chunk_once_stream, &stream);
}

static void test_watch_prints_while_editing(void) {
  int master_fd;
  int slave_fd;
  int wake_pipe[2];
  int result_pipe[2];
  int flags;
  int status;
  pid_t pid;
  char result[128];
  char terminal[16384];
  size_t terminal_len;
  ssize_t n;

  TEST("external watch preserves UTF-8 draft, queue, and native status");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(wake_pipe) == 0, "wake pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  flags = fcntl(wake_pipe[0], F_GETFL);
  ASSERT_TRUE(flags >= 0 &&
                  fcntl(wake_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0,
              "wake pipe is not nonblocking");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    struct watch_print_state state;
    char *line;
    char output[128];
    int written;
    close(master_fd);
    close(wake_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl || sl_set_statusline(sl, 1, 0) != SL_OK ||
        sl_set_status_element(sl, 0, "waiting") != SL_OK ||
        sl_prompt_queue_append(sl, "queued", 0, NULL) != SL_OK)
      _exit(2);
    state.fd = wake_pipe[0];
    state.calls = 0;
    state.callback_pid = 0;
    watch_id = 0;
    if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ, watch_print_callback,
                     &state, &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(3);
    line = sl_readline(sl, "watch> ");
    if (!line)
      _exit(4);
    written = snprintf(output, sizeof(output), "%s|%d|%ld", line, state.calls,
                       (long)state.callback_pid);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(5);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(wake_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(wake_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "watch> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "watch prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(contains_bytes(terminal, "queued"),
              "queued preview was not rendered");
  ASSERT_TRUE(contains_bytes(terminal, "waiting"),
              "initial status was not rendered");
  ASSERT_TRUE(write(master_fd, "\303\245", 2) == 2,
              "partial UTF-8 draft write failed");
  while (!contains_bytes(terminal, "\303\245")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "partial UTF-8 draft was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(wake_pipe[1], "w", 1) == 1, "wake write failed");
  while (!contains_bytes(terminal, "[watch] streamed update")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "watch output was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  while (!contains_bytes(terminal, "streaming")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "watch status update was not rendered");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "lo\r", 3) == 3, "final draft write failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "watch result missing");
  result[n] = '\0';
  close(master_fd);
  close(wake_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "watch child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "watch child failed");
  {
    char expected[128];
    int written;
    written =
        snprintf(expected, sizeof(expected), "\303\245lo|1|%ld", (long)pid);
    ASSERT_TRUE(written > 0 && written < (int)sizeof(expected),
                "expected watch result overflowed");
    ASSERT_TRUE(strcmp(result, expected) == 0,
                "watch output did not preserve the active draft");
  }
  PASS();
}

struct watch_sequence_state {
  int fd;
  int ack_fd;
  int calls;
};

static int watch_sequence_callback(sl_t *sl, const sl_watch_event_t *event,
                                   void *userdata) {
  struct watch_sequence_state *state;
  char drain[16];
  state = (struct watch_sequence_state *)userdata;
  (void)sl;
  if (!state || !event || (event->events & SL_WATCH_READ) == 0)
    return SL_ERROR_INVALID;
  if (read(state->fd, drain, sizeof(drain)) <= 0)
    return SL_ERROR_IO;
  state->calls++;
  if (write(state->ack_fd, "W", 1) != 1)
    return SL_ERROR_IO;
  return SL_OK;
}

static void test_watch_preserves_escape_continuation_timeout(void) {
  int master_fd;
  int slave_fd;
  int wake_pipe[2];
  int ack_pipe[2];
  int result_pipe[2];
  int status;
  pid_t pid;
  char ack;
  char result[128];
  char terminal[4096];
  size_t terminal_len;
  ssize_t n;

  TEST("external watch preserves an active escape continuation");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(wake_pipe) == 0 && pipe(ack_pipe) == 0 &&
                  pipe(result_pipe) == 0,
              "watch sequence pipe setup failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    struct watch_sequence_state state;
    char *line;
    char output[128];
    int written;

    close(master_fd);
    close(wake_pipe[1]);
    close(ack_pipe[0]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl || sl_history_add(sl, "prior") != SL_OK)
      _exit(2);
    state.fd = wake_pipe[0];
    state.ack_fd = ack_pipe[1];
    state.calls = 0;
    watch_id = 0;
    if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ, watch_sequence_callback,
                     &state, &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(3);
    line = sl_readline(sl, "sequence> ");
    if (!line)
      _exit(4);
    written = snprintf(output, sizeof(output), "%s|%d", line, state.calls);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(5);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(wake_pipe[0]);
    close(ack_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(wake_pipe[0]);
  close(ack_pipe[1]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "sequence> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "sequence prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "\033", 1) == 1,
              "escape sequence prefix write failed");
  usleep(25000);
  ASSERT_TRUE(write(wake_pipe[1], "w", 1) == 1, "watch wake write failed");
  n = read_some_with_timeout(ack_pipe[0], &ack, 1);
  ASSERT_TRUE(n == 1 && ack == 'W', "watch callback did not acknowledge");
  usleep(25000);
  ASSERT_TRUE(write(master_fd, "[A\r", 3) == 3,
              "escape sequence continuation write failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "escape sequence result missing");
  result[n] = '\0';
  close(master_fd);
  close(wake_pipe[1]);
  close(ack_pipe[0]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "sequence child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "sequence child failed");
  ASSERT_TRUE(strcmp(result, "prior|1") == 0,
              "watch wake split the escape sequence");
  PASS();
}

static int watch_complete_queued_turn(sl_t *sl, const sl_watch_event_t *event,
                                      void *userdata) {
  int fd;
  char drain[16];
  if (!event || (event->events & SL_WATCH_READ) == 0 || !userdata)
    return SL_ERROR_INVALID;
  fd = *(int *)userdata;
  if (read(fd, drain, sizeof(drain)) <= 0)
    return SL_ERROR_IO;
  return sl_set_status_busy(sl, 0);
}

struct watch_completion_race_state {
  int wake_fd;
  int entered_fd;
  int release_fd;
};

struct watch_prompt_completion_race_state {
  int wake_fd;
  int entered_fd;
  int release_fd;
  int cancel;
};

static int watch_complete_queued_turn_after_input_race(
    sl_t *sl, const sl_watch_event_t *event, void *userdata) {
  struct watch_completion_race_state *state;
  char byte;
  char drain[16];
  state = (struct watch_completion_race_state *)userdata;
  if (!state || !event || (event->events & SL_WATCH_READ) == 0)
    return SL_ERROR_INVALID;
  if (read(state->wake_fd, drain, sizeof(drain)) <= 0)
    return SL_ERROR_IO;
  byte = 'W';
  if (write(state->entered_fd, &byte, 1) != 1)
    return SL_ERROR_IO;
  if (read(state->release_fd, &byte, 1) != 1)
    return SL_ERROR_IO;
  return sl_set_status_busy(sl, 0);
}

static int watch_complete_prompt_after_input_race(sl_t *sl,
                                                  const sl_watch_event_t *event,
                                                  void *userdata) {
  struct watch_prompt_completion_race_state *state;
  char byte;
  char drain[16];
  int rc;
  state = (struct watch_prompt_completion_race_state *)userdata;
  if (!state || !event || (event->events & SL_WATCH_READ) == 0)
    return SL_ERROR_INVALID;
  if (read(state->wake_fd, drain, sizeof(drain)) <= 0)
    return SL_ERROR_IO;
  rc = state->cancel ? sl_cancel(sl) : sl_submit(sl);
  if (rc != SL_OK)
    return rc;
  byte = 'W';
  if (write(state->entered_fd, &byte, 1) != 1)
    return SL_ERROR_IO;
  if (read(state->release_fd, &byte, 1) != 1)
    return SL_ERROR_IO;
  return SL_OK;
}

static void test_queued_turns_auto_dispatches_on_watch(void) {
  int master_fd;
  int slave_fd;
  int wake_pipe[2];
  int result_pipe[2];
  int flags;
  int status;
  pid_t pid;
  char result[128];
  char terminal[8192];
  size_t terminal_len;
  ssize_t n;

  TEST("queued-turns completion releases FIFO work from a watch callback");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(wake_pipe) == 0, "wake pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  flags = fcntl(wake_pipe[0], F_GETFL);
  ASSERT_TRUE(flags >= 0 &&
                  fcntl(wake_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0,
              "wake pipe is not nonblocking");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_prompt_source_t source;
    sl_t *sl;
    sl_watch_id_t watch_id;
    char *line;
    char output[128];
    int written;
    close(master_fd);
    close(wake_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) !=
            SL_OK ||
        sl_set_status_busy(sl, 1) != SL_OK ||
        sl_prompt_queue_append(sl, "queued", 0, NULL) != SL_OK)
      _exit(2);
    watch_id = 0;
    if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ,
                     watch_complete_queued_turn, &wake_pipe[0],
                     &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(3);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (!line)
      _exit(4);
    written = snprintf(output, sizeof(output), "%d:%s", (int)source, line);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(5);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(wake_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(wake_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "turn> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "queued-turns watch prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(wake_pipe[1], "w", 1) == 1, "wake write failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "queued-turns watch result missing");
  result[n] = '\0';
  close(master_fd);
  close(wake_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid,
              "queued-turns watch child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued-turns watch child failed");
  ASSERT_TRUE(strcmp(result, "2:queued") == 0,
              "queued-turns watch delivery source mismatch");
  PASS();
}

static void test_queued_turns_preserve_alt_enter_during_completion(void) {
  int master_fd;
  int slave_fd;
  int wake_pipe[2];
  int result_pipe[2];
  int flags;
  int status;
  pid_t pid;
  char result[128];
  char terminal[8192];
  size_t terminal_len;
  ssize_t n;

  TEST("queued-turns preserves Alt-Enter during completion dispatch");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(wake_pipe) == 0, "wake pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  flags = fcntl(wake_pipe[0], F_GETFL);
  ASSERT_TRUE(flags >= 0 &&
                  fcntl(wake_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0,
              "wake pipe is not nonblocking");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_prompt_source_t source;
    sl_t *sl;
    sl_watch_id_t watch_id;
    char *line;
    char output[128];
    int written;
    close(master_fd);
    close(wake_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) !=
            SL_OK ||
        sl_set_status_busy(sl, 1) != SL_OK ||
        sl_prompt_queue_append(sl, "queued", 0, NULL) != SL_OK)
      _exit(2);
    watch_id = 0;
    if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ,
                     watch_complete_queued_turn, &wake_pipe[0],
                     &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(3);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (!line)
      _exit(4);
    written = snprintf(output, sizeof(output), "%d:%s", (int)source, line);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(5);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(wake_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(wake_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "turn> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "queued-turns Alt-Enter prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "steer\033", strlen("steer\033")) ==
                  (ssize_t)strlen("steer\033"),
              "Alt-Enter prefix write failed");
  usleep(25000);
  ASSERT_TRUE(write(wake_pipe[1], "w", 1) == 1, "completion wake write failed");
  usleep(25000);
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1,
              "Alt-Enter continuation write failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "queued-turns Alt-Enter result missing");
  result[n] = '\0';
  close(master_fd);
  close(wake_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid,
              "queued-turns Alt-Enter child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued-turns Alt-Enter child failed");
  ASSERT_TRUE(strcmp(result, "1:steer") == 0,
              "completion dispatch preempted Alt-Enter immediate turn");
  PASS();
}

static void
test_queued_turns_prioritize_input_arriving_during_completion(void) {
  int master_fd;
  int slave_fd;
  int wake_pipe[2];
  int entered_pipe[2];
  int release_pipe[2];
  int result_pipe[2];
  int flags;
  int status;
  pid_t pid;
  char entered;
  char result[128];
  char terminal[8192];
  size_t terminal_len;
  ssize_t n;

  TEST("queued-turns prioritize input arriving during completion");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(wake_pipe) == 0, "wake pipe failed");
  ASSERT_TRUE(pipe(entered_pipe) == 0, "callback-entered pipe failed");
  ASSERT_TRUE(pipe(release_pipe) == 0, "callback-release pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  flags = fcntl(wake_pipe[0], F_GETFL);
  ASSERT_TRUE(flags >= 0 &&
                  fcntl(wake_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0,
              "wake pipe is not nonblocking");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_prompt_source_t source;
    sl_t *sl;
    sl_watch_id_t watch_id;
    struct watch_completion_race_state race_state;
    char *line;
    char output[128];
    int written;
    close(master_fd);
    close(wake_pipe[1]);
    close(entered_pipe[0]);
    close(release_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    cfg.prompt_queue = 1;
    sl = sl_create_with_config(&cfg);
    if (!sl ||
        sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) !=
            SL_OK ||
        sl_set_status_busy(sl, 1) != SL_OK ||
        sl_prompt_queue_append(sl, "queued", 0, NULL) != SL_OK)
      _exit(2);
    race_state.wake_fd = wake_pipe[0];
    race_state.entered_fd = entered_pipe[1];
    race_state.release_fd = release_pipe[0];
    watch_id = 0;
    if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ,
                     watch_complete_queued_turn_after_input_race, &race_state,
                     &watch_id) != SL_OK ||
        watch_id == 0)
      _exit(3);
    source = SL_PROMPT_SOURCE_NONE;
    line = sl_next_prompt(sl, "turn> ", &source);
    if (!line)
      _exit(4);
    written = snprintf(output, sizeof(output), "%d:%s", (int)source, line);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(5);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(slave_fd);
    close(wake_pipe[0]);
    close(entered_pipe[1]);
    close(release_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(wake_pipe[0]);
  close(entered_pipe[1]);
  close(release_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "turn> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "queued-turns completion-race prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(wake_pipe[1], "w", 1) == 1, "completion wake write failed");
  n = read_some_with_timeout(entered_pipe[0], &entered, 1);
  ASSERT_TRUE(n == 1 && entered == 'W', "completion callback did not start");
  ASSERT_TRUE(write(master_fd, "steer\033\r", strlen("steer\033\r")) ==
                  (ssize_t)strlen("steer\033\r"),
              "Alt-Enter immediate input write failed");
  ASSERT_TRUE(write(release_pipe[1], "r", 1) == 1,
              "completion callback release failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "queued-turns completion-race result missing");
  result[n] = '\0';
  close(master_fd);
  close(wake_pipe[1]);
  close(entered_pipe[0]);
  close(release_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid,
              "queued-turns completion-race child wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "queued-turns completion-race child failed");
  ASSERT_TRUE(strcmp(result, "1:steer") == 0,
              "completion dispatch preempted newly arrived Alt-Enter input");
  PASS();
}

static void test_watch_completion_preserves_input_for_next_prompt(void) {
  int cancel;

  TEST("watch submit and cancel preserve input for the next prompt");
  for (cancel = 0; cancel < 2; cancel++) {
    int master_fd;
    int slave_fd;
    int wake_pipe[2];
    int entered_pipe[2];
    int release_pipe[2];
    int result_pipe[2];
    int flags;
    int status;
    pid_t pid;
    char entered;
    char result[128];
    char terminal[8192];
    size_t terminal_len;
    ssize_t n;

    ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
                "openpty failed");
    ASSERT_TRUE(pipe(wake_pipe) == 0, "wake pipe failed");
    ASSERT_TRUE(pipe(entered_pipe) == 0, "callback-entered pipe failed");
    ASSERT_TRUE(pipe(release_pipe) == 0, "callback-release pipe failed");
    ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
    flags = fcntl(wake_pipe[0], F_GETFL);
    ASSERT_TRUE(flags >= 0 &&
                    fcntl(wake_pipe[0], F_SETFL, flags | O_NONBLOCK) == 0,
                "wake pipe is not nonblocking");
    pid = fork();
    ASSERT_TRUE(pid >= 0, "fork failed");
    if (pid == 0) {
      sl_config_t cfg;
      sl_prompt_source_t source;
      sl_t *sl;
      sl_watch_id_t watch_id;
      struct watch_prompt_completion_race_state race_state;
      char *line;
      char output[128];
      int written;
      close(master_fd);
      close(wake_pipe[1]);
      close(entered_pipe[0]);
      close(release_pipe[1]);
      close(result_pipe[0]);
      sl_config_init(&cfg);
      cfg.input_fd = slave_fd;
      cfg.output_fd = slave_fd;
      cfg.prompt_queue = 1;
      sl = sl_create_with_config(&cfg);
      if (!sl)
        _exit(2);
      race_state.wake_fd = wake_pipe[0];
      race_state.entered_fd = entered_pipe[1];
      race_state.release_fd = release_pipe[0];
      race_state.cancel = cancel;
      watch_id = 0;
      if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ,
                       watch_complete_prompt_after_input_race, &race_state,
                       &watch_id) != SL_OK ||
          watch_id == 0)
        _exit(3);
      source = SL_PROMPT_SOURCE_NONE;
      line = sl_next_prompt(sl, "first> ", &source);
      if (cancel) {
        if (line || sl_last_readline_status(sl) != SL_READLINE_CANCELLED)
          _exit(4);
      } else {
        if (!line || strcmp(line, "") != 0 || source != SL_PROMPT_SOURCE_DIRECT)
          _exit(4);
        sl_free_string(sl, line);
      }
      source = SL_PROMPT_SOURCE_NONE;
      line = sl_next_prompt(sl, "next> ", &source);
      if (!line)
        _exit(5);
      written = snprintf(output, sizeof(output), "%d:%s", (int)source, line);
      sl_free_string(sl, line);
      if (written <= 0 || written >= (int)sizeof(output))
        _exit(6);
      (void)write(result_pipe[1], output, (size_t)written);
      sl_destroy(sl);
      close(slave_fd);
      close(wake_pipe[0]);
      close(entered_pipe[1]);
      close(release_pipe[0]);
      close(result_pipe[1]);
      _exit(0);
    }
    close(slave_fd);
    close(wake_pipe[0]);
    close(entered_pipe[1]);
    close(release_pipe[0]);
    close(result_pipe[1]);
    terminal_len = 0;
    terminal[0] = '\0';
    while (!contains_bytes(terminal, "first> ")) {
      n = read_some_with_timeout(master_fd, terminal + terminal_len,
                                 sizeof(terminal) - 1 - terminal_len);
      ASSERT_TRUE(n > 0, "watch-completion first prompt missing");
      terminal_len += (size_t)n;
      terminal[terminal_len] = '\0';
    }
    ASSERT_TRUE(write(wake_pipe[1], "w", 1) == 1,
                "completion wake write failed");
    n = read_some_with_timeout(entered_pipe[0], &entered, 1);
    ASSERT_TRUE(n == 1 && entered == 'W', "completion callback did not start");
    ASSERT_TRUE(write(master_fd, "next\r", strlen("next\r")) ==
                    (ssize_t)strlen("next\r"),
                "next-prompt input write failed");
    ASSERT_TRUE(write(release_pipe[1], "r", 1) == 1,
                "completion callback release failed");
    n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
    ASSERT_TRUE(n > 0, "watch-completion result missing");
    result[n] = '\0';
    close(master_fd);
    close(wake_pipe[1]);
    close(entered_pipe[0]);
    close(release_pipe[1]);
    close(result_pipe[0]);
    ASSERT_TRUE(waitpid(pid, &status, 0) == pid,
                "watch-completion child wait failed");
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "watch-completion child failed");
    ASSERT_TRUE(strcmp(result, "1:next") == 0,
                "watch completion discarded input for the next prompt");
  }
  PASS();
}

struct watch_fairness_state {
  int fds[9];
  int calls[9];
};

static int watch_fairness_callback(sl_t *sl, const sl_watch_event_t *event,
                                   void *userdata) {
  struct watch_fairness_state *state;
  int index;
  (void)sl;
  state = (struct watch_fairness_state *)userdata;
  if (!state || !event || (event->events & SL_WATCH_READ) == 0)
    return SL_ERROR_INVALID;
  for (index = 0; index < 9; index++) {
    if (state->fds[index] == event->fd)
      break;
  }
  if (index == 9)
    return SL_ERROR_INVALID;
  state->calls[index]++;
  /* Deliberately retain readiness to model a level-triggered output flood. */
  return SL_OK;
}

static void test_watch_fairness_rotates_ready_flood(void) {
  int master_fd;
  int slave_fd;
  int wake_pipes[9][2];
  int result_pipe[2];
  int i;
  int status;
  pid_t pid;
  char terminal[4096];
  char result[128];
  size_t terminal_len;
  ssize_t n;

  TEST("external watch flood rotates past the dispatch budget");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  for (i = 0; i < 9; i++)
    ASSERT_TRUE(pipe(wake_pipes[i]) == 0, "wake pipe failed");
  ASSERT_TRUE(pipe(result_pipe) == 0, "result pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    struct watch_fairness_state state;
    char *line;
    char output[128];
    int written;
    close(master_fd);
    close(result_pipe[0]);
    memset(&state, 0, sizeof(state));
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);
    for (i = 0; i < 9; i++) {
      close(wake_pipes[i][1]);
      state.fds[i] = wake_pipes[i][0];
      watch_id = 0;
      if (sl_watch_add(sl, wake_pipes[i][0], SL_WATCH_READ,
                       watch_fairness_callback, &state, &watch_id) != SL_OK ||
          watch_id == 0)
        _exit(3);
    }
    line = sl_readline(sl, "fair> ");
    if (!line)
      _exit(4);
    written = snprintf(output, sizeof(output), "%s|%d", line, state.calls[8]);
    sl_free_string(sl, line);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(5);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    for (i = 0; i < 9; i++)
      close(wake_pipes[i][0]);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(result_pipe[1]);
  for (i = 0; i < 9; i++)
    close(wake_pipes[i][0]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "fair> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "fairness prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  for (i = 0; i < 9; i++)
    ASSERT_TRUE(write(wake_pipes[i][1], "w", 1) == 1, "wake write failed");
  ASSERT_TRUE(write(master_fd, "x\r", 2) == 2, "fairness input write failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "fairness result missing");
  result[n] = '\0';
  close(master_fd);
  for (i = 0; i < 9; i++)
    close(wake_pipes[i][1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "fairness wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "fairness child failed");
  ASSERT_TRUE(strcmp(result, "x|0") != 0,
              "ready watch beyond dispatch budget starved");
  PASS();
}

struct watch_lifecycle_state {
  int removed_calls;
  int error_calls;
  int hangup_calls;
  int destroy_rejected;
  unsigned int error_events;
  unsigned int hangup_events;
};

static int watch_lifecycle_removed_callback(sl_t *sl,
                                            const sl_watch_event_t *event,
                                            void *userdata) {
  struct watch_lifecycle_state *state;
  (void)sl;
  (void)event;
  state = (struct watch_lifecycle_state *)userdata;
  if (!state)
    return SL_ERROR_INVALID;
  state->removed_calls++;
  return SL_OK;
}

static int watch_lifecycle_error_callback(sl_t *sl,
                                          const sl_watch_event_t *event,
                                          void *userdata) {
  struct watch_lifecycle_state *state;
  (void)sl;
  state = (struct watch_lifecycle_state *)userdata;
  if (!state || !event)
    return SL_ERROR_INVALID;
  sl_destroy(sl);
  state->destroy_rejected =
      strcmp(sl_last_error(sl),
             "cannot destroy softline handle from a watch callback") == 0;
  state->error_calls++;
  state->error_events = event->events;
  return sl_watch_remove(sl, event->id);
}

static int watch_lifecycle_hangup_callback(sl_t *sl,
                                           const sl_watch_event_t *event,
                                           void *userdata) {
  struct watch_lifecycle_state *state;
  (void)sl;
  state = (struct watch_lifecycle_state *)userdata;
  if (!state || !event)
    return SL_ERROR_INVALID;
  state->hangup_calls++;
  state->hangup_events = event->events;
  return sl_watch_remove(sl, event->id);
}

static int watch_lifecycle_failure_callback(sl_t *sl,
                                            const sl_watch_event_t *event,
                                            void *userdata) {
  (void)sl;
  (void)event;
  (void)userdata;
  return SL_ERROR_INVALID;
}

static void test_watch_lifecycle_reports_terminal_events(void) {
  int master_fd;
  int slave_fd;
  int removed_pipe[2];
  int error_pipe[2];
  int hangup_pipe[2];
  int fail_pipe[2];
  int result_pipe[2];
  int status;
  pid_t pid;
  char terminal[8192];
  char result[128];
  size_t terminal_len;
  ssize_t n;

  TEST("external watch lifecycle reports error, hangup, and failure");
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, NULL) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(removed_pipe) == 0 && pipe(error_pipe) == 0 &&
                  pipe(hangup_pipe) == 0 && pipe(fail_pipe) == 0 &&
                  pipe(result_pipe) == 0,
              "watch lifecycle pipe setup failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    struct watch_lifecycle_state state;
    char *line;
    char output[128];
    int written;

    close(master_fd);
    close(removed_pipe[1]);
    close(error_pipe[1]);
    close(hangup_pipe[1]);
    close(fail_pipe[1]);
    close(result_pipe[0]);
    memset(&state, 0, sizeof(state));
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl)
      _exit(2);

    watch_id = 0;
    if (sl_watch_add(sl, removed_pipe[0], SL_WATCH_READ,
                     watch_lifecycle_removed_callback, &state,
                     &watch_id) != SL_OK ||
        sl_watch_modify(sl, watch_id, SL_WATCH_HANGUP) != SL_OK ||
        sl_watch_remove(sl, watch_id) != SL_OK)
      _exit(3);
    watch_id = 0;
    if (sl_watch_add(sl, removed_pipe[0], SL_WATCH_READ,
                     watch_lifecycle_removed_callback, &state,
                     &watch_id) != SL_OK ||
        sl_watch_clear(sl) != SL_OK)
      _exit(4);
    line = sl_readline(sl, "remove> ");
    if (!line)
      _exit(5);
    sl_free_string(sl, line);

    watch_id = 0;
    if (sl_watch_add(sl, error_pipe[0], SL_WATCH_READ | SL_WATCH_ERROR,
                     watch_lifecycle_error_callback, &state,
                     &watch_id) != SL_OK)
      _exit(6);
    close(error_pipe[0]);
    line = sl_readline(sl, "error> ");
    if (!line)
      _exit(7);
    sl_free_string(sl, line);

    watch_id = 0;
    if (sl_watch_add(sl, hangup_pipe[0], SL_WATCH_HANGUP,
                     watch_lifecycle_hangup_callback, &state,
                     &watch_id) != SL_OK)
      _exit(8);
    line = sl_readline(sl, "hangup> ");
    if (!line)
      _exit(9);
    sl_free_string(sl, line);

    watch_id = 0;
    if (sl_watch_add(sl, fail_pipe[0], SL_WATCH_READ,
                     watch_lifecycle_failure_callback, NULL,
                     &watch_id) != SL_OK)
      _exit(10);
    line = sl_readline(sl, "fail> ");
    if (line) {
      sl_free_string(sl, line);
      _exit(11);
    }
    written =
        snprintf(output, sizeof(output), "%d|%d|%u|%d|%u|%d|%d",
                 state.removed_calls, state.error_calls, state.error_events,
                 state.hangup_calls, state.hangup_events,
                 (int)sl_last_readline_status(sl), state.destroy_rejected);
    if (written <= 0 || written >= (int)sizeof(output))
      _exit(12);
    (void)write(result_pipe[1], output, (size_t)written);
    sl_destroy(sl);
    close(removed_pipe[0]);
    close(hangup_pipe[0]);
    close(fail_pipe[0]);
    close(slave_fd);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(removed_pipe[0]);
  close(error_pipe[0]);
  close(hangup_pipe[0]);
  close(fail_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "remove> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "remove prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(removed_pipe[1], "r", 1) == 1,
              "removed watch signal failed");
  ASSERT_TRUE(write(master_fd, "remove\r", 7) == 7,
              "removed watch input failed");
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "error> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "error prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(master_fd, "error\r", 6) == 6, "error input failed");
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "hangup> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "hangup prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  close(hangup_pipe[1]);
  ASSERT_TRUE(write(master_fd, "hangup\r", 7) == 7, "hangup input failed");
  terminal_len = 0;
  terminal[0] = '\0';
  while (!contains_bytes(terminal, "fail> ")) {
    n = read_some_with_timeout(master_fd, terminal + terminal_len,
                               sizeof(terminal) - 1 - terminal_len);
    ASSERT_TRUE(n > 0, "failure prompt missing");
    terminal_len += (size_t)n;
    terminal[terminal_len] = '\0';
  }
  ASSERT_TRUE(write(fail_pipe[1], "f", 1) == 1, "failure signal failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "watch lifecycle result missing");
  result[n] = '\0';
  close(master_fd);
  close(removed_pipe[1]);
  close(error_pipe[1]);
  close(fail_pipe[1]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "watch lifecycle wait failed");
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "watch lifecycle child failed");
  ASSERT_TRUE(strcmp(result, "0|1|4|1|8|5|1") == 0,
              "watch lifecycle did not preserve event semantics");
  PASS();
}

struct live_output_test_state {
  int fd;
};

static int live_output_test_watch(sl_t *sl, const sl_watch_event_t *event,
                                  void *userdata) {
  struct live_output_test_state *state;
  char command;
  state = (struct live_output_test_state *)userdata;
  if (!event || !state || (event->events & SL_WATCH_READ) == 0 ||
      read(state->fd, &command, 1) != 1)
    return SL_ERROR_IO;
  if (command == '1')
    return sl_output_stream_write(sl, "Hello", 5);
  if (command == '2')
    return sl_output_stream_write(sl, " world", 6);
  if (command == '3') {
    return sl_output_stream_write(sl, "\nnext", 5);
  }
  return SL_ERROR_INVALID;
}

static void test_live_output_stream_chunk_protocol(void) {
  struct winsize ws;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char terminal[8192];
  size_t used;
  ssize_t amount;
  int tries;

  TEST("live output preserves split ANSI and UTF-8 and validates end");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 7;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL, "setup failed");
  ASSERT_TRUE(sl->output_stream_begin(sl) == SL_OK, "receiver begin failed");
  ASSERT_TRUE(sl_output_stream_begin(sl) == SL_ERROR_INVALID,
              "overlapping begin accepted");
  ASSERT_TRUE(sl_output_stream_write(sl, NULL, 0) == SL_OK &&
                  sl_output_stream_write(sl, NULL, 1) == SL_ERROR_INVALID,
              "invalid byte-span handling failed");
  ASSERT_TRUE(sl_output_stream_write(sl, "\033[2J", 4) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "ok", 2) == SL_OK,
              "unsupported CSI command did not fail at its write boundary");
  ASSERT_TRUE(sl_output_stream_write(sl, "\033[1;31", 6) == SL_OK &&
                  sl_output_stream_end(sl) == SL_ERROR_INVALID,
              "incomplete ANSI sequence ended silently");
  ASSERT_TRUE(sl->output_stream_write(sl, "m\xc3", 2) == SL_OK &&
                  sl_output_stream_end(sl) == SL_ERROR_INVALID,
              "incomplete UTF-8 sequence ended silently");
  ASSERT_TRUE(sl_output_stream_write(sl, "\x84", 1) == SL_OK &&
                  sl_output_stream_write(sl, " e\xcc", 3) == SL_OK &&
                  sl_output_stream_write(sl, "\x81", 1) == SL_OK &&
                  sl_output_stream_write(sl, "\033[0m\n", 5) == SL_OK,
              "split styled UTF-8 bytes failed");
  ASSERT_TRUE(sl_output_stream_write(sl, "\xf0\x80\x80\x80", 4) ==
                      SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\x80", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\xc2", 1) == SL_OK &&
                  sl_output_stream_write(sl, "\x9b", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "\xc3", 1) == SL_OK &&
                  sl_output_stream_write(sl, "x", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "R", 1) == SL_OK,
              "malformed UTF-8 or C1 control left the live parser unusable");
  ASSERT_TRUE(sl->output_stream_end(sl) == SL_OK, "receiver end failed");
  ASSERT_TRUE(sl_output_stream_write(sl, "x", 1) == SL_ERROR_INVALID &&
                  sl_output_stream_end(sl) == SL_ERROR_INVALID,
              "write or end after closure accepted");
  sl_destroy(sl);
  close(slave_fd);
  used = 0;
  terminal[0] = '\0';
  for (tries = 0; tries < 100 && used < sizeof(terminal) - 1; tries++) {
    amount = read_some_with_timeout_ms(master_fd, terminal + used,
                                       sizeof(terminal) - 1 - used, 20);
    if (amount <= 0)
      break;
    used += (size_t)amount;
    terminal[used] = '\0';
  }
  close(master_fd);
  ASSERT_TRUE(contains_bytes(terminal, "\033[1;31m") &&
                  contains_bytes(terminal, "\xc3\x84") &&
                  contains_bytes(terminal, " e") &&
                  contains_bytes(terminal, "\xcc\x81") &&
                  contains_bytes(terminal, "ok"),
              "styled Unicode was not emitted before stream end");
  PASS();
}

static void test_native_output_ignores_editor_width(void) {
  struct winsize ws;
  struct vt_screen screen;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char terminal[8192];
  size_t used;
  ssize_t amount;
  int tries;

  TEST("native output keeps terminal width after editor width setter");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 7;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl != NULL && sl_output_stream_begin(sl) == SL_OK &&
                  sl_set_screen_width(sl, 6) == SL_OK &&
                  sl_output_stream_write(sl, "abcdefg", 7) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "width update failed");
  sl_destroy(sl);
  close(slave_fd);
  used = 0;
  terminal[0] = '\0';
  for (tries = 0; tries < 100 && used < sizeof(terminal) - 1; tries++) {
    amount = read_some_with_timeout_ms(master_fd, terminal + used,
                                       sizeof(terminal) - 1 - used, 20);
    if (amount <= 0)
      break;
    used += (size_t)amount;
    terminal[used] = '\0';
  }
  close(master_fd);
  vt_init(&screen, 7, 30);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "abcdefg") &&
                  contains_bytes(terminal, "abcdefg"),
              "editor width setter altered native producer output");
  PASS();
}

static size_t read_live_pty_output(int fd, char *bytes, size_t capacity) {
  size_t used;
  ssize_t amount;
  used = 0;
  while (used + 1 < capacity) {
    amount =
        read_some_with_timeout_ms(fd, bytes + used, capacity - used - 1, 20);
    if (amount <= 0)
      break;
    used += (size_t)amount;
  }
  bytes[used] = '\0';
  return used;
}

static void test_unbounded_finite_output_between_sessions(void) {
  struct winsize ws;
  struct vt_screen screen;
  struct one_chunk_once middle;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[8192];

  TEST("unbounded finite output remains visible between live sessions");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  middle.text = "middle\n";
  middle.sent = 0;
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "first\n", 6) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK &&
                  sl_print_above(sl, one_chunk_once_stream, &middle) == SL_OK &&
                  sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "last\n", 5) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "unbounded output sessions failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "unbounded output missing");
  vt_init(&screen, 8, 30);
  vt_apply(&screen, output);
  ASSERT_TRUE(vt_contains(&screen, "first") && vt_contains(&screen, "middle") &&
                  vt_contains(&screen, "last") &&
                  !vt_contains(&screen, "lastle"),
              "unbounded finite output was overwritten by a later session");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

static void test_unbounded_stream_then_readline_preserves_transcript(void) {
  struct winsize ws;
  struct vt_screen screen;
  int master_fd;
  int slave_fd;
  int ready_pipe[2];
  int result_pipe[2];
  pid_t pid;
  char terminal[8192];
  char result[16];
  char ready;
  ssize_t n;
  int status;

  TEST("readline after unbounded stream preserves its final transcript row");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0 &&
                  pipe(ready_pipe) == 0 && pipe(result_pipe) == 0,
              "pty or pipe failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    struct idle_ready_state idle;
    char *line;
    close(master_fd);
    close(ready_pipe[0]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl || sl_output_stream_begin(sl) != SL_OK ||
        sl_output_stream_write(sl, "FIRST", 5) != SL_OK ||
        sl_output_stream_end(sl) != SL_OK)
      _exit(2);
    idle.fd = ready_pipe[1];
    idle.ready = 0;
    if (sl_set_idle_callback(sl, idle_signal_ready_once, &idle) != SL_OK)
      _exit(3);
    line = sl_readline(sl, "> ");
    if (!line)
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave_fd);
    close(ready_pipe[1]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(ready_pipe[1]);
  close(result_pipe[1]);
  ASSERT_TRUE(read_some_with_timeout(ready_pipe[0], &ready, 1) == 1 &&
                  ready == 'R',
              "readline did not become idle after the stream");
  ASSERT_TRUE(read_live_pty_output(master_fd, terminal, sizeof(terminal)) > 0,
              "stream and readline output missing");
  vt_init(&screen, 8, 30);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(vt_contains(&screen, "FIRST") && vt_contains(&screen, "> "),
              "readline overwrote the stream's final transcript row");
  ASSERT_TRUE(write(master_fd, "ok\r", 3) == 3, "submit failed");
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result));
  ASSERT_TRUE(n == 2 && memcmp(result, "ok", 2) == 0,
              "readline did not return submitted input");
  close(master_fd);
  close(ready_pipe[0]);
  close(result_pipe[0]);
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "readline child failed");
  PASS();
}

struct prompt_output_gate {
  int fd, mode;
};

static int gate_draft_output(sl_t *sl, sl_key_t key, void *userdata,
                             sl_key_action_t *action) {
  struct prompt_output_gate *gate = userdata;
  char byte;
  (void)key;
  if (gate->mode == 2)
    _exit(0);
  if (read(gate->fd, &byte, 1) != 1 || sl_insert(sl, "a") != SL_OK)
    return SL_ERROR;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static const char *run_finite_output_prompt(int mode) {
  struct prompt_output_child child;
  struct timespec deadline;
  const char *failure = NULL;
  struct vt_screen screen;
  char terminal[8192];
  char result[16];
  char ready;
  size_t used;
  ssize_t n;
  int status;

  OUTPUT_REQUIRE(prompt_output_open(&child, 30) == 0, "pty or pipe failed");
  child.pid = fork();
  OUTPUT_REQUIRE(child.pid >= 0, "fork failed");
  if (child.pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    struct idle_ready_state idle;
    struct one_chunk_once middle;
    struct prompt_output_gate gate;
    char *line;
    close(child.master);
    close(child.ready[0]);
    close(child.result[0]);
    close(child.gate[1]);
    sl_config_init(&cfg);
    cfg.input_fd = child.slave;
    cfg.output_fd = child.slave;
    cfg.screen_width = 10;
    sl = sl_create_with_config(&cfg);
    middle.text = "MIDDLE\n";
    middle.sent = 0;
    if (!sl || sl_output_stream_begin(sl) != SL_OK ||
        sl_output_stream_write(sl, "FIRST\n", 6) != SL_OK ||
        sl_output_stream_end(sl) != SL_OK ||
        sl_print_above(sl, one_chunk_once_stream, &middle) != SL_OK)
      _exit(2);
    gate.fd = child.gate[0];
    gate.mode = mode;
    if (mode && sl_bind_key(sl, 'a', gate_draft_output, &gate) != SL_OK)
      _exit(5);
    idle.fd = child.ready[1];
    idle.ready = 0;
    if (sl_set_idle_callback(sl, idle_signal_ready_once, &idle) != SL_OK)
      _exit(3);
    line = sl_readline(sl, "> ");
    if (!line)
      _exit(4);
    (void)write(child.result[1], line, strlen(line));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(child.slave);
    close(child.ready[1]);
    close(child.result[1]);
    _exit(0);
  }
  prompt_output_parent(&child);
  OUTPUT_REQUIRE(read_some_with_timeout(child.ready[0], &ready, 1) == 1 &&
                     ready == 'R',
                 "readline did not become idle after finite output");
  used = read_live_pty_output(child.master, terminal, sizeof(terminal));
  OUTPUT_REQUIRE(used > 0 && write(child.master, "draft", 5) == 5,
                 "finite output or input missing");
  OUTPUT_REQUIRE(clock_gettime(CLOCK_MONOTONIC, &deadline) == 0,
                 "clock failed");
  deadline.tv_sec += 5;
  for (;;) {
    vt_init(&screen, 8, 30);
    vt_apply(&screen, terminal);
    if (vt_contains(&screen, "> draft"))
      break;
    OUTPUT_REQUIRE(used + 1 < sizeof(terminal) &&
                       prompt_output_before_deadline(&deadline),
                   "readline overwrote finite output");
    n = read_some_with_timeout_ms(child.master, terminal + used,
                                  sizeof(terminal) - used - 1, 20);
    if (mode == 1 && child.gate[1] >= 0 && n == 0) {
      OUTPUT_REQUIRE(write(child.gate[1], "x", 1) == 1, "gate release failed");
      prompt_output_close_fd(&child.gate[1]);
    }
    OUTPUT_REQUIRE(n >= 0, "readline overwrote finite output");
    used += (size_t)n;
    terminal[used] = '\0';
  }
  vt_init(&screen, 8, 30);
  vt_apply(&screen, terminal);
  OUTPUT_REQUIRE(vt_contains(&screen, "FIRST") &&
                     vt_contains(&screen, "MIDDLE") &&
                     vt_contains(&screen, "> draft"),
                 "readline overwrote finite output");
  OUTPUT_REQUIRE(write(child.master, "\r", 1) == 1, "submit failed");
  n = read_until_eof_with_timeout(child.result[0], result, sizeof(result));
  OUTPUT_REQUIRE(n == 5 && memcmp(result, "draft", 5) == 0,
                 "readline did not return submitted input");
  OUTPUT_REQUIRE(waitpid(child.pid, &status, 0) == child.pid, "waitpid failed");
  child.pid = -1;
  OUTPUT_REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                 "finite output child failed");
output_cleanup:
  if (prompt_output_cleanup(&child))
    failure = "failed to reap PTY child";
  return failure;
}

static void test_finite_output_then_readline_preserves_transcript(void) {
  const char *failure;
  TEST("readline after finite output preserves its final row");
  failure = run_finite_output_prompt(0);
  ASSERT_TRUE(!failure, failure);
  PASS();
  TEST("finite output stays intact across a gated draft redraw");
  failure = run_finite_output_prompt(1);
  ASSERT_TRUE(!failure, failure);
  PASS();
  TEST("incomplete finite-output draft fails and reaps its PTY child");
  failure = run_finite_output_prompt(2);
  ASSERT_TRUE(failure &&
                  strcmp(failure, "readline overwrote finite output") == 0,
              "missing output or child cleanup failure was hidden");
  PASS();
}

#undef OUTPUT_REQUIRE

struct multiline_stream_end_state {
  int master_fd;
  int attempted;
  int failed;
  unsigned int history_before_end;
  unsigned int history_after_end;
  unsigned int history_after_resize;
  struct vt_screen screen;
};

static void
multiline_stream_end_snapshot(struct multiline_stream_end_state *state) {
  char bytes[16384];
  (void)read_live_pty_output(state->master_fd, bytes, sizeof(bytes));
  vt_apply(&state->screen, bytes);
}

static void idle_end_multiline_stream_once(sl_t *sl, void *userdata) {
  struct multiline_stream_end_state *state;
  state = (struct multiline_stream_end_state *)userdata;
  if (state->attempted)
    return;
  state->attempted = 1;
  if (sl_set_buffer(sl, "one\ntwo") != SL_OK ||
      sl_output_stream_write(sl, "A\nB\nC\nD\nE\nF", 11) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  multiline_stream_end_snapshot(state);
  state->history_before_end = state->screen.history_count;
  if (sl_output_stream_end(sl) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  multiline_stream_end_snapshot(state);
  state->history_after_end = state->screen.history_count;
  if (sl_output_stream_begin(sl) != SL_OK ||
      sl_set_screen_width(sl, 19) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  multiline_stream_end_snapshot(state);
  state->history_after_resize = state->screen.history_count;
finish:
  (void)sl_submit(sl);
}

static void test_multiline_stream_end_preserves_transcript_position(void) {
  struct winsize ws;
  struct multiline_stream_end_state state;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char *line;

  TEST("multiline editor stream teardown and resize keep transcript position");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  memset(&state, 0, sizeof(state));
  state.master_fd = master_fd;
  vt_init(&state.screen, 8, 20);
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_set_idle_callback(sl, idle_end_multiline_stream_once,
                                       &state) == SL_OK,
              "multiline stream setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line && strcmp(line, "one\ntwo") == 0 && state.attempted &&
                  !state.failed &&
                  state.history_after_end == state.history_before_end &&
                  state.history_after_resize == state.history_before_end &&
                  !vt_history_contains(&state.screen, "A") &&
                  vt_contains(&state.screen, "A") &&
                  vt_contains(&state.screen, "F"),
              "stream teardown scrolled or duplicated retained transcript");
  sl_free_string(sl, line);
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

struct history_search_key_state {
  int enter_count;
  int submit_binding;
};

static int history_search_enter_key(sl_t *sl, sl_key_t key, void *userdata,
                                    sl_key_action_t *action) {
  struct history_search_key_state *state =
      (struct history_search_key_state *)userdata;
  (void)key;
  if (sl_history_search_active(sl) != (state->enter_count == 0) ||
      sl->history_search_active(sl) != (state->enter_count == 0))
    return SL_ERROR;
  state->enter_count++;
  *action = state->submit_binding ? SL_KEY_ACTION_SUBMIT : SL_KEY_ACTION_PASS;
  return SL_OK;
}

static void
test_live_output_preserves_reverse_search_prompt(int submit_binding) {
  struct winsize ws;
  struct vt_screen screen;
  int master_fd;
  int slave_fd;
  int wake_pipe[2];
  int result_pipe[2];
  pid_t pid;
  char terminal[32768];
  char chunk[4096];
  char result[64];
  size_t terminal_len;
  size_t output_mark;
  unsigned int history_before;
  ssize_t n;
  int status;
  int tries;

  TEST(submit_binding
           ? "Enter binding can submit a live history match immediately"
           : "live history match stays editable after Enter");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 16;
  ws.ws_row = 6;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  ASSERT_TRUE(pipe(wake_pipe) == 0 && pipe(result_pipe) == 0,
              "pipe setup failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    sl_watch_id_t watch_id;
    struct live_output_test_state state;
    struct history_search_key_state key_state;
    char *line;
    close(master_fd);
    close(wake_pipe[1]);
    close(result_pipe[0]);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl || sl_history_add(sl, "alpha") != SL_OK ||
        sl_output_stream_begin(sl) != SL_OK)
      _exit(2);
    key_state.enter_count = 0;
    key_state.submit_binding = submit_binding;
    if (sl_history_search_active(NULL) || sl_history_search_active(sl) ||
        sl->history_search_active(sl) ||
        sl_bind_key(sl, SL_KEY_ENTER, history_search_enter_key, &key_state) !=
            SL_OK)
      _exit(6);
    state.fd = wake_pipe[0];
    watch_id = 0;
    if (sl_watch_add(sl, wake_pipe[0], SL_WATCH_READ, live_output_test_watch,
                     &state, &watch_id) != SL_OK)
      _exit(3);
    line = sl_readline(sl, "chat> ");
    if (!line || sl_history_search_active(sl) ||
        key_state.enter_count != (submit_binding ? 1 : 2))
      _exit(4);
    (void)write(result_pipe[1], line, strlen(line));
    sl_free_string(sl, line);
    if (sl_output_stream_end(sl) != SL_OK)
      _exit(5);
    sl_destroy(sl);
    close(slave_fd);
    close(wake_pipe[0]);
    close(result_pipe[1]);
    _exit(0);
  }
  close(slave_fd);
  close(wake_pipe[0]);
  close(result_pipe[1]);
  terminal_len = 0;
  terminal[0] = '\0';
  for (tries = 0; tries < 10 && !contains_bytes(terminal, "chat> "); tries++) {
    n = (ssize_t)read_live_pty_output(master_fd, chunk, sizeof(chunk));
    if (n > 0)
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), chunk,
                            n);
  }
  ASSERT_TRUE(contains_bytes(terminal, "chat> "), "initial prompt missing");
  ASSERT_TRUE(write(master_fd, "\022", 1) == 1, "reverse search input failed");
  for (tries = 0; tries < 40; tries++) {
    n = read_some_with_timeout(master_fd, chunk, sizeof(chunk));
    ASSERT_TRUE(n > 0, "reverse search prompt missing");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), chunk, n);
    if (contains_bytes(terminal, "(r-search)`':"))
      break;
  }
  ASSERT_TRUE(tries < 40, "reverse search did not become visible");
  n = (ssize_t)read_live_pty_output(master_fd, chunk, sizeof(chunk));
  if (n > 0)
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), chunk, n);
  vt_init(&screen, 6, 16);
  vt_apply(&screen, terminal);
  history_before = screen.history_count;
  output_mark = terminal_len;
  ASSERT_TRUE(write(wake_pipe[1], "1", 1) == 1, "live write signal failed");
  for (tries = 0;
       tries < 10 && !contains_bytes(terminal + output_mark, "Hello");
       tries++) {
    n = read_some_with_timeout(master_fd, chunk, sizeof(chunk));
    ASSERT_TRUE(n > 0, "live write missing during reverse search");
    append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), chunk, n);
  }
  ASSERT_TRUE(!contains_bytes(terminal + output_mark, "chat> "),
              "live output redrew the ordinary prompt over reverse search");
  vt_init(&screen, 6, 16);
  vt_apply(&screen, terminal);
  ASSERT_TRUE(screen.history_count == history_before,
              "live output moved or replaced the reverse-search prompt");
  output_mark = terminal_len;
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "search accept failed");
  if (!submit_binding) {
    for (tries = 0;
         tries < 10 && !contains_bytes(terminal + output_mark, "chat> ");
         tries++) {
      n = read_some_with_timeout(master_fd, chunk, sizeof(chunk));
      ASSERT_TRUE(n > 0, "ordinary prompt missing after accepting history");
      append_terminal_bytes(terminal, &terminal_len, sizeof(terminal), chunk,
                            n);
    }
    ASSERT_TRUE(contains_bytes(terminal + output_mark, "chat> "),
                "history accept did not restore the ordinary prompt");
    ASSERT_TRUE(read_some_with_timeout_ms(result_pipe[0], result,
                                          sizeof(result), 50) == 0,
                "history accept submitted the prompt");
    ASSERT_TRUE(
        write(master_fd, "\001edited \005!\r", strlen("\001edited \005!\r")) ==
            (ssize_t)strlen("\001edited \005!\r"),
        "editing accepted history failed");
  }
  n = read_some_with_timeout(result_pipe[0], result, sizeof(result) - 1);
  ASSERT_TRUE(n > 0, "search result missing");
  result[n] = '\0';
  close(wake_pipe[1]);
  close(result_pipe[0]);
  /* The child still ends its output stream after sending the result. Keep
   * the PTY master open until that final terminal write has completed. */
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid, "search child wait failed");
  close(master_fd);
  ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
                  strcmp(result, submit_binding ? "alpha" : "edited alpha!") ==
                      0,
              "reverse search result changed after live output");
  PASS();
}

struct live_submit_state {
  int fired;
  int status;
};

static void live_submit_draft_idle(sl_t *sl, void *userdata) {
  struct live_submit_state *state;
  state = (struct live_submit_state *)userdata;
  if (state->fired)
    return;
  state->fired = 1;
  state->status = sl_set_buffer(sl, "draft");
  if (state->status == SL_OK)
    state->status = sl_submit(sl);
}

static void test_live_output_after_readline_submit_clears_editor(void) {
  struct winsize ws;
  struct vt_screen screen;
  struct live_submit_state state;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[16384];
  char *line;

  TEST("live output after readline submission excludes old editor cells");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.clear_prompt_on_exit = 1;
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
              "live stream setup failed");
  memset(&state, 0, sizeof(state));
  ASSERT_TRUE(sl_set_idle_callback(sl, live_submit_draft_idle, &state) == SL_OK,
              "idle submit setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line && strcmp(line, "draft") == 0 && state.fired &&
                  state.status == SL_OK,
              "live readline submission failed");
  sl_free_string(sl, line);
  ASSERT_TRUE(sl_output_stream_write(sl, "\nX", 2) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "post-submit live output failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  vt_init(&screen, 8, 20);
  vt_apply(&screen, output);
  ASSERT_TRUE(vt_contains(&screen, "X") && !vt_contains(&screen, "draft"),
              "completed editor text leaked into the live transcript");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

static void test_retained_stream_tracks_readline_scrollback(void) {
  struct winsize ws;
  struct vt_screen screen;
  struct live_submit_state state;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[32768];
  char *line;
  int turn;

  TEST("retained stream follows scrollback through ordinary readline turns");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "FIRST", 5) == SL_OK &&
                  sl_output_stream_end(sl) == SL_OK,
              "initial output stream failed");
  vt_init(&screen, 8, 20);
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "initial transcript output missing");
  vt_apply(&screen, output);
  ASSERT_TRUE(vt_contains(&screen, "FIRST"), "initial transcript row missing");
  memset(&state, 0, sizeof(state));
  ASSERT_TRUE(sl_set_idle_callback(sl, live_submit_draft_idle, &state) == SL_OK,
              "idle callback setup failed");
  for (turn = 0; turn < 8; turn++) {
    state.fired = 0;
    line = sl_readline(sl, "> ");
    ASSERT_TRUE(line && strcmp(line, "draft") == 0 && state.status == SL_OK,
                "ordinary readline submission failed");
    sl_free_string(sl, line);
    ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
                "readline output missing");
    vt_apply(&screen, output);
  }
  ASSERT_TRUE(vt_history_contains(&screen, "FIRST") &&
                  !vt_contains(&screen, "FIRST"),
              "old transcript did not move into scrollback");
  ASSERT_TRUE(sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "SECOND", 6) == SL_OK &&
                  sl_set_screen_width(sl, 19) == SL_OK,
              "restarted stream or resize failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "resized stream output missing");
  vt_apply(&screen, output);
  ASSERT_TRUE(
      vt_history_contains(&screen, "FIRST") && !vt_contains(&screen, "FIRST") &&
          vt_contains(&screen, "SECOND") && vt_contains(&screen, "draft"),
      "resize replayed scrollback or erased a newer submitted turn");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

struct retained_growth_state {
  int master_fd;
  int fired;
  int failed;
  unsigned int history_after_growth;
  struct vt_screen screen;
};

static void retained_growth_snapshot(struct retained_growth_state *state) {
  char bytes[32768];
  (void)read_live_pty_output(state->master_fd, bytes, sizeof(bytes));
  vt_apply(&state->screen, bytes);
}

static void retained_growth_idle(sl_t *sl, void *userdata) {
  struct retained_growth_state *state;
  state = (struct retained_growth_state *)userdata;
  if (state->fired)
    return;
  state->fired = 1;
  if (sl_output_stream_write(sl, "A\nB\nC\nD\nE\nF", 11) != SL_OK ||
      sl_output_stream_end(sl) != SL_OK ||
      sl_set_buffer(sl, "one\ntwo\nthree") != SL_OK ||
      sl_set_status_message(sl, "message") != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  retained_growth_snapshot(state);
  state->history_after_growth = state->screen.history_count;
  if (!vt_contains(&state->screen, "C") || !vt_contains(&state->screen, "D") ||
      !vt_contains(&state->screen, "E") || !vt_contains(&state->screen, "F") ||
      sl_output_stream_begin(sl) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  retained_growth_snapshot(state);
  if (state->screen.history_count != state->history_after_growth ||
      !vt_contains(&state->screen, "C") || !vt_contains(&state->screen, "D") ||
      !vt_contains(&state->screen, "E") || !vt_contains(&state->screen, "F"))
    state->failed = 1;
finish:
  (void)sl_submit(sl);
}

static void test_retained_stream_survives_prompt_growth(void) {
  struct winsize ws;
  struct retained_growth_state state;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char *line;

  TEST("retained transcript survives prompt growth and stream reopening");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  memset(&state, 0, sizeof(state));
  state.master_fd = master_fd;
  vt_init(&state.screen, 8, 20);
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_set_idle_callback(sl, retained_growth_idle, &state) ==
                      SL_OK,
              "retained stream setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line && strcmp(line, "one\ntwo\nthree") == 0 && state.fired &&
                  !state.failed,
              "prompt growth or stream reopen displaced retained rows");
  sl_free_string(sl, line);
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

static void retained_shrink_idle(sl_t *sl, void *userdata) {
  struct retained_growth_state *state;
  state = (struct retained_growth_state *)userdata;
  if (state->fired)
    return;
  state->fired = 1;
  if (sl_set_buffer(sl, "one\ntwo") != SL_OK ||
      sl_output_stream_write(sl, "TRANSCRIPT", 10) != SL_OK ||
      sl_output_stream_end(sl) != SL_OK || sl_set_buffer(sl, "x") != SL_OK ||
      sl_set_status_message(sl, NULL) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  retained_growth_snapshot(state);
  if (strncmp(state->screen.cells[0], "TRANSCRIPT", 10) != 0 ||
      strncmp(state->screen.cells[7], "> x", 3) != 0 ||
      state->screen.row != 7 || state->screen.col != 3 ||
      sl_set_buffer(sl, "three\nfour") != SL_OK ||
      sl_set_status_message(sl, NULL) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  retained_growth_snapshot(state);
  if (strncmp(state->screen.cells[0], "TRANSCRIPT", 10) != 0 ||
      strncmp(state->screen.cells[6], "> three", 7) != 0 ||
      strncmp(state->screen.cells[7], "  four", 6) != 0 ||
      sl_output_stream_begin(sl) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  retained_growth_snapshot(state);
  if (!vt_contains(&state->screen, "TRANSCRIPT") ||
      !vt_contains(&state->screen, "> three") ||
      !vt_contains(&state->screen, "four"))
    state->failed = 1;
finish:
  (void)sl_submit(sl);
}

static void test_retained_stream_survives_prompt_shrink(void) {
  struct winsize ws;
  struct retained_growth_state state;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char *line;

  TEST("retained transcript survives draft shrink and regrowth");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  memset(&state, 0, sizeof(state));
  state.master_fd = master_fd;
  vt_init(&state.screen, 8, 20);
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_set_idle_callback(sl, retained_shrink_idle, &state) ==
                      SL_OK,
              "retained stream setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line && strcmp(line, "three\nfour") == 0 && state.fired &&
                  !state.failed,
              "draft resize displaced the prompt or retained transcript");
  sl_free_string(sl, line);
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

struct native_scroll_probe {
  int master_fd;
  int output_fd;
  int resize;
  int fired;
  int result;
};

static void native_scroll_probe_idle(sl_t *sl, void *userdata) {
  struct native_scroll_probe *probe;
  struct winsize ws;
  const char *bytes;
  probe = (struct native_scroll_probe *)userdata;
  if (probe->fired)
    return;
  probe->fired = 1;
  if (probe->resize > 0) {
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 30;
    ws.ws_row = 8;
    if (ioctl(probe->master_fd, TIOCSWINSZ, &ws) != 0 ||
        write(probe->output_fd, "\033[1;25HOUTSID", 13) != 13) {
      probe->result = SL_ERROR_IO;
      (void)sl_cancel(sl);
      return;
    }
    bytes = "FIRST\nSECOND\nTHIRD";
  } else {
    bytes = "\033[41mRED\nNEXT";
  }
  probe->result = sl_output_stream_write(sl, bytes, strlen(bytes));
  if (probe->resize < 0 && probe->result == SL_OK) {
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 16;
    ws.ws_row = 8;
    if (ioctl(probe->master_fd, TIOCSWINSZ, &ws) != 0)
      probe->result = SL_ERROR_IO;
    else
      probe->result = sl_set_status_message(sl, NULL);
  }
  (void)sl_cancel(sl);
}

static void test_live_output_native_scroll_resets_prompt_style(void) {
  struct winsize ws;
  struct native_scroll_probe probe;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[16384];
  char *line;
  const char *sync_start;
  const char *reset;
  const char *prompt;
  const char *resized;

  TEST("native stream resets style before restoring the prompt cursor");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  cfg.prompt_theme = SL_PROMPT_THEME_PLAIN;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
              "plain stream setup failed");
  memset(&probe, 0, sizeof(probe));
  probe.master_fd = master_fd;
  probe.output_fd = slave_fd;
  probe.resize = -1;
  ASSERT_TRUE(sl_set_idle_callback(sl, native_scroll_probe_idle, &probe) ==
                  SL_OK,
              "idle callback setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line == NULL && probe.fired && probe.result == SL_OK,
              "styled live write failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  sync_start = strstr(output, "\033[41mRED\r\nNEXT");
  reset = sync_start ? strstr(sync_start, "\033[0m") : NULL;
  prompt = reset ? strstr(reset, "\033[65535;3H") : NULL;
  ASSERT_TRUE(sync_start && reset && prompt && reset < prompt,
              "native stream was rewritten or leaked style into the prompt");
  resized = reset ? strstr(reset + 4, "\033[1;7r") : NULL;
  ASSERT_TRUE(resized && strstr(resized, "\033[65535;3H") &&
                  !strstr(reset + 4, "\033[41m") && !strstr(output, "\0337") &&
                  !strstr(output, "\0338"),
              "fitted resize lost margin/cursor restoration or leaked style");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "stream end failed");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

static void test_live_output_prompt_growth_preserves_history(void) {
  struct winsize ws;
  struct vt_screen screen;
  int master_fd;
  int slave_fd;
  pid_t pid;
  char output[32768];
  char chunk[2048];
  size_t used;
  ssize_t amount;
  int tries;
  int status;

  TEST("prompt growth scrolls only occupied output rows");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 20;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  pid = fork();
  ASSERT_TRUE(pid >= 0, "fork failed");
  if (pid == 0) {
    sl_config_t cfg;
    sl_t *sl;
    char *line;
    close(master_fd);
    sl_config_init(&cfg);
    cfg.input_fd = slave_fd;
    cfg.output_fd = slave_fd;
    sl = sl_create_with_config(&cfg);
    if (!sl || sl_output_stream_begin(sl) != SL_OK ||
        sl_output_stream_write(
            sl, "ONE\nTWO\nTHREE\nFOUR\nFIVE\nSIX\nSEVEN",
            strlen("ONE\nTWO\nTHREE\nFOUR\nFIVE\nSIX\nSEVEN")) != SL_OK ||
        sl_set_statusline(sl, 1, 0) != SL_OK)
      _exit(2);
    line = sl_readline(sl, "> ");
    if (!line)
      _exit(3);
    sl_free_string(sl, line);
    if (sl_output_stream_end(sl) != SL_OK)
      _exit(4);
    sl_destroy(sl);
    close(slave_fd);
    _exit(0);
  }
  close(slave_fd);
  used = 0;
  output[0] = '\0';
  for (tries = 0; tries < 100; tries++) {
    vt_init(&screen, 8, 20);
    vt_apply(&screen, output);
    if (vt_contains(&screen, "TWO") && vt_contains(&screen, "THREE") &&
        vt_contains(&screen, "SEVEN") && vt_contains(&screen, "> "))
      break;
    amount = read_some_with_timeout_ms(master_fd, chunk, sizeof(chunk), 50);
    if (amount <= 0)
      continue;
    ASSERT_TRUE(used + (size_t)amount < sizeof(output),
                "prompt growth output exceeded test buffer");
    append_terminal_bytes(output, &used, sizeof(output), chunk, amount);
  }
  if (!vt_contains(&screen, "TWO") || !vt_contains(&screen, "THREE") ||
      !vt_contains(&screen, "SEVEN"))
    vt_dump(&screen);
  ASSERT_TRUE(screen.history_count == 1 &&
                  strncmp(screen.history[0], "ONE", 3) == 0 &&
                  strncmp(screen.cells[0], "TWO", 3) == 0 &&
                  strncmp(screen.cells[1], "THREE", 5) == 0 &&
                  strncmp(screen.cells[2], "FOUR", 4) == 0 &&
                  strncmp(screen.cells[5], "SEVEN", 5) == 0 &&
                  strncmp(screen.cells[6], "+", 1) == 0 &&
                  strncmp(screen.cells[7], "> ", 2) == 0,
              "prompt growth did not preserve surviving transcript cells");
  ASSERT_TRUE(write(master_fd, "\r", 1) == 1, "submit failed");
  ASSERT_TRUE(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "prompt growth child failed");
  close(master_fd);
  PASS();
}

static void test_live_output_error_resets_terminal_style(void) {
  struct winsize ws;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[4096];
  const char *styled;
  const char *last_reset;
  const char *next;

  TEST("invalid live output resets terminal styling before returning");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 6;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
              "live output setup failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(sl_output_stream_write(sl, "\033[31mred\001", 9) ==
                  SL_ERROR_INVALID,
              "invalid styled output was accepted");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "styled output missing");
  styled = strstr(output, "red");
  last_reset = NULL;
  next = output;
  while ((next = strstr(next, "\033[0m")) != NULL) {
    last_reset = next;
    next++;
  }
  ASSERT_TRUE(styled && last_reset && last_reset > styled,
              "terminal style remained active after invalid output");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "stream end failed");
  ASSERT_TRUE(sl_output_stream_begin(sl) == SL_OK, "second stream failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(sl_output_stream_write(sl, "\033[31;8msecret",
                                     strlen("\033[31;8msecret")) ==
                      SL_ERROR_INVALID &&
                  sl_output_stream_write(sl, "visible", 7) == SL_OK,
              "unsupported SGR attribute was accepted");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0 &&
                  contains_bytes(output, "visible") &&
                  !contains_bytes(output, "secret") &&
                  contains_bytes(output, "\033[0mvisible") &&
                  !contains_bytes(output, "\0337") &&
                  !contains_bytes(output, "\0338"),
              "unsupported SGR changed rendered output or style");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "second stream end failed");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

static void test_live_output_reconciles_physical_resize(void) {
  struct winsize ws;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[8192];

  TEST("live output reconciles a physical resize without an active prompt");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 30;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK &&
                  sl_output_stream_write(sl, "hello", 5) == SL_OK,
              "initial output failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ws.ws_row = 3;
  ASSERT_TRUE(ioctl(master_fd, TIOCSWINSZ, &ws) == 0, "resize failed");
  ASSERT_TRUE(sl_output_stream_write(sl, " world", 6) == SL_OK,
              "resized output failed");
  ASSERT_TRUE(read_live_pty_output(master_fd, output, sizeof(output)) > 0,
              "resized output missing");
  ASSERT_TRUE(contains_bytes(output, " world\033[0m") &&
                  !contains_bytes(output, "\0337") &&
                  !contains_bytes(output, "\0338") &&
                  !contains_bytes(output, "hello") &&
                  !contains_bytes(output, "\n"),
              "resized output replayed text or failed to restore its cursor");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "stream end failed");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

struct live_resize_tail_state {
  int master_fd;
  int fired;
  int failed;
  char before_resize[8192];
  char after_resize[8192];
  char after_write[8192];
  char after_idle[8192];
  char after_expand[8192];
  char after_expand_write[8192];
};

static void live_resize_tail_idle(sl_t *sl, void *userdata) {
  struct live_resize_tail_state *state;
  struct winsize ws;
  state = (struct live_resize_tail_state *)userdata;
  if (state->fired == 2)
    return;
  if (state->fired == 1) {
    state->fired = 2;
    if (sl_set_status_message(sl, "status update after terminal resize") !=
        SL_OK)
      state->failed = 1;
    (void)read_live_pty_output(state->master_fd, state->after_idle,
                               sizeof(state->after_idle));
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = 40;
    ws.ws_row = 8;
    if (ioctl(state->master_fd, TIOCSWINSZ, &ws) != 0 ||
        sl_set_screen_width(sl, 0) != SL_OK)
      state->failed = 1;
    (void)read_live_pty_output(state->master_fd, state->after_expand,
                               sizeof(state->after_expand));
    if (sl_output_stream_write(sl, "t", 1) != SL_OK)
      state->failed = 1;
    (void)read_live_pty_output(state->master_fd, state->after_expand_write,
                               sizeof(state->after_expand_write));
    (void)sl_cancel(sl);
    return;
  }
  state->fired = 1;
  if (sl_set_buffer(sl, "hello world sentence") != SL_OK ||
      sl_set_status_message(sl, NULL) != SL_OK ||
      sl_output_stream_write(sl, "FIRST\nSECOND\nabcdefghijklmnopqr", 31) !=
          SL_OK) {
    state->failed = 1;
    goto finish;
  }
  (void)read_live_pty_output(state->master_fd, state->before_resize,
                             sizeof(state->before_resize));
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 16;
  ws.ws_row = 8;
  if (ioctl(state->master_fd, TIOCSWINSZ, &ws) != 0 ||
      sl_set_screen_width(sl, 0) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  (void)read_live_pty_output(state->master_fd, state->after_resize,
                             sizeof(state->after_resize));
  if (sl_output_stream_write(sl, "s", 1) != SL_OK) {
    state->failed = 1;
    goto finish;
  }
  (void)read_live_pty_output(state->master_fd, state->after_write,
                             sizeof(state->after_write));
  return;
finish:
  (void)sl_cancel(sl);
}

static void test_live_output_resize_continues_current_row(void) {
  struct winsize ws;
  struct live_resize_tail_state state;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char *line;

  TEST("live output width resize continues current row without scrolling");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 40;
  ws.ws_row = 8;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  memset(&state, 0, sizeof(state));
  state.master_fd = master_fd;
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_set_statusline(sl, 1, 0) == SL_OK &&
                  sl_output_stream_begin(sl) == SL_OK &&
                  sl_set_idle_callback(sl, live_resize_tail_idle, &state) ==
                      SL_OK,
              "live resize setup failed");
  line = sl_readline(sl, "> ");
  ASSERT_TRUE(line == NULL && state.fired == 2 && !state.failed &&
                  !contains_bytes(state.after_resize, "\n") &&
                  !contains_bytes(state.after_write, "\n") &&
                  !contains_bytes(state.after_idle, "\n"),
              "resizing an unfinished output row inserted a terminal line");
  ASSERT_TRUE(!contains_bytes(state.after_resize, "FIRST") &&
                  !contains_bytes(state.after_resize, "SECOND") &&
                  !contains_bytes(state.after_resize, "abcdefghijklmnop"),
              "resize replayed cached transcript text");
  ASSERT_TRUE(contains_bytes(state.after_write, "s\033[0m") &&
                  contains_bytes(state.after_expand_write, "t\033[0m") &&
                  !contains_bytes(state.after_expand, "FIRST") &&
                  !contains_bytes(state.after_expand, "SECOND") &&
                  !contains_bytes(state.after_expand, "abcdefghijklmnop") &&
                  !contains_bytes(state.after_expand, "\n") &&
                  !contains_bytes(state.after_expand_write, "\n"),
              "resize rewrote output instead of continuing the tracked cursor");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

static void test_live_output_clears_promptless_scroll_row(void) {
  struct winsize ws;
  struct vt_screen screen;
  sl_config_t cfg;
  sl_t *sl;
  int master_fd;
  int slave_fd;
  char output[8192];
  size_t used;

  TEST("promptless native scroll clears stale bottom-row text");
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = 10;
  ws.ws_row = 5;
  ASSERT_TRUE(openpty(&master_fd, &slave_fd, NULL, NULL, &ws) == 0,
              "openpty failed");
  sl_config_init(&cfg);
  cfg.input_fd = slave_fd;
  cfg.output_fd = slave_fd;
  sl = sl_create_with_config(&cfg);
  ASSERT_TRUE(sl && sl_output_stream_begin(sl) == SL_OK,
              "promptless stream setup failed");
  (void)read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(write(slave_fd, "\033[5;1HGHOST", 11) == 11,
              "bottom-row seed failed");
  ASSERT_TRUE(sl_output_stream_write(sl, "line\nX", 6) == SL_OK,
              "promptless native scroll failed");
  used = read_live_pty_output(master_fd, output, sizeof(output));
  ASSERT_TRUE(used > 0, "promptless output missing");
  vt_init(&screen, 5, 10);
  vt_apply(&screen, output);
  ASSERT_TRUE(vt_contains(&screen, "line") && vt_contains(&screen, "X") &&
                  !vt_contains(&screen, "XHOST"),
              "stale bottom-row text contaminated live output");
  ASSERT_TRUE(sl_output_stream_end(sl) == SL_OK, "stream end failed");
  sl_destroy(sl);
  close(slave_fd);
  close(master_fd);
  PASS();
}

#else
static void
test_live_output_preserves_reverse_search_prompt(int submit_binding) {
  (void)submit_binding;
  TEST("live output preserves reverse-search prompt and stable history");
  printf("SKIP\n");
  tests_passed++;
}
static void test_live_output_after_readline_submit_clears_editor(void) {
  TEST("live output after readline submission excludes old editor cells");
  printf("SKIP\n");
  tests_passed++;
}
static void test_retained_stream_tracks_readline_scrollback(void) {
  TEST("retained stream follows scrollback through ordinary readline turns");
  printf("SKIP\n");
  tests_passed++;
}
static void test_live_output_native_scroll_resets_prompt_style(void) {
  TEST("native stream resets style before restoring the prompt cursor");
  printf("SKIP\n");
  tests_passed++;
}
static void test_live_output_prompt_growth_preserves_history(void) {
  TEST("prompt growth scrolls displaced transcript into native history");
  printf("SKIP\n");
  tests_passed++;
}
static void test_unbounded_finite_output_between_sessions(void) {
  TEST("unbounded finite output remains visible between live sessions");
  printf("SKIP\n");
  tests_passed++;
}
static void test_unbounded_stream_then_readline_preserves_transcript(void) {
  TEST("readline after unbounded stream preserves its final transcript row");
  printf("SKIP\n");
  tests_passed++;
}
static void test_multiline_stream_end_preserves_transcript_position(void) {
  TEST("multiline editor stream teardown and resize keep transcript position");
  printf("SKIP\n");
  tests_passed++;
}
static void test_live_output_stream_chunk_protocol(void) {
  TEST("live output preserves split ANSI and UTF-8 and validates end");
  printf("SKIP\n");
  tests_passed++;
}
static void test_native_output_ignores_editor_width(void) {
  TEST("live unbounded output honors a midstream width setter");
  printf("SKIP\n");
  tests_passed++;
}
static void test_queued_turns_profile_promotes_manually(void) {
  TEST("queued-turns profile submits and promotes under manual delivery");
  printf("SKIP\n");
  tests_passed++;
}
static void test_queued_turns_cancellation_pauses_auto_delivery(void) {
  TEST("queued-turns cancellation pauses automatic delivery");
  printf("SKIP\n");
  tests_passed++;
}
static void test_watch_prints_while_editing(void) {
  TEST("external watch prints above a partial active draft");
  printf("SKIP\n");
  tests_passed++;
}
static void test_watch_preserves_escape_continuation_timeout(void) {
  TEST("external watch preserves an active escape continuation");
  printf("SKIP\n");
  tests_passed++;
}
static void test_queued_turns_auto_dispatches_on_watch(void) {
  TEST("queued-turns auto-delivers FIFO work from a watch callback");
  printf("SKIP\n");
  tests_passed++;
}
static void test_queued_turns_preserve_alt_enter_during_completion(void) {
  TEST("queued-turns preserves Alt-Enter during completion dispatch");
  printf("SKIP\n");
  tests_passed++;
}
static void test_watch_fairness_rotates_ready_flood(void) {
  TEST("external watch flood rotates past the dispatch budget");
  printf("SKIP\n");
  tests_passed++;
}
static void test_watch_lifecycle_reports_terminal_events(void) {
  TEST("external watch lifecycle reports error, hangup, and failure");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_enter_and_ctrl_j(void) {
  TEST("pty readline is non-destructive and Ctrl-J inserts newline");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_readline_uses_default_prompt(void) {
  TEST("pty readline uses default prompt");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_readline_stops_at_line_max(void) {
  TEST("pty readline stops at configured line max");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_ctrl_d_exits(void) {
  TEST("pty Ctrl-D on empty prompt returns NULL");
  printf("SKIP\n");
  tests_passed++;
}
static void test_normal_prompt_proceeds_after_output(void) {
  TEST("normal prompt proceeds after output");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_history_navigation_restores_draft(void) {
  TEST("history up recalls newest entry");
  printf("SKIP\n");
  tests_passed++;
  TEST("history down restores edited draft");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_readline_does_not_auto_add_history(void) {
  TEST("TTY readline does not auto-add submitted history");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_r_searches_memory_history(void) {
  TEST("Ctrl-R searches in-memory history newest first");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_r_repeats_and_wraps_matches(void) {
  TEST("Ctrl-R repeats cycle older matches and wrap");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_r_no_match_and_cancel_restore_draft(void) {
  TEST("Ctrl-R no-match submit preserves draft");
  printf("SKIP\n");
  tests_passed++;
  TEST("Ctrl-R cancel restores edited draft");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_r_searches_loaded_history_file(void) {
  TEST("Ctrl-R searches loaded history file");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_r_searches_unicode_and_multiline_history(void) {
  TEST("Ctrl-R searches Unicode and multiline history");
  printf("SKIP\n");
  tests_passed++;
}
static void test_bracketed_paste_is_literal_content(void) {
  TEST("bracketed paste treats carriage return as content");
  printf("SKIP\n");
  tests_passed++;
}
static void test_bracketed_paste_stops_at_line_max(void) {
  TEST("bracketed paste stops at configured line max");
  printf("SKIP\n");
  tests_passed++;
}
static void test_tab_render_expands_beyond_line_bytes(void) {
  TEST("tab rendering can exceed line byte count");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_tab_inserts_text(void) {
  TEST("key binding TAB inserts text");
  printf("SKIP\n");
  tests_passed++;
}
static void test_prompt_queue_dispatches_fifo_with_themes(void) {
  TEST("prompt queue previews and dispatches FIFO across themes");
  printf("SKIP\n");
  tests_passed++;
}
static void test_prompt_queue_alt_e_recalls_newest(void) {
  TEST("Alt-E recalls the newest queued prompt into the editor");
  printf("SKIP\n");
  tests_passed++;
}
static void test_prompt_themes_style_normal_readline(void) {
  TEST("prompt themes style normal readline UI");
  printf("SKIP\n");
  tests_passed++;
}
static void test_statusline_uses_palette_offset_and_truncation(void) {
  TEST("status line offsets colours and truncates after 32 elements");
  printf("SKIP\n");
  tests_passed++;
}
static void test_default_statusline_uses_ansi_palette(void) {
  TEST("default status line uses standard ANSI palette");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_alt_m_inserts_text(void) {
  TEST("key binding Alt-M inserts text");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_f1_submits(void) {
  TEST("key binding F1 submits");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_can_edit_buffer_and_submit(void) {
  TEST("key binding can edit buffer and submit");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_can_cancel(void) {
  TEST("key binding can cancel");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_enter_can_pass_to_default(void) {
  TEST("key binding Enter can pass to default");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_ctrl_enter_can_call_submit(void) {
  TEST("key binding Ctrl-Enter can call submit");
  printf("SKIP\n");
  tests_passed++;
}
static void test_next_prompt_retains_raw_input_between_turns(void) {
  TEST("next_prompt preserves immediate Ctrl-Enter between turns");
  printf("SKIP\n");
  tests_passed++;
}
static void test_key_binding_ctrl_r_overrides_history_search(void) {
  TEST("key binding Ctrl-R overrides history search");
  printf("SKIP\n");
  tests_passed++;
}
static void test_terminal_mode_is_restored(void) {
  TEST("readline restores terminal mode before returning");
  printf("SKIP\n");
  tests_passed++;
}
static void test_non_ctrl_c_signal_does_not_interrupt_readline(void) {
  TEST("non-Ctrl-C signal does not interrupt readline");
  printf("SKIP\n");
  tests_passed++;
}
static void test_pty_logical_line_keys(void) {
  TEST("Line navigation and deletion respect actual newlines");
  printf("SKIP\n");
  tests_passed++;
}
static void test_readline_keeps_normal_completion_with_queueing(void) {
  TEST("readline retains completion output when queueing is enabled");
  printf("SKIP\n");
  tests_passed++;
}
static void test_print_above_pulls_stream_chunks(void) {
  TEST("print_above pulls streamed chunks");
  printf("SKIP\n");
  tests_passed++;
}
static void test_word_wrap_keeps_words_intact(void) {
  TEST("word wrap reflows at word boundaries");
  printf("SKIP\n");
  tests_passed++;
}
static void test_word_wrap_cursor_at_skipped_space(void) {
  TEST("word wrap preserves cursor at skipped space");
  printf("SKIP\n");
  tests_passed++;
}
static void test_active_word_wrap_does_not_duplicate_prompt(void) {
  TEST("active word wrap does not duplicate prompt");
  printf("SKIP\n");
  tests_passed++;
}
static void test_active_exact_width_row_does_not_autowrap_prompt(void) {
  TEST("active exact-width row does not autowrap prompt");
  printf("SKIP\n");
  tests_passed++;
}
static void test_normal_prompt_wraps_at_bottom_with_long_prompt(void) {
  TEST("normal prompt wraps at bottom with long prompt");
  printf("SKIP\n");
  tests_passed++;
}
static void test_normal_prompt_growth_scrolls_at_screen_bottom(void) {
  TEST("normal prompt growth scrolls at screen bottom");
  printf("SKIP\n");
  tests_passed++;
}
static void test_resize_reflows_without_keypress(void) {
  TEST("resize reflows while readline is idle");
  printf("SKIP\n");
  tests_passed++;
}
static void test_visual_up_moves_across_wrapped_rows(void) {
  TEST("up arrow moves across wrapped visual rows");
  printf("SKIP\n");
  tests_passed++;
}
static void test_native_begin_failure_restores_termios(void) {
  TEST("failed native begin restores termios ownership");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_c_interrupts_child(int native) {
  (void)native;
  TEST("Ctrl-C interrupts active readline");
  printf("SKIP\n");
  tests_passed++;
}
static void test_ctrl_c_signal_is_not_delivered_twice(int native) {
  (void)native;
  TEST("Ctrl-C is raised once after raw cleanup");
  printf("SKIP\n");
  tests_passed++;
}
static void test_idle_callback_can_submit_without_input(void) {
  TEST("idle callback can submit without input");
  printf("SKIP\n");
  tests_passed++;
}
static void test_idle_callback_runs_with_quiet_watch(void) {
  TEST("idle callback runs while an external watch is quiet");
  printf("SKIP\n");
  tests_passed++;
}
static void test_idle_callback_can_cancel_without_input(void) {
  TEST("idle callback can cancel without input");
  printf("SKIP\n");
  tests_passed++;
}
static void test_narrow_terminal_does_not_submit_before_enter(void) {
  TEST("narrow terminal does not submit before Enter");
  printf("SKIP\n");
  tests_passed++;
}
static void test_live_output_end_restores_unbounded_cursor(void) {
  TEST("ending unbounded live output restores the active editor cursor");
  printf("SKIP\n");
  tests_passed++;
}
static void test_unbounded_finite_output_preserves_active_prompt(void) {
  TEST("unbounded finite output preserves an active editor between sessions");
  printf("SKIP\n");
  tests_passed++;
}
static void test_idle_callback_prints_above_active_prompt(void) {
  TEST("idle callback prints above active prompt");
  printf("SKIP\n");
  tests_passed++;
}
static void test_normal_prompt_pins_at_bottom_for_live_output(void) {
  TEST("normal prompt pins at bottom for live output");
  printf("SKIP\n");
  tests_passed++;
}
static void test_cursor_probe_preserves_concurrent_queue_input(void) {
  TEST("cursor probe preserves concurrent queue input");
  printf("SKIP\n");
  tests_passed++;
}
static void test_utf8_input_and_backspace(void) {
  TEST("UTF-8 input is preserved and backspace is character-wide");
  printf("SKIP\n");
  tests_passed++;
}
static void test_utf8_swedish_input_is_rendered_as_full_sequences(void) {
  TEST("UTF-8 Swedish input renders as full sequences");
  printf("SKIP\n");
  tests_passed++;
}
static void test_unicode_width_wraps_japanese_and_emoji(void) {
  TEST("Unicode width wraps Japanese and emoji");
  printf("SKIP\n");
  tests_passed++;
}
static void test_unicode_backspace_deletes_clusters(void) {
  TEST("Unicode backspace deletes combining and emoji clusters");
  printf("SKIP\n");
  tests_passed++;
}

static void test_live_output_reconciles_physical_resize(void) {
  TEST("live output reconciles a physical resize without an active prompt");
  printf("SKIP\n");
  tests_passed++;
}

static void test_live_output_resize_continues_current_row(void) {
  TEST("live output width resize continues current row without scrolling");
  printf("SKIP\n");
  tests_passed++;
}

static void test_live_output_clears_promptless_scroll_row(void) {
  TEST("promptless native scroll clears stale bottom-row text");
  printf("SKIP\n");
  tests_passed++;
}

static void test_live_output_error_resets_terminal_style(void) {
  TEST("invalid live output resets terminal styling before returning");
  printf("SKIP\n");
  tests_passed++;
}
#endif

int main(int argc, char **argv) {
  printf("softline unit tests\n");
  printf("===================\n\n");
  if (argc == 2 && strcmp(argv[1], "large-drafts") == 0) {
    test_large_draft_limits();
    return tests_passed == tests_run ? 0 : 1;
  }
#if SL_TEST_PTY
  if (argc == 2 && strcmp(argv[1], "wrapped-output-prompt") == 0) {
    test_normal_wrapped_prompt_proceeds_after_output();
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "finite-output-prompt") == 0) {
    test_finite_output_then_readline_preserves_transcript();
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "readline-edit") == 0) {
    test_readline_edit_commands();
    return tests_passed == tests_run ? 0 : 1;
  }
#endif
  if (argc == 2 && strcmp(argv[1], "line-keys") == 0) {
    test_pty_logical_line_keys();
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "reverse-search") == 0) {
    test_ctrl_r_searches_memory_history();
    test_ctrl_r_repeats_and_wraps_matches();
    test_ctrl_r_no_match_and_cancel_restore_draft();
    test_ctrl_r_searches_loaded_history_file();
    test_ctrl_r_searches_unicode_and_multiline_history();
    test_live_output_preserves_reverse_search_prompt(0);
    test_live_output_preserves_reverse_search_prompt(1);
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "native") == 0) {
    test_native_output_preserves_source_bytes();
    test_native_tab_expansion();
    test_native_output_without_reported_size();
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "review") == 0) {
    test_live_output_retains_row_with_full_editor(1);
    test_finite_output_recovers_from_partial_sequences();
    return tests_passed == tests_run ? 0 : 1;
  }
  if (argc == 2 && strcmp(argv[1], "cleanup") == 0) {
    test_native_begin_failure_restores_termios();
    test_ctrl_c_interrupts_child(1);
    test_ctrl_c_signal_is_not_delivered_twice(1);
    return tests_passed == tests_run ? 0 : 1;
  }

  test_config_init();
  test_large_draft_limits();
  test_receiver_shell();
  test_status_message_api();
  test_parser_only_stream_allows_geometry_changes();
  test_live_output_retains_row_with_full_editor(0);
  test_live_output_retains_row_with_full_editor(1);
  test_finite_output_recovers_from_partial_sequences();
  test_free_function_wrappers_use_receiver_methods();
  test_prompt_queue_control_api();
  test_set_cursor_clamps_to_utf8_cluster_boundary();
  test_destroy_null();
  test_invalid_config_is_rejected();
  test_invalid_receiver_arguments();
  test_plain_readline();
  test_plain_next_prompt_is_direct();
  test_plain_readline_uses_default_prompt();
  test_plain_readline_consumes_crlf_once();
  test_plain_readline_empty_eof_returns_null();
  test_plain_readline_retries_eintr();
  test_plain_readline_stops_at_line_max();
  test_readline_status_is_per_instance();
  test_history_is_per_instance();
  test_history_io_failures_are_reported();
  test_history_save_uses_private_permissions();
  test_history_round_trips_multiline_entries();
  test_history_rejects_oversized_entries();
  test_stream_failures_are_reported();
  test_print_above_uses_lf_for_non_tty_output();
  test_pty_enter_and_ctrl_j();
  test_pty_readline_uses_default_prompt();
  test_pty_readline_stops_at_line_max();
  test_normal_prompt_proceeds_after_output();
  test_pty_ctrl_d_exits();
  test_pty_history_navigation_restores_draft();
  test_pty_readline_does_not_auto_add_history();
  test_ctrl_r_searches_memory_history();
  test_ctrl_r_repeats_and_wraps_matches();
  test_ctrl_r_no_match_and_cancel_restore_draft();
  test_ctrl_r_searches_loaded_history_file();
  test_ctrl_r_searches_unicode_and_multiline_history();
  test_bracketed_paste_is_literal_content();
  test_bracketed_paste_stops_at_line_max();
  test_tab_render_expands_beyond_line_bytes();
  test_key_binding_tab_inserts_text();
  test_prompt_queue_dispatches_fifo_with_themes();
  test_prompt_queue_dispatches_fifo_in_normal_scrollback();
  test_prompt_queue_clips_control_rows_on_narrow_terminals();
  test_prompt_queue_previews_control_bytes_safely();
  test_prompt_queue_rejects_reduced_capacity();
  test_prompt_queue_alt_e_recalls_newest();
  test_prompt_themes_style_normal_readline();
  test_statusline_uses_palette_offset_and_truncation();
  test_default_statusline_uses_ansi_palette();
  test_queueing_resets_history_navigation();
  test_key_binding_alt_m_inserts_text();
  test_key_binding_f1_submits();
  test_key_binding_can_edit_buffer_and_submit();
  test_key_binding_can_cancel();
  test_key_binding_enter_can_pass_to_default();
  test_key_binding_ctrl_enter_can_call_submit();
  test_next_prompt_retains_raw_input_between_turns();
  test_queued_turns_profile_promotes_manually();
  test_queued_turns_cancellation_pauses_auto_delivery();
  test_key_binding_ctrl_r_overrides_history_search();
#if SL_TEST_PTY
  test_prompt_reset_keys();
#endif
  test_terminal_mode_is_restored();
  test_non_ctrl_c_signal_does_not_interrupt_readline();
  test_pty_logical_line_keys();
  test_readline_keeps_normal_completion_with_queueing();
  test_print_above_pulls_stream_chunks();
  test_word_wrap_keeps_words_intact();
  test_word_wrap_cursor_at_skipped_space();
  test_active_word_wrap_does_not_duplicate_prompt();
  test_active_exact_width_row_does_not_autowrap_prompt();
  test_normal_prompt_wraps_at_bottom_with_long_prompt();
  test_normal_prompt_growth_scrolls_at_screen_bottom();
  test_resize_reflows_without_keypress();
  test_visual_up_moves_across_wrapped_rows();
  test_native_begin_failure_restores_termios();
  test_ctrl_c_interrupts_child(0);
  test_ctrl_c_interrupts_child(1);
  test_ctrl_c_signal_is_not_delivered_twice(0);
  test_ctrl_c_signal_is_not_delivered_twice(1);
  test_idle_callback_can_submit_without_input();
  test_idle_callback_runs_with_quiet_watch();
  test_busy_spinner_ticks_with_quiet_watch();
  test_idle_callback_can_cancel_without_input();
  test_watch_prints_while_editing();
  test_watch_preserves_escape_continuation_timeout();
  test_queued_turns_auto_dispatches_on_watch();
  test_queued_turns_preserve_alt_enter_during_completion();
  test_queued_turns_prioritize_input_arriving_during_completion();
  test_watch_completion_preserves_input_for_next_prompt();
  test_watch_fairness_rotates_ready_flood();
  test_watch_lifecycle_reports_terminal_events();
  test_redirected_live_output_validates_stream();
  test_quoted_prompt_output_api();
  test_quoted_prompt_long_unbroken_word();
  test_quoted_prompt_chunk_boundary();
  test_native_output_preserves_source_bytes();
  test_native_tab_expansion();
  test_native_output_without_reported_size();
  test_quoted_prompt_terminal_style();
  test_quoted_prompt_resets_inherited_style();
  test_quoted_prompt_spacing_across_sessions();
  test_redirected_quoted_prompt_spacing_across_sessions();
  test_redirected_quote_spacing_after_finite_output();
  test_quoted_prompt_large_whitespace_run();
  test_quote_spacing_after_partial_invalid_write();
  test_unbounded_finite_output_between_sessions();
  test_unbounded_stream_then_readline_preserves_transcript();
  test_multiline_stream_end_preserves_transcript_position();
  test_live_output_preserves_reverse_search_prompt(0);
  test_live_output_preserves_reverse_search_prompt(1);
  test_live_output_after_readline_submit_clears_editor();
  test_retained_stream_tracks_readline_scrollback();
  test_retained_stream_survives_prompt_growth();
  test_retained_stream_survives_prompt_shrink();
  test_live_output_native_scroll_resets_prompt_style();
  test_live_output_prompt_growth_preserves_history();
  test_live_output_stream_chunk_protocol();
  test_live_output_error_resets_terminal_style();
  test_native_output_ignores_editor_width();
  test_live_output_reconciles_physical_resize();
  test_live_output_resize_continues_current_row();
  test_live_output_clears_promptless_scroll_row();
  test_final_render_failure_reports_error();
  test_narrow_terminal_does_not_submit_before_enter();
  test_live_output_end_restores_unbounded_cursor();
  test_unbounded_finite_output_preserves_active_prompt();
  test_idle_callback_prints_above_active_prompt();
  test_normal_prompt_pins_at_bottom_for_live_output();
  test_pinned_stream_failure_restores_prompt_cursor();
  test_cursor_probe_preserves_concurrent_queue_input();
  test_utf8_input_and_backspace();
  test_utf8_swedish_input_is_rendered_as_full_sequences();
  test_unicode_width_wraps_japanese_and_emoji();
  test_unicode_backspace_deletes_clusters();

  printf("\n%d/%d tests passed\n", tests_passed, tests_run);
  return tests_passed == tests_run ? 0 : 1;
}
