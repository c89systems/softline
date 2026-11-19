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
whichever needs more space, including room for its toolbar and padding. When the
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
status in its toolbar. The dock embeds that same view in an iframe, with a
separate WebSocket; closing the iframe closes its transport. Only messages from
that exact frame and origin can resize or close the dock. Reconnection
never silently starts a new shell: open another terminal deliberately.
Healthy shells have no idle/session timeout.
The TUI's Alt-S remote terminal and browser terminals can remain open and run
concurrently. They use separate PTYs with independent shell state, dimensions
and lifetimes; closing or resizing one does not affect the other.

Opening a browser terminal does not pause chat, the agent, or prompt input.
The example continues servicing chat while the shell runs, including when
terminal output is blocked by flow control. A downstream host such as CAI may
choose to pause its **agent** until shell exit. It must still service its I/O
event loop to move terminal bytes, process close/exit and serve conversation
scrolling/replay. Softline does not impose an agent scheduling policy.

To dim and lock the composer during that pause, emit the host-controlled chat
event `{"type":"prompt_state","disabled":"1","text":"Agent waiting for terminal"}`
through `sl_www_emit()`. Send `disabled="0"` when input may resume. `text` is an
optional tooltip, not a new status row. The draft and selection are retained;
typing, completion acceptance, sending/steering and adding images are blocked,
while conversation scrolling, output rendering and the terminal continue.
The lock does not affect queue/approval controls. It is independent of terminal
visibility: panel closure never overrides the host's lock. Keyboard terminal
toggle remains available while the composer is locked. A new host session
epoch clears a stale lock and closes any old terminal panel.

The host chooses whether to send that state to one view or every conversation
view, and supplies its current state during replay/reconnect. Reconnection to
the same host retains the last lock until the host updates it. Treat it as a UI
state: hosts must still enforce their own admission policy. Neither the UI lock
nor terminal opening changes the actual prompt queue's scheduling.

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
`softline_www_terminal_adapter` verifies blocked input, cancellation, failed
launch and child reaping. `softline_www_terminal_client` checks browser transport
logic with xterm/DOM doubles, including acknowledgements after consumption,
resize, overflow, disconnect, parent-message validation, measured row geometry,
and closing only after final-output handling. The chat client tests cover docking,
short viewports, theme changes, independent chat, prompt locking and pending
completion invalidation. Asset tests compare the
shipped bytes with the vendored files. These checks do not substitute for real
browser layout/keyboard testing, which remains deferred to a browser-capable host.
