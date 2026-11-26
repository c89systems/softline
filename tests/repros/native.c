#define _DEFAULT_SOURCE
#include "softline/softline.h"
#include "termios_snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define SETUP(expression)                                                      \
  do {                                                                         \
    if (!(expression)) {                                                       \
      fprintf(stderr, "fixture line %d: %s\n", __LINE__, #expression);         \
      exit(2);                                                                 \
    }                                                                          \
  } while (0)
#define WRITE(sl, text) sl_output_stream_write(sl, text, sizeof(text) - 1)

static int expect(int ok, const char *description) {
  if (!ok)
    fprintf(stderr, "regression: %s\n", description);
  return !ok;
}

static int block_finish_fd = -1, blocked_finish;

ssize_t __real_write(int fd, const void *bytes, size_t length);
ssize_t __wrap_write(int fd, const void *bytes, size_t length);

ssize_t __wrap_write(int fd, const void *bytes, size_t length) {
  if (fd == block_finish_fd && length >= 7 &&
      !memcmp(bytes, "\033[r\033[0m", 7)) {
    blocked_finish++;
    errno = EAGAIN;
    return -1;
  }
  return __real_write(fd, bytes, length);
}

static sl_t *editor(int input, int output) {
  sl_config_t config;
  sl_t *sl;
  sl_config_init(&config);
  config.input_fd = input;
  config.output_fd = output;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  sl = sl_create_with_config(&config);
  SETUP(sl);
  return sl;
}

static void open_terminal(int *master, int *slave, unsigned short rows) {
  struct winsize size = {0, 40, 0, 0};
  size.ws_row = rows;
  SETUP(!openpty(master, slave, NULL, NULL, &size));
}

static void clip_editor(sl_t *sl, void *userdata) {
  int *phase = userdata;
  if (*phase == 0)
    SETUP(sl_set_buffer(sl, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12") == SL_OK);
  else if (*phase == 1)
    SETUP(sl_set_buffer(sl, "") == SL_OK);
  else
    SETUP(sl_suspend(sl) == SL_OK);
  ++*phase;
}

static int clipped_reopen(void) {
  int master, slave, phase = 0, result;
  struct winsize size = {6, 40, 0, 0};
  struct termios before, after;
  sl_t *sl;
  open_terminal(&master, &slave, 14);
  SETUP(!tcgetattr(slave, &before));
  sl = editor(slave, slave);
  SETUP(sl_output_stream_begin(sl) == SL_OK && WRITE(sl, "producer") == SL_OK);
  /* Render the large draft, shrink it, then suspend with its short frame
   * still visible. The height change clips the producer above that frame. */
  SETUP(sl_set_idle_callback(sl, clip_editor, &phase) == SL_OK);
  SETUP(!sl_readline(sl, "> ") && phase == 3 &&
        sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
  SETUP(!ioctl(master, TIOCSWINSZ, &size));
  SETUP(sl_output_stream_reconcile(sl) == SL_OK);
  block_finish_fd = slave;
  SETUP(sl_output_stream_end_for_handoff(sl) == SL_ERROR_IO && blocked_finish);
  SETUP(!tcgetattr(slave, &after) && test_termios_equal(&before, &after));
  block_finish_fd = -1;
  result = sl_output_stream_begin(sl);
  if (result == SL_OK)
    SETUP(WRITE(sl, "continuation") == SL_OK &&
          sl_output_stream_end(sl) == SL_OK);
  else
    fprintf(stderr, "output_stream_begin returned %d\n", result);
  sl_destroy(sl);
  close(slave);
  close(master);
  return expect(result == SL_OK, "a writable terminal at the saved dimensions "
                                 "must reopen after the producer was clipped");
}

static int disconnected_close(void) {
  int master, slave, output_master, output_slave, result, restored;
  struct termios before, after;
  sl_t *sl;
  open_terminal(&master, &slave, 12);
  open_terminal(&output_master, &output_slave, 12);
  SETUP(!tcgetattr(slave, &before));
  sl = editor(slave, output_slave);
  SETUP(sl_output_stream_begin(sl) == SL_OK && WRITE(sl, "producer") == SL_OK);
  SETUP(!tcgetattr(slave, &after) && !test_termios_equal(&before, &after));
  SETUP(!close(output_master));
  result = sl_output_stream_end(sl);
  SETUP(!tcgetattr(slave, &after));
  restored = test_termios_equal(&before, &after);
  if (!restored || result != SL_ERROR_IO)
    fprintf(stderr, "close result %d, input restored %d\n", result, restored);
  sl_destroy(sl);
  close(output_slave);
  close(slave);
  close(master);
  return expect(result == SL_ERROR_IO && restored,
                "closing a native stream after its output TTY disconnects must "
                "release the separate input TTY and report I/O failure");
}

struct relay {
  int disconnect;
  int sent, steps, closed;
};

static int relay_adapter(void *userdata, sl_terminal_event_t *event) {
  struct relay *relay = userdata;
  if (event->operation == SL_TERMINAL_OPEN && relay->disconnect >= 0) {
    SETUP(!close(relay->disconnect));
  } else if (event->operation == SL_TERMINAL_STEP) {
    if (relay->disconnect >= 0 && ++relay->steps == 8)
      return SL_ERROR;
    event->ready = relay->disconnect < 0;
  } else if (event->operation == SL_TERMINAL_READ && !relay->sent++) {
    static const char output[] = "child";
    SETUP(event->length >= sizeof(output) - 1);
    memcpy(event->data, output, sizeof(output) - 1);
    event->transferred = sizeof(output) - 1;
  } else if (event->operation == SL_TERMINAL_CLOSE)
    ++relay->closed;
  return SL_OK;
}

static int disconnected_relay(void) {
  int master, slave, output_master, output_slave, result;
  struct relay relay;
  struct termios before, after;
  sl_t *sl;
  open_terminal(&master, &slave, 12);
  open_terminal(&output_master, &output_slave, 12);
  SETUP(!tcgetattr(slave, &before));
  memset(&relay, 0, sizeof(relay));
  relay.disconnect = output_master;
  sl = editor(slave, output_slave);
  result = sl_terminal_handoff(sl, relay_adapter, &relay);
  SETUP(relay.closed == 1 && !tcgetattr(slave, &after) &&
        test_termios_equal(&before, &after));
  sl_destroy(sl);
  close(output_slave);
  close(slave);
  close(master);
  if (relay.steps >= 8)
    fprintf(stderr, "relay reached the fixture stop after %d STEP calls\n",
            relay.steps);
  return expect(result == SL_ERROR_IO && relay.steps < 8,
                "an idle relay must notice its output TTY disappearing even "
                "when it has no pending output");
}

static int relay_reopen(void) {
  sl_t *sl = editor(STDIN_FILENO, STDOUT_FILENO);
  struct relay relay;
  memset(&relay, 0, sizeof(relay));
  relay.disconnect = -1;
  SETUP(sl_output_stream_begin(sl) == SL_OK && WRITE(sl, "first") == SL_OK);
  SETUP(sl_output_stream_end(sl) == SL_OK);
  SETUP(sl_terminal_handoff(sl, relay_adapter, &relay) == SL_OK &&
        relay.closed == 1);
  SETUP(sl_output_stream_begin(sl) == SL_OK && WRITE(sl, "second") == SL_OK);
  SETUP(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  return 0;
}

static int zero_width_wrap(const char *commands, const char *replies) {
  struct winsize size;
  int cmd = open(commands, O_RDWR), reply = open(replies, O_RDWR);
  char byte, ready[64];
  sl_t *sl = editor(STDIN_FILENO, STDOUT_FILENO);
  SETUP(cmd >= 0 && reply >= 0);
  SETUP(sl_output_stream_begin(sl) == SL_OK);
  SETUP(WRITE(sl, "123456789012345e") == SL_OK);
  for (;;) {
    int length;
    SETUP(!ioctl(STDOUT_FILENO, TIOCGWINSZ, &size));
    length = snprintf(ready, sizeof(ready), "%u\n", (unsigned int)size.ws_row);
    SETUP(length > 0 && write(reply, ready, (size_t)length) == length);
    SETUP(read(cmd, &byte, 1) == 1);
    if (byte == 'x')
      break;
    SETUP(byte == 'r');
  }
  SETUP(WRITE(sl, "\314\201\nZ") == SL_OK);
  SETUP(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  close(reply);
  close(cmd);
  return 0;
}

struct input {
  int master, phase;
  const char *bytes;
  size_t length;
};

static void send_keys(sl_t *sl, void *userdata) {
  struct input *input = userdata;
  if (!input->phase++)
    SETUP(write(input->master, input->bytes, input->length) ==
          (ssize_t)input->length);
  else
    SETUP(sl_cancel(sl) == SL_OK);
}

static char *read_keys(sl_t *sl, int master, const char *bytes, size_t length) {
  struct input input;
  char *text;
  input.master = master;
  input.phase = 0;
  input.bytes = bytes;
  input.length = length;
  SETUP(sl_set_idle_callback(sl, send_keys, &input) == SL_OK);
  text = sl_readline(sl, "> ");
  SETUP(input.phase > 0);
  SETUP(sl_set_idle_callback(sl, NULL, NULL) == SL_OK);
  return text;
}

static int raw_enter(void) {
  int master, slave, accepted;
  struct termios attrs, after;
  char *text;
  sl_t *sl;
  open_terminal(&master, &slave, 12);
  SETUP(!tcgetattr(slave, &attrs));
  attrs.c_iflag |= IGNCR;
  SETUP(!tcsetattr(slave, TCSANOW, &attrs));
  sl = editor(slave, slave);
  text = read_keys(sl, master, "draft\r", 6);
  accepted = text && !strcmp(text, "draft");
  SETUP(!tcgetattr(slave, &after) && test_termios_equal(&attrs, &after));
  sl_free_string(sl, text);
  sl_destroy(sl);
  close(slave);
  close(master);
  return expect(accepted, "Enter must submit while Softline owns raw input "
                          "even when the original TTY ignored CR");
}

static int utf8_following_key(void) {
  int master, slave, retained;
  char *text;
  sl_t *sl;
  open_terminal(&master, &slave, 12);
  sl = editor(slave, slave);
  text = read_keys(sl, master, "\303ok\r", 4);
  retained = text && strstr(text, "ok");
  sl_free_string(sl, text);
  sl_destroy(sl);
  close(slave);
  close(master);
  return expect(
      retained,
      "a malformed UTF-8 lead byte must not consume the next printable key");
}

static int literal_paste_nul(void) {
  static const char keys[] = "\033[200~left\0right\033[201~ end\r";
  int master, slave, retained;
  char *text;
  sl_t *sl;
  open_terminal(&master, &slave, 12);
  sl = editor(slave, slave);
  text = read_keys(sl, master, keys, sizeof(keys) - 1);
  /* A C string cannot represent the pasted NUL. Reject it explicitly or
   * preserve the surrounding text without submitting a truncated prompt. */
  retained = text ? !strcmp(text, "leftright end")
                  : sl_last_readline_status(sl) == SL_READLINE_ERROR;
  if (!retained)
    fprintf(stderr, "submitted <%s>\n", text ? text : "NULL");
  sl_free_string(sl, text);
  sl_destroy(sl);
  close(slave);
  close(master);
  return expect(retained, "an embedded NUL in a bracketed paste must not "
                          "silently truncate the submitted prompt");
}

static int append_during_recall(sl_t *sl, sl_key_t key, void *userdata,
                                sl_key_action_t *action) {
  int *called = userdata;
  SETUP(key == SL_KEY_F1 && !strcmp(sl_buffer(sl), "newest"));
  SETUP(sl_history_add(sl, "incoming") == SL_OK);
  ++*called;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int history_recall_after_eviction(void) {
  static const char keys[] = "\033[A\033OP\033[A\r";
  int master, slave, called = 0, advanced;
  char *text;
  sl_t *sl;
  open_terminal(&master, &slave, 12);
  sl = editor(slave, slave);
  SETUP(sl_history_set_max_len(sl, 3) == SL_OK &&
        sl_history_add(sl, "oldest") == SL_OK &&
        sl_history_add(sl, "middle") == SL_OK &&
        sl_history_add(sl, "newest") == SL_OK &&
        sl_bind_key(sl, SL_KEY_F1, append_during_recall, &called) == SL_OK);
  text = read_keys(sl, master, keys, sizeof(keys) - 1);
  SETUP(called == 1);
  advanced = text && !strcmp(text, "middle");
  if (!advanced)
    fprintf(stderr, "recalled <%s>\n", text ? text : "NULL");
  sl_free_string(sl, text);
  sl_destroy(sl);
  close(slave);
  close(master);
  return expect(advanced, "Up from a recalled entry must move to the previous "
                          "surviving entry after history eviction");
}

static int split_crlf(void) {
  int fds[2], output, retained;
  char *text;
  sl_t *sl;
  SETUP(!pipe(fds));
  output = open("/dev/null", O_WRONLY);
  SETUP(output >= 0);
  sl = editor(fds[0], output);
  SETUP(write(fds[1], "one\r", 4) == 4);
  text = sl_readline(sl, NULL);
  SETUP(text && !strcmp(text, "one"));
  sl_free_string(sl, text);
  SETUP(write(fds[1], "\ntwo\r\n", 6) == 6 && !close(fds[1]));
  text = sl_readline(sl, NULL);
  retained = text && !strcmp(text, "two");
  if (!retained)
    fprintf(stderr, "second readline returned <%s>\n", text ? text : "NULL");
  sl_free_string(sl, text);
  sl_destroy(sl);
  close(output);
  close(fds[0]);
  return expect(retained, "CRLF must delimit one record even when LF arrives "
                          "after the first readline returns");
}

static int insert_borrowed_buffer(void) {
  sl_t *sl = sl_create();
  int correct;
  SETUP(sl && sl_set_buffer(sl, "abcdef") == SL_OK &&
        sl_set_cursor(sl, 2) == SL_OK);
  SETUP(sl_insert(sl, sl_buffer(sl) + 3) == SL_OK);
  correct = !strcmp(sl_buffer(sl), "abdefcdef");
  if (!correct)
    fprintf(stderr, "draft <%s>\n", sl_buffer(sl));
  sl_destroy(sl);
  return expect(
      correct,
      "inserting a borrowed draft suffix must copy the original suffix");
}

static int insert_cluster_boundary(void) {
  sl_t *sl = sl_create();
  size_t cursor;
  SETUP(sl && sl_set_buffer(sl, "\314\201x") == SL_OK &&
        sl_set_cursor(sl, 0) == SL_OK && sl_insert(sl, "e") == SL_OK);
  SETUP(!strcmp(sl_buffer(sl), "e\314\201x"));
  cursor = sl_cursor(sl);
  if (cursor != 3)
    fprintf(stderr, "cursor is at byte %lu in e + U+0301 + x\n",
            (unsigned long)cursor);
  sl_destroy(sl);
  return expect(cursor == 3, "inserting a base character must advance over the "
                             "resulting combining cluster");
}

static int quote_without_onlcr(void) {
  struct termios attrs;
  sl_t *sl;
  SETUP(!tcgetattr(STDOUT_FILENO, &attrs));
  attrs.c_oflag |= OPOST;
  attrs.c_oflag &= ~ONLCR;
  SETUP(!tcsetattr(STDOUT_FILENO, TCSANOW, &attrs));
  sl = editor(STDIN_FILENO, STDOUT_FILENO);
  SETUP(sl_output_stream_begin(sl) == SL_OK && WRITE(sl, "answer\n") == SL_OK);
  SETUP(sl_output_stream_write_quoted_prompt(sl, "one\ntwo") == SL_OK);
  SETUP(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  return 0;
}

static int history_record_boundary(const char *build) {
  const char raw[] = "broken\\\nvalid\n";
  char path[4096], bytes[80];
  int fd, status, have_valid, length;
  ssize_t count;
  sl_t *sl = sl_create();
  SETUP(sl);
  length = snprintf(path, sizeof(path), "%s/history-regression.XXXXXX", build);
  SETUP(length > 0 && (size_t)length < sizeof(path));
  fd = mkstemp(path);
  SETUP(fd >= 0 && write(fd, raw, sizeof(raw) - 1) == sizeof(raw) - 1 &&
        !close(fd));
  status = sl_history_load(sl, path);
  SETUP(sl_history_save(sl, path) == SL_OK);
  fd = open(path, O_RDONLY);
  SETUP(fd >= 0);
  count = read(fd, bytes, sizeof(bytes) - 1);
  SETUP(count >= 0);
  bytes[count] = 0;
  have_valid = strstr(bytes, "\nvalid\n") != NULL || !strcmp(bytes, "valid\n");
  close(fd);
  unlink(path);
  sl_destroy(sl);
  fprintf(stderr, "import status %d, snapshot <%s>\n", status, bytes);
  return expect(have_valid, "a trailing backslash in one imported record must "
                            "not consume the next record");
}

int main(int argc, char **argv) {
  SETUP(argc >= 2);
  alarm(4);
  if (!strcmp(argv[1], "clipped-handoff-reopen"))
    return clipped_reopen();
  if (!strcmp(argv[1], "disconnected-stream-close"))
    return disconnected_close();
  if (!strcmp(argv[1], "disconnected-idle-relay"))
    return disconnected_relay();
  if (!strcmp(argv[1], "relay-after-close"))
    return relay_reopen();
  if (!strcmp(argv[1], "zero-width-wrap") && argc == 4)
    return zero_width_wrap(argv[2], argv[3]);
  if (!strcmp(argv[1], "raw-enter"))
    return raw_enter();
  if (!strcmp(argv[1], "utf8-following-key"))
    return utf8_following_key();
  if (!strcmp(argv[1], "literal-paste-nul"))
    return literal_paste_nul();
  if (!strcmp(argv[1], "history-recall-after-eviction"))
    return history_recall_after_eviction();
  if (!strcmp(argv[1], "split-crlf"))
    return split_crlf();
  if (!strcmp(argv[1], "insert-borrowed-buffer"))
    return insert_borrowed_buffer();
  if (!strcmp(argv[1], "insert-cluster-boundary"))
    return insert_cluster_boundary();
  if (!strcmp(argv[1], "quote-without-onlcr"))
    return quote_without_onlcr();
  if (!strcmp(argv[1], "history-record-boundary") && argc == 3)
    return history_record_boundary(argv[2]);
  return 2;
}
