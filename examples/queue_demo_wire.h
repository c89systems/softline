#ifndef SOFTLINE_QUEUE_DEMO_WIRE_H
#define SOFTLINE_QUEUE_DEMO_WIRE_H
#include "softline/softline.h"

/* Example-only bounded framing. This is not a Softline wire protocol/API. */
#define DEMO_OUT_MAX 16
#define DEMO_ITEM_MAX 64
#define DEMO_PROMPT_MAX (128u * 1024u)
/* A full accepted queue snapshot, with bounded per-entry metadata. */
#define DEMO_FRAME_MAX (DEMO_ITEM_MAX * (DEMO_PROMPT_MAX + 1024u) + 4096u)
#define DEMO_QUEUED_BYTES_MAX (DEMO_FRAME_MAX + 2u * DEMO_PROMPT_MAX)
enum demo_message {
  DEMO_HELLO = 1,
  DEMO_REQUEST,
  DEMO_RESULT,
  DEMO_CHANGE,
  DEMO_PROMPT,
  DEMO_OUTPUT,
  DEMO_OUTPUT_END,
  DEMO_CANCEL,
  DEMO_RESPONSE_BEGIN,
  DEMO_SEGMENT_END,
  DEMO_MESSAGE,
  DEMO_NOTE,
  DEMO_SHELL,
  DEMO_QUIT,
  DEMO_IDLE,
  DEMO_SHELL_BEGIN
};
typedef struct demo_packet {
  unsigned char *data;
  size_t length, offset, capacity;
  int failed;
} demo_packet_t;
typedef struct demo_link {
  int fd;
  unsigned char header[4];
  size_t header_length;
  demo_packet_t input;
  demo_packet_t output[DEMO_OUT_MAX];
  size_t count, queued_bytes;
} demo_link_t;
void demo_packet_free(demo_packet_t *packet);
void demo_put_number(demo_packet_t *packet, unsigned long number);
void demo_put_string(demo_packet_t *packet, const char *text);
unsigned long demo_number(demo_packet_t *packet);
const char *demo_string(demo_packet_t *packet);
void demo_put_request(demo_packet_t *packet, const sl_queue_request_t *request);
void demo_get_request(demo_packet_t *packet, sl_queue_request_t *request);
void demo_put_result(demo_packet_t *packet, const sl_queue_result_t *result);
void demo_get_result(demo_packet_t *packet, sl_queue_result_t *result);
void demo_put_entry(demo_packet_t *packet, const sl_queue_entry_t *entry);
void demo_get_entry(demo_packet_t *packet, sl_queue_entry_t *entry);
void demo_put_change(demo_packet_t *packet, const sl_queue_change_t *change);
int demo_get_change(demo_packet_t *packet, sl_queue_change_t *change,
                    sl_queue_entry_t *entries, size_t capacity);
void demo_link_init(demo_link_t *link, int fd);
void demo_link_close(demo_link_t *link);
int demo_link_queue(demo_link_t *link, demo_packet_t *packet);
int demo_link_flush(demo_link_t *link);
/* Return one for a complete frame, zero for EAGAIN, minus one for loss/error.
 * Reads never fetch bytes belonging to a second frame. */
int demo_link_read(demo_link_t *link, demo_packet_t *packet);
int demo_nonblock(int fd);
#endif
