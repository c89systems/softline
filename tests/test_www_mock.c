#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "web_chat.h"
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "www mock line %d: %s\n", __LINE__, #c);                 \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
typedef struct {
  demo_web_chat_t *web;
  sl_t *sl;
  int submissions;
  struct rlimit file_limit;
  char *large;
  unsigned int large_revision;
} app_t;
static volatile sig_atomic_t done;
static int terminal_fd = -1;
static void stop(int n) {
  (void)n;
  done = 1;
}
static void snapshot(void *context, demo_web_chat_t *web) {
  app_t *app = context;
  unsigned int i;
  char revision[32];
  if (!app->large) {
    size_t index;
    demo_web_queue_begin(web);
    for (index = 0; index < sl_prompt_queue_count(app->sl); ++index) {
      sl_prompt_queue_entry_info_t info;
      char *text = NULL, id[32];
      if (sl_prompt_queue_info(app->sl, index, &info) == SL_OK &&
          sl_prompt_queue_peek(app->sl, index, &text) == SL_OK) {
        (void)snprintf(id, sizeof(id), "%lu", info.id);
        demo_web_queue_entry(web, id, "", text,
                             info.mode == SL_PROMPT_QUEUE_MODE_STEER,
                             info.held);
        sl_free_string(app->sl, text);
      }
    }
    demo_web_queue_end(web);
    return;
  }
  (void)snprintf(revision, sizeof(revision), "large-%u", app->large_revision);
  demo_web_queue_begin(web);
  for (i = 0; i < 64; ++i) {
    char id[32];
    (void)snprintf(id, sizeof(id), "large-item-%u", i);
    demo_web_queue_entry(web, id, revision, app->large, 0, 0);
  }
  demo_web_queue_end(web);
}
static int submit(void *context, const char *text, int steer,
                  sl_queue_request_id_t *request_id) {
  app_t *app = context;
  char id[32];
  if (!strcmp(text, "deferred rejection")) {
    *request_id = 77;
    return SL_OK;
  }
  ++app->submissions;
  if (strcmp(sl_buffer(app->sl), "independent ANSI draft"))
    return SL_ERROR;
  (void)snprintf(id, sizeof(id), "item%d", app->submissions);
  demo_web_message(app->web, "user", text);
  demo_web_response(app->web, "# Reply\n\n", 9);
  demo_web_response(app->web, "\342", 1);
  demo_web_response(app->web, "\202\254\n", 3);
  demo_web_response_end(app->web);
  {
    size_t index = sl_prompt_queue_count(app->sl);
    int result = sl_prompt_queue_append(app->sl, text, 0, NULL);
    return result == SL_OK && steer
               ? sl_prompt_queue_set_mode(app->sl, index,
                                          SL_PROMPT_QUEUE_MODE_STEER)
               : result;
  }
}
static int control(void *context, const char *action, const char *id,
                   const char *revision) {
  app_t *app = context;
  sl_queue_result_t result;
  (void)id;
  (void)revision;
  if (!strcmp(action, "approval-fixture")) {
    size_t index = sl_prompt_queue_count(app->sl);
    if (sl_prompt_queue_append(app->sl, "first approval", 1, NULL) != SL_OK ||
        sl_prompt_queue_set_mode(app->sl, index, SL_PROMPT_QUEUE_MODE_STEER) !=
            SL_OK ||
        sl_prompt_queue_append(app->sl, "later approval", 1, NULL) != SL_OK)
      return SL_ERROR;
    return SL_OK;
  }
  if (!strcmp(action, "large-queue")) {
    app->large = malloc(128u * 1024u + 1);
    if (!app->large)
      return SL_ERROR_NOMEM;
    memset(app->large, 'x', 128u * 1024u);
    app->large[128u * 1024u] = 0;
    ++app->large_revision;
    snapshot(app, app->web);
    ++app->large_revision;
    snapshot(app, app->web);
    return SL_OK;
  }
  if (!strcmp(action, "queue-history")) {
    demo_web_queue_begin(app->web);
    demo_web_queue_entry(app->web, "obsolete-approval", "old", "old approval",
                         0, 1);
    demo_web_queue_end(app->web);
    demo_web_queue_begin(app->web);
    demo_web_queue_end(app->web);
    demo_web_message(app->web, "note", "replay boundary");
    return SL_OK;
  }
  if (!strcmp(action, "limit")) {
    struct rlimit limit;
    if (getrlimit(RLIMIT_FSIZE, &app->file_limit))
      return SL_ERROR;
    limit = app->file_limit;
    limit.rlim_cur = 0;
    signal(SIGXFSZ, SIG_IGN);
    return setrlimit(RLIMIT_FSIZE, &limit) ? SL_ERROR : SL_OK;
  }
  if (!strcmp(action, "unlimit"))
    return setrlimit(RLIMIT_FSIZE, &app->file_limit) ? SL_ERROR : SL_OK;
  if (strcmp(action, "release"))
    return SL_ERROR_INVALID;
  memset(&result, 0, sizeof(result));
  result.request_id = 77;
  result.status = SL_QUEUE_REJECTED;
  result.reason = "fixture rejection";
  demo_web_result(app->web, &result);
  return SL_OK;
}
static int edit(void *context, const char *id, const char *revision,
                const char *text, const char *token,
                sl_prompt_queue_mode_t mode, sl_queue_request_id_t *out) {
  app_t *app = context;
  size_t index;
  (void)revision;
  (void)token;
  (void)mode;
  (void)out;
  if (strcmp(sl_buffer(app->sl), "independent ANSI draft"))
    return SL_ERROR;
  for (index = 0; index < sl_prompt_queue_count(app->sl); ++index) {
    sl_prompt_queue_entry_info_t info;
    char current[32];
    if (sl_prompt_queue_info(app->sl, index, &info) != SL_OK)
      return SL_ERROR;
    snprintf(current, sizeof(current), "%lu", info.id);
    if (!strcmp(current, id))
      return sl_prompt_queue_replace(app->sl, index, text);
  }
  return SL_ERROR_CONFLICT;
}
static void child_server(int output, const char *directory) {
  app_t app;
  demo_web_ops_t ops;
  sl_config_t config;
  unsigned short port;
  int enabled, fd;
  memset(&app, 0, sizeof(app));
  memset(&ops, 0, sizeof(ops));
  ops.submit = submit;
  ops.control = control;
  ops.edit = edit;
  ops.snapshot = snapshot;
  if (chdir(directory) || setenv("SOFTLINE_WEB", "1", 1) ||
      setenv("SOFTLINE_WEB_PORT", "0", 1))
    _exit(2);
  unsetenv("SOFTLINE_CHAT_IMAGE_DIR");
  unsetenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES");
  fd = open("/dev/null", O_RDWR);
  sl_config_init(&config);
  config.input_fd = config.output_fd = fd;
  config.prompt_queue = 1;
  app.sl = sl_create_with_config(&config);
  if (!app.sl || sl_set_buffer(app.sl, "independent ANSI draft") != SL_OK)
    _exit(2);
  app.web = demo_web_open(NULL, &ops, &app, 0, &enabled);
  if (!app.web || !enabled)
    _exit(2);
  port = demo_web_port(app.web);
  if (write(output, &port, sizeof(port)) != (ssize_t)sizeof(port))
    _exit(2);
  close(output);
  signal(SIGTERM, stop);
  signal(SIGPIPE, SIG_IGN);
  while (!done)
    if (demo_www_poll(demo_web_listener(app.web), 10) || demo_web_tick(app.web))
      break;
  demo_web_close(app.web);
  free(app.large);
  sl_destroy(app.sl);
  close(fd);
  _exit(0);
}
static int connect_to(unsigned short port) {
  struct sockaddr_in address;
  struct timeval timeout;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(0x7f000001UL);
  address.sin_port = htons(port);
  timeout.tv_sec = 3;
  timeout.tv_usec = 0;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  if (connect(fd, (struct sockaddr *)&address, sizeof(address))) {
    close(fd);
    return -1;
  }
  return fd;
}
static int write_all(int fd, const void *data, size_t n) {
  const unsigned char *p = data;
  while (n) {
    ssize_t k = write(fd, p, n);
    if (k < 0 && errno == EINTR)
      continue;
    if (k <= 0)
      return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}
static int read_all(int fd, void *data, size_t n) {
  unsigned char *p = data;
  struct timespec time;
  double deadline;
  clock_gettime(CLOCK_MONOTONIC, &time);
  deadline = time.tv_sec * 1000.0 + time.tv_nsec / 1000000.0 + 3000;
  while (n) {
    struct pollfd waits[2];
    ssize_t k;
    double left;
    clock_gettime(CLOCK_MONOTONIC, &time);
    left = deadline - time.tv_sec * 1000.0 - time.tv_nsec / 1000000.0;
    if (left <= 0)
      return -1;
    waits[0].fd = fd;
    waits[0].events = POLLIN;
    waits[0].revents = 0;
    waits[1].fd = terminal_fd;
    waits[1].events = POLLIN;
    waits[1].revents = 0;
    if (poll(waits, 2, (int)left + 1) <= 0)
      return -1;
    if (waits[1].revents & POLLIN) {
      char discard[4096];
      (void)read(terminal_fd, discard, sizeof(discard));
    }
    if (!waits[0].revents)
      continue;
    k = read(fd, p, n);
    if (k < 0 && errno == EINTR)
      continue;
    if (k <= 0)
      return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}
static int ws_open_after(unsigned short port, const char *after) {
  char request[1024];
  int size =
      snprintf(request, sizeof(request),
               "GET /events?after=%s HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: "
               "websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: "
               "13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
               after);
  int fd = connect_to(port);
  char header[1024];
  size_t at = 0, i;
  if (fd < 0 || size < 0 || (size_t)size >= sizeof(request))
    return -1;
  for (i = 0; i < (size_t)size; ++i)
    if (write_all(fd, request + i, 1))
      goto fail;
  while (at + 1 < sizeof(header)) {
    if (read_all(fd, header + at, 1))
      goto fail;
    header[++at] = 0;
    if (strstr(header, "\r\n\r\n"))
      break;
  }
  if (!strstr(header, "101 Switching Protocols") ||
      !strstr(header, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="))
    goto fail;
  return fd;
fail:
  close(fd);
  return -1;
}
static int ws_open(unsigned short port) { return ws_open_after(port, ""); }
static int ws_send(int fd, int opcode, int final, const void *data, size_t n) {
  unsigned char header[8], *copy = malloc(n ? n : 1);
  size_t i, length = n < 126 ? 2 : 4;
  int result;
  if (!copy || n > 65535) {
    free(copy);
    return -1;
  }
  header[0] = (unsigned char)(opcode | (final ? 128 : 0));
  header[1] = (unsigned char)(128 | (n < 126 ? n : 126));
  if (length == 4) {
    header[2] = (unsigned char)(n >> 8);
    header[3] = (unsigned char)n;
  }
  for (i = 0; i < 4; ++i)
    header[length + i] = (unsigned char)(i + 1);
  for (i = 0; i < n; ++i)
    copy[i] = ((const unsigned char *)data)[i] ^ header[length + i % 4];
  result = write_all(fd, header, length + 4) || write_all(fd, copy, n);
  free(copy);
  return result ? -1 : 0;
}
static int ws_text(int fd, const char *text) {
  return ws_send(fd, 1, 1, text, strlen(text));
}
static int ws_read(int fd, char *data, size_t cap, int *opcode) {
  unsigned char h[8];
  uint64_t n;
  size_t i;
  if (read_all(fd, h, 2) || !(h[0] & 128) || (h[1] & 128))
    return -1;
  *opcode = h[0] & 15;
  n = h[1] & 127;
  if (n == 126) {
    if (read_all(fd, h, 2))
      return -1;
    n = (uint64_t)h[0] << 8 | h[1];
  } else if (n == 127) {
    if (read_all(fd, h, 8))
      return -1;
    n = 0;
    for (i = 0; i < 8; ++i)
      n = (n << 8) | h[i];
  }
  if (n >= cap || read_all(fd, data, (size_t)n))
    return -1;
  data[n] = 0;
  return (int)n;
}
static int until(int fd, char *data, size_t cap, const char *needle) {
  int i, opcode;
  for (i = 0; i < 200; ++i) {
    if (ws_read(fd, data, cap, &opcode) < 0)
      return -1;
    if (opcode == 1 && strstr(data, needle))
      return 0;
  }
  return -1;
}
static int json_field(const char *data, const char *name, char *out,
                      size_t cap) {
  char key[64];
  const char *start, *end;
  size_t n;
  snprintf(key, sizeof(key), "\"%s\":\"", name);
  start = strstr(data, key);
  if (!start || !(end = strchr(start + strlen(key), '"')))
    return -1;
  start += strlen(key);
  n = (size_t)(end - start);
  if (n >= cap)
    return -1;
  memcpy(out, start, n);
  out[n] = 0;
  return 0;
}
static int replay_cursors(unsigned short port, const char *epoch, char *data,
                          size_t cap) {
  const char *stale[] = {"2", "previous-host.2"};
  size_t i;
  int fd, opcode, failed = 0;
  char cursor[128];
  for (i = 0; i < 2; ++i) {
    fd = ws_open_after(port, stale[i]);
    if (fd < 0)
      return -1;
    if (ws_read(fd, data, cap, &opcode) < 0 ||
        !strstr(data, "\"type\":\"reset\"") ||
        until(fd, data, cap, "aGVsbG8gd29ybGQ="))
      failed = 1;
    close(fd);
    if (failed)
      return -1;
  }
  snprintf(cursor, sizeof(cursor), "%s.2", epoch);
  fd = ws_open_after(port, cursor);
  if (fd < 0)
    return -1;
  failed = ws_read(fd, data, cap, &opcode) < 0 ||
           !strstr(data, "\"type\":\"session\"");
  close(fd);
  return failed ? -1 : 0;
}
static int queued_text(int fd, char *data, size_t cap, const char *text,
                       char *id, char *revision, int *held, int *steer,
                       const char *accepted) {
  char encoded[256], current[128] = "", rev[128] = "";
  int opcode, found = 0, h = 0, s = 0, frames, complete = 0,
              acknowledged = !accepted, provisional = 0;
  (void)demo_www_base64((const unsigned char *)text, strlen(text), encoded);
  for (frames = 0; frames < 300; ++frames) {
    if (ws_read(fd, data, cap, &opcode) < 0) {
      fprintf(stderr, "waiting for queued text '%s'; last frame: %s\n", text,
              data);
      return -1;
    }
    if (strstr(data, "\"type\":\"queue_item\"")) {
      if (json_field(data, "id", current, sizeof(current)) ||
          json_field(data, "revision", rev, sizeof(rev)))
        return -1;
      h = strstr(data, "\"held\":\"1\"") != NULL;
      s = strstr(data, "\"mode\":\"steer\"") != NULL;
      provisional = strstr(data, "\"mode\":\"pending\"") != NULL;
    }
    if (!provisional && strstr(data, "\"type\":\"queue_text\"") &&
        strstr(data, encoded)) {
      strcpy(id, current);
      strcpy(revision, rev);
      *held = h;
      *steer = s;
      found = 1;
    }
    if (found && strstr(data, "\"type\":\"queue_end\""))
      complete = 1;
    if (accepted && strstr(data, "\"type\":\"accepted\"")) {
      char request[65];
      if (!json_field(data, "request", request, sizeof(request)) &&
          !strcmp(request, accepted))
        acknowledged = 1;
    }
    if (complete && acknowledged)
      return 0;
  }
  return -1;
}
static int edit_turns(int fd, unsigned short port, char *data, size_t cap,
                      int fixture) {
  char id[128], revision[128], updated[128], new_revision[128], request[1024];
  int held, steer, next_held, next_steer;
  if (fixture) {
    if (ws_text(fd, "action=approval-fixture") ||
        until(fd, data, cap, "\"type\":\"accepted\""))
      return -1;
  } else {
    if (ws_text(fd, "action=send&view=edits&request=blocking&request_seq=1&"
                    "text=blocking+turn") ||
        until(fd, data, cap, "\"request\":\"blocking\""))
      return -1;
    if (ws_text(fd, "action=steer&view=edits&request=queued&request_seq=2&text="
                    "editable+queued+text"))
      return -1;
  }
  if (queued_text(fd, data, cap,
                  fixture ? "first approval" : "editable queued text", id,
                  revision, &held, &steer, fixture ? NULL : "queued") ||
      held != fixture || !steer)
    return -1;
  snprintf(request, sizeof(request),
           "action=edit&view=edits&request=saved&request_seq=3&id=%s&revision=%"
           "s&text=revised+queued+text",
           id, revision);
  if (ws_text(fd, request))
    return -1;
  if (queued_text(fd, data, cap, "revised queued text", updated, new_revision,
                  &next_held, &next_steer, "saved") ||
      strcmp(updated, id) || !strcmp(new_revision, revision) ||
      held != next_held || steer != next_steer)
    return -1;
  if (ws_text(fd, request) || until(fd, data, cap, "\"request\":\"saved\"") ||
      !strstr(data, "\"type\":\"accepted\""))
    return -1;
  snprintf(request, sizeof(request),
           "action=edit&view=edits&request=stale&request_seq=4&id=%s&revision=%"
           "s&text=must+not+replace",
           id, revision);
  if (ws_text(fd, request) || until(fd, data, cap, "\"request\":\"stale\"") ||
      !strstr(data, "\"type\":\"error\""))
    return -1;
  if (fixture) {
    char later[128], later_revision[128];
    int observer = ws_open(port), status;
    if (observer < 0)
      return -1;
    status = queued_text(observer, data, cap, "later approval", later,
                         later_revision, &next_held, &next_steer, NULL);
    close(observer);
    if (status)
      return -1;
    snprintf(request, sizeof(request),
             "action=edit&view=edits&request=later&request_seq=5&id=%s&"
             "revision=%s&text=must+not+replace",
             later, later_revision);
    if (ws_text(fd, request) || until(fd, data, cap, "\"request\":\"later\"") ||
        !strstr(data, "\"type\":\"error\""))
      return -1;
  }
  snprintf(request, sizeof(request),
           "action=approve&request=stale-approval&id=%s&revision=%s", id,
           revision);
  if (ws_text(fd, request) ||
      until(fd, data, cap, "\"request\":\"stale-approval\"") ||
      !strstr(data, "\"type\":\"error\""))
    return -1;
  if (!fixture) {
    snprintf(request, sizeof(request),
             "action=edit&view=edits&request=shell-edit&request_seq=6&id=%s&"
             "revision=%s&text=%%2Fshell",
             id, new_revision);
    if (ws_text(fd, request) ||
        until(fd, data, cap, "\"request\":\"shell-edit\"") ||
        !strstr(data, "\"type\":\"error\""))
      return -1;
  }
  return 0;
}
static void inherited_gate(const int *gate) {
  close(gate[1]);
  if (dup2(gate[0], 9) < 0 || setenv("SOFTLINE_CHAT_GATE_FD", "9", 1))
    _exit(2);
  if (gate[0] != 9)
    close(gate[0]);
}
/* Inspect raw producer deltas, carrying only a short suffix across WS frames.
 */
static int chart_turns(int fd, char *data, size_t cap) {
  static const char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned int kinds = 0;
  int turn;
  for (turn = 1; turn <= 9; ++turn) {
    char request[160], id[64] = {0}, tail[96] = {0};
    size_t used = 0;
    int chart = 0, kind = 0, frames;
    snprintf(request, sizeof(request),
             "action=send&request=chart-%d&view=charts&request_seq=%d&text="
             "chart+turn+%d",
             turn, turn, turn);
    if (ws_text(fd, request))
      return -1;
    for (frames = 0; frames < 2000; ++frames) {
      int opcode;
      if (ws_read(fd, data, cap, &opcode) < 0)
        return -1;
      if (opcode != 1)
        continue;
      if (strstr(data, "\"type\":\"error\""))
        return -1;
      if (strstr(data, "\"type\":\"begin\"") &&
          strstr(data, "\"role\":\"assistant\"")) {
        const char *start = strstr(data, "\"id\":\""), *end;
        size_t length;
        if (!start || !(end = strchr(start + 6, '"')))
          return -1;
        length = (size_t)(end - start) + 1;
        if (length >= sizeof(id))
          return -1;
        memcpy(id, start, length);
        id[length] = 0;
      }
      if (strstr(data, "\"type\":\"delta\"") &&
          strstr(data, "\"role\":\"assistant\"")) {
        const char *encoded = strstr(data, "\"data\":\"");
        unsigned int value = 0, bits = 0;
        if (!encoded)
          return -1;
        for (encoded += 8; *encoded && *encoded != '"' && *encoded != '=';
             ++encoded) {
          const char *digit = strchr(alphabet, *encoded);
          if (!digit)
            return -1;
          value = (value << 6) | (unsigned int)(digit - alphabet);
          bits += 6;
          if (bits >= 8) {
            bits -= 8;
            if (used + 1 == sizeof(tail))
              memmove(tail, tail + 1, --used);
            tail[used++] = (char)((value >> bits) & 255);
            tail[used] = 0;
            if (strstr(tail, "mdf-"))
              chart = 1;
            if (strstr(tail, "mdf-bar-chart") ||
                strstr(tail, "mdf-horizontal-bar-chart"))
              kind = 1;
            if (strstr(tail, "mdf-vertical-bar-chart"))
              kind = 2;
            if (strstr(tail, "mdf-tile-chart"))
              kind = 4;
          }
        }
      }
      if (id[0] && strstr(data, "\"type\":\"end\"") && strstr(data, id))
        break;
    }
    if (frames == 2000 || chart != (turn % 3 == 0) || (chart && !kind))
      return -1;
    kinds |= (unsigned int)kind;
  }
  return kinds == 7 ? 0 : -1;
}
static int completion_file(const char *root, const char *name, int directory) {
  char path[8192];
  int fd;
  if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path))
    return -1;
  if (directory)
    return mkdir(path, 0700);
  fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  return fd < 0 ? -1 : close(fd);
}
static void remove_tree(const char *root) {
  DIR *d = opendir(root);
  struct dirent *entry;
  if (!d)
    return;
  while ((entry = readdir(d)) != NULL) {
    char path[8192];
    struct stat info;
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >=
            (int)sizeof(path) ||
        lstat(path, &info))
      continue;
    if (S_ISDIR(info.st_mode))
      remove_tree(path);
    else
      unlink(path);
  }
  closedir(d);
  rmdir(root);
}
int main(int argc, char **argv) {
  char root[4096] = {0}, *data = malloc(SL_WWW_FRAME_MAX + 1),
       *http = malloc(512 * 1024), path[4096] = {0};
  int pipes[2] = {-1, -1}, fd = -1, one = -1, two = -1, status, opcode,
      failed = 1;
  pid_t child = -1, owner = -1;
  int slave = -1, dual, charts, edits, gate[2] = {-1, -1};
  char epoch[96];
  const char *mode = argc >= 4 ? argv[3] : "";
  unsigned short queue_port = 0;
  unsigned short port;
  size_t used;
  static const unsigned char png[16] = {137, 80,  78, 71, 13, 10, 26, 10,
                                        0,   255, 1,  2,  3,  4,  5,  6};
  (void)signal(SIGPIPE, SIG_IGN);
  CHECK((argc == 2 || argc == 4 || argc == 5) && data && http);
  charts = !strncmp(mode, "charts-", 7);
  edits = !strncmp(mode, "edits-", 6);
  if (charts)
    mode += 7;
  if (edits)
    mode += 6;
  dual = !strcmp(mode, "dual") || !strcmp(mode, "remote");
  CHECK(snprintf(root, sizeof(root), "%s/www-mock-XXXXXX", argv[1]) <
            (int)sizeof(root) &&
        mkdtemp(root));
  CHECK(!completion_file(root, "README.md", 0));
  CHECK(!completion_file(root, "examples", 1));
  CHECK(!completion_file(root, "examples/chat.c", 0));
  CHECK(!completion_file(root, "examples/other.c", 0));
  CHECK(!completion_file(root, "space dir", 1));
  CHECK(!completion_file(root, "space dir/file.txt", 0));
  CHECK(!completion_file(root, "caf\303\251.c", 0));
  CHECK(!completion_file(root, "icon@2x.png", 0));
  CHECK(!completion_file(root, "@literal.txt", 0));
  CHECK(!completion_file(root, "many", 1));
  {
    size_t i;
    for (i = 0; i < 300; ++i) {
      char name[64];
      snprintf(name, sizeof(name), "many/item%03lu", (unsigned long)i);
      CHECK(!completion_file(root, name, 0));
    }
  }
  CHECK(pipe(pipes) == 0);
  if (edits)
    CHECK(pipe(gate) == 0);
  if (argc == 5) {
    char line[1024];
    unsigned int parsed = 0;
    size_t at = 0;
    owner = fork();
    CHECK(owner >= 0);
    if (!owner) {
      if (chdir(root) || dup2(pipes[1], STDOUT_FILENO) < 0)
        _exit(2);
      close(pipes[0]);
      setenv("SOFTLINE_WEB", "0", 1);
      if (edits)
        inherited_gate(gate);
      execl(argv[4], argv[4], "0", (char *)NULL);
      _exit(2);
    }
    close(pipes[1]);
    pipes[1] = -1;
    while (!parsed) {
      char c;
      CHECK(read_all(pipes[0], &c, 1) == 0);
      if (c == '\n') {
        line[at] = 0;
        (void)sscanf(line, "LISTEN 127.0.0.1 %u", &parsed);
        at = 0;
      } else if (at + 1 < sizeof(line))
        line[at++] = c;
    }
    CHECK(parsed <= 65535);
    queue_port = (unsigned short)parsed;
    close(pipes[0]);
    pipes[0] = -1;
    CHECK(pipe(pipes) == 0);
  }
  if (dual)
    CHECK(openpty(&terminal_fd, &slave, NULL, NULL, NULL) == 0);
  if (dual) {
    sl_config_t config;
    sl_t *seed;
    int null_fd = open("/dev/null", O_RDWR);
    CHECK(null_fd >= 0);
    sl_config_init(&config);
    config.input_fd = config.output_fd = null_fd;
    seed = sl_create_with_config(&config);
    CHECK(seed &&
          sl_history_open(seed, "softline.examples.chat", root) == SL_OK &&
          sl_history_add(seed, "history seed") == SL_OK);
    sl_destroy(seed);
    close(null_fd);
  }
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(pipes[0]);
    if (argc >= 4) {
      if (chdir(root) || dup2(pipes[1], STDOUT_FILENO) < 0 ||
          dup2(pipes[1], STDERR_FILENO) < 0 || setenv("SOFTLINE_WEB", "1", 1) ||
          setenv("SOFTLINE_WEB_PORT", "0", 1) ||
          setenv("SOFTLINE_HISTORY_DIR", root, 1) ||
          setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "0", 1) ||
          setenv("SOFTLINE_CHAT_CHAR_MS", "0", 1))
        _exit(2);
      if (dual) {
        close(terminal_fd);
        if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0)
          _exit(2);
        close(slave);
      }
      unsetenv("SOFTLINE_CHAT_IMAGE_DIR");
      unsetenv("SOFTLINE_CHAT_IMAGE_MAX_BYTES");
      if (edits)
        inherited_gate(gate);
      if (argc == 5) {
        char number[16];
        (void)snprintf(number, sizeof(number), "%u", (unsigned int)queue_port);
        execl(argv[2], argv[2], "127.0.0.1", number, (char *)NULL);
      } else if (dual)
        execl(argv[2], argv[2], (char *)NULL);
      else
        execl(argv[2], argv[2], !strcmp(mode, "owner") ? "0" : "--web-only",
              (char *)NULL);
      _exit(2);
    }
    child_server(pipes[1], root);
  }
  close(pipes[1]);
  pipes[1] = -1;
  if (slave >= 0) {
    close(slave);
    slave = -1;
  }
  if (argc >= 4) {
    char line[1024];
    unsigned int parsed = 0;
    size_t at = 0;
    while (!parsed) {
      char c;
      CHECK(read_all(pipes[0], &c, 1) == 0);
      if (c == '\n') {
        line[at] = 0;
        (void)sscanf(line, "WEB http://127.0.0.1:%u", &parsed);
        at = 0;
      } else if (at + 1 < sizeof(line))
        line[at++] = c;
    }
    CHECK(parsed <= 65535);
    port = (unsigned short)parsed;
  } else
    CHECK(read_all(pipes[0], &port, sizeof(port)) == 0);
  close(pipes[0]);
  pipes[0] = -1;
  fd = connect_to(port);
  CHECK(fd >= 0);
  CHECK(write_all(fd, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", 35) == 0);
  used = 0;
  for (;;) {
    ssize_t n = read(fd, http + used, 512 * 1024 - used - 1);
    if (n < 0 && errno == EINTR)
      continue;
    CHECK(n >= 0);
    if (!n)
      break;
    used += (size_t)n;
    CHECK(used < 512 * 1024 - 1);
  }
  http[used] = 0;
  CHECK(strstr(http, "200 OK") && strstr(http, "Welcome to softline") &&
        strstr(http, "id=\"send\""));
  close(fd);
  fd = -1;
  one = ws_open(port);
  two = ws_open(port);
  CHECK(one >= 0 && two >= 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"session\"") == 0);
  CHECK(!json_field(data, "id", epoch, sizeof(epoch)));
  {
    static const char *const triggers[] = {"/", "h", "v", "n", "@"};
    size_t i;
    for (i = 0; i < sizeof(triggers) / sizeof(triggers[0]); ++i) {
      char expected[32];
      CHECK(until(one, data, SL_WWW_FRAME_MAX + 1,
                  "\"type\":\"completion_provider\"") == 0);
      snprintf(expected, sizeof(expected), "\"trigger\":\"%s\"", triggers[i]);
      CHECK(strstr(data, expected));
      CHECK(strstr(data, i < 2 ? "\"scope\":\"start\"" : "\"scope\":\"word\""));
      CHECK(strstr(data, i == 4 ? "\"strip\":\"1\"" : "\"strip\":\"0\""));
      CHECK(strstr(data,
                   i == 0 || i == 4 ? "\"expand\":\"1\"" : "\"expand\":\"0\""));
    }
  }
  if (charts) {
    CHECK(!chart_turns(one, data, SL_WWW_FRAME_MAX + 1));
    failed = 0;
    goto cleanup;
  }
  if (edits) {
    int edited = edit_turns(one, port, data, SL_WWW_FRAME_MAX + 1, 0);
    if (edited)
      fprintf(stderr, "edit test last frame: %s\n", data);
    CHECK(!edited);
    failed = 0;
    goto cleanup;
  }
  if (dual)
    CHECK(write_all(terminal_fd, "independent ANSI draft\022", 23) == 0);
  CHECK(ws_send(one, 1, 0, "action=send&request=first&", 26) == 0);
  {
    const char *tail = "text=hello+world&view=test&request_seq=1";
    CHECK(ws_send(one, 0, 1, tail, strlen(tail)) == 0);
  }
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"accepted\"") == 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "aGVsbG8gd29ybGQ=") == 0);
  CHECK(!replay_cursors(port, epoch, data, SL_WWW_FRAME_MAX + 1));
  CHECK(ws_send(one, 9, 1, "p", 1) == 0);
  do {
    CHECK(ws_read(one, data, SL_WWW_FRAME_MAX + 1, &opcode) >= 0);
  } while (opcode != 10);
  CHECK(!strcmp(data, "p"));
  /* A repeated request is acknowledged without admitting a second prompt. */
  CHECK(ws_text(one, "action=send&request=first&text=hello+world&view=test&"
                     "request_seq=1") == 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"accepted\"") == 0);
  CHECK(
      ws_text(
          one,
          "action=send&request=first&text=different&view=test&request_seq=1") ==
      0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"error\"") == 0);
  CHECK(ws_text(one,
                "action=send&request=invalid&text=&view=test&request_seq=2") ==
        0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"error\"") == 0 &&
        strstr(data, "\"request\":\"invalid\""));
  if (argc == 2) {
    CHECK(!edit_turns(one, port, data, SL_WWW_FRAME_MAX + 1, 1));
    CHECK(ws_text(one, "action=send&request=deferred&text=deferred+rejection&"
                       "view=test&request_seq=2") == 0);
    CHECK(ws_send(one, 9, 1, "barrier", 7) == 0);
    do {
      CHECK(ws_read(one, data, SL_WWW_FRAME_MAX + 1, &opcode) >= 0);
      CHECK(!strstr(data, "\"request\":\"deferred\""));
    } while (opcode != 10);
    CHECK(ws_text(one, "action=release") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "fixture rejection") == 0 &&
          strstr(data, "\"request\":\"deferred\""));
  }
  CHECK(ws_text(one, "action=complete&request=7&text=%2Fst&cursor=3&all=0") ==
        0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"value\":\"/status\"") == 0);
  {
    static const struct {
      const char *query, *first, *second;
    } batches[] = {
        {"text=%40REA&cursor=4&start=0&end=4&resolve=%40", "@README.md",
         "@examples/"},
        {"text=%40examples%2Fch&cursor=12&start=0&end=12&resolve=%"
         "40examples%2F",
         "@examples/chat.c", "@examples/other.c"},
        {"text=%2Fapprove&cursor=8&start=0&end=8&resolve=%2Fapprove",
         "/approve on", "/approve off"},
        {"text=%40many%2Fitem299&cursor=13&start=0&end=13&resolve=%40many%2F",
         "@many/item000", "@many/item299"}};
    size_t i;
    for (i = 0; i < sizeof(batches) / sizeof(batches[0]); ++i) {
      char query[512], ticket[64], expected[256];
      int first = 0, second = 0;
      snprintf(query, sizeof(query), "action=complete&request=snapshot-%lu&%s",
               (unsigned long)i, batches[i].query);
      snprintf(ticket, sizeof(ticket), "\"request\":\"snapshot-%lu\"",
               (unsigned long)i);
      CHECK(ws_text(one, query) == 0);
      for (;;) {
        CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, ticket) == 0);
        if (strstr(data, "\"type\":\"complete_end\"")) {
          CHECK(strstr(data, "\"status\":\"ok\""));
          break;
        }
        snprintf(expected, sizeof(expected), "\"candidate\":\"%s\"",
                 batches[i].first);
        if (strstr(data, expected))
          first = 1;
        snprintf(expected, sizeof(expected), "\"candidate\":\"%s\"",
                 batches[i].second);
        if (strstr(data, expected))
          second = 1;
      }
      CHECK(first && second);
    }
  }
  CHECK(ws_text(
            one,
            "action=complete&request=8&text=%E2%82%AC+%2Fst&cursor=7&all=0") ==
        0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"value\":\"/status\"") == 0 &&
        strstr(data, "\"start\":\"4\""));
  CHECK(
      ws_text(one, "action=complete&request=9&text=%E2%82%AC&cursor=1&all=0") ==
      0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"error\"") == 0);
  {
    static const struct {
      const char *query, *value, *start, *end;
      int continuation;
    } cases[] = {
        {"text=%40REA&cursor=4", "README.md", "0", "4", 0},
        {"text=%40exam&cursor=5", "@examples/", "0", "5", 1},
        {"text=%40examples%2Fch&cursor=12", "examples/chat.c", "0", "12", 0},
        {"text=%E2%82%AC+%40examples%2Fch+tail&cursor=16", "examples/chat.c",
         "4", "16", 0},
        {"text=%40caf%C3%A9&cursor=6", "caf\303\251.c", "0", "6", 0},
        {"text=%40icon%40&cursor=6", "icon@2x.png", "0", "6", 0},
        {"text=%40%40li&cursor=4", "@literal.txt", "0", "4", 0},
        {"text=%40space+dir%2Ffi&cursor=13&start=0&end=13",
         "space dir/file.txt", "0", "13", 0}};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      char query[512], ticket[64], expected[256];
      snprintf(query, sizeof(query),
               "action=complete&request=file-%lu&all=0&%s", (unsigned long)i,
               cases[i].query);
      snprintf(ticket, sizeof(ticket), "\"request\":\"file-%lu\"",
               (unsigned long)i);
      CHECK(ws_text(one, query) == 0);
      CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, ticket) == 0);
      snprintf(expected, sizeof(expected), "\"value\":\"%s\"", cases[i].value);
      CHECK(strstr(data, expected));
      snprintf(expected, sizeof(expected), "\"start\":\"%s\"", cases[i].start);
      CHECK(strstr(data, expected));
      snprintf(expected, sizeof(expected), "\"end\":\"%s\"", cases[i].end);
      CHECK(strstr(data, expected));
      CHECK(strstr(data, cases[i].continuation ? "\"continue\":\"1\""
                                               : "\"continue\":\"0\""));
    }
    CHECK(ws_text(one, "action=complete&request=no-file&text=%40examples%"
                       "2Fmissing&cursor=17") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"request\":\"no-file\"") ==
              0 &&
          !strstr(data, "\"value\""));
    {
      static const char *bad_ranges[] = {
          "text=%40REA&cursor=4&end=4",
          "text=%40REA&cursor=4&start=0&end=99",
          "text=literal%40REA&cursor=11&start=7&end=11",
          "text=%40REA&cursor=4&start=99&end=4",
          "text=%40REA&cursor=4&start=0&end=3",
          "text=%40REA+%E2%82%AC&cursor=4&start=0&end=6"};
      size_t bad;
      for (bad = 0; bad < sizeof(bad_ranges) / sizeof(bad_ranges[0]); ++bad) {
        char query[256];
        snprintf(query, sizeof(query), "action=complete&request=bad-range&%s",
                 bad_ranges[bad]);
        CHECK(ws_text(one, query) == 0);
        CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"error\"") ==
              0);
      }
    }
    CHECK(ws_text(one, "action=send&request=file-send&text=README.md&view="
                       "completion&request_seq=1") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"request\":\"file-send\"") ==
              0 &&
          strstr(data, "\"type\":\"accepted\""));
    CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "UkVBRE1FLm1k") == 0);
    CHECK(ws_text(one, "action=steer&request=file-steer&text=examples%2Fchat.c&"
                       "view=completion&request_seq=2") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1,
                "\"request\":\"file-steer\"") == 0 &&
          strstr(data, "\"type\":\"accepted\""));
    CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "ZXhhbXBsZXMvY2hhdC5j") == 0);
  }
  CHECK(ws_text(one, "action=image_begin&id=upload&media_type=image%2Fpng&"
                     "filename=x.png&size=16") == 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  CHECK(ws_send(one, 2, 1, png, 1) == 0 &&
        until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
            0);
  CHECK(ws_send(one, 2, 1, png + 1, 15) == 0 &&
        until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
            0);
  CHECK(ws_text(one, "action=image_end&id=upload") == 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_done\"") == 0);
  {
    char *p = strstr(data, "\"path\":\"");
    char *end;
    CHECK(p);
    p += 8;
    end = strchr(p, '"');
    CHECK(end && (size_t)(end - p) < sizeof(path));
    memcpy(path, p, (size_t)(end - p));
    path[end - p] = 0;
  }
  fd = open(path, O_RDONLY);
  CHECK(fd >= 0 && read_all(fd, http, sizeof(png)) == 0 &&
        !memcmp(http, png, sizeof(png)));
  close(fd);
  fd = -1;
  CHECK(ws_text(one, "action=image_begin&id=cancel&media_type=image%2Fpng&"
                     "filename=x.png&size=16") == 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  CHECK(ws_text(one, "action=image_cancel&id=cancel") == 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_error\"") ==
        0);
  CHECK(
      ws_text(
          one,
          "action=image_begin&id=large&media_type=image%2Fpng&size=33554433") ==
      0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_error\"") ==
        0);
  if (dual) {
    char encoded[128];
    (void)demo_www_base64((const unsigned char *)"independent ANSI draft", 22,
                          encoded);
    CHECK(write_all(terminal_fd, "\007\r", 2) == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, encoded) == 0);
  }
  /* A second independent source has a fresh backend wire identity. */
  if (argc == 2) {
    CHECK(ws_text(one, "action=limit") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"accepted\"") == 0);
    CHECK(ws_text(one, "action=image_begin&id=disk-failure&media_type=image%"
                       "2Fpng&size=16") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
          0);
    CHECK(ws_send(one, 2, 1, png, sizeof(png)) == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_error\"") ==
          0);
    CHECK(ws_text(one, "action=unlimit") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"accepted\"") == 0);
    CHECK(
        ws_text(one,
                "action=image_begin&id=retry&media_type=image%2Fpng&size=16") ==
        0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
          0);
    CHECK(ws_text(one, "action=image_cancel&id=retry") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_error\"") ==
          0);
  }
  CHECK(ws_text(two, "action=image_begin&id=jpeg&media_type=image%2Fjpeg&"
                     "filename=x.jpg&size=3") == 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  CHECK(ws_send(two, 2, 1, "\377\330\377", 3) == 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  CHECK(ws_text(two, "action=image_end&id=jpeg") == 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_done\"") == 0);
  CHECK(
      ws_text(two,
              "action=image_begin&id=invalid&media_type=image%2Fpng&size=16") ==
      0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  CHECK(ws_send(two, 2, 1, "bad", 3) == 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_error\"") ==
        0);
  /* Reconnect replays retained text; a duplicate offer remains one admission.
   */
  if (argc == 2) {
    CHECK(ws_text(one, "action=queue-history") == 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"accepted\"") == 0);
  }
  close(two);
  two = -1;
  two = ws_open(port);
  CHECK(two >= 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "aGVsbG8gd29ybGQ=") == 0);
  if (argc == 2) {
    char marker[128];
    int iteration;
    (void)demo_www_base64((const unsigned char *)"replay boundary",
                          strlen("replay boundary"), marker);
    for (iteration = 0; iteration < 200; ++iteration) {
      CHECK(ws_read(two, data, SL_WWW_FRAME_MAX + 1, &opcode) >= 0);
      CHECK(!strstr(data, "obsolete-approval"));
      if (strstr(data, marker))
        break;
    }
    CHECK(iteration < 200);
  }
  CHECK(ws_text(two, "action=send&request=first&text=hello+world&view=test&"
                     "request_seq=1") == 0);
  CHECK(until(two, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"accepted\"") == 0);
  /* Disconnect while DATA is outstanding, then reuse another source. */
  CHECK(
      ws_text(
          one,
          "action=image_begin&id=disconnect&media_type=image%2Fpng&size=16") ==
      0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  CHECK(ws_send(one, 2, 1, png, 1) == 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_credit\"") ==
        0);
  close(one);
  one = -1;
  one = ws_open(port);
  CHECK(one >= 0);
  CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"session\"") == 0);
  {
    int iteration;
    for (iteration = 0; iteration < 8; ++iteration) {
      char begin[160], end[80], expected[80];
      (void)snprintf(
          begin, sizeof(begin),
          "action=image_begin&id=sequence-%d&media_type=image%%2Fjpeg&size=3",
          iteration);
      (void)snprintf(end, sizeof(end), "action=image_end&id=sequence-%d",
                     iteration);
      (void)snprintf(expected, sizeof(expected), "\"id\":\"sequence-%d\"",
                     iteration);
      CHECK(ws_text(one, begin) == 0);
      CHECK(until(one, data, SL_WWW_FRAME_MAX + 1,
                  "\"type\":\"image_credit\"") == 0);
      CHECK(ws_send(one, 2, 1, "\377\330\377", 3) == 0);
      CHECK(until(one, data, SL_WWW_FRAME_MAX + 1,
                  "\"type\":\"image_credit\"") == 0);
      CHECK(ws_text(one, end) == 0);
      CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"image_done\"") ==
                0 &&
            strstr(data, expected));
    }
  }
  close(one);
  one = -1;
  close(two);
  two = -1;
  if (argc == 2) {
    one = ws_open(port);
    CHECK(one >= 0);
    CHECK(until(one, data, SL_WWW_FRAME_MAX + 1, "\"type\":\"session\"") == 0);
    CHECK(ws_text(one, "action=large-queue") == 0);
    while (ws_read(one, data, SL_WWW_FRAME_MAX + 1, &opcode) >= 0) {
    }
    close(one);
    one = ws_open(port);
    CHECK(one >= 0);
    {
      int iteration, items = 0;
      for (iteration = 0; iteration < 5000; ++iteration) {
        CHECK(ws_read(one, data, SL_WWW_FRAME_MAX + 1, &opcode) >= 0);
        if (strstr(data, "\"type\":\"queue_item\""))
          ++items;
        if (strstr(data, "\"type\":\"queue_end\""))
          break;
      }
      CHECK(iteration < 5000 && items == 64);
    }
    close(one);
    one = -1;
  }
  failed = 0;
cleanup:
  if (gate[0] >= 0)
    close(gate[0]);
  if (gate[1] >= 0)
    close(gate[1]);
  if (fd >= 0)
    close(fd);
  if (one >= 0)
    close(one);
  if (two >= 0)
    close(two);
  if (pipes[0] >= 0)
    close(pipes[0]);
  if (pipes[1] >= 0)
    close(pipes[1]);
  if (child > 0) {
    int exited = 0, tries;
    if (dual && terminal_fd >= 0) {
      (void)write_all(terminal_fd, "\033q", 2);
      for (tries = 0; tries < 100; ++tries) {
        struct pollfd drain;
        char discard[4096];
        if (waitpid(child, &status, WNOHANG) == child) {
          exited = 1;
          break;
        }
        drain.fd = terminal_fd;
        drain.events = POLLIN;
        drain.revents = 0;
        if (poll(&drain, 1, 20) > 0 && (drain.revents & POLLIN))
          (void)read(terminal_fd, discard, sizeof(discard));
      }
    }
    if (!exited) {
      kill(child, SIGTERM);
      waitpid(child, &status, 0);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
      failed = 1;
  }
  if (owner > 0) {
    kill(owner, SIGTERM);
    waitpid(owner, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status))
      failed = 1;
  }
  if (slave >= 0)
    close(slave);
  if (terminal_fd >= 0)
    close(terminal_fd);
  if (*root)
    remove_tree(root);
  free(data);
  free(http);
  return failed;
}
