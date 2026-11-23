# Native editor guide

This guide covers the terminal editor's keymap and integration behavior.
The installed `<softline/softline.h>` is the reference for public C
declarations, defaults, ownership and result codes. The [Lua API](../lua/README.md)
documents the corresponding Lua bindings. See the [project README](../README.md)
for build and example commands.

## Keyboard editing

The built-in editing keys are:

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
- `Ctrl-B` / Left moves backward by common UTF-8 cluster boundaries. `Ctrl-F` /
  Right moves forward, or accepts/commits an active completion at its token
  endpoint; see [completion providers](#completion-providers).
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
- Ctrl+V acquires PNG or JPEG data on Linux with X11. A custom Ctrl+V
  binding takes precedence. See [clipboard and image handover](image-paste.md).
- `Alt-B` moves to the current or preceding alphanumeric word's beginning;
  `Alt-F` moves to the current or next word's end. Punctuation separates words.
  Unicode 16 letters, marks and numbers participate without splitting clusters.
- `Alt-D` kills through the next alphanumeric word's end; `Alt-Backspace`
  kills back to the preceding word's beginning. These word motions may cross
  actual newlines; Ctrl-W retains the line boundary described below.
- Ctrl+T cycles registered [transcripts](transcripts.md). With no viewers,
  it transposes adjacent Unicode clusters. At a newline or buffer end it
  swaps the preceding pair. At buffer start or without a pair it leaves
  the draft intact.
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
  its prior contents, then clears the editor. A draft containing only whitespace leaves both the draft and stash unchanged. `Alt-J` appends that saved text at the end of the current draft,
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

## Rendering and callbacks

Softline wraps long drafts at word boundaries where possible, measures cursor
position in terminal cells, and recalculates the viewport when the terminal
resizes. Ordinary readline prompts participate in scrollback. A persistent
native output session keeps the prompt at the terminal bottom. The
[streaming output guide](softline-mdf-stream-design.md) covers producer
ownership, geometry and resize reconciliation.

The prompt, queue previews, and status lines share a theme. Use
`sl_set_prompt_theme()`, `sl_set_statusline()` and the `sl_set_status_*`
methods to configure them. The application supplies status text and busy state.

Applications can bind decoded keys and register idle or descriptor-watch
callbacks. A key binding can pass input to the built-in action, handle it,
submit, cancel, interrupt, request repaint, or suspend input. Use
`sl_last_readline_status()` to classify the result and `sl_last_error()`
for the handle-owned diagnostic. Release returned strings with
`sl_free_string()`. See [terminal handoff](terminal-handoff-design.md)
for suspension, terminal ownership and recovery.

## Prompt queue

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

Queue and pending-send previews use a 500 ms display delay while the queue is
hidden. New entries appear immediately while the queue is visible. An empty
rendered queue rearms the delay. Change it with
`sl_config_t.prompt_queue_display_delay_ms` or
`sl_set_prompt_queue_display_delay()`. Zero displays immediately. Queue
operations and dispatch are immediate, as are approvals, active edits and
connection/error notices.

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
`line, source, id`, and `queue_take()` returns `text, id`.

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

## History storage and recording

`history_open()` loads existing entries immediately and attaches synchronous
append persistence. The exact nonempty key is hashed with SHA-256 into
`$XDG_STATE_HOME/softline/history/<hash>.history`; an unset, empty or relative
`XDG_STATE_HOME` falls back to `$HOME/.local/state`. Pass an absolute directory
as the third argument to override the complete storage directory. Missing
parents are created recursively with mode 0700; the store directory is private
and files/lock files are 0600. Native store files must be regular, user-owned,
not symlinks or hard links. No extra link dependency is required.

Each prompt occupies one physical line: backslash, LF, CR and Tab are encoded
as `\\`, `\n`, `\r` and `\t`; UTF-8 remains literal. Ctrl-J is LF, so it uses
`\n`. Empty entries and consecutive duplicates are ignored. The retained cap
bounds recall, not the append-only file. A zero cap disables recording entirely.

`history_add()` appends immediately before returning. No exit-time save or
per-entry fsync is performed. Same-key processes serialize writes through a
stable flock lock file (also serializing independent handles in one process).
An unfinished final record is ignored on load and removed
before the next append. Other processes' writes appear on the next load, not
live in another handle's recall list. `history_close()` detaches persistence
without clearing recall. `history_compact()` explicitly retains the newest
capped entries from the shared file under the same lock and replaces it
atomically; it never writes a stale recall snapshot over another writer.

For custom storage, call `history_set_backend(key, load, append, userdata)`.
The optional load callback receives an emitter and supplies entries oldest
first; the required append callback receives each accepted entry. Both return
`SL_OK` or a negative status. Softline copies the key; userdata stays caller-owned
until detach or destruction. Emitters and borrowed strings must not be retained.
Imports bypass append. Attachment/load failure preserves the old backend and
recall; append failure preserves recall, with external rollback owned by the
backend. Do not reenter history methods or destroy the handle from either hook.
The existing `history_load(filename)` also bypasses append; `history_save(filename)`
is an explicit snapshot operation, not the native store's concurrency protocol.

Automatic recording is disabled by default. `history_set_auto_add(1)` records
accepted editor submissions and queue/steer drafts at acceptance. Queue delivery,
promotion, Ctrl-R match selection, cancellation and rejected drafts do not
append. Programmatic queue insert/replace needs explicit `history_add()`.
Applications using auto-add must not add delivered queued entries again.

Both C and Lua chat examples use the shared key `softline.examples.chat`, across
all themes. `SOFTLINE_HISTORY_DIR` overrides their complete directory. Examples
record accepted commands as well as ordinary prompts. See the [Lua history API](../lua/README.md#persistent-history).

## OSC 8 hyperlinks

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

## Completion providers

`sl_set_completion_providers()` and the receiver's `set_completion_providers`
method copy an array of `sl_completion_provider_t` registrations. Each has a
printable non-whitespace ASCII `trigger`, a `scope`, `strip_trigger`, a synchronous
`sl_completion_callback_t`, optional `sl_completion_expand_callback_t` in
`expand`, and borrowed host context. Duplicate triggers,
invalid fields and copy failure preserve the previous registrations.
A zero count clears them. `SL_COMPLETION_START` (zero/default) anchors only
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
commit and on Tab outside an active cycle, when actual text contains a completed
candidate. `completed` includes the trigger. It also works for a unique, fully
present match, and for known ancestor candidates when the user has typed part
of a child path. Emit
full strings strictly extending that prefix. A nonempty result becomes a
separate completion level. Commit previews its first matching candidate;
Tab accepts it. A Tab-triggered lookup accepts the candidate on that same Tab.
An empty result retains normal completion or host Tab handling. No continuation
is called for ghost text alone. An existing matching suggestion is accepted
before trying a shorter ancestor continuation, including after each lazy lookup.

Each prefix is resolved at most once per activation, including empty and failed
attempts. Softline keeps parent and child snapshots separate, choosing the most
specific cached list with a candidate matching the draft. Backspace below its
prefix returns to the original parent list, rather than mixing parent candidates
with descendants. Continuing inside a cached level does not call the provider again.
Repeated Tabs stay within the current list, including a one-candidate list
with a continuation provider. Right or Ctrl-F commits the selected candidate
without moving the cursor, resolving that candidate's next list for an immediate
ghost preview. It never inserts that next candidate or recursively resolves
its descendants. An empty or failed continuation suppresses competing parent
ghosts. A nonmutating suspension preserves the preview and cached results.
After committing a leaf with no continuation preview, Right/Ctrl-F resumes
ordinary forward character movement. At the end of the draft, no-op forward
keys preserve the commit and leave its text unchanged.
These keys also accept a ghost at its token endpoint. Within an existing word
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
example, typing `hello ` keeps `world` visible for `hello world`. Tab accepts
the phrase and further Tabs cycle matching phrases. Whitespace without a
remaining matching suffix ends the activation. Deleting the trigger, moving
outside the token, submitting, queueing, replacing the draft or replacing
registrations also ends it. Bracketed paste inserts literal text without
starting new provider lookups while the paste is in progress. A separator in
the paste can still finish an active completion.
Accepted candidates and restored undo text may themselves contain spaces;
those spaces do not end the activation. Other edits or movement end the Tab
cycle and rematch the current prefix against the cached snapshot. A new trigger
activation or an explicit continuation lookup can call the host; ordinary
redraws do not.

If a phrase stops matching and the current word starts with another eligible
registered trigger, that word starts a fresh activation. For example,
`hello @` leaves the `hello world` phrase and starts the host's `@` provider.
A still-matching phrase keeps ownership of embedded trigger characters, and
start-only providers remain ineligible after whitespace.

Matching is literal and case-sensitive, starting with the trigger itself.
The first match appears as a dim inline suffix. Tab accepts that full candidate;
further Tabs cycle the originally resolved matching set in list order, wrapping
to the first. Thus
`/` can cycle all slash commands, while Backspace from `/command1` to `/command`
restricts the next cycle to that prefix. No whitespace is appended. With no
active cycle and no available continuation, a unique fully present completion
or no match lets normal host bindings and queue mappings receive Tab. Matching
completion precedes both.
Reverse search and bracketed paste retain their existing key/literal semantics.
With providers configured, Tab during acquisition of a backend edit hold
waits for that outcome and earlier deferred typing. It then completes the actual
draft or falls through to the host, including when the hold was rejected. Other
host control bindings remain responsive while the hold is pending.

Completion inside text replaces the remaining current whitespace-delimited
word while preserving subsequent unrelated text. A complete already-present
phrase suffix is reused. An entire Tab cycle is one undoable edit, separate from
adjacent typing, and retains queue/approval identity, holds and order. Accepting
a candidate over the draft limit leaves the draft/cursor unchanged and consumes
the acceptance key with a bell.
If reused phrase bytes join an existing combining-mark or emoji continuation,
the cursor advances past the resulting whole cluster; those existing bytes are
preserved. Subsequent movement, deletion and undo keep their cluster semantics.

Ghost text is a display-only projection inside the multiline editor, wrapping
and paging with it. It does not change the draft, actual cursor, undo journal,
queue/approval entries or host status/reasoning. Enter submits only actual text.
Tab or Right/Ctrl-F at the token endpoint accepts a suggestion; only Tab starts
cycling. Existing matching text after the cursor is reused in the preview;
unrelated text remains visible. Other navigation operates on the actual draft.
There is no separate completion panel. Ghosts follow cursor moves, host list
changes, undo, suspension and resize. No producer
transcript is retained.

`strip_trigger` is off by default. When enabled, normal completion exit or
submission removes the one introductory character if the whole token matches
a resolved candidate. This includes exact typed matches and accepted
continuations. Partial or unmatched text keeps its trigger, including edits
that make an accepted token unmatched. In queued or held edited entries the
removal preserves identity, hold and FIFO order.
Enter submits actual text and never accepts a ghost. For example, `@REA` + Tab
can accept `@README.md`; Space or Enter removes `@`, leaving `README.md`.
Trigger removal is undoable. When a pasted separator finishes an exact
completion, one undo restores the draft from before that paste. Whole-draft
replacement or discard ends the activation under the usual editing rules.
Failed providers preserve the trigger. Paced prompt simulation treats trigger
removal as part of typing and continues with the remaining text.

Use `sl_set_completion_predicate()` or `editor->set_completion_predicate` to
check context before a provider resolves a new token. The callback may read
the draft and cursor. A positive result allows the provider. Zero or a negative
result skips the provider silently and preserves the literal trigger. The
editor can check the same token again on a later input turn. For example, a
host can decline its filename trigger in a shell draft. Passing `NULL` clears
the predicate. Replacing or clearing it discards cached completions while
preserving the draft and cursor.

C/Lua simple multiline and chat examples use callbacks for `/`, `h`, `v`, `n`
and `@`. Slash callbacks provide the host's commands. `@` takes a fresh current
directory snapshot on activation. It is eligible at word boundaries and strips
the marker when an exact match exits or is submitted. The local and remote
example hosts share the file discovery implementation.
Directories end in `/`; commit one with Right/Ctrl-F to preview its first child
through the same resolver, without scanning descendants. Tab accepts/cycles
that directory's children. The command resolver similarly extends `/approve`
to `/approve on` and `/approve off`. Hidden entries are
included, `.git` entries excluded, and symlinks listed without following them.

## Scope

The editor follows a handle-based C89/POSIX interface. Its current scope includes
synchronous completion and reverse incremental history search. Potential future
work includes asynchronous completion, richer history search, fuller Unicode
segmentation and a split-phase input loop.
