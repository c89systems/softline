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
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%d: %s (case %d)\n", __LINE__, #x, mode);               \
      return 1;                                                                \
    }                                                                          \
  } while (0)

typedef struct fixture {
  sl_t *sl;
  int mode, slave, notice, control, opened, closed, resized, pending_tilde,
      acked, resource;
  char received[256];
  size_t length;
} fixture_t;
static const char typed[] = "abc~.\r~~x\n~z\r";
static const char forwarded[] = "abc~.\r~x\n~z\r";
static const char output[] = "binary\0output\r\n";
static int adapter(void *context, sl_terminal_event_t *e) {
  fixture_t *f = context;
  char command;
  if (e->operation == SL_TERMINAL_OPEN) {
    ++f->opened;
    f->resource = dup(f->slave);
    if (f->resource < 0)
      return SL_ERROR_IO;
    if (sl_terminal_handoff(f->sl, adapter, f) != SL_ERROR_INVALID)
      return SL_ERROR;
    if (f->mode == 6)
      return SL_ERROR_IO;
    if (f->mode == 12)
      return 1;
    if (f->mode < 5 || f->mode == 9 || f->mode == 10 || f->mode == 11)
      (void)write(f->notice, "o", 1);
  } else if (e->operation == SL_TERMINAL_CLOSE) {
    ++f->closed;
    close(f->resource);
  } else if (e->operation == SL_TERMINAL_STEP) {
    int available = -1;
    if (f->mode == 7 || f->mode == 10)
      return SL_ERROR_IO;
    if (read(f->control, &command, 1) == 1) {
      if (command == 'q')
        return SL_ERROR_IO;
      if (command == 't')
        f->pending_tilde = 1;
    }
    if (f->pending_tilde && !ioctl(f->slave, FIONREAD, &available) &&
        !available) {
      (void)write(f->notice, "t", 1);
      f->pending_tilde = 0;
    }
    e->ready = f->mode != 1;
    e->timeout_ms = 10;
  } else if (e->operation == SL_TERMINAL_READ) {
    if (f->mode == 8) {
      e->transferred = e->length + 1;
      return SL_OK;
    }
    if (f->mode == 5 && !f->acked++) {
      memcpy(e->data, output, sizeof(output) - 1);
      e->transferred = sizeof(output) - 1;
    } else if (f->mode == 3) {
      memset(e->data, 'o', e->length);
      e->transferred = e->length;
    } else if (f->mode != 5)
      return SL_ERROR_NOT_READY;
  } else if (e->operation == SL_TERMINAL_WRITE) {
    if (f->mode == 2 || f->mode == 11) {
      if (!f->acked++)
        (void)write(f->notice, "w", 1);
      return SL_ERROR_NOT_READY;
    }
    if (f->mode == 9) {
      (void)write(f->notice, "w", 1);
      e->transferred = e->length + 1;
      return SL_OK;
    }
    if (f->length >= sizeof(f->received))
      return SL_ERROR_FULL;
    f->received[f->length++] = e->data[0];
    e->transferred = 1;
    if (f->length == sizeof(forwarded) - 1)
      (void)write(f->notice, "w", 1);
  } else if (e->operation == SL_TERMINAL_RESIZE) {
    ++f->resized;
    if ((f->resized == 1 && (e->rows != 5 || e->columns != 20)) ||
        (f->resized == 2 && (e->rows != 40 || e->columns != 120)))
      return SL_ERROR;
    (void)write(f->notice, "r", 1);
  }
  return SL_OK;
}
static int await(int fd, char expected) {
  struct pollfd p;
  char byte;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  return poll(&p, 1, 1500) != 1 || read(fd, &byte, 1) != 1 || byte != expected;
}
static int drive(int mode, int master, int notice, int control) {
  struct winsize size;
  char backlog[4096];
  if (await(notice, 'o'))
    return 1;
  if (mode == 11) {
    memset(backlog, 'x', sizeof(backlog));
    if (write(master, backlog, sizeof(backlog)) != (ssize_t)sizeof(backlog) ||
        await(notice, 'w'))
      return 7;
    return write(master, backlog, 256) != 256;
  }
  if (!mode) {
    if (write(master, typed, sizeof(typed) - 1) != (ssize_t)sizeof(typed) - 1 ||
        await(notice, 'w'))
      return 2;
    if (write(master, "~", 1) != 1 || write(control, "t", 1) != 1 ||
        await(notice, 't'))
      return 3;
    return write(master, ".", 1) != 1;
  }
  if (mode == 2 || mode == 9) {
    if (write(master, "input", 5) != 5 || await(notice, 'w'))
      return 4;
  }
  if (mode == 4) {
    memset(&size, 0, sizeof(size));
    size.ws_row = 5;
    size.ws_col = 20;
    if (ioctl(master, TIOCSWINSZ, &size) || await(notice, 'r'))
      return 5;
    size.ws_row = 40;
    size.ws_col = 120;
    if (ioctl(master, TIOCSWINSZ, &size) || await(notice, 'r'))
      return 6;
  }
  return write(master, "\r~.", 3) != 3;
}
static double seconds(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1000000000;
}
static int run(int mode) {
  fixture_t f;
  sl_config_t config;
  int master, slave, notice[2], control[2], flags, status, result, child_status;
  int output_master = -1, output_slave = -1, output_flags = 0;
  pid_t child = -1;
  struct termios before, after, output_before, output_after;
  struct winsize size;
  double started;
  memset(&f, 0, sizeof(f));
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size));
  if (mode == 13) {
    CHECK(!openpty(&output_master, &output_slave, NULL, NULL, &size));
    CHECK(!tcgetattr(output_slave, &output_before));
    output_flags = fcntl(output_slave, F_GETFL);
  }
  CHECK(!pipe(notice) && !pipe(control));
  CHECK(fcntl(control[0], F_SETFL, O_NONBLOCK) == 0);
  CHECK(!tcgetattr(slave, &before));
  if (mode == 14) {
    before.c_iflag |= INLCR | IGNCR | PARMRK;
    before.c_lflag |= ECHONL;
    CHECK(!tcsetattr(slave, TCSANOW, &before));
  }
  flags = fcntl(slave, F_GETFL);
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  if (mode == 13)
    config.output_fd = output_slave;
  f.sl = sl_create_with_config(&config);
  CHECK(f.sl && sl_set_buffer(f.sl, "draft\303\251\nsecond") == SL_OK &&
        sl_set_cursor(f.sl, 3) == SL_OK);
  f.mode = mode == 13 ? 5 : mode == 14 ? 0 : mode;
  f.slave = slave;
  f.notice = notice[1];
  f.control = control[0];
  if (mode == 3) {
    char bytes[2048];
    memset(bytes, 'x', sizeof(bytes));
    CHECK(!fcntl(slave, F_SETFL, flags | O_NONBLOCK));
    while (write(slave, bytes, sizeof(bytes)) > 0) {
    }
    CHECK(errno == EAGAIN && !fcntl(slave, F_SETFL, flags));
  }
  if (mode < 5 || mode == 9 || mode == 10 || mode == 11 || mode == 14) {
    child = fork();
    CHECK(child >= 0);
    if (!child) {
      int code;
      alarm(3);
      code = drive(mode == 14 ? 0 : mode, master, notice[0], control[1]);
      if (code)
        (void)write(control[1], "q", 1);
      _exit(code);
    }
  }
  started = seconds();
  result = sl_terminal_handoff(f.sl, adapter, &f);
  CHECK(seconds() - started < (mode == 14 ? 2.0 : 1.0));
  if (mode < 5 || mode == 14)
    CHECK(result == SL_TERMINAL_ESCAPED);
  else if (mode == 5 || mode == 13)
    CHECK(result == SL_OK);
  else if (mode == 8 || mode == 9 || mode == 12)
    CHECK(result == SL_ERROR_INVALID);
  else if (mode == 11)
    CHECK(result == SL_ERROR_FULL);
  else
    CHECK(result == SL_ERROR_IO);
  CHECK(f.opened == 1 && f.closed == 1);
  CHECK(fcntl(f.resource, F_GETFD) == -1 && errno == EBADF);
  CHECK(!strcmp(sl_buffer(f.sl), "draft\303\251\nsecond") &&
        sl_cursor(f.sl) == 3);
  CHECK(!tcgetattr(slave, &after));
  CHECK(before.c_iflag == after.c_iflag && before.c_oflag == after.c_oflag &&
        before.c_lflag == after.c_lflag && before.c_cflag == after.c_cflag &&
        !memcmp(before.c_cc, after.c_cc, NCCS) &&
        flags == fcntl(slave, F_GETFL));
  if (!mode || mode == 14)
    CHECK(f.length == sizeof(forwarded) - 1 &&
          !memcmp(f.received, forwarded, f.length));
  if (mode == 4)
    CHECK(f.resized == 2);
  if (mode == 5 || mode == 13) {
    char bytes[128];
    CHECK(read(mode == 13 ? output_master : master, bytes, sizeof(bytes)) ==
              (ssize_t)sizeof(output) - 1 &&
          !memcmp(bytes, output, sizeof(output) - 1));
  }
  if (child > 0) {
    CHECK(waitpid(child, &child_status, 0) == child);
    CHECK(WIFEXITED(child_status) && !WEXITSTATUS(child_status));
  }
  sl_destroy(f.sl);
  if (mode == 13) {
    CHECK(!tcgetattr(output_slave, &output_after));
    CHECK(output_before.c_oflag == output_after.c_oflag &&
          output_flags == fcntl(output_slave, F_GETFL));
    close(output_master);
    close(output_slave);
  }
  close(master);
  close(slave);
  close(notice[0]);
  close(notice[1]);
  close(control[0]);
  close(control[1]);
  status = 0;
  return status;
}
static int shell_fixture(void) {
  char line[128];
  struct termios attrs;
  if (tcgetattr(0, &attrs) || !(attrs.c_lflag & ICANON) ||
      tcgetpgrp(0) != getpgrp())
    return 3;
  puts("LOCAL_READY");
  fflush(stdout);
  while (fgets(line, sizeof(line), stdin)) {
    if (!strcmp(line, "fail\n"))
      return 7;
    if (!strcmp(line, "hang\n")) {
      signal(SIGHUP, SIG_IGN);
      puts("LOCAL_HUNG");
      fflush(stdout);
      for (;;)
        pause();
    }
  }
  return 0;
}
static int marker(int master, const char *expected) {
  char bytes[512];
  size_t used = 0;
  struct pollfd p;
  p.fd = master;
  p.events = POLLIN;
  p.revents = 0;
  while (used < sizeof(bytes) - 1) {
    ssize_t got;
    if (poll(&p, 1, 1500) != 1)
      return 1;
    got = read(master, bytes + used, sizeof(bytes) - used - 1);
    if (got <= 0)
      return 1;
    used += (size_t)got;
    bytes[used] = 0;
    if (strstr(bytes, expected))
      return 0;
  }
  return 1;
}
static int local_adapter(const char *executable, int mode) {
  int master, slave, child_status, result, exit_status;
  pid_t driver;
  sl_t *sl;
  sl_config_t config;
  struct winsize size;
  double started;
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size));
  sl_config_init(&config);
  config.input_fd = config.output_fd = slave;
  sl = sl_create_with_config(&config);
  CHECK(sl);
  driver = fork();
  CHECK(driver >= 0);
  if (!driver) {
    alarm(4);
    if (marker(master, "LOCAL_READY"))
      _exit(2);
    if (mode) {
      if (write(master, "hang\n", 5) != 5 || marker(master, "LOCAL_HUNG"))
        _exit(3);
      _exit(write(master, "\r~.", 3) != 3);
    }
    _exit(write(master, "fail\n", 5) != 5);
  }
  started = seconds();
  result = sl_terminal_local(sl, executable, &exit_status);
  CHECK(seconds() - started < 2.0);
  CHECK(mode ? result == SL_TERMINAL_ESCAPED
             : result == SL_OK && exit_status == 7);
  CHECK(waitpid(driver, &child_status, 0) == driver &&
        WIFEXITED(child_status) && !WEXITSTATUS(child_status));
  /* Closing the handle finishes only bounded local reaping; no other child
   * may remain, including a shell which ignored SIGHUP. */
  sl_destroy(sl);
  CHECK(waitpid(-1, &child_status, WNOHANG) == -1 && errno == ECHILD);
  close(master);
  close(slave);
  return 0;
}
int main(int argc, char **argv) {
  int mode;
  if (argc == 2 && !strcmp(argv[1], "-i"))
    return shell_fixture();
  if (argc == 2 && !strcmp(argv[1], "raw-flags"))
    return run(14);
  for (mode = 0; mode <= 14; ++mode)
    if (run(mode))
      return 1;
  return local_adapter(argv[0], 0) || local_adapter(argv[0], 1);
}
