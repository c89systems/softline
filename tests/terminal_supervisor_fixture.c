/* Compile the production launcher unchanged, substituting only the child-wait
 * boundary so competing events are deterministic on Linux and Darwin. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif
#define _XOPEN_SOURCE 700
#include <sys/wait.h>
static pid_t fixture_waitpid(pid_t, int *, int);
#define main supervisor_main
#define waitpid fixture_waitpid
#include "../examples/terminal_supervisor.c"
#undef main
#undef waitpid
static int stop_signal, kernel_pending, injected;
static pid_t fixture_waitpid(pid_t pid, int *status, int options) {
  pid_t result = waitpid(pid, status, options);
  if (result == pid && WIFSTOPPED(*status) && !injected) {
    sigset_t mask;
    injected = 1;
    if (sigprocmask(SIG_SETMASK, NULL, &mask) ||
        sigismember(&mask, stop_signal) != 1)
      _exit(124);
    if (kernel_pending) {
      if (kill(getpid(), stop_signal))
        _exit(124);
    } else
      notification(stop_signal);
  }
  return result;
}
int main(int argc, char **argv) {
  const char *mode = getenv("SOFTLINE_TEST_STOP_SIGNAL");
  if (!mode || sscanf(mode, "%d:%d", &stop_signal, &kernel_pending) != 2 ||
      (stop_signal != SIGHUP && stop_signal != SIGTERM &&
       stop_signal != SIGINT && stop_signal != SIGQUIT))
    return 124;
  return supervisor_main(argc, argv);
}
