/* Real WebSocket/PTY checks using the owning test's bounded wire helpers. */
typedef struct terminal_client {
  int fd, ready, credit, exited, status;
  char tail[2048];
  size_t used, total;
} terminal_client_t;
static void terminal_capture(terminal_client_t *t, const void *bytes,
                             size_t length) {
  const char *data = bytes;
  size_t drop;
  t->total += length;
  if (length >= sizeof(t->tail)) {
    memcpy(t->tail, data + length - sizeof(t->tail), sizeof(t->tail));
    t->used = sizeof(t->tail);
  } else {
    drop = t->used + length > sizeof(t->tail)
               ? t->used + length - sizeof(t->tail)
               : 0;
    if (drop) {
      memmove(t->tail, t->tail + drop, t->used - drop);
      t->used -= drop;
    }
    memcpy(t->tail + t->used, data, length);
    t->used += length;
  }
}
static int terminal_next(terminal_client_t *t, char *data, size_t cap,
                         int ack) {
  int opcode, n = ws_read(t->fd, data, cap, &opcode);
  if (n < 0 || opcode == 8)
    return -1;
  if (opcode == 2) {
    size_t length = (size_t)n;
    char control[64];
    if (!length || length > 4096)
      return -1;
    terminal_capture(t, data, length);
    if (ack) {
      snprintf(control, sizeof(control), "action=ack&bytes=%lu",
               (unsigned long)length);
      if (ws_text(t->fd, control))
        return -1;
    }
  } else if (opcode == 1) {
    if (strstr(data, "\"type\":\"ready\""))
      t->ready = 1;
    else if (strstr(data, "\"type\":\"input_credit\""))
      t->credit = 1;
    else if (strstr(data, "\"type\":\"exit\"")) {
      char value[32];
      if (json_field(data, "status", value, sizeof(value)))
        return -1;
      t->exited = 1;
      t->status = atoi(value);
    } else {
      fprintf(stderr, "unexpected terminal event: %s\n", data);
      return -1;
    }
  }
  return 0;
}
static int terminal_wait(terminal_client_t *t, char *data, size_t cap,
                         const void *needle, size_t length) {
  int frames;
  for (frames = 0; frames < 500; ++frames) {
    size_t at;
    for (at = 0; at + length <= t->used; ++at)
      if (!memcmp(t->tail + at, needle, length))
        return 0;
    if (terminal_next(t, data, cap, 1))
      return -1;
  }
  return -1;
}
static int terminal_open(terminal_client_t *t, unsigned short port, char *data,
                         size_t cap) {
  memset(t, 0, sizeof(*t));
  t->fd = ws_open_route(port, "/terminal/events", "");
  if (t->fd < 0 || ws_text(t->fd, "action=open&rows=24&columns=80"))
    return -1;
  while (!t->ready || !t->credit)
    if (terminal_next(t, data, cap, 1))
      return -1;
  return 0;
}
static int terminal_input(terminal_client_t *t, char *data, size_t cap,
                          const char *input, size_t length) {
  while (!t->credit)
    if (terminal_next(t, data, cap, 1))
      return -1;
  t->credit = 0;
  return ws_send(t->fd, 2, 1, input, length);
}
static int native_terminal_flush(demo_link_t *link) {
  int tries;
  for (tries = 0; tries < 20 && link->count; ++tries) {
    struct pollfd wait;
    if (demo_link_flush(link))
      return -1;
    if (!link->count)
      break;
    wait.fd = link->fd;
    wait.events = POLLOUT;
    wait.revents = 0;
    if (poll(&wait, 1, 100) <= 0)
      return -1;
  }
  return link->count ? -1 : 0;
}
static int native_terminal_next(demo_link_t *link, terminal_client_t *t) {
  demo_packet_t packet = {0};
  unsigned long kind;
  int status, failed = 0;
  while (!(status = demo_link_read(link, &packet))) {
    struct pollfd wait;
    wait.fd = link->fd;
    wait.events = POLLIN;
    wait.revents = 0;
    if (poll(&wait, 1, 2000) <= 0)
      return -1;
  }
  if (status < 0)
    return -1;
  kind = demo_number(&packet);
  if (kind == DEMO_PTY_READY)
    t->ready = 1;
  else if (kind == DEMO_PTY_DATA) {
    size_t length;
    const char *bytes = demo_bytes(&packet, &length);
    if (!packet.failed)
      terminal_capture(t, bytes, length);
  } else if (kind == DEMO_PTY_EXIT) {
    t->status = (int)demo_number(&packet);
    t->exited = 1;
  } else
    failed = 1;
  failed |= packet.failed || packet.offset != packet.length;
  demo_packet_free(&packet);
  return failed ? -1 : 0;
}
static int native_terminal_wait(demo_link_t *link, terminal_client_t *t,
                                const char *needle) {
  int frames;
  size_t length = strlen(needle);
  for (frames = 0; frames < 500; ++frames) {
    size_t at;
    for (at = 0; at + length <= t->used; ++at)
      if (!memcmp(t->tail + at, needle, length))
        return 0;
    if (native_terminal_next(link, t))
      return -1;
  }
  return -1;
}
static int native_terminal_input(demo_link_t *link, const char *text) {
  demo_packet_t packet = {0};
  demo_put_number(&packet, DEMO_PTY_DATA);
  demo_put_bytes(&packet, text, strlen(text));
  if (demo_link_queue(link, &packet)) {
    demo_packet_free(&packet);
    return -1;
  }
  return native_terminal_flush(link);
}
static int terminal_state_wait(int peer, char *data, size_t cap, size_t active,
                               int disabled) {
  char expected[96];
  (void)snprintf(expected, sizeof(expected),
                 "\"active\":\"%lu\",\"disable_prompt\":\"%d\"",
                 (unsigned long)active, disabled);
  return until(peer, data, cap, expected);
}
static int terminal_roundtrip(unsigned short port, unsigned short queue_port,
                              int chat, const char *root, int policy,
                              char *data, size_t cap) {
  terminal_client_t *t = calloc(1, sizeof(*t));
  terminal_client_t *native = calloc(1, sizeof(*native));
  demo_link_t native_link;
  const char *command;
  const unsigned char raw[] = {0xe2, 0x82, 0xac, 255, 0, 'X'};
  int failed = 1, frames, observer = -1;
  size_t before;
  struct pollfd wait;
  demo_link_init(&native_link, -1);
  if (t)
    t->fd = -1;
#define TERMINAL_CHECK(c)                                                      \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "terminal line %d: %s\n", __LINE__, #c);                 \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)
#define TERMINAL_STATE(n)                                                      \
  do {                                                                         \
    TERMINAL_CHECK(!terminal_state_wait(chat, data, cap, n, policy));          \
    TERMINAL_CHECK(!terminal_state_wait(observer, data, cap, n, policy));      \
  } while (0)
  TERMINAL_CHECK(t && native);
  TERMINAL_CHECK(!terminal_open(t, port, data, cap));
  native_link.fd = connect_to(queue_port);
  TERMINAL_CHECK(native_link.fd >= 0 && !demo_nonblock(native_link.fd));
  {
    demo_packet_t open = {0};
    demo_put_number(&open, DEMO_PTY_OPEN);
    demo_put_number(&open, 31);
    demo_put_number(&open, 93);
    demo_put_string(&open, "xterm-256color");
    TERMINAL_CHECK(!demo_link_queue(&native_link, &open) &&
                   !native_terminal_flush(&native_link));
  }
  while (!native->ready)
    TERMINAL_CHECK(!native_terminal_next(&native_link, native));
  TERMINAL_CHECK(!native_terminal_input(
      &native_link,
      "stty -echo; export SOFTLINE_WEB_TERMINAL_ISOLATION=native; printf "
      "'NATIVE-%s\\n' READY; stty size\n"));
  TERMINAL_CHECK(!native_terminal_wait(&native_link, native, "NATIVE-READY"));
  TERMINAL_CHECK(!native_terminal_wait(&native_link, native, "31 93"));
  observer = ws_open(port);
  TERMINAL_CHECK(observer >= 0);
  TERMINAL_STATE(2);
  command = "stty -echo; export SOFTLINE_WEB_TERMINAL_ISOLATION=first; printf "
            "'\\124\\105\\122\\115\\055\\122\\105\\101\\104\\131\\n'; pwd; "
            "stty size\n";
  TERMINAL_CHECK(!terminal_input(t, data, cap, command, strlen(command)));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, "TERM-READY", 10));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, root, strlen(root)));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, "24 80", 5));
  TERMINAL_CHECK(!ws_text(t->fd, "action=resize&rows=37&columns=101"));
  command = "stty size; printf '\\342\\202\\254\\377\\000\\130\\n'\n";
  TERMINAL_CHECK(!terminal_input(t, data, cap, command, strlen(command)));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, "37 101", 6));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, raw, sizeof(raw)));
  command = "sh -c 'trap \"echo INTERRUPTED; exit 0\" INT; echo CHILD-READY; "
            "while :; do :; done'\n";
  TERMINAL_CHECK(!terminal_input(t, data, cap, command, strlen(command)));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, "CHILD-READY", 11));
  TERMINAL_CHECK(!terminal_input(t, data, cap, "\003", 1));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, "INTERRUPTED", 11));
  command = "i=0; while [ \"$i\" -lt 20000 ]; do printf 0123456789; "
            "i=$((i+1)); done\n";
  TERMINAL_CHECK(!terminal_input(t, data, cap, command, strlen(command)));
  before = t->total;
  wait.fd = t->fd;
  wait.events = POLLIN;
  for (frames = 0; frames < 10000; ++frames) {
    wait.revents = 0;
    if (poll(&wait, 1, 200) <= 0)
      break;
    TERMINAL_CHECK(!terminal_next(t, data, cap, 0));
  }
  TERMINAL_CHECK(t->total - before == 65536);
  native->used = 0;
  TERMINAL_CHECK(!native_terminal_input(
      &native_link, "stty size; printf 'NATIVE-%s\\n' "
                    "\"$SOFTLINE_WEB_TERMINAL_ISOLATION\"\n"));
  TERMINAL_CHECK(!native_terminal_wait(&native_link, native, "NATIVE-native"));
  TERMINAL_CHECK(!native_terminal_wait(&native_link, native, "31 93"));
  TERMINAL_CHECK(!ws_text(chat, "action=send&view=terminal&request=terminal-"
                                "chat&request_seq=1&text=chat+remains+live"));
  TERMINAL_CHECK(!until(chat, data, cap, "\"request\":\"terminal-chat\""));
  TERMINAL_CHECK(strstr(data, "\"type\":\"accepted\""));
  wait.revents = 0;
  TERMINAL_CHECK(poll(&wait, 1, 100) == 0);
  close(t->fd);
  t->fd = -1;
  TERMINAL_STATE(1);
  TERMINAL_CHECK(!terminal_open(t, port, data, cap));
  TERMINAL_STATE(2);
  command = "printf 'FRESH-%s\\n' "
            "\"${SOFTLINE_WEB_TERMINAL_ISOLATION-unset}\"; pwd\n";
  TERMINAL_CHECK(!terminal_input(t, data, cap, command, strlen(command)));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, "FRESH-unset", 11));
  TERMINAL_CHECK(!terminal_wait(t, data, cap, root, strlen(root)));
  TERMINAL_CHECK(!terminal_input(t, data, cap, "exit 7\n", 7));
  while (!t->exited)
    TERMINAL_CHECK(!terminal_next(t, data, cap, 1));
  TERMINAL_CHECK(t->status == 7);
  TERMINAL_STATE(1);
  native->used = 0;
  TERMINAL_CHECK(!native_terminal_input(
      &native_link, "stty size; printf 'SURVIVED-%s\\n' "
                    "\"$SOFTLINE_WEB_TERMINAL_ISOLATION\"\n"));
  TERMINAL_CHECK(
      !native_terminal_wait(&native_link, native, "SURVIVED-native"));
  TERMINAL_CHECK(!native_terminal_wait(&native_link, native, "31 93"));
  TERMINAL_CHECK(!native_terminal_input(&native_link, "exit 9\n"));
  while (!native->exited)
    TERMINAL_CHECK(!native_terminal_next(&native_link, native));
  TERMINAL_CHECK(native->status == 9);
  TERMINAL_STATE(0);
  close(t->fd);
  t->fd = -1;
  TERMINAL_CHECK(!terminal_open(t, port, data, cap));
  TERMINAL_STATE(1);
  TERMINAL_CHECK(!ws_text(t->fd, "action=ack&bytes=999999"));
  TERMINAL_CHECK(!until(t->fd, data, cap, "\"type\":\"error\""));
  TERMINAL_STATE(0);
  failed = 0;
cleanup:
  if (observer >= 0)
    close(observer);
  demo_link_close(&native_link);
  if (t && t->fd >= 0)
    close(t->fd);
  free(t);
  free(native);
  return failed;
#undef TERMINAL_CHECK
#undef TERMINAL_STATE
}
