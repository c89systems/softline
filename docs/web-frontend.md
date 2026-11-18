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

`SOFTLINE_WEB_TITLE`, `SOFTLINE_WEB_WELCOME` (Markdown), and
`SOFTLINE_WEB_BANNER` (trusted host HTML/SVG in the welcome area) customize the
examples. `SOFTLINE_WEB_HEADER` optionally supplies trusted HTML, SVG or image
markup in the upper-left toolbar; that area is hidden and unbranded by default.
`SOFTLINE_QUEUE_DISPLAY_DELAY_MS` defaults to 500 and affects only Q/S/pending
visibility. Admission, execution, acknowledgement, and FIFO have no delay.
New rows show immediately while the queue is already visible; approval rows,
connection errors, and host status remain immediate.

The mock listener in `examples/www_mock.c` is example/test support. It is not
installed, exported, or linked into libsoftline. It accepts eight loopback
connections, uses bounded buffers and no threads, and drops lagging browsers
so they can reconnect and replay without blocking the terminal or producer.

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

For Kore, use `http_response_header`/`http_response` for immutable assets and
`kore_websocket_handshake` with connect/message/disconnect callbacks for events.
The write callback sends one bounded JSON event through
`kore_websocket_send(..., WEBSOCKET_OP_TEXT, ...)`. Do not collect a whole model
response for an HTTP response. The host owns origin/authentication checks,
connection lifetime, routing, scheduling, backpressure, history, and replay.
Softline never calls Kore or assumes a particular server or HTTP version.
The browser Clipboard API needs HTTPS or loopback for the copy action.
Copy returns the original agent Markdown, including whitespace and fences.
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

## Browser event contract

All JSON values below are strings. Sequence numbers are opaque decimal strings
and must be assigned by the host's history owner. Replies may omit `seq`.

| Event | Fields and behavior |
| --- | --- |
| `session` | `id`: host epoch; reconnect retries only within the same epoch |
| `reset` | Optional `text`; clear the retained transcript and queue before replay |
| `begin` | `id`, `role` (`user`, `assistant`, `note`) |
| `delta` | `id`, `role`, `text`, or `encoding=base64` plus `data`; append incrementally |
| `end` | `id`; finish the Markdown parser/UTF-8 decoder |
| `queue_begin`, `queue_end` | Atomic queue projection boundaries |
| `queue_item` | `id`, `revision`, `mode=queued/steer/pending`, `held=0/1` |
| `queue_text` | `id`, `encoding=base64`, `data`; bounded text chunks |
| `status` | Host-owned `text`, `busy=0/1`; a reserved single-line slot keeps layout stable, with long text ellipsized and available in its title |
| `completion_provider` | Host registration: one-character `trigger`, `scope=start/word`, `strip=0/1`, `expand=0/1`; send on each connection |
| `complete` | `request`, raw full `candidate` including trigger; explicit `+` menu also uses canonical `value`, `label`, `description`, UTF-8 byte `start`/`end`, optional `continue=1` |
| `complete_end` | `request`, `status=ok/error`; completes one provider snapshot, including empty results |
| `accepted`, `error` | `request`, optional `text`; optional `view` targets one draft owner |
| `image_credit` | `id`; one more bounded binary chunk may be sent |
| `image_done` | `id`, absolute backend `path`; insert only into the originating draft |
| `image_error` | `id`, `text`; cancel the operation and keep the draft |

Browser text frames are URL-encoded actions:

- `send`/`steer`: `view`, unique `request`, monotonic `request_seq`, `text`.
- `complete`: `request`, `text`, UTF-8 byte `cursor`, `all=0/1`. Inline
  completion supplies UTF-8 byte `start`/`end` and `resolve`: initially the
  registered trigger, then an explicitly requested continuation prefix. Return
  the entire inventory for that prefix followed by `complete_end`, even when
  current typed text matches only a subset. `all=1` opens the explicit `+` menu.
- `approve`/`reject`: `id`, `revision`; `cancel` stops current generation/backlog.
- `image_begin`: unique `id`, `media_type=image/png` or `image/jpeg`, `filename`,
  exact `size`; wait for credit, send bounded binary chunks, then `image_end`.
  `image_cancel` carries the same `id` and is valid before BEGIN is delivered.

Typing uses the same completion interaction as the TUI: a dim inline ghost
leaves the draft untouched. Tab accepts and cycles the original matching subset;
Right/Ctrl-F at the token endpoint accepts or commits the selected candidate,
resolves its children once, and previews without inserting a child. Enter,
the send button and Alt-Enter submit actual draft text, never a ghost. Ordinary
typing filters cached provider snapshots without another request. Tab resolves
known directory ancestors lazily for a partial path such as `@examples/ch`.
Only the explicit `+` button opens a menu, for completions, images and recovery.

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
check page using the mock listener. Run that page and the following UX checks
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
