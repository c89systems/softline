#include "queue_demo_terminal.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

typedef struct remote_terminal {
  demo_link_t link;
  struct sockaddr_in address;
  int connecting, opened, ready, eof, resized, exit_status;
  unsigned int rows, columns;
  char output[4096];
  size_t length;
  double deadline;
} remote_terminal_t;
static double terminal_time(void) {
  struct timespec time;
  (void)clock_gettime(CLOCK_MONOTONIC, &time);
  return (double)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}
static int size_packet(demo_link_t *link, unsigned long kind, unsigned int rows,
                       unsigned int columns) {
  demo_packet_t p = {0};
  demo_put_number(&p, kind);
  demo_put_number(&p, rows);
  demo_put_number(&p, columns);
  if (demo_link_queue(link, &p)) {
    demo_packet_free(&p);
    return SL_ERROR_IO;
  }
  return SL_OK;
}
static int remote_callback(void *context, sl_terminal_event_t *e) {
  remote_terminal_t *r = context;
  demo_packet_t p = {0};
  int status;
  struct linger discard = {1, 0};
  switch (e->operation) {
  case SL_TERMINAL_OPEN:
    r->rows = e->rows;
    r->columns = e->columns;
    r->deadline = terminal_time() + 3000;
    r->link.fd = socket(AF_INET, SOCK_STREAM, 0);
    if (r->link.fd < 0 || demo_nonblock(r->link.fd))
      return SL_ERROR_IO;
    /* A fresh, dedicated PTY connection must not retain queued input after
     * cancellation or frontend death. Normal EOF has already been drained. */
    if (setsockopt(r->link.fd, SOL_SOCKET, SO_LINGER, &discard,
                   sizeof(discard)))
      return SL_ERROR_IO;
    status =
        connect(r->link.fd, (struct sockaddr *)&r->address, sizeof(r->address));
    if (status && errno != EINPROGRESS)
      return SL_ERROR_IO;
    r->connecting = status != 0;
    return SL_OK;
  case SL_TERMINAL_STEP:
    if (!r->ready && terminal_time() >= r->deadline)
      return SL_ERROR_NOT_READY;
    if (r->connecting && e->events) {
      int error = 0;
      socklen_t length = sizeof(error);
      if (getsockopt(r->link.fd, SOL_SOCKET, SO_ERROR, &error, &length) ||
          error)
        return SL_ERROR_IO;
      r->connecting = 0;
    }
    if (!r->connecting) {
      if (!r->opened) {
        status = size_packet(&r->link, DEMO_PTY_OPEN, r->rows, r->columns);
        if (status != SL_OK)
          return status;
        r->opened = 1;
      }
      if (r->resized && r->link.count < 2) {
        status = size_packet(&r->link, DEMO_PTY_RESIZE, r->rows, r->columns);
        if (status != SL_OK)
          return status;
        r->resized = 0;
      }
      if (demo_link_flush(&r->link))
        return SL_ERROR_IO;
      if (!r->length && !r->eof) {
        status = demo_link_read(&r->link, &p);
        if (status < 0)
          return SL_ERROR_IO;
        if (status) {
          unsigned long kind = demo_number(&p);
          if (kind == DEMO_PTY_READY)
            r->ready = 1;
          else if (kind == DEMO_PTY_DATA && r->ready) {
            const char *data = demo_bytes(&p, &r->length);
            if (!p.failed)
              memcpy(r->output, data, r->length);
          } else if (kind == DEMO_PTY_EXIT) {
            r->exit_status = (int)demo_number(&p);
            r->ready = 1;
            r->eof = 1;
          } else
            p.failed = 1;
          status = p.failed || p.offset != p.length ? SL_ERROR_IO : SL_OK;
          demo_packet_free(&p);
          if (status != SL_OK)
            return status;
        }
      }
    }
    e->fd = r->link.fd;
    e->interests =
        ((!r->length && !r->eof) ? SL_WATCH_READ : 0) |
        ((r->connecting || r->link.count || r->resized) ? SL_WATCH_WRITE : 0);
    e->ready = r->ready;
    e->timeout_ms = r->ready ? -1 : (int)(r->deadline - terminal_time());
    if (e->timeout_ms < 0 && !r->ready)
      e->timeout_ms = 0;
    return SL_OK;
  case SL_TERMINAL_READ:
    if (!r->length)
      return r->eof ? SL_OK : SL_ERROR_NOT_READY;
    e->transferred = e->length < r->length ? e->length : r->length;
    memcpy(e->data, r->output, e->transferred);
    r->length -= e->transferred;
    memmove(r->output, r->output + e->transferred, r->length);
    return SL_OK;
  case SL_TERMINAL_WRITE:
    if (r->link.count >= 2)
      return SL_ERROR_NOT_READY;
    demo_put_number(&p, DEMO_PTY_DATA);
    demo_put_bytes(&p, e->data, e->length);
    if (demo_link_queue(&r->link, &p)) {
      demo_packet_free(&p);
      return SL_ERROR_IO;
    }
    e->transferred = e->length;
    return SL_OK;
  case SL_TERMINAL_RESIZE:
    r->rows = e->rows;
    r->columns = e->columns;
    r->resized = 1;
    return SL_OK;
  case SL_TERMINAL_CLOSE:
    demo_link_close(&r->link);
    return SL_OK;
  }
  return SL_ERROR_INVALID;
}
int demo_terminal_remote(sl_t *editor, const struct sockaddr_in *address,
                         int *exit_status) {
  remote_terminal_t *r = calloc(1, sizeof(*r));
  int status;
  if (!r)
    return SL_ERROR_NOMEM;
  demo_link_init(&r->link, -1);
  r->link.input_limit = 8192;
  r->address = *address;
  r->exit_status = -1;
  status = sl_terminal_handoff(editor, remote_callback, r);
  *exit_status = r->exit_status;
  demo_link_close(&r->link);
  free(r);
  return status;
}
int demo_terminal_server_input(demo_terminal_server_t *s, demo_link_t *link,
                               unsigned long kind, demo_packet_t *p) {
  sl_terminal_event_t e;
  memset(&e, 0, sizeof(e));
  if (kind == DEMO_PTY_OPEN || kind == DEMO_PTY_RESIZE) {
    if (kind == DEMO_PTY_OPEN && s->pty.fd >= 0)
      return -1;
    e.rows = (unsigned int)demo_number(p);
    e.columns = (unsigned int)demo_number(p);
    if (p->failed || p->offset != p->length || !e.rows || !e.columns ||
        e.rows > 65535 || e.columns > 65535)
      return -1;
    e.operation = kind == DEMO_PTY_OPEN ? SL_TERMINAL_OPEN : SL_TERMINAL_RESIZE;
    if (sl_pty_callback(&s->pty, &e) != SL_OK)
      return -1;
  } else if (kind == DEMO_PTY_DATA && !s->eof) {
    size_t length;
    const char *data = demo_bytes(p, &length);
    if (p->failed || length > sizeof(s->input) - s->input_len)
      return -1;
    memcpy(s->input + s->input_len, data, length);
    s->input_len += length;
  } else
    return -1;
  return 0;
}
int demo_terminal_server_pump(demo_terminal_server_t *s, demo_link_t *link) {
  sl_terminal_event_t e;
  char data[2048];
  int status;
  (void)sl_pty_reap(&s->pty);
  if (s->eof)
    return 0;
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_STEP;
  status = sl_pty_callback(&s->pty, &e);
  if (status != SL_OK) {
    demo_packet_t p = {0};
    if (s->pty.status < 0)
      return -1;
    demo_put_number(&p, DEMO_PTY_EXIT);
    demo_put_number(&p, (unsigned long)s->pty.status);
    s->eof = 1;
    if (demo_link_queue(link, &p)) {
      demo_packet_free(&p);
      return -1;
    }
    return 0;
  }
  if (!e.ready)
    return 0;
  if (!s->ready) {
    demo_packet_t p = {0};
    demo_put_number(&p, DEMO_PTY_READY);
    if (demo_link_queue(link, &p)) {
      demo_packet_free(&p);
      return -1;
    }
    s->ready = 1;
  }
  if (s->input_len) {
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_WRITE;
    e.data = s->input;
    e.length = s->input_len;
    status = sl_pty_callback(&s->pty, &e);
    if (status == SL_OK) {
      s->input_len -= e.transferred;
      memmove(s->input, s->input + e.transferred, s->input_len);
    } else if (status != SL_ERROR_NOT_READY)
      return -1;
  }
  if (!s->eof && link->count < 2) {
    demo_packet_t p = {0};
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_READ;
    e.data = data;
    e.length = sizeof(data);
    status = sl_pty_callback(&s->pty, &e);
    if (status == SL_ERROR_NOT_READY)
      return 0;
    if (status != SL_OK)
      return -1;
    if (e.transferred) {
      demo_put_number(&p, DEMO_PTY_DATA);
      demo_put_bytes(&p, data, e.transferred);
    } else {
      demo_put_number(&p, DEMO_PTY_EXIT);
      demo_put_number(&p,
                      (unsigned long)(s->pty.status < 0 ? 0 : s->pty.status));
      s->eof = 1;
    }
    if (demo_link_queue(link, &p)) {
      demo_packet_free(&p);
      return -1;
    }
  }
  return 0;
}
