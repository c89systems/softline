#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "chat_charts.h"
#include "chat_demo_data.h"
#include "queue_demo_image.h"
#include "queue_demo_terminal.h"
#include "queue_demo_wire.h"
#include "web_chat.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/tcp.h>
#if defined(__APPLE__)
#include <netinet/tcp_fsm.h>
#endif
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* A deliberately volatile, single-controller example authority. Its queue and
 * idempotency records belong to this process, never to an attached editor. */
#define CLIENT_MAX 32
#define RESULT_MAX 16
typedef struct saved_result {
  sl_queue_result_t value;
  char id[128], revision[128], token[128], reason[128];
} saved_result_t;
typedef struct client {
  char name[64];
  unsigned long high;
  saved_result_t results[RESULT_MAX];
  size_t next;
} client_t;
typedef struct item {
  sl_queue_entry_t value;
  char id[128], revision[128], token[128], owner[64];
  int direct_start;
} item_t;
typedef struct owner {
  demo_web_chat_t *web;
  client_t web_client, *request_client;
  sl_queue_result_status_t web_status;
  int web_reasoning;
  int terminal_policy, terminal_sent, sent_terminal_policy;
  size_t sent_handoffs;
  demo_image_store_t image;
  demo_terminal_server_t *terminal, *closed_terminals;
  demo_link_t link;
  item_t items[DEMO_ITEM_MAX];
  client_t clients[CLIENT_MAX];
  client_t *client;
  size_t count;
  unsigned long revision, serial, token;
  int stopped, auto_approval, active, injection;
  int gated, input_empty, shell_pending, shell_requested, attach_wait_seam,
      initialized, demo_enabled, gate_fd, waiting_gate, stall_hello_once;
  unsigned int stream_initial_delay;
  enum demo_stream_mode stream_mode;
  int stream_has_owner_delay;
  unsigned long interval_ms;
  unsigned int turn_index;
  demo_charts_t charts;
  char chart[DEMO_CHART_CAPACITY];
  int chart_pending;
  const char *answer;
  size_t answer_pos;
  char previous;
  double next_piece, next_injection;
} owner_t;
static int web_submit(void *, const char *, int, sl_queue_request_id_t *);
static void terminal_lock_command(owner_t *, const char *);
static int web_control(void *, const char *, const char *, const char *);
static void web_snapshot(void *, demo_web_chat_t *);
static int owner_command(const char *text) {
  return is_local_command(text) || command_named(text, "/type");
}
static volatile sig_atomic_t quitting;
static volatile sig_atomic_t producer_paused;
static int completion_fd = -1;
/* Optional deterministic fixture control: queue service remains live while
 * response production is paused, just like the local demo's child producer. */
static void pause_producer(int signum) { producer_paused = signum == SIGUSR1; }
static void stop_server(int signum) {
  (void)signum;
  quitting = 1;
}
static double milliseconds(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* FIN can sit behind unread payload while the PTY stops accepting input.
 * Observe transport lifetime independently, without consuming/dropping data.
 * The existing PTY/reaping tick bounds this check while reads are paused. */
static int terminal_peer_closed(int fd) {
#if defined(__APPLE__)
  struct tcp_connection_info info;
  enum {
    option = TCP_CONNECTION_INFO,
    closed = TCPS_CLOSED,
    ended = TCPS_CLOSE_WAIT
  };
#else
  struct tcp_info info;
  enum { option = TCP_INFO, closed = TCP_CLOSE, ended = TCP_CLOSE_WAIT };
#endif
  socklen_t length = sizeof(info);
  memset(&info, 0, sizeof(info));
  return getsockopt(fd, IPPROTO_TCP, option, &info, &length) ||
         info.tcpi_state == closed || info.tcpi_state == ended;
}
static char *duplicate(const char *text) {
  size_t n = strlen(text) + 1;
  char *copy = malloc(n);
  if (copy)
    memcpy(copy, text, n);
  return copy;
}
static void disconnect(owner_t *o) {
  demo_image_store_close(&o->image);
  if (o->terminal) {
    sl_terminal_event_t event;
    memset(&event, 0, sizeof(event));
    event.operation = SL_TERMINAL_CLOSE;
    (void)sl_terminal_session_dispatch(o->terminal->session, &event);
    o->terminal->next = o->closed_terminals;
    o->closed_terminals = o->terminal;
    o->terminal = NULL;
    demo_www_set_terminal_handoffs(demo_web_listener(o->web), 0);
  }
  demo_link_close(&o->link);
  o->client = NULL;
  o->terminal_sent = 0;
  o->gated = o->shell_pending = o->shell_requested = 0;
  o->input_empty = 1;
}
static void send_packet(owner_t *o, demo_packet_t *p) {
  if (o->link.fd >= 0 &&
      (demo_link_queue(&o->link, p) || demo_link_flush(&o->link)))
    disconnect(o);
  demo_packet_free(p);
}
static void text_event(owner_t *o, int kind, const char *text) {
  demo_packet_t p = {0};
  if (o->web) {
    if (kind == DEMO_PROMPT)
      demo_web_message(o->web, "user", text);
    else if (kind == DEMO_RESPONSE_BEGIN) {
      demo_web_response_end(o->web);
      o->web_reasoning = 0;
      demo_web_status(o->web, "Thinking...", 1);
    } else if (kind == DEMO_OUTPUT) {
      demo_web_response(o->web, text, strlen(text));
      if (!o->web_reasoning && o->answer_pos >= 15) {
        o->web_reasoning = 1;
        demo_web_status(o->web, "Reasoning...", 1);
      }
    } else if (kind == DEMO_OUTPUT_END || kind == DEMO_SEGMENT_END) {
      demo_web_response_end(o->web);
      if (kind == DEMO_OUTPUT_END)
        demo_web_status(o->web, "", 0);
    } else if (kind == DEMO_MESSAGE)
      demo_web_status(o->web, text ? text : "", o->active);
    else if (kind == DEMO_NOTE)
      demo_web_message(o->web, "note", text ? text : "");
  }
  if (!o->client || o->shell_pending)
    return;
  demo_put_number(&p, (unsigned long)kind);
  if (kind == DEMO_OUTPUT)
    demo_put_bytes(&p, text, strlen(text));
  else
    demo_put_string(&p, text);
  send_packet(o, &p);
}
static void terminal_snapshot(owner_t *o) {
  size_t active = o->terminal && !o->terminal->eof ? 1 : 0;
  int enabled = o->terminal_policy;
  char text[64];
  if (!o->client || o->shell_pending)
    return;
  if (o->web)
    demo_www_terminal_status(demo_web_listener(o->web), &active, &enabled);
  if (o->terminal_sent && o->sent_handoffs == active &&
      o->sent_terminal_policy == enabled)
    return;
  (void)snprintf(text, sizeof(text), "%lu %d", (unsigned long)active, enabled);
  text_event(o, DEMO_TERMINAL_STATE, text);
  o->terminal_sent = o->client != NULL;
  o->sent_handoffs = active;
  o->sent_terminal_policy = enabled;
}
static void handoff_shell(owner_t *o, const char *text) {
  text_event(o, DEMO_SEGMENT_END, NULL);
  text_event(o, DEMO_SHELL, text);
  o->shell_requested = 0;
  o->shell_pending = 1;
}
static sl_queue_entry_t exposed(owner_t *o, size_t index) {
  sl_queue_entry_t e = o->items[index].value;
  e.ready = !e.held && !e.edit_held && !e.stopped &&
            (owner_command(e.text) || !o->active);
  if (!o->client || strcmp(o->items[index].owner, o->client->name))
    e.edit_token = NULL;
  return e;
}
static void change(owner_t *o, sl_queue_change_kind_t kind, size_t index,
                   const char *removed, sl_queue_request_id_t request) {
  demo_packet_t p = {0};
  sl_queue_change_t c;
  sl_queue_entry_t e;
  char previous[64], revision[64];
  (void)snprintf(previous, sizeof(previous), "q%lu", o->revision);
  if (kind != SL_QUEUE_SNAPSHOT)
    ++o->revision;
  (void)snprintf(revision, sizeof(revision), "q%lu", o->revision);
  demo_web_queue(o->web);
  if (!o->client || o->shell_pending)
    return;
  memset(&c, 0, sizeof(c));
  c.kind = kind;
  c.previous_revision = previous;
  c.revision = revision;
  c.index = index;
  c.item_id = removed;
  c.request_id = o->request_client ? 0 : request;
  c.total_count = o->count;
  c.stopped = o->stopped;
  c.auto_approval = o->auto_approval;
  if (kind == SL_QUEUE_UPSERT) {
    e = exposed(o, index);
    c.entries = &e;
    c.count = 1;
  }
  if (kind == SL_QUEUE_SNAPSHOT) {
    /* Avoid a multi-KiB automatic entries array on musl's small thread stack.
     */
    sl_queue_entry_t *entries = calloc(o->count + 1, sizeof(*entries));
    size_t i;
    if (!entries) {
      disconnect(o);
      return;
    }
    for (i = 0; i < o->count; ++i)
      entries[i] = exposed(o, i);
    c.entries = entries;
    c.count = o->count;
    demo_put_change(&p, &c);
    free(entries);
  } else
    demo_put_change(&p, &c);
  send_packet(o, &p);
}
static void touch(owner_t *o, size_t index, sl_queue_request_id_t request) {
  item_t *item = o->items + index;
  sl_queue_entry_t current = exposed(o, index);
  item->value.ready = current.ready;
  item->value.stopped = current.stopped;
  (void)snprintf(item->revision, sizeof(item->revision), "v%lu", ++o->serial);
  change(o, SL_QUEUE_UPSERT, index, NULL, request);
}
static void remove_item(owner_t *o, size_t index,
                        sl_queue_request_id_t request) {
  char id[128];
  size_t i;
  (void)snprintf(id, sizeof(id), "%s", o->items[index].id);
  free((void *)o->items[index].value.text);
  --o->count;
  memmove(o->items + index, o->items + index + 1,
          (o->count - index) * sizeof(*o->items));
  memset(o->items + o->count, 0, sizeof(*o->items));
  /* Moving structs invalidates pointers into their embedded token storage. */
  for (i = index; i < o->count; ++i) {
    item_t *it = o->items + i;
    it->value.id = it->id;
    it->value.revision = it->revision;
    it->value.edit_token = it->value.edit_held ? it->token : NULL;
  }
  change(o, SL_QUEUE_REMOVE, index, id, request);
}
static item_t *add(owner_t *o, const char *text, sl_prompt_queue_mode_t mode,
                   int held, int action, sl_queue_request_id_t request) {
  item_t *item;
  char *owned;
  if (o->count == DEMO_ITEM_MAX || !(owned = duplicate(text)))
    return NULL;
  item = o->items + o->count++;
  (void)snprintf(item->id, sizeof(item->id), "item-%lu", ++o->serial);
  item->value.id = item->id;
  item->value.revision = item->revision;
  item->value.text = owned;
  item->value.mode = mode;
  item->value.held = held && !o->auto_approval;
  item->value.stopped = request && !held ? 0 : o->stopped;
  item->value.action = action;
  touch(o, o->count - 1, request);
  return item;
}
static void emit_result(owner_t *o, const sl_queue_result_t *result) {
  demo_packet_t p = {0};
  if (o->request_client) {
    o->web_status = result->status;
    return;
  }
  demo_put_result(&p, result);
  send_packet(o, &p);
}
static void save_result(client_t *client, const sl_queue_result_t *r) {
  saved_result_t *saved = client->results + client->next;
  memset(saved, 0, sizeof(*saved));
  saved->value = *r;
#define SAVE(field, member)                                                    \
  do {                                                                         \
    if (r->field) {                                                            \
      (void)snprintf(saved->member, sizeof(saved->member), "%s", r->field);    \
      saved->value.field = saved->member;                                      \
    }                                                                          \
  } while (0)
  SAVE(item_id, id);
  SAVE(item_revision, revision);
  SAVE(edit_token, token);
  SAVE(reason, reason);
#undef SAVE
  client->next = (client->next + 1) % RESULT_MAX;
}
static saved_result_t *lookup(client_t *c, unsigned long id) {
  size_t i;
  for (i = 0; i < RESULT_MAX; ++i)
    if (c->results[i].value.request_id == id)
      return c->results + i;
  return NULL;
}
static void set_approval(owner_t *o, int enabled,
                         sl_queue_request_id_t request) {
  size_t i;
  o->auto_approval = enabled;
  for (i = 0; enabled && i < o->count; ++i)
    if (o->items[i].value.held) {
      o->items[i].value.held = 0;
      touch(o, i, request);
    }
  change(o, SL_QUEUE_STATE, 0, NULL, request);
}
static void set_stopped(owner_t *o, int stopped,
                        sl_queue_request_id_t request) {
  size_t i;
  o->stopped = stopped;
  for (i = 0; i < o->count; ++i)
    if (o->items[i].value.stopped != stopped) {
      sl_queue_entry_t current;
      o->items[i].value.stopped = stopped;
      current = exposed(o, i);
      o->items[i].value.ready = current.ready;
      change(o, SL_QUEUE_UPSERT, i, NULL, request);
    }
  change(o, SL_QUEUE_STATE, 0, NULL, request);
}
static void schedule_injection(owner_t *o, double now) {
  unsigned long interval = o->interval_ms;
  if (!interval)
    interval = 10000u + (unsigned int)rand() % 10001u;
  o->next_injection = now + (double)interval;
}
static void inject(owner_t *o) {
  char text[2048];
  int steering = (o->injection % 2) != 0;
  unsigned int shape = o->injection ? (unsigned int)rand() % 4u : 0u;
  const char *details = shape ? approval_details[shape]
                        : steering
                            ? "Focus the next answer on verification."
                            : "Summarize the next steps before continuing.";
  (void)snprintf(text, sizeof(text), "Demo %s request #%u: %s",
                 steering ? "steering" : "queued",
                 (unsigned int)o->injection + 1, details);
  if (add(o, text,
          steering ? SL_PROMPT_QUEUE_MODE_STEER : SL_PROMPT_QUEUE_MODE_QUEUED,
          1, 0, 0))
    ++o->injection;
}
static int command(const char *text) {
  return text[0] == '/' || text[0] == '!';
}
static void handle_request(owner_t *o, const sl_queue_request_t *r) {
  sl_queue_result_t result;
  client_t *client = o->request_client ? o->request_client : o->client;
  unsigned long previous_high;
  item_t *item = NULL;
  size_t index, i;
  saved_result_t *saved;
  char queue_revision[64];
  if (!client || !r->request_id) {
    disconnect(o);
    return;
  }
  memset(&result, 0, sizeof(result));
  result.request_id = r->request_id;
  result.status = SL_QUEUE_REJECTED;
  result.reason = "invalid request";
  if (r->request_id <= client->high) {
    saved = lookup(client, r->request_id);
    if (saved)
      emit_result(o, &saved->value);
    else {
      result.status = SL_QUEUE_UNKNOWN;
      result.reason = "result expired; request is never readmitted";
      emit_result(o, &result);
    }
    return;
  }
  previous_high = client->high;
  client->high = r->request_id;
  for (index = 0; index < o->count; ++index)
    if (r->item_id && !strcmp(r->item_id, o->items[index].id)) {
      item = o->items + index;
      break;
    }
  if (r->operation >= SL_QUEUE_BEGIN_EDIT &&
      r->operation <= SL_QUEUE_SET_INTENT) {
    if (!item || !r->item_revision ||
        strcmp(item->revision, r->item_revision)) {
      result.reason = "item was changed or claimed before this request";
      goto done;
    }
    if (item->value.edit_held && r->operation != SL_QUEUE_BEGIN_EDIT &&
        (!r->edit_token || strcmp(item->owner, client->name) ||
         strcmp(item->token, r->edit_token))) {
      result.reason = "edit hold belongs to another controller";
      goto done;
    }
    if ((r->operation == SL_QUEUE_END_EDIT ||
         r->operation == SL_QUEUE_ABORT_EDIT) &&
        !item->value.edit_held) {
      result.reason = "no owned edit hold";
      goto done;
    }
    if ((r->operation == SL_QUEUE_BEGIN_EDIT ||
         r->operation == SL_QUEUE_APPROVE ||
         r->operation == SL_QUEUE_WITHDRAW) &&
        item->value.held)
      for (i = 0; i < index; ++i)
        if (o->items[i].value.held) {
          result.reason = "approval decisions are FIFO";
          goto done;
        }
  }
  (void)snprintf(queue_revision, sizeof(queue_revision), "q%lu", o->revision);
  if (r->operation >= SL_QUEUE_STOP && r->operation <= SL_QUEUE_AUTO_APPROVAL &&
      (!r->queue_revision || strcmp(r->queue_revision, queue_revision))) {
    result.reason =
        "queue changed; inspect current state and retry deliberately";
    goto done;
  }
  switch (r->operation) {
  case SL_QUEUE_OFFER:
    if (!r->text || !*r->text || strlen(r->text) > DEMO_PROMPT_MAX ||
        (r->mode != SL_PROMPT_QUEUE_MODE_QUEUED &&
         r->mode != SL_PROMPT_QUEUE_MODE_STEER))
      break;
    item =
        add(o, r->text, r->mode, r->enabled, command(r->text), r->request_id);
    if (!item) {
      result.reason = "demo queue is full (64 entries)";
      break;
    }
    if (!o->active && o->stopped && !r->enabled && !command(r->text)) {
      item->direct_start = 1;
      set_stopped(o, 0, r->request_id);
    }
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_BEGIN_EDIT:
    item->value.edit_held = 1;
    (void)snprintf(item->owner, sizeof(item->owner), "%s", client->name);
    (void)snprintf(item->token, sizeof(item->token), "hold-%lu", ++o->token);
    item->value.edit_token = item->token;
    touch(o, index, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_END_EDIT:
    if (!r->text || !*r->text || strlen(r->text) > DEMO_PROMPT_MAX)
      break;
    {
      char *text = duplicate(r->text);
      if (!text)
        break;
      free((void *)item->value.text);
      item->value.text = text;
      item->value.mode = r->mode;
      item->value.action = command(text);
    }
    /* fall through */
  case SL_QUEUE_ABORT_EDIT:
    item->value.edit_held = 0;
    item->value.edit_token = NULL;
    item->owner[0] = 0;
    touch(o, index, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_APPROVE:
    item->value.held = 0;
    touch(o, index, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_WITHDRAW:
    result.item_id = item->id;
    result.item_revision = item->revision;
    /* Cache the identity before compacting the item array. */
    result.status = SL_QUEUE_ACCEPTED;
    result.reason = NULL;
    save_result(client, &result);
    remove_item(o, index, r->request_id);
    emit_result(
        o,
        &client->results[(client->next + RESULT_MAX - 1) % RESULT_MAX].value);
    return;
  case SL_QUEUE_SET_INTENT:
    item->value.mode = r->mode;
    if (o->stopped && !item->value.held && !item->value.edit_held) {
      item->direct_start = 1;
      set_stopped(o, 0, r->request_id);
    }
    touch(o, index, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_STOP:
  case SL_QUEUE_RESUME:
    set_stopped(o, r->operation == SL_QUEUE_STOP, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_CLEAR:
    while (o->count)
      remove_item(o, o->count - 1, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_AUTO_APPROVAL:
    set_approval(o, r->enabled, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_SYNCHRONIZE:
    change(o, SL_QUEUE_SNAPSHOT, 0, NULL, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_RESOLVE:
    saved = lookup(client, r->target_request_id);
    if (saved)
      emit_result(o, &saved->value);
    else {
      sl_queue_result_t unknown;
      memset(&unknown, 0, sizeof(unknown));
      unknown.request_id = r->target_request_id;
      unknown.status = r->target_request_id > previous_high ? SL_QUEUE_REJECTED
                                                            : SL_QUEUE_UNKNOWN;
      unknown.reason = "original result unavailable; not automatically retried";
      if (unknown.status == SL_QUEUE_REJECTED)
        save_result(client, &unknown);
      emit_result(o, &unknown);
    }
    result.status = SL_QUEUE_ACCEPTED;
    break;
  default:
    break;
  }
done:
  if (result.status == SL_QUEUE_ACCEPTED) {
    result.reason = NULL;
    if (item) {
      result.item_id = item->id;
      result.item_revision = item->revision;
      result.edit_token = item->value.edit_token;
    }
  }
  save_result(client, &result);
  emit_result(
      o, &client->results[(client->next + RESULT_MAX - 1) % RESULT_MAX].value);
}
static int web_request(owner_t *o, sl_queue_request_t *r) {
  if (o->web_client.high == ULONG_MAX)
    return SL_ERROR_FULL;
  r->request_id = o->web_client.high + 1;
  o->request_client = &o->web_client;
  o->web_status = SL_QUEUE_REJECTED;
  handle_request(o, r);
  o->request_client = NULL;
  return o->web_status == SL_QUEUE_ACCEPTED ? SL_OK : SL_ERROR_CONFLICT;
}
static int web_submit(void *context, const char *text, int steer,
                      sl_queue_request_id_t *request_id) {
  owner_t *o = context;
  sl_queue_request_t r;
  (void)request_id;
  if (command_named(text, "/terminal-lock")) {
    terminal_lock_command(o, text);
    return SL_OK;
  }
  if (is_shell_command(text) || !strcmp(text, "/quit") ||
      command_named(text, "/type"))
    return SL_ERROR_INVALID;
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_OFFER;
  r.mode = steer ? SL_PROMPT_QUEUE_MODE_STEER : SL_PROMPT_QUEUE_MODE_QUEUED;
  r.text = text;
  return web_request(o, &r);
}
static int web_control(void *context, const char *action, const char *id,
                       const char *revision) {
  owner_t *o = context;
  sl_queue_request_t r;
  if (!strcmp(action, "cancel")) {
    o->active = o->waiting_gate = o->shell_requested = 0;
    set_stopped(o, 1, 0);
    text_event(o, DEMO_OUTPUT_END, NULL);
    return SL_OK;
  }
  memset(&r, 0, sizeof(r));
  if (!strcmp(action, "approve"))
    r.operation = SL_QUEUE_APPROVE;
  else if (!strcmp(action, "reject"))
    r.operation = SL_QUEUE_WITHDRAW;
  else
    return SL_ERROR_INVALID;
  r.item_id = id;
  r.item_revision = revision;
  return web_request(o, &r);
}
static void web_snapshot(void *context, demo_web_chat_t *web) {
  owner_t *o = context;
  size_t i;
  demo_web_queue_begin(web);
  for (i = 0; i < o->count; ++i) {
    const sl_queue_entry_t *e = &o->items[i].value;
    demo_web_queue_entry(web, e->id, e->revision, e->text,
                         e->mode == SL_PROMPT_QUEUE_MODE_STEER, e->held);
  }
  demo_web_queue_end(web);
}
static int web_edit(void *context, const char *id, const char *revision,
                    const char *text, const char *token,
                    sl_prompt_queue_mode_t mode, sl_queue_request_id_t *out) {
  owner_t *o = context;
  sl_queue_request_t r;
  size_t i;
  int status;
  (void)token;
  (void)out;
  if (is_shell_command(text) || !strcmp(text, "/quit"))
    return SL_ERROR_UNSUPPORTED;
  for (i = 0; i < o->count; ++i)
    if (!strcmp(o->items[i].id, id) && o->items[i].value.edit_held)
      return SL_ERROR_CONFLICT;
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_BEGIN_EDIT;
  r.item_id = id;
  r.item_revision = revision;
  status = web_request(o, &r);
  if (status != SL_OK)
    return status;
  for (i = 0; i < o->count && strcmp(o->items[i].id, id); ++i) {
  }
  if (i == o->count)
    return SL_ERROR_CONFLICT;
  r.operation = SL_QUEUE_END_EDIT;
  r.item_revision = o->items[i].revision;
  r.edit_token = o->items[i].token;
  r.mode = mode;
  r.text = text;
  status = web_request(o, &r);
  if (status != SL_OK) {
    r.operation = SL_QUEUE_ABORT_EDIT;
    r.text = NULL;
    (void)web_request(o, &r);
  }
  return status;
}
static void hello(owner_t *o, const char *name, unsigned long delay_us,
                  unsigned long request_high, int approvals,
                  unsigned long interval_ms, unsigned int seed) {
  size_t i;
  /* Optional fixture: TCP succeeds but the first subscription never replies. */
  if (o->stall_hello_once) {
    o->stall_hello_once = 0;
    return;
  }
  if (!name || !*name || strlen(name) >= sizeof(o->clients[0].name) ||
      o->client || delay_us > 60000000 || approvals < 0 || approvals > 1 ||
      interval_ms > 60000) {
    disconnect(o);
    return;
  }
  for (i = 0; i < CLIENT_MAX; ++i)
    if (!strcmp(o->clients[i].name, name)) {
      o->client = o->clients + i;
      break;
    }
  if (!o->client)
    for (i = 0; i < CLIENT_MAX; ++i)
      if (!*o->clients[i].name) {
        o->client = o->clients + i;
        /* On a new/restarted volatile owner, all earlier frontend identities
         * are uncertain, never readmitted or declared definitely rejected.
         * Existing namespaces keep their actual observed watermark, allowing
         * an unsent proposal on this owner to be resolved as unaccepted. */
        o->client->high = request_high;
        (void)snprintf(o->client->name, sizeof(o->client->name), "%s", name);
        break;
      }
  if (!o->client) {
    disconnect(o);
    return;
  }
  if (!o->stream_has_owner_delay && o->stream_mode == DEMO_STREAM_DEFAULT)
    o->stream_initial_delay = (unsigned int)delay_us;
  o->interval_ms = interval_ms;
  if (!o->initialized) {
    srand(seed);
    schedule_injection(o, milliseconds());
    o->initialized = 1;
  }
  o->demo_enabled = approvals;
  o->gated = 1;
  o->attach_wait_seam = o->active;
  change(o, SL_QUEUE_SNAPSHOT, 0, NULL, 0);
  if (o->active)
    text_event(o, DEMO_RESPONSE_BEGIN, NULL);
  else
    text_event(o, DEMO_OUTPUT_END, NULL);
}
static void input(owner_t *o) {
  int n;
  for (n = 0; n < 8 && o->link.fd >= 0; ++n) {
    demo_packet_t p = {0};
    int status = demo_link_read(&o->link, &p);
    unsigned long kind;
    if (!status)
      break;
    if (status < 0) {
      disconnect(o);
      break;
    }
    kind = demo_number(&p);
    if (kind == DEMO_PTY_OPEN && !o->client && !o->terminal) {
      o->link.input_limit = 8192;
      o->terminal = calloc(1, sizeof(*o->terminal));
      if (!o->terminal) {
        demo_packet_free(&p);
        disconnect(o);
        break;
      }
      o->terminal->wait.fd = -1;
      if (demo_terminal_server_input(o->terminal, &o->link, kind, &p))
        disconnect(o);
    } else if (o->terminal) {
      if (demo_terminal_server_input(o->terminal, &o->link, kind, &p))
        disconnect(o);
    } else if (kind == DEMO_HELLO) {
      const char *name = demo_string(&p);
      unsigned long delay_us = demo_number(&p);
      unsigned long request_high = demo_number(&p);
      int approvals = (int)demo_number(&p);
      unsigned long interval_ms = demo_number(&p);
      unsigned int seed = (unsigned int)demo_number(&p);
      if (demo_packet_done(&p))
        hello(o, name, delay_us, request_high, approvals, interval_ms, seed);
      else
        disconnect(o);
    } else if (kind == DEMO_REQUEST && o->client) {
      sl_queue_request_t r;
      o->gated = 1;
      demo_get_request(&p, &r);
      if (demo_packet_done(&p))
        handle_request(o, &r);
      else
        disconnect(o);
    } else if (kind >= DEMO_IMAGE_BEGIN && kind <= DEMO_IMAGE_DONE &&
               o->client && !o->shell_pending) {
      if (demo_image_store_input(&o->image, &o->link, kind, &p, milliseconds()))
        disconnect(o);
    } else if (kind == DEMO_CANCEL && o->client && demo_packet_done(&p)) {
      o->active = o->waiting_gate = o->shell_requested = 0;
      set_stopped(o, 1, 0);
      text_event(o, DEMO_OUTPUT_END, NULL);
    } else if (kind == DEMO_IDLE && o->client) {
      o->input_empty = (int)demo_number(&p);
      if (!demo_packet_done(&p))
        disconnect(o);
      else
        o->gated = 0;
    } else if (kind == DEMO_SHELL_BEGIN && o->client && demo_packet_done(&p)) {
      o->shell_requested = 1;
    } else
      disconnect(o);
    demo_packet_free(&p);
    if (o->terminal && o->terminal->input_len)
      break;
  }
}

static void start_response(owner_t *o, double now) {
  o->active = 1;
  o->answer =
      response_templates[o->turn_index++ % (sizeof(response_templates) /
                                            sizeof(response_templates[0]))];
  o->answer_pos = 0;
  o->chart_pending =
      demo_chart_next(&o->charts, o->chart, sizeof(o->chart)) > 0;
  o->previous = 0;
  o->next_piece = now;
  o->attach_wait_seam = 0;
  o->waiting_gate = o->gate_fd >= 0;
  text_event(o, DEMO_RESPONSE_BEGIN, NULL);
}
static void terminal_lock_command(owner_t *o, const char *text) {
  demo_www_t *web = demo_web_listener(o->web);
  size_t active;
  int enabled = o->terminal_policy;
  if (web)
    demo_www_terminal_status(web, &active, &enabled);
  enabled = terminal_lock_setting(text, enabled);
  if (enabled < 0) {
    text_event(o, DEMO_MESSAGE, "Usage: /terminal-lock [on|off]");
    return;
  }
  o->terminal_policy = enabled;
  demo_www_set_terminal_prompt_lock(web, o->terminal_policy);
  text_event(o, DEMO_MESSAGE,
             o->terminal_policy ? "Terminal prompt lock on."
                                : "Terminal prompt lock off.");
}
static int type_delay(const char **source, unsigned long maximum,
                      unsigned long *value) {
  char *end;
  if (**source < '0' || **source > '9')
    return -1;
  errno = 0;
  *value = strtoul(*source, &end, 10);
  if (errno || *value > maximum || *end != ':')
    return -1;
  *source = end + 1;
  return 0;
}

static void type_command(owner_t *o, const char *text) {
  sl_prompt_simulation_options_t options = {0, 30, 0,
                                            SL_PROMPT_QUEUE_MODE_QUEUED};
  unsigned long seconds = SL_PROMPT_SIMULATION_DELAY_MAX_MS / 1000;
  int status;
  text += sizeof("/type") - 1;
  while (*text == ' ')
    ++text;
  if (*text == '{') {
    ++text;
    if (type_delay(&text, seconds, &options.start_delay_ms) ||
        type_delay(&text, SL_PROMPT_SIMULATION_DELAY_MAX_MS,
                   &options.character_delay_ms) ||
        type_delay(&text, seconds, &options.submit_delay_ms) ||
        (*text != 'q' && *text != 's') || text[1] != '}' || text[2] != ' ')
      goto usage;
    options.mode =
        *text == 's' ? SL_PROMPT_QUEUE_MODE_STEER : SL_PROMPT_QUEUE_MODE_QUEUED;
    options.start_delay_ms *= 1000;
    options.submit_delay_ms *= 1000;
    text += 3;
  }
  if (!text[strspn(text, " \t\n")])
    goto usage;
  status = demo_web_simulate_prompt(o->web, text, &options);
  text_event(o, DEMO_MESSAGE,
             status == SL_OK ? "Prompt sent to browser."
             : status == SL_ERROR_NOT_READY
                 ? "No browser is available for /type."
                 : "Could not send simulated prompt to browser.");
  return;
usage:
  text_event(
      o, DEMO_MESSAGE,
      "Usage: /type [{start-seconds:character-ms:submit-seconds:q|s}] prompt");
}

static void host_action(owner_t *o, const char *text, int at_seam, double now) {
  if (command_named(text, "/terminal-lock")) {
    terminal_lock_command(o, text);
  } else if (command_named(text, "/type")) {
    type_command(o, text);
  } else if (command_named(text, "/stream")) {
    unsigned int delay =
        demo_stream_delay(o->stream_mode, o->stream_initial_delay);
    enum demo_stream_mode mode = demo_stream_setting(text, delay);
    if (mode != DEMO_STREAM_INVALID) {
      o->stream_mode = mode;
      if (demo_stream_delay(mode, o->stream_initial_delay) != delay)
        o->next_piece = now;
    }
    text_event(o, DEMO_MESSAGE, demo_stream_message(mode));
  } else if (!strcmp(text, "/status")) {
    char line[256];
    (void)snprintf(line, sizeof(line),
                   "Status: %s / backlog %s / %lu queued / auto-approval %s",
                   o->active ? "busy" : "idle",
                   o->stopped ? "paused" : "running", (unsigned long)o->count,
                   o->auto_approval ? "on" : "off");
    text_event(o, DEMO_MESSAGE, line);
  } else if (!strcmp(text, "/resume")) {
    set_stopped(o, 0, 0);
    text_event(o, DEMO_MESSAGE, "Backlog resumed.");
  } else if (command_named(text, "/approve")) {
    int enabled = !o->auto_approval;
    if (!strcmp(text, "/approve on"))
      enabled = 1;
    else if (!strcmp(text, "/approve off"))
      enabled = 0;
    else if (strcmp(text, "/approve")) {
      text_event(o, DEMO_MESSAGE, "Usage: /approve [on|off]");
      return;
    }
    set_approval(o, enabled, 0);
    text_event(o, DEMO_MESSAGE,
               enabled ? "Auto-approval ON (all waiting requests approved)."
                       : "Auto-approval OFF (new requests need approval).");
  } else if (!strcmp(text, "/quit")) {
    text_event(o, DEMO_QUIT, NULL);
  } else if (is_shell_command(text)) {
    handoff_shell(o, text);
  } else {
    text_event(o, DEMO_PROMPT, text);
    if (command_named(text, "/steer"))
      text_event(o, DEMO_NOTE, "Steering applied.");
    else if (!at_seam)
      start_response(o, now);
  }
}
static int ready(owner_t *o, item_t *item, int at_seam) {
  if (item->value.held || item->value.edit_held)
    return 0;
  /* Conversation settings can change even while prompt dispatch is gated. */
  if (command_named(item->value.text, "/terminal-lock") ||
      command_named(item->value.text, "/stream") ||
      command_named(item->value.text, "/type"))
    return 1;
  if (o->gated)
    return 0;
  if (owner_command(item->value.text))
    return 1;
  if (item->value.stopped)
    return 0;
  if (!o->active)
    return !is_shell_command(item->value.text) ||
           (o->client && !o->shell_pending && o->input_empty);
  return at_seam && item->value.mode == SL_PROMPT_QUEUE_MODE_STEER &&
         !is_shell_command(item->value.text);
}
static int eligible(owner_t *o, size_t index, int at_seam) {
  const sl_queue_entry_t *after = &o->items[index].value;
  size_t i;
  if (!ready(o, o->items + index, at_seam))
    return 0;
  if (owner_command(after->text))
    return 1;
  for (i = 0; i < index; ++i) {
    const sl_queue_entry_t *before = &o->items[i].value;
    if (owner_command(before->text) || before->stopped)
      continue;
    if (!before->action &&
        (before->held || (after->mode == SL_PROMPT_QUEUE_MODE_STEER &&
                          before->mode == SL_PROMPT_QUEUE_MODE_QUEUED)))
      continue;
    return 0;
  }
  return 1;
}
static void claim_ready(owner_t *o, int at_seam, double now) {
  size_t i, selected, budget = DEMO_ITEM_MAX;
  int segment_ended = 0;
  while (budget--) {
    selected = o->count;
    for (i = 0; i < o->count; ++i)
      if (eligible(o, i, at_seam)) {
        if (selected == o->count || (!o->active && o->items[i].direct_start))
          selected = i;
        if (o->active || o->items[i].direct_start)
          break;
      }
    if (selected == o->count)
      return;
    if (at_seam && !segment_ended) {
      text_event(o, DEMO_SEGMENT_END, NULL);
      segment_ended = 1;
    }
    {
      char *text = duplicate(o->items[selected].value.text);
      int was_active = o->active;
      if (!text)
        return;
      remove_item(o, selected, 0);
      host_action(o, text, was_active, now);
      free(text);
      if (!was_active && o->active)
        return;
    }
  }
}
static void refresh(owner_t *o) {
  size_t i;
  for (i = 0; i < o->count; ++i) {
    sl_queue_entry_t current = exposed(o, i);
    if (o->items[i].value.ready != current.ready ||
        o->items[i].value.stopped != current.stopped) {
      o->items[i].value.ready = current.ready;
      o->items[i].value.stopped = current.stopped;
      change(o, SL_QUEUE_UPSERT, i, NULL, 0);
    }
  }
}
static void pump(owner_t *o, double now) {
  char chunk[DEMO_STREAM_DIRECT_BYTES + 1];
  size_t used = 0, produced = 0, budget;
  unsigned int delay;
  if (o->shell_requested && !o->active)
    handoff_shell(o, NULL);
  claim_ready(o, 0, now);
  delay = demo_stream_delay(o->stream_mode, o->stream_initial_delay);
  budget = now < o->next_piece ? 0
           : delay             ? DEMO_STREAM_LLM_BYTES
                               : DEMO_STREAM_DIRECT_BYTES;
  while (o->active && !o->waiting_gate && !producer_paused &&
         produced < budget) {
    char byte = o->answer[o->answer_pos++];
    if (!o->attach_wait_seam)
      chunk[used++] = byte;
    o->next_piece = now + (double)delay * ++produced / 1000.0;
    if (byte == '\n' && o->previous == '\n') {
      if (used) {
        chunk[used] = 0;
        text_event(o, DEMO_OUTPUT, chunk);
        used = 0;
      }
      o->attach_wait_seam = 0;
      claim_ready(o, 1, now);
      if (o->shell_requested)
        handoff_shell(o, NULL);
    }
    o->previous = byte;
    if (!o->answer[o->answer_pos] && o->chart_pending) {
      o->answer = o->chart;
      o->answer_pos = 0;
      o->chart_pending = 0;
    }
    if (!o->answer[o->answer_pos]) {
      if (used) {
        chunk[used] = 0;
        text_event(o, DEMO_OUTPUT, chunk);
        used = 0;
      }
      o->active = 0;
      if (completion_fd >= 0) {
        ssize_t notified = write(completion_fd, "\n", 1);
        (void)notified;
      }
      text_event(o, DEMO_OUTPUT_END, NULL);
      if (o->shell_requested)
        handoff_shell(o, NULL);
      claim_ready(o, 0, now);
      break;
    }
  }
  if (used) {
    chunk[used] = 0;
    text_event(o, DEMO_OUTPUT, chunk);
  }
  refresh(o);
}
int main(int argc, char **argv) {
  struct sockaddr_in address;
  socklen_t address_length = sizeof(address);
  int listener, yes = 1;
  char *end;
  long port = argc > 1 ? strtol(argv[1], &end, 10) : 7635;
  owner_t *o = calloc(1, sizeof(*o));
  const char *gate = getenv("SOFTLINE_CHAT_GATE_FD");
  const char *stream_delay = getenv("SOFTLINE_CHAT_CHAR_MS");
  if (!o)
    return 1;
  if (argc > 2 || (argc > 1 && (!*argv[1] || *end)) || port < 0 ||
      port > 65535) {
    fprintf(stderr, "usage: %s [PORT (0..65535)]\n", argv[0]);
    free(o);
    return 1;
  }
  demo_link_init(&o->link, -1);
  demo_charts_init(&o->charts,
                   (unsigned long)time(NULL) ^ (unsigned long)getpid());
  if (demo_image_store_init(&o->image)) {
    free(o);
    return 1;
  }
  o->revision = 1;
  o->stream_initial_delay = demo_stream_initial_delay(stream_delay);
  o->stream_has_owner_delay = stream_delay && *stream_delay;
  o->gate_fd = -1;
  if (getenv("SOFTLINE_CHAT_TEST_COMPLETION_FD")) {
    const char *value = getenv("SOFTLINE_CHAT_TEST_COMPLETION_FD");
    long fd = strtol(value, &end, 10);
    if (!*value || *end || fd < 3 || fd > INT_MAX || demo_nonblock((int)fd)) {
      free(o);
      return 1;
    }
    completion_fd = (int)fd;
  }
  o->stall_hello_once = getenv("SOFTLINE_CHAT_TEST_STALL_HELLO") != NULL;
  o->input_empty = 1;
  if (demo_www_terminal_lock_environment(&o->terminal_policy)) {
    perror("terminal prompt policy");
    free(o);
    return 1;
  }
  (void)snprintf(o->web_client.name, sizeof(o->web_client.name), "web");
  if (gate) {
    long fd = strtol(gate, &end, 10);
    if (!*gate || *end || fd < 3 || fd > 9) {
      fprintf(stderr, "SOFTLINE_CHAT_GATE_FD must be an inherited FD 3..9\n");
      free(o);
      return 1;
    }
    o->gate_fd = (int)fd;
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, stop_server);
  signal(SIGINT, stop_server);
  if (getenv("SOFTLINE_CHAT_TEST_SIGNALS")) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = pause_producer;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) ||
        sigaction(SIGUSR2, &action, NULL)) {
      perror("fixture signal setup");
      free(o);
      return 1;
    }
  }
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    perror("socket");
    free(o);
    return 1;
  }
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(0x7f000001UL);
  address.sin_port = htons((unsigned short)port);
  if (bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
      listen(listener, 4) || demo_nonblock(listener)) {
    perror("queue owner listen");
    close(listener);
    free(o);
    return 1;
  }
  (void)getsockname(listener, (struct sockaddr *)&address, &address_length);
  printf("LISTEN 127.0.0.1 %u\n", (unsigned)ntohs(address.sin_port));
  fflush(stdout);
  {
    demo_web_ops_t ops;
    int enabled;
    memset(&ops, 0, sizeof(ops));
    ops.submit = web_submit;
    ops.control = web_control;
    ops.edit = web_edit;
    ops.snapshot = web_snapshot;
    ops.terminal = 1;
    ops.terminal_unlock_command = "/terminal-lock off";
    o->web = demo_web_open(NULL, &ops, o, 8081, &enabled);
    if (enabled && !o->web) {
      perror("web listener");
      close(listener);
      free(o);
      return 1;
    }
    if (o->web) {
      printf("WEB http://127.0.0.1:%u\n", (unsigned)demo_web_port(o->web));
      fflush(stdout);
    }
  }
  while (!quitting) {
    struct pollfd fds[4 + DEMO_WWW_FDS];
    double now = milliseconds(), due = now + 60000;
    double image_due = demo_image_store_due(&o->image);
    int ready, web_fds = 0, timeout;
    if (image_due && image_due < due)
      due = image_due;
    if (o->active && !o->waiting_gate && !producer_paused &&
        o->next_piece < due)
      due = o->next_piece;
    if (o->demo_enabled && o->next_injection < due)
      due = o->next_injection;
    if ((o->terminal || o->closed_terminals) && due > now + 20)
      due = now + 20;
    fds[0].fd = listener;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = o->link.fd;
    fds[1].events = ((!o->terminal || !o->terminal->input_len) ? POLLIN : 0) |
                    (o->link.count ? POLLOUT : 0);
    fds[1].revents = 0;
    fds[2].fd = o->waiting_gate ? o->gate_fd : -1;
    fds[2].events = POLLIN;
    fds[2].revents = 0;
    fds[3].fd = o->terminal && !o->terminal->eof ? o->terminal->wait.fd : -1;
    fds[3].events = (o->link.count < 2 ? POLLIN : 0) |
                    (o->terminal && o->terminal->input_len ? POLLOUT : 0);
    if (!fds[3].events)
      fds[3].fd = -1;
    fds[3].revents = 0;
    if (o->web)
      web_fds = demo_www_descriptors(demo_web_listener(o->web), fds + 4,
                                     DEMO_WWW_FDS);
    if (web_fds < 0)
      break;
    timeout = demo_web_timeout(o->web, due <= now ? 0 : (int)(due - now) + 1);
    ready = poll(fds, (nfds_t)(4 + web_fds), timeout);
    if (ready < 0 && errno != EINTR)
      break;
    if (o->terminal && o->terminal->input_len &&
        terminal_peer_closed(o->link.fd))
      disconnect(o);
    if (fds[0].revents & POLLIN) {
      int fd = accept(listener, NULL, NULL);
      if (fd >= 0) {
        disconnect(o);
        if (demo_nonblock(fd))
          close(fd);
        else {
          int enabled = 1;
          (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled,
                           sizeof(enabled));
          demo_link_init(&o->link, fd);
        }
      }
    } else if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
      input(o);
    if ((fds[1].revents & POLLOUT) && o->link.fd >= 0 &&
        demo_link_flush(&o->link))
      disconnect(o);
    if (fds[2].revents & (POLLIN | POLLHUP)) {
      char token;
      ssize_t got = read(o->gate_fd, &token, 1);
      if (got == 1 && token == '\n')
        o->waiting_gate = 0;
      else if (got == 0) {
        o->waiting_gate = o->active = 0;
        text_event(o, DEMO_OUTPUT_END, NULL);
      }
    }
    now = milliseconds();
    if (demo_image_store_tick(&o->image, &o->link, now))
      disconnect(o);
    if (o->terminal && demo_terminal_server_pump(o->terminal, &o->link))
      disconnect(o);
    demo_www_set_terminal_handoffs(demo_web_listener(o->web),
                                   o->terminal && !o->terminal->eof ? 1 : 0);
    {
      demo_terminal_server_t **at = &o->closed_terminals;
      while (*at) {
        demo_terminal_server_t *s = *at;
        if (sl_terminal_session_destroy(s->session) == SL_OK) {
          *at = s->next;
          free(s);
        } else
          at = &s->next;
      }
    }
    if (demo_web_tick(o->web))
      break;
    terminal_snapshot(o);
    pump(o, now);
    if (o->demo_enabled && now >= o->next_injection) {
      schedule_injection(o, now);
      if (o->count < 4)
        inject(o);
    }
  }
  demo_web_close(o->web);
  o->web = NULL;
  disconnect(o);
  {
    int remaining = 100;
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 1000000;
    while (o->closed_terminals) {
      demo_terminal_server_t *s = o->closed_terminals;
      if (sl_terminal_session_destroy(s->session) == SL_ERROR_NOT_READY &&
          remaining-- > 0) {
        (void)nanosleep(&delay, NULL);
        continue;
      }
      o->closed_terminals = s->next;
      free(s);
    }
  }
  close(listener);
  while (o->count) {
    free((void *)o->items[--o->count].value.text);
  }
  free(o);
  return 0;
}
