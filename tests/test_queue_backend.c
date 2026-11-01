#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(test)                                                            \
  do {                                                                         \
    if (!(test)) {                                                             \
      fprintf(stderr, "%d: %s\n", __LINE__, #test);                            \
      return 1;                                                                \
    }                                                                          \
  } while (0)

#define LARGE_PASTE_LEN (128u * 1024u)

struct bridge {
  sl_t *sl;
  int calls, detached, reentry, failed, stage, master, signal[2], version;
  int host_discard;
  sl_prompt_id_t local_id;
  sl_queue_request_t request;
  char text[256], id[128], revision[128], token[128];
};

static int request(void *context, const sl_queue_request_t *value) {
  struct bridge *bridge = context;
  sl_queue_change_t change;
  ++bridge->calls;
  bridge->request = *value;
  snprintf(bridge->text, sizeof(bridge->text), "%s",
           value->text ? value->text : "");
  snprintf(bridge->id, sizeof(bridge->id), "%s",
           value->item_id ? value->item_id : "");
  snprintf(bridge->revision, sizeof(bridge->revision), "%s",
           value->item_revision ? value->item_revision : "");
  snprintf(bridge->token, sizeof(bridge->token), "%s",
           value->edit_token ? value->edit_token : "");
  bridge->request.text = value->text ? bridge->text : NULL;
  bridge->request.item_id = value->item_id ? bridge->id : NULL;
  bridge->request.item_revision =
      value->item_revision ? bridge->revision : NULL;
  bridge->request.edit_token = value->edit_token ? bridge->token : NULL;
  if (bridge->reentry) {
    memset(&change, 0, sizeof(change));
    change.kind = SL_QUEUE_DISCONNECTED;
    CHECK(sl_queue_backend_update(bridge->sl, &change) == SL_ERROR_INVALID);
    sl_destroy(bridge->sl);
    CHECK(bridge->sl && !bridge->detached);
  }
  return bridge->failed ? SL_ERROR_IO : SL_OK;
}

static void detach(void *context) {
  struct bridge *bridge = context;
  ++bridge->detached;
  bridge->sl = NULL;
}

static int snapshot(sl_t *sl, const char *revision,
                    const sl_queue_entry_t *entries, size_t count) {
  sl_queue_change_t change;
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_SNAPSHOT;
  change.revision = revision;
  change.entries = entries;
  change.count = change.total_count = count;
  return sl_queue_backend_update(sl, &change);
}

static sl_t *create(struct bridge *bridge, int input, int output,
                    size_t limit) {
  sl_config_t config;
  sl_queue_backend_t backend;
  sl_config_init(&config);
  memset(&backend, 0, sizeof(backend));
  backend.request = request;
  backend.detach = detach;
  backend.capabilities = (1UL << (SL_QUEUE_RESOLVE + 1)) - 1;
  backend.max_pending_requests = limit;
  config.input_fd = input;
  config.output_fd = output;
  config.prompt_queue = 1;
  config.queue_backend = &backend;
  config.queue_backend_context = bridge;
  bridge->sl = sl_create_with_config(&config);
  /* The constructor must copy this stack callback table. */
  backend.request = NULL;
  return bridge->sl;
}

static int api(void) {
  struct bridge bridge;
  sl_t *sl;
  sl_queue_request_t offer;
  sl_queue_request_id_t first, second, third;
  sl_queue_backend_info_t info;
  sl_queue_pending_info_t pending;
  sl_queue_entry_t entry, borrowed;
  sl_queue_change_t change;
  sl_queue_result_t result;
  sl_prompt_queue_entry_info_t local;
  char *text = NULL;
  int pipefd[2], calls;
  memset(&bridge, 0, sizeof(bridge));
  CHECK(pipe(pipefd) == 0);
  sl = create(&bridge, pipefd[0], pipefd[1], 2);
  CHECK(sl && sl->queue_backend_entry && snapshot(sl, "q0", NULL, 0) == SL_OK);
  CHECK(bridge.calls == 0);
  bridge.reentry = 1;
  memset(&offer, 0, sizeof(offer));
  offer.operation = SL_QUEUE_OFFER;
  offer.text = "first";
  CHECK(sl_queue_backend_request(sl, &offer, &first) == SL_OK && first);
  CHECK(sl_queue_backend_pending(sl, 0, &pending) == SL_OK && pending.pending);
  CHECK(!strcmp(pending.request.text, "first"));
  CHECK(sl_set_buffer(sl, "newer draft") == SL_OK);
  memset(&entry, 0, sizeof(entry));
  entry.id = "opaque/backend-A";
  entry.revision = "item-A:1";
  entry.text = "first";
  entry.ready = 1;
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_UPSERT;
  change.previous_revision = "q0";
  change.revision = "q1";
  change.entries = &entry;
  change.count = 1;
  change.total_count = 1;
  change.request_id = first;
  calls = bridge.calls;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK && bridge.calls == calls);
  CHECK(sl_queue_backend_entry(sl, 0, &borrowed) == SL_OK && borrowed.ready);
  CHECK(sl_prompt_queue_info(sl, 0, &local) == SL_OK && !local.eligible);
  CHECK(sl_prompt_queue_take(sl, 0, &text) == SL_ERROR_UNSUPPORTED && !text);
  CHECK(sl_prompt_queue_clear(sl) == SL_ERROR_UNSUPPORTED);
  CHECK(sl_set_auto_approval(sl, 1) == SL_ERROR_UNSUPPORTED);
  CHECK(sl_prompt_queue_replace(sl, 0, "unauthorized") == SL_ERROR_UNSUPPORTED);
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  change.kind = SL_QUEUE_REMOVE;
  change.previous_revision = "q1";
  change.revision = "q2";
  change.item_id = entry.id;
  change.count = 0;
  change.total_count = 0;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  memset(&result, 0, sizeof(result));
  result.request_id = first;
  result.item_id = entry.id;
  result.item_revision = entry.revision;
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  CHECK(sl_prompt_queue_count(sl) == 0 &&
        !strcmp(sl_buffer(sl), "newer draft"));
  CHECK(sl_queue_backend_request(sl, &offer, &first) == SL_OK);
  CHECK(sl_queue_backend_request(sl, &offer, &second) == SL_OK &&
        second != first);
  CHECK(sl_queue_backend_request(sl, &offer, &third) == SL_ERROR_FULL &&
        !third);
  result.request_id = first;
  result.status = SL_QUEUE_REJECTED;
  result.reason = "admission rejected";
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  CHECK(sl_queue_backend_recover(sl, first) == SL_ERROR_INVALID);
  CHECK(sl_set_buffer(sl, "") == SL_OK);
  CHECK(sl_queue_backend_recover(sl, first) == SL_OK &&
        !strcmp(sl_buffer(sl), "first"));
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_DISCONNECTED;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  CHECK(sl_set_buffer(sl, "") == SL_OK);
  CHECK(sl_queue_backend_recover(sl, second) == SL_ERROR_NOT_READY);
  CHECK(sl_queue_backend_request(sl, &offer, &third) == SL_ERROR_NOT_READY);
  CHECK(snapshot(sl, "q2", NULL, 0) == SL_OK);
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK && info.synchronized);
  result.request_id = second;
  result.status = SL_QUEUE_ACCEPTED;
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  change.kind = SL_QUEUE_STATE;
  change.previous_revision = "wrong";
  change.revision = "q3";
  calls = bridge.calls;
  CHECK(sl_queue_backend_update(sl, &change) == SL_ERROR_CONFLICT &&
        calls == bridge.calls);
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK && !info.synchronized);
  CHECK(snapshot(sl, "q3", NULL, 0) == SL_OK);
  CHECK(snapshot(sl, "q1", NULL, 0) == SL_ERROR_CONFLICT);
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK &&
        !strcmp(info.revision, "q3"));
  sl_destroy(sl);
  CHECK(bridge.detached == 1 && bridge.sl == NULL);
  close(pipefd[0]);
  close(pipefd[1]);
  return 0;
}

static int failures(void) {
  struct bridge bridge;
  sl_queue_request_t request_value;
  sl_queue_request_id_t offer, control;
  sl_queue_result_t result;
  sl_queue_backend_info_t info;
  sl_queue_entry_t entries[2];
  sl_queue_change_t change;
  sl_queue_pending_info_t pending;
  sl_t *sl;
  int pipes[2], calls;
  memset(&bridge, 0, sizeof(bridge));
  CHECK(pipe(pipes) == 0);
  sl = create(&bridge, pipes[0], pipes[1], 1);
  CHECK(sl);
  memset(entries, 0, sizeof(entries));
  entries[0].id = "item";
  entries[0].revision = "v1";
  entries[0].text = "accepted";
  entries[0].held = 1;
  CHECK(snapshot(sl, "q0", entries, 1) == SL_OK);
  CHECK(sl_set_buffer(sl, "unsent") == SL_OK);
  memset(&request_value, 0, sizeof(request_value));
  request_value.text = "proposal";
  bridge.failed = 1;
  CHECK(sl_queue_backend_request(sl, &request_value, &offer) == SL_ERROR_IO &&
        !offer);
  CHECK(!strcmp(sl_buffer(sl), "unsent"));
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK && !info.pending_count);
  bridge.failed = 0;
  request_value.operation = SL_QUEUE_BEGIN_EDIT;
  request_value.item_id = "item";
  request_value.text = NULL;
  CHECK(sl_queue_backend_request(sl, &request_value, &offer) == SL_OK);
  memset(&result, 0, sizeof(result));
  result.request_id = offer;
  result.status = SL_QUEUE_REJECTED;
  result.reason = "claim won before edit";
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK &&
        !strcmp(sl_buffer(sl), "unsent"));
  CHECK(sl_prompt_queue_count(sl) == 1 &&
        sl_queue_backend_info(sl, &info) == SL_OK && !info.pending_count);
  memset(&request_value, 0, sizeof(request_value));
  request_value.text = "retained proposal";
  CHECK(sl_queue_backend_request(sl, &request_value, &offer) == SL_OK);
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_DISCONNECTED;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  entries[1] = entries[0];
  CHECK(snapshot(sl, "q1", entries, 2) ==
        SL_ERROR_INVALID); /* duplicate identity is atomic */
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK && !info.synchronized &&
        !strcmp(info.revision, "q0"));
  CHECK(sl_prompt_queue_count(sl) == 1 && !strcmp(sl_buffer(sl), "unsent"));
  CHECK(snapshot(sl, "q0", entries, 1) == SL_OK);
  CHECK(sl_queue_backend_request(sl, &request_value, &control) ==
        SL_ERROR_FULL);
  /* A saturated ordinary table still has room to reconcile original outcomes.
   */
  memset(&request_value, 0, sizeof(request_value));
  request_value.operation = SL_QUEUE_RESOLVE;
  request_value.target_request_id = offer;
  CHECK(sl_queue_backend_request(sl, &request_value, &control) == SL_OK);
  CHECK(bridge.request.target_request_id == offer);
  CHECK(sl_queue_backend_request(sl, &request_value, NULL) ==
        SL_ERROR_NOT_READY);
  request_value.operation = SL_QUEUE_SYNCHRONIZE;
  CHECK(sl_queue_backend_request(sl, &request_value, &control) == SL_OK);
  result.request_id = control;
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  CHECK(sl_queue_backend_request(sl, &request_value, &control) ==
        SL_OK); /* explicit retry */
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK && info.pending_count == 1);
  CHECK(sl_queue_backend_pending(sl, 0, &pending) == SL_OK &&
        pending.request.request_id == offer &&
        pending.status == SL_QUEUE_UNKNOWN);
  CHECK(snapshot(sl, "q0", entries, 1) == SL_OK);
  result.request_id = offer;
  result.status = SL_QUEUE_REJECTED;
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  CHECK(sl_set_buffer(sl, "") == SL_OK &&
        sl_queue_backend_recover(sl, offer) == SL_OK);
  CHECK(!strcmp(sl_buffer(sl), "retained proposal"));
  calls = bridge.calls;
  sl_destroy(sl);
  CHECK(bridge.calls == calls && bridge.detached == 1);
  close(pipes[0]);
  close(pipes[1]);
  return 0;
}

static int fifo(void) {
  struct bridge bridge;
  sl_queue_entry_t entries[2];
  sl_queue_request_t r;
  sl_queue_request_id_t id;
  sl_queue_result_t result;
  sl_queue_change_t change;
  sl_queue_backend_info_t info;
  sl_t *sl;
  int pipes[2], operation;
  memset(&bridge, 0, sizeof(bridge));
  CHECK(pipe(pipes) == 0);
  sl = create(&bridge, pipes[0], pipes[1], 4);
  CHECK(sl);
  memset(entries, 0, sizeof(entries));
  entries[0].id = "first";
  entries[0].revision = "v1";
  entries[0].text = "first held";
  entries[0].held = 1;
  entries[1] = entries[0];
  entries[1].id = "second";
  entries[1].text = "second held";
  CHECK(snapshot(sl, "q0", entries, 2) == SL_OK);
  memset(&r, 0, sizeof(r));
  r.item_id = "second";
  for (operation = SL_QUEUE_BEGIN_EDIT; operation <= SL_QUEUE_WITHDRAW;
       ++operation) {
    if (operation != SL_QUEUE_BEGIN_EDIT && operation != SL_QUEUE_APPROVE &&
        operation != SL_QUEUE_WITHDRAW)
      continue;
    r.operation = (sl_queue_operation_t)operation;
    CHECK(sl_queue_backend_request(sl, &r, NULL) == SL_ERROR_CONFLICT &&
          !bridge.calls);
  }
  r.item_id = "first";
  r.operation = SL_QUEUE_APPROVE;
  CHECK(sl_queue_backend_request(sl, &r, &id) == SL_OK);
  memset(&result, 0, sizeof(result));
  result.request_id = id;
  result.item_id = "first";
  result.item_revision = "v2";
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  r.item_id = "second";
  CHECK(sl_queue_backend_request(sl, &r, NULL) == SL_ERROR_CONFLICT);
  entries[0].held = 0;
  entries[0].revision = "v2";
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_UPSERT;
  change.previous_revision = "q0";
  change.revision = "q1";
  change.count = 1;
  change.entries = entries;
  change.total_count = 2;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  r.operation = SL_QUEUE_BEGIN_EDIT;
  CHECK(sl_queue_backend_request(sl, &r, &id) == SL_OK && bridge.calls == 2);
  change.kind = SL_QUEUE_REMOVE;
  change.count = 0;
  change.previous_revision = "q1";
  change.revision = "q2";
  change.item_id = "outside-window";
  CHECK(sl_queue_backend_update(sl, &change) == SL_ERROR_CONFLICT);
  CHECK(sl_queue_backend_info(sl, &info) == SL_OK && !info.synchronized &&
        !strcmp(info.revision, "q1"));
  sl_destroy(sl);
  close(pipes[0]);
  close(pipes[1]);
  return 0;
}

static int native_watch(sl_t *sl, const sl_watch_event_t *event,
                        void *context) {
  struct bridge *bridge = context;
  sl_queue_result_t result;
  sl_queue_entry_t entry;
  sl_queue_change_t change;
  char byte;
  CHECK(read(event->fd, &byte, 1) == 1);
  CHECK(!strcmp(sl_buffer(sl), "new"));
  memset(&result, 0, sizeof(result));
  result.request_id = bridge->request.request_id;
  if (bridge->stage == 2) {
    result.status = SL_QUEUE_REJECTED;
    result.reason = "deferred rejection";
    CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
    CHECK(!strcmp(sl_buffer(sl), "new"));
    CHECK(write(bridge->master, "\033n\033e", 4) == 4);
    bridge->stage = 3;
  } else {
    memset(&entry, 0, sizeof(entry));
    entry.id = "backend/accepted";
    entry.revision = "v1";
    entry.text = "alpha";
    entry.ready = 1;
    memset(&change, 0, sizeof(change));
    change.kind = SL_QUEUE_UPSERT;
    change.previous_revision = "q0";
    change.revision = "q1";
    change.entries = &entry;
    change.count = 1;
    change.total_count = 1;
    change.request_id = result.request_id;
    CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
    result.item_id = entry.id;
    result.item_revision = entry.revision;
    CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
    CHECK(!strcmp(sl_buffer(sl), "new"));
    CHECK(write(bridge->master, "\033n\004", 3) == 3);
    bridge->stage = 6;
  }
  return SL_OK;
}

static void native_idle(sl_t *sl, void *context) {
  struct bridge *bridge = context;
  char drain[2048];
  while (read(bridge->master, drain, sizeof(drain)) > 0) {
  }
  if (!bridge->stage) {
    bridge->stage = 1;
    if (write(bridge->master, "alpha\rnew", 9) != 9)
      bridge->failed = 1;
  } else if ((bridge->stage == 1 || bridge->stage == 4) &&
             !strcmp(sl_buffer(sl), "new")) {
    ++bridge->stage;
    if (write(bridge->signal[1], "x", 1) != 1)
      bridge->failed = 1;
  } else if (bridge->stage == 3 && !strcmp(sl_buffer(sl), "alpha")) {
    bridge->stage = 4;
    if (write(bridge->master, "\rnew", 4) != 4)
      bridge->failed = 1;
  }
}

static int native(void) {
  struct bridge bridge;
  sl_prompt_source_t source;
  sl_watch_id_t watch;
  struct winsize size;
  int slave;
  char *delivered;
  memset(&bridge, 0, sizeof(bridge));
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&bridge.master, &slave, NULL, NULL, &size) == 0);
  CHECK(fcntl(bridge.master, F_SETFL, O_NONBLOCK) == 0 &&
        pipe(bridge.signal) == 0);
  CHECK(create(&bridge, slave, slave, 4));
  CHECK(snapshot(bridge.sl, "q0", NULL, 0) == SL_OK);
  CHECK(sl_watch_add(bridge.sl, bridge.signal[0], SL_WATCH_READ, native_watch,
                     &bridge, &watch) == SL_OK);
  CHECK(sl_set_idle_callback(bridge.sl, native_idle, &bridge) == SL_OK);
  delivered = sl_next_prompt(bridge.sl, "> ", &source);
  CHECK(!delivered && source == SL_PROMPT_SOURCE_NONE && bridge.stage == 6 &&
        !bridge.failed);
  CHECK(sl_last_readline_status(bridge.sl) == SL_READLINE_EOF &&
        bridge.calls == 2);
  CHECK(sl_prompt_queue_count(bridge.sl) == 1);
  sl_destroy(bridge.sl);
  CHECK(bridge.detached == 1);
  close(bridge.master);
  close(slave);
  close(bridge.signal[0]);
  close(bridge.signal[1]);
  return 0;
}

static int edit_watch(sl_t *sl, const sl_watch_event_t *event, void *context) {
  struct bridge *bridge = context;
  sl_queue_entry_t entry;
  sl_queue_change_t change;
  sl_queue_result_t result;
  char byte, previous[24], revision[24], item_revision[24];
  CHECK(read(event->fd, &byte, 1) == 1);
  snprintf(previous, sizeof(previous), "q%d", bridge->version);
  ++bridge->version;
  snprintf(revision, sizeof(revision), "q%d", bridge->version);
  snprintf(item_revision, sizeof(item_revision), "v%d", bridge->version);
  memset(&entry, 0, sizeof(entry));
  entry.id = "remote/edit";
  entry.revision = item_revision;
  entry.text = bridge->stage == 2 ? "original" : "edited";
  entry.mode = SL_PROMPT_QUEUE_MODE_STEER;
  entry.held = bridge->version < 4;
  if (bridge->request.operation == SL_QUEUE_BEGIN_EDIT) {
    entry.edit_held = 1;
    entry.edit_token = bridge->version == 2 ? "lease-one" : "lease-two";
  }
  memset(&change, 0, sizeof(change));
  change.kind = bridge->request.operation == SL_QUEUE_WITHDRAW
                    ? SL_QUEUE_REMOVE
                    : SL_QUEUE_UPSERT;
  change.previous_revision = previous;
  change.revision = revision;
  change.item_id = entry.id;
  change.entries = &entry;
  change.count = change.total_count = change.kind == SL_QUEUE_REMOVE ? 0 : 1;
  change.request_id = bridge->request.request_id;
  CHECK(sl_queue_backend_update(sl, &change) == SL_OK);
  memset(&result, 0, sizeof(result));
  result.request_id = bridge->request.request_id;
  result.item_id = entry.id;
  result.item_revision = entry.revision;
  result.edit_token = entry.edit_token;
  CHECK(sl_queue_backend_result(sl, &result) == SL_OK);
  if (bridge->stage == 6) {
    struct winsize size;
    memset(&size, 0, sizeof(size));
    size.ws_col = 40;
    size.ws_row = 12;
    CHECK(ioctl(bridge->master, TIOCSWINSZ, &size) == 0);
    CHECK(sl_output_stream_write(sl, "Concurrent output.\n", 19) == SL_OK);
    CHECK(!strcmp(sl_buffer(sl), "new"));
  }
  ++bridge->stage;
  return SL_OK;
}

static int edit_step(sl_t *sl, struct bridge *bridge) {
  sl_prompt_queue_entry_info_t draft, info;
  char drain[2048];
  while (read(bridge->master, drain, sizeof(drain)) > 0) {
  }
  CHECK(sl_prompt_queue_draft_info(sl, &draft) == SL_OK);
  switch (bridge->stage) {
  case 0:
    CHECK(write(bridge->master, "\033a\033e", 4) == 4);
    bridge->stage = 1;
    break;
  case 1:
  case 11:
    if (bridge->calls != (bridge->stage == 1 ? 1 : 4))
      break;
    CHECK(bridge->request.operation == SL_QUEUE_BEGIN_EDIT && !draft.id &&
          !*sl_buffer(sl));
    ++bridge->stage;
    CHECK(write(bridge->signal[1], "x", 1) == 1);
    break;
  case 3:
    if (!draft.id)
      break;
    CHECK(draft.id == bridge->local_id && draft.held &&
          !strcmp(sl_buffer(sl), "original"));
    CHECK(sl_set_buffer(sl, "edited") == SL_OK);
    CHECK(write(bridge->master, "\r", 1) == 1);
    bridge->stage = 4;
    break;
  case 4:
    if (bridge->calls != 2)
      break;
    CHECK(bridge->request.operation == SL_QUEUE_END_EDIT &&
          !strcmp(bridge->text, "edited"));
    CHECK(!strcmp(bridge->id, "remote/edit") &&
          !strcmp(bridge->revision, "v2"));
    CHECK(!strcmp(bridge->token, "lease-one") &&
          bridge->request.mode == SL_PROMPT_QUEUE_MODE_STEER);
    CHECK(!draft.id && !*sl_buffer(sl));
    CHECK(write(bridge->master, "new", 3) == 3);
    bridge->stage = 5;
    break;
  case 5:
    if (strcmp(sl_buffer(sl), "new"))
      break;
    bridge->stage = 6;
    CHECK(write(bridge->signal[1], "x", 1) == 1);
    break;
  case 7:
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK &&
          info.id == bridge->local_id && info.held);
    CHECK(write(bridge->master, "\033n\033a\033y", 6) == 6);
    bridge->stage = 8;
    break;
  case 8:
    if (bridge->calls != 3)
      break;
    CHECK(bridge->request.operation == SL_QUEUE_APPROVE);
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && info.held);
    bridge->stage = 9;
    CHECK(write(bridge->signal[1], "x", 1) == 1);
    break;
  case 10:
    CHECK(sl_prompt_queue_info(sl, 0, &info) == SL_OK && !info.held &&
          !info.eligible);
    CHECK(write(bridge->master, "\033e", 2) == 2);
    bridge->stage = 11;
    break;
  case 13:
    if (!draft.id)
      break;
    CHECK(draft.id == bridge->local_id && !strcmp(sl_buffer(sl), "edited"));
    if (bridge->host_discard) {
      CHECK(sl_history_add(sl, "search candidate") == SL_OK);
      CHECK(write(bridge->master, "\022", 1) == 1);
      bridge->stage = 19;
      break;
    }
    CHECK(write(bridge->master, "\033n", 2) == 2);
    bridge->stage = 14;
    break;
  case 14:
    if (bridge->calls != 5)
      break;
    CHECK(bridge->request.operation == SL_QUEUE_WITHDRAW && !draft.id &&
          !*sl_buffer(sl));
    bridge->stage = 15;
    CHECK(write(bridge->signal[1], "x", 1) == 1);
    break;
  case 16:
    CHECK(sl_prompt_queue_count(sl) == 0);
    if (bridge->host_discard) {
      CHECK(!*sl_buffer(sl) && !sl_history_search_active(sl));
      CHECK(write(bridge->master, "\007\004", 2) == 2);
      bridge->stage = 18;
      break;
    }
    CHECK(write(bridge->master, "\037", 1) == 1);
    bridge->stage = 17;
    break;
  case 17:
    if (strcmp(sl_buffer(sl), "edited"))
      break;
    CHECK(!draft.id && sl_prompt_queue_count(sl) == 0);
    CHECK(write(bridge->master, "\033n\004", 3) == 3);
    bridge->stage = 18;
    break;
  case 19:
    if (!sl_history_search_active(sl))
      break;
    CHECK(sl_prompt_queue_reject(sl, draft.id) == SL_OK);
    CHECK(!sl_history_search_active(sl) && !*sl_buffer(sl));
    bridge->stage = 14;
    break;
  default:
    break;
  }
  return 0;
}
static void edit_idle(sl_t *sl, void *context) {
  struct bridge *bridge = context;
  if (edit_step(sl, bridge)) {
    bridge->failed = 1;
    (void)sl_cancel(sl);
  }
}
static int native_edit(int host_discard) {
  struct bridge bridge;
  sl_queue_entry_t entry;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_source_t source;
  sl_watch_id_t watch;
  struct winsize size;
  int slave;
  memset(&bridge, 0, sizeof(bridge));
  bridge.version = 1;
  bridge.host_discard = host_discard;
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(openpty(&bridge.master, &slave, NULL, NULL, &size) == 0);
  CHECK(fcntl(bridge.master, F_SETFL, O_NONBLOCK) == 0 &&
        pipe(bridge.signal) == 0);
  CHECK(create(&bridge, slave, slave, 8));
  memset(&entry, 0, sizeof(entry));
  entry.id = "remote/edit";
  entry.revision = "v1";
  entry.text = "original";
  entry.mode = SL_PROMPT_QUEUE_MODE_STEER;
  entry.held = 1;
  CHECK(snapshot(bridge.sl, "q1", &entry, 1) == SL_OK);
  CHECK(sl_prompt_queue_info(bridge.sl, 0, &info) == SL_OK);
  bridge.local_id = info.id;
  CHECK(sl_watch_add(bridge.sl, bridge.signal[0], SL_WATCH_READ, edit_watch,
                     &bridge, &watch) == SL_OK);
  CHECK(sl_set_idle_callback(bridge.sl, edit_idle, &bridge) == SL_OK);
  CHECK(sl_output_stream_begin(bridge.sl) == SL_OK);
  CHECK(sl_next_prompt(bridge.sl, "> ", &source) == NULL);
  CHECK(source == SL_PROMPT_SOURCE_NONE && bridge.stage == 18 &&
        !bridge.failed && bridge.calls == 5);
  CHECK(sl_output_stream_end(bridge.sl) == SL_OK);
  sl_destroy(bridge.sl);
  close(bridge.master);
  close(slave);
  close(bridge.signal[0]);
  close(bridge.signal[1]);
  return 0;
}

struct delayed {
  struct bridge bridge;
  int kind, phase, ticks, version;
  sl_queue_request_id_t original;
  int slave;
  size_t written;
  char *paste;
  int control_calls;
};

static int delayed_result(struct delayed *d, sl_queue_request_id_t id,
                          int hold) {
  sl_queue_entry_t entry;
  sl_queue_result_t result;
  sl_queue_change_t change;
  char previous[32], revision[32], item_revision[32];
  snprintf(previous, sizeof(previous), "q%d", d->version);
  snprintf(revision, sizeof(revision), "q%d", ++d->version);
  snprintf(item_revision, sizeof(item_revision), "v%d", d->version);
  memset(&entry, 0, sizeof(entry));
  entry.id = "delayed";
  entry.text = "original";
  entry.revision = item_revision;
  entry.held = 1;
  entry.edit_held = hold;
  entry.edit_token = hold ? "owned" : NULL;
  memset(&change, 0, sizeof(change));
  change.kind = SL_QUEUE_UPSERT;
  change.previous_revision = previous;
  change.revision = revision;
  change.entries = &entry;
  change.count = change.total_count = 1;
  change.request_id = id;
  CHECK(sl_queue_backend_update(d->bridge.sl, &change) == SL_OK);
  memset(&result, 0, sizeof(result));
  result.request_id = id;
  result.item_id = entry.id;
  result.item_revision = entry.revision;
  result.edit_token = entry.edit_token;
  CHECK(sl_queue_backend_result(d->bridge.sl, &result) == SL_OK);
  return 0;
}

static int delayed_step(sl_t *sl, struct delayed *d) {
  struct bridge *b = &d->bridge;
  char drain[2048];
  sl_prompt_queue_entry_info_t draft;
  const char *expected = d->kind == 5   ? d->paste + 6
                         : d->kind == 1 ? "x\303\251\nz"
                                        : "x\303\251";
  if (d->kind != 5)
    while (read(b->master, drain, sizeof(drain)) > 0) {
    }
  CHECK(sl_prompt_queue_draft_info(sl, &draft) == SL_OK);
  if (!d->phase) {
    const char *keys = d->kind == 5 ? "original\r\033e\025"
                       : d->kind == 1
                           ? "original\r\033e\025\033[200~x\303\251\nz\033[201~"
                           : "original\r\033e\025x\303\251";
    CHECK(write(b->master, keys, strlen(keys)) == (ssize_t)strlen(keys));
    d->phase = 1;
  } else if (d->phase == 1 && b->calls == 1 && ++d->ticks >= 4) {
    CHECK(b->request.operation == SL_QUEUE_OFFER && !*sl_buffer(sl) &&
          !draft.id);
    d->original = b->request.request_id;
    d->ticks = 0;
    if (d->kind == 2) {
      CHECK(sl_cancel(sl) == SL_OK);
      d->phase = 6;
    } else {
      CHECK(delayed_result(d, d->original, 0) == 0);
      d->phase = 2;
    }
  } else if (d->phase == 2 && b->calls == 2 && ++d->ticks >= 4) {
    CHECK(b->request.operation == SL_QUEUE_BEGIN_EDIT && !*sl_buffer(sl) &&
          !draft.id);
    d->original = b->request.request_id;
    if (d->kind == 6) {
      CHECK(write(b->master, "\033q", 2) == 2);
      d->phase = 8;
    } else if (d->kind == 5) {
      int pending;
      size_t length = LARGE_PASTE_LEN + 12;
      if (d->written < length) {
        size_t chunk = length - d->written;
        ssize_t sent;
        if (chunk > 4096)
          chunk = 4096;
        sent = write(b->master, d->paste + d->written, chunk);
        if (sent > 0)
          d->written += (size_t)sent;
        return 0;
      }
      CHECK(!ioctl(d->slave, FIONREAD, &pending));
      if (pending)
        return 0;
      d->paste[6 + LARGE_PASTE_LEN] = 0;
      CHECK(delayed_result(d, d->original, 1) == 0);
      d->phase = 3;
    } else if (d->kind >= 3) {
      if (d->kind == 4)
        CHECK(write(b->master, "\033", 1) == 1);
      else
        CHECK(sl_cancel(sl) == SL_OK);
      d->phase = 6;
    } else {
      CHECK(d->control_calls == 1);
      CHECK(delayed_result(d, d->original, 1) == 0);
      d->phase = 3;
    }
  } else if (d->phase == 3 && !strcmp(sl_buffer(sl), expected)) {
    CHECK(draft.id && draft.held && b->calls == 2);
    CHECK(sl_suspend(sl) == SL_OK);
    d->phase = 7;
  } else if (d->phase == 4) {
    CHECK(!draft.id && !*sl_buffer(sl));
    if (d->kind >= 3) {
      sl_queue_result_t result;
      if (b->request.operation != SL_QUEUE_ABORT_EDIT)
        return 0;
      CHECK(b->calls == 3 && !strcmp(b->token, "owned"));
      memset(&result, 0, sizeof(result));
      result.request_id = b->request.request_id;
      CHECK(delayed_result(d, result.request_id, 0) == 0);
    } else
      CHECK(b->calls == 2);
    CHECK(write(b->master, "\004", 1) == 1);
    d->phase = 5;
  }
  return 0;
}
static void delayed_idle(sl_t *sl, void *context) {
  struct delayed *d = context;
  if (delayed_step(sl, d)) {
    d->bridge.failed = 1;
    (void)sl_cancel(sl);
  }
}
static int cancel_key(sl_t *sl, sl_key_t key, void *context,
                      sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)context;
  *action = SL_KEY_ACTION_CANCEL;
  return SL_OK;
}
static int deferred_control(sl_t *sl, sl_key_t key, void *context,
                            sl_key_action_t *action) {
  struct delayed *d = context;
  (void)sl;
  ++d->control_calls;
  *action =
      key == SL_KEY_ALT_BASE + 'q' ? SL_KEY_ACTION_SUSPEND : SL_KEY_ACTION_PASS;
  return SL_OK;
}
static int delayed_edit(int kind) {
  struct delayed *d = calloc(1, sizeof(*d));
  sl_prompt_source_t source;
  struct winsize size;
  int slave;
  pid_t drainer = -1;
  CHECK(d);
  d->kind = kind;
  if (kind == 5) {
    size_t i;
    d->paste = malloc(LARGE_PASTE_LEN + 13);
    CHECK(d->paste);
    memcpy(d->paste, "\033[200~", 6);
    for (i = 0; i < LARGE_PASTE_LEN; ++i)
      d->paste[i + 6] = i % 67 == 66 ? '\n' : (char)('a' + i % 26);
    memcpy(d->paste + 6 + LARGE_PASTE_LEN, "\033[201~", 7);
  }
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(!openpty(&d->bridge.master, &slave, NULL, NULL, &size));
  d->slave = slave;
  CHECK(fcntl(d->bridge.master, F_SETFL, O_NONBLOCK) == 0);
  if (kind == 5) {
    /* A real terminal drains output during replay too, when no idle callback
     * runs. Do not let this fixture's PTY output capacity throttle the editor.
     */
    drainer = fork();
    CHECK(drainer >= 0);
    if (!drainer) {
      struct pollfd fd;
      char bytes[2048];
      close(slave);
      fd.fd = d->bridge.master;
      fd.events = POLLIN;
      while (poll(&fd, 1, -1) > 0 && read(fd.fd, bytes, sizeof(bytes)) > 0) {
      }
      _exit(0);
    }
  }
  CHECK(create(&d->bridge, slave, slave, kind >= 3 && kind != 5 ? 1 : 8));
  CHECK(sl_bind_key(d->bridge.sl, SL_KEY_ESCAPE, cancel_key, NULL) == SL_OK);
  if (kind < 2 || kind == 6)
    CHECK(sl_bind_key(d->bridge.sl,
                      kind == 6 ? SL_KEY_ALT_BASE + 'q' : SL_KEY_CTRL_U,
                      deferred_control, d) == SL_OK);
  CHECK(sl_suspend(d->bridge.sl) == SL_ERROR_INVALID);
  CHECK(snapshot(d->bridge.sl, "q0", NULL, 0) == SL_OK);
  CHECK(sl_set_idle_callback(d->bridge.sl, delayed_idle, d) == SL_OK);
  CHECK(!sl_next_prompt(d->bridge.sl, "> ", &source) && !d->bridge.failed);
  CHECK(source == SL_PROMPT_SOURCE_NONE);
  if (kind == 6) {
    CHECK(d->phase == 8 && d->control_calls == 1 && d->bridge.calls == 2 &&
          sl_last_readline_status(d->bridge.sl) == SL_READLINE_SUSPENDED);
  } else if (kind < 2 || kind == 5) {
    CHECK(d->phase == 7 &&
          sl_last_readline_status(d->bridge.sl) == SL_READLINE_SUSPENDED);
    CHECK(!strcmp(sl_buffer(d->bridge.sl), kind == 5   ? d->paste + 6
                                           : kind == 1 ? "x\303\251\nz"
                                                       : "x\303\251"));
    CHECK(d->bridge.calls ==
          2); /* suspension never stops or aborts owner work */
    if (kind < 2)
      CHECK(d->control_calls == 1);
  } else {
    sl_queue_result_t stopped;
    CHECK(d->phase == 6 &&
          sl_last_readline_status(d->bridge.sl) == SL_READLINE_CANCELLED);
    if (kind >= 3) {
      /* The single ordinary slot still holds BEGIN_EDIT. Cleanup must reuse
       * that slot after its late acknowledgement, without opening a draft. */
      CHECK(d->bridge.request.operation == SL_QUEUE_BEGIN_EDIT);
    } else {
      CHECK(d->bridge.request.operation == SL_QUEUE_STOP);
      memset(&stopped, 0, sizeof(stopped));
      stopped.request_id = d->bridge.request.request_id;
      CHECK(sl_queue_backend_result(d->bridge.sl, &stopped) == SL_OK);
    }
    CHECK(delayed_result(d, d->original, kind >= 3) == 0);
    d->phase = 4;
    CHECK(!sl_next_prompt(d->bridge.sl, "> ", &source) && !d->bridge.failed);
    CHECK(d->phase == 5 &&
          sl_last_readline_status(d->bridge.sl) == SL_READLINE_EOF);
  }
  sl_destroy(d->bridge.sl);
  if (drainer > 0) {
    int status;
    CHECK(!kill(drainer, SIGTERM) && waitpid(drainer, &status, 0) == drainer);
  }
  close(d->bridge.master);
  close(slave);
  free(d->paste);
  free(d);
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "large-edit"))
    return delayed_edit(5);
  if (argc == 2 && !strcmp(argv[1], "controls"))
    return delayed_edit(6);
  return api() || failures() || fifo() || native() || native_edit(0) ||
         native_edit(1) || delayed_edit(0) || delayed_edit(1) ||
         delayed_edit(2) || delayed_edit(3) || delayed_edit(4);
}
