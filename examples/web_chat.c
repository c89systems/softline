#define _POSIX_C_SOURCE 200809L
#include "web_chat.h"
#include "completion_resolve.h"
#include "queue_demo_image.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define WEB_QUEUE_MAX 80u

typedef struct web_view {
  struct web_view *next;
  struct demo_web_chat *host;
  demo_www_peer_t *peer;
  sl_t *image;
  sl_image_paste_id_t image_id;
  char upload[65], mime[16], name[256];
  unsigned char bytes[SL_WWW_CHUNK_MAX];
  size_t length, offset, expected, received;
  int begun, ended, source_closed, credit, done, aborted, inflight;
  demo_image_store_t store;
  demo_link_t storage_reply;
  demo_packet_t storage_pending;
  unsigned long storage_kind;
  int storage_paused;
} web_view_t;
struct demo_web_chat {
  demo_www_t *www;
  sl_t *editor;
  demo_web_ops_t ops;
  void *context;
  web_view_t *views;
  unsigned long serial;
  char response[40], session[96];
  char *status;
  int busy;
  size_t image_limit;
  /* Example idempotency window. A session epoch prevents retrying requests
   * into a restarted volatile owner with no outcome records. */
  struct {
    char id[65];
    char view[65];
    char *text;
    unsigned long sequence;
    sl_queue_request_id_t backend_request;
    int status, steer;
  } requests[128];
  struct {
    char view[65];
    unsigned long high;
  } actors[128];
  size_t next_request;
  /* Bounded host-owned queue projection, separate from conversation replay. */
  struct {
    char id[128], revision[128];
    char *text;
    int steer, held;
  } queue[WEB_QUEUE_MAX];
  size_t queue_count, queue_build;
  int queue_changed;
};
static const char *field(const sl_www_request_t *r, const char *name) {
  const char *value = sl_www_field(r, name);
  return value ? value : "";
}
static int reply(web_view_t *v, const char *type, const char *name,
                 const char *value) {
  sl_www_field_t f[2] = {{"id", NULL, 0}, {NULL, NULL, 0}};
  f[0].value = v->upload;
  f[1].name = name;
  f[1].value = value;
  return demo_www_reply(v->peer, type, f, name ? 2 : 1);
}
static int source_next(void *context, sl_image_paste_event_t *e) {
  web_view_t *v = context;
  memset(e, 0, sizeof(*e));
  e->media_type = v->mime;
  e->filename = v->name;
  e->offset = v->offset;
  if (!v->begun) {
    v->begun = 1;
    e->operation = SL_IMAGE_PASTE_BEGIN;
    return 1;
  }
  if (v->length) {
    e->operation = SL_IMAGE_PASTE_DATA;
    e->data = v->bytes;
    e->length = v->length;
    v->offset += v->length;
    v->length = 0;
    v->inflight = 1;
    return 1;
  }
  if (v->ended == 1) {
    v->ended = 2;
    e->operation = SL_IMAGE_PASTE_END;
    return 1;
  }
  return 0;
}
static int source_prepare(void *context, sl_image_paste_wait_t *wait) {
  web_view_t *v = context;
  memset(wait, 0, sizeof(*wait));
  wait->timeout_ms = !v->begun || v->length || v->ended == 1 ? 0 : -1;
  return SL_OK;
}
static void source_close(void *context) {
  ((web_view_t *)context)->source_closed = 1;
}
static const char *source_error(void *context) {
  (void)context;
  return "browser image source failed";
}
static int local_image(web_view_t *v, const sl_image_paste_event_t *e) {
  demo_packet_t p = {0};
  unsigned long kind;
  if (v->storage_pending.data) {
    if (e->operation == SL_IMAGE_PASTE_DATA) {
      v->storage_paused = 1;
      return SL_ERROR_NOT_READY;
    }
    return SL_ERROR_FULL;
  }
  if (e->operation == SL_IMAGE_PASTE_BEGIN)
    kind = DEMO_IMAGE_BEGIN;
  else if (e->operation == SL_IMAGE_PASTE_DATA)
    kind = DEMO_IMAGE_DATA;
  else if (e->operation == SL_IMAGE_PASTE_END)
    kind = DEMO_IMAGE_END;
  else if (e->operation == SL_IMAGE_PASTE_ABORT)
    kind = DEMO_IMAGE_ABORT;
  else
    kind = DEMO_IMAGE_DONE;
  demo_put_number(&p, e->id);
  if (kind == DEMO_IMAGE_BEGIN)
    demo_put_string(&p, e->media_type);
  else if (kind == DEMO_IMAGE_DATA) {
    demo_put_number(&p, (unsigned long)e->offset);
    demo_put_image_bytes(&p, e->data, e->length);
  } else if (kind == DEMO_IMAGE_END)
    demo_put_number(&p, (unsigned long)e->offset);
  if (p.failed) {
    demo_packet_free(&p);
    return SL_ERROR_NOMEM;
  }
  /* Callback work is bounded copying only. The owner loop performs storage. */
  v->storage_pending = p;
  v->storage_kind = kind;
  return SL_OK;
}
static int image_event(void *context, const sl_image_paste_event_t *e) {
  web_view_t *v = context;
  int result = v->host->ops.image
                   ? v->host->ops.image(v->host->context, v->image, e)
                   : local_image(v, e);
  if (e->operation == SL_IMAGE_PASTE_BEGIN ||
      e->operation == SL_IMAGE_PASTE_DATA) {
    if (result == SL_OK) {
      v->credit = 1;
      v->inflight = 0;
    }
  } else if (e->operation == SL_IMAGE_PASTE_DONE)
    v->done = 1;
  else if (e->operation == SL_IMAGE_PASTE_ABORT)
    v->aborted = 1;
  return result;
}
static int service_storage(web_view_t *v) {
  int result;
  if (!v->storage_pending.data)
    return SL_OK;
  result = demo_image_store_input(&v->store, &v->storage_reply, v->storage_kind,
                                  &v->storage_pending, 0);
  demo_packet_free(&v->storage_pending);
  if (result)
    return SL_ERROR_IO;
  if (v->storage_paused) {
    v->storage_paused = 0;
    (void)sl_image_paste_resume(v->image, v->image_id);
  }
  return SL_OK;
}
static int service_image(web_view_t *v) {
  unsigned int budget = 4;
  if (!v->image_id)
    return SL_OK;
  while (budget--) {
    if (sl_image_paste_pump(v->image) != SL_OK)
      return SL_ERROR;
    if (service_storage(v) != SL_OK)
      return SL_ERROR_IO;
    while (!v->host->ops.image && v->storage_reply.count) {
      demo_packet_t p = v->storage_reply.output[0];
      unsigned long kind, id;
      memmove(v->storage_reply.output, v->storage_reply.output + 1,
              (v->storage_reply.count - 1) * sizeof(p));
      --v->storage_reply.count;
      v->storage_reply.queued_bytes -= p.length;
      p.offset = 4;
      kind = demo_number(&p);
      id = demo_number(&p);
      if (id == v->image_id && kind == DEMO_IMAGE_RESULT) {
        const char *path = demo_string(&p), *reason = demo_string(&p);
        if (!p.failed) {
          if (!path && v->ended != 2)
            (void)sl_image_paste_cancel(v->image, id);
          else
            (void)sl_image_paste_complete(v->image, id, path, reason);
        }
      }
      demo_packet_free(&p);
    }
    if (service_storage(v) != SL_OK)
      return SL_ERROR_IO;
    if (v->done || v->aborted) {
      if (v->done)
        (void)reply(v, "image_done", "path", sl_buffer(v->image));
      else
        (void)reply(v, "image_error", "text", sl_last_error(v->image));
      v->image_id = 0;
      v->credit = 0;
      return SL_OK;
    }
    if (v->credit) {
      v->credit = 0;
      (void)reply(v, "image_credit", NULL, NULL);
    }
  }
  return SL_OK;
}
static void closed(void *context, demo_www_peer_t *peer) {
  demo_web_chat_t *w = context;
  web_view_t *v = demo_www_peer_get(peer), **at;
  if (!v)
    return;
  for (at = &w->views; *at && *at != v; at = &(*at)->next) {
  }
  if (*at)
    *at = v->next;
  sl_destroy(v->image);
  demo_image_store_close(&v->store);
  demo_link_close(&v->storage_reply);
  demo_packet_free(&v->storage_pending);
  free(v);
  demo_www_peer_data(peer, NULL);
}
static int replay(void *context, void *peer, const char *after) {
  demo_web_chat_t *w = context;
  web_view_t *v = calloc(1, sizeof(*v));
  sl_config_t config;
  sl_www_field_t f = {"id", NULL, 0};
  (void)after;
  if (!v)
    return SL_ERROR_NOMEM;
  v->host = w;
  v->peer = peer;
  v->store.fd = -1;
  demo_link_init(&v->storage_reply, -1);
  sl_config_init(&config);
  config.image_paste_callback = image_event;
  config.image_paste_context = v;
  config.image_paste_max_bytes = w->image_limit;
  v->image = sl_create_with_config(&config);
  if (!v->image || (!w->ops.image && demo_image_store_init(&v->store))) {
    sl_destroy(v->image);
    free(v);
    return SL_ERROR;
  }
  v->next = w->views;
  w->views = v;
  demo_www_peer_data(peer, v);
  f.value = w->session;
  (void)demo_www_reply(peer, "session", &f, 1);
  w->queue_changed = 1;
  demo_web_queue(w);
  {
    sl_www_field_t status[2] = {{"text", NULL, 0}, {"busy", NULL, 0}};
    status[0].value = w->status;
    status[1].value = w->busy ? "1" : "0";
    (void)demo_www_reply(peer, "status", status, 2);
  }
  return SL_OK;
}
typedef struct {
  const sl_www_request_t *request;
  const char *prefix;
  const char *ticket;
  char start[32], end[32];
  size_t count;
  int all;
} completion_t;
static int completion(const char *candidate, void *context) {
  completion_t *c = context;
  sl_www_field_t f[7] = {{"request", NULL, 0}, {"label", NULL, 0},
                         {"value", NULL, 0},   {"start", NULL, 0},
                         {"end", NULL, 0},     {"description", "", 0},
                         {"continue", "0", 0}};
  if (c->count >= 256 || !strcmp(candidate, "/shell") ||
      !strcmp(candidate, "/quit") ||
      (!c->all && strncmp(candidate, c->prefix, strlen(c->prefix))))
    return SL_OK;
  f[0].value = c->ticket;
  f[1].value = f[2].value = candidate;
  if (candidate[0] == '@') {
    size_t length = strlen(candidate);
    if (length == 1 || candidate[length - 1] == '/')
      f[6].value = "1";
    else
      f[2].value = candidate + 1;
  }
  f[3].value = c->start;
  f[4].value = c->end;
  ++c->count;
  return sl_www_emit(c->request->reply, c->request->reply_context, "complete",
                     NULL, f, 7);
}
static int complete(demo_web_chat_t *w, const sl_www_request_t *r) {
  const char *text = field(r, "text");
  char *end, *prefix;
  unsigned long cursor;
  size_t start, finish;
  completion_t c;
  int result;
  errno = 0;
  cursor = strtoul(field(r, "cursor"), &end, 10);
  if (errno || *end || cursor > strlen(text) ||
      ((unsigned char)text[cursor] & 192) == 128)
    return SL_ERROR_INVALID;
  start = finish = (size_t)cursor;
  while (start && text[start - 1] != ' ' && text[start - 1] != '\t' &&
         text[start - 1] != '\n')
    --start;
  while (text[finish] && text[finish] != ' ' && text[finish] != '\t' &&
         text[finish] != '\n')
    ++finish;
  prefix = malloc((size_t)cursor - start + 1);
  if (!prefix)
    return SL_ERROR_NOMEM;
  memcpy(prefix, text + start, (size_t)cursor - start);
  prefix[(size_t)cursor - start] = 0;
  memset(&c, 0, sizeof(c));
  c.request = r;
  c.prefix = prefix;
  c.ticket = field(r, "request");
  c.all = !strcmp(field(r, "all"), "1");
  (void)snprintf(c.start, sizeof(c.start), "%lu", (unsigned long)start);
  (void)snprintf(c.end, sizeof(c.end), "%lu", (unsigned long)finish);
  if (c.all) {
    static const char *const triggers[] = {"/", "h", "v", "n"};
    size_t i;
    result = SL_OK;
    for (i = 0; i < sizeof(triggers) / sizeof(triggers[0]) && result == SL_OK;
         ++i)
      result = demo_resolve(w->editor, triggers[i], completion, &c, w);
    if (result == SL_OK)
      result = completion("@", &c);
  } else if (prefix[0] == '@' || !strcmp(prefix, "/approve"))
    result = demo_resolve(w->editor, prefix, completion, &c, w);
  else {
    char trigger[2];
    trigger[0] = prefix[0];
    trigger[1] = 0;
    result = demo_resolve(w->editor, trigger, completion, &c, w);
  }
  if (!c.count && result == SL_OK) {
    sl_www_field_t f = {"request", NULL, 0};
    f.value = c.ticket;
    result = sl_www_emit(r->reply, r->reply_context, "complete", NULL, &f, 1);
  }
  free(prefix);
  return result;
}
static int action(void *context, const sl_www_request_t *r) {
  demo_web_chat_t *w = context;
  web_view_t *v = demo_www_peer_get(r->peer);
  const char *a = field(r, "action"), *id = field(r, "id"),
             *request = field(r, "request");
  int result = SL_ERROR_INVALID, deferred = 0;
  if (!v)
    return SL_ERROR_INVALID;
  if (!strcmp(a, "complete"))
    return complete(w, r);
  if (!strcmp(a, "image_begin")) {
    sl_image_paste_source_t source;
    char *end;
    unsigned long length;
    const char *mime = field(r, "media_type");
    if (v->image_id || !*id || strlen(id) > 64 ||
        (strcmp(mime, "image/png") && strcmp(mime, "image/jpeg")))
      goto bad_image;
    errno = 0;
    length = strtoul(field(r, "size"), &end, 10);
    if (errno || *end || length > w->image_limit || length < 3)
      goto bad_image;
    (void)snprintf(v->upload, sizeof(v->upload), "%s", id);
    (void)snprintf(v->mime, sizeof(v->mime), "%s", mime);
    (void)snprintf(v->name, sizeof(v->name), "%s", field(r, "filename"));
    v->expected = (size_t)length;
    v->received = v->length = v->offset = 0;
    v->begun = v->ended = v->source_closed = v->credit = v->done = v->aborted =
        v->inflight = 0;
    if (sl_set_buffer(v->image, "") != SL_OK)
      goto bad_image;
    memset(&source, 0, sizeof(source));
    source.context = v;
    source.next = source_next;
    source.prepare = source_prepare;
    source.close = source_close;
    source.error = source_error;
    result = sl_image_paste_start_source(v->image, &source, &v->image_id);
    if (result != SL_OK)
      goto bad_image;
    return service_image(v);
  }
  if (!strcmp(a, "image_cancel")) {
    if (v->image_id && !strcmp(id, v->upload))
      (void)sl_image_paste_cancel(v->image, v->image_id);
    return service_image(v);
  }
  if (!strcmp(a, "image_end")) {
    if (!v->image_id || strcmp(id, v->upload) || v->received != v->expected ||
        v->received < (!strcmp(v->mime, "image/png") ? 8u : 3u))
      goto bad_image;
    v->ended = 1;
    return service_image(v);
  }
  if (!strcmp(a, "send") || !strcmp(a, "steer")) {
    size_t i, actor;
    const char *view = field(r, "view"), *text = field(r, "text");
    unsigned long sequence;
    char *end;
    int steer = !strcmp(a, "steer");
    errno = 0;
    sequence = strtoul(field(r, "request_seq"), &end, 10);
    if (!*request || strlen(request) > 64 || !*field(r, "text") ||
        strlen(field(r, "text")) > DEMO_PROMPT_MAX || !*view ||
        strlen(view) > 64 || errno || *end || !sequence ||
        *field(r, "request_seq") < '0' || *field(r, "request_seq") > '9')
      goto report;
    for (actor = 0; actor < 128; ++actor)
      if (!strcmp(w->actors[actor].view, view))
        break;
    if (actor == 128) {
      for (actor = 0; actor < 128 && w->actors[actor].view[0]; ++actor) {
      }
      if (actor == 128) {
        result = SL_ERROR_FULL;
        goto report;
      }
      (void)snprintf(w->actors[actor].view, 65, "%s", view);
    }
    for (i = 0; i < 128; ++i)
      if (!strcmp(w->requests[i].id, request)) {
        result = !strcmp(w->requests[i].view, view) &&
                         w->requests[i].sequence == sequence &&
                         w->requests[i].steer == steer &&
                         !strcmp(w->requests[i].text, text)
                     ? w->requests[i].status
                     : SL_ERROR_CONFLICT;
        deferred = result == SL_OK && w->requests[i].backend_request != 0;
        break;
      }
    if (i == 128 && sequence <= w->actors[actor].high)
      result = SL_ERROR_CONFLICT;
    else if (i == 128) {
      char *copy = malloc(strlen(text) + 1);
      if (!copy) {
        result = SL_ERROR_NOMEM;
        goto report;
      }
      strcpy(copy, text);
      {
        size_t tries;
        for (tries = 0;
             tries < 128 && w->requests[w->next_request].backend_request;
             ++tries)
          w->next_request = (w->next_request + 1) % 128;
        if (tries == 128) {
          free(copy);
          result = SL_ERROR_FULL;
          goto report;
        }
      }
      w->actors[actor].high = sequence;
      w->requests[w->next_request].backend_request = 0;
      result =
          w->ops.submit
              ? w->ops.submit(w->context, text, steer,
                              &w->requests[w->next_request].backend_request)
              : SL_ERROR_INVALID;
      if (result != SL_OK)
        w->requests[w->next_request].backend_request = 0;
      deferred = w->requests[w->next_request].backend_request != 0;
      free(w->requests[w->next_request].text);
      (void)snprintf(w->requests[w->next_request].id, 65, "%s", request);
      (void)snprintf(w->requests[w->next_request].view, 65, "%s", view);
      w->requests[w->next_request].sequence = sequence;
      w->requests[w->next_request].steer = steer;
      w->requests[w->next_request].text = copy;
      w->requests[w->next_request].status = result;
      w->next_request = (w->next_request + 1) % 128;
    }
  } else if (w->ops.control)
    result = w->ops.control(w->context, a, id, field(r, "revision"));
report:
  if (!deferred) {
    sl_www_field_t f[2] = {{"request", NULL, 0}, {"text", NULL, 0}};
    f[0].value = request;
    f[1].value = result == SL_OK ? "" : "The host rejected this action.";
    (void)sl_www_emit(r->reply, r->reply_context,
                      result == SL_OK ? "accepted" : "error", NULL, f, 2);
  }
  demo_web_queue(w);
  return SL_OK;
bad_image:
  {
    sl_www_field_t error[2] = {
        {"id", NULL, 0}, {"text", "Invalid or unsupported image upload.", 0}};
    error[0].value = id;
    if (v->image_id && !strcmp(id, v->upload)) {
      (void)sl_image_paste_cancel(v->image, v->image_id);
      (void)service_image(v);
    }
    (void)sl_www_emit(r->reply, r->reply_context, "image_error", NULL, error,
                      2);
    return SL_OK;
  }
}
static int binary(void *context, const sl_www_request_t *r, const void *data,
                  size_t length) {
  web_view_t *v = demo_www_peer_get(r->peer);
  const unsigned char *bytes = data;
  static const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10},
                             jpeg[] = {255, 216, 255};
  const unsigned char *signature;
  size_t i, signature_length;
  (void)context;
  if (!v || !v->image_id || v->ended || v->length || v->inflight ||
      length > v->expected - v->received)
    return SL_ERROR_INVALID;
  signature = !strcmp(v->mime, "image/png") ? png : jpeg;
  signature_length = !strcmp(v->mime, "image/png") ? 8 : 3;
  for (i = 0; i < length && v->received + i < signature_length; ++i)
    if (bytes[i] != signature[v->received + i]) {
      (void)sl_image_paste_cancel(v->image, v->image_id);
      (void)service_image(v);
      return SL_OK;
    }
  memcpy(v->bytes, data, length);
  v->length = length;
  v->received += length;
  return service_image(v);
}
demo_web_chat_t *demo_web_open(sl_t *editor, const demo_web_ops_t *ops,
                               void *context, unsigned short default_port,
                               int *enabled) {
  const char *on = getenv("SOFTLINE_WEB"), *port = getenv("SOFTLINE_WEB_PORT");
  demo_web_chat_t *w;
  sl_www_config_t config;
  struct timespec now;
  unsigned long number = default_port;
  char *end;
  *enabled = on && !strcmp(on, "1");
  if (on && strcmp(on, "0") && strcmp(on, "1")) {
    *enabled = 1;
    errno = EINVAL;
    return NULL;
  }
  if (!*enabled)
    return NULL;
  if (port) {
    errno = 0;
    number = strtoul(port, &end, 10);
    if (errno || !*port || *end || number > 65535) {
      errno = EINVAL;
      return NULL;
    }
  }
  w = calloc(1, sizeof(*w));
  if (!w)
    return NULL;
  w->editor = editor;
  w->ops = *ops;
  w->context = context;
  if (demo_image_limit(&w->image_limit)) {
    free(w);
    errno = EINVAL;
    return NULL;
  }
  (void)clock_gettime(CLOCK_REALTIME, &now);
  (void)snprintf(w->session, sizeof(w->session), "%lu-%lu-%lu",
                 (unsigned long)getpid(), (unsigned long)now.tv_sec,
                 (unsigned long)now.tv_nsec);
  sl_www_config_init(&config);
  config.enabled = 1;
  config.action = action;
  config.binary = binary;
  config.replay = replay;
  config.context = w;
  config.image_max_bytes = w->image_limit;
  if (getenv("SOFTLINE_QUEUE_DISPLAY_DELAY_MS")) {
    long delay;
    errno = 0;
    delay = strtol(getenv("SOFTLINE_QUEUE_DISPLAY_DELAY_MS"), &end, 10);
    if (errno || *end || delay < 0 || delay > INT_MAX) {
      free(w);
      errno = EINVAL;
      return NULL;
    }
    config.queue_display_delay_ms = (int)delay;
  }
  if (getenv("SOFTLINE_WEB_TITLE"))
    config.title = getenv("SOFTLINE_WEB_TITLE");
  if (getenv("SOFTLINE_WEB_WELCOME"))
    config.welcome = getenv("SOFTLINE_WEB_WELCOME");
  config.banner = getenv("SOFTLINE_WEB_BANNER");
  w->www = demo_www_open(&config, (unsigned short)number, closed);
  if (!w->www) {
    demo_web_close(w);
    return NULL;
  }
  return w;
}
void demo_web_close(demo_web_chat_t *w) {
  if (w) {
    size_t i;
    demo_www_close(w->www);
    for (i = 0; i < 128; ++i)
      free(w->requests[i].text);
    for (i = 0; i < WEB_QUEUE_MAX; ++i)
      free(w->queue[i].text);
    free(w->status);
    free(w);
  }
}
int demo_web_tick(demo_web_chat_t *w) {
  web_view_t *v;
  if (!w)
    return 0;
  for (v = w->views; v; v = v->next)
    if (service_image(v) != SL_OK)
      return -1;
  return demo_www_poll(w->www, 0);
}
void demo_web_result(demo_web_chat_t *w, const sl_queue_result_t *result) {
  size_t i;
  if (!w || !result)
    return;
  for (i = 0; i < 128; ++i)
    if (w->requests[i].backend_request == result->request_id &&
        result->request_id) {
      sl_www_field_t fields[3] = {
          {"request", NULL, 0}, {"view", NULL, 0}, {"text", NULL, 0}};
      int ok = result->status == SL_QUEUE_ACCEPTED;
      fields[0].value = w->requests[i].id;
      fields[1].value = w->requests[i].view;
      fields[2].value =
          ok ? ""
          : result->status == SL_QUEUE_UNKNOWN
              ? "The backend cannot confirm this prompt's outcome. Inspect the "
                "conversation before recovering it under +."
          : result->reason && strlen(result->reason) <= 4096
              ? result->reason
              : "The backend rejected the prompt.";
      (void)demo_www_publish(w->www, ok ? "accepted" : "error", fields, 3);
      w->requests[i].backend_request = 0;
      w->requests[i].status = ok ? SL_OK : SL_ERROR;
      break;
    }
}
int demo_web_timeout(demo_web_chat_t *w, int timeout_ms) {
  web_view_t *v;
  if (!w)
    return timeout_ms;
  for (v = w->views; v; v = v->next)
    if (v->image_id) {
      sl_image_paste_wait_t wait;
      if (sl_image_paste_prepare(v->image, &wait) != SL_OK)
        return 0;
      if (wait.timeout_ms >= 0 &&
          (timeout_ms < 0 || wait.timeout_ms < timeout_ms))
        timeout_ms = wait.timeout_ms;
    }
  return timeout_ms;
}
unsigned short demo_web_port(demo_web_chat_t *w) {
  return w ? demo_www_port(w->www) : 0;
}
demo_www_t *demo_web_listener(demo_web_chat_t *w) { return w ? w->www : NULL; }
void demo_web_queue_begin(demo_web_chat_t *w) {
  if (w)
    w->queue_build = 0;
}
void demo_web_queue_entry(demo_web_chat_t *w, const char *id,
                          const char *revision, const char *text, int steer,
                          int held) {
  size_t i;
  char *copy;
  if (!w || w->queue_build == WEB_QUEUE_MAX || strlen(id) >= 128 ||
      strlen(revision) >= 128 || strlen(text) > 128u * 1024u)
    return;
  i = w->queue_build++;
  if (w->queue[i].text && !strcmp(w->queue[i].text, text) &&
      !strcmp(w->queue[i].id, id) && !strcmp(w->queue[i].revision, revision) &&
      w->queue[i].steer == steer && w->queue[i].held == held)
    return;
  copy = malloc(strlen(text) + 1);
  if (!copy)
    return;
  strcpy(copy, text);
  free(w->queue[i].text);
  w->queue[i].text = copy;
  strcpy(w->queue[i].id, id);
  strcpy(w->queue[i].revision, revision);
  w->queue[i].steer = steer;
  w->queue[i].held = held;
  w->queue_changed = 1;
}
static void publish_queue_entry(demo_web_chat_t *w, size_t index) {
  const char *id = w->queue[index].id, *revision = w->queue[index].revision,
             *text = w->queue[index].text;
  int steer = w->queue[index].steer, held = w->queue[index].held;
  sl_www_field_t f[4] = {{"id", NULL, 0},
                         {"revision", NULL, 0},
                         {"mode", NULL, 0},
                         {"held", NULL, 0}};
  char *encoded;
  size_t length;
  if (!w)
    return;
  f[0].value = id;
  f[1].value = revision;
  f[2].value = steer == 2 ? "pending" : steer ? "steer" : "queued";
  f[3].value = held ? "1" : "0";
  (void)demo_www_publish(w->www, "queue_item", f, 4);
  encoded = malloc(5500);
  if (!encoded)
    return;
  length = strlen(text);
  while (length) {
    size_t n = length > 4096 ? 4096 : length;
    sl_www_field_t part[3] = {
        {"id", NULL, 0}, {"data", NULL, 0}, {"encoding", "base64", 0}};
    part[0].value = id;
    part[1].value = encoded;
    part[1].length = demo_www_base64((const unsigned char *)text, n, encoded);
    (void)demo_www_publish(w->www, "queue_text", part, 3);
    text += n;
    length -= n;
  }
  free(encoded);
}
void demo_web_queue_end(demo_web_chat_t *w) {
  size_t i;
  if (!w)
    return;
  if (w->queue_build != w->queue_count)
    w->queue_changed = 1;
  for (i = w->queue_build; i < w->queue_count; ++i) {
    free(w->queue[i].text);
    w->queue[i].text = NULL;
  }
  w->queue_count = w->queue_build;
  if (!w->queue_changed)
    return;
  (void)demo_www_publish(w->www, "queue_begin", NULL, 0);
  for (i = 0; i < w->queue_count; ++i)
    if (w->queue[i].text)
      publish_queue_entry(w, i);
  (void)demo_www_publish(w->www, "queue_end", NULL, 0);
  w->queue_changed = 0;
}
void demo_web_queue(demo_web_chat_t *w) {
  size_t i;
  sl_queue_backend_info_t backend;
  if (!w)
    return;
  if (w->ops.snapshot) {
    w->ops.snapshot(w->context, w);
    return;
  }
  if (!w->editor)
    return;
  demo_web_queue_begin(w);
  if (sl_queue_backend_info(w->editor, &backend) == SL_OK) {
    for (i = 0; i < backend.visible_count; ++i) {
      sl_queue_entry_t e;
      if (sl_queue_backend_entry(w->editor, i, &e) == SL_OK)
        demo_web_queue_entry(w, e.id, e.revision, e.text,
                             e.mode == SL_PROMPT_QUEUE_MODE_STEER, e.held);
    }
    for (i = 0; i < backend.pending_count; ++i) {
      sl_queue_pending_info_t pending;
      char id[64];
      if (sl_queue_backend_pending(w->editor, i, &pending) != SL_OK ||
          pending.request.operation != SL_QUEUE_OFFER ||
          !pending.request.text ||
          (!pending.pending && pending.status != SL_QUEUE_UNKNOWN))
        continue;
      (void)snprintf(id, sizeof(id), "pending-%lu", pending.request.request_id);
      demo_web_queue_entry(w, id, "", pending.request.text, 2, 0);
    }
  } else
    for (i = 0; i < sl_prompt_queue_count(w->editor); ++i) {
      sl_prompt_queue_entry_info_t info;
      char *text = NULL, id[32];
      if (sl_prompt_queue_info(w->editor, i, &info) == SL_OK &&
          sl_prompt_queue_peek(w->editor, i, &text) == SL_OK) {
        (void)snprintf(id, sizeof(id), "%lu", info.id);
        demo_web_queue_entry(w, id, "", text,
                             info.mode == SL_PROMPT_QUEUE_MODE_STEER,
                             info.held);
        sl_free_string(w->editor, text);
      }
    }
  demo_web_queue_end(w);
}
void demo_web_status(demo_web_chat_t *w, const char *text, int busy) {
  sl_www_field_t f[2] = {{"text", NULL, 0}, {"busy", NULL, 0}};
  char *copy;
  if (!w)
    return;
  if (w->status && !strcmp(w->status, text ? text : "") && w->busy == busy)
    return;
  copy = malloc(strlen(text ? text : "") + 1);
  if (!copy)
    return;
  strcpy(copy, text ? text : "");
  free(w->status);
  w->status = copy;
  w->busy = busy;
  f[0].value = copy;
  f[1].value = busy ? "1" : "0";
  (void)demo_www_publish(w->www, "status", f, 2);
}
void demo_web_message(demo_web_chat_t *w, const char *role, const char *text) {
  char id[40];
  if (!w)
    return;
  (void)snprintf(id, sizeof(id), "m%lu", ++w->serial);
  demo_www_begin(w->www, id, role);
  demo_www_text(w->www, id, role, text, strlen(text));
  demo_www_end(w->www, id);
}
void demo_web_response(demo_web_chat_t *w, const void *bytes, size_t length) {
  if (!w || !length)
    return;
  if (!w->response[0]) {
    (void)snprintf(w->response, sizeof(w->response), "m%lu", ++w->serial);
    demo_www_begin(w->www, w->response, "assistant");
  }
  demo_www_text(w->www, w->response, "assistant", bytes, length);
}
void demo_web_response_end(demo_web_chat_t *w) {
  if (w && w->response[0]) {
    demo_www_end(w->www, w->response);
    w->response[0] = 0;
  }
}
