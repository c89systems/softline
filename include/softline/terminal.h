#ifndef SOFTLINE_TERMINAL_H
#define SOFTLINE_TERMINAL_H
#include "softline.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Maximum bytes in one session READ/WRITE. Larger values must be chunked. */
#define SL_TERMINAL_SESSION_CHUNK_MAX 4096u
/** Host-driven server PTY. No sockets, listener, worker or editor is created.
 */
typedef struct sl_terminal_session sl_terminal_session_t;
/** Host policy, copied by create. Initialize before overriding fields. */
typedef struct sl_terminal_session_config {
  /** Executable path; NULL uses SHELL, then /bin/sh. No command parsing. */
  const char *program;
  /** NULL-terminated argv including argv[0]; NULL selects program and -i. */
  const char *const *arguments;
  /** Working directory; NULL inherits the host directory. */
  const char *directory;
  /** NULL inherits the host environment; otherwise a complete envp. */
  const char *const *environment;
  /** Optional TERM override, merged into the selected environment. */
  const char *terminal_type;
} sl_terminal_session_config_t;

/** Single-use, owner-thread receiver. Dispatch OPEN once, then STEP for
 * readiness, READ/WRITE for bounded byte flow, RESIZE and CLOSE. All methods
 * return promptly. The host owns transport, authorization and scheduling.
 * The session owns its descriptors; readiness descriptors are borrowed.
 * CLOSE terminates the shell and foreground group without waiting. Continue
 * servicing status/destroy until the child is reaped; no background reaper
 * or transcript is retained. This receiver owns waitpid for its child; host
 * SIGCHLD handlers must leave reaping to it. An idle shell has no idle timeout.
 */
struct sl_terminal_session {
  /** Apply the existing terminal adapter event contract. */
  int (*dispatch)(sl_terminal_session_t *, sl_terminal_event_t *);
  /** Poll status: NOT_READY while running, OK with exit code or -1. A known
   * launch failure may precede reaping; destroy still reports NOT_READY. */
  int (*status)(sl_terminal_session_t *, int *);
  /** Close, reap nonblocking, and free. NOT_READY retains the receiver;
   * retry from the host loop. NULL destruction succeeds. */
  int (*destroy)(sl_terminal_session_t *);
  /** Private implementation; never inspect or mutate. */
  void *impl;
};
/** Initialize host policy with inherited directory/environment and shell. */
void sl_terminal_session_config_init(sl_terminal_session_config_t *);
/** Copy policy without spawning. Up to 128 arguments, 1024 environment entries
 * and 128 KiB aggregate strings are accepted. NULL means invalid input or OOM.
 */
sl_terminal_session_t *
sl_terminal_session_create(const sl_terminal_session_config_t *);
/** Receiver counterpart. Failed OPEN still requires CLOSE/destroy. Repeated
 * CLOSE is harmless. READ/WRITE before ready return NOT_READY. STEP reports a
 * known launch failure as ERROR_IO; status supplies the failure exit code. */
int sl_terminal_session_dispatch(sl_terminal_session_t *,
                                 sl_terminal_event_t *);
/** Receiver counterpart; never blocks waiting for the child. */
int sl_terminal_session_status(sl_terminal_session_t *, int *);
/** Receiver counterpart; NOT_READY means the object is still owned by caller.
 */
int sl_terminal_session_destroy(sl_terminal_session_t *);
/** Direct sl_terminal_handoff adapter using a session as its context. */
int sl_terminal_session_adapter(void *, sl_terminal_event_t *);

#ifdef __cplusplus
}
#endif
#endif
