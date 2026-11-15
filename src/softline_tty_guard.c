#define _XOPEN_SOURCE 700
#include "softline_tty_guard.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

/* Foreground terminal ownership is process-wide. Slots and saved attributes
 * remain at stable addresses while a signal runs. No allocation, transport,
 * child wait, stdio or terminal output is used by the signal path. */
#define GUARDS 64
struct sl_tty_guard {
  volatile sig_atomic_t active;
  int fd[2], flags[2], tty[2], resume_flags[2], resume_valid[2];
  struct termios original[2], resume[2];
};
static sl_tty_guard_t guards[GUARDS];
static const int base_signals[] = {SIGINT,
                                   SIGHUP,
                                   SIGTERM,
                                   SIGQUIT,
                                   SIGPIPE,
                                   SIGALRM,
                                   SIGUSR1,
                                   SIGUSR2,
                                   SIGVTALRM,
                                   SIGPROF,
                                   SIGXCPU,
                                   SIGXFSZ,
                                   SIGTSTP,
                                   SIGTTIN,
                                   SIGTTOU
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
#define SIGNAL_CAPACITY 128u
static int signals[SIGNAL_CAPACITY];
static size_t signal_count;
#define SIGNALS signal_count
static struct sigaction previous[SIGNAL_CAPACITY];
static struct sigaction wrapper_action[SIGNAL_CAPACITY];
static volatile sig_atomic_t owner, registered;
static int count, exit_registered;
static volatile sig_atomic_t signal_depth;
int sl_tty_setattr(int fd, int action, const struct termios *t) {
  int result;
  do {
    result = tcsetattr(fd, action, t);
  } while (result < 0 && errno == EINTR);
  return result;
}
static int set_flags(int fd, int flags) {
  int result;
  do {
    result = fcntl(fd, F_SETFL, flags);
  } while (result < 0 && errno == EINTR);
  return result;
}
static int restore(sl_tty_guard_t *g, int modes, int flags) {
  int i, result = 0;
  for (i = 1; i >= 0; --i) {
    if (modes && g->tty[i] &&
        sl_tty_setattr(g->fd[i], TCSANOW, &g->original[i]))
      result = -1;
    if (flags && set_flags(g->fd[i], g->flags[i]))
      result = -1;
  }
  return result;
}
static void restore_all(void) {
  int i;
  if (owner != getpid())
    return;
  for (i = GUARDS - 1; i >= 0; --i)
    if (guards[i].active)
      (void)restore(guards + i, 1, 1);
}
static void guard_signal(int signo, siginfo_t *info, void *context);
static struct sigaction action(void) {
  struct sigaction a;
  memset(&a, 0, sizeof(a));
  sigfillset(&a.sa_mask);
  a.sa_sigaction = guard_signal;
  a.sa_flags = SA_SIGINFO;
  return a;
}
static void guard_signal(int signo, siginfo_t *info, void *context) {
  size_t which;
  int i, j, saved_errno = errno;
  struct sigaction old;
  sigset_t mask, ours;
  for (which = 0; which < SIGNALS && signals[which] != signo; ++which) {
  }
  if (which == SIGNALS)
    return;
  ++signal_depth;
  old = previous[which];
  if (owner == getpid()) {
    if (signal_depth == 1)
      for (i = 0; i < GUARDS; ++i)
        if (guards[i].active)
          for (j = 0; j < 2; ++j) {
            sl_tty_guard_t *g = guards + i;
            g->resume_flags[j] = fcntl(g->fd[j], F_GETFL);
            g->resume_valid[j] =
                g->tty[j] && !tcgetattr(g->fd[j], &g->resume[j]);
          }
    restore_all();
  }
  /* Delegate with the application's original disposition. Restore before
   * invoking a custom handler too: it may exit or leave by siglongjmp. */
  if (old.sa_handler != SIG_IGN && old.sa_handler != SIG_DFL) {
    ucontext_t *u = context;
    mask = u->uc_sigmask;
    for (i = 1; i < (int)(sizeof(sigset_t) * CHAR_BIT); ++i)
      if (sigismember(&old.sa_mask, i) == 1)
        sigaddset(&mask, i);
    if (!(old.sa_flags & SA_NODEFER))
      sigaddset(&mask, signo);
    if (old.sa_flags & SA_RESETHAND) {
      previous[which].sa_handler = SIG_DFL;
      previous[which].sa_flags = 0;
      sigemptyset(&previous[which].sa_mask);
    }
    (void)sigprocmask(SIG_SETMASK, &mask, &ours);
    if (old.sa_flags & SA_SIGINFO)
      old.sa_sigaction(signo, info, context);
    else
      old.sa_handler(signo);
    (void)sigprocmask(SIG_SETMASK, &ours, NULL);
  } else if (old.sa_handler == SIG_DFL) {
    sigset_t one;
    (void)sigaction(signo, &old, NULL);
    sigemptyset(&one);
    sigaddset(&one, signo);
    (void)sigprocmask(SIG_UNBLOCK, &one, &ours);
    (void)raise(signo);
    (void)sigprocmask(SIG_SETMASK, &ours, NULL);
    /* A job-control stop returns here after SIGCONT. */
    if (registered)
      (void)sigaction(signo, &wrapper_action[which], NULL);
  }
  --signal_depth;
  if (!signal_depth && owner == getpid())
    for (i = 0; i < GUARDS; ++i)
      if (guards[i].active)
        for (j = 0; j < 2; ++j) {
          sl_tty_guard_t *g = guards + i;
          pid_t foreground = tcgetpgrp(g->fd[j]);
          if (foreground >= 0 && foreground != getpgrp())
            continue;
          if (g->resume_valid[j])
            (void)sl_tty_setattr(g->fd[j], TCSANOW, &g->resume[j]);
          if (g->resume_flags[j] >= 0)
            (void)set_flags(g->fd[j], g->resume_flags[j]);
        }
  errno = saved_errno;
}
sl_tty_guard_t *sl_tty_guard_begin(int input, int output,
                                   const sl_tty_guard_t *parent) {
  sl_tty_guard_t *g = NULL;
  sigset_t all, mask;
  struct sigaction a;
  size_t i, installed = 0;
  int slot, j, fds[2];
  sigfillset(&all);
  if (sigprocmask(SIG_BLOCK, &all, &mask))
    return NULL;
  if (!signal_count) {
    for (i = 0; i < sizeof(base_signals) / sizeof(base_signals[0]); ++i)
      signals[signal_count++] = base_signals[i];
#if defined(SIGRTMIN) && defined(SIGRTMAX)
    {
      int first = SIGRTMIN, last = SIGRTMAX, number;
      if (last - first + 1 > (int)(SIGNAL_CAPACITY - signal_count))
        goto failed;
      for (number = first; number <= last; ++number)
        signals[signal_count++] = number;
    }
#endif
  }
  if (owner && owner != getpid()) {
    for (slot = 0; slot < GUARDS; ++slot)
      if (guards[slot].active) {
        close(guards[slot].fd[0]);
        close(guards[slot].fd[1]);
        guards[slot].active = 0;
      }
    count = 0;
    if (registered)
      for (i = 0; i < SIGNALS; ++i) {
        struct sigaction current;
        if (!sigaction(signals[i], NULL, &current) &&
            (current.sa_flags & SA_SIGINFO) &&
            current.sa_sigaction == guard_signal)
          (void)sigaction(signals[i], &previous[i], NULL);
      }
    registered = 0;
  }
  owner = getpid();
  for (slot = 0; slot < GUARDS; ++slot)
    if (!guards[slot].active) {
      g = guards + slot;
      break;
    }
  if (!g)
    goto failed;
  memset(g, 0, sizeof(*g));
  g->fd[0] = g->fd[1] = -1;
  fds[0] = input;
  fds[1] = output;
  for (j = 0; j < 2; ++j) {
    g->fd[j] = fcntl(fds[j], F_DUPFD_CLOEXEC, 3);
    if (g->fd[j] < 0)
      goto failed;
    g->flags[j] = fcntl(g->fd[j], F_GETFL);
    if (g->flags[j] < 0)
      goto failed;
    if (!tcgetattr(g->fd[j], &g->original[j]))
      g->tty[j] = 1;
    else if (j == 0 || errno != ENOTTY)
      goto failed;
    if (parent && parent->active) {
      g->flags[j] = parent->flags[j];
      g->tty[j] = parent->tty[j];
      g->original[j] = parent->original[j];
    }
  }
  if (!exit_registered) {
    if (atexit(restore_all))
      goto failed;
    exit_registered = 1;
  }
  if (!registered) {
    a = action();
    for (i = 0; i < SIGNALS; ++i) {
      if (sigaction(signals[i], NULL, &previous[i]))
        goto failed;
      wrapper_action[i] = a;
      wrapper_action[i].sa_flags |=
          previous[i].sa_flags & (SA_ONSTACK | SA_RESTART);
      if (previous[i].sa_handler != SIG_IGN &&
          sigaction(signals[i], &wrapper_action[i], NULL))
        goto failed;
      ++installed;
    }
    registered = 1;
  }
  g->active = 1;
  ++count;
  (void)sigprocmask(SIG_SETMASK, &mask, NULL);
  return g;
failed:
  for (i = 0; i < installed; ++i)
    (void)sigaction(signals[i], &previous[i], NULL);
  if (g)
    for (j = 0; j < 2; ++j)
      if (g->fd[j] >= 0) {
        close(g->fd[j]);
        g->fd[j] = -1;
      }
  (void)sigprocmask(SIG_SETMASK, &mask, NULL);
  return NULL;
}
int sl_tty_guard_restore_modes(sl_tty_guard_t *g) {
  sigset_t all, mask;
  int result;
  if (!g || !g->active)
    return -1;
  sigfillset(&all);
  if (sigprocmask(SIG_BLOCK, &all, &mask))
    return -1;
  result = restore(g, 1, 0);
  (void)sigprocmask(SIG_SETMASK, &mask, NULL);
  return result;
}
int sl_tty_guard_end(sl_tty_guard_t *g, int modes, int flags) {
  sigset_t all, mask;
  size_t i;
  int result;
  if (!g || !g->active)
    return -1;
  sigfillset(&all);
  if (sigprocmask(SIG_BLOCK, &all, &mask))
    return -1;
  result = restore(g, modes, flags);
  g->active = 0;
  --count;
  close(g->fd[0]);
  close(g->fd[1]);
  if (!count && registered) {
    for (i = 0; i < SIGNALS; ++i) {
      struct sigaction current;
      if (sigaction(signals[i], NULL, &current))
        result = -1;
      else if ((current.sa_flags & SA_SIGINFO) &&
               current.sa_sigaction == guard_signal &&
               sigaction(signals[i], &previous[i], NULL))
        result = -1;
    }
    registered = 0;
    signal_depth = 0;
  }
  (void)sigprocmask(SIG_SETMASK, &mask, NULL);
  return result;
}
