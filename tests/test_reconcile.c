#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <pty.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

struct active_state {
  int slave, result;
};

static void submit(sl_t *sl, void *data) {
  struct active_state *state = data;
  struct winsize size;
  (void)sl_set_buffer(sl, "original draft");
  (void)sl_set_cursor(sl, 3);
  memset(&size, 0, sizeof(size));
  size.ws_col = 73;
  size.ws_row = 22;
  state->result = ioctl(state->slave, TIOCSWINSZ, &size) != 0 ||
                  sl_output_stream_reconcile(sl) != SL_OK ||
                  strcmp(sl_buffer(sl), "original draft") != 0 ||
                  sl_cursor(sl) != 3;
  (void)sl_submit(sl);
}

static int same_modes(const struct termios *a, const struct termios *b) {
  return a->c_iflag == b->c_iflag && a->c_oflag == b->c_oflag &&
         a->c_cflag == b->c_cflag && a->c_lflag == b->c_lflag &&
         memcmp(a->c_cc, b->c_cc, sizeof(a->c_cc)) == 0;
}

static int run_case(int ansi) {
  int master, slave;
  sl_config_t config;
  sl_t *sl;
  char *line, *queued;
  char output[32768];
  size_t used = 0;
  ssize_t count;
  struct termios original, before, after;
  struct winsize size;
  struct active_state active;
  const char *prefix = ansi ? "\033[3" : "\342";
  const char *suffix = ansi ? "2mCOLOR\033[0m\n" : "\224\200\n";
  const char *needle = ansi ? "\033[32mCOLOR" : "\342\224\200";
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
  CHECK(tcgetattr(slave, &original) == 0);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.prompt_queue_max_entries = 4;
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_output_stream_reconcile(sl) == SL_ERROR_INVALID);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  active.slave = slave;
  active.result = 0;
  CHECK(sl_set_idle_callback(sl, submit, &active) == SL_OK);
  line = sl_next_prompt(sl, "custom> ", NULL);
  CHECK(line && strcmp(line, "original draft") == 0 && !active.result);
  sl_free_string(sl, line);
  CHECK(sl_set_idle_callback(sl, NULL, NULL) == SL_OK);
  CHECK(sl_set_buffer(sl, "preserved draft") == SL_OK &&
        sl_set_cursor(sl, 4) == SL_OK);
  CHECK(sl_prompt_queue_append(sl, "queued turn", 0, NULL) == SL_OK);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(sl_history_add(sl, "history sentinel") == SL_OK);
  CHECK(sl_output_stream_write(sl, "PRODUCER_SENTINEL\n", 18) == SL_OK);
  CHECK(tcgetattr(slave, &before) == 0);
  size.ws_col = 80;
  size.ws_row = 30;
  CHECK(ioctl(slave, TIOCSWINSZ, &size) == 0);
  CHECK(sl_output_stream_write(sl, prefix, strlen(prefix)) == SL_OK);
  CHECK(tcgetattr(slave, &after) == 0 && same_modes(&before, &after));
  size.ws_col = 61;
  size.ws_row = 18;
  CHECK(ioctl(slave, TIOCSWINSZ, &size) == 0);
  /* Reflow leaves queued input for the next read, including submit. */
  CHECK(write(master, "typed\r", 6) == 6);
  CHECK(sl->output_stream_reconcile(sl) == SL_OK);
  CHECK(tcgetattr(slave, &after) == 0 && same_modes(&before, &after));
  CHECK(strcmp(sl_buffer(sl), "preserved draft") == 0 && sl_cursor(sl) == 4);
  CHECK(sl_last_readline_status(sl) == SL_READLINE_SUBMITTED);
  CHECK(sl_prompt_queue_count(sl) == 1 &&
        sl_prompt_queue_peek(sl, 0, &queued) == SL_OK);
  CHECK(strcmp(queued, "queued turn") == 0);
  sl_free_string(sl, queued);
  CHECK(sl_output_stream_reconcile(sl) ==
        SL_OK); /* Stable geometry, no reply. */
  CHECK(sl_output_stream_write(sl, suffix, strlen(suffix)) == SL_OK);
  line = sl_next_prompt(sl, "custom> ", NULL);
  CHECK(line && strcmp(line, "typed") == 0);
  sl_free_string(sl, line);
  CHECK(write(master, "\033[A\r", 4) == 4);
  line = sl_next_prompt(sl, "custom> ", NULL);
  CHECK(line && strcmp(line, "history sentinel") == 0);
  sl_free_string(sl, line);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  CHECK(sl_output_stream_reconcile(sl) == SL_ERROR_INVALID);
  CHECK(tcgetattr(slave, &after) == 0 && same_modes(&original, &after));
  sl_destroy(sl);
  CHECK(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
  while ((count = read(master, output + used, sizeof(output) - used - 1)) > 0)
    used += (size_t)count;
  output[used] = '\0';
  CHECK(strstr(output, needle) != NULL);
  CHECK(strstr(strstr(output, needle) + strlen(needle), needle) == NULL);
  CHECK(strstr(output, "PRODUCER_SENTINEL") != NULL);
  CHECK(strstr(strstr(output, "PRODUCER_SENTINEL") + 17, "PRODUCER_SENTINEL") ==
        NULL);
  CHECK(strstr(output, "custom> ") != NULL);
  CHECK(strstr(output, "\033[6n") == NULL);
  close(master);
  close(slave);
  return 0;
}

static int redirected(void) {
  int input[2], output[2];
  sl_config_t config;
  sl_t *sl;
  char bytes[8];
  CHECK(pipe(input) == 0 && pipe(output) == 0);
  sl_config_init(&config);
  config.input_fd = input[0];
  config.output_fd = output[1];
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_output_stream_begin(sl) == SL_OK);
  CHECK(sl_output_stream_write(sl, "\342", 1) == SL_OK);
  CHECK(sl_output_stream_reconcile(sl) == SL_OK);
  CHECK(sl_output_stream_write(sl, "\224\200", 2) == SL_OK);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  CHECK(read(output[0], bytes, sizeof(bytes)) == 3);
  CHECK(memcmp(bytes, "\342\224\200", 3) == 0);
  sl_destroy(sl);
  close(input[0]);
  close(input[1]);
  close(output[0]);
  close(output[1]);
  return 0;
}

static int without_frame(void) {
  int master, slave;
  sl_config_t config;
  sl_t *sl;
  struct winsize size;
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_output_stream_begin(sl) == SL_OK);
  CHECK(sl_output_stream_write(sl, "unframed", 8) == SL_OK);
  size.ws_col = 60;
  CHECK(ioctl(slave, TIOCSWINSZ, &size) == 0);
  CHECK(sl_output_stream_reconcile(sl) == SL_OK);
  CHECK(sl_output_stream_write(sl, " continuation\n", 14) == SL_OK);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  close(master);
  close(slave);
  return 0;
}

int main(void) {
  CHECK(sl_output_stream_reconcile(NULL) == SL_ERROR_INVALID);
  CHECK(run_case(0) == 0);
  CHECK(run_case(1) == 0);
  CHECK(redirected() == 0);
  CHECK(without_frame() == 0);
  return 0;
}
