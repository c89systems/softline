#define _POSIX_C_SOURCE 200809L
#include "www_mock.h"
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "restart line %d: %s (%s)\n", __LINE__, #c,              \
              strerror(errno));                                                \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

int main(void) {
  static const unsigned char body[] = "ok\n";
  static const char request[] = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";
  sl_www_config_t config;
  sl_www_asset_t asset;
  demo_www_t *web = NULL, *other = NULL;
  struct sockaddr_in address;
  unsigned short port = 0;
  int fd = -1, result = 1, attempt;
  sl_www_config_init(&config);
  asset.path = "/";
  asset.content_type = "text/plain";
  asset.data = body;
  asset.length = sizeof(body) - 1;
  config.enabled = 1;
  config.assets = &asset;
  config.asset_count = 1;
  for (attempt = 0; attempt < 3; ++attempt) {
    int eof = 0, step;
    size_t received = 0, sent = 0;
    web = demo_www_open(&config, port, NULL);
    CHECK(web);
    if (!port)
      port = demo_www_port(web);
    CHECK(port && demo_www_port(web) == port);
    /* Reuse must never take over a currently listening server's port. */
    other = demo_www_open(&config, port, NULL);
    CHECK(!other && errno == EADDRINUSE);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(0x7f000001UL);
    address.sin_port = htons(port);
    CHECK(!connect(fd, (struct sockaddr *)&address, sizeof(address)));
    while (sent < sizeof(request) - 1) {
      ssize_t n = send(fd, request + sent, sizeof(request) - 1 - sent, 0);
      if (n < 0 && errno == EINTR)
        continue;
      CHECK(n > 0);
      sent += (size_t)n;
    }
    for (step = 0; step < 3000 && !eof; ++step) {
      struct pollfd pollfd;
      char bytes[512];
      int ready;
      CHECK(demo_www_poll(web, 1) == 0);
      pollfd.fd = fd;
      pollfd.events = POLLIN;
      pollfd.revents = 0;
      ready = poll(&pollfd, 1, 0);
      if (ready < 0 && errno == EINTR)
        continue;
      CHECK(ready >= 0);
      if (ready) {
        ssize_t n = recv(fd, bytes, sizeof(bytes), 0);
        if (n < 0 && errno == EINTR)
          continue;
        CHECK(n >= 0);
        if (!n)
          eof = 1;
        else
          received += (size_t)n;
      }
    }
    /* Drain the server's FIN before closing the client, leaving server-side
     * TIME_WAIT on this exact port. The next iteration must bind immediately.
     */
    CHECK(eof && received >= sizeof(body) - 1);
    CHECK(!close(fd));
    fd = -1;
    demo_www_close(web);
    web = NULL;
  }
  result = 0;
cleanup:
  if (fd >= 0)
    close(fd);
  demo_www_close(other);
  demo_www_close(web);
  return result;
}
