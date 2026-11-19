# Browser terminal

`make run-chat-queue-server` serves the chat and terminal on the same mock web
listener, normally `http://127.0.0.1:8081`. Open **Terminal** from `+`, or press
Ctrl+` in the chat prompt. The page opens in a separate tab with no opener access.
Restart an already running example and reload the chat to obtain the new assets
and terminal capability.

The terminal uses vendored MIT xterm.js 6.0.0 and its fit addon 0.11.0. All
JavaScript, CSS and JetBrains Mono bytes are served locally; there is no CDN,
package manager or browser installation at runtime. The terminal uses the chat
theme saved for that origin and a monospace terminal grid. Resize updates the
server PTY; Ctrl+C, Ctrl+D, escape sequences and binary terminal responses travel
as PTY input. The server shell uses `$SHELL` or `/bin/sh`, in the server's current
directory (`build/` for the Make target).

Each terminal WebSocket creates an independent `sl_terminal_session_t`. It uses
the public OPEN/STEP/READ/WRITE/RESIZE/CLOSE contract on the example host's event
loop. Terminal output is never conversation output and is not retained in the
chat replay log. Closing/navigating away disconnects the socket, terminates the
shell and foreground process group, and arranges nonblocking child reaping.
Shell exit preserves the final output and reports its exit status. Reconnection
never silently starts a new shell: reload/open another terminal deliberately.
Healthy shells have no idle/session timeout.
The TUI's Alt-S remote terminal and browser terminals can remain open and run
concurrently. They use separate PTYs with independent shell state, dimensions
and lifetimes; closing or resizing one does not affect the other.

The library ships `/terminal`, `/terminal.js`, `/terminal.css`, `/xterm.js`,
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
resize, overflow, disconnect and final-output handling. Asset tests compare the
shipped bytes with the vendored files. These checks do not substitute for real
browser layout/keyboard testing, which remains deferred to a browser-capable host.
