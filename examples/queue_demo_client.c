#define _POSIX_C_SOURCE 200809L
#include "queue_demo_client.h"
#include "queue_demo_wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct demo_client {
  sl_t *editor;
  sl_queue_backend_t backend;
  demo_link_t link;
  struct sockaddr_in address;
  sl_watch_id_t socket_watch, wake_watch;
  int wake[2], connected, connecting, reconnect, resolve;
  sl_queue_request_id_t resolved_through;
  sl_queue_request_id_t request_high;
  char name[64];
  sl_queue_entry_t entries[DEMO_ITEM_MAX];
};
static int wake(demo_client_t *c) {
  char byte = 'w';
  ssize_t n;
  do {
    n = write(c->wake[1], &byte, 1);
  } while (n < 0 && errno == EINTR);
  return n == 1 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
             ? SL_OK
             : SL_ERROR_IO;
}
static void lost(demo_client_t *c) {
  sl_queue_change_t change;
  if (c->editor && c->socket_watch)
    (void)sl_watch_remove(c->editor, c->socket_watch);
  c->socket_watch = 0;
  c->connected = c->connecting = 0;
  demo_link_close(&c->link);
  if (c->editor) {
    memset(&change, 0, sizeof(change));
    change.kind = SL_QUEUE_DISCONNECTED;
    (void)sl_queue_backend_update(c->editor, &change);
  }
}
static void interest(demo_client_t *c) {
  if (c->socket_watch)
    (void)sl_watch_modify(
        c->editor, c->socket_watch,
        SL_WATCH_READ |
            ((c->connecting || c->link.count) ? SL_WATCH_WRITE : 0));
}
static int request(void *context, const sl_queue_request_t *r) {
  demo_client_t *c = context;
  demo_packet_t p = {0};
  if (r->request_id > c->request_high)
    c->request_high = r->request_id;
  if (!c->connected)
    return SL_ERROR_NOT_READY;
  demo_put_request(&p, r);
  if (demo_link_queue(&c->link, &p)) {
    demo_packet_free(&p);
    return SL_ERROR_FULL;
  }
  /* No editor calls, network waits or socket I/O on this callback path. */
  return wake(c);
}
static void detach(void *context) {
  demo_client_t *c = context;
  c->editor = NULL;
  c->socket_watch = c->wake_watch = 0;
  demo_link_close(&c->link);
  /* No STOP/CLEAR. Accepted items and edit holds belong to the owner. */
  if (c->wake[0] >= 0)
    close(c->wake[0]);
  if (c->wake[1] >= 0)
    close(c->wake[1]);
  c->wake[0] = c->wake[1] = -1;
}
static void resolve(demo_client_t *c) {
  sl_queue_backend_info_t info;
  size_t i;
  if (!c->resolve || sl_queue_backend_info(c->editor, &info) != SL_OK ||
      !info.synchronized)
    return;
  for (i = 0; i < info.pending_count; ++i) {
    sl_queue_pending_info_t pending;
    sl_queue_request_t r;
    if (sl_queue_backend_pending(c->editor, i, &pending) != SL_OK ||
        pending.pending || pending.status != SL_QUEUE_UNKNOWN ||
        pending.request.request_id <= c->resolved_through ||
        pending.request.operation == SL_QUEUE_RESOLVE)
      continue;
    memset(&r, 0, sizeof(r));
    r.operation = SL_QUEUE_RESOLVE;
    r.target_request_id = pending.request.request_id;
    if (sl_queue_backend_request(c->editor, &r, NULL) == SL_OK)
      c->resolved_through = r.target_request_id;
    return;
  }
  c->resolve = 0;
}
static int receive(demo_client_t *c, demo_packet_t *p) {
  unsigned long kind = demo_number(p);
  int status = SL_OK;
  if (kind == DEMO_RESULT) {
    sl_queue_result_t r;
    demo_get_result(p, &r);
    if (p->failed || p->offset != p->length)
      return -1;
    status = sl_queue_backend_result(c->editor, &r);
    if (status == SL_OK)
      resolve(c);
  } else if (kind == DEMO_CHANGE) {
    sl_queue_change_t change;
    if (demo_get_change(p, &change, c->entries, DEMO_ITEM_MAX) ||
        p->offset != p->length)
      return -1;
    status = sl_queue_backend_update(c->editor, &change);
    if (status == SL_ERROR_CONFLICT)
      status = SL_OK; /* native pump requests resync */
    if (change.kind == SL_QUEUE_SNAPSHOT && status == SL_OK)
      resolve(c);
  } else if (kind == DEMO_PROMPT || kind == DEMO_OUTPUT ||
             kind == DEMO_OUTPUT_END) {
    const char *text = demo_string(p);
    if (p->failed || p->offset != p->length ||
        (!text && kind != DEMO_OUTPUT_END))
      return -1;
    if (kind == DEMO_PROMPT)
      status = sl_output_stream_write_quoted_prompt(c->editor, text);
    else if (text)
      status = sl_output_stream_write(c->editor, text, strlen(text));
  } else
    return -1;
  return status == SL_OK ? 0 : -1;
}
static int socket_ready(sl_t *sl, const sl_watch_event_t *event,
                        void *context) {
  demo_client_t *c = context;
  int i;
  (void)sl;
  if (c->connecting) {
    int error = 0;
    socklen_t length = sizeof(error);
    demo_packet_t p = {0};
    if (getsockopt(c->link.fd, SOL_SOCKET, SO_ERROR, &error, &length) ||
        error) {
      lost(c);
      return SL_OK;
    }
    c->connecting = 0;
    c->connected = 1;
    demo_put_number(&p, DEMO_HELLO);
    demo_put_string(&p, c->name);
    demo_put_number(&p, getenv("SOFTLINE_CHAT_CHAR_MS") &&
                                !strcmp(getenv("SOFTLINE_CHAT_CHAR_MS"), "0")
                            ? 0
                            : 100);
    demo_put_number(&p, c->request_high);
    if (demo_link_queue(&c->link, &p)) {
      demo_packet_free(&p);
      lost(c);
      return SL_OK;
    }
  }
  if (c->connected && demo_link_flush(&c->link)) {
    lost(c);
    return SL_OK;
  }
  for (i = 0; i < 8 && (event->events &
                        (SL_WATCH_READ | SL_WATCH_HANGUP | SL_WATCH_ERROR));
       ++i) {
    demo_packet_t p = {0};
    int status = demo_link_read(&c->link, &p);
    if (!status)
      break;
    if (status < 0 || receive(c, &p)) {
      demo_packet_free(&p);
      lost(c);
      return SL_OK;
    }
    demo_packet_free(&p);
  }
  interest(c);
  return SL_OK;
}
static int connect_owner(demo_client_t *c) {
  int fd, status;
  lost(c);
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return SL_ERROR_IO;
  if (demo_nonblock(fd)) {
    close(fd);
    return SL_ERROR_IO;
  }
  status = connect(fd, (struct sockaddr *)&c->address, sizeof(c->address));
  if (status && errno != EINPROGRESS) {
    close(fd);
    return SL_ERROR_IO;
  }
  demo_link_init(&c->link, fd);
  c->connecting = 1;
  c->resolve = 1;
  c->resolved_through = 0;
  if (sl_watch_add(c->editor, fd, SL_WATCH_READ | SL_WATCH_WRITE, socket_ready,
                   c, &c->socket_watch) != SL_OK) {
    lost(c);
    return SL_ERROR_IO;
  }
  return SL_OK;
}
static int wake_ready(sl_t *sl, const sl_watch_event_t *event, void *context) {
  demo_client_t *c = context;
  char bytes[128];
  ssize_t got;
  (void)sl;
  (void)event;
  do {
    got = read(c->wake[0], bytes, sizeof(bytes));
  } while (got > 0 || (got < 0 && errno == EINTR));
  if (c->reconnect) {
    c->reconnect = 0;
    (void)connect_owner(c);
  }
  if (c->connected && demo_link_flush(&c->link))
    lost(c);
  interest(c);
  return SL_OK;
}
demo_client_t *demo_client_create(const char *host, unsigned short port) {
  demo_client_t *c = calloc(1, sizeof(*c));
  struct timespec now;
  if (!c)
    return NULL;
  c->wake[0] = c->wake[1] = -1;
  demo_link_init(&c->link, -1);
  c->address.sin_family = AF_INET;
  c->address.sin_port = htons(port);
  /* Numeric loopback address avoids DNS waits and exposing an unauthenticated
   * demo. */
  if (!host || inet_pton(AF_INET, host, &c->address.sin_addr) != 1 ||
      ntohl(c->address.sin_addr.s_addr) != INADDR_LOOPBACK || !port ||
      pipe(c->wake) || demo_nonblock(c->wake[0]) || demo_nonblock(c->wake[1])) {
    demo_client_destroy(c);
    return NULL;
  }
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  (void)snprintf(c->name, sizeof(c->name), "frontend-%lu-%lu-%lu",
                 (unsigned long)getpid(), (unsigned long)now.tv_sec,
                 (unsigned long)now.tv_nsec);
  c->backend.request = request;
  c->backend.detach = detach;
  c->backend.capabilities = (1UL << (SL_QUEUE_RESOLVE + 1)) - 1;
  c->backend.max_pending_requests = 16;
  return c;
}
const sl_queue_backend_t *demo_client_backend(demo_client_t *c) {
  return c ? &c->backend : NULL;
}
int demo_client_attach(demo_client_t *c, sl_t *editor) {
  if (!c || !editor || c->editor)
    return SL_ERROR_INVALID;
  c->editor = editor;
  if (sl_watch_add(editor, c->wake[0], SL_WATCH_READ, wake_ready, c,
                   &c->wake_watch) != SL_OK)
    return SL_ERROR_IO;
  return demo_client_reconnect(c);
}
int demo_client_reconnect(demo_client_t *c) {
  if (!c || !c->editor)
    return SL_ERROR_INVALID;
  c->reconnect = 1;
  return wake(c);
}
int demo_client_cancel(demo_client_t *c) {
  demo_packet_t p = {0};
  if (!c || !c->connected)
    return SL_ERROR_NOT_READY;
  demo_put_number(&p, DEMO_CANCEL);
  if (demo_link_queue(&c->link, &p)) {
    demo_packet_free(&p);
    return SL_ERROR_FULL;
  }
  return wake(c);
}
void demo_client_destroy(demo_client_t *c) {
  if (!c)
    return;
  /* The editor must be destroyed first: its detach ends the borrowed context.
   */
  detach(c);
  free(c);
}
