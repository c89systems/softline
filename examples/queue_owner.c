#define _POSIX_C_SOURCE 200809L
#include "queue_demo_wire.h"
#include <arpa/inet.h>
#include <errno.h>
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
} item_t;
typedef struct owner {
  demo_link_t link;
  item_t items[DEMO_ITEM_MAX];
  client_t clients[CLIENT_MAX];
  client_t *client;
  size_t count;
  unsigned long revision, serial, token;
  int stopped, auto_approval, active, piece, injection;
  unsigned long pace_ms;
  double next_piece, next_injection;
} owner_t;
static volatile sig_atomic_t quitting;
static void stop_server(int signum) {
  (void)signum;
  quitting = 1;
}
static double milliseconds(void) {
  struct timespec ts;
  (void)clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static char *duplicate(const char *text) {
  size_t n = strlen(text) + 1;
  char *copy = malloc(n);
  if (copy)
    memcpy(copy, text, n);
  return copy;
}
static void disconnect(owner_t *o) {
  demo_link_close(&o->link);
  o->client = NULL;
}
static void send_packet(owner_t *o, demo_packet_t *p) {
  if (o->link.fd >= 0 &&
      (demo_link_queue(&o->link, p) || demo_link_flush(&o->link)))
    disconnect(o);
  demo_packet_free(p);
}
static void text_event(owner_t *o, int kind, const char *text) {
  demo_packet_t p = {0};
  if (!o->client)
    return;
  demo_put_number(&p, (unsigned long)kind);
  demo_put_string(&p, text);
  send_packet(o, &p);
}
static sl_queue_entry_t exposed(owner_t *o, size_t index) {
  sl_queue_entry_t e = o->items[index].value;
  e.stopped = o->stopped;
  e.ready = !o->stopped && !e.held && !e.edit_held &&
            (!o->active || e.mode == SL_PROMPT_QUEUE_MODE_STEER || e.action);
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
  if (!o->client)
    return;
  memset(&c, 0, sizeof(c));
  c.kind = kind;
  c.previous_revision = previous;
  c.revision = revision;
  c.index = index;
  c.item_id = removed;
  c.request_id = request;
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
  item->value.action = action;
  touch(o, o->count - 1, request);
  return item;
}
static void emit_result(owner_t *o, const sl_queue_result_t *result) {
  demo_packet_t p = {0};
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
static void inject(owner_t *o) {
  char text[768];
  int n = ++o->injection;
  (void)snprintf(
      text, sizeof(text), "Remote %s request #%d: %s",
      n % 2 ? "queued" : "steering", n,
      n % 3 ? "Summarize the verification before continuing."
            : "Check that typing remains responsive while output is streaming, "
              "that queue and steering intent survive editing, "
              "and that approval decisions stay in arrival order. Disconnect "
              "the frontend while this request is held, then "
              "reattach and confirm that the same accepted request is still "
              "awaiting a decision.");
  (void)add(o, text,
            n % 2 ? SL_PROMPT_QUEUE_MODE_QUEUED : SL_PROMPT_QUEUE_MODE_STEER, 1,
            0, 0);
}
static int command(const char *text) {
  return !strcmp(text, "/status") || !strcmp(text, "/stop") ||
         !strcmp(text, "/resume") || !strcmp(text, "/approve") ||
         !strcmp(text, "/approve on") || !strcmp(text, "/approve off") ||
         !strcmp(text, "/inject");
}
static void handle_request(owner_t *o, const sl_queue_request_t *r) {
  sl_queue_result_t result;
  client_t *client = o->client;
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
    if (!r->text || !*r->text || strlen(r->text) > 4096 ||
        (r->mode != SL_PROMPT_QUEUE_MODE_QUEUED &&
         r->mode != SL_PROMPT_QUEUE_MODE_STEER))
      break;
    if ((*r->text == '/' && !command(r->text)) || *r->text == '!') {
      result.reason = "unsupported remote host command; /status /stop /resume "
                      "/approve /inject";
      break;
    }
    item = add(o, r->text,
               command(r->text) ? SL_PROMPT_QUEUE_MODE_QUEUED : r->mode,
               r->enabled, command(r->text), r->request_id);
    if (!item) {
      result.reason = "demo queue is full (64 entries)";
      break;
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
    if (!r->text || !*r->text || strlen(r->text) > 4096 ||
        (*r->text == '/' && !command(r->text)) || *r->text == '!')
      break;
    {
      char *text = duplicate(r->text);
      if (!text)
        break;
      free((void *)item->value.text);
      item->value.text = text;
      item->value.mode = command(text) ? SL_PROMPT_QUEUE_MODE_QUEUED : r->mode;
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
    item->value.mode =
        item->value.action ? SL_PROMPT_QUEUE_MODE_QUEUED : r->mode;
    touch(o, index, r->request_id);
    result.status = SL_QUEUE_ACCEPTED;
    break;
  case SL_QUEUE_STOP:
  case SL_QUEUE_RESUME:
    o->stopped = r->operation == SL_QUEUE_STOP;
    change(o, SL_QUEUE_STATE, 0, NULL, r->request_id);
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
static void hello(owner_t *o, const char *name, unsigned long pace_ms,
                  unsigned long request_high) {
  size_t i;
  if (!name || !*name || strlen(name) >= sizeof(o->clients[0].name) ||
      o->client || pace_ms > 1000) {
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
  o->pace_ms = pace_ms;
  change(o, SL_QUEUE_SNAPSHOT, 0, NULL, 0);
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
    if (kind == DEMO_HELLO) {
      const char *name = demo_string(&p);
      unsigned long pace_ms = demo_number(&p);
      unsigned long request_high = demo_number(&p);
      if (!p.failed && p.offset == p.length)
        hello(o, name, pace_ms, request_high);
      else
        disconnect(o);
    } else if (kind == DEMO_REQUEST && o->client) {
      sl_queue_request_t r;
      demo_get_request(&p, &r);
      if (!p.failed && p.offset == p.length)
        handle_request(o, &r);
      else
        disconnect(o);
    } else if (kind == DEMO_CANCEL && o->client && p.offset == p.length) {
      o->active = 0;
      text_event(o, DEMO_OUTPUT,
                 "\n[turn cancelled; backlog remains stopped]\n");
    } else
      disconnect(o);
    demo_packet_free(&p);
  }
}
static void host_action(owner_t *o, const char *text) {
  if (!strcmp(text, "/resume")) {
    o->stopped = 0;
    change(o, SL_QUEUE_STATE, 0, NULL, 0);
  } else if (!strcmp(text, "/stop")) {
    o->stopped = 1;
    change(o, SL_QUEUE_STATE, 0, NULL, 0);
  } else if (!strncmp(text, "/approve", 8))
    set_approval(o,
                 !strcmp(text, "/approve on")    ? 1
                 : !strcmp(text, "/approve off") ? 0
                                                 : !o->auto_approval,
                 0);
  else if (!strcmp(text, "/inject"))
    inject(o);
  else {
    char line[256];
    (void)snprintf(line, sizeof(line),
                   "\nOwner: %lu accepted, %s, auto-approval %s, turn %s.\n",
                   (unsigned long)o->count, o->stopped ? "stopped" : "running",
                   o->auto_approval ? "on" : "off",
                   o->active ? "active" : "idle");
    text_event(o, DEMO_OUTPUT, line);
  }
}
static void pump(owner_t *o, double now) {
  size_t i;
  /* Backend alone claims work at its turn/steering seams. */
  for (i = 0; i < o->count; ++i) {
    sl_queue_entry_t e = exposed(o, i);
    if (e.held || e.edit_held || (!e.action && !e.ready))
      continue;
    text_event(o, DEMO_PROMPT, e.text);
    if (e.action) {
      char *text = duplicate(e.text);
      if (!text)
        break;
      remove_item(o, i, 0);
      host_action(o, text);
      free(text);
    } else {
      remove_item(o, i, 0);
      if (o->active)
        text_event(o, DEMO_OUTPUT,
                   "\n[steering accepted at the owner output seam]\n");
      else {
        o->active = 1;
        o->piece = 0;
        o->next_piece = now;
      }
    }
    break;
  }
  if (o->active && now >= o->next_piece) {
    static const char *pieces[] = {
        "The queue ",       "belongs to ",        "the separate ",
        "owner process. ",  "Typing, editing, ",  "approvals and ",
        "terminal resize ", "remain available. ", "Disconnecting ",
        "this frontend ",   "does not stop ",     "accepted work.\n"};
    text_event(o, DEMO_OUTPUT, pieces[o->piece % 12]);
    o->next_piece = now + (double)o->pace_ms;
    if (++o->piece == 72) {
      o->active = 0;
      text_event(o, DEMO_OUTPUT_END, NULL);
    }
  }
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
int main(int argc, char **argv) {
  struct sockaddr_in address;
  socklen_t address_length = sizeof(address);
  int listener, yes = 1;
  char *end;
  long port = argc > 1 ? strtol(argv[1], &end, 10) : 7635;
  owner_t *o = calloc(1, sizeof(*o));
  const char *approval = getenv("SOFTLINE_CHAT_APPROVAL_DEMO");
  if (!o)
    return 1;
  if (argc > 2 || (argc > 1 && (!*argv[1] || *end)) || port < 0 ||
      port > 65535) {
    fprintf(stderr, "usage: %s [PORT (0..65535)]\n", argv[0]);
    free(o);
    return 1;
  }
  demo_link_init(&o->link, -1);
  o->revision = 1;
  o->pace_ms = 100;
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, stop_server);
  signal(SIGINT, stop_server);
  listener = socket(AF_INET, SOCK_STREAM, 0);
  if (listener < 0) {
    perror("socket");
    free(o);
    return 1;
  }
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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
  o->next_injection = milliseconds() + 12000;
  while (!quitting) {
    struct pollfd fds[2];
    double now = milliseconds(), due = now + 60000;
    int ready;
    if (o->active)
      due = o->next_piece;
    if ((!approval || strcmp(approval, "0")) && o->next_injection < due)
      due = o->next_injection;
    fds[0].fd = listener;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = o->link.fd;
    fds[1].events = POLLIN | (o->link.count ? POLLOUT : 0);
    fds[1].revents = 0;
    ready = poll(fds, 2, due <= now ? 0 : (int)(due - now));
    if (ready < 0 && errno != EINTR)
      break;
    if (fds[0].revents & POLLIN) {
      int fd = accept(listener, NULL, NULL);
      if (fd >= 0) {
        disconnect(o);
        if (demo_nonblock(fd))
          close(fd);
        else
          demo_link_init(&o->link, fd);
      }
    } else if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
      input(o);
    if ((fds[1].revents & POLLOUT) && o->link.fd >= 0 &&
        demo_link_flush(&o->link))
      disconnect(o);
    now = milliseconds();
    pump(o, now);
    if ((!approval || strcmp(approval, "0")) && now >= o->next_injection) {
      inject(o);
      o->next_injection = now + 12000;
    }
  }
  disconnect(o);
  close(listener);
  while (o->count) {
    free((void *)o->items[--o->count].value.text);
  }
  free(o);
  return 0;
}
