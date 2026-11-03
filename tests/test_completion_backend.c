#define _DEFAULT_SOURCE
#include "completion_fixture.h"
#include <pty.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      return 1;                                                                \
    }                                                                          \
  } while (0)

struct fixture {
  int master, calls, acknowledged, kind, expansions;
  sl_queue_request_id_t request;
  const char *text, *keys;
};
static int request(void *context, const sl_queue_request_t *r) {
  struct fixture *f = context;
  CHECK(r->operation == SL_QUEUE_BEGIN_EDIT && !f->request);
  f->request = r->request_id;
  return SL_OK;
}
static int host_tab(sl_t *sl, sl_key_t key, void *context,
                    sl_key_action_t *action) {
  struct fixture *f = context;
  (void)key;
  ++f->calls;
  *action = SL_KEY_ACTION_HANDLED;
  return sl_insert(sl, "#");
}
static int acknowledge(sl_t *sl, sl_key_t key, void *context,
                       sl_key_action_t *action) {
  struct fixture *f = context;
  sl_queue_result_t result;
  sl_queue_change_t change;
  sl_queue_entry_t entry;
  (void)key;
  CHECK(f->request && !f->calls && !*sl_buffer(sl));
  memset(&result, 0, sizeof(result));
  result.request_id = f->request;
  if (f->kind == 3)
    result.status = SL_QUEUE_REJECTED;
  else {
    memset(&entry, 0, sizeof(entry));
    entry.id = "held";
    entry.revision = "v2";
    entry.text = f->text;
    entry.held = entry.edit_held = 1;
    entry.edit_token = "owned";
    memset(&change, 0, sizeof(change));
    change.kind = SL_QUEUE_UPSERT;
    change.previous_revision = "q1";
    change.revision = "q2";
    change.entries = &entry;
    change.count = change.total_count = 1;
    change.request_id = f->request;
    CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
    result.item_id = entry.id;
    result.item_revision = entry.revision;
    result.edit_token = entry.edit_token;
  }
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  f->acknowledged = 1;
  *action = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}
static int suspend_key(sl_t *sl, sl_key_t key, void *context,
                       sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)context;
  *action = SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}
static int start(sl_t *sl, const sl_watch_event_t *event, void *context) {
  struct fixture *f = context;
  char byte;
  (void)sl;
  CHECK(read(event->fd, &byte, 1) == 1);
  CHECK(write(f->master, f->keys, strlen(f->keys)) == (ssize_t)strlen(f->keys));
  return SL_OK;
}
static int complete_root(sl_t *sl, char trigger, sl_completion_emit_t emit,
                         void *context, void *userdata) {
  (void)sl;
  (void)trigger;
  (void)userdata;
  return emit("verification", context);
}
static int complete_more(sl_t *sl, const char *completed,
                         sl_completion_emit_t emit, void *context,
                         void *userdata) {
  struct fixture *f = userdata;
  (void)sl;
  CHECK(f->acknowledged && !f->calls && !strcmp(completed, "verification"));
  ++f->expansions;
  return emit("verification detail", context);
}
static int run(int native, int kind) {
  struct fixture f;
  struct winsize size = {24, 80, 0, 0};
  int slave, wake[2];
  sl_config_t config;
  sl_queue_backend_t backend;
  sl_queue_entry_t entry;
  sl_queue_change_t change;
  static const struct completion_item completion = {"verification",
                                                    SL_COMPLETION_WORD};
  struct completion_fixture completions = {&completion, 1, 0};
  sl_watch_id_t watch;
  sl_prompt_queue_entry_info_t draft;
  sl_t *sl;
  const char *expected = kind == 4   ? "verification detail"
                         : kind == 2 ? "other#"
                         : kind == 3 ? "#"
                                     : "verification";
  memset(&f, 0, sizeof(f));
  f.kind = kind;
  f.text = kind == 4   ? "verification"
           : kind == 1 ? "v"
           : kind == 2 ? "other"
                       : "ver";
  f.keys = kind == 1 ? "\033eer\t\024\033s" : "\033e\t\024\033s";
  CHECK(!openpty(&f.master, &slave, NULL, NULL, &size) && !pipe(wake));
  sl_config_init(&config);
  memset(&backend, 0, sizeof(backend));
  backend.request = request;
  backend.capabilities = (1UL << (SL_QUEUE_RESOLVE + 1)) - 1;
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.queue_backend = &backend;
  config.queue_backend_context = &f;
  sl = sl_create_with_config(&config);
  CHECK(sl && sl_set_prompt_queue_profile(
                  sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
  memset(&entry, 0, sizeof(entry));
  entry.id = "held";
  entry.revision = "v1";
  entry.text = f.text;
  entry.held = 1;
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_SNAPSHOT;
  change.revision = "q1";
  change.entries = &entry;
  change.count = change.total_count = 1;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  if (kind == 4) {
    sl_completion_provider_t provider;
    memset(&provider, 0, sizeof(provider));
    provider.trigger = 'v';
    provider.scope = SL_COMPLETION_WORD;
    provider.callback = complete_root;
    provider.expand = complete_more;
    provider.userdata = &f;
    CHECK(sl_set_completion_providers(sl, &provider, 1) == SL_OK);
  } else
    CHECK(fixture_setup(sl, &completions) == SL_OK);
  CHECK(sl_bind_key(sl, SL_KEY_TAB, host_tab, &f) == SL_OK);
  CHECK(sl_bind_key(sl, SL_KEY_CTRL_T, acknowledge, &f) == SL_OK);
  CHECK(sl_bind_key(sl, SL_KEY_ALT_BASE + 's', suspend_key, &f) == SL_OK);
  CHECK(sl_watch_add(sl, wake[0], SL_WATCH_READ, start, &f, &watch) == SL_OK);
  CHECK(write(wake[1], "x", 1) == 1);
  CHECK(!(native ? sl_next_prompt(sl, "> ", NULL) : sl_readline(sl, "> ")));
  CHECK(sl_last_readline_status(sl) == SL_READLINE_SUSPENDED && f.acknowledged);
  CHECK(!strcmp(sl_buffer(sl), expected) &&
        f.calls == (kind == 2 || kind == 3));
  CHECK(f.expansions == (kind == 4));
  CHECK(sl_prompt_queue_draft_info(sl, &draft) == SL_OK);
  CHECK(kind == 3 ? !draft.id : draft.id && draft.held);
  sl_destroy(sl);
  close(f.master);
  close(slave);
  close(wake[0]);
  close(wake[1]);
  return 0;
}
int main(void) {
  int native, kind;
  for (native = 0; native <= 1; ++native)
    for (kind = 0; kind < 5; ++kind)
      CHECK(!run(native, kind));
  return 0;
}
