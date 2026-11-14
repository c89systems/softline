#include "softline_clipboard.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__linux__)
static char *sl_clipboard_expand_template(const char *path_template);
#endif

static const char *sl_clipboard_token_value(const char *token, size_t length,
                                            const char *home,
                                            const char *cache) {
  if ((length == 4 && memcmp(token, "HOME", 4) == 0) ||
      (length == 4 && memcmp(token, "home", 4) == 0))
    return home;
  if ((length == 14 && memcmp(token, "XDG_CACHE_HOME", 14) == 0) ||
      (length == 14 && memcmp(token, "xdg_cache_home", 14) == 0))
    return cache;
  return NULL;
}

int sl_clipboard_valid_path_template(const char *path_template) {
  const char *cursor, *end, *star;
  if (!path_template)
    return 1;
  star = strchr(path_template, '*');
  if (!star || strchr(star + 1, '*') ||
      path_template[strlen(path_template) - 1] == '/')
    return 0;
  if (path_template[0] != '/' &&
      !(path_template[0] == '~' && path_template[1] == '/') &&
      strncmp(path_template, "{{", 2) != 0)
    return 0;
  for (cursor = path_template; *cursor;) {
    if (cursor[0] == '}' && cursor[1] == '}')
      return 0;
    if (cursor[0] == '{' && cursor[1] == '{') {
      end = strstr(cursor + 2, "}}");
      if (!end || !sl_clipboard_token_value(
                      cursor + 2, (size_t)(end - cursor - 2), "home", "cache"))
        return 0;
      cursor = end + 2;
    } else {
      cursor++;
    }
  }
  return 1;
}

void sl_clipboard_discard_image(const char *path, const char *path_template) {
  char *directory;
  char *expanded = NULL;
  char *slash;
  const char *star;
  size_t id_end;
  if (!path)
    return;
  (void)unlink(path);
#if defined(__linux__)
  if (path_template) {
    expanded = sl_clipboard_expand_template(path_template);
    path_template = expanded;
  }
#endif
  if (!path_template || !(star = strchr(path_template, '*')) ||
      !strchr(star, '/')) {
    free(expanded);
    return;
  }
  id_end = (size_t)(star - path_template) + 20;
  directory = strdup(path);
  if (!directory) {
    free(expanded);
    return;
  }
  slash = strrchr(directory, '/');
  while (slash && (size_t)(slash - directory) >= id_end) {
    *slash = '\0';
    if (rmdir(directory) != 0)
      break;
    slash = strrchr(directory, '/');
  }
  free(directory);
  free(expanded);
}

#if defined(__linux__)

#include "softline_resolver.h"
#include <arpa/inet.h>
#include <poll.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define SL_IMAGE_MAX_BYTES (32u * 1024u * 1024u)
#define SL_CLIPBOARD_TIMEOUT_MS 10000
#define SL_X11_PROPERTY_NOTIFY 28
#define SL_X11_SELECTION_NOTIFY 31
#define SL_X11_PROPERTY_CHANGE_MASK (1u << 22)
#define SL_X11_WINDOW_EVENT_MASK (1u << 11)
#define SL_X11_ATOM_STRING 31

typedef struct {
  int fd;
  uint16_t sequence;
  uint32_t resource_base, resource_mask, root;
  int peer_family;
  unsigned char peer_address[16];
} sl_x11_t;

typedef struct {
  char host[256];
  unsigned int display, screen;
  int local;
} sl_x11_display_t;

static void sl_clipboard_error(char *error, size_t size, const char *message) {
  if (error && size)
    (void)snprintf(error, size, "%s", message);
}

static uint16_t sl_get16(const unsigned char *p) {
  return (uint16_t)((unsigned int)p[0] | ((unsigned int)p[1] << 8));
}

static uint32_t sl_get32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static void sl_put16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}

static void sl_put32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
  p[2] = (unsigned char)(value >> 16);
  p[3] = (unsigned char)(value >> 24);
}

static int64_t sl_now_ms(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int sl_x11_parse_display(const char *value, sl_x11_display_t *display) {
  const char *colon, *end;
  size_t host_len;
  unsigned long number;
  char *parsed;
  if (!value || !*value)
    return -1;
  memset(display, 0, sizeof(*display));
  colon = strrchr(value, ':');
  if (!colon || !colon[1])
    return -1;
  host_len = (size_t)(colon - value);
  if (host_len >= sizeof(display->host))
    return -1;
  memcpy(display->host, value, host_len);
  display->host[host_len] = '\0';
  if (host_len >= 2 && display->host[0] == '[' &&
      display->host[host_len - 1] == ']') {
    memmove(display->host, display->host + 1, host_len - 2);
    display->host[host_len - 2] = '\0';
  }
  if (strcmp(display->host, "unix") == 0 ||
      strcmp(display->host, "unix/") == 0 || !display->host[0])
    display->local = 1;
  number = strtoul(colon + 1, &parsed, 10);
  if (parsed == colon + 1 || number > 59535)
    return -1;
  display->display = (unsigned int)number;
  end = parsed;
  if (*end == '.') {
    number = strtoul(end + 1, &parsed, 10);
    if (parsed == end + 1 || number > 255)
      return -1;
    display->screen = (unsigned int)number;
    end = parsed;
  }
  return *end == '\0' ? 0 : -1;
}

/* Xauthority records use network-order lengths even for little-endian X11. */
static int sl_auth_field(FILE *file, unsigned char *value, size_t capacity,
                         size_t *length) {
  unsigned char size[2];
  if (fread(size, 1, 2, file) != 2)
    return -1;
  *length = ((size_t)size[0] << 8) | size[1];
  if (*length > capacity || fread(value, 1, *length, file) != *length)
    return -1;
  return 0;
}

/* A hostname such as localhost may connect over either address family. Match
 * the credential to the connected peer, not to the DISPLAY spelling. */
static int sl_x11_authority(const sl_x11_display_t *display, const sl_x11_t *x,
                            unsigned char *cookie, size_t *cookie_len) {
  static const unsigned char ipv6_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                  0, 0, 0, 0, 0, 0, 0, 1};
  const char *path = getenv("XAUTHORITY"), *home;
  char fallback[512], hostname[256], number[16];
  unsigned char family_bytes[2], address[256], index[32], name[64], data[256];
  size_t address_len, index_len, name_len, data_len;
  int family, score, best = 0, loopback;
  FILE *file;
  if (!path || !*path) {
    home = getenv("HOME");
    if (!home || strlen(home) + sizeof("/.Xauthority") > sizeof(fallback))
      return 0;
    (void)snprintf(fallback, sizeof(fallback), "%s/.Xauthority", home);
    path = fallback;
  }
  file = fopen(path, "rb");
  if (!file)
    return 0;
  if (gethostname(hostname, sizeof(hostname)) != 0)
    hostname[0] = '\0';
  hostname[sizeof(hostname) - 1] = '\0';
  (void)snprintf(number, sizeof(number), "%u", display->display);
  loopback = (x->peer_family == AF_INET && x->peer_address[0] == 127) ||
             (x->peer_family == AF_INET6 &&
              memcmp(x->peer_address, ipv6_loopback, 16) == 0);
  while (fread(family_bytes, 1, 2, file) == 2) {
    family = ((int)family_bytes[0] << 8) | family_bytes[1];
    if (sl_auth_field(file, address, sizeof(address), &address_len) != 0 ||
        sl_auth_field(file, index, sizeof(index), &index_len) != 0 ||
        sl_auth_field(file, name, sizeof(name), &name_len) != 0 ||
        sl_auth_field(file, data, sizeof(data), &data_len) != 0)
      break;
    if (index_len != strlen(number) || memcmp(index, number, index_len) ||
        name_len != 18 || memcmp(name, "MIT-MAGIC-COOKIE-1", 18) || !data_len)
      continue;
    score = 0;
    if (family == 65535)
      score = 1;
    if ((x->peer_family == AF_UNIX || loopback) && family == 256 &&
        address_len == strlen(hostname) &&
        memcmp(address, hostname, address_len) == 0)
      score = 3;
    if (x->peer_family == AF_INET && family == 0 && address_len == 4 &&
        memcmp(address, x->peer_address, 4) == 0)
      score = 4;
    if (x->peer_family == AF_INET6 && family == 6 && address_len == 16 &&
        memcmp(address, x->peer_address, 16) == 0)
      score = 4;
    if (score > best) {
      memcpy(cookie, data, data_len);
      *cookie_len = data_len;
      best = score;
    }
  }
  fclose(file);
  return best;
}

static int sl_clipboard_random(unsigned char *bytes, size_t count) {
  int fd = open("/dev/urandom", O_RDONLY);
  ssize_t read_count;
  if (fd < 0)
    return -1;
  while (count) {
    read_count = read(fd, bytes, count);
    if (read_count < 0 && errno == EINTR)
      continue;
    if (read_count <= 0) {
      (void)close(fd);
      return -1;
    }
    bytes += read_count;
    count -= (size_t)read_count;
  }
  return close(fd);
}

/* xid's 12-byte layout: Unix seconds, machine id, PID, random-seeded atomic
 * counter. Base32hex is lowercase, unpadded, and exactly 20 characters. */
static int sl_clipboard_xid(char id[21]) {
  static volatile unsigned int counter;
  unsigned char raw[12], seed[3];
  char machine[256];
  const char *alphabet = "0123456789abcdefghijklmnopqrstuv";
  unsigned int hash = 2166136261u;
  unsigned int value, bits = 0, accumulator = 0;
  size_t index, out = 0;
  time_t now = time(NULL);
  int fd;
  ssize_t n;
  if (now == (time_t)-1)
    return -1;
  if (__sync_fetch_and_add(&counter, 0u) == 0) {
    if (sl_clipboard_random(seed, sizeof(seed)) != 0)
      return -1;
    value =
        ((unsigned int)seed[0] << 16) | ((unsigned int)seed[1] << 8) | seed[2];
    (void)__sync_bool_compare_and_swap(&counter, 0u, value ? value : 1u);
  }
  raw[0] = (unsigned char)((unsigned long)now >> 24);
  raw[1] = (unsigned char)((unsigned long)now >> 16);
  raw[2] = (unsigned char)((unsigned long)now >> 8);
  raw[3] = (unsigned char)now;
  fd = open("/etc/machine-id", O_RDONLY);
  n = fd >= 0 ? read(fd, machine, sizeof(machine)) : -1;
  if (fd >= 0)
    (void)close(fd);
  if (n <= 0) {
    if (gethostname(machine, sizeof(machine)) != 0)
      return -1;
    machine[sizeof(machine) - 1] = '\0';
    n = (ssize_t)strlen(machine);
  }
  for (index = 0; index < (size_t)n; index++)
    hash = (hash ^ (unsigned char)machine[index]) * 16777619u;
  raw[4] = (unsigned char)(hash >> 16);
  raw[5] = (unsigned char)(hash >> 8);
  raw[6] = (unsigned char)hash;
  value = (unsigned int)getpid();
  raw[7] = (unsigned char)(value >> 8);
  raw[8] = (unsigned char)value;
  value = __sync_add_and_fetch(&counter, 1u);
  raw[9] = (unsigned char)(value >> 16);
  raw[10] = (unsigned char)(value >> 8);
  raw[11] = (unsigned char)value;
  for (index = 0; index < sizeof(raw); index++) {
    accumulator = (accumulator << 8) | raw[index];
    bits += 8;
    while (bits >= 5) {
      bits -= 5;
      id[out++] = alphabet[(accumulator >> bits) & 31u];
    }
  }
  if (bits)
    id[out++] = alphabet[(accumulator << (5 - bits)) & 31u];
  id[out] = '\0';
  return out == 20 ? 0 : -1;
}

static int sl_clipboard_mkdir_parents(const char *path) {
  char *directory = strdup(path);
  char *slash, *cursor;
  struct stat info;
  int result = 0;
  if (!directory)
    return -1;
  slash = strrchr(directory, '/');
  if (!slash || slash == directory) {
    result = slash ? 0 : -1;
    free(directory);
    return result;
  }
  *slash = '\0';
  for (cursor = directory + 1;; cursor++) {
    if (*cursor != '/' && *cursor != '\0')
      continue;
    {
      char saved = *cursor;
      *cursor = '\0';
      if (mkdir(directory, 0700) != 0 &&
          (errno != EEXIST || stat(directory, &info) != 0 ||
           !S_ISDIR(info.st_mode))) {
        result = -1;
        *cursor = saved;
        break;
      }
      *cursor = saved;
      if (saved == '\0')
        break;
    }
  }
  free(directory);
  return result;
}

static char *sl_clipboard_expand_template(const char *path_template) {
  const char *home = getenv("HOME");
  const char *cache = getenv("XDG_CACHE_HOME");
  const char *cursor, *start, *end, *replacement;
  char *fallback = NULL, *result, *write;
  size_t capacity, length;
  if (!home || home[0] != '/')
    home = NULL;
  if (!cache || cache[0] != '/') {
    if (home) {
      fallback = malloc(strlen(home) + sizeof("/.cache"));
      if (fallback)
        (void)snprintf(fallback, strlen(home) + sizeof("/.cache"), "%s/.cache",
                       home);
    }
    cache = fallback;
  }
  if (!path_template)
    path_template = "{{XDG_CACHE_HOME}}/softline/*";
  capacity = strlen(path_template) + 1;
  cursor = path_template;
  if (cursor[0] == '~' && cursor[1] == '/') {
    if (!home || strlen(home) > (size_t)-1 - capacity)
      goto fail;
    capacity += strlen(home);
    cursor++;
  }
  while ((start = strstr(cursor, "{{")) != NULL) {
    end = strstr(start + 2, "}}");
    if (!end)
      goto fail;
    replacement = sl_clipboard_token_value(start + 2, (size_t)(end - start - 2),
                                           home, cache);
    if (!replacement || strlen(replacement) > (size_t)-1 - capacity)
      goto fail;
    capacity += strlen(replacement);
    cursor = end + 2;
  }
  result = malloc(capacity);
  if (!result)
    goto fail;
  cursor = path_template;
  write = result;
  if (cursor[0] == '~' && cursor[1] == '/') {
    if (!home)
      goto fail_result;
    length = strlen(home);
    memcpy(write, home, length);
    write += length;
    cursor++;
  }
  while ((start = strstr(cursor, "{{")) != NULL) {
    length = (size_t)(start - cursor);
    memcpy(write, cursor, length);
    write += length;
    end = strstr(start + 2, "}}");
    if (!end)
      goto fail_result;
    replacement = sl_clipboard_token_value(start + 2, (size_t)(end - start - 2),
                                           home, cache);
    if (!replacement)
      goto fail_result;
    length = strlen(replacement);
    memcpy(write, replacement, length);
    write += length;
    cursor = end + 2;
  }
  length = strlen(cursor);
  memcpy(write, cursor, length + 1);
  free(fallback);
  return result;
fail_result:
  free(result);
fail:
  free(fallback);
  return NULL;
}

static int sl_clipboard_image_file(const char *path_template,
                                   const char *suffix, char **path) {
  char *expanded_template;
  const char *star;
  size_t before, after, length;
  char id[21];
  int fd, attempt;
  expanded_template = sl_clipboard_expand_template(path_template);
  if (!expanded_template)
    return -1;
  path_template = expanded_template;
  if (!sl_clipboard_valid_path_template(path_template)) {
    free(expanded_template);
    return -1;
  }
  star = strchr(path_template, '*');
  before = (size_t)(star - path_template);
  after = strlen(star + 1);
  length = before + 20 + after + 1 + strlen(suffix) + 1;
  *path = malloc(length);
  if (!*path) {
    free(expanded_template);
    return -1;
  }
  for (attempt = 0; attempt < 8; attempt++) {
    if (sl_clipboard_xid(id) != 0)
      break;
    (void)snprintf(*path, length, "%.*s%s%s.%s", (int)before, path_template, id,
                   star + 1, suffix);
    if (sl_clipboard_mkdir_parents(*path) != 0)
      break;
    fd = open(*path, O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      free(expanded_template);
      return fd;
    }
    if (errno != EEXIST)
      break;
  }
  free(*path);
  *path = NULL;
  free(expanded_template);
  return -1;
}

/* One nonblocking X11 conversation. Request, reply and payload buffers are
 * bounded independently of the selected image's size. */
enum sl_capture_phase {
  CP_RESOLVE,
  CP_CONNECT,
  CP_SETUP,
  CP_SETUP_REPLY,
  CP_ATOM,
  CP_ATOM_REPLY,
  CP_WINDOW,
  CP_TIME,
  CP_TIME_EVENT,
  CP_SELECT,
  CP_CONVERT,
  CP_SELECT_EVENT,
  CP_BEGIN,
  CP_PROPERTY,
  CP_PROPERTY_REPLY,
  CP_INCR_EVENT,
  CP_END,
  CP_DONE
};
#define SL_CAPTURE_CHUNK 16384u
#define SL_SETUP_MAX 262140u
struct sl_clipboard {
  sl_x11_t x;
  sl_x11_display_t display;
  sl_lookup_t *lookup;
  struct ares_addrinfo *addresses;
  struct ares_addrinfo_node *address;
  enum sl_capture_phase phase;
  unsigned char *packet;
  size_t have, need;
  unsigned char tx[320];
  size_t tx_len, tx_at;
  uint32_t window, atoms[5], timestamp, target, property_offset;
  unsigned int atom_index;
  uint16_t expected;
  int incremental, is_png, property_first;
  size_t limit, total;
  unsigned char signature[8];
  size_t signature_len;
  char error[160];
};
static const char *const sl_capture_atoms[] = {
    "CLIPBOARD", "image/png", "image/jpeg", "SOFTLINE_IMAGE_PASTE", "INCR"};

static int sl_capture_fail(sl_clipboard_t *c, const char *reason) {
  sl_clipboard_error(c->error, sizeof(c->error), reason);
  return -1;
}

static void sl_capture_request(sl_clipboard_t *c, unsigned int opcode,
                               size_t length) {
  memset(c->tx, 0, sizeof(c->tx));
  c->tx[0] = (unsigned char)opcode;
  sl_put16(c->tx + 2, (uint16_t)(length / 4));
  c->tx_len = length;
  c->tx_at = 0;
  c->expected = ++c->x.sequence;
}

static int sl_capture_send(sl_clipboard_t *c) {
  ssize_t n;
  if (c->tx_at == c->tx_len)
    return 1;
  n = send(c->x.fd, c->tx + c->tx_at, c->tx_len - c->tx_at, MSG_NOSIGNAL);
  if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
    return 0;
  if (n <= 0)
    return sl_capture_fail(c, "X11 clipboard connection lost while writing");
  c->tx_at += (size_t)n;
  return c->tx_at == c->tx_len;
}

/* One recv per call. A peer cannot make a pump spin on unsolicited events. */
static int sl_capture_packet(sl_clipboard_t *c, int setup) {
  ssize_t n;
  size_t extra, bound = setup ? SL_SETUP_MAX : SL_CAPTURE_CHUNK + 32u;
  if (c->have == c->need && c->need) {
    c->have = c->need = 0;
  }
  if (!c->need)
    c->need = setup ? 8 : 32;
  n = recv(c->x.fd, c->packet + c->have, c->need - c->have, 0);
  if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
    return 0;
  if (n <= 0)
    return sl_capture_fail(c, "X11 clipboard owner or connection lost");
  c->have += (size_t)n;
  if (c->have != c->need)
    return 0;
  if (c->need == (setup ? 8u : 32u)) {
    if (setup)
      extra = (size_t)sl_get16(c->packet + 6) * 4;
    else if (c->packet[0] == 1 || (c->packet[0] & 127) == 35) {
      uint32_t units = sl_get32(c->packet + 4);
      if (units > (bound - 32) / 4)
        return sl_capture_fail(c,
                               "X11 clipboard reply exceeds bounded request");
      extra = (size_t)units * 4;
    } else
      extra = 0;
    if (extra) {
      if (extra > bound - c->need)
        return sl_capture_fail(c, "X11 clipboard setup exceeds limit");
      c->need += extra;
      return 0;
    }
  }
  if (!setup && c->packet[0] == 0)
    return sl_capture_fail(c, "X11 clipboard protocol error");
  return 1;
}

static int sl_capture_connect(sl_clipboard_t *c) {
  struct sockaddr_un local;
  const struct sockaddr *address;
  socklen_t length;
  struct sockaddr_storage peer;
  int result, flags;
  if (c->x.fd >= 0) {
    struct pollfd fd;
    fd.fd = c->x.fd;
    fd.events = POLLOUT;
    fd.revents = 0;
    if (poll(&fd, 1, 0) <= 0)
      return 0;
    length = sizeof(result);
    if (getsockopt(c->x.fd, SOL_SOCKET, SO_ERROR, &result, &length) != 0 ||
        result) {
      close(c->x.fd);
      c->x.fd = -1;
      if (c->display.local)
        return sl_capture_fail(c, "cannot connect to X11 DISPLAY");
      c->address = c->address->ai_next;
      return 0;
    }
    length = sizeof(peer);
    if (getpeername(c->x.fd, (struct sockaddr *)&peer, &length) != 0)
      return sl_capture_fail(c, "cannot inspect X11 peer");
    c->x.peer_family = peer.ss_family;
    if (peer.ss_family == AF_INET)
      memcpy(c->x.peer_address, &((struct sockaddr_in *)&peer)->sin_addr, 4);
    else if (peer.ss_family == AF_INET6)
      memcpy(c->x.peer_address, &((struct sockaddr_in6 *)&peer)->sin6_addr, 16);
    c->phase = CP_SETUP;
    return 1;
  }
  if (c->display.local) {
    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    (void)snprintf(local.sun_path, sizeof(local.sun_path), "/tmp/.X11-unix/X%u",
                   c->display.display);
    address = (struct sockaddr *)&local;
    length = sizeof(local);
    c->x.fd = socket(AF_UNIX, SOCK_STREAM, 0);
  } else {
    if (!c->address)
      return sl_capture_fail(c, "cannot connect to X11 DISPLAY");
    address = c->address->ai_addr;
    length = c->address->ai_addrlen;
    c->x.fd =
        socket(c->address->ai_family, SOCK_STREAM, c->address->ai_protocol);
  }
  if (c->x.fd < 0)
    return sl_capture_fail(c, "cannot open X11 connection");
  flags = fcntl(c->x.fd, F_GETFL, 0);
  if (flags < 0 || fcntl(c->x.fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      fcntl(c->x.fd, F_SETFD, FD_CLOEXEC) < 0)
    return sl_capture_fail(c, "cannot configure X11 connection");
  result = connect(c->x.fd, address, length);
  if (result < 0 && errno != EINPROGRESS) {
    close(c->x.fd);
    c->x.fd = -1;
    if (c->display.local)
      return sl_capture_fail(c, "cannot connect to X11 DISPLAY");
    c->address = c->address->ai_next;
  }
  return 0;
}

sl_clipboard_t *sl_clipboard_open(size_t limit) {
  sl_clipboard_t *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  c->x.fd = -1;
  c->limit = limit;
  c->packet = malloc(SL_SETUP_MAX);
  if (!c->packet) {
    free(c);
    return NULL;
  }
  c->phase = CP_RESOLVE;
  if (!getenv("DISPLAY") || !*getenv("DISPLAY"))
    sl_capture_fail(c, "X11 DISPLAY is unavailable");
  else if (sl_x11_parse_display(getenv("DISPLAY"), &c->display) != 0)
    sl_capture_fail(c, "invalid X11 DISPLAY");
  return c;
}

int sl_clipboard_prepare(sl_clipboard_t *c, struct pollfd *fds, size_t capacity,
                         int *timeout_ms) {
  if (!c || !capacity)
    return -1;
  *timeout_ms = 0;
  if (c->error[0] || c->phase == CP_DONE)
    return 0;
  if (c->lookup)
    return sl_resolve_prepare(c->lookup, fds, capacity, timeout_ms);
  if (c->x.fd < 0 || c->phase == CP_RESOLVE || c->phase == CP_SETUP ||
      c->phase == CP_ATOM || c->phase == CP_WINDOW || c->phase == CP_TIME ||
      c->phase == CP_SELECT || c->phase == CP_CONVERT || c->phase == CP_BEGIN ||
      c->phase == CP_PROPERTY || c->phase == CP_END)
    return 0;
  fds[0].fd = c->x.fd;
  fds[0].events =
      c->phase == CP_CONNECT || c->tx_at < c->tx_len ? POLLOUT : POLLIN;
  fds[0].revents = 0;
  *timeout_ms = -1;
  return 1;
}

/* Emit at most one bounded event, and perform at most one socket operation. */
int sl_clipboard_next(sl_clipboard_t *c, sl_image_paste_event_t *event) {
  int result;
  size_t length, cursor, i;
  unsigned char *p;
  if (!c || c->error[0])
    return -1;
  memset(event, 0, sizeof(*event));
  event->media_type =
      c->target ? (c->is_png ? "image/png" : "image/jpeg") : NULL;
  event->filename = c->target ? (c->is_png ? "image.png" : "image.jpeg") : NULL;
  event->offset = c->total;
  if (c->tx_at < c->tx_len)
    return sl_capture_send(c) < 0 ? -1 : 0;
  switch (c->phase) {
  case CP_RESOLVE:
    if (c->display.local) {
      c->phase = CP_CONNECT;
      return 0;
    }
    if (!c->lookup) {
      char port[8];
      (void)snprintf(port, sizeof(port), "%u", 6000 + c->display.display);
      c->lookup = sl_resolve_start(c->display.host, port, NULL);
      if (!c->lookup)
        return sl_capture_fail(c, "cannot resolve X11 DISPLAY");
    }
    result = sl_resolve_step(c->lookup);
    if (result < 0)
      return sl_capture_fail(c, "cannot resolve X11 DISPLAY");
    if (!result)
      return 0;
    c->addresses = sl_resolve_take(c->lookup);
    sl_resolve_destroy(c->lookup);
    c->lookup = NULL;
    c->address = c->addresses->nodes;
    c->phase = CP_CONNECT;
    return 0;
  case CP_CONNECT:
    return sl_capture_connect(c) < 0 ? -1 : 0;
  case CP_SETUP: {
    unsigned char cookie[256];
    size_t cookie_len = 0;
    (void)sl_x11_authority(&c->display, &c->x, cookie, &cookie_len);
    memset(c->tx, 0, sizeof(c->tx));
    c->tx[0] = 'l';
    sl_put16(c->tx + 2, 11);
    if (cookie_len) {
      sl_put16(c->tx + 6, 18);
      sl_put16(c->tx + 8, (uint16_t)cookie_len);
      memcpy(c->tx + 12, "MIT-MAGIC-COOKIE-1", 18);
      memcpy(c->tx + 32, cookie, cookie_len);
    }
    c->tx_len = 12 + (cookie_len ? 20 + ((cookie_len + 3) & ~(size_t)3) : 0);
    c->tx_at = 0;
    c->phase = CP_SETUP_REPLY;
    return 0;
  }
  case CP_SETUP_REPLY:
    result = sl_capture_packet(c, 1);
    if (result <= 0)
      return result;
    p = c->packet;
    if (p[0] != 1 || c->need < 40)
      return sl_capture_fail(c, "X11 server rejected setup; check XAUTHORITY");
    c->x.resource_base = sl_get32(p + 12);
    c->x.resource_mask = sl_get32(p + 16);
    cursor = 40 + ((sl_get16(p + 24) + 3u) & ~3u) + p[29] * 8u;
    for (i = 0; i < p[28]; ++i) {
      unsigned int j, depths;
      if (cursor > c->need || c->need - cursor < 40)
        return sl_capture_fail(c, "invalid X11 setup");
      if (i == c->display.screen) {
        c->x.root = sl_get32(p + cursor);
        break;
      }
      depths = p[cursor + 39];
      cursor += 40;
      for (j = 0; j < depths; ++j) {
        if (cursor > c->need || c->need - cursor < 8)
          return sl_capture_fail(c, "invalid X11 setup");
        cursor += 8 + (size_t)sl_get16(p + cursor + 2) * 24;
      }
    }
    if (!c->x.resource_mask || !c->x.root)
      return sl_capture_fail(c, "invalid X11 screen");
    c->window =
        c->x.resource_base | (c->x.resource_mask & (~c->x.resource_mask + 1));
    c->phase = CP_ATOM;
    return 0;
  case CP_ATOM:
    length = strlen(sl_capture_atoms[c->atom_index]);
    sl_capture_request(c, 16, 8 + ((length + 3) & ~(size_t)3));
    sl_put16(c->tx + 4, (uint16_t)length);
    memcpy(c->tx + 8, sl_capture_atoms[c->atom_index], length);
    c->phase = CP_ATOM_REPLY;
    return 0;
  case CP_ATOM_REPLY:
    result = sl_capture_packet(c, 0);
    if (result <= 0)
      return result;
    if (c->packet[0] != 1 || sl_get16(c->packet + 2) != c->expected)
      return 0;
    c->atoms[c->atom_index] = sl_get32(c->packet + 8);
    if (!c->atoms[c->atom_index])
      return sl_capture_fail(c, "invalid X11 atom");
    c->phase = ++c->atom_index == 5 ? CP_WINDOW : CP_ATOM;
    return 0;
  case CP_WINDOW:
    sl_capture_request(c, 1, 36);
    sl_put32(c->tx + 4, c->window);
    sl_put32(c->tx + 8, c->x.root);
    sl_put16(c->tx + 16, 1);
    sl_put16(c->tx + 18, 1);
    sl_put16(c->tx + 22, 2);
    sl_put32(c->tx + 28, SL_X11_WINDOW_EVENT_MASK);
    sl_put32(c->tx + 32, SL_X11_PROPERTY_CHANGE_MASK);
    c->phase = CP_TIME;
    return 0;
  case CP_TIME:
    sl_capture_request(c, 18, 28);
    sl_put32(c->tx + 4, c->window);
    sl_put32(c->tx + 8, c->atoms[3]);
    sl_put32(c->tx + 12, SL_X11_ATOM_STRING);
    c->tx[16] = 8;
    sl_put32(c->tx + 20, 1);
    c->tx[24] = 'x';
    c->phase = CP_TIME_EVENT;
    return 0;
  case CP_TIME_EVENT:
  case CP_INCR_EVENT:
    result = sl_capture_packet(c, 0);
    if (result <= 0)
      return result;
    p = c->packet;
    if ((p[0] & 127) != SL_X11_PROPERTY_NOTIFY ||
        sl_get32(p + 4) != c->window || sl_get32(p + 8) != c->atoms[3] || p[16])
      return 0;
    if (c->phase == CP_TIME_EVENT) {
      c->timestamp = sl_get32(p + 12);
      c->target = c->atoms[1];
      c->is_png = 1;
      c->phase = CP_SELECT;
    } else {
      c->property_offset = 0;
      c->property_first = 1;
      c->phase = CP_PROPERTY;
    }
    return 0;
  case CP_SELECT:
    sl_capture_request(c, 19, 12);
    sl_put32(c->tx + 4, c->window);
    sl_put32(c->tx + 8, c->atoms[3]);
    c->phase = CP_CONVERT;
    return 0;
  case CP_CONVERT:
    sl_capture_request(c, 24, 24);
    sl_put32(c->tx + 4, c->window);
    sl_put32(c->tx + 8, c->atoms[0]);
    sl_put32(c->tx + 12, c->target);
    sl_put32(c->tx + 16, c->atoms[3]);
    sl_put32(c->tx + 20, c->timestamp);
    c->phase = CP_SELECT_EVENT;
    return 0;
  case CP_SELECT_EVENT:
    result = sl_capture_packet(c, 0);
    if (result <= 0)
      return result;
    p = c->packet;
    if ((p[0] & 127) != SL_X11_SELECTION_NOTIFY ||
        sl_get32(p + 8) != c->window || sl_get32(p + 12) != c->atoms[0] ||
        sl_get32(p + 16) != c->target)
      return 0;
    if (sl_get32(p + 20) != c->atoms[3]) {
      if (!c->is_png)
        return sl_capture_fail(c, "no PNG or JPEG image on the X11 clipboard");
      c->target = c->atoms[2];
      c->is_png = 0;
      c->phase = CP_SELECT;
    } else
      c->phase = CP_BEGIN;
    return 0;
  case CP_BEGIN:
    event->operation = SL_IMAGE_PASTE_BEGIN;
    c->property_first = 1;
    c->phase = CP_PROPERTY;
    return 1;
  case CP_PROPERTY:
    sl_capture_request(c, 20, 24);
    sl_put32(c->tx + 4, c->window);
    sl_put32(c->tx + 8, c->atoms[3]);
    sl_put32(c->tx + 12, 0); /* AnyPropertyType; detect INCR on first reply. */
    sl_put32(c->tx + 16, c->property_offset);
    sl_put32(c->tx + 20, SL_CAPTURE_CHUNK / 4);
    c->phase = CP_PROPERTY_REPLY;
    return 0;
  case CP_PROPERTY_REPLY: {
    uint32_t after, type;
    result = sl_capture_packet(c, 0);
    if (result <= 0)
      return result;
    p = c->packet;
    if (p[0] != 1 || sl_get16(p + 2) != c->expected)
      return 0;
    type = sl_get32(p + 8);
    if (c->property_first && !c->incremental && type == c->atoms[4] &&
        p[1] == 32) {
      if (sl_get32(p + 16) != 1 || c->need != 36)
        return sl_capture_fail(c, "invalid X11 INCR header");
      if ((size_t)sl_get32(p + 32) > c->limit)
        return sl_capture_fail(c, "clipboard image exceeds byte limit");
      c->incremental = 1;
      c->phase = CP_INCR_EVENT;
      sl_capture_request(c, 19, 12);
      sl_put32(c->tx + 4, c->window);
      sl_put32(c->tx + 8, c->atoms[3]);
      return 0;
    }
    c->property_first = 0;
    after = sl_get32(p + 12);
    length = sl_get32(p + 16);
    if (type != c->target || p[1] != 8 || length > SL_CAPTURE_CHUNK ||
        ((length + 3) & ~(size_t)3) != c->need - 32 ||
        (after && (!length || (length & 3))))
      return sl_capture_fail(c, "invalid X11 image property");
    if (length > c->limit - c->total ||
        (size_t)after > c->limit - c->total - length)
      return sl_capture_fail(c, "clipboard image exceeds byte limit");
    if (length && c->signature_len < sizeof(c->signature)) {
      size_t take = sizeof(c->signature) - c->signature_len;
      if (take > length)
        take = length;
      memcpy(c->signature + c->signature_len, p + 32, take);
      c->signature_len += take;
    }
    c->property_offset += (uint32_t)(length / 4);
    if (after)
      c->phase = CP_PROPERTY;
    else {
      c->phase = c->incremental && length ? CP_INCR_EVENT : CP_END;
      sl_capture_request(c, 19, 12);
      sl_put32(c->tx + 4, c->window);
      sl_put32(c->tx + 8, c->atoms[3]);
    }
    if (!length)
      return 0;
    event->operation = SL_IMAGE_PASTE_DATA;
    event->data = p + 32;
    event->length = length;
    event->offset = c->total;
    c->total += length;
    return 1;
  }
  case CP_END: {
    static const unsigned char png[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if ((c->is_png && (c->signature_len < 8 || memcmp(c->signature, png, 8))) ||
        (!c->is_png && (c->signature_len < 3 || c->signature[0] != 255 ||
                        c->signature[1] != 216 || c->signature[2] != 255)))
      return sl_capture_fail(c, "invalid or truncated clipboard image");
    event->operation = SL_IMAGE_PASTE_END;
    event->offset = c->total;
    c->phase = CP_DONE;
    return 1;
  }
  case CP_DONE:
    return 0;
  }
  return sl_capture_fail(c, "invalid clipboard capture state");
}

const char *sl_clipboard_error_text(const sl_clipboard_t *c) {
  return c ? c->error : "out of memory starting clipboard capture";
}

void sl_clipboard_close(sl_clipboard_t *c) {
  if (!c)
    return;
  if (c->x.fd >= 0)
    close(c->x.fd);
  sl_resolve_destroy(c->lookup);
  sl_resolve_free(c->addresses);
  free(c->packet);
  free(c);
}

/* Standalone saving is a sink over the exact same acquisition state machine.
 * Adapter mode never enters this function or creates a staging file. */
int sl_clipboard_save_image(const char *path_template, char **path, char *error,
                            size_t error_size) {
  sl_clipboard_t *c = sl_clipboard_open(SL_IMAGE_MAX_BYTES);
  sl_image_paste_event_t event;
  struct pollfd fds[SL_IMAGE_PASTE_MAX_FDS];
  int count, timeout, result = -1, fd = -1, status;
  int64_t deadline = sl_now_ms() + SL_CLIPBOARD_TIMEOUT_MS, remaining;
  char *saved = NULL;
  *path = NULL;
  while (c && (remaining = deadline - sl_now_ms()) > 0) {
    status = sl_clipboard_next(c, &event);
    if (status < 0)
      break;
    if (status > 0) {
      if (event.operation == SL_IMAGE_PASTE_BEGIN) {
        fd = sl_clipboard_image_file(path_template, c->is_png ? "png" : "jpeg",
                                     &saved);
        if (fd < 0) {
          sl_capture_fail(c, "cannot create clipboard image file");
          break;
        }
      } else if (event.operation == SL_IMAGE_PASTE_DATA) {
        size_t at = 0;
        while (at < event.length) {
          ssize_t n = write(fd, event.data + at, event.length - at);
          if (n < 0 && errno == EINTR)
            continue;
          if (n <= 0)
            goto done;
          at += (size_t)n;
        }
      } else if (event.operation == SL_IMAGE_PASTE_END) {
        if (close(fd) != 0) {
          fd = -1;
          break;
        }
        fd = -1;
        *path = saved;
        saved = NULL;
        result = 0;
        break;
      }
      continue;
    }
    count = sl_clipboard_prepare(c, fds, SL_IMAGE_PASTE_MAX_FDS, &timeout);
    if (count < 0)
      break;
    if (timeout < 0 || timeout > remaining)
      timeout = (int)remaining;
    if (poll(fds, (nfds_t)count, timeout) < 0 && errno != EINTR)
      break;
  }
done:
  if (result && error && error_size)
    (void)snprintf(error, error_size,
                   "clipboard image transfer failed or exceeded 32 MiB: %s",
                   c && c->error[0] ? c->error : "timed out or write failed");
  if (fd >= 0)
    close(fd);
  if (saved)
    sl_clipboard_discard_image(saved, path_template);
  free(saved);
  sl_clipboard_close(c);
  return result;
}
#else
struct sl_clipboard {
  int unused;
};
sl_clipboard_t *sl_clipboard_open(size_t limit) {
  (void)limit;
  return NULL;
}
int sl_clipboard_next(sl_clipboard_t *c, sl_image_paste_event_t *e) {
  (void)c;
  (void)e;
  return -1;
}
int sl_clipboard_prepare(sl_clipboard_t *c, struct pollfd *p, size_t n,
                         int *t) {
  (void)c;
  (void)p;
  (void)n;
  *t = -1;
  return -1;
}
const char *sl_clipboard_error_text(const sl_clipboard_t *c) {
  (void)c;
  return "X11 image paste is only available on Linux";
}
void sl_clipboard_close(sl_clipboard_t *c) { (void)c; }
int sl_clipboard_save_image(const char *p, char **out, char *error, size_t n) {
  (void)p;
  *out = NULL;
  if (error && n)
    (void)snprintf(error, n, "X11 image paste is only available on Linux");
  return -1;
}
#endif
