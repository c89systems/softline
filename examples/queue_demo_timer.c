#define _POSIX_C_SOURCE 200809L
#include "queue_demo_timer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/timerfd.h>
#elif defined(__APPLE__)
#include <sys/event.h>
#endif

int demo_timer_create(unsigned int interval_ms) {
  int fd;
#if defined(__linux__)
  struct itimerspec timer;
#elif defined(__APPLE__)
  struct kevent timer;
#endif
  if (!interval_ms) {
    errno = EINVAL;
    return -1;
  }
#if defined(__linux__)
  fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (fd < 0)
    return -1;
  timer.it_interval.tv_sec = interval_ms / 1000;
  timer.it_interval.tv_nsec = (long)(interval_ms % 1000) * 1000000L;
  timer.it_value = timer.it_interval;
  if (timerfd_settime(fd, 0, &timer, NULL)) {
    close(fd);
    return -1;
  }
#elif defined(__APPLE__)
  fd = kqueue();
  if (fd < 0)
    return -1;
  EV_SET(&timer, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, interval_ms, NULL);
  if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ||
      kevent(fd, &timer, 1, NULL, 0, NULL) < 0) {
    close(fd);
    return -1;
  }
#else
  errno = ENOSYS;
  return -1;
#endif
  return fd;
}
int demo_timer_drain(int fd) {
#if defined(__linux__)
  uint64_t count;
  ssize_t status;
  do {
    status = read(fd, &count, sizeof(count));
  } while (status < 0 && errno == EINTR);
  return status == (ssize_t)sizeof(count) ? 0 : -1;
#elif defined(__APPLE__)
  struct kevent event;
  struct timespec immediate = {0, 0};
  int status;
  do {
    status = kevent(fd, NULL, 0, &event, 1, &immediate);
  } while (status < 0 && errno == EINTR);
  return status == 1 ? 0 : -1;
#else
  (void)fd;
  errno = ENOSYS;
  return -1;
#endif
}
