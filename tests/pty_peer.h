#ifndef SOFTLINE_TEST_PTY_PEER_H
#define SOFTLINE_TEST_PTY_PEER_H

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

/* The logical editor tests use a real PTY without a renderer. Answer cursor
 * probes there so native setup does not exercise the no-reply timeout on
 * every case. Terminal integration tests supply their own cursor reports. */
struct pty_peer {
  pthread_t thread;
  struct pollfd fds[2];
  int stop;
};

static void *pty_peer_run(void *context) {
  struct pty_peer *peer = context;
  const char query[] = "\033[6n", reply[] = "\033[1;1R";
  size_t matched = 0;
  for (;;) {
    int ready = poll(peer->fds, 2, -1);
    char bytes[512];
    ssize_t count, index;
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return peer;
    if (peer->fds[1].revents)
      return NULL;
    count = read(peer->fds[0].fd, bytes, sizeof(bytes));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return peer;
    for (index = 0; index < count; ++index) {
      if (bytes[index] != query[matched]) {
        matched = bytes[index] == query[0] ? 1 : 0;
      } else if (++matched == sizeof(query) - 1) {
        ssize_t written;
        do {
          written = write(peer->fds[0].fd, reply, sizeof(reply) - 1);
        } while (written < 0 && errno == EINTR);
        if (written != (ssize_t)(sizeof(reply) - 1))
          return peer;
        matched = 0;
      }
    }
  }
}

static int pty_peer_start(struct pty_peer *peer, int master) {
  int stop[2];
  if (pipe(stop))
    return 1;
  peer->fds[0].fd = master;
  peer->fds[0].events = POLLIN;
  peer->fds[1].fd = stop[0];
  peer->fds[1].events = POLLIN;
  peer->stop = stop[1];
  if (pthread_create(&peer->thread, NULL, pty_peer_run, peer)) {
    close(stop[0]);
    close(stop[1]);
    return 1;
  }
  return 0;
}

static int pty_peer_stop(struct pty_peer *peer) {
  void *result;
  int failed;
  close(peer->stop);
  failed = pthread_join(peer->thread, &result) || result != NULL;
  close(peer->fds[1].fd);
  return failed;
}
#endif
