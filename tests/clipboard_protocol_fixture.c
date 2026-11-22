/* Drive the production nonblocking X11 state machine through a fixture peer. */
#include "../src/softline_clipboard.c"
#include <signal.h>

int __real_socket(int family, int type, int protocol);
static int socket_fault, socket_v6, socket_v4, socket_local;
int __wrap_socket(int family, int type, int protocol) {
  if (socket_fault) {
    if (family == AF_INET6)
      ++socket_v6;
    else if (family == AF_INET)
      ++socket_v4;
    else if (family == AF_UNIX)
      ++socket_local;
    if (family != AF_INET || socket_fault == 2) {
      errno = EAFNOSUPPORT;
      return -1;
    }
  }
  return __real_socket(family, type, protocol);
}
static int connect_fallback(int mode) {
  sl_clipboard_t *c = calloc(1, sizeof(*c));
  struct ares_addrinfo_node first, second;
  struct sockaddr_in address;
  struct sockaddr_in6 unsupported;
  sl_image_paste_event_t event;
  socklen_t length = sizeof(address);
  int listener = -1, peer = -1, result = 1, status;
  int64_t deadline = sl_now_ms() + 1000;
  if (!c)
    return 1;
  c->x.fd = -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  listener = __real_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener < 0 ||
      bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
      getsockname(listener, (struct sockaddr *)&address, &length) ||
      listen(listener, 1) || fcntl(listener, F_SETFL, O_NONBLOCK) < 0)
    goto done;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));
  memset(&unsupported, 0, sizeof(unsupported));
  unsupported.sin6_family = AF_INET6;
  first.ai_family = AF_INET6;
  first.ai_protocol = IPPROTO_TCP;
  first.ai_addr = (struct sockaddr *)&unsupported;
  first.ai_addrlen = sizeof(unsupported);
  first.ai_next = &second;
  second.ai_family = AF_INET;
  second.ai_protocol = IPPROTO_TCP;
  second.ai_addr = (struct sockaddr *)&address;
  second.ai_addrlen = sizeof(address);
  c->address = &first;
  c->display.local = mode == 3;
  c->phase = CP_CONNECT;
  socket_fault = mode == 2 ? 2 : 1;
  socket_v6 = socket_v4 = socket_local = 0;
  status = sl_clipboard_next(c, &event);
  if (mode == 3) {
    result = !(status < 0 && socket_local == 1 && !socket_v6 && !socket_v4);
    goto done;
  }
  if (status != 0 || c->error[0] || c->address != &second || socket_v6 != 1)
    goto done;
  while (sl_now_ms() < deadline) {
    struct pollfd fd;
    status = sl_clipboard_next(c, &event);
    if (status < 0) {
      result = !(mode == 2 && !c->address && socket_v4 == 1);
      break;
    }
    if (c->phase == CP_SETUP) {
      peer = accept(listener, NULL, NULL);
      result = !(mode == 1 && peer >= 0 && c->x.peer_family == AF_INET &&
                 socket_v6 == 1 && socket_v4 == 1);
      break;
    }
    fd.fd = c->x.fd;
    fd.events = POLLOUT;
    fd.revents = 0;
    if (fd.fd >= 0 && poll(&fd, 1, 10) < 0)
      break;
  }
done:
  socket_fault = 0;
  if (result)
    fprintf(stderr, "clipboard socket fallback failed (mode=%d)\n", mode);
  if (peer >= 0)
    close(peer);
  if (listener >= 0)
    close(listener);
  sl_clipboard_close(c);
  return result;
}

int main(int argc, char **argv) {
  sl_clipboard_t *c;
  sl_image_paste_event_t event;
  struct pollfd fds[SL_IMAGE_PASTE_MAX_FDS];
  int flags, timeout, count, status = 0, result = -1, fd = -1;
  int64_t deadline, remaining;
  const char *mode;
  if (argc == 2 && !strcmp(argv[1], "connect-fallback"))
    return connect_fallback(1) | connect_fallback(2) | connect_fallback(3);
  if (argc != 6)
    return 2;
  c = calloc(1, sizeof(*c));
  if (!c)
    return 2;
  c->x.fd = atoi(argv[2]);
  flags = fcntl(c->x.fd, F_GETFL, 0);
  if (flags < 0 || fcntl(c->x.fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return 2;
  c->packet = malloc(SL_SETUP_MAX);
  if (!c->packet)
    return 2;
  c->x.sequence = (uint16_t)strtoul(argv[3], NULL, 10);
  c->x.root = 1;
  c->window = 10;
  c->atoms[0] = 25;
  c->atoms[1] = c->target = 30;
  c->atoms[3] = 20;
  c->atoms[4] = 40;
  c->timestamp = 123;
  c->is_png = c->property_first = 1;
  c->limit = SL_IMAGE_MAX_BYTES;
  deadline = sl_now_ms() + atoi(argv[5]);
  mode = argv[1];
  (void)signal(SIGPIPE, SIG_IGN);
  if (!strcmp(mode, "atom"))
    c->phase = CP_ATOM;
  else if (!strcmp(mode, "window"))
    c->phase = CP_WINDOW;
  else if (!strcmp(mode, "delete")) {
    sl_capture_request(c, 19, 12);
    sl_put32(c->tx + 4, 10);
    sl_put32(c->tx + 8, 20);
    c->phase = CP_DONE;
  } else if (!strcmp(mode, "timestamp"))
    c->phase = CP_TIME;
  else if (!strcmp(mode, "select"))
    c->phase = CP_SELECT;
  else {
    c->phase = CP_PROPERTY;
    fd = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
      return 2;
  }
  while ((remaining = deadline - sl_now_ms()) > 0) {
    status = sl_clipboard_next(c, &event);
    if (status < 0)
      break;
    if (status > 0) {
      if (event.operation == SL_IMAGE_PASTE_DATA) {
        size_t at = 0;
        while (at < event.length) {
          ssize_t n = write(fd, event.data + at, event.length - at);
          if (n <= 0)
            goto done;
          at += (size_t)n;
        }
      } else if (event.operation == SL_IMAGE_PASTE_END) {
        result = 0;
        break;
      }
    }
    if (fd < 0 && c->tx_at == c->tx_len &&
        ((!strcmp(mode, "atom") && c->phase == CP_ATOM && c->atom_index == 1) ||
         (!strcmp(mode, "window") && c->phase == CP_TIME) ||
         (!strcmp(mode, "delete") && c->phase == CP_DONE) ||
         (!strcmp(mode, "timestamp") && c->phase == CP_SELECT &&
          c->timestamp == 123) ||
         (!strcmp(mode, "select") && c->phase == CP_BEGIN))) {
      result = 0;
      break;
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
  if (fd >= 0)
    close(fd);
  sl_clipboard_close(c);
  return result ? 1 : 0;
}
