# Web frontend

`<softline/www.h>` supplies an opt-in receiver, embedded HTML/CSS/JavaScript,
and locally served JetBrains Mono. It has no HTTP listener, worker, terminal
dependency, transcript store, or external native dependency. It can be used on
a local host, a remote queue owner, or both. All calls run on the host's owner
thread. Initialize with `sl_www_config_init()`; `enabled` defaults to zero.

## Run the examples

| Target | Views | Web port |
| --- | --- | --- |
| `make run-chat-web` | Local ANSI and browser | 8080 |
| `make run-chat-web-only` | Local browser only | 8080 |
| `make run-chat-queue-server` | Shared remote conversation owner and browser | 8081 |
| `make run-chat-remote` | Remote ANSI client and browser | 8080 |

The printed `WEB` URL identifies the loopback listener. `WEB_PORT` and
`QUEUE_WEB_PORT` override the Make defaults. Direct binaries require
`SOFTLINE_WEB=1`; `SOFTLINE_WEB_PORT=0` selects an ephemeral port. The local
web targets and queue-server target run in `build/`, so completed images land
there by default. `SOFTLINE_CHAT_IMAGE_DIR` changes backend storage;
`SOFTLINE_CHAT_IMAGE_MAX_BYTES` changes the enforced default of 32 MiB.
Remote-client browser uploads go to the remote queue owner's image store.

Every third simulated response contains a randomized libmdf chart fence.
Horizontal bars, vertical bars and pies appear in shuffled groups, so nine
responses show all three kinds. ANSI and every attached browser receive the
same chart source and data. The plain-text Lua queue example remains separate.

`SOFTLINE_WEB_TITLE`, `SOFTLINE_WEB_WELCOME` (Markdown), and
`SOFTLINE_WEB_BANNER` (trusted host HTML/SVG in the welcome area) customize the
examples. `SOFTLINE_WEB_HEADER` optionally supplies trusted HTML, SVG or image
markup in the upper-left toolbar; that area is hidden and unbranded by default.
Set `SOFTLINE_WEB_ANIMATIONS=1` to try the animated page in any browser chat
example, for example `SOFTLINE_WEB_ANIMATIONS=1 make run-chat-web-only`.
`SOFTLINE_QUEUE_DISPLAY_DELAY_MS` defaults to 500 and affects only Q/S/pending
visibility. Admission, execution, acknowledgement, and FIFO have no delay.
New rows show immediately while the queue is already visible; approval rows,
connection errors, and host status remain immediate.

The mock listener in `examples/www_mock.c` is example/test support. It is not
installed, exported, or linked into libsoftline. It accepts eight loopback
connections, uses bounded buffers and no threads, and drops lagging browsers
so they can reconnect and replay without blocking the terminal or producer.
Open the listener through `127.0.0.1` or `localhost` using its configured port.

## Host integration

Mount the receiver at a route ending in `/`, such as `/chat/`. Redirect
`/chat` to `/chat/`. Strip that prefix before asset lookup. The public receiver
methods and matching free functions are equivalent:

```c
sl_www_config_t config;
sl_www_t *web;
sl_www_config_init(&config);
config.enabled = 1;
config.action = host_action;
config.binary = host_image_chunk;
config.replay = host_replay;
config.context = conversation;
web = sl_www_create(&config);
/* GET/HEAD /chat/<path>: web->asset(web, "/<path>", &asset).
 * Supply asset.content_type and asset.length; HEAD omits the bytes.
 * WebSocket /chat/events: use the server's public WebSocket handshake.
 * On open: web->connect(web, peer, last_sequence).
 * On text: web->message(web, peer, bytes, length, send_frame, peer).
 * On binary: web->binary(web, peer, bytes, length, send_frame, peer).
 * On close: retire peer replay work and cancel its unfinished image source.
 */
```

Use `sl_www_create_with_flags(&config, SL_WWW_ANIMATIONS)` to serve the animated
chat page. The handler adds the separate motion stylesheet and module to the
page it serves at `/`. The composer moves into place on the first successful
send or when opening a shell from the welcome screen, and user bubbles glide
and fade into place. The shell split opens from the bottom and the transcript
split opens from the right, or from the top when stacked on narrow screens.
The motion respects the browser’s reduced-motion preference. Sending and paced
typing keep their usual timing.

For Kore, use `http_response_header`/`http_response` for immutable assets and
`kore_websocket_handshake` with connect/message/disconnect callbacks for events.
The write callback sends one bounded JSON event through
`kore_websocket_send(..., WEBSOCKET_OP_TEXT, ...)`. Do not collect a whole model
response for an HTTP response. The host owns origin/authentication checks,
connection lifetime, routing, scheduling, backpressure, history, and replay.
Softline never calls Kore or assumes a particular server or HTTP version.
The browser Clipboard API needs HTTPS or loopback for the copy action.
Copy returns the original agent Markdown, including whitespace and fences.
The browser adds the action when it creates a response from a `begin` event
or receives its first `delta`. An `end` event finishes that message's decoder
and Markdown parser.
Incoming source chunks are retained alongside the rendered response; they are
joined only when Copy is clicked. Copy during generation returns the complete
UTF-8 prefix received so far. Other chat actions also work
over ordinary HTTP, using browser-generated random request identities.

Configuration strings are copied at construction. Custom asset descriptors,
paths, MIME types, bytes, and callback contexts are borrowed until destruction.
Set `config.header` to optional upper-left toolbar HTML, for example
`<a href="./"><img src="./logo.svg" alt="My project"></a>`. NULL, empty or
whitespace-only headers hide the area without reserving space. The existing
`config.banner` independently controls welcome-area branding. Toolbar graphics
fit within a 32px height and the available toolbar width.

Header and banner HTML are trusted host input; Markdown/model output is rendered as text
and safe DOM elements. Optional graphics are custom assets registered in
`config.assets`. Configuration text is bounded by the event encoder's 8192-byte
aggregate limit. Constructor failure returns NULL. The `images` configuration
hint is enabled only when a binary callback exists; the host enforces limits.

Action callbacks borrow decoded UTF-8 form fields for the call. Field names
are ASCII identifiers. Duplicate fields, malformed escapes, NUL, invalid UTF-8,
more than 16 fields, fields over 128 KiB, and oversized requests are rejected.
Binary callbacks receive at most 8192 bytes. Reentrant receiver calls are
rejected; enqueue host work if it would reenter the same receiver.

`sl_www_emit()` encodes **one bounded event**, calls the writer once, and keeps
no history. `SL_OK` consumes that frame; `SL_ERROR_NOT_READY` consumes none.
Retry the frame or disconnect/replay. Emit response deltas as bytes arrive;
never collect a complete response to simulate streaming. Each event permits
16 fields and 8192 aggregate value bytes. Values are UTF-8 JSON strings;
`type` and optional `seq` are reserved. Base64 permits a producer's byte chunks
to split UTF-8 characters while keeping each JSON frame valid.

To simulate one prompt in a browser, call
`receiver->simulate_prompt(receiver, writer, writer_context, text, options)`
or `sl_www_simulate_prompt()` with the writer for one live chat connection.
The shared `sl_prompt_simulation_options_t` specifies milliseconds before
typing, between visible characters, and before submission, plus queue or
steer intent. Zero typing delay inserts the full text in one operation.
The browser needs an unlocked composer. It clears whitespace-only drafts and
preserves drafts with content by declining the simulation. After the final
delay it uses the normal send action. Manual edits take over the draft.
Delivery is transient and a disconnect cancels unfinished automation.

For optional feedback, call `sl_www_simulate_prompt_tracked()` with a fresh
simulation ID and the selected peer's writer. The browser sends advisory
`simulation_status` actions to the existing `sl_www_action_t` callback.
Match each report against the host-owned `peer` and the simulation ID. The
writer's `SL_OK` confirms frame delivery to the writer. A missing browser
report leaves the outcome unknown. The browser drops reports if the socket
is congested or has changed, and the host decides whether to surface them.

| Status | Meaning |
| --- | --- |
| `started` | The browser accepted the complete request and scheduled its start. |
| `declined` | The request could not start. `reason` describes the condition, such as `draft_present` or `prompt_locked`. |
| `cancelled` | The browser stopped the request. `reason` can identify manual input, host cancellation, or a failed submit. |
| `submitted` | The browser sent the ordinary queue or steer action. `request` is that action's existing request ID. The normal `accepted` or `error` response describes admission. |

Call `sl_www_cancel_prompt_simulation()` with the same writer and ID to stop
a pending or active simulation. The browser leaves any inserted text in the
composer and ignores the command once the simulation has submitted. Use IDs of
1 to 64 ASCII letters, digits, underscores, or hyphens. Send each tracked
simulation and cancellation directly to the peer as transient events.

## Browser event contract

All JSON values below are strings. Replay cursors are opaque strings assigned
by the host's history owner. Bind them to the session epoch: the examples use
`epoch.sequence` and reset/replay from the beginning for foreign, missing or
expired cursors. A bare number never authorizes replay across host restarts.
Replies may omit `seq`.

| Event | Fields and behavior |
| --- | --- |
| `session` | `id`: host epoch. `disable_auto_resend=1` disables automatic retries on same-epoch reconnects. The field defaults to `0`. |
| `capabilities` | `edit=1` enables revision-checked queue edits. Optional `terminal` is a same-origin absolute path to a terminal page. Optional `terminal_unlock_command` is a single-line host command. During an automatic terminal lock, the `+` menu displays that text and submits it verbatim through the normal prompt handler, preserving the draft. |
| `reset` | Optional `text`; clear the retained transcript and queue before replay |
| `begin` | `id`, `role` (`user`, `assistant`, `note`) |
| `delta` | `id`, `role`, `text`, or `encoding=base64` plus `data`; append incrementally |
| `end` | `id`; finish the Markdown parser/UTF-8 decoder |
| `queue_begin`, `queue_end` | Atomic queue projection boundaries |
| `queue_item` | `id`, `revision`, `mode=queued/steer/pending`, `held=0/1` |
| `queue_text` | `id`, `encoding=base64`, `data`; bounded text chunks |
| `status` | Host-owned `text`, `busy=0/1`; a reserved single-line slot keeps layout stable, with long text ellipsized and available in its title |
| `prompt_state` | Host-owned `disabled=0/1`, optional tooltip `text`; dim/lock the composer while retaining the draft, scrolling and feed. Send current state during reconnect/replay; optional `view` targets one browser |
| `terminal_state` | Atomic host snapshot: `active` is the conversation-wide active/initializing handoff count, `disable_prompt=0/1` selects automatic prompt locking (default `0`). Count both TUI and browser sessions; publish on lifecycle or policy changes and replay current state |
| `completion_provider` | Host registration: one-character `trigger`, `scope=start/word`, `strip=0/1`, `expand=0/1`. Send the current registrations after `session` on each connection. |
| `complete` | `request`, raw full `candidate` including trigger; explicit `+` menu also uses canonical `value`, `label`, `description`, UTF-8 byte `start`/`end`, optional `continue=1` |
| `complete_end` | `request`, `status=ok/error`; completes one provider snapshot, including empty results |
| `simulate_begin`, `simulate_text`, `simulate_end` | Timing, text chunks, and commit boundary for a prompt simulation. Tracked requests carry the same `id` on all three events. |
| `simulate_cancel` | `id` identifies the pending or active tracked simulation to stop. |
| `accepted`, `error` | `request`, optional `text`; optional `view` targets one draft owner |
| `image_credit` | `id`; one more bounded binary chunk may be sent |
| `image_done` | `id`, absolute backend `path`; insert only into the originating draft |
| `image_error` | `id`, `text`; cancel the operation and keep the draft |

Set `disable_auto_resend=1` on the initial `session` event of each connection
when the host may lose request deduplication state without changing its epoch.
Send the setting together with `id` via `sl_www_emit()` so it applies before
the browser decides whether to retry. For example, the session event can be
`{"type":"session","id":"conversation-epoch","disable_auto_resend":"1"}`.

With this setting, queued prompts, steers, and queue edits awaiting a receipt
move to the `+` recovery menu as **Delivery unconfirmed.** The browser also
recovers unconfirmed text after an epoch change. Selecting a recovery entry
restores its text to the composer for inspection and deliberate submission.
The browser retains up to 32 recovery entries in the current tab. The host owns
receipt lookup and may send `accepted` or `error` with the original browser
request ID in the same epoch to resolve an unconfirmed entry. Acceptance removes
the entry, and rejection keeps the text available for recovery.

Browser text frames are URL-encoded actions:

- `send`/`steer`: `view`, unique `request`, monotonic `request_seq`, `text`.
- `simulation_status` carries `view`, simulation `id`, `status`, and optional
  `reason` or submission `request` as described above. Treat it as advisory and
  return `SL_OK` from the action callback to acknowledge handling it locally.
- `complete`: `request`, `text`, UTF-8 byte `cursor`, `all=0/1`. Inline
  completion supplies UTF-8 byte `start`/`end` and `resolve`: initially the
  registered trigger, then an explicitly requested continuation prefix. Return
  the entire inventory for that prefix followed by `complete_end`, even when
  current typed text matches only a subset. For the explicit `+` menu, answer
  `all=1` by enumerating candidates from the same registrations used by the TUI.
  A lazy provider can offer its root with `continue=1` and resolve children when
  selected, as the examples do for filename completion.
- `approve`/`reject`: `id`, `revision`; `cancel` stops current generation/backlog.
- `edit`: `view`, unique `request`, monotonic `request_seq`, `id`, `revision`,
  replacement `text`. Preserve identity, position, Q/S intent and approval hold;
  acknowledge only after the backend accepts the replacement.
- `image_begin`: unique `id`, `media_type=image/png` or `image/jpeg`, `filename`,
  exact `size`; wait for credit, send bounded binary chunks, then `image_end`.
  `image_cancel` carries the same `id` and is valid before BEGIN is delivered.

Hosts register all completion triggers and candidates, including slash commands
and filename completion. The chat examples register `/` and `@`.
Typing uses the same completion interaction as the TUI: a dim inline ghost
leaves the draft untouched. Tab accepts and cycles the original matching subset;
Right/Ctrl-F at the token endpoint accepts or commits the selected candidate,
resolves its children once, and previews without inserting a child. Enter,
the send button and Alt-Enter submit actual draft text, never a ghost. Ordinary
typing filters cached provider snapshots without another request. Tab resolves
known directory ancestors lazily for a partial path such as `@examples/ch`.
Only the explicit `+` button opens a menu, for completions, images and recovery.

The `+` menu also exposes queue and approval actions with shortcut hints:

| Action | Browser shortcut (Control on every platform) |
| --- | --- |
| Return focus to the prompt | Esc (outside other text fields, selectors and dialogs) |
| Open/close the terminal panel | Ctrl+` (also works inside the terminal) |
| Edit focused approval, otherwise newest queued entry | Ctrl+Shift+Enter |
| Focus the oldest pending approval | Ctrl+Shift+1 |
| Approve the focused request | Ctrl+Shift+2 |
| Reject the focused request | Ctrl+Shift+3 |

Queue and terminal shortcuts apply in the prompt and focused approval row and avoid Chrome/Chromium's
Alt menu bindings and tab-search shortcut.

The prompt focuses on startup, before configuration or connection completes,
and after sending. Returning to the window restores prompt focus only when
the page body has focus; theme/font controls, text fields and open dialogs
retain their focus. Esc elsewhere on the page closes the action menu and focuses
the prompt without changing the draft, selection or conversation scroll position,
and without cancelling generation. Inside the prompt, Esc keeps its existing
completion/cancellation behavior. The `+` menu also includes **Focus prompt**.

Keyboard approval decisions require explicit focus; Escape leaves that focus, decisions
clear it, and key repeat cannot approve or reject subsequent requests. Approval
rows have **Approve · Edit · Reject** buttons; later approval rows stay disabled
until the oldest is decided. Controls and the menu remain responsive while a
completion provider is pending. Ordinary textarea arrows, selections and native
modifier navigation remain browser-owned when completion does not handle them.

Queue edits open a separate form, preserving the view's unsent prompt. The
form uses Enter to save and Shift+Enter for a newline. Saving unchanged text
simply closes the form without a queue mutation. Local revisions follow entry
identity, so consuming an earlier row does not invalidate an untouched target.
The queue continues normally while the form is open. Saving compares the captured
revision; a changed/consumed entry rejects the save and retains the edited text
for inspection or copying. Remote saves acquire and release a backend edit token
during the save. Duplicate requests replay their outcome. Closing the form
before saving leaves the queue unchanged.
Host restart invalidates an open edit target, retaining its text for copying;
reused queue IDs cannot redirect that old edit into the new conversation.

The examples advertise queue editing. `make run-chat-queue-server` also enables
the [browser terminal](web-terminal.md) at `/terminal` on its existing web port
(8081 by default). Ctrl+` and the `+` menu open the locally served xterm.js view
in a bottom panel; closing it terminates its shell, and shell exit closes it.
The conversation and prompt stay above the panel. Drag the divider to adjust
the terminal height, or focus it and use the arrow keys. Opening the terminal
leaves chat and input enabled by default. Use `/terminal-lock` in the examples
to toggle whether every conversation web prompt locks during TUI or browser
handoffs. A `terminal_state` snapshot changes that policy at runtime.
The host owns lifecycle counts and propagation across its views.
Hosts can explicitly dim/lock the composer with a `prompt_state`
event (`disabled="1"`, optional tooltip `text`) and unlock with `disabled="0"`.
Explicit and automatic locks combine: ending the last handoff releases only
the automatic lock. The panel never overrides the server's shared state.
Each connection owns a new server PTY in the server's working directory;
chat and other terminal sessions remain independent. The other chat examples
leave this endpoint disabled. A host can advertise its own mounted terminal page
with `SOFTLINE_WEB_TERMINAL_URL=/terminal`. The host owns its authentication and
WebSocket transport; the library supplies assets and terminal-session APIs.

The C examples share one registration table between native and web editors
(`/`, `h`, `v`, `n`, `@`), including scope and trigger stripping. Exiting an
active filename completion removes only its introductory `@`; email addresses,
pasted literal text and at-signs in filenames remain intact. Stale replies cannot
reactivate a submitted draft or accept text after the user moves away.
Provider acquisition has a three-second timeout; a failed initial provider keeps
its trigger literal. Pending acceptance preserves subsequent editor input order
while conversation events and host actions remain responsive.

All tabs and ANSI share the accepted conversation and queue. Drafts live in
their originating views. Browser image sources use `sl_image_paste_start_source`
on separate image-only editor handles, feeding the same constructor callback,
timeouts, backpressure, completion insertion, and cancellation as native Ctrl-V.

After an upload is acknowledged, its temporary thumbnail disappears and the
plain backend path remains in the draft. Hover that path to show a floating
local image preview. Click the hover thumbnail, an uploading thumbnail, a posted
image path or a rendered conversation image to open the larger image viewer.
The viewer fits the image within the viewport with margins, preserves its aspect
ratio, and provides close and copy-image icons. Escape or a backdrop click closes
it and restores focus. Posted paths also support keyboard activation. Editing
and sending still use the original plain text. No backend file-serving endpoint is needed:
only images uploaded in this browser tab have previews, and a reload loses them.
The client retains at most 64 recently uploaded or previewed local blob URLs,
releasing evicted URLs and those retained when leaving the page. Reconnect/replay
preserves these local associations. Streamed messages match paths incrementally,
with only an unfinished known-path prefix retained between chunks.

Copy Image writes image pixels rather than the image path. PNG uploads use the
original file; JPEG and other rendered images are converted to PNG for the
clipboard. The upload stream and stored backend file are unchanged. The viewer
pins its local file until closed, independently of upload cancellation or preview
cache eviction. Browser clipboard permission is required; remote images must also
be readable under the browser's image/CORS rules. Copy failures leave the viewer
open with an error notice. See the [Clipboard API](https://developer.mozilla.org/en-US/docs/Web/API/Clipboard/write)
and [WebKit clipboard activation guidance](https://webkit.org/blog/10855/async-clipboard-api/).

Source ownership transfers only after successful adoption; close runs once.
The example callback copies at most one bounded protocol chunk; file writes
and publication run in the example host loop after the callback returns.
No file is staged at the frontend. The browser reads `File.stream()` and waits
for credit between 8192-byte chunks. Only a bounded image signature is read
separately. The mock host stores the completed image at the backend and returns
its absolute path. No browser draft changes the terminal's unfinished draft.

The example host retains a bounded replay log (16 MiB or 8192 events), a queue
projection of at most 80 entries (64 accepted plus 16 pending), 128 request
outcomes, and 128 view high-water marks. Per-view high-water marks prevent
expired outcomes from being admitted again. Retries require the original ID,
sequence, view, text, and intent. Remote browser acceptance waits for the actual
backend outcome, rather than treating socket handoff as admission. Rejected or
unknown prompts are recoverable through `+`; unknown outcomes need deliberate
inspection before resending. A restarted host gets a new epoch and does not
silently reoffer unconfirmed text. This is volatile example history, not durable
session storage. Production hosts implement their own replay policy.
Late join/reconnect replays conversation records and a fresh queue/status
projection; historical approval prompts and status changes are skipped.

## Rendering and verification

The vendored MIT `streaming-markdown` parser is pinned, with provenance and its
local text-node patch in `vendor/streaming-markdown/README.softline`. It appends
to the DOM incrementally rather than reparsing accumulated source. The adapter
limits unfinished constructs/nesting and uses literal text fallback. It renders
libmdf `mdf-bar-chart`/`mdf-horizontal-bar-chart`, `mdf-vertical-bar-chart`, and
`mdf-tile-chart` CSV fences. Tile charts become real SVG pies. Sorting, color,
percentage aliases, quoted CSV, zero totals, and invalid-data fallback are
supported. Chart state is bounded to 64 Ki characters/256 rows and drawings are
coalesced to animation frames. Completed text remains in the DOM; raw source
chunks remain available for Markdown copying. JetBrains Mono is vendored under
OFL 1.1.

Native tests cover handler validation, exact embedded assets, source-level
HTML/CSS/control contracts, custom-source lifecycle/backpressure/timeouts,
WebSocket framing, real local/remote/owner hosts, two live clients, independent
ANSI drafts, and backend image storage. Logical client regressions execute the
shipped JavaScript with DOM and transport test doubles:
`node tests/check-web-completion.mjs`. The shared `completion-parity.tsv`
inventory and key sequences also run through real native PTY readline in
`softline_completion_parity`. CTest registers the JavaScript check when Node is
already available; no JavaScript packages or runtime are installed. These tests
do not prove browser layout or real browser event integration.
`make run-web-checks` serves a browser Markdown/chart and client-interaction
check page using the mock listener. Use
`SOFTLINE_WEB_ANIMATIONS=1 make run-web-checks` to include animated split geometry
checks. Run that page and the following UX checks
after moving to a browser-capable host; no Chrome or browser libraries are
installed by this project:

1. Welcome and first-send composer position; narrow/mobile viewport and each
   theme/font, long messages/code, scroll-up preservation and jump-to-latest.
2. Two tabs plus ANSI: independent drafts, common replies/queue/approval, and
   immediate FIFO admission with delayed visibility only while initially empty.
3. Slash/`@`/`+` completion, keyboard selection, response copy, reconnect/replay,
   backend rejection recovery, and host restart with an unconfirmed prompt.
4. PNG/JPEG select/drop/paste, multi-image separators, typing during transfer,
   Escape/Ctrl-C during a pending file read or credit wait, immediate typing/send
   after cancellation, limits/timeouts, remote paths, and disconnect cleanup.
