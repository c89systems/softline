#define _POSIX_C_SOURCE 200809L
#include "softline/softline.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t interrupt_count;

static void returned_interrupt(int signum) {
  (void)signum;
  interrupt_count++;
}

static int same_input_mode(const struct termios *before,
                           const struct termios *after) {
  return before->c_iflag == after->c_iflag &&
         before->c_oflag == after->c_oflag &&
         before->c_cflag == after->c_cflag &&
         before->c_lflag == after->c_lflag &&
         memcmp(before->c_cc, after->c_cc, sizeof(before->c_cc)) == 0;
}

static int interrupted(int handler) {
  sl_t *sl;
  char *input;
  int result;
  struct termios before, after;
  if (tcgetattr(STDIN_FILENO, &before) != 0 ||
      signal(SIGINT, handler ? returned_interrupt : SIG_IGN) == SIG_ERR)
    return 2;
  sl = sl_create();
  if (!sl || sl_output_stream_begin(sl) != SL_OK)
    return 2;
  input = sl_readline(sl, "> ");
  result = !input && sl_last_readline_status(sl) == SL_READLINE_INTERRUPTED &&
                   tcgetattr(STDIN_FILENO, &after) == 0 &&
                   same_input_mode(&before, &after) &&
                   interrupt_count == (handler ? 1 : 0)
               ? SL_OK
               : SL_ERROR;
  sl_free_string(sl, input);
  if (result == SL_OK)
    result = sl_output_stream_write(sl, "AFTER INTERRUPT", 15);
  if (result == SL_OK)
    result = sl_output_stream_end(sl);
  sl_destroy(sl);
  if (tcgetattr(STDIN_FILENO, &after) != 0 || !same_input_mode(&before, &after))
    return 2;
  return result == SL_OK ? 0 : 1;
}

static int stream_chunks(const char *text, const char *chunk_text,
                         int handoff) {
  sl_t *sl;
  size_t offset, length, chunk;
  int result;
  chunk = (size_t)strtoul(chunk_text, NULL, 10);
  if (chunk == 0)
    return 2;
  sl = sl_create();
  if (!sl)
    return 2;
  length = strlen(text);
  result = sl_output_stream_begin(sl);
  for (offset = 0; result == SL_OK && offset < length;) {
    size_t take = length - offset;
    if (take > chunk)
      take = chunk;
    result = sl_output_stream_write(sl, text + offset, take);
    offset += take;
  }
  if (result == SL_OK && handoff) {
    char *input = sl_readline(sl, "> ");
    result = input && !input[0] ? SL_OK : SL_ERROR;
    sl_free_string(sl, input);
    if (result == SL_OK)
      result = sl_output_stream_write(sl, "Y", 1);
  }
  if (result == SL_OK)
    result = sl_output_stream_end(sl);
  sl_destroy(sl);
  return result == SL_OK ? 0 : 1;
}

static int consecutive_streams(void) {
  static const char *messages[] = {"FIRST\r", "SECOND\n", "THIRD\n"};
  sl_t *sl = sl_create();
  size_t i;
  int result = SL_OK;
  if (!sl)
    return 2;
  for (i = 0; i < sizeof(messages) / sizeof(messages[0]) && result == SL_OK;
       i++) {
    result = sl_output_stream_begin(sl);
    if (result == SL_OK)
      result = sl_output_stream_write(sl, messages[i], strlen(messages[i]));
    if (result == SL_OK)
      result = sl_output_stream_end(sl);
  }
  sl_destroy(sl);
  return result == SL_OK ? 0 : 1;
}

static int quoted_after(const char *first) {
  sl_t *sl = sl_create();
  int result;
  if (!sl)
    return 2;
  result = sl_output_stream_begin(sl);
  if (result == SL_OK)
    result = sl_output_stream_write(sl, first, strlen(first));
  if (result == SL_OK)
    result = sl_output_stream_end(sl);
  if (result == SL_OK)
    result = sl_output_stream_begin(sl);
  if (result == SL_OK)
    result = sl_output_stream_write_quoted_prompt(sl, "NEXT");
  if (result == SL_OK)
    result = sl_output_stream_end(sl);
  sl_destroy(sl);
  return result == SL_OK ? 0 : 1;
}

static int quote_spacing(const char *initial, const char *between,
                         const char *outside) {
  sl_t *sl;
  int result;
  struct termios before, after;
  if (tcgetattr(STDIN_FILENO, &before) != 0 ||
      write(STDOUT_FILENO, initial, strlen(initial)) !=
          (ssize_t)strlen(initial))
    return 2;
  sl = sl_create();
  if (!sl)
    return 2;
  result = sl_output_stream_begin(sl);
  if (result == SL_OK)
    result = sl_output_stream_write_quoted_prompt(sl, "FIRST");
  if (result == SL_OK)
    result = sl_output_stream_write(sl, between, strlen(between));
  if (result == SL_OK)
    result = sl_output_stream_write_quoted_prompt(sl, "SECOND");
  if (result == SL_OK)
    result = *outside ? sl_output_stream_end_for_handoff(sl)
                      : sl_output_stream_end(sl);
  if (result == SL_OK && (tcgetattr(STDIN_FILENO, &after) != 0 ||
                          !same_input_mode(&before, &after) ||
                          write(STDOUT_FILENO, outside, strlen(outside)) !=
                              (ssize_t)strlen(outside)))
    result = SL_ERROR;
  if (result == SL_OK)
    result = sl_output_stream_begin(sl);
  if (result == SL_OK)
    result = sl_output_stream_write_quoted_prompt(sl, "THIRD");
  if (result == SL_OK)
    result = sl_output_stream_end(sl);
  sl_destroy(sl);
  if (tcgetattr(STDIN_FILENO, &after) != 0 || !same_input_mode(&before, &after))
    return 2;
  return result == SL_OK ? 0 : 1;
}

struct finite_source {
  const char *prefix;
  const char *source;
  size_t offset;
};

static int finite_chunk(sl_t *sl, void *userdata, const char **bytes,
                        size_t *length) {
  struct finite_source *producer = (struct finite_source *)userdata;
  size_t prefix_length = strlen(producer->prefix);
  (void)sl;
  if (producer->offset < prefix_length) {
    *bytes = producer->prefix + producer->offset;
    *length = 1;
  } else {
    *bytes = producer->source + producer->offset - prefix_length;
    *length = **bytes ? 1 : 0;
  }
  producer->offset++;
  return SL_OK;
}

struct retained_output {
  int commands, replies, result, started;
  const char *source;
};

static int retained_reply(struct retained_output *state) {
  struct winsize geometry;
  unsigned short reply[2];
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &geometry) != 0)
    return SL_ERROR_IO;
  reply[0] = geometry.ws_col;
  reply[1] = geometry.ws_row;
  return write(state->replies, reply, sizeof(reply)) == (ssize_t)sizeof(reply)
             ? SL_OK
             : SL_ERROR_IO;
}

struct paused_finite {
  struct finite_source source;
  struct retained_output *state;
  size_t gate_offset;
};

static int paused_chunk(sl_t *sl, void *userdata, const char **bytes,
                        size_t *length) {
  struct paused_finite *producer = (struct paused_finite *)userdata;
  if (producer->source.offset == producer->gate_offset) {
    char command;
    if (retained_reply(producer->state) != SL_OK ||
        read(producer->state->commands, &command, 1) != 1 || command != 'g')
      return SL_ERROR_IO;
  }
  return finite_chunk(sl, &producer->source, bytes, length);
}

static void retained_idle(sl_t *sl, void *userdata) {
  struct retained_output *state = (struct retained_output *)userdata;
  if (state->started)
    return;
  state->started = 1;
  state->result =
      sl_output_stream_write(sl, state->source, strlen(state->source));
  if (state->result == SL_OK)
    state->result = sl_output_stream_end(sl);
  while (state->result == SL_OK) {
    char command;
    struct paused_finite producer;
    /* Stay in the callback while the parent resizes, so another editor
     * iteration cannot reconcile the geometry before print_above does. */
    if (retained_reply(state) != SL_OK ||
        read(state->commands, &command, 1) != 1) {
      state->result = SL_ERROR_IO;
      break;
    }
    if (command == 'x')
      break;
    if (command != 'f' && command != 'm' && command != 'e') {
      state->result = SL_ERROR_INVALID;
      break;
    }
    producer.source.prefix = "";
    producer.source.source = "XYZ";
    producer.source.offset = 0;
    producer.state = state;
    producer.gate_offset = command == 'm' ? 1 : 3;
    state->result = command == 'f'
                        ? sl_print_above(sl, finite_chunk, &producer.source)
                        : sl_print_above(sl, paused_chunk, &producer);
  }
  (void)sl_cancel(sl);
}

static int retained(const char *command_path, const char *reply_path,
                    const char *source) {
  struct retained_output state;
  sl_t *sl;
  char *input;
  memset(&state, 0, sizeof(state));
  state.commands = open(command_path, O_RDWR);
  state.replies = open(reply_path, O_RDWR);
  state.source = source;
  if (state.commands < 0 || state.replies < 0)
    return 2;
  sl = sl_create();
  if (!sl || sl_output_stream_begin(sl) != SL_OK ||
      sl_set_idle_callback(sl, retained_idle, &state) != SL_OK)
    return 2;
  input = sl_readline(sl, "> ");
  if (input || sl_last_readline_status(sl) != SL_READLINE_CANCELLED ||
      !state.started)
    state.result = SL_ERROR;
  sl_free_string(sl, input);
  sl_destroy(sl);
  close(state.commands);
  close(state.replies);
  return state.result == SL_OK ? 0 : 1;
}

static int gated_fragment(sl_t *sl, char command) {
  switch (command) {
  case 'p':
    return sl_output_stream_write(sl, "Y", 1);
  case 'S':
    return sl_output_stream_write(sl, "\033[32m", 5);
  case 'n':
    return sl_output_stream_write(sl, "\nY", 2);
  case 'h':
    return sl_output_stream_write(sl, "\rY", 2);
  case 'a':
    return sl_output_stream_write(sl, "\033", 1);
  case 'A':
    return sl_output_stream_write(sl, "[32mCOLOR\033[0m", 13);
  case 'u':
    return sl_output_stream_write(sl, "\342", 1);
  case 'U':
    return sl_output_stream_write(sl, "\224\200", 2);
  case 'v':
    return sl_output_stream_write(sl, "\033[H", 3) == SL_ERROR_INVALID
               ? SL_OK
               : SL_ERROR;
  default:
    return SL_ERROR_INVALID;
  }
}

static int pinned_readline(const char *width, const char *expected) {
  sl_config_t config;
  sl_t *sl;
  char *line;
  int result, interrupted = !strcmp(expected, "--interrupt");
  if (interrupted && signal(SIGINT, SIG_IGN) == SIG_ERR)
    return 2;
  sl_config_init(&config);
  config.live_scroll_region = 1;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  config.screen_width = atoi(width);
  sl = sl_create_with_config(&config);
  if (!sl)
    return 2;
  line = sl_readline(sl, "> ");
  if (interrupted)
    result =
        line != NULL || sl_last_readline_status(sl) != SL_READLINE_INTERRUPTED;
  else if (!strcmp(expected, "--eof"))
    result = line != NULL || sl_last_readline_status(sl) != SL_READLINE_EOF;
  else
    result = !line || strcmp(line, expected);
  sl_free_string(sl, line);
  sl_destroy(sl);
  if (!result && puts("AFTER") == EOF)
    result = 1;
  return result;
}

struct live_gate {
  int command_fd;
  int ack_fd;
  int done;
  int result;
  int panels;
};

static void live_gate_ack(struct live_gate *gate) {
  struct winsize geometry;
  unsigned short reply[2];
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &geometry) != 0) {
    gate->result = SL_ERROR;
    return;
  }
  reply[0] = geometry.ws_col;
  reply[1] = geometry.ws_row;
  if (write(gate->ack_fd, reply, sizeof(reply)) != (ssize_t)sizeof(reply))
    gate->result = SL_ERROR;
}

static int live_gate_watch(sl_t *sl, const sl_watch_event_t *event,
                           void *userdata) {
  struct live_gate *gate = userdata;
  char command;
  (void)event;
  if (read(gate->command_fd, &command, 1) != 1)
    gate->result = SL_ERROR;
  else if (command == 'r')
    gate->result = sl_output_stream_reconcile(sl);
  else if (command == 'd') {
    struct winsize size;
    char damage[256];
    int count = 0, row;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0)
      gate->result = SL_ERROR_IO;
    else {
      count = snprintf(damage, sizeof(damage), "\033[r");
      for (row = size.ws_row - (gate->panels ? 4 : 1); row < size.ws_row; row++)
        count += snprintf(damage + count, sizeof(damage) - (size_t)count,
                          "\033[%d;1HBROKEN\033[0K", row + 1);
      gate->result = write(STDOUT_FILENO, damage, (size_t)count) == count
                         ? SL_OK
                         : SL_ERROR_IO;
    }
  } else if (command == 't') {
    const char *elements[] = {"reset-state"};
    gate->panels = 1;
    gate->result =
        sl_set_statusline(sl, 1, 0) == SL_OK &&
                sl_set_status_elements(sl, elements, 1) == SL_OK &&
                sl_set_status_message(sl, "working") == SL_OK &&
                sl_set_prompt_queue(sl, 1, 8, 3) == SL_OK &&
                sl_set_prompt_queue_display_delay(sl, 0) == SL_OK &&
                sl_set_prompt_queue_delivery(
                    sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK &&
                sl_prompt_queue_append(sl, "queued for reset", 0, NULL) == SL_OK
            ? SL_OK
            : SL_ERROR;
  } else if (command == 'b')
    gate->result = sl_set_buffer(sl, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10");
  else if (command == 'e')
    gate->result = sl_set_buffer(sl, "");
  else if (command == 'x')
    gate->done = 1;
  else
    gate->result = gated_fragment(sl, command);
  if (gate->result != SL_OK || gate->done)
    sl_cancel(sl);
  else
    live_gate_ack(gate);
  return gate->result;
}

enum gate_mode { GATED, GATED_LIVE, GATED_SHELL };

static int gated(const char *command_path, const char *ack_path,
                 const char *text, const char *prompt, enum gate_mode mode) {
  sl_t *sl;
  int command_fd, ack_fd, result = SL_OK;
  char command = 'r';
  command_fd = open(command_path, O_RDWR);
  ack_fd = open(ack_path, O_RDWR);
  if (command_fd < 0 || ack_fd < 0)
    return 2;
  sl = sl_create();
  if (!sl || sl_output_stream_begin(sl) != SL_OK)
    return 2;
  for (;;) {
    struct winsize geometry;
    unsigned short reply[2];
    if (command == 's') {
      size_t i;
      for (i = 0; result == SL_OK && text[i]; i++)
        result = sl_output_stream_write(sl, text + i, 1);
    } else if (command == 'c') {
      result = sl_output_stream_reconcile(sl);
    } else if (command == 'w') {
      result = sl_set_screen_width(sl, 12);
    } else if (command == 'f') {
      struct finite_source producer;
      producer.prefix = "F: ";
      producer.source = text;
      producer.offset = 0;
      result = sl_output_stream_end(sl);
      if (result == SL_OK)
        result = sl_print_above(sl, finite_chunk, &producer);
      if (result == SL_OK)
        result = sl_output_stream_begin(sl);
    } else if (command == 'i') {
      char *input;
      if (mode == GATED_LIVE) {
        struct live_gate gate;
        sl_watch_id_t watch;
        gate.command_fd = command_fd;
        gate.ack_fd = ack_fd;
        gate.done = 0;
        gate.result = SL_OK;
        gate.panels = 0;
        result = sl_watch_add(sl, command_fd, SL_WATCH_READ, live_gate_watch,
                              &gate, &watch);
        if (result != SL_OK)
          break;
        live_gate_ack(&gate);
        input = sl_readline(sl, prompt);
        sl_free_string(sl, input);
        sl_watch_remove(sl, watch);
        result = gate.done ? gate.result : SL_ERROR;
        if (result == SL_OK)
          result = sl_output_stream_end(sl);
        break;
      }
      input = sl_readline(sl, prompt);
      result = input && !input[0] ? SL_OK : SL_ERROR;
      sl_free_string(sl, input);
    } else if (command == 'x') {
      result = sl_output_stream_end(sl);
      break;
    } else if (command != 'r') {
      result = gated_fragment(sl, command);
    }
    if (result != SL_OK || ioctl(STDOUT_FILENO, TIOCGWINSZ, &geometry) != 0)
      break;
    reply[0] = geometry.ws_col;
    reply[1] = geometry.ws_row;
    if (mode == GATED_SHELL) {
      char message[32];
      int n = snprintf(message, sizeof(message), "%u %u\n",
                       (unsigned int)reply[0], (unsigned int)reply[1]);
      if (n < 1 || n >= (int)sizeof(message) ||
          write(ack_fd, message, (size_t)n) != n)
        result = SL_ERROR;
    } else if (write(ack_fd, reply, sizeof(reply)) != (ssize_t)sizeof(reply)) {
      result = SL_ERROR;
    }
    if (result != SL_OK || read(command_fd, &command, 1) != 1) {
      result = SL_ERROR;
      break;
    }
  }
  sl_destroy(sl);
  close(command_fd);
  close(ack_fd);
  return result == SL_OK ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "--pinned-readline") == 0)
    return pinned_readline(argv[2], argv[3]);
  if (argc == 5 && strcmp(argv[1], "--quote-spacing") == 0)
    return quote_spacing(argv[2], argv[3], argv[4]);
  if (argc == 2 && strcmp(argv[1], "--consecutive-streams") == 0)
    return consecutive_streams();
  if (argc == 3 && strcmp(argv[1], "--quoted-after") == 0)
    return quoted_after(argv[2]);
  if (argc == 5 && strcmp(argv[1], "--finite-retained") == 0)
    return retained(argv[2], argv[3], argv[4]);
  if (argc == 3 && strcmp(argv[1], "--interrupt") == 0)
    return interrupted(strcmp(argv[2], "handler") == 0);
  if (argc == 4 && strcmp(argv[1], "--chunks") == 0)
    return stream_chunks(argv[2], argv[3], 0);
  if (argc == 4 && strcmp(argv[1], "--handoff") == 0)
    return stream_chunks(argv[2], argv[3], 1);
  if (argc == 5 && strcmp(argv[1], "--gated") == 0)
    return gated(argv[2], argv[3], argv[4], "> ", GATED);
  if (argc == 6 && strcmp(argv[1], "--gated") == 0)
    return gated(argv[2], argv[3], argv[4], argv[5], GATED);
  if (argc == 5 && strcmp(argv[1], "--gated-live") == 0)
    return gated(argv[2], argv[3], argv[4], "> ", GATED_LIVE);
  if (argc == 5 && strcmp(argv[1], "--gated-shell") == 0)
    return gated(argv[2], argv[3], argv[4], "> ", GATED_SHELL);
  return 2;
}
