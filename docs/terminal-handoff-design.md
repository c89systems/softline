# Local and remote PTY handoff

Status: implemented by `sl_terminal_handoff()` and the local PTY adapter
`sl_terminal_local()`. Local C/Lua chat uses the local adapter. Remote C chat
uses a backend PTY over the demo transport, independently of frontend rendering.

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

Use the existing suspension and output-session lifecycle. Do not add a shell
method to the public receiver merely to select an adapter. C and Lua hosts must
be able to select local or remote sessions with the same behavior. Callback
operations are declared in `sl_terminal_event_t` and `sl_terminal_operation_t`.
The public receiver layout is unchanged.

## Terminal and output ownership

Suspend editing, end the output session outside editor callbacks, and transfer
exclusive terminal ownership to the relay. The relay controls local terminal
input modes and forwards PTY output bytes directly to the real terminal.
Ordinary controls such as Ctrl-C and Ctrl-D reach the PTY. Forward size changes
while opening and running, including a single shrink or grow.

This is a byte relay, not a terminal emulator. Do not parse output into a screen,
track its cursor using new escape queries, or save/replay transcript contents.
Softline does not select an alternate screen for handoff; a program in the PTY
may use the terminal normally. Existing editor terminal acquisition remains
responsible for reestablishing its prompt position on return.

Only bounded transport/parser chunks and pending partial writes are retained.
Backpressure stops further reads in the affected direction without preventing
local escape handling. Do not accumulate a complete session or spool output.
Cancellation must not wait to flush pending output.
The relay bounds each direction to a 4096-byte chunk. If input exceeds the
pending-input limit, end the session with `SL_ERROR_FULL`; never silently drop
ordinary keystrokes or grow an unbounded input queue. It continues scanning the
current input chunk for an escape before reporting overflow.

Other producers must not write to the terminal during handoff. For the remote
queue demo, retain detachment at a response seam, continued backend execution,
and subscription recovery on return. Missed producer output is discarded, not
buffered or replayed. A host can keep a shared transport alive for its shell
adapter, provided queue output cannot reach the terminal or accumulate unboundedly.

On every exit path, restore local terminal modes, release the session, reread
geometry, begin the existing output session and resume the preserved editor.
Report setup, transport or shell failures without discarding the draft. If the
local terminal itself is unusable, perform cleanup and return an explicit error
rather than claiming successful restoration.
Output-session teardown and reacquisition remain nonblocking, including after
escape. If the terminal cannot accept the required controls, return
`SL_ERROR_IO`, release input modes and discard the unusable UI surface. Retain
the editor continuation; the host may reopen the output session when writable.
Do not wait for terminal drainage. Cancelling a local PTY terminates its current
foreground job group as well as the shell group before closing the master.

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
