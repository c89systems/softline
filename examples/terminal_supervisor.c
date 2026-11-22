#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* A launcher, not a relay: the application keeps the real terminal. Only this
 * small, single-threaded process retains recovery ownership across exec. */
#define SIGNAL_LIMIT 128
typedef struct terminal {
  int fd, flags, tty, running_flags;
  struct termios original, running;
} terminal_t;
static terminal_t terminals[3];
static int wakeup[2] = {-1, -1};
static volatile sig_atomic_t pending[SIGNAL_LIMIT];
static int signals[SIGNAL_LIMIT], signal_count;
static struct sigaction inherited[SIGNAL_LIMIT];
static sigset_t original_mask;
static pid_t child, foreground;
static int transferred, child_reaped;

static double milliseconds(void) {
  struct timespec t;
  (void)clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int attributes(int fd, const struct termios *value) {
  int result;
  do {
    result = tcsetattr(fd, TCSANOW, value);
  } while (result < 0 && errno == EINTR);
  return result;
}
static int flags(int fd, int value) {
  int result;
  do {
    result = fcntl(fd, F_SETFL, value);
  } while (result < 0 && errno == EINTR);
  return result;
}
static int restore(int running) {
  int i, result = 0;
  for (i = 2; i >= 0; --i) {
    terminal_t *t = terminals + i;
    if (t->fd < 0)
      continue;
    if (t->tty && attributes(t->fd, running ? &t->running : &t->original))
      result = -1;
    if (t->flags >= 0 && flags(t->fd, running ? t->running_flags : t->flags))
      result = -1;
  }
  return result;
}
static int remember_running(void) {
  int i;
  for (i = 0; i < 3; ++i) {
    terminal_t *t = terminals + i;
    if (t->fd < 0)
      continue;
    t->running_flags = fcntl(t->fd, F_GETFL);
    if (t->running_flags < 0 || (t->tty && tcgetattr(t->fd, &t->running)))
      return -1;
  }
  return 0;
}
static int give_terminal(pid_t group) {
  int result;
  do {
    result = tcsetpgrp(terminals[0].fd, group);
  } while (result < 0 && errno == EINTR);
  return result;
}
static void notification(int number) {
  int saved_errno = errno;
  char byte = 0;
  pending[number] = 1;
  (void)write(wakeup[1], &byte, 1);
  errno = saved_errno;
}
static int add_signal(int number) {
  struct sigaction action;
  if (number <= 0 || number >= SIGNAL_LIMIT || signal_count == SIGNAL_LIMIT) {
    errno = EOVERFLOW;
    return -1;
  }
  memset(&action, 0, sizeof(action));
  action.sa_handler = notification;
  sigfillset(&action.sa_mask);
  if (sigaction(number, NULL, &inherited[signal_count]))
    return -1;
  /* The child retains inherited policy. The forwarding parent must remain
   * reachable when the application later installs a handler of its own. */
  if (sigaction(number, &action, NULL))
    return -1;
  signals[signal_count++] = number;
  return 0;
}
static int prepare_signals(void) {
  static const int watched[] = {SIGCHLD,
                                SIGCONT,
                                SIGWINCH,
                                SIGTSTP,
                                SIGTTIN,
                                SIGINT,
                                SIGQUIT,
                                SIGHUP,
                                SIGTERM,
                                SIGPIPE,
                                SIGALRM,
                                SIGUSR1,
                                SIGUSR2,
                                SIGVTALRM,
                                SIGPROF,
                                SIGXCPU,
                                SIGXFSZ
#ifdef SIGIO
                                ,
                                SIGIO
#endif
#ifdef SIGPWR
                                ,
                                SIGPWR
#endif
#ifdef SIGSTKFLT
                                ,
                                SIGSTKFLT
#endif
  };
  size_t i;
  sigset_t all;
  sigfillset(&all);
  if (sigprocmask(SIG_BLOCK, &all, &original_mask))
    return -1;
  for (i = 0; i < sizeof(watched) / sizeof(watched[0]); ++i)
    if (add_signal(watched[i]))
      return -1;
#if defined(SIGRTMIN) && defined(SIGRTMAX)
  {
    int number;
    /* A runtime can reserve a signal within the advertised RT range. */
    for (number = SIGRTMIN; number <= SIGRTMAX; ++number)
      if (add_signal(number) && errno != EINVAL)
        return -1;
  }
#endif
  return 0;
}
static int suspend_host(void) {
  (void)kill(-child, SIGSTOP);
  if (remember_running() || restore(0) || give_terminal(foreground))
    return -1;
  transferred = 0;
  /* SIGSTOP works even for orphaned groups. The shell observes this launcher's
   * whole stopped job, including a Make parent. The host cannot change the
   * terminal while the shell owns it. Never steal it on a background resume. */
  if (kill(-foreground, SIGSTOP) || tcgetpgrp(terminals[0].fd) != foreground ||
      give_terminal(child))
    return -1;
  transferred = 1;
  if (restore(1))
    return -1;
  return kill(-child, SIGCONT);
}
static int collect_termination(void) {
  static const int termination[] = {SIGHUP, SIGTERM, SIGINT, SIGQUIT};
  sigset_t queued, one;
  size_t i;
  if (sigpending(&queued))
    return -1;
  for (i = 0; i < sizeof(termination) / sizeof(termination[0]); ++i) {
    int number = termination[i], received;
    if (sigismember(&queued, number) != 1)
      continue;
    /* Only termination signals are consumed here: unlike job-control signals,
     * they cannot be discarded by a concurrent SIGCONT. All are blocked on
     * this single owner thread, so sigwait consumes an already queued signal.
     */
    sigemptyset(&one);
    sigaddset(&one, number);
    if (sigwait(&one, &received))
      return -1;
    pending[received] = 1;
  }
  return 0;
}
static int supervise(void) {
  struct pollfd wait;
  int status, terminating = 0;
  double deadline = 0;
  sigset_t blocked, awake;
  int i;
  sigfillset(&blocked);
  awake = original_mask;
  /* The host owns the foreground group. Parent terminal operations must not
   * stop the recovery process. Forwarded signals must reach the parent even
   * when blocked at launch; the child's inherited mask remains unchanged. */
  for (i = 0; i < signal_count; ++i)
    sigdelset(&awake, signals[i]);
  sigaddset(&awake, SIGTTOU);
  wait.fd = wakeup[0];
  wait.events = POLLIN;
  for (;;) {
    pid_t got;
    int stop = 0, stopped = 0, timeout = -1;
    char bytes[128];
    (void)sigprocmask(SIG_BLOCK, &blocked, NULL);
    got = waitpid(child, &status, WNOHANG | WUNTRACED);
    if (got == child) {
      if (WIFEXITED(status)) {
        child_reaped = 1;
        return terminating ? 128 + terminating : WEXITSTATUS(status);
      }
      if (WIFSIGNALED(status)) {
        child_reaped = 1;
        return terminating ? 128 + terminating : 128 + WTERMSIG(status);
      }
      stopped = WIFSTOPPED(status);
    } else if (got < 0 && errno != EINTR)
      return -1;
    /* Reconcile already handled and kernel-queued termination before deciding
     * to stop the recovery parent along with the shell job. */
    if (stopped && collect_termination())
      return -1;
    for (i = 0; i < signal_count; ++i) {
      int number = signals[i];
      if (!pending[number])
        continue;
      pending[number] = 0;
      if (number == SIGCHLD || number == SIGCONT)
        continue;
      if (number == SIGWINCH) {
        (void)kill(-child, number);
        continue;
      }
      if (number == SIGTSTP || number == SIGTTIN) {
        stop = 1;
        continue;
      }
      /* Application control signals retain the application's policy. */
      if (number != SIGHUP && number != SIGTERM && number != SIGINT &&
          number != SIGQUIT) {
        (void)kill(-child, number);
        continue;
      }
      if (!terminating) {
        terminating = number;
        deadline = milliseconds() + 1000;
        (void)kill(-child, number);
        (void)kill(-child, SIGCONT);
      }
    }
    if (stopped) {
      if (terminating)
        (void)kill(-child, SIGCONT);
      else if (suspend_host())
        return -1;
    } else if (stop && !terminating)
      (void)kill(-child, SIGSTOP);
    if (terminating) {
      double remaining = deadline - milliseconds();
      if (remaining <= 0) {
        (void)kill(-child, SIGKILL);
        timeout = 100;
      } else
        timeout = (int)remaining + 1;
    }
    /* Handlers only set flags and wake this loop. A full pipe is already a
     * wakeup, so notifications cannot be lost between checking and polling. */
    while (read(wakeup[0], bytes, sizeof(bytes)) > 0) {
    }
    (void)sigprocmask(SIG_SETMASK, &awake, NULL);
    if (poll(&wait, 1, timeout) < 0 && errno != EINTR)
      return -1;
  }
}
int main(int argc, char **argv) {
  int i, start[2] = {-1, -1}, result = -1;
  char ready;
  for (i = 0; i < 3; ++i) {
    terminals[i].fd = -1;
    terminals[i].flags = -1;
  }
  if (argc == 2 && !strcmp(argv[1], "--help")) {
    puts("Usage: softline_terminal_supervisor -- PROGRAM [ARG ...]\n"
         "Run a TUI on its existing terminal with an independent recovery "
         "parent.\n"
         "No PTY or I/O relay is created. Non-TTY input/output executes "
         "directly.");
    return 0;
  }
  if (argc < 3 || strcmp(argv[1], "--")) {
    fputs("Usage: softline_terminal_supervisor -- PROGRAM [ARG ...]\n", stderr);
    return 2;
  }
  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    execvp(argv[2], argv + 2);
    i = errno == ENOENT ? 127 : 126;
    perror("softline terminal: exec");
    return i;
  }
  foreground = tcgetpgrp(STDIN_FILENO);
  if (foreground < 0 || foreground != getpgrp()) {
    fputs("softline terminal: launcher requires the foreground terminal\n",
          stderr);
    return 125;
  }
  for (i = 0; i < 3; ++i) {
    terminal_t *t = terminals + i;
    t->fd = fcntl(i, F_DUPFD_CLOEXEC, 3);
    if (t->fd < 0) {
      if (i == 2 && errno == EBADF)
        continue;
      goto finished;
    }
    t->flags = fcntl(t->fd, F_GETFL);
    if (t->flags < 0)
      goto finished;
    if (!tcgetattr(t->fd, &t->original))
      t->tty = 1;
    else if (errno != ENOTTY)
      goto finished;
  }
  if (pipe(wakeup) || pipe(start))
    goto finished;
  for (i = 0; i < 2; ++i)
    if (fcntl(wakeup[i], F_SETFD, FD_CLOEXEC) ||
        fcntl(wakeup[i], F_SETFL, O_NONBLOCK) ||
        fcntl(start[i], F_SETFD, FD_CLOEXEC))
      goto finished;
  if (prepare_signals())
    goto finished;
  child = fork();
  if (child < 0)
    goto finished;
  if (!child) {
    close(start[1]);
    close(wakeup[0]);
    close(wakeup[1]);
    if (setpgid(0, 0) || read(start[0], &ready, 1) != 1)
      _exit(125);
    close(start[0]);
    for (i = 0; i < 3; ++i)
      if (terminals[i].fd >= 0)
        close(terminals[i].fd);
    for (i = 0; i < signal_count; ++i)
      (void)sigaction(signals[i], &inherited[i], NULL);
    (void)sigprocmask(SIG_SETMASK, &original_mask, NULL);
    execvp(argv[2], argv + 2);
    i = errno == ENOENT ? 127 : 126;
    perror("softline terminal: exec");
    _exit(i);
  }
  close(start[0]);
  start[0] = -1;
  if (setpgid(child, child) || give_terminal(child))
    goto finished;
  transferred = 1;
  if (write(start[1], "R", 1) != 1)
    goto finished;
  close(start[1]);
  start[1] = -1;
  result = supervise();
finished:
  if (child > 0) {
    int status;
    (void)kill(-child, SIGKILL);
    if (!child_reaped) {
      (void)kill(child, SIGKILL);
      while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  if (transferred) {
    if (give_terminal(foreground))
      result = -1;
    if (restore(0))
      result = -1;
  }
  for (i = 0; i < 3; ++i)
    if (terminals[i].fd >= 0)
      close(terminals[i].fd);
  for (i = 0; i < 2; ++i) {
    if (start[i] >= 0)
      close(start[i]);
    if (wakeup[i] >= 0)
      close(wakeup[i]);
  }
  if (result < 0) {
    fputs("softline terminal: supervision or terminal restoration failed\n",
          stderr);
    return 125;
  }
  return result;
}
