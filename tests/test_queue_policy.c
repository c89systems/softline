#define _DEFAULT_SOURCE
#include "pty_peer.h"
#include <pty.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "line %d: %s\n", __LINE__, #c);                          \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

struct fixture {
  sl_t *sl;
  struct pty_peer peer;
  int master, slave, native, history, calls, fail_policy, fail_history;
  int ready_write;
  const char *keys;
  sl_prompt_id_t edited;
  sl_prompt_queue_mode_t submitted_mode;
};

static int classify(sl_t *sl, const char *text, sl_prompt_queue_mode_t mode,
                    sl_prompt_queue_policy_t *policy, void *userdata) {
  struct fixture *f = userdata;
  sl_prompt_queue_entry_info_t info;
  f->submitted_mode = mode;
  ++f->calls;
  CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK);
  if (info.id)
    f->edited = info.id;
  if (f->fail_policy == 1)
    return SL_ERROR_INVALID;
  policy->enqueue = text[0] == '/' || text[0] == '!';
  policy->independent = !strcmp(text, "/status");
  policy->executable = strncmp(text, "/hold", 5) != 0 && text[0] != '!';
  if (f->fail_policy == 2)
    policy->executable = 2;
  return SL_OK;
}

static int history(const char *key, const char *text, void *userdata) {
  struct fixture *f = userdata;
  (void)key;
  (void)text;
  if (f->fail_history)
    return SL_ERROR_IO;
  ++f->history;
  return SL_OK;
}

static int action(sl_t *sl, sl_key_t key, void *userdata,
                  sl_key_action_t *out) {
  (void)sl;
  (void)userdata;
  *out = key == SL_KEY_ESCAPE ? SL_KEY_ACTION_CANCEL : SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}

static int wake(sl_t *sl, const sl_watch_event_t *event, void *userdata) {
  struct fixture *f = userdata;
  char byte;
  (void)sl;
  CHECK(read(event->fd, &byte, 1) == 1);
  CHECK(write(f->master, f->keys, strlen(f->keys)) == (ssize_t)strlen(f->keys));
  return SL_OK;
}

static int submit_key(sl_t *sl, sl_key_t key, void *userdata,
                      sl_key_action_t *out) {
  (void)sl;
  (void)key;
  (void)userdata;
  *out = SL_KEY_ACTION_SUBMIT;
  return SL_OK;
}

static int signal_ready(sl_t *sl, sl_key_t key, void *userdata,
                        sl_key_action_t *out) {
  struct fixture *f = userdata;
  (void)sl;
  (void)key;
  CHECK(write(f->ready_write, "x", 1) == 1);
  *out = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int boundary_ready(sl_t *sl, const sl_watch_event_t *event,
                          void *userdata) {
  sl_prompt_queue_entry_info_t info;
  char byte;
  (void)userdata;
  CHECK(read(event->fd, &byte, 1) == 1);
  CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK);
  CHECK(sl_prompt_queue_set_executable(sl, info.id, 1) == SL_OK);
  return SL_OK;
}

static void setup(struct fixture *f, int native, int capacity) {
  sl_config_t cfg;
  struct winsize size = {24, 80, 0, 0};
  memset(f, 0, sizeof(*f));
  CHECK(openpty(&f->master, &f->slave, NULL, NULL, &size) == 0);
  sl_config_init(&cfg);
  cfg.input_fd = cfg.output_fd = f->slave;
  cfg.prompt_theme = SL_PROMPT_THEME_PLAIN;
  cfg.prompt_queue = 1;
  cfg.prompt_queue_max_entries = capacity;
  f->sl = sl_create_with_config(&cfg);
  f->native = native;
  CHECK(f->sl);
  CHECK(sl_set_prompt_queue_profile(
            f->sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
  CHECK(sl_set_prompt_queue_policy(f->sl, classify, f) == SL_OK);
  CHECK(sl_bind_key(f->sl, SL_KEY_ALT_BASE + 's', action, f) == SL_OK);
  CHECK(sl_bind_key(f->sl, SL_KEY_ESCAPE, action, f) == SL_OK);
  CHECK(sl_history_set_backend(f->sl, "policy", NULL, history, f) == SL_OK);
  CHECK(sl_history_set_auto_add(f->sl, 1) == SL_OK);
  if (native) {
    CHECK(!pty_peer_start(&f->peer, f->master));
    CHECK(sl_output_stream_begin(f->sl) == SL_OK);
  }
}

static void teardown(struct fixture *f) {
  sl_destroy(f->sl);
  if (f->native)
    CHECK(!pty_peer_stop(&f->peer));
  close(f->master);
  close(f->slave);
}

/* A readable watch gates each key burst after raw input acquisition. No sleeps
 * or terminal-output timing inference; pending real keys precede dispatch. */
static char *keys(struct fixture *f, const char *input,
                  sl_prompt_source_t *source) {
  int ready[2];
  sl_watch_id_t id;
  char *text;
  CHECK(pipe(ready) == 0);
  f->keys = input;
  CHECK(sl_watch_add(f->sl, ready[0], SL_WATCH_READ, wake, f, &id) == SL_OK);
  CHECK(write(ready[1], "x", 1) == 1);
  text = sl_next_prompt(f->sl, "> ", source);
  CHECK(sl_watch_remove(f->sl, id) == SL_OK);
  close(ready[0]);
  close(ready[1]);
  return text;
}

static void suspend_keys(struct fixture *f, const char *input) {
  sl_prompt_source_t source;
  char *text = keys(f, input, &source);
  if (text)
    fprintf(stderr, "unexpected delivery: %s (burst %s)\n", text, input);
  CHECK(!text);
  CHECK(sl_last_readline_status(f->sl) == SL_READLINE_SUSPENDED);
  CHECK(source == SL_PROMPT_SOURCE_NONE);
}

static sl_prompt_queue_entry_info_t entry(struct fixture *f, size_t index) {
  sl_prompt_queue_entry_info_t info;
  CHECK(sl_prompt_queue_info(f->sl, index, &info) == SL_OK);
  return info;
}

static void expect(struct fixture *f, const char *input, const char *expected,
                   sl_prompt_source_t expected_source, sl_prompt_id_t id) {
  sl_prompt_source_t source;
  char *text = keys(f, input, &source);
  CHECK(text && !strcmp(text, expected));
  CHECK(source == expected_source);
  CHECK(!id || sl_last_prompt_id(f->sl) == id);
  sl_free_string(f->sl, text);
}

static void admission(int native) {
  struct fixture f;
  sl_prompt_id_t id;
  setup(&f, native, 8);
  expect(&f, "  /command  \r\033eX \r", "/commandX", SL_PROMPT_SOURCE_QUEUED,
         0);
  id = sl_last_prompt_id(f.sl);
  CHECK(id && id == f.edited && f.calls == 2 && f.history == 1);
  CHECK(sl_prompt_queue_count(f.sl) == 0);
  CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
  suspend_keys(&f, " ordinary \r /hold first \r /hold second \033\r\033s");
  CHECK(entry(&f, 0).policy.enqueue == 0);
  CHECK(entry(&f, 1).policy.enqueue && !entry(&f, 1).policy.executable);
  CHECK(entry(&f, 1).mode == SL_PROMPT_QUEUE_MODE_QUEUED);
  CHECK(entry(&f, 2).mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(!entry(&f, 2).policy.executable);
  teardown(&f);

  setup(&f, native, 8);
  CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "  accepted  ", 0, &id) == SL_OK);
  expect(&f, "  \n\033\r", "  accepted  ", SL_PROMPT_SOURCE_PROMOTED, id);
  CHECK(!f.calls && !f.history);
  teardown(&f);
}

static void boundaries(int native) {
  struct fixture f;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t id;
  char *text = NULL;
  int seam;
  setup(&f, native, 8);
  CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
  suspend_keys(&f, "/hold review\r\033s");
  info = entry(&f, 0);
  id = info.id;
  CHECK(!info.held && !info.eligible && !info.policy.executable);
  for (seam = 0; seam < 3; ++seam) {
    CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
    suspend_keys(&f, "\033s");
    CHECK(entry(&f, 0).id == id && !entry(&f, 0).eligible);
  }
  suspend_keys(&f, "\033\r\033s");
  CHECK(entry(&f, 0).mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(sl_prompt_queue_take(f.sl, 0, &text) == SL_ERROR_INVALID && !text);
  expect(&f, "/status\r", "/status", SL_PROMPT_SOURCE_QUEUED, 0);
  CHECK(sl_prompt_queue_count(f.sl) == 1 && entry(&f, 0).id == id);
  suspend_keys(&f, "\033e\001\013/hold edited\r\033s");
  CHECK(entry(&f, 0).id == id && !entry(&f, 0).eligible);
  CHECK(entry(&f, 0).mode == SL_PROMPT_QUEUE_MODE_STEER);
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 1) == SL_OK);
  expect(&f, "", "/hold edited", SL_PROMPT_SOURCE_PROMOTED, id);
  CHECK(sl_prompt_queue_count(f.sl) == 0);
  teardown(&f);
}

static void stopped(int native) {
  struct fixture f;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t old, passive;
  sl_prompt_source_t source;
  int stop;
  char *text = NULL;
  setup(&f, native, 8);
  CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "old user turn", 0, &old) == SL_OK);
  CHECK(!keys(&f, "\033", &source));
  CHECK(sl_last_readline_status(f.sl) == SL_READLINE_CANCELLED);
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  CHECK(entry(&f, 0).stopped && !entry(&f, 0).eligible);
  CHECK(sl_prompt_queue_append(f.sl, "passive", 0, &passive) == SL_OK);
  CHECK(sl_set_status_busy(f.sl, 0) == SL_OK);
  CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_AUTO) ==
        SL_OK);
  expect(&f, "/status\r", "/status", SL_PROMPT_SOURCE_QUEUED, 0);
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  CHECK(entry(&f, 0).id == old && entry(&f, 0).stopped);
  CHECK(entry(&f, 1).id == passive && entry(&f, 1).stopped);
  expect(&f, "/goal resume\r", "/goal resume", SL_PROMPT_SOURCE_QUEUED, 0);
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  CHECK(sl_prompt_queue_take(f.sl, 0, &text) == SL_ERROR_INVALID && !text);
  suspend_keys(&f, "\033e!\r\033s");
  info = entry(&f, 1);
  CHECK(info.id == passive && info.stopped);
  CHECK(sl_prompt_queue_resume(f.sl) == SL_OK);
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && !stop);
  expect(&f, "", "old user turn", SL_PROMPT_SOURCE_QUEUED, old);
  expect(&f, "", "passive!", SL_PROMPT_SOURCE_QUEUED, passive);
  teardown(&f);
}

static void capacity(int native) {
  struct fixture f;
  sl_prompt_id_t id, held, overflow;
  sl_prompt_queue_entry_info_t info;
  char *text = NULL;
  setup(&f, native, 2);
  suspend_keys(&f, "/hold original\r\033eX\033s");
  CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK);
  id = info.id;
  CHECK(id && sl_prompt_queue_count(f.sl) == 0);
  CHECK(sl_inject_queued_prompt(f.sl, "approval", &held) == SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "overflow", 0, &overflow) ==
            SL_ERROR_FULL &&
        !overflow);
  suspend_keys(&f, "\r\033s");
  CHECK(entry(&f, 0).id == id && entry(&f, 1).id == held);
  CHECK(entry(&f, 1).held && !entry(&f, 1).eligible);
  CHECK(sl_prompt_queue_set_executable(f.sl, held, 0) == SL_OK);
  CHECK(sl_prompt_queue_approve(f.sl, held) == SL_OK);
  CHECK(!entry(&f, 1).held && !entry(&f, 1).policy.executable);
  CHECK(sl_prompt_queue_take(f.sl, 1, &text) == SL_ERROR_INVALID && !text);
  /* Full fresh admission preserves the complete draft. */
  suspend_keys(&f, "/new\r\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "/new"));
  CHECK(sl_prompt_queue_count(f.sl) == 2);
  CHECK(sl_prompt_queue_reject(f.sl, id) == SL_OK);
  CHECK(sl_prompt_queue_reject(f.sl, held) == SL_OK);
  expect(&f, "\r", "/new", SL_PROMPT_SOURCE_QUEUED, 0);
  teardown(&f);
}

static void shell_and_order(int native) {
  struct fixture f;
  sl_prompt_id_t shell, normal;
  char *text = NULL;
  setup(&f, native, 8);
  CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
  suspend_keys(&f, "!shell\rnormal\r\033s");
  shell = entry(&f, 0).id;
  normal = entry(&f, 1).id;
  CHECK(sl_set_status_busy(f.sl, 0) == SL_OK);
  CHECK(!entry(&f, 1).eligible);
  CHECK(sl_prompt_queue_take(f.sl, 1, &text) == SL_ERROR_INVALID && !text);
  suspend_keys(&f, "\033eX\r\033s");
  CHECK(entry(&f, 1).id == normal);
  CHECK(sl_prompt_queue_set_executable(f.sl, shell, 1) == SL_OK);
  expect(&f, "", "!shell", SL_PROMPT_SOURCE_QUEUED, shell);
  expect(&f, "", "normalX", SL_PROMPT_SOURCE_QUEUED, normal);
  CHECK(sl_get_prompt_queue_stopped(NULL, NULL) == SL_ERROR_INVALID);
  CHECK(sl_set_prompt_queue_policy(NULL, NULL, NULL) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_set_executable(NULL, 1, 0) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_resume(NULL) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_set_executable(f.sl, 0, 1) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_set_executable(f.sl, normal, 2) == SL_ERROR_INVALID);
  CHECK(sl_prompt_queue_set_executable(f.sl, normal, 1) == SL_ERROR_INVALID);
  f.fail_policy = 1;
  suspend_keys(&f, "  /bad  \r\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "  /bad  "));
  CHECK(sl_prompt_queue_count(f.sl) == 0);
  f.fail_policy = 0;
  expect(&f, "\r", "/bad", SL_PROMPT_SOURCE_QUEUED, 0);
  teardown(&f);
}

static void transitions(int native) {
  struct fixture f;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t id, held;
  int calls, ready[2];
  sl_watch_id_t watch;
  setup(&f, native, 8);
  /* Custom bindings, including overriding Enter, cannot bypass admission. */
  CHECK(sl_bind_key(f.sl, SL_KEY_ENTER, submit_key, NULL) == SL_OK);
  expect(&f, "/command\r", "/command", SL_PROMPT_SOURCE_QUEUED, 0);
  CHECK(f.calls == 1);
  CHECK(sl_bind_key(f.sl, SL_KEY_ENTER, NULL, NULL) == SL_OK);
  /* Invalid classification and history backend failures both preserve drafts.
   */
  f.fail_policy = 2;
  suspend_keys(&f, "/invalid\r\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "/invalid") && !sl_prompt_queue_count(f.sl));
  f.fail_policy = 0;
  f.fail_history = 1;
  calls = f.history;
  suspend_keys(&f, "\r\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "/invalid") && !sl_prompt_queue_count(f.sl));
  CHECK(f.history == calls);
  f.fail_history = 0;
  expect(&f, "\r", "/invalid", SL_PROMPT_SOURCE_QUEUED, 0);
  /* Paste newlines remain literal; search Enter selects without admission. */
  calls = f.calls;
  suspend_keys(&f, "\033[200~/hold pasted\nsecond line\033[201~\033s");
  CHECK(f.calls == calls && !sl_prompt_queue_count(f.sl));
  CHECK(!strcmp(sl_buffer(f.sl), "/hold pasted\nsecond line"));
  suspend_keys(&f, "\r\033s");
  id = entry(&f, 0).id;
  CHECK(sl_history_add(f.sl, "/selected") == SL_OK);
  calls = f.calls;
  suspend_keys(&f, "\022selected\r\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "/selected") && f.calls == calls);
  suspend_keys(&f, "\r\033s");
  CHECK(sl_prompt_queue_count(f.sl) == 2 && !entry(&f, 1).eligible);
  CHECK(sl_prompt_queue_reject(f.sl, id) == SL_OK);
  expect(&f, "", "/selected", SL_PROMPT_SOURCE_QUEUED, 0);
  teardown(&f);
  /* Empty submission removes its entry. Rejection of an active held edit
   * cannot release an execution-blocked entry or disturb another draft. */
  setup(&f, native, 8);
  suspend_keys(&f, "/hold original\r\033e\001\013\r\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "") && sl_prompt_queue_count(f.sl) == 0);
  suspend_keys(&f, "/hold retained\r\033s");
  info = entry(&f, 0);
  CHECK(!strcmp(sl_buffer(f.sl), "") && !info.policy.executable);
  CHECK(sl_inject_queued_prompt(f.sl, "approval", &held) == SL_OK);
  suspend_keys(&f, "\033e\033s");
  CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK && info.id == held);
  CHECK(sl_prompt_queue_set_executable(f.sl, held, 0) == SL_OK);
  CHECK(sl_set_auto_approval(f.sl, 1) == SL_OK);
  CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK && !info.held &&
        !info.policy.executable);
  CHECK(sl_prompt_queue_reject(f.sl, held) == SL_OK);
  CHECK(!strcmp(sl_buffer(f.sl), "") && sl_prompt_queue_count(f.sl) == 1);
  teardown(&f);
  /* A readiness event wakes delivery directly through the owner reactor. */
  setup(&f, native, 8);
  CHECK(pipe(ready) == 0);
  f.ready_write = ready[1];
  CHECK(sl_watch_add(f.sl, ready[0], SL_WATCH_READ, boundary_ready, &f,
                     &watch) == SL_OK);
  CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'r', signal_ready, &f) == SL_OK);
  expect(&f, "/hold ready\r\033r", "/hold ready", SL_PROMPT_SOURCE_QUEUED, 0);
  CHECK(sl_watch_remove(f.sl, watch) == SL_OK);
  close(ready[0]);
  close(ready[1]);
  teardown(&f);
}

static void policy_edges(int native) {
  struct fixture f;
  sl_prompt_source_t source;
  sl_prompt_id_t id;
  int stop;
  setup(&f, native, 8);
  CHECK(sl_set_prompt_queue_profile(f.sl, SL_PROMPT_QUEUE_PROFILE_DEFAULT) ==
        SL_OK);
  CHECK(!keys(&f, "/hold default\r\033", &source));
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  id = entry(&f, 0).id;
  CHECK(entry(&f, 0).stopped);
  expect(&f, "/status\r", "/status", SL_PROMPT_SOURCE_QUEUED, 0);
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 1) == SL_OK);
  CHECK(entry(&f, 0).stopped && !entry(&f, 0).eligible);
  CHECK(sl_prompt_queue_resume(f.sl) == SL_OK);
  expect(&f, "", "/hold default", SL_PROMPT_SOURCE_QUEUED, id);
  teardown(&f);
  setup(&f, native, 8);
  suspend_keys(&f, "/hold original\r\033s");
  id = entry(&f, 0).id;
  CHECK(!keys(&f, "\033", &source));
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  suspend_keys(&f, "\033eX\033\r\033s");
  CHECK(entry(&f, 0).id == id && !entry(&f, 0).stopped);
  CHECK(!entry(&f, 0).policy.executable);
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 1) == SL_OK);
  expect(&f, "", "/hold originalX", SL_PROMPT_SOURCE_PROMOTED, id);
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  teardown(&f);
  setup(&f, native, 8);
  CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  suspend_keys(&f, "/command\r\033s");
  id = entry(&f, 0).id;
  CHECK(entry(&f, 0).eligible);
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 0) == SL_OK);
  CHECK(sl_prompt_queue_resume(f.sl) == SL_OK);
  CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_AUTO) ==
        SL_OK);
  CHECK(!entry(&f, 0).eligible);
  suspend_keys(&f, "\033s");
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 1) == SL_OK);
  expect(&f, "", "/command", SL_PROMPT_SOURCE_QUEUED, id);
  teardown(&f);
  setup(&f, native, 8);
  CHECK(sl_set_prompt_queue_profile(f.sl, SL_PROMPT_QUEUE_PROFILE_DEFAULT) ==
        SL_OK);
  suspend_keys(&f, "/passive\t\033s");
  CHECK(f.calls == 0 && !entry(&f, 0).policy.enqueue);
  CHECK(sl_prompt_queue_clear(f.sl) == SL_OK);
  suspend_keys(&f, "/hold retained\r\033s");
  id = entry(&f, 0).id;
  CHECK(sl_set_prompt_queue_policy(f.sl, NULL, NULL) == SL_OK);
  CHECK(!keys(&f, "\033", &source));
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  CHECK(entry(&f, 0).id == id && entry(&f, 0).stopped);
  teardown(&f);
  /* Legacy steering must still be explicitly deliverable after cancellation;
   * promoting it never waives execution readiness or releases the backlog. */
  setup(&f, native, 8);
  CHECK(sl_set_prompt_queue_policy(f.sl, NULL, NULL) == SL_OK);
  CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "old", 0, &id) == SL_OK);
  CHECK(!keys(&f, "\033", &source));
  suspend_keys(&f, "steer\033\r\033s");
  CHECK(entry(&f, 1).eligible && !entry(&f, 1).stopped);
  CHECK(entry(&f, 0).stopped);
  CHECK(sl_prompt_queue_reject(f.sl, entry(&f, 1).id) == SL_OK);
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 0) == SL_OK);
  suspend_keys(&f, "\033\r\033s");
  CHECK(!entry(&f, 0).stopped && !entry(&f, 0).eligible);
  CHECK(sl_get_prompt_queue_stopped(f.sl, &stop) == SL_OK && stop);
  CHECK(sl_prompt_queue_set_executable(f.sl, id, 1) == SL_OK);
  CHECK(entry(&f, 0).eligible);
  teardown(&f);
}

static void history_once(int native) {
  struct fixture f;
  sl_prompt_id_t id;
  setup(&f, native, 8);
  CHECK(sl_inject_queued_prompt(f.sl, "injected", &id) == SL_OK);
  suspend_keys(&f, "\033eX\r\033s");
  CHECK(f.history == 1 && entry(&f, 0).id == id && entry(&f, 0).held);
  suspend_keys(&f, "\033eY\r\033s");
  CHECK(f.history == 1 && entry(&f, 0).id == id);
  teardown(&f);
  setup(&f, native, 8);
  CHECK(sl_history_set_auto_add(f.sl, 0) == SL_OK);
  suspend_keys(&f, "/hold late\r\033s");
  CHECK(f.history == 0);
  CHECK(sl_history_set_auto_add(f.sl, 1) == SL_OK);
  suspend_keys(&f, "\033eX\r\033s");
  CHECK(f.history == 1);
  suspend_keys(&f, "\033eY\r\033s");
  CHECK(f.history == 1);
  teardown(&f);
}

static void steering_eligibility(int native) {
  struct fixture f;
  sl_prompt_id_t turn, steer, held;
  char *text = NULL;
  setup(&f, native, 8);
  CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
        SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "ordinary turn", 0, &turn) == SL_OK);
  CHECK(sl_prompt_queue_set_executable(f.sl, turn, 0) == SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "steering", 0, &steer) == SL_OK);
  CHECK(sl_prompt_queue_set_mode(f.sl, 1, SL_PROMPT_QUEUE_MODE_STEER) == SL_OK);
  CHECK(entry(&f, 1).eligible);
  CHECK(sl_prompt_queue_take(f.sl, 1, &text) == SL_OK &&
        !strcmp(text, "steering"));
  sl_free_string(f.sl, text);
  suspend_keys(&f, "/hold whole turn\r\033s");
  CHECK(sl_prompt_queue_append(f.sl, "later steering", 0, &steer) == SL_OK);
  CHECK(sl_prompt_queue_set_mode(f.sl, 2, SL_PROMPT_QUEUE_MODE_STEER) == SL_OK);
  CHECK(!entry(&f, 2).eligible);
  CHECK(sl_prompt_queue_take(f.sl, 2, &text) == SL_ERROR_INVALID && !text);
  CHECK(sl_prompt_queue_clear(f.sl) == SL_OK);
  CHECK(sl_inject_queued_prompt(f.sl, "held suggestion", &held) == SL_OK);
  CHECK(sl_prompt_queue_append(f.sl, "ordinary user turn", 0, &turn) == SL_OK);
  CHECK(entry(&f, 1).eligible);
  CHECK(sl_prompt_queue_take(f.sl, 1, &text) == SL_OK);
  sl_free_string(f.sl, text);
  CHECK(entry(&f, 0).id == held && entry(&f, 0).held);
  teardown(&f);
}

struct callback_submission {
  struct fixture *fixture;
  const char *text;
  int search, submitted;
};

static void submit_callback(sl_t *sl, void *userdata) {
  struct callback_submission *submission = userdata;
  if (submission->submitted ||
      (submission->search && strcmp(sl_buffer(sl), submission->text)))
    return;
  if (!submission->search)
    CHECK(sl_set_buffer(sl, submission->text) == SL_OK);
  submission->submitted = 1;
  CHECK(sl_submit(sl) == SL_OK);
  CHECK(write(submission->fixture->master, "\033s", 2) == 2);
}

static int submit_watch(sl_t *sl, const sl_watch_event_t *event,
                        void *userdata) {
  char byte;
  CHECK(read(event->fd, &byte, 1) == 1);
  submit_callback(sl, userdata);
  return SL_OK;
}

static void callback_submissions(int native) {
  struct fixture f;
  struct callback_submission submission;
  sl_watch_id_t watch = 0;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_source_t source;
  int profile, search, watched, executable, ready[2];
  for (profile = 0; profile < 2; ++profile)
    for (search = 0; search < 2; ++search)
      for (watched = 0; watched < 2; ++watched)
        for (executable = 0; executable < 2; ++executable) {
          setup(&f, native, 8);
          CHECK(sl_set_prompt_queue_profile(
                    f.sl, profile ? SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS
                                  : SL_PROMPT_QUEUE_PROFILE_DEFAULT) == SL_OK);
          CHECK(sl_set_prompt_queue_delivery(
                    f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
          memset(&submission, 0, sizeof(submission));
          submission.fixture = &f;
          submission.search = search;
          submission.text = executable ? "/command callback" : "/hold callback";
          if (search)
            CHECK(sl_history_add(f.sl, submission.text) == SL_OK);
          if (watched) {
            CHECK(pipe(ready) == 0);
            f.ready_write = ready[1];
            CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'r', signal_ready, &f) ==
                  SL_OK);
            CHECK(sl_watch_add(f.sl, ready[0], SL_WATCH_READ, submit_watch,
                               &submission, &watch) == SL_OK);
          } else
            CHECK(sl_set_idle_callback(f.sl, submit_callback, &submission) ==
                  SL_OK);
          CHECK(!keys(&f,
                      search ? (watched ? "\022\033r" : "\022")
                             : (watched ? "\033r" : ""),
                      &source));
          CHECK(sl_last_readline_status(f.sl) == SL_READLINE_SUSPENDED);
          CHECK(source == SL_PROMPT_SOURCE_NONE && submission.submitted);
          CHECK(f.calls == 1 && f.history == 1);
          CHECK(f.submitted_mode == SL_PROMPT_QUEUE_MODE_QUEUED);
          CHECK(sl_prompt_queue_count(f.sl) == 1);
          info = entry(&f, 0);
          CHECK(info.id && info.mode == SL_PROMPT_QUEUE_MODE_QUEUED);
          CHECK(info.policy.enqueue && info.policy.executable == executable);
          CHECK(info.eligible == executable && !info.held && !info.stopped);
          CHECK(!strcmp(sl_buffer(f.sl), ""));
          if (watched) {
            CHECK(sl_watch_remove(f.sl, watch) == SL_OK);
            close(ready[0]);
            close(ready[1]);
          } else
            CHECK(sl_set_idle_callback(f.sl, NULL, NULL) == SL_OK);
          CHECK(sl_prompt_queue_set_executable(f.sl, info.id, 1) == SL_OK);
          CHECK(sl_set_prompt_queue_delivery(
                    f.sl, SL_PROMPT_QUEUE_DELIVERY_AUTO) == SL_OK);
          expect(&f, "", submission.text, SL_PROMPT_SOURCE_QUEUED, info.id);
          CHECK(f.calls == 1 && f.history == 1);
          teardown(&f);
        }
}

struct edit_readiness {
  sl_prompt_id_t id;
  int executable, requeue;
};

static int update_edited_readiness(sl_t *sl, sl_key_t key, void *userdata,
                                   sl_key_action_t *out) {
  struct edit_readiness *update = userdata;
  sl_prompt_queue_entry_info_t info;
  (void)key;
  CHECK(sl_prompt_queue_draft_info(sl, &info) == SL_OK &&
        info.id == update->id);
  CHECK(sl_prompt_queue_set_executable(sl, info.id, update->executable) ==
        SL_OK);
  if (update->requeue)
    CHECK(sl_prompt_queue_enqueue_draft(sl) == SL_OK);
  *out = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

/* Dispatch gets the first idle iteration. Suspend only if work is stranded or
 * intentionally blocked, so a lost wakeup fails without a timing-based sleep.
 */
static void suspend_requeued(sl_t *sl, void *userdata) {
  struct fixture *f = userdata;
  if (sl_prompt_queue_count(sl))
    CHECK(write(f->master, "\033s", 2) == 2);
}

static void edit_readiness(int native) {
  struct fixture f;
  struct edit_readiness update;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_source_t source;
  int api, state;
  for (api = 0; api < 2; ++api)
    for (state = 0; state < 5; ++state) {
      setup(&f, native, 8);
      CHECK(sl_set_prompt_queue_profile(
                f.sl, SL_PROMPT_QUEUE_PROFILE_DEFAULT) == SL_OK);
      suspend_keys(&f, "/hold requeue\r\033s");
      update.id = entry(&f, 0).id;
      update.executable = state != 2;
      update.requeue = api;
      if (state == 1)
        CHECK(sl_prompt_queue_set_executable(f.sl, update.id, 1) == SL_OK);
      if (state == 3)
        CHECK(sl_set_prompt_queue_delivery(
                  f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
      if (state == 4)
        CHECK(!keys(&f, "\033", &source));
      CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'r', update_edited_readiness,
                        &update) == SL_OK);
      CHECK(sl_set_idle_callback(f.sl, suspend_requeued, &f) == SL_OK);
      if (state < 2) {
        expect(&f, api ? "\033e\033r" : "\033e\033r\t", "/hold requeue",
               SL_PROMPT_SOURCE_QUEUED, update.id);
        CHECK(sl_prompt_queue_count(f.sl) == 0);
      } else {
        suspend_keys(&f, api ? "\033e\033r" : "\033e\033r\t");
        CHECK(sl_prompt_queue_count(f.sl) == 1);
        info = entry(&f, 0);
        CHECK(info.id == update.id &&
              info.policy.executable == update.executable);
        CHECK(info.stopped == (state == 4));
        CHECK(info.eligible == (state == 3));
      }
      CHECK(f.calls == 1 && f.history == 1);
      teardown(&f);
    }
}

static void edited_barriers(int native) {
  struct fixture f;
  struct callback_submission submission;
  sl_prompt_id_t older, newer;
  char *text = NULL;
  int route, barrier;
  for (route = 0; route < 3; ++route)
    for (barrier = 0; barrier < 3; ++barrier) {
      setup(&f, native, 8);
      CHECK(sl_set_prompt_queue_profile(
                f.sl, SL_PROMPT_QUEUE_PROFILE_DEFAULT) == SL_OK);
      if (barrier == 1)
        suspend_keys(&f, "/hold older\r\033s");
      CHECK(sl_set_prompt_queue_policy(f.sl, NULL, NULL) == SL_OK);
      CHECK(sl_set_prompt_queue_delivery(
                f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
      if (barrier == 1)
        older = entry(&f, 0).id;
      else {
        CHECK(sl_prompt_queue_append(f.sl, "older", 0, &older) == SL_OK);
        if (barrier == 0)
          CHECK(sl_prompt_queue_set_executable(f.sl, older, 0) == SL_OK);
      }
      CHECK(sl_prompt_queue_append(f.sl, "newer", 0, &newer) == SL_OK);
      CHECK(entry(&f, 1).eligible == (barrier == 2));
      if (route == 1)
        CHECK(sl_bind_key(f.sl, SL_KEY_ENTER, submit_key, NULL) == SL_OK);
      memset(&submission, 0, sizeof(submission));
      submission.fixture = &f;
      submission.text = "newerX";
      submission.search = 1;
      if (route == 2)
        CHECK(sl_set_idle_callback(f.sl, submit_callback, &submission) ==
              SL_OK);
      if (barrier == 2) {
        expect(&f, route == 2 ? "\033eX" : "\033eX\r", "newerX",
               SL_PROMPT_SOURCE_DIRECT, newer);
        CHECK(entry(&f, 0).id == older && !entry(&f, 0).held);
      } else {
        CHECK(sl_prompt_queue_take(f.sl, 1, &text) == SL_ERROR_INVALID &&
              !text);
        suspend_keys(&f, route == 2 ? "\033eX" : "\033eX\r\033s");
        CHECK(sl_prompt_queue_count(f.sl) == 2);
        CHECK(entry(&f, 0).id == older && entry(&f, 1).id == newer);
        CHECK(!entry(&f, 1).eligible);
        CHECK(sl_prompt_queue_take(f.sl, 1, &text) == SL_ERROR_INVALID &&
              !text);
        CHECK(sl_prompt_queue_set_executable(f.sl, older, 1) == SL_OK);
        CHECK(sl_prompt_queue_take(f.sl, 0, &text) == SL_OK);
        CHECK(!strcmp(text, barrier == 1 ? "/hold older" : "older"));
        sl_free_string(f.sl, text);
        CHECK(sl_prompt_queue_take(f.sl, 0, &text) == SL_OK);
        CHECK(!strcmp(text, "newerX") && sl_last_prompt_id(f.sl) == newer);
        sl_free_string(f.sl, text);
      }
      CHECK(f.history == (barrier == 1 ? 2 : 1));
      teardown(&f);
    }
}

struct policy_removal {
  struct fixture *fixture;
  int removed, waiting_for_watch;
};

static void remove_policy(sl_t *sl, void *userdata) {
  struct policy_removal *removal = userdata;
  if (removal->waiting_for_watch)
    return;
  if (!removal->removed) {
    CHECK(sl_set_prompt_queue_policy(sl, NULL, NULL) == SL_OK);
    removal->removed = 1;
  } else
    CHECK(write(removal->fixture->master, "\033s", 2) == 2);
}

static int remove_policy_watch(sl_t *sl, const sl_watch_event_t *event,
                               void *userdata) {
  struct policy_removal *removal = userdata;
  char byte;
  CHECK(read(event->fd, &byte, 1) == 1);
  CHECK(removal->waiting_for_watch);
  removal->waiting_for_watch = 0;
  remove_policy(sl, userdata);
  return SL_OK;
}

static void policy_wakeup(int native) {
  struct fixture f;
  struct policy_removal removal;
  sl_prompt_id_t steer, ordinary;
  sl_prompt_source_t source;
  sl_watch_id_t watch = 0;
  int watched, blocked, ready[2];
  for (watched = 0; watched < 2; ++watched)
    for (blocked = 0; blocked < 4; ++blocked) {
      setup(&f, native, 8);
      CHECK(sl_set_prompt_queue_profile(
                f.sl, SL_PROMPT_QUEUE_PROFILE_DEFAULT) == SL_OK);
      CHECK(sl_prompt_queue_append(f.sl, "steer", 0, &steer) == SL_OK);
      CHECK(sl_prompt_queue_set_mode(f.sl, 0, SL_PROMPT_QUEUE_MODE_STEER) ==
            SL_OK);
      CHECK(sl_prompt_queue_append(f.sl, "ordinary", blocked == 3, &ordinary) ==
            SL_OK);
      CHECK(!entry(&f, 1).eligible);
      if (blocked == 1)
        CHECK(sl_set_prompt_queue_delivery(
                  f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
      if (blocked == 2)
        CHECK(!keys(&f, "\033", &source));
      removal.fixture = &f;
      removal.removed = 0;
      removal.waiting_for_watch = watched;
      CHECK(sl_set_idle_callback(f.sl, remove_policy, &removal) == SL_OK);
      if (watched) {
        CHECK(pipe(ready) == 0);
        f.ready_write = ready[1];
        CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'r', signal_ready, &f) ==
              SL_OK);
        CHECK(sl_watch_add(f.sl, ready[0], SL_WATCH_READ, remove_policy_watch,
                           &removal, &watch) == SL_OK);
      }
      if (!blocked) {
        expect(&f, watched ? "\033r" : "", "ordinary", SL_PROMPT_SOURCE_QUEUED,
               ordinary);
        CHECK(sl_prompt_queue_count(f.sl) == 1 && entry(&f, 0).id == steer);
      } else {
        suspend_keys(&f, watched ? "\033r" : "");
        CHECK(sl_prompt_queue_count(f.sl) == 2);
        CHECK(entry(&f, 0).id == steer && entry(&f, 1).id == ordinary);
        CHECK(entry(&f, 1).eligible == (blocked == 1));
        CHECK(entry(&f, 1).stopped == (blocked == 2));
        CHECK(entry(&f, 1).held == (blocked == 3));
      }
      CHECK(removal.removed && !removal.waiting_for_watch && f.calls == 0 &&
            f.history == 0);
      if (watched) {
        CHECK(sl_watch_remove(f.sl, watch) == SL_OK);
        close(ready[0]);
        close(ready[1]);
      }
      teardown(&f);
    }
}

static int empty_edit_key(sl_t *sl, sl_key_t key, void *userdata,
                          sl_key_action_t *out) {
  int *requeue = userdata;
  (void)key;
  CHECK(((*requeue) ? sl_prompt_queue_enqueue_draft(sl) : sl_submit(sl)) ==
        SL_OK);
  *out = SL_KEY_ACTION_HANDLED;
  return SL_OK;
}

static int discard_override(sl_t *sl, sl_key_t key, void *userdata,
                            sl_key_action_t *out) {
  (void)sl;
  (void)key;
  *out = *(sl_key_action_t *)userdata;
  return SL_OK;
}

static void discard_edges(int native) {
  struct fixture f;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t removed, waiting, ordinary;
  sl_key_action_t action = SL_KEY_ACTION_HANDLED;
  char *text = NULL;
  int scenario;
  for (scenario = 0; scenario < 4; ++scenario) {
    setup(&f, native, 4);
    CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
    CHECK(sl_history_add(f.sl, "matching history") == SL_OK);
    CHECK(sl_inject_queued_prompt(f.sl, "original", &removed) == SL_OK);
    CHECK(sl_inject_steer_prompt(f.sl, "waiting", &waiting) == SL_OK);
    CHECK(sl_prompt_queue_append(f.sl, "ordinary", 0, &ordinary) == SL_OK);
    suspend_keys(&f, "\033e-edited\033s");
    CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK &&
          info.id == removed);
    if (scenario == 0) {
      CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'n', discard_override,
                        &action) == SL_OK);
      suspend_keys(&f, "\033n\033s");
      CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK &&
            info.id == removed);
      CHECK(!strcmp(sl_buffer(f.sl), "original-edited"));
      action = SL_KEY_ACTION_PASS;
    } else if (scenario == 1) {
      suspend_keys(&f, "\033[200~\033n\033[201~\033s");
      CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK &&
            info.id == removed);
    } else if (scenario == 2) {
      CHECK(sl_set_auto_approval(f.sl, 1) == SL_OK);
      CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK && !info.held);
    }
    suspend_keys(&f, scenario == 3   ? "\022match\033n\007\037\033s"
                     : scenario == 0 ? "\030\033n\037\033s"
                                     : "\033n\037\033s");
    CHECK(!sl_history_search_active(f.sl));
    CHECK(scenario == 3 ? !strcmp(sl_buffer(f.sl), "matching history")
                        : !strncmp(sl_buffer(f.sl), "original-edited", 15));
    CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK && !info.id);
    CHECK(sl_prompt_queue_count(f.sl) == 2);
    CHECK(entry(&f, 0).id == waiting && entry(&f, 1).id == ordinary);
    CHECK(entry(&f, 0).held == (scenario != 2));
    CHECK(sl_prompt_queue_peek(f.sl, 0, &text) == SL_OK &&
          !strcmp(text, "waiting"));
    sl_free_string(f.sl, text);
    CHECK(sl_prompt_queue_reject(f.sl, removed) == SL_ERROR_INVALID);
    CHECK(f.calls == 0 && f.history == 1);
    /* Clearing the recovered text cannot remove another queued identity. */
    CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'n', NULL, NULL) == SL_OK);
    suspend_keys(&f, "\033nfresh\033n\033N\037\033s");
    CHECK(!strcmp(sl_buffer(f.sl), "fresh"));
    CHECK(sl_prompt_queue_count(f.sl) == 2);
    teardown(&f);
  }
  for (scenario = 0; scenario < 4; ++scenario) {
    int held = scenario % 2;
    sl_prompt_queue_mode_t mode =
        scenario < 2 ? SL_PROMPT_QUEUE_MODE_QUEUED : SL_PROMPT_QUEUE_MODE_STEER;
    setup(&f, native, 4);
    CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
    CHECK(sl_prompt_queue_append(f.sl, "saved\ntext", held, &removed) == SL_OK);
    CHECK(sl_prompt_queue_set_mode(f.sl, 0, mode) == SL_OK);
    suspend_keys(&f, "\033e\033k\033s");
    CHECK(!*sl_buffer(f.sl));
    CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK &&
          info.id == removed && info.held == held && info.mode == mode);
    suspend_keys(&f, "prefix \033j\033s");
    CHECK(!strcmp(sl_buffer(f.sl), "prefix saved\ntext"));
    CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK &&
          info.id == removed && info.held == held && info.mode == mode);
    suspend_keys(&f, "\033n\037\033s");
    CHECK(!strcmp(sl_buffer(f.sl), "prefix saved\ntext"));
    CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK && !info.id);
    CHECK(sl_prompt_queue_count(f.sl) == 0);
    CHECK(sl_prompt_queue_approve(f.sl, removed) == SL_ERROR_INVALID);
    expect(&f, "\r", "prefix saved\ntext", SL_PROMPT_SOURCE_DIRECT, 0);
    CHECK(sl_last_prompt_id(f.sl) == 0);
    teardown(&f);
  }
}

static void empty_edits(int native) {
  struct fixture f;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t removed, next;
  const char *submit[] = {"\r",    "\033\r", "\t",   "\033r",
                          "\033r", "\033n",  "\033N"};
  char burst[32], *text = NULL;
  int origin, steer, route, requeue, calls, recorded;
  for (origin = 0; origin < 3; ++origin)
    for (steer = 0; steer < 2; ++steer)
      for (route = 0; route < 7; ++route) {
        setup(&f, native, 8);
        CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
        if (origin == 2) {
          suspend_keys(&f, "/hold remove\r\033s");
          removed = entry(&f, 0).id;
        } else
          CHECK(sl_prompt_queue_append(f.sl, "remove", origin == 1, &removed) ==
                SL_OK);
        CHECK(sl_prompt_queue_set_mode(f.sl, 0,
                                       steer ? SL_PROMPT_QUEUE_MODE_STEER
                                             : SL_PROMPT_QUEUE_MODE_QUEUED) ==
              SL_OK);
        if (route == 2)
          CHECK(sl_set_prompt_queue_profile(
                    f.sl, SL_PROMPT_QUEUE_PROFILE_DEFAULT) == SL_OK);
        CHECK(sl_set_prompt_queue_delivery(
                  f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
        requeue = route == 4;
        CHECK(sl_bind_key(f.sl, SL_KEY_ALT_BASE + 'r', empty_edit_key,
                          &requeue) == SL_OK);
        calls = f.calls;
        recorded = f.history;
        sprintf(burst, "\033e%s%s\037\033s", route < 5 ? "\001\013" : "",
                submit[route]);
        suspend_keys(&f, burst);
        CHECK(sl_prompt_queue_count(f.sl) == 0);
        CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK && !info.id);
        CHECK(!strcmp(sl_buffer(f.sl), route < 5     ? ""
                                       : origin == 2 ? "/hold remove"
                                                     : "remove") &&
              sl_last_prompt_id(f.sl) == 0);
        CHECK(sl_prompt_queue_approve(f.sl, removed) == SL_ERROR_INVALID);
        CHECK(sl_prompt_queue_reject(f.sl, removed) == SL_ERROR_INVALID);
        CHECK(f.calls == calls && f.history == recorded);
        CHECK(sl_set_prompt_queue(f.sl, 1, 1, 1) == SL_OK);
        CHECK(sl_prompt_queue_append(f.sl, "next", 1, &next) == SL_OK &&
              next > removed);
        CHECK(sl_prompt_queue_append(f.sl, "full", 0, NULL) == SL_ERROR_FULL);
        teardown(&f);
      }
  /* Trimmed whitespace follows the empty-edit route. */
  setup(&f, native, 8);
  CHECK(sl_inject_queued_prompt(f.sl, "remove", &removed) == SL_OK);
  suspend_keys(&f, "\033e\001\013 \r\033s");
  CHECK(sl_prompt_queue_count(f.sl) == 0 && !*sl_buffer(f.sl));
  teardown(&f);
  /* The runtime opt-out preserves whitespace edits verbatim. */
  setup(&f, native, 8);
  CHECK(sl_set_prompt_trim(f.sl, 0) == SL_OK);
  CHECK(sl_inject_queued_prompt(f.sl, "keep", &removed) == SL_OK);
  suspend_keys(&f, "\033e\001\013 \r\033s");
  CHECK(entry(&f, 0).id == removed && entry(&f, 0).held);
  CHECK(sl_prompt_queue_peek(f.sl, 0, &text) == SL_OK && !strcmp(text, " "));
  sl_free_string(f.sl, text);
  teardown(&f);
  /* Deleting an edit must not resume the stopped backlog behind it. */
  for (route = 0; route < 2; ++route) {
    sl_prompt_source_t source;
    int stopped;
    setup(&f, native, 8);
    CHECK(sl_set_status_busy(f.sl, 1) == SL_OK);
    CHECK(sl_prompt_queue_append(f.sl, "backlog", 0, &next) == SL_OK);
    CHECK(!keys(&f, "\033", &source));
    CHECK(sl_inject_steer_prompt(f.sl, "remove", &removed) == SL_OK);
    suspend_keys(&f, route ? "\033e\033n\033s" : "\033e\001\013\r\033s");
    CHECK(sl_get_prompt_queue_stopped(f.sl, &stopped) == SL_OK && stopped);
    CHECK(sl_prompt_queue_count(f.sl) == 1 && entry(&f, 0).id == next);
    CHECK(entry(&f, 0).stopped && !entry(&f, 0).eligible);
    teardown(&f);
  }
}

static void shortcut_mappings(int native) {
  const char letters[] = "kjn";
  struct fixture f;
  sl_prompt_queue_keys_t bindings;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_id_t id;
  char burst[64];
  int context, action_index, key_index;
  for (context = 0; context < 3; ++context)
    for (action_index = 0; action_index < 3; ++action_index)
      for (key_index = 0; key_index < 3; ++key_index) {
        setup(&f, native, 4);
        CHECK(sl_set_prompt_queue_profile(
                  f.sl, context == 0
                            ? SL_PROMPT_QUEUE_PROFILE_DEFAULT
                            : SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS) == SL_OK);
        CHECK(sl_set_status_busy(f.sl, context == 1) == SL_OK);
        CHECK(sl_set_prompt_queue_delivery(
                  f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) == SL_OK);
        suspend_keys(&f, "saved\033k\033s");
        memset(&bindings, 0, sizeof(bindings));
        if (action_index == 0)
          bindings.enqueue_draft = SL_KEY_ALT_BASE + letters[key_index];
        else if (action_index == 1)
          bindings.edit_newest = SL_KEY_ALT_BASE + letters[key_index];
        else
          bindings.submit_or_promote_newest =
              SL_KEY_ALT_BASE + letters[key_index];
        CHECK(sl_set_prompt_queue_keys(f.sl, &bindings) == SL_OK);
        if (action_index == 1)
          CHECK(sl_prompt_queue_append(f.sl, "queued", 0, &id) == SL_OK);
        sprintf(burst, "%s\033%c\033s",
                action_index == 1   ? ""
                : action_index == 2 ? "/hold command"
                                    : "draft",
                letters[key_index]);
        suspend_keys(&f, burst);
        if (action_index == 1) {
          CHECK(sl_prompt_queue_draft_info(f.sl, &info) == SL_OK &&
                info.id == id);
          CHECK(!strcmp(sl_buffer(f.sl), "queued"));
          CHECK(sl_prompt_queue_count(f.sl) == 0);
        } else if (action_index == 0 && context == 2) {
          /* An idle queued-turns enqueue is deliberately ignored. Its key
           * must still never fall through to stash/append/clear defaults. */
          CHECK(!strcmp(sl_buffer(f.sl), "draft"));
          CHECK(sl_prompt_queue_count(f.sl) == 0);
        } else {
          CHECK(!*sl_buffer(f.sl) && sl_prompt_queue_count(f.sl) == 1);
          CHECK(entry(&f, 0).mode == (action_index == 2
                                          ? SL_PROMPT_QUEUE_MODE_STEER
                                          : SL_PROMPT_QUEUE_MODE_QUEUED));
        }
        teardown(&f);
      }
  /* A disabled queue must not reserve its configured keys. */
  setup(&f, native, 4);
  suspend_keys(&f, "saved\033k\033s");
  memset(&bindings, 0, sizeof(bindings));
  bindings.enqueue_draft = SL_KEY_ALT_BASE + 'j';
  CHECK(sl_set_prompt_queue_keys(f.sl, &bindings) == SL_OK);
  CHECK(sl_set_prompt_queue(f.sl, 0, 4, 3) == SL_OK);
  suspend_keys(&f, "prefix\033j\033s");
  CHECK(!strcmp(sl_buffer(f.sl), "prefixsaved"));
  teardown(&f);
}

static void shortcut_focus(int native) {
  const char letters[] = "kKjJjJ";
  struct fixture f;
  sl_prompt_id_t id;
  char burst[32];
  int index;
  for (index = 0; index < 6; ++index) {
    setup(&f, native, 4);
    CHECK(sl_set_prompt_queue_delivery(f.sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL) ==
          SL_OK);
    if (index >= 4)
      suspend_keys(&f, "saved\033k\033s");
    CHECK(sl_inject_queued_prompt(f.sl, "approval", &id) == SL_OK);
    sprintf(burst, "\033a\033%c\033n\033s", letters[index]);
    suspend_keys(&f, burst);
    CHECK(!*sl_buffer(f.sl));
    CHECK(sl_prompt_queue_count(f.sl) == 1 && entry(&f, 0).id == id &&
          entry(&f, 0).held);
    suspend_keys(&f, "\033a\033y\033s");
    CHECK(sl_prompt_queue_count(f.sl) == 1 && entry(&f, 0).id == id &&
          !entry(&f, 0).held);
    CHECK(f.calls == 0 && f.history == 0);
    teardown(&f);
  }
}

int main(int argc, char **argv) {
  int native;
  (void)argv;
  alarm(25);
  for (native = 0; native <= 1; native++) {
    fprintf(stderr, "policy tests: native=%d\n", native);
    if (argc > 1) {
      if (!strcmp(argv[1], "shortcut-precedence")) {
        shortcut_mappings(native);
        shortcut_focus(native);
      } else if (!strcmp(argv[1], "shortcut-mappings"))
        shortcut_mappings(native);
      else if (!strcmp(argv[1], "shortcut-focus"))
        shortcut_focus(native);
      else if (!strcmp(argv[1], "discard"))
        discard_edges(native);
      else if (!strcmp(argv[1], "empty-edits"))
        empty_edits(native);
      else if (!strcmp(argv[1], "barriers"))
        edited_barriers(native);
      else if (!strcmp(argv[1], "policy_wakeup"))
        policy_wakeup(native);
      else if (!strcmp(argv[1], "readiness"))
        edit_readiness(native);
      else if (!strcmp(argv[1], "callbacks"))
        callback_submissions(native);
      else if (!strcmp(argv[1], "steering"))
        steering_eligibility(native);
      else if (!strcmp(argv[1], "history"))
        history_once(native);
      else
        policy_edges(native);
      continue;
    }
    admission(native);
    boundaries(native);
    stopped(native);
    capacity(native);
    shell_and_order(native);
    transitions(native);
    policy_edges(native);
    history_once(native);
    steering_eligibility(native);
    callback_submissions(native);
    edit_readiness(native);
    edited_barriers(native);
    policy_wakeup(native);
    empty_edits(native);
    discard_edges(native);
  }
  puts("Queue policy regressions passed");
  return 0;
}
