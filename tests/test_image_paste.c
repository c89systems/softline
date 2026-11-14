#define _GNU_SOURCE 1
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <softline/softline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "image line %d: %s\n", __LINE__, #c);                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)
extern size_t image_fixture_size, image_fixture_reads;
extern int image_fixture_gate, image_fixture_fail;
extern const char *image_fixture_mime;
unsigned char image_fixture_byte(size_t);
struct image_api {
  int (*start)(sl_t *, sl_image_paste_id_t *);
  int (*prepare)(sl_t *, sl_image_paste_wait_t *);
  int (*pump)(sl_t *);
  int (*resume)(sl_t *, sl_image_paste_id_t);
  int (*complete)(sl_t *, sl_image_paste_id_t, const char *, const char *);
  int (*cancel)(sl_t *, sl_image_paste_id_t);
};
static const struct image_api free_api = {
    sl_image_paste_start,  sl_image_paste_prepare,  sl_image_paste_pump,
    sl_image_paste_resume, sl_image_paste_complete, sl_image_paste_cancel};
struct host {
  sl_t *sl;
  sl_image_paste_id_t id;
  size_t received;
  const struct image_api *api;
  int begin, end, aborts, done, pause, paused, reject, failed;
};
static int receive_image(void *context, const sl_image_paste_event_t *e) {
  struct host *h = context;
  size_t i;
  if (!h->id)
    h->id = e->id;
  if (e->id != h->id)
    h->failed = 1;
  if (e->operation == SL_IMAGE_PASTE_BEGIN) {
    ++h->begin;
    if (!e->media_type || strcmp(e->media_type, image_fixture_mime) ||
        h->received)
      h->failed = 1;
    if (h->reject == 1)
      return SL_ERROR;
    /* Reentrancy is rejected, including destruction. */
    if (sl_insert(h->sl, "bad") != SL_ERROR_INVALID)
      h->failed = 1;
    sl_destroy(h->sl);
  } else if (e->operation == SL_IMAGE_PASTE_DATA) {
    if (h->reject == 2)
      return SL_ERROR;
    if (e->offset != h->received || !e->length || e->length > 16384)
      h->failed = 1;
    for (i = 0; i < e->length; ++i)
      if (e->data[i] != image_fixture_byte(e->offset + i))
        h->failed = 1;
    if (h->pause) {
      h->paused = 1;
      return SL_ERROR_NOT_READY;
    }
    h->received += e->length;
  } else if (e->operation == SL_IMAGE_PASTE_END) {
    if (e->offset != h->received)
      h->failed = 1;
    ++h->end;
  } else if (e->operation == SL_IMAGE_PASTE_ABORT) {
    if (!e->reason || e->offset != h->received)
      h->failed = 1;
    ++h->aborts;
  } else if (e->operation == SL_IMAGE_PASTE_DONE) {
    if (!h->end || h->aborts || e->offset != h->received || h->done)
      h->failed = 1;
    ++h->done;
  } else
    h->failed = 1;
  return SL_OK;
}
static sl_t *editor(struct host *h, size_t limit, size_t line_limit,
                    int timeout) {
  sl_config_t config;
  memset(h, 0, sizeof(*h));
  sl_config_init(&config);
  config.image_paste_callback = receive_image;
  config.image_paste_context = h;
  config.image_paste_max_bytes = limit;
  config.line_max_len = line_limit;
  config.image_paste_source_timeout_ms = timeout;
  config.image_paste_host_timeout_ms = timeout;
  config.image_paste_total_timeout_ms = timeout;
  h->sl = sl_create_with_config(&config);
  return h->sl;
}
static int drive(struct host *h) {
  unsigned int i;
  for (i = 0; i < 10000 && !h->end && !h->aborts && !h->paused; ++i)
    CHECK((h->api ? h->api->pump(h->sl) : sl_image_paste_pump(h->sl)) == SL_OK);
  CHECK(i < 10000 && !h->failed);
  return 0;
}
static int basic(int receiver) {
  struct host h;
  sl_t *sl;
  sl_image_paste_id_t id, another;
  sl_image_paste_wait_t wait;
  size_t reads;
  struct image_api api = free_api;
  image_fixture_size = 32769;
  image_fixture_gate = 1;
  sl = editor(&h, 0, 131072, 0);
  CHECK(sl && sl_set_buffer(sl, "AB") == SL_OK &&
        sl_set_cursor(sl, 1) == SL_OK);
  if (receiver) {
    api.start = sl->image_paste_start;
    api.prepare = sl->image_paste_prepare;
    api.pump = sl->image_paste_pump;
    api.resume = sl->image_paste_resume;
    api.complete = sl->image_paste_complete;
    api.cancel = sl->image_paste_cancel;
  }
  CHECK(api.start && api.prepare && api.pump && api.resume && api.complete &&
        api.cancel);
  h.api = &api;
  CHECK(api.start(sl, &id) == SL_OK);
  CHECK(api.start(sl, &another) == SL_ERROR_INVALID);
  CHECK(api.complete(sl, id, "/backend/image.png", NULL) == SL_ERROR_INVALID);
  h.pause = 1;
  CHECK(drive(&h) == 0 && h.begin == 1 && h.paused && h.received == 0);
  reads = image_fixture_reads;
  CHECK(api.prepare(sl, &wait) == SL_OK && wait.count == 0 &&
        wait.timeout_ms > 0);
  CHECK(sl_insert(sl, "X") == SL_OK && !strcmp(sl_buffer(sl), "AXB"));
  CHECK(sl_set_cursor(sl, 0) == SL_OK && sl_insert(sl, "Y") == SL_OK);
  CHECK(sl_set_cursor(sl, 4) == SL_OK && sl_insert(sl, "Z") == SL_OK);
  CHECK(api.pump(sl) == SL_OK && image_fixture_reads == reads);
  CHECK(api.resume(sl, id + 1) == SL_ERROR_INVALID);
  CHECK(api.resume(sl, id) == SL_OK);
  h.paused = 0;
  CHECK(drive(&h) == 0 && h.paused && image_fixture_reads == reads);
  h.pause = h.paused = 0;
  CHECK(api.resume(sl, id) == SL_OK && api.pump(sl) == SL_OK);
  CHECK(h.received == 16384 && !h.end);
  CHECK(api.pump(sl) == SL_OK && image_fixture_reads == reads && !h.end);
  image_fixture_gate = 0;
  CHECK(drive(&h) == 0 && h.end == 1 && h.received == image_fixture_size);
  CHECK(!strcmp(sl_buffer(sl), "YAXBZ"));
  CHECK(api.prepare(sl, &wait) == SL_OK && wait.count == 0 &&
        wait.timeout_ms > 0);
  CHECK(api.complete(sl, id, "relative.png", NULL) == SL_ERROR_INVALID);
  CHECK(api.complete(sl, id, "/bad\033path", NULL) == SL_ERROR_INVALID);
  CHECK(api.complete(sl, id, "/backend/image.png", NULL) == SL_OK);
  CHECK(!strcmp(sl_buffer(sl), "YAXBZ"));
  CHECK(api.complete(sl, id, "/backend/duplicate", NULL) == SL_ERROR_INVALID);
  CHECK(api.pump(sl) == SL_OK &&
        !strcmp(sl_buffer(sl), "YA/backend/image.pngXBZ"));
  CHECK(api.cancel(sl, id) == SL_ERROR_INVALID && !h.aborts);
  CHECK(h.done == 1 && !h.failed);
  memset(&h, 0, sizeof(h));
  h.sl = sl;
  h.api = &api;
  CHECK(api.start(sl, &id) == SL_OK && api.cancel(sl, id) == SL_OK);
  CHECK(h.aborts == 1 && !h.failed && api.cancel(sl, id) == SL_ERROR_INVALID);
  sl_destroy(sl);
  return 0;
}
static int limits(void) {
  size_t sizes[] = {33554432u, 33554433u, 32u, 33u, 33554433u, 33554434u};
  size_t limits[] = {0, 0, 32u, 32u, 33554433u, 33554433u};
  size_t i;
  for (i = 0; i < 6; ++i) {
    struct host h;
    sl_image_paste_id_t id;
    sl_t *sl = editor(&h, limits[i], 100, 0);
    CHECK(sl);
    image_fixture_size = sizes[i];
    CHECK(sl_image_paste_start(sl, &id) == SL_OK && drive(&h) == 0);
    if (i & 1) {
      CHECK(h.aborts == 1 && !h.end &&
            h.received <= (limits[i] ? limits[i] : 33554432u));
      CHECK(sl_image_paste_complete(sl, id, "/late", NULL) == SL_ERROR_INVALID);
    } else {
      CHECK(h.end == 1 && !h.aborts && h.received == sizes[i]);
      CHECK(sl_image_paste_complete(sl, id, "/stored", NULL) == SL_OK);
      CHECK(sl_image_paste_pump(sl) == SL_OK &&
            !strcmp(sl_buffer(sl), "/stored"));
    }
    sl_destroy(sl);
  }
  return 0;
}
static int failures(void) {
  struct host h;
  sl_t *sl;
  sl_image_paste_id_t id;
  int mode;
  image_fixture_size = 64;
  for (mode = 0; mode < 9; ++mode) {
    sl = editor(&h, 0, 8, 0);
    CHECK(sl && sl_set_buffer(sl, "keep") == SL_OK &&
          sl_image_paste_start(sl, &id) == SL_OK);
    if (mode == 0) {
      CHECK(sl_image_paste_cancel(sl, id) == SL_OK);
      CHECK(sl_image_paste_cancel(sl, id) == SL_ERROR_INVALID);
    } else if (mode == 1)
      CHECK(sl_set_buffer(sl, "new") == SL_OK);
    else if (mode == 2) {
      h.reject = 1;
      CHECK(drive(&h) == 0);
    } else if (mode == 3) {
      image_fixture_fail = 1;
      CHECK(drive(&h) == 0);
      image_fixture_fail = 0;
    } else if (mode == 4) {
      CHECK(drive(&h) == 0 && h.end == 1);
      CHECK(sl_image_paste_complete(sl, id, "/long-path", NULL) == SL_OK);
      CHECK(sl_image_paste_pump(sl) == SL_OK);
    } else if (mode == 5) {
      CHECK(drive(&h) == 0 && h.end == 1);
      CHECK(sl_image_paste_complete(sl, id, NULL, "backend publish failed") ==
            SL_OK);
    } else if (mode == 6) {
      h.reject = 2;
      CHECK(drive(&h) == 0);
    } else if (mode == 7) {
      CHECK(sl_set_buffer(sl, "replacement exceeds line limit") == SL_ERROR);
      CHECK(!h.aborts && !strcmp(sl_buffer(sl), "keep"));
      CHECK(sl_image_paste_cancel(sl, id) == SL_OK);
    } else {
      sl_destroy(sl);
      sl = NULL;
    }
    CHECK(h.aborts == 1 && !h.failed);
    if (sl) {
      CHECK(!strcmp(sl_buffer(sl), mode == 1 ? "new" : "keep"));
      CHECK(sl_image_paste_complete(sl, id, "/late", NULL) == SL_ERROR_INVALID);
      sl_destroy(sl);
      CHECK(h.aborts == 1);
    }
  }
  return 0;
}
static int deadlines(void) {
  int phase;
  image_fixture_size = 64;
  for (phase = 0; phase < 3; ++phase) {
    struct host h;
    sl_image_paste_wait_t wait;
    sl_image_paste_id_t id;
    sl_t *sl = editor(&h, 0, 100, 20);
    CHECK(sl && sl_image_paste_start(sl, &id) == SL_OK);
    if (phase == 0) {
      image_fixture_gate = 1;
      CHECK(sl_image_paste_pump(sl) == SL_OK &&
            sl_image_paste_pump(sl) == SL_OK);
    }
    if (phase == 1) {
      h.pause = 1;
      CHECK(drive(&h) == 0 && h.paused);
    }
    if (phase == 2)
      CHECK(drive(&h) == 0 && h.end);
    CHECK(sl_image_paste_prepare(sl, &wait) == SL_OK && wait.timeout_ms >= 0);
    CHECK(poll(NULL, 0, wait.timeout_ms + 1) == 0);
    if (phase == 1)
      CHECK(sl_image_paste_resume(sl, id) == SL_ERROR_INVALID);
    else if (phase == 2)
      CHECK(sl_image_paste_complete(sl, id, "/late", NULL) == SL_ERROR_INVALID);
    else
      CHECK(sl_image_paste_pump(sl) == SL_OK);
    CHECK(h.aborts == 1 && !h.failed);
    image_fixture_gate = 0;
    sl_destroy(sl);
  }
  return 0;
}
struct native {
  struct host host;
  int master, stage, failed, cancel, suspend, queued, literal, completions;
};
static int deadline_domains(void) {
  int mode;
  for (mode = 0; mode < 5; ++mode) {
    struct host h;
    sl_config_t config;
    sl_image_paste_id_t id;
    memset(&h, 0, sizeof(h));
    sl_config_init(&config);
    config.image_paste_callback = receive_image;
    config.image_paste_context = &h;
    config.image_paste_source_timeout_ms = mode >= 3 ? 1000 : 20;
    config.image_paste_host_timeout_ms = mode >= 3 ? 20 : 1000;
    config.image_paste_total_timeout_ms = mode == 1 ? 20 : 2000;
    h.sl = sl_create_with_config(&config);
    CHECK(h.sl);
    h.pause = mode != 2 && mode != 4;
    CHECK(sl_image_paste_start(h.sl, &id) == SL_OK && drive(&h) == 0);
    CHECK(mode == 2 || mode == 4 ? h.end : h.paused);
    /* Only the active timeout or the overall cap may expire this wait. */
    CHECK(poll(NULL, 0, 30) == 0);
    if (mode == 1 || mode >= 3) {
      if (mode == 4)
        CHECK(sl_image_paste_complete(h.sl, id, "/late", NULL) ==
              SL_ERROR_INVALID);
      else
        CHECK(sl_image_paste_resume(h.sl, id) == SL_ERROR_INVALID);
      CHECK(h.aborts == 1);
    } else {
      if (mode == 0) {
        h.pause = h.paused = 0;
        CHECK(sl_image_paste_resume(h.sl, id) == SL_OK && drive(&h) == 0 &&
              h.end);
      }
      CHECK(sl_image_paste_complete(h.sl, id, "/stored", NULL) == SL_OK);
      CHECK(sl_image_paste_pump(h.sl) == SL_OK && h.done == 1 && !h.aborts);
    }
    CHECK(!h.failed);
    sl_destroy(h.sl);
  }
  return 0;
}
static int image_completion(sl_t *sl, char trigger, sl_completion_emit_t emit,
                            void *context, void *userdata) {
  struct native *n = userdata;
  (void)sl;
  (void)trigger;
  ++n->completions;
  return emit("/candidate", context);
}
static int suspend_upload(sl_t *sl, sl_key_t key, void *context,
                          sl_key_action_t *action) {
  (void)sl;
  (void)key;
  (void)context;
  *action = SL_KEY_ACTION_SUSPEND;
  return SL_OK;
}
static int pending_feed(sl_t *sl, void *context, const char **chunk,
                        size_t *length) {
  int *sent = context;
  (void)sl;
  *chunk = "feed during upload\n";
  *length = *sent ? 0 : strlen(*chunk);
  *sent = 1;
  return SL_OK;
}
static void native_idle(sl_t *sl, void *context) {
  struct native *n = context;
  if (!n->stage) {
    n->stage = 1;
    if (sl_set_buffer(sl, n->literal ? "" : "AB") != SL_OK ||
        sl_set_cursor(sl, n->literal ? 0 : 1) != SL_OK ||
        sl_image_paste_start(sl, &n->host.id) != SL_OK)
      n->failed = 1;
    n->host.pause = 1;
  } else if (n->stage == 1 && n->host.paused) {
    n->stage = 2;
    if (sl_submit(sl) != SL_ERROR_NOT_READY ||
        (n->queued &&
         sl_prompt_queue_enqueue_draft(sl) != SL_ERROR_NOT_READY) ||
        write(n->master, "X\r\033\r\t", 5) != 5)
      n->failed = 1;
  } else if (n->stage == 2) {
    int sent = 0;
    struct winsize resized;
    memset(&resized, 0, sizeof(resized));
    resized.ws_col = 60;
    resized.ws_row = 18;
    if (strcmp(sl_buffer(sl), n->literal ? "X" : "AXB") ||
        sl_prompt_queue_count(sl) != 0)
      n->failed = 1;
    if (ioctl(n->master, TIOCSWINSZ, &resized) != 0 ||
        sl_print_above(sl, pending_feed, &sent) != SL_OK)
      n->failed = 1;
    n->stage = 3;
    if (n->suspend) {
      if (write(n->master, "\024", 1) != 1)
        n->failed = 1;
    } else if (n->cancel) {
      if (write(n->master, "\003", 1) != 1)
        n->failed = 1;
    } else {
      n->host.pause = n->host.paused = 0;
      if (sl_image_paste_resume(sl, n->host.id) != SL_OK)
        n->failed = 1;
    }
  } else if (n->stage == 3 && (n->host.end || n->host.aborts)) {
    n->stage = 4;
    if (!n->cancel && sl_image_paste_complete(
                          sl, n->host.id, "/backend/image.png", NULL) != SL_OK)
      n->failed = 1;
  } else if (n->stage == 4) {
    n->stage = 5;
    if (strcmp(sl_buffer(sl), n->cancel    ? "AXB"
                              : n->literal ? "/backend/image.pngX"
                                           : "A/backend/image.pngXB") ||
        n->completions)
      n->failed = 1;
    if (write(n->master, n->cancel ? "\r" : "\037\r", n->cancel ? 1 : 2) !=
        (n->cancel ? 1 : 2))
      n->failed = 1;
  }
  if (n->failed)
    (void)sl_cancel(sl);
}
static int native_cases(void) {
  int mode;
  for (mode = 0; mode < 5; ++mode) {
    struct native n;
    sl_config_t config;
    struct winsize size;
    sl_t *sl;
    sl_completion_provider_t provider;
    int slave;
    char *line;
    memset(&n, 0, sizeof(n));
    memset(&size, 0, sizeof(size));
    size.ws_col = 100;
    size.ws_row = 24;
    CHECK(openpty(&n.master, &slave, NULL, NULL, &size) == 0);
    sl_config_init(&config);
    config.input_fd = slave;
    config.output_fd = slave;
    config.prompt_queue = mode != 0;
    config.image_paste_callback = receive_image;
    config.image_paste_context = &n.host;
    sl = sl_create_with_config(&config);
    CHECK(sl);
    n.host.sl = sl;
    n.cancel = mode == 1;
    n.suspend = mode == 3;
    n.queued = mode != 0;
    n.literal = mode == 4;
    memset(&provider, 0, sizeof(provider));
    provider.trigger = '/';
    provider.strip_trigger = 1;
    provider.callback = image_completion;
    provider.userdata = &n;
    CHECK(sl_set_completion_providers(sl, &provider, 1) == SL_OK);
    CHECK(sl_bind_key(sl, SL_KEY_CTRL_T, suspend_upload, NULL) == SL_OK);
    image_fixture_size = 64;
    CHECK(sl_set_idle_callback(sl, native_idle, &n) == SL_OK);
    line = sl_readline(sl, "> ");
    if (n.suspend) {
      sl_image_paste_wait_t wait;
      CHECK(!line && sl_last_readline_status(sl) == SL_READLINE_SUSPENDED &&
            n.stage == 3);
      n.host.pause = n.host.paused = 0;
      CHECK(sl_image_paste_resume(sl, n.host.id) == SL_OK &&
            drive(&n.host) == 0);
      CHECK(sl_image_paste_complete(sl, n.host.id, "/backend/image.png",
                                    NULL) == SL_OK);
      CHECK(sl_image_paste_pump(sl) == SL_OK && !strcmp(sl_buffer(sl), "AXB"));
      CHECK(sl_image_paste_prepare(sl, &wait) == SL_OK &&
            wait.timeout_ms == -1);
      n.stage = 4;
      line = sl_readline(sl, "> ");
    }
    CHECK(line && !strcmp(line, n.literal ? "X" : "AXB") && !n.failed &&
          !n.host.failed && n.stage == 5);
    CHECK(n.host.aborts == (mode == 1) && n.host.end == (mode != 1));
    sl_free_string(sl, line);
    sl_destroy(sl);
    close(slave);
    close(n.master);
  }
  return 0;
}
static void *worker(void *unused) {
  int result;
  pthread_attr_t attributes;
  size_t stack, guard;
  (void)unused;
  if (pthread_getattr_np(pthread_self(), &attributes) != 0 ||
      pthread_attr_getstacksize(&attributes, &stack) != 0 ||
      pthread_attr_getguardsize(&attributes, &guard) != 0 || !guard ||
      stack > 160u * 1024u)
    return (void *)1;
  pthread_attr_destroy(&attributes);
  result = basic(0) || basic(1) || limits() || failures() || deadlines() ||
           deadline_domains() || native_cases();
  return (void *)(size_t)result;
}
int main(void) {
  pthread_t thread;
  void *result;
#if defined(__GLIBC__)
  pthread_attr_t attributes;
  CHECK(pthread_getattr_default_np(&attributes) == 0);
  CHECK(pthread_attr_setstacksize(&attributes, 128u * 1024u) == 0);
  CHECK(pthread_setattr_default_np(&attributes) == 0);
  CHECK(pthread_attr_destroy(&attributes) == 0);
#endif
  CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
  CHECK(pthread_join(thread, &result) == 0);
  return (int)(size_t)result;
}
