#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include "termios_snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "supervisor line %d: %s (%s)\n", __LINE__, #c,           \
              strerror(errno));                                                \
      return 1;                                                                \
    }                                                                          \
  } while (0)
typedef struct ready {
  pid_t pid, group;
  dev_t input, output;
  int foreground;
} ready_t;
typedef struct fixture {
  pid_t session, supervisor;
  int master, slave, output_master, output_slave, notice, control, states;
  int input_flags, output_flags;
  struct termios input, output;
  ready_t host;
} fixture_t;
static const char *launcher, *self;
static pid_t active_session, active_supervisor, active_host;
static int resize_notice;
static int stop_signal, kernel_pending;
static volatile sig_atomic_t continued;
static void resumed(int number) {
  (void)number;
  continued = 1;
}
static void resized(int number) {
  int saved = errno;
  (void)number;
  (void)write(resize_notice, "W", 1);
  errno = saved;
}
static void cleanup(void) {
  if (active_host > 0) {
    kill(-active_host, SIGKILL);
    kill(active_host, SIGKILL);
  }
  if (active_supervisor > 0) {
    kill(-active_supervisor, SIGKILL);
    kill(active_supervisor, SIGKILL);
  }
  if (active_session > 0) {
    int status;
    kill(active_session, SIGKILL);
    while (waitpid(active_session, &status, 0) < 0 && errno == EINTR) {
    }
  }
}
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static int exact(int fd, void *value, size_t length, int writing) {
  char *bytes = value;
  double due = now() + 4;
  while (length && now() < due) {
    struct pollfd p;
    ssize_t n;
    p.fd = fd;
    p.events = writing ? POLLOUT : POLLIN;
    if (poll(&p, 1, 20) <= 0)
      continue;
    n = writing ? write(fd, bytes, length) : read(fd, bytes, length);
    if (n > 0) {
      bytes += n;
      length -= (size_t)n;
    } else if (!n || (errno != EINTR && errno != EAGAIN))
      return 1;
  }
  return length != 0;
}
static int host(int argc, char **argv) {
  struct termios raw;
  struct stat input, output;
  struct rlimit limit = {0, 0};
  ready_t r;
  int notify, command, i;
  char byte;
  CHECK(argc == 5);
  notify = atoi(argv[3]);
  command = atoi(argv[4]);
  if (!strcmp(argv[2], "replacement"))
    _exit(43);
  CHECK(!tcgetattr(0, &raw));
  raw.c_iflag &=
      ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  raw.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  raw.c_oflag &= ~OPOST;
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  CHECK(!tcsetattr(0, TCSANOW, &raw));
  CHECK(!tcgetattr(1, &raw));
  raw.c_oflag &= ~OPOST;
  CHECK(!tcsetattr(1, TCSANOW, &raw));
  CHECK(!fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK));
  CHECK(!fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK));
  if (!strcmp(argv[2], "ignore"))
    CHECK(signal(SIGTERM, SIG_IGN) != SIG_ERR);
  if (!strcmp(argv[2], "inherited-ignore") || !strcmp(argv[2], "keep-ignore")) {
    struct sigaction action;
    CHECK(!sigaction(SIGUSR1, NULL, &action) && action.sa_handler == SIG_IGN);
  }
  if (!strcmp(argv[2], "inherited-blocked") ||
      !strcmp(argv[2], "keep-blocked")) {
    sigset_t mask;
    CHECK(!sigprocmask(SIG_SETMASK, NULL, &mask) &&
          sigismember(&mask, SIGUSR1) == 1);
  }
  if (!strcmp(argv[2], "resize") || !strcmp(argv[2], "control") ||
      !strcmp(argv[2], "inherited-ignore") ||
      !strcmp(argv[2], "inherited-blocked")) {
    resize_notice = notify;
    CHECK(signal(!strcmp(argv[2], "resize") ? SIGWINCH : SIGUSR1, resized) !=
          SIG_ERR);
  }
  if (!strcmp(argv[2], "inherited-blocked")) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    CHECK(!sigprocmask(SIG_UNBLOCK, &mask, NULL));
  }
  CHECK(!setrlimit(RLIMIT_CORE, &limit));
  CHECK(!fstat(0, &input) && !fstat(1, &output));
  memset(&r, 0, sizeof(r));
  r.pid = getpid();
  r.group = getpgrp();
  r.input = input.st_rdev;
  r.output = output.st_rdev;
  r.foreground = tcgetpgrp(0) == getpgrp();
  CHECK(!exact(notify, &r, sizeof(r), 1));
  if (!strcmp(argv[2], "keep-blocked")) {
    double due = now() + 2;
    sigset_t mask;
    struct timespec delay = {0, 1000000};
    do {
      CHECK(!sigpending(&mask));
      if (sigismember(&mask, SIGUSR1) == 1)
        break;
      nanosleep(&delay, NULL);
    } while (now() < due);
    CHECK(sigismember(&mask, SIGUSR1) == 1);
    CHECK(!exact(notify, "W", 1, 1));
  }
  if (!strcmp(argv[2], "bytes")) {
    unsigned char block[256];
    CHECK(!exact(0, block, sizeof(block), 0));
    for (i = 0; i < 256; ++i)
      CHECK(block[i] == (unsigned char)i);
    for (i = 0; i < 257; ++i)
      CHECK(!exact(1, block, sizeof(block), 1));
    return 9;
  }
  CHECK(!exact(command, &byte, 1, 0));
  if (!strcmp(argv[2], "resize")) {
    struct winsize size;
    CHECK(!ioctl(0, TIOCGWINSZ, &size));
    CHECK(size.ws_row == 14 && size.ws_col == 54);
  }
  if (!strcmp(argv[2], "exit"))
    return 7;
  if (!strcmp(argv[2], "close")) {
    close(0);
    close(1);
    close(2);
    _exit(42);
  }
  if (!strcmp(argv[2], "exec")) {
    execl(argv[0], argv[0], "host", "replacement", argv[3], argv[4],
          (char *)NULL);
    return 1;
  }
  if (!strcmp(argv[2], "kill"))
    raise(SIGKILL);
  if (!strcmp(argv[2], "crash")) {
    signal(SIGSEGV, SIG_DFL);
    raise(SIGSEGV);
  }
  if (!strcmp(argv[2], "rawexit"))
    _exit(42);
  return 0;
}
static int wait_state(pid_t pid, int *status, int options) {
  double due = now() + 5;
  struct timespec delay = {0, 1000000};
  do {
    pid_t got = waitpid(pid, status, options | WNOHANG);
    if (got == pid)
      return 0;
    if (got < 0)
      return 1;
    nanosleep(&delay, NULL);
  } while (now() < due);
  kill(pid, SIGKILL);
  waitpid(pid, status, 0);
  return 1;
}
/* Model Make waiting in the launcher's original job, and an interactive shell
 * continuing that job in the background while retaining terminal ownership. */
static int nested_wait(pid_t wrapper, int background) {
  int status;
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = resumed;
  sigemptyset(&action.sa_mask);
  CHECK(!sigaction(SIGCONT, &action, NULL));
  for (;;) {
    pid_t got = waitpid(wrapper, &status, 0);
    if (got == wrapper)
      break;
    CHECK(got < 0 && errno == EINTR);
    if (background && continued) {
      struct termios baseline, owned, actual;
      sigset_t mask;
      pid_t other;
      int ignored;
      sigemptyset(&mask);
      sigaddset(&mask, SIGTTOU);
      CHECK(!sigprocmask(SIG_BLOCK, &mask, NULL));
      CHECK(!tcgetattr(0, &baseline));
      owned = baseline;
      owned.c_lflag ^= ECHO;
      other = fork();
      CHECK(other >= 0);
      if (!other) {
        if (setpgid(0, 0))
          _exit(1);
        for (;;)
          pause();
      }
      CHECK(!setpgid(other, other));
      CHECK(!tcsetattr(0, TCSANOW, &owned));
      CHECK(!tcsetpgrp(0, other));
      CHECK(!kill(wrapper, SIGCONT));
      CHECK(!wait_state(wrapper, &status, 0));
      CHECK(tcgetpgrp(0) == other);
      CHECK(!tcgetattr(0, &actual) && test_termios_equal(&actual, &owned));
      CHECK(!kill(other, SIGKILL));
      CHECK(!wait_state(other, &ignored, 0));
      CHECK(!tcsetpgrp(0, getpgrp()));
      CHECK(!tcsetattr(0, TCSANOW, &baseline));
      break;
    }
  }
  CHECK(WIFEXITED(status));
  return WEXITSTATUS(status);
}
/* A shell owns the controlling terminal beyond the lifetime of a launched
 * job. Model that here too: Darwin revokes the slave when its leader exits. */
static void session_job(int tty, int states, int control) {
  int gate[2], status;
  pid_t job, got;
  char byte;
  ssize_t n;
  if (setsid() < 0 || ioctl(tty, TIOCSCTTY, 0) || pipe(gate))
    _exit(124);
  job = fork();
  if (job < 0)
    _exit(124);
  if (!job) {
    close(gate[1]);
    close(states);
    if (read(gate[0], &byte, 1) != 1)
      _exit(124);
    close(gate[0]);
    return;
  }
  close(gate[0]);
  if (setpgid(job, job) || tcsetpgrp(tty, job) ||
      exact(states, &job, sizeof(job), 1) || write(gate[1], "R", 1) != 1) {
    kill(job, SIGKILL);
    _exit(124);
  }
  close(gate[1]);
  do {
    do {
      got = waitpid(job, &status, WUNTRACED);
    } while (got < 0 && errno == EINTR);
    if (got != job || exact(states, &status, sizeof(status), 1))
      _exit(124);
  } while (WIFSTOPPED(status));
  close(states);
  /* Keep the slave valid while the caller checks restoration. */
  do {
    n = read(control, &byte, 1);
  } while (n > 0 || (n < 0 && errno == EINTR));
  _exit(n < 0 ? 124 : 0);
}
static int launch(fixture_t *f, const char *mode, int split, int missing) {
  int notices[2], commands[2], states[2];
  struct winsize size = {24, 80, 0, 0};
  struct stat input, output;
  memset(f, 0, sizeof(*f));
  f->output_slave = f->output_master = -1;
  CHECK(!openpty(&f->master, &f->slave, NULL, NULL, &size));
  if (split)
    CHECK(!openpty(&f->output_master, &f->output_slave, NULL, NULL, &size));
  CHECK(!tcgetattr(f->slave, &f->input));
  f->input.c_iflag |= IXON | ICRNL;
  f->input.c_lflag |= ECHO | ICANON | ISIG;
  CHECK(!tcsetattr(f->slave, TCSANOW, &f->input));
  CHECK(!tcgetattr(split ? f->output_slave : f->slave, &f->output));
  CHECK(!fstat(f->slave, &input));
  CHECK(!fstat(split ? f->output_slave : f->slave, &output));
  f->input_flags = fcntl(f->slave, F_GETFL);
  f->output_flags = fcntl(split ? f->output_slave : f->slave, F_GETFL);
  CHECK(!pipe(notices) && !pipe(commands) && !pipe(states));
  f->session = fork();
  CHECK(f->session >= 0);
  if (!f->session) {
    char notify[32], control[32];
    close(notices[0]);
    close(commands[1]);
    close(states[0]);
    close(f->master);
    if (split)
      close(f->output_master);
    session_job(f->slave, states[1], commands[0]);
    if (dup2(f->slave, 0) < 0 ||
        dup2(split ? f->output_slave : f->slave, 1) < 0 || dup2(1, 2) < 0)
      _exit(124);
    close(f->slave);
    if (split)
      close(f->output_slave);
    snprintf(notify, sizeof(notify), "%d", notices[1]);
    snprintf(control, sizeof(control), "%d", commands[0]);
    if (!strcmp(mode, "inherited-ignore") || !strcmp(mode, "keep-ignore"))
      CHECK(signal(SIGUSR1, SIG_IGN) != SIG_ERR);
    if (!strcmp(mode, "inherited-blocked") || !strcmp(mode, "keep-blocked")) {
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, SIGUSR1);
      CHECK(!sigprocmask(SIG_BLOCK, &mask, NULL));
    }
    if (stop_signal) {
      char injection[32];
      snprintf(injection, sizeof(injection), "%d:%d", stop_signal,
               kernel_pending);
      CHECK(!setenv("SOFTLINE_TEST_STOP_SIGNAL", injection, 1));
    }
    if (!strcmp(mode, "nested") || !strcmp(mode, "background")) {
      pid_t wrapper = fork();
      if (wrapper < 0)
        _exit(1);
      if (wrapper > 0)
        _exit(nested_wait(wrapper, !strcmp(mode, "background")));
    }
    if (missing)
      execl(launcher, launcher, "--", "/softline/nonexistent-program",
            (char *)NULL);
    else
      execl(launcher, launcher, "--", self, "host", mode, notify, control,
            (char *)NULL);
    _exit(123);
  }
  close(notices[1]);
  close(commands[0]);
  close(states[1]);
  active_session = f->session;
  f->states = states[0];
  CHECK(!exact(f->states, &f->supervisor, sizeof(f->supervisor), 0));
  active_supervisor = f->supervisor;
  f->notice = notices[0];
  f->control = commands[1];
  if (!missing) {
    struct termios raw;
    CHECK(!exact(f->notice, &f->host, sizeof(f->host), 0));
    active_host = f->host.pid;
    CHECK(f->host.pid != f->supervisor && f->host.group == f->host.pid);
    CHECK(f->host.foreground && f->host.input == input.st_rdev &&
          f->host.output == output.st_rdev);
    CHECK(!tcgetattr(f->slave, &raw) && !(raw.c_lflag & ICANON));
  }
  return 0;
}
static int restored(fixture_t *f) {
  struct termios input, output;
  int flags;
  CHECK(!tcgetattr(f->slave, &input) && test_termios_equal(&input, &f->input));
  CHECK(
      !tcgetattr(f->output_slave >= 0 ? f->output_slave : f->slave, &output) &&
      test_termios_equal(&output, &f->output));
  flags = fcntl(f->slave, F_GETFL);
  if (flags < 0 || ((flags ^ f->input_flags) & TEST_TTY_FILE_FLAGS))
    fprintf(stderr, "supervisor input flags expected %x, got %x\n",
            f->input_flags, flags);
  CHECK(flags >= 0 && !((flags ^ f->input_flags) & TEST_TTY_FILE_FLAGS));
  flags = fcntl(f->output_slave >= 0 ? f->output_slave : f->slave, F_GETFL);
  CHECK(flags >= 0 && !((flags ^ f->output_flags) & TEST_TTY_FILE_FLAGS));
  return 0;
}
static void drain_output(int fd) {
  struct pollfd p;
  char bytes[1024];
  p.fd = fd;
  p.events = POLLIN;
  while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN) &&
         read(fd, bytes, sizeof(bytes)) > 0) {
  }
}
static int finish_status(fixture_t *f, int status, int code) {
  int session_status, restore_status;
  active_supervisor = 0;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == code);
  restore_status = restored(f);
  if (f->host.pid) {
    CHECK(kill(f->host.pid, 0) < 0 && errno == ESRCH);
  }
  active_host = 0;
  /* A PTY may wait for outstanding output when its session is revoked. */
  drain_output(f->master);
  if (f->output_master >= 0)
    drain_output(f->output_master);
  close(f->notice);
  close(f->control);
  close(f->states);
  CHECK(!wait_state(f->session, &session_status, 0));
  active_session = 0;
  CHECK(WIFEXITED(session_status) && WEXITSTATUS(session_status) == 0);
  close(f->slave);
  close(f->master);
  if (f->output_slave >= 0) {
    close(f->output_slave);
    close(f->output_master);
  }
  return restore_status;
}
static int finish(fixture_t *f, int code) {
  int status;
  CHECK(!exact(f->states, &status, sizeof(status), 0));
  return finish_status(f, status, code);
}
static int shutdown_races(void) {
  static const int termination[] = {SIGHUP, SIGTERM, SIGINT, SIGQUIT};
  size_t i;
  for (i = 0; i < sizeof(termination) / sizeof(termination[0]); ++i) {
    for (kernel_pending = 0; kernel_pending < 2; ++kernel_pending) {
      fixture_t f;
      int status;
      stop_signal = termination[i];
      CHECK(!launch(&f, "idle", 0, 0));
      CHECK(!kill(f.host.pid, SIGSTOP));
      CHECK(!exact(f.states, &status, sizeof(status), 0));
      CHECK(!WIFSTOPPED(status));
      CHECK(!finish_status(&f, status, 128 + stop_signal));
    }
  }
  stop_signal = SIGTERM;
  for (kernel_pending = 0; kernel_pending < 2; ++kernel_pending) {
    fixture_t f;
    int status;
    CHECK(!launch(&f, "ignore", 0, 0));
    CHECK(!kill(f.host.pid, SIGSTOP));
    CHECK(!exact(f.states, &status, sizeof(status), 0));
    CHECK(!WIFSTOPPED(status));
    CHECK(!finish_status(&f, status, 128 + SIGTERM));
  }
  return 0;
}
static int inherited_signals(void) {
  static const char *modes[] = {"inherited-ignore", "inherited-blocked",
                                "keep-ignore", "keep-blocked"};
  size_t i;
  for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    fixture_t f;
    char byte;
    CHECK(!launch(&f, modes[i], 0, 0));
    CHECK(!kill(f.supervisor, SIGUSR1));
    if (strcmp(modes[i], "keep-ignore"))
      CHECK(!exact(f.notice, &byte, 1, 0) && byte == 'W');
    CHECK(!exact(f.control, "x", 1, 1));
    CHECK(!finish(&f, 0));
  }
  return 0;
}
static int stopped(int directed) {
  fixture_t f;
  struct termios raw;
  int status;
  double due;
  CHECK(!launch(&f,
                directed == 2   ? "nested"
                : directed == 3 ? "background"
                                : "idle",
                0, 0));
  CHECK(!kill(directed == 1 ? f.supervisor : f.host.pid,
              directed == 1 ? SIGTSTP : SIGSTOP));
  CHECK(!exact(f.states, &status, sizeof(status), 0));
  CHECK(WIFSTOPPED(status));
  CHECK(!restored(&f));
  CHECK(tcgetpgrp(f.master) == f.supervisor);
  CHECK(!kill(directed == 2 ? -f.supervisor : f.supervisor, SIGCONT));
  if (directed == 3)
    return finish(&f, 125);
  due = now() + 2;
  do {
    struct timespec delay = {0, 1000000};
    CHECK(!tcgetattr(f.slave, &raw));
    if (!(raw.c_lflag & ICANON) && tcgetpgrp(f.master) == f.host.pid)
      break;
    nanosleep(&delay, NULL);
  } while (now() < due);
  CHECK(!(raw.c_lflag & ICANON));
  CHECK(!exact(f.control, "x", 1, 1));
  return finish(&f, 0);
}
static int binary(void) {
  fixture_t f;
  unsigned char input[256], block[256];
  int i, j;
  CHECK(!launch(&f, "bytes", 0, 0));
  for (i = 0; i < 256; ++i)
    input[i] = (unsigned char)i;
  CHECK(!exact(f.master, input, sizeof(input), 1));
  for (i = 0; i < 257; ++i) {
    CHECK(!exact(f.master, block, sizeof(block), 0));
    for (j = 0; j < 256; ++j)
      CHECK(block[j] == input[j]);
  }
  return finish(&f, 9);
}
static int nonterminal(void) {
  pid_t pid = fork();
  int status;
  CHECK(pid >= 0);
  if (!pid) {
    int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(null, 1);
    dup2(null, 2);
    if (null > 2)
      close(null);
    execl(launcher, launcher, "--", self, "passthrough", (char *)NULL);
    _exit(1);
  }
  CHECK(!wait_state(pid, &status, 0));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 23);
  return 0;
}
int main(int argc, char **argv) {
  static const char *modes[] = {"idle", "exit", "rawexit", "close",
                                "exec", "kill", "crash"};
  static const int codes[] = {0, 7, 42, 42, 43, 128 + SIGKILL, 128 + SIGSEGV};
  size_t i;
  fixture_t f;
  if (argc > 1 && !strcmp(argv[1], "host"))
    return host(argc, argv);
  if (argc == 2 && !strcmp(argv[1], "passthrough"))
    return 23;
  CHECK(argc == 2 || argc == 3);
  CHECK(!atexit(cleanup));
  launcher = argv[1];
  self = argv[0];
  if (argc == 3) {
    if (!strcmp(argv[2], "shutdown"))
      return shutdown_races();
    CHECK(!strcmp(argv[2], "signals"));
    return inherited_signals();
  }
  for (i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    CHECK(!launch(&f, modes[i], i == 3, 0));
    CHECK(!exact(f.control, "x", 1, 1));
    CHECK(!finish(&f, codes[i]));
  }
  CHECK(!launch(&f, "idle", 0, 1));
  CHECK(!finish(&f, 127));
  CHECK(!launch(&f, "idle", 0, 0));
  CHECK(!kill(f.supervisor, SIGTERM));
  CHECK(!finish(&f, 128 + SIGTERM));
  CHECK(!launch(&f, "ignore", 0, 0));
  CHECK(!kill(f.supervisor, SIGTERM));
  CHECK(!finish(&f, 128 + SIGTERM));
  CHECK(!stopped(0) && !stopped(1) && !stopped(2) && !stopped(3));
  CHECK(!launch(&f, "resize", 0, 0));
  {
    struct winsize size = {14, 54, 0, 0};
    char byte;
    CHECK(!ioctl(f.master, TIOCSWINSZ, &size));
    CHECK(!exact(f.notice, &byte, 1, 0) && byte == 'W');
    CHECK(!exact(f.control, "x", 1, 1));
    CHECK(!finish(&f, 0));
  }
  CHECK(!binary() && !nonterminal());
  CHECK(!launch(&f, "control", 0, 0));
  {
    char byte;
    CHECK(!kill(f.supervisor, SIGUSR1));
    CHECK(!exact(f.notice, &byte, 1, 0) && byte == 'W');
    CHECK(!exact(f.control, "x", 1, 1));
    CHECK(!finish(&f, 0));
  }
  return 0;
}
