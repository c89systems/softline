#include "console_pty.h"
#include <dirent.h>

static char history_path[4096];

/* The editor and transcript both use '> '. Count only text painted inside
 * the output region, so echoing an edited draft cannot masquerade as delivery.
 */
static int quoted(struct editor *e, const char *text, size_t offset) {
  const char *at = e->printed + offset;
  while ((at = strstr(at, text)) != NULL) {
    if (e->print_rows[at - e->printed] <= e->bottom)
      return 1;
    at++;
  }
  return 0;
}

static int wait_quote(struct editor *e, const char *text, size_t offset) {
  double deadline = now() + 3;
  do {
    if (quoted(e, text, offset))
      return 0;
    if (pump(e, 20))
      break;
  } while (now() < deadline);
  return 1;
}

/* Status redraws can occur between producer bytes. Observe producer text
 * independently of those UI writes, as the real terminal does. */
static int wait_output(struct editor *e, const char *text, size_t offset) {
  char *output = malloc(sizeof(e->printed));
  double deadline = now() + 3;
  int found = 0;
  if (!output)
    return 1;
  do {
    size_t i, used = 0;
    for (i = offset; i < e->printed_length; ++i)
      if (e->print_output[i])
        output[used++] = e->printed[i];
    output[used] = 0;
    if (strstr(output, text)) {
      found = 1;
      break;
    }
    if (pump(e, 20))
      break;
  } while (now() < deadline);
  free(output);
  return !found;
}

static int at_column(struct editor *e, const char *text, size_t offset,
                     int column) {
  const char *at = e->printed + offset;
  while ((at = strstr(at, text)) != NULL) {
    if (e->print_columns[at - e->printed] == column)
      return 1;
    at++;
  }
  return 0;
}

static int focused_input_cursor(struct editor *e, size_t offset) {
  double deadline = now() + 3;
  do {
    const char *at = strstr(e->printed + offset,
                            "Alt-Y approve / Alt-N reject / Alt-E edit");
    if (at && e->row == e->rows - 1 && e->column == 2)
      return 0;
    if (pump(e, 20))
      break;
  } while (now() < deadline);
  return 1;
}

static int exercise(void) {
  struct editor e;
  size_t mark, printed;
  int failed = 0;
  REQUIRE(start(&e, "", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "Approval demo:", 0) == 0);
  mark = e.length;
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "protected-draft") == 0);
  REQUIRE(wait_for(&e, "protected-draft", mark) == 0);
  REQUIRE(wait_for(&e, "2 awaiting approval", mark) == 0);
  REQUIRE(at_column(&e, "2 awaiting approval", printed, 2));
  REQUIRE(wait_records(&e, history_path, "") == 0);
  REQUIRE(!contains(&e, "Queued:", mark));
  REQUIRE(!contains(&e, "Steering:", mark));
  REQUIRE(!quoted(&e, "> Demo", printed));
  mark = e.length;
  REQUIRE(send_bytes(&e, "\025") == 0);
  REQUIRE(wait_for(&e,
                   "A 2 awaiting approval / Alt-A focus / Alt-E edit / Queued:",
                   mark) == 0);
  REQUIRE(at_column(&e, "A 2 awaiting approval", printed, 0));
  REQUIRE(wait_for(&e, "Demo queued request #1:", mark) == 0);
  REQUIRE(at_column(&e, "Demo queued request #1:", printed, 2));
  REQUIRE(e.bottom == e.rows - 6);
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "\033a") == 0);
  REQUIRE(wait_for(&e, "\033[1;7mA ", mark) == 0);
  REQUIRE(wait_for(&e, "Alt-Y approve / Alt-N reject / Alt-E edit", mark) == 0);
  REQUIRE(focused_input_cursor(&e, printed) == 0);
  REQUIRE(e.bottom == e.rows - 6);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033") == 0);
  REQUIRE(wait_for(&e, "Alt-A focus / Alt-E edit", mark) == 0);
  REQUIRE(send_bytes(&e, "\033a") == 0);
  REQUIRE(send_bytes(&e, "\033n") == 0);
  REQUIRE(wait_for(&e, "A 1 approval / Alt-A focus / Alt-E edit / Steering:",
                   mark) == 0);
  REQUIRE(!quoted(&e, "> Demo queued request #1", printed));
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033e") == 0);
  REQUIRE(wait_for(&e, "H editing: approval required", mark) == 0);
  REQUIRE(send_bytes(&e, "\025edited-demo-steer\r") == 0);
  REQUIRE(wait_for(&e, "Alt-E edit / Steering:", mark) == 0);
  REQUIRE(!quoted(&e, "> edited-demo-steer", printed));
  REQUIRE(send_bytes(&e, "\033a\033y") == 0);
  REQUIRE(wait_quote(&e, "> edited-demo-steer", printed) == 0);
  /* Held entries must survive producer seams and completion without being
   * taken or preventing an ordinary user turn from running. */
  REQUIRE(wait_for(&e, "Try another prompt.", mark) == 0);
  mark = e.length;
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "ordinary-user-turn\r") == 0);
  REQUIRE(wait_quote(&e, "> ordinary-user-turn", printed) == 0);
  REQUIRE(wait_for(&e, "Alt-A focus", mark) == 0);
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "\033a") == 0);
  REQUIRE(wait_for(&e, "Alt-Y approve / Alt-N reject / Alt-E edit", mark) == 0);
  REQUIRE(focused_input_cursor(&e, printed) == 0);
  REQUIRE(wait_output(&e, "The stream is still live.", printed) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\033") == 0);
  REQUIRE(wait_for(&e, "Alt-A focus", mark) == 0);
  REQUIRE(!contains(&e, "chat readline failed", 0));
  REQUIRE(send_bytes(&e, "/quit\r") == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}

static int automatic(void) {
  struct editor e;
  size_t mark, printed;
  int failed = 0;
  REQUIRE(start(&e, "", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "Approval demo:", 0) == 0);
  REQUIRE(send_bytes(&e, "draft") == 0);
  REQUIRE(wait_for(&e, "2 awaiting approval", 0) == 0);
  mark = e.length;
  printed = e.printed_length;
  REQUIRE(send_bytes(&e, "\025/approve on\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval ON", mark) == 0);
  mark = e.length;
  REQUIRE(wait_quote(&e, "> Demo queued request #1", printed) == 0);
  REQUIRE(wait_quote(&e, "> Demo steering request #2", printed) == 0);
  REQUIRE(wait_quote(&e, "> Demo queued request #3", printed) == 0);
  REQUIRE(!contains(&e, "Alt-A focus", mark));
  mark = e.length;
  REQUIRE(send_bytes(&e, "/approve off\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval OFF", mark) == 0);
  REQUIRE(wait_for(&e, "Alt-A focus", mark) == 0);
  REQUIRE(send_bytes(&e, "/approve maybe\r") == 0);
  REQUIRE(wait_for(&e, "Usage: /approve [on|off]", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\025/approve\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval ON", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/approve\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval OFF", mark) == 0);
  REQUIRE(!quoted(&e, "> /approve", 0));
  REQUIRE(send_bytes(&e, "\025/quit\r") == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}

static int commands(void) {
  struct editor e;
  size_t mark;
  int failed = 0;
  setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "0", 1);
  REQUIRE(start(&e, "", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/approve\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval ON", mark) == 0);
  /* A usage response proves the preceding explicit command was consumed.
   * The subsequent toggle then exposes its state even if setting the same
   * value did not repaint the status. */
  mark = e.length;
  REQUIRE(send_bytes(&e, "/approve on\r/approve maybe\r") == 0);
  REQUIRE(wait_for(&e, "Usage: /approve [on|off]", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\025/approve\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval OFF", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/approve off\r/approve maybe\r") == 0);
  REQUIRE(wait_for(&e, "Usage: /approve [on|off]", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\025/approve\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval ON", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "/approve maybe\r") == 0);
  REQUIRE(wait_for(&e, "Usage: /approve [on|off]", mark) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "\025/approve\r") == 0);
  REQUIRE(wait_for(&e, "Auto-approval OFF", mark) == 0);
  REQUIRE(!quoted(&e, "> /approve", 0));
  REQUIRE(send_bytes(&e, "/quit\r") == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}

static int invalid_seeds(void) {
  struct editor e;
  const char *values[] = {"not-a-seed", "-1", "42949672960"};
  size_t i;
  int failed = 0;
  for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    setenv("SOFTLINE_CHAT_APPROVAL_SEED", values[i], 1);
    REQUIRE(start(&e, "", NULL, NULL) == 0);
    REQUIRE(wait_for(&e, "SOFTLINE_CHAT_APPROVAL_SEED must be", 0) == 0);
    REQUIRE(finish(&e) == 1 && e.child == -1);
    close_editor(&e);
  }
cleanup:
  close_editor(&e);
  setenv("SOFTLINE_CHAT_APPROVAL_SEED", "1", 1);
  return failed;
}

static int noninteractive(void) {
  struct editor e;
  int failed = 0;
  REQUIRE(plain_input(&e, "ordinary\n/quit\n", "plain") == 0);
  REQUIRE(contains(&e, "> ordinary", 0));
  REQUIRE(!contains(&e, "Approval demo:", 0));
  REQUIRE(!contains(&e, "Demo queued request", 0));
cleanup:
  close_editor(&e);
  return failed;
}

int main(int argc, char **argv) {
  char directory[4096], path[4096];
  DIR *dir;
  struct dirent *entry;
  int failed;
  if (argc != 5 && argc != 6)
    return 2;
  executable = argv[1];
  if (argc == 6)
    remote_owner = argv[5];
  root = argv[2];
  c_chat = 1;
  test_rows = 24;
  test_columns = 120;
  if (snprintf(directory, sizeof(directory), "%s/chat-approval.XXXXXX",
               argv[3]) >= (int)sizeof(directory) ||
      !mkdtemp(directory))
    return 1;
  if (snprintf(history_path, sizeof(history_path), "%s/%s", directory,
               argv[4]) >= (int)sizeof(history_path)) {
    rmdir(directory);
    return 1;
  }
  setenv("SOFTLINE_HISTORY_DIR", directory, 1);
  setenv("SOFTLINE_PROMPT_THEME", "plain", 1);
  setenv("SOFTLINE_CHAT_CHAR_MS", "2", 1);
  setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "1", 1);
  setenv("SOFTLINE_CHAT_APPROVAL_MS", "300", 1);
  setenv("SOFTLINE_CHAT_APPROVAL_SEED", "1", 1);
  failed = exercise() || automatic() || invalid_seeds() || noninteractive() ||
           commands();
  dir = opendir(directory);
  if (dir) {
    while ((entry = readdir(dir)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;
      if (snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name) <
          (int)sizeof(path))
        unlink(path);
    }
    closedir(dir);
  }
  rmdir(directory);
  return failed;
}
