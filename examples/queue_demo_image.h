#ifndef SOFTLINE_QUEUE_DEMO_IMAGE_H
#define SOFTLINE_QUEUE_DEMO_IMAGE_H
#include "queue_demo_wire.h"

/* Example storage, not part of libsoftline. Completed files belong to the
 * server operator; only unfinished or explicitly aborted files are removed. */
typedef struct demo_image_store {
  char directory[4096], temporary[4096], path[4096];
  size_t limit, received;
  unsigned long id;
  int fd, published;
  double started, progress;
} demo_image_store_t;
int demo_image_limit(size_t *limit);
int demo_image_store_init(demo_image_store_t *store);
void demo_image_store_close(demo_image_store_t *store);
typedef int (*demo_image_reply_fn)(void *, unsigned long, size_t, const char *,
                                   const char *);
int demo_image_store_apply(demo_image_store_t *, const sl_image_paste_event_t *,
                           double now, demo_image_reply_fn, void *);
double demo_image_store_due(const demo_image_store_t *store);
int demo_image_store_timeout(demo_image_store_t *, double, demo_image_reply_fn,
                             void *);
#ifdef SOFTLINE_CBOR_TCP
int demo_image_store_input(demo_image_store_t *store, demo_link_t *link,
                           unsigned long kind, demo_packet_t *packet,
                           double now);
int demo_image_store_tick(demo_image_store_t *store, demo_link_t *link,
                          double now);
#endif
#endif
