#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <softline/terminal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define test_environment (*_NSGetEnviron())
#else
extern char **environ;
#define test_environment environ
#endif
#define TOTAL (256u * 1024u)
#if defined(SOFTLINE_TEST_WRAP_WAITPID)
static int hold_reap;
pid_t __real_waitpid(pid_t, int *, int);
pid_t __wrap_waitpid(pid_t pid, int *status, int options) {
  if (hold_reap && (options & WNOHANG))
    return 0;
  return __real_waitpid(pid, status, options);
}
#endif
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "session line %d: %s\n", __LINE__, #c);                  \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static unsigned char byte_at(size_t n) {
  return (unsigned char)((n * 37u + n / 251u) % 256u);
}
static int write_all(int fd, const char *p, size_t n) {
  while (n) {
    ssize_t got = write(fd, p, n);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return 1;
    p += got;
    n -= (size_t)got;
  }
  return 0;
}
static int child(void) {
  struct termios raw;
  char cwd[128], bytes[4096];
  size_t received = 0;
  if (!isatty(0) || !isatty(1) || !getcwd(cwd, sizeof(cwd)) ||
      strcmp(cwd, "/") || !getenv("SOFTLINE_SESSION_TEST") ||
      strcmp(getenv("SOFTLINE_SESSION_TEST"), "selected") || !getenv("TERM") ||
      strcmp(getenv("TERM"), "softline-test") || tcgetattr(0, &raw))
    return 100;
  raw.c_iflag = 0;
  raw.c_oflag = 0;
  raw.c_lflag = 0;
  raw.c_cflag |= CS8;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(0, TCSANOW, &raw) || write_all(1, "READY\n", 6))
    return 101;
  while (received < TOTAL) {
    size_t count = TOTAL - received;
    ssize_t n;
    if (count > sizeof(bytes))
      count = sizeof(bytes);
    n = read(0, bytes, count);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0 || write_all(1, bytes, (size_t)n))
      return 102;
    received += (size_t)n;
  }
  return 23;
}
static void step(sl_terminal_session_t *s) {
  sl_terminal_event_t e;
  struct pollfd fd;
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_STEP;
  if (s->dispatch(s, &e) != SL_OK)
    return;
  fd.fd = e.fd;
  fd.events = POLLIN | POLLOUT;
  fd.revents = 0;
  (void)poll(&fd, 1, 1);
}
static int dispose(sl_terminal_session_t *s) {
  double due = now() + 2;
  int status;
  do {
    status = s->destroy(s);
    if (status != SL_ERROR_NOT_READY)
      return status;
    {
      struct timespec t = {0, 1000000};
      nanosleep(&t, NULL);
    }
  } while (now() < due);
  return SL_ERROR_NOT_READY;
}
static int stream(const char *program, int receiver) {
  sl_terminal_session_config_t c;
  sl_terminal_session_t *s;
  sl_terminal_event_t e;
  const char *arguments[] = {NULL, "--session-child", NULL};
  const char *environment[] = {"SOFTLINE_SESSION_TEST=selected", "TERM=wrong",
                               NULL};
  char bytes[4096], ready[6];
  size_t sent = 0, received = 0, ready_bytes = 0, i;
  double due = now() + 5;
  int result, blocked = 0, exit_status = -1;
  arguments[0] = program;
  sl_terminal_session_config_init(&c);
  c.program = program;
  c.arguments = arguments;
  c.directory = "/";
  c.environment = environment;
  c.terminal_type = "softline-test";
  s = sl_terminal_session_create(&c);
  CHECK(s);
  arguments[1] = "wrong";
  environment[0] = "wrong";
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_OPEN;
  e.rows = 19;
  e.columns = 73;
  CHECK((receiver ? s->dispatch(s, &e) : sl_terminal_session_dispatch(s, &e)) ==
        SL_OK);
  CHECK(s->dispatch(s, &e) == SL_ERROR_INVALID);
  while (ready_bytes < sizeof(ready) && now() < due) {
    step(s);
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_READ;
    e.data = ready + ready_bytes;
    e.length = sizeof(ready) - ready_bytes;
    result = s->dispatch(s, &e);
    CHECK(result == SL_OK || result == SL_ERROR_NOT_READY);
    if (result == SL_OK) {
      CHECK(e.transferred);
      ready_bytes += e.transferred;
    }
  }
  CHECK(ready_bytes == 6 && !memcmp(ready, "READY\n", 6));
  /* Withhold output consumption until both bounded PTY directions apply
   * backpressure. Then resume using deliberately unequal chunk boundaries. */
  while (sent < TOTAL && now() < due) {
    size_t count = TOTAL - sent;
    if (count > sizeof(bytes))
      count = sizeof(bytes);
    for (i = 0; i < count; ++i)
      bytes[i] = (char)byte_at(sent + i);
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_WRITE;
    e.data = bytes;
    e.length = count;
    result = s->dispatch(s, &e);
    if (result == SL_ERROR_NOT_READY) {
      blocked = 1;
      break;
    }
    CHECK(result == SL_OK && e.transferred && e.transferred <= count);
    sent += e.transferred;
  }
  CHECK(blocked);
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_RESIZE;
  e.rows = 31;
  e.columns = 91;
  CHECK(s->dispatch(s, &e) == SL_OK);
  while (received < TOTAL && now() < due) {
    if (sent < TOTAL) {
      size_t count = TOTAL - sent;
      if (count > 17)
        count = 17;
      for (i = 0; i < count; ++i)
        bytes[i] = (char)byte_at(sent + i);
      memset(&e, 0, sizeof(e));
      e.operation = SL_TERMINAL_WRITE;
      e.data = bytes;
      e.length = count;
      result = s->dispatch(s, &e);
      CHECK(result == SL_OK || result == SL_ERROR_NOT_READY);
      if (result == SL_OK) {
        CHECK(e.transferred);
        sent += e.transferred;
      }
    }
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_READ;
    e.data = bytes;
    e.length = 37;
    result = s->dispatch(s, &e);
    CHECK(result == SL_OK || result == SL_ERROR_NOT_READY);
    if (result == SL_OK) {
      CHECK(e.transferred);
      for (i = 0; i < e.transferred; ++i)
        CHECK((unsigned char)bytes[i] == byte_at(received + i));
      received += e.transferred;
    }
    if (result == SL_ERROR_NOT_READY)
      step(s);
  }
  CHECK(sent == TOTAL && received == TOTAL);
  while (sl_terminal_session_status(s, &exit_status) == SL_ERROR_NOT_READY &&
         now() < due)
    step(s);
  CHECK(exit_status == 23);
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_READ;
  e.data = bytes;
  e.length = 1;
  CHECK(s->dispatch(s, &e) == SL_OK && !e.transferred);
  CHECK(dispose(s) == SL_OK);
  return 0;
}
static int invalid(void) {
  sl_terminal_session_config_t c;
  sl_terminal_session_t *s;
  sl_terminal_event_t e;
  int status = -1;
  double due = now() + 2;
  sl_terminal_session_config_init(&c);
  c.program = "/no/such/softline-program";
  s = sl_terminal_session_create(&c);
  CHECK(s);
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_OPEN;
  e.rows = 0;
  e.columns = 80;
  CHECK(s->dispatch(s, &e) == SL_ERROR_INVALID);
  e.rows = 24;
  CHECK(s->dispatch(s, &e) == SL_OK);
#if defined(SOFTLINE_TEST_WRAP_WAITPID)
  hold_reap = 1;
#endif
  while (s->status(s, &status) == SL_ERROR_NOT_READY && now() < due)
    step(s);
  CHECK(status == 127);
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_STEP;
  CHECK(s->dispatch(s, &e) == SL_ERROR_IO);
#if defined(SOFTLINE_TEST_WRAP_WAITPID)
  hold_reap = 0;
#endif
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_CLOSE;
  CHECK(s->dispatch(s, &e) == SL_OK && s->dispatch(s, &e) == SL_OK);
  CHECK(dispose(s) == SL_OK);
  CHECK(sl_terminal_session_destroy(NULL) == SL_OK);
  c.directory = "/no/such/softline-directory";
  c.program = "/bin/sh";
  s = sl_terminal_session_create(&c);
  CHECK(s);
  memset(&e, 0, sizeof(e));
  e.operation = SL_TERMINAL_OPEN;
  e.rows = 24;
  e.columns = 80;
  CHECK(s->dispatch(s, &e) == SL_OK);
  due = now() + 2;
  status = -1;
  while (s->status(s, &status) == SL_ERROR_NOT_READY && now() < due)
    step(s);
  CHECK(status == 126 && dispose(s) == SL_OK);
  return 0;
}
static int fd_count(void) {
  int fd, count = 0;
  for (fd = 0; fd < 1024; ++fd)
    if (fcntl(fd, F_GETFD) >= 0)
      ++count;
  return count;
}
static int cancellation(void) {
  sl_terminal_session_config_t c;
  sl_terminal_session_t *s;
  sl_terminal_event_t e;
  const char *arguments[] = {"cat", NULL};
  const char *too_many[130];
  char bytes[1];
  int before = fd_count(), i, status;
  sl_terminal_session_config_init(&c);
  c.program = "/bin/cat";
  c.arguments = arguments;
  for (i = 0; i < 3; ++i) {
    s = sl_terminal_session_create(&c);
    CHECK(s);
    memset(&e, 0, sizeof(e));
    e.operation = SL_TERMINAL_OPEN;
    e.rows = 24;
    e.columns = 80;
    CHECK(sl_terminal_session_adapter(s, &e) == SL_OK);
    e.operation = SL_TERMINAL_READ;
    e.data = bytes;
    e.length = SL_TERMINAL_SESSION_CHUNK_MAX + 1u;
    CHECK(s->dispatch(s, &e) == SL_ERROR_INVALID && !e.transferred);
    e.length = 1;
    CHECK(s->dispatch(s, &e) == SL_ERROR_NOT_READY);
    e.operation = SL_TERMINAL_RESIZE;
    e.rows = 0;
    CHECK(s->dispatch(s, &e) == SL_ERROR_INVALID);
    e.operation = SL_TERMINAL_CLOSE;
    CHECK(s->dispatch(s, &e) == SL_OK && s->dispatch(s, &e) == SL_OK);
    e.operation = SL_TERMINAL_STEP;
    CHECK(s->dispatch(s, &e) == SL_ERROR_INVALID);
    CHECK(dispose(s) == SL_OK);
    CHECK(waitpid(-1, &status, WNOHANG) == -1 && errno == ECHILD);
    CHECK(fd_count() == before);
  }
  for (i = 0; i < 129; ++i)
    too_many[i] = "x";
  too_many[129] = NULL;
  c.arguments = too_many;
  CHECK(!sl_terminal_session_create(&c));
  c.arguments = arguments;
  c.program = "";
  CHECK(!sl_terminal_session_create(&c));
  CHECK(fd_count() == before);
  return 0;
}
static int empty_environment(void) {
  sl_terminal_session_config_t config;
  sl_terminal_session_t *s;
  char **saved = test_environment;
  sl_terminal_session_config_init(&config);
  test_environment = NULL;
  s = sl_terminal_session_create(&config);
  test_environment = saved;
  CHECK(s && dispose(s) == SL_OK);
  return 0;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "--session-child"))
    return child();
  CHECK(!stream(argv[0], 0) && !stream(argv[0], 1) && !invalid() &&
        !cancellation() && !empty_environment());
  return 0;
}
