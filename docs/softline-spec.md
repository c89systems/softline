# softline Current State And Readline Gap Spec

## Purpose

softline is a small C89/POSIX prompt editor for multiline command entry. Its
current center of gravity is shells, REPLs, chat prompts, and terminal tools
where `Enter` submits the current buffer and `Ctrl-J` inserts a literal newline.

softline is not currently a complete GNU Readline replacement. It is a
handle-owned editor core with enough terminal rendering, history, key binding,
and native chat support to build multiline readline-like interfaces without
global editor state.

## Current Public Shape

The public API is the installed header `include/softline/softline.h`.

- `sl_t` is a receiver-style handle. Constructors return a handle with method
  pointers plus an opaque implementation pointer.
- `sl_create()` creates a handle with default configuration.
- `sl_create_with_config()` accepts `sl_config_t` for input/output fds, prompt
  wrapping width, live scroll regions, queueing, themes, status lines, history length,
  line length, image paste, OSC 8 policy, and the optional queue backend.
- `sl_config_init()` sets the documented defaults:
  - input fd: `STDIN_FILENO`
  - output fd: `STDOUT_FILENO`
  - ordinary readline at the current cursor
  - history max length: 100
  - complete editable draft max length: 131072 UTF-8 bytes (128 KiB)
  - prompt queueing and live scroll regions: disabled
  - prompt queue capacity and previews when enabled: 64 and 3
  - prompt theme: ANSI-only `default`
  - status line and spinner: disabled
  - status busy state and idle marker: false and `+`
  - queue authority: native local queue (`queue_backend = NULL`)
  - OSC 8 hyperlinks: disabled (`SL_OSC8_OFF`)
- `readline()` returns a project-allocated string that must be released with
  `free_string()`.
- `last_readline_status()` classifies the most recent `readline()` outcome as
  submitted text, EOF, cancellation, interrupt, suspension, or error, for both
  `readline()` and `next_prompt()`.
- Free-function wrappers mirror the receiver methods. Receiver methods are the
  primary handle-oriented style; wrappers provide conventional C call sites and
  NULL-argument guarding.

Each handle owns its own:

- editable line buffer
- cursor position
- history list
- terminal raw-mode state
- rendered prompt cache
- prompt queue, theme, and status-line state
- key bindings
- idle callback
- last-error text

softline does not keep global editor state.

## Current Editing Behavior

The editor supports these built-in behaviors:

- `Enter` submits the current buffer.
- `Ctrl-J` inserts a newline.
- `Ctrl-L` rereads PTY geometry, restores owned scroll margins and repaints
  the prompt, status and queue rows, including unchanged cached cells. It
  preserves the draft/cursor, history search, approval focus, queue identities,
  producer position and pending ANSI/UTF-8 fragments. It leaves the transcript
  intact and does not replay output. Bindings can override `SL_KEY_CTRL_L` or
  return `SL_KEY_ACTION_RESET` from another key in C and Lua.
- `Ctrl-D` returns `NULL` on an empty buffer and deletes forward inside text.
- `Ctrl-C` restores terminal state and raises `SIGINT`.
- `Ctrl-A` / Home moves to the start of the current newline-delimited line.
- `Ctrl-E` / End moves to the end of the current newline-delimited line.
  Soft wrapping does not create line boundaries for either command.
- `Ctrl-B` / Left and `Ctrl-F` / Right move by common UTF-8 cluster boundaries.
- Up and Down navigate visual rows first, then history where applicable.
  Consecutive vertical movements retain their preferred screen column across
  short or empty rows. Other navigation, editing and width changes reset it;
  producer output and Ctrl-L preserve it.
  `Ctrl-P` and `Ctrl-N` navigate previous and next history entries directly.
- `Ctrl-R` starts reverse incremental search over the handle's current history.
  `history_search_active()` reports this mode, including a failed match, and
  returns false after selection, cancellation or editor exit. It is a read-only
  query on the editor-owner thread, exposed in both C and Lua. Key callbacks
  run before built-in handling; submission guards should pass search keys
  through so Enter selects an editable match. The chat examples do this for
  shell and exit guards, retaining their restrictions on actual submissions.
  Typing updates the query, repeated `Ctrl-R` cycles older matches and wraps,
  Enter accepts the displayed match into the normal editable prompt without
  submitting or queueing it; a subsequent Enter follows the normal keymap.
  Escape or `Ctrl-G` restores the draft. An explicit Enter binding requesting
  `SL_KEY_ACTION_SUBMIT` can opt into immediate submission during search.
- `Ctrl-V` in the TTY editor saves PNG or JPEG data from the X11 `CLIPBOARD`
  selection to an owner-only cache file and inserts its visible absolute path
  at the cursor. The image file persists after the handle closes. On Linux,
  X11 is accessed directly over Unix or TCP sockets with Xauthority credentials,
  including a `DISPLAY` forwarded by `ssh -Y`. No host X11 library is required.
  Both Linux library forms embed MIT-licensed c-ares 1.34.8 privately. TCP
  display hosts support literal IPv4/IPv6, `/etc/hosts`, then DNS configured by
  `/etc/resolv.conf`, including search domains. Lookup shares the clipboard's
  10-second deadline and uses no libc NSS modules or additional link libraries.
  The same overall deadline bounds X11 setup, replies, selection and INCR events;
  incoming unrelated events do not restart it. INCR capture waits for the
  zero-length terminator and deletes that property to acknowledge completion
  to the clipboard owner. Missing events or a missing terminator fail the paste,
  preserve the draft and remove any partial capture file.
  The default path is `${XDG_CACHE_HOME}/softline/{xid}.png` or `.jpeg`,
  falling back to `$HOME/.cache/softline/` when `XDG_CACHE_HOME` is unset or
  relative. The xid is 20 lowercase base32hex characters in the
  [rs/xid](https://github.com/rs/xid) format, generated internally.
  `image_paste_path_template` in the config or the receiver setter
  `set_image_paste_path_template()` overrides the path. An override must be an
  absolute path template with exactly one `*` for the xid; `~/` and
  `{{HOME}}`, `{{home}}`, `{{XDG_CACHE_HOME}}`, `{{xdg_cache_home}}` expand to
  their directories. The XDG token follows the same fallback rule. The image
  type always appends `.png` or `.jpeg` after substitution, including when the
  template already ends in an extension. For example, `/tmp/softline/*/image`
  creates `/tmp/softline/{xid}/image.png`. Parents are created recursively.
  `disable_image_paste = 1` disables built-in capture; the draft stays intact
  and `last_error()` reports that it is disabled. If the clipboard lacks a supported image,
  the draft stays unchanged and `last_error()` explains the failure. A custom
  binding for `SL_KEY_CTRL_V` overrides the built-in paste. This operation is
  unavailable on other platforms and does not change bracketed text paste.
- `Alt-B` moves to the current or preceding alphanumeric word's beginning;
  `Alt-F` moves to the current or next word's end. Punctuation separates words.
  Unicode 16 letters, marks and numbers participate without splitting clusters.
- `Alt-D` kills through the next alphanumeric word's end; `Alt-Backspace`
  kills back to the preceding word's beginning. These word motions may cross
  actual newlines; Ctrl-W retains the line boundary described below.
- `Ctrl-T` transposes adjacent Unicode clusters. At a newline or buffer end it
  swaps the preceding pair; at buffer start or with no preceding pair it is a no-op.
- Backspace and Delete operate at the cursor.
- `Ctrl-K` deletes from the cursor to the next newline or buffer end, preserving
  later lines. At a newline it removes only that newline, joining the lines;
  at buffer end it does nothing. Soft wrapping does not create line boundaries.
- `Ctrl-U` deletes from the current newline-delimited line's start to the cursor,
  preserving the preceding newline. At line start it removes only that preceding
  newline, joining the lines. At buffer start it does nothing; soft wrapping does
  not create line boundaries.
- `Ctrl-W` deletes the preceding whitespace-delimited word and any trailing spaces
  or tabs within the current actual line. At line start it removes only the
  preceding newline, preserving words on the previous line. At buffer start it
  does nothing.
- Ctrl-U/K/W and Alt-D/Alt-Backspace save text in a ten-entry kill ring owned by
  the handle. Consecutive kills append or prepend in textual order. Ctrl-Y inserts
  the newest kill, and repeated Alt-Y rotates the immediately preceding yank.
  Other editing/navigation ends yank rotation. Focused approval decisions retain
  priority over Alt-Y. A yank exceeding `line_max_len` leaves the draft unchanged.
- `Alt-K` saves the entire multiline draft in one handle-local stash, replacing
  its prior contents, then clears the editor. An empty draft leaves the stash
  unchanged. `Alt-J` appends that saved text at the end of the current draft,
  regardless of cursor position, retaining the saved copy for repeated use.
  With no saved text it does nothing. An append exceeding `line_max_len` leaves
  text, cursor and stash unchanged. `Alt-N` clears the whole ordinary draft
  without writing the stash or kill ring. All three operations support undo;
  Ctrl-U and Ctrl-K retain their actual-line boundaries. The stash survives
  submissions, cancellation and shell handoff until replaced or handle teardown.
  While editing a queued entry, stashing/appending edits only its text, keeping
  the active identity and holds; the stash itself never carries queue metadata.
  Custom bindings and enabled queue key mappings override these defaults,
  including when the mapped queue action is a no-op. Bracketed paste does not
  invoke the shortcuts. Draft shortcuts dismiss approval focus even when empty
  input or an absent stash makes them a no-op.
- `Ctrl-_` and `Ctrl-X Ctrl-U` undo the last editing command. Continuous typing
  and each bracketed paste are single undo groups; callback buffer edits within
  one key command form a group. Undo restores text and cursor, preserving queue
  identities and approval holds. Loading another queued/history entry or starting
  a new readline resets undo; cancelling reverse search preserves it. The kill
  ring persists across these boundaries. Undo contains draft edits only.
- Bracketed paste mode is enabled while editing; pasted carriage returns become
  buffer content instead of submitting the prompt.
- Input is limited by `line_max_len`.
- The editable input viewport occupies at most half the terminal rows (at least
  one row), independently of the complete draft length. Queue/status rows are
  separate; native output retains at least two output rows where geometry allows.
  Only visible input rows are formatted and painted. At the end of a long draft,
  the last rows are visible. Moving within the viewport keeps it stationary;
  crossing its top/bottom edge scrolls just enough to reveal the cursor's row.
  The prompt prefix stays on the first visible input row when it fits the width.
  Resize recomputes wrapping and viewport capacity without discarding draft text.
  This does not retain or replay producer output.
  While reverse search is active, its query occupies a separate, width-limited
  header row above the scrollable result so scrolling cannot hide the query.
- Editing falls back to a plain line reader when either input or output is not
  a TTY; it does not emit prompts.

The command names follow the [GNU Readline reference](https://tiswww.case.edu/php/chet/readline/rluserman.html).
For actual newline boundaries, the supported line-deletion commands follow
[prompt_toolkit's multiline Readline bindings](https://github.com/prompt-toolkit/python-prompt-toolkit/blob/main/src/prompt_toolkit/key_binding/bindings/named_commands.py).
Ctrl-W additionally stops its word scan at the current line's beginning.

UTF-8 is preserved as bytes while rendering uses terminal-cell width for
combining marks, East Asian wide characters, and common emoji sequences.
Cursor and delete operations avoid splitting common UTF-8 clusters. Public
cursor offsets remain byte offsets.

## Current Rendering Behavior

softline renders multiline prompts directly to a terminal fd.

Current rendering guarantees:

- Long input wraps.
- Word runs are kept together where terminal width allows.
- Exact-width rows avoid accidental terminal autowrap prompt duplication.
- Prompts can grow at the bottom of a terminal without overwriting prior output.
- Resize is handled while `readline()` is idle.
- Render capacity is derived from the configured line length rather than a fixed
  row cap.
- Render failures during active editing return as errors, not as submitted
  input.

Ordinary readline participates in terminal scrollback. Persistent chat uses
native full-width output beginning at the original cursor and a prompt
at the terminal bottom. After physical resize, an active editor returns to the
current bottom and restores its output margin even while no output arrives.
A retained frame between editor calls follows native reflow until editing resumes.
Rectangular viewports and bounds configuration are unsupported.
Softline never implies a retained full transcript.

Ordinary readline width changes reconcile the old prompt's physical rows and
cursor before updating its layout. Cursor reports locate the frame when the
terminal supports them; only owned prompt rows are rewritten. The replacement
frame uses normal terminal scrolling when it needs additional space.


### Prompt Queue

This section describes the native local queue. A constructor-selected
[queue backend](queue-backends.md) instead owns accepted work and execution;
its entries never return from `next_prompt()` as local model input. Matching
completion consumes Tab before the queue keymap in either mode.

Interactive ordinary readline and native chat prompts can opt into a prompt queue through
configuration or `set_prompt_queue()`. Non-TTY input remains a plain line
reader, so queueing does not alter it. Tab moves a nonempty active editor into
the queue and clears the editor; Tab on an ordinary empty editor does nothing.
Submitting or requeueing an empty queued edit deletes that entry. Alt-E
removes the most recent queue entry and restores it to the active editor for
editing.

`next_prompt()` is the dispatch boundary. With automatic delivery it removes
the oldest ordinary queued entry before opening the editor and reports
`SL_PROMPT_SOURCE_QUEUED`. With manual delivery the host selects entries by
mode and takes them at safe seams or turn boundaries. An editor submission
reports `SL_PROMPT_SOURCE_DIRECT`; promotion of a queued entry reports
`SL_PROMPT_SOURCE_PROMOTED`. Existing `readline()` callers remain direct-only.

While an editor is active, the renderer owns a queue panel above it. The panel
shows a total count plus a capped oldest-first preview list, and supports all
built-in prompt themes, including the ANSI-only `default` theme. The selected
prompt theme also styles the active editor and optional status line in both
ordinary readline and native chat sessions. The panel is not built with `print_above()` because
queue entries must be removable and must reflow with the active editor.
Application key bindings retain precedence over the Tab and Alt-E defaults.

Each entry has a nonzero opaque ID, allocated monotonically per handle and
never reused, and an approval hold independent of QUEUED/STEER intent. C
`sl_prompt_queue_append()` and `sl_prompt_queue_insert()` take a hold flag and
optional ID output. Lua `queue_append`/`queue_insert` take an optional boolean
hold and return the ID. `sl_prompt_queue_info()` / Lua `queue_info()` expose
`id`, `mode`, `held`, `policy`, `stopped`, and `eligible`. Held entries appear in the approval panel/count;
ordinary Q/S previews remain visible and use their own preview limit.

Automatic delivery skips held entries while preserving FIFO order among
eligible ordinary entries. Held entries cannot be taken or promoted. The host
calls `sl_prompt_queue_approve()` or `sl_prompt_queue_reject()` by ID (Lua:
`queue_approve` / `queue_reject`); approval releases only the hold and respects
busy, manual delivery and steering. Approval decisions are FIFO by arrival,
including entries created with insert/append and an entry being edited.
Out-of-order approve/reject calls fail with `SL_ERROR_INVALID` without mutation.
Rejection removes only the selected entry. Conversation-wide auto-approval
releases all waiting holds as described below.

Alt-E retains the entry's identity, mode and hold while editing and reserves
its queue slot. `sl_prompt_queue_draft_info()` / `queue_draft_info()` expose
that origin. Requeueing or submitting a nonempty held edit retains the same ID, hold and
arrival position for delivery, including when auto-approval releases waiting holds;
editing is never implicit approval. Empty submission or requeue deletes the
edited entry, including approvals, without delivering it. Cancellation, EOF or
read failure restores the original entry, discarding edits. The host may approve or
reject an active edit by ID; rejection clears its draft. Clearing the queue preserves an active
edit; disabling it during an edit fails. Every queued edit shows its Q/S/H origin
and `Alt-N discard`. Alt-N (or Alt-Shift-N) discards that edited entry immediately
without submitting it. Ctrl-_ can recover the discarded text and cursor as a
fresh ordinary draft, never the removed identity, hold or earlier edit journal.
This text-only recovery applies to keyboard discard; host rejection and empty
submission/requeue retain their existing undo-reset behavior. Outside a queued
edit or focused approval, Alt-N clears the ordinary draft. Bracketed paste treats
the same bytes as pasted input rather than a discard command.

Hosts inject messages through `sl_inject_queued_prompt()` or
`sl_inject_steer_prompt()` (Lua: `inject_queued_prompt`, `inject_steer_prompt`).
Both return stable identities and use the same conversation-wide approval
policy. Queueing must be enabled; calls run on the editor owner thread,
including from descriptor-watch callbacks. Queued injections use ordinary
FIFO delivery. Steering injections retain STEER intent for the host to take
at a safe response seam. Approval never changes the selected destination.

`sl_set_auto_approval()` / Lua `set_auto_approval()` sets automatic approval
for the entire conversation's Softline instance. It is off initially.
Enabling it immediately approves **all** waiting entries, including an active
edit and explicit holds created through queue insert/append, and future
injections bypass approval. It preserves text, identity, ordering, busy state,
delivery mode and steering intent, and does not submit an active draft.
Disabling it does not revoke approvals. `sl_get_auto_approval()` / Lua
`get_auto_approval()` reads the setting, which survives queue clear/disable.

Softline renders and handles inline approval controls. The oldest held entry
is offered only when the input buffer is empty and pending terminal input has
been consumed; paste, history search and an active queued edit defer it.
A nonempty draft defers a new question and shows a passive pending count.
Once offered, a question stays visible when typing resumes; the input keeps
its contents, cursor and keyboard focus. Sending or clearing a draft permits
a new question at the next empty input boundary. Editing, requeueing, cancellation
and reopening input cannot move another approval ahead of the first. Submitting
an empty queued edit deletes that entry and releases the editor for the next
approval; it never approves or delivers the deleted entry. Later held messages
remain hidden except in the waiting count until earlier entries are decided or deleted.
The question shows the message and its queued/steering route:
Alt-A explicitly focuses the approval, inverting the foreground/background of
its compact heading. The terminal cursor stays at the draft's editing position;
focus changes key routing and heading colours, not cursor placement.
Alt-Y approves and Alt-N rejects
(case-insensitive), only while focused. Enter never answers an approval;
arrow keys retain ordinary editor/history behavior. Escape or Alt-A returns
focus to the ordinary input without answering. Alt-E edits the full offered
entry with either focus state,
even if newer entries exist. Approval headings use `A ` in the two-column
gutter; normal text and wrapped continuations start with two spaces.
Editing/submitting/requeueing retains
its hold until explicit approval or auto-approval. Ctrl-L preserves approval
focus while recovering layout. Other editing keys return to normal
editing; plain Y/N are ordinary text even when focused. Focused Alt-Y/Alt-N,
focus dismissal and offered-entry Alt-E take precedence over application
bindings; other bindings keep their usual behavior.
Non-TTY handles use the same holds and APIs without interactive
approval controls. Ordinary typed prompts never acquire approval holds.

The approval area uses an `A ` heading combining the count, controls and
Queued/Steering route, followed by at most three indented message rows.
The singular count reads `1 approval`; plural counts read `N awaiting approval`.
Headings and message text wrap at word boundaries to the terminal width;
tokens wider than a row wrap at grapheme boundaries. Message previews count
wrapped rows toward the three-row limit and append `...` to the final row when
text is omitted, keeping whole words where they fit beside the ellipsis.
Only presentation is truncated: editing and delivery retain
the full original text. Page Up/Down keep their ordinary binding behavior.

`sl_last_prompt_id()` / Lua `last_prompt_id()` reports the ID delivered by the
last read/take, including directly submitted queued edits. Ordinary direct
input and unsuccessful reads/takes report zero. Lua `next_prompt()` returns
`line, source, id`, and `queue_take()` returns `text, id`. This receiver/API
extension first shipped with ABI 2. Native admission/execution policies extend
the receiver and entry layout to ABI 3, one increment from released v0.13.0.

`sl_set_prompt_queue_policy()` (Lua `set_queue_policy`) installs an optional,
short owner-thread classifier for deliberate interactive submission. It only
inspects state and returns boolean `enqueue`, `independent` and `executable`
fields. Passive enqueue never calls it. Opted-in submissions enter the visible
native queue even while idle; callback failures and full queues preserve the
draft. Pending keys, paste and history selection precede automatic delivery, so
same-burst Alt-E can edit the admitted entry before it runs. Editing preserves
its identity, reserved position, approval and stop state; history appends only
once at admission. Non-TTY input remains unchanged.

`sl_prompt_queue_set_executable()` (Lua `queue_set_executable`) updates execution
readiness by identity, including an active edit. This is independent of approval
and cancellation. Managed entries use explicit execution readiness rather than
the busy signal, including steering; manual mode still prevents automatic
delivery. Updates from watch callbacks schedule delivery through the native
reactor. Effective `eligible` metadata and `prompt_queue_take()` share the native
ordering and hold checks. Ordinary turns retain FIFO ordering. Steering keeps
its seam priority over ordinary turns, but cannot pass an earlier managed
command or waive its own execution hold. Legacy approval suggestions do not
hold ordinary user input;
independent feed-local queries may pass blocked work without modifying it or
changing FIFO approval decisions. No command names are built into Softline.

Cancellation marks retained entries as stopped. Fresh managed submissions can
execute without releasing that backlog, while passive enqueues and edits retain
the stop. Explicit Alt-Enter promotion resumes only that entry, with draft and
empty-editor promotion both retaining execution holds. `sl_get_prompt_queue_stopped()`
(Lua `queue_stopped`) inspects the backlog stop; `sl_prompt_queue_resume()`
(Lua `queue_resume`) explicitly resumes it while preserving manual mode, active
edits, approval and execution holds. An automatic delivery setter never clears
the stop. Ordinary profiles retain their existing direct-submission behavior.

### Prompt Themes And Status Lines

The renderer owns prompt presentation so the marker, typed text, queue panel,
and optional status line always share the active layout. The built-in `default`
theme uses only standard 16-colour ANSI sequences: a bold bright-white marker,
normal terminal-colour input, blue queue and approval text, dark-gray separators,
standard-colour status elements, and red/green busy markers. Queue/approval text
uses each theme's first status-element colour role; focused approvals invert
their heading foreground/background. `plain` is
uncoloured for the editor and queue, while quoted prompts and status messages
use neutral ANSI styling. The remaining named themes use their embedded
palettes.

Status lines are opt-in rows between queue previews and the editor. A theme has
eight element colours. `statusline_start_element` selects the first palette
slot and later elements advance modulo eight. Softline retains at most 32
elements; a longer bulk update uses the first 31 followed by `...`. Element
text must be valid UTF-8 without C0/C1 controls or DEL. Idle shows
a green `+` by default; callers can supply another printable ASCII marker or
`'\0'` for a blank reserved slot. Busy shows a red `x`, or a red 500ms
`/-\\|` spinner when both busy and spinner are enabled.

An optional status message sits immediately above the status line. The default
`! ` prefix uses the theme's muted colour and the italic message uses its
secondary colour. The host may update or clear it during an active output
session. Long text wraps at words and continuation rows are indented to the
display width of the configurable prefix. The C and Lua APIs expose separate
theme colour choices for the prefix and message text.

## Current Extension Points

Persistent output is renderer-agnostic. The C and Lua APIs expose begin,
write, and end, and the C chat example composes libmdf externally. See
[the composition contract](softline-mdf-stream-design.md). Softline does not
link libmdf or own its document lifecycle.

The C and Lua chat examples support exact `!sh` and `/shell` submissions
without arguments on interactive terminals, only between operations. Enter or
idle Alt-Enter launches `$SHELL -i`, falling back to `/bin/sh -i` when unset or
empty. Busy submissions remain editable in the queue until the whole turn ends;
steering intent cannot waive the command's execution hold. Argument-bearing text remains an
ordinary chat turn. The application closes its idle output session, runs the
shell through the local PTY adapter with full scrolling, then begins a fresh
session at its final cursor and current geometry. Clear and resize inside the
shell are allowed. History, queue and configuration survive on the same handle;
transcript replay is unnecessary. Failed launches and nonzero exits report an
error and return to chat. This uses existing output-session APIs and adds no
receiver shell method or receiver ABI change.

Alt-S uses the same between-operation shell handoff without submitting the
draft. A key callback returns `SL_KEY_ACTION_SUSPEND`; the editor returns NULL
(nil in Lua) with `SL_READLINE_SUSPENDED`, retaining text, cursor, preferred
column, undo, kill ring, reverse search, history navigation and approval/edit
state. The next readline/next_prompt resumes that continuation before automatic
queue delivery. Held edits retain their identity, position and explicit approval
requirement. Suspending does not append history, approve, requeue or cancel.
It also preserves completion snapshots and the active Tab cycle without stripping
an optional trigger; resumed completion does not call the provider again.
The application runs the child outside callbacks, after ending its output
session, then begins a fresh session and resumes editing. Shell clear/resize
changes the physical layout only: owned UI wraps at the current geometry, with
no producer transcript storage or replay. Exact `!sh` and `/shell` submissions
still return to an empty prompt. Suspension does not pause background producers;
the caller must arrange exclusive terminal ownership. The local examples reject
Alt-S while an operation is running. Remote chat instead requests handoff at a
response seam, detaches output delivery while the shell runs and resubscribes
to future output on return. The backend continues executing without transcript
buffering or replay. An owner-thread idle/watch callback can request the same
suspension with `sl_suspend()` (Lua `sl:suspend()`); the child still runs after
readline returns. This method is part of the unreleased ABI 4 layout.
The generic action and status extend existing enums; the `sl_suspend()` receiver
method is included in ABI 4. The terminal adapter free functions add no shell
controls to the receiver.

The shared local/remote PTY adapter and locally handled escape from a hung
session are specified in [the terminal handoff design](terminal-handoff-design.md).
`sl_terminal_handoff()` runs any host-supplied nonblocking adapter;
`sl_terminal_local()` supplies the local PTY adapter. Enter followed by `~.`
returns to the preserved editor even when the shell or transport is hung.
The shell process owns this adapter's lifetime. After reaping it, the adapter
drains currently available output up to 64 KiB, stopping at the first empty read
or the byte limit. A background job retaining or continuously writing the slave
cannot prolong that drain indefinitely. The remote demo uses the same adapter.

Host buffer replacements and insertions during suspension are journaled like
active edits, including while the output session is closed. No-op or rejected
mutations do not add undo entries. Undo validates a complete command group
against the current draft length and allocation before changing bytes. Context
changes such as accepting history search or rejecting an edited approval retire
the old journal. Producer redraws preserve the preferred editing column unless
the effective wrapping width changes. Unsupported Ctrl-X chords consume their
complete UTF-8 character without leaking continuation bytes into the draft.

Public cursor navigation ends typing coalescing in idle/watch callbacks as well
as key bindings. A suspend callback that changes the draft or cursor ends its
command normally: its edits form one undo group, separate from typing after
resumption. Only a suspension without editor changes preserves the preceding
command identity, including yank rotation.

Vertical navigation clamps the preferred column to a cursor position owned by
the requested visual row. A soft-wrap endpoint belongs to the following row;
hard-newline positions remain on their preceding row. Rows containing only
expanded tab continuation cells are skipped because no draft cursor position
exists there. Navigation can reach the EOF position after an exactly full final
row; its continuation row is painted only while the cursor occupies it.

The C and Lua APIs also expose a quoted-prompt writer for an open output
session. It wraps literal submitted text, repeats a configurable prefix on
each visible row, and adds only the line breaks needed for one empty row on
either side. The quote prefix and italic text have independent colours.

### Key Bindings

`bind_key()` attaches a per-handle callback to a key. The callback may:

- handle the key completely
- pass through to built-in behavior
- submit
- cancel
- interrupt
- reset prompt layout from PTY geometry while preserving editor/producer state
- mutate the active buffer with public methods such as `insert()`,
  `set_buffer()`, `set_cursor()`, `submit()`, and `cancel()`

Current key coverage includes common control keys, arrows, Home/End, Delete,
Paste Begin/End, F1-F10, distinguishable Ctrl-Enter sequences, and Alt-letter
keys represented through `SL_KEY_ALT_BASE`.

### Idle Callback

`set_idle_callback()` registers a callback invoked while `readline()` is active
and no input byte is available. It can update the buffer, print above the
prompt, submit, or cancel through public methods.

This remains a local owner-thread hook. Use external watches for immediate
application wakeups while an editor is active.

### External Watches

`watch_add()` registers an application-owned descriptor on an interactive
handle. During `readline()` and `next_prompt()`, Softline waits for terminal
input and registered watch readiness together, then invokes the callback on the
same owner thread. The callback drains bounded application work and may print
above the prompt, mutate queue state, and update status without racing the
active editor. Softline does not read, close, duplicate, or change flags on a
watched descriptor. Non-TTY handles reject watches. To preserve interactive
fairness, ready watches are visited round-robin and Softline dispatches at most
eight callbacks before it gives terminal input another chance to run.

### Streaming Output Above Prompt

Native chat sizes the prompt from its current rendered frame. Empty queue
slots and absent status messages reserve no rows. Adding or removing queue,
status, or editor rows adjusts the output margin immediately. Growth scrolls
existing output cells only by the rows needed to fit; shrink clears former
prompt cells and returns their rows to output without moving transcript cells.
The editable input pages at half the terminal height, or earlier when needed
to leave two output rows and the queue/status panel. Queue and status rows
stay visible whenever they fit.

`print_above()` accepts a chunk callback and writes all chunks above the active
prompt. The callback returns `SL_OK` with a non-empty chunk to continue, or
`SL_OK` with length `0` to finish.

`output_stream_begin()` opens a persistent session; every later
`output_stream_write()` forwards its byte span immediately, without waiting for
EOF or the next prompt. Full-terminal transcripts start at the existing cursor;
the editable prompt starts at the bottom. Producer
bytes pass through unchanged in a VT scroll region above the prompt. Native
transcript text is never cached, padded, rewrapped, cleared, or replayed.
Prompt updates compare the previous frame and patch only changed cells; output
feeds preserve unchanged prompt cells and restore the editor cursor. The
scroll region ends immediately above the current prompt frame.
Resize anchors the active editor at the current terminal bottom, updating only
changed prompt cells or placement and internal output geometry. The output
margin is restored during reconciliation, before output resumes. The terminal
owns transcript reflow; retained inactive frames follow that native position.
Ending chat restores the full scroll region and clears input rows while
retaining queue and status rows. The cursor stays at column zero on the current
input row; exit adds no newline or scroll. `clear_prompt_on_exit` defaults to zero;
setting it to one clears the whole prompt area and returns below the transcript.
Ending a producer session inside an active editor retains the native prompt and
scroll region for subsequent finite output or another stream; closing the handle
always restores terminal state. Non-TTY sessions emit no teardown controls.
Closing an idle native output session also releases terminal input ownership.
Applications may run a foreground child before beginning another session to
probe its final cursor and build a fresh prompt, retaining history and settings.
This handoff belongs between editor calls, outside active editor callbacks;
the application owns child execution. Ending the stream does not finish an
external renderer document. Incomplete ANSI/UTF-8 returns `SL_ERROR_INVALID`
(Lua: `nil, softline.ERROR_INVALID`) and leaves the session open for the missing
bytes.
Reopening a closed native session probes the cursor again. A nonzero column
restarts quote separation for an unfinished externally written line; otherwise
known spacing remains. An editor-retained session keeps its continuation state.
Native feeds batch bounded chunks with cursor restoration and retry short
writes as needed, without cursor visibility toggles or repainting unchanged
frames. Feed calls add no intentional delay and never request or wait for a
terminal cursor reply, including on resize. Local scalar widths shared with
prompt layout track advances. Terminal Unicode versions and cluster shaping
can differ from the Unicode 16.0 accounting;
producer bytes and wrapping remain unchanged. No producer text or grapheme
lookahead is retained.
An owner-thread watch callback can feed an external renderer and forward each
sink emission directly while editing and queueing continue. Softline stores
cursor geometry and bounded partial ANSI/UTF-8 state. Plain ASCII lines use a
scalar cell count; Unicode uses conventional scalar widths without glyph span
tables. Cursor reports outside feed calls can record ownership boundaries.
Width resize retains the tracked endpoint for an opaque
line, clamping an offscreen column. Exact continuation after Unicode/tab reflow,
including CR-overwritten tails, is deliberately outside the contract. It retains
no producer text for resize. Carriage return preserves the visible tail of an
ASCII line; its cursor position and extent are tracked separately.
SGR and UTF-8 sequences can cross writes; ending with an incomplete sequence
fails and keeps the session open.
With an active prompt, a printable continuation after a known ASCII write ending
at the terminal's right edge uses CR and IND after the cursor handoff. The
resulting hard row boundary can remain split after width growth, and a Unicode
cluster divided there may lose its attachment. The accepted scope and rationale
are recorded in `softline-mdf-stream-design.md` under the active-prompt
right-edge review exception.
At the bottom output margin, width growth can reflow a row boundary differently
in VTE and tmux; exact continuation at that seam is outside the native contract
without transcript replay. Rapid tmux resize commands can also update its
visible grid before the matching PTY size reaches Softline, so output emitted
in that interval may be corrupted. The paced resize tests do not cover this
queued-notification diagnostic.

For finite `print_above()` output on an ordinary readline prompt, rendering uses
normal clear-and-redraw scrollback by default. Set `live_scroll_region = 1` in
`sl_config_t` or call `sl_set_live_scroll_region()` to opt into a
cursor-position probe at startup and between producer callbacks as output
advances the prompt to the terminal bottom. Multiple feeds in one callback
stay immediate; ownership is refreshed at the next input-owned boundary. When
supported, softline temporarily scrolls the full-width region above the
retained prompt, avoiding a prompt
repaint while keeping queue, status, wrapping, and resize reflow aligned to the
bottom. The terminal scroll region is reset on every completion, cancellation,
error, and handle teardown. If the terminal does not answer the probe, output
uses the compatible clear-and-redraw path.

After a TTY live session, finite `print_above()` output continues in native
terminal scrollback. Producer output accepts printable UTF-8, ANSI SGR and opt-in OSC 8
bytes without cell reserialization.

## Current History Behavior

History is per handle.

Supported behavior:

- add entries manually or opt into recording accepted editor/queue drafts
- cap history length
- navigate history during editing
- preserve an edited draft while moving through history
- reverse incremental search over manually added or explicitly loaded entries
- save and load history snapshots, bypassing append on imports
- attach synchronous custom load/append hooks in C and Lua
- load/append native keyed XDG state files with shared-process locking
- recover incomplete final records and explicitly compact under the same lock
- encode multiline history entries
- reject oversized history records without splitting them
- force saved history files to owner-only permissions

softline does not currently provide a global history registry, timestamped
history, configurable duplicate suppression, forward search, history expansion, or
shell-style history metadata.

## Current Error And Cleanup Behavior

- Most fallible methods return `SL_OK` or a negative `sl_status_t`.
- `readline()` returns `NULL` for EOF, cancel, interrupt, or failure.
- `last_readline_status()` distinguishes those `NULL` outcomes and also records
  successful submitted text.
- `last_error()` exposes the most recent diagnostic string.
- `destroy(NULL)` is safe.
- Public string results are released through `free_string()`.
- Terminal raw mode and bracketed paste are restored on normal return, cancel,
  interrupt, and tested error paths.
- While Softline owns raw mode, kernel tab expansion (`TAB3`/`OXTABS`, such as
  `stty -tabs`) is disabled so the terminal handles producer tabs. OPOST/ONLCR
  and other output flags are preserved. Original flags are restored on release,
  including when input and output are separate TTYs. Softline does not expand
  tabs into producer bytes.
  Native writes after raw-mode release borrow the same output flags for each
  write and restore them before returning, without flushing pending input.

The return value remains optimized for the common case: a non-`NULL` result is
submitted text, while `NULL` means no submitted text. Callers that need precise
classification should read `last_readline_status()` before the next
`readline()` call.

## Current Packaging And Portability

softline currently targets C89 plus POSIX terminal APIs.

Current build surfaces include:

- CMake presets
- static and shared library builds
- shared-library ABI/SOVERSION policy through the source-owned
  `SOFTLINE_ABI_VERSION`, currently `4`, decoupled from the project release
  version and not a CMake cache option. Constructor-selected backends extend
  the public configuration and receiver layouts, advancing exactly once from
  released v0.15.1's ABI 3. Consumers must rebuild.
- installed CMake package exports with canonical target `softline::softline`
- installed pkg-config metadata
- Lua 5.5 facade packaged as LuaRocks source artifacts
- example binaries
- Lua examples matching the simple and chat workflows
- CTest unit/example coverage
- Lua facade smoke coverage
- sanitizer test target
- package verification scripts
- external installed-package consumer smoke test

The library is not currently a Windows console implementation, a terminfo
database consumer, or a curses-style screen abstraction.

## Current Hyperlink And Completion Behavior

### OSC 8 hyperlinks

Validated producer output (`output_stream_write()` and native finite output)
rejects OSC controls by default. `sl_osc8_mode_t` selects `SL_OSC8_OFF`,
`SL_OSC8_ON` or `SL_OSC8_AUTO` through `sl_config_t.osc8_mode` or
`sl_set_osc8_mode()` / `set_osc8_mode()`. Set policy between output streams;
an open stream, partial control or retained producer link returns
`SL_ERROR_NOT_READY`. Invalid modes return `SL_ERROR_INVALID` without changes.

`sl_get_osc8_info()` / `get_osc8_info()` returns cached `sl_osc8_info_t` fields:
`mode`, `detected` and `enabled`. Detection recognizes known output-terminal
environments conservatively; it is a hint, not proof of support. `enabled` is
the effective validated-feed policy and is the value to give a renderer such
as libmdf's `options.osc8`. OFF always disables acceptance, ON explicitly
overrides detection, and AUTO requires a recognized TTY on `output_fd`.
Unknown terminals, redirection, `TERM=dumb`, and unverified tmux/screen paths
remain disabled under AUTO. Detection runs at construction or a policy setter,
using environment variables and `isatty()` only. The getter is cached; no
terminal query, input read, timeout or feed round trip is added.

Enabled feeds accept only `OSC 8 ; params ; URI ST`, with BEL or ESC-backslash
as the terminator, and an empty URI to close. Other OSC commands remain
rejected. Parameters are printable ASCII, and URIs are printable valid UTF-8;
embedded controls, C1 characters, malformed UTF-8 and oversized controls are
rejected before that control reaches the terminal. `SL_OSC8_MAX_BYTES` is 8192,
including opener and terminator. Controls and UTF-8 may span arbitrary writes;
only a bounded incomplete control or character is held. Labels stream directly
and are never retained for transcript replay. Ending on an incomplete control
returns an error, allowing the caller to finish it or destroy the handle.

Native writes retain only active hyperlink metadata, close the hardware link
before returning to editor UI, and reopen it for the next producer fragment.
The URI and `id` parameters survive fragment boundaries. Queue, approval,
reasoning, status, input and ghost text are outside producer links. Quoted user
prompts close the producer link as a new output record. Redirected feeds retain
raw producer controls and close any unfinished link when the stream ends or
the handle is destroyed, including after an incomplete-control error.
Ordinary finite `print_above()` without a native surface retains its existing
raw host-output behavior; this policy is for validated feeds.

Local and remote C chat use AUTO and pass `enabled` to their test-only libmdf
renderer. `make run-chat OSC8=on` overrides detection; `OSC8=off` disables links.
No libmdf dependency is introduced into the library.

### Completion providers

`set_completion_providers()` / `sl_set_completion_providers()` atomically copies
an array of `sl_completion_provider_t` registrations. Each has one printable
non-whitespace ASCII `trigger`, a `scope`, `strip_trigger`, a synchronous
`sl_completion_callback_t`, optional `sl_completion_expand_callback_t` in
`expand`, and borrowed host context. Duplicate triggers,
invalid fields and copy failure preserve the previous registrations.
NULL/count zero clears them. `SL_COMPLETION_START` (zero/default) anchors only
at byte zero of the draft. `SL_COMPLETION_WORD` also allows a trigger after
ASCII whitespace (space, Tab, LF, CR, vertical Tab and form feed).

The input owner calls the provider once when its trigger activates a token.
The C callback receives the editor, trigger, `sl_completion_emit_t`, emitter
context and host context. It emits full strings including the trigger, in
cycling order; each emission is copied immediately, so temporary host strings
are safe. Strings are printable valid UTF-8 with LF/Tab allowed and bounded by
`line_max_len`; duplicates merge, and malformed/control-bearing candidates
fail. A failed callback or emission discards the entire activation's list,
leaves typed text including its trigger intact, and is not retried until a new
activation. Empty successful lists are cached too. Providers may read editor
state but must not mutate/destroy it, retain the emitter or yield. No provider
is invoked by rendering or a producer feed. Asynchronous resolution is not part
of this interface; hosts provide a synchronous snapshot of their current data.

Optional `expand(self, completed, emit, context, userdata)` runs on Right/Ctrl-F
commit and on Tab outside an active cycle, when actual text contains a completed candidate.
`completed` includes the trigger. It also works for a unique, fully present match, and for
known ancestor candidates when the user has typed part of a child path. Emit
full strings strictly extending that prefix. A nonempty result becomes a
separate completion level. Commit previews its first matching candidate;
Tab accepts it. A Tab-triggered lookup accepts the candidate on that same Tab.
An empty result retains normal completion or host Tab handling. No continuation
is called for ghost text alone. An existing matching suggestion is accepted
before trying a shorter ancestor continuation, including after each lazy lookup.

Each prefix is resolved at most once per activation, including empty and failed
attempts. Softline keeps parent and child snapshots separate, choosing the most
specific cached list with a candidate matching the draft. Backspace below its prefix
returns to the original parent list, rather than mixing parent candidates with
descendants. Continuing inside a cached level does not call the provider again.
Repeated Tabs stay within the current list, including a one-candidate list
with a continuation provider. Right or Ctrl-F commits the selected candidate
without moving the cursor, resolving that candidate's next list for an immediate
ghost preview. It never inserts that next candidate or recursively resolves
its descendants. An empty or failed continuation suppresses competing parent
ghosts. A nonmutating suspension preserves the preview and cached results.
After committing a leaf with no continuation preview, Right/Ctrl-F resumes
ordinary forward character movement.
These keys also accept a ghost at its token endpoint. Within existing text
they retain normal character movement; explicit host bindings take precedence.
Typing further into that directory, or Tab without a parent cycle,
reuses its cached children. This preserves cycling through the first list after
Backspace while allowing repeated navigation into previously visited levels.
Continuation failure discards only its new list, rings once and preserves the
draft and prior candidates; editing can rematch the parent list. The initial
callback still runs only once per token. All snapshots end with that activation.

Softline owns that snapshot while the token is active, including through typing,
Backspace, Undo, ambiguous matches and Tab cycling. Whitespace inserted by
ordinary editing (including yank or host insertion) continues the activation
when it matches a candidate prefix with further characters to complete. For
example, typing `hello ` keeps `world` visible for `hello world`; Tab accepts
the phrase and further Tabs cycle matching phrases. Whitespace without a
remaining matching suffix ends the activation. Bracketed paste,
deleting the trigger, moving outside the token, submission,
queueing, replacing the draft or replacing registrations ends the activation.
Accepted candidates and restored undo text may themselves contain spaces;
those spaces do not end the activation. Other editing/navigation ends the Tab
cycle and rematches the current prefix against the cached snapshot, without
calling the provider.

If a phrase stops matching and the current word starts with another eligible
registered trigger, that word starts a fresh activation. For example,
`hello @` leaves the `hello world` phrase and starts the host's `@` provider.
A still-matching phrase keeps ownership of embedded trigger characters, and
start-only providers remain ineligible after whitespace.

Matching is literal and case-sensitive, starting with the trigger itself.
The first match appears as a dim inline
suffix. Tab accepts that full candidate; further Tabs cycle the originally
resolved matching set in list order, wrapping to the first. Thus
`/` can cycle all slash commands, while Backspace from `/command1` to `/command`
restricts the next cycle to that prefix. No whitespace is appended. With no
active cycle, a unique fully present completion or no match lets normal host
bindings and queue mappings receive Tab. Matching completion precedes both.
Reverse search and bracketed paste retain their existing key/literal semantics.
With providers configured, Tab during acquisition of a backend edit hold
waits for that outcome and earlier deferred typing. It then completes the actual
draft or falls through to the host, including when the hold was rejected. Other
host control bindings remain responsive while the hold is pending.

Completion inside text replaces the remaining current whitespace-delimited
word while preserving subsequent unrelated text. A complete already-present
phrase suffix is reused. An entire Tab cycle is one undoable edit, separate from adjacent
typing, and retains queue/approval identity, holds and order. An expansion over
the draft limit leaves the draft/cursor unchanged and consumes Tab with a bell.
If reused phrase bytes join an existing combining-mark or emoji continuation,
the cursor advances past the resulting whole cluster; those existing bytes are
preserved. Subsequent movement, deletion and undo keep their cluster semantics.

Ghost text is a display-only projection inside the multiline editor, wrapping
and paging with it. It does not change the draft, actual cursor, undo journal,
queue/approval entries or host status/reasoning. Enter submits only actual text;
Tab is required to accept a suggestion. Existing matching text after the cursor
is reused in the preview; unrelated text remains visible. Navigation operates
on the actual draft. There is no separate completion panel. Ghosts follow
cursor moves, host list changes, undo, suspension and resize. No producer
transcript is retained.

`strip_trigger` is off by default. When enabled, normal completion exit or
submission removes the one introductory character, including from queued or
held edited entries, without changing their identities, holds or FIFO order.
Enter submits actual text and never accepts a ghost. For example, `@REA` + Tab
can accept `@README.md`; Space or Enter removes `@`, leaving `README.md`.
Trigger removal is undoable. Whole-draft replacement/discard uses its existing
editing-context rules rather than adding a second deletion. Failed providers
never strip the trigger. Registration replacement ends any active completion.

C/Lua simple multiline and chat examples use callbacks for `/`, `h`, `v`, `n`
and `@`. Slash callbacks provide the host's commands. `@` takes a fresh current
directory snapshot on activation, accepts at word boundaries and removes its
marker on exit/submission. It uses the same frontend in
local/remote chat; the example host owns file discovery, not the Softline library.
Directories end in `/`; commit one with Right/Ctrl-F to preview its first child
through the same resolver, without scanning descendants. Tab accepts/cycles
that directory's children.
The command resolver similarly
extends `/approve` to `/approve on` and `/approve off`. Hidden entries are
included, `.git` entries excluded, and symlinks listed without following them.

## Gap List Toward A More Complete Multiline Readline Alternative

This section is a product and engineering backlog, not a promise that every item
belongs in softline. Each item should get tests before being considered part of
the public contract. OSC 8 and synchronous trigger completion are implemented
as described above.

### Hints And Autosuggestions

Missing:

- right-side hints
- validation messages
- async suggestion updates
- style policy for hints in narrow terminals

Important constraint: hints must never become submitted buffer content unless the
user accepts them explicitly.

### Searchable History

Supported:

- reverse incremental search
- search prompt rendering inside multiline and native chat sessions
- accept/cancel search state

Missing:

- forward search
- history filtering hooks
- duplicate suppression options

### Editing Model

Missing or incomplete compared with mature readline-style editors:

- kill ring
- yank / yank-pop
- undo / redo
- transpose character / word
- case operations
- region/mark operations
- configurable word-boundary policy
- full Emacs-style command set
- vi insert/normal mode
- keymap layers

softline currently has a small fixed editing model plus per-key callbacks.

### Unicode And Width Semantics

Current behavior is UTF-8-preserving and terminal-width-aware for common
interactive prompt input. It is not a complete Unicode Text Segmentation or
locale policy implementation.

Missing:

- full Unicode grapheme-cluster segmentation
- locale-specific ambiguous-width policy
- Unicode normalization policy
- invalid UTF-8 policy
- locale-aware width tests
- callback APIs that expose both byte offsets and character-cell positions

This is one of the larger gaps for a production-grade multiline readline
alternative.

### Terminal Capability Handling

Current behavior emits common ANSI/VT-style escape sequences.

Missing:

- terminfo/termcap capability lookup
- general terminal feature detection beyond the cached OSC 8 policy
- fallback strategies for limited terminals
- configurable bracketed paste support
- richer key sequence decoding
- mouse/pointer policy, if ever needed
- robust behavior under terminal multiplexers with unusual sequences

### Event Loop And Async Integration

Interactive reads are an owner-thread reactor over terminal input and watched
application descriptors. Workers signal an application-created wake FD; they
must never call Softline directly. Future work may add a separately driven
session API or platform-specific backends, but it must preserve this
single-owner rendering rule.

### Output And Transcript Management

`print_above()` and a persistent output session handle chunked output above the
active prompt. Softline retains cursor geometry and bounded parser state, with
scalar cell counts shared with prompt layout. Producer output has no glyph-span
or cluster reconstruction model. A bounded counter of viewport shifts helps
compute cursor deltas when growth restores only part of the terminal history;
no transcript rows, cells or strings are kept by Softline.
The terminal owns transcript cells and scrollback. Physical resizing leaves
surviving output cells as the terminal reflowed them; changing the output
margin does not scroll them again. Output clipped off the top is not recovered
or replayed by Softline. If an unfinished line moves into scrollback, its next
fragment starts at the first visible output row instead of adding empty rows
down to the prompt. If the clipped position was at a hard line boundary, new
output starts next to the prompt. Native scrollback cells and spacing remain
unchanged.
Positions are tracked relative to the terminal bottom,
without terminal cursor save/restore. Resize reconciliation queries the live
input cursor to account for terminal movement and adjusts only owned prompt
rows whose cells or placement changed. Fitted, unchanged rows are preserved.
Width changes rebuild prompt layout. The active editable prompt stays at the
terminal bottom even when unchanged text fits the larger terminal.

A physical resize can race an in-flight VT batch and misplace even ASCII
output. This accepted concurrency limit is documented with its reproducer in
the stream design's concurrent physical resize exception. Regression gates
test stable resize boundaries; the unpaused diagnostic remains available.
An unobserved resize of a retained frame also has a deliberate immediate-feed
fallback limit: earlier hard-ended ASCII reflow and unknown terminal history
cannot be inferred from the current-line delta. This includes writes after
readline returns. The stream design documents its reproducer and policy source;
the active input loop observes resize before dispatching producer watches.

Missing:

- scrollback buffer abstraction for alternate-screen applications
- offscreen transcript replay or scrollback
- caller-driven reflow of already emitted content after resize
- arbitrary terminal controls beyond documented SGR and opt-in OSC 8

These transcript-management features are outside the current native chat
contract. There is one full-width chat layout, and no boxed viewport API.

### Styling

Missing:

- arbitrary prompt-span styling API
- syntax highlighting callback
- completion/hint style policy
- active selection style
- terminal color capability handling
- style reset guarantees around callbacks

Any styling API must preserve byte ownership and terminal reset safety.

### Persistence And History Policy

History persistence is opt-in. `history_set_backend()` copies a nonempty key,
loads oldest-first through a scoped emitter, and attaches the append hook only
on success. Loading is staged: an invalid import or failed hook preserves the
prior backend and retained entries. Imports, including `history_load()`, bypass
append. Append hooks run synchronously before a newly accepted history entry is
committed to recall; failure leaves recall unchanged. Custom backend rollback
and userdata lifetime belong to the application. History callbacks cannot
reenter history operations or destroy the handle. C receiver/free APIs and Lua
methods expose the same behavior; Lua emitters reject use outside their load.

`history_open(key, directory)` attaches the native backend. Exact key bytes map
to a SHA-256 filename under `$XDG_STATE_HOME/softline/history`, falling back to
`$HOME/.local/state` for absent, empty or relative XDG values. An absolute override
selects the complete directory. Missing directories are recursively created
0700; the store directory is private. Files and stable per-key lock files are
0600, regular, user-owned and not symlinks/hard links. No additional link library
or terminal I/O is involved.

The format has one physical LF-terminated record per prompt. Backslash, LF,
CR and Tab escape as `\\`, `\n`, `\r`, `\t`; other UTF-8 bytes are literal.
Native load ignores an incomplete final record, and native append truncates it
under the key lock before writing the next complete record. Records exceeding
line_max_len or containing NUL are rejected. Legacy snapshot loading still
accepts an unterminated final record and CRLF line endings. Snapshot saving is
an explicit overwrite and does not participate in native shared-file locking.

Each append completes immediately; no save is deferred to exit and no per-entry
fsync is promised. Same-key processes serialize load, append and explicit
compaction through a separate stable flock lock file, including independent handles in
one process. Compaction rereads the shared
file with bounded retained entries and atomically renames a private replacement,
so a stale handle cannot discard unseen committed prompts. No automatic
compaction or metadata/version marker is added. Recall is per handle, bounded
by its cap and updated by explicit loading; a zero cap disables recording.
Empty and consecutive duplicate entries are suppressed.

`history_set_auto_add()` is disabled by default. When enabled, accepted direct
editor submissions and queue/steer drafts enter history immediately. Queue
release/promotion, cancelled/rejected drafts and Ctrl-R selection do not append.
Programmatic queue insertion/replacement stays application-controlled. Both chat
examples enable auto-add and attach the shared `softline.examples.chat` key at
startup; their `SOFTLINE_HISTORY_DIR` override keeps tests repository-local.
`history_close()` retains recall and recording policy, detaching without saves.

Timestamped metadata, configurable duplicate policy and power-loss durability
are outside this simple prompt store.

### Diagnostics

Missing:

- structured error object
- callback failure propagation detail
- optional diagnostic callback

This is important if softline becomes a lower-level component for larger
interactive applications.

### Portability

Missing:

- Windows console backend
- non-POSIX fallback strategy
- broader OS matrix tests
- terminal emulator compatibility matrix
- package smoke tests on more targets

### Security And Robustness

Areas that need continued hardening:

- malformed escape sequence handling
- very large configured line limits
- hostile terminal dimensions
- repeated resize storms
- callback reentrancy policy
- signal interaction beyond Ctrl-C
- explicit history snapshot overwrite semantics outside native store locking

### Compatibility With Readline Expectations

softline does not currently aim for Readline ABI or API compatibility.

Missing if that goal changes:

- Readline-compatible headers or adapter layer
- Readline-style global variables
- `.inputrc` parsing
- named commands
- macro recording/playback
- completion API compatibility
- history API compatibility
- redisplay hooks

The current architecture is handle-oriented and intentionally not Readline's
global-state model. A compatibility layer should be considered a separate
facade, not a reason to weaken the core handle model.

## Suggested Near-Term Milestones

1. Asynchronous completion providers

   Synchronous trigger providers, inline phrase ghosts, Tab cycling and lazy
   continuation callbacks are implemented. Add asynchronous resolution only
   when an embedding host requires it.

2. Unicode width policy

   Decide whether to extend the built-in cluster/width rules or adopt a
   width/grapheme dependency for fuller Unicode coverage.

3. Event-loop-friendly session API

   Split `readline()` into create session, feed input, render, poll, and finish
   operations while keeping the existing blocking API as a facade.

4. Searchable history

   Extend current reverse search with forward search, filtering hooks, and any
   richer search lifecycle API that later callers need.

5. Structured diagnostics

   Preserve `last_error()` and `last_readline_status()` but add richer
   machine-readable detail for advanced callers.
