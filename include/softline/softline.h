#ifndef SOFTLINE_H
#define SOFTLINE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/** Receiver handle for one independent softline editor instance. */
typedef struct sl sl_t;

/** Opaque per-handle identifier for an external event watch. */
typedef unsigned long sl_watch_id_t;

/** Opaque nonzero queue identity, unique for the lifetime of one handle.
 * Zero denotes a direct draft without a queued origin. IDs are never reused. */
typedef unsigned long sl_prompt_id_t;

/** Readiness conditions reported for an application-owned watch descriptor. */
typedef enum sl_watch_events {
  /** The descriptor can be read without blocking. */
  SL_WATCH_READ = 1u << 0,
  /** The descriptor can be written without blocking. */
  SL_WATCH_WRITE = 1u << 1,
  /** The descriptor reported an I/O error. */
  SL_WATCH_ERROR = 1u << 2,
  /** The descriptor reported peer closure. */
  SL_WATCH_HANGUP = 1u << 3
} sl_watch_events_t;

/** One owner-thread readiness notification for an external watch. */
typedef struct sl_watch_event {
  /** Registration identifier. */
  sl_watch_id_t id;
  /** Application-owned descriptor that became ready. */
  int fd;
  /** Bitwise OR of sl_watch_events_t readiness conditions. */
  unsigned int events;
} sl_watch_event_t;

/**
 * Handle a ready application-owned descriptor on the thread driving
 * readline()/next_prompt(). The callback owns draining the descriptor and may
 * use editor methods, but must not recursively enter readline()/next_prompt()
 * or destroy self. A destroy() or sl_destroy() call during this callback is
 * ignored and records a diagnostic on the handle.
 */
typedef int (*sl_watch_callback_t)(sl_t *self, const sl_watch_event_t *event,
                                   void *userdata);

/** Operation supplied to a nonblocking terminal-session adapter. */
typedef enum sl_terminal_operation {
  /** Start a new session at rows/columns; CLOSE follows even on failure. */
  SL_TERMINAL_OPEN = 1,
  /** Service events and supply the next fd/interests/timeout and ready state.
   */
  SL_TERMINAL_STEP,
  /** Fill data up to length; SL_OK with transferred=0 means EOF. */
  SL_TERMINAL_READ,
  /** Consume data up to length; report partial progress in transferred. */
  SL_TERMINAL_WRITE,
  /** Apply the latest rows/columns without waiting for acknowledgment. */
  SL_TERMINAL_RESIZE,
  /** Cancel/release once, without waiting for peer acknowledgment or exit. */
  SL_TERMINAL_CLOSE
} sl_terminal_operation_t;

/** Mutable, callback-scoped terminal I/O request. */
typedef struct sl_terminal_event {
  /** Requested operation. */
  sl_terminal_operation_t operation;
  /** Ready SL_WATCH_* bits for STEP; zero for an initial/deadline step. */
  unsigned int events;
  /** Next pollable descriptor, or -1 for deadline/input-only waiting. */
  int fd;
  /** Next SL_WATCH_READ/WRITE interests. */
  unsigned int interests;
  /** Maximum milliseconds until next STEP, -1 for no adapter deadline. */
  int timeout_ms;
  /** STEP sets one when READ/WRITE may be called, zero while opening. */
  int ready;
  /** Current physical terminal rows. */
  unsigned int rows;
  /** Current physical terminal columns. */
  unsigned int columns;
  /** Borrowed byte buffer; WRITE callbacks must not modify it. */
  char *data;
  /** Available READ capacity or WRITE byte count. */
  size_t length;
  /** READ/WRITE sets bytes transferred, never exceeding length. */
  size_t transferred;
} sl_terminal_event_t;

/** Owner-thread adapter callback. Return SL_OK or an SL_ERROR_* value.
 * READ/WRITE return SL_ERROR_NOT_READY for would-block. All callbacks must
 * return promptly, must not reenter Softline, and borrow event/data only for
 * the call. STEP must arrange readiness/deadlines for pending transport work.
 * The context is borrowed until the exactly-once CLOSE callback returns. */
typedef int (*sl_terminal_callback_t)(void *context,
                                      sl_terminal_event_t *event);

/** Eligible start positions for one literal completion. */
typedef enum sl_completion_scope {
  /** Only byte zero of the entire draft; the default value. */
  SL_COMPLETION_START = 0,
  /** Byte zero or immediately after ASCII whitespace, including Tab and LF. */
  SL_COMPLETION_WORD = 1
} sl_completion_scope_t;

/** Copy a full candidate, including its trigger, into this activation's list.
 * Call synchronously within the provider; strings are copied during emit().
 * Return SL_OK or an error. Providers must stop emitting on error. */
typedef int (*sl_completion_emit_t)(const char *text, void *context);

/** Resolve a trigger once on the editor owner thread. Emit candidates in Tab
 * cycling order and return SL_OK or an error. Read editor state but do not
 * mutate/destroy the handle or retain emit/context. No provider runs from a
 * feed or renderer. Failure discards the list, preserving the draft. */
typedef int (*sl_completion_callback_t)(sl_t *self, char trigger,
                                        sl_completion_emit_t emit,
                                        void *context, void *userdata);

/** Continue a completed candidate on Right/Ctrl-F commit, or on Tab outside a
 * cycling session. Commit previews the first returned candidate without
 * inserting it; Tab accepts that ghost. completed includes the trigger. It may
 * also be a completed ancestor of a partially typed child. Emit full strings
 * strictly extending it, or emit no strings to retain normal completion/host
 * Tab handling. Results, including empty/failed attempts, are cached per prefix
 * until activation ends. Parent lists remain available after Backspace.
 * Repeated Tabs stay within the current list, even for a single candidate;
 * typing or committing ends that cycle. Failure discards only this continuation
 * list. After an empty/failed continuation, forward keys resume movement;
 * at the draft end, no-op forward keys preserve the committed text. Same
 * owner-thread and lifetime rules as the initial callback apply. */
typedef int (*sl_completion_expand_callback_t)(sl_t *self,
                                               const char *completed,
                                               sl_completion_emit_t emit,
                                               void *context, void *userdata);

/** One host-supplied completion trigger registration. */
typedef struct sl_completion_provider {
  /** Printable non-whitespace ASCII introductory character, e.g. '/' or '@'. */
  char trigger;
  /** Eligible positions; default is draft start only. */
  sl_completion_scope_t scope;
  /** Remove the trigger when completion exits or the prompt is submitted. */
  int strip_trigger;
  /** Synchronous callback called once per active token. */
  sl_completion_callback_t callback;
  /** Borrowed host context, valid until registrations are replaced. */
  void *userdata;
  /** Optional lazy continuation callback, cached per completed string. */
  sl_completion_expand_callback_t expand;
} sl_completion_provider_t;

/** Policy for OSC 8 hyperlinks in validated producer output. */
typedef enum sl_osc8_mode {
  /** Default: reject OSC 8. */
  SL_OSC8_OFF = 0,
  /** Explicit host override, including redirected output. */
  SL_OSC8_ON = 1,
  /** Enable only for a passively recognized output terminal. */
  SL_OSC8_AUTO = 2
} sl_osc8_mode_t;

/** Cached terminal detection and effective hyperlink policy. */
typedef struct sl_osc8_info {
  /** Configured policy. */
  sl_osc8_mode_t mode;
  /** Known terminal environment on output_fd; not a probe. */
  int detected;
  /** Whether validated feed accepts OSC 8 now. */
  int enabled;
} sl_osc8_info_t;

/** Maximum complete OSC 8 control length, including opener and terminator. */
#define SL_OSC8_MAX_BYTES 8192

/** Successful local termination of a terminal relay by beginning-of-line ~. */
#define SL_TERMINAL_ESCAPED 1

/** Run a local/remote PTY byte relay between editor calls. End/begin an open
 * output session automatically; preserve the editor and never store transcript.
 * Enter then ~. escapes locally, ~~ sends a literal tilde. Return SL_OK on EOF,
 * SL_TERMINAL_ESCAPED on escape, or an SL_ERROR_* value. Adapter callbacks must
 * not block. Bounded input overflow fails explicitly instead of growing memory.
 * Pending output is discarded on cancellation/failure. This free function adds
 * no shell-control method to the receiver.
 * If terminal output prevents teardown/reacquisition, return SL_ERROR_IO with
 * input modes released and the output session closed. The editor continuation
 * remains intact; reopen with sl_output_stream_begin() when output is writable.
 */
int sl_terminal_handoff(sl_t *self, sl_terminal_callback_t callback,
                        void *context);

/** Run the built-in local PTY adapter through sl_terminal_handoff(). shell is
 * one executable path (NULL uses SHELL or /bin/sh), invoked with -i. Optional
 * exit_status receives the shell exit code, 128+signal, or -1 if unavailable.
 * After shell exit, drain currently available output up to 64 KiB; background
 * jobs retaining the PTY slave do not keep this handoff open.
 * The handle owns eventual nonblocking child reaping after cancellation. */
int sl_terminal_local(sl_t *self, const char *shell, int *exit_status);

/**
 * Idle hook invoked while sl_readline() is active and no input byte is ready.
 *
 * The callback runs synchronously on the thread calling sl_readline(). It may
 * use public receiver methods to inspect or mutate the active buffer, submit,
 * cancel, or print above the active prompt. It must not destroy self.
 */
typedef void (*sl_idle_callback_t)(sl_t *self, void *userdata);

/**
 * Chunk producer for sl_print_above().
 *
 * Return SL_OK with a non-empty chunk to continue. Return SL_OK with *len set
 * to 0 to end the stream. The chunk memory only needs to remain valid until
 * the callback is invoked again or sl_print_above() returns.
 */
typedef int (*sl_stream_callback_t)(sl_t *self, void *userdata,
                                    const char **chunk, size_t *len);

/**
 * Decoded key identifiers delivered to key binding callbacks.
 *
 * Printable UTF-8 input is inserted by the built-in editor path rather than
 * reported through these constants. Alt-letter keys not listed directly are
 * represented as SL_KEY_ALT_BASE plus the unsigned byte value of the letter
 * when the terminal sequence is recognized.
 */
typedef enum sl_key {
  /** No key was decoded. */
  SL_KEY_NONE = 0,
  /** Ctrl-C, normally terminal interrupt. */
  SL_KEY_CTRL_C = 3,
  /** Ctrl-D, EOF on an empty buffer or delete-forward in text. */
  SL_KEY_CTRL_D = 4,
  /** Ctrl-A, move to the current newline-delimited line's beginning. */
  SL_KEY_CTRL_A = 1,
  /** Ctrl-B, move backward by one UTF-8 cluster. */
  SL_KEY_CTRL_B = 2,
  /** Ctrl-E, move to the current newline-delimited line's end. */
  SL_KEY_CTRL_E = 5,
  /** Ctrl-F, accept/commit an active completion at its token endpoint;
   * otherwise move forward by one UTF-8 cluster. */
  SL_KEY_CTRL_F = 6,
  /** Tab, accept/cycle matching completions before host bindings/queue keys. */
  SL_KEY_TAB = 9,
  /** Ctrl-J, insert a literal newline. */
  SL_KEY_CTRL_J = 10,
  /** Ctrl-K, delete to the next newline or buffer end. At a newline,
   * delete only that newline to join lines. Soft wraps are not boundaries. */
  SL_KEY_CTRL_K = 11,
  /** Ctrl-L, reread PTY geometry and repaint softline's owned prompt area.
   * Preserves the draft, queues and transcript; does not clear the terminal. */
  SL_KEY_CTRL_L = 12,
  /** Enter or carriage return. During reverse history search, accept the match
   * into the normal editable prompt without submitting or queueing it. */
  SL_KEY_ENTER = 13,
  /** Ctrl-N, recall the next history entry. */
  SL_KEY_CTRL_N = 14,
  /** Ctrl-P, recall the previous history entry. */
  SL_KEY_CTRL_P = 16,
  /** Ctrl-R, reverse incremental history search unless rebound. Enter returns
   * the displayed match to normal editing; Escape or Ctrl-G restores the draft.
   * An explicit Enter binding may request SL_KEY_ACTION_SUBMIT instead. */
  SL_KEY_CTRL_R = 18,
  /** Ctrl-T, transpose adjacent UTF-8 clusters. */
  SL_KEY_CTRL_T = 20,
  /** Ctrl-U, delete from the current line's beginning to the cursor,
   * or at line start delete only the preceding newline. Soft wraps are not
   * boundaries. At buffer start, do nothing. */
  SL_KEY_CTRL_U = 21,
  /** Ctrl-V, paste an X11 clipboard image path when no binding overrides it. */
  SL_KEY_CTRL_V = 22,
  /** Ctrl-W, delete the previous whitespace-delimited word within the current
   * line, or at line start delete only the preceding newline. */
  SL_KEY_CTRL_W = 23,
  /** Ctrl-X prefix; Ctrl-X Ctrl-U requests undo unless Ctrl-X is rebound. */
  SL_KEY_CTRL_X = 24,
  /** Ctrl-Y, insert the newest text from this handle's kill ring. */
  SL_KEY_CTRL_Y = 25,
  /** Ctrl-underscore, undo the last draft editing command or typing run. */
  SL_KEY_CTRL_UNDERSCORE = 31,
  /** Escape key. */
  SL_KEY_ESCAPE = 27,
  /** Backspace key. */
  SL_KEY_BACKSPACE = 127,
  /** Up arrow. */
  SL_KEY_UP = 1000,
  /** Down arrow. */
  SL_KEY_DOWN = 1001,
  /** Left arrow. */
  SL_KEY_LEFT = 1002,
  /** Right arrow, the same completion/forward action as Ctrl-F. */
  SL_KEY_RIGHT = 1003,
  /** Home key, move to the current newline-delimited line's beginning. */
  SL_KEY_HOME = 1004,
  /** End key, move to the current newline-delimited line's end. */
  SL_KEY_END = 1005,
  /** Delete key. */
  SL_KEY_DELETE = 1006,
  /** Alt-B, move backward by word. */
  SL_KEY_ALT_B = 1007,
  /** Alt-F, move forward by word. */
  SL_KEY_ALT_F = 1008,
  /** Escape sequence was recognized as unsupported or incomplete. */
  SL_KEY_UNKNOWN = 1009,
  /** Bracketed paste begin sequence. */
  SL_KEY_PASTE_BEGIN = 1010,
  /** Bracketed paste end sequence. */
  SL_KEY_PASTE_END = 1011,
  /** Function key F1. */
  SL_KEY_F1 = 1012,
  /** Function key F2. */
  SL_KEY_F2 = 1013,
  /** Function key F3. */
  SL_KEY_F3 = 1014,
  /** Function key F4. */
  SL_KEY_F4 = 1015,
  /** Function key F5. */
  SL_KEY_F5 = 1016,
  /** Function key F6. */
  SL_KEY_F6 = 1017,
  /** Function key F7. */
  SL_KEY_F7 = 1018,
  /** Function key F8. */
  SL_KEY_F8 = 1019,
  /** Function key F9. */
  SL_KEY_F9 = 1020,
  /** Function key F10. */
  SL_KEY_F10 = 1021,
  /** Ctrl-Enter when a terminal sends a distinguishable sequence. */
  SL_KEY_CTRL_ENTER = 1022,
  /** Alt-Enter, normally emitted as Escape followed by carriage return. */
  SL_KEY_ALT_ENTER = 1023,
  /** Page Up key. */
  SL_KEY_PAGE_UP = 1024,
  /** Page Down key. */
  SL_KEY_PAGE_DOWN = 1025,
  /** Alt-Backspace, kill the preceding alphanumeric word. */
  SL_KEY_ALT_BACKSPACE = 1026,
  /** Base value for Alt-letter bindings not listed as dedicated constants. */
  SL_KEY_ALT_BASE = 4096,
  /** Alt-A, focus the oldest pending approval when the draft is empty. */
  SL_KEY_ALT_A = SL_KEY_ALT_BASE + 'a',
  /** Alt-E, edit the newest queued draft by default. */
  SL_KEY_ALT_E = SL_KEY_ALT_BASE + 'e',
  /** Alt-D, kill through the end of the next alphanumeric word. */
  SL_KEY_ALT_D = SL_KEY_ALT_BASE + 'd',
  /** Alt-Y, rotate the last yank; focused approval controls take precedence. */
  SL_KEY_ALT_Y = SL_KEY_ALT_BASE + 'y',
  /** Alt-M key. */
  SL_KEY_ALT_M = 4205
} sl_key_t;

/**
 * Action requested by a key binding callback.
 *
 * A callback stores one of these values in its action out-parameter before
 * returning SL_OK. Returning a negative sl_status_t fails the active operation
 * regardless of the action value.
 */
typedef enum sl_key_action {
  /** Let softline run the built-in behavior for the key. */
  SL_KEY_ACTION_PASS = 0,
  /** Treat the key as fully handled by the callback. */
  SL_KEY_ACTION_HANDLED = 1,
  /** Submit the current buffer. */
  SL_KEY_ACTION_SUBMIT = 2,
  /** Cancel the active readline() call. */
  SL_KEY_ACTION_CANCEL = 3,
  /** Restore terminal state and raise SIGINT. */
  SL_KEY_ACTION_INTERRUPT = 4,
  /** Reset prompt layout from PTY geometry and repaint owned UI rows while
   * preserving editor and producer state. Also the default Ctrl-L action. */
  SL_KEY_ACTION_RESET = 5,
  /** Return NULL with SL_READLINE_SUSPENDED, retaining the editor for the next
   * readline()/next_prompt(). The owner may hand off the terminal between
   * calls using output_stream_end()/begin(); never launch a child here. */
  SL_KEY_ACTION_SUSPEND = 6
} sl_key_action_t;

/**
 * Per-handle key binding callback.
 *
 * The callback may inspect or mutate the active buffer through the public
 * receiver methods. It must store the requested outcome in *action and return
 * SL_OK, or return a negative sl_status_t to fail the active operation.
 */
typedef int (*sl_key_callback_t)(sl_t *self, sl_key_t key, void *userdata,
                                 sl_key_action_t *action);

/**
 * Status codes returned by fallible softline operations.
 *
 * Public methods return SL_OK on success or one of the negative values on
 * failure. When a handle is available, sl_last_error() may contain additional
 * handle-owned diagnostic text after a failure.
 */
typedef enum sl_status {
  /** Operation completed successfully. */
  SL_OK = 0,
  /** Unclassified failure. */
  SL_ERROR = -1,
  /** Invalid argument or invalid configuration. */
  SL_ERROR_INVALID = -2,
  /** Memory allocation failed. */
  SL_ERROR_NOMEM = -3,
  /** Terminal, file, or descriptor I/O failed. */
  SL_ERROR_IO = -4,
  /** A bounded prompt queue has no room for another entry. */
  SL_ERROR_FULL = -5,
  /** The selected backend does not support this operation. */
  SL_ERROR_UNSUPPORTED = -6,
  /** An expected queue/item revision no longer matches. */
  SL_ERROR_CONFLICT = -7,
  /** The backend is disconnected, unsynchronized or awaiting a result. */
  SL_ERROR_NOT_READY = -8
} sl_status_t;

/** Classification of the most recent sl_readline() result. */
typedef enum sl_readline_status {
  /** No readline() call has completed on this handle. */
  SL_READLINE_NONE = 0,
  /** readline() returned submitted text owned by the caller. */
  SL_READLINE_SUBMITTED = 1,
  /** readline() returned NULL because input ended without submitted text. */
  SL_READLINE_EOF = 2,
  /** readline() returned NULL because editing was cancelled. */
  SL_READLINE_CANCELLED = 3,
  /** readline() restored terminal state and raised SIGINT. */
  SL_READLINE_INTERRUPTED = 4,
  /** readline() returned NULL because an I/O, allocation, or render error
     occurred. */
  SL_READLINE_ERROR = 5,
  /** Editing was suspended without submission or cancellation. The next
   * readline()/next_prompt() resumes it before delivering queued prompts. */
  SL_READLINE_SUSPENDED = 6
} sl_readline_status_t;

/** How a text value returned by sl_next_prompt() entered the application. */
typedef enum sl_prompt_source {
  /** No prompt text was returned. */
  SL_PROMPT_SOURCE_NONE = 0,
  /** The user submitted the active editor with Enter or a mapped submit key. */
  SL_PROMPT_SOURCE_DIRECT = 1,
  /** An eligible queued entry was delivered automatically. */
  SL_PROMPT_SOURCE_QUEUED = 2,
  /** A promoted entry or a policy-managed steering entry was delivered. */
  SL_PROMPT_SOURCE_PROMOTED = 3
} sl_prompt_source_t;

/** Controls when sl_next_prompt() automatically delivers queued entries. */
typedef enum sl_prompt_queue_delivery {
  /** Deliver FIFO entries automatically before opening a direct editor. */
  SL_PROMPT_QUEUE_DELIVERY_AUTO = 0,
  /** Keep queued entries local until the host explicitly takes or promotes one.
   */
  SL_PROMPT_QUEUE_DELIVERY_MANUAL = 1
} sl_prompt_queue_delivery_t;

/** Application-controlled delivery intent attached to each queued prompt. */
typedef enum sl_prompt_queue_mode {
  /** A normal follow-up turn, eligible for automatic idle delivery. */
  SL_PROMPT_QUEUE_MODE_QUEUED = 0,
  /** A host-selected turn to deliver at a safe seam. */
  SL_PROMPT_QUEUE_MODE_STEER = 1
} sl_prompt_queue_mode_t;

/** Optional host policy for a deliberate editor submission. All fields are
 * booleans. Zero enqueue retains the selected profile's normal behavior. */
typedef struct sl_prompt_queue_policy {
  /** Admit through the visible queue even when idle. */
  int enqueue;
  /** Permit this entry to pass unrelated blocked entries (read-only queries).
   * Other entries retain their positions, holds and delivery intentions. */
  int independent;
  /** Host execution boundary is ready; independent of human approval. */
  int executable;
} sl_prompt_queue_policy_t;

/** Classify a nonempty deliberate submission before admission. policy starts
 * with {0,0,1}, or the edited entry's policy. Return SL_OK to accept the
 * policy. Failure leaves the draft intact. mode reports Enter versus steering
 * intent. Runs on the owner thread; inspect state only, do not mutate the
 * editor or queue, recurse into input, destroy self, or perform blocking work.
 * Softline recognizes no application command names. Passive enqueues do not
 * invoke it. */
typedef int (*sl_prompt_queue_policy_callback_t)(
    sl_t *self, const char *text, sl_prompt_queue_mode_t mode,
    sl_prompt_queue_policy_t *policy, void *userdata);

/** Identity and independent delivery controls for one queued prompt. */
typedef struct sl_prompt_queue_entry_info {
  /** Stable handle-local identity, preserved by replacement and editing. */
  sl_prompt_id_t id;
  /** Ordinary queued delivery or host-selected steering intent. */
  sl_prompt_queue_mode_t mode;
  /** Nonzero until UI/API approval or conversation-wide auto-approval. */
  int held;
  /** Admission and execution policy, preserved through editing/requeue. */
  sl_prompt_queue_policy_t policy;
  /** Entry belongs to a backlog paused by cancellation. */
  int stopped;
  /** May be taken by an owner-thread consumer now, considering holds, stopped
   * state and ordering. Automatic delivery additionally respects manual mode
   * and, for ordinary profile entries, busy/steering behavior. */
  int eligible;
} sl_prompt_queue_entry_info_t;

/** Built-in prompt queue interaction mappings. */
typedef enum sl_prompt_queue_profile {
  /** Tab enqueues and Alt-E edits the newest queued entry. */
  SL_PROMPT_QUEUE_PROFILE_DEFAULT = 0,
  /**
   * Turn-oriented queueing. While status is busy, Enter enqueues nonempty
   * drafts. In automatic delivery mode, idle next_prompt() delivers the oldest
   * ordinary queued turn. While busy, Alt-Enter queues a steer entry; with an
   * empty editor it marks the newest queued entry as steer. The host may take
   * steer entries at safe response seams. Cancellation pauses automatic
   * release; Alt-E edits newest.
   */
  SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS = 1
} sl_prompt_queue_profile_t;

/** Per-handle built-in prompt queue key mappings. SL_KEY_NONE disables an
 * action. */
typedef struct sl_prompt_queue_keys {
  /** Queue the active nonempty draft and continue editing. */
  sl_key_t enqueue_draft;
  /** Restore the newest queued entry into an empty active draft. */
  sl_key_t edit_newest;
  /** Submit a nonempty draft, or promote the newest queue entry when empty. */
  sl_key_t submit_or_promote_newest;
} sl_prompt_queue_keys_t;

/** Per-handle outbound request identity. A bridge supplies its durable
 * namespace when mapping this identity onto a backend transport or journal. */
typedef unsigned long sl_queue_request_id_t;

/** Operations sent to an application-owned queue. No operation grants the
 * frontend authority to consume model input. */
typedef enum sl_queue_operation {
  /** Accept text with Q/S intent. */
  SL_QUEUE_OFFER = 0,
  /** Acquire a revision-checked edit hold. */
  SL_QUEUE_BEGIN_EDIT = 1,
  /** Replace text/intent and release the acquired edit hold. */
  SL_QUEUE_END_EDIT = 2,
  /** Release an edit hold without replacing accepted text. */
  SL_QUEUE_ABORT_EDIT = 3,
  /** Release the item's human approval hold. */
  SL_QUEUE_APPROVE = 4,
  /** Withdraw an unclaimed item. */
  SL_QUEUE_WITHDRAW = 5,
  /** Change Q/S intent, preserving identity and position. */
  SL_QUEUE_SET_INTENT = 6,
  /** Stop automatic backlog delivery. */
  SL_QUEUE_STOP = 7,
  /** Explicitly resume the stopped backlog. */
  SL_QUEUE_RESUME = 8,
  /** Clear accepted pending items atomically. */
  SL_QUEUE_CLEAR = 9,
  /** Change backend auto-approval policy, including already held items. */
  SL_QUEUE_AUTO_APPROVAL = 10,
  /** Obtain an ordered bounded snapshot. */
  SL_QUEUE_SYNCHRONIZE = 11,
  /** Resolve an earlier request whose outcome is unknown. */
  SL_QUEUE_RESOLVE = 12
} sl_queue_operation_t;

/** One borrowed outbound request; pointers remain valid for the callback only.
 * IDs and revisions are opaque nonempty strings, limited to 127 bytes. Text is
 * bounded by line_max_len. A successful callback return acknowledges handoff,
 * not backend admission. Expected revisions are filled from the current mirror
 * when the corresponding caller field is NULL. */
typedef struct sl_queue_request {
  /** Identity assigned by Softline before handoff. */
  sl_queue_request_id_t request_id;
  /** Requested backend mutation or inspection. */
  sl_queue_operation_t operation;
  /** Target backend item, or NULL for queue-wide operations and offers. */
  const char *item_id;
  /** Expected item revision, or NULL when there is no target. */
  const char *item_revision;
  /** Expected queue revision, or NULL for initial synchronization. */
  const char *queue_revision;
  /** Acquired edit token for END_EDIT/ABORT_EDIT, otherwise NULL. */
  const char *edit_token;
  /** Offered/replacement text, otherwise NULL. */
  const char *text;
  /** Requested delivery intent; backend classification remains authoritative.
   */
  sl_prompt_queue_mode_t mode;
  /** Offer requires approval, or AUTO_APPROVAL's requested boolean value. */
  int enabled;
  /** Earlier per-handle identity for RESOLVE, otherwise zero. */
  sl_queue_request_id_t target_request_id;
} sl_queue_request_t;

/** Explicit result of an outbound request. */
typedef enum sl_queue_result_status {
  /** Backend durably accepted/applied the operation, or explicitly volatile. */
  SL_QUEUE_ACCEPTED = 0,
  /** Backend definitely rejected the operation. */
  SL_QUEUE_REJECTED = 1,
  /** Outcome unknown; resolve/retry the original identity, never a new offer.
   */
  SL_QUEUE_UNKNOWN = 2
} sl_queue_result_status_t;

/** Borrowed response. Results retire provisional requests; only ordered updates
 * change the accepted mirror, so a late result cannot recreate a consumed item.
 */
typedef struct sl_queue_result {
  /** Original request identity. */
  sl_queue_request_id_t request_id;
  /** Backend outcome. */
  sl_queue_result_status_t status;
  /** Affected backend item, if any. */
  const char *item_id;
  /** Accepted item revision, if any. */
  const char *item_revision;
  /** Edit token returned by BEGIN_EDIT, otherwise NULL. */
  const char *edit_token;
  /** Optional human-readable diagnostic, bounded by line_max_len. */
  const char *reason;
} sl_queue_result_t;

/** Borrowed accepted backend entry. The backend owns text, ordering, approval,
 * edit holds and readiness. Softline copies a bounded presentation mirror. */
typedef struct sl_queue_entry {
  /** Stable opaque backend identity. */
  const char *id;
  /** Current opaque item revision. */
  const char *revision;
  /** Accepted text, limited to line_max_len. */
  const char *text;
  /** Accepted Q/S intent. */
  sl_prompt_queue_mode_t mode;
  /** Human approval hold. */
  int held;
  /** Edit hold; only the holder's attachment receives its token. */
  int edit_held;
  /** Holder's opaque edit token, or NULL for another attachment's hold. */
  const char *edit_token;
  /** Backend-reported readiness, never a frontend execution grant. */
  int ready;
  /** Backend-reported stopped state for this item. */
  int stopped;
  /** Nonzero for an opaque host action, zero for model input. */
  int action;
} sl_queue_entry_t;

/** Ordered changes in one backend subscription. */
typedef enum sl_queue_change_kind {
  /** Replace the bounded view, including the first subscription snapshot. */
  SL_QUEUE_SNAPSHOT = 0,
  /** Insert/update one entry at index in the bounded ordered view. */
  SL_QUEUE_UPSERT = 1,
  /** Remove one entry by backend identity, including claim/consumption. */
  SL_QUEUE_REMOVE = 2,
  /** Update queue-wide state without changing entries. */
  SL_QUEUE_STATE = 3,
  /** Lose transport authority; preserve mirror, draft and outstanding outcomes.
   */
  SL_QUEUE_DISCONNECTED = 4
} sl_queue_change_kind_t;

/** Borrowed owner-thread update. Non-snapshot updates must chain their previous
 * revision to the current mirror. A gap preserves the view and schedules one
 * explicit resynchronization request at the next native dispatch boundary.
 * Snapshots belong to the same ordered subscription, not the response stream.
 */
typedef struct sl_queue_change {
  /** Snapshot, delta, state transition or disconnect. */
  sl_queue_change_kind_t kind;
  /** Previous queue revision for deltas, otherwise NULL. */
  const char *previous_revision;
  /** New queue revision, except for disconnect. */
  const char *revision;
  /** Bounded entries for a snapshot, or the single UPSERT entry. */
  const sl_queue_entry_t *entries;
  /** Snapshot entry count, one for UPSERT, zero otherwise. */
  size_t count;
  /** Ordered UPSERT position; existing entries cannot move. */
  size_t index;
  /** Removed backend identity for REMOVE. */
  const char *item_id;
  /** Causing request from this attachment, or zero for another origin. */
  sl_queue_request_id_t request_id;
  /** Backend's total accepted pending count, possibly larger than the view. */
  size_t total_count;
  /** Authoritative backlog stopped state. */
  int stopped;
  /** Authoritative auto-approval policy. */
  int auto_approval;
} sl_queue_change_t;

/** Constructor-selected backend callbacks. Copy at construction; context stays
 * borrowed until detach returns during editor destruction. Callbacks run only
 * on the editor owner thread, never block, reenter Softline or synchronously
 * apply a result/update. A bridge uses fd watches for deferred inbound work. */
typedef struct sl_queue_backend {
  /** Handoff callback; SL_OK means copied/enqueued, not admitted. */
  int (*request)(void *context, const sl_queue_request_t *request);
  /** Cancel/unsubscribe frontend observations; never clear/stop the backend. */
  void (*detach)(void *context);
  /** Supported operations: bit (1UL << sl_queue_operation_t). */
  unsigned long capabilities;
  /** Bound on ordinary retained requests; zero selects 16. Three additional
   * slots reserve one SYNCHRONIZE, RESOLVE and STOP each, so ordinary pressure
   * cannot prevent reconciliation or stopping before an edit is released. */
  size_t max_pending_requests;
} sl_queue_backend_t;

/** Borrowed backend-mode state, valid until the next result/update/request. */
typedef struct sl_queue_backend_info {
  /** Nonzero when the constructor selected a backend. */
  int enabled;
  /** Nonzero after a valid snapshot and until disconnect/gap. */
  int synchronized;
  /** Current opaque queue revision, or NULL before the initial snapshot. */
  const char *revision;
  /** Accepted entries currently mirrored, excluding provisional requests. */
  size_t visible_count;
  /** Authoritative total accepted pending count. */
  size_t total_count;
  /** Retained requests, including rejected text and unknown outcomes. */
  size_t pending_count;
} sl_queue_backend_info_t;

/** Borrowed retained request, valid until the next backend operation. */
typedef struct sl_queue_pending_info {
  /** Original request. Strings are owned by the handle. */
  sl_queue_request_t request;
  /** Nonzero while waiting for an explicit response. */
  int pending;
  /** Last response status when pending is zero. */
  sl_queue_result_status_t status;
  /** Optional backend/transport diagnostic. */
  const char *reason;
} sl_queue_pending_info_t;

/**
 * Renderer-owned visual treatment for the complete interactive prompt UI.
 *
 * The selected theme styles the prompt marker, queue preview panel, optional
 * status line, and status markers. SL_PROMPT_THEME_DEFAULT uses only the
 * standard 16 ANSI colours; the other colour themes may use their embedded
 * palette encodings.
 */
typedef enum sl_prompt_theme {
  /** Compact, uncoloured prompt UI. */
  SL_PROMPT_THEME_PLAIN = 0,
  /** A restrained cyan accent treatment. */
  SL_PROMPT_THEME_ACCENT = 1,
  /** The Dracula true-colour palette. */
  SL_PROMPT_THEME_DRACULA = 2,
  /** The Gruvbox true-colour palette. */
  SL_PROMPT_THEME_GRUVBOX = 3,
  /** An amber monochrome CRT palette. */
  SL_PROMPT_THEME_MONOCHROME = 4,
  /** A green monochrome terminal palette. */
  SL_PROMPT_THEME_MONOGREEN = 5,
  /** The Outrun true-colour palette. */
  SL_PROMPT_THEME_OUTRUN = 6,
  /** The Riced true-colour palette. */
  SL_PROMPT_THEME_RICED = 7,
  /** The Synthwave true-colour palette. */
  SL_PROMPT_THEME_SYNTHWAVE = 8,
  /** The standard 16-colour ANSI prompt palette. */
  SL_PROMPT_THEME_DEFAULT = 9
} sl_prompt_theme_t;

/** Theme palette roles for the transient status message. */
typedef enum sl_theme_color {
  /** Subdued separator colour. */
  SL_THEME_COLOR_MUTED = 0,
  /** Secondary text colour. */
  SL_THEME_COLOR_SECONDARY = 1,
  /** Prompt marker colour. */
  SL_THEME_COLOR_PROMPT = 2,
  /** Queue and approval colour: the fourth status element colour. */
  SL_THEME_COLOR_QUEUE = 3,
  /** Editor input colour. */
  SL_THEME_COLOR_INPUT = 4,
  /** First status element colour. */
  SL_THEME_COLOR_ELEMENT_0 = 5,
  /** Second status element colour. */
  SL_THEME_COLOR_ELEMENT_1,
  /** Third status element colour. */
  SL_THEME_COLOR_ELEMENT_2,
  /** Fourth status element colour. */
  SL_THEME_COLOR_ELEMENT_3,
  /** Fifth status element colour. */
  SL_THEME_COLOR_ELEMENT_4,
  /** Sixth status element colour. */
  SL_THEME_COLOR_ELEMENT_5,
  /** Seventh status element colour. */
  SL_THEME_COLOR_ELEMENT_6,
  /** Eighth status element colour. */
  SL_THEME_COLOR_ELEMENT_7
} sl_theme_color_t;

/** RGB colour for a separately styled quoted transcript prompt. */
typedef struct sl_quote_color {
  /** Red channel, 0..255. */
  unsigned char red;
  /** Green channel, 0..255. */
  unsigned char green;
  /** Blue channel, 0..255. */
  unsigned char blue;
} sl_quote_color_t;

/** Override colours for quoted prompts. The prefix is dimmed and the prompt
 * text is italic regardless of the selected colours. */
typedef struct sl_quote_style {
  /** Dimmed prefix colour. */
  sl_quote_color_t prefix;
  /** Italic prompt-text colour. */
  sl_quote_color_t text;
} sl_quote_style_t;

/** Import one borrowed, NUL-terminated history entry. Valid only during the
 * load callback. Return its status to stop loading on an invalid entry. */
typedef int (*sl_history_emit_t)(void *context, const char *prompt);

/** Load entries oldest first by calling emit(context, prompt). Return SL_OK
 * on success or a negative status. key is copied by Softline; userdata remains
 * caller-owned. Do not retain emit/context or mutate/destroy the same handle
 * during a history callback. Imported entries never invoke the append hook. */
typedef int (*sl_history_load_callback_t)(const char *key,
                                          sl_history_emit_t emit, void *context,
                                          void *userdata);

/** Persist one newly accepted history entry before history_add returns.
 * Return SL_OK or a negative status. Failure leaves in-memory history
 * unchanged; external storage rollback is the backend's responsibility. Prompt
 * and key are borrowed for the call; do not mutate/destroy the same handle
 * here. */
typedef int (*sl_history_append_callback_t)(const char *key, const char *prompt,
                                            void *userdata);

/** Maximum number of retained status-line elements. Excess bulk elements end
 * in a final `...` element. */
#define SL_STATUS_MAX_ELEMENTS 32

/**
 * Editor configuration initialized by sl_config_init().
 *
 * Call sl_config_init() for documented defaults before overriding fields.
 * Passing an all-zero struct to sl_create_with_config() is invalid because
 * line_max_len must be non-zero.
 */
typedef struct sl_config {
  /** Input file descriptor; default is STDIN_FILENO. */
  int input_fd;
  /** Output file descriptor; default is STDOUT_FILENO. */
  int output_fd;
  /** Editable line wrapping width; zero follows the terminal. Native chat
   * always uses the physical terminal width. */
  int screen_width;
  /** Non-zero lets an ordinary readline prompt pin itself at the terminal
   * bottom and stream output through a temporary scroll region. Disabled by
   * default, preserving normal clear-and-redraw scrollback behavior. Native
   * chat uses its own scroll region regardless of this setting. */
  int live_scroll_region;
  /** Maximum retained history entries; default is 100. */
  int history_max_len;
  /** Maximum complete editable draft in UTF-8 bytes; default is 131072. */
  size_t line_max_len;
  /** Non-zero enables the interactive prompt queue. The default keymap uses
   * Tab to enqueue, after matching completion. Editing is plain when either
   * input or output is not a TTY. */
  int prompt_queue;
  /** Maximum queued prompts; default is 64 when queueing is enabled. */
  int prompt_queue_max_entries;
  /** Maximum FIFO previews shown above the active editor; default is 3. */
  int prompt_queue_preview_entries;
  /** Built-in visual treatment for the complete interactive prompt UI;
   * defaults to SL_PROMPT_THEME_DEFAULT. */
  sl_prompt_theme_t prompt_theme;
  /** Non-zero renders the optional status line between queue previews and the
   * editor. */
  int statusline;
  /** Palette index assigned to the first status-line element; successive
   * elements cycle through the theme's eight element colours. */
  size_t statusline_start_element;
  /** Non-zero animates /-\\| while the status line is busy. */
  int status_spinner;
  /** Non-zero selects the busy status marker. */
  int status_busy;
  /** Printable ASCII marker shown while idle; defaults to '+'. '\0' leaves
   * its reserved status marker slot blank. */
  char status_idle_marker;
  /** Native chat exit behavior. Zero (the default) clears only the
   * input rows, keeps queue and status rows, and returns at column zero on the
   * current input row. Non-zero clears the whole prompt area and returns below
   * the transcript instead. */
  int clear_prompt_on_exit;
  /** Non-zero disables built-in Ctrl-V X11 PNG/JPEG capture. Zero (the
   * default) enables it; a Ctrl-V key binding still takes precedence. Linux
   * DISPLAY hosts use bundled MIT c-ares for literal IPv4/IPv6, /etc/hosts,
   * then /etc/resolv.conf DNS; no libc NSS modules or extra link library. */
  int disable_image_paste;
  /** Optional image path template resolving to an absolute path, with exactly
   * one `*` placeholder
   * for a 20-character xid. Softline copies it when the handle is created.
   * NULL (default) uses the softline directory under XDG_CACHE_HOME, falling
   * back to the softline directory under $HOME/.cache. Templates may begin
   * with `~/` and use `{{HOME}}`,
   * `{{home}}`, `{{XDG_CACHE_HOME}}`, or `{{xdg_cache_home}}`. XDG tokens use
   * XDG_CACHE_HOME when absolute, otherwise $HOME/.cache. The selected image
   * type always adds `.png` or `.jpeg` after expansion. Parent directories
   * are created recursively. For example, the template formed by
   * `/tmp/softline/` + `*` + `/image` produces
   * `/tmp/softline/{xid}/image.png`. */
  const char *image_paste_path_template;
  /** NULL keeps the native local queue. A backend requires prompt_queue=1 and
   * selects external authority for this handle's entire lifetime. */
  const sl_queue_backend_t *queue_backend;
  /** Borrowed backend callback context, released only after editor teardown. */
  void *queue_backend_context;
  /** OSC 8 policy; default OFF. See sl_get_osc8_info(). */
  sl_osc8_mode_t osc8_mode;
} sl_config_t;

/**
 * Public receiver shell for one editor instance.
 *
 * Method fields are initialized by sl_create() and sl_create_with_config() and
 * mirror the sl_* wrapper functions. The impl field is private implementation
 * state; callers must not read, write, copy, or free it.
 */
struct sl {
  /**
   * Read one submitted line.
   *
   * Returns a softline-allocated string on submitted input, or NULL for EOF,
   * cancellation, interrupt, suspension, or error. Use last_readline_status()
   * before the next readline() call to classify a NULL return. While raw mode
   * is owned, kernel tab expansion (TAB3/OXTABS) is disabled; OPOST/ONLCR are
   * preserved and original flags are restored on release. Optional
   * ordinary-readline scroll-region ownership is acquired at startup and
   * retried between producer callbacks as output advances the prompt. Multiple
   * feeds in one callback stay immediate; ownership is refreshed at the next
   * input-owned boundary. Ctrl-C restores terminal state before raising SIGINT.
   * If the handler returns, an existing native output session remains
   * available. SL_KEY_ACTION_SUSPEND retains the draft, cursor and editing
   * context for the next readline()/next_prompt(), ahead of queued delivery. It
   * does not pause producers. The application owns exclusive terminal handoff
   * between calls.
   */
  char *(*readline)(sl_t *self, const char *prompt);
  /** Destroy the handle; NULL-safe through sl_destroy(). */
  void (*destroy)(sl_t *self);
  /** Release strings returned by softline with the handle's allocator; NULL is
   * ignored. */
  void (*free_string)(sl_t *self, char *ptr);
  /** Add one entry and synchronously persist through any attached append hook.
   * Empty/consecutive duplicates and a zero cap do not append; hook failure
   * preserves recall. */
  int (*history_add)(sl_t *self, const char *line);
  /** Set the maximum retained history length; zero clears and disables history.
   */
  int (*history_set_max_len)(sl_t *self, int max_len);
  /** Overwrite a private snapshot file using escaped records; does not lock or
   * compact an attached native store. */
  int (*history_save)(sl_t *self, const char *filename);
  /** Import a snapshot into current bounded history, bypassing append hooks. */
  int (*history_load)(sl_t *self, const char *filename);
  /** Set ordinary readline wrapping width; zero follows the terminal.
   * Native chat always uses physical terminal width. Updates active prompt
   * wrapping when its effective width changes. Without an active readline,
   * changing this hint does not move or resize a native output session. */
  int (*set_screen_width)(sl_t *self, int width);
  /** Enable or disable bottom-pinned scroll-region output for ordinary
   * readline. The setting applies to subsequent print_above() calls; native
   * chat always uses its own scroll region. Acquisition occurs at startup or
   * between producer callbacks, without requesting cursor replies in feeds. */
  int (*set_live_scroll_region)(sl_t *self, int enabled);
  /** Register or clear the per-handle idle callback. */
  int (*set_idle_callback)(sl_t *self, sl_idle_callback_t callback,
                           void *userdata);
  /** Bind one decoded key to a per-handle callback. An Enter callback
   * requesting SL_KEY_ACTION_SUBMIT can submit reverse-search matches
   * immediately. */
  int (*bind_key)(sl_t *self, sl_key_t key, sl_key_callback_t callback,
                  void *userdata);
  /** Insert text bytes at the active cursor position. UTF-8 text is kept
   * cluster-safe. Mutations remain undoable while the editor is suspended. */
  int (*insert)(sl_t *self, const char *text);
  /** Replace the active buffer with text bytes and move the cursor to the end.
   * Mutations remain undoable while the editor is suspended.
   */
  int (*set_buffer)(sl_t *self, const char *text);
  /** Return the active buffer as a handle-owned NUL-terminated string. */
  const char *(*buffer)(const sl_t *self);
  /** Return the active cursor position as a byte offset into buffer(). */
  size_t (*cursor)(const sl_t *self);
  /** Move the active cursor to a byte offset, clamped to a UTF-8 cluster
   * boundary. */
  int (*set_cursor)(sl_t *self, size_t cursor);
  /** Submit the active buffer or reverse-search match through queue admission.
   * Idle/watch submissions have ordinary queued intent. */
  int (*submit)(sl_t *self);
  /** Cancel the active readline() call from a callback or idle hook. */
  int (*cancel)(sl_t *self);
  /** Suspend the active editor from an owner-thread callback, preserving the
   * draft/undo and returning SL_READLINE_SUSPENDED without stopping backlog. */
  int (*suspend)(sl_t *self);
  /** Stream callback output above the active prompt. Ordinary readline clears
   * and redraws by default, or uses an enabled live scroll region after
   * reaching the terminal bottom. Native chat retains its prompt frame and
   * reconciles physical resize before writing each callback-produced chunk.
   * Feed calls have no intentional delay and never wait for cursor replies.
   * The unobserved-resize limits documented for output_stream_write() also
   * apply when resize occurs inside this producer callback.
   * Finite output cannot overlap an open live session. Failed native finite
   * output discards partial ANSI/UTF-8 bytes; later calls start cleanly. */
  int (*print_above)(sl_t *self, sl_stream_callback_t callback, void *userdata);
  /** Return the most recent readline() status for this handle. */
  sl_readline_status_t (*last_readline_status)(const sl_t *self);
  /** Return the most recent handle-owned diagnostic string, or NULL. */
  const char *(*last_error)(const sl_t *self);
  /** Private implementation pointer; callers must not read or modify it. */
  void *impl;
  /** Auto-deliver the next queued prompt FIFO, or read a direct/promotion
   * result. Manual delivery always opens the editor. On interactive handles,
   * this retains terminal input ownership between results; destroy() restores
   * the terminal. source may be NULL. */
  char *(*next_prompt)(sl_t *self, const char *prompt,
                       sl_prompt_source_t *source);
  /** Enable/configure Tab queueing; reducing capacity below queued work
   * fails. */
  int (*set_prompt_queue)(sl_t *self, int enabled, int max_entries,
                          int preview_entries);
  /** Select one of the built-in themes for the complete interactive prompt
   * UI. */
  int (*set_prompt_theme)(sl_t *self, sl_prompt_theme_t theme);
  /** Enable or disable the status line and select its first palette index;
   * successive elements cycle through the theme's eight colours. */
  int (*set_statusline)(sl_t *self, int enabled, size_t starting_element);
  /** Replace status-line elements; inputs beyond 32 are rendered with a final
   * ellipsis element. */
  int (*set_status_elements)(sl_t *self, const char *const *elements,
                             size_t count);
  /** Set, replace, or clear one retained status-line element. */
  int (*set_status_element)(sl_t *self, size_t index, const char *element);
  /** Set busy state; busy renders x (or a spinner), while idle uses the
   * configured marker, which defaults to green +. In the queued-turns profile
   * with automatic delivery, busy retains turns and idle releases one
   * ordinary queued turn into an active next_prompt() call. */
  int (*set_status_busy)(sl_t *self, int busy);
  /** Enable or disable the 500ms /-\\| busy spinner. */
  int (*set_status_spinner)(sl_t *self, int enabled);
  /** Set the printable ASCII idle marker, or '\0' for a blank reserved slot. */
  int (*set_status_idle_marker)(sl_t *self, char marker);
  /** Return queued entries, excluding an active edit's reserved slot. */
  size_t (*prompt_queue_count)(const sl_t *self);
  /** Return this handle's configured prompt queue capacity. */
  size_t (*prompt_queue_capacity)(const sl_t *self);
  /** Copy one oldest-first queue entry into a softline-allocated string. */
  int (*prompt_queue_peek)(const sl_t *self, size_t index, char **out);
  /** Insert text with an explicit approval hold (0 or 1). Return its stable
   * identity through optional out_id, which is zeroed on failure. Automatic
   * approval overrides a requested hold. */
  int (*prompt_queue_insert)(sl_t *self, size_t index, const char *text,
                             int held, sl_prompt_id_t *out_id);
  /** Append nonempty text and optionally return its identity. held is 0 or 1;
   * held entries cannot be delivered or promoted until approved. Automatic
   * approval overrides a requested hold. */
  int (*prompt_queue_append)(sl_t *self, const char *text, int held,
                             sl_prompt_id_t *out_id);
  /** Replace one oldest-first queue entry with nonempty text. */
  int (*prompt_queue_replace)(sl_t *self, size_t index, const char *text);
  /** Remove one eligible queue entry and transfer it to the caller. Refuses
   * approval/execution holds, stopped entries and policy ordering barriers. */
  int (*prompt_queue_take)(sl_t *self, size_t index, char **out);
  /** Clear queued entries without changing the active editor draft. */
  int (*prompt_queue_clear)(sl_t *self);
  /** Atomically queue the active nonempty draft and clear the editor.
   * An empty queued edit deletes its entry instead. */
  int (*prompt_queue_enqueue_draft)(sl_t *self);
  /** Inspect one oldest-first entry's identity, mode and approval hold. */
  int (*prompt_queue_info)(const sl_t *self, size_t index,
                           sl_prompt_queue_entry_info_t *out);
  /** Inspect the active draft's queued origin; id is zero for an ordinary
   * draft. Alt-E reserves its queue position and capacity until requeue,
   * delivery or rejection.
   * Alt-N or empty submission/requeue deletes the entry. Cancellation, EOF and
   * read failure restore the original entry with its identity/hold. Undo after
   * Alt-N recovers text only, as an ordinary draft without the removed ID. */
  int (*prompt_queue_draft_info)(const sl_t *self,
                                 sl_prompt_queue_entry_info_t *out);
  /** Release one queued or actively edited entry's hold by stable identity.
   * Only the oldest pending approval may be decided; out-of-order requests
   * fail with SL_ERROR_INVALID. Approving does not change QUEUED/STEER intent
   * or queue-wide delivery. */
  int (*prompt_queue_approve)(sl_t *self, sl_prompt_id_t id);
  /** Discard a queued or actively edited entry by stable identity. Rejecting
   * a held entry follows the same FIFO approval order. Rejecting an active
   * edit clears that draft; other entries/drafts are untouched. */
  int (*prompt_queue_reject)(sl_t *self, sl_prompt_id_t id);
  /** Identity returned by the last successful readline(), next_prompt() or
   * queue take; zero for a direct draft or an unsuccessful read/take. */
  sl_prompt_id_t (*last_prompt_id)(const sl_t *self);
  /** Inject a queued prompt with approval governed by auto_approval. Returns
   * its stable ID through optional out_id, zeroed on failure. */
  int (*inject_queued_prompt)(sl_t *self, const char *text,
                              sl_prompt_id_t *out_id);
  /** Inject a steering prompt under the same approval policy. Approved
   * steering entries remain host-delivered at a safe steering seam. */
  int (*inject_steer_prompt)(sl_t *self, const char *text,
                             sl_prompt_id_t *out_id);
  /** Set conversation-wide automatic approval (0 or 1), initially off.
   * Enabling approves every waiting entry, including an active edit, and
   * future injections. Disabling does not revoke existing approvals. */
  int (*set_auto_approval)(sl_t *self, int enabled);
  /** Return this conversation instance's automatic approval setting. */
  int (*get_auto_approval)(const sl_t *self, int *out);
  /** Set automatic or host-controlled queued delivery for next_prompt(). This
   * also controls whether queued-turns releases ordinary entries on idle.
   * Busy queued-turns retains ordinary profile entries; managed entries use
   * their explicit execution readiness. */
  int (*set_prompt_queue_delivery)(sl_t *self,
                                   sl_prompt_queue_delivery_t delivery);
  /** Return this handle's queued delivery mode. */
  int (*get_prompt_queue_delivery)(const sl_t *self,
                                   sl_prompt_queue_delivery_t *out);
  /** Select built-in default or queued-turns prompt queue behavior. */
  int (*set_prompt_queue_profile)(sl_t *self,
                                  sl_prompt_queue_profile_t profile);
  /** Return this handle's selected prompt queue profile. */
  int (*get_prompt_queue_profile)(const sl_t *self,
                                  sl_prompt_queue_profile_t *out);
  /** Set built-in prompt queue action keys; duplicate non-NONE keys fail. */
  int (*set_prompt_queue_keys)(sl_t *self, const sl_prompt_queue_keys_t *keys);
  /** Return this handle's built-in prompt queue action keys. */
  int (*get_prompt_queue_keys)(const sl_t *self, sl_prompt_queue_keys_t *out);
  /** Install an optional owner-thread submission classifier; NULL removes it.
   * Policy-managed entries use their explicit execution boundary instead of
   * the busy flag. Manual delivery still prevents automatic release. */
  int (*set_prompt_queue_policy)(sl_t *self,
                                 sl_prompt_queue_policy_callback_t callback,
                                 void *userdata);
  /** Set execution readiness by identity, including an active edit. Readiness
   * never approves or resumes an entry. Schedules native delivery when ready.
   */
  int (*prompt_queue_set_executable)(sl_t *self, sl_prompt_id_t id,
                                     int executable);
  /** Inspect cancellation's backlog stop, independently of delivery mode. */
  int (*get_prompt_queue_stopped)(const sl_t *self, int *out);
  /** Explicitly resume the retained backlog without changing manual delivery,
   * approval, execution readiness or editing state. */
  int (*prompt_queue_resume)(sl_t *self);
  /** Register an application-owned descriptor to wake active TTY editing;
   * non-TTY handles reject watches. */
  int (*watch_add)(sl_t *self, int fd, unsigned int events,
                   sl_watch_callback_t callback, void *userdata,
                   sl_watch_id_t *out_id);
  /** Change one watch's requested readiness conditions. */
  int (*watch_modify)(sl_t *self, sl_watch_id_t id, unsigned int events);
  /** Remove one registered watch. */
  int (*watch_remove)(sl_t *self, sl_watch_id_t id);
  /** Remove every registered watch from this handle. */
  int (*watch_clear)(sl_t *self);
  /** Begin one owner-thread, renderer-agnostic live output session. Only one
   * session may be open per handle. Output occupies terminal rows above the
   * prompt. A full-terminal transcript starts at the current cursor and the
   * editable prompt starts at the bottom. Physical resize preserves the
   * terminal's position for an unchanged frame; only prompt layout changes
   * update cells. Before that frame, output uses the full terminal and leaves
   * the producer cursor in place between writes. Native output bytes pass
   * through unchanged; wrapping and scrollback are preserved. The output
   * margin follows actual prompt
   * height. At least three terminal rows are required: two output rows and one
   * prompt row. An unfinished line clipped into scrollback continues at the
   * first visible output row without replay. A clipped hard line boundary
   * resumes next to the prompt; existing scrollback cells and spacing are
   * preserved. Rapid tmux resizes can leave its grid ahead of the reported
   * PTY size while output is emitted; exact output during that interval is
   * not guaranteed. With either descriptor off-TTY, output is validated and
   * forwarded without terminal controls. Reopening a closed native session
   * probes the cursor again. A nonzero column restarts quote separation for
   * an unfinished externally written line; otherwise known spacing remains.
   * A session retained by an active editor keeps its continuation. */
  int (*output_stream_begin)(sl_t *self);
  /** Reconcile a settled resize before resuming synchronous producer writes.
   * Call on the owner thread, outside a feed/callback producer. Repaint only
   * owned prompt cells, preserving draft, cursor, queue, history, status,
   * queued input and incremental producer parsing. No transcript is retained
   * or replayed. A needed cursor report may wait up to 100 ms; failure returns
   * SL_ERROR_IO, and the caller must not resume output assuming reconciliation.
   * An open stream is required (otherwise SL_ERROR_INVALID). Redirected
   * sessions are a no-op. Terminal modes are restored before return. */
  int (*output_stream_reconcile)(sl_t *self);
  /** Forward exactly length bytes into the open session. Complete parsed
   * input is visible before return; a write boundary adds no newline or
   * document boundary. bytes may be NULL only when
   * length is zero. Input must be printable UTF-8, LF/CR/Tab, ANSI SGR, or
   * OSC 8 when enabled by set_osc8_mode();
   * unsupported terminal controls fail with SL_ERROR_INVALID. Malformed UTF-8
   * also fails. Native LF positioning follows the output TTY's OPOST/ONLCR
   * settings without altering producer bytes. Kernel tab expansion (TAB3 or
   * OXTABS) is temporarily disabled while Softline owns raw mode and restored
   * on exit; tabs reach the terminal unchanged. Native writes after raw-mode
   * release disable expansion for the write and restore it before returning,
   * without flushing pending input. Feed calls add no intentional delay and
   * never request or wait for terminal cursor replies, including on resize.
   * Cursor advances use local scalar cell widths shared with prompt layout;
   * no producer transcript, glyph spans or grapheme tail is buffered.
   * Terminal Unicode versions and cluster shaping can differ from the
   * Unicode 16.0 scalar accounting.
   * A physical resize can race a VT write and misplace even ASCII output;
   * this accepted limit cannot be made atomic without a terminal protocol.
   * A retained frame resized without an input-owned observation (including
   * after readline returns) uses a delta estimate. Earlier hard-ended ASCII
   * reflow and unknown terminal history can misplace continuation even after
   * resize settles. The active input loop observes resize before dispatching
   * producer watches. Between synchronous writes, use output_stream_reconcile()
   * after resize settles and before feeding again; feeds never observe input.
   * Wrapping remains producer/terminal-owned. Width reflow of a Unicode/tab
   * line can misplace continuation, including CR tails; its tracked column is
   * retained and clamped only if offscreen. No grapheme buffering or
   * renderer-specific wrapping is used. With an active prompt, continuing after
   * a known ASCII write at the right edge advances a hard row; width growth may
   * leave it split and a divided Unicode cluster may lose its attachment.
   * Bottom-margin reflow can likewise leave a continuation split. Transcript
   * bytes are not replayed to repair either boundary. A last-column report
   * cannot distinguish Unicode pending wrap from a cursor before the final
   * cell; continuation can overwrite that cell. These are intentional
   * opaque-feed limits; transcript cells are never reconstructed or cleared to
   * compensate. */
  int (*output_stream_write)(sl_t *self, const char *bytes, size_t length);
  /** End the open session without finishing an external renderer document.
   * An active editor keeps its native prompt and scroll region for later
   * finite output or another stream. Otherwise native chat closes: input rows
   * are cleared, queue and status rows remain, and the cursor stays at column
   * zero on the current input row without a newline. clear_prompt_on_exit
   * selects clearing the whole prompt area and returning below the transcript.
   * Closing outside an active editor releases terminal input ownership. A
   * foreground child may run before output_stream_begin() probes a fresh
   * cursor and builds a new prompt; handle history and settings are retained.
   * Run this handoff between editor calls, outside active editor callbacks;
   * the application owns child execution.
   * Incomplete ANSI/UTF-8 leaves it open and returns SL_ERROR_INVALID so the
   * caller may supply the missing bytes. */
  int (*output_stream_end)(sl_t *self);
  /** Set, replace, or clear the italic message above the status line.
   * The default prefix is "! "; long text wraps at words with continuation
   * rows indented to the prefix width. NULL or an empty string clears it.
   * Text must be printable single-line UTF-8. */
  int (*set_status_message)(sl_t *self, const char *message);
  /** Read one queued entry's delivery intent. */
  int (*prompt_queue_get_mode)(const sl_t *self, size_t index,
                               sl_prompt_queue_mode_t *out);
  /** Change one queued entry's delivery intent. */
  int (*prompt_queue_set_mode)(sl_t *self, size_t index,
                               sl_prompt_queue_mode_t mode);
  /** Write literal submitted text into the open stream as a themed, italic
   * quote. Repeat the prefix on each wrapped row and add only the line breaks
   * needed for one empty row on either side. Preceding bytes must form a
   * complete ANSI/UTF-8 sequence; text may contain printable UTF-8, LF, and
   * Tab. */
  int (*output_stream_write_quoted_prompt)(sl_t *self, const char *text);
  /** Set a nonempty, printable, single-line UTF-8 quote prefix. NULL restores
   * the default "> ". The prefix is repeated on each visible wrapped row. */
  int (*set_quoted_prompt_prefix)(sl_t *self, const char *prefix);
  /** Override quoted-prompt RGB colours. NULL restores theme defaults. */
  int (*set_quoted_prompt_style)(sl_t *self, const sl_quote_style_t *style);
  /** Set a printable single-line status prefix. NULL restores "! "; an
   * empty string hides it. It appears only while a message is present. */
  int (*set_status_message_prefix)(sl_t *self, const char *prefix);
  /** Choose palette roles independently for prefix and italic message text. */
  int (*set_status_message_colors)(sl_t *self, sl_theme_color_t prefix_color,
                                   sl_theme_color_t text_color);
  /** Set an absolute image path template containing exactly one `*`, or NULL
   * to restore the XDG cache default. Accepts `~/`, `{{HOME}}`, `{{home}}`,
   * `{{XDG_CACHE_HOME}}`, and `{{xdg_cache_home}}`. The handle copies the
   * string. Appends `.png` or `.jpeg` after substituting a 20-character xid. */
  int (*set_image_paste_path_template)(sl_t *self, const char *path_template);
  /** Attach custom load/append hooks and load immediately. See
   * sl_history_set_backend() for ownership and failure semantics. */
  int (*history_set_backend)(sl_t *self, const char *key,
                             sl_history_load_callback_t load,
                             sl_history_append_callback_t append,
                             void *userdata);
  /** Load and attach the native keyed store; see sl_history_open(). */
  int (*history_open)(sl_t *self, const char *key, const char *directory);
  /** Detach persistence, retaining in-memory history and auto-add policy. */
  int (*history_close)(sl_t *self);
  /** Compact the native store under its lock; see sl_history_compact(). */
  int (*history_compact)(sl_t *self);
  /** Opt into recording accepted editor submissions and queued drafts. Zero
   * (default) keeps application-controlled history_add(). Queue delivery,
   * promotion, search selection, cancellation and rejected drafts are not
   * recorded. Programmatic queue insert/replace requires explicit history_add.
   * Empty and consecutive duplicate entries are suppressed. */
  int (*history_set_auto_add)(sl_t *self, int enabled);
  /** Return nonzero while reverse history search is active, including a
   * failed match. Zero after selection, cancellation or editor exit. Key
   * callbacks run before built-in handling: return SL_KEY_ACTION_PASS during
   * search to let Enter select an editable match before submission guards run.
   * Read-only; call on the editor-owner thread. */
  int (*history_search_active)(const sl_t *self);
  /** Hand off a typed backend request; see sl_queue_backend_request(). */
  int (*queue_backend_request)(sl_t *self, const sl_queue_request_t *request,
                               sl_queue_request_id_t *out_request_id);
  /** Apply an asynchronous response on the owner thread. */
  int (*queue_backend_result)(sl_t *self, const sl_queue_result_t *result);
  /** Apply a bounded ordered backend observation on the owner thread. */
  int (*queue_backend_update)(sl_t *self, const sl_queue_change_t *change);
  /** Inspect backend mode, synchronization and bounded pending counts. */
  int (*queue_backend_info)(const sl_t *self, sl_queue_backend_info_t *out);
  /** Inspect one borrowed accepted backend entry in view order. */
  int (*queue_backend_entry)(const sl_t *self, size_t index,
                             sl_queue_entry_t *out);
  /** Inspect retained requests, oldest first. */
  int (*queue_backend_pending)(const sl_t *self, size_t index,
                               sl_queue_pending_info_t *out);
  /** Recover definitely rejected proposal text into an empty draft, without
   * backend mutation or automatic retry. Unknown outcomes cannot be recovered.
   */
  int (*queue_backend_recover)(sl_t *self, sl_queue_request_id_t request_id);
  /** Replace trigger registrations; see sl_set_completion_providers(). */
  int (*set_completion_providers)(sl_t *self,
                                  const sl_completion_provider_t *providers,
                                  size_t count);
  /** Set hyperlink policy between streams; see sl_set_osc8_mode(). */
  int (*set_osc8_mode)(sl_t *self, sl_osc8_mode_t mode);
  /** Read cached hyperlink policy/detection; see sl_get_osc8_info(). */
  int (*get_osc8_info)(const sl_t *self, sl_osc8_info_t *info);
};

/**
 * Fill config with default file descriptors, wrapping width, history, and line
 * limits.
 *
 * Passing NULL is ignored. Defaults are stdin/stdout file descriptors,
 * ordinary readline rendering, 100 history entries, and a 128 KiB editable
 * line. Prompt queueing and live scroll regions are disabled; the queue
 * defaults to a capacity of 64 and three previews when enabled. The prompt
 * theme is SL_PROMPT_THEME_DEFAULT, the status line and spinner are disabled,
 * busy is false, the idle status marker is '+', and clear_prompt_on_exit is
 * zero. X11 image paste is enabled and uses the XDG cache directory. OSC 8
 * hyperlinks are disabled (SL_OSC8_OFF).
 */
void sl_config_init(sl_config_t *config);

/**
 * Create a handle with default configuration.
 *
 * Returns a new receiver shell on success, or NULL on allocation failure. The
 * caller owns the handle and must release it with sl_destroy() or destroy().
 */
sl_t *sl_create(void);

/**
 * Create a handle from config.
 *
 * Passing NULL uses the same defaults as sl_create(). On success the caller
 * owns the returned handle and must release it with sl_destroy() or destroy().
 */
sl_t *sl_create_with_config(const sl_config_t *config);

/**
 * readline() returns a project-allocated string on submitted input and NULL for
 * EOF, cancellation, interrupt, suspension, or error. Call
 * sl_last_readline_status() before the next readline() call to classify NULL
 * precisely. While raw mode is owned, kernel tab expansion (TAB3/OXTABS) is
 * disabled; OPOST/ONLCR are preserved and original flags are restored on
 * release. Ctrl-C restores terminal state before raising SIGINT. A returning
 * handler leaves an existing native output session available.
 * SL_KEY_ACTION_SUSPEND retains the editor for the next
 * readline()/next_prompt(), before queued delivery, including completion
 * snapshots/Tab cycles and any unstripped trigger. Terminal handoff and
 * producer coordination belong to the application, outside callbacks, using
 * output_stream_end()/begin().
 */
char *sl_readline(sl_t *self, const char *prompt);

/**
 * Return the next application prompt. Automatic delivery returns queued prompts
 * FIFO without entering the terminal editor and sets *source to
 * SL_PROMPT_SOURCE_QUEUED. Manual delivery always opens the editor while
 * retaining queued entries. Queued-turns cancellation retains drafts and
 * stops automatic release until a direct submit or Alt-Enter promotion. A
 * queued-turns Alt-Enter promotion sets *source to SL_PROMPT_SOURCE_PROMOTED.
 * Otherwise submitted editor text is direct. On interactive handles,
 * next_prompt() retains terminal input ownership between results; sl_destroy()
 * or closing an idle native output session restores the terminal.
 * source may be NULL. With a constructor-selected queue backend, accepted work
 * is never returned as local model input: the editor stays active across
 * offers and applies deferred backend observations through owner callbacks.
 * Backend editing requires a TTY; headless hosts use typed backend requests.
 */
char *sl_next_prompt(sl_t *self, const char *prompt,
                     sl_prompt_source_t *source);

/**
 * Destroy a handle and restore terminal state owned by it.
 *
 * This function is NULL-safe. Do not use self or any handle-owned buffer after
 * destruction.
 */
void sl_destroy(sl_t *self);

/** Release a string returned by softline; NULL ptr is ignored. */
void sl_free_string(sl_t *self, char *ptr);

/** Add one non-NULL entry, invoking the attached append hook synchronously.
 * Empty and consecutive duplicate entries are ignored. A zero history cap
 * disables recording and persistence. Hook failure leaves memory unchanged. */
int sl_history_add(sl_t *self, const char *line);

/** Set the maximum retained history length; zero clears and disables history.
 */
int sl_history_set_max_len(sl_t *self, int max_len);

/** Overwrite filename with a private snapshot. Escape backslash, LF, CR and
 * Tab; UTF-8 stays literal. This API does not use native store locking. */
int sl_history_save(sl_t *self, const char *filename);

/** Import snapshot records into existing bounded history, bypassing append.
 * Decode backslash, LF, CR and Tab escapes; accept CRLF separators and an
 * unterminated final record. Invalid/oversized records are skipped with error.
 */
int sl_history_load(sl_t *self, const char *filename);

/** Attach a backend and load entries oldest first into the existing bounded
 * history. key must be nonempty and is copied. load may be NULL; append is
 * required. Callbacks return SL_OK or a negative status and may not reenter
 * history methods or destroy this handle. On failure the previous backend and
 * history remain intact. Caller-owned userdata must outlive the attachment.
 * Loading, including history_load(), bypasses append. No automatic editor
 * recording is enabled; use history_add() or history_set_auto_add(). */
int sl_history_set_backend(sl_t *self, const char *key,
                           sl_history_load_callback_t load,
                           sl_history_append_callback_t append, void *userdata);

/** Load and attach the native append-only store. Nonempty key's exact bytes
 * map to a lowercase SHA-256 filename with .history suffix. directory overrides
 * the complete storage directory and must be absolute; NULL uses
 * $XDG_STATE_HOME/softline/history, falling back to $HOME/.local/state when
 * XDG_STATE_HOME is empty or relative. Missing directories are recursively
 * created (0700); leaf directory and regular files must be owned by this user.
 * Files and lock files are 0600, with symlinks rejected. Missing history is
 * empty. Records escape backslash, LF, CR and Tab; UTF-8 stays literal. Each
 * accepted entry is appended immediately under a stable per-key flock lock,
 * also serializing independent handles in one process. Incomplete final records
 * are ignored on load and removed before append. No exit-time save, automatic
 * compaction or per-entry fsync is performed. Failures preserve the previous
 * attachment/history; last_error() explains. Same-key handles/processes share
 * storage, not live in-memory recall. */
int sl_history_open(sl_t *self, const char *key, const char *directory);

/** Detach persistence without changing retained entries or auto-add policy.
 * No callbacks or final save are performed. Idempotent outside callbacks. */
int sl_history_close(sl_t *self);

/** Explicitly compact an attached native store to the newest history_max_len
 * entries by rereading the shared file under its stable lock and replacing it
 * atomically. Does not overwrite other writers using a stale memory snapshot
 * or change this handle's recall list. Zero cap retains no entries. Custom or
 * detached backends return SL_ERROR_INVALID. */
int sl_history_compact(sl_t *self);

/** Enable automatic recording at editor submission and queue acceptance.
 * Disabled by default. See the receiver method for excluded operations.
 * Applications enabling this must not record delivered queued prompts again. */
int sl_history_set_auto_add(sl_t *self, int enabled);

/** Return nonzero while the editor is in reverse history search, including
 * when there is no match. Zero outside search or for NULL. Key callbacks run
 * before built-in key handling: return SL_KEY_ACTION_PASS during search to
 * allow Enter to select an editable match. Returns zero after selection,
 * cancellation or editor exit. Read-only; call on the editor-owner thread. */
int sl_history_search_active(const sl_t *self);

/** Set ordinary readline wrapping width; zero follows the terminal.
 * Native chat always uses physical terminal width. Updates active prompt
 * wrapping when its effective width changes. Without an active readline,
 * changing this hint does not move or resize a native output session. */
int sl_set_screen_width(sl_t *self, int width);

/** Enable or disable bottom-pinned scroll-region output for ordinary readline.
 * Disabled by default. Ownership is checked at startup and between producer
 * callbacks and acquired once the prompt reaches the terminal bottom.
 * Multiple feeds within one callback remain immediate and acquire ownership
 * at the next input-owned boundary. Native chat uses its own scroll region
 * regardless of this setting. */
int sl_set_live_scroll_region(sl_t *self, int enabled);

/** Set an absolute image path template containing exactly one `*`, or NULL
 * for the XDG cache default. Accepts `~/`, `{{HOME}}`, `{{home}}`,
 * `{{XDG_CACHE_HOME}}`, and `{{xdg_cache_home}}`. The handle copies the string.
 * A 20-character xid replaces `*`, then `.png` or `.jpeg` is appended. */
int sl_set_image_paste_path_template(sl_t *self, const char *path_template);

/** Enable/configure native local queueing for the interactive terminal editor;
 * disabling clears the queue. Editing is plain when either input or output is
 * not a TTY. Reducing the capacity below the current queue length returns
 * SL_ERROR_INVALID. Constructor-selected backend handles reject changing queue
 * enablement/capacity with SL_ERROR_UNSUPPORTED; use their bounded view and
 * typed requests for backend mutations. */
int sl_set_prompt_queue(sl_t *self, int enabled, int max_entries,
                        int preview_entries);

/** Return the number of oldest-first entries currently queued on self. */
size_t sl_prompt_queue_count(const sl_t *self);

/** Return the configured capacity of self's bounded prompt queue. */
size_t sl_prompt_queue_capacity(const sl_t *self);

/** Copy queue index into a softline-allocated string returned through out. */
int sl_prompt_queue_peek(const sl_t *self, size_t index, char **out);

/** Insert text with held 0 or 1 and optionally return its new identity.
 * Index zero is oldest. out_id is zeroed on failure. Auto-approval overrides
 * a requested hold. Native local queues only; backend handles use typed OFFER
 * requests and return SL_ERROR_UNSUPPORTED here. */
int sl_prompt_queue_insert(sl_t *self, size_t index, const char *text, int held,
                           sl_prompt_id_t *out_id);

/** Append nonempty text within line_max_len, with held 0 or 1, and optionally
 * return its new identity. out_id is zeroed on failure; held entries require
 * approval. Auto-approval overrides a requested hold. Native local queues only;
 * backend handles use typed OFFER requests. */
int sl_prompt_queue_append(sl_t *self, const char *text, int held,
                           sl_prompt_id_t *out_id);

/** Inject an ordinary queued message, held unless auto_approval is enabled.
 * Queueing must be enabled. Call on the editor owner thread, including from
 * a watch callback. out_id is optional and zeroed on failure. For native local
 * queues; backend handles receive accepted entries through ordered updates. */
int sl_inject_queued_prompt(sl_t *self, const char *text,
                            sl_prompt_id_t *out_id);
/** Inject a steering message with the same approval policy and stable ID.
 * Approval preserves steering intent; the host takes it at a safe seam. */
int sl_inject_steer_prompt(sl_t *self, const char *text,
                           sl_prompt_id_t *out_id);
/** Set auto_approval for this conversation's entire instance (strict 0 or 1).
 * Initially off. Enabling approves all waiting and edited entries as well as
 * future injections, without changing delivery intent or submitting a draft.
 * Turning it off affects subsequent approval requests only. Backend handles
 * use SL_QUEUE_AUTO_APPROVAL and observe the accepted state in updates. */
int sl_set_auto_approval(sl_t *self, int enabled);
/** Read auto_approval into out; invalid arguments return SL_ERROR_INVALID. */
int sl_get_auto_approval(const sl_t *self, int *out);

/** Replace native local queue index with nonempty text within line_max_len.
 * Index zero is the oldest entry. Backend handles use typed edit requests. */
int sl_prompt_queue_replace(sl_t *self, size_t index, const char *text);

/** Remove native local queue index and transfer its allocated text through out.
 * Unsupported in backend mode: the backend alone owns execution/delivery. */
int sl_prompt_queue_take(sl_t *self, size_t index, char **out);

/** Inspect one queued entry's delivery intent. New entries are QUEUED. */
int sl_prompt_queue_get_mode(const sl_t *self, size_t index,
                             sl_prompt_queue_mode_t *out);
/** Change one entry's delivery intent; the host chooses when to take STEER. */
int sl_prompt_queue_set_mode(sl_t *self, size_t index,
                             sl_prompt_queue_mode_t mode);

/** Clear queued entries without changing the active editor draft. */
int sl_prompt_queue_clear(sl_t *self);

/** Queue the active nonempty draft and clear it atomically while editing.
 * An empty queued edit deletes its entry; an ordinary empty draft is invalid.
 */
int sl_prompt_queue_enqueue_draft(sl_t *self);

/** Inspect an entry's stable identity, delivery intent and approval hold. */
int sl_prompt_queue_info(const sl_t *self, size_t index,
                         sl_prompt_queue_entry_info_t *out);
/** Inspect a queued edit; id is zero for a draft without a queued origin.
 * Alt-E retains identity, mode and hold, reserving its queue position and
 * capacity. Enter or a submit callback requeues nonempty held edits there
 * without delivering them. Empty submission or requeue deletes the edited
 * entry, including approvals. Alt-N discards the active queued edit without
 * submission; undo recovers only its text as a fresh draft. Cancellation, EOF
 * or read failure restores the
 * original entry, discarding its edits. */
int sl_prompt_queue_draft_info(const sl_t *self,
                               sl_prompt_queue_entry_info_t *out);
/** Approve the oldest pending identity; does not force delivery. Later held
 * identities fail with SL_ERROR_INVALID locally or SL_ERROR_CONFLICT in backend
 * mode, preserving FIFO approval order. Backend SL_OK means asynchronous
 * handoff only; approval changes arrive through accepted updates. */
int sl_prompt_queue_approve(sl_t *self, sl_prompt_id_t id);
/** Remove an identity or clear its active edit. A held identity must be the
 * oldest pending approval; later held identities fail with SL_ERROR_INVALID
 * locally or SL_ERROR_CONFLICT in backend mode. Backend SL_OK means withdrawal
 * handoff only; the backend retains authority over the accepted entry. */
int sl_prompt_queue_reject(sl_t *self, sl_prompt_id_t id);
/** Last read/take identity, or zero for direct input, failure or cancellation.
 * IDs are handle-local, monotonic, never reused, and bounded by LONG_MAX. */
sl_prompt_id_t sl_last_prompt_id(const sl_t *self);

/** Set automatic or manual queued delivery. Busy queued-turns retains entries
 * regardless of this setting; manual lets the host take entries at chosen
 * seams and turn boundaries. */
int sl_set_prompt_queue_delivery(sl_t *self,
                                 sl_prompt_queue_delivery_t delivery);

/** Return the queued delivery mode through out. */
int sl_get_prompt_queue_delivery(const sl_t *self,
                                 sl_prompt_queue_delivery_t *out);

/** Select the default or busy-driven queued-turns queue interaction profile. */
int sl_set_prompt_queue_profile(sl_t *self, sl_prompt_queue_profile_t profile);

/** Return the selected prompt queue interaction profile through out. */
int sl_get_prompt_queue_profile(const sl_t *self,
                                sl_prompt_queue_profile_t *out);

/** Set built-in queue action keys; duplicate non-NONE keys are invalid. */
int sl_set_prompt_queue_keys(sl_t *self, const sl_prompt_queue_keys_t *keys);

/** Return built-in queue action keys through out. */
int sl_get_prompt_queue_keys(const sl_t *self, sl_prompt_queue_keys_t *out);

/** Install or remove the deliberate-submission policy callback. */
int sl_set_prompt_queue_policy(sl_t *self,
                               sl_prompt_queue_policy_callback_t callback,
                               void *userdata);

/** Change one queued/edited identity's execution readiness (0 or 1). */
int sl_prompt_queue_set_executable(sl_t *self, sl_prompt_id_t id,
                                   int executable);

/** Read the retained backlog's stopped state. */
int sl_get_prompt_queue_stopped(const sl_t *self, int *out);

/** Resume the backlog explicitly; all other delivery gates still apply. */
int sl_prompt_queue_resume(sl_t *self);

/** Select a built-in theme for the complete interactive prompt UI. */
int sl_set_prompt_theme(sl_t *self, sl_prompt_theme_t theme);

/** Enable or disable the status line and choose the palette index used for
 * its first element. Status elements wrap between elements where possible. */
int sl_set_statusline(sl_t *self, int enabled, size_t starting_element);

/** Set, replace, or clear the italic status message above the status line.
 * The default prefix is "! ". NULL or an empty string clears the message.
 * Long text wraps at words, with continuation rows indented to the display
 * width of the prefix. Text must be printable single-line UTF-8 without
 * control characters. */
int sl_set_status_message(sl_t *self, const char *message);

/** Set the status message prefix. NULL restores "! "; "" hides it. */
int sl_set_status_message_prefix(sl_t *self, const char *prefix);

/** Select theme palette roles for the prefix and italic message text.
 * Defaults are MUTED and SECONDARY respectively. */
int sl_set_status_message_colors(sl_t *self, sl_theme_color_t prefix_color,
                                 sl_theme_color_t text_color);

/** Replace all status-line elements. At most 32 elements are retained; longer
 * input is represented by the first 31 elements followed by `...`. Elements
 * must be valid UTF-8 without C0/C1 controls or DEL. */
int sl_set_status_elements(sl_t *self, const char *const *elements,
                           size_t count);

/** Set, replace, or clear one status-line element. index must be below
 * SL_STATUS_MAX_ELEMENTS; non-NULL text must be valid UTF-8 without C0/C1
 * controls or DEL. */
int sl_set_status_element(sl_t *self, size_t index, const char *element);

/** Set the status-line busy state. With the spinner disabled, busy renders x;
 * idle renders the configured marker (default '+'), or a blank slot for '\0'.
 * In the queued-turns profile with queueing enabled, this is the native turn
 * lifecycle signal: busy retains turns and idle releases one FIFO turn. */
int sl_set_status_busy(sl_t *self, int busy);

/** Enable or disable the 500ms /-\\| spinner used while status is busy. */
int sl_set_status_spinner(sl_t *self, int enabled);

/** Set a printable ASCII idle marker (default '+'), or '\0' for a blank slot.
 */
int sl_set_status_idle_marker(sl_t *self, char marker);

/** Register or clear an idle callback for this handle. */
int sl_set_idle_callback(sl_t *self, sl_idle_callback_t callback,
                         void *userdata);

/** Register an application-owned descriptor to wake an active TTY editor.
 * Softline never consumes or closes fd; non-TTY handles reject watches. Ready
 * watches are dispatched round-robin, with at most eight callbacks before
 * terminal input is given another chance to run. */
int sl_watch_add(sl_t *self, int fd, unsigned int events,
                 sl_watch_callback_t callback, void *userdata,
                 sl_watch_id_t *out_id);

/** Change one watch's requested readiness conditions. */
int sl_watch_modify(sl_t *self, sl_watch_id_t id, unsigned int events);

/** Remove one registered watch. */
int sl_watch_remove(sl_t *self, sl_watch_id_t id);

/** Remove every registered watch from this handle. */
int sl_watch_clear(sl_t *self);

/** Bind key to callback for this handle; NULL callback removes the binding.
 * Bind SL_KEY_ENTER with SL_KEY_ACTION_SUBMIT to submit a reverse-search match
 * immediately instead of accepting it into the editable prompt. */
int sl_bind_key(sl_t *self, sl_key_t key, sl_key_callback_t callback,
                void *userdata);

/** Insert text bytes at the active cursor on the editor-owner thread. During
 * active or suspended editing, mutations retain undo support. Matching spaces
 * can continue completion phrases; a separating space ends the activation. */
int sl_insert(sl_t *self, const char *text);

/** Replace the active buffer with text bytes and move the cursor to the end.
 * Undoable during active/suspended editing; ends any completion activation. */
int sl_set_buffer(sl_t *self, const char *text);

/** Return the active buffer, owned by the handle until it is changed or
 * destroyed. */
const char *sl_buffer(const sl_t *self);

/** Return the active cursor position as a byte offset into sl_buffer(). */
size_t sl_cursor(const sl_t *self);

/** Set cursor to a byte offset; invalid offsets are clamped to valid UTF-8
 * cluster boundaries. Ends typing coalescing and the current Tab cycle. */
int sl_set_cursor(sl_t *self, size_t cursor);

/** Submit the active readline() buffer or reverse-search match from a callback.
 * Applies queue admission; idle/watch submissions have ordinary queued intent.
 */
int sl_submit(sl_t *self);

/** Cancel the active readline() operation from inside a callback. */
int sl_cancel(sl_t *self);

/** Request the existing terminal-handoff suspension from an owner-thread idle
 * or fd callback. Like SL_KEY_ACTION_SUSPEND, preserve the draft, cursor, undo,
 * search and queue context. Returns SL_ERROR_INVALID outside active readline.
 * Run child processes after readline returns SL_READLINE_SUSPENDED. */
int sl_suspend(sl_t *self);

/** Write callback-produced chunks above the active prompt. Ordinary readline
 * clears and redraws by default, or uses an
 * enabled live scroll region once the prompt reaches the terminal bottom.
 * Native chat retains its prompt frame after a live session and reconciles
 * physical resize before writing each callback-produced chunk. Finite output
 * cannot overlap an open live session. The unobserved-resize limits documented
 * for output_stream_write() also apply when resize occurs inside this producer
 * callback. Feed calls never wait for cursor replies. Failed native finite
 * output discards partial ANSI/UTF-8 bytes; later calls start cleanly. */
int sl_print_above(sl_t *self, sl_stream_callback_t callback, void *userdata);

/** Start a persistent output session on self's editor-owner thread. The
 * caller owns its producer, wakeup, renderer, and document lifecycle. A
 * native transcript starts at the current cursor; the editable prompt is
 * initially at the bottom. Physical resize preserves the terminal's prompt
 * position; unchanged cells and the cursor remain untouched. Before that frame,
 * output uses the full terminal and leaves the producer cursor in place
 * between writes.
 * Output feeds preserve unchanged prompt cells; prompt updates patch only
 * changed cells. The output margin follows the current prompt frame height;
 * growth scrolls existing output cells
 * only enough to fit, and shrink returns rows without moving transcript cells.
 * At least three terminal rows are required: two scroll rows and one prompt
 * row. Native transcript reflow belongs to the terminal; Softline never clears
 * or replays it. An unfinished line clipped into scrollback continues at the
 * first visible output row. A clipped hard line boundary resumes next to the
 * prompt; existing scrollback cells and spacing are preserved.
 * Rapid tmux resizes can leave its grid ahead of the reported PTY size while
 * output is emitted; exact output during that interval is not guaranteed.
 * Returns SL_ERROR_INVALID if a session is already open.
 * Reopening a closed native session probes the cursor again. A nonzero column
 * restarts quote separation for an unfinished externally written line;
 * otherwise known spacing remains. An active editor keeps its
 * continuation state. */
int sl_output_stream_begin(sl_t *self);

/** Observe a settled resize and repaint the retained prompt before producer
 * continuation. Call on the owner thread outside producer feeds. An open
 * session is required; invalid/reentrant calls return SL_ERROR_INVALID.
 * Preserves draft/cursor, queue/history/status, queued input, terminal modes
 * and split UTF-8/SGR parsing. Never retains or replays transcript bytes.
 * A required cursor report waits at most 100 ms; failure returns SL_ERROR_IO.
 * Do not resume output assuming reconciliation after failure. No-op off-TTY.
 * Call before the first write after resize; feeds remain observation-free. */
int sl_output_stream_reconcile(sl_t *self);

/** Forward length bytes immediately to the live output session. Zero length
 * is a no-op; no newline or response boundary is implied. Valid bytes are
 * printable UTF-8, LF/CR/Tab, ANSI SGR, and opt-in OSC 8; malformed or
 * unsupported control sequences report SL_ERROR_INVALID. A failed write may
 * have emitted a prefix, while a successful write has emitted all complete
 * input units. Native LF positioning follows the output TTY's OPOST/ONLCR
 * settings; producer bytes are unchanged. Kernel tab expansion (TAB3 or OXTABS)
 * is temporarily disabled while Softline owns raw mode and restored on exit;
 * OPOST/ONLCR are preserved and tabs reach the terminal unchanged. Native
 * writes after raw-mode release disable expansion for the write and restore it
 * before returning, without flushing pending input. Feed calls add no
 * intentional delay and never request or wait for terminal cursor replies,
 * including on resize. Cursor advances use local scalar cell widths shared with
 * prompt layout, without retaining producer text, glyph spans or grapheme
 * tails. Terminal Unicode versions and cluster shaping can differ from the
 * Unicode 16.0 accounting. Wrapping remains producer/terminal-owned. A physical
 * resize can race the VT write and misplace even ASCII output; this is an
 * accepted concurrency limit. A retained frame resized without an input-owned
 * observation, including after readline returns, uses a delta estimate. Earlier
 * hard-ended ASCII reflow or unknown terminal history can misplace continuation
 * even after resize settles. The active input loop observes resize before
 * dispatching producer watches. Synchronous producers use
 * sl_output_stream_reconcile() after resize settles and before the next feed.
 * Feeds never observe input or model transcript text. Width reflow of an
 * unfinished Unicode/tab line can misplace continuation, including CR tails;
 * its tracked column is retained and clamped only if offscreen. No grapheme
 * buffering or renderer-specific wrapping is used. At a known ASCII right edge
 * with an active prompt, a later printable continuation uses CR and IND after
 * the cursor handoff. This hard row boundary can remain split after width
 * growth, and a Unicode cluster split there may lose its attachment. At the
 * bottom output margin, native reflow differs between terminals; width growth
 * may leave a continuation on a separate row. A last-column report cannot
 * distinguish Unicode pending wrap from a cursor before the final cell;
 * continuation can overwrite that cell. These are intentional opaque-feed
 * limits. Transcript cells are never reconstructed, cleared or replayed to
 * compensate. */
int sl_output_stream_write(sl_t *self, const char *bytes, size_t length);

/** End the output session. The caller remains responsible for finishing any
 * external renderer document first. An active editor keeps its native prompt
 * and scroll region, permitting later finite output or another stream. With
 * no active editor, native chat closes. By default, native input rows are
 * cleared, queue and status rows remain visible, and the TTY cursor returns at
 * column zero on the current input row without a newline or scroll.
 * clear_prompt_on_exit selects clearing the whole prompt area and returning
 * below output instead. Without a prompt, return below output.
 * Closing an idle native session also releases terminal input ownership.
 * Applications may run a foreground child and then output_stream_begin() to
 * probe its final cursor and create a fresh prompt, keeping handle history
 * and settings. This handoff must run outside active editor callbacks.
 * The application owns child execution.
 * Incomplete ANSI/UTF-8 returns SL_ERROR_INVALID and leaves the session open.
 */
int sl_output_stream_end(sl_t *self);

/** Write a submitted prompt as a themed quote into an open output stream.
 * Word-wrap at the current output width and prefix every visible row. Add only
 * the missing line breaks for one empty row before and after the quote.
 * Preceding streamed bytes must form a complete ANSI/UTF-8 sequence. The
 * input must be valid UTF-8 with printable characters, LF, and Tab only.
 * Formatting uses bounded internal chunks regardless of input length. */
int sl_output_stream_write_quoted_prompt(sl_t *self, const char *text);

/** Set a nonempty printable single-line UTF-8 prefix; NULL restores "> ". */
int sl_set_quoted_prompt_prefix(sl_t *self, const char *prefix);

/** Override per-theme quote colours, or pass NULL to restore theme defaults. */
int sl_set_quoted_prompt_style(sl_t *self, const sl_quote_style_t *style);

/** Atomically replace completion providers on the owner thread. NULL/count
 * zero clears them; invalid fields, duplicate triggers or copy failure keep
 * the previous registry. Registration changes end the current completion.
 * Each activation copies one callback-produced list, reused through typing,
 * Backspace, Undo and Tab cycling. Typed/yanked/host-inserted whitespace keeps
 * the activation when a candidate matches through it with a suffix remaining;
 * otherwise it ends the activation. Deleting the trigger, moving the cursor
 * outside its token, and submitting/replacing/queueing the draft also end it.
 * Candidates include the trigger and contain printable UTF-8, LF/Tab, bounded
 * by line_max_len. The first match is a dim
 * inline ghost immediately after the trigger; Tab accepts it and repeated
 * Tabs cycle the original matches. A new eligible word-boundary trigger starts
 * a fresh activation when the previous phrase stops matching.
 * Right/Ctrl-F accepts a ghost at its token endpoint or commits a Tab selection
 * without moving the cursor. With no further preview, it resumes ordinary
 * movement; a no-op at the draft end preserves the commit and its text.
 * Editing/navigation resets that cycle but retains the activation list inside
 * its token. Enter submits actual text, never the ghost. strip_trigger is
 * undoable and applied on normal exit or submission; replacing/discarding the
 * draft needs no extra deletion. Failed providers are not retried until another
 * activation. Optional expand callbacks run on Right/Ctrl-F commit and on Tab
 * outside an active cycle, including for fully present unique candidates.
 * Deeper lists are separate snapshots; Backspace selects the original parent
 * list. Without further candidates, a fully present unique match or no match
 * lets Tab reach ordinary host/queue handling. */
int sl_set_completion_providers(sl_t *self,
                                const sl_completion_provider_t *providers,
                                size_t count);

/** Set OSC 8 policy on the owner thread between output streams. Refused with
 * SL_ERROR_NOT_READY while a stream, partial control or producer link is open.
 * Refreshes passive detection from output_fd and the terminal environment.
 * ON overrides detection; AUTO stays disabled for unknown, redirected and
 * unverified multiplexer paths. No terminal query or input wait is performed.
 */
int sl_set_osc8_mode(sl_t *self, sl_osc8_mode_t mode);

/** Return cached detection and effective feed policy without terminal I/O.
 * detected means a recognized environment, not guaranteed terminal support.
 * Pass enabled to a renderer's hyperlink option. Only OSC 8 is enabled: other
 * OSC controls remain rejected. Native feed links cannot leak into editor UI.
 * Complete controls are bounded by SL_OSC8_MAX_BYTES; incomplete controls alone
 * may be retained across writes. Producer text is never buffered for replay.
 */
int sl_get_osc8_info(const sl_t *self, sl_osc8_info_t *info);

/** Return the status of the most recent readline() call on this handle. */
sl_readline_status_t sl_last_readline_status(const sl_t *self);

/** Return the most recent diagnostic string owned by the handle, or NULL. */
const char *sl_last_error(const sl_t *self);

/** Hand off one backend operation on the owner thread. Softline assigns the
 * request identity and copies bounded fields before calling the bridge. SL_OK
 * means handoff only. Synchronous reentry from the bridge is rejected.
 * Typed BEGIN_EDIT does not request a native editor; its accepted request
 * retires without readline, freeing the ordinary retention slot. END_EDIT and
 * ABORT_EDIT require the owned edit hold in the accepted mirror. */
int sl_queue_backend_request(sl_t *self, const sl_queue_request_t *request,
                             sl_queue_request_id_t *out_request_id);

/** Apply one deferred response without delivering/echoing model text. Duplicate
 * completed responses are harmless. Results never mutate the accepted mirror
 * or synchronously invoke the outbound callback. Native edit intent is retained
 * until its exact owned hold opens, is cancelled, or is reconciled as
 * unavailable. A late accepted acquisition missing from the view requests one
 * fresh snapshot at native dispatch; an existing newer draft remains editable
 * until cleared or submitted. Typed BEGIN_EDIT acceptance retires without
 * opening an editor.
 */
int sl_queue_backend_result(sl_t *self, const sl_queue_result_t *result);

/** Copy one ordered snapshot/delta. Validation/allocation failure preserves the
 * previous view. Revision gaps return SL_ERROR_CONFLICT and require a snapshot.
 * This method never invokes the outbound callback. */
int sl_queue_backend_update(sl_t *self, const sl_queue_change_t *change);

/** Inspect a borrowed backend-mode view; local handles report enabled=0. */
int sl_queue_backend_info(const sl_t *self, sl_queue_backend_info_t *out);

/** Inspect one borrowed accepted backend entry at zero-based index, including
 * ID and revision. The view stays valid until the next backend operation. */
int sl_queue_backend_entry(const sl_t *self, size_t index,
                           sl_queue_entry_t *out);

/** Inspect retained requests at zero-based index, oldest first. Rejected offers
 * retain their text; unknown outcomes must be resolved by original request ID.
 */
int sl_queue_backend_pending(const sl_t *self, size_t index,
                             sl_queue_pending_info_t *out);

/** Recover a rejected text proposal into an empty ordinary draft without
 * resubmitting. The corresponding retained rejection is removed only after the
 * copy succeeds. */
int sl_queue_backend_recover(sl_t *self, sl_queue_request_id_t request_id);

#ifdef __cplusplus
}
#endif

#endif
