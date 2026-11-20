#include "queue_demo_wire.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "CBOR stream line %d: %s\n", __LINE__, #c);              \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int wire_bytes(void) {
  static const unsigned char expected[] = {0, 0,    0,   5,   0x83,
                                           1, 0x61, 'a', 0xf6};
  unsigned char actual[sizeof(expected)];
  demo_link_t sender, receiver;
  demo_packet_t p = {0};
  int fd[2];
  size_t i;
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
  CHECK(!demo_nonblock(fd[0]) && !demo_nonblock(fd[1]));
  demo_link_init(&sender, fd[0]);
  demo_link_init(&receiver, fd[1]);
  demo_put_number(&p, 1);
  demo_put_string(&p, "a");
  demo_put_string(&p, NULL);
  CHECK(!demo_link_queue(&sender, &p) && !demo_link_flush(&sender));
  CHECK(!sender.count && !sender.queued_bytes);
  CHECK(read(fd[1], actual, sizeof(actual)) == (ssize_t)sizeof(actual));
  CHECK(!memcmp(actual, expected, sizeof(actual)));
  for (i = 0; i < sizeof(expected); ++i) {
    CHECK(write(fd[0], expected + i, 1) == 1);
    CHECK(demo_link_read(&receiver, &p) == (i + 1 == sizeof(expected)));
  }
  CHECK(demo_number(&p) == 1 && !strcmp(demo_string(&p), "a") &&
        demo_string(&p) == NULL && demo_packet_done(&p));
  demo_packet_free(&p);
  demo_link_close(&receiver);
  demo_link_close(&sender);
  return 0;
}

typedef struct {
  sl_t *sl;
  demo_link_t sender;
} fixture_t;

static int request(void *context, const sl_queue_request_t *r) {
  fixture_t *f = context;
  demo_packet_t p = {0};
  demo_put_pinned_request(&p, f->sl, r);
  if (!p.failed && !p.heap && p.lease && p.count == 11 &&
      p.fields[10].bytes == r->text && !demo_link_queue(&f->sender, &p))
    return SL_OK;
  demo_packet_free(&p);
  return SL_ERROR;
}

static int partial_request(void) {
  static char text[64 * 1024 + 1];
  sl_config_t config;
  sl_queue_backend_t backend = {0};
  sl_queue_change_t snapshot = {0};
  sl_queue_request_t offer = {0}, read_request;
  sl_queue_result_t result = {0};
  sl_queue_pending_info_t pending;
  demo_link_t receiver;
  demo_packet_t p = {0};
  int fd[2], small = 16384, status, i;
  fixture_t f;
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
  CHECK(!setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)));
  CHECK(!demo_nonblock(fd[0]) && !demo_nonblock(fd[1]));
  demo_link_init(&f.sender, fd[0]);
  demo_link_init(&receiver, fd[1]);
  sl_config_init(&config);
  config.line_max_len = sizeof(text);
  config.prompt_queue = 1;
  config.queue_backend = &backend;
  config.queue_backend_context = &f;
  backend.request = request;
  backend.capabilities =
      (1UL << SL_QUEUE_OFFER) | (1UL << SL_QUEUE_SYNCHRONIZE);
  f.sl = sl_create_with_config(&config);
  CHECK(f.sl);
  snapshot.kind = SL_QUEUE_SNAPSHOT;
  snapshot.revision = "1";
  CHECK(sl_queue_backend_update(f.sl, &snapshot) == SL_OK);
  memset(text, 'a', sizeof(text) - 1);
  offer.operation = SL_QUEUE_OFFER;
  offer.text = text;
  CHECK(sl_queue_backend_request(f.sl, &offer, &result.request_id) == SL_OK);
  memset(text, 'z', sizeof(text) - 1);
  CHECK(!demo_link_flush(&f.sender) && f.sender.count == 1 &&
        f.sender.queued_bytes > 0 && f.sender.queued_bytes < sizeof(text));
  result.status = SL_QUEUE_ACCEPTED;
  result.item_id = "id";
  result.item_revision = "2";
  CHECK(sl_queue_backend_result(f.sl, &result) == SL_OK);
  CHECK(sl_queue_backend_pending(f.sl, 0, &pending) != SL_OK);
  for (i = 0; i < 100; ++i) {
    status = demo_link_read(&receiver, &p);
    CHECK(status >= 0);
    if (status)
      break;
    CHECK(!demo_link_flush(&f.sender));
  }
  CHECK(i < 100 && demo_number(&p) == DEMO_REQUEST);
  demo_get_request(&p, &read_request);
  CHECK(demo_packet_done(&p) && read_request.request_id == result.request_id &&
        strlen(read_request.text) == sizeof(text) - 1 &&
        strspn(read_request.text, "a") == sizeof(text) - 1);
  demo_packet_free(&p);
  demo_link_close(&receiver);
  demo_link_close(&f.sender);
  sl_destroy(f.sl);
  return 0;
}

int main(void) { return wire_bytes() || partial_request(); }
