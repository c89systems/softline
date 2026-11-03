/* Test private X11 operations through their socket protocol, without exports.
 */
#include "../src/softline_clipboard.c"

#include <signal.h>

int main(int argc, char **argv) {
  sl_x11_t x;
  uint32_t timestamp;
  int fd, flags, timeout_ms, result = -1;
  if (argc != 6)
    return 2;
  memset(&x, 0, sizeof(x));
  x.fd = atoi(argv[2]);
  /* Match sl_x11_connect(): the deadline relies on nonblocking socket I/O. */
  flags = fcntl(x.fd, F_GETFL, 0);
  if (flags < 0 || fcntl(x.fd, F_SETFL, flags | O_NONBLOCK) < 0)
    return 2;
  x.sequence = (uint16_t)strtoul(argv[3], NULL, 10);
  x.root = 1;
  timeout_ms = atoi(argv[5]);
  if (timeout_ms <= 0 || timeout_ms > SL_CLIPBOARD_TIMEOUT_MS)
    return 2;
  x.deadline = sl_now_ms() + timeout_ms;
  (void)signal(SIGPIPE, SIG_IGN);
  if (strcmp(argv[1], "atom") == 0)
    result = sl_x11_atom(&x, "CLIPBOARD") == 30 ? 0 : -1;
  else if (strcmp(argv[1], "window") == 0)
    result = sl_x11_create_window(&x, 10);
  else if (strcmp(argv[1], "delete") == 0)
    result = sl_x11_delete_property(&x, 10, 20);
  else if (strcmp(argv[1], "timestamp") == 0) {
    result = sl_x11_timestamp(&x, 10, 20, &timestamp);
    if (result == 0 && timestamp != 123)
      result = -1;
  } else if (strcmp(argv[1], "select") == 0)
    result = sl_x11_select_image(&x, 10, 25, 20, 30, 123) == 1 ? 0 : -1;
  else if (strcmp(argv[1], "transfer") == 0 || strcmp(argv[1], "direct") == 0) {
    fd = open(argv[4], O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
      return 2;
    result = sl_x11_transfer(&x, 10, 20, 30, 40, 1, fd);
    if (close(fd) != 0)
      result = -1;
  }
  (void)close(x.fd);
  return result == 0 ? 0 : 1;
}
