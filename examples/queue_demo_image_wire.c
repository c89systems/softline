#include "queue_demo_image.h"
#include <string.h>
static int wire_reply(void *context, unsigned long id, size_t offset,
                      const char *path, const char *reason) {
  demo_link_t *link = context;
  unsigned long kind = path || reason ? DEMO_IMAGE_RESULT : DEMO_IMAGE_READY;
  demo_packet_t p = {0};
  int result;
  demo_put_number(&p, kind);
  demo_put_number(&p, id);
  if (kind == DEMO_IMAGE_READY)
    demo_put_number(&p, (unsigned long)offset);
  else {
    demo_put_string(&p, path);
    demo_put_string(&p, reason);
  }
  result = demo_link_queue(link, &p);
  demo_packet_free(&p);
  return result;
}

int demo_image_store_input(demo_image_store_t *s, demo_link_t *link,
                           unsigned long kind, demo_packet_t *p, double now) {
  sl_image_paste_event_t e;
  memset(&e, 0, sizeof(e));
  e.id = demo_number(p);
  switch (kind) {
  case DEMO_IMAGE_BEGIN:
    e.operation = SL_IMAGE_PASTE_BEGIN;
    e.media_type = demo_string(p);
    break;
  case DEMO_IMAGE_DATA:
    e.operation = SL_IMAGE_PASTE_DATA;
    e.offset = demo_number(p);
    e.data = demo_image_bytes(p, &e.length);
    break;
  case DEMO_IMAGE_END:
    e.operation = SL_IMAGE_PASTE_END;
    e.offset = demo_number(p);
    break;
  case DEMO_IMAGE_ABORT:
    e.operation = SL_IMAGE_PASTE_ABORT;
    break;
  case DEMO_IMAGE_DONE:
    e.operation = SL_IMAGE_PASTE_DONE;
    break;
  default:
    return -1;
  }
  if (!demo_packet_done(p))
    return -1;
  return demo_image_store_apply(s, &e, now, wire_reply, link);
}
int demo_image_store_tick(demo_image_store_t *s, demo_link_t *link,
                          double now) {
  return demo_image_store_timeout(s, now, wire_reply, link);
}
