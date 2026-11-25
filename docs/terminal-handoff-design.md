# Local and remote PTY handoff

The editor's `terminal_local` method opens a local PTY shell. Its
`terminal_handoff` method relays terminal I/O through a host-supplied adapter.
Local C and Lua chat examples use the local adapter. Remote C chat uses a
backend PTY over the demo transport.

`<softline/terminal.h>` exposes the server PTY as an independent receiver.
The remote example uses this public API; it neither includes private PTY headers
nor compiles a second PTY implementation. The client uses its existing terminal
in raw mode; only the server needs to create a PTY.

## Server session API

Initialize `sl_terminal_session_config_t`, then create a receiver with
`sl_terminal_session_create()`. Creation copies host-owned executable, argv,
directory and environment policy without spawning. NULL argv selects an
interactive shell; NULL environment inherits the host's environment. An optional
terminal type overrides TERM. Explicit environment lists replace the inherited
environment. Paths are executable paths, never shell command strings.

`session->dispatch()` and `sl_terminal_session_dispatch()` implement the same
OPEN/STEP/READ/WRITE/RESIZE/CLOSE event contract as the TUI relay. They require
neither an editor nor terminal stdin/stdout. STEP exposes a borrowed readiness
descriptor/deadline for the host loop. Dispatch at most 4096 bytes per READ or
WRITE, retain unconsumed input on partial writes, and stop reading output when
the transport or frontend has no capacity. OPEN validates rows/columns and is
single-use; create another receiver for a new session. READ returning zero is
output EOF; `status()` separately reports the child exit status.
An exec/setup failure is reported as soon as its status is known, even if the
failed child has not yet been reaped. It must never become a successful exit
merely because process teardown is still pending.
The example wire adapter preserves an unavailable exit status as -1 in the C
API (wire value 256), rather than inventing exit code zero.

CLOSE is idempotent, terminates the shell and foreground group, and never waits
for peer acknowledgment or child exit. `destroy()` performs nonblocking cleanup:
SL_ERROR_NOT_READY retains ownership with the caller, who must retry from the
event loop until SL_OK. There is no worker or hidden child reaper. Server
sessions install no process signal handlers and do not change host terminal
attributes. They may run on a headless host.
The built-in receiver owns child reaping. A host SIGCHLD handler should wake the
owner loop and leave this child to the receiver; a process-wide waitpid(-1)
reaper must not consume its status. Hosts requiring centralized child ownership
can supply their own PTY adapter through the generic terminal callback contract.

The transport maps these logical operations onto its own ordered messages.
Softline supplies no socket, authentication policy or mandatory wire encoding.
Session ownership is per frontend view; closing or losing that attachment
terminates its session, without shell persistence or automatic reattachment.

## Browser and Kore integration boundary

The browser assets include an xterm.js terminal. A Kore route or another
host-owned WebSocket endpoint can create one PTY session per authorized
connection and drive it from the host event loop. The host selects the shell,
arguments, environment and working directory. See the
[browser terminal guide](web-terminal.md) for routes, input and output flow
control, and the mock listener used by the examples.

## Behavior

Alt-S requests a new terminal session without submitting the draft. The host
chooses a local or remote adapter through the same session-opening callback.
A local adapter creates a real PTY and starts the host-selected shell; a remote
adapter obtains a PTY from the host's transport. The remote PTY may be on a third
machine, independent of the queue owner. Authentication, framing, credentials,
shell selection and environment belong to the host.

Each handoff opens a new session. Exit, failure or explicit termination returns
to the existing editor. Preserve the draft, byte cursor position, undo history,
kill ring, saved whole draft, history navigation, and queued-edit/approval
identity and holds.
Reflow the prompt at the current terminal size. Preserve the preferred editing
column unless wrapping width changed. Ordinary `!sh` and `/shell` submissions
use the same session path but still return to an empty prompt.

Handoff must retain the existing application policy for obtaining an exclusive
terminal seam. It does not approve, submit, cancel or reorder queued work.
Queue connectivity must not determine local shell availability; remote shell
availability is determined by its own adapter.

## Shared adapter contract

Softline owns the foreground byte relay and terminal release/restoration. Both
adapters supply the same operations and lifecycle:

| Operation | Required behavior |
| --- | --- |
| Open | Start a new session with the current rows and columns; report ready, pending or failed. |
| Readiness | STEP services ready events and supplies one pollable descriptor, interests, next deadline and readiness; multiplex other sources behind that descriptor. |
| Read | Produce available PTY output bytes, would-block, EOF or failure. |
| Write | Accept input bytes, allowing partial progress and would-block. |
| Resize | Accept the latest rows and columns without waiting for peer acknowledgment. |
| Close | Cancel pending opening or terminate an opened session; release frontend resources without waiting for remote completion. |

Callbacks run on the relay owner thread, return promptly and never reenter the
editor. Read/write buffers are borrowed for the callback duration. The host
retains its context until close returns; close occurs exactly once after an
opening attempt, including partial setup failure. Adapters clean up resources
they acquire. Local process cleanup must not block waiting for a child to exit;
the local adapter owns termination and arranges eventual reaping without keeping
the terminal session open. Remote termination is best effort after connection
loss and cannot be a prerequisite for restoring the prompt.

The relay services local input regardless of adapter readiness. A pending open,
blocked write or stalled close cannot monopolize its loop. Adapter deadlines
govern connection/setup failure, not how long a user may keep a healthy shell
open. Inactivity alone is not an error.

Use the existing suspension and output-session lifecycle. The editor receiver
offers `terminal_local` and `terminal_handoff` in C and Lua. The equivalent
C free functions and Lua module functions use the same relay. Suspension
retains the editor continuation. Callback operations are declared in
`sl_terminal_event_t` and `sl_terminal_operation_t`.

## Terminal and output ownership

Request suspension from a key, idle, or watch callback, then wait for the
editor call to return with `SL_READLINE_SUSPENDED`. Call
`editor->terminal_local(editor, shell, &exit_status)` or
`editor->terminal_handoff(editor, adapter, context)` in the owner loop.
The relay uses the ordinary close policy for any open output session and
reopens it on return. It controls local input modes and forwards PTY output
bytes directly to the terminal. Ctrl-C and Ctrl-D reach the PTY, and resize
events are forwarded during setup and while the relay is running.

This is a byte relay, not a terminal emulator. Do not parse output into a screen,
track its cursor using new escape queries, or save/replay transcript contents.
Softline does not select an alternate screen for handoff; a program in the PTY
may use the terminal normally. Existing editor terminal acquisition remains
responsible for reestablishing its prompt position on return.

For a foreground program the host runs directly, such as an alternate-screen
pager, suspend the editor and call `editor->output_stream_end_for_handoff(editor)`
with an open stream and the chat screen visible. On success, the native close
releases terminal input ownership and clears the displayed queue, status, and
input rows. Prior output, scrollback, and editor state are preserved. If a
partial ANSI or UTF-8 sequence prevents close, finish the sequence and retry
before handing over the terminal. If output is blocked or the native TTY
disconnects, the close returns `SL_ERROR_IO` and releases terminal input
ownership without waiting for drainage. Reopening after a native I/O failure
requires a cursor-position reply before resetting the margins and restoring
cursor visibility. If teardown may have moved the cursor, recovery uses the
saved producer endpoint at the same terminal dimensions. It clears surviving
prompt rows before accepting new output. A missing reply or changed dimensions
that invalidate the saved geometry return `SL_ERROR_IO` with the stream closed.
Destroying an editor with unfinished display cleanup attempts to restore
full-screen scrolling and show the cursor without blocking.
After a successful handoff and return from the program, call
`editor->output_stream_begin(editor)` and resume the same editor.
`sl_output_stream_end_for_handoff()` is the equivalent free function. Use
`output_stream_end` for application shutdown with the configured exit policy.

Only bounded transport/parser chunks and pending partial writes are retained.
Backpressure stops further reads in the affected direction without preventing
local escape handling. Do not accumulate a complete session or spool output.
Cancellation must not wait to flush pending output.
The relay bounds each direction to a 4096-byte chunk. If input exceeds the
pending-input limit, end the session with `SL_ERROR_FULL`; never silently drop
ordinary keystrokes or grow an unbounded input queue. It continues scanning the
current input chunk for an escape before reporting overflow.

The host must keep other producers' writes away from the terminal during
handoff. In the remote queue demo, the frontend detaches at a response
boundary while the backend keeps running. The frontend discards missed
output and resubscribes on return. A host can keep a shared transport alive
for its shell adapter by isolating queue writes from the terminal and
bounding pending output. The bounded chat backlog belongs to the
[read-only transcript viewer](transcripts.md#output-and-scrollback).

The relay restores local terminal modes and releases the session on exit.
The host can resume the preserved editor at the current geometry after normal
exit, setup failure, transport loss, or local escape.
Output-session teardown and reacquisition remain nonblocking, including after
escape. If the terminal cannot accept the required controls, return
`SL_ERROR_IO`, release input modes and discard the unusable UI surface. Retain
the editor continuation; the host may reopen the output session when writable.
Do not wait for terminal drainage. Cancelling a local PTY terminates its current
foreground job group as well as the shell group before closing the master.

## Raw-mode recovery

### Independent launcher

The C example Make targets launch the whole TUI application through
`build/debug/examples/softline_terminal_supervisor -- PROGRAM [ARG ...]`.
This is a host-side launcher, outside libsoftline. It creates one recovery parent
and executes the application in its own foreground process group on the existing
terminal. It creates no client PTY and never reads, writes, parses or buffers the
application's terminal stream. Stdin/stdout/stderr, terminal identity, geometry,
environment, working directory and arguments pass directly to the application.
Kernel terminal input and window-change signals reach its foreground group.
Softline, Lua and transport callbacks remain together in that application.

Before starting the application, the parent saves terminal attributes and file
flags using close-on-exec duplicate descriptors. It restores them and returns
foreground ownership when the application exits, including `_exit()`, an exec
replacement, SIGKILL or a crash in the application. Closing/reusing the
application's descriptors cannot remove the parent's recovery descriptors.
When the application stops, the parent saves its active modes, restores the
original terminal and stops its own shell job. Foreground continuation restores
the active modes and resumes the application. Supervisor HUP/TERM/INT/QUIT requests
are forwarded to the application group, with SIGKILL after a one-second grace
period if it does not exit. Other application control signals are forwarded
without imposing termination. Exit codes pass through; signal termination uses
128 plus the signal number. Launcher/setup/restoration failures return 125,
and exec failures return 126/127.

Already handled and kernel-queued termination requests take priority over a host
stop observed in the same event-loop turn. The recovery parent forwards watched
signals even when ignored or blocked at launch, so the application may install
its own handlers or unblock them later. The application inherits the original
dispositions and mask unchanged; the parent owns a separate forwarding policy.

The launcher requires foreground startup and continuation when stdin and stdout
are terminals. A background continuation is rejected without taking foreground
ownership or changing the current owner's terminal settings. With non-TTY stdin
or stdout it execs directly without supervision;
Softline uses its existing noninteractive path. Web-only and queue-server Make
targets run directly. Other hosts can explicitly use this launcher; merely
linking libsoftline does not fork a process. The launcher is example/host source,
not an installed SDK executable or an implicit runtime inside the library.

Recovery still requires the parent to survive and the terminal to accept
restoration. Killing/stopping the supervisor itself without a catchable signal,
crashing both processes, or losing the terminal defeats this protection.
This recovery concerns terminal attributes, descriptor flags and foreground
ownership; it does not reconstruct terminal screen contents after a crash.

### In-process library guards

Native editing and handoff protect the original terminal attributes and file
flags before changing them. Close-on-exec duplicate descriptors keep restoration
attached to the original terminals even if application descriptors are closed or
reused. Restoration retries EINTR, uses TCSANOW without waiting for output to
drain, and also runs at normal process exit. Setup and callback failures take the
same cleanup path.

While a TUI owns terminal input, temporary signal guards restore the terminal
before default termination or job-control suspension and before invoking an
existing application handler. Returning handlers and foreground job resumption
reacquire the prior modes. Signal information, application masks, restart
flags and alternate signal stacks are preserved. The guard restores
dispositions when ownership ends and honors one-shot handlers where
`sigaction()` reports `SA_RESETHAND`. On macOS, `sigaction()` omits that
flag on readback, so the guard treats an existing one-shot handler as
persistent. [XNU signal implementation](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.121.6/bsd/kern/kern_sig.c)

Register application handlers before acquiring TUI input. Ignored signals
retain their disposition. Replacing a guarded disposition during ownership
opts that signal out of the guard, and the replacement is preserved on release.

TUI acquisition/release and signal policy belong to one process owner thread.
Multithreaded hosts must route these signals to that thread. Independent server
PTY receivers impose no such process-wide TUI ownership. Application callbacks
and signal handlers must not reenter Softline. SIGKILL and SIGSTOP cannot be
caught; fatal crashes, `_exit()`, and an unusable/removed terminal can also
prevent restoration. An OS restoration failure is reported; recovery is never
claimed for a terminal that cannot accept its saved attributes.

## Escape from a hung session

Use OpenSSH's termination sequence: Enter, then `~.`, with no final Enter.
Recognize `~` at the start of input and immediately after CR or LF. At that
position, `~~` forwards one literal tilde; `~` followed by another unsupported
character forwards both bytes. Elsewhere a tilde is ordinary input. See the
[OpenSSH escape-character contract](https://man.openbsd.org/ssh#ESCAPE_CHARACTERS).

The shared relay recognizes this locally, for both adapters, before forwarding
input. Keep at most the pending escape prefix across reads. `~.` consumes those
two bytes, initiates session cancellation and restores the editor without a
peer response, socket timeout, output drain or child-exit wait. This guarantees
escape from an unresponsive session when the local terminal and the adapter's
nonblocking callback contract remain usable. It does not guarantee that a
disconnected remote process has terminated. Other SSH escape commands are not
part of this feature.

## Verification contract

Register compiled unit and real PTY integration tests with bounded timeouts.
Use deterministic adapter fixtures and readiness barriers, not long sleeps.

- Run the shared relay against a real local PTY and a separately hosted PTY
  reached through an adapter. Assert input, output, EOF and resize behavior.
- Preserve multiline UTF-8 drafts, cursor position, undo/yank behavior and held
  edits across normal exit, nonzero exit, open failure, transport loss and escape.
  Command-based handoff must still resume an empty prompt.
- Test `~.` at startup and after both CR and LF, split across reads. Test `~~`,
  unsupported sequences and mid-line tildes without corrupting forwarded bytes.
- Stall opening, reads and writes separately; saturate bounded buffers. Feed
  the escape and assert prompt restoration within one second without releasing
  the stalled peer or waiting for its acknowledgment.
- Resize during setup, active I/O and backpressure, then resize again while
  returning. Assert restored prompt rows and cursor position in the real terminal.
- Assert terminal mode restoration, exactly-once close, child reaping and no
  leaked descriptors on success, partial setup failure and cancellation.
- Exercise local and remote shell selection independently of queue connectivity.
  Prove backend progress during handoff and reconciliation on return, with no
  transcript replay, changed approval holds or duplicate queue submissions.

Run affected native and Lua checks plus sanitizer checks for relay ownership and
partial-I/O paths. Keep platform compile coverage for Linux and Darwin.
