#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <softline/softline.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "source line %d: %s\n", __LINE__, #c);                   \
      return 1;                                                                \
    }                                                                          \
  } while (0)
typedef struct {
  sl_t *sl;
  int step, closed, pause, paused, invalid, fail, begins, ends, done, aborts,
      bad, idle;
  size_t sent, received;
  unsigned char chunk[8192];
  char name[32];
} fixture_t;
static int next(void *context, sl_image_paste_event_t *e) {
  fixture_t *f = context;
  if (f->closed)
    f->bad = 1;
  if (sl_insert(f->sl, "reentry") != SL_ERROR_INVALID)
    f->bad = 1;
  sl_destroy(f->sl);
  if (f->fail)
    return -1;
  if (f->idle)
    return 0;
  memset(e, 0, sizeof(*e));
  e->offset = f->sent;
  if (!f->step++) {
    e->operation = SL_IMAGE_PASTE_BEGIN;
    e->media_type = "image/png";
    e->filename = f->name;
  } else if (f->sent < 1024u * 1024u) {
    size_t i;
    for (i = 0; i < sizeof(f->chunk); ++i)
      f->chunk[i] = (unsigned char)(f->sent + i);
    e->operation = SL_IMAGE_PASTE_DATA;
    e->data = f->chunk;
    e->length = sizeof(f->chunk);
    if (f->invalid == 2)
      e->length = SL_IMAGE_PASTE_CHUNK_MAX + 1;
    f->sent += e->length;
    if (f->invalid == 1)
      e->offset++;
  } else
    e->operation = SL_IMAGE_PASTE_END;
  return 1;
}
static int prepare(void *context, sl_image_paste_wait_t *w) {
  fixture_t *f = context;
  memset(w, 0, sizeof(*w));
  w->timeout_ms = f->idle ? -1 : 0;
  return SL_OK;
}
static const char *error(void *context) {
  (void)context;
  return "source failure";
}
static void close_source(void *context) {
  fixture_t *f = context;
  ++f->closed;
  memset(f->name, 'x', sizeof(f->name));
  if (sl_set_buffer(f->sl, "reentry") != SL_ERROR_INVALID)
    f->bad = 1;
}
static int receive(void *context, const sl_image_paste_event_t *e) {
  fixture_t *f = context;
  size_t i;
  if (e->operation == SL_IMAGE_PASTE_BEGIN)
    ++f->begins;
  else if (e->operation == SL_IMAGE_PASTE_DATA) {
    if (f->pause) {
      f->paused = 1;
      return SL_ERROR_NOT_READY;
    }
    if (e->offset != f->received)
      f->bad = 1;
    for (i = 0; i < e->length; ++i)
      if (e->data[i] != (unsigned char)(f->received + i))
        f->bad = 1;
    f->received += e->length;
  } else if (e->operation == SL_IMAGE_PASTE_END)
    ++f->ends;
  else if (e->operation == SL_IMAGE_PASTE_DONE) {
    ++f->done;
    if (f->closed != 1 || strcmp(e->filename, "source.png"))
      f->bad = 1;
  } else if (e->operation == SL_IMAGE_PASTE_ABORT) {
    ++f->aborts;
    if (f->closed != 1 || !e->reason)
      f->bad = 1;
  }
  return SL_OK;
}
static int run(int mode, int receiver) {
  fixture_t f;
  sl_config_t config;
  sl_image_paste_source_t source;
  sl_image_paste_id_t id = 0;
  sl_image_paste_wait_t wait;
  int fd = open("/dev/null", O_RDWR), i;
  CHECK(fd >= 0);
  memset(&f, 0, sizeof(f));
  strcpy(f.name, "source.png");
  f.pause = mode == 1;
  f.invalid = mode == 2 ? 1 : mode == 6 ? 2 : 0;
  f.fail = mode == 3;
  f.idle = mode == 4;
  sl_config_init(&config);
  config.input_fd = config.output_fd = fd;
  config.image_paste_callback = receive;
  config.image_paste_context = &f;
  config.image_paste_source_timeout_ms = mode == 4 ? 10 : 0;
  if (mode == 5)
    config.image_paste_max_bytes = 8192;
  f.sl = sl_create_with_config(&config);
  CHECK(f.sl && sl_set_buffer(f.sl, "draft ") == SL_OK);
  memset(&source, 0, sizeof(source));
  source.context = &f;
  source.next = next;
  source.prepare = prepare;
  source.close = close_source;
  source.error = error;
  CHECK((receiver ? f.sl->image_paste_start_source(f.sl, &source, &id)
                  : sl_image_paste_start_source(f.sl, &source, &id)) == SL_OK &&
        id);
  CHECK(sl_image_paste_prepare(f.sl, &wait) == SL_OK && !wait.count &&
        wait.timeout_ms >= 0);
  CHECK(sl_insert(f.sl, "tail") == SL_OK);
  if (mode == 4) {
    struct timespec delay;
    delay.tv_sec = 0;
    delay.tv_nsec = 20000000;
    nanosleep(&delay, NULL);
  }
  for (i = 0; i < 300 && !f.ends && !f.aborts; ++i) {
    CHECK(sl_image_paste_pump(f.sl) == SL_OK);
    if (f.paused) {
      size_t sent = f.sent;
      CHECK(sl_image_paste_pump(f.sl) == SL_OK && f.sent == sent &&
            !f.received);
      f.pause = f.paused = 0;
      CHECK(sl_image_paste_resume(f.sl, id) == SL_OK);
    }
  }
  if (mode < 2) {
    CHECK(f.ends == 1 && f.closed == 1 && f.received == 1024u * 1024u);
    CHECK(sl_image_paste_complete(f.sl, id, "/backend/image.png", NULL) ==
          SL_OK);
    CHECK(sl_image_paste_pump(f.sl) == SL_OK && f.done == 1 && !f.aborts);
    CHECK(!strcmp(sl_buffer(f.sl), "draft /backend/image.pngtail"));
  } else
    CHECK(f.aborts == 1 && !f.done && !strcmp(sl_buffer(f.sl), "draft tail"));
  CHECK(!f.bad && f.closed == 1);
  sl_destroy(f.sl);
  CHECK(f.closed == 1);
  close(fd);
  return 0;
}
int main(void) {
  int mode, receiver;
  for (receiver = 0; receiver < 2; ++receiver)
    for (mode = 0; mode < 7; ++mode)
      CHECK(!run(mode, receiver));
  return 0;
}
