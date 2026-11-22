# Read-only terminal transcripts

A transcript displays output from a host-owned producer. A producer can be a
background PTY, a command log, or any other source of UTF-8 text or terminal
bytes. The host is responsible for starting and stopping producers and for
deciding which views a conversation can see.

## Native editor

Include `<softline/transcript.h>`. The C89 interface uses the editor's owner
thread, as do the watch and idle callbacks. For example, register a viewer
and feed each chunk from a watch on a background PTY.

```c
sl_transcript_id_t id;
int result;

result = sl_transcript_add(editor, "Build", source_rows, source_columns, &id);
/* After successful registration, call this for each available chunk. */
result = sl_transcript_write(editor, id, SL_TRANSCRIPT_VT, bytes, length);
```

Use `SL_TRANSCRIPT_TEXT` for plain UTF-8 text. Text supports newlines and tabs
and discards other ASCII controls. VT mode interprets terminal output.
Either format can span calls. When mixing the formats, switch at complete UTF-8
and VT sequence boundaries. Use `sl_transcript_resize` for source geometry
changes and `sl_transcript_reset` before replacing a source or replaying it.
The last rendered screen remains available after the producer stops, until
the host calls `sl_transcript_remove`.

**Ctrl+T** cycles through registered viewers in order and returns to chat
after the last one. **Esc** and **Ctrl+C** return directly. Arrow keys pan
and **Page Up**, **Page Down**, **Home**, and **End** move through the
available rows. A host binding for Ctrl+T takes precedence when the editor
is on the chat screen. With no viewers, Ctrl+T transposes characters as usual.
An **Alt+S** handoff restores the chat screen before invoking the host's key
binding. The interactive shell can use its own alternate screen.

Each viewer emulates cursor moves, erase, styles, and alternate-screen
transitions in its own screen at the source geometry. Softline paints the
resulting cells on its physical alternate screen. The viewport can crop or
pan over the source grid. Softline sends no input or terminal query replies
to a producer. It accepts up to eight viewers per editor, at up to 200 rows
and 320 columns each. Native normal-screen scrollback retains up to 2,000
rows or 2 MiB per viewer, whichever is reached first.

The watch and idle callbacks, queue delivery, and browser feeds keep running.
While a transcript occupies the terminal, native chat output is validated and
queued for display. When chat returns, Softline restores the terminal's normal
screen and replays the retained output above the current draft. The backlog
holds up to 1,000 newline-delimited lines or 256 KiB, whichever fills first.
Older lines fall out first. A single line too large to fit is dropped through
its next newline. Replay after a cutoff starts with default text styling.
Outside the transcript viewer, chat output goes directly to the terminal.

## Web handler

Use `sl_www_emit` on the existing conversation event connection. All fields
are strings. Use a stable nonempty `id` of up to 80 characters within the
conversation session.

| Event | Fields |
| --- | --- |
| `transcript_open` | `id`, `title`, `rows`, `columns` |
| `transcript_data` for text | `id`, `text` |
| `transcript_data` for VT | `id`, `encoding=base64`, `data` |
| `transcript_resize` | `id`, `rows`, `columns` |
| `transcript_reset` | `id` |
| `transcript_remove` | `id` |

Send data in chunks of up to 16 KiB before base64 encoding. Text events
carry complete UTF-8 characters. VT events can split UTF-8 and terminal
sequences across chunks. The browser keeps its own screen for each viewer.
An existing ID can be re-announced on resume, and a host can send a
reset and stream replay from a known initial screen. After a conversation
reset or session change, announce the current registry again before replaying
any chunks.

**F8** or the **Transcripts** button cycles through viewers and closes the
pane after the last one. Select a tab to jump directly to a viewer. Use the
arrow keys to switch tabs when the tab bar has focus. The X closes the pane.
The pane is to the right of the conversation on wide screens and above it
on narrow screens. Drag its divider or focus it and use the arrow keys to
adjust the space shared by the transcript and the conversation, including any
interactive terminal under chat. Each read-only xterm.js instance uses the
producer's declared rows and columns and up to 2,000 scrollback rows. If pending
input exceeds the viewer's 256 KiB budget, it shows a recovery
message until the next host reset. Host replay should pace chunks as needed.

The C chat examples populate a text log and a simulated worker screen.
Both update as replies complete. Try `make run-chat-web` to see both UIs,
`make run-chat-local` for the native editor, or the remote chat targets.
