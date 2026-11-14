# Streaming image-paste handover

A constructor-configured `image_paste_callback` replaces local clipboard-image
storage with owner-thread handover. Ctrl-V still selects PNG, falling back to
JPEG. An explicit `sl_image_paste_start()` uses exactly the same lifecycle at the
current draft cursor. A key binding for Ctrl-V takes precedence. This feature
requires Linux and an available X11 DISPLAY, including SSH-forwarded X11. Plain
SSH without X11 forwarding does not provide clipboard access.

Only encoded PNG/JPEG image content is acquired. File-list clipboard targets,
local-file opening, arbitrary file upload and terminal file-transfer protocols
are outside this interface. Pasted text and path-looking strings remain text.
Softline does not implement a backend transport, filesystem service or workers.

## Callback lifecycle

Set `image_paste_callback` and borrowed `image_paste_context` in `sl_config_t`
before constructing the editor. All calls and callbacks use the editor owner
thread. The callback must return promptly, must not reenter Softline, and must
not block or yield. Its context remains valid until editor destruction finishes.

1. BEGIN admits the selected media type and suggested display filename before
   image content is requested. Its nonzero ID identifies this transfer only;
   IDs are never reused during one handle's lifetime.
2. DATA supplies a binary chunk, length and byte offset. Return SL_OK only after
   consuming or copying all bytes. SL_ERROR_NOT_READY means no consumption;
   Softline retains that one chunk, stops acquisition and retries it only after
   `sl_image_paste_resume()`. Metadata and data pointers are callback-scoped.
3. END confirms clean acquisition EOF and the actual accepted byte count. It
   does not acknowledge backend storage. There are no more source reads.
4. After backend publication, call `sl_image_paste_complete()` with its real,
   absolute filesystem path. A transfer ID, URL or frontend cache path is not
   a substitute. The path is copied and inserted at a subsequent dispatch
   boundary. To report backend failure, pass NULL for the path and a reason.
5. ABORT reports source failure, rejected admission/chunks, timeout, cancellation,
   invalidated draft or failed result insertion. It is terminal and exactly once;
   the host aborts/releases incomplete or unreferenced backend work. No local
   cache fallback occurs. DONE reports successful insertion. Each admitted
   transfer ends with exactly one ABORT or DONE, allowing the host to release
   its transfer bookkeeping. Return values of terminal notifications are ignored.

A callback failure aborts the upload while keeping editing available. Callback
failure reasons are concise native diagnostics; backend failures can supply a
bounded reason through complete. Invalid/stale/duplicate resume, cancel and
completion calls return SL_ERROR_INVALID and cannot duplicate insertion. An
invalid result path can be corrected while the transfer remains in backend wait.
Never call through a destroyed handle; remove application watches before closing
it. Destruction aborts pending work before releasing the borrowed context.

## Bounds and deadlines

`image_paste_max_bytes = 0` selects **10 MB (10,000,000 bytes) per image**. Positive
values select explicit byte limits up to INT_MAX. Actual acquired bytes enforce
the limit even when X11 size hints are dishonest. Known oversized properties are
rejected early. An exact-limit image requires clean EOF. Source chunks are at
most 16,384 bytes, with fixed bounded X11 setup/property buffers; no whole-image
buffer, mmap, temporary file or frontend cache copy is created in handover mode.
INCR properties are acknowledged only after the host accepts their chunks.

Timeout options are milliseconds; zero selects the defaults:

| Option | Default | Measures |
| --- | --- | --- |
| `image_paste_source_timeout_ms` | 10,000 | No newly accepted image chunk or representation admission; excludes host pause |
| `image_paste_host_timeout_ms` | 30,000 | Each host pause or backend acknowledgement wait |
| `image_paste_total_timeout_ms` | 120,000 | Entire transfer, including pauses |

Negative timeouts and unrepresentable limits are rejected. Once an acknowledged
path is retained for safe insertion during terminal suspension, it no longer has
an acquisition/backend deadline. Retained result paths are limited to 4,095 bytes
and must be valid UTF-8 absolute paths without terminal control characters.
Without an adapter, standalone image caching retains its 32 MiB limit and path
template behavior, using the same X11 acquisition machinery.

## Editing and event-loop integration

One transfer can be pending per editor. Pending status is rendered outside prompt
text. Typing remains available; edits before the insertion anchor move it, while
typing exactly at the anchor follows the future image path. A deletion across the
anchor, draft replacement, history recall, queued-edit replacement/cancellation
or editor destruction aborts the transfer. Hosts switching sessions/workspaces
must cancel the old transfer or replace its draft before accepting new work.

Enter, Alt-Enter, configured queue admission keys and programmatic submission
retain the draft while an image is pending. Ctrl-C cancels only the pending
image and immediately continues editing the preserved draft. Outside a transfer,
Ctrl-C retains its ordinary interrupt behavior. Explicit host key bindings retain
precedence except for pending-upload cancellation/submission guards.

After acknowledgement, insertion is literal, atomic against `line_max_len`, and
one undoable edit. It does not trigger completion, add history or submit anything.
Undo affects text only; backend retention/deletion belongs to the application.
Existing queue identities, edit holds and approval state follow their normal
submission path when the user explicitly submits the resolved draft.

Rendering suspension preserves acquisition. An application's loop can call
`sl_image_paste_prepare()` to obtain borrowed descriptors/readiness interests and
the next timeout, then `sl_image_paste_pump()` on readiness or deadline. Prepare
again after each operation. A zero timeout requests an immediate bounded step;
-1 means no deadline. There is no periodic polling requirement. Native editing
uses these same functions automatically. While suspended, a completed result is
retained and inserted when the original draft resumes. Replacing it cancels.

Network readiness is application-owned. A host can register its transport or
wakeup descriptor with `sl_watch_add()`, then resume/complete on the owner thread.
No worker may access the editor directly. Backpressure must continue through the
host transport: accepting chunks into an unbounded application queue defeats the
resource bound even though Softline retains only one chunk.

See the public header for C declarations and [the Lua facade](../lua/README.md)
for equivalent callbacks and methods. `tests/clipboard_fixture.c` and
`tests/lua-fixtures/image-paste.lua` demonstrate streaming checksums with deferred
acknowledgement; they explicitly submit their final draft to drive test automation.
