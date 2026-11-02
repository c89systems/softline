#define _DEFAULT_SOURCE
#include <pty.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static const sl_completion_t candidates[] = {
    {"/status", SL_COMPLETION_START},
    {"/stop", SL_COMPLETION_START},
    {"/approve on", SL_COMPLETION_START},
    {"/approve off", SL_COMPLETION_START},
    {"/resume", SL_COMPLETION_START},
    {"/shell", SL_COMPLETION_START},
    {"/shellshare", SL_COMPLETION_START},
    {"/command1", SL_COMPLETION_START},
    {"/command2", SL_COMPLETION_START},
    {"/cmd", SL_COMPLETION_START},
    {"verification", SL_COMPLETION_WORD},
    {"next steps", SL_COMPLETION_WORD},
    {"next", SL_COMPLETION_WORD},
    {"hello 👩", SL_COMPLETION_WORD},
    {"ready ✈", SL_COMPLETION_WORD},
    {"trailing ", SL_COMPLETION_START},
    {"tabbed\t", SL_COMPLETION_START},
    {"line\n", SL_COMPLETION_START},
    {"caféclair", SL_COMPLETION_WORD},
    {"cafêtre", SL_COMPLETION_WORD},
    {"e\314\201clair", SL_COMPLETION_WORD},
    {"e\314\200tre", SL_COMPLETION_WORD}};

struct driver {
  int master, calls;
  const char *initial, *keys;
  size_t cursor;
};
enum scenario {
  NORMAL,
  HELD_EDIT,
  QUEUE_TAB,
  COPIED_LIST,
  CLEARED_LIST,
  CLUSTER_FIRST,
  CLUSTER_LAST
};
static int host_tab(sl_t *sl, sl_key_t key, void *data,
                    sl_key_action_t *action) {
  struct driver *d = data;
  (void)key;
  ++d->calls;
  *action = SL_KEY_ACTION_HANDLED;
  return sl_insert(sl, "#");
}
static int suspend_key(sl_t *sl, sl_key_t key, void *data,
                       sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)data;
  *action = SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}
static int drive(sl_t *sl, const sl_watch_event_t *event, void *data) {
  struct driver *d = data;
  char byte;
  if (read(event->fd, &byte, 1) != 1 ||
      sl_set_buffer(sl, d->initial) != SL_OK ||
      sl_set_cursor(sl, d->cursor) != SL_OK ||
      write(d->master, d->keys, strlen(d->keys)) != (ssize_t)strlen(d->keys))
    return SL_ERROR_IO;
  return SL_OK;
}

static int run(int native, const char *initial, size_t cursor, const char *keys,
               const char *expected, int calls, size_t limit, int scenario) {
  int master, slave, wake[2];
  struct winsize size = {24, 80, 0, 0};
  struct driver d;
  sl_config_t config;
  sl_t *sl;
  sl_watch_id_t watch;
  char *line;
  sl_prompt_id_t id = 0;
  sl_prompt_queue_entry_info_t info;
  sl_completion_t invalid = {"bad\033[31m", SL_COMPLETION_START};
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(wake));
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  config.line_max_len = limit;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  if (limit > 20) {
    CHECK(sl->set_completions(sl, candidates,
                              sizeof(candidates) / sizeof(candidates[0])) ==
          SL_OK);
    CHECK(sl_set_completions(sl, &invalid, 1) == SL_ERROR_INVALID);
    invalid.text = "\300\257";
    CHECK(sl_set_completions(sl, &invalid, 1) == SL_ERROR_INVALID);
    invalid.text = "fine";
    invalid.scope = (sl_completion_scope_t)9;
    CHECK(sl_set_completions(sl, &invalid, 1) == SL_ERROR_INVALID);
    CHECK(sl_set_completions(sl, NULL, 1) == SL_ERROR_INVALID);
    CHECK(sl_set_completions(sl, candidates, (size_t)-1) == SL_ERROR_INVALID);
  } else {
    sl_completion_t short_one = {"verification", SL_COMPLETION_WORD};
    CHECK(sl_set_completions(sl, &short_one, 1) == SL_OK);
  }
  memset(&d, 0, sizeof(d));
  d.master = master;
  d.initial = initial;
  d.cursor = cursor;
  d.keys = keys;
  if (scenario != QUEUE_TAB)
    CHECK(sl_bind_key(sl, SL_KEY_TAB, host_tab, &d) == SL_OK);
  if (scenario == COPIED_LIST) {
    char text[] = "verification";
    sl_completion_t copied[2];
    copied[0].text = copied[1].text = text;
    copied[0].scope = SL_COMPLETION_START;
    copied[1].scope = SL_COMPLETION_WORD;
    CHECK(sl_set_completions(sl, copied, 2) == SL_OK);
    memset(text, 'X', sizeof(text) - 1);
    copied[1].scope = SL_COMPLETION_START;
  }
  if (scenario == CLEARED_LIST)
    CHECK(sl_set_completions(sl, NULL, 0) == SL_OK);
  if (scenario == CLUSTER_FIRST || scenario == CLUSTER_LAST) {
    sl_completion_t cluster[2] = {{"cafet", SL_COMPLETION_START},
                                  {"cafe\314\201x", SL_COMPLETION_START}};
    if (scenario == CLUSTER_FIRST) {
      sl_completion_t swap = cluster[0];
      cluster[0] = cluster[1];
      cluster[1] = swap;
    }
    CHECK(sl_set_completions(sl, cluster, 2) == SL_OK);
  }
  CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  if (scenario == HELD_EDIT)
    CHECK(sl_prompt_queue_append(sl, "ver", 1, &id) == SL_OK);
  CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, drive, &d, &watch) == SL_OK);
  CHECK(write(wake[1], "x", 1) == 1);
  if (native)
    CHECK(sl_output_stream_begin(sl) == SL_OK);
  line = native ? sl_next_prompt(sl, "> ", NULL) : sl_readline(sl, "> ");
  if (scenario == HELD_EDIT) {
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == id &&
          info.held);
    CHECK(sl_prompt_queue_peek(sl, 0, &line) == SL_OK);
  }
  if (!line || strcmp(line, expected) || d.calls != calls) {
    fprintf(stderr, "native=%d initial=%s got=%s expected=%s calls=%d/%d\n",
            native, initial, line ? line : "NULL", expected, d.calls, calls);
    return 1;
  }
  sl_free_string(sl, line);
  if (scenario == QUEUE_TAB) {
    CHECK(sl_prompt_queue_count(sl) == (*expected ? 0u : 1u));
    if (!*expected) {
      CHECK(sl_prompt_queue_peek(sl, 0, &line) == SL_OK);
      CHECK(!strcmp(line, "verification"));
      sl_free_string(sl, line);
    }
  }
  sl_destroy(sl);
  close(wake[0]);
  close(wake[1]);
  close(master);
  close(slave);
  return 0;
}

int main(void) {
  static const struct {
    const char *initial, *keys, *expected;
    size_t cursor;
    int calls;
  } cases[] = {
      {"", "/sta\t\r", "/status", 0, 0},
      {"/status", "\t\r", "/status#", 7, 1},
      {"", "\t\r", "#", 0, 1},
      {"/bad", "\t\r", "/bad#", 4, 1},
      {"say /sta", "\t\r", "say /sta#", 8, 1},
      {"say ver", "\t\r", "say verification", 7, 0},
      {"say\nver", "\t\r", "say\nverification", 7, 0},
      {"say\tver", "\t\r", "say\tverification", 7, 0},
      {"say/ver", "\t\r", "say/ver#", 7, 1},
      {"VER", "\t\r", "VER#", 3, 1},
      {"/app", "\t\r", "/approve on", 4, 0},
      {"/app", "\t\t\r", "/approve off", 4, 0},
      {"/app", "\t\t\t\r", "/approve on", 4, 0},
      {"/shell", "\t\t\r", "/shellshare", 6, 0},
      {"/", "\t\t\t\t\t\t\t\t\177\t\t\r", "/command2", 1, 0},
      {"/", "\t\t\177\t\t\r", "/stop#", 1, 1},
      {"/app", "\t\t\037\r", "/app", 4, 0},
      {"/app", "\t\tX\037\r", "/approve off", 4, 0},
      {"next steps later", "\t\t\013\r", "next", 2, 0},
      {"next steps later", "\t\t\t\013\r", "next steps", 2, 0},
      {"/", "\r", "/", 1, 0},
      {"e", "\r", "e", 1, 0},
      {"/app", "\t\002!\r", "/approve o!n", 4, 0},
      {"/resX later", "\t\r", "/resume later", 4, 0},
      {"say next steps later", "\t\r", "say next steps later", 8, 0},
      {"say neX later", "\t\r", "say next steps later", 6, 0},
      {"trailing later", "\t\r", "trailing #later", 9, 1},
      {"trailing later", "\t\t\r", "trailing #later", 4, 1},
      {"tabbed\tlater", "\t\r", "tabbed\t#later", 7, 1},
      {"line\nlater", "\t\r", "line\n#later", 5, 1},
      {"next steps\314\201 later", "\t\r", "next steps\314\201 later", 2, 0},
      {"next steps\314\201 later", "\t\002!\r", "next step!s\314\201 later", 2,
       0},
      {"next steps\314\201 later", "\t\177\r", "next step later", 2, 0},
      {"next steps\314\201 later", "\t\002!\037\006\177\r", "next step later",
       2, 0},
      {"next steps\314\201 later", "\t\025\031\r", "next steps\314\201 later",
       2, 0},
      {"next steps\314\201 later", "\t\013\r", "next steps\314\201", 2, 0},
      {"hello 👩‍💻 later", "\t\002!\r", "hello !👩‍💻 later", 2,
       0},
      {"hello 👩‍💻 later", "\t\177\r", "hello  later", 2, 0},
      {"ready ✈️ later", "\t\002!\r", "ready !✈️ later", 2, 0},
      {"caf", "\t\r", "caféclair", 3, 0},
      {"caf", "\t\t\r", "cafêtre", 3, 0},
      {"e", "\t\r", "e\314\201clair", 1, 0},
      {"/sta", "\t\037\r", "/sta", 4, 0},
      {"", "/sta\tX\037\r", "/status", 0, 0},
      {"/sta", "\033[200~\t\033[201~\r", "/sta\t", 4, 0},
      {"/sta", "\022\t\007\r", "/sta", 4, 1}};
  size_t i;
  int native;
  CHECK(sl_set_completions(NULL, candidates, 1) == SL_ERROR_INVALID);
  for (native = 0; native <= 1; ++native) {
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      CHECK(!run(native, cases[i].initial, cases[i].cursor, cases[i].keys,
                 cases[i].expected, cases[i].calls, 131072, 0));
    CHECK(!run(native, "x ver", 5, "\t\r", "x ver", 0, 12, 0));
    CHECK(!run(native, "", 0, "\033e\t\r\033s", "verification", 0, 131072, 1));
    CHECK(!run(native, "ver", 3, "\t\r", "verification", 0, 131072, QUEUE_TAB));
    CHECK(!run(native, "verification", 12, "\t\r", "", 0, 131072, QUEUE_TAB));
    CHECK(!run(native, "x ver", 5, "\t\r", "x verification", 0, 131072,
               COPIED_LIST));
    CHECK(!run(native, "/sta", 4, "\t\r", "/sta#", 1, 131072, CLEARED_LIST));
    CHECK(!run(native, "ca", 2, "\t\r", "cafe\314\201x", 0, 131072,
               CLUSTER_FIRST));
    CHECK(!run(native, "ca", 2, "\t\r", "cafet", 0, 131072, CLUSTER_LAST));
    {
      char *initial = malloc(50005), *expected = malloc(50014);
      CHECK(initial && expected);
      memset(initial, 'x', 50000);
      memcpy(initial + 50000, "\nver", 5);
      memcpy(expected, initial, 50001);
      memcpy(expected + 50001, "verification", 13);
      CHECK(!run(native, initial, 50004, "\t\r", expected, 0, 131072, NORMAL));
      free(initial);
      free(expected);
    }
  }
  return 0;
}
