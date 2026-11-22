#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "queue_demo_terminal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
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
      fprintf(stderr, "transport line %d: %s (mode %d)\n", __LINE__, #c,       \
              mode);                                                           \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static int malformed(int mode) {
  int listener, fd, master, slave, report[2], status, result, got, code;
  pid_t child;
  struct sockaddr_in address;
  socklen_t length = sizeof(address);
  struct winsize size;
  struct termios original, after;
  struct pollfd p;
  demo_link_t link;
  demo_packet_t packet = {0};
  char bytes[4097];
  double due;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(0x7f000001UL);
  listener = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(listener >= 0 &&
        !bind(listener, (struct sockaddr *)&address, sizeof(address)) &&
        !listen(listener, 1) &&
        !getsockname(listener, (struct sockaddr *)&address, &length));
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(report) &&
        !tcgetattr(slave, &original));
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    sl_config_t config;
    sl_t *sl;
    close(listener);
    close(master);
    close(report[0]);
    alarm(5);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    sl = sl_create_with_config(&config);
    if (!sl)
      _exit(2);
    result = demo_terminal_remote(sl, &address, &code);
    sl_destroy(sl);
    result = result == ((mode == 3 || mode == 5) ? SL_OK : SL_ERROR_IO) &&
                     code == (mode == 5 ? 0 : -1)
                 ? 0
                 : 1;
    (void)write(report[1], &result, sizeof(result));
    _exit(result);
  }
  close(report[1]);
  p.fd = listener;
  p.events = POLLIN;
  p.revents = 0;
  CHECK(poll(&p, 1, 3000) == 1);
  fd = accept(listener, NULL, NULL);
  close(listener);
  CHECK(fd >= 0 && !demo_nonblock(fd));
  demo_link_init(&link, fd);
  due = now() + 2;
  got = 0;
  while (now() < due) {
    got = demo_link_read(&link, &packet);
    CHECK(got >= 0);
    if (got)
      break;
    p.fd = fd;
    p.events = POLLIN;
    (void)poll(&p, 1, 5);
  }
  CHECK(got == 1 && demo_number(&packet) == DEMO_PTY_OPEN);
  demo_packet_free(&packet);
  if (mode) {
    demo_put_number(&packet, DEMO_PTY_READY);
    CHECK(!demo_link_queue(&link, &packet));
  }
  memset(bytes, 'X', sizeof(bytes));
  bytes[4096] = 0;
  demo_put_number(&packet, mode >= 3   ? DEMO_PTY_EXIT
                           : mode == 2 ? 999u
                                       : DEMO_PTY_DATA);
  /* Deliberately bypass the terminal encoder's chunk cap to probe the
   * receiving adapter's validation of an untrusted peer. */
  if (mode >= 3)
    demo_put_number(&packet, mode == 3 ? 256u : mode == 4 ? 257u : 0u);
  else if (mode == 1)
    demo_put_image_bytes(&packet, (const unsigned char *)bytes, sizeof(bytes));
  else
    demo_put_bytes(&packet, bytes, 1);
  CHECK(!demo_link_queue(&link, &packet));
  due = now() + 2;
  while (link.count && now() < due) {
    CHECK(!demo_link_flush(&link));
    p.fd = fd;
    p.events = POLLOUT;
    (void)poll(&p, 1, 1);
  }
  CHECK(!link.count);
  demo_link_close(&link);
  p.fd = report[0];
  p.events = POLLIN;
  p.revents = 0;
  CHECK(poll(&p, 1, 4000) == 1 &&
        read(report[0], &result, sizeof(result)) == (ssize_t)sizeof(result));
  CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        !WEXITSTATUS(status) && !result);
  CHECK(!tcgetattr(slave, &after) && original.c_lflag == after.c_lflag &&
        original.c_iflag == after.c_iflag && original.c_oflag == after.c_oflag);
  CHECK(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
  got = (int)read(master, bytes, sizeof(bytes));
  CHECK(got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  close(master);
  close(slave);
  close(report[0]);
  return 0;
}
int main(void) {
  return malformed(0) || malformed(1) || malformed(2) || malformed(3) ||
         malformed(4) || malformed(5);
}
