#include "console_pty.h"
#include <dirent.h>
#include <sys/stat.h>

static int fixture_shell(int argc, char **argv) {
  struct termios attrs;
  struct winsize size;
  char line[128];
  int index;
  if (tcgetattr(0, &attrs) || !(attrs.c_lflag & ICANON) ||
      !(attrs.c_lflag & ECHO) || tcgetpgrp(0) != getpgrp())
    return 3;
  printf("SHELL_READY");
  for (index = 1; index < argc; ++index)
    printf(" %s", argv[index]);
  puts(" cooked foreground");
  fflush(stdout);
  while (fgets(line, sizeof(line), stdin)) {
    line[strcspn(line, "\r\n")] = 0;
    if (!strcmp(line, "clear"))
      puts("\033[2J\033[HSHELL_CLEARED");
    else if (!strcmp(line, "size")) {
      if (ioctl(0, TIOCGWINSZ, &size))
        return 4;
      printf("SHELL_SIZE %u %u\n", size.ws_row, size.ws_col);
    } else if (!strcmp(line, "exit"))
      return 0;
    else if (!strcmp(line, "fail"))
      return 7;
    else if (!strcmp(line, "partial")) {
      fputs("SHELL_PARTIAL", stdout);
      fflush(stdout);
      return 0;
    }
    fflush(stdout);
  }
  return 0;
}
static int prompt(struct editor *e, size_t offset) {
  struct termios attrs;
  double deadline;
  if (wait_for(e, "\033[?2004h", offset))
    return 1;
  deadline = now() + .1;
  while (now() < deadline)
    if (pump(e, 10))
      return 1;
  if (tcgetattr(e->master, &attrs))
    return 1;
  return e->row != e->rows - 1 || (attrs.c_lflag & ICANON) ||
         e->bottom >= e->rows - 1;
}
static void settle(struct editor *e, double seconds) {
  double deadline = now() + seconds;
  while (now() < deadline)
    if (pump(e, 10))
      break;
}
static int quit(struct editor *e) {
  struct termios attrs;
  if (send_bytes(e, "/quit\r") || finish(e) || tcgetattr(e->master, &attrs))
    return 1;
  return !(attrs.c_lflag & ICANON) || !(attrs.c_lflag & ECHO) || e->top != 0 ||
         e->bottom != e->rows - 1;
}
static int geometry(struct editor *e, int rows, int columns) {
  struct winsize size;
  memset(&size, 0, sizeof(size));
  size.ws_row = (unsigned short)rows;
  size.ws_col = (unsigned short)columns;
  if (ioctl(e->master, TIOCSWINSZ, &size))
    return 1;
  e->rows = rows;
  e->columns = columns;
  if (e->row >= rows)
    e->row = rows - 1;
  if (e->column >= columns)
    e->column = columns - 1;
  e->top = 0;
  e->bottom = rows - 1;
  return 0;
}
static int wait_cursor(struct editor *e, const char *text, size_t offset,
                       int row, int column) {
  double deadline;
  if (wait_for(e, text, offset))
    return 1;
  deadline = now() + 2;
  do {
    if (!e->escape_length && e->row == row && e->column == column)
      return 0;
    if (pump(e, 20))
      break;
  } while (now() < deadline);
  fprintf(stderr, "Cursor after %s: expected (%d,%d), got (%d,%d)\n", text, row,
          column, e->row, e->column);
  return 1;
}
static int preserve_draft(const char *shell, const char *history_path) {
  struct editor e;
  const char *exits[] = {"exit\n", "fail\n", "partial\n"};
  int failed = 0, index;
  size_t mark;
  REQUIRE(setenv("SHELL", shell, 1) == 0);
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(prompt(&e, 0) == 0);
  REQUIRE(send_bytes(&e, "\033[200~draft first\nsecond tail\033[201~"
                         "\002\002\002\002") == 0);
  for (index = 0; index < 3; ++index) {
    mark = e.length;
    REQUIRE(send_bytes(&e, "\033s") == 0);
    REQUIRE(wait_for(&e, "SHELL_READY -i cooked foreground", mark) == 0);
    REQUIRE(e.top == 0 && e.bottom == e.rows - 1);
    REQUIRE(contains(&e, "\033[?2004l", mark));
    /* Suspending must not accept the draft into persistent history. */
    REQUIRE(records(history_path, ""));
    REQUIRE(send_bytes(&e, "clear\n") == 0);
    REQUIRE(wait_for(&e, "SHELL_CLEARED", mark) == 0);
    REQUIRE(geometry(&e, index == 1 ? 12 : 30, index == 1 ? 50 : 100) == 0);
    mark = e.length;
    REQUIRE(send_bytes(&e, exits[index]) == 0);
    REQUIRE(prompt(&e, mark) == 0);
    REQUIRE(contains(&e, "draft first", mark));
    REQUIRE(contains(&e, "second tail", mark));
    /* Force the text and final cursor control into separate PTY reads. */
    e.read_limit = 1;
    mark = e.length;
    REQUIRE(send_bytes(&e, "X") == 0);
    REQUIRE(wait_cursor(&e, "Xtail", mark, e.rows - 1, 10) == 0);
    mark = e.length;
    REQUIRE(send_bytes(&e, "\037") == 0);
    REQUIRE(wait_cursor(&e, "tail", mark, e.rows - 1, 9) == 0);
    e.read_limit = 0;
  }
  REQUIRE(send_bytes(&e, "!\r") == 0);
  REQUIRE(wait_records(&e, history_path, "draft first\\nsecond !tail\n") == 0);
  REQUIRE(send_bytes(&e, "\003") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Operation cancelled" : "[operation] cancelled",
                   0) == 0);
  REQUIRE(prompt(&e, 0) == 0);
  REQUIRE(quit(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int exercise(const char *shell) {
  struct editor e;
  int failed = 0, alias, dimension, quotes;
  size_t mark, printed;
  const char *aliases[] = {"!sh\r", "/shell\033\r"};
  const int dimensions[][2] = {{12, 50}, {30, 110}, {18, 60}};
  char marker[64];
  const char *position;
  if (setenv("SHELL", shell, 1))
    return 1;
  if (remote_owner)
    setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1);
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(prompt(&e, 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "!sh\t") == 0);
  REQUIRE(wait_for(&e, "!sh", mark) == 0);
  REQUIRE(!contains(&e, "SHELL_READY", mark));
  REQUIRE(send_bytes(&e, "\025") == 0);
  for (alias = 0; alias < 2; ++alias) {
    mark = e.length;
    REQUIRE(send_bytes(&e, aliases[alias]) == 0);
    REQUIRE(wait_for(&e, "SHELL_READY -i cooked foreground", mark) == 0);
    REQUIRE(e.top == 0 && e.bottom == e.rows - 1);
    REQUIRE(contains(&e, "\033[?2004l", mark));
    REQUIRE(send_bytes(&e, "clear\n") == 0);
    REQUIRE(wait_for(&e, "SHELL_CLEARED", mark) == 0);
    for (dimension = 0; dimension < 3; ++dimension) {
      mark = e.length;
      REQUIRE(geometry(&e, dimensions[dimension][0],
                       dimensions[dimension][1]) == 0);
      REQUIRE(send_bytes(&e, "size\n") == 0);
      snprintf(marker, sizeof(marker), "SHELL_SIZE %d %d",
               dimensions[dimension][0], dimensions[dimension][1]);
      REQUIRE(wait_for(&e, marker, mark) == 0);
    }
    mark = e.length;
    REQUIRE(send_bytes(&e, "exit\n") == 0);
    REQUIRE(prompt(&e, mark) == 0);
  }
  mark = e.length;
  REQUIRE(send_bytes(&e, "!sh\r") == 0);
  REQUIRE(wait_for(&e, "SHELL_READY", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "fail\n") == 0);
  REQUIRE(wait_for(&e, "shell exited unsuccessfully", mark) == 0);
  REQUIRE(prompt(&e, mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/shell\r") == 0);
  REQUIRE(wait_for(&e, "SHELL_READY", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "partial\n") == 0);
  REQUIRE(wait_for(&e, "SHELL_PARTIAL", mark) == 0);
  REQUIRE(prompt(&e, mark) == 0);
  mark = e.length;
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "!sh\r") == 0);
  REQUIRE(wait_for(&e, "SHELL_READY", mark) == 0);
  position = e.printed + printed;
  quotes = 0;
  while ((position = strstr(position, "> !sh")) != NULL) {
    size_t at = (size_t)(position - e.printed);
    if (e.print_rows[at] < e.rows - 1) {
      REQUIRE(e.print_columns[at] == 0);
      ++quotes;
    }
    ++position;
  }
  REQUIRE(quotes > 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "exit\n") == 0);
  REQUIRE(prompt(&e, mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033[A") == 0);
  REQUIRE(wait_for(&e, "!sh", mark) == 0);
  REQUIRE(send_bytes(&e, "\025start\r") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Thinking..." : "[operation] processing input.",
                   mark) == 0);
  mark = e.length;
  if (remote_owner) {
    REQUIRE(send_bytes(&e, "\033[200~preserved draft\nsecond tail\033[201~"
                           "\002\002\002\002\033s") == 0);
    REQUIRE(wait_for(&e, "SHELL_READY", mark) == 0);
    REQUIRE(e.top == 0 && e.bottom == e.rows - 1);
    mark = e.length;
    REQUIRE(send_bytes(&e, "exit\n") == 0);
    REQUIRE(prompt(&e, mark) == 0);
    REQUIRE(wait_for(&e, "> preserved draft", mark) == 0);
    mark = e.length;
    REQUIRE(send_bytes(&e, "X") == 0);
    REQUIRE(wait_cursor(&e, "Xtail", mark, e.rows - 1, 10) == 0);
    mark = e.length;
    REQUIRE(send_bytes(&e, "\037") == 0);
    REQUIRE(wait_cursor(&e, "tail", mark, e.rows - 1, 9) == 0);
    REQUIRE(send_bytes(&e, "\033n") == 0);
  } else {
    REQUIRE(send_bytes(&e, "\033s") == 0);
    REQUIRE(wait_for(&e, "Shell is available only between turns.", mark) == 0);
    REQUIRE(!contains(&e, "SHELL_READY", mark));
  }
  mark = e.length;
  REQUIRE(send_bytes(&e, "\025queued\r") == 0);
  REQUIRE(wait_for(&e, "Q 1. queued", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\003") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Operation cancelled" : "[operation] cancelled",
                   mark) == 0);
  /* Cancellation output precedes re-entering raw prompt input. Wait for that
   * observable handoff before sending a new command. */
  REQUIRE(prompt(&e, mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/shell\r") == 0);
  REQUIRE(wait_for(&e, "SHELL_READY", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "exit\n") == 0);
  REQUIRE(prompt(&e, mark) == 0);
  REQUIRE(contains(&e, "Q 1. queued", mark));
  REQUIRE(quit(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int fallback(const char *shell) {
  struct editor e;
  struct termios attrs;
  size_t mark;
  int failed = 0;
  if (shell) {
    if (setenv("SHELL", shell, 1))
      return 1;
  } else
    unsetenv("SHELL");
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(prompt(&e, 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/shell\r") == 0);
  settle(&e, .3);
  if (!shell || strcmp(shell, "/bin/bash")) {
    REQUIRE(tcgetattr(e.master, &attrs) == 0);
    REQUIRE(attrs.c_lflag & ICANON);
  }
  REQUIRE(send_bytes(&e, "printf 'REAL_SHELL_READY\\n'\n") == 0);
  REQUIRE(wait_for(&e, "REAL_SHELL_READY\r\n", mark) == 0);
  REQUIRE(send_bytes(&e, "sleep 30\n") == 0);
  settle(&e, .1);
  REQUIRE(send_bytes(&e, "\003") == 0);
  settle(&e, .1);
  mark = e.length;
  REQUIRE(send_bytes(&e, "exit\n") == 0);
  REQUIRE(prompt(&e, mark) == 0);
  REQUIRE(quit(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int failed_launch(const char *directory) {
  struct editor e;
  char missing[4096];
  size_t mark;
  int failed = 0;
  if (snprintf(missing, sizeof(missing), "%s/nonexistent-shell", directory) >=
          (int)sizeof(missing) ||
      setenv("SHELL", missing, 1))
    return 1;
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(prompt(&e, 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "kept-draft\033s") == 0);
  REQUIRE(wait_for(&e, "shell exited unsuccessfully", mark) == 0);
  REQUIRE(prompt(&e, mark) == 0);
  REQUIRE(contains(&e, "kept-draft", mark));
  REQUIRE(send_bytes(&e, "\025") == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/shell\r") == 0);
  REQUIRE(wait_for(&e, "shell exited unsuccessfully", mark) == 0);
  REQUIRE(prompt(&e, mark) == 0);
  REQUIRE(quit(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int busy_history(const char *path) {
  struct editor e;
  const char *aliases[] = {"!sh", "/shell"};
  char expected[512] = "!sh\n/shell\nbusy-history-start\n", input[64];
  FILE *file;
  size_t mark;
  int failed = 0, alias;
  file = fopen(path, "wb");
  if (!file)
    return 1;
  fputs("!sh\n/shell\n", file);
  if (fclose(file) || chmod(path, 0600))
    return 1;
  if (setenv("SOFTLINE_CHAT_OPERATION_STEP_MS", "5000", 1))
    return 1;
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(prompt(&e, 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "busy-history-start\r") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Thinking..." : "[operation] processing input.",
                   mark) == 0);
  REQUIRE(wait_records(&e, path, expected) == 0);
  for (alias = 0; alias < 2; ++alias) {
    mark = e.length;
    snprintf(input, sizeof(input), "\022%s", aliases[alias]);
    REQUIRE(send_bytes(&e, input) == 0);
    REQUIRE(wait_for(&e, aliases[alias], mark) == 0);
    settle(&e, .05);
    REQUIRE(send_bytes(&e, "\r") == 0);
    settle(&e, .1);
    REQUIRE(records(path, expected));
    REQUIRE(send_bytes(&e, "-edited\r") == 0);
    strcat(expected, aliases[alias]);
    strcat(expected, "-edited\n");
    REQUIRE(wait_records(&e, path, expected) == 0);
    REQUIRE(!contains(&e, "SHELL_READY", 0));
  }
cleanup:
  close_editor(&e);
  return failed;
}
static void remove_files(const char *directory) {
  DIR *dir = opendir(directory);
  struct dirent *entry;
  char path[4096];
  if (!dir)
    return;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) <
        (int)sizeof(path))
      unlink(path);
  }
  closedir(dir);
}
int main(int argc, char **argv) {
  char directory[4096] = {0}, shell[4096] = {0}, history_dir[4096] = {0},
       history_path[4096] = {0}, file[4096];
  struct editor e;
  const char *position;
  int failed = 1, count = 0;
  if (argc == 2 && !strcmp(argv[1], "-i"))
    return fixture_shell(argc, argv);
  if ((argc != 7 && argc != 8) ||
      (strcmp(argv[1], "c") && strcmp(argv[1], "lua") &&
       strcmp(argv[1], "remote")))
    return 2;
  c_chat = strcmp(argv[1], "lua") != 0;
  if (argc == 8)
    remote_owner = argv[7];
  executable = argv[2];
  root = argv[3];
  test_rows = 24;
  test_columns = 100;
  test_echo = 1;
  if (snprintf(directory, sizeof(directory), "%s/chat-shell.XXXXXX", argv[4]) >=
          (int)sizeof(directory) ||
      !mkdtemp(directory))
    return 1;
  if (snprintf(shell, sizeof(shell), "%s/shell ' helper", directory) >=
          (int)sizeof(shell) ||
      snprintf(history_dir, sizeof(history_dir), "%s/prompt-history",
               directory) >= (int)sizeof(history_dir) ||
      snprintf(history_path, sizeof(history_path), "%s/%s", history_dir,
               argv[5]) >= (int)sizeof(history_path))
    goto cleanup;
  if (mkdir(history_dir, 0700) || symlink(argv[6], shell))
    goto cleanup;
  setenv("HOME", directory, 1);
  setenv("SOFTLINE_HISTORY_DIR", history_dir, 1);
  snprintf(file, sizeof(file), "%s/history", directory);
  setenv("HISTFILE", file, 1);
  setenv("ENV", "", 1);
  setenv("BASH_ENV", "", 1);
  setenv("INPUTRC", "/dev/null", 1);
  setenv("SHELL", shell, 1);
  setenv("SOFTLINE_PROMPT_THEME", "plain", 1);
  setenv("SOFTLINE_CHAT_CHAR_MS", "50", 1);
  if (busy_history(history_path))
    goto cleanup;
  remove_files(history_dir);
  setenv("SOFTLINE_CHAT_OPERATION_STEP_MS", "500", 1);
  if (preserve_draft(shell, history_path))
    goto cleanup;
  remove_files(history_dir);
  if (exercise(shell) || fallback(NULL) || fallback("") ||
      fallback("/bin/bash") || failed_launch(directory))
    goto cleanup;
  setenv("SHELL", shell, 1);
  if (plain_input(&e, "!sh arg\n/shell arg\n!sh\n/shell\n/quit\n", "plain")) {
    close_editor(&e);
    goto cleanup;
  }
  position = e.bytes;
  while ((position = strstr(
              position, "Shell requires an interactive terminal.")) != NULL) {
    ++count;
    ++position;
  }
  failed = count != 2 || !strstr(e.bytes, "!sh arg") ||
           !strstr(e.bytes, "/shell arg") || strstr(e.bytes, "SHELL_READY") ||
           strstr(e.bytes, "\033[");
  close_editor(&e);
cleanup:
  remove_files(history_dir);
  rmdir(history_dir);
  remove_files(directory);
  rmdir(directory);
  return failed;
}
