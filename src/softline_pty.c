#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include "softline_pty.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

int sl_pty_reap(sl_pty_t *p) {
  int status;
  pid_t got;
  if (!p->pid)
    return 1;
  do {
    got = waitpid(p->pid, &status, WNOHANG);
  } while (got < 0 && errno == EINTR);
  if (got == p->pid) {
    if (!p->exec_failure)
      p->status = WIFEXITED(status)     ? WEXITSTATUS(status)
                  : WIFSIGNALED(status) ? 128 + WTERMSIG(status)
                                        : -1;
    p->pid = 0;
  } else if (got < 0 && errno == ECHILD)
    p->pid = 0;
  return !p->pid;
}
static double pty_time(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void child_fail(int fd, unsigned char status) {
  (void)write(fd, &status, 1);
  _exit(status);
}
static int pty_open(sl_pty_t *p, sl_terminal_event_t *e) {
  int slave;
  int started[2];
  char path[256];
  long limit = sysconf(_SC_OPEN_MAX);
  struct winsize size;
  p->fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
  p->status = -1;
  if (p->fd < 0 || grantpt(p->fd) || unlockpt(p->fd))
    return SL_ERROR_IO;
  if (ptsname_r(p->fd, path, sizeof(path)))
    return SL_ERROR_IO;
  (void)fcntl(p->fd, F_SETFD, FD_CLOEXEC);
  memset(&size, 0, sizeof(size));
  size.ws_row = (unsigned short)e->rows;
  size.ws_col = (unsigned short)e->columns;
  if (ioctl(p->fd, TIOCSWINSZ, &size))
    return SL_ERROR_IO;
  if (pipe(started))
    return SL_ERROR_IO;
  p->exec_fd = started[0];
  if (fcntl(started[0], F_SETFL, O_NONBLOCK) ||
      fcntl(started[0], F_SETFD, FD_CLOEXEC) ||
      fcntl(started[1], F_SETFD, FD_CLOEXEC)) {
    close(started[1]);
    return SL_ERROR_IO;
  }
  p->pid = fork();
  if (p->pid < 0) {
    close(started[1]);
    p->pid = 0;
    return SL_ERROR_IO;
  }
  if (!p->pid) {
    long fd;
    sigset_t mask;
    if (setsid() < 0)
      child_fail(started[1], 126);
    slave = open(path, O_RDWR);
    if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) || dup2(slave, 0) < 0 ||
        dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
      child_fail(started[1], 126);
    for (fd = 3; fd < (limit > 0 ? limit : 1024); ++fd)
      if (fd != started[1])
        close((int)fd);
    (void)signal(SIGINT, SIG_DFL);
    (void)signal(SIGQUIT, SIG_DFL);
    (void)signal(SIGTERM, SIG_DFL);
    (void)signal(SIGHUP, SIG_DFL);
    (void)signal(SIGPIPE, SIG_DFL);
    (void)signal(SIGCHLD, SIG_DFL);
    (void)signal(SIGTSTP, SIG_DFL);
    (void)signal(SIGTTIN, SIG_DFL);
    (void)signal(SIGTTOU, SIG_DFL);
    (void)signal(SIGWINCH, SIG_DFL);
    sigemptyset(&mask);
    (void)sigprocmask(SIG_SETMASK, &mask, NULL);
    execl(p->shell, p->shell, "-i", (char *)NULL);
    child_fail(started[1], 127);
  }
  close(started[1]);
  return SL_OK;
}
int sl_pty_callback(void *context, sl_terminal_event_t *e) {
  sl_pty_t *p = context;
  struct winsize size;
  ssize_t n;
  switch (e->operation) {
  case SL_TERMINAL_OPEN:
    p->fd = -1;
    p->exec_fd = -1;
    p->pid = 0;
    return pty_open(p, e);
  case SL_TERMINAL_STEP:
    (void)sl_pty_reap(p);
    if (p->exec_fd >= 0) {
      unsigned char failed;
      n = read(p->exec_fd, &failed, 1);
      if (n >= 0) {
        close(p->exec_fd);
        p->exec_fd = -1;
        if (n) {
          p->status = failed;
          p->exec_failure = 1;
        }
        p->ready = 1;
      } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        return SL_ERROR_IO;
    }
    e->fd = p->exec_fd >= 0 ? p->exec_fd : p->eof_deadline ? -1 : p->fd;
    e->interests = SL_WATCH_READ | (p->write_blocked ? SL_WATCH_WRITE : 0);
    e->timeout_ms = p->eof_deadline ? 1 : 50;
    e->ready = p->ready;
    return SL_OK;
  case SL_TERMINAL_READ:
    if (p->exec_failure)
      return SL_OK;
    do {
      n = read(p->fd, e->data, e->length);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return SL_ERROR_NOT_READY;
    if (n < 0 && errno != EIO)
      return SL_ERROR_IO;
    if (n <= 0 && !sl_pty_reap(p)) {
      if (!p->eof_deadline)
        p->eof_deadline = pty_time() + 50;
      if (pty_time() < p->eof_deadline)
        return SL_ERROR_NOT_READY;
    }
    e->transferred = n > 0 ? (size_t)n : 0;
    return SL_OK;
  case SL_TERMINAL_WRITE:
    if (p->exec_failure)
      return SL_ERROR_NOT_READY;
    do {
      n = write(p->fd, e->data, e->length);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      p->write_blocked = 1;
      return SL_ERROR_NOT_READY;
    }
    if (n <= 0)
      return SL_ERROR_IO;
    e->transferred = (size_t)n;
    p->write_blocked = 0;
    return SL_OK;
  case SL_TERMINAL_RESIZE:
    memset(&size, 0, sizeof(size));
    size.ws_row = (unsigned short)e->rows;
    size.ws_col = (unsigned short)e->columns;
    return ioctl(p->fd, TIOCSWINSZ, &size) ? SL_ERROR_IO : SL_OK;
  case SL_TERMINAL_CLOSE:
    if (p->exec_fd >= 0)
      close(p->exec_fd);
    p->exec_fd = -1;
    if (p->fd >= 0)
      close(p->fd);
    p->fd = -1;
    if (p->pid && !sl_pty_reap(p)) {
      (void)kill(-p->pid, SIGHUP);
      (void)kill(-p->pid, SIGKILL);
      (void)sl_pty_reap(p);
    }
    return SL_OK;
  }
  return SL_ERROR_INVALID;
}
