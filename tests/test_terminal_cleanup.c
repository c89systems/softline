#include "softline/softline.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%d: %s\n", __LINE__, #c);                               \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct pressure {
  int slave, notice, opened, closed;
} pressure_t;
typedef struct outcome {
  int status, closed, preserved, restored;
} outcome_t;
static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1000000000;
}
static int fill_output(int fd) {
  char chunk[2048];
  int flags = fcntl(fd, F_GETFL), full;
  memset(chunk, 'x', sizeof(chunk));
  if (tcflow(fd, TCOOFF))
    return 1;
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK))
    return 1;
  while (write(fd, chunk, sizeof(chunk)) > 0) {
  }
  full = errno == EAGAIN || errno == EWOULDBLOCK;
  return fcntl(fd, F_SETFL, flags) || !full;
}
static int adapter(void *context, sl_terminal_event_t *event) {
  pressure_t *p = context;
  if (event->operation == SL_TERMINAL_OPEN) {
    ++p->opened;
    if (fill_output(p->slave) || write(p->notice, "o", 1) != 1)
      return SL_ERROR_IO;
  } else if (event->operation == SL_TERMINAL_STEP)
    event->ready = 0;
  else if (event->operation == SL_TERMINAL_CLOSE)
    ++p->closed;
  return SL_OK;
}
/* Only cursor-report framing is retained by this terminal fixture. */
static void drain(int master) {
  static const char query[] = "\033[6n";
  char bytes[2048];
  size_t i;
  ssize_t got = read(master, bytes, sizeof(bytes));
  static size_t matched;
  for (i = 0; got > 0 && i < (size_t)got; ++i) {
    matched = bytes[i] == query[matched] ? matched + 1
              : bytes[i] == '\033'       ? 1
                                         : 0;
    if (matched == sizeof(query) - 1) {
      (void)write(master, "\033[1;1R", 6);
      matched = 0;
    }
  }
}
static int backpressure(int entry) {
  int master, slave, notice[2], control[2], report[2], status, received;
  pid_t runner;
  struct winsize size;
  struct pollfd fds[2];
  outcome_t result;
  char byte;
  double started, until, returned;
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size));
  CHECK(!pipe(notice) && !pipe(control) && !pipe(report));
  runner = fork();
  CHECK(runner >= 0);
  if (!runner) {
    sl_config_t config;
    sl_t *sl;
    pressure_t p;
    struct termios original, after;
    int flags = fcntl(slave, F_GETFL);
    close(master);
    memset(&p, 0, sizeof(p));
    p.slave = slave;
    p.notice = notice[1];
    tcgetattr(slave, &original);
    original.c_iflag &= ~IXON;
    if (tcsetattr(slave, TCSANOW, &original))
      _exit(7);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    sl = sl_create_with_config(&config);
    if (!sl || sl_set_buffer(sl, "preserved draft") || sl_set_cursor(sl, 4) ||
        sl_output_stream_begin(sl))
      _exit(2);
    if (write(notice[1], "r", 1) != 1 || read(control[0], &byte, 1) != 1)
      _exit(3);
    if (entry && (fill_output(slave) || write(notice[1], "o", 1) != 1))
      _exit(4);
    result.status = sl_terminal_handoff(sl, adapter, &p);
    result.closed = p.closed;
    result.preserved =
        !strcmp(sl_buffer(sl), "preserved draft") && sl_cursor(sl) == 4;
    result.restored =
        !tcgetattr(slave, &after) && original.c_lflag == after.c_lflag &&
        original.c_iflag == after.c_iflag &&
        original.c_oflag == after.c_oflag && flags == fcntl(slave, F_GETFL);
    if (write(report[1], &result, sizeof(result)) != (ssize_t)sizeof(result) ||
        read(control[0], &byte, 1) != 1)
      _exit(5);
    /* After drainage, the retained editor can acquire a fresh output session.
     */
    if (sl_output_stream_begin(sl) || sl_output_stream_end(sl))
      _exit(6);
    sl_destroy(sl);
    _exit(0);
  }
  close(report[1]);
  close(notice[1]);
  close(control[0]);
  fds[0].fd = notice[0];
  fds[0].events = POLLIN;
  fds[1].fd = master;
  fds[1].events = POLLIN;
  until = seconds() + 3;
  received = 0;
  while (seconds() < until && poll(fds, 2, 100) >= 0) {
    if (fds[0].revents & POLLIN) {
      received = read(notice[0], &byte, 1) == 1 && byte == 'r';
      break;
    }
    if (fds[1].revents & POLLIN)
      drain(master);
  }
  CHECK(received && write(control[1], "s", 1) == 1);
  fds[0].revents = 0;
  CHECK(poll(fds, 1, 2000) == 1 && read(notice[0], &byte, 1) == 1 &&
        byte == 'o');
  started = seconds();
  if (!entry)
    CHECK(write(master, "~.", 2) == 2);
  fds[0].fd = report[0];
  fds[0].revents = 0;
  received =
      poll(fds, 1, 900) == 1 &&
      read(report[0], &result, sizeof(result)) == (ssize_t)sizeof(result);
  returned = seconds() - started;
  CHECK(tcflow(slave, TCOON) == 0);
  if (!received) {
    kill(runner, SIGKILL);
    waitpid(runner, &status, 0);
  } else {
    CHECK(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
    while (poll(&fds[1], 1, 0) == 1 && fds[1].revents & POLLIN)
      drain(master);
    CHECK(write(control[1], "d", 1) == 1);
    until = seconds() + 2;
    while (waitpid(runner, &status, WNOHANG) == 0 && seconds() < until) {
      if (poll(&fds[1], 1, 10) == 1 && fds[1].revents & POLLIN)
        drain(master);
    }
    if (seconds() >= until) {
      kill(runner, SIGKILL);
      waitpid(runner, &status, 0);
    }
  }
  close(master);
  close(slave);
  close(notice[0]);
  close(control[1]);
  close(report[0]);
  CHECK(received && returned < 1.0);
  if (!(result.status == SL_ERROR_IO && result.closed == (entry ? 0 : 1) &&
        result.preserved && result.restored))
    fprintf(stderr, "entry=%d status=%d closed=%d preserved=%d restored=%d\n",
            entry, result.status, result.closed, result.preserved,
            result.restored);
  CHECK(result.status == SL_ERROR_IO && result.closed == (entry ? 0 : 1) &&
        result.preserved && result.restored);
  CHECK(WIFEXITED(status) && !WEXITSTATUS(status));
  return 0;
}
static int job_shell(void) {
  int ready[2], status;
  pid_t job;
  char byte;
  if (pipe(ready))
    return 2;
  job = fork();
  if (job < 0)
    return 3;
  if (!job) {
    setpgid(0, 0);
    signal(SIGHUP, SIG_IGN);
    (void)write(ready[1], "r", 1);
    for (;;)
      pause();
  }
  setpgid(job, job);
  if (read(ready[0], &byte, 1) != 1 || tcsetpgrp(0, job))
    return 4;
  printf("JOB %ld\n", (long)job);
  fflush(stdout);
  waitpid(job, &status, 0);
  return 0;
}
static int foreground(const char *executable) {
  int master, slave, info[2], status, result, exit_status, stopped = 0;
  pid_t driver, job = 0;
  struct winsize size;
  struct timespec delay;
  sl_config_t config;
  sl_t *sl;
  double until;
  CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1));
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(info));
  driver = fork();
  CHECK(driver >= 0);
  if (!driver) {
    char bytes[128];
    size_t used = 0;
    struct pollfd p;
    long pid;
    alarm(3);
    p.fd = master;
    p.events = POLLIN;
    p.revents = 0;
    while (used < sizeof(bytes) - 1 && poll(&p, 1, 2000) == 1) {
      ssize_t got = read(master, bytes + used, sizeof(bytes) - used - 1);
      if (got <= 0)
        _exit(2);
      used += (size_t)got;
      bytes[used] = 0;
      if (strchr(bytes, '\n') && sscanf(bytes, "JOB %ld", &pid) == 1) {
        job = (pid_t)pid;
        (void)write(info[1], &job, sizeof(job));
        _exit(write(master, "~.", 2) != 2);
      }
    }
    _exit(3);
  }
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  result = sl_terminal_local(sl, executable, &exit_status);
  sl_destroy(sl);
  CHECK(waitpid(driver, &status, 0) == driver && WIFEXITED(status) &&
        !WEXITSTATUS(status));
  CHECK(read(info[0], &job, sizeof(job)) == (ssize_t)sizeof(job) && job > 0);
  until = seconds() + .3;
  delay.tv_sec = 0;
  delay.tv_nsec = 1000000;
  do {
    pid_t got = waitpid(job, &status, WNOHANG);
    if (got == job ||
        (got < 0 && errno == ECHILD && kill(job, 0) < 0 && errno == ESRCH)) {
      stopped = 1;
      break;
    }
    nanosleep(&delay, NULL);
  } while (seconds() < until);
  if (!stopped) {
    kill(-job, SIGKILL);
    waitpid(job, &status, 0);
  }
  close(master);
  close(slave);
  close(info[0]);
  close(info[1]);
  CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 0));
  CHECK(result == SL_TERMINAL_ESCAPED && stopped);
  return 0;
}
static int closed_stdio(void) {
  int master, slave, status, bad_byte = 0;
  pid_t runner;
  struct winsize size;
  struct pollfd p;
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size));
  runner = fork();
  CHECK(runner >= 0);
  if (!runner) {
    sl_config_t config;
    sl_t *sl;
    int result, exit_status = -1;
    alarm(3);
    close(master);
    close(0);
    close(1);
    close(2);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    sl = sl_create_with_config(&config);
    result = sl_terminal_local(sl, "/dev/null/softline-shell", &exit_status);
    sl_destroy(sl);
    close(slave);
    _exit(result == SL_OK && exit_status == 127 ? 0 : 1);
  }
  close(slave);
  p.fd = master;
  p.events = POLLIN;
  p.revents = 0;
  while (poll(&p, 1, 3500) > 0) {
    char bytes[128];
    size_t i;
    ssize_t got = read(master, bytes, sizeof(bytes));
    if (got <= 0)
      break;
    for (i = 0; i < (size_t)got; ++i)
      if ((unsigned char)bytes[i] == 127)
        bad_byte = 1;
  }
  close(master);
  CHECK(waitpid(runner, &status, 0) == runner && WIFEXITED(status) &&
        !WEXITSTATUS(status));
  CHECK(!bad_byte);
  return 0;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "-i"))
    return job_shell();
  if (argc == 2 && !strcmp(argv[1], "backpressure"))
    return backpressure(0);
  if (argc == 2 && !strcmp(argv[1], "foreground"))
    return foreground(argv[0]);
  if (argc == 2 && !strcmp(argv[1], "closed-stdio"))
    return closed_stdio();
  return backpressure(0) || backpressure(1) || foreground(argv[0]) ||
         closed_stdio();
}
