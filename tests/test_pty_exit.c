#include "queue_demo_terminal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const char binary_tail[] = "TAIL\0BYTES";
static const char tail_end[] = "SHELL_TAIL_DONE";
static double seconds(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + t.tv_nsec / 1000000000.0;
}
static int write_all(int fd, const char *text, size_t length) {
  while (length) {
    ssize_t n = write(fd, text, length);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return 1;
    text += n;
    length -= (size_t)n;
  }
  return 0;
}
/* A separate background process keeps the slave open after this shell exits.
 * Its watchdog is only for failed tests; successful tests reap it themselves.
 */
static int shell_fixture(void) {
  int ready[2], go[2], i;
  pid_t job;
  char byte, text[2048];
  if (pipe(ready) || pipe(go))
    return 2;
  job = fork();
  if (job < 0)
    return 3;
  if (!job) {
    close(ready[0]);
    close(go[1]);
    setpgid(0, 0);
    signal(SIGHUP, SIG_IGN);
    signal(SIGALRM, SIG_DFL);
    alarm(4);
    if (write_all(ready[1], "r", 1) || read(go[0], &byte, 1) != 1)
      _exit(0);
    if (!strcmp(getenv("SOFTLINE_TEST_PTY_WRITER"), "1")) {
      memset(text, 'x', sizeof(text));
      while (!write_all(1, text, sizeof(text))) {
      }
    } else
      for (;;)
        pause();
    _exit(0);
  }
  close(ready[1]);
  close(go[0]);
  if (read(ready[0], &byte, 1) != 1)
    return 4;
  snprintf(text, sizeof(text), "HELD %ld\n", (long)job);
  if (write_all(1, text, strlen(text)) ||
      write_all(1, binary_tail, sizeof(binary_tail) - 1))
    return 5;
  memset(text, 't', sizeof(text));
  for (i = 0; i < 8; ++i)
    if (write_all(1, text, sizeof(text)))
      return 6;
  if (write_all(1, tail_end, sizeof(tail_end) - 1) || write_all(go[1], "g", 1))
    return 8;
  return 7;
}
static int contains(const char *bytes, size_t length, const char *text,
                    size_t size) {
  size_t i;
  for (i = 0; size <= length && i <= length - size; ++i)
    if (!memcmp(bytes + i, text, size))
      return 1;
  return 0;
}
/* A perpetually readable source makes the exit-drain byte bound deterministic,
 * independently of whether a background writer happens to lose a scheduling
 * race and let a real PTY become empty. Reaping must not refill the budget. */
static int drain_budget(void) {
  sl_pty_t p;
  sl_terminal_event_t event;
  char bytes[257];
  size_t total = 0;
  int iterations = 0, failed = 0;
  double deadline = seconds() + 1;
  struct timespec delay = {0, 1000000};
  memset(&p, 0, sizeof(p));
  p.fd = open("/dev/zero", O_RDONLY);
  p.exec_fd = -1;
  p.ready = 1;
  if (p.fd < 0)
    return 1;
  p.pid = fork();
  if (!p.pid)
    _exit(7);
  if (p.pid < 0) {
    close(p.fd);
    return 1;
  }
  while (!sl_pty_reap(&p) && seconds() < deadline)
    nanosleep(&delay, NULL);
  if (p.pid || p.status != 7)
    failed = 1;
  while (!failed && ++iterations < 300) {
    memset(&event, 0, sizeof(event));
    event.operation = SL_TERMINAL_READ;
    event.data = bytes;
    event.length = sizeof(bytes);
    if (!sl_pty_reap(&p) || sl_pty_callback(&p, &event) != SL_OK ||
        event.length != sizeof(bytes) || event.transferred > sizeof(bytes)) {
      failed = 1;
      break;
    }
    total += event.transferred;
    if (!event.transferred)
      break;
  }
  if (iterations >= 300 || total != 65536)
    failed = 1;
  memset(&event, 0, sizeof(event));
  event.operation = SL_TERMINAL_CLOSE;
  (void)sl_pty_callback(&p, &event);
  if (p.pid) {
    int status;
    kill(p.pid, SIGKILL);
    waitpid(p.pid, &status, 0);
  }
  if (failed)
    fprintf(stderr, "exit drain used %lu bytes in %d reads\n",
            (unsigned long)total, iterations);
  return failed;
}
#define REQUIRE(x)                                                             \
  do {                                                                         \
    if (!(x)) {                                                                \
      fprintf(stderr, "%d: %s (%s)\n", __LINE__, #x, mode);                    \
      failed = 1;                                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (0)

static int run(const char *executable, const char *owner_path,
               const char *mode) {
  int remote = !strncmp(mode, "remote", 6), failed = 0, port = 0;
  int startup[2] = {-1, -1}, master = -1, slave = -1, status = 0, finished = 0;
  pid_t owner = 0, frontend = 0, job = 0;
  FILE *output = NULL;
  char *bytes = NULL;
  size_t used = 0, capacity = 128 * 1024;
  struct winsize size;
  struct pollfd p;
  double started = 0;
  REQUIRE(!prctl(PR_SET_CHILD_SUBREAPER, 1));
  REQUIRE(!setenv("SOFTLINE_TEST_PTY_WRITER",
                  strstr(mode, "writer") ? "1" : "0", 1));
  if (remote) {
    char line[128];
    REQUIRE(!pipe(startup));
    owner = fork();
    REQUIRE(owner >= 0);
    if (!owner) {
      dup2(startup[1], 1);
      close(startup[0]);
      close(startup[1]);
      setenv("SOFTLINE_CHAT_APPROVAL_DEMO", "0", 1);
      setenv("SHELL", executable, 1);
      execl(owner_path, owner_path, "0", (char *)NULL);
      _exit(127);
    }
    close(startup[1]);
    startup[1] = -1;
    output = fdopen(startup[0], "r");
    REQUIRE(output);
    startup[0] = -1;
    REQUIRE(fgets(line, sizeof(line), output) &&
            sscanf(line, "LISTEN 127.0.0.1 %d", &port) == 1);
  }
  bytes = calloc(capacity + 1, 1);
  REQUIRE(bytes);
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  REQUIRE(!openpty(&master, &slave, NULL, NULL, &size));
  REQUIRE(fcntl(master, F_SETFL, O_NONBLOCK) == 0);
  started = seconds();
  frontend = fork();
  REQUIRE(frontend >= 0);
  if (!frontend) {
    sl_config_t config;
    sl_t *sl;
    struct termios before, after;
    struct sockaddr_in address;
    int result, exit_status = -1, valid;
    alarm(5);
    close(master);
    sl_config_init(&config);
    config.input_fd = config.output_fd = slave;
    sl = sl_create_with_config(&config);
    if (!sl || tcgetattr(slave, &before) ||
        sl_set_buffer(sl, "kept draft") != SL_OK)
      _exit(2);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port);
    address.sin_addr.s_addr = htonl(0x7f000001UL);
    result = remote ? demo_terminal_remote(sl, &address, &exit_status)
                    : sl_terminal_local(sl, executable, &exit_status);
    valid = result == SL_OK && exit_status == 7 &&
            !strcmp(sl_buffer(sl), "kept draft") && !tcgetattr(slave, &after) &&
            before.c_iflag == after.c_iflag &&
            before.c_oflag == after.c_oflag && before.c_lflag == after.c_lflag;
    sl_destroy(sl);
    close(slave);
    _exit(valid ? 0 : 1);
  }
  close(slave);
  slave = -1;
  p.fd = master;
  p.events = POLLIN;
  p.revents = 0;
  while (seconds() - started < 1.5) {
    char chunk[2048];
    ssize_t n;
    pid_t got;
    REQUIRE(poll(&p, 1, 10) >= 0 || errno == EINTR);
    n = read(master, chunk, sizeof(chunk));
    if (n > 0 && used < capacity) {
      size_t copy = (size_t)n < capacity - used ? (size_t)n : capacity - used;
      memcpy(bytes + used, chunk, copy);
      used += copy;
      if (!job) {
        char *header = strstr(bytes, "HELD ");
        long number;
        if (header && strchr(header, '\n') &&
            sscanf(header, "HELD %ld", &number) == 1)
          job = (pid_t)number;
      }
    }
    if (frontend > 0) {
      got = waitpid(frontend, &status, WNOHANG);
      if (got == frontend) {
        frontend = 0;
        finished = 1;
      }
    }
    if (finished && n <= 0)
      break;
  }
  if (!finished)
    fprintf(stderr, "handoff exceeded bounded drain: %.3fs (%s)\n",
            seconds() - started, mode);
  REQUIRE(finished && WIFEXITED(status) && !WEXITSTATUS(status));
  REQUIRE(job > 0);
  REQUIRE(contains(bytes, used, binary_tail, sizeof(binary_tail) - 1));
  REQUIRE(contains(bytes, used, tail_end, sizeof(tail_end) - 1));
cleanup:
  if (frontend > 0) {
    kill(frontend, SIGKILL);
    waitpid(frontend, &status, 0);
  }
  if (owner > 0) {
    kill(owner, SIGTERM);
    waitpid(owner, &status, 0);
  }
  if (job > 0 && waitpid(job, &status, WNOHANG) == 0) {
    kill(job, SIGKILL);
    waitpid(job, &status, 0);
  }
  if (master >= 0)
    close(master);
  if (slave >= 0)
    close(slave);
  if (output)
    fclose(output);
  if (startup[0] >= 0)
    close(startup[0]);
  if (startup[1] >= 0)
    close(startup[1]);
  free(bytes);
  (void)prctl(PR_SET_CHILD_SUBREAPER, 0);
  return failed;
}
int main(int argc, char **argv) {
  if (argc == 2 && !strcmp(argv[1], "-i"))
    return shell_fixture();
  if (argc != 3)
    return 2;
  if (!strcmp(argv[2], "budget"))
    return drain_budget();
  signal(SIGPIPE, SIG_IGN);
  return run(argv[0], argv[1], argv[2]);
}
