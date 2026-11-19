# Browser terminal

`make run-chat-queue-server` serves the chat and terminal on the same mock web
listener, normally `http://127.0.0.1:8081`. Open **Terminal** from `+`, or press
Ctrl+` in the chat prompt. The terminal slides up in a bottom panel and takes
layout space: conversation and composer stay above it, with their own scrolling.
Ctrl+` in the terminal or prompt, or the terminal's close icon, closes the panel
and terminates its shell. Exiting the shell also closes the panel automatically,
after final output has been consumed by the renderer. It never leaves a hidden
shell running. Opening again starts a new shell.
Restart an already running example and reload the chat to obtain the new assets
and terminal capability.

The terminal uses vendored MIT xterm.js 6.0.0 and its fit addon 0.11.0. All
JavaScript, CSS and JetBrains Mono bytes are served locally; there is no CDN,
package manager or browser installation at runtime. The terminal uses the chat
theme selected in chat (including live theme changes) and a monospace terminal grid.
The panel reserves at least 25 measured terminal rows or one third of the viewport,
whichever needs more space, including room for its footer and padding. A single
thin line separates chat and terminal; connection/exit status and the close icon
sit in a compact 24-pixel footer below the grid. When the
window or on-screen keyboard leaves too little space, it shrinks to preserve the
composer and some conversation space. Font loading, window resizing and composer
growth recalculate the size. Reduced-motion preferences disable the slide animation.
Resize updates the
server PTY; Ctrl+C, Ctrl+D, escape sequences and binary terminal responses travel
as PTY input. The server shell uses `$SHELL` or `/bin/sh`, in the server's current
directory (`build/` for the Make target).

Each terminal WebSocket creates an independent `sl_terminal_session_t`. It uses
the public OPEN/STEP/READ/WRITE/RESIZE/CLOSE contract on the example host's event
loop. Terminal output is never conversation output and is not retained in the
chat replay log. Closing/navigating away disconnects the socket, terminates the
shell and foreground process group, and arranges nonblocking child reaping.
The standalone `/terminal` asset remains usable directly and reports shell exit
status in its footer. The dock embeds that same view in an iframe, with a
separate WebSocket; closing the iframe closes its transport. Only messages from
that exact frame and origin can resize or close the dock. Reconnection
never silently starts a new shell: open another terminal deliberately.
Healthy shells have no idle/session timeout.
The TUI's Alt-S remote terminal and browser terminals can remain open and run
concurrently. They use separate PTYs with independent shell state, dimensions
and lifetimes; closing or resizing one does not affect the other.

By default, opening a browser terminal does not pause chat, the agent, or prompt input.
The example continues servicing chat while the shell runs, including when
terminal output is blocked by flow control. A downstream host such as CAI may
choose to pause its **agent** until shell exit. It must still service its I/O
event loop to move terminal bytes, process close/exit and serve conversation
scrolling/replay. Softline does not impose an agent scheduling policy.

Concurrent prompt and terminal is the default. In the chat examples, submit
`/terminal-lock` from the web UI or TUI to toggle automatic locking for the
conversation. Use `/terminal-lock on` or `/terminal-lock off` to set it directly.
While a browser prompt is automatically locked, choose **+ → /terminal-lock off**
to restore typing with the shell still open and the draft intact.
`SOFTLINE_WEB_DISABLE_PROMPT_DURING_TERMINAL=1` sets the initial policy at startup.
The server counts browser PTYs automatically, and
the native handoff paths report their activity to the same conversation owner.
Every browser prompt locks while any handoff is initializing or running, then
unlocks after the last exits, disconnects or fails. Cleanup/reaping does not
retain a stale UI lock. The same state is sent to late-joining/reconnecting views.
The queue owner forwards this snapshot through the demo transport to remote
clients serving their own web UI. Configure the queue owner's policy; its
snapshot is authoritative for those mirrored views, including when the owner's
own web listener is off. A local TUI handoff retains the host's existing native
relay pause; hosts needing live I/O during that pause must keep servicing their
event loop.

This is a live policy: the example host can call
`demo_www_set_terminal_prompt_lock(listener, enabled)` during chat, including
while shells are open. `demo_www_set_terminal_handoffs(listener, count)` supplies
the host's current non-browser handoff count, added to the listener's browser
sessions. The examples wire both native and browser lifecycle paths; the setters
are example integration helpers, not part of the installed library API.
`demo_www_set_terminal_state(listener, count, enabled)` updates a mirrored
count/policy atomically, and `demo_www_terminal_status()` reads current state.

Other hosts publish the atomic event
`{"type":"terminal_state","active":"2","disable_prompt":"1"}` through
`sl_www_emit()` to all views of that conversation. Update it whenever the active
handoff count or policy changes, and replay the current snapshot after `session`.
Do not replay historical counts as current state. Turning the policy off while
sessions remain open immediately permits input again. Hosts serving one
conversation through multiple listeners must propagate the same authoritative
state to those listeners. This carries host scheduling policy without making
the web receiver own PTYs, sockets, a conversation registry or an agent loop.

To explicitly dim and lock the composer independently of that policy, emit the host-controlled chat
event `{"type":"prompt_state","disabled":"1","text":"Agent waiting for terminal"}`
through `sl_www_emit()`. Send `disabled="0"` when input may resume. `text` is an
optional tooltip, not a new status row. The draft and selection are retained;
typing, completion acceptance, sending/steering and adding images are blocked,
while conversation scrolling, output rendering and the terminal continue.
The lock does not affect queue/approval controls. It is independent of terminal
visibility: panel closure never overrides the host's lock or the server's shared
handoff count. Keyboard terminal
toggle remains available while the composer is locked. A new host session
epoch clears stale locks/policy and closes any old terminal panel.

The host chooses whether to send that state to one view or every conversation
view, and supplies its current state during replay/reconnect. Reconnection to
the same host retains the last lock until the host updates it. Treat it as a UI
state: hosts must still enforce their own admission policy. Neither the UI lock
nor terminal opening changes the actual prompt queue's scheduling.

Terminal **handoff** transfers interactive control. It does not automatically
send a terminal transcript to an agent. Hosts already receive raw PTY output
through `SL_TERMINAL_READ` and can feed both the frontend and their own capture
consumer while chat remains live. The examples forward those chunks without
recording them. Recording a session and delivering its contents to an agent on
exit is host policy, such as a CAI feature; there is no built-in "send all
terminal content" action. The browser's bounded scrollback is not a full-session
recording, and raw PTY bytes include terminal control sequences.

Custom terminal pages embedded through the `terminal` capability can use the
same small frame bridge: post `softline-terminal-closed` to the same-origin
parent on close or shell exit, and `softline-terminal-size` with numeric
`rowHeight` and `chromeHeight` after measurement. The dock sends
`softline-terminal-stop` before removal and `softline-terminal-theme` with the
selected theme on load/change. The shipped view handles these messages; custom
pages must validate both the source window and origin too.

The library ships `/terminal`, `/terminal.js`, `/terminal-panel.js`, `/terminal.css`, `/xterm.js`,
`/xterm-addon-fit.js`, `/xterm.css`, and their license assets through
`sl_www_asset()`. Its web handler remains off by default and opens no listener,
socket or worker. In Kore or another host, mount those immutable assets and
supply a host-owned WebSocket route at the relative `terminal/events` path.
The examples' `web_terminal.c` illustrates the adapter using the public terminal
API and existing bounded web action decoder. The host selects authorization,
shell policy and event-loop integration. The queue owner's transport is a mock
example only; it is not installed into libsoftline.

The example terminal WebSocket uses these bounded messages:

| Direction | Message |
| --- | --- |
| Browser → host, text | URL-encoded `action=open&rows=N&columns=N`, once |
| Browser → host, text | `action=resize&rows=N&columns=N` |
| Browser → host, text | `action=ack&bytes=N`, after xterm's write callback consumes output |
| Browser → host, binary | At most 4096 raw input bytes per granted input credit |
| Host → browser, text | JSON `{"type":"ready"}`, then `{"type":"input_credit"}` |
| Host → browser, binary | Raw PTY output chunks, at most 4096 bytes each |
| Host → browser, text | JSON `{"type":"exit","status":"N"}`, after final output |
| Host → browser, text | JSON `{"type":"error","text":"..."}` on failure |

Only 64 KiB of output may be in flight before renderer acknowledgements. When
the window fills, the host stops reading the PTY; it continues processing chat,
other peers, terminal input and disconnects. Input has one 4096-byte host buffer
and a 64 KiB browser pending-input ring. Overflow closes explicitly rather than
dropping keystrokes or growing an unbounded queue. These are transport buffers,
not session-duration limits. The rendered terminal has 5000 scrollback lines;
the host retains no terminal transcript. A closing WebSocket gets a one-second
handshake window while its PTY cleanup proceeds independently.

`softline_www_terminal_owner` drives the actual HTTP/WebSocket listener and real
shell: working directory, geometry, Ctrl+C, UTF-8/NUL/high bytes, separate PTYs,
EOF/exit status, output backpressure with simultaneous chat, and invalid credit.
It also keeps the TUI's native TCP PTY open alongside the browser PTY, verifies
native I/O while browser output is blocked, and checks independent shell state
and exit status after a browser shell closes.
`softline_www_terminal_lock` repeats the real native/browser lifecycle with the
automatic policy enabled, including multiple views and a late join. Local Alt-S
and failed-launch tests verify the native chat hooks. The remote handoff test
verifies that the backend policy overrides the frontend's local default and
reaches both browser views. The mock test changes
policy live while handoffs remain active and verifies both browser connections.
`softline_www_terminal_adapter` verifies blocked input, cancellation, failed
launch and child reaping. `softline_www_terminal_client` checks browser transport
logic with xterm/DOM doubles, including acknowledgements after consumption,
resize, overflow, disconnect, parent-message validation, measured row geometry,
and closing only after final-output handling. The chat client tests cover docking,
short viewports, theme changes, independent chat, prompt locking and pending
completion invalidation. Asset tests compare the
shipped bytes with the vendored files. These checks do not substitute for real
browser layout/keyboard testing, which remains deferred to a browser-capable host.
