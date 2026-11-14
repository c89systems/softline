#include "queue_demo_wire.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int reserve(demo_packet_t *packet, size_t extra) {
  unsigned char *data;
  size_t capacity = packet->capacity ? packet->capacity : 256;
  if (extra > DEMO_FRAME_MAX - packet->length) {
    packet->failed = 1;
    return -1;
  }
  while (capacity < packet->length + extra)
    capacity *= 2;
  if (capacity > DEMO_FRAME_MAX)
    capacity = DEMO_FRAME_MAX;
  if (capacity <= packet->capacity)
    return 0;
  data = realloc(packet->data, capacity);
  if (!data) {
    packet->failed = 1;
    return -1;
  }
  packet->data = data;
  packet->capacity = capacity;
  return 0;
}
void demo_packet_free(demo_packet_t *packet) {
  free(packet->data);
  memset(packet, 0, sizeof(*packet));
}
void demo_put_string(demo_packet_t *packet, const char *text) {
  size_t length = text ? strlen(text) + 1 : 0;
  uint32_t size, wire;
  if (length > DEMO_FRAME_MAX) {
    packet->failed = 1;
    return;
  }
  size = (uint32_t)length;
  wire = htonl(size);
  if (packet->failed || reserve(packet, 4 + (size_t)size))
    return;
  memcpy(packet->data + packet->length, &wire, 4);
  packet->length += 4;
  if (size) {
    memcpy(packet->data + packet->length, text, size);
    packet->length += size;
  }
}
void demo_put_number(demo_packet_t *packet, unsigned long number) {
  char text[32];
  (void)snprintf(text, sizeof(text), "%lu", number);
  demo_put_string(packet, text);
}
void demo_put_bytes(demo_packet_t *packet, const char *bytes, size_t length) {
  if (packet->failed || length > 4096 || reserve(packet, length)) {
    packet->failed = 1;
    return;
  }
  memcpy(packet->data + packet->length, bytes, length);
  packet->length += length;
}
void demo_put_image_bytes(demo_packet_t *packet, const unsigned char *bytes,
                          size_t length) {
  if (packet->failed || !length || length > DEMO_IMAGE_CHUNK_MAX ||
      reserve(packet, length)) {
    packet->failed = 1;
    return;
  }
  memcpy(packet->data + packet->length, bytes, length);
  packet->length += length;
}
const unsigned char *demo_image_bytes(demo_packet_t *packet, size_t *length) {
  *length = packet->length - packet->offset;
  if (!*length || *length > DEMO_IMAGE_CHUNK_MAX) {
    packet->failed = 1;
    return NULL;
  }
  packet->offset = packet->length;
  return packet->data + packet->length - *length;
}
const char *demo_bytes(demo_packet_t *packet, size_t *length) {
  *length = packet->length - packet->offset;
  if (*length > 4096) {
    packet->failed = 1;
    return NULL;
  }
  packet->offset = packet->length;
  return (const char *)packet->data + packet->length - *length;
}
const char *demo_string(demo_packet_t *packet) {
  uint32_t size;
  const char *text;
  if (packet->failed || packet->length - packet->offset < 4)
    goto fail;
  memcpy(&size, packet->data + packet->offset, 4);
  size = ntohl(size);
  packet->offset += 4;
  if (!size)
    return NULL;
  if (size > packet->length - packet->offset)
    goto fail;
  text = (const char *)packet->data + packet->offset;
  if (text[size - 1] || memchr(text, 0, size - 1))
    goto fail;
  packet->offset += size;
  return text;
fail:
  packet->failed = 1;
  return NULL;
}
unsigned long demo_number(demo_packet_t *packet) {
  const char *text = demo_string(packet);
  char *end;
  unsigned long number;
  if (!text || !*text || *text == '-') {
    packet->failed = 1;
    return 0;
  }
  errno = 0;
  number = strtoul(text, &end, 10);
  if (errno || *end)
    packet->failed = 1;
  return number;
}
void demo_put_request(demo_packet_t *p, const sl_queue_request_t *r) {
  demo_put_number(p, DEMO_REQUEST);
  demo_put_number(p, r->request_id);
  demo_put_number(p, (unsigned long)r->operation);
  demo_put_number(p, (unsigned long)r->mode);
  demo_put_number(p, (unsigned long)r->enabled);
  demo_put_number(p, r->target_request_id);
  demo_put_string(p, r->item_id);
  demo_put_string(p, r->item_revision);
  demo_put_string(p, r->queue_revision);
  demo_put_string(p, r->edit_token);
  demo_put_string(p, r->text);
}
void demo_get_request(demo_packet_t *p, sl_queue_request_t *r) {
  memset(r, 0, sizeof(*r));
  r->request_id = demo_number(p);
  r->operation = (sl_queue_operation_t)demo_number(p);
  r->mode = (sl_prompt_queue_mode_t)demo_number(p);
  r->enabled = (int)demo_number(p);
  r->target_request_id = demo_number(p);
  r->item_id = demo_string(p);
  r->item_revision = demo_string(p);
  r->queue_revision = demo_string(p);
  r->edit_token = demo_string(p);
  r->text = demo_string(p);
}
void demo_put_result(demo_packet_t *p, const sl_queue_result_t *r) {
  demo_put_number(p, DEMO_RESULT);
  demo_put_number(p, r->request_id);
  demo_put_number(p, (unsigned long)r->status);
  demo_put_string(p, r->item_id);
  demo_put_string(p, r->item_revision);
  demo_put_string(p, r->edit_token);
  demo_put_string(p, r->reason);
}
void demo_get_result(demo_packet_t *p, sl_queue_result_t *r) {
  memset(r, 0, sizeof(*r));
  r->request_id = demo_number(p);
  r->status = (sl_queue_result_status_t)demo_number(p);
  r->item_id = demo_string(p);
  r->item_revision = demo_string(p);
  r->edit_token = demo_string(p);
  r->reason = demo_string(p);
}
void demo_put_entry(demo_packet_t *p, const sl_queue_entry_t *e) {
  demo_put_string(p, e->id);
  demo_put_string(p, e->revision);
  demo_put_string(p, e->text);
  demo_put_string(p, e->edit_token);
  demo_put_number(p, (unsigned long)e->mode);
  demo_put_number(p, (unsigned long)e->held);
  demo_put_number(p, (unsigned long)e->edit_held);
  demo_put_number(p, (unsigned long)e->ready);
  demo_put_number(p, (unsigned long)e->stopped);
  demo_put_number(p, (unsigned long)e->action);
}
void demo_get_entry(demo_packet_t *p, sl_queue_entry_t *e) {
  memset(e, 0, sizeof(*e));
  e->id = demo_string(p);
  e->revision = demo_string(p);
  e->text = demo_string(p);
  e->edit_token = demo_string(p);
  e->mode = (sl_prompt_queue_mode_t)demo_number(p);
  e->held = (int)demo_number(p);
  e->edit_held = (int)demo_number(p);
  e->ready = (int)demo_number(p);
  e->stopped = (int)demo_number(p);
  e->action = (int)demo_number(p);
}
void demo_put_change(demo_packet_t *p, const sl_queue_change_t *c) {
  size_t i;
  demo_put_number(p, DEMO_CHANGE);
  demo_put_number(p, (unsigned long)c->kind);
  demo_put_string(p, c->previous_revision);
  demo_put_string(p, c->revision);
  demo_put_number(p, c->request_id);
  demo_put_number(p, (unsigned long)c->total_count);
  demo_put_number(p, (unsigned long)c->index);
  demo_put_string(p, c->item_id);
  demo_put_number(p, (unsigned long)c->stopped);
  demo_put_number(p, (unsigned long)c->auto_approval);
  demo_put_number(p, (unsigned long)c->count);
  for (i = 0; i < c->count; ++i)
    demo_put_entry(p, c->entries + i);
}
int demo_get_change(demo_packet_t *p, sl_queue_change_t *c,
                    sl_queue_entry_t *entries, size_t capacity) {
  size_t i;
  memset(c, 0, sizeof(*c));
  c->kind = (sl_queue_change_kind_t)demo_number(p);
  c->previous_revision = demo_string(p);
  c->revision = demo_string(p);
  c->request_id = demo_number(p);
  c->total_count = (size_t)demo_number(p);
  c->index = (size_t)demo_number(p);
  c->item_id = demo_string(p);
  c->stopped = (int)demo_number(p);
  c->auto_approval = (int)demo_number(p);
  c->count = (size_t)demo_number(p);
  if (c->count > capacity) {
    p->failed = 1;
    return -1;
  }
  c->entries = entries;
  for (i = 0; i < c->count; ++i)
    demo_get_entry(p, entries + i);
  return p->failed ? -1 : 0;
}
int demo_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ? -1 : 0;
}
void demo_link_init(demo_link_t *link, int fd) {
  memset(link, 0, sizeof(*link));
  link->fd = fd;
  link->input_limit = DEMO_FRAME_MAX;
}
void demo_link_close(demo_link_t *link) {
  size_t i;
  if (link->fd >= 0)
    close(link->fd);
  demo_packet_free(&link->input);
  for (i = 0; i < link->count; ++i)
    demo_packet_free(link->output + i);
  demo_link_init(link, -1);
}
int demo_link_queue(demo_link_t *link, demo_packet_t *packet) {
  unsigned char *framed;
  uint32_t length;
  if (packet->failed || !packet->length || packet->length > DEMO_FRAME_MAX ||
      link->count == DEMO_OUT_MAX ||
      link->queued_bytes + packet->length + 4 > DEMO_QUEUED_BYTES_MAX)
    return -1;
  framed = realloc(packet->data, packet->length + 4);
  if (!framed)
    return -1;
  memmove(framed + 4, framed, packet->length);
  length = htonl((uint32_t)packet->length);
  memcpy(framed, &length, 4);
  packet->data = framed;
  packet->length += 4;
  packet->offset = 0;
  packet->capacity = packet->length;
  link->queued_bytes += packet->length;
  link->output[link->count++] = *packet;
  memset(packet, 0, sizeof(*packet));
  return 0;
}
int demo_link_flush(demo_link_t *link) {
  size_t budget = 64u * 1024u;
#if defined(MSG_NOSIGNAL)
  int flags = MSG_NOSIGNAL;
#elif defined(SO_NOSIGPIPE)
  int flags = 0;
  int enabled = 1;
  if (link->count &&
      setsockopt(link->fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
    return -1;
#else
#error "Queue demo sockets require per-socket SIGPIPE suppression"
#endif
  while (link->count && budget) {
    demo_packet_t *p = link->output;
    size_t amount = p->length - p->offset;
    ssize_t wrote;
    if (amount > budget)
      amount = budget;
    wrote = send(link->fd, p->data + p->offset, amount, flags);
    if (wrote < 0 && errno == EINTR)
      continue;
    if (wrote < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return 0;
    if (wrote <= 0)
      return -1;
    p->offset += (size_t)wrote;
    budget -= (size_t)wrote;
    link->queued_bytes -= (size_t)wrote;
    if (p->offset == p->length) {
      demo_packet_free(p);
      --link->count;
      memmove(p, p + 1, link->count * sizeof(*p));
      memset(p + link->count, 0, sizeof(*p));
    }
  }
  return 0;
}
int demo_link_read(demo_link_t *link, demo_packet_t *packet) {
  for (;;) {
    ssize_t got;
    if (link->header_length < 4) {
      got = recv(link->fd, link->header + link->header_length,
                 4 - link->header_length, 0);
      if (got > 0)
        link->header_length += (size_t)got;
      if (got > 0 && link->header_length == 4) {
        uint32_t length;
        memcpy(&length, link->header, 4);
        length = ntohl(length);
        if (!length || length > link->input_limit ||
            reserve(&link->input, length))
          return -1;
        link->input.capacity = length;
      }
    } else {
      got = recv(link->fd, link->input.data + link->input.length,
                 link->input.capacity - link->input.length, 0);
      if (got > 0)
        link->input.length += (size_t)got;
      if (got > 0 && link->input.length == link->input.capacity) {
        *packet = link->input;
        memset(&link->input, 0, sizeof(link->input));
        link->header_length = 0;
        return 1;
      }
    }
    if (got > 0)
      continue;
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return 0;
    return -1;
  }
}
