#include "vte_test.h"

struct editor {
  struct screen screen;
  pid_t child;
  unsigned long serial;
};
static int rendered(struct editor *e) {
  char expected[64];
  double deadline = now() + 3;
  snprintf(expected, sizeof(expected), "rendered-%lu", ++e->serial);
  v.send(e->screen.terminal, "\033m", 2);
  do {
    const char *title = v.title(e->screen.terminal);
    if (title && !strcmp(title, expected))
      return 0;
    pump_once();
  } while (now() < deadline);
  fprintf(stderr, "Editor render barrier timed out: %s\n", expected);
  return 1;
}
static void stop_editor(struct editor *e) {
  int status;
  if (e->child > 0) {
    kill(e->child, SIGTERM);
    waitpid(e->child, &status, 0);
  }
  v.destroy(e->screen.window);
  pump_once();
}
static int wait_marker(struct editor *e, const char *marker) {
  double deadline = now() + 3;
  do {
    struct snapshot current = transcript(&e->screen);
    if (strstr(current.text, marker))
      return 0;
    pump_once();
  } while (now() < deadline);
  fprintf(stderr, "Readline marker missing: %s\n", marker);
  return 1;
}
static int start_editor(struct editor *e, const char *example, int width,
                        const char *prior, const char *source,
                        const char *position, const char *mode) {
  struct winsize size;
  void *pty;
  int master, slave;
  e->child = -1;
  e->serial = 0;
  e->screen = create();
  resize(&e->screen, width, 8);
  v.feed(e->screen.terminal, prior, (long)strlen(prior));
  memset(&size, 0, sizeof(size));
  size.ws_col = (unsigned short)width;
  size.ws_row = 8;
  if (openpty(&master, &slave, NULL, NULL, &size))
    return 1;
  pty = v.pty(master, NULL, NULL);
  if (!pty) {
    close(master);
    close(slave);
    return 1;
  }
  v.set_pty(e->screen.terminal, pty);
  v.unref(pty);
  e->child = fork();
  if (e->child == 0) {
    close(master);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    setenv("SOFTLINE_PROMPT_THEME", "plain", 1);
    execl(example, example, mode, (char *)NULL);
    _exit(127);
  }
  close(slave);
  if (e->child < 0 || wait_marker(e, mode ? "> " : "softline> "))
    return 1;
  v.send(e->screen.terminal, source, (long)strlen(source));
  v.send(e->screen.terminal, position, (long)strlen(position));
  return rendered(e);
}
static int prompt(struct editor *e, struct snapshot *result) {
  struct snapshot body;
  char *begin;
  size_t index;
  body = transcript(&e->screen);
  *result = snapshot(&e->screen, 0);
  strcpy(result->text, body.text);
  begin = strstr(result->text, "softline> ");
  if (!begin)
    return 1;
  for (index = 0; result->text + index < begin; ++index)
    if (result->text[index] == '\n')
      --result->row;
  memmove(result->text, begin, strlen(begin) + 1);
  return 0;
}
static int case_editor(const char *example, const char *source,
                       const char *position, int filled) {
  struct editor actual, reference;
  struct snapshot observed, expected, current;
  char prior[4096], kept[4096], submitted[4096], label[32];
  int count = filled ? 20 : 2, index, width, stage, failed = 1;
  const int widths[] = {20, 36, 18};
  size_t used = 0, keep_used, submit_used, length;
  const char *line, *end, *begin;
  memset(&observed, 0, sizeof(observed));
  memset(&expected, 0, sizeof(expected));
  for (index = 0; index < count; ++index)
    used += (size_t)snprintf(prior + used, sizeof(prior) - used, "keep%03d\r\n",
                             index);
  if (start_editor(&actual, example, 40, prior, source, position, NULL))
    goto cleanup;
  for (stage = 0; stage < 3; ++stage) {
    width = widths[stage];
    resize_geometry(&actual.screen, width, 8);
    if (rendered(&actual))
      goto cleanup;
    current = transcript(&actual.screen);
    keep_used = 0;
    for (line = current.text; *line; line = end + 1) {
      end = strchr(line, '\n');
      if (!end)
        break;
      if (!strncmp(line, "keep", 4)) {
        length = (size_t)(end - line);
        if (keep_used + length + 2 > sizeof(kept))
          goto cleanup;
        memcpy(kept + keep_used, line, length);
        keep_used += length;
        kept[keep_used++] = '\r';
        kept[keep_used++] = '\n';
      }
    }
    if (keep_used != used || memcmp(kept, prior, used)) {
      fprintf(stderr, "Readline overwrote retained output\n%s\n", current.text);
      goto cleanup;
    }
    if (start_editor(&reference, example, width, prior, source, position,
                     NULL)) {
      stop_editor(&reference);
      goto cleanup;
    }
    if (prompt(&actual, &observed) || prompt(&reference, &expected) ||
        strcmp(observed.text, expected.text) || observed.row != expected.row ||
        observed.column != expected.column) {
      fprintf(stderr,
              "Resized readline differs from fresh terminal: width=%d "
              "source=%s\n%s\n%s\n",
              width, source, observed.text, expected.text);
      stop_editor(&reference);
      goto cleanup;
    }
    stop_editor(&reference);
    if (stage == 1) {
      char damage[128];
      int length;
      current = snapshot(&actual.screen, 0);
      length = snprintf(damage, sizeof(damage), "\rBROKEN\033[0K\r");
      if (current.column > 0)
        length += snprintf(damage + length, sizeof(damage) - (size_t)length,
                           "\033[%ldC", current.column);
      v.feed(actual.screen.terminal, damage, length);
      pump();
      v.send(actual.screen.terminal, "\014", 1);
      if (rendered(&actual) || prompt(&actual, &observed) ||
          strcmp(observed.text, expected.text) ||
          observed.row != expected.row || observed.column != expected.column) {
        fprintf(stderr, "Ctrl-L did not restore the owned editor frame\n");
        goto cleanup;
      }
    }
  }
  v.send(actual.screen.terminal, "\r", 1);
  if (wait_marker(&actual, "submitted: "))
    goto cleanup;
  /* Wait for the next editable prompt, then concatenate only submission rows.
   */
  if (rendered(&actual))
    goto cleanup;
  current = transcript(&actual.screen);
  begin = strstr(current.text, "submitted: ");
  if (!begin)
    goto cleanup;
  submit_used = 0;
  for (line = begin; *line; line = end + 1) {
    if (!strncmp(line, "softline> ", 10))
      break;
    end = strchr(line, '\n');
    if (!end)
      goto cleanup;
    length = (size_t)(end - line);
    if (submit_used + length + 1 > sizeof(submitted))
      goto cleanup;
    memcpy(submitted + submit_used, line, length);
    submit_used += length;
  }
  submitted[submit_used] = 0;
  snprintf(label, sizeof(label), "%s", "submitted: ");
  if (strncmp(submitted, label, strlen(label)) ||
      strcmp(submitted + strlen(label), source)) {
    fprintf(stderr, "Readline draft changed: source=%s\n%s\n", source,
            current.text);
    goto cleanup;
  }
  failed = 0;
cleanup:
  stop_editor(&actual);
  return failed || errors;
}

static int case_themes(const char *example, const char *source,
                       const char *position) {
  const int codes[] = {21, 14, 21, 12, 14, 11, 14, 13, 14, 20,
                       14, 15, 14, 17, 14, 18, 14, 19, 21, 21};
  struct editor editor;
  struct snapshot expected, actual;
  char key[16], submitted[512];
  size_t index;
  double deadline;
  int failed = 1;
  if (start_editor(&editor, example, 40, "retained output\r\n", source,
                   position, "--themes"))
    goto cleanup;
  expected = transcript(&editor.screen);
  /* Cover short/long, plain/RGB, styled-input and unchanged transitions.
   * Default-to-gruvbox used to print the RGB escape suffix as prompt text. */
  for (index = 0; index < sizeof(codes) / sizeof(codes[0]); ++index) {
    int length = snprintf(key, sizeof(key), "\033[%d~", codes[index]);
    v.send(editor.screen.terminal, key, length);
    if (rendered(&editor))
      goto cleanup;
    actual = transcript(&editor.screen);
    if (strcmp(actual.text, expected.text) || actual.row != expected.row ||
        actual.column != expected.column) {
      fprintf(stderr,
              "Theme change corrupted the prompt: key=%d draft=%s\n"
              "expected:\n%s\nactual:\n%s\n",
              codes[index], source, expected.text, actual.text);
      goto cleanup;
    }
  }
  /* Submitting the draft proves the underlying text stayed intact too. */
  v.send(editor.screen.terminal, "\005!\r", 3);
  snprintf(submitted, sizeof(submitted), "submitted: %s!", source);
  deadline = now() + 3;
  do {
    char *begin, *read, *write;
    actual = transcript(&editor.screen);
    begin = strstr(actual.text, "submitted: ");
    if (begin) {
      read = write = begin;
      while (*read) {
        if (*read != '\n')
          *write++ = *read;
        ++read;
      }
      *write = '\0';
      if (!strcmp(begin, submitted)) {
        failed = 0;
        break;
      }
    }
    pump_once();
  } while (now() < deadline);
  if (failed)
    fprintf(stderr, "Theme change lost the submitted draft: %s\n%s\n",
            submitted, actual.text);
cleanup:
  stop_editor(&editor);
  return failed || errors;
}

static int inspect_draft(struct editor *e, const char *text, size_t cursor) {
  char expected[128];
  size_t i, length = strlen(text);
  unsigned long sum = 0;
  double deadline = now() + 5;
  for (i = 0; i < length; ++i)
    sum = (sum + (unsigned char)text[i]) % 65521;
  snprintf(expected, sizeof(expected), "draft-%lu-%lu-%lu",
           (unsigned long)length, (unsigned long)cursor, sum);
  v.send(e->screen.terminal, "\033t", 2);
  do {
    const char *title = v.title(e->screen.terminal);
    if (title && !strcmp(title, expected))
      return 0;
    pump_once();
  } while (now() < deadline);
  fprintf(stderr, "Draft mismatch: expected %s, got %s\n", expected,
          v.title(e->screen.terminal));
  return 1;
}
static char *visible_editor(struct editor *e) {
  long first = (long)v.adjustment_value(v.adjustment(e->screen.terminal));
  return v.text(e->screen.terminal, first, 0,
                first + v.rows(e->screen.terminal) - 1,
                v.columns(e->screen.terminal), NULL, NULL, NULL);
}
static int viewport_rows(struct editor *e, int first, int last, int cursor) {
  long column, row;
  char *frame;
  char marker[32];
  int i, failed = 0;
  v.cursor(e->screen.terminal, &column, &row);
  row -= cursor - first;
  frame = v.text(e->screen.terminal, row, 0, row + last - first,
                 v.columns(e->screen.terminal), NULL, NULL, NULL);
  for (i = 0; i < 40; ++i) {
    snprintf(marker, sizeof(marker), "line-%02d abcdefghij", i);
    if (!!strstr(frame, marker) != (i >= first && i <= last))
      failed = 1;
  }
  if (failed)
    fprintf(stderr, "Expected input rows %d..%d:\n%s\n", first, last, frame);
  v.free(frame);
  return failed;
}
static int case_viewport(const char *example, int native) {
  struct editor e;
  char source[2048], *large = NULL, *frame;
  size_t used = 0;
  int i, failed = 1;
  e.child = -1;
  if (start_editor(&e, example, 80, "retained marker\r\n", "", "",
                   native ? "--viewport-native" : "--viewport"))
    goto cleanup;
  resize_geometry(&e.screen, 80, 24);
  if (rendered(&e))
    goto cleanup;
  for (i = 0; i < 40; ++i)
    used += (size_t)snprintf(source + used, sizeof(source) - used,
                             "line-%02d abcdefghij%s", i, i == 39 ? "" : "\n");
  v.send(e.screen.terminal, "\033[200~", 6);
  v.send(e.screen.terminal, source, (long)used);
  v.send(e.screen.terminal, "\033[201~", 6);
  if (rendered(&e) || inspect_draft(&e, source, used) ||
      viewport_rows(&e, 28, 39, 39))
    goto cleanup;
  {
    struct snapshot history = transcript(&e.screen);
    if (strstr(history.text, "line-00")) {
      fprintf(stderr, "Hidden input was painted into terminal history\n");
      goto cleanup;
    }
  }
  for (i = 0; i < 5; ++i)
    v.send(e.screen.terminal, "\033[A", 3);
  if (rendered(&e) || viewport_rows(&e, 28, 39, 34))
    goto cleanup;
  for (i = 0; i < 7; ++i)
    v.send(e.screen.terminal, "\033[A", 3);
  if (rendered(&e) || viewport_rows(&e, 27, 38, 27))
    goto cleanup;
  v.send(e.screen.terminal, "\033o", 2);
  if (rendered(&e) || viewport_rows(&e, 27, 38, 27))
    goto cleanup;
  frame = visible_editor(&e);
  i = strstr(frame, "producer marker") != NULL;
  v.free(frame);
  if (!i)
    goto cleanup;
  for (i = 0; i < 12; ++i)
    v.send(e.screen.terminal, "\033[B", 3);
  if (rendered(&e) || viewport_rows(&e, 28, 39, 39) ||
      inspect_draft(&e, source, used))
    goto cleanup;
  resize_geometry(&e.screen, 80, 10);
  if (rendered(&e) || viewport_rows(&e, 35, 39, 39))
    goto cleanup;
  resize_geometry(&e.screen, 80, 24);
  if (rendered(&e) || viewport_rows(&e, 28, 39, 39))
    goto cleanup;
  v.send(e.screen.terminal, "\033g", 2);
  if (rendered(&e) || viewport_rows(&e, 0, 11, 0) ||
      inspect_draft(&e, source, 0))
    goto cleanup;
  v.send(e.screen.terminal, "\033h\033n", 4);
  if (rendered(&e) || inspect_draft(&e, "", 0))
    goto cleanup;
  used = 0;
  for (i = 0; i < 40; ++i)
    used += (size_t)snprintf(source + used, sizeof(source) - used, "\t中é%s",
                             i == 39 ? "END" : "\n");
  v.send(e.screen.terminal, "\033[200~", 6);
  v.send(e.screen.terminal, source, (long)used);
  v.send(e.screen.terminal, "\033[201~", 6);
  resize_geometry(&e.screen, 16, 8);
  if (rendered(&e) || inspect_draft(&e, source, used))
    goto cleanup;
  resize_geometry(&e.screen, 80, 24);
  if (rendered(&e) || inspect_draft(&e, source, used))
    goto cleanup;
  v.send(e.screen.terminal, "\033g", 2);
  if (rendered(&e) || inspect_draft(&e, source, 0))
    goto cleanup;
  v.send(e.screen.terminal, "\033h\033n", 4);
  /* A real maximum-size paste, followed by an over-limit UTF-8 character.
   * No text staging in the fixture: the actual terminal/input path handles it.
   */
  large = malloc(131073);
  if (!large)
    goto cleanup;
  memset(large, 'x', 131070);
  memcpy(large + 131070, "é", 3);
  v.send(e.screen.terminal, "\033[200~", 6);
  v.send(e.screen.terminal, large, 131072);
  v.send(e.screen.terminal, "中\033[201~", 9);
  if (rendered(&e) || inspect_draft(&e, large, 131072))
    goto cleanup;
  resize_geometry(&e.screen, 24, 8);
  if (rendered(&e) || inspect_draft(&e, large, 131072))
    goto cleanup;
  resize_geometry(&e.screen, 80, 24);
  if (rendered(&e) || inspect_draft(&e, large, 131072))
    goto cleanup;
  v.send(e.screen.terminal, "\037", 1);
  if (rendered(&e) || inspect_draft(&e, "", 0))
    goto cleanup;
  /* Undo clears the paste as one edit. The saved whole draft can also round
   * trip at the limit without rendering hidden rows into terminal history. */
  v.send(e.screen.terminal, "\033[200~", 6);
  v.send(e.screen.terminal, large, 131072);
  v.send(e.screen.terminal, "\033[201~\033k\033j", 10);
  if (rendered(&e) || inspect_draft(&e, large, 131072))
    goto cleanup;
  /* The maximum number of hard rows uses heap layout metadata and the same
   * bounded viewport, including when navigating to either end. */
  v.send(e.screen.terminal, "\033n", 2);
  memset(large, '\n', 131072);
  v.send(e.screen.terminal, "\033[200~", 6);
  v.send(e.screen.terminal, large, 131072);
  v.send(e.screen.terminal, "\033[201~", 6);
  if (rendered(&e) || inspect_draft(&e, large, 131072))
    goto cleanup;
  v.send(e.screen.terminal, "\033g", 2);
  if (rendered(&e) || inspect_draft(&e, large, 0))
    goto cleanup;
  v.send(e.screen.terminal, "\033h\037\033j", 5);
  memset(large, 'x', 131070);
  memcpy(large + 131070, "é", 3);
  if (rendered(&e) || inspect_draft(&e, large, 131072))
    goto cleanup;
  v.send(e.screen.terminal, "\r", 1);
  if (wait_marker(&e, "submitted-length: 131072"))
    goto cleanup;
  failed = 0;
cleanup:
  if (failed)
    fprintf(stderr, "Viewport case failed (%s)\n",
            native ? "native" : "readline");
  free(large);
  stop_editor(&e);
  return failed || errors;
}

int main(int argc, char **argv) {
  const char *sources[] = {
      "one two three four", "alpha beta gamma delta epsilon zeta eta",
      "café 中文 é words", "abcdefghij", "abcdefghijklmnopqrstuvwxyZ"};
  const char *positions[] = {"\001", "\001\033[C\033[C\033[C", ""};
  size_t source, position;
  int filled;
  if ((argc != 2 && argc != 3) || load())
    return 2;
  if (argc == 3 && !strcmp(argv[2], "viewport"))
    return case_viewport(argv[1], 0) || case_viewport(argv[1], 1);
  if (argc == 3 && !strcmp(argv[2], "themes")) {
    const char *drafts[] = {"", "a", "abc", "café 中文 é",
                            "alpha beta gamma delta epsilon zeta eta"};
    for (source = 0; source < sizeof(drafts) / sizeof(drafts[0]); ++source)
      if (case_themes(argv[1], drafts[source], ""))
        return 1;
    return case_themes(argv[1], "abc", "\001\033[C") || errors != 0;
  }
  for (source = 0; source < sizeof(sources) / sizeof(sources[0]); ++source)
    for (position = 0; position < 3; ++position)
      for (filled = 0; filled < 2; ++filled)
        if (case_editor(argv[1], sources[source], positions[position], filled))
          return 1;
  return errors != 0;
}
