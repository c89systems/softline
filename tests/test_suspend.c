#define _DEFAULT_SOURCE
#include <pty.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

struct input {
  int master, sent, failed;
  const char *keys, *initial;
  size_t cursor;
};

static void type_keys(sl_t *sl, void *userdata) {
  struct input *input = userdata;
  if (input->sent)
    return;
  input->sent = 1;
  if (input->initial && (sl_set_buffer(sl, input->initial) != SL_OK ||
                         sl_set_cursor(sl, input->cursor) != SL_OK))
    input->failed = 1;
  if (write(input->master, input->keys, strlen(input->keys)) !=
      (ssize_t)strlen(input->keys))
    input->failed = 1;
  if (input->failed)
    (void)sl_cancel(sl);
}

static int ready_input(sl_t *sl, const sl_watch_event_t *event,
                       void *userdata) {
  struct input *input = userdata;
  char byte;
  if (read(event->fd, &byte, 1) != 1)
    return SL_ERROR_IO;
  type_keys(sl, input);
  return input->failed ? SL_ERROR_IO : SL_OK;
}

static int suspend_key(sl_t *sl, sl_key_t key, void *userdata,
                       sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  *action = SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}

static int record_history(const char *key, const char *prompt, void *userdata) {
  int *count = userdata;
  (void)key;
  (void)prompt;
  ++*count;
  return SL_OK;
}

static char *read_keys(sl_t *sl, int master, int native, const char *initial,
                       size_t cursor, const char *keys,
                       sl_prompt_source_t *source) {
  struct input input;
  char *line;
  int wake[2];
  sl_watch_id_t watch;
  memset(&input, 0, sizeof(input));
  input.master = master;
  input.initial = initial;
  input.cursor = cursor;
  input.keys = keys;
  if (pipe(wake) != 0)
    return NULL;
  if (sl_watch_add(sl, wake[0], SL_WATCH_READ, ready_input, &input, &watch) !=
      SL_OK) {
    close(wake[0]);
    close(wake[1]);
    return NULL;
  }
  /* Wake the owner as soon as raw input is acquired, without a polling sleep.
   */
  if (write(wake[1], "x", 1) != 1) {
    (void)sl_watch_remove(sl, watch);
    close(wake[0]);
    close(wake[1]);
    return NULL;
  }
  line = native ? sl_next_prompt(sl, "draft> ", source)
                : sl_readline(sl, "draft> ");
  (void)sl_watch_remove(sl, watch);
  close(wake[0]);
  close(wake[1]);
  return line;
}

static int editing(int native) {
  static const struct {
    const char *name, *initial;
    size_t cursor;
    const char *before, *suspended;
    size_t suspended_cursor;
    const char *after, *expected;
    int search;
  } cases[] = {
      {"multiline Unicode cursor", NULL, 0,
       "\033[200~café\n中文 draft\033[201~\002\002\033s", "café\n中文 draft",
       sizeof("café\n中文 dra") - 1, "!\r", "café\n中文 dra!ft", 0},
      {"undo journal", NULL, 0, "abc\002X\033s", "abXc", 3, "\037\r", "abc", 0},
      {"kill ring", NULL, 0, "one two\027\033s", "one ", 4, "\031\r", "one two",
       0},
      {"preferred column", "abcdefghij\nx\nabcdefghij", 8, "\033[B\033s",
       "abcdefghij\nx\nabcdefghij", 12, "\033[B!\r",
       "abcdefghij\nx\nabcdefgh!ij", 0},
      {"reverse search cancellation", "my draft", 2, "\022match\033s",
       "matching history", 16, "\007!\r", "my draft!", 1},
      {"reverse search acceptance", "my draft", 2, "\022match\033s",
       "matching history", 16, "\r!\r", "matching history!", 1},
      {"history navigation", NULL, 0, "draft\033[A\033s", "matching history",
       16, "\033[B!\r", "draft!", 0},
      {"empty draft", NULL, 0, "\033s", "", 0, "!\r", "!", 0},
      {"yank rotation", NULL, 0, "one two\027\001\013\031\033s", "one ", 4,
       "\033y\r", "two", 0}};
  size_t index;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int master, slave, appended = 0;
    struct winsize size = {24, 80, 0, 0};
    struct termios modes;
    sl_config_t config;
    sl_t *sl;
    sl_prompt_source_t source;
    sl_prompt_id_t queued;
    char *line;
    fprintf(stderr, "%s: %s\n", native ? "native" : "readline",
            cases[index].name);
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_queue = 1;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    CHECK(sl_history_set_backend(sl, "suspend", NULL, record_history,
                                 &appended) == SL_OK);
    CHECK(sl_history_add(sl, "matching history") == SL_OK);
    CHECK(sl_history_set_auto_add(sl, 1) == SL_OK);
    CHECK(sl_set_status_message(sl, "status sentinel") == SL_OK);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    line = read_keys(sl, master, native, cases[index].initial,
                     cases[index].cursor, cases[index].before, &source);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(!native || source == SL_PROMPT_SOURCE_NONE);
    CHECK(!strcmp(sl_buffer(sl), cases[index].suspended));
    CHECK(sl_cursor(sl) == cases[index].suspended_cursor);
    CHECK(sl_history_search_active(sl) == cases[index].search);
    CHECK(appended == 1);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    CHECK(tcgetattr(slave, &modes) == 0 && (modes.c_lflag & ICANON));
    size.ws_row = 16;
    size.ws_col = index == 3 ? 80 : 60;
    CHECK(ioctl(slave, TIOCSWINSZ, &size) == 0);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    /* Repeated suspension must keep the same continuation, including search. */
    line = read_keys(sl, master, native, NULL, 0, "\033s", &source);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(!strcmp(sl_buffer(sl), cases[index].suspended));
    CHECK(sl_cursor(sl) == cases[index].suspended_cursor);
    CHECK(sl_history_search_active(sl) == cases[index].search);
    CHECK(sl_prompt_queue_append(sl, "arrived during handoff", 0, &queued) ==
          SL_OK);
    line = read_keys(sl, master, native, NULL, 0, cases[index].after, &source);
    CHECK(line && !strcmp(line, cases[index].expected));
    CHECK(!native || source == SL_PROMPT_SOURCE_DIRECT);
    CHECK(appended == 2 && !sl_history_search_active(sl));
    sl_free_string(sl, line);
    line = sl_next_prompt(sl, "draft> ", &source);
    CHECK(line && !strcmp(line, "arrived during handoff"));
    CHECK(source == SL_PROMPT_SOURCE_QUEUED && sl_last_prompt_id(sl) == queued);
    sl_free_string(sl, line);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

static int held_edit(void) {
  int master, slave;
  struct winsize size = {24, 80, 0, 0};
  sl_config_t config;
  sl_t *sl;
  sl_prompt_id_t held, other;
  sl_prompt_queue_entry_info_t info;
  char *line;
  CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.prompt_theme = SL_PROMPT_THEME_PLAIN;
  sl = sl_create_with_config(&config);
  CHECK(sl &&
        sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
  CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(sl_prompt_queue_append(sl, "held", 1, &held) == SL_OK);
  CHECK(sl_prompt_queue_insert(sl, 0, "other", 1, &other) == SL_OK);
  CHECK(sl_output_stream_begin(sl) == SL_OK);
  line = read_keys(sl, master, 1, NULL, 0, "\033e-edited\033s", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
  CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK && info.id == held &&
        info.held);
  CHECK(!strcmp(sl_buffer(sl), "held-edited"));
  CHECK(sl_output_stream_end(sl) == SL_OK &&
        sl_output_stream_begin(sl) == SL_OK);
  line = read_keys(sl, master, 1, NULL, 0, "\r\033s", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
  CHECK(sl_prompt_queue_peek(sl, 1, &line) == SL_OK &&
        !strcmp(line, "held-edited"));
  sl_free_string(sl, line);
  CHECK(sl_prompt_queue_info(sl, 1, &info) == SL_OK && info.id == held &&
        info.held);
  CHECK(sl_output_stream_end(sl) == SL_OK &&
        sl_output_stream_begin(sl) == SL_OK);
  /* Empty submission deletes the oldest approval; the next one can be
   * focused before suspension and decided after handoff. */
  line = read_keys(sl, master, 1, NULL, 0, "\033e\001\013\r\033a\033s", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
  CHECK(sl_prompt_queue_count(sl) == 1);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == other &&
        info.held);
  CHECK(sl_prompt_queue_reject(sl, held) == SL_ERROR_INVALID);
  CHECK(sl_output_stream_end(sl) == SL_OK &&
        sl_output_stream_begin(sl) == SL_OK);
  /* Focus survives suspension: Alt-Y acts on the offered approval. */
  line = read_keys(sl, master, 1, NULL, 0, "\033y\033s", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == other &&
        !info.held);
  CHECK(sl_history_add(sl, "matching history") == SL_OK);
  line = read_keys(sl, master, 1, NULL, 0, "draft\022match\033s", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
  CHECK(sl_history_search_active(sl));
  CHECK(sl_output_stream_end(sl) == SL_OK);
  /* Destroy while suspended owns search allocations as well as the draft. */
  sl_destroy(sl);
  close(master);
  close(slave);
  return 0;
}

static int mutations(int native) {
  static const struct {
    const char *name, *before, *replacement, *inserted;
    size_t at, limit;
    int rejected;
    const char *undo[3];
    size_t cursor[3];
  } cases[] = {
      {"clear", "abc", "", NULL, 0, 1024, 0, {"abc", ""}, {3, 0}},
      {"shrink", "abc", "z", NULL, 0, 1024, 0, {"abc", ""}, {3, 0}},
      {"replace", "abc", "xyz", NULL, 0, 1024, 0, {"abc", ""}, {3, 0}},
      {"grow", "abc", "a longer draft", NULL, 0, 1024, 0, {"abc", ""}, {3, 0}},
      {"reallocate", "abc", NULL, NULL, 0, 1024, 0, {"abc", ""}, {3, 0}},
      {"insert at start", "abc", NULL, "X", 0, 1024, 0, {"abc", ""}, {0, 0}},
      {"insert in middle", "abc", NULL, "X", 1, 1024, 0, {"abc", ""}, {1, 0}},
      {"insert at end", "abc", NULL, "X", 3, 1024, 0, {"abc", ""}, {3, 0}},
      {"Unicode replacement",
       "aé中",
       "👩‍💻\né",
       NULL,
       0,
       1024,
       0,
       {"aé中", ""},
       {6, 0}},
      {"Unicode insertion",
       "aé中",
       NULL,
       "👩‍💻\né",
       3,
       1024,
       0,
       {"aé中", ""},
       {3, 0}},
      {"multiple host edits",
       "abc",
       "new",
       "!",
       3,
       1024,
       0,
       {"new", "abc", ""},
       {3, 3, 0}},
      {"same text", "abc", "abc", NULL, 0, 1024, 0, {""}, {0}},
      {"empty insertion", "abc", NULL, "", 1, 1024, 0, {""}, {0}},
      {"rejected replacement", "abc", NULL, NULL, 0, 8, 1, {""}, {0}},
      {"rejected insertion",
       "abc",
       NULL,
       "too long to fit",
       1,
       8,
       1,
       {""},
       {0}},
      {"full draft", "abc", "12345678", NULL, 0, 8, 0, {"abc", ""}, {3, 0}}};
  size_t index, undo;
  char large[513], keys[128];
  memset(large, 'L', sizeof(large) - 1);
  large[sizeof(large) - 1] = 0;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int master, slave, result, appended = 0;
    struct winsize size = {24, 80, 0, 0};
    sl_config_t config;
    sl_t *sl;
    char *line;
    const char *replacement = cases[index].replacement;
    fprintf(stderr, "%s mutation: %s\n", native ? "native" : "readline",
            cases[index].name);
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    config.line_max_len = cases[index].limit;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    CHECK(sl_history_set_backend(sl, "mutations", NULL, record_history,
                                 &appended) == SL_OK);
    CHECK(sl_history_set_auto_add(sl, 1) == SL_OK);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    snprintf(keys, sizeof(keys), "%s\033s", cases[index].before);
    line = read_keys(sl, master, native, NULL, 0, keys, NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    if (!replacement && !cases[index].inserted)
      replacement = large;
    if (replacement) {
      result = index % 2 ? sl_set_buffer(sl, replacement)
                         : sl->set_buffer(sl, replacement);
      CHECK(cases[index].rejected ? result != SL_OK : result == SL_OK);
    }
    if (cases[index].inserted) {
      CHECK(sl_set_cursor(sl, cases[index].at) == SL_OK);
      result = index % 2 ? sl_insert(sl, cases[index].inserted)
                         : sl->insert(sl, cases[index].inserted);
      CHECK(cases[index].rejected ? result != SL_OK : result == SL_OK);
    }
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    for (undo = 0; undo < 3 && cases[index].undo[undo]; ++undo) {
      line = read_keys(sl, master, native, NULL, 0, "\037\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!strcmp(sl_buffer(sl), cases[index].undo[undo]));
      CHECK(sl_cursor(sl) == cases[index].cursor[undo]);
      CHECK(appended == 0);
    }
    line = read_keys(sl, master, native, NULL, 0, "\037\r", NULL);
    CHECK(line && !*line &&
          sl_last_readline_status(sl) == SL_READLINE_SUBMITTED);
    sl_free_string(sl, line);
    CHECK(appended == 0);
    /* A fresh prompt cannot undo into the completed continuation. */
    line = read_keys(sl, master, native, NULL, 0, "new\037\037\r", NULL);
    CHECK(line && !*line);
    sl_free_string(sl, line);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

struct column_input {
  int master, slave, phase, failed, resize, repeats;
  const char *draft, *move;
  size_t cursor;
};

static int resize_before_move(sl_t *sl, sl_key_t key, void *userdata,
                              sl_key_action_t *action) {
  struct column_input *input = userdata;
  (void)sl;
  (void)key;
  *action = SL_KEY_ACTION_PASS;
  if (input->resize == 2 && input->phase == 2) {
    struct winsize size = {24, 60, 0, 0};
    if (ioctl(input->slave, TIOCSWINSZ, &size) != 0)
      return SL_ERROR_IO;
  }
  return SL_OK;
}

static int output(sl_t *sl, void *userdata, const char **chunk,
                  size_t *length) {
  int *sent = userdata;
  (void)sl;
  *chunk = "output above the editor\n";
  *length = (*sent)++ ? 0 : strlen(*chunk);
  return SL_OK;
}

static void output_between_moves(sl_t *sl, void *userdata) {
  struct column_input *input = userdata;
  const char *keys = input->move;
  char finish[32];
  if (input->phase == 0) {
    if (sl_set_buffer(sl, input->draft) != SL_OK ||
        sl_set_cursor(sl, input->cursor) != SL_OK)
      input->failed = 1;
  } else if (input->phase == 1) {
    int repeat;
    if (input->resize == 1 && sl_set_screen_width(sl, 60) != SL_OK)
      input->failed = 1;
    for (repeat = 0; repeat < input->repeats; ++repeat) {
      int sent = 0;
      if (sl_print_above(sl, output, &sent) != SL_OK)
        input->failed = 1;
    }
    snprintf(finish, sizeof(finish), "%s!\r", input->move);
    keys = finish;
  } else
    return;
  ++input->phase;
  if (write(input->master, keys, strlen(keys)) != (ssize_t)strlen(keys))
    input->failed = 1;
  if (input->failed)
    (void)sl_cancel(sl);
}

static int columns(void) {
  static const struct {
    const char *draft, *move, *expected;
    size_t cursor;
    int resize, repeats;
  } cases[] = {{"abcdefghij\nx\nabcdefghij", "\033[B",
                "abcdefghij\nx\nabcdefgh!ij", 8, 0, 1},
               {"abcdefghij\n\nabcdefghij", "\033[B",
                "abcdefghij\n\nabcdefgh!ij", 8, 0, 2},
               {"abcdefghij\n中\nabcdefghij", "\033[B",
                "abcdefghij\n中\nabcdefgh!ij", 8, 0, 1},
               {"abcdefghij\nx\nabcdefghij", "\033[A",
                "abcdefgh!ij\nx\nabcdefghij", 21, 0, 1},
               {"abcdefghij\nx\nabcdefghij", "\033[B",
                "abcdefghij\nx\na!bcdefghij", 8, 1, 1},
               {"abcdefghij\nx\nabcdefghij", "\033[B",
                "abcdefghij\nx\na!bcdefghij", 8, 2, 1}};
  size_t index;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int master, slave;
    struct winsize size = {24, 80, 0, 0};
    sl_config_t config;
    struct column_input input;
    sl_t *sl;
    char *line;
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl);
    memset(&input, 0, sizeof(input));
    input.master = master;
    input.slave = slave;
    input.draft = cases[index].draft;
    input.cursor = cases[index].cursor;
    input.move = cases[index].move;
    input.resize = cases[index].resize;
    input.repeats = cases[index].repeats;
    CHECK(sl_bind_key(sl, SL_KEY_DOWN, resize_before_move, &input) == SL_OK);
    CHECK(sl_set_idle_callback(sl, output_between_moves, &input) == SL_OK);
    line = sl_readline(sl, "draft> ");
    CHECK(line && !input.failed && !strcmp(line, cases[index].expected));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

struct rejection {
  sl_prompt_id_t id;
  int result;
};

static int reject_key(sl_t *sl, sl_key_t key, void *userdata,
                      sl_key_action_t *action) {
  struct rejection *rejection = userdata;
  (void)key;
  rejection->result = sl_prompt_queue_reject(sl, rejection->id);
  *action = SL_KEY_ACTION_HANDLED;
  return rejection->result;
}

static int cancel_key(sl_t *sl, sl_key_t key, void *userdata,
                      sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)userdata;
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}

static int transitions(int native) {
  static const char *const before[] = {"abc\022match\033s",
                                       "abc\022match\033s",
                                       "\033e-edit\033s",
                                       "\033e-edit\022match\033s",
                                       "\033e-edit\022match\t\037\033s",
                                       "abc\033s",
                                       "abc\033s",
                                       "one two\027\001\013\031\033s"};
  int scenario;
  for (scenario = 0; scenario < 8; ++scenario) {
    int master, slave;
    struct winsize size = {24, 80, 0, 0};
    sl_config_t config;
    sl_t *sl;
    sl_prompt_id_t other = 0;
    sl_prompt_queue_entry_info_t info;
    struct rejection rejection;
    char *line;
    fprintf(stderr, "%s transition: %d\n", native ? "native" : "readline",
            scenario);
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_queue = 1;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_ESCAPE, cancel_key, NULL) == SL_OK);
    CHECK(sl_history_add(sl, "matching history") == SL_OK);
    CHECK(sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
    memset(&rejection, 0, sizeof(rejection));
    if (scenario >= 2 && scenario <= 4) {
      CHECK(sl_prompt_queue_append(sl, "held", 1, &rejection.id) == SL_OK);
      CHECK(sl_prompt_queue_append(sl, "other", 1, &other) == SL_OK);
      CHECK(sl_bind_key(sl, SL_KEY_TAB, reject_key, &rejection) == SL_OK);
    }
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    line = read_keys(sl, master, native, NULL, 0, before[scenario], NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    if (scenario == 0 || scenario == 1) {
      /* Host edits the search candidate, while cancellation still owns the
       * saved draft and acceptance creates a new undo baseline. */
      CHECK(sl_set_buffer(sl, "replacement") == SL_OK);
      line = read_keys(sl, master, native, NULL, 0,
                       scenario == 0 ? "\007\037\033s" : "\r\037\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!strcmp(sl_buffer(sl), scenario == 0 ? "" : "replacement"));
    } else if (scenario == 2) {
      CHECK(sl_set_buffer(sl, "") == SL_OK);
      line = read_keys(sl, master, native, NULL, 0, "\037\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!strcmp(sl_buffer(sl), "held-edit"));
      CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
            info.id == rejection.id && info.held);
      line = read_keys(sl, master, native, NULL, 0, "\037\r\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(sl_prompt_queue_peek(sl, 0, &line) == SL_OK &&
            !strcmp(line, "held"));
      sl_free_string(sl, line);
      CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK &&
            info.id == rejection.id && info.held);
      CHECK(sl_prompt_queue_info(sl, 1, &info) == SL_OK && info.id == other &&
            info.held);
    } else if (scenario == 3 || scenario == 4) {
      if (scenario == 3)
        CHECK(sl_prompt_queue_reject(sl, rejection.id) == SL_OK);
      else
        CHECK(rejection.result == SL_OK);
      CHECK(!sl_history_search_active(sl) && !*sl_buffer(sl));
      line = read_keys(sl, master, native, NULL, 0, "\007\037\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!*sl_buffer(sl));
      CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK && !info.id);
      CHECK(sl_prompt_queue_count(sl) == 1);
      CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.id == other &&
            info.held);
    } else if (scenario == 5 || scenario == 6) {
      CHECK(sl_set_buffer(sl, scenario == 5 ? "replacement" : "") == SL_OK);
      line = read_keys(sl, master, native, NULL, 0,
                       scenario == 5 ? "\033" : "\004", NULL);
      CHECK(!line &&
            sl_last_readline_status(sl) ==
                (scenario == 5 ? SL_READLINE_CANCELLED : SL_READLINE_EOF));
      line = read_keys(sl, master, native, NULL, 0, "\037fresh\037\r", NULL);
      CHECK(line && !*line);
      sl_free_string(sl, line);
    } else {
      CHECK(sl_insert(sl, "X") == SL_OK);
      line = read_keys(sl, master, native, NULL, 0, "\033y\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!strcmp(sl_buffer(sl), "one X"));
      line = read_keys(sl, master, native, NULL, 0, "\037\033s", NULL);
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
      CHECK(!strcmp(sl_buffer(sl), "one "));
    }
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

struct cursor_group {
  int master, phase, failed, split, wake_write;
  const char *before, *expected;
  size_t target;
};

static void cursor_group_step(sl_t *sl, void *userdata) {
  struct cursor_group *state = userdata;
  const char *keys;
  if (state->phase == 0)
    keys = state->before;
  else if (state->split && state->phase == 1 &&
           !strcmp(sl_buffer(sl), state->expected)) {
    /* Navigate while the next UTF-8 key awaits continuation, after its
     * typing command has already begun. */
    if (write(state->master, "\303", 1) != 1 ||
        write(state->wake_write, "x", 1) != 1)
      state->failed = 1;
    ++state->phase;
    return;
  } else if ((state->phase == 1 || (state->split && state->phase == 2)) &&
             !strcmp(sl_buffer(sl), state->expected)) {
    if (sl_set_cursor(sl, state->target) != SL_OK)
      state->failed = 1;
    keys = state->split ? "\251\037\033s" : "X\037\033s";
  } else
    return;
  ++state->phase;
  if (write(state->master, keys, strlen(keys)) != (ssize_t)strlen(keys))
    state->failed = 1;
  if (state->failed)
    (void)sl_cancel(sl);
}

static int cursor_group_watch(sl_t *sl, const sl_watch_event_t *event,
                              void *userdata) {
  struct cursor_group *state = userdata;
  char byte;
  if (state->phase == 1 && strcmp(sl_buffer(sl), state->expected))
    return SL_OK;
  if (read(event->fd, &byte, 1) != 1)
    return SL_ERROR_IO;
  cursor_group_step(sl, state);
  return state->failed ? SL_ERROR_IO : SL_OK;
}

static int cursor_groups(int native, int watch) {
  static const struct {
    const char *text;
    size_t target, cursor;
  } cases[] = {
      {"abc", 0, 0}, {"abc", 1, 1}, {"abc", (size_t)-1, 3}, {"aé中", 2, 1}};
  size_t index;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int master, slave, wake[2];
    struct winsize size = {24, 80, 0, 0};
    sl_config_t config;
    sl_t *sl;
    sl_watch_id_t id = 0;
    struct cursor_group state;
    char *line;
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    memset(&state, 0, sizeof(state));
    state.master = master;
    state.before = state.expected = cases[index].text;
    state.target = cases[index].target;
    state.split = watch == 2;
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    if (watch) {
      CHECK(pipe(wake) == 0);
      state.wake_write = wake[1];
      CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, cursor_group_watch, &state,
                         &id) == SL_OK);
      CHECK(write(wake[1], "xx", 2) == 2);
    } else
      CHECK(sl_set_idle_callback(sl, cursor_group_step, &state) == SL_OK);
    line = native ? sl_next_prompt(sl, "draft> ", NULL)
                  : sl_readline(sl, "draft> ");
    if (watch) {
      CHECK(sl_watch_remove(sl, id) == SL_OK);
      close(wake[0]);
      close(wake[1]);
    } else
      CHECK(sl_set_idle_callback(sl, NULL, NULL) == SL_OK);
    CHECK(!state.failed && !line &&
          sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(!strcmp(sl_buffer(sl), cases[index].text));
    CHECK(sl_cursor(sl) == cases[index].cursor);
    line = read_keys(sl, master, native, NULL, 0, "\037\r", NULL);
    CHECK(line && !*line);
    sl_free_string(sl, line);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

static int mutating_suspend(sl_t *sl, sl_key_t key, void *userdata,
                            sl_key_action_t *action) {
  int mode = *(int *)userdata;
  int status = SL_OK;
  (void)key;
  if (mode == 1)
    status = sl_insert(sl, "*");
  else if (mode == 2)
    status = sl_set_buffer(sl, "new");
  else if (mode == 3)
    status = sl_set_buffer(sl, "");
  else if (mode == 4)
    status = sl_set_cursor(sl, 0);
  else if (mode == 5) {
    status = sl_insert(sl, "*");
    if (status == SL_OK)
      status = sl_insert(sl, "!");
  } else if (mode == 6) {
    status = sl_set_buffer(sl, "abc");
    if (status == SL_OK)
      status = sl_insert(sl, "");
  } else if (mode == 7)
    status = sl_insert(sl, "this insertion exceeds the draft limit") == SL_OK
                 ? SL_ERROR
                 : SL_OK;
  else if (mode == 8) {
    status = sl_set_buffer(sl, "temporary");
    if (status == SL_OK)
      status = sl_set_buffer(sl, "abc");
  }
  *action = SL_KEY_ACTION_SUSPEND;
  return status;
}

static int suspend_groups(int native) {
  const char *const expected[] = {"abc",   "abc*", "new", "",   "abc",
                                  "abc*!", "abc",  "abc", "abc"};
  int mode;
  for (mode = 0; mode < 9; ++mode) {
    int master, slave;
    struct winsize size = {24, 80, 0, 0};
    sl_config_t config;
    sl_t *sl;
    char *line;
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.line_max_len = 16;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 't', mutating_suspend, &mode) ==
          SL_OK);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    line = read_keys(sl, master, native, NULL, 0, "abc\033t", NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(!strcmp(sl_buffer(sl), expected[mode]));
    line = read_keys(sl, master, native, NULL, 0, "X\037\033s", NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(!strcmp(sl_buffer(sl), expected[mode]));
    CHECK(sl_cursor(sl) == (mode == 4 ? 0 : strlen(expected[mode])));
    /* A callback's edits undo together, separately from both typing runs. */
    line = read_keys(sl, master, native, NULL, 0, "\037\033s", NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(
        !strcmp(sl_buffer(sl),
                mode == 0 || mode == 4 || mode == 6 || mode == 7 ? "" : "abc"));
    line = read_keys(sl, master, native, NULL, 0, "\037\r", NULL);
    CHECK(line && !*line);
    sl_free_string(sl, line);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

static int visual_rows(int native) {
  static const struct {
    const char *name, *text, *keys;
    int columns;
    size_t start, expected;
  } cases[] = {
      {"reported Down/Up", "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb",
       "\033[B\033[A", 24, 17, 17},
      {"Down stays before a soft boundary",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb", "\033[B", 24, 17, 34},
      {"Up from the last fragment", "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb",
       "\033[A", 24, 36, 19},
      {"repeated Up progresses", "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb",
       "\033[A\033[A", 24, 36, 1},
      {"Up retains a remembered wide column",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb", "\033[B\033[B\033[A", 24, 17,
       34},
      {"repeated Down reaches EOF", "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb",
       "\033[B\033[B", 24, 17, 36},
      {"round trip retains the preferred column",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbbb", "\033[B\033[B\033[A\033[A", 24,
       17, 17},
      {"exactly full last row", "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbb",
       "\033[B\033[B\033[A\033[A", 24, 17, 17},
      {"Down reaches the full row's EOF position",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbb", "\033[B\033[B", 24, 17, 35},
      {"Up from full-row EOF preserves its left column",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbb", "\033[A", 24, 35, 18},
      {"full hard newline remains addressable",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbb\nc", "\033[B", 24, 17, 35},
      {"word wrap owns its boundary", "aaaaaaaaaaaaaaaaa\naaaa bbbbbbbbbbbbb",
       "\033[B", 24, 17, 21},
      {"multiple skipped spaces", "aaaaaaaaaaaaaaaaa\naaaa   bbbbbbbbbbbbb",
       "\033[B\033[A", 24, 17, 17},
      {"leftmost word-wrapped positions",
       "aaaaaaaaaaaaaaaaa\naaaa   bbbbbbbbbbbbb", "\033[B\033[B", 24, 0, 25},
      {"wide cluster moves as a whole",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbb中Z", "\033[B", 24, 17, 33},
      {"combining cluster remains whole",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbbéZ", "\033[B", 24, 17, 34},
      {"joined emoji remains whole",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbb👩‍💻Z", "\033[B", 24, 17, 33},
      {"tab starts on its original row",
       "aaaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbb\tZ", "\033[B", 24, 17, 34},
      {"tab continuation rows have no cursor position", "aaa\na\tX",
       "\033[B\033[B", 10, 3, 7},
      {"Up skips unaddressable tab rows", "aaa\na\tX", "\033[A", 10, 7, 5},
      {"tab round trip", "aaa\na\tX", "\033[B\033[B\033[A\033[A", 10, 3, 3},
      {"newline after a split tab is addressable", "aaa\na\t\nZ",
       "\033[B\033[B", 10, 3, 6},
      {"empty logical row remains addressable", "abc\n\nxyz", "\033[B", 24, 3,
       4},
      {"empty row round trip", "abc\n\nxyz", "\033[B\033[A", 24, 3, 3}};
  size_t index;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int master, slave;
    struct winsize size = {24, 24, 0, 0};
    sl_config_t config;
    sl_t *sl;
    char keys[64], *line;
    fprintf(stderr, "%s visual rows: %s\n", native ? "native" : "readline",
            cases[index].name);
    size.ws_col = (unsigned short)cases[index].columns;
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    snprintf(keys, sizeof(keys), "%s\033s", cases[index].keys);
    line = read_keys(sl, master, native, cases[index].text, cases[index].start,
                     keys, NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    if (sl_cursor(sl) != cases[index].expected)
      fprintf(stderr, "cursor %lu, expected %lu\n",
              (unsigned long)sl_cursor(sl),
              (unsigned long)cases[index].expected);
    CHECK(sl_cursor(sl) == cases[index].expected);
    CHECK(!strcmp(sl_buffer(sl), cases[index].text));
    line = read_keys(sl, master, native, NULL, 0, "!\r", NULL);
    CHECK(line && !strncmp(line, cases[index].text, cases[index].expected));
    CHECK(line[cases[index].expected] == '!' &&
          !strcmp(line + cases[index].expected + 1,
                  cases[index].text + cases[index].expected));
    sl_free_string(sl, line);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return 0;
}

static int stash_lifetime(int native) {
  struct {
    sl_t *sl;
    int master, slave;
  } handles[2];
  struct winsize size = {24, 40, 0, 0};
  sl_config_t config;
  char *line;
  int index;
  for (index = 0; index < 2; ++index) {
    CHECK(openpty(&handles[index].master, &handles[index].slave, NULL, NULL,
                  &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = handles[index].slave;
    config.line_max_len = 16;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    handles[index].sl = sl_create_with_config(&config);
    CHECK(handles[index].sl &&
          sl_bind_key(handles[index].sl, SL_KEY_ALT_BASE + 's', suspend_key,
                      NULL) == SL_OK);
    if (native)
      CHECK(sl_output_stream_begin(handles[index].sl) == SL_OK);
  }
  line = read_keys(handles[0].sl, handles[0].master, native, "abcdefgh", 3,
                   "\033k\033s", NULL);
  CHECK(!line && !*sl_buffer(handles[0].sl));
  line = read_keys(handles[1].sl, handles[1].master, native, "second", 2,
                   "\033j\033s", NULL);
  CHECK(!line && !strcmp(sl_buffer(handles[1].sl), "second") &&
        sl_cursor(handles[1].sl) == 2);
  line = read_keys(handles[0].sl, handles[0].master, native, "12345678", 0,
                   "\033j\033s", NULL);
  CHECK(!line && !strcmp(sl_buffer(handles[0].sl), "12345678abcdefgh"));
  line = read_keys(handles[0].sl, handles[0].master, native, "123456789", 0,
                   "\033j\033s", NULL);
  CHECK(!line && !strcmp(sl_buffer(handles[0].sl), "123456789") &&
        sl_cursor(handles[0].sl) == 0);
  line = read_keys(handles[0].sl, handles[0].master, native, NULL, 0,
                   "\033n\033j\r", NULL);
  CHECK(line && !strcmp(line, "abcdefgh"));
  sl_free_string(handles[0].sl, line);
  line = read_keys(handles[0].sl, handles[0].master, native, NULL, 0, "temp\r",
                   NULL);
  CHECK(line && !strcmp(line, "temp"));
  sl_free_string(handles[0].sl, line);
  if (native)
    CHECK(sl_output_stream_end(handles[0].sl) == SL_OK &&
          sl_output_stream_begin(handles[0].sl) == SL_OK);
  line = read_keys(handles[0].sl, handles[0].master, native, NULL, 0,
                   "\033j\033s", NULL);
  CHECK(!line && !strcmp(sl_buffer(handles[0].sl), "abcdefgh"));
  for (index = 0; index < 2; ++index) {
    if (native)
      CHECK(sl_output_stream_end(handles[index].sl) == SL_OK);
    sl_destroy(handles[index].sl);
    close(handles[index].master);
    close(handles[index].slave);
  }
  return 0;
}

static int draft_shortcuts(int native) {
  static const struct {
    const char *text, *keys, *expected;
    size_t cursor, expected_cursor;
  } cases[] = {
      {"", "\033j\033n", "", 0, 0},
      {"café\n中文", "\033kprefix \001\033j", "prefix café\n中文", 1,
       sizeof("prefix café\n中文") - 1},
      {"abc", "\033k\033j\033j", "abcabc", 1, 6},
      {"abc", "\033k\033k\033j", "abc", 1, 3},
      {"   ", "\033k", "   ", 1, 1},
      {"\t \n\r\v\f", "\033K", "\t \n\r\v\f", 2, 2},
      {"abc", "\033k \033k\033j", " abc", 1, 4},
      {"abc", "\033k\033[200~ \t\n\033[201~\033K\033J", " \t\nabc", 1, 6},
      {"abc", "\033k \033k\037\033j", "abc", 1, 3},
      {" \tword\n ", "\033k\033j", " \tword\n ", 2, 8},
      {"abc", "\033ksecond\033k\033j", "second", 1, 6},
      {"café\n中文", "\033k\037", "café\n中文", 1, 1},
      {"abc", "\033kprefix\001\033j\037", "prefix", 1, 0},
      {"café\n中文", "\033n\037", "café\n中文", 1, 1},
      {"abc", "\033kother\033n\033j", "abc", 1, 3},
      {"kill", "\025stash\033kdiscard\033n\031\033j", "killstash", 4, 9},
      {"one", "\025two\025draft\033k\033j\001\031\033y", "onedraft", 3, 3},
      {"abc", "\033K\033J\033N\037", "abc", 1, 3},
      {"café\n中文", "\022match\033k\033j", "matching history", 1, 16},
      {"abc", "\022match\033n\037", "matching history", 1, 16}};
  size_t index;
  for (index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index) {
    int master, slave;
    struct winsize size = {24, 40, 0, 0};
    sl_config_t config;
    sl_t *sl;
    char keys[128], *line;
    fprintf(stderr, "draft shortcut: native=%d case=%lu\n", native,
            (unsigned long)index);
    CHECK(openpty(&master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    config.prompt_theme = SL_PROMPT_THEME_PLAIN;
    sl = sl_create_with_config(&config);
    CHECK(sl &&
          sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, NULL) == SL_OK);
    CHECK(sl_history_add(sl, "matching history") == SL_OK);
    if (native)
      CHECK(sl_output_stream_begin(sl) == SL_OK);
    snprintf(keys, sizeof(keys), "%s\033s", cases[index].keys);
    line = read_keys(sl, master, native, cases[index].text, cases[index].cursor,
                     keys, NULL);
    CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED);
    CHECK(!strcmp(sl_buffer(sl), cases[index].expected));
    CHECK(sl_cursor(sl) == cases[index].expected_cursor);
    if (native)
      CHECK(sl_output_stream_end(sl) == SL_OK);
    sl_destroy(sl);
    close(master);
    close(slave);
  }
  return stash_lifetime(native);
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "draft-shortcuts"))
    return draft_shortcuts(0) || draft_shortcuts(1);
  if (argc == 2 && !strcmp(argv[1], "visual-rows"))
    return visual_rows(0) || visual_rows(1);
  if (argc == 2 && !strcmp(argv[1], "cursor-groups"))
    return cursor_groups(0, 0) || cursor_groups(0, 1) || cursor_groups(1, 0) ||
           cursor_groups(1, 1) || cursor_groups(0, 2) || cursor_groups(1, 2);
  if (argc == 2 && !strcmp(argv[1], "suspend-groups"))
    return suspend_groups(0) || suspend_groups(1);
  if (argc == 2 && !strcmp(argv[1], "mutations"))
    return mutations(0) || mutations(1);
  if (argc == 2 && !strcmp(argv[1], "columns"))
    return columns();
  if (argc == 2 && !strcmp(argv[1], "transitions"))
    return transitions(0) || transitions(1);
  return editing(0) || editing(1) || held_edit();
}
