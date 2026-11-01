#define _DEFAULT_SOURCE
#include "queue_demo_client.h"
#include "queue_demo_wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%d: %s\n", __LINE__, #x);                               \
      return 1;                                                                \
    }                                                                          \
  } while (0)
typedef struct record {
  char id[128], revision[128], token[128], text[4097];
  int held, edit_held;
} record_t;
typedef struct peer {
  demo_link_t link;
  record_t entries[DEMO_ITEM_MAX];
  sl_queue_entry_t scratch[DEMO_ITEM_MAX];
  size_t count;
  int stopped, approval;
  char revision[128], result_id[128], result_revision[128], result_token[128];
  unsigned long serial, result;
  unsigned long watch_target;
  int watched;
  sl_queue_result_status_t watched_status;
  sl_queue_result_status_t status;
  int active, shells;
  size_t source_bytes;
  char last_source[3];
  char source_tail[64];
  unsigned long delay_us;
} peer_t;
static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static void copy(char *out, size_t size, const char *in) {
  snprintf(out, size, "%s", in ? in : "");
}
static int wait_fd(int fd, short events, double deadline) {
  struct pollfd p;
  int rc;
  double left = deadline - now();
  if (left <= 0)
    return -1;
  p.fd = fd;
  p.events = events;
  p.revents = 0;
  do {
    rc = poll(&p, 1, (int)left + 1);
  } while (rc < 0 && errno == EINTR);
  return rc > 0 ? 0 : -1;
}
static int flush(peer_t *p) {
  double deadline = now() + 2000;
  while (p->link.count) {
    if (demo_link_flush(&p->link))
      return -1;
    if (p->link.count && wait_fd(p->link.fd, POLLOUT, deadline))
      return -1;
  }
  return 0;
}
static int receive(peer_t *p) {
  demo_packet_t packet = {0};
  unsigned long kind;
  int rc;
  double deadline = now() + 2000;
  for (;;) {
    rc = demo_link_read(&p->link, &packet);
    if (rc)
      break;
    if (wait_fd(p->link.fd, POLLIN, deadline))
      return -1;
  }
  if (rc < 0)
    return -1;
  kind = demo_number(&packet);
  if (kind == DEMO_CHANGE) {
    sl_queue_change_t c;
    size_t i;
    if (demo_get_change(&packet, &c, p->scratch, DEMO_ITEM_MAX))
      goto fail;
    if (c.kind == SL_QUEUE_SNAPSHOT)
      p->count = c.count;
    else if (c.kind == SL_QUEUE_UPSERT) {
      if (c.index > p->count)
        goto fail;
      if (c.total_count > p->count) {
        if (p->count == DEMO_ITEM_MAX)
          goto fail;
        memmove(p->entries + c.index + 1, p->entries + c.index,
                (p->count - c.index) * sizeof(*p->entries));
        ++p->count;
      }
    } else if (c.kind == SL_QUEUE_REMOVE) {
      for (i = 0; i < p->count; ++i)
        if (!strcmp(p->entries[i].id, c.item_id))
          break;
      if (i == p->count)
        goto fail;
      --p->count;
      memmove(p->entries + i, p->entries + i + 1,
              (p->count - i) * sizeof(*p->entries));
    }
    for (i = 0; i < c.count; ++i) {
      record_t *r = p->entries + (c.kind == SL_QUEUE_SNAPSHOT ? i : c.index);
      const sl_queue_entry_t *e = c.entries + i;
      copy(r->id, sizeof(r->id), e->id);
      copy(r->revision, sizeof(r->revision), e->revision);
      copy(r->text, sizeof(r->text), e->text);
      copy(r->token, sizeof(r->token), e->edit_token);
      r->held = e->held;
      r->edit_held = e->edit_held;
    }
    copy(p->revision, sizeof(p->revision), c.revision);
    p->stopped = c.stopped;
    p->approval = c.auto_approval;
  } else if (kind == DEMO_RESULT) {
    sl_queue_result_t r;
    demo_get_result(&packet, &r);
    p->result = r.request_id;
    p->status = r.status;
    if (r.request_id == p->watch_target) {
      p->watched = 1;
      p->watched_status = r.status;
    }
    copy(p->result_id, sizeof(p->result_id), r.item_id);
    copy(p->result_revision, sizeof(p->result_revision), r.item_revision);
    copy(p->result_token, sizeof(p->result_token), r.edit_token);
  } else if (kind == DEMO_PROMPT || kind == DEMO_OUTPUT ||
             kind == DEMO_OUTPUT_END || kind == DEMO_RESPONSE_BEGIN ||
             kind == DEMO_SEGMENT_END || kind == DEMO_MESSAGE ||
             kind == DEMO_NOTE || kind == DEMO_SHELL || kind == DEMO_QUIT) {
    const char *text = demo_string(&packet);
    if (kind == DEMO_RESPONSE_BEGIN)
      p->active = 1;
    if (kind == DEMO_OUTPUT_END)
      p->active = 0;
    if (kind == DEMO_SHELL)
      ++p->shells;
    if (kind == DEMO_OUTPUT && text) {
      size_t i;
      for (i = 0; text[i]; ++i) {
        p->last_source[0] = p->last_source[1];
        p->last_source[1] = text[i];
        {
          size_t length = strlen(p->source_tail);
          if (length + 1 == sizeof(p->source_tail)) {
            memmove(p->source_tail, p->source_tail + 1, --length);
          }
          p->source_tail[length] = text[i];
          p->source_tail[length + 1] = 0;
        }
        ++p->source_bytes;
      }
    }
  } else
    goto fail;
  if (packet.failed || packet.offset != packet.length)
    goto fail;
  demo_packet_free(&packet);
  return 0;
fail:
  demo_packet_free(&packet);
  return -1;
}
static int attach(peer_t *p, unsigned short port, const char *name) {
  struct sockaddr_in address;
  demo_packet_t packet = {0};
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0)
    return -1;
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) ||
      demo_nonblock(fd)) {
    close(fd);
    return -1;
  }
  p->count = 0;
  p->revision[0] = 0;
  p->active = -1;
  demo_link_init(&p->link, fd);
  demo_put_number(&packet, DEMO_HELLO);
  demo_put_string(&packet, name);
  demo_put_number(&packet, p->delay_us ? p->delay_us : 2000);
  demo_put_number(&packet, p->serial);
  demo_put_number(&packet, 0);
  demo_put_number(&packet, 0);
  demo_put_number(&packet, 1);
  if (demo_link_queue(&p->link, &packet) || flush(p)) {
    demo_packet_free(&packet);
    return -1;
  }
  while (!*p->revision || p->active < 0)
    if (receive(p))
      return -1;
  return 0;
}
static int call_id(peer_t *p, sl_queue_request_t *r, unsigned long id) {
  demo_packet_t packet = {0};
  r->request_id = id;
  r->queue_revision = p->revision;
  demo_put_request(&packet, r);
  if (demo_link_queue(&p->link, &packet) || flush(p)) {
    demo_packet_free(&packet);
    return -1;
  }
  p->result = 0;
  while (p->result != id)
    if (receive(p))
      return -1;
  return 0;
}
static int call(peer_t *p, sl_queue_request_t *r) {
  return call_id(p, r, ++p->serial);
}
static int owner_cases(peer_t *p, unsigned short port) {
  sl_queue_request_t r;
  char first[128], second[128], token[128], revision[128];
  unsigned long offer_id;
  /* The handle previously issued ID 1, but this volatile owner has no record
   * of it. A restarted owner cannot call that historical outcome rejected. */
  p->serial = 1;
  CHECK(attach(p, port, "controller-A") == 0 && !p->count);
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_RESOLVE;
  r.target_request_id = 1;
  p->watch_target = 1;
  CHECK(!call(p, &r) && p->watched && p->watched_status == SL_QUEUE_UNKNOWN);
  p->watch_target = 0;
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_STOP;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->stopped);
  r.operation = SL_QUEUE_OFFER;
  r.text = "first";
  r.enabled = 1;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->count == 1);
  CHECK(p->entries[0].held);
  offer_id = p->serial;
  copy(first, sizeof(first), p->result_id);
  /* Duplicate admission is a result replay, not a second input. */
  CHECK(!call_id(p, &r, offer_id) && p->count == 1 &&
        !strcmp(first, p->result_id));
  r.text = "second";
  r.enabled = 1;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->count == 2);
  copy(second, sizeof(second), p->result_id);
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_BEGIN_EDIT;
  r.item_id = first;
  r.item_revision = p->entries[0].revision;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED &&
        p->entries[0].edit_held);
  copy(token, sizeof(token), p->result_token);
  copy(revision, sizeof(revision), p->result_revision);
  demo_link_close(&p->link);
  CHECK(!attach(p, port, "controller-A") && p->count == 2 &&
        p->entries[0].edit_held && *p->entries[0].token);
  r.operation = SL_QUEUE_RESOLVE;
  r.item_id = NULL;
  r.item_revision = NULL;
  r.target_request_id = offer_id;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->count == 2);
  demo_link_close(&p->link);
  p->serial = 0;
  CHECK(!attach(p, port, "controller-B") && p->count == 2 &&
        p->entries[0].edit_held && !*p->entries[0].token);
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_END_EDIT;
  r.item_id = first;
  r.item_revision = revision;
  r.edit_token = token;
  r.text = "stale controller";
  CHECK(!call(p, &r) && p->status == SL_QUEUE_REJECTED &&
        !strcmp(p->entries[0].text, "first"));
  r.operation = SL_QUEUE_BEGIN_EDIT;
  r.edit_token = NULL;
  r.text = NULL;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED &&
        strcmp(token, p->result_token));
  copy(token, sizeof(token), p->result_token);
  copy(revision, sizeof(revision), p->result_revision);
  r.operation = SL_QUEUE_ABORT_EDIT;
  r.item_revision = revision;
  r.edit_token = token;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED &&
        !p->entries[0].edit_held);
  r.operation = SL_QUEUE_WITHDRAW;
  r.item_revision = p->entries[0].revision;
  r.edit_token = NULL;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->count == 1 &&
        !strcmp(p->entries[0].id, second));
  /* Programmatic approval offers use the same owner FIFO as native input. */
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_OFFER;
  r.text = "held first";
  r.enabled = 1;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED);
  while (p->count != 2 || !p->entries[1].held)
    CHECK(!receive(p));
  r.text = "held second";
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED);
  while (p->count != 3 || !p->entries[2].held)
    CHECK(!receive(p));
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_BEGIN_EDIT;
  r.item_id = p->entries[2].id;
  r.item_revision = p->entries[2].revision;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_REJECTED &&
        !p->entries[2].edit_held);
  r.operation = SL_QUEUE_APPROVE;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_REJECTED && p->entries[1].held &&
        p->entries[2].held);
  r.operation = SL_QUEUE_AUTO_APPROVAL;
  r.item_id = r.item_revision = NULL;
  r.enabled = 1;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->approval &&
        !p->entries[1].held && !p->entries[2].held);
  /* Clean the injected examples atomically before exercising the real editor.
   */
  r.operation = SL_QUEUE_CLEAR;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && !p->count);
  r.operation = SL_QUEUE_AUTO_APPROVAL;
  r.enabled = 0;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && !p->approval);
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_OFFER;
  r.text = "second";
  r.enabled = 0;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->count == 1);
  r.operation = SL_QUEUE_STOP;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED && p->stopped);
  demo_link_close(&p->link);
  return 0;
}
typedef struct frontend {
  int master, stage, failed;
  int connecting, ready;
  demo_client_t *client;
  double deadline;
  char revision[128];
} frontend_t;
static int frontend_event(void *context, int kind, const char *text) {
  frontend_t *f = context;
  (void)text;
  if (kind == DEMO_CONNECTING && ++f->connecting == 1)
    return demo_client_reconnect(f->client);
  if (kind == DEMO_READY && !f->ready++) {
    if (f->connecting != 2) {
      fprintf(stderr, "Host callback reconnect was lost (%d attempts)\n",
              f->connecting);
      f->failed = 1;
    }
  }
  return SL_OK;
}
static void drive(sl_t *sl, void *context) {
  frontend_t *f = context;
  sl_queue_backend_info_t info;
  sl_queue_entry_t e;
  char drain[2048];
  const char *keys = NULL;
  while (read(f->master, drain, sizeof(drain)) > 0) {
  }
  if (sl_queue_backend_info(sl, &info) != SL_OK || !info.synchronized)
    return;
  if (now() > f->deadline) {
    fprintf(stderr, "frontend stage %d: draft='%s', visible=%lu pending=%lu\n",
            f->stage, sl_buffer(sl), (unsigned long)info.visible_count,
            (unsigned long)info.pending_count);
    f->failed = 1;
    (void)write(f->master, "\033n\004", 3);
    f->deadline = now() + 2000;
    return;
  }
  if (!f->stage) {
    keys = "native\rnew";
    f->stage = 1;
  } else if (f->stage == 1 && info.visible_count == 2 && !info.pending_count) {
    sl_queue_request_t r;
    memset(&r, 0, sizeof(r));
    r.operation = SL_QUEUE_STOP;
    if (sl_queue_backend_request(sl, &r, NULL) != SL_OK)
      f->failed = 1;
    f->stage = 11;
  } else if (f->stage == 11 && info.visible_count == 2 && !info.pending_count) {
    sl_queue_request_t r;
    if (strcmp(sl_buffer(sl), "new"))
      f->failed = 1;
    /* Handoff a proposal, then deliberately reconnect before its wake pipe
     * flush. Resolve the original ID as unaccepted; never auto-flush it. */
    memset(&r, 0, sizeof(r));
    r.text = "not sent";
    if (sl_queue_backend_request(sl, &r, NULL) != SL_OK ||
        demo_client_reconnect(f->client) != SL_OK)
      f->failed = 1;
    f->stage = 6;
  } else if (f->stage == 6 && info.visible_count == 2 &&
             info.pending_count == 1) {
    sl_queue_pending_info_t pending;
    if (sl_queue_backend_pending(sl, 0, &pending) != SL_OK || pending.pending ||
        pending.status != SL_QUEUE_REJECTED)
      return;
    if (strcmp(sl_buffer(sl), "new"))
      f->failed = 1;
    keys = "\033n\033e";
    f->stage = 7;
  } else if (f->stage == 7 && !strcmp(sl_buffer(sl), "not sent")) {
    keys = "\033n\033e";
    f->stage = 2;
  } else if (f->stage == 2 && !strcmp(sl_buffer(sl), "native")) {
    keys = " edited\rnew";
    f->stage = 3;
  } else if (f->stage == 3 && !info.pending_count &&
             sl_queue_backend_entry(sl, 1, &e) == SL_OK &&
             !strcmp(e.text, "native edited")) {
    if (strcmp(sl_buffer(sl), "new"))
      f->failed = 1;
    keys = "\033n\033e";
    f->stage = 4;
  } else if (f->stage == 4 && !strcmp(sl_buffer(sl), "native edited")) {
    copy(f->revision, sizeof(f->revision), info.revision);
    keys = "\001\013\004";
    f->stage = 5;
  }
  if (keys && write(f->master, keys, strlen(keys)) != (ssize_t)strlen(keys))
    f->failed = 1;
}
static int frontend_case(peer_t *p, unsigned short port) {
  frontend_t f;
  demo_client_t *client = demo_client_create("127.0.0.1", port);
  sl_config_t config;
  sl_t *sl;
  struct winsize size;
  int slave;
  char *line;
  CHECK(client);
  memset(&f, 0, sizeof(f));
  f.client = client;
  demo_client_events(client, frontend_event, &f);
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(!openpty(&f.master, &slave, NULL, NULL, &size) &&
        !demo_nonblock(f.master));
  f.deadline = now() + 5000;
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  config.prompt_queue = 1;
  config.prompt_queue_max_entries = DEMO_ITEM_MAX;
  config.queue_backend = demo_client_backend(client);
  config.queue_backend_context = client;
  sl = sl_create_with_config(&config);
  CHECK(sl && demo_client_attach(client, sl) == SL_OK);
  CHECK(sl_set_idle_callback(sl, drive, &f) == SL_OK);
  line = sl_next_prompt(sl, "> ", NULL);
  CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_EOF &&
        f.stage == 5 && !f.failed);
  sl_destroy(sl);
  demo_client_destroy(client);
  close(f.master);
  close(slave);
  p->serial = 0;
  CHECK(!attach(p, port, "observer") && p->count == 2);
  CHECK(!strcmp(p->revision, f.revision)); /* detach did not STOP or CLEAR */
  CHECK(!strcmp(p->entries[0].text, "second") &&
        !strcmp(p->entries[1].text, "native edited"));
  CHECK(p->entries[1].edit_held && !*p->entries[1].token);
  demo_link_close(&p->link);
  return 0;
}
static int framing(void) {
  int pair[2], i;
  demo_link_t link;
  demo_packet_t packet = {0}, received = {0};
  uint32_t length;
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair) && !demo_nonblock(pair[0]));
  demo_link_init(&link, pair[0]);
  length = htonl(4);
  CHECK(write(pair[1], &length, 2) == 2 &&
        demo_link_read(&link, &received) == 0);
  CHECK(write(pair[1], (char *)&length + 2, 2) == 2 &&
        demo_link_read(&link, &received) == 0);
  CHECK(write(pair[1], "test", 4) == 4 &&
        demo_link_read(&link, &received) == 1);
  CHECK(received.length == 4 && !memcmp(received.data, "test", 4));
  demo_packet_free(&received);
  for (i = 0; i < DEMO_OUT_MAX; ++i) {
    demo_put_string(&packet, "bounded");
    CHECK(!demo_link_queue(&link, &packet));
  }
  demo_put_string(&packet, "overflow");
  CHECK(demo_link_queue(&link, &packet) == -1);
  demo_packet_free(&packet);
  length = htonl(DEMO_FRAME_MAX + 1);
  CHECK(write(pair[1], &length, 4) == 4 &&
        demo_link_read(&link, &received) == -1);
  demo_link_close(&link);
  close(pair[1]);
  return 0;
}

typedef struct live {
  int master, slave, stage, failed;
  double deadline;
  char tail[256];
  size_t used;
  demo_client_t *client;
} live_t;
static int live_event(void *context, int kind, const char *text) {
  live_t *f = context;
  size_t keep;
  if (kind != DEMO_OUTPUT)
    return SL_OK;
  keep = strlen(text);
  if (keep >= sizeof(f->tail))
    return SL_ERROR_FULL;
  if (f->used + keep >= sizeof(f->tail)) {
    size_t remove = f->used + keep - sizeof(f->tail) + 1;
    memmove(f->tail, f->tail + remove, f->used - remove);
    f->used -= remove;
  }
  memcpy(f->tail + f->used, text, keep + 1);
  f->used += keep;
  return SL_OK;
}
static void live_drive(sl_t *sl, void *context) {
  live_t *f = context;
  sl_queue_backend_info_t info;
  char bytes[1024];
  ssize_t n;
  while ((n = read(f->master, bytes, sizeof(bytes))) > 0) {
  }
  (void)demo_client_idle(f->client);
  if (now() > f->deadline) {
    f->failed = 1;
    (void)write(f->master, "\033n\004", 3);
    return;
  }
  if (sl_queue_backend_info(sl, &info) != SL_OK || !info.synchronized)
    return;
  if (!f->stage) {
    (void)write(f->master, "typing", 6);
    f->stage = 1;
  } else if (f->stage == 1 && strstr(f->tail, "Try another prompt.")) {
    if (strcmp(sl_buffer(sl), "typing")) {
      fprintf(stderr, "live draft: '%s'\n", sl_buffer(sl));
      f->failed = 1;
    }
    /* Cursor-report and resize behavior belongs to the emulator tests. This
     * bare PTY verifies fd-driven output and a concurrent draft without
     * inventing terminal cursor reports. */
    (void)write(f->master, "\033n\004", 3);
    f->stage = 2;
  }
}
static int live_case(peer_t *p, unsigned short port) {
  sl_queue_request_t r;
  demo_client_t *client;
  sl_config_t config;
  sl_t *sl;
  live_t f;
  struct winsize size;
  CHECK(!attach(p, port, "live-observer"));
  p->serial = 0;
  memset(&r, 0, sizeof(r));
  r.operation = SL_QUEUE_CLEAR;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED);
  r.operation = SL_QUEUE_RESUME;
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED);
  r.operation = SL_QUEUE_OFFER;
  r.text = "stream while I type";
  CHECK(!call(p, &r) && p->status == SL_QUEUE_ACCEPTED);
  demo_link_close(&p->link);
  client = demo_client_create("127.0.0.1", port);
  CHECK(client);
  memset(&f, 0, sizeof(f));
  f.client = client;
  demo_client_events(client, live_event, &f);
  demo_client_options(client, 2000, 0, 0, 1);
  memset(&size, 0, sizeof(size));
  size.ws_col = 80;
  size.ws_row = 24;
  CHECK(!openpty(&f.master, &f.slave, NULL, NULL, &size) &&
        !demo_nonblock(f.master));
  f.deadline = now() + 3000;
  sl_config_init(&config);
  config.input_fd = config.output_fd = f.slave;
  config.prompt_queue = 1;
  config.prompt_queue_max_entries = DEMO_ITEM_MAX;
  config.queue_backend = demo_client_backend(client);
  config.queue_backend_context = client;
  sl = sl_create_with_config(&config);
  CHECK(sl && demo_client_attach(client, sl) == SL_OK);
  CHECK(sl_set_idle_callback(sl, live_drive, &f) == SL_OK &&
        sl_output_stream_begin(sl) == SL_OK);
  CHECK(!sl_next_prompt(sl, "> ", NULL) &&
        sl_last_readline_status(sl) == SL_READLINE_EOF);
  CHECK(f.stage == 2 && !f.failed);
  CHECK(sl_output_stream_end(sl) == SL_OK);
  sl_destroy(sl);
  demo_client_destroy(client);
  close(f.master);
  close(f.slave);
  return 0;
}
static int shell_case(peer_t *p, unsigned short port) {
  sl_queue_request_t request;
  demo_packet_t packet = {0};
  size_t before;
  p->serial = 0;
  CHECK(!attach(p, port, "shell-controller"));
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_CLEAR;
  CHECK(!call(p, &request) && p->status == SL_QUEUE_ACCEPTED);
  request.operation = SL_QUEUE_RESUME;
  CHECK(!call(p, &request) && p->status == SL_QUEUE_ACCEPTED);
  request.operation = SL_QUEUE_OFFER;
  request.text = "shell-active";
  CHECK(!call(p, &request) && p->status == SL_QUEUE_ACCEPTED);
  /* Hand off at a source seam with a second turn already accepted. */
  request.text = "background turn";
  CHECK(!call(p, &request) && p->status == SL_QUEUE_ACCEPTED);
  demo_put_number(&packet, DEMO_IDLE);
  demo_put_number(&packet, 0);
  CHECK(!demo_link_queue(&p->link, &packet) && !flush(p));
  while (!p->active)
    CHECK(!receive(p));
  demo_put_number(&packet, DEMO_SHELL_BEGIN);
  CHECK(!demo_link_queue(&p->link, &packet) && !flush(p));
  while (!p->shells)
    CHECK(!receive(p));
  CHECK(p->active && !strcmp(p->last_source, "\n\n"));
  before = p->source_bytes;
  /* Leave this subscription unread for longer than both 2 ms/character
   * responses. Both turns must finish without output backpressure. */
  (void)poll(NULL, 0, 1000);
  demo_link_close(&p->link);
  CHECK(!attach(p, port, "shell-controller"));
  CHECK(!p->active && !p->count && before == p->source_bytes);
  demo_link_close(&p->link);
  return 0;
}

static int completion_shell_case(peer_t *p, unsigned short port) {
  sl_queue_request_t request;
  demo_packet_t packet = {0};
  p->serial = 0;
  p->delay_us = 20000;
  CHECK(!attach(p, port, "completion-shell"));
  memset(&request, 0, sizeof(request));
  request.operation = SL_QUEUE_OFFER;
  request.text = "completion seam";
  CHECK(!call(p, &request) && p->status == SL_QUEUE_ACCEPTED);
  request.text = "following turn";
  CHECK(!call(p, &request) && p->status == SL_QUEUE_ACCEPTED);
  p->source_tail[0] = 0;
  p->shells = 0;
  demo_put_number(&packet, DEMO_IDLE);
  demo_put_number(&packet, 0);
  CHECK(!demo_link_queue(&p->link, &packet) && !flush(p));
  /* Request after the final paragraph begins. Completion is the next seam;
   * the already accepted following turn must not delay the terminal grant. */
  while (!strstr(p->source_tail, "Try another"))
    CHECK(!receive(p));
  demo_put_number(&packet, DEMO_SHELL_BEGIN);
  CHECK(!demo_link_queue(&p->link, &packet) && !flush(p));
  while (!p->shells)
    CHECK(!receive(p));
  CHECK(!p->active && strstr(p->source_tail, "Try another prompt.\n"));
  demo_link_close(&p->link);
  return 0;
}

int main(int argc, char **argv) {
  int startup[2], status, result, port;
  FILE *output;
  char line[128];
  pid_t child;
  peer_t *p;
  CHECK(argc == 2 && !framing());
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    execl(argv[1], argv[1], "invalid-port", (char *)NULL);
    _exit(127);
  }
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 1);
  CHECK(!pipe(startup));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    dup2(startup[1], STDOUT_FILENO);
    close(startup[0]);
    close(startup[1]);
    setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "0", 1);
    execl(argv[1], argv[1], "0", (char *)NULL);
    _exit(127);
  }
  close(startup[1]);
  output = fdopen(startup[0], "r");
  CHECK(output && fgets(line, sizeof(line), output) &&
        sscanf(line, "LISTEN 127.0.0.1 %d", &port) == 1);
  p = calloc(1, sizeof(*p));
  CHECK(p);
  demo_link_init(&p->link, -1);
  signal(SIGPIPE, SIG_IGN);
  result = owner_cases(p, (unsigned short)port);
  if (!result)
    result = frontend_case(p, (unsigned short)port);
  if (!result)
    result = live_case(p, (unsigned short)port);
  if (!result)
    result = shell_case(p, (unsigned short)port);
  if (!result)
    result = completion_shell_case(p, (unsigned short)port);
  demo_link_close(&p->link);
  free(p);
  kill(child, SIGTERM);
  CHECK(waitpid(child, &status, 0) == child);
  fclose(output);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return result;
}
