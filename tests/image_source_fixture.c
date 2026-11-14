/* Deterministic incremental acquisition fixture. Never stages a whole image. */
#include "../src/softline_clipboard.h"
#include <stdlib.h>
#include <string.h>

size_t image_fixture_size = 64;
size_t image_fixture_reads;
int image_fixture_gate, image_fixture_fail;
const char *image_fixture_mime = "image/png";
struct sl_clipboard {
  unsigned char data[16384];
  size_t offset;
  int phase;
};
unsigned char image_fixture_byte(size_t at) {
  static const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  return at < 8 ? signature[at] : (unsigned char)(at * 37u);
}
sl_clipboard_t *sl_clipboard_open(size_t limit) {
  (void)limit;
  image_fixture_reads = 0;
  return calloc(1, sizeof(sl_clipboard_t));
}
int sl_clipboard_next(sl_clipboard_t *c, sl_image_paste_event_t *event) {
  size_t i, count;
  if (image_fixture_fail)
    return -1;
  memset(event, 0, sizeof(*event));
  event->media_type = image_fixture_mime;
  event->filename =
      !strcmp(image_fixture_mime, "image/png") ? "image.png" : "image.jpeg";
  event->offset = c->offset;
  if (!c->phase) {
    c->phase = 1;
    event->operation = SL_IMAGE_PASTE_BEGIN;
    return 1;
  }
  if (image_fixture_gate && c->offset)
    return 0;
  if (c->offset == image_fixture_size) {
    if (c->phase == 2)
      return 0;
    c->phase = 2;
    event->operation = SL_IMAGE_PASTE_END;
    return 1;
  }
  count = image_fixture_size - c->offset;
  if (count > sizeof(c->data))
    count = sizeof(c->data);
  for (i = 0; i < count; ++i)
    c->data[i] = image_fixture_byte(c->offset + i);
  event->operation = SL_IMAGE_PASTE_DATA;
  event->data = c->data;
  event->length = count;
  c->offset += count;
  ++image_fixture_reads;
  return 1;
}
int sl_clipboard_prepare(sl_clipboard_t *c, struct pollfd *fds, size_t capacity,
                         int *timeout_ms) {
  (void)c;
  (void)fds;
  (void)capacity;
  *timeout_ms = image_fixture_gate ? -1 : 0;
  return 0;
}
const char *sl_clipboard_error_text(const sl_clipboard_t *c) {
  (void)c;
  return "fixture source read failure";
}
void sl_clipboard_close(sl_clipboard_t *c) { free(c); }
int sl_clipboard_valid_path_template(const char *s) {
  (void)s;
  return 1;
}
void sl_clipboard_discard_image(const char *p, const char *t) {
  (void)p;
  (void)t;
}
int sl_clipboard_save_image(const char *t, char **p, char *e, size_t n) {
  (void)t;
  (void)p;
  (void)e;
  (void)n;
  abort(); /* An adapter failure must never enter standalone saving. */
}
