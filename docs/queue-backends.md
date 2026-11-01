# Asynchronous queue backends

The ordinary constructor retains the native local queue. A constructor-selected
backend instead owns accepted text, ordering, Q/S intent, approval and edit holds,
stopped state, readiness and execution. Softline owns the draft, cursor, undo,
keys and presentation. Its bounded mirror cannot execute or auto-flush work.
There is no authority switch under a live handle.

```c
sl_config_t config;
sl_queue_backend_t backend;
memset(&backend, 0, sizeof(backend));
backend.request = bridge_request;
backend.detach = bridge_detach;
backend.capabilities = (1UL << (SL_QUEUE_RESOLVE + 1)) - 1;
sl_config_init(&config);
config.prompt_queue = 1;
config.queue_backend = &backend;
config.queue_backend_context = bridge;
editor = sl_create_with_config(&config);
```

The constructor copies the callback table; its context stays borrowed until
detach returns during destruction. A backend requires queueing enabled.
Unsupported capabilities return `SL_ERROR_UNSUPPORTED` without local fallback.
This public C layout extends released v0.15.1's ABI 3 to ABI 4: rebuild consumers
with matching headers/libraries. The [Lua reference](../lua/README.md) describes
the matching constructor fields, typed operations, results and updates.

## Requests and observations

`sl_queue_backend_request()` assigns a per-handle ID and retains bounded fields
before calling `backend.request(context, request)`. Request fields are borrowed
for that callback only. Copy them, enqueue without waiting and signal an fd.
`SL_OK` means handoff, not admission. Failed handoff preserves the draft/edit.
Do not wait for storage/network/acknowledgements, reenter Softline, or deliver
an inbound response synchronously from the request callback.

Register the bridge descriptor with `sl_watch_add()` and drain bounded inbound
work on the editor owner thread. `sl_queue_backend_result()` records an outcome;
`sl_queue_backend_update()` copies accepted state. Neither invokes the request
callback. The library adds no network/storage dependency, worker thread or
interval queue polling.

An initial `SL_QUEUE_SNAPSHOT` establishes the bounded view. `UPSERT`, `REMOVE`
and `STATE` deltas chain `previous_revision` to `revision`. Existing entries
cannot move through UPSERT. Each change includes total count, stopped state and
auto-approval; per-item readiness/stop changes require entry updates. Snapshots
and deltas belong to one ordered subscription, separate from result delivery.
Do not return stale snapshots in the response stream. Unsolicited replacement
snapshots of an already synchronized view are rejected.

A gap preserves the current view and schedules one synchronization handoff at
the next native dispatch boundary. Failed/rejected synchronization requires an
explicit retry. Headless hosts submit synchronization themselves. Validation or
allocation failure preserves the previous view. IDs/revisions/edit tokens are
opaque nonempty strings of at most 127 bytes; text/reasons use `line_max_len`.
`prompt_queue_max_entries` bounds the view. Larger authorities supply a bounded
ordered window with explicit eviction/insertion deltas or resynchronization;
the UI reports when total count exceeds the window. No full enumeration occurs
after ordinary updates. `max_pending_requests` bounds ordinary retained requests
(default 16). Two additional slots reserve one synchronization and one resolution
request so saturation cannot prevent reconciliation.

## Native editor behavior

Enter offers Q text. Alt-Enter offers S text or promotes the newest visible
entry. Configured queue keys retain precedence. After successful handoff the
draft clears, while a provisional record preserves its text until an outcome.
Late results never overwrite a newer draft. Rejected text can be recovered into
an empty editor with Alt-E or `sl_queue_backend_recover()`, without resubmitting.

Alt-E requests an exact ID/revision hold; only an acknowledged owned hold opens
the edit. A claim that wins leaves the editor untouched. Nonempty submission
replaces that identity; empty submission or Alt-N withdraws it. The backend
preserves position, intent and approval hold, compares revisions and enforces
FIFO approval decisions. Results never recreate removed mirror entries.

Immediate Enter → Alt-E and Alt-Enter promotion preserve that intent while
admission is pending. Input typed while the edit hold is being acquired stays
in a bounded frontend key queue, including whole UTF-8 characters and bracketed
paste. Subsequent edit/approval keys wait for the preceding item operation's
outcome. Producer output continues directly to the terminal. Cancelling clears
the deferred edit intent and releases a hold acknowledged after cancellation;
it cannot reopen the cancelled editor.

Explicit cancellation requests ABORT. EOF, terminal failure and destruction
preserve the backend edit hold. A failed abort handoff retains the local edit;
rejected text proposals remain recoverable. Another controlling attachment can
explicitly reacquire a hold; it cannot see the old attachment's token. A timeout
must not release an edit into execution. Stop and current-turn cancellation are
separate ordered actions; the application supplies the latter's execution API.

`sl_next_prompt()` remains active across remote submissions. Accepted backend
work never returns as model input, and `sl_prompt_queue_take()` is unsupported.
The backend emits the transcript prompt at its actual application seam; the
adapter writes it once through the normal output feed. Transcript bytes are
never buffered or replayed. Non-TTY backend editing is explicitly unsupported;
headless hosts still use the typed API.

Old synchronous setters for insertion, replacement, clear, intent, policy,
readiness, auto-approval and delivery return `SL_ERROR_UNSUPPORTED`. Use typed
acknowledged operations. Existing approve/reject, enqueue-draft and resume
methods perform asynchronous handoff. Read-only native queue methods inspect
the visible cache; `sl_queue_backend_entry()` exposes backend identities and
revisions. `sl_queue_backend_info()` includes total count. Profiles change keys
and presentation only, not backend gates.

## Loss and lifetime

Publish `SL_QUEUE_DISCONNECTED` on transport loss. Keep mirror and draft, and
mark outstanding mutations `SL_QUEUE_UNKNOWN`. Read-only control requests can
be discarded and issued anew. Keep the bridge namespace stable for a handle;
resolve original request IDs with `SL_QUEUE_RESOLVE` after reconnect and apply
the authoritative ordered snapshot/deltas. Do not automatically reoffer or
flush offline requests. Unavailable outcomes remain unknown; only definitely
rejected text can be recovered as unaccepted. Deduplication and persistence
belong to the backend.

Destroy invokes detach once. Cancel subscriptions and finish accessing the
handle before returning. Detach never sends STOP, CLEAR or ABORT. Release the
borrowed context after editor destruction has finished.

## Two-terminal demo

```sh
# Terminal one:
make run-chat-queue-server
# Terminal two:
make run-chat-remote
# All C themes support this selector:
make run-chat-riced CHAT_QUEUE=remote
# Existing local behavior remains the default:
make run-chat-local
```

Both commands accept `QUEUE_PORT` (default 7635). `QUEUE_HOST` must be numeric
`127.0.0.1`: this unauthenticated example only binds loopback. The owner is
explicitly volatile, not a production protocol or durable service. Restarting
it loses its queue. Limits are 64 items, 32 frontend namespaces and 16 recent
results per namespace. Expired results stay unknown; old requests are never
readmitted. Reaching the namespace bound requires a new owner, without silently
evicting deduplication records.
An attachment supplies its prior request watermark when establishing a new
namespace, keeping outcomes lost in an owner restart unknown and preventing
readmission under an old identity. This does not provide durable queue storage.

Both C executables use the released v0.15.1 chat frontend: Softline renders the
editor, quotes, queue, approvals, reasoning and status rows. Frontend libmdf
renders response Markdown with the same themes, margins and resize handling.
The owner emits raw source chunks and lifecycle events; it has no terminal
renderer or transcript store. `/status`, `/approve [on|off]`, `/steer`, `/resume`,
`/quit`, `!sh` and `/shell` retain their local policy. Other text remains model
input. Alt-E edits under a hold, Alt-N discards, and Alt-A then Alt-Y/Alt-N
approves/rejects in FIFO order. Ctrl-C stops backlog and cancels the current
turn; `/resume` releases stopped work. A new ordinary user turn can resume the
backlog, as in the released local demo.

Injections use the same random messages and 10–20 second schedule as local chat.
Set `CHAT_APPROVAL_DEMO=0` on the **frontend** to disable them.
Alt-R explicitly reconnects and resolves original IDs without reoffering.
Alt-S preserves the draft and requests shell handoff at the next response seam
(immediately when idle). The frontend closes its subscription before running
the shell, so the owner continues producing and executing accepted work without
socket backpressure from an unread frontend. On return it resynchronizes queue
state and joins future output at a response seam. Missed output is discarded,
never buffered or replayed. Local C/Lua Alt-S remains between operations.
Ctrl-D on an empty editor detaches; accepted work and holds belong to the owner.
The `run-chat-without-delay` variants also work in remote mode: the frontend
requests zero producer delay when it attaches to the owner.

`softline_queue_backend` tests deferred outcomes and native PTY keys.
`softline_queue_tcp` starts the actual owner and fd adapter, proving admission
deduplication, hold handoff, approval FIFO and frontend teardown/reconstruction.
The released chat policy, shell and approval interactions also run against the
remote frontend and actual owner. VTE tests assert physical rows, wrapped and
truncated previews, focus, edit/discard, multiline input, themes and resize.
