/* Drive the production nonblocking X11 state machine through a fixture peer. */
#include "../src/softline_clipboard.c"
#include <signal.h>

int main(int argc, char **argv) {
  sl_clipboard_t *c;
  sl_image_paste_event_t event;
  struct pollfd fds[SL_IMAGE_PASTE_MAX_FDS];
  int flags, timeout, count, status = 0, result = -1, fd = -1;
  int64_t deadline, remaining;
  const char *mode;
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
