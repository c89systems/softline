#define _DEFAULT_SOURCE
#include "completion_fixture.h"
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

static const struct completion_item candidates[] = {
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
  int master, calls, phase;
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
  CLUSTER_LAST,
  HOST_ENQUEUE,
  PHRASE_WHITESPACE,
  PHRASE_TRIGGER,
  RIGHT_OVERRIDE,
  CTRL_F_OVERRIDE
};
static void enqueue_completed(sl_t *sl, void *data) {
  struct driver *d = data;
  if (!d->phase && !strcmp(sl_buffer(sl), "/status")) {
    d->phase = sl_prompt_queue_enqueue_draft(sl) == SL_OK ? 1 : -1;
    if (write(d->master, "\t\r", 2) != 2)
      d->phase = -1;
  }
}
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
static int insert_space(sl_t *sl, sl_key_t key, void *data,
                        sl_key_action_t *action) {
  (void)key;
  (void)data;
  *action = SL_KEY_ACTION_HANDLED;
  return sl_insert(sl, " ");
}
static int drive(sl_t *sl, const sl_watch_event_t *event, void *data) {
  struct driver *d = data;
  char byte;
  if (read(event->fd, &byte, 1) != 1 ||
      (d->initial && (sl_set_buffer(sl, d->initial) != SL_OK ||
                      sl_set_cursor(sl, d->cursor) != SL_OK)) ||
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
  struct completion_fixture fixture = {
      candidates, sizeof(candidates) / sizeof(candidates[0]), 0};
  struct completion_item cluster[2] = {{"cafet", SL_COMPLETION_START},
                                       {"cafe\314\201x", SL_COMPLETION_START}};
  static const struct completion_item copied[] = {
      {"verification", SL_COMPLETION_WORD},
      {"verification", SL_COMPLETION_WORD}};
  static const struct completion_item short_one[] = {
      {"verification", SL_COMPLETION_WORD}};
  sl_completion_provider_t invalid = {'/', SL_COMPLETION_START, 0, NULL, NULL,
                                      NULL};
  static const struct completion_item phrases[] = {
      {"hello world", SL_COMPLETION_WORD},
      {"hello there", SL_COMPLETION_WORD},
      {"hello  world", SL_COMPLETION_WORD},
      {"hello\tworld", SL_COMPLETION_WORD},
      {"hello\nworld", SL_COMPLETION_WORD},
      {"end ", SL_COMPLETION_WORD},
      {"@hello world", SL_COMPLETION_WORD},
      {"@hello there", SL_COMPLETION_WORD},
      {"@end ", SL_COMPLETION_WORD},
      {"worldly", SL_COMPLETION_WORD}};
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(wake));
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  config.line_max_len = limit;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  if (limit > 20) {
    CHECK(fixture_setup(sl, &fixture) == SL_OK);
    CHECK(sl_set_completion_providers(sl, &invalid, 1) == SL_ERROR_INVALID);
    invalid.callback = fixture_complete;
    invalid.scope = (sl_completion_scope_t)9;
    CHECK(sl_set_completion_providers(sl, &invalid, 1) == SL_ERROR_INVALID);
    CHECK(sl_set_completion_providers(sl, NULL, 1) == SL_ERROR_INVALID);
    CHECK(sl_set_completion_providers(sl, &invalid, (size_t)-1) ==
          SL_ERROR_INVALID);
  } else {
    fixture.items = short_one;
    fixture.count = 1;
    CHECK(fixture_setup(sl, &fixture) == SL_OK);
  }
  memset(&d, 0, sizeof(d));
  d.master = master;
  d.initial = initial;
  d.cursor = cursor;
  d.keys = keys;
  if (scenario == HOST_ENQUEUE)
    CHECK(sl_set_idle_callback(sl, enqueue_completed, &d) == SL_OK);
  if (scenario != QUEUE_TAB)
    CHECK(sl_bind_key(sl, SL_KEY_TAB, host_tab, &d) == SL_OK);
  if (scenario == RIGHT_OVERRIDE || scenario == CTRL_F_OVERRIDE)
    CHECK(sl_bind_key(sl,
                      scenario == RIGHT_OVERRIDE ? SL_KEY_RIGHT : SL_KEY_CTRL_F,
                      host_tab, &d) == SL_OK);
  if (scenario == COPIED_LIST) {
    fixture.items = copied;
    fixture.count = 2;
    CHECK(fixture_setup(sl, &fixture) == SL_OK);
  }
  if (scenario == CLEARED_LIST)
    CHECK(sl_set_completion_providers(sl, NULL, 0) == SL_OK);
  if (scenario == PHRASE_WHITESPACE || scenario == PHRASE_TRIGGER) {
    sl_completion_provider_t providers[] = {
        {'h', SL_COMPLETION_WORD, 0, fixture_complete, NULL, NULL},
        {'@', SL_COMPLETION_WORD, 1, fixture_complete, NULL, NULL},
        {'e', SL_COMPLETION_WORD, 0, fixture_complete, NULL, NULL},
        {'w', SL_COMPLETION_WORD, 0, fixture_complete, NULL, NULL}};
    providers[0].userdata = providers[1].userdata = providers[2].userdata =
        providers[3].userdata = &fixture;
    fixture.items = phrases;
    fixture.count = sizeof(phrases) / sizeof(phrases[0]);
    fixture.calls = 0;
    CHECK(sl_set_completion_providers(sl, providers, 4) == SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_CTRL_T, insert_space, NULL) == SL_OK);
  }
  if (scenario == CLUSTER_FIRST || scenario == CLUSTER_LAST) {
    if (scenario == CLUSTER_FIRST) {
      struct completion_item swap = cluster[0];
      cluster[0] = cluster[1];
      cluster[1] = swap;
    }
    fixture.items = cluster;
    fixture.count = 2;
    CHECK(fixture_setup(sl, &fixture) == SL_OK);
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
  if (scenario == PHRASE_WHITESPACE)
    CHECK(fixture.calls == 1);
  if (scenario == PHRASE_TRIGGER)
    CHECK(fixture.calls == 2);
  if (scenario == HOST_ENQUEUE) {
    CHECK(d.phase == 1 && sl_prompt_queue_count(sl) == 1);
    CHECK(sl_prompt_queue_peek(sl, 0, &line) == SL_OK);
    CHECK(!strcmp(line, "/status"));
    sl_free_string(sl, line);
  }
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

struct provider_data {
  struct driver driver;
  int calls, generation, kind;
};
static int dynamic_provider(sl_t *sl, char trigger, sl_completion_emit_t emit,
                            void *context, void *userdata) {
  struct provider_data *data = userdata;
  char value[64];
  int status;
  (void)sl;
  ++data->calls;
  if (data->kind == 1)
    return SL_OK;
  if (data->kind == 2)
    return SL_ERROR_IO;
  if (data->kind == 3) {
    (void)emit("bad\033[31m", context);
    return SL_OK;
  }
  if (data->kind == 5) {
    (void)emit("@\300\257", context);
    return SL_OK;
  }
  if (data->kind >= 16 && !data->generation) {
    snprintf(value, sizeof(value), "%cnext stop", trigger);
    if (emit(value, context) != SL_OK)
      return SL_ERROR;
    snprintf(value, sizeof(value), "%cnext steps", trigger);
    if (emit(value, context) != SL_OK)
      return SL_ERROR;
    snprintf(value, sizeof(value), "%cnext", trigger);
    return emit(value, context);
  }
  snprintf(value, sizeof(value), "%c%s", trigger,
           trigger == '@'     ? "alpha.txt"
           : data->generation ? "fresh1"
                              : "command1");
  status = emit(value, context);
  memset(value, 'X', sizeof(value));
  if (status != SL_OK)
    return status;
  if (data->kind == 9) {
    (void)emit("@bad\033[31m", context);
    return SL_OK;
  }
  if (data->kind == 4)
    return emit("/command1", context);
  snprintf(value, sizeof(value), "%c%s", trigger,
           trigger == '@'     ? "beta.txt"
           : data->generation ? "fresh2"
                              : "command2");
  return emit(value, context);
}
static int update_provider(sl_t *sl, sl_key_t key, void *userdata,
                           sl_key_action_t *action) {
  struct provider_data *data = userdata;
  (void)sl;
  (void)key;
  if (data->kind == 6 && strcmp(sl_buffer(sl), "@al")) {
    fprintf(stderr, "strip undo restored <%s>\n", sl_buffer(sl));
    return SL_ERROR;
  }
  if (data->kind == 8 && sl_prompt_queue_enqueue_draft(sl) != SL_OK)
    return SL_ERROR;
  if (data->kind == 10 &&
      (sl_set_cursor(sl, 0) != SL_OK || sl_insert(sl, "X") != SL_OK))
    return SL_ERROR;
  if (data->kind == 11 && sl_insert(sl, " ") != SL_OK)
    return SL_ERROR;
  if (data->kind == 11 && strcmp(sl_buffer(sl), "a "))
    return SL_ERROR;
  if (data->kind == 15 && sl_set_buffer(sl, "/") != SL_OK)
    return SL_ERROR;
  ++data->generation;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}
static int provider_cases(int native) {
  static const struct {
    char trigger;
    int strip, kind, calls, host_calls, word;
    const char *keys, *expected;
  } cases[] = {
      {'/', 0, 0, 1, 0, 0, "/\024\t\t\r", "/command2"},
      {'/', 0, 0, 2, 0, 0, "/\024\025/\t\r", "/fresh1"},
      {'/', 0, 0, 1, 0, 0, "/command1\177\024\t\t\r", "/command2"},
      {'/', 0, 0, 0, 1, 0, "say /\t\r", "say /#"},
      {'/', 0, 0, 1, 0, 1, "say\n/\t\r", "say\n/command1"},
      {'/', 0, 4, 1, 1, 0, "/\t\t\r", "/command1#"},
      {'@', 1, 0, 1, 0, 1, "@al\r", "al"},
      {'@', 1, 0, 1, 0, 1, "@a\t\r", "alpha.txt"},
      {'@', 1, 0, 2, 0, 1, "say @a\t @b\t\r", "say alpha.txt beta.txt"},
      {'@', 1, 0, 1, 0, 1, "@al \r", "al "},
      {'@', 1, 0, 1, 0, 1, "@al\001X\r", "Xal"},
      {'@', 1, 0, 1, 0, 1, "@al\033e\r", "al"},
      {'@', 1, 0, 2, 0, 1, "@\177@b\t\r", "beta.txt"},
      {'@', 0, 0, 1, 0, 1, "@a\t\r", "@alpha.txt"},
      {'@', 1, 1, 1, 1, 1, "@abc\177d\024\t\r", "abd#"},
      {'@', 1, 2, 1, 1, 1, "@abc\t\r", "@abc#"},
      {'@', 1, 3, 1, 1, 1, "@abc\t\r", "@abc#"},
      {'@', 1, 5, 1, 1, 1, "@abc\t\r", "@abc#"},
      {'@', 1, 6, 2, 0, 1, "@al \037\024\r", "al"},
      {'@', 1, 7, 1, 0, 1, "\033e\t\r\033s", "alpha.txt"},
      {'@', 1, 8, 1, 1, 1, "@a\t\024\t\r", "#"},
      {'@', 1, 9, 1, 1, 1, "@a\t\r", "@a#"},
      {'@', 1, 10, 1, 0, 1, "@al\024\r", "Xal"},
      {'@', 1, 11, 2, 0, 1, "@a\024@b\t\r", "a beta.txt"},
      {'/', 0, 12, 1, 0, 1, "/c\177\024\037\t\r", "/command1"},
      {'@', 1, 13, 2, 0, 1, " \001\013@a\031@b\t\r", "a beta.txt"},
      {'/', 0, 14, 1, 0, 1, "/\t\024\037\t\r", "/command1"},
      {'/', 0, 15, 2, 0, 0, "/\024\t\r", "/fresh1"},
      {'/', 0, 16, 1, 0, 0, "\t\t\t\024\037\t\r", "/next stop steps later"},
      {'@', 1, 17, 1, 0, 1, "\t\t\t\024\037\t\r", "next stop steps later"},
      {'/', 0, 18, 1, 1, 0, "\t\t\t\024\037\005\t\r", "/next steps later#"}};
  size_t i;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    int master, slave, wake[2];
    struct winsize size = {24, 80, 0, 0};
    struct provider_data data;
    sl_config_t config;
    sl_completion_provider_t provider;
    sl_watch_id_t watch;
    sl_prompt_id_t id = 0;
    sl_prompt_queue_entry_info_t info;
    sl_t *sl;
    char *line;
    CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(wake));
    memset(&data, 0, sizeof(data));
    data.kind = cases[i].kind;
    data.driver.master = master;
    data.driver.initial = "";
    if (data.kind >= 16) {
      data.driver.initial =
          cases[i].trigger == '@' ? "@next steps later" : "/next steps later";
      data.driver.cursor = 3;
    }
    data.driver.keys = cases[i].keys;
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    config.prompt_queue = 1;
    sl = sl_create_with_config(&config);
    CHECK(sl);
    CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    if (data.kind == 7)
      CHECK(sl_prompt_queue_append(sl, "@a", 1, &id) == SL_OK);
    provider.trigger = cases[i].trigger;
    provider.scope = cases[i].word ? SL_COMPLETION_WORD : SL_COMPLETION_START;
    provider.strip_trigger = cases[i].strip;
    provider.callback = dynamic_provider;
    provider.userdata = &data;
    provider.expand = NULL;
    CHECK(sl->set_completion_providers(sl, &provider, 1) == SL_OK);
    memset(&provider, 0, sizeof(provider));
    CHECK(sl_bind_key(sl, SL_KEY_TAB, host_tab, &data.driver) == SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_CTRL_T, update_provider, &data) == SL_OK);
    CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, drive, &data.driver,
                       &watch) == SL_OK);
    CHECK(write(wake[1], "x", 1) == 1);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    line = sl_readline(sl, "> ");
    if (data.kind == 7) {
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == id &&
            info.held);
      CHECK(sl_prompt_queue_peek(sl, 0, &line) == SL_OK);
    }
    if (!line || strcmp(line, cases[i].expected) ||
        data.calls != cases[i].calls ||
        data.driver.calls != cases[i].host_calls) {
      fprintf(stderr,
              "provider case %lu native=%d got=%s expected=%s callbacks=%d/%d "
              "host=%d/%d\n",
              (unsigned long)i, native, line ? line : "NULL", cases[i].expected,
              data.calls, cases[i].calls, data.driver.calls,
              cases[i].host_calls);
      return 1;
    }
    sl_free_string(sl, line);
    if (data.kind == 8) {
      CHECK(sl_prompt_queue_count(sl) == 1 &&
            sl_prompt_queue_peek(sl, 0, &line) == SL_OK);
      CHECK(!strcmp(line, "alpha.txt"));
      sl_free_string(sl, line);
    }
    sl_destroy(sl);
    close(master);
    close(slave);
    close(wake[0]);
    close(wake[1]);
  }
  return 0;
}

struct expansion_data {
  struct driver driver;
  int roots, expansions, kind;
};
static int expansion_root(sl_t *sl, char trigger, sl_completion_emit_t emit,
                          void *context, void *userdata) {
  struct expansion_data *data = userdata;
  int status;
  (void)sl;
  ++data->roots;
  if (data->kind == 10 || data->kind == 12) {
    status = emit("@lu", context);
    return status == SL_OK ? emit("@lua/", context) : status;
  }
  status = emit(trigger == '/' ? "/approve" : "@lua/", context);
  if (status != SL_OK || data->kind == 1 || data->kind == 7 || data->kind == 9)
    return status;
  return emit(trigger == '/' ? "/about" : "@lib/", context);
}
static int expansion_more(sl_t *sl, const char *prefix,
                          sl_completion_emit_t emit, void *context,
                          void *userdata) {
  struct expansion_data *data = userdata;
  int status;
  ++data->expansions;
  CHECK(sl_set_completion_providers(sl, NULL, 0) == SL_ERROR_NOT_READY);
  if (data->kind == 10 || data->kind == 12)
    return emit("@lu-next", context);
  if (data->kind == 11) {
    if (!strcmp(prefix, "@lua/")) {
      status = emit("@lua/a", context);
      return status == SL_OK ? emit("@lua/apple", context) : status;
    }
    return emit("@lua/a-child", context);
  }
  if (data->kind == 2 || data->kind == 9)
    return SL_OK;
  if (data->kind == 3)
    return SL_ERROR_IO;
  if (data->kind == 4) {
    (void)emit("@lua/valid", context);
    (void)emit("@other/invalid", context);
    return SL_OK;
  }
  if (data->kind == 5)
    return emit(prefix, context);
  if (!strcmp(prefix, "/approve")) {
    status = emit("/approve on", context);
    return status == SL_OK ? emit("/approve off", context) : status;
  }
  if (!strcmp(prefix, "@lua/")) {
    if (data->kind != 1) {
      status = emit("@lua/source.c", context);
      if (status != SL_OK)
        return status;
    }
    return emit("@lua/sub/", context);
  }
  if (!strcmp(prefix, "@lua/sub/"))
    return emit("@lua/sub/deep.c", context);
  return SL_OK;
}
static int expansion_cases(int native) {
  static const struct {
    int kind, expansions, host_calls;
    const char *initial;
    size_t cursor;
    const char *keys, *expected;
  } cases[] = {
      {0, 0, 0, "", 0, "@\t\t\t\r", "lua/"},
      {0, 0, 0, "", 0, "@\t\t\r", "lib/"},
      {1, 0, 0, "", 0, "@l\t\t\t\r", "lua/"},
      {0, 1, 0, "", 0, "@\t\006\t\t\t\r", "lua/source.c"},
      {0, 2, 0, "", 0, "@\t\006\t\t\006\t\r", "lua/sub/deep.c"},
      {0, 1, 0, "", 0, "@l\033[C\t\r", "lua/source.c"},
      {0, 1, 0, "", 0, "@l\006\t\r", "lua/source.c"},
      {0, 1, 0, "", 0, "@l\t\006\r", "lua/"},
      {0, 1, 0, "", 0, "@lua/\006\r", "lua/"},
      {0, 1, 0, "", 0, "@l\t\006\037\r", "l"},
      {0, 1, 0, "say @l later", 6, "\t\006!\r", "say lua/! later"},
      {0, 0, 0, "", 0, "@l\t\r", "lua/"},
      {0, 1, 0, "", 0, "@l\t\006\t\r", "lua/source.c"},
      {0, 1, 0, "", 0, "@lua/\t\r", "lua/source.c"},
      {0, 1, 0, "", 0, "@lua/so\t\r", "lua/source.c"},
      {0, 2, 0, "", 0, "@lua/sub/de\t\r", "lua/sub/deep.c"},
      {0, 1, 0, "", 0, "@l\t\006\t\t\r", "lua/sub/"},
      {0, 2, 0, "", 0, "@l\t\006\t\t\033[C\t\r", "lua/sub/deep.c"},
      {1, 2, 0, "", 0, "@l\t\006\t\006\t\r", "lua/sub/deep.c"},
      {0, 1, 0, "", 0, "@lua/\t\177\177\177\177\177\177\177\177\177\177\t\r",
       "lua/"},
      {0, 1, 0, "", 0, "@lua/\t\177\177\177\177\177\177\177\177\t\r",
       "lua/source.c"},
      {0, 1, 0, "", 0, "@lua/\t\037\t\r", "lua/source.c"},
      {3, 1, 0, "", 0, "@l\t\006\177\177\177\177\t\t\r", "lib/"},
      {4, 1, 0, "", 0, "@l\t\006\177\177\177\177\t\t\r", "lib/"},
      {5, 1, 0, "", 0, "@l\t\006\177\177\177\177\t\t\r", "lib/"},
      {3, 1, 1, "", 0, "@l\t\006\t\r", "lua/#"},
      {2, 1, 1, "", 0, "@l\t\006\t\r", "lua/#"},
      {6, 0, 0, "", 0, "@l\t\t\r", "lib/"},
      {7, 0, 1, "", 0, "@l\t\t\r", "lua/#"},
      {9, 1, 1, "", 0, "@l\t\006\t\r", "lua/#"},
      {0, 1, 0, "say @luX later", 7, "\t\006\t\r", "say lua/source.c later"},
      {8, 1, 0, "", 0, "/app\t\006\t\t\r", "/approve off"},
      {8, 1, 0, "", 0, "/app\t\006\t\177\177\177 of\t\r", "/approve off"},
      {0, 1, 0, "", 0,
       "@lua/\t\177\177\177\177\177\177\177\177\177\177\177\t\t\r", "lib/"},
      {10, 0, 0, "", 0, "@lua\t\r", "lua/"},
      {10, 0, 0, "", 0, "@l\t\t\177\t\r", "lua/"},
      {10, 1, 0, "", 0, "@l\t\006\t\r", "lu-next"},
      {10, 1, 0, "", 0, "@l\006\t\r", "lu-next"},
      {10, 1, 0, "", 0, "@l\t\006\177\t\t\r", "lua/"},
      {12, 1, 0, "", 0, "@l\t\006\033s", "lu-next"},
      {11, 1, 0, "", 0, "@lua/app\t\r", "lua/apple"}};
  size_t i;
  for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    struct expansion_data data;
    struct winsize size = {24, 80, 0, 0};
    int slave, wake[2];
    sl_config_t config;
    sl_completion_provider_t provider;
    sl_watch_id_t watch;
    sl_t *sl;
    char *line;
    memset(&data, 0, sizeof(data));
    data.kind = cases[i].kind;
    CHECK(!openpty(&data.driver.master, &slave, NULL, NULL, &size) &&
          !pipe(wake));
    data.driver.initial = cases[i].initial;
    data.driver.cursor = cases[i].cursor;
    data.driver.keys = cases[i].keys;
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl);
    memset(&provider, 0, sizeof(provider));
    provider.trigger = data.kind == 8 ? '/' : '@';
    provider.scope = SL_COMPLETION_WORD;
    provider.strip_trigger = data.kind != 8;
    provider.callback = expansion_root;
    provider.expand = data.kind == 6 || data.kind == 7 ? NULL : expansion_more;
    provider.userdata = &data;
    CHECK(sl_set_completion_providers(sl, &provider, 1) == SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_TAB, host_tab, &data.driver) == SL_OK);
    if (data.kind == 12)
      CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, drive, &data.driver,
                       &watch) == SL_OK);
    CHECK(write(wake[1], "x", 1) == 1);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    line = sl_readline(sl, "> ");
    if (data.kind == 12) {
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!strcmp(sl_buffer(sl), "@lu") && sl_cursor(sl) == 3 &&
            data.expansions == 1);
      data.driver.initial = NULL;
      data.driver.keys = "\t\r";
      CHECK(write(wake[1], "x", 1) == 1);
      line = sl_readline(sl, "> ");
    }
    if (!line || strcmp(line, cases[i].expected) || data.roots != 1 ||
        data.expansions != cases[i].expansions ||
        data.driver.calls != cases[i].host_calls) {
      fprintf(stderr,
              "expansion %lu native=%d got=%s expected=%s roots=%d "
              "expand=%d/%d host=%d/%d\n",
              (unsigned long)i, native, line ? line : "NULL", cases[i].expected,
              data.roots, data.expansions, cases[i].expansions,
              data.driver.calls, cases[i].host_calls);
      return 1;
    }
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(data.driver.master);
    close(slave);
    close(wake[0]);
    close(wake[1]);
  }
  return 0;
}

static int suspend_api(sl_t *sl, sl_key_t key, void *data,
                       sl_key_action_t *action) {
  (void)key;
  (void)data;
  *action = SL_KEY_ACTION_HANDLED;
  return sl_suspend(sl);
}

static int completion_suspend(void) {
  static const struct completion_item items[] = {
      {"@alpha", SL_COMPLETION_WORD},
      {"@amber", SL_COMPLETION_WORD},
      {"@hello world", SL_COMPLETION_WORD},
      {"@café.txt", SL_COMPLETION_WORD}};
  static const struct {
    const char *keys, *draft, *resume, *expected;
    size_t cursor;
    int held;
    sl_key_t binding;
  } cases[] = {
      {"@a\033s", "@a", "\t\t\r", "amber", 2, 0, SL_KEY_NONE},
      {"@a\t\033s", "@alpha", "\t\r", "amber", 6, 0, SL_KEY_NONE},
      {"@hello \033s", "@hello ", "\t\r", "hello world", 7, 0, SL_KEY_NONE},
      {"@al\002\033s", "@al", "\t\r", "alpha", 2, 0, SL_KEY_NONE},
      {"@café\002\033s", "@café", "\t\r", "café.txt", 4, 0, SL_KEY_NONE},
      {"@a\024", "@a", "\t\r", "alpha", 2, 0, SL_KEY_NONE},
      {"\033e\033s", "@a", "\t\r\033s", "alpha", 2, 1, SL_KEY_NONE},
      {"@a\r", "@a", "\t\t\r", "amber", 2, 0, SL_KEY_ENTER},
      {"@a\t\r", "@alpha", "\t\r", "amber", 6, 0, SL_KEY_ENTER},
      {"@a\t\t\r", "@amber", "\t\r", "alpha", 6, 0, SL_KEY_ENTER},
      {"@a\t\033\r", "@alpha", "\t\r", "amber", 6, 0, SL_KEY_ALT_ENTER},
      {"@a\t\033e", "@alpha", "\t\r", "amber", 6, 0, SL_KEY_ALT_E},
      {"@a\t ", "@alpha", "\t\r", "amber", 6, 0, (sl_key_t)' '},
      {"@a\t\n", "@alpha", "\t\r", "amber", 6, 0, SL_KEY_CTRL_J},
      {"@a\t\033[200~", "@alpha", "\t\r", "amber", 6, 0, SL_KEY_PASTE_BEGIN}};
  int native, via_api;
  size_t i;
  for (via_api = 0; via_api <= 1; ++via_api)
    for (native = 0; native <= 1; ++native)
      for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        int slave, wake[2];
        struct winsize size = {24, 80, 0, 0};
        struct driver d;
        struct completion_fixture fixture;
        sl_config_t config;
        sl_completion_provider_t provider;
        sl_watch_id_t watch;
        sl_prompt_id_t id = 0;
        sl_prompt_queue_entry_info_t info;
        sl_t *sl;
        char *line;
        if (via_api && cases[i].binding == SL_KEY_NONE)
          continue;
        memset(&d, 0, sizeof(d));
        CHECK(!openpty(&d.master, &slave, NULL, NULL, &size) && !pipe(wake));
        sl_config_init(&config);
        config.input_fd = config.output_fd = slave;
        config.prompt_theme = SL_PROMPT_THEME_PLAIN;
        config.prompt_queue = 1;
        sl = sl_create_with_config(&config);
        CHECK(sl);
        fixture.items = items;
        fixture.count = sizeof(items) / sizeof(items[0]);
        fixture.calls = 0;
        memset(&provider, 0, sizeof(provider));
        provider.trigger = '@';
        provider.scope = SL_COMPLETION_WORD;
        provider.strip_trigger = 1;
        provider.callback = fixture_complete;
        provider.userdata = &fixture;
        CHECK(sl_set_completion_providers(sl, &provider, 1) == SL_OK);
        CHECK(sl_set_prompt_queue_delivery(
                  sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
        CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) ==
              SL_OK);
        CHECK(sl_bind_key(sl, SL_KEY_CTRL_T, suspend_api, NULL) == SL_OK);
        if (cases[i].binding != SL_KEY_NONE)
          CHECK(sl_bind_key(sl, cases[i].binding,
                            via_api ? suspend_api : suspend_key,
                            NULL) == SL_OK);
        if (cases[i].held)
          CHECK(sl_prompt_queue_append(sl, "@a", 1, &id) == SL_OK);
        CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, drive, &d, &watch) ==
              SL_OK);
        if (native)
          CHECK(sl_output_stream_begin(sl) == SL_OK);
        d.keys = cases[i].keys;
        CHECK(write(wake[1], "x", 1) == 1);
        CHECK(!sl_readline(sl, "> ") &&
              sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
        CHECK(!strcmp(sl_buffer(sl), cases[i].draft) &&
              sl_cursor(sl) == cases[i].cursor && fixture.calls == 1);
        CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK && info.id == id);
        if (id)
          CHECK(info.held);
        if (native) {
          CHECK(sl_output_stream_end(sl) == SL_OK);
          CHECK(sl_output_stream_begin(sl) == SL_OK);
          CHECK(!strcmp(sl_buffer(sl), cases[i].draft) &&
                sl_cursor(sl) == cases[i].cursor);
        }
        d.keys = cases[i].resume;
        if (cases[i].binding != SL_KEY_NONE)
          CHECK(sl_bind_key(sl, cases[i].binding, NULL, NULL) == SL_OK);
        CHECK(write(wake[1], "x", 1) == 1);
        line = sl_readline(sl, "> ");
        if (id) {
          CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
          CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == id &&
                info.held);
          CHECK(sl_prompt_queue_peek(sl, 0, &line) == SL_OK);
        }
        if (!line || strcmp(line, cases[i].expected) || fixture.calls != 1) {
          fprintf(stderr,
                  "suspend case=%lu native=%d got=%s expected=%s calls=%d\n",
                  (unsigned long)i, native, line ? line : "NULL",
                  cases[i].expected, fixture.calls);
          return 1;
        }
        sl_free_string(sl, line);
        sl_destroy(sl);
        close(d.master);
        close(slave);
        close(wake[0]);
        close(wake[1]);
      }
  return 0;
}

static int whitespace_cases(void) {
  static const struct {
    const char *initial, *keys, *expected;
    size_t cursor;
    int calls;
  } cases[] = {{"", "hello \t\r", "hello world", 0, 0},
               {"", "hello w\t\r", "hello world", 0, 0},
               {"", "hello \t\t\r", "hello there", 0, 0},
               {"", "hello \t\t\177\t\r", "hello there", 0, 0},
               {"", "hello world \t\r", "hello world #", 0, 1},
               {"", "end \t\r", "end #", 0, 1},
               {"", "@end \t\r", "end #", 0, 1},
               {"", "hello x \t\r", "hello x #", 0, 1},
               {"", "hello  \t\r", "hello  world", 0, 0},
               {"", "hello\n\t\r", "hello\nworld", 0, 0},
               {"", "hello \r", "hello ", 0, 0},
               {"", "say hello \t\r", "say hello world", 0, 0},
               {"", "hello \t\037\r", "hello ", 0, 0},
               {"", "hello \177\t\r", "hello world", 0, 0},
               {"", "@hello \t\r", "hello world", 0, 0},
               {"", "@hello \t\t\r", "hello there", 0, 0},
               {"", "@hello w\t \t\r", "hello world #", 0, 1},
               {"", "@hello x \t\r", "hello x #", 0, 1},
               {"", "hello\024\t\r", "hello world", 0, 0},
               {"", "@hello\024\t\r", "hello world", 0, 0},
               {" ", "\001\013hello\031\t\r", "hello world", 1, 0},
               {"helloX later", "\002\006 \t\r", "hello world later", 5, 0}};
  size_t i;
  int native;
  for (native = 0; native <= 1; ++native) {
    CHECK(!run(native, "", 0, "hello @\t\r", "hello hello world", 0, 131072,
               PHRASE_TRIGGER));
    CHECK(!run(native, "", 0, "hello\n@\t\r", "hello\nhello world", 0, 131072,
               PHRASE_TRIGGER));
    CHECK(!run(native, "", 0, "@hello @\t\r", "hello hello world", 0, 131072,
               PHRASE_TRIGGER));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      CHECK(!run(native, cases[i].initial, cases[i].cursor, cases[i].keys,
                 cases[i].expected, cases[i].calls, 131072, PHRASE_WHITESPACE));
  }
  return 0;
}

int main(int argc, char **argv) {
  static const struct {
    const char *initial, *keys, *expected;
    size_t cursor;
    int calls;
  } cases[] = {
      {"", "/sta\t\r", "/status", 0, 0},
      {"", "/sta\006\r", "/status", 0, 0},
      {"", "/sta\033[C\r", "/status", 0, 0},
      {"/app", "\006\t\r", "/approve on#", 4, 1},
      {"/app", "\033[C\t\r", "/approve on#", 4, 1},
      {"/sta", "\006\037\r", "/sta", 4, 0},
      {"say ver later", "\006!\r", "say verification! later", 7, 0},
      {"say verX later", "\006!\r", "say verX! later", 7, 0},
      {"say ver", "\002\006!\r", "say ver!", 7, 0},
      {"caf", "\006\t\r", "caféclair#", 3, 1},
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
  if (argc == 2 && !strcmp(argv[1], "whitespace"))
    return whitespace_cases();
  if (argc == 2 && !strcmp(argv[1], "suspend"))
    return completion_suspend();
  CHECK(sl_set_completion_providers(NULL, NULL, 0) == SL_ERROR_INVALID);
  for (native = 0; native <= 1; ++native) {
    CHECK(!expansion_cases(native));
    CHECK(!provider_cases(native));
    CHECK(
        !run(native, "/sta", 4, "\006\r", "/sta#", 1, 131072, CTRL_F_OVERRIDE));
    CHECK(!run(native, "/sta", 4, "\033[C\r", "/sta#", 1, 131072,
               RIGHT_OVERRIDE));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i)
      CHECK(!run(native, cases[i].initial, cases[i].cursor, cases[i].keys,
                 cases[i].expected, cases[i].calls, 131072, 0));
    CHECK(!run(native, "x ver", 5, "\t\r", "x ver", 0, 12, 0));
    CHECK(!run(native, "x ver", 5, "\006\t\r", "x ver", 0, 12, 0));
    CHECK(!run(native, "", 0, "\033e\t\r\033s", "verification", 0, 131072, 1));
    CHECK(
        !run(native, "", 0, "\033e\006\r\033s", "verification", 0, 131072, 1));
    CHECK(!run(native, "ver", 3, "\t\r", "verification", 0, 131072, QUEUE_TAB));
    CHECK(!run(native, "verification", 12, "\t\r", "", 0, 131072, QUEUE_TAB));
    CHECK(!run(native, "x ver", 5, "\t\r", "x verification", 0, 131072,
               COPIED_LIST));
    CHECK(!run(native, "/sta", 4, "\t\r", "/sta#", 1, 131072, CLEARED_LIST));
    CHECK(!run(native, "ca", 2, "\t\r", "cafe\314\201x", 0, 131072,
               CLUSTER_FIRST));
    CHECK(!run(native, "ca", 2, "\t\r", "cafet", 0, 131072, CLUSTER_LAST));
    CHECK(!run(native, "/", 1, "\t", "#", 1, 131072, HOST_ENQUEUE));
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
