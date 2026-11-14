# softline Lua facade

The `softline` Lua module is a thin facade over the installed C library. It
links with `libsoftline` and uses only the public `softline/softline.h` API.
Lua 5.5 is the supported Lua runtime for this facade.

```lua
local softline = require("softline")

local sl = softline.new()
local line, status = sl:readline("softline> ")
if line then
  sl:history_add(line)
else
  if status == softline.READLINE_EOF then
    -- input ended
  end
end
sl:close()
```

## Handles

`softline.new([config])` returns an independent editor handle. The optional
config table mirrors `sl_config_t`. Unknown fields are rejected, including
removed boxed-layout options (`bounded`, `screen_x`, `screen_y`, and
`screen_height`). Invalid config names or values raise a Lua error. Supported
fields are:

- `input_fd`
- `output_fd`
- `screen_width`
- `live_scroll_region`
- `disable_image_paste` (default `false`; disables built-in Ctrl-V image capture)
- `image_paste_path_template` (default `nil`; XDG cache path)
- `clear_prompt_on_exit` (default `false`: clear input rows, keep queue and
  status rows, and return at column zero on the current input row; `true` clears
  the whole prompt area and returns below the transcript)
- `prompt_queue`
- `prompt_queue_max_entries`
- `prompt_queue_preview_entries`
- `queue_backend` (default `nil`; keeps the native local queue; see
  [queue backends](#constructor-selected-queue-backend))
- `queue_backend_context` (default `nil`; retained until backend detach)
- `osc8_mode` (default `softline.OSC8_OFF`; see [hyperlinks](#osc-8-hyperlinks))
- `prompt_theme`
- `statusline`
- `statusline_start_element`
- `status_spinner`
- `status_busy`
- `status_idle_marker`
- `history_max_len`
- `line_max_len` (complete draft limit in UTF-8 bytes; default 131072 / 128 KiB)

Each handle owns its buffer, cursor, history, prompt state, and diagnostics.
Call `sl:close()` when done; the Lua finalizer also closes an unclosed handle.
Queueing and live scroll regions affect only editing with both input and output
attached to TTYs; mixed or piped streams remain plain line readers. Defaults
match `sl_config_init()`: queueing,
live scroll regions, status lines, and spinners are off; the theme is
`PROMPT_THEME_DEFAULT`; and the idle status marker is `+`.

## Methods

- `sl:readline([prompt])` returns a submitted string, or `nil, status` for EOF,
  cancellation, interrupt, suspension, or error. Ctrl-C restores terminal state before
  raising SIGINT; if a handler returns, an existing native output session
  remains available.
  Ctrl-R searches history. Enter accepts the displayed match into the normal
  editable prompt without submitting or queueing it; a subsequent Enter follows
  the normal keymap. Escape or Ctrl-G restores the original draft.
- While terminal raw mode is owned, kernel tab expansion (`TAB3`/`OXTABS`) is
  disabled so the terminal handles tabs. OPOST/ONLCR and other output flags are
  preserved; original flags are restored on release.
- `sl:next_prompt([prompt])` returns `line, source, id`. The ID is zero for a
  direct draft without queued origin. With the native local queue, automatic
  delivery
  dispatches FIFO entries before opening an editor; manual delivery retains
  them for explicit take or Alt-Enter promotion. `source` is
  `PROMPT_SOURCE_QUEUED`, `PROMPT_SOURCE_DIRECT`, or
  `PROMPT_SOURCE_PROMOTED`. On interactive handles it retains terminal input
  ownership between results; `close()` restores the terminal. On no result it
  returns `nil, status`. With a configured queue backend it keeps editing across
  submissions; accepted backend work never returns as local model input.
- `sl:history_add(line)` adds one history entry and synchronously calls the
  attached append hook; empty and consecutive duplicate entries are ignored.
- `sl:history_set_max_len(max_len)` changes the retained history cap; `0`
  clears and disables history.
- `sl:history_save(filename)` writes history with owner-only permissions.
- `sl:history_load(filename)` imports history entries, bypassing append hooks.
- `sl:history_set_backend(key, hooks)` loads and attaches custom `load`/`append`
  callbacks; see [persistent history](#persistent-history).
- `sl:history_open(key, directory)` loads and attaches native keyed storage;
  omit `directory` for the XDG state default.
- `sl:history_close()` detaches storage without clearing recall or saving on exit.
- `sl:history_compact()` atomically retains the newest capped entries from the
  native shared file under its stable lock; unavailable for custom backends.
- `sl:history_set_auto_add(enabled)` opts into immediate recording at editor
  and queue acceptance. Disabled by default; delivery, promotion, Ctrl-R
  selection, cancellation and rejected drafts do not append.
- `sl:history_search_active()` returns a boolean indicating reverse-search
  mode, including a failed match. It is false after selection, cancellation,
  or editor exit. The query is read-only and runs on the editor-owner thread.
  Key callbacks run before built-in handling; submission guards should return
  `softline.KEY_ACTION_PASS` during search so Enter selects an editable match.
- `sl:set_screen_width(width)` sets ordinary readline wrapping width; `0`
  returns to terminal-width probing. Native chat uses physical terminal width.
  Without an active readline, this hint does not move or resize native output.
- `sl:set_live_scroll_region(enabled)` opts an ordinary readline prompt into
  bottom-pinned scroll-region output after it reaches the terminal bottom.
  Ownership is acquired at readline startup and retried between producer
  callbacks as output advances the prompt; multiple feeds in one callback
  remain immediate and refresh ownership at the next input-owned boundary.
  It is disabled by default; native chat always uses its own scroll region.
- `sl:set_image_paste_path_template(template)` copies an override path; pass
  `nil` to restore the default. A template contains exactly one `*` for a
  20-character lowercase base32hex xid. Softline always appends `.png` or
  `.jpeg`: `/tmp/softline/*/image` yields
  `/tmp/softline/{xid}/image.png`. Directories are created recursively.
  Templates support `~/`, `{{HOME}}`, `{{home}}`, `{{XDG_CACHE_HOME}}`, and
  `{{xdg_cache_home}}`. The XDG token uses `XDG_CACHE_HOME` when absolute,
  otherwise `$HOME/.cache`. The default is
  `${XDG_CACHE_HOME}/softline/{xid}.png` or `.jpeg` with that fallback.
- `sl:set_idle_callback(callback)` registers a no-argument Lua callback that
  runs while an interactive editor is idle; pass `nil` to clear it. The
  callback may use methods such as `print_above`, `insert`, `submit`, or
  `cancel`. Closing that handle from its own idle callback is rejected. A Lua
  callback error ends the active `readline()` or `next_prompt()` with
  `READLINE_ERROR`; `sl:last_error()` returns the captured Lua error text.
- `sl:watch_add(fd_or_file, events, callback)` registers an integer descriptor
  or open Lua file handle for owner-thread wakeups while editing. The callback
  receives `id, fd, events`; it must drain bounded application work. Use
  `sl:watch_modify(id, events)`, `sl:watch_remove(id)`, or
  `sl:watch_clear()` to manage registrations. Watches require interactive TTY
  handles and callbacks may not recursively enter `readline` or `next_prompt`.
  Softline retains a supplied file handle until its watch is removed, cleared,
  or the Softline handle closes; do not close that file while registered.
  Ready watches are visited round-robin, with at most eight callbacks before
  terminal input is handled again.
- `sl:set_prompt_queue(enabled, max_entries, preview_entries)` enables the
  native local chat queue. The default profile maps Tab to queue a nonempty
  editor and
  Alt-E to edit the newest queued entry. Disabling the queue clears it;
  reducing `max_entries` below the current queue length fails.
- `sl:queue_count()` and `sl:queue_capacity()` return the queue state.
- `sl:inject_queued_prompt(text)` and `sl:inject_steer_prompt(text)` are the
  host injection calls, returning a stable entry ID. Both require an enabled
  queue and hold the message unless automatic approval is enabled. Approval
  preserves queued versus steering delivery; the host takes approved steering
  entries at a safe seam. Call on the editor owner thread, including from a
  watch callback.
- `sl:set_auto_approval(enabled)` takes a boolean for the entire conversation's
  Softline instance, initially false. Enabling approves every waiting entry
  (including edited entries and explicit queue holds) and future injections;
  disabling affects future approval requests without revoking approvals.
  The setting survives queue clear/disable. `sl:get_auto_approval()` reads it.
  Softline presents an inline message above the input only after the
  input buffer and pending terminal input are empty. Drafts, paste and history
  search defer new questions, leaving a passive pending count. An already
  displayed question remains visible when typing resumes; Q/S previews stay
  visible alongside it. Approval decisions follow FIFO arrival order through
  editing, requeueing and cancellation. Later held messages appear only in
  the count until the first is decided. Out-of-order `queue_approve` and
  `queue_reject` calls fail without changing the queue. Alt-A focuses
  approval with inverted foreground/background on the compact heading.
  The cursor stays at its draft editing position; only key routing changes.
  Only focused Alt-Y/Alt-N approve/reject (case-insensitive); Enter never approves.
  Escape returns to typing. Alt-E edits the full
  offered entry in either focus state; submission/requeue
  keeps its identity, hold, destination and approval priority. Ordinary typing
  returns keyboard focus to input and remains unheld; plain Y/N remain
  ordinary text in either focus state.
  The approval prompt uses `A `; normal text starts with two spaces.
  One heading combines count, controls and route. The message preview shows
  up to three wrapped lines, with `...` on the last when text is omitted.
  Editing and delivery retain the full message. Counts read `1 approval` or
  `N awaiting approval`.
  Focused panel actions and offered-entry Alt-E precede
  application bindings; other keys retain their usual binding behavior.
- `sl:queue_peek(index)`, `sl:queue_insert(index, text[, held])`,
  `sl:queue_append(text[, held])`, `sl:queue_replace(index, text)`, and
  `sl:queue_take(index)` inspect or mutate oldest-first queue entries. Lua
  indexes are one-based. `peek` returns text; `take` returns `text, id` and
  refuses held entries. Insert/append return a new opaque per-handle ID, never
  reused, with `held` defaulting to false. Automatic approval overrides explicit
  holds. Replacement preserves ID and hold.
  These synchronous mutation methods describe the native local queue; backend
  mode uses [typed requests](#constructor-selected-queue-backend) and rejects
  unsupported local mutations.
- `sl:queue_info(index)` returns `{id, mode, held, policy, stopped, eligible}`;
  `policy` contains boolean `enqueue`, `independent` and `executable` fields.
  `sl:queue_draft_info()`
  returns the same metadata for an Alt-E edit, or nil for an ordinary draft.
  `sl:last_prompt_id()` returns the last read/take ID, or zero for direct input,
  cancellation or failure. IDs remain integers across C and Lua (1..LONG_MAX).
- `sl:queue_approve(id)` releases a hold without changing delivery mode;
  `sl:queue_reject(id)` discards that entry. Both also operate on an active
  queued edit, where rejection clears only that draft. Held entries display
  `H` and are skipped during automatic delivery; ordinary user prompts proceed.
  Alt-E, edits, Tab/Enter requeue and busy steering preserve identity and hold.
  Submitting a nonempty held edit requeues it; it never implicitly approves or delivers.
  Submitting or requeueing an empty edit deletes its entry, including approvals.
  Alt-N discards the edited entry immediately without erasing or submitting it;
  the editing indicator shows `Alt-N discard`. Ctrl-_ recovers only its text as
  a fresh draft; its old identity and hold stay removed. On an ordinary draft,
  Alt-N clears the text with undo support. Alt-K saves and clears the whole draft
  in one per-handle stash; Alt-J appends that saved text after current input.
  The stash survives submissions and is independent of Ctrl-Y/Alt-Y's kill ring.
  Editing reserves one queue slot; cancellation/EOF/read failure restores the
  original entry. Clear preserves an active edit, and disable is refused until
  that edit is requeued, delivered, cancelled or explicitly rejected.
- `sl:queue_clear()` clears queued entries without changing the active draft;
  `sl:queue_draft()` atomically queues an active nonempty draft, or deletes the
  edited queue entry if its draft is empty.
- `sl:queue_mode(index)` returns `softline.QUEUE_MODE_QUEUED` or
  `softline.QUEUE_MODE_STEER`; `sl:queue_set_mode(index, mode)` changes an
  entry's delivery intent. The host chooses which steer to take at a safe seam.
- `sl:set_queue_delivery("auto" | "manual")` selects automatic FIFO delivery
  or host-controlled retention. Busy `queued_turns` retains entries in either
  mode; manual mode lets the host choose both steer and ordinary turns;
  `sl:queue_delivery()` returns that mode.
- `sl:set_queue_profile("default" | "queued_turns")` selects the built-in
  keymap and queue policy. `queued_turns` maps Enter to enqueue while
  `sl:set_status_busy(true)` is active, Alt-E to edit-newest, and Alt-Enter
  to queue a steer draft while busy or mark the newest queued entry as steer
  when the editor is empty.
  With automatic delivery, `sl:set_status_busy(false)` releases one oldest
  ordinary queued turn; manual delivery leaves it for the host to take.
  `sl:queue_profile()`
  returns the selected profile.
  While idle, Alt-Enter submits a nonempty draft immediately with
  `PROMPT_SOURCE_DIRECT`, or promotes the newest queued entry with
  `PROMPT_SOURCE_PROMOTED`.
  Cancellation retains queued drafts and stops automatic FIFO release until a
  direct submission or manual promotion resumes it.
- `sl:queue_keys()` returns a table with `enqueue_draft`, `edit_newest`, and
  `submit_or_promote_newest`; pass a table with those fields to
  `sl:set_queue_keys(keys)` to override built-in actions. Use `nil` for a
  disabled action. Explicit `sl:bind_key()` callbacks still take precedence.
- `sl:set_prompt_theme(theme)` selects one of `PROMPT_THEME_DEFAULT`, `PROMPT_THEME_PLAIN`,
  `PROMPT_THEME_ACCENT`, `PROMPT_THEME_DRACULA`, `PROMPT_THEME_GRUVBOX`,
  `PROMPT_THEME_MONOCHROME`, `PROMPT_THEME_MONOGREEN`, `PROMPT_THEME_OUTRUN`,
  `PROMPT_THEME_RICED`, or `PROMPT_THEME_SYNTHWAVE` for the whole interactive
  prompt UI, including status lines and queue panels.
- `sl:set_queue_policy(callback)` opts into native admission of deliberate
  interactive submissions. The short owner-thread callback receives
  `(text, mode, policy)` and returns a policy table, or nil to keep its initial
  values. Fields are booleans: `enqueue` (default false), `independent` (default
  false), `executable` (default true). Editing starts with the original policy.
  Only inspect state in this callback; do not mutate or recursively enter the
  editor/queue, close the handle, or block. A callback error or invalid policy
  preserves the draft and queue. Pass nil to remove the callback.
  Managed entries enter the visible native queue even when idle. Same-burst
  Alt-E precedes delivery, preserving identity, position and approval. History
  records admission once, without a second append on editing or delivery.
  Passive `sl:queue_draft()` and background injection never call the classifier.
- `sl:queue_set_executable(id, ready)` sets a boolean execution boundary for a
  queued entry or active edit. It never approves or resumes work. A readiness
  change from an owner-thread watch schedules native delivery; no synthetic
  input or polling is required. Managed entries use this readiness instead of
  the busy flag, including steering entries. Manual delivery still blocks
  automatic release. `queue_take` rejects entries whose `eligible` is false.
  Independent queries may pass blocked entries without modifying them; normal
  queued turns retain FIFO ordering. Steering may pass ordinary turns at a
  seam, but not older managed commands or its own execution hold. Legacy
  approval suggestions do not hold ordinary user prompts. This does not change FIFO
  approval decisions. Non-TTY input retains its existing direct behavior.
- `sl:queue_stopped()` exposes the backlog stop caused by cancellation.
  Fresh managed submissions can execute without releasing the old backlog;
  passive enqueues and edits inherit the stop. `sl:queue_resume()` explicitly
  resumes that backlog, preserving execution and approval holds and manual
  delivery. Setting automatic delivery does not clear the stop. Explicit
  Alt-Enter promotion resumes only the managed entry, preserving its execution
  hold and steering intent. Existing direct submission/promotion semantics
  remain unchanged when no policy is installed.
- `sl:set_statusline(enabled, starting_element)` enables the optional status
  line and chooses the palette slot for its first element. Themes provide
  eight element colours, and subsequent elements cycle through those colours.
- `sl:set_status_message(text)` sets or updates the status area above the
  status line. It displays `! ` in the theme's muted colour followed by italic
  text in the theme's secondary colour. Pass `nil` or `""` to clear it.
  Long text wraps at words, with continuation rows indented to the prefix
  width. The message may be changed while an output session is streaming.
- `sl:set_status_message_prefix(prefix)` changes `! `; pass `""` to hide the
  prefix or `nil` to restore it.
- `sl:set_status_message_colors(prefix_color, text_color)` selects independent
  theme roles, such as `softline.THEME_COLOR_MUTED` and
  `softline.THEME_COLOR_ELEMENT_2`.
- `sl:set_status_elements(elements)` replaces all status elements. At most 32
  are retained; longer input uses the first 31 followed by `...`. The limit is
  exported as `STATUS_MAX_ELEMENTS`. Elements must be valid UTF-8 and cannot
  contain C0/C1 controls or DEL.
- `sl:set_status_element(index, value)` updates one zero-based element; pass
  `nil` as `value` to clear it.
- `sl:set_status_busy(busy)` selects the red busy `x` or spinner marker. With
  the `queued_turns` profile and queueing enabled, it is also the native turn
  lifecycle signal: busy retains turns; idle releases one oldest ordinary turn
  only when automatic queue delivery is selected.
- `sl:set_status_spinner(enabled)` enables the 500ms `/ - \\ |` busy spinner.
- `sl:set_status_idle_marker(marker)` selects a one-byte printable ASCII green
  idle marker; it defaults to `+`. Pass `nil` to leave the reserved two-column
  idle slot blank.
- `sl:insert(text)` inserts text bytes at the active cursor.
- `sl:set_buffer(text)` replaces the active buffer and moves the cursor to the
  end.
  Both buffer mutations remain undoable while the editor is suspended; failed
  mutations leave the draft and its undo history intact.
- `sl:buffer()` returns the active buffer as a Lua string.
- `sl:cursor()` returns the cursor as a byte offset into `sl:buffer()`.
- `sl:set_cursor(offset)` clamps the byte offset to a valid UTF-8 cluster
  boundary when possible.
- `sl:submit()` submits the active buffer or reverse-search match from a callback,
  applying queue admission. Idle/watch submissions have ordinary queued intent.
- `sl:cancel()` cancels the active `readline()`.
- `sl:bind_key(key, callback)` binds a decoded key to a callback. The callback
  receives the key code and returns a `softline.KEY_ACTION_*` value, or `nil`
  to mark the key handled. Passing `nil` as the callback removes the binding.
  Binding `softline.KEY_ENTER` to return `softline.KEY_ACTION_SUBMIT` opts into
  immediate submission of a reverse-search match instead of accepting it for
  editing.
  `softline.KEY_CTRL_L` resets prompt layout from the PTY size and repaints
  owned UI rows while preserving editor and producer state and the transcript.
  Return `softline.KEY_ACTION_RESET` from another key binding to remap recovery;
  a Ctrl-L binding overrides the built-in behavior normally.
  Ctrl-A/E move to the start/end of the actual newline-delimited line. Ctrl-U/K
  kill to its start/end; at the boundary they remove only the adjacent newline.
  Ctrl-W kills a word within that actual line or the preceding newline at line
  start. Soft wraps do not create these boundaries.
  Ctrl-Y yanks killed draft text and Alt-Y rotates that yank; focused approval
  controls retain priority over Alt-Y. Ctrl-_ or Ctrl-X Ctrl-U undoes draft
  edits, Ctrl-T transposes Unicode clusters, and Alt-D/Alt-Backspace kill words.
  Undo is scoped to the current draft; the ten-entry kill ring persists across
  prompts on the same instance. These keys can be rebound like other keys.
  The default `softline.KEY_CTRL_V` action saves a PNG or JPEG from the Linux
  X11 clipboard and inserts its persistent cache path; a binding overrides
  that action. It uses `DISPLAY`, including one forwarded by `ssh -Y`, and a
  missing image leaves the buffer unchanged with a `last_error()` diagnostic.
  Linux static and shared libraries privately bundle MIT-licensed c-ares for
  literal IPv4/IPv6, `/etc/hosts`, then `/etc/resolv.conf` DNS (including search
  domains). Lookup shares the 10-second paste deadline without libc NSS modules
  or an additional link library.
- `sl:print_above(source)` prints above the active prompt. Ordinary prompts clear
  and redraw by default, or use an
  enabled live scroll region after reaching the terminal bottom. `source` may
  be a string, an array-like table of string chunks, or a function that
  receives a 1-based chunk index and returns the next string or `nil`.
  Native chat reconciles physical resize before each chunk, including resize
  while a source function is running.
  Failed native finite output discards partial ANSI/UTF-8 bytes so the next
  call starts cleanly.
- `sl:output_stream_reconcile()` observes a settled resize before synchronous
  producer continuation, outside feeds on the owner thread. It preserves draft,
  cursor, queue, history, status, queued input, terminal modes and split stream
  parsing while repainting only owned prompt cells. A required cursor reply
  waits up to 100 ms; failure returns `nil, error`. An open stream is required;
  redirected sessions are a no-op. Check success before resuming output.
- `sl:output_stream_begin()` opens one persistent output session above the
  prompt. Transcript output starts at the existing terminal cursor; the editable
  prompt starts at the bottom. Physical resize preserves the terminal's native
  prompt position; only prompt layout changes update its cells. Producer bytes
  pass through unchanged. Before the first frame, output uses the full terminal and
  leaves its cursor in place between writes, preserving native autowrap and
  Unicode clusters. Cursor reports occur at ownership and resize boundaries.
  Feed calls have no intentional delay and never request or wait for cursor
  replies, including during resize. Local scalar cell widths, shared with prompt
  layout, track cursor advances. Terminal Unicode versions and cluster shaping
  can differ from the Unicode 16.0 accounting.
  The delta fallback cannot infer earlier hard-ended ASCII reflow or unknown
  terminal history if a retained prompt resizes without an input-owned observation,
  including after readline returns. The active input loop observes resize
  before dispatching producer watches. This delta fallback is deliberately
  immediate; see the stream design's unobserved retained-frame resize exception.
  No producer transcript, glyph spans or grapheme tails are buffered. Physical
  resize can race an in-flight VT batch and misplace even ASCII output. This accepted
  concurrency limit has an unpaused VTE diagnostic (`--streaming-race`);
  there is no transcript replay or feed wait to conceal it. Reflow
  belongs to the terminal. With an active prompt, a known ASCII right-edge
  continuation advances a hard row after the cursor handoff. That row may not
  rejoin on width growth, and a split Unicode cluster can lose its attachment.
  At the bottom output margin, VTE and tmux can reflow that boundary
  differently on width growth; exact continuation there is not guaranteed.
  Unfinished Unicode/tab lines have no reflow model. Width resize retains the
  tracked column, clamping it only if offscreen; continuation can land in a gap
  or overwrite text, including CR tails. A last-column report cannot distinguish
  Unicode pending wrap from a cursor before the last cell. These are intentional
  opaque-feed limits; Softline never reconstructs or replays the transcript.
  The output margin follows the actual prompt height, including visible queue
  entries, nonempty status messages, status lines, and editor rows. Unused
  preview slots occupy no space. At physical capacity the editor pages, leaving
  at least two output rows. Native chat needs at least three terminal rows, uses physical
  terminal width, and supports no rectangular viewport.
  An unfinished line clipped into scrollback continues at the first visible
  output row without replay. If the clipped position was at a hard line
  boundary, new output starts next to the prompt. Existing scrollback cells
  and spacing are preserved. Rapid tmux resizes may update its visible grid
  before delivering the matching PTY size; output during that interval can be
  corrupted. The native stream does not replay transcript bytes to repair it.
  The application owns its renderer, wakeup, and
  response/document lifecycle; Softline has no Markdown dependency.
- `sl:output_stream_write(bytes)` sends a Lua byte string immediately into the
  open session. Calls from a watch callback can alternate with prompt typing.
  Chunk boundaries add no newline or response separator. The stream accepts
  printable UTF-8, LF/CR/Tab, ANSI SGR styling, and opt-in OSC 8 hyperlinks.
  OSC 8 is off by default; `sl:get_osc8_info().enabled` reports the effective
  feed policy. Other OSC controls and malformed input return `nil, status`.
  An empty string succeeds without
  changing the screen. No full response is buffered. Native LF positioning
  follows the output TTY's OPOST/ONLCR settings without changing those settings
  or producer bytes. While Softline owns raw mode, kernel tab expansion
  (`TAB3`/`OXTABS`, such as `stty -tabs`) is temporarily disabled so the terminal
  receives tabs unchanged. Original flags are restored when raw mode is
  released, including when input and output use separate TTYs.
  Native writes after that release disable expansion for the write and restore
  it before returning, without flushing pending input.
- `sl:output_stream_write_quoted_prompt(text)` writes submitted text between
  renderer segments as a wrapped, themed quote, repeating the prefix on every
  visible row and keeping one empty row on each side. The text is literal, so
  Markdown punctuation remains visible. `sl:set_quoted_prompt_prefix(prefix)`
  changes the default `> ` prefix; `nil` restores it.
  `sl:set_quoted_prompt_style({prefix={r,g,b}, text={r,g,b}})` overrides the
  theme colours; `nil` restores theme defaults. The prefix stays faded and the
  text stays italic.
- `sl:output_stream_end()` ends the producer session without finishing an
  external renderer document. While an editor is active it retains the
  prompt and scroll region for later finite output or another stream. Otherwise
  native chat closes using `clear_prompt_on_exit`: by default it clears only
  input rows, keeps queue/status rows, and leaves the cursor at column zero on
  the current input row without a newline or scroll. Setting
  `clear_prompt_on_exit = true` clears the whole prompt area and returns below
  the transcript. With no rendered prompt it returns below output.
  Closing an idle native session also releases terminal
  input ownership. Reopening a closed native session probes the cursor again.
  A nonzero column restarts quote separation for an unfinished externally
  written line; otherwise known spacing remains. A caller may run a foreground
  child between editor calls, then call
  `output_stream_begin()` to probe its final cursor and build a fresh prompt
  without replacing the handle or its history/settings. Do this outside
  editor callbacks; child execution belongs to the application. Non-TTY output
  adds no teardown controls. If the last write left an ANSI or UTF-8 sequence
  incomplete, it returns `nil, softline.ERROR_INVALID` and keeps the session
  open so the missing bytes can be supplied. Only one session may be open per
  editor. Output-session and quoted-prompt methods run on the Lua/editor owner
  thread; foreign producers should notify a watched descriptor instead of
  calling Lua.
- Physical resize preserves an unchanged prompt and cursor at their native
  terminal positions. Changed prompt layout updates only owned cells.
  The application updates its external renderer width separately. Softline
  does not cache or repaint the transcript.
- `sl:last_readline_status()` returns the last readline status code.
- `sl:last_error()` returns the last handle-owned diagnostic string, or `nil`.
- `sl:close()` destroys the handle and restores terminal state. Native chat
  cleanup uses the same `clear_prompt_on_exit` policy as `output_stream_end()`.

Fallible setter and operation methods return `true` on success or
`nil, status` on failure; getters and prompt reads return their documented
values. The module exports the C status, prompt-source,
queue-mode, theme, theme-colour, watch-event, and key constants. Queue delivery
and profile use the documented Lua strings instead of C enum integers.

`KEY_NONE` disables a configurable queue action. For Alt-letter bindings
without a named constant, add the letter byte to `softline.KEY_ALT_BASE`.

- `softline.READLINE_NONE`
- `softline.READLINE_SUBMITTED`
- `softline.READLINE_EOF`
- `softline.READLINE_CANCELLED`
- `softline.READLINE_INTERRUPTED`
- `softline.READLINE_ERROR`
- `softline.READLINE_SUSPENDED`
- `softline.OK`
- `softline.ERROR`
- `softline.ERROR_INVALID`
- `softline.ERROR_NOMEM`
- `softline.ERROR_IO`
- `softline.PROMPT_SOURCE_NONE`
- `softline.PROMPT_SOURCE_DIRECT`
- `softline.PROMPT_SOURCE_QUEUED`
- `softline.PROMPT_SOURCE_PROMOTED`
- `softline.QUEUE_MODE_QUEUED`
- `softline.QUEUE_MODE_STEER`
- `softline.ERROR_FULL`
- `softline.WATCH_READ`
- `softline.WATCH_WRITE`
- `softline.WATCH_ERROR`
- `softline.WATCH_HANGUP`
- `softline.PROMPT_THEME_PLAIN`
- `softline.PROMPT_THEME_ACCENT`
- `softline.PROMPT_THEME_DRACULA`
- `softline.PROMPT_THEME_GRUVBOX`
- `softline.PROMPT_THEME_MONOCHROME`
- `softline.PROMPT_THEME_MONOGREEN`
- `softline.PROMPT_THEME_OUTRUN`
- `softline.PROMPT_THEME_RICED`
- `softline.PROMPT_THEME_SYNTHWAVE`
- `softline.PROMPT_THEME_DEFAULT`
- `softline.THEME_COLOR_MUTED`
- `softline.THEME_COLOR_SECONDARY`
- `softline.THEME_COLOR_PROMPT`
- `softline.THEME_COLOR_QUEUE`
- `softline.THEME_COLOR_INPUT`
- `softline.THEME_COLOR_ELEMENT_0`
- `softline.THEME_COLOR_ELEMENT_1`
- `softline.THEME_COLOR_ELEMENT_2`
- `softline.THEME_COLOR_ELEMENT_3`
- `softline.THEME_COLOR_ELEMENT_4`
- `softline.THEME_COLOR_ELEMENT_5`
- `softline.THEME_COLOR_ELEMENT_6`
- `softline.THEME_COLOR_ELEMENT_7`
- `softline.STATUS_MAX_ELEMENTS`
- `softline.KEY_NONE`
- `softline.KEY_CTRL_A`
- `softline.KEY_CTRL_B`
- `softline.KEY_CTRL_C`
- `softline.KEY_CTRL_D`
- `softline.KEY_CTRL_E`
- `softline.KEY_CTRL_F`
- `softline.KEY_CTRL_J`
- `softline.KEY_CTRL_K`
- `softline.KEY_CTRL_L`
- `softline.KEY_CTRL_R`
- `softline.KEY_CTRL_T`
- `softline.KEY_CTRL_U`
- `softline.KEY_CTRL_V`
- `softline.KEY_CTRL_W`
- `softline.KEY_CTRL_X`
- `softline.KEY_CTRL_Y`
- `softline.KEY_CTRL_UNDERSCORE`
- `softline.KEY_ESCAPE`
- `softline.KEY_BACKSPACE`
- `softline.KEY_TAB`
- `softline.KEY_ENTER`
- `softline.KEY_CTRL_ENTER`
- `softline.KEY_ALT_ENTER`
- `softline.KEY_CTRL_N`
- `softline.KEY_CTRL_P`
- `softline.KEY_UP`
- `softline.KEY_DOWN`
- `softline.KEY_LEFT`
- `softline.KEY_RIGHT`
- `softline.KEY_HOME`
- `softline.KEY_END`
- `softline.KEY_DELETE`
- `softline.KEY_ALT_B`
- `softline.KEY_ALT_F`
- `softline.KEY_ALT_D`
- `softline.KEY_ALT_BACKSPACE`
- `softline.KEY_ALT_Y`
- `softline.KEY_UNKNOWN`
- `softline.KEY_PASTE_BEGIN`
- `softline.KEY_PASTE_END`
- `softline.KEY_F1`
- `softline.KEY_F2`
- `softline.KEY_F3`
- `softline.KEY_F4`
- `softline.KEY_F5`
- `softline.KEY_F6`
- `softline.KEY_F7`
- `softline.KEY_F8`
- `softline.KEY_F9`
- `softline.KEY_F10`
- `softline.KEY_ALT_A`
- `softline.KEY_ALT_E`
- `softline.KEY_PAGE_UP`
- `softline.KEY_PAGE_DOWN`
- `softline.KEY_ALT_BASE`
- `softline.KEY_ALT_M`
- `softline.KEY_ACTION_PASS`
- `softline.KEY_ACTION_HANDLED`
- `softline.KEY_ACTION_SUBMIT`
- `softline.KEY_ACTION_CANCEL`
- `softline.KEY_ACTION_INTERRUPT`
- `softline.KEY_ACTION_RESET`
- `softline.KEY_ACTION_SUSPEND`

## Examples

The repository ships Lua examples for simple prompts and queued chat turns.
The Lua chat example demonstrates queue delivery without libmdf.

The exact commands `!sh` and `/shell` open `$SHELL -i` (`/bin/sh -i` when
unset or empty), only between turns on an interactive terminal. Enter or idle
Alt-Enter launches the shell. Busy shell submissions remain queued and editable
until the turn ends; steering cannot bypass that execution hold. Arguments are
not interpreted as shell commands. The example uses the local PTY adapter,
ending its output session and beginning a new session on return. Clear and
resize inside the shell are allowed; history, queue and settings survive, and
the transcript is never replayed. Failed launches and nonzero exits report an
error and return to chat.

Alt-S uses that same shell handoff between turns while keeping the draft,
cursor, undo, kill ring, history search and queue-edit/approval state. The
command forms still return to an empty prompt. A key callback may return
`softline.KEY_ACTION_SUSPEND`: `sl:readline()` or `sl:next_prompt()` returns
`nil, softline.READLINE_SUSPENDED` without submission, history append or
cancellation. The next editor call resumes that draft before automatic queue
delivery. Run the child in the owner loop after `sl:output_stream_end()`, then
call `sl:output_stream_begin()` and resume editing at the current geometry.
Suspension does not pause producers; the application must ensure exclusive
terminal ownership. The example rejects Alt-S while an operation runs.

```sh
make lua-test
softline_lua_env="$(make lua-env)" && eval "${softline_lua_env}"
lua examples/simple.lua
lua examples/chat.lua
```

The facade loads the installed public shared `libsoftline`; a static-only SDK
is rejected. Local SDK tests install from the common Debug library producer.

`examples/chat.lua` accepts the same `SOFTLINE_PROMPT_THEME` values as the C
chat example. It uses the default native persistent output session: transcript
output starts at the original cursor, while the prompt starts at the bottom
and follows the terminal's native resize position.
Output feeds preserve unchanged prompt cells; prompt updates patch only cells
that differ from the previous frame.

To run against the in-tree debug `libsoftline` instead of the installed local
SDK:

```sh
make lua-debug-test
make lua-debug-simple
make lua-debug-chat
```

## Persistent history

```lua
local sl = softline.new()
assert(sl:history_open("my-app:project-id"))
assert(sl:history_set_auto_add(true))
```

The key is a nonempty string copied by Softline. Native storage uses
`$XDG_STATE_HOME/softline/history/<sha256-of-key>.history`, falling back to
`$HOME/.local/state` when the XDG value is absent, empty or relative. An absolute
`directory` overrides the complete directory. Parents are recursively created
with mode 0700; the store directory and regular, user-owned files are private
(files and lock files 0600; file symlinks/hard links are rejected).

One physical line holds one escaped prompt: `\\`, `\n`, `\r`, `\t` represent
backslash, LF (including Ctrl-J), CR and Tab. Unicode remains UTF-8. Entries
are appended before the accepting call returns, without exit-time saves or
per-entry fsync. Same-key processes and independent handles use a stable flock lock. Loading ignores a torn
final record; appending removes it first. Recall is bounded by `history_max_len`;
the file grows until explicit compaction. Zero cap disables recording. Same-key
handles share storage, but refresh recall only when they load it.

Custom storage uses the same acceptance boundary:

```lua
assert(sl:history_set_backend("my-app:project-id", {
  load = function(key, emit)
    -- Iterate your storage oldest first; emit entries individually.
    for prompt in storage:prompts(key) do
      assert(emit(prompt))
    end
    return true
  end,
  append = function(key, prompt)
    return storage:append(key, prompt) -- true or nil, negative status
  end,
}))
```

`load` is optional; `append` is required. Hooks return `true` or `nil, status`,
where status is a negative Softline error. Exceptions return `nil, status,
message` to the caller. Hooks run synchronously in the coroutine calling the
history/editor method, even if another coroutine created the handle. They
cannot yield across the C call. Automatic recording failures return `nil,
softline.READLINE_ERROR` from readline/next_prompt, with an additional message
for a Lua callback exception. Exception messages belong to the current hook
invocation; a later status-only failure does not repeat an earlier handled
exception. The emitter works only during load; retaining and
calling it later raises an error. Imports never call append. Attachment failure
preserves the previous hooks and recall; append failure preserves recall.
External storage rollback is the backend's responsibility. Do not reenter
history operations or close the handle from a hook. Callback closures are owned
by the handle and can be garbage-collected even when they capture that handle.
An explicit `queue_draft()` append failure returns `nil, status` (and the
callback exception message when present), retaining the editable draft without
admitting a queue entry. Once an explicit history call returns an exception
message, that message is consumed: a later cancellation, EOF, or unrelated
editor error does not return it again. An unhandled automatic recording
exception still supplies the message for its `READLINE_ERROR` result.

Manual `history_add()` remains the default recording policy. With auto-add,
programmatic queue insertion/replacement still requires an explicit add;
applications must not add delivered queued entries again. The explicit
`history_save(filename)` snapshot API does not participate in native locking or
compaction and should not rewrite an active shared native file.

The C and Lua chat examples attach `softline.examples.chat` at startup and enable
auto-add for accepted prompts/commands, including queued and steered drafts.
All themes use the same key. `SOFTLINE_HISTORY_DIR` overrides their directory.

## Constructor-selected queue backend

`sl:suspend()` lets an owner-thread idle/watch callback request the same
draft-preserving terminal handoff as `KEY_ACTION_SUSPEND`. It returns `nil, status`
outside active readline and does not stop backend work. Run the foreground
child after readline returns `READLINE_SUSPENDED`, then resume the editor.

Ordinary constructors retain the native local queue. `queue_backend` instead
selects an application-owned asynchronous queue for the entire handle lifetime;
`prompt_queue` must be enabled. `queue_backend_context` is any Lua value retained
until teardown and passed to the copied request/detach callbacks.

```lua
local outgoing = {}
local sl = softline.new {
  prompt_queue = true,
  queue_backend_context = outgoing,
  queue_backend = {
    capabilities = (1 << (softline.QUEUE_RESOLVE + 1)) - 1,
    max_pending_requests = 16,
    request = function(request, context)
      if #context == 16 then return false end
      context[#context + 1] = request
      -- Signal the adapter's watched descriptor here; never wait for a reply.
      return true
    end,
    detach = function(context)
      -- Cancel/unsubscribe observations before releasing the context.
      -- Accepted backend work remains owned by the backend.
    end,
  },
}
assert(sl:queue_backend_update {
  kind = softline.QUEUE_SNAPSHOT, revision = "initial", entries = {},
  total_count = 0,
})
```

The adapter drains its pollable descriptor through `watch_add()` and applies
results/updates on the editor-owner thread. Request callbacks return `true` only
for handoff; backend acceptance arrives later. They cannot reenter Softline.
All copied text is bounded by `line_max_len`, backend IDs/revisions/tokens by
127 bytes, and snapshots by `prompt_queue_max_entries`. No network or storage
implementation is imposed by Softline. The C constructor uses equivalent
callbacks and a borrowed context; rebuild C/Lua consumers for shared ABI 5.

- `sl:queue_backend_request(request)` returns a per-handle request ID, or
  `nil, status`. Request fields are `operation`, `item_id`, `item_revision`,
  `queue_revision`, `edit_token`, `text`, `mode`, `enabled` and
  `target_request_id`. Omitted expected revisions use the current mirror.
  Capability bits are `1 << operation`.
- `sl:queue_backend_result(result)` accepts `request_id`, `status`, optional
  `item_id`, `item_revision`, `edit_token` and `reason`. Successful item operations
  report item identity/revision. A successful begin-edit also reports its token.
  Typed begin-edit requests retire on acceptance without opening readline,
  allowing headless hosts to end/abort edits with a one-request retention limit.
  The owned hold must also appear in the ordered view before end/abort handoff.
  Native edit intent can remain retained until its editor opens. Results never
  recreate accepted mirror entries or synchronously call the outbound callback.
- `sl:queue_backend_update(change)` applies `kind`, `previous_revision`,
  `revision`, `entries`, one-based `index`, `item_id`, causing `request_id`,
  authoritative `total_count`, `stopped` and `auto_approval`. Entry fields are
  `id`, `revision`, `text`, `mode`, `held`, `edit_held`, holder-only `edit_token`,
  `ready`, `stopped` and `action`. Updates must follow one ordered subscription.
  A gap requires an explicit snapshot; inbound calls never invoke the outbound
  request callback synchronously. The next native dispatch requests resync.
- `sl:queue_backend_info()` returns backend enablement, synchronization,
  queue revision, visible/total counts and retained request count.
- `sl:queue_backend_entry(index)` returns a Lua table copied from the native
  view, including opaque backend identity and revision.
- `sl:queue_backend_pending(index)` returns the retained request fields plus
  `pending`, last `status` and optional `reason`, oldest first.
- `sl:queue_backend_recover(request_id)` restores definitely rejected proposal
  text into an empty ordinary draft. Alt-E also recovers such proposals. Unknown
  outcomes require resolution using their original ID; they never auto-flush.

Operations are `softline.QUEUE_OFFER`, `softline.QUEUE_BEGIN_EDIT`,
`softline.QUEUE_END_EDIT`, `softline.QUEUE_ABORT_EDIT`, `softline.QUEUE_APPROVE`,
`softline.QUEUE_WITHDRAW`, `softline.QUEUE_SET_INTENT`, `softline.QUEUE_STOP`,
`softline.QUEUE_RESUME`, `softline.QUEUE_CLEAR`, `softline.QUEUE_AUTO_APPROVAL`,
`softline.QUEUE_SYNCHRONIZE` and `softline.QUEUE_RESOLVE`.
Response statuses are `softline.QUEUE_ACCEPTED`, `softline.QUEUE_REJECTED` and
`softline.QUEUE_UNKNOWN`. Changes are `softline.QUEUE_SNAPSHOT`,
`softline.QUEUE_UPSERT`, `softline.QUEUE_REMOVE`, `softline.QUEUE_STATE` and
`softline.QUEUE_DISCONNECTED`. Recoverable API errors include
`softline.ERROR_UNSUPPORTED`, `softline.ERROR_CONFLICT` and
`softline.ERROR_NOT_READY`.

Enter/queue keys hand off offers and keep the native editor/watch loop active.
Alt-E opens a queued edit only after its hold is acknowledged. Enter requeues
under that same identity; empty submission/Alt-N requests withdrawal. Approval
markers change only with backend observations. Backend entries cannot be taken
as local model input. Legacy synchronous mutation methods report unsupported in
backend mode; use typed requests for clear, auto-approval, policy and intent.
Profile/key selection remains local presentation. Backend input editing requires
a TTY; noninteractive hosts can use the typed request API directly.
After reconnect, an accepted native acquisition with a missing or mismatched
owned hold requests one fresh snapshot. A changed/absent hold in that view
retires the intent without recreating work. A newer draft typed while disconnected
remains editable/submittable; the recovered edit opens when the editor is empty.

`max_pending_requests` bounds ordinary retained requests; three additional slots
reserve one synchronization, resolution and STOP request each. Native
cancellation/interrupt sends STOP before ABORT_EDIT, even at an ordinary
one-request limit. A pending STOP is not duplicated; on disconnect it remains
unknown and must be resolved by its original ID. Callbacks run on the
calling coroutine, including when the constructor's coroutine has been
collected. Callback/context references are owned by the receiver and ordinary
reference cycles remain collectible. Accepted offers enter auto-add history
once, in the coroutine applying the result, without replacing a newer draft.

## OSC 8 hyperlinks

The `osc8_mode` configuration field controls hyperlinks.
`softline.new({osc8_mode = softline.OSC8_AUTO})` opts into conservative passive
terminal detection. The default is `softline.OSC8_OFF`; `softline.OSC8_ON`
explicitly overrides detection. `sl:set_osc8_mode(mode)` updates policy between
streams. `sl:get_osc8_info()` returns `{mode, detected, enabled}`. Give `enabled`
to the host renderer's hyperlink option; `detected` is a cached environment
hint, not a terminal probe. AUTO remains off for unknown terminals, redirection
and unverified multiplexers. Enabled streams accept bounded OSC 8 controls
with BEL or ESC-backslash termination, including fragmented writes, while
other OSC commands remain rejected. `softline.OSC8_MAX_BYTES` bounds each
complete control, including its opener and terminator. Native links cannot
leak into editor UI.
See the [feed contract](../docs/softline-spec.md#osc-8-hyperlinks).

## Completion providers

`sl:set_completion_providers(registrations)` replaces trigger registrations;
nil or an empty table clears them. Each specifies `trigger`, optional `scope`
(default `softline.COMPLETION_START`), optional `strip_trigger` (default false)
and `complete(trigger)`, which returns an array of full candidate strings.
Optional `expand(completed)` supplies further candidates when Right/Ctrl-F
commits a selection, or when Tab resolves a completed candidate outside a cycle.
`softline.COMPLETION_WORD` also permits the trigger after ASCII whitespace.

```lua
assert(sl:set_completion_providers({
  {trigger = "/", complete = function()
    return {"/approve", "/status"}
  end, expand = function(completed)
    if completed == "/approve" then return {"/approve on", "/approve off"} end
    return {}
  end},
  {trigger = "@", scope = softline.COMPLETION_WORD, strip_trigger = true,
   complete = function() return {"@README.md", "@src/"} end,
   expand = function(completed)
     if completed == "@src/" then return {"@src/softline.c"} end
     return {}
   end},
}))
```

The initial `complete(trigger)` callback runs once per activation and returns
the host's current snapshot. It may read the editor but must not mutate/close
it or yield. Softline copies the strings, caches them through
typing/Backspace/Undo, and shows the first match as a dimmed inline ghost from
the first trigger character.

Tab accepts the ghost; repeated Tabs stay within the original matching list,
including a single candidate when `expand` is configured. Right/Ctrl-F accepts
a ghost at its token endpoint or
commits the current selection without moving the cursor. Commit resolves only
that candidate's continuation and previews its first match; it does not insert
the child or recursively enter further levels. Tab accepts the preview and
then cycles that new list. Within an existing word, Right/Ctrl-F moves by UTF-8
clusters; explicit key bindings take precedence over this default behavior.
An empty or failed continuation keeps competing parent ghosts suppressed.
Once a leaf is committed, Right/Ctrl-F resumes ordinary movement when text
follows. At the end of the draft, no-op forward keys preserve the commit and
draft text. Tab or actual editing can restart matching.

Suspension preserves the activation, Tab cycle, trigger and cursor through
terminal handoff. Resuming uses the same cached list without calling the
provider again; suspension does not strip the trigger.
Other edits or movement reset cycling while retaining the snapshot inside its
token. Typed, yanked or host-inserted whitespace keeps completion active when
it matches a candidate with a suffix remaining: typing `hello ` can still offer
`world`. If the phrase stops matching and the current word starts with another
eligible trigger, that word starts a new activation: `hello @` leaves the
`hello world` phrase and starts the `@` provider. A still-matching phrase retains
embedded trigger characters. Start-only providers remain ineligible after
whitespace. Whitespace without a matching suffix, deleting the trigger,
leaving the token, queueing/submitting or replacing the draft ends completion.
`strip_trigger` then removes the marker
as an undoable edit. Enter submits actual text, never a ghost. Initial provider
errors, invalid UTF-8/NUL/control-bearing candidates or callback exceptions
discard that list, preserve typed text including its marker and suppress
retries for that token.
No provider runs from a feed/render operation. Callback closures are owned by
the editor userdata and remain collectable even when they capture that editor.
The optional continuation receives the full completed string including its
trigger, even for a unique match. It must return candidates strictly extending
that string, or `{}` to leave ordinary completion/host Tab handling available.
It can also resolve a completed ancestor of a partially typed child path.
Each prefix is cached separately until activation ends; Backspace selects the parent list.
Continuations do not interrupt Tab cycling. Commit a parent again or type
further into its prefix to reuse cached children without another callback.
Continuation errors discard only their new list and preserve the draft and
earlier candidates. The examples use this for one-directory-at-a-time file
lookup and `/approve` subcommands, through the same callback functions.
See the [full contract](../docs/softline-spec.md#completion-providers).

## PTY terminal handoff

`softline.terminal_local(sl[, shell])` opens a fresh local PTY and invokes one
shell executable with `-i` (default `$SHELL`, falling back to `/bin/sh`). It uses
`sl_terminal_local()` through the same relay as remote adapters. Call between
editor calls after suspension, outside key/watch/idle callbacks. An existing
output session ends and restarts automatically. The result is `true, exit_status,
escaped`, or `nil, error_code`. The exit status is -1 while unavailable, otherwise
an exit code or 128+signal. `escaped` reports local escape with Enter then `~.`.
The handle owns eventual nonblocking reaping of cancelled local children.

`softline.terminal_handoff(sl, callback)` runs a host-selected adapter, returning
`true, escaped` or `nil, error_code`. A callback exception is raised only after
terminal restoration and the close callback. The function keeps the callback
rooted during handoff. Reentry, including `sl:close()`, is forbidden. Neither
function adds a receiver shell-control method; both use the shared adapter path.

If physical terminal output prevents session restoration, the function returns
`nil, softline.ERROR_IO` promptly with input modes released and the output
session closed. The draft and editing continuation remain intact. Reopen with
`sl:output_stream_begin()` once output is writable; no terminal drainage is
required before the failed handoff returns.

The callback takes one event table, borrows it for that invocation, and returns
a result table. `operation` is one of `softline.TERMINAL_OPEN`,
`softline.TERMINAL_STEP`, `softline.TERMINAL_READ`, `softline.TERMINAL_WRITE`,
`softline.TERMINAL_RESIZE`, or `softline.TERMINAL_CLOSE`. All callbacks return
promptly. Optional result `status` defaults to `softline.OK`; use
`softline.ERROR_NOT_READY` for read/write would-block, or another error code on
failure. The remaining fields follow the C `sl_terminal_event_t` contract:

- OPEN/RESIZE receive current `rows` and `columns`.
- STEP receives ready `events` bits. Return `ready` (boolean), `fd` (default -1),
  `interests` (WATCH_READ/WRITE bits, default 0), and `timeout_ms` (default -1).
  Service transport/control frames here; provide deadlines while opening.
- READ receives capacity in `length`. Return a binary `data` string no longer
  than that capacity; an empty string is EOF.
- WRITE receives a binary `data` string and `length`. Return `transferred`,
  allowing partial progress; zero progress requires ERROR_NOT_READY.
- CLOSE runs exactly once after OPEN, including failure. Cancel/release without
  waiting for a child or peer response. The host owns adapter resources.

The host owns transport, authentication and framing. PTY output goes directly
through the real terminal without interpretation, transcript buffering or replay.
The relay retains at most bounded chunks, forwards size changes, and restores
local terminal modes and the suspended draft/cursor/undo/queue holds on return.
Enter then `~.` works even while setup or transport I/O is stalled; `~~` at the
start of a line sends one literal tilde. Oversized pending input fails explicitly
instead of silently dropping bytes. See [the full contract](../docs/terminal-handoff-design.md).

## Streaming image paste

`image_paste_callback` (default nil) receives PNG/JPEG clipboard data instead of
saving an image locally. `image_paste_context` is any retained Lua value passed
as the first callback argument; the second is an event table. Callback/context
cycles are collectible. Callbacks run synchronously in the coroutine driving
readline, next_prompt or the explicit pump/cancel operation, through a protected
Lua frame. They must return promptly without yielding or reentering the editor.

`image_paste_max_bytes` defaults to 33,554,432 bytes (32 MiB) when zero. Positive
values select a per-image cap, at most INT_MAX. `image_paste_source_timeout_ms`,
`image_paste_host_timeout_ms` and `image_paste_total_timeout_ms` use zero for
10,000, 30,000 and 120,000 milliseconds respectively. Negative values are invalid.
See [the full image contract](../docs/image-paste.md) for editing, ownership,
resource bounds and source-vs-host deadline semantics.

Events use `softline.IMAGE_PASTE_BEGIN`, `softline.IMAGE_PASTE_DATA`,
`softline.IMAGE_PASTE_END`, `softline.IMAGE_PASTE_ABORT` and
`softline.IMAGE_PASTE_DONE`. DONE confirms successful path insertion; ABORT
confirms failure/cancellation. Their return values are ignored. Fields are `id`,
`operation`, `media_type`, `filename`, `offset`, `length`, `data` and `reason`.
DATA is a binary Lua string, including embedded NULs. Return true or softline.OK
only after consuming/copying the entire chunk. Return softline.ERROR_NOT_READY
only for DATA to pause without consuming it. Other returns or a Lua exception
abort the upload; the exception is available through `sl:last_error()`.

- `sl:image_paste_start()` returns a transfer ID or nil, status. Native Ctrl-V
  starts the same image-only handover. File clipboard lists are not uploads.
- `sl:image_paste_prepare()` returns `{fds={{fd=..., events=...}, ...},
  timeout_ms=...}` for an external owner loop. Descriptors belong to Softline.
- `sl:image_paste_pump()` advances bounded work without waiting. Native editing
  automatically prepares and pumps while continuing to handle terminal input.
- `sl:image_paste_resume(id)` permits retry of the same paused chunk.
- `sl:image_paste_complete(id, absolute_path)` acknowledges published backend
  storage after END. The path is inserted at the originating draft anchor on a
  subsequent safe dispatch, as one undoable edit. Use
  `sl:image_paste_complete(id, nil, reason)` for backend failure.
- `sl:image_paste_cancel(id)` aborts the transfer and preserves draft text.
  Ctrl-C does this while an upload is pending; ordinary interrupt behavior
  applies otherwise. Enter/Alt-Enter retain drafts with pending uploads.

The host owns upload status presentation. Image handover adds no row and never
changes status-message text or spinner state. Record lifecycle updates in the
callback, then use the normal status-message APIs from the owner loop after the
callback returns.

Mutation methods return true or nil, status. Stale IDs, duplicate notifications
and invalid ordering return softline.ERROR_INVALID. Metadata/paths are literal;
no completion, prompt submission, history append or server-file deletion occurs
as a consequence of progress or acknowledgement. Without a callback, standalone
image saving remains available. Rebuild C and Lua consumers for ABI 5.
