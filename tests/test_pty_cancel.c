#define _POSIX_C_SOURCE 200809L
#include "softline_pty.h"
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int reached[2], release_child[2], established;
pid_t __real_setsid(void);
pid_t __wrap_setsid(void);

/* Hold the actual adapter child on either side of session creation. */
pid_t __wrap_setsid(void) {
  char byte;
  pid_t session = established ? __real_setsid() : 0;
  if (session < 0 || write(reached[1], "r", 1) != 1 ||
      read(release_child[0], &byte, 1) != 1)
    _exit(125);
  return established ? session : __real_setsid();
}

static double milliseconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int cancel_child(int after_session) {
  sl_pty_t p;
  sl_terminal_event_t event;
  struct pollfd ready;
  void (*previous)(int);
  double deadline;
  char byte;
  int result, cancelled = 0, at_boundary = 0, status;
  memset(&p, 0, sizeof(p));
  memset(&event, 0, sizeof(event));
  p.fd = p.exec_fd = -1;
  p.shell = "/bin/sh";
  established = after_session;
  if (pipe(reached) || pipe(release_child))
    return 1;
  previous = signal(SIGHUP, SIG_IGN);
  event.operation = SL_TERMINAL_OPEN;
  event.rows = 24;
  event.columns = 80;
  result = sl_pty_callback(&p, &event);
  (void)signal(SIGHUP, previous);
  ready.fd = reached[0];
  ready.events = POLLIN;
  ready.revents = 0;
  if (result == SL_OK && p.pid > 0 && poll(&ready, 1, 2000) == 1 &&
      read(reached[0], &byte, 1) == 1)
    at_boundary =
        after_session ? getsid(p.pid) == p.pid : getsid(p.pid) == getsid(0);
  event.operation = SL_TERMINAL_CLOSE;
  result = sl_pty_callback(&p, &event);
  deadline = milliseconds() + 1000;
  while (p.pid && milliseconds() < deadline) {
    (void)sl_pty_reap(&p);
    if (p.pid)
      (void)poll(NULL, 0, 1);
  }
  cancelled = !p.pid && p.status == 128 + SIGKILL;
  if (p.pid) {
    (void)kill(p.pid, SIGKILL);
    while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {
    }
    p.pid = 0;
  }
  /* CLOSE remains safe after cancellation/reaping, without any live child. */
  if (sl_pty_callback(&p, &event) != SL_OK || p.fd != -1 || p.exec_fd != -1)
    result = SL_ERROR;
  close(reached[0]);
  close(reached[1]);
  close(release_child[0]);
  close(release_child[1]);
  if (!at_boundary || !cancelled || result != SL_OK) {
    fprintf(stderr, "after_session=%d boundary=%d cancelled=%d result=%d\n",
            after_session, at_boundary, cancelled, result);
    return 1;
  }
  return 0;
}

int main(void) { return cancel_child(0) || cancel_child(1); }
