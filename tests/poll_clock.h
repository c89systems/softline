#ifndef SOFTLINE_TEST_POLL_CLOCK_H
#define SOFTLINE_TEST_POLL_CLOCK_H

#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

struct clock_event {
  char kind;
  long now;
};

struct clock_step {
  long advance;
  int ready;
};

#ifdef SOFTLINE_TEST_POLL_CLOCK
static int clock_control = -1, clock_events = -1;
static long virtual_ms = 1000;

int __real_poll(struct pollfd *, nfds_t, int);
int __real_clock_gettime(clockid_t, struct timespec *);

int __wrap_clock_gettime(clockid_t id, struct timespec *time) {
  if (clock_control < 0 || id != CLOCK_MONOTONIC)
    return __real_clock_gettime(id, time);
  time->tv_sec = virtual_ms / 1000;
  time->tv_nsec = (virtual_ms % 1000) * 1000000;
  return 0;
}

int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout_ms) {
  long deadline = virtual_ms + timeout_ms;
  if (clock_control < 0 || count < 2)
    return __real_poll(fds, count, timeout_ms);
  for (;;) {
    struct clock_event event = {'P', 0};
    struct clock_step step;
    int ready;
    event.now = virtual_ms;
    if (write(clock_events, &event, sizeof(event)) != (ssize_t)sizeof(event) ||
        read(clock_control, &step, sizeof(step)) != (ssize_t)sizeof(step)) {
      errno = EIO;
      return -1;
    }
    /* A fixture step may cross a deadline. Wake the editor at its actual
     * deadline first and let the driver observe what it painted there. */
    virtual_ms += step.advance;
    if (timeout_ms >= 0 && virtual_ms > deadline && !step.ready)
      virtual_ms = deadline;
    ready = __real_poll(fds, count, step.ready ? 1000 : 0);
    if (ready || (timeout_ms >= 0 && virtual_ms >= deadline))
      return ready;
    if (step.ready) {
      errno = ETIMEDOUT;
      return -1;
    }
  }
}
#endif
#endif
