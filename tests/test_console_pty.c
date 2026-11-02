#include "console_pty.h"
static int simple(const char *script, const char *marker) {
  struct editor e;
  int failed = 0;
  REQUIRE(start(&e, script, NULL, NULL) == 0);
  REQUIRE(wait_for(&e, marker, 0) == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int approval(void) {
  struct editor e;
  int failed = 0;
  REQUIRE(start(&e, "tests/lua-fixtures/queue-approval.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "Alt-A focus", 0) == 0);
  REQUIRE(send_bytes(&e, "\033e") == 0);
  REQUIRE(wait_for(&e, "H editing: approval required", 0) == 0);
  REQUIRE(send_bytes(&e, "-edited\t") == 0);
  REQUIRE(wait_for(&e, "lua queue approval passed", 0) == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int policy(void) {
  struct editor e;
  size_t i;
  const char *bursts[] = {"/command\r\033eX\r",
                          "/hold review\r\033s",
                          "/status\r",
                          "\033\r\033s",
                          "\033",
                          "/status\r",
                          "/goal resume\r",
                          "/bad\r\033s",
                          "\r",
                          "/coroutine\r",
                          "ordinary\r",
                          "/exception\r\033s",
                          "\r",
                          "/metadata\r",
                          "/hold callback",
                          "/command callback",
                          "\022",
                          "\022",
                          "/hold callback",
                          "/command callback",
                          "\022",
                          "\022",
                          "/hold requeue\r\033s",
                          "\033e\033r\t",
                          "/hold requeue\r\033s",
                          "\033e\033r",
                          "\033eX\r\033s",
                          "",
                          "\033e\001\013\r\037\033s",
                          "\033e\033r\037\033s",
                          "\033e\001\013\r\037\033s",
                          "\033e\033r\037\033s",
                          "\033e\001\013\r\037\033s",
                          "\033e\033r\037\033s",
                          "\033e\001\013\r\037\033s",
                          "\033e\033r\037\033s",
                          "\033e\001\013\033",
                          "\033e\033n\037\033s",
                          "\033e\033n\037\033s",
                          "\033e\033N\037\033s",
                          "\033e\033N\037\033s",
                          "\033kprefix \001\033j\033s",
                          "\033n\037\033s",
                          "\033n\033j\033s",
                          "\033k\033j\033n\033s"};
  int failed = 0;
  REQUIRE(start(&e, "tests/lua-fixtures/queue-policy.lua", NULL, NULL) == 0);
  for (i = 0; i < sizeof(bursts) / sizeof(bursts[0]); i++) {
    char prompt[32];
    snprintf(prompt, sizeof(prompt), "policy-%lu> ", (unsigned long)i + 1);
    REQUIRE(wait_for(&e, prompt, 0) == 0);
    REQUIRE(send_bytes(&e, bursts[i]) == 0);
  }
  REQUIRE(wait_for(&e, "lua queue policy passed", 0) == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int history(void) {
  struct editor e;
  const char *methods[] = {"readline", "next_prompt"};
  const char *outcomes[] = {"status",      "false", "exception", "cancel",
                            "cancel_hook", "eof",   "key_error"};
  size_t m, o;
  int failed = 0;
  REQUIRE(start(&e, "tests/lua-fixtures/history-editor.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", 0) == 0);
  REQUIRE(send_bytes(&e, "queued\033r\014\t") == 0);
  REQUIRE(wait_for(&e, "QUEUE-ROLLBACK-PASS", 0) == 0);
  REQUIRE(send_bytes(&e, "\t") == 0);
  REQUIRE(wait_for(&e, "QUEUE-STATUS-PASS", 0) == 0);
  REQUIRE(send_bytes(&e, "\t") == 0);
  REQUIRE(wait_for(&e, "QUEUE-ACCEPT-PASS", 0) == 0);
  REQUIRE(send_bytes(&e, "direct\r") == 0);
  REQUIRE(wait_for(&e, "COROUTINE-EDITOR-PASS", 0) == 0);
  REQUIRE(finish(&e) == 0);
  close_editor(&e);
  REQUIRE(start(&e, "tests/lua-fixtures/history-search.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", 0) == 0);
  REQUIRE(send_bytes(&e, "draft\022match\r\025draft\022missing\r\022match\007"
                         "\022match\007!\r") == 0);
  REQUIRE(wait_for(&e, "SEARCH-QUERY-PASS", 0) == 0);
  REQUIRE(finish(&e) == 0);
  close_editor(&e);
  for (m = 0; m < 2; ++m)
    for (o = 0; o < 7; ++o) {
      const char *keys = "\025direct\r";
      if (o == 3)
        keys = "\033";
      if (o == 4 || o == 6)
        keys = "\007";
      if (o == 5)
        keys = "\025\004";
      REQUIRE(start(&e, "tests/lua-fixtures/history-diagnostic.lua", methods[m],
                    outcomes[o]) == 0);
      REQUIRE(wait_for(&e, "\033[?2004h", 0) == 0);
      REQUIRE(send_bytes(&e, "queued\t") == 0);
      REQUIRE(wait_for(&e, "QUEUE-ERROR-HANDLED", 0) == 0);
      REQUIRE(send_bytes(&e, keys) == 0);
      REQUIRE(wait_for(&e, "EDITOR-DIAGNOSTIC-PASS", 0) == 0);
      REQUIRE(finish(&e) == 0);
      close_editor(&e);
    }
cleanup:
  close_editor(&e);
  return failed;
}
static int live(void) {
  struct editor e;
  int failed = 0, clear;
  for (clear = 0; clear < 2; ++clear) {
    REQUIRE(start(&e, "tests/lua_live_output.lua", clear ? "clear" : "keep",
                  NULL) == 0);
    REQUIRE(wait_for(&e, "first", 0) == 0);
    REQUIRE(!contains(&e, "second", 0));
    REQUIRE(send_bytes(&e, "draft") == 0);
    REQUIRE(wait_for(&e, "second", 0) == 0);
    REQUIRE(send_bytes(&e, "\r") == 0);
    REQUIRE(wait_for(&e, "RESULT:draft", 0) == 0);
    REQUIRE(finish(&e) == 0);
    close_editor(&e);
  }
cleanup:
  close_editor(&e);
  return failed;
}
static int cancel(void) {
  struct editor e;
  int failed = 0;
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "> ", 0) == 0);
  REQUIRE(contains(&e, "demo/project", 0));
  REQUIRE(!contains(&e, "\033[?1049h", 0));
  REQUIRE(send_bytes(&e, "\003") == 0);
  REQUIRE(wait_for(&e, "[cancelled]", 0) == 0);
  REQUIRE(send_bytes(&e, "/quit\r") == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}
static int watch(void) {
  struct editor e;
  int failed = 0, scenario;
  size_t offset;
  if (setenv("SOFTLINE_CHAT_OPERATION_STEP_MS", "150", 1))
    return 1;
  for (scenario = 0; scenario < 5; ++scenario) {
    REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
    REQUIRE(wait_for(&e, "> ", 0) == 0);
    offset = e.length;
    REQUIRE(send_bytes(&e, "start\r") == 0);
    REQUIRE(wait_for(&e, "[turn] start", offset) == 0);
    REQUIRE(wait_for(&e, "\033[?2004h", offset) == 0);
    if (scenario == 0) {
      REQUIRE(send_bytes(&e, "queued\r") == 0);
      REQUIRE(wait_for(&e, "Q 1.", offset) == 0);
      offset = e.length;
      REQUIRE(send_bytes(&e, "\003") == 0);
      REQUIRE(wait_for(&e, "[operation] cancelled", offset) == 0);
      REQUIRE(wait_for(&e, "\033[?2004h", offset) == 0);
      REQUIRE(!contains(&e, "[queued] queued", offset));
      offset = e.length;
      REQUIRE(send_bytes(&e, "\033\r") == 0);
      REQUIRE(wait_for(&e, "[promoted] queued", offset) == 0);
      REQUIRE(wait_for(&e, "[operation] processing input.", offset) == 0);
      offset = e.length;
      REQUIRE(send_bytes(&e, "steer\033\r") == 0);
      REQUIRE(wait_for(&e, "S 1. ", offset) == 0);
      REQUIRE(wait_for(&e, "[active operation] consumed: steer", offset) == 0);
      offset = e.length;
      REQUIRE(send_bytes(&e, "\033") == 0);
      REQUIRE(wait_for(&e, "[operation] cancelled", offset) == 0);
      REQUIRE(wait_for(&e, "\033[?2004h", offset) == 0);
      offset = e.length;
      REQUIRE(send_bytes(&e, "\033[A") == 0);
      REQUIRE(wait_for(&e, "steer", offset) == 0);
      REQUIRE(!contains(&e, "start", offset));
      REQUIRE(send_bytes(&e, "\025/quit\r") == 0);
    } else if (scenario == 1) {
      REQUIRE(send_bytes(&e, "followup\r/quit\r") == 0);
      REQUIRE(wait_for(&e, "Q 2. ", offset) == 0);
      REQUIRE(wait_for(&e, "[queued] followup", offset) == 0);
    } else if (scenario == 2) {
      REQUIRE(send_bytes(&e, "/quit\033\r") == 0);
      REQUIRE(wait_for(&e, "S 1. ", offset) == 0);
    } else if (scenario == 3) {
      REQUIRE(send_bytes(&e, "/quit\r") == 0);
      REQUIRE(wait_for(&e, "Q 1. ", offset) == 0);
      offset = e.length;
      REQUIRE(send_bytes(&e, "\003") == 0);
      REQUIRE(wait_for(&e, "[operation] cancelled", offset) == 0);
      REQUIRE(wait_for(&e, "\033[?2004h", offset) == 0);
      REQUIRE(send_bytes(&e, "\033\r") == 0);
    } else {
      REQUIRE(send_bytes(&e, "followup\r") == 0);
      REQUIRE(wait_for(&e, "Q 1.", offset) == 0);
      offset = e.length;
      REQUIRE(send_bytes(&e, "\004") == 0);
    }
    REQUIRE(finish(&e) == 0);
    if (scenario == 4)
      REQUIRE(!contains(&e, "[queued] followup", offset));
    close_editor(&e);
  }
cleanup:
  close_editor(&e);
  return failed;
}
static int mixed_tty(void) {
  struct editor e;
  int slave, output[2], input = -1, failed = 0;
  struct termios attributes;
  char script[4096];
  memset(&e, 0, sizeof(e));
  e.child = -1;
  e.master = -1;
  REQUIRE(openpty(&e.master, &slave, NULL, NULL, NULL) == 0);
  REQUIRE(tcgetattr(slave, &attributes) == 0);
  attributes.c_lflag &= (tcflag_t)~ECHO;
  REQUIRE(tcsetattr(slave, TCSANOW, &attributes) == 0);
  REQUIRE(pipe(output) == 0);
  snprintf(script, sizeof(script), "%s/examples/chat.lua", root);
  e.child = fork();
  if (e.child == 0) {
    close(e.master);
    close(output[0]);
    setsid();
    dup2(slave, 0);
    dup2(output[1], 1);
    dup2(output[1], 2);
    close(slave);
    close(output[1]);
    chdir(root);
    execl(executable, executable, script, (char *)NULL);
    _exit(127);
  }
  close(slave);
  close(output[1]);
  REQUIRE(e.child > 0);
  REQUIRE(send_bytes(&e, "hello\r/quit\r") == 0);
  input = e.master;
  e.master = output[0];
  REQUIRE(finish(&e) == 0);
  REQUIRE(strcmp(e.bytes, "[turn] hello\n") == 0);
cleanup:
  if (input >= 0)
    close(input);
  close_editor(&e);
  return failed;
}
static int stored_history(const char *build, const char *filename) {
  struct editor e;
  int failed = 0;
  size_t offset;
  char directory[4096], path[4096], lock_path[4096];
  const char *three =
      "history-first\nqueued-before-dispatch\nsteer-before-dispatch\n";
  const char *four = "history-first\nqueued-before-dispatch\nsteer-before-"
                     "dispatch\nhistory-first-edited\n";
  memset(&e, 0, sizeof(e));
  e.child = e.master = -1;
  if (snprintf(directory, sizeof(directory), "%s/chat-history.XXXXXX", build) >=
          (int)sizeof(directory) ||
      strlen(directory) + strlen(filename) + 7 >= sizeof(path))
    return 1;
  if (!mkdtemp(directory))
    return 1;
  strcpy(path, directory);
  strcat(path, "/");
  strcat(path, filename);
  strcpy(lock_path, path);
  strcat(lock_path, ".lock");
  REQUIRE(setenv("SOFTLINE_HISTORY_DIR", directory, 1) == 0);
  REQUIRE(setenv("SOFTLINE_PROMPT_THEME", "plain", 1) == 0);
  REQUIRE(setenv("SOFTLINE_CHAT_CHAR_MS", "10", 1) == 0);
  REQUIRE(setenv("SOFTLINE_CHAT_OPERATION_STEP_MS", "150", 1) == 0);
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "> ", 0) == 0);
  offset = e.length;
  REQUIRE(send_bytes(&e, "history-first\r") == 0);
  REQUIRE(wait_records(&e, path, "history-first\n") == 0);
  REQUIRE(wait_for(&e, c_chat ? "Thinking..." : "\033[?2004h", offset) == 0);
  REQUIRE(send_bytes(&e, "\025queued-before-dispatch\r") == 0);
  REQUIRE(wait_records(&e, path, "history-first\nqueued-before-dispatch\n") ==
          0);
  REQUIRE(send_bytes(&e, "steer-before-dispatch\033\r") == 0);
  REQUIRE(wait_records(&e, path, three) == 0);
  offset = e.length;
  REQUIRE(wait_for(&e,
                   c_chat ? "> queued-before-dispatch"
                          : "[queued] queued-before-dispatch",
                   offset) == 0);
  REQUIRE(records(path, three));
  /* Kill the owned fixture job: exit-time saving cannot explain durability. */
  close_editor(&e);
  REQUIRE(records(path, three));
  REQUIRE(start(&e, "examples/chat.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "> ", 0) == 0);
  offset = e.length;
  REQUIRE(send_bytes(&e, "cancelled-draft\033") == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", offset) == 0);
  REQUIRE(records(path, three));
  REQUIRE(send_bytes(&e, "\022history-first") == 0);
  REQUIRE(wait_for(&e, "history-first", offset) == 0);
  REQUIRE(send_bytes(&e, "\r") == 0);
  pump(&e, 150);
  REQUIRE(records(path, three));
  REQUIRE(send_bytes(&e, "-edited\r") == 0);
  REQUIRE(wait_records(&e, path, four) == 0);
  close_editor(&e);
  REQUIRE(plain_input(&e, "plain-last\n/quit\n", "riced") == 0);
  REQUIRE(records(path, "history-first\nqueued-before-dispatch\nsteer-before-"
                        "dispatch\nhistory-first-edited\nplain-last\n/quit\n"));
cleanup:
  close_editor(&e);
  unlink(path);
  unlink(lock_path);
  if (rmdir(directory)) {
    fprintf(stderr, "Chat history fixture left unexpected state: %s\n",
            directory);
    failed = 1;
  }
  return failed;
}
static int readline_edit(void) {
  struct editor e;
  int failed = 0;
  size_t mark;
  int index;
  char marker[32];
  REQUIRE(start(&e, "tests/lua-fixtures/readline-edit.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "word> ", 0) == 0);
  REQUIRE(send_bytes(&e, "one two\001\033f!\r") == 0);
  REQUIRE(wait_for(&e, "kill> ", 0) == 0);
  REQUIRE(send_bytes(&e, "café—中文\001\033d\031\037\031\005\r") == 0);
  REQUIRE(wait_for(&e, "again> ", 0) == 0);
  REQUIRE(send_bytes(&e, "\031\r") == 0);
  REQUIRE(wait_for(&e, "undo> ", 0) == 0);
  REQUIRE(send_bytes(&e, "draft\030\025\r") == 0);
  REQUIRE(wait_for(&e, "transpose> ", 0) == 0);
  REQUIRE(send_bytes(&e, "aé\002\024\r") == 0);
  REQUIRE(wait_for(&e, "suspend> ", 0) == 0);
  REQUIRE(send_bytes(&e, "abcdef\002\002\002X\033s") == 0);
  REQUIRE(wait_for(&e, "resume> ", 0) == 0);
  REQUIRE(send_bytes(&e, "\037\r") == 0);
  for (index = 1; index <= 5; ++index) {
    snprintf(marker, sizeof(marker), "m%d> ", index);
    REQUIRE(wait_for(&e, marker, 0) == 0);
    REQUIRE(send_bytes(&e, "abc\033s") == 0);
    snprintf(marker, sizeof(marker), "u%d> ", index);
    REQUIRE(wait_for(&e, marker, 0) == 0);
    REQUIRE(send_bytes(&e, "\037\033s") == 0);
    snprintf(marker, sizeof(marker), "f%d> ", index);
    REQUIRE(wait_for(&e, marker, 0) == 0);
    REQUIRE(send_bytes(&e, "\037\r") == 0);
  }
  REQUIRE(wait_for(&e, "host-cursor> ", 0) == 0);
  REQUIRE(send_bytes(&e, "abc") == 0);
  REQUIRE(wait_for(&e, "CURSOR-MOVED", 0) == 0);
  REQUIRE(send_bytes(&e, "X\037\r") == 0);
  REQUIRE(wait_for(&e, "group> ", 0) == 0);
  REQUIRE(send_bytes(&e, "abc\033t") == 0);
  REQUIRE(wait_for(&e, "group-resume> ", 0) == 0);
  REQUIRE(send_bytes(&e, "X\037\r") == 0);
  REQUIRE(wait_for(&e, "prefix> ", 0) == 0);
  REQUIRE(send_bytes(&e, "\030éok\r") == 0);
  REQUIRE(wait_for(&e, "search> ", 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "draft\022match\033s") == 0);
  REQUIRE(wait_for(&e, "SEARCH-SUSPENDED", mark) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", mark) == 0);
  REQUIRE(send_bytes(&e, "\007!\r") == 0);
  REQUIRE(wait_for(&e, "draft> ", 0) == 0);
  REQUIRE(send_bytes(&e, "\t\033[B\033[A!\r") == 0);
  REQUIRE(wait_for(&e, "destroy> ", 0) == 0);
  REQUIRE(send_bytes(&e, "draft\022match\033s") == 0);
  REQUIRE(wait_for(&e, "READLINE-EDIT-PASS", 0) == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}

static int terminal(void) {
  struct editor e;
  int failed = 0;
  size_t mark;
  REQUIRE(start(&e, "tests/lua-fixtures/terminal.lua", NULL, NULL) == 0);
  REQUIRE(wait_for(&e, "terminal> ", 0) == 0);
  REQUIRE(send_bytes(&e, "\033e\033s") == 0);
  REQUIRE(wait_for(&e, "TERMINAL-READY", 0) == 0);
  mark = e.length;
  REQUIRE(send_bytes(&e, "~.") == 0);
  REQUIRE(wait_for(&e, "TERMINAL-RESUMED", mark) == 0);
  REQUIRE(wait_for(&e, "\033[?2004h", mark) == 0);
  REQUIRE(send_bytes(&e, "\033s") == 0);
  REQUIRE(wait_for(&e, "lua terminal relay passed", mark) == 0);
  REQUIRE(finish(&e) == 0);
cleanup:
  close_editor(&e);
  return failed;
}

int main(int argc, char **argv) {
  if (argc == 7 && !strcmp(argv[1], "store")) {
    if (strcmp(argv[2], "c") && strcmp(argv[2], "lua"))
      return 2;
    c_chat = !strcmp(argv[2], "c");
    executable = argv[3];
    root = argv[4];
    return stored_history(argv[5], argv[6]);
  }
  if (argc != 4)
    return 2;
  executable = argv[2];
  root = argv[3];
  if (!strcmp(argv[1], "terminal"))
    return terminal();
  if (!strcmp(argv[1], "readline_edit"))
    return readline_edit();
  if (!strcmp(argv[1], "idle"))
    return simple("tests/lua-fixtures/idle.lua",
                  "lua idle callback smoke passed");
  if (!strcmp(argv[1], "approval"))
    return approval();
  if (!strcmp(argv[1], "policy"))
    return policy();
  if (!strcmp(argv[1], "watch_gc"))
    return simple("tests/lua_watch_file_gc.lua",
                  "lua watched file retention passed");
  if (!strcmp(argv[1], "history"))
    return history();
  if (!strcmp(argv[1], "live"))
    return live();
  if (!strcmp(argv[1], "cancel"))
    return cancel();
  if (!strcmp(argv[1], "watch"))
    return watch();
  if (!strcmp(argv[1], "mixed_tty"))
    return mixed_tty();
  return 2;
}
