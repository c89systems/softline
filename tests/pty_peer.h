#ifndef SOFTLINE_TEST_PTY_PEER_H
#define SOFTLINE_TEST_PTY_PEER_H

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

/* Drain output from real PTYs used without an emulator. */
struct pty_peer {
  pthread_t thread;
  struct pollfd fds[2];
  int stop;
};

static void *pty_peer_run(void *context) {
  struct pty_peer *peer = context;
  for (;;) {
    int ready = poll(peer->fds, 2, -1);
    char bytes[512];
    ssize_t count;
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
