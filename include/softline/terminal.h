#ifndef SOFTLINE_TERMINAL_H
#define SOFTLINE_TERMINAL_H

/** @file
 * Host-driven server PTY sessions for terminal handoff adapters.
 * The host supplies a transport, an owner-thread event loop and shell policy.
 */

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
  /** Apply an adapter event. @see sl_terminal_session_dispatch */
  int (*dispatch)(sl_terminal_session_t *self, sl_terminal_event_t *event);
  /** Poll the exit status. @see sl_terminal_session_status */
  int (*status)(sl_terminal_session_t *self, int *out_status);
  /** Close the receiver and reap without blocking.
   * @see sl_terminal_session_destroy */
  int (*destroy)(sl_terminal_session_t *self);
  /** Private implementation; never inspect or mutate. */
  void *impl;
};
/** Initialize all policy fields to NULL to inherit shell, directory and
 * environment defaults. A NULL config pointer is ignored. */
void sl_terminal_session_config_init(sl_terminal_session_config_t *config);
/** Copy policy without spawning. Up to 128 arguments, 1024 environment entries
 * and 128 KiB aggregate strings are accepted.
 * @param config Initialized policy whose strings are copied.
 * @return Caller-owned receiver, or NULL for invalid input or allocation
 * failure. Release a successful result with sl_terminal_session_destroy(). */
sl_terminal_session_t *
sl_terminal_session_create(const sl_terminal_session_config_t *config);
/** Apply one adapter event. Failed OPEN still requires close or destroy.
 * Repeated CLOSE succeeds. READ/WRITE before readiness return
 * SL_ERROR_NOT_READY. STEP reports a known launch failure as SL_ERROR_IO.
 * @param self Session on the host's owner thread.
 * @param event Operation and in/out fields borrowed for this call.
 * @return SL_OK or an SL_ERROR_* status. */
int sl_terminal_session_dispatch(sl_terminal_session_t *self,
                                 sl_terminal_event_t *event);
/** Poll child exit status without blocking. A known launch failure can
 * report status before the child is reaped.
 * @param self Session on the host's owner thread.
 * @param out_status Receives the exit code, 128 plus a terminating signal,
 * or -1 if status is unavailable.
 * @return SL_OK after exit or known launch failure, SL_ERROR_NOT_READY while
 * the child is running, or SL_ERROR_INVALID for invalid arguments. */
int sl_terminal_session_status(sl_terminal_session_t *self, int *out_status);
/** Close, reap nonblocking, and free a session. NULL is allowed.
 * @return SL_OK when finished. SL_ERROR_NOT_READY retains the receiver so the
 * caller can retry from the host loop after child exit. */
int sl_terminal_session_destroy(sl_terminal_session_t *self);
/** Adapt a server session to sl_terminal_handoff().
 * @param context A borrowed sl_terminal_session_t used as adapter context.
 * @param event Operation and in/out fields borrowed for this call.
 * @return The result of sl_terminal_session_dispatch(). */
int sl_terminal_session_adapter(void *context, sl_terminal_event_t *event);

#ifdef __cplusplus
}
#endif
#endif
