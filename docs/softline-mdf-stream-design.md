# Composable Live Output: Softline and libmdf

## Status and boundary

This is the acceptance contract for Softline's live output refactor. Softline
does not own, include, link, package, or expose libmdf. The application owns
its producer, event transport, Markdown renderer, renderer options (including
the two-column margin used by the chat examples), and document boundaries.
Only the chat examples and integration tests depend on libmdf. A released
libsoftline, its Lua facade, headers, CMake exports, and package metadata have
no libmdf dependency.

libmdf 0.19.0 has incremental feed, a bound sink, and in-document width and
margin changes through `set_geometry()`. The chat composer updates both
renderer geometries on the Softline owner thread before feeding later
Markdown; Softline reads the terminal dimensions for its native prompt.
The composer uses a two-column left margin except on very narrow terminals,
where it drops the margin to retain libmdf's three content columns. Neither
Softline nor the examples recreate or replay an unfinished Markdown document.
Softline never infers libmdf margins. The example supplies its borrowed stdout
fd to libmdf's destination-aware ANSI AUTO policy: terminal output is themed,
and redirected output is escape-free. Renderer handles are rebuilt against
libmdf 0.19.0's options layout; this private example dependency does not change
Softline's ABI.

## Generic Softline output session

One editor may have one open live output session. Public C receiver methods
and equivalent free functions begin the session, write a byte span, and end
the session. Lua exposes the same lifecycle on its editor userdata. The API
uses Softline-owned names and byte strings only, no renderer-specific types.

The session is owner-thread-only. A producer thread/process hands fragments to
the application through a pipe or another bounded transport. The application
registers its descriptor with `sl_watch_add()` (Lua: `watch_add()`), drains it
on the editor thread, feeds its renderer, and forwards each sink emission
directly into the Softline session. Softline does not buffer whole messages,
interpret Markdown, own the transport, or run a worker. A sink write is visible
before the corresponding Softline write returns; no finish/EOF or next prompt
is needed to make an already-decided fragment appear.

Softline adds no content bytes, response separator, or space at chunk
boundaries. With an active prompt, a write that ends exactly at the terminal's
right edge can change how the next write wraps; the scoped exception below
describes this case. Terminal cursor and style controls surround native writes
to isolate the editor. Output position and producer style are tracked by
Softline rather than by terminal cursor save/restore sequences. Native LF
positioning follows the output TTY's OPOST/ONLCR settings; producer bytes are
unchanged. While Softline owns raw mode, kernel tab expansion (`TAB3`/`OXTABS`)
is disabled: the kernel counts ANSI bytes rather than terminal cursor
movements, so its expansion cannot track the VT cursor. Tabs reach the terminal
unchanged. Original flags are restored when raw mode is released, including
for a separate output TTY. Other output flags, including OPOST/ONLCR, are
preserved. Native writes after raw-mode release (including a returning SIGINT
handler) temporarily disable output tab expansion and restore the flags before
returning. They do not reacquire raw mode or flush pending input. A failed
native session startup restores raw mode only when
that startup acquired it, preserving raw mode already owned by the editor.
Ending a session does not finish a Markdown document. The application calls its
renderer’s document lifecycle itself. Each successful write accepts its entire
span; errors are reported on the handle and never silently discard accepted
data. A zero-length write is a no-op. Writes after end and overlapping begins
fail. Existing finite `sl_print_above()` remains available in native terminal scrollback. It cannot be interleaved with an open live session.

The quoted-prompt helper writes submitted text literally into an open session
between external renderer segments. It wraps at the current output width,
repeats a configurable prefix on each visible row, styles the prefix and
italic text independently, and adds only missing line breaks for one empty
row on either side. Prompt text does not pass through libmdf. The optional
status message sits above the status line, may change during a live session,
and wraps continuation rows under its configurable first-row prefix. Both
features are exposed through the C receiver/free functions and Lua methods.

## Terminal ownership and geometry

The output session and editor prompt have one serial terminal owner. A
full-terminal transcript begins at the existing terminal cursor. The editable
prompt stays at the current terminal bottom during active editing, including
after height growth. A retained frame between editor calls follows native
reflow until editing resumes. Changed prompt cells or placement are updated.
The VT scroll region ends immediately above the current prompt
frame. Native producer bytes are written unchanged. Softline retains bounded
partial ANSI/UTF-8 parser state and an output cursor. It never rewraps, pads,
clears, or replays native transcript text. Positions are distances from the terminal bottom.
Resize reconciliation queries the live input cursor and combines its observed
movement with the known layout of the old prompt. Unchanged prompt rows that
fit the new width keep their cached cells. The logical prompt frame survives
successive width changes so a wrapped physical row does not become a new
independent source row. Width resize rebuilds only prompt
layout; transcript reflow remains owned by the terminal. Plain ASCII lines use
scalar cell counts. Unicode cursor advances share the prompt scalar width
helpers, without glyph span tables or transcript reflow reconstruction. For an opaque unfinished line, resize keeps the tracked column and
clamps it only if it is offscreen; exact continuation after its reflow is an
accepted limitation below.
Carriage return moves the producer cursor without discarding cells to its
right. A plain ASCII line's visible extent remains available for resize
positioning; producer bytes continue to pass through immediately.
When no prompt frame has been rendered, the live cursor belongs to the producer.
Writes leave that cursor in place, preserving native pending wrap and Unicode
clusters across chunks. No editor rows are reserved until a frame exists.
A full-height region follows terminal resize without another margin command.
Cursor reports refresh the producer's bottom-relative row and column at resize,
first editor handoff, finite-output completion, and session close. At unchanged
geometry, the handoff corrects the logical column used by later width changes
as well as the current cursor position. A physical resize's viewport movement
is not counted as emitted text. Feed writes never request or read cursor reports,
including Unicode writes and resize reconciliation invoked by a feed call.
There is no sleep, timeout, timed coalescing or producer-tail delay. Unicode
scalar advances are accounted locally; the original bytes reach the terminal
unchanged. Cursor observation remains at non-feed ownership boundaries.
A bounded viewport-shift estimate survives output-session boundaries. It contains
no text, cells, glyph spans or Softline scrollback. It limits history restoration
when growth exposes more rows than Softline's output has scrolled. Pre-existing
terminal history is unknown; the fallback guesses no history for an initial
row-zero cursor and otherwise assumes history can fill newly exposed space.
Neither is a statement about the real terminal history. Resize while input owns
the cursor observes the actual terminal position.
Moving to an editor or temporary parked cursor releases
producer ownership, so later writes use the stored output position. Rendering
a prompt sets its actual margin.
At a known ASCII right edge with an active prompt, returning to the editor clears the
terminal's pending-wrap state. A later printable continuation uses CR and IND
to reach the next row without overwriting the last cell. This is a hard row
boundary in terminals that distinguish hard and soft wraps. Producer-emitted
LF is handled normally; no extra row advance is needed when LF comes next.
Finite callbacks and live writes share the same native geometry reconciliation.
Finite output reconciles the retained prompt before invoking its producer and
again after each callback returns, including EOF. A resize during a callback
uses the same tracked cursor delta as a live write. Input-owned resize
reconciliation occurs before dispatching ready producer watches; feed calls
never acquire a cursor report themselves.
Ordinary readline's optional scroll-region ownership is checked at startup
and retried between producer callbacks after finite output advances its prompt.
Multiple feeds in a single callback remain immediate and defer acquisition
until the next input-owned boundary.
Physical resize updates internal geometry, restores the reserved output margin
and repositions an active editor at the terminal bottom. Restoring margins
invalidates the cached hardware cursor even when the text is unchanged; the
renderer restores the input cursor before returning. A prompt-free full-height
producer retains its hardware cursor and pending wrap. Retained-frame output
preparation also restores the input cursor when an incomplete or rejected
fragment emits no producer bytes; a later feed is not needed to restore it.
The incremental ANSI/UTF-8 validator is unchanged. Each batch that positions
the producer cursor establishes its output margin alongside the producer bytes,
cursor moves, and prompt return.
Ctrl-L performs an owner-thread layout reset. It rereads PTY geometry,
reasserts owned margins and repaints every visible prompt/status/queue row,
including unchanged cached rows. Cached ownership and reflow geometry remain
available; producer counters, pending ANSI/UTF-8 fragments, draft/cursor,
history-search state and queue/approval identities remain unchanged. This
operation does not clear terminal history or replay producer bytes. Hosts may
remap it through the RESET key action without changing the receiver layout.
Margin state is not cached across these batches because terminal resize can
reset it after the new PTY size becomes observable.
Updating margins after physical resize
must not scroll the transcript again; cells clipped off the top are not
recovered or replayed. When an unfinished line leaves the screen, its next
fragment resumes at the first visible output row. If the clipped position was
at a hard line boundary, new output starts next to the prompt. Neither case
changes cells or blank rows already in scrollback.
Width reflow resets a clipped endpoint during resize reconciliation rather than
waiting for another feed. A shrink followed by growth without intervening
producer bytes must not revive that invalid row or column over earlier output.
Height-only round trips retain the native history position until a feed needs
to address it. This changes only endpoint counters; producer bytes remain
unbuffered.
Clipping resets physical column and wrap measurements without ending the
producer line. Repeated resizes, SGR-only writes and CR preserve unfinished-line
state; an actual LF establishes the hard line boundary.
A cached previous prompt frame allows
patches of only changed cells; native feeds preserve unchanged prompt cells
and restore the editor cursor. Erasing an owned prompt row uses the default
background and erase-to-end from column zero. This avoids leaving a full-width
blank row that native terminal reflow can split when the width shrinks.
The output margin follows the current rendered
prompt height, with no empty queue slots or absent status-message rows. Growth
scrolls existing output cells only enough to fit; shrink returns freed rows
without moving transcript cells. The editor pages only at physical capacity.
Native feeds batch bounded chunks with cursor restoration, retrying short
writes as needed, without hiding the cursor or repainting unchanged frames.
Ending chat restores the full scroll
region and clears only input rows, preserving queue and status rows. The cursor
stays at column zero on the current input row without a newline or scroll.

The geometry model follows the terminal's live cursor rather than a saved
terminal cursor slot. [tmux's resize implementation](https://github.com/tmux/tmux/blob/3.6/screen.c)
moves rows above the cursor into history on shrink and pulls history back on
growth; it resets scroll margins after height changes. The
[Codex history writer](https://github.com/openai/codex/blob/main/codex-rs/tui/src/insert_history.rs)
uses a full-width vertical scroll region above its editable viewport and
application-owned coordinates to return to the input cursor. Softline applies
that separation to incremental producer writes and retains only the prompt
frame for cell comparisons.
`clear_prompt_on_exit = 1` selects clearing the whole prompt area and returning
below the transcript. Ending a stream inside an active editor retains the
prompt and scroll region for later finite output or another stream. Closing
the handle always restores terminal state. Non-TTY sessions emit no teardown
controls. Ctrl-C restores unrestricted scroll margins, the bottom input cursor,
and termios before raising SIGINT. If a handler returns, the existing native
output session remains available and its scroll margin is reinstated.
Cursor queries outside an active raw readline temporarily acquire noncanonical,
non-echo input without flushing pending bytes, then restore the prior attributes.
Native chat needs at least three terminal rows: two for output and
one for the prompt.

Rectangular viewports and bounds configuration are unsupported. Native chat
always uses the physical terminal width. `sl_set_screen_width()` controls
ordinary readline wrapping; it cannot rewrap producer output in a chat session.
Without an active readline, changing the hint leaves native output geometry
untouched. The next write or session close reconciles any physical resize.
Softline detects dynamic dimensions through `TIOCGWINSZ` without taking
ownership of `SIGWINCH`. An application may register its own signal/self-pipe
or other event source to update its external renderer before feeding more data.
Input bytes, editor cursor position, queue contents and pending parser state
survive resize. Memory use is independent of completed transcript length;
feed state is bounded independently of unfinished line length. The stream
accepts one-byte fragments indefinitely.

## Compatibility and public surfaces

The C and Lua surfaces expose a single native chat layout. ABI 1 introduced the
public `output_stream_reconcile` receiver method. ABI 2 retains it and adds
per-entry queue approval and stable identities. ABI 3 adds native submission
policies and per-entry execution/stop state. ABI 4 adds constructor-selected
asynchronous queue backends. Equivalent free functions validate NULL handles.
Public declarations document ownership, thread context, lifecycle, errors,
and chunk semantics for clangd. Lua methods document the same contract in the
Lua reference and adjacent public binding comments. Lua cannot invoke its VM
from a foreign producer thread; a watched descriptor supplies owner-thread
events. The core and Lua libraries do not import libmdf.

### Shared-library ABI

The current ABI policy supersedes the earlier development ABI 0 exception.
The reconciliation method advanced released v0.10.0's ABI 0 to ABI 1 in v0.11.0.
Queue approval extends that released receiver layout and changes insertion
signatures in ABI 2. Native queue policies extend the receiver and entry layouts,
advancing released v0.13.0's ABI 2 to ABI 3. Constructor-selected backends advance
released v0.15.1's ABI 3 to the current ABI 4. Both C and Lua consumers must
rebuild against the matching header and library. Local release rehearsal tags
are not the released baseline for this decision.

### Review exception: active-prompt right-edge continuation

The production executive accepted this tradeoff on 2026-09-29 for generic
native output with an active prompt. If a known ASCII write ends at the terminal's
right edge and a later write continues the same line, Softline uses CR and IND
because returning to the prompt has cleared the terminal's pending wrap. The
continuation stays visible without overwriting the last cell, but the row is
hard-wrapped: widening the terminal may not join it with the previous row.
If a Unicode grapheme crosses that same boundary, a combining mark or joiner
may fail to attach to the preceding character. Softline does not buffer or
replay producer text to repair the cluster. This exception does not apply while
the producer owns the terminal cursor before the first prompt frame, or when
the producer emits LF before continuing. The chat example gives libmdf its
current geometry and forwards its sink emissions; libmdf's explicit line wraps
avoid the seam in the tested ordinary paragraphs, but sink writes have no
general line-boundary guarantee. Reconsider this exception if lossless
arbitrary-chunk Unicode output with an active prompt becomes a requirement or
if a portable solution preserves pending wrap without reintroducing the
rejected cursor-ownership, replay, or buffering behavior.

The terminal can also harden a wrap when output reaches the bottom of its
scroll margin. VTE retains that row boundary after width growth in the
40-to-52-column reproducer; tmux reflows the corresponding rows in the
30-to-80-column growth regression. Softline cannot infer which physical row
will hold the continuation from a producer-byte cell count alone. Resetting
the count at every margin scroll fixes the VTE case but overwrites output in
tmux. Without transcript replay or a terminal text query, exact continuation
at this seam is outside the accepted native-streaming contract.

### Review exception: output clipped behind a bottom-anchored prompt

The production executive accepted terminal-native clipping without replay on
2026-09-29. A physical height shrink keeps the actual terminal cursor at the
prompt. With little or no prior scrollback, an unfinished output line can then
move above the visible screen, even when its tail is retained by the terminal.
For example, emit 35 ASCII cells, CR, and Q at 40 columns, then grow to 80 by
12 rows and shrink to 30 by 8 rows. The producer cursor can no longer address
its original cell while the prompt remains at the bottom. Softline continues
at the first visible output row, as specified in the README. It does not
replay or buffer producer text, and it does not move the terminal cursor away
from the input prompt during idle resize. Exact agreement with a promptless
terminal's cursor in this offscreen case is outside the supported contract.

Unicode/tab retained tails also have the opaque-feed limitation below.

### Review exception: concurrent physical resize

VT has one hardware cursor and no atomic operation combining a PTY size
snapshot with a terminal write. A resize between those operations can apply
an in-flight producer batch to a different row. This accepted tradeoff also
affects ASCII, and was reproduced with the withdrawn v0.8.0 library: during
the live example's mixed 97×24 → 45×24 → 30×18 → 80×28 → 40×12 → 97×24
sequence, `single` can disappear from `A single line can be italic, bold, or
code.` No transcript replay, feed wait or terminal-specific workaround is
introduced to conceal this race.

The standard VTE streaming resize check pauses its private example worker
and freezes the editor at an acknowledged idle poll while the physical
resize is applied. It drains the completed frame, then resumes the editor
before asserting prompt reconciliation; the worker stays paused until that
check passes. The spinner is also a terminal writer, so pausing only the
worker did not establish a settled resize boundary: a status repaint could
cross GTK allocation and overwrite the input row. No production timing,
buffering or repaint behavior is changed by this fixture coordination.
It verifies continuation, prompt placement and unchanged transcript cells
once the terminal geometry agrees with the PTY. The former Python
`--streaming-race` diagnostic is no longer exposed by the native harness;
the paced gate does not prove race-free VT output. The separately paced tmux
check has the same scope restriction.
The themed width-sweep gate likewise pauses its private producer during each
sweep and freezes the editor only for each physical allocation, resuming it
before checking prompt updates. The former Python `--resize-race` diagnostic
is likewise no longer exposed; its unpaused behavior is outside the paced
gate's proof. The shared coordination
helper has explicit stop/poll and input barriers, including pending replies
needed to finish a frame, with resume checks after resize/drain failures.
Input responsiveness is tested with the editor running.

### Review exception: unobserved retained-frame resize

Policy source: the developer's hard zero-delay feed requirement and earlier
decision to retain terminal-owned transcript reflow without replay or a
Softline scrollback model. This applies only when a retained editor frame is
resized without an input-owned cursor observation, including writes after
`readline()` has returned and resizes performed inside a producer callback.
The active chat loop observes resize before dispatching producer watches.

Synchronous producers now have an explicit owner operation:
`sl_output_stream_reconcile()`. Call it after resize settles and before the
next producer write. It may wait up to 100 ms for a cursor report, reconciles
and repaints only the owned prompt frame, and preserves queued input, draft,
cursor, queue, history, status and incremental UTF-8/SGR state. A missing report
returns `SL_ERROR_IO`; callers must handle that failure before continuing.
No transcript is stored or replayed. This does not change observation-free
feeds or repair a resize that races a physical write.

The fallback uses bottom-relative deltas and current-line scalar geometry.
It cannot infer reflow of earlier hard-ended lines, or distinguish empty history
from a cleared and homed screen with preserved terminal history. Even a settled
resize can therefore misplace ASCII continuation in this path. Row zero is not
proof of empty history; the row counter is an estimate only. Do not add a feed
cursor-report wait, transcript buffer, line/span reconstruction or replay to
repair this ambiguity. The withdrawn 0.8.0 passed these cases by waiting for a
cursor report inside the feed, which is the behavior being removed.

Reproducers in VTE, initially 40×8: emit 80 ASCII `a`s followed by `\nABC`,
retain a submitted prompt, grow to 80×8, then emit `Y`; the fallback may put `Y`
on a separate row. Alternatively, fill terminal history, use ED+CUP to clear
and home without erasing history, emit `ABC`, retain a submitted prompt, grow
to 40×12, then emit `Y`; an empty-history estimate can address a history row.
The native-output regression runs both scenarios with readline still active
and asserts exact direct-terminal continuation. Feed latency checks assert no
cursor requests even when a feed must reconcile resize itself.

Callers that omit owner reconciliation retain this fallback limitation.
A missing input-owned observation is separate from the concurrent physical
resize race above.

### Review exception: opaque feed

The production executive requires zero intentional feed latency, unchanged
producer bytes, no transcript replay and no glyph-span reflow model. After
withdrawing v0.8.0 for synchronous Unicode cursor round trips, local scalar
cell accounting replaces those replies. UTF-8 validation is bounded parser
state; no grapheme lookahead or full-message buffering is introduced. Prompt
layout remains separate. Conventional scalar widths cover ordinary accents,
combining marks, format controls, wide characters and table borders, using
Unicode 16.0 scalar properties. Terminal Unicode versions and emoji
shaping may differ and can misplace an active-prompt continuation. This limit
must not be addressed by adding feed waits or changing producer bytes.

After an unfinished Unicode/tab line reflows on width resize, its tracked
endpoint cannot identify the terminal's new logical position. Softline retains
the tracked column and clamps an offscreen column; continuation can land in
a gap or overwrite text. This includes CR-overwritten tails, wide cells crossing
a new margin, and tabs narrowed below their original span. A cursor report at
the last physical column also cannot distinguish Unicode pending wrap from a
cursor before the last cell, so an active-prompt continuation there may overwrite
the last cell or lose a cluster attachment. Softline does not build a glyph/span
model, add per-character probes, buffer graphemes, or replay the feed to recover
these cases. This exception does not permit modifying producer bytes or clearing
transcript cells during resize. Plain ASCII continuation, normal prompt layout,
ordinary scalar Unicode rendering away from those right-edge ambiguities, byte fidelity
and terminal-state restoration remain tested requirements. Reconsider only if the production executive changes the
opaque-feed/latency contract. The CR-tail diagnostic retains a reproducer.

## Verification

- Isolated PTY tests enable kernel tab expansion before startup, verify literal
  tabs and configured LF processing, and check restoration after ordinary
  readline, stream end, and destruction. Input/output sharing one TTY and
  separate TTYs are covered. Real VTE compares chunked tab output and height
  changes with direct terminal output.
  A returning SIGINT handler also preserves literal tab bytes in subsequent
  native writes and restores the original flags before each write returns.
- Real VTE PTY responses and tmux compare chunked producer output and the exit
  cursor with direct terminal bytes. Cases cover exact-width ASCII, combining
  marks, flags, joined emoji, variation selectors, and keycaps. VTE also checks
  first editor handoff and live width/height transitions with empty terminals
  and 100 prior rows, preserving internal blank rows. Finite output in one-byte
  Unicode chunks ends at the native endpoint before another stream begins.
  Writes after a retained editor frame and repeated terminal growth resume on
  the producer's last line, including a width hint between resize and write,
  with and without prior scrollback. Wrapped Unicode handoff tests keep the
  width fixed while changing height. Opaque-feed tests compare mixed-width
  Unicode, tabs and CR tails in one-byte and complete emissions, followed by
  terminal-owned static width growth. Exact post-reflow opaque continuation is
  covered by the scoped exception rather than a span reconstruction test.
  Finite callbacks after ending a live session inside readline continue the
  producer's last line through repeated width/height changes. Plain text,
  multiple lines, UTF-8, and ANSI styles run with empty and filled terminals;
  callbacks pause during a chunk and before EOF so resize cannot be handled
  by an intervening editor iteration. The full retained transcript, bottom
  input cursor, absence of stale prompts, and final exit cursor are checked.
  Ignored SIGINT and returning handlers leave the input mode restored before
  more output; subsequent cursor reports remain absent from the transcript and
  session close restores the original input attributes.
  After `readline` returns, shrinking rows and sometimes columns before session
  close clears the retained input row at its new location and exits at the new
  bottom row.
- When GTK3 VTE and `xvfb-run` are installed, a real-engine regression runs the
  chat example in an already filled terminal. It compares physical transcript
  rows across 97↔96 and stepwise 110↔90 column changes with three responses,
  three prompt themes, and two terminal heights. Blank rows count as part of
  the transcript, so an erased row wrapping into an extra gap fails the test.
  A second VTE check generates three varied responses continuously while
  changing both width and height, with an editable draft and existing
  scrollback, under plain and Gruvbox prompts. It verifies complete paragraphs
  remain in order and the draft remains visible.
- A PTY test writes `Hello`, pauses, and observes it above an editable prompt
  before EOF. Later writes of ` world` continue the same line. Typing and
  queue edits remain responsive between writes.
- Identical output in one-byte and larger chunks yields the same visible
  screen, including styled ANSI and UTF-8 split across write boundaries.
- Prompt growth, queue/status rows, width setters, and terminal resize retain
  native geometry without replaying or clearing producer output.
  Real-terminal reflow tests use a gated libmdf producer with short paragraphs,
  lists, quotes, code blocks, combining characters, and wide characters. Each
  content set runs both without resizing and through width/height transitions;
  the driver waits until the application observes each requested PTY size.
  Static resize checks pause the producer and compare surviving transcript
  cells before and after Softline handles the resize; the terminal's own
  reflow is the reference. They cover paragraphs, lists, code, Unicode, and
  queue rows. A separate PTY matrix rejects scroll commands during physical
  resize and verifies scrolling remains available for ordinary prompt growth.
  Combined row/column growth tests append to an unfinished wrapped line after
  native reflow, with and without pre-existing scrollback, and compare the
  entire retained transcript to ensure earlier output was not overwritten.
  A producer-only matrix appends after width, height and combined resize,
  with ASCII, hard line breaks and Unicode, with and without scrollback.
  It compares the complete transcript to the terminal's native endpoint and
  then checks the handoff to a bottom-anchored editable prompt.
  Clipped unfinished-line tests compare native scrollback before and after
  continuation and reject newly inserted gaps; blank rows already introduced
  by native resize are preserved. The clipped-line PTY check also covers new
  output at a hard line boundary.
- The tmux live-example check spaces resize commands so each PTY notification
  settles. [tmux queues notifications for 250 ms](https://github.com/tmux/tmux/blob/3.6/server-client.c#L2647-L2713)
  while changing its grid immediately. Faster successive resizes can leave the
  visible grid and reported PTY geometry different while output is emitted;
  that case can still corrupt output. The `--queued-resize` diagnostic in
  `tests/test_tmux_resize.c` retains this reproducer. The standard paced
  test does not prove that diagnostic passes. There is no tmux-specific
  production workaround or transcript replay.
- The Unicode output matrix compares actual raw and libmdf sink emissions
  through direct PTYs and active native prompts. It checks complete producer
  byte equality, fragmented UTF-8, style transitions, width growth, pre-existing
  scrollback, and input restoration after Ctrl-C. A private endpoint test
  checks that a previous line's observed movement cannot shift a new line's
  column. The feed latency regression keeps terminal probing enabled, withholds
  replies during tiny UTF-8/SGR writes, and rejects any feed cursor request. It
  also covers physical resize, quoted output and concurrent typing, and fails
  against the withdrawn v0.8.0 library.
- First-handoff checks submit an empty prompt after producer-owned output,
  repeatedly shrink and grow width and height, and append output at each size.
  They compare against direct terminal bytes for ASCII, styled flags, joined
  emoji, combining marks, CJK, and hard line breaks, with and without prior
  scrollback. Fully visible flag lines also cover wrapping and exact-width
  endpoints.
- Ordinary readline checks narrow and widen drafts with the caret at their
  beginning, middle, and end. They preserve every prior transcript row and
  compare the resized prompt with a fresh prompt at the same width, including
  Unicode, multiple prompt rows, and existing scrollback.
- Sink/write failure and session teardown leave a usable editor and report
  an actionable diagnostic. Repeated begin/end and invalid calls are tested.
  Native finite-output regressions reject incomplete ANSI/UTF-8 and callback
  failures, then verify the next call emits its bytes without stale prefixes.
  Starting a live stream during ordinary readline pages an existing wrapped
  draft before creating its output surface, preserving two output rows and
  the complete editable buffer.
- The C chat example composes a libmdf incremental renderer with the generic
  session. The composer configures default palette and a two-column
  left margin, passes submitted prompts to Softline's themed quoted-prompt
  helper between libmdf output segments, and emits varied Markdown response
  text one source character per 20 ms while the user may type, queue, promote,
  and cancel.
- The Lua facade tests the generic session lifecycle and byte forwarding;
  the separate Lua chat example remains a plain queued-turn demonstration.
- Packaging and artifact checks prove that libsoftline, its Lua facade, and
  installed package metadata have no libmdf link/runtime dependency. Only
  example and integration-test targets link the pinned libmdf SDK.
