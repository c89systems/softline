#include "queue_demo_wire.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEMO_FIELDS_MAX (DEMO_ITEM_MAX * 10u + 16u)
#define IO_BUDGET (64u * 1024u)
enum { SEND_ARRAY, SEND_FIELD, SEND_DATA, SEND_STRING_END, SEND_FINISH };
enum { READ_HEADER, READ_ARRAY, READ_FIELDS, READ_END };

static size_t head_size(uint64_t value) {
  if (value < 24u)
    return 1;
  if (value <= 255u)
    return 2;
  if (value <= 65535u)
    return 3;
  return value <= UINT32_MAX ? 5 : 9;
}

static demo_field_t *fields(demo_packet_t *p) {
  return p->heap ? p->heap : p->fields;
}

static demo_field_t *append(demo_packet_t *p, c89cbor_type type, uint64_t value,
                            const void *bytes) {
  demo_field_t *field;
  size_t size = type == C89CBOR_NULL ? 1 : head_size(value);
  if (type == C89CBOR_TEXT || type == C89CBOR_BYTES)
    size += (size_t)value;
  if (p->failed || p->count == DEMO_FIELDS_MAX ||
      size > DEMO_FRAME_MAX - p->body_size)
    goto failed;
  if (p->count == (p->heap ? p->capacity : DEMO_INLINE_FIELDS)) {
    size_t capacity = p->heap ? p->capacity * 2 : DEMO_INLINE_FIELDS * 2;
    demo_field_t *heap;
    if (capacity > DEMO_FIELDS_MAX)
      capacity = DEMO_FIELDS_MAX;
    heap = realloc(p->heap, capacity * sizeof(*heap));
    if (!heap)
      goto failed;
    if (!p->heap)
      memcpy(heap, p->fields, sizeof(p->fields));
    p->heap = heap;
    p->capacity = capacity;
  }
  field = fields(p) + p->count++;
  field->type = type;
  field->value = value;
  field->bytes = bytes;
  p->body_size += size;
  return field;
failed:
  p->failed = 1;
  return NULL;
}

void demo_packet_free(demo_packet_t *p) {
  size_t i;
  if (p->lease)
    sl_queue_backend_unpin_request(p->lease);
  else
    for (i = 0; i < p->count; ++i)
      free((void *)fields(p)[i].bytes);
  free(p->heap);
  memset(p, 0, sizeof(*p));
}

int demo_packet_done(const demo_packet_t *p) {
  return !p->failed && p->offset == p->count;
}

static void put_span(demo_packet_t *p, c89cbor_type type, const void *bytes,
                     size_t length) {
  char *owned = NULL;
  if (p->failed || length > DEMO_FRAME_MAX) {
    p->failed = 1;
    return;
  }
  if (!p->lease) {
    owned = malloc(length + 1);
    if (!owned) {
      p->failed = 1;
      return;
    }
    if (length)
      memcpy(owned, bytes, length);
    owned[length] = 0;
    bytes = owned;
  }
  if (!append(p, type, length, bytes))
    free(owned);
}

void demo_put_string(demo_packet_t *p, const char *text) {
  if (text)
    put_span(p, C89CBOR_TEXT, text, strlen(text));
  else
    (void)append(p, C89CBOR_NULL, 0, NULL);
}

void demo_put_number(demo_packet_t *p, unsigned long number) {
  (void)append(p, C89CBOR_UINT, number, NULL);
}

void demo_put_bytes(demo_packet_t *p, const char *bytes, size_t length) {
  if (length > 4096) {
    p->failed = 1;
    return;
  }
  put_span(p, C89CBOR_BYTES, bytes, length);
}

void demo_put_image_bytes(demo_packet_t *p, const unsigned char *bytes,
                          size_t length) {
  if (!length || length > DEMO_IMAGE_CHUNK_MAX) {
    p->failed = 1;
    return;
  }
  put_span(p, C89CBOR_BYTES, bytes, length);
}

static const demo_field_t *take(demo_packet_t *p) {
  if (p->failed || p->offset == p->count) {
    p->failed = 1;
    return NULL;
  }
  return fields(p) + p->offset++;
}

static const void *get_bytes(demo_packet_t *p, size_t *length, size_t limit) {
  const demo_field_t *field = take(p);
  *length = 0;
  if (!field || field->type != C89CBOR_BYTES || field->value > limit) {
    p->failed = 1;
    return NULL;
  }
  *length = (size_t)field->value;
  return field->bytes;
}

const unsigned char *demo_image_bytes(demo_packet_t *p, size_t *length) {
  const void *bytes = get_bytes(p, length, DEMO_IMAGE_CHUNK_MAX);
  if (!*length)
    p->failed = 1;
  return bytes;
}

const char *demo_bytes(demo_packet_t *p, size_t *length) {
  return get_bytes(p, length, 4096);
}

const char *demo_string(demo_packet_t *p) {
  const demo_field_t *field = take(p);
  if (field && field->type == C89CBOR_NULL)
    return NULL;
  if (!field || field->type != C89CBOR_TEXT) {
    p->failed = 1;
    return NULL;
  }
  return field->bytes;
}

unsigned long demo_number(demo_packet_t *p) {
  const demo_field_t *field = take(p);
  if (!field || field->type != C89CBOR_UINT || field->value > ULONG_MAX) {
    p->failed = 1;
    return 0;
  }
  return (unsigned long)field->value;
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

void demo_put_pinned_request(demo_packet_t *p, sl_t *sl,
                             const sl_queue_request_t *request) {
  if (sl_queue_backend_pin_request(sl, request, &p->lease) != SL_OK) {
    p->failed = 1;
    return;
  }
  demo_put_request(p, request);
}

int demo_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  int enabled = 1;
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return -1;
  /* Small records should be sent promptly on this loopback stream. */
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
  return 0;
}

void demo_link_init(demo_link_t *link, int fd) {
  memset(link, 0, sizeof(*link));
  link->fd = fd;
  link->input_limit = DEMO_FRAME_MAX;
  link->remaining = sizeof(link->header);
}

void demo_link_close(demo_link_t *link) {
  size_t i;
  if (link->fd >= 0)
    close(link->fd);
  if (link->writer_live)
    (void)c89cbor_encoder_cleanup(&link->encoder);
  if (link->reader_live)
    (void)c89cbor_reader_cleanup(&link->reader);
  demo_packet_free(&link->input);
  for (i = 0; i < link->count; ++i)
    demo_packet_free(link->output + i);
  demo_link_init(link, -1);
}

static size_t payload_size(const demo_packet_t *p) {
  return head_size(p->count) + p->body_size;
}

int demo_link_queue(demo_link_t *link, demo_packet_t *p) {
  size_t payload = payload_size(p), size = payload + sizeof(link->header);
  if (p->failed || !p->count || payload > DEMO_FRAME_MAX ||
      link->count == DEMO_OUT_MAX ||
      size > DEMO_QUEUED_BYTES_MAX - link->queued_bytes)
    return -1;
  link->queued_bytes += size;
  link->output[link->count++] = *p;
  memset(p, 0, sizeof(*p));
  return 0;
}

/* Submit one native CBOR token. DATA borrows the retained field. */
static int next_token(demo_link_t *link, demo_packet_t *p) {
  c89cbor_token token = {0};
  const demo_field_t *field;
  if (link->send_phase == SEND_ARRAY) {
    token.type = C89CBOR_ARRAY;
    token.argument = p->count;
    link->send_phase = SEND_FIELD;
  } else if (link->send_field == p->count) {
    if (link->send_phase == SEND_FINISH)
      return C89CBOR_DONE;
    token.type = C89CBOR_END;
    token.argument = C89CBOR_ARRAY;
    link->send_phase = SEND_FINISH;
  } else {
    field = fields(p) + link->send_field;
    if (link->send_phase == SEND_FIELD) {
      token.type = field->type;
      token.argument = field->value;
      if (field->type == C89CBOR_TEXT || field->type == C89CBOR_BYTES)
        link->send_phase = field->value ? SEND_DATA : SEND_STRING_END;
      else
        ++link->send_field;
    } else if (link->send_phase == SEND_DATA) {
      token.type = C89CBOR_DATA;
      token.data = field->bytes;
      token.length = (size_t)field->value;
      link->send_phase = SEND_STRING_END;
    } else {
      token.type = C89CBOR_END;
      token.argument = field->type;
      ++link->send_field;
      link->send_phase = SEND_FIELD;
    }
  }
  link->send_borrowed = token.type == C89CBOR_DATA;
  return c89cbor_encoder_put(&link->encoder, &token);
}

/* Gather native encoder output for a bounded scatter/gather write. The
 * encoder reuses its header storage, so keep only those small headers here.
 * DATA stays in its original storage until the record's last write. */
static int stage_output(demo_link_t *link, demo_packet_t *p, size_t budget) {
  link->send_pos = link->send_count = 0;
  if (!link->writer_live) {
    if (c89cbor_length32(NULL, (uint32_t)payload_size(p), DEMO_FRAME_MAX,
                         link->header) != C89CBOR_OK ||
        c89cbor_encoder_init(NULL, &link->encoder, NULL, 0, 0) != C89CBOR_OK)
      return -1;
    link->writer_live = 1;
    link->send_spans[0].iov_base = link->header;
    link->send_spans[0].iov_len = sizeof(link->header);
    link->send_count = 1;
    budget -= sizeof(link->header);
  }
  while (link->send_count < DEMO_SEND_SPANS && budget && !link->send_finished) {
    const void *bytes;
    size_t length;
    size_t index = link->send_count;
    int status = c89cbor_encoder_peek(&link->encoder, &bytes, &length);
    if (status == C89CBOR_OK) {
      status = next_token(link, p);
      if (status == C89CBOR_DONE)
        link->send_finished = 1;
      else if (status != C89CBOR_OK)
        return -1;
      continue;
    }
    if (status != C89CBOR_OUTPUT)
      return -1;
    if (length > budget)
      length = budget;
    if (!link->send_borrowed) {
      if (length > sizeof(link->send_headers[index]))
        return -1;
      memcpy(link->send_headers[index], bytes, length);
      bytes = link->send_headers[index];
    }
    link->send_spans[index].iov_base = (void *)bytes;
    link->send_spans[index].iov_len = length;
    ++link->send_count;
    budget -= length;
    if (c89cbor_encoder_advance(&link->encoder, length) < 0)
      return -1;
  }
  return 0;
}

static ssize_t send_spans(demo_link_t *link) {
  struct msghdr message = {0};
  ssize_t written;
  int flags = 0;
#if defined(MSG_NOSIGNAL)
  flags = MSG_NOSIGNAL;
#endif
  message.msg_iov = link->send_spans + link->send_pos;
  message.msg_iovlen = link->send_count - link->send_pos;
  do {
    written = sendmsg(link->fd, &message, flags);
  } while (written < 0 && errno == EINTR);
  if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return 0;
  return written > 0 ? written : -1;
}

int demo_link_flush(demo_link_t *link) {
  size_t budget = IO_BUDGET;
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
  {
    int enabled = 1;
    if (link->count && setsockopt(link->fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                                  sizeof(enabled)))
      return -1;
  }
#elif !defined(MSG_NOSIGNAL)
#error "Queue demo sockets require per-socket SIGPIPE suppression"
#endif
  while (link->count && budget) {
    demo_packet_t *p = link->output;
    if (link->send_pos == link->send_count) {
      if (!link->writer_live && budget < sizeof(link->header))
        return 0;
      if (stage_output(link, p, budget))
        return -1;
    }
    if (link->send_pos < link->send_count) {
      ssize_t written = send_spans(link);
      size_t remaining;
      if (written < 0)
        return -1;
      if (!written)
        return 0;
      remaining = (size_t)written;
      budget -= remaining;
      link->queued_bytes -= remaining;
      while (remaining) {
        struct iovec *span = link->send_spans + link->send_pos;
        if (remaining < span->iov_len) {
          span->iov_base = (char *)span->iov_base + remaining;
          span->iov_len -= remaining;
          break;
        }
        remaining -= span->iov_len;
        ++link->send_pos;
      }
    }
    if (link->send_finished && link->send_pos == link->send_count) {
      if (c89cbor_encoder_finish(&link->encoder) != C89CBOR_OK)
        return -1;
      (void)c89cbor_encoder_cleanup(&link->encoder);
      link->writer_live = link->send_finished = 0;
      link->send_field = 0;
      link->send_phase = SEND_ARRAY;
      demo_packet_free(p);
      --link->count;
      memmove(p, p + 1, link->count * sizeof(*p));
      memset(p + link->count, 0, sizeof(*p));
    }
  }
  return 0;
}

static int read_token(demo_link_t *link, const c89cbor_token *token) {
  demo_packet_t *p = &link->input;
  if (link->read_phase == READ_ARRAY) {
    if (token->type != C89CBOR_ARRAY || token->indefinite || !token->argument ||
        token->argument > DEMO_FIELDS_MAX)
      return -1;
    link->item_count = (size_t)token->argument;
    link->read_phase = READ_FIELDS;
    return 0;
  }
  if (link->read_phase != READ_FIELDS)
    return -1;
  if (link->reading_string) {
    demo_field_t *field = fields(p) + p->count - 1;
    if (token->type == C89CBOR_END &&
        token->argument == (uint64_t)field->type &&
        link->string_read == field->value) {
      link->reading_string = 0;
      return 0;
    }
    if (token->type != C89CBOR_DATA ||
        token->length > field->value - link->string_read ||
        (field->type == C89CBOR_TEXT && memchr(token->data, 0, token->length)))
      return -1;
    memcpy((char *)field->bytes + link->string_read, token->data,
           token->length);
    link->string_read += token->length;
    return 0;
  }
  if (p->count == link->item_count) {
    if (token->type != C89CBOR_END || token->argument != C89CBOR_ARRAY)
      return -1;
    link->read_phase = READ_END;
    return 0;
  }
  if (token->type == C89CBOR_UINT || token->type == C89CBOR_NULL)
    return append(p, token->type, token->argument, NULL) ? 0 : -1;
  if ((token->type == C89CBOR_TEXT || token->type == C89CBOR_BYTES) &&
      !token->indefinite && token->argument <= link->frame_size) {
    size_t length = (size_t)token->argument;
    char *storage = malloc(length + 1);
    if (!storage)
      return -1;
    storage[length] = 0;
    if (!append(p, token->type, length, storage)) {
      free(storage);
      return -1;
    }
    link->reading_string = 1;
    link->string_read = 0;
    return 0;
  }
  return -1;
}

int demo_link_read(demo_link_t *link, demo_packet_t *packet) {
  size_t budget = IO_BUDGET;
  if (!link->reader_live) {
    if (c89cbor_reader_init(NULL, &link->reader, C89CBOR_LENGTH32,
                            (uint32_t)link->input_limit) != C89CBOR_OK)
      return -1;
    link->reader_live = 1;
  }
  for (;;) {
    c89cbor_event event;
    size_t consumed;
    int status =
        c89cbor_reader_next(&link->reader, link->buffer + link->consumed,
                            link->buffered - link->consumed, &consumed, &event);
    link->consumed += consumed;
    if (status == C89CBOR_TOKEN) {
      if (event.type == C89CBOR_RECORD_BEGIN) {
        /* A handler can narrow the limit after the connection's first frame. */
        if (event.length > link->input_limit)
          return -1;
        link->frame_size = link->remaining = event.length;
        link->read_phase = READ_ARRAY;
      } else if (event.type == C89CBOR_RECORD_TOKEN) {
        if (read_token(link, &event.token))
          return -1;
      } else {
        if (link->read_phase != READ_END || link->remaining)
          return -1;
        *packet = link->input;
        memset(&link->input, 0, sizeof(link->input));
        link->read_phase = READ_HEADER;
        link->remaining = sizeof(link->header);
        return 1;
      }
    } else if (status == C89CBOR_INPUT) {
      size_t amount = link->remaining;
      ssize_t got;
      if (!amount)
        return -1;
      if (!budget)
        return 0;
      if (amount > sizeof(link->buffer))
        amount = sizeof(link->buffer);
      if (amount > budget)
        amount = budget;
      do {
        got = recv(link->fd, link->buffer, amount, 0);
      } while (got < 0 && errno == EINTR);
      if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
      if (got <= 0)
        return -1;
      link->buffered = (size_t)got;
      link->consumed = 0;
      link->remaining -= (size_t)got;
      budget -= (size_t)got;
    } else {
      return -1;
    }
  }
}
