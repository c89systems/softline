#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "www_mock.h"
#include "web_terminal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PEERS 8
#define LOG_BYTES (16u * 1024u * 1024u)
#define LOG_EVENTS 8192u
#define REPLY_BYTES (512u * 1024u)
#define INPUT_CAP (SL_WWW_REQUEST_MAX + 32u)
typedef struct frame {
  struct frame *next;
  unsigned char *data;
  size_t length, offset;
} frame_t;
typedef struct record {
  struct record *next;
  unsigned long seq;
  unsigned char *data;
  size_t length;
  int state;
} record_t;
typedef struct terminal_entry {
  struct terminal_entry *next;
  demo_web_terminal_t *value;
} terminal_entry_t;
struct demo_www_peer {
  demo_www_t *server;
  int fd, websocket, dead, opcode;
  unsigned char *input, *message;
  size_t input_length, message_length, queued;
  frame_t *head, *tail;
  record_t *cursor;
  unsigned long replay_before;
  sl_www_asset_t asset;
  size_t asset_at;
  int asset_active, close_after_write;
  sl_watch_id_t watch;
  void *data;
  int terminal_route;
  int terminal_closing;
  double terminal_close_due;
  terminal_entry_t *terminal;
};
struct demo_www {
  int listener, polling;
  unsigned short port;
  sl_www_t *handler;
  sl_www_config_t host;
  demo_www_closed_t closed;
  demo_www_peer_t peers[PEERS];
  sl_t *editor;
  sl_watch_id_t watch;
  record_t *head, *tail;
  size_t log_bytes, log_events;
  unsigned long sequence;
  char epoch[96];
  int publishing_state;
  int terminal_enabled;
  size_t terminal_count;
  int terminal_prompt_lock, published_prompt_lock;
  size_t external_handoffs, published_handoffs;
  terminal_entry_t *retired;
};
const char *demo_www_epoch(const demo_www_t *w) { return w->epoch; }
static int peer_write(void *, const void *, size_t);
static double terminal_time(void) {
  struct timespec now;
  (void)clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000.0 + now.tv_nsec / 1000000.0;
}
void demo_www_enable_terminal(demo_www_t *w) {
  if (w)
    w->terminal_enabled = 1;
}
int demo_www_terminal_enabled(const demo_www_t *w) {
  return w && w->terminal_enabled;
}
static size_t terminal_handoffs(const demo_www_t *w) {
  size_t i, count = w->external_handoffs;
  for (i = 0; i < PEERS; ++i)
    if (w->peers[i].terminal &&
        demo_web_terminal_active(w->peers[i].terminal->value))
      ++count;
  return count;
}
static int terminal_state(demo_www_t *w, demo_www_peer_t *peer) {
  char active[32];
  sl_www_field_t fields[2] = {{"active", NULL, 0}, {"disable_prompt", NULL, 0}};
  size_t count = terminal_handoffs(w);
  fields[0].value = active;
  (void)snprintf(active, sizeof(active), "%lu", (unsigned long)count);
  fields[1].value = w->terminal_prompt_lock ? "1" : "0";
  if (peer)
    return demo_www_reply(peer, "terminal_state", fields, 2);
  if (count == w->published_handoffs &&
      w->terminal_prompt_lock == w->published_prompt_lock)
    return SL_OK;
  if (demo_www_publish(w, "terminal_state", fields, 2) != SL_OK)
    return SL_ERROR;
  w->published_handoffs = count;
  w->published_prompt_lock = w->terminal_prompt_lock;
  return SL_OK;
}
void demo_www_set_terminal_prompt_lock(demo_www_t *w, int enabled) {
  if (!w)
    return;
  w->terminal_prompt_lock = !!enabled;
  (void)terminal_state(w, NULL);
}
void demo_www_set_terminal_handoffs(demo_www_t *w, size_t active) {
  if (!w || active > (size_t)-1 - PEERS)
    return;
  w->external_handoffs = active;
  (void)terminal_state(w, NULL);
}
void demo_www_set_terminal_state(demo_www_t *w, size_t active, int enabled) {
  if (!w || active > (size_t)-1 - PEERS)
    return;
  w->external_handoffs = active;
  w->terminal_prompt_lock = !!enabled;
  (void)terminal_state(w, NULL);
}
void demo_www_terminal_status(const demo_www_t *w, size_t *active,
                              int *enabled) {
  *active = w ? terminal_handoffs(w) : 0;
  *enabled = w ? w->terminal_prompt_lock : 0;
}
int demo_www_terminal_lock_environment(int *enabled) {
  const char *value = getenv("SOFTLINE_WEB_DISABLE_PROMPT_DURING_TERMINAL");
  *enabled = value && !strcmp(value, "1");
  if (value && strcmp(value, "0") && strcmp(value, "1")) {
    errno = EINVAL;
    return -1;
  }
  return 0;
}
int demo_www_timeout(const demo_www_t *w, int timeout) {
  size_t i;
  if (!w || (timeout >= 0 && timeout <= 20))
    return timeout;
  if (w->terminal_count)
    return 20;
  for (i = 0; i < PEERS; ++i)
    if (w->peers[i].terminal_closing)
      return 20;
  return timeout;
}
static void retire_terminal(demo_www_peer_t *p) {
  if (!p->terminal)
    return;
  demo_web_terminal_close(p->terminal->value);
  p->terminal->next = p->server->retired;
  p->server->retired = p->terminal;
  p->terminal = NULL;
}
static void reap_terminals(demo_www_t *w) {
  terminal_entry_t **at = &w->retired;
  while (*at) {
    terminal_entry_t *entry = *at;
    if (demo_web_terminal_destroy(entry->value) == SL_ERROR_NOT_READY)
      at = &entry->next;
    else {
      *at = entry->next;
      free(entry);
      --w->terminal_count;
    }
  }
}
static int nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
                 fcntl(fd, F_SETFD, FD_CLOEXEC) < 0
             ? -1
             : 0;
}
static ssize_t socket_write(int fd, const void *data, size_t length) {
#if defined(MSG_NOSIGNAL)
  return send(fd, data, length, MSG_NOSIGNAL);
#else
  return send(fd, data, length, 0);
#endif
}
static void drop(demo_www_peer_t *p) {
  frame_t *frame;
  if (p->fd < 0)
    return;
  if (p->watch && p->server->editor)
    (void)sl_watch_remove(p->server->editor, p->watch);
  p->watch = 0;
  retire_terminal(p);
  close(p->fd);
  p->fd = -1;
  if (p->server->closed)
    p->server->closed(p->server->host.context, p);
  while ((frame = p->head) != NULL) {
    p->head = frame->next;
    free(frame->data);
    free(frame);
  }
  free(p->input);
  free(p->message);
  memset(p, 0, sizeof(*p));
  p->fd = -1;
}
static void interest(demo_www_peer_t *p) {
  if (p->watch && p->server->editor)
    (void)sl_watch_modify(
        p->server->editor, p->watch,
        SL_WATCH_READ |
            ((p->head || p->cursor || p->asset_active) ? SL_WATCH_WRITE : 0));
}
static int queue_frame(demo_www_peer_t *p, int opcode, const void *bytes,
                       size_t length) {
  frame_t *f;
  size_t header = opcode < 0 ? 0 : length < 126 ? 2 : length <= 65535 ? 4 : 10;
  size_t i;
  if (p->fd < 0 || length > REPLY_BYTES ||
      length + header > REPLY_BYTES - p->queued) {
    p->dead = 1;
    return SL_ERROR_FULL;
  }
  f = calloc(1, sizeof(*f));
  if (!f)
    return SL_ERROR_NOMEM;
  f->data = malloc(length + header);
  if (!f->data) {
    free(f);
    return SL_ERROR_NOMEM;
  }
  f->length = length + header;
  if (header) {
    f->data[0] = (unsigned char)(128 | opcode);
    f->data[1] = header == 2 ? (unsigned char)length : header == 4 ? 126 : 127;
    if (header > 2)
      for (i = 2; i < header; ++i)
        f->data[i] =
            (unsigned char)(((uint64_t)length >> (8 * (header - 1 - i))) &
                            255u);
  }
  memcpy(f->data + header, bytes, length);
  if (p->tail)
    p->tail->next = f;
  else
    p->head = f;
  p->tail = f;
  p->queued += f->length;
  interest(p);
  return SL_OK;
}
static int terminal_write(void *context, int binary, const void *bytes,
                          size_t length) {
  return queue_frame(context, binary ? 2 : 1, bytes, length);
}
static void finish_terminal(demo_www_peer_t *p, int failed) {
  unsigned char code[2] = {3, 232}; /* 1011 / 1000 */
  code[1] = (unsigned char)(failed ? 243 : 232);
  if (failed) {
    sl_www_field_t error = {"text", "The server terminal failed.", 0};
    (void)sl_www_emit(peer_write, p, "error", NULL, &error, 1);
  }
  retire_terminal(p);
  if (queue_frame(p, 8, code, sizeof(code)) != SL_OK)
    p->dead = 1;
  p->terminal_closing = 1;
  p->terminal_close_due = terminal_time() + 1000;
}
static int peer_write(void *context, const void *data, size_t length) {
  return queue_frame(context, 1, data, length);
}
int demo_www_reply(demo_www_peer_t *p, const char *type,
                   const sl_www_field_t *fields, size_t count) {
  return p && p->websocket
             ? sl_www_emit(peer_write, p, type, NULL, fields, count)
             : SL_ERROR_INVALID;
}
int demo_www_simulate_prompt(demo_www_peer_t *p, const char *text,
                             const sl_prompt_simulation_options_t *options) {
  return p && p->fd >= 0 && p->websocket && !p->terminal_route && !p->dead
             ? sl_www_simulate_prompt(p->server->handler, peer_write, p, text,
                                      options)
             : SL_ERROR_NOT_READY;
}
size_t demo_www_base64(const unsigned char *in, size_t n, char *out) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t i, at = 0;
  for (i = 0; i < n; i += 3) {
    unsigned long bits = (unsigned long)in[i] << 16;
    if (i + 1 < n)
      bits |= (unsigned long)in[i + 1] << 8;
    if (i + 2 < n)
      bits |= in[i + 2];
    out[at++] = alphabet[(bits >> 18) & 63];
    out[at++] = alphabet[(bits >> 12) & 63];
    out[at++] = i + 1 < n ? alphabet[(bits >> 6) & 63] : '=';
    out[at++] = i + 2 < n ? alphabet[bits & 63] : '=';
  }
  out[at] = 0;
  return at;
}
/* RFC 6455 handshake SHA-1. Only used by this loopback mock listener; this is
 * not an authentication primitive or a dependency of the shipped library. */
static uint32_t rotate(uint32_t v, unsigned int n) {
  return (v << n) | (v >> (32 - n));
}
static void websocket_accept(const char *key, char out[29]) {
  unsigned char input[128], digest[20];
  uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u,
                   0xc3d2e1f0u};
  size_t n = strlen(key), blocks, block, i;
  memset(input, 0, sizeof(input));
  memcpy(input, key, n);
  memcpy(input + n, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
  n += 36;
  input[n] = 128;
  blocks = (n + 9 + 63) / 64;
  input[blocks * 64 - 2] = (unsigned char)((n * 8) >> 8);
  input[blocks * 64 - 1] = (unsigned char)(n * 8);
  for (block = 0; block < blocks; ++block) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (i = 0; i < 16; ++i) {
      const unsigned char *p = input + block * 64 + i * 4;
      w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 |
             p[3];
    }
    for (i = 16; i < 80; ++i)
      w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (i = 0; i < 80; ++i) {
      uint32_t f = i < 20   ? ((b & c) | (~b & d))
                   : i < 40 ? b ^ c ^ d
                   : i < 60 ? ((b & c) | (b & d) | (c & d))
                            : b ^ c ^ d;
      uint32_t k = i < 20   ? 0x5a827999u
                   : i < 40 ? 0x6ed9eba1u
                   : i < 60 ? 0x8f1bbcdcu
                            : 0xca62c1d6u;
      uint32_t t = rotate(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotate(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  for (i = 0; i < 20; ++i)
    digest[i] = (unsigned char)(h[i / 4] >> (24 - 8 * (i % 4)));
  (void)demo_www_base64(digest, 20, out);
}
static int header(const char *request, const char *name, char *out,
                  size_t cap) {
  const char *line = strstr(request, "\r\n");
  size_t length = strlen(name);
  int found = 0;
  while (line) {
    const char *end;
    size_t i;
    line += 2;
    end = strstr(line, "\r\n");
    if (!end)
      return -1;
    if (end == line)
      return found;
    if (*line == ' ' || *line == '\t')
      return -1;
    for (i = 0; i < length && line + i < end; ++i) {
      unsigned char c = (unsigned char)line[i];
      if (c >= 'A' && c <= 'Z')
        c = (unsigned char)(c + 'a' - 'A');
      if (c != (unsigned char)name[i])
        break;
    }
    if (i == length && line + i < end && line[i] == ':') {
      const char *value = line + i + 1, *last = end;
      while (value < last && (*value == ' ' || *value == '\t'))
        ++value;
      while (last > value && (last[-1] == ' ' || last[-1] == '\t'))
        --last;
      if (found || (size_t)(last - value) >= cap)
        return -1;
      memcpy(out, value, (size_t)(last - value));
      out[last - value] = 0;
      found = 1;
    }
    line = end;
  }
  return -1;
}

static int loopback_host(const char *host, unsigned short port) {
  char authority[sizeof("127.0.0.1:65535")];
  if (!strcasecmp(host, "localhost") || !strcmp(host, "127.0.0.1"))
    return 1;
  (void)snprintf(authority, sizeof(authority), "localhost:%u",
                 (unsigned int)port);
  if (!strcasecmp(host, authority))
    return 1;
  (void)snprintf(authority, sizeof(authority), "127.0.0.1:%u",
                 (unsigned int)port);
  return !strcmp(host, authority);
}
static int replay(void *context, void *peer, const char *after) {
  demo_www_t *w = context;
  demo_www_peer_t *p = peer;
  record_t *r;
  unsigned long seq = 0;
  int status;
  char *end;
  if (after && !strncmp(after, w->epoch, strlen(w->epoch)) &&
      after[strlen(w->epoch)] == '.') {
    const char *number = after + strlen(w->epoch) + 1;
    errno = 0;
    seq = strtoul(number, &end, 10);
    if (errno || *end || *number < '0' || *number > '9')
      seq = 0;
  }
  if (!seq || (w->head && seq < w->head->seq - 1) || seq > w->sequence) {
    sl_www_field_t f = {"text", "", 0};
    if (w->head && w->head->seq > 1)
      f.value = "Showing the retained conversation window.";
    if (demo_www_reply(p, "reset", &f, 1) != SL_OK)
      return SL_ERROR;
    seq = 0;
  }
  for (r = w->head; r && r->seq <= seq; r = r->next) {
  }
  p->cursor = r;
  p->replay_before = w->sequence;
  while (p->cursor && p->cursor->state && p->cursor->seq <= p->replay_before)
    p->cursor = p->cursor->next;
  status = w->host.replay
               ? w->host.replay(w->host.context, peer, seq ? after : "")
               : SL_OK;
  return status == SL_OK ? terminal_state(w, p) : status;
}
static int action(void *context, const sl_www_request_t *r) {
  demo_www_t *w = context;
  return w->host.action ? w->host.action(w->host.context, r) : SL_ERROR_INVALID;
}
static int binary(void *context, const sl_www_request_t *r, const void *p,
                  size_t n) {
  demo_www_t *w = context;
  return w->host.binary ? w->host.binary(w->host.context, r, p, n)
                        : SL_ERROR_INVALID;
}
static int http(demo_www_peer_t *p) {
  char method[8], path[4096], version[16], key[64], host[256], origin[512];
  char response[1024], accept[29], expected[264], upgrade[64], connection[128],
      ws_version[16], *query, *end;
  const char *after = "";
  int count;
  p->input[p->input_length] = 0;
  end = strstr((char *)p->input, "\r\n\r\n");
  if (!end)
    return p->input_length >= 8192 ? -1 : 0;
  if (sscanf((char *)p->input, "%7s %4095s %15s", method, path, version) != 3 ||
      strcmp(version, "HTTP/1.1"))
    return -1;
  if (header((char *)p->input, "host", host, sizeof(host)) != 1 ||
      !loopback_host(host, p->server->port)) {
    static const char bad_host[] = "HTTP/1.1 400 Bad Request\r\n"
                                   "Connection: close\r\n"
                                   "Content-Length: 0\r\n\r\n";
    p->close_after_write = 1;
    return queue_frame(p, -1, bad_host, sizeof(bad_host) - 1) == SL_OK ? 0 : -1;
  }
  query = strchr(path, '?');
  if (query)
    *query++ = 0;
  if ((!strcmp(path, "/events") ||
       (!strcmp(path, "/terminal/events") && p->server->terminal_enabled)) &&
      !strcmp(method, "GET")) {
    size_t i;
    if (header((char *)p->input, "upgrade", upgrade, sizeof(upgrade)) != 1 ||
        strcasecmp(upgrade, "websocket") ||
        header((char *)p->input, "connection", connection,
               sizeof(connection)) != 1 ||
        header((char *)p->input, "sec-websocket-version", ws_version,
               sizeof(ws_version)) != 1 ||
        strcmp(ws_version, "13"))
      return -1;
    {
      char *token = connection;
      int found = 0;
      while (*token) {
        char *next = strchr(token, ','), *last;
        if (next)
          *next = 0;
        while (*token == ' ' || *token == '\t')
          ++token;
        last = token + strlen(token);
        while (last > token && (last[-1] == ' ' || last[-1] == '\t'))
          *--last = 0;
        if (!strcasecmp(token, "upgrade"))
          found = 1;
        if (!next)
          break;
        token = next + 1;
      }
      if (!found)
        return -1;
    }
    if (header((char *)p->input, "sec-websocket-key", key, sizeof(key)) != 1 ||
        strlen(key) != 24)
      return -1;
    for (i = 0; key[i]; ++i)
      if (!((key[i] >= 'A' && key[i] <= 'Z') ||
            (key[i] >= 'a' && key[i] <= 'z') ||
            (key[i] >= '0' && key[i] <= '9') || key[i] == '+' ||
            key[i] == '/' || key[i] == '='))
        return -1;
    count = header((char *)p->input, "origin", origin, sizeof(origin));
    if (count < 0)
      return -1;
    if (count == 1) {
      (void)snprintf(expected, sizeof(expected), "http://%s", host);
      if (strcasecmp(origin, expected))
        return -1;
    }
    websocket_accept(key, accept);
    count = snprintf(
        response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: "
        "Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    if (count < 0 || (size_t)count >= sizeof(response) ||
        queue_frame(p, -1, response, (size_t)count) != SL_OK)
      return -1;
    p->websocket = 1;
    if (!strcmp(path, "/terminal/events")) {
      terminal_entry_t *entry;
      if (p->server->terminal_count == PEERS ||
          !(entry = calloc(1, sizeof(*entry))))
        return -1;
      entry->value = demo_web_terminal_create(terminal_write, p);
      if (!entry->value) {
        free(entry);
        return -1;
      }
      p->terminal = entry;
      p->terminal_route = 1;
      ++p->server->terminal_count;
    }
    if (query) {
      char *at = strstr(query, "after=");
      if (at && (at == query || at[-1] == '&')) {
        after = at + 6;
        at = strchr((char *)after, '&');
        if (at)
          *at = 0;
      }
    }
    if (!p->terminal_route &&
        sl_www_connect(p->server->handler, p, after) != SL_OK)
      return -1;
  } else {
    int found = !strcmp(method, "GET") || !strcmp(method, "HEAD");
    found = found && sl_www_asset(p->server->handler, path, &p->asset) == SL_OK;
    count = snprintf(response, sizeof(response),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: "
                     "%lu\r\nConnection: close\r\nX-Content-Type-Options: "
                     "nosniff\r\nCache-Control: no-cache\r\n\r\n",
                     found ? "200 OK" : "404 Not Found",
                     found ? p->asset.content_type : "text/plain",
                     (unsigned long)(found ? p->asset.length : 0));
    if (count < 0 || (size_t)count >= sizeof(response) ||
        queue_frame(p, -1, response, (size_t)count) != SL_OK)
      return -1;
    p->asset_active = found && strcmp(method, "HEAD");
    p->close_after_write = 1;
  }
  {
    size_t consumed = (size_t)(end + 4 - (char *)p->input);
    memmove(p->input, p->input + consumed, p->input_length - consumed);
    p->input_length -= consumed;
  }
  return 1;
}
static int input_frame(demo_www_peer_t *p) {
  unsigned char *b = p->input;
  uint64_t length;
  size_t header = 2, i, total;
  int opcode, final, result;
  size_t limit = p->terminal_route ? 4096u : SL_WWW_REQUEST_MAX;
  if (p->input_length < 2)
    return 0;
  final = (b[0] & 128) != 0;
  opcode = b[0] & 15;
  length = b[1] & 127;
  if ((b[0] & 112) || !(b[1] & 128))
    return -1;
  if (length == 126) {
    if (p->input_length < 4)
      return 0;
    length = (uint64_t)b[2] << 8 | b[3];
    header = 4;
    if (length < 126)
      return -1;
  } else if (length == 127) {
    if (p->input_length < 10)
      return 0;
    length = 0;
    for (i = 2; i < 10; ++i)
      length = (length << 8) | b[i];
    header = 10;
    if (length <= 65535)
      return -1;
  }
  if (length > limit || (opcode >= 8 && (!final || length > 125)))
    return -1;
  total = header + 4 + (size_t)length;
  if (p->input_length < total)
    return 0;
  for (i = 0; i < (size_t)length; ++i)
    b[header + 4 + i] ^= b[header + i % 4];
  if (opcode == 8)
    return -1;
  if (opcode == 9)
    result = queue_frame(p, 10, b + header + 4, (size_t)length);
  else if (opcode == 10)
    result = SL_OK;
  else if (p->terminal_closing)
    result = SL_OK;
  else {
    if ((opcode != 0 && opcode != 1 && opcode != 2) ||
        (opcode == 0 ? !p->opcode : p->opcode != 0) ||
        (size_t)length > limit - p->message_length)
      return -1;
    if (opcode)
      p->opcode = opcode;
    memcpy(p->message + p->message_length, b + header + 4, (size_t)length);
    p->message_length += (size_t)length;
    result = SL_OK;
    if (final) {
      result =
          p->terminal_route
              ? demo_web_terminal_message(p->terminal->value, p->opcode == 2,
                                          p->message, p->message_length)
          : p->opcode == 2 ? sl_www_binary(p->server->handler, p, p->message,
                                           p->message_length, peer_write, p)
                           : sl_www_message(p->server->handler, p, p->message,
                                            p->message_length, peer_write, p);
      p->opcode = 0;
      p->message_length = 0;
      if (result != SL_OK) {
        sl_www_field_t f = {"text", "Action rejected by host.", 0};
        if (p->terminal_route)
          finish_terminal(p, 1);
        else
          (void)demo_www_reply(p, "error", &f, 1);
        result = SL_OK;
      }
    }
  }
  if (result != SL_OK)
    return -1;
  memmove(b, b + total, p->input_length - total);
  p->input_length -= total;
  return 1;
}
static int flush(demo_www_peer_t *p) {
  unsigned int budget = 16;
  while (budget--) {
    ssize_t n;
    /* Replay conversation bytes, then the host's fresh state projection.
     * Historical approvals/status must not briefly become live controls. */
    while (!p->head && p->cursor && p->cursor->state &&
           p->cursor->seq <= p->replay_before)
      p->cursor = p->cursor->next;
    if (!p->head && p->cursor) {
      record_t *r = p->cursor;
      if (queue_frame(p, 1, r->data, r->length) != SL_OK)
        return -1;
      p->cursor = r->next;
    }
    if (p->head) {
      frame_t *f = p->head;
      n = socket_write(p->fd, f->data + f->offset, f->length - f->offset);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
      if (n <= 0)
        return -1;
      f->offset += (size_t)n;
      if (f->offset != f->length)
        return 0;
      p->head = f->next;
      if (!p->head)
        p->tail = NULL;
      p->queued -= f->length;
      free(f->data);
      free(f);
    } else if (p->asset_active) {
      size_t count = p->asset.length - p->asset_at;
      if (count > 16384)
        count = 16384;
      n = socket_write(p->fd, p->asset.data + p->asset_at, count);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
      if (n <= 0)
        return -1;
      p->asset_at += (size_t)n;
      if (p->asset_at == p->asset.length)
        p->asset_active = 0;
    } else
      return p->close_after_write ? -1 : 0;
  }
  return 0;
}
static int ready(sl_t *sl, const sl_watch_event_t *event, void *context) {
  (void)sl;
  (void)event;
  return demo_www_poll(context, 0) == 0 ? SL_OK : SL_ERROR_IO;
}
int demo_www_descriptors(demo_www_t *w, struct pollfd *fds, size_t capacity) {
  size_t i;
  if (!w || !fds || capacity < DEMO_WWW_FDS)
    return -1;
  fds[0].fd = w->listener;
  fds[0].events = POLLIN;
  fds[0].revents = 0;
  for (i = 0; i < PEERS; ++i) {
    demo_www_peer_t *p = w->peers + i;
    if (p->terminal_closing && terminal_time() >= p->terminal_close_due)
      p->dead = 1;
    if (p->dead)
      drop(p);
    interest(p);
    fds[i + 1].fd = p->fd;
    fds[i + 1].events =
        (p->close_after_write ? 0 : POLLIN) |
        ((p->head || p->cursor || p->asset_active) ? POLLOUT : 0);
    fds[i + 1].revents = 0;
    demo_web_terminal_descriptor(p->terminal ? p->terminal->value : NULL,
                                 fds + PEERS + 1 + i);
  }
  return DEMO_WWW_FDS;
}
int demo_www_poll(demo_www_t *w, int timeout_ms) {
  struct pollfd fds[DEMO_WWW_FDS];
  size_t i;
  int result;
  if (!w || w->polling)
    return w ? 0 : -1;
  w->polling = 1;
  reap_terminals(w);
  if (demo_www_descriptors(w, fds, DEMO_WWW_FDS) < 0) {
    w->polling = 0;
    return -1;
  }
  result = poll(fds, DEMO_WWW_FDS, demo_www_timeout(w, timeout_ms));
  if (result < 0 && errno != EINTR) {
    w->polling = 0;
    return -1;
  }
  if (fds[0].revents & POLLIN) {
    int fd = accept(w->listener, NULL, NULL);
    if (fd >= 0) {
      for (i = 0; i < PEERS && w->peers[i].fd >= 0; ++i) {
      }
      if (i == PEERS || nonblock(fd))
        close(fd);
      else {
        demo_www_peer_t *p = w->peers + i;
#if defined(SO_NOSIGPIPE)
        int yes = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
        p->server = w;
        p->fd = fd;
        p->input = malloc(INPUT_CAP + 1);
        p->message = malloc(SL_WWW_REQUEST_MAX + 1);
        if (!p->input || !p->message ||
            (w->editor && sl_watch_add(w->editor, fd, SL_WATCH_READ, ready, w,
                                       &p->watch) != SL_OK))
          drop(p);
      }
    }
  }
  for (i = 0; i < PEERS; ++i) {
    demo_www_peer_t *p = w->peers + i;
    if (p->fd < 0)
      continue;
    if (fds[i + 1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      drop(p);
      continue;
    }
    if (!p->close_after_write && (fds[i + 1].revents & POLLIN)) {
      ssize_t n = recv(p->fd, p->input + p->input_length,
                       INPUT_CAP - p->input_length, 0);
      int rc = 0;
      if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR)) {
        drop(p);
        continue;
      }
      if (n > 0)
        p->input_length += (size_t)n;
      if (!p->websocket)
        rc = http(p);
      if (rc >= 0 && p->websocket)
        do {
          rc = input_frame(p);
        } while (rc > 0 && !p->dead && !p->close_after_write);
      if (rc < 0 || p->input_length == INPUT_CAP || p->dead) {
        drop(p);
        continue;
      }
    }
    if (p->terminal) {
      int state = demo_web_terminal_pump(p->terminal->value);
      if (state)
        finish_terminal(p, state < 0);
    }
    if (flush(p) < 0) {
      drop(p);
      continue;
    }
    interest(p);
  }
  (void)terminal_state(w, NULL);
  w->polling = 0;
  return 0;
}
demo_www_t *demo_www_open(const sl_www_config_t *config, unsigned short port,
                          demo_www_closed_t closed) {
  demo_www_t *w = calloc(1, sizeof(*w));
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  sl_www_config_t adapted;
  int reuse = 1;
  struct timespec now;
  size_t i;
  if (!w || !config) {
    free(w);
    return NULL;
  }
  w->listener = -1;
  if (clock_gettime(CLOCK_REALTIME, &now)) {
    free(w);
    return NULL;
  }
  (void)snprintf(w->epoch, sizeof(w->epoch), "%lu-%lu-%lu",
                 (unsigned long)getpid(), (unsigned long)now.tv_sec,
                 (unsigned long)now.tv_nsec);
  w->host = *config;
  w->closed = closed;
  for (i = 0; i < PEERS; ++i)
    w->peers[i].fd = -1;
  adapted = *config;
  adapted.replay = replay;
  adapted.action = action;
  adapted.binary = config->binary ? binary : NULL;
  adapted.context = w;
  w->handler = sl_www_create(&adapted);
  w->listener = socket(AF_INET, SOCK_STREAM, 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(0x7f000001UL);
  address.sin_port = htons(port);
  if (!w->handler || w->listener < 0 ||
      setsockopt(w->listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                 sizeof(reuse)) ||
      nonblock(w->listener) ||
      bind(w->listener, (struct sockaddr *)&address, sizeof(address)) ||
      listen(w->listener, PEERS) ||
      getsockname(w->listener, (struct sockaddr *)&address, &length)) {
    demo_www_close(w);
    return NULL;
  }
  w->port = ntohs(address.sin_port);
  return w;
}
int demo_www_attach(demo_www_t *w, sl_t *editor) {
  if (!w || !editor || w->editor)
    return SL_ERROR_INVALID;
  w->editor = editor;
  return sl_watch_add(editor, w->listener, SL_WATCH_READ, ready, w, &w->watch);
}
void demo_www_close(demo_www_t *w) {
  size_t i;
  if (!w)
    return;
  for (i = 0; i < PEERS; ++i)
    drop(w->peers + i);
  while (w->retired) {
    reap_terminals(w);
    if (w->retired)
      (void)poll(NULL, 0, 1);
  }
  if (w->watch && w->editor)
    (void)sl_watch_remove(w->editor, w->watch);
  if (w->listener >= 0)
    close(w->listener);
  while (w->head) {
    record_t *r = w->head;
    w->head = r->next;
    free(r->data);
    free(r);
  }
  sl_www_destroy(w->handler);
  free(w);
}
unsigned short demo_www_port(const demo_www_t *w) { return w ? w->port : 0; }
void demo_www_peer_data(demo_www_peer_t *p, void *data) {
  if (p)
    p->data = data;
}
void *demo_www_peer_get(demo_www_peer_t *p) { return p ? p->data : NULL; }
static int save_record(void *context, const void *data, size_t length) {
  demo_www_t *w = context;
  record_t *r = calloc(1, sizeof(*r));
  size_t i;
  if (!r)
    return SL_ERROR_NOMEM;
  r->data = malloc(length);
  if (!r->data) {
    free(r);
    return SL_ERROR_NOMEM;
  }
  memcpy(r->data, data, length);
  r->length = length;
  r->seq = w->sequence;
  r->state = w->publishing_state;
  if (w->tail)
    w->tail->next = r;
  else
    w->head = r;
  w->tail = r;
  w->log_bytes += length;
  ++w->log_events;
  for (i = 0; i < PEERS; ++i) {
    demo_www_peer_t *p = w->peers + i;
    if (p->fd >= 0 && p->websocket && !p->terminal_route && !p->cursor) {
      p->cursor = r;
      interest(p);
    }
  }
  while (w->head && (w->log_bytes > LOG_BYTES || w->log_events > LOG_EVENTS)) {
    record_t *old = w->head;
    for (i = 0; i < PEERS; ++i)
      if (w->peers[i].cursor == old) {
        demo_www_peer_t *p = w->peers + i;
        if (old->state && old->seq <= p->replay_before)
          p->cursor = old->next;
        else {
          p->cursor = NULL;
          p->dead = 1;
        }
      }
    w->head = old->next;
    w->log_bytes -= old->length;
    --w->log_events;
    if (!w->head)
      w->tail = NULL;
    free(old->data);
    free(old);
  }
  return SL_OK;
}
int demo_www_publish(demo_www_t *w, const char *type,
                     const sl_www_field_t *fields, size_t count) {
  char seq[128];
  if (!w)
    return SL_OK;
  if (w->sequence == ULONG_MAX)
    return SL_ERROR_FULL;
  (void)snprintf(seq, sizeof(seq), "%s.%lu", w->epoch, ++w->sequence);
  w->publishing_state =
      type && (!strncmp(type, "queue_", 6) || !strcmp(type, "status") ||
               !strcmp(type, "terminal_state"));
  return sl_www_emit(save_record, w, type, seq, fields, count);
}
void demo_www_begin(demo_www_t *w, const char *id, const char *role) {
  sl_www_field_t f[2] = {{"id", NULL, 0}, {"role", NULL, 0}};
  f[0].value = id;
  f[1].value = role;
  (void)demo_www_publish(w, "begin", f, 2);
}
void demo_www_end(demo_www_t *w, const char *id) {
  sl_www_field_t f = {"id", NULL, 0};
  f.value = id;
  (void)demo_www_publish(w, "end", &f, 1);
}
void demo_www_text(demo_www_t *w, const char *id, const char *role,
                   const void *bytes, size_t length) {
  const unsigned char *p = bytes;
  char *encoded;
  sl_www_field_t f[4] = {{"id", NULL, 0},
                         {"role", NULL, 0},
                         {"data", NULL, 0},
                         {"encoding", "base64", 0}};
  if (!w)
    return;
  encoded = malloc(5500);
  if (!encoded)
    return;
  f[0].value = id;
  f[1].value = role;
  f[2].value = encoded;
  while (length) {
    size_t n = length > 4096 ? 4096 : length;
    f[2].length = demo_www_base64(p, n, encoded);
    if (demo_www_publish(w, "delta", f, 4) != SL_OK)
      break;
    p += n;
    length -= n;
  }
  free(encoded);
}
