#include "softline/terminal.h"
#include "softline_pty.h"
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define session_environment (*_NSGetEnviron())
#else
extern char **environ;
#define session_environment environ
#endif
#define POLICY_BYTES (128u * 1024u)
#define POLICY_ITEMS 128u
typedef struct session {
  sl_terminal_session_t api;
  sl_pty_t pty;
  char *program, *directory;
  char **arguments, **environment;
  size_t policy_bytes;
  int opened, closed, busy;
} session_t;
static char *copy(session_t *s, const char *text) {
  char *p;
  size_t n = 0;
  if (!text)
    return NULL;
  while (n <= POLICY_BYTES - s->policy_bytes && text[n])
    ++n;
  if (n >= POLICY_BYTES - s->policy_bytes)
    return NULL;
  p = malloc(n + 1);
  if (p) {
    memcpy(p, text, n + 1);
    s->policy_bytes += n + 1;
  }
  return p;
}
static void free_vector(char **v) {
  size_t i;
  if (!v)
    return;
  for (i = 0; v[i]; ++i)
    free(v[i]);
  free(v);
}
static char **vector(session_t *s, const char *const *v, const char *term,
                     size_t limit) {
  size_t i, n = 0, out = 0;
  char **result;
  while (v && v[n]) {
    if (n == limit)
      return NULL;
    ++n;
  }
  result = calloc(n + 2, sizeof(*result));
  if (!result)
    return NULL;
  for (i = 0; i < n; ++i) {
    if (term && !strncmp(v[i], "TERM=", 5))
      continue;
    result[out] = copy(s, v[i]);
    if (!result[out]) {
      free_vector(result);
      return NULL;
    }
    ++out;
  }
  if (term) {
    size_t length = 0;
    while (length <= POLICY_BYTES - 6 && term[length])
      ++length;
    if (length > POLICY_BYTES - 6 ||
        length + 6 > POLICY_BYTES - s->policy_bytes) {
      free_vector(result);
      return NULL;
    }
    result[out] = malloc(length + 6);
    if (!result[out]) {
      free_vector(result);
      return NULL;
    }
    memcpy(result[out], "TERM=", 5);
    memcpy(result[out] + 5, term, length + 1);
    s->policy_bytes += length + 6;
  }
  return result;
}
void sl_terminal_session_config_init(sl_terminal_session_config_t *c) {
  if (c)
    memset(c, 0, sizeof(*c));
}
int sl_terminal_session_dispatch(sl_terminal_session_t *self,
                                 sl_terminal_event_t *e) {
  session_t *s = self ? self->impl : NULL;
  int result;
  if (!s || !e || s->busy)
    return SL_ERROR_INVALID;
  e->transferred = 0;
  if (e->operation < SL_TERMINAL_OPEN || e->operation > SL_TERMINAL_CLOSE)
    return SL_ERROR_INVALID;
  if ((e->operation == SL_TERMINAL_OPEN ||
       e->operation == SL_TERMINAL_RESIZE) &&
      (!e->rows || !e->columns || e->rows > 65535 || e->columns > 65535))
    return SL_ERROR_INVALID;
  if (e->operation == SL_TERMINAL_CLOSE) {
    if (s->closed)
      return SL_OK;
    s->closed = 1;
    return sl_pty_callback(&s->pty, e);
  }
  if (s->closed)
    return SL_ERROR_INVALID;
  if (e->operation == SL_TERMINAL_OPEN) {
    if (s->opened)
      return SL_ERROR_INVALID;
    s->opened = 1;
  } else if (!s->opened)
    return SL_ERROR_INVALID;
  if (e->operation == SL_TERMINAL_READ || e->operation == SL_TERMINAL_WRITE) {
    if (!e->data || !e->length || e->length > SL_TERMINAL_SESSION_CHUNK_MAX)
      return SL_ERROR_INVALID;
    if (!s->pty.ready)
      return SL_ERROR_NOT_READY;
  }
  s->busy = 1;
  result = sl_pty_callback(&s->pty, e);
  if (e->operation == SL_TERMINAL_STEP && s->pty.exec_failure &&
      result == SL_OK)
    result = SL_ERROR_IO;
  s->busy = 0;
  return result;
}
int sl_terminal_session_status(sl_terminal_session_t *self, int *out) {
  session_t *s = self ? self->impl : NULL;
  if (!s || !out || s->busy)
    return SL_ERROR_INVALID;
  *out = -1;
  if (!s->opened || (!sl_pty_reap(&s->pty) && !s->pty.exec_failure))
    return SL_ERROR_NOT_READY;
  *out = s->pty.status;
  return SL_OK;
}
int sl_terminal_session_destroy(sl_terminal_session_t *self) {
  session_t *s = self ? self->impl : NULL;
  sl_terminal_event_t e;
  if (!self)
    return SL_OK;
  if (!s || s->busy)
    return SL_ERROR_INVALID;
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_CLOSE;
  (void)sl_terminal_session_dispatch(self, &e);
  if (!sl_pty_reap(&s->pty))
    return SL_ERROR_NOT_READY;
  free(s->program);
  free(s->directory);
  free_vector(s->arguments);
  free_vector(s->environment);
  free(s);
  return SL_OK;
}
int sl_terminal_session_adapter(void *context, sl_terminal_event_t *e) {
  return sl_terminal_session_dispatch(context, e);
}
sl_terminal_session_t *
sl_terminal_session_create(const sl_terminal_session_config_t *c) {
  session_t *s;
  const char *program;
  const char *arguments[3];
  if (!c || (c->program && !*c->program))
    return NULL;
  s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->api.impl = s;
  s->api.dispatch = sl_terminal_session_dispatch;
  s->api.status = sl_terminal_session_status;
  s->api.destroy = sl_terminal_session_destroy;
  s->pty.fd = s->pty.exec_fd = -1;
  s->pty.status = -1;
  program = c->program ? c->program : getenv("SHELL");
  if (!program || !*program)
    program = "/bin/sh";
  arguments[0] = program;
  arguments[1] = "-i";
  arguments[2] = NULL;
  s->program = copy(s, program);
  if (c->directory)
    s->directory = copy(s, c->directory);
  s->arguments =
      vector(s, c->arguments ? c->arguments : arguments, NULL, POLICY_ITEMS);
  s->environment =
      vector(s,
             c->environment ? c->environment
                            : (const char *const *)session_environment,
             c->terminal_type, 1024u);
  if (!s->program || (c->directory && !s->directory) || !s->arguments ||
      !s->arguments[0] || !s->environment) {
    (void)sl_terminal_session_destroy(&s->api);
    return NULL;
  }
  s->pty.shell = s->program;
  s->pty.directory = s->directory;
  s->pty.arguments = s->arguments;
  s->pty.environment = s->environment;
  return &s->api;
}
