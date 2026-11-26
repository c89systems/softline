#ifndef SOFTLINE_TERMINAL_H
#define SOFTLINE_TERMINAL_H

/** Host-driven server PTY sessions for terminal handoff adapters.
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
  /** Apply one adapter event on the owner thread. OPEN once, then use
   * STEP for readiness. CLOSE remains valid after failed OPEN. */
  int (*dispatch)(sl_terminal_session_t *self, sl_terminal_event_t *event);
  /** Poll for exit without waiting. On success, `out_status` receives the
   * exit code or 128 plus the signal. `SL_ERROR_NOT_READY` leaves it at -1
   * while status is unavailable. */
  int (*status)(sl_terminal_session_t *self, int *out_status);
  /** Close and reap without blocking. Retry on `SL_ERROR_NOT_READY`,
   * which leaves ownership with the caller. `SL_OK` releases the receiver. */
  int (*destroy)(sl_terminal_session_t *self);
  /** Private implementation; never inspect or mutate. */
  void *impl;
};
/** Initialize all policy fields to NULL to inherit shell, directory and
 * environment defaults. A NULL config pointer is ignored. */
void sl_terminal_session_config_init(sl_terminal_session_config_t *config);
/** Create a PTY session without spawning a child.
 *
 * Copies the initialized `config` and its strings. Accepts up to 128
 * arguments, 1024 environment entries and 128 KiB of strings.
 * Returns a caller-owned receiver or null on invalid input or allocation
 * failure. Release it with `sl_terminal_session_destroy()`. */
sl_terminal_session_t *
sl_terminal_session_create(const sl_terminal_session_config_t *config);
/** Apply one borrowed `event` on the session's owner thread.
 *
 * Failed OPEN still requires close or destroy. Repeated CLOSE succeeds.
 * READ and WRITE return `SL_ERROR_NOT_READY` before readiness. STEP reports
 * a known launch failure as `SL_ERROR_IO`. Returns `SL_OK` or an error. */
int sl_terminal_session_dispatch(sl_terminal_session_t *self,
                                 sl_terminal_event_t *event);
/** Poll child exit status on the owner thread without blocking.
 *
 * `out_status` receives the exit code, 128 plus the terminating signal,
 * or -1 if unavailable. Returns `SL_OK` after exit or known launch failure
 * even if reaping is pending, `SL_ERROR_NOT_READY` while status is unavailable,
 * or `SL_ERROR_INVALID` for invalid arguments. */
int sl_terminal_session_status(sl_terminal_session_t *self, int *out_status);
/** Close, reap without blocking, and free a session. Null is allowed.
 * Returns `SL_OK` when finished. On `SL_ERROR_NOT_READY`, keep the
 * receiver and retry from the host loop after child exit. */
int sl_terminal_session_destroy(sl_terminal_session_t *self);
/** Use a borrowed `sl_terminal_session_t` as the adapter context for
 * `sl_terminal_handoff()`. Passes the borrowed `event` to
 * `sl_terminal_session_dispatch()` and returns its result. */
int sl_terminal_session_adapter(void *context, sl_terminal_event_t *event);

#ifdef __cplusplus
}
#endif
#endif
