#define _POSIX_C_SOURCE 200809L

#include "softline/softline.h"
#include <libmdf/mdf.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* A gated producer: each command advances a bounded source chunk. The ACK
 * follows all terminal writes, so the driver can resize at a known seam. */
static const char *const documents[] = {
    "# Heading\n\nA short paragraph with *italic*, **bold**, and `code`.\n\n"
    "## Finish\n\nEND short.\n",
    "# Lists\n\n- First item has several words that wrap across columns.\n"
    "- Second item stays distinct.\n\n1. Ordered first.\n2. Ordered second.\n\n"
    "> A quoted paragraph.\n\nEND lists.\n",
    "# Code and Unicode\n\n```c\nint value = 42;\nreturn value;\n```\n\n"
    "Räksmörgås café e\314\201 中文 日本語.\n\n"
    "A longer paragraph has different word lengths, punctuation, and enough "
    "text to cross multiple terminal rows during the width transitions.\n\n"
    "END unicode.\n"};

/* Continue a soft-wrapped line across simultaneous width/height growth. */
static const char growth_line[] =
    "BEGIN abcdefghijklmnopqrstuvwxyz abcdefghijklmnopqrstuvwxyz "
    "abcdefghijklmnopqrstuvwxyz END!";

static const char *const quoted_prompts[] = {"hello", "poih", "café"};

struct fixture {
  sl_t *sl;
  mdf *renderer;
  const char *source;
  const char *quoted_prompt;
  size_t offset;
  int line_open;
  int escape;
  int commands;
  int acknowledgments;
};

/* Exercise the live-output API before its first editor frame. */
static int producer_only(struct fixture *fixture, int document) {
  static const char *const text[] = {"abcdefghijklmnopqrstuvwxyz", "one\nABC",
                                     "caf\303\251 \344\270\255\346\226\207"};
  char command = 'r';
  if (sl_output_stream_begin(fixture->sl) != SL_OK)
    return 2;
  for (;;) {
    struct winsize size;
    unsigned char reply[6];
    int result = SL_OK;
    ssize_t received;
    if (command == 's')
      result = sl_output_stream_write(fixture->sl, text[document],
                                      strlen(text[document]));
    else if (command == 'p')
      result = sl_output_stream_write(fixture->sl, "END", 3);
    else if (command == 'w')
      result = sl_set_screen_width(fixture->sl, 12);
    else if (command == 'i') {
      char *line = sl_readline(fixture->sl, "> ");
      result = line && strcmp(line, "draft") == 0 ? SL_OK : SL_ERROR;
      sl_free_string(fixture->sl, line);
    } else if (command == 'x')
      return sl_output_stream_end(fixture->sl) == SL_OK ? 0 : 2;
    if (result != SL_OK || ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0)
      return 2;
    reply[0] = 'a';
    reply[1] = (unsigned char)(size.ws_col >> 8);
    reply[2] = (unsigned char)size.ws_col;
    reply[3] = (unsigned char)(size.ws_row >> 8);
    reply[4] = (unsigned char)size.ws_row;
    reply[5] = 0;
    if (write(fixture->acknowledgments, reply, sizeof(reply)) !=
        (ssize_t)sizeof(reply))
      return 2;
    do {
      received = read(fixture->commands, &command, 1);
    } while (received < 0 && errno == EINTR);
    if (received != 1)
      return 2;
  }
}

static int sink(void *userdata, const char *bytes, size_t length) {
  struct fixture *fixture = userdata;
  size_t i;
  if (sl_output_stream_write(fixture->sl, bytes, length) != SL_OK)
    return -1;
  for (i = 0; i < length; i++) {
    unsigned char ch = (unsigned char)bytes[i];
    if (fixture->escape) {
      if (ch == 'm')
        fixture->escape = 0;
    } else if (ch == 27) {
      fixture->escape = 1;
    } else if (ch == '\n' || ch == '\r') {
      fixture->line_open = 0;
    } else if (ch >= 32 || ch == '\t') {
      fixture->line_open = 1;
    }
  }
  return 0;
}

static int ready(sl_t *sl, const sl_watch_event_t *event, void *userdata) {
  struct fixture *fixture = userdata;
  struct winsize size;
  char command;
  unsigned char acknowledgment[6];
  int result = SL_OK;
  const char *elements[] = {"fixture"};
  int pause;
  (void)event;
  if (read(fixture->commands, &command, 1) != 1)
    return SL_ERROR_IO;
  pause = command == 'b';
  if (command == 's') {
    size_t remaining = strlen(fixture->source + fixture->offset);
    size_t length = remaining > 7 ? 7 : remaining;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
        fixture->renderer->set_geometry(fixture->renderer, size.ws_col, 0, 0) !=
            MDF_OK ||
        fixture->renderer->feed(fixture->renderer,
                                fixture->source + fixture->offset,
                                length) != MDF_OK)
      return SL_ERROR_IO;
    fixture->offset += length;
  } else if (command == 'f') {
    if (fixture->renderer->finish_document(fixture->renderer) != MDF_OK)
      return SL_ERROR_IO;
    result = sl_set_status_message(sl, NULL);
  } else if (command == 'r') {
    result = sl_set_status_message(sl, "Thinking...");
  } else if (command == 'u') {
    result = sl_output_stream_write(sl, growth_line, sizeof(growth_line) - 1);
    fixture->line_open = 1;
  } else if (command == 'p') {
    result = sl_output_stream_write(sl, "AFTER RESIZE", 12);
    fixture->line_open = 1;
  } else if (command == 'h') {
    result = sl_output_stream_write_quoted_prompt(sl, fixture->quoted_prompt);
    if (result == SL_OK)
      result = sl_output_stream_write(sl, "~", 1);
    fixture->line_open = 1;
  } else if (command == 'b') {
    /* Hold the event loop across the driver's native resize capture. tmux
     * resumes stopped pane processes, so SIGSTOP is not a reliable barrier. */
  } else if (command == 'v') {
    result = sl_set_status_elements(sl, elements, 1);
  } else if (command == 'q') {
    result = sl_prompt_queue_append(sl, "queued message wraps across columns",
                                    0, NULL);
  } else if (command == 'g') {
    result = sl_set_status_message(sl, "Goodbye.");
  } else {
    return SL_ERROR_INVALID;
  }
  command =
      command == 's' && fixture->source[fixture->offset] == '\0' ? 'd' : 'a';
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0)
    return SL_ERROR_IO;
  acknowledgment[0] = (unsigned char)command;
  acknowledgment[1] = (unsigned char)(size.ws_col >> 8);
  acknowledgment[2] = (unsigned char)size.ws_col;
  acknowledgment[3] = (unsigned char)(size.ws_row >> 8);
  acknowledgment[4] = (unsigned char)size.ws_row;
  acknowledgment[5] = (unsigned char)fixture->line_open;
  if (result != SL_OK ||
      write(fixture->acknowledgments, acknowledgment, sizeof(acknowledgment)) !=
          (ssize_t)sizeof(acknowledgment))
    return SL_ERROR_IO;
  if (pause && (read(fixture->commands, &command, 1) != 1 || command != 'b'))
    return SL_ERROR_IO;
  return SL_OK;
}

int main(int argc, char **argv) {
  struct fixture fixture = {0};
  sl_config_t config;
  sl_watch_id_t watch;
  mdf_options options;
  mdf_sink output;
  char *line;
  int document;
  const char *elements[] = {"fixture"};
  if (argc != 4 && !(argc == 5 && strcmp(argv[4], "--producer-only") == 0))
    return 2;
  document = atoi(argv[3]);
  if (document < 0 ||
      document >= (int)(sizeof(documents) / sizeof(documents[0])))
    return 2;
  fixture.source = documents[document];
  fixture.quoted_prompt = quoted_prompts[document];
  fixture.commands = open(argv[1], O_RDWR);
  fixture.acknowledgments = open(argv[2], O_RDWR);
  sl_config_init(&config);
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  config.prompt_queue = 1;
  config.prompt_queue_display_delay_ms = 0;
  config.statusline = argc == 4;
  fixture.sl = sl_create_with_config(&config);
  if (argc == 5) {
    int result;
    if (fixture.commands < 0 || fixture.acknowledgments < 0 || !fixture.sl)
      return 2;
    result = producer_only(&fixture, document);
    sl_destroy(fixture.sl);
    close(fixture.commands);
    close(fixture.acknowledgments);
    return result;
  }
  mdf_options_init(&options);
  options.output_fd = STDOUT_FILENO;
  options.width = 80;
  if (fixture.commands < 0 || fixture.acknowledgments < 0 || !fixture.sl ||
      mdf_create(MDF_FORMAT_ANSI, &options, &fixture.renderer) != MDF_OK)
    return 2;
  output.userdata = &fixture;
  output.write = sink;
  if (fixture.renderer->set_sink(fixture.renderer, &output) != MDF_OK ||
      sl_set_status_elements(fixture.sl, elements, 1) != SL_OK ||
      sl_set_status_message(fixture.sl, "Thinking...") != SL_OK ||
      sl_output_stream_begin(fixture.sl) != SL_OK ||
      sl_watch_add(fixture.sl, fixture.commands, SL_WATCH_READ, ready, &fixture,
                   &watch) != SL_OK)
    return 2;
  line = sl_readline(fixture.sl, "> ");
  sl_free_string(fixture.sl, line);
  fixture.renderer->destroy(fixture.renderer);
  sl_destroy(fixture.sl);
  close(fixture.commands);
  close(fixture.acknowledgments);
  return 0;
}
