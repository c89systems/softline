#include "vte_output_sync.h"
static int render(const char *fixture, const char *source, int chunk,
                  int handoff, int grow, int tabs, const char *initial,
                  struct snapshot *result) {
  struct screen s = create();
  int master = -1, slave = -1, status = 0, failed = 0;
  pid_t child = -1;
  struct winsize size;
  struct termios term;
  char chunk_text[32], direct[16384];
  size_t i, used = 0;
  double deadline;
  if (initial) {
    v.feed(s.terminal, initial, (long)strlen(initial));
    if (output_parsed(&s, -1)) {
      failed = 1;
      goto cleanup;
    }
  }
  if (chunk == 0) {
    for (i = 0; source[i] && used + 4 < sizeof(direct); ++i) {
      if (source[i] == '\n')
        direct[used++] = '\r';
      direct[used++] = source[i];
    }
    if (handoff)
      direct[used++] = 'Y';
    direct[used++] = '\r';
    direct[used++] = '\n';
    v.feed(s.terminal, direct, (long)used);
    if (output_parsed(&s, -1)) {
      failed = 1;
      goto cleanup;
    }
  } else {
    void *pty;
    int sent = 0;
    memset(&size, 0, sizeof(size));
    size.ws_row = 8;
    size.ws_col = 40;
    if (openpty(&master, &slave, NULL, NULL, &size) != 0) {
      failed = 1;
      goto cleanup;
    }
    if (tabs && tcgetattr(slave, &term) == 0) {
      term.c_oflag |= OPOST | TAB3;
      tcsetattr(slave, TCSANOW, &term);
    }
    pty = v.pty(master, NULL, NULL);
    if (!pty) {
      fprintf(stderr, "VTE PTY attach failed\n");
      failed = 1;
      goto cleanup;
    }
    v.set_pty(s.terminal, pty);
    v.unref(pty);
    snprintf(chunk_text, sizeof(chunk_text), "%d", chunk);
    child = fork();
    if (child == 0) {
      dup2(slave, 0);
      dup2(slave, 1);
      dup2(slave, 2);
      if (slave > 2)
        close(slave);
      execl(fixture, fixture, handoff ? "--handoff" : "--chunks", source,
            chunk_text, (char *)NULL);
      _exit(127);
    }
    if (child < 0) {
      failed = 1;
      goto cleanup;
    }
    close(slave);
    slave = -1;
    deadline = now() + 5;
    while (waitpid(child, &status, WNOHANG) == 0) {
      pump_once();
      if (handoff && !sent) {
        struct snapshot current = snapshot(&s, 0);
        if (strstr(current.text, "> ")) {
          v.send(s.terminal, "\r", 1);
          sent = 1;
        }
      }
      if (now() >= deadline) {
        fprintf(stderr, "Native VTE fixture timed out\n");
        failed = 1;
        goto cleanup;
      }
    }
    child = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        output_closed(&s, deadline)) {
      failed = 1;
      goto cleanup;
    }
    if (handoff) {
      long row, column;
      v.cursor(s.terminal, &column, &row);
      if (column != 0 || row != v.rows(s.terminal) - 1) {
        fprintf(stderr, "Editor exit cursor changed: (%ld,%ld)\n", row, column);
        failed = 1;
        goto cleanup;
      }
    }
  }
  if (grow)
    resize(&s, 80, 8);
  *result = snapshot(&s, handoff);
cleanup:
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (slave >= 0)
    close(slave);
  /* VTE owns master after vte_pty_new_foreign_sync. */
  v.destroy(s.window);
  return failed;
}
static int compare_expected(const char *fixture, const char *source,
                            int handoff, int grow, int tabs,
                            const char *initial,
                            const struct snapshot *expected) {
  struct snapshot actual;
  int chunks[4], index, previous;
  chunks[0] = 1;
  chunks[1] = 4;
  chunks[2] = 8;
  chunks[3] = (int)strlen(source);
  for (index = 0; index < 4; ++index) {
    if (chunks[index] == 0)
      continue;
    for (previous = 0; previous < index; ++previous)
      if (chunks[previous] == chunks[index])
        break;
    if (previous != index)
      continue;
    if (render(fixture, source, chunks[index], handoff, grow, tabs, initial,
               &actual))
      return 1;
    if (expected->row != actual.row || expected->column != actual.column ||
        strcmp(expected->text, actual.text)) {
      fprintf(stderr,
              "Native/direct VTE mismatch: chunk=%d handoff=%d grow=%d tabs=%d "
              "source=%s\nexpected=(%ld,%ld) %s\nactual=(%ld,%ld) %s\n",
              chunks[index], handoff, grow, tabs, source, expected->row,
              expected->column, expected->text, actual.row, actual.column,
              actual.text);
      return 1;
    }
  }
  return 0;
}
static int compare(const char *fixture, const char *source, int handoff,
                   int grow, int tabs, const char *initial) {
  struct snapshot expected;
  if (render(fixture, source, 0, handoff, grow, tabs, initial, &expected))
    return 1;
  return compare_expected(fixture, source, handoff, grow, tabs, initial,
                          &expected);
}
struct unicode_result {
  struct snapshot before, after;
  unsigned char bytes[16384];
  size_t length;
};
static int unicode_render(const char *fixture, const char *trace,
                          const char *mode, const char *producer,
                          const char *source, int chunk, int filled,
                          struct unicode_result *result) {
  struct screen s = create();
  struct winsize size;
  int master = -1, slave = -1, status = 0, failed = 1, sent = 0;
  pid_t child = -1, waited;
  char prior[4096], chunk_text[32];
  size_t used = 0;
  int index;
  void *pty;
  double deadline;
  FILE *file;
  memset(result, 0, sizeof(*result));
  if (filled) {
    for (index = 0; index < 100; ++index)
      used += (size_t)snprintf(prior + used, sizeof(prior) - used,
                               "existing row %03d\r\n", index);
    v.feed(s.terminal, prior, (long)used);
    if (output_parsed(&s, -1))
      goto cleanup;
  }
  memset(&size, 0, sizeof(size));
  size.ws_row = 8;
  size.ws_col = 40;
  if (openpty(&master, &slave, NULL, NULL, &size))
    goto cleanup;
  pty = v.pty(master, NULL, NULL);
  if (!pty) {
    close(master);
    goto cleanup;
  }
  v.set_pty(s.terminal, pty);
  v.unref(pty);
  snprintf(chunk_text, sizeof(chunk_text), "%d", chunk);
  child = fork();
  if (child == 0) {
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    execl(fixture, fixture, mode, producer, chunk_text, source, trace,
          (char *)NULL);
    _exit(127);
  }
  if (child < 0)
    goto cleanup;
  close(slave);
  slave = -1;
  deadline = now() + 8;
  while ((waited = waitpid(child, &status, WNOHANG)) == 0) {
    pump_once();
    if (!sent && !strcmp(mode, "interrupted")) {
      struct snapshot current = transcript(&s);
      if (!strncmp(current.text, "> ", 2) || strstr(current.text, "\n> ")) {
        v.send(s.terminal, "\003", 1);
        sent = 1;
      }
    }
    if (now() >= deadline) {
      struct snapshot current = transcript(&s);
      fprintf(stderr, "Unicode fixture timed out: %s\n%s\n", mode,
              current.text);
      goto cleanup;
    }
  }
  if (waited < 0)
    goto cleanup;
  child = -1;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      (!strcmp(mode, "interrupted") && !sent) || output_closed(&s, deadline))
    goto cleanup;
  result->before = transcript(&s);
  resize(&s, 80, 12);
  result->after = transcript(&s);
  file = fopen(trace, "rb");
  if (!file)
    goto cleanup;
  result->length = fread(result->bytes, 1, sizeof(result->bytes), file);
  failed = ferror(file) || result->length == sizeof(result->bytes);
  fclose(file);
cleanup:
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (slave >= 0)
    close(slave);
  v.destroy(s.window);
  return failed;
}
static int unicode_case(const char *fixture, const char *trace,
                        const char *mode, const char *producer,
                        const char *source, int chunk, int filled) {
  struct unicode_result expected, actual;
  if (unicode_render(fixture, trace, "direct", producer, source, chunk, filled,
                     &expected) ||
      unicode_render(fixture, trace, mode, producer, source, chunk, filled,
                     &actual))
    return 1;
  if (strcmp(actual.before.text, expected.before.text) ||
      strcmp(actual.after.text, expected.after.text) ||
      actual.length != expected.length ||
      memcmp(actual.bytes, expected.bytes, actual.length)) {
    fprintf(
        stderr,
        "Unicode mismatch: mode=%s producer=%s chunk=%d filled=%d source=%s\n"
        "Before expected: %s\nBefore actual: %s\nAfter expected: %s\nAfter "
        "actual: %s\n",
        mode, producer, chunk, filled, source, expected.before.text,
        actual.before.text, expected.after.text, actual.after.text);
    return 1;
  }
  return 0;
}
static int unicode_cases(const char *fixture, const char *build) {
  const char *raw[] = {"\033[1m🇸🇪\033[0mXYZ\n",
                       "\033[3m👩‍💻\033[0mXYZ\n",
                       "éXYZ 中文.\n",
                       "❤️XYZ 1️⃣.\n",
                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa🇸🇪XYZ\n",
                       "🇸🇪👩‍💻\nnext line\n",
                       "🄀中X\n",
                       "中🄀X\n",
                       "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa🄀中X\n",
                       "é🄀中X\n",
                       "🆎🆑中X\n",
                       "a‌b\n",
                       "a🂡b\n",
                       "🀄🃏⌚中X\n",
                       "a⚗🛠b\n",
                       "a⁠b\n",
                       "a﻿b\n",
                       "a‎b\n",
                       "a࢘b\n",
                       "a؀b\n",
                       "aᅠb\n",
                       "a­b\n",
                       "a⃝b\n"};
  const char *markdown[] = {"Hello **world** and `code`.\n",
                            "**🇸🇪**XYZ\n",
                            "🇸🇪**XYZ**\n",
                            "`👩‍💻`XYZ\n",
                            "éXYZ 中文.\n",
                            "❤️XYZ 1️⃣.\n"};
  char work[4096], trace[4096];
  size_t index, count;
  int producer, chunk, filled, failed = 0;
  snprintf(work, sizeof(work), "%s/unicode-output.XXXXXX", build);
  if (!mkdtemp(work))
    return 1;
  if (strlen(work) + sizeof("/trace.bytes") > sizeof(trace)) {
    rmdir(work);
    return 1;
  }
  strcpy(trace, work);
  strcat(trace, "/trace.bytes");
  for (producer = 0; producer < 2 && !failed; ++producer) {
    const char **sources = producer ? markdown : raw;
    count = producer ? sizeof(markdown) / sizeof(markdown[0])
                     : sizeof(raw) / sizeof(raw[0]);
    for (index = 0; index < count && !failed; ++index)
      for (chunk = 0; chunk < 2 && !failed; ++chunk)
        for (filled = 0; filled < 2 && !failed; ++filled)
          failed = unicode_case(
              fixture, trace, "active", producer ? "mdf" : "raw",
              sources[index], chunk ? (int)strlen(sources[index]) : 1, filled);
  }
  for (index = 0; index < 4 && !failed; ++index)
    for (filled = 0; filled < 2 && !failed; ++filled)
      failed = unicode_case(fixture, trace, "interrupted", "raw", raw[index], 1,
                            filled);
  unlink(trace);
  rmdir(work);
  return failed || errors;
}
static int capture(const char *fixture, const char *mode, const char *argument,
                   char *bytes, size_t capacity) {
  struct winsize size;
  struct pollfd descriptor;
  int master = -1, slave = -1, status, failed = 1, eof = 0;
  pid_t child = -1;
  double deadline;
  size_t used = 0;
  ssize_t count;
  memset(&size, 0, sizeof(size));
  size.ws_row = 8;
  size.ws_col = 40;
  if (openpty(&master, &slave, NULL, NULL, &size))
    goto cleanup;
  child = fork();
  if (child == 0) {
    close(master);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    if (argument)
      execl(fixture, fixture, mode, argument, (char *)NULL);
    else
      execl(fixture, fixture, mode, (char *)NULL);
    _exit(127);
  }
  if (child < 0)
    goto cleanup;
  close(slave);
  slave = -1;
  descriptor.fd = master;
  descriptor.events = POLLIN;
  deadline = now() + 5;
  while (now() < deadline && used + 1 < capacity) {
    if (poll(&descriptor, 1, 20) < 0)
      goto cleanup;
    if (!(descriptor.revents & (POLLIN | POLLHUP)))
      continue;
    count = read(master, bytes + used, capacity - used - 1);
    if (count == 0 || (count < 0 && errno == EIO)) {
      eof = 1;
      break;
    }
    if (count < 0)
      goto cleanup;
    used += (size_t)count;
  }
  bytes[used] = 0;
  if (!eof || waitpid(child, &status, 0) != child)
    goto cleanup;
  child = -1;
  failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
cleanup:
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  return failed;
}
static int without_cpr(const char *fixture) {
  struct screen actual = create(), expected = create();
  struct snapshot a, e;
  char bytes[16384];
  int failed = 1;
  if (capture(fixture, "--consecutive-streams", NULL, bytes, sizeof(bytes)))
    goto cleanup;
  v.feed(actual.terminal, bytes, (long)strlen(bytes));
  v.feed(expected.terminal, "FIRST\r\nSECOND\r\nTHIRD\r\n", 22);
  pump();
  a = snapshot(&actual, 0);
  e = snapshot(&expected, 0);
  if (strcmp(a.text, e.text) || a.row != e.row || a.column != e.column)
    goto cleanup;
  v.destroy(actual.window);
  v.destroy(expected.window);
  pump();
  actual = create();
  expected = create();
  if (capture(fixture, "--quoted-after", "abc", bytes, sizeof(bytes)))
    goto cleanup;
  v.feed(expected.terminal, bytes, (long)strlen(bytes));
  if (capture(fixture, "--quoted-after", "abc\r", bytes, sizeof(bytes)))
    goto cleanup;
  v.feed(actual.terminal, bytes, (long)strlen(bytes));
  pump();
  a = snapshot(&actual, 0);
  e = snapshot(&expected, 0);
  failed = strcmp(a.text, e.text) || a.row != e.row || a.column != e.column;
cleanup:
  if (failed)
    fprintf(stderr, "No-CPR stream/quoted CR transcript changed\n");
  v.destroy(actual.window);
  v.destroy(expected.window);
  pump();
  return failed || errors;
}
static int interrupted(const char *fixture, const char *disposition) {
  struct screen s = create();
  struct winsize size;
  struct snapshot current;
  int master = -1, slave = -1, status, failed = 1, sent = 0;
  pid_t child = -1, waited;
  double deadline;
  void *pty;
  memset(&size, 0, sizeof(size));
  size.ws_col = 40;
  size.ws_row = 8;
  if (openpty(&master, &slave, NULL, NULL, &size))
    goto cleanup;
  pty = v.pty(master, NULL, NULL);
  if (!pty) {
    close(master);
    goto cleanup;
  }
  v.set_pty(s.terminal, pty);
  v.unref(pty);
  child = fork();
  if (child == 0) {
    close(master);
    dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2)
      close(slave);
    execl(fixture, fixture, "--interrupt", disposition, (char *)NULL);
    _exit(127);
  }
  close(slave);
  slave = -1;
  if (child < 0)
    goto cleanup;
  deadline = now() + 5;
  while ((waited = waitpid(child, &status, WNOHANG)) == 0) {
    pump_once();
    current = transcript(&s);
    if (!sent &&
        (!strncmp(current.text, "> ", 2) || strstr(current.text, "\n> "))) {
      v.send(s.terminal, "\003", 1);
      sent = 1;
    }
    if (now() >= deadline)
      goto cleanup;
  }
  if (waited < 0)
    goto cleanup;
  child = -1;
  if (output_closed(&s, deadline))
    goto cleanup;
  current = transcript(&s);
  failed = !sent || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
           strcmp(current.text, "AFTER INTERRUPT\n") || current.row != 1 ||
           current.column != 0;
cleanup:
  if (failed)
    fprintf(stderr, "Interrupt restoration failed: %s\n", disposition);
  if (child > 0) {
    kill(child, SIGTERM);
    waitpid(child, &status, 0);
  }
  if (slave >= 0)
    close(slave);
  v.destroy(s.window);
  pump();
  return failed || errors;
}
static int boundaries(const char *fixture) {
  char text[2048], initial[41];
  const char *glyphs[] = {"🇸🇪", "👩‍💻", "❤️", "1️⃣"};
  int sample, index, handoff, grow;
  for (sample = 0; sample < 8; ++sample) {
    text[0] = 0;
    if (sample < 4) {
      memset(text, 'a', sample == 2 ? 39 : 40);
      text[sample == 2 ? 39 : 40] = 0;
      if (sample == 1)
        strcat(text, "X");
      else if (sample == 2)
        strcat(text, "éX");
      else if (sample == 3)
        strcat(text, "́X");
    } else {
      for (index = 0; index < (sample == 5 ? 10 : 22); ++index)
        strcat(text, glyphs[sample - 4]);
      strcat(text, "X");
    }
    for (handoff = 0; handoff < 2; ++handoff)
      for (grow = 0; grow <= !handoff; ++grow) {
        struct snapshot expected;
        char flat[32768], content[2050];
        size_t used = 0, byte;
        if (render(fixture, text, 0, handoff, grow, 0, NULL, &expected))
          return 1;
        for (byte = 0; expected.text[byte]; ++byte)
          if (expected.text[byte] != '\n')
            flat[used++] = expected.text[byte];
        flat[used] = 0;
        strcpy(content, text);
        if (handoff)
          strcat(content, "Y");
        if (strcmp(flat, content) ||
            compare_expected(fixture, text, handoff, grow, 0, NULL, &expected))
          return 1;
      }
  }
  for (sample = 39; sample <= 40; ++sample) {
    memset(initial, 'a', (size_t)sample);
    initial[sample] = 0;
    for (handoff = 0; handoff < 2; ++handoff)
      for (grow = 0; grow <= !handoff; ++grow)
        if (compare(fixture, "X", handoff, grow, 0, initial))
          return 1;
  }
  return errors != 0;
}
int main(int argc, char **argv) {
  const char *cases[] = {"abcdefghijklmnopqrstuvwxyz",
                         "one\nABC",
                         "abc\r",
                         "abc\rZ",
                         "café 中文",
                         "é👩‍💻🇸🇪中",
                         "\033[1mStyled\033[0m",
                         "áX",
                         "🄀中X",
                         "中🏳X"};
  char wrapped[128];
  size_t index;
  int handoff, grow;
  if ((argc != 3 && argc != 4) || load())
    return 2;
  if (argc == 4)
    return !strcmp(argv[1], "unicode") ? unicode_cases(argv[2], argv[3]) : 2;
  if (!strcmp(argv[1], "control"))
    return without_cpr(argv[2]) || interrupted(argv[2], "ignore") ||
           interrupted(argv[2], "handler");
  if (!strcmp(argv[1], "boundaries"))
    return boundaries(argv[2]);
  if (!strcmp(argv[1], "tabs")) {
    const char *tabs[] = {"\tX", "a\tX", "a\t\tX"};
    for (index = 0; index < sizeof(tabs) / sizeof(tabs[0]); ++index)
      if (compare(argv[2], tabs[index], 0, 0, 1, NULL))
        return 1;
  } else if (!strcmp(argv[1], "opaque")) {
    const char *opaque[] = {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaa🄀中X",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaa中🏳X",
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaa中中\r"
                            "bbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                            "\t\t\tX", "é👩‍💻🇸🇪中"};
    for (grow = 0; grow <= 1; ++grow)
      for (index = 0; index < sizeof(opaque) / sizeof(opaque[0]); ++index)
        if (compare(argv[2], opaque[index], 0, grow, 0, NULL))
          return 1;
  } else if (!strcmp(argv[1], "native")) {
    memset(wrapped, 'a', 40);
    wrapped[40] = 0;
    for (handoff = 0; handoff <= 1; ++handoff)
      for (grow = 0; grow <= !handoff; ++grow) {
        for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
          if (compare(argv[2], cases[index], handoff, grow, 0, NULL))
            return 1;
        if (compare(argv[2], wrapped, handoff, grow, 0, NULL))
          return 1;
        wrapped[40] = 'X';
        wrapped[41] = 0;
        if (compare(argv[2], wrapped, handoff, grow, 0, NULL))
          return 1;
        wrapped[40] = 0;
        if (compare(argv[2], "X", handoff, grow, 0, wrapped))
          return 1;
      }
  } else
    return 2;
  if (errors)
    return 1;
  puts("Native GTK/VTE output, byte chunks, Unicode, endpoint and static "
       "reflow passed.");
  return 0;
}
