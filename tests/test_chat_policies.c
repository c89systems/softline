#include "console_pty.h"
#include <dirent.h>

static int idle_after(struct editor *e, size_t offset, const char *completed) {
  double deadline = now() + 10;
  do {
    const char *done = strstr(e->printed + offset, completed);
    if (done && strstr(done, c_chat ? "+ streaming demo" : "+ gpt-5.6-terra"))
      return 0;
    if (pump(e, 20))
      return 1;
  } while (now() < deadline);
  return 1;
}

static int reattached(struct editor *e, size_t offset) {
  double deadline = now() + 2;
  if (!remote_owner)
    return 0;
  do {
    /* A resumed empty editor first paints the unsynchronized panel. Its
     * removal leaves the reasoning row, status line and single input row. */
    if (strstr(e->bytes + offset, "P 0 pending") && !e->escape_length &&
        e->bottom == e->rows - 4)
      return 0;
    if (pump(e, 10))
      return 1;
  } while (now() < deadline);
  return 1;
}

static int exercise(const char *build, const char *shell,
                    const char *filename) {
  struct editor e;
  char directory[4096], path[4096], descriptor[16];
  char history_path[4096];
  const char *completed =
      c_chat ? "Try another prompt." : "[operation] produced a result.";
  size_t mark, printed;
  int gate[2], failed = 0;
  DIR *dir;
  struct dirent *item;
  if (snprintf(directory, sizeof(directory), "%s/chat-policies.XXXXXX",
               build) >= (int)sizeof(directory) ||
      !mkdtemp(directory) || pipe(gate))
    return 1;
  snprintf(descriptor, sizeof(descriptor), "%d", gate[0]);
  if (snprintf(history_path, sizeof(history_path), "%s/%s", directory,
               filename) >= (int)sizeof(history_path))
    return 1;
  setenv("SOFTLINE_CHAT_GATE_FD", descriptor, 1);
  setenv("SOFTLINE_HISTORY_DIR", directory, 1);
  setenv("SOFTLINE_CHAT_CHAR_MS", "0", 1);
  setenv("SOFTLINE_CHAT_OPERATION_STEP_MS", "0", 1);
  setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "0", 1);
  setenv("SOFTLINE_PROMPT_THEME", "plain", 1);
  setenv("SHELL", shell, 1);
  test_echo = 1;
  test_rows = 24;
  test_columns = 120;
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "Policy demo:", 0) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", 0) == 0);
  /* The entire burst enters the native queue before any command can run. */
  mark = e.length;
  REQUIRE(send_bytes(&e, "/statux\r\033e\177s\r") == 0);
  REQUIRE(wait_for(&e, "Status: idle / backlog running", mark) == 0);
  REQUIRE(!contains(&e, "Thinking...", mark));
  REQUIRE(send_bytes(&e, "start\r") == 0);
  REQUIRE(wait_for(&e, "Thinking...", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/steer verify\033\r/shell\r") == 0);
  REQUIRE(wait_for(&e, "Q 2. /shell", mark) == 0);
  REQUIRE(!contains(&e, "Steering applied.", mark));
  REQUIRE(!contains(&e, "SHELL_READY", mark));
  REQUIRE(send_bytes(&e, "\033e\025!sh\r/status\r") == 0);
  REQUIRE(wait_for(&e, "Status: busy / backlog running / 2 queued", mark) == 0);
  REQUIRE(wait_for(&e, "Q 2. !sh", mark) == 0);
  REQUIRE(!contains(&e, "SHELL_READY", mark));
  REQUIRE(send_bytes(&e, "/approve on\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval ON", mark) == 0);
  REQUIRE(
      wait_records(
          &e, history_path,
          "/statux\nstart\n/steer verify\n/shell\n/status\n/approve on\n") ==
      0);
  /* A real inherited pipe gates the worker, not an elapsed-time assertion. */
  printed = e.printed_length;
  REQUIRE(write(gate[1], "x\n", 2) == 2);
  REQUIRE(wait_for(&e, "Steering applied.", mark) == 0);
  REQUIRE(wait_for(&e, "SHELL_READY -i cooked foreground", mark) == 0);
  {
    const char *steered = strstr(e.printed + printed, "Steering applied.");
    const char *done = strstr(e.printed + printed, completed);
    const char *ready = strstr(e.printed + printed, "SHELL_READY");
    REQUIRE(steered && done && ready && steered < done && done < ready);
  }
  mark = e.length;
  REQUIRE(send_bytes(&e, "exit\n") == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", mark) == 0);
  REQUIRE(reattached(&e, mark) == 0);
  mark = e.length;
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "edit-boundary\r") == 0);
  REQUIRE(wait_for(&e, "Thinking...", mark) == 0);
  REQUIRE(send_bytes(&e, "/shell\r\033eX") == 0);
  REQUIRE(wait_for(&e, "Q editing / Alt-N discard", mark) == 0);
  REQUIRE(wait_for(&e, "/shellX", mark) == 0);
  REQUIRE(write(gate[1], "x\n", 2) == 2);
  REQUIRE(wait_for(&e, c_chat ? "The stream is still live." : completed,
                   mark) == 0);
  REQUIRE(idle_after(&e, printed,
                     c_chat ? "The stream is still live." : completed) == 0);
  /* Discard deletes the shell entry even after its execution boundary becomes
   * ready. It must not launch without a new deliberate submission. */
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033n") == 0);
  REQUIRE(send_bytes(&e, "/status\r") == 0);
  REQUIRE(wait_for(&e, "Status: idle / backlog running / 0 queued", mark) == 0);
  REQUIRE(!contains(&e, "SHELL_READY", mark));
  REQUIRE(send_bytes(&e, "!sh\r") == 0);
  REQUIRE(wait_for(&e, "SHELL_READY", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "exit\n") == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", mark) == 0);
  REQUIRE(reattached(&e, mark) == 0);
  /* The next worker is also gated, so cancellation and backlog retention are
   * observed independently of its duration. */
  REQUIRE(send_bytes(&e, "second\r") == 0);
  REQUIRE(wait_for(&e, "Thinking...", mark) == 0);
  REQUIRE(send_bytes(&e, "old\r") == 0);
  REQUIRE(wait_for(&e, "Q 1. old", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Operation cancelled" : "[operation] cancelled",
                   mark) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/status\r") == 0);
  REQUIRE(wait_for(&e, "Status: idle / backlog paused / 1 queued", mark) == 0);
  REQUIRE(!contains(&e, c_chat ? "> old" : "[queued] old", mark));
  mark = e.length;
  REQUIRE(send_bytes(&e, "/resume\r") == 0);
  REQUIRE(wait_for(&e, "Backlog resumed.", mark) == 0);
  REQUIRE(wait_for(&e, c_chat ? "> old" : "[queued] old", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Operation cancelled" : "[operation] cancelled",
                   mark) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", mark) == 0);
  REQUIRE(send_bytes(&e, "/quit\r") == 0);
  REQUIRE(finish(&e) == 0);
  close_editor(&e);
  REQUIRE(plain_input(&e, "/status\n/quit\n", "plain") == 0);
  REQUIRE(contains(&e, "/status", 0) && !contains(&e, "Policy demo:", 0));
cleanup:
  close_editor(&e);
  close(gate[0]);
  close(gate[1]);
  dir = opendir(directory);
  if (dir) {
    while ((item = readdir(dir)) != NULL) {
      if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, ".."))
        continue;
      if (snprintf(path, sizeof(path), "%s/%s", directory, item->d_name) <
          (int)sizeof(path))
        unlink(path);
    }
    closedir(dir);
  }
  rmdir(directory);
  return failed;
}

int main(int argc, char **argv) {
  if (argc != 7 && argc != 8)
    return 2;
  c_chat = !strcmp(argv[1], "c") || !strcmp(argv[1], "remote");
  if (argc == 8)
    remote_owner = argv[7];
  executable = argv[2];
  root = argv[3];
  return exercise(argv[4], argv[5], argv[6]);
}
