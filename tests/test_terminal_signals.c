#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include "termios_snapshot.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <softline/softline.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
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
      fprintf(stderr, "signal line %d: %s\n", __LINE__, #c);                   \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static int notice, slave_fd, exit_on_open, announced, nested;
static int check_stack, check_ignore;
static char alternate_stack[32768];
static struct termios baseline;
static volatile sig_atomic_t handled, inject_restore, restore_interrupted;
static sl_t *editor;
#if defined(SOFTLINE_TEST_WRAP_TERMIOS)
static int inject_tc, inject_flags, setup_failed;
int __real_tcsetattr(int, int, const struct termios *);
int __wrap_tcsetattr(int fd, int action, const struct termios *t) {
  if (inject_tc && !--inject_tc) {
    setup_failed = 1;
    errno = EIO;
    return -1;
  }
  if (inject_restore && (t->c_lflag & ICANON)) {
    inject_restore = 0;
    restore_interrupted = 1;
    errno = EINTR;
    return -1;
  }
  return __real_tcsetattr(fd, action, t);
}
int __real_fcntl(int, int, ...);
int __wrap_fcntl(int fd, int command, ...) {
  va_list args;
  int argument;
  if (command == F_GETFL || command == F_GETFD)
    return __real_fcntl(fd, command);
  va_start(args, command);
  argument = va_arg(args, int);
  va_end(args);
  if (command == F_SETFL && inject_flags && !--inject_flags) {
    setup_failed = 1;
    errno = EIO;
    return -1;
  }
  return __real_fcntl(fd, command, argument);
}
#endif
static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static void secondary(int signo) {
  struct termios t;
  char byte = signo == SIGUSR1 && !tcgetattr(slave_fd, &t) &&
                      test_termios_equal(&t, &baseline)
                  ? 'N'
                  : 'F';
  (void)write(notice, &byte, 1);
}
static void custom(int signo, siginfo_t *info, void *context) {
  struct termios t;
  char byte;
  if (check_stack) {
    stack_t stack;
    sigset_t mask;
    if (sigaltstack(NULL, &stack) || !(stack.ss_flags & SS_ONSTACK) ||
        sigprocmask(SIG_SETMASK, NULL, &mask) ||
        sigismember(&mask, SIGUSR2) != 1) {
      (void)write(notice, "F", 1);
      handled = 1;
      return;
    }
  }
  (void)context;
  byte = signo == SIGTERM && info->si_pid == getppid() &&
                 !tcgetattr(slave_fd, &t) && test_termios_equal(&t, &baseline)
             ? 'C'
             : 'F';
  (void)write(notice, &byte, 1);
  if (nested)
    (void)raise(SIGUSR1);
  handled = 1;
}
static void idle(sl_t *sl, void *context) {
  (void)context;
  if (!announced++) {
    (void)write(notice, "R", 1);
  }
  if (handled)
    (void)sl_cancel(sl);
}
static int adapter(void *context, sl_terminal_event_t *e) {
  (void)context;
  if (e->operation == SL_TERMINAL_OPEN) {
    (void)write(notice, "R", 1);
    if (check_ignore) {
      struct sigaction a;
      if (sigaction(SIGTERM, NULL, &a) || a.sa_handler != SIG_IGN)
        return SL_ERROR;
    }
    if (exit_on_open == 1)
      exit(42);
    if (exit_on_open == 2) {
      inject_restore = 1;
      return SL_ERROR_IO;
    }
    if (exit_on_open == 3) {
      int fd;
      close(slave_fd);
      fd = open("/dev/null", O_RDWR | O_NONBLOCK);
      if (fd < 0 || dup2(fd, slave_fd) < 0)
        return SL_ERROR;
      if (fd != slave_fd)
        close(fd);
      return SL_ERROR_IO;
    }
  }
  if (e->operation == SL_TERMINAL_STEP) {
    e->ready = 0;
    e->fd = -1;
    e->timeout_ms = 1;
    if (handled) {
      struct termios t;
      if (tcgetattr(slave_fd, &t) || (t.c_lflag & ICANON))
        return SL_ERROR;
      return SL_ERROR_IO;
    }
  }
  return SL_OK;
}
static int wait_child(pid_t pid, int *status, int options) {
  double due = now() + 3;
  struct timespec t = {0, 1000000};
  do {
    pid_t result = waitpid(pid, status, options | WNOHANG);
    if (result == pid)
      return 0;
    if (result < 0)
      return 1;
    (void)nanosleep(&t, NULL);
  } while (now() < due);
  (void)kill(pid, SIGKILL);
  (void)waitpid(pid, status, 0);
  return 1;
}
static int read_notice(int fd, char expected) {
  struct pollfd p;
  char byte;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  return poll(&p, 1, 3000) != 1 || read(fd, &byte, 1) != 1 || byte != expected;
}
static int run(int signo, int kind, int readline_mode) {
  int master, slave, pipefd[2], status, flags, output,
      output_master = -1, output_flags, after_flags;
  pid_t pid;
  struct termios before, after = {0}, before_output;
  struct winsize size;
  memset(&size, 0, sizeof(size));
  size.ws_row = 24;
  size.ws_col = 80;
  CHECK(!openpty(&master, &slave, NULL, NULL, &size) && !pipe(pipefd));
  CHECK(!tcgetattr(slave, &before));
  output = slave;
  if (readline_mode >= 2) {
    CHECK(!openpty(&output_master, &output, NULL, NULL, &size));
    CHECK(!tcgetattr(output, &before_output));
    before_output.c_iflag ^= IXON;
#if defined(TABDLY) && defined(TAB3)
    if (kind == 10)
      before_output.c_oflag = (before_output.c_oflag & ~TABDLY) | TAB3;
#endif
    CHECK(!tcsetattr(output, TCSANOW, &before_output));
  }
  CHECK(!tcgetattr(output, &before_output));
  output_flags = fcntl(output, F_GETFL);
  CHECK(output_flags >= 0);
  flags = fcntl(slave, F_GETFL);
  CHECK(flags >= 0);
  pid = fork();
  CHECK(pid >= 0);
  if (!pid) {
    sl_config_t c;
    struct rlimit core = {0, 0};
    struct sigaction a;
    int result;
    close(master);
    if (output_master >= 0)
      close(output_master);
    close(pipefd[0]);
    notice = pipefd[1];
    slave_fd = slave;
    baseline = before;
    handled = announced = 0;
    exit_on_open = kind == 3 ? 1 : kind == 4 ? 2 : kind == 8 ? 3 : 0;
    nested = kind == 5;
    check_stack = kind == 6;
    check_ignore = kind == 7;
    (void)setrlimit(RLIMIT_CORE, &core);
    if (signo == SIGTSTP)
      (void)setpgid(0, 0);
    memset(&a, 0, sizeof(a));
    sigemptyset(&a.sa_mask);
    a.sa_handler = SIG_DFL;
    if (check_ignore)
      a.sa_handler = SIG_IGN;
    if (kind == 1 || kind == 2 || kind == 5 || kind == 6) {
      a.sa_sigaction = custom;
      a.sa_flags = SA_SIGINFO | (kind == 2 ? SA_RESETHAND : 0);
      if (check_stack) {
        stack_t stack;
        stack.ss_sp = alternate_stack;
        stack.ss_size = sizeof(alternate_stack);
        stack.ss_flags = 0;
        if (sigaltstack(&stack, NULL))
          _exit(19);
        a.sa_flags |= SA_ONSTACK | SA_RESTART;
        sigaddset(&a.sa_mask, SIGUSR2);
      }
    }
    if (signo && sigaction(signo, &a, NULL))
      _exit(10);
    if (nested && (signal(SIGUSR1, secondary) == SIG_ERR))
      _exit(18);
    sl_config_init(&c);
    c.input_fd = slave;
    c.output_fd = output;
    editor = sl_create_with_config(&c);
    if (!editor)
      _exit(11);
#if defined(SOFTLINE_TEST_WRAP_TERMIOS)
    if (kind >= 9) {
      inject_tc = kind == 9 ? 1 : kind == 10 ? 2 : 0;
      inject_flags = kind >= 11 ? kind - 10 : 0;
      setup_failed = 0;
      (void)write(notice, "R", 1);
    }
#endif
    if (readline_mode == 1 || readline_mode == 3) {
      char *line;
      if (sl_set_idle_callback(editor, idle, NULL))
        _exit(12);
      line = sl_readline(editor, "> ");
      if (kind >= 9 && sl_last_readline_status(editor) != SL_READLINE_ERROR)
        _exit(21);
      if (line)
        sl_free_string(editor, line);
      result = SL_ERROR_IO;
    } else
      result = sl_terminal_handoff(editor, adapter, NULL);
    if (signo) {
      if (sigaction(signo, NULL, &a))
        _exit(13);
      if ((kind == 1 || kind == 5 || kind == 6) && a.sa_sigaction != custom)
        _exit(14);
      if (kind == 2 && a.sa_handler != SIG_DFL)
        _exit(15);
      if (kind == 7 && a.sa_handler != SIG_IGN)
        _exit(23);
    }
    sl_destroy(editor);
    editor = NULL;
    if (kind == 4 && !restore_interrupted)
      _exit(16);
    if (kind == 8 && !(fcntl(slave_fd, F_GETFL) & O_NONBLOCK))
      _exit(20);
#if defined(SOFTLINE_TEST_WRAP_TERMIOS)
    if (kind >= 9 && !setup_failed)
      _exit(22);
#endif
    _exit(result == (kind == 7 ? SL_TERMINAL_ESCAPED : SL_ERROR_IO) ? 0 : 17);
  }
  close(pipefd[1]);
  CHECK(!read_notice(pipefd[0], 'R'));
  if (kind == 7) {
    CHECK(!kill(pid, SIGTERM) && write(master, "~.", 2) == 2);
  }
  if (kind < 3 || kind == 5 || kind == 6) {
    CHECK(!kill(pid, signo));
    if (kind == 1 || kind == 2 || kind == 5 || kind == 6)
      CHECK(!read_notice(pipefd[0], 'C'));
    if (kind == 5)
      CHECK(!read_notice(pipefd[0], 'N'));
    if (signo == SIGTSTP) {
      double due;
      CHECK(!wait_child(pid, &status, WUNTRACED) && WIFSTOPPED(status));
      after_flags = fcntl(slave, F_GETFL);
      CHECK(after_flags >= 0 && !((after_flags ^ flags) & TEST_TTY_FILE_FLAGS));
      CHECK(!tcgetattr(slave, &after) && test_termios_equal(&before, &after));
      CHECK(!kill(pid, SIGCONT));
      due = now() + 2;
      do {
        struct timespec t = {0, 1000000};
        (void)nanosleep(&t, NULL);
        CHECK(!tcgetattr(slave, &after));
      } while ((after.c_lflag & ICANON) && now() < due);
      CHECK(!(after.c_lflag & ICANON));
      CHECK(!kill(pid, SIGTERM));
    }
  }
  CHECK(!wait_child(pid, &status, 0));
  after_flags = fcntl(slave, F_GETFL);
  if (tcgetattr(slave, &after) || !test_termios_equal(&before, &after) ||
      after_flags < 0 || ((after_flags ^ flags) & TEST_TTY_FILE_FLAGS)) {
    fprintf(stderr,
            "signal %d case %d readline %d status %d\n"
            "before iflag=%lx oflag=%lx cflag=%lx lflag=%lx ispeed=%lu "
            "ospeed=%lu flags=%x\n"
            "after  iflag=%lx oflag=%lx cflag=%lx lflag=%lx ispeed=%lu "
            "ospeed=%lu flags=%x\n",
            signo, kind, readline_mode, status, (unsigned long)before.c_iflag,
            (unsigned long)before.c_oflag, (unsigned long)before.c_cflag,
            (unsigned long)before.c_lflag, (unsigned long)cfgetispeed(&before),
            (unsigned long)cfgetospeed(&before), flags,
            (unsigned long)after.c_iflag, (unsigned long)after.c_oflag,
            (unsigned long)after.c_cflag, (unsigned long)after.c_lflag,
            (unsigned long)cfgetispeed(&after),
            (unsigned long)cfgetospeed(&after), after_flags);
    return 1;
  }
  after_flags = fcntl(output, F_GETFL);
  CHECK(after_flags >= 0 &&
        !((after_flags ^ output_flags) & TEST_TTY_FILE_FLAGS));
  CHECK(!tcgetattr(output, &after) &&
        test_termios_equal(&before_output, &after));
  if (kind == 3)
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 42);
  else if (kind == 1 || kind == 2 || kind == 4 || kind == 5 || kind == 6 ||
           kind == 7 || kind == 8 || kind >= 9)
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  else
    CHECK(WIFSIGNALED(status) &&
          WTERMSIG(status) == (signo == SIGTSTP ? SIGTERM : signo));
  close(master);
  close(slave);
  close(pipefd[0]);
  if (output_master >= 0) {
    close(output_master);
    close(output);
  }
  return 0;
}
int main(void) {
  static const int sigs[] = {SIGINT,    SIGTERM, SIGHUP,  SIGPIPE,
                             SIGALRM,   SIGUSR1, SIGUSR2, SIGQUIT,
                             SIGVTALRM, SIGPROF, SIGXCPU, SIGXFSZ};
  size_t i;
  for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i)
    CHECK(!run(sigs[i], 0, 0) && !run(sigs[i], 0, 1));
  CHECK(!run(SIGTERM, 1, 0) && !run(SIGTERM, 2, 0) && !run(SIGTERM, 1, 1));
  CHECK(!run(SIGTERM, 5, 0));
  CHECK(!run(SIGTERM, 6, 0) && !run(SIGTERM, 0, 2) && !run(SIGTERM, 1, 2));
  CHECK(!run(SIGTERM, 7, 0));
  CHECK(!run(SIGTSTP, 0, 0) && !run(SIGTSTP, 0, 1));
  CHECK(!run(0, 3, 0));
  CHECK(!run(0, 8, 0));
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  CHECK(!run(SIGRTMIN, 0, 0) && !run(SIGRTMAX, 0, 1));
#endif
#ifdef SIGPWR
  CHECK(!run(SIGPWR, 0, 0));
#endif
#if defined(SOFTLINE_TEST_WRAP_TERMIOS)
  CHECK(!run(0, 4, 0));
  CHECK(!run(0, 4, 2));
  CHECK(!run(0, 9, 2) && !run(0, 10, 2) && !run(0, 11, 2) && !run(0, 12, 2) &&
        !run(0, 13, 2));
  CHECK(!run(0, 9, 1));
#if defined(TABDLY) && defined(TAB3)
  CHECK(!run(0, 10, 3));
#endif
#endif
  return 0;
}
