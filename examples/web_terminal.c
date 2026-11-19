#include "web_terminal.h"
#include "softline/terminal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTPUT_WINDOW (64u * 1024u)
struct demo_web_terminal {
  sl_terminal_session_t *session;
  sl_www_t *receiver;
  demo_web_terminal_write_t write;
  void *context;
  sl_terminal_event_t wait;
  char input[SL_TERMINAL_SESSION_CHUNK_MAX];
  size_t length, offset, outstanding;
  int opened, ready, closed, eof;
};
static int text_write(void *context, const void *bytes, size_t length) {
  demo_web_terminal_t *t = context;
  return t->write(t->context, 0, bytes, length);
}
static int notify(demo_web_terminal_t *t, const char *kind, const char *name,
                  const char *value) {
  sl_www_field_t field;
  field.name = name;
  field.value = value;
  field.length = 0;
  return sl_www_emit(text_write, t, kind, NULL, name ? &field : NULL,
                     name ? 1 : 0);
}
static int number(const sl_www_request_t *r, const char *name,
                  unsigned long *out) {
  const char *text = sl_www_field(r, name);
  char *end;
  if (!text || *text < '0' || *text > '9')
    return -1;
  errno = 0;
  *out = strtoul(text, &end, 10);
  return errno || *end ? -1 : 0;
}
static int action(void *context, const sl_www_request_t *r) {
  demo_web_terminal_t *t = context;
  const char *kind = sl_www_field(r, "action");
  sl_terminal_event_t event;
  unsigned long rows, columns, bytes;
  if (!kind || t->closed || t->eof)
    return SL_ERROR_INVALID;
  if (!strcmp(kind, "ack")) {
    if (!t->ready || number(r, "bytes", &bytes) || !bytes ||
        bytes > t->outstanding)
      return SL_ERROR_INVALID;
    t->outstanding -= bytes;
    return SL_OK;
  }
  if (strcmp(kind, "open") && strcmp(kind, "resize"))
    return SL_ERROR_INVALID;
  if (number(r, "rows", &rows) || number(r, "columns", &columns) || !rows ||
      rows > 65535 || !columns || columns > 65535)
    return SL_ERROR_INVALID;
  memset(&event, 0, sizeof(event));
  event.rows = (unsigned int)rows;
  event.columns = (unsigned int)columns;
  if (!strcmp(kind, "open")) {
    if (t->opened)
      return SL_ERROR_INVALID;
    t->opened = 1;
    event.operation = SL_TERMINAL_OPEN;
  } else {
    if (!t->ready)
      return SL_ERROR_NOT_READY;
    event.operation = SL_TERMINAL_RESIZE;
  }
  return sl_terminal_session_dispatch(t->session, &event);
}
static int binary(void *context, const sl_www_request_t *request,
                  const void *bytes, size_t length) {
  demo_web_terminal_t *t = context;
  (void)request;
  if (!t->ready || t->closed || t->eof || t->length || !length ||
      length > sizeof(t->input))
    return SL_ERROR_INVALID;
  memcpy(t->input, bytes, length);
  t->length = length;
  t->offset = 0;
  return SL_OK;
}
demo_web_terminal_t *demo_web_terminal_create(demo_web_terminal_write_t write,
                                              void *context) {
  demo_web_terminal_t *t;
  sl_terminal_session_config_t policy;
  sl_www_config_t config;
  if (!write || !(t = calloc(1, sizeof(*t))))
    return NULL;
  t->write = write;
  t->context = context;
  t->wait.fd = -1;
  sl_terminal_session_config_init(&policy);
  policy.terminal_type = "xterm-256color";
  t->session = sl_terminal_session_create(&policy);
  sl_www_config_init(&config);
  config.enabled = 1;
  config.action = action;
  config.binary = binary;
  config.context = t;
  t->receiver = sl_www_create(&config);
  if (!t->session || !t->receiver) {
    if (t->session)
      (void)sl_terminal_session_destroy(t->session);
    sl_www_destroy(t->receiver);
    free(t);
    return NULL;
  }
  return t;
}
int demo_web_terminal_message(demo_web_terminal_t *t, int is_binary,
                              const void *bytes, size_t length) {
  if (!t || (!is_binary && length > 128))
    return SL_ERROR_INVALID;
  return is_binary
             ? sl_www_binary(t->receiver, t, bytes, length, text_write, t)
             : sl_www_message(t->receiver, t, bytes, length, text_write, t);
}
int demo_web_terminal_pump(demo_web_terminal_t *t) {
  sl_terminal_event_t event;
  char output[SL_TERMINAL_SESSION_CHUNK_MAX], code[32];
  int status, exit_code = -1, budget = 4;
  if (!t || t->closed || t->eof || !t->opened)
    return t && t->eof ? 1 : 0;
  memset(&t->wait, 0, sizeof(t->wait));
  t->wait.operation = SL_TERMINAL_STEP;
  t->wait.fd = -1;
  t->wait.timeout_ms = -1;
  status = sl_terminal_session_dispatch(t->session, &t->wait);
  if (status != SL_OK)
    return -1;
  if (!t->wait.ready)
    return 0;
  if (!t->ready) {
    t->ready = 1;
    if (notify(t, "ready", NULL, NULL) != SL_OK ||
        notify(t, "input_credit", NULL, NULL) != SL_OK)
      return -1;
  }
  if (t->length) {
    memset(&event, 0, sizeof(event));
    event.operation = SL_TERMINAL_WRITE;
    event.data = t->input + t->offset;
    event.length = t->length - t->offset;
    status = sl_terminal_session_dispatch(t->session, &event);
    if (status != SL_OK && status != SL_ERROR_NOT_READY)
      return -1;
    if (status == SL_OK)
      t->offset += event.transferred;
    if (t->offset == t->length) {
      t->length = t->offset = 0;
      if (notify(t, "input_credit", NULL, NULL) != SL_OK)
        return -1;
    }
  }
  while (budget-- && t->outstanding < OUTPUT_WINDOW) {
    memset(&event, 0, sizeof(event));
    event.operation = SL_TERMINAL_READ;
    event.data = output;
    event.length = OUTPUT_WINDOW - t->outstanding;
    if (event.length > sizeof(output))
      event.length = sizeof(output);
    status = sl_terminal_session_dispatch(t->session, &event);
    if (status == SL_ERROR_NOT_READY)
      break;
    if (status != SL_OK)
      return -1;
    if (!event.transferred) {
      if (sl_terminal_session_status(t->session, &exit_code) ==
          SL_ERROR_NOT_READY)
        return 0;
      (void)snprintf(code, sizeof(code), "%d", exit_code);
      t->eof = 1;
      return notify(t, "exit", "status", code) == SL_OK ? 1 : -1;
    }
    if (t->write(t->context, 1, output, event.transferred) != SL_OK)
      return -1;
    t->outstanding += event.transferred;
  }
  return 0;
}
void demo_web_terminal_descriptor(demo_web_terminal_t *t, struct pollfd *fd) {
  fd->fd = -1;
  fd->events = fd->revents = 0;
  if (!t || t->closed || t->eof || !t->opened)
    return;
  fd->fd = t->wait.fd;
  if (!t->ready || t->outstanding < OUTPUT_WINDOW)
    fd->events |= POLLIN;
  if (t->length)
    fd->events |= POLLOUT;
  if (!fd->events)
    fd->fd = -1;
}
void demo_web_terminal_close(demo_web_terminal_t *t) {
  sl_terminal_event_t event;
  if (!t || t->closed)
    return;
  t->closed = 1;
  memset(&event, 0, sizeof(event));
  event.operation = SL_TERMINAL_CLOSE;
  (void)sl_terminal_session_dispatch(t->session, &event);
}
int demo_web_terminal_destroy(demo_web_terminal_t *t) {
  int status;
  if (!t)
    return SL_OK;
  demo_web_terminal_close(t);
  status = sl_terminal_session_destroy(t->session);
  if (status == SL_ERROR_NOT_READY)
    return status;
  sl_www_destroy(t->receiver);
  free(t);
  return status;
}
