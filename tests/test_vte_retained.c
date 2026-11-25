#include "vte_output_sync.h"
#include <fcntl.h>
#include <sys/stat.h>

struct session {
  struct screen actual, direct;
  pid_t child;
  int commands, replies, slave;
  char work[4096], command_path[4096], reply_path[4096];
};
#define REQUIRE(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      struct snapshot capture = transcript(&g.actual);                         \
      fprintf(stderr, "Retained VTE assertion: %s\n%s\n", #c, capture.text);   \
      failed = 1;                                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

static void close_session(struct session *g) {
  int status;
  if (g->child > 0) {
    kill(g->child, SIGTERM);
    waitpid(g->child, &status, 0);
  }
  if (g->commands >= 0)
    close(g->commands);
  if (g->replies >= 0)
    close(g->replies);
  if (g->slave >= 0)
    close(g->slave);
  if (g->actual.window)
    v.destroy(g->actual.window);
  if (g->direct.window)
    v.destroy(g->direct.window);
  unlink(g->command_path);
  unlink(g->reply_path);
  rmdir(g->work);
}
static int start_session(struct session *g, const char *fixture,
                         const char *build, const char *mode,
                         const char *source, int filled, int homed, int tabs,
                         int rows, const char *prompt) {
  static const char initial_boundary[] = "\033[r\033[65535;1H\033D";
  char prior[4096];
  size_t used = 0;
  int master, slave, index;
  struct winsize size;
  struct termios attributes;
  void *pty;
  memset(g, 0, sizeof(*g));
  g->child = -1;
  g->commands = g->replies = g->slave = -1;
  g->actual = create();
  g->direct = create();
  if (rows != 8) {
    resize(&g->actual, 40, rows);
    resize(&g->direct, 40, rows);
  }
  if (filled || homed) {
    for (index = 0; index < 100; ++index)
      used += (size_t)snprintf(prior + used, sizeof(prior) - used,
                               "existing row %03d\r\n", index);
    if (homed) {
      memcpy(prior + used, "\033[2J\033[H", 7);
      used += 7;
    }
    v.feed(g->actual.terminal, prior, (long)used);
    v.feed(g->direct.terminal, prior, (long)used);
    if (output_parsed(&g->actual, -1) || output_parsed(&g->direct, -1))
      return 1;
  }
  v.feed(g->direct.terminal, initial_boundary, sizeof(initial_boundary) - 1);
  if (output_parsed(&g->direct, -1))
    return 1;
  if (snprintf(g->work, sizeof(g->work), "%s/vte-retained.XXXXXX", build) >=
          (int)sizeof(g->work) ||
      !mkdtemp(g->work))
    return 1;
  if (strlen(g->work) + sizeof("/commands") > sizeof(g->command_path))
    return 1;
  strcpy(g->command_path, g->work);
  strcat(g->command_path, "/commands");
  strcpy(g->reply_path, g->work);
  strcat(g->reply_path, "/replies");
  if (mkfifo(g->command_path, 0600) || mkfifo(g->reply_path, 0600))
    return 1;
  g->commands = open(g->command_path, O_RDWR | O_NONBLOCK);
  g->replies = open(g->reply_path, O_RDWR | O_NONBLOCK);
  if (g->commands < 0 || g->replies < 0)
    return 1;
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = (unsigned short)rows;
  if (openpty(&master, &slave, NULL, NULL, &size))
    return 1;
  if (tabs && tcgetattr(slave, &attributes) == 0) {
    attributes.c_oflag |= OPOST | TAB3;
    tcsetattr(slave, TCSANOW, &attributes);
  }
  pty = v.pty(master, NULL, NULL);
  if (!pty) {
    close(master);
    close(slave);
    return 1;
  }
  v.set_pty(g->actual.terminal, pty);
  v.unref(pty);
  g->child = fork();
  if (g->child == 0) {
    close(master);
    close(g->commands);
    close(g->replies);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    if (prompt)
      execl(fixture, fixture, mode, g->command_path, g->reply_path, source,
            prompt, (char *)NULL);
    else
      execl(fixture, fixture, mode, g->command_path, g->reply_path, source,
            (char *)NULL);
    _exit(127);
  }
  g->slave = slave;
  return g->child < 0;
}
static int ack(struct session *g, int columns, int rows) {
  unsigned short geometry[2];
  size_t used = 0;
  ssize_t count;
  double deadline = now() + 4;
  while (used < sizeof(geometry) && now() < deadline) {
    count = read(g->replies, (char *)geometry + used, sizeof(geometry) - used);
    if (count > 0)
      used += (size_t)count;
    else if (count < 0 && errno != EAGAIN && errno != EINTR)
      return 1;
    else
      pump_once();
  }
  if (used != sizeof(geometry)) {
    fprintf(stderr, "Fixture acknowledgement timed out\n");
    return 1;
  }
  return (columns && (geometry[0] != columns || geometry[1] != rows)) ||
         output_parsed(&g->actual, g->slave);
}
static int command(struct session *g, char byte, int columns, int rows) {
  return write(g->commands, &byte, 1) != 1 || ack(g, columns, rows);
}
static void feed(struct screen *s, const char *bytes) {
  v.feed(s->terminal, bytes, (long)strlen(bytes));
  errors += output_parsed(s, -1);
}
static void source_feed(struct screen *s, const char *source) {
  char bytes[16384];
  size_t input, used = 0;
  for (input = 0; source[input] && used + 2 < sizeof(bytes); ++input) {
    if (source[input] == '\n')
      bytes[used++] = '\r';
    bytes[used++] = source[input];
  }
  if (source[input]) {
    ++errors;
    return;
  }
  v.feed(s->terminal, bytes, (long)used);
  errors += output_parsed(s, -1);
}
static int same(struct session *g, int retained, int finite) {
  struct snapshot observed = transcript(&g->actual),
                  expected = transcript(&g->direct);
  size_t length = strlen(observed.text), index;
  int prompts = 0, lines = 0;
  if (retained) {
    for (index = 0; index < length; ++index) {
      if ((index == 0 || observed.text[index - 1] == '\n') &&
          !strncmp(observed.text + index, "> ", 2))
        ++prompts;
      if (observed.text[index] == '\n')
        ++lines;
    }
    if (length < 3 || strcmp(observed.text + length - 3, "> \n") ||
        (finite &&
         (prompts != 1 || observed.row != lines - 1 || observed.column != 2))) {
      fprintf(stderr, "Retained prompt/cursor changed: (%ld,%ld)\n%s\n",
              observed.row, observed.column, observed.text);
      return 1;
    }
    length -= 3;
    observed.text[length] = 0;
    while (length >= 2 && observed.text[length - 1] == '\n' &&
           observed.text[length - 2] == '\n')
      observed.text[--length] = 0;
  }
  if (strcmp(observed.text, expected.text)) {
    fprintf(stderr, "Expected transcript:\n%s\nActual transcript:\n%s\n",
            expected.text, observed.text);
    return 1;
  }
  return 0;
}
static int wait_text(struct session *g, const char *needle) {
  double deadline = now() + 4;
  do {
    struct snapshot current = transcript(&g->actual);
    if (strstr(current.text, needle))
      return 0;
    pump_once();
  } while (now() < deadline);
  fprintf(stderr, "Missing VTE marker: %s\n", needle);
  return 1;
}
static int finish(struct session *g) {
  int status;
  pid_t waited;
  double deadline = now() + 4;
  char byte = 'x';
  if (write(g->commands, &byte, 1) != 1)
    return 1;
  close(g->slave);
  g->slave = -1;
  do {
    pump_once();
    waited = waitpid(g->child, &status, WNOHANG);
    if (waited == g->child) {
      g->child = -1;
      return !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
             output_closed(&g->actual, deadline);
    }
    if (waited < 0)
      return 1;
  } while (now() < deadline);
  fprintf(stderr, "Gated VTE fixture failed to exit\n");
  return 1;
}
static int stream_case(const char *fixture, const char *build,
                       const char *source, int filled) {
  struct session g;
  int failed = 0, stage;
  const int dimensions[][2] = {{20, 6}, {80, 12}, {38, 8}, {40, 8}};
  struct snapshot final;
  long visible;
  REQUIRE(start_session(&g, fixture, build, "--gated", source, filled, 0, 0, 8,
                        NULL) == 0);
  REQUIRE(ack(&g, 40, 8) == 0);
  REQUIRE(command(&g, 's', 0, 0) == 0);
  source_feed(&g.direct, source);
  REQUIRE(same(&g, 0, 0) == 0);
  for (stage = 0; stage < 4; ++stage) {
    resize(&g.direct, dimensions[stage][0], dimensions[stage][1]);
    resize(&g.actual, dimensions[stage][0], dimensions[stage][1]);
    REQUIRE(command(&g, 'r', dimensions[stage][0], dimensions[stage][1]) == 0);
    REQUIRE(command(&g, 'p', 0, 0) == 0);
    feed(&g.direct, "Y");
    REQUIRE(same(&g, 0, 0) == 0);
  }
  REQUIRE(command(&g, 'w', 0, 0) == 0);
  REQUIRE(same(&g, 0, 0) == 0);
  REQUIRE(command(&g, 'p', 0, 0) == 0);
  feed(&g.direct, "Y");
  REQUIRE(same(&g, 0, 0) == 0);
  REQUIRE(command(&g, 'f', 0, 0) == 0);
  feed(&g.direct, "\r\nF: ");
  source_feed(&g.direct, source);
  feed(&g.direct, "\r\n");
  REQUIRE(same(&g, 0, 0) == 0);
  REQUIRE(command(&g, 'p', 0, 0) == 0);
  feed(&g.direct, "Y");
  REQUIRE(same(&g, 0, 0) == 0);
  REQUIRE(write(g.commands, "i", 1) == 1);
  REQUIRE(wait_text(&g, "> ") == 0);
  v.send(g.actual.terminal, "\r", 1);
  REQUIRE(ack(&g, 0, 0) == 0);
  if (!strcmp(source, "abcdefghijklmnopqrstuvwxyz")) {
    resize(&g.actual, 40, 12);
    REQUIRE(command(&g, 'r', 40, 12) == 0);
    REQUIRE(command(&g, 'w', 0, 0) == 0);
    REQUIRE(command(&g, 'p', 0, 0) == 0);
    REQUIRE(wait_text(&g, "\nYY\n") == 0);
    resize(&g.actual, 40, 16);
    REQUIRE(command(&g, 'r', 40, 16) == 0);
    REQUIRE(command(&g, 'p', 0, 0) == 0);
    REQUIRE(wait_text(&g, "\nYYY\n") == 0);
  }
  resize(&g.actual, filled ? 32 : 40, 6);
  REQUIRE(finish(&g) == 0);
  final = transcript(&g.actual);
  visible =
      final.row - (long)v.adjustment_value(v.adjustment(g.actual.terminal));
  REQUIRE(visible == 5 && final.column == 0);
  REQUIRE(strncmp(final.text, "> ", 2) && !strstr(final.text, "\n> "));
cleanup:
  close_session(&g);
  return failed || errors;
}
static int handoff_case(const char *fixture, const char *build,
                        const char *source, int filled, int live, int homed,
                        int tabs, const char *draft, int height_only) {
  struct session g;
  int failed = 0, stage, stages;
  const int standard[][2] = {{80, 12}, {30, 8}, {80, 8}, {40, 12}};
  const int heights[][2] = {{40, 12}, {40, 8}};
  const int tails[][2] = {{30, 12}, {40, 12}, {30, 12}, {40, 12}};
  const int live_sizes[][2] = {{80, 8}, {40, 8}, {80, 8}};
  const int (*dimensions)[2] = height_only == 2 ? tails
                               : height_only    ? heights
                               : live           ? live_sizes
                                                : standard;
  stages = height_only == 2 ? 4 : height_only ? 2 : live ? 3 : 4;
  REQUIRE(start_session(&g, fixture, build, live ? "--gated-live" : "--gated",
                        source, filled, homed, tabs, 8, NULL) == 0);
  REQUIRE(ack(&g, 40, 8) == 0);
  REQUIRE(command(&g, 's', 0, 0) == 0);
  source_feed(&g.direct, source);
  REQUIRE(write(g.commands, "i", 1) == 1);
  REQUIRE(wait_text(&g, "> ") == 0);
  if (draft) {
    REQUIRE(!live);
    v.send(g.actual.terminal, draft, (long)strlen(draft));
    for (stage = 0; stage < 10; ++stage)
      pump();
    resize(&g.actual, 13, 8);
    resize(&g.direct, 13, 8);
    for (stage = 0; stage < 10; ++stage)
      pump();
    v.send(g.actual.terminal, "\025\r", 2);
  } else if (!live)
    v.send(g.actual.terminal, "\r", 1);
  REQUIRE(ack(&g, 0, 0) == 0);
  for (stage = 0; stage < stages; ++stage) {
    resize(&g.direct, dimensions[stage][0], dimensions[stage][1]);
    resize(&g.actual, dimensions[stage][0], dimensions[stage][1]);
    REQUIRE(command(&g, 'p', dimensions[stage][0], dimensions[stage][1]) == 0);
    feed(&g.direct, "Y");
    REQUIRE(same(&g, 1, 0) == 0);
  }
  REQUIRE(finish(&g) == 0);
cleanup:
  close_session(&g);
  return failed || errors;
}
static int multiline_append(const char *fixture, const char *build,
                            int filled) {
  static const int sizes[][2] = {
      {30, 7}, {30, 12}, {50, 12}, {35, 8}, {40, 12}};
  struct session g;
  char expected[sizeof("producer tail") + sizeof(sizes) / sizeof(sizes[0])];
  size_t i;
  int failed = 0;
  strcpy(expected, "producer tail");
  REQUIRE(start_session(&g, fixture, build, "--gated-live", "producer tail",
                        filled, 0, 0, 12, NULL) == 0);
  REQUIRE(ack(&g, 40, 12) == 0);
  REQUIRE(command(&g, 's', 40, 12) == 0);
  REQUIRE(command(&g, 'i', 40, 12) == 0);
  REQUIRE(wait_text(&g, "> ") == 0);
  v.send(g.actual.terminal, "\n\n\n\033[A\033[Az", 10);
  REQUIRE(wait_text(&g, "\n  z\n") == 0);
  for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    resize(&g.actual, sizes[i][0], sizes[i][1]);
    REQUIRE(command(&g, 'p', sizes[i][0], sizes[i][1]) == 0);
    strcat(expected, "Y");
    REQUIRE(wait_text(&g, expected) == 0);
    REQUIRE(wait_text(&g, "\n  z\n") == 0);
  }
  REQUIRE(finish(&g) == 0);
cleanup:
  close_session(&g);
  return failed || errors;
}

static int finite_case(const char *fixture, const char *build,
                       const char *source, int filled) {
  struct session g;
  int failed = 0, stage;
  int dimensions[][2] = {{40, 12}, {52, 16}, {32, 16}, {40, 20}, {40, 24}};
  struct snapshot current;
  long input_row;
  if (filled) {
    dimensions[2][1] = 12;
    dimensions[3][1] = 16;
    dimensions[4][1] = 12;
  }
  REQUIRE(start_session(&g, fixture, build, "--finite-retained", source, filled,
                        0, 0, 8, NULL) == 0);
  REQUIRE(ack(&g, 40, 8) == 0);
  source_feed(&g.direct, source);
  REQUIRE(same(&g, 1, 1) == 0);
  for (stage = 0; stage < 5; ++stage) {
    char step = stage == 2 ? 'm' : stage == 3 ? 'e' : 'f';
    resize(&g.direct, dimensions[stage][0], dimensions[stage][1]);
    resize(&g.actual, dimensions[stage][0], dimensions[stage][1]);
    REQUIRE(command(&g, step, dimensions[stage][0], dimensions[stage][1]) == 0);
    feed(&g.direct, step == 'm' ? "X" : "XYZ");
    REQUIRE(same(&g, 1, 1) == 0);
    if (step != 'f') {
      resize(&g.direct, dimensions[stage][0] + 8, dimensions[stage][1] + 4);
      resize(&g.actual, dimensions[stage][0] + 8, dimensions[stage][1] + 4);
      REQUIRE(command(&g, 'g', dimensions[stage][0] + 8,
                      dimensions[stage][1] + 4) == 0);
      if (step == 'm')
        feed(&g.direct, "YZ");
      REQUIRE(same(&g, 1, 1) == 0);
    }
  }
  current = snapshot(&g.actual, 0);
  input_row = current.row;
  REQUIRE(finish(&g) == 0);
  REQUIRE(same(&g, 0, 0) == 0);
  current = snapshot(&g.actual, 0);
  REQUIRE(current.row == input_row && current.column == 0);
cleanup:
  close_session(&g);
  return failed || errors;
}
static int roundtrip(const char *fixture, const char *build) {
  struct session g;
  int failed = 0, stage;
  const int widths[] = {20, 40, 20, 40};
  char expected[64] = "ABC";
  REQUIRE(start_session(&g, fixture, build, "--gated", "ABC", 1, 0, 0, 12,
                        "123456789012345678901234567890> ") == 0);
  REQUIRE(ack(&g, 40, 12) == 0);
  REQUIRE(command(&g, 's', 0, 0) == 0);
  REQUIRE(write(g.commands, "i", 1) == 1);
  REQUIRE(wait_text(&g, "1234567890> ") == 0);
  v.send(g.actual.terminal, "\r", 1);
  REQUIRE(ack(&g, 0, 0) == 0);
  for (stage = 0; stage < 4; ++stage) {
    resize(&g.actual, widths[stage], 12);
    REQUIRE(command(&g, 'p', widths[stage], 12) == 0);
    strcat(expected, "Y");
    REQUIRE(wait_text(&g, expected) == 0);
  }
  REQUIRE(finish(&g) == 0);
cleanup:
  close_session(&g);
  return failed || errors;
}
static int clamped(const char *fixture, const char *build, int first_resize) {
  struct session g;
  int failed = 0, stage, count = 0;
  struct snapshot current;
  const char *needle = "> aaaaaaaaaaaaaaaaaa\n", *position;
  REQUIRE(start_session(&g, fixture, build, "--gated", "", 0, 0, 0, 8, NULL) ==
          0);
  REQUIRE(ack(&g, 40, 8) == 0);
  if (first_resize)
    resize(&g.actual, 50, 10);
  REQUIRE(write(g.commands, "i", 1) == 1);
  REQUIRE(wait_text(&g, "> \n") == 0);
  v.send(g.actual.terminal, "aaaaaaaaaaaaaaaaaa", 18);
  REQUIRE(wait_text(&g, needle) == 0);
  resize(&g.actual, 20, 8);
  for (stage = 0; stage < 15; ++stage)
    pump();
  current = transcript(&g.actual);
  position = current.text;
  while ((position = strstr(position, needle)) != NULL) {
    ++count;
    position += strlen(needle);
  }
  REQUIRE(count == 1);
  v.send(g.actual.terminal, "\025\r", 2);
  REQUIRE(ack(&g, 0, 0) == 0);
  REQUIRE(finish(&g) == 0);
cleanup:
  close_session(&g);
  return failed || errors;
}
static void repeat(char *text, const char *glyph, int count, const char *tail) {
  int index;
  text[0] = 0;
  for (index = 0; index < count; ++index)
    strcat(text, glyph);
  strcat(text, tail);
}
int main(int argc, char **argv) {
  const char *basic[] = {"abcdefghijklmnopqrstuvwxyz", "one\nABC", "café 中文"};
  const char *finite[] = {
      "ABC", "one\nline",        "café",        "\033[1mStyled\033[0m",
      "🇸🇪",  "\033[1m🇸🇪\033[0m", "👩‍💻", "é 中文"};
  const char *handoff[] = {
      "ABC",          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaX",
      "🇸🇪",           "\033[1m🇸🇪\033[0mX",
      "👩‍💻X", "é 中文",
      "🇸🇪\nnext",     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\rQ"};
  const char *emoji[] = {"🇸🇪", "👩‍💻", "❤️", "1️⃣"};
  char source[8192], draft[1024];
  size_t index;
  int filled, failed = 0;
  if (argc != 4 || load())
    return 2;
  if (!strcmp(argv[1], "stream")) {
    for (index = 0; index < sizeof(basic) / sizeof(basic[0]) && !failed;
         ++index)
      for (filled = 0; filled < 2 && !failed; ++filled)
        failed = stream_case(argv[2], argv[3], basic[index], filled);
    for (index = 0; index < 5 && !failed; ++index) {
      repeat(source, index == 4 ? "a" : emoji[index],
             index == 1   ? 10
             : index == 4 ? 40
                          : 22,
             index == 4 ? "" : "X");
      for (filled = 0; filled < 2 && !failed; ++filled)
        failed = stream_case(argv[2], argv[3], source, filled);
    }
    if (!failed)
      failed = roundtrip(argv[2], argv[3]);
    for (filled = 0; filled < 2 && !failed; ++filled)
      failed = clamped(argv[2], argv[3], filled);
  } else if (!strcmp(argv[1], "finite")) {
    for (index = 0; index < sizeof(finite) / sizeof(finite[0]) && !failed;
         ++index)
      for (filled = 0; filled < 2 && !failed; ++filled)
        failed = finite_case(argv[2], argv[3], finite[index], filled);
  } else if (!strcmp(argv[1], "handoff")) {
    for (index = 0; index < sizeof(handoff) / sizeof(handoff[0]) && !failed;
         ++index)
      for (filled = 0; filled < 2 && !failed; ++filled)
        failed = handoff_case(argv[2], argv[3], handoff[index], filled, 0, 0, 0,
                              NULL, 0);
    for (index = 0; index < 3 && !failed; ++index) {
      repeat(source, index == 2 ? "👩‍💻" : "🇸🇪",
             index == 0   ? 22
             : index == 1 ? 181
                          : 81,
             "X");
      failed = handoff_case(argv[2], argv[3], source, 0, 0, 0, 0, NULL, 1);
    }
    repeat(draft, "中", 18, "");
    if (!failed)
      failed = handoff_case(argv[2], argv[3], "MARKER", 1, 0, 0, 0, draft, 0);
    for (index = 0; index < 2 && !failed; ++index) {
      repeat(source, "a", 35, index ? "\rZ" : "\r");
      failed = handoff_case(argv[2], argv[3], source, 1, 0, 0, 0, NULL, 0);
    }
    repeat(source, "a", 80, "\nABC");
    for (filled = 0; filled < 2 && !failed; ++filled)
      failed = handoff_case(argv[2], argv[3], source, filled, 1, 0, 0, NULL, 0);
    if (!failed)
      failed = handoff_case(argv[2], argv[3], "ABC", 0, 1, 1, 0, NULL, 1);
    for (filled = 0; filled < 2 && !failed; ++filled)
      failed = multiline_append(argv[2], argv[3], filled);
  } else if (!strcmp(argv[1], "tabs")) {
    const char *tabs[] = {"\tX", "a\tX", "a\t\tX"};
    for (index = 0; index < 3 && !failed; ++index)
      for (filled = 0; filled < 2 && !failed; ++filled)
        failed = handoff_case(argv[2], argv[3], tabs[index], filled, 0, 0, 1,
                              NULL, 1);
  } else if (!strcmp(argv[1], "diagnostic-cr-tail")) {
    /* Accepted opaque-feed positioning limitation: manual diagnostic only. */
    for (index = 0; index < 3 && !failed; ++index) {
      repeat(source, "a", index == 1 ? 29 : 27, index == 1 ? "中\r" : "中中\r");
      repeat(draft, "b", index == 0 ? 28 : index == 1 ? 29 : 30, "");
      strcat(source, draft);
      for (filled = 0; filled < 2 && !failed; ++filled)
        failed =
            handoff_case(argv[2], argv[3], source, filled, 0, 0, 0, NULL, 2);
    }
  } else
    return 2;
  return failed || errors;
}
