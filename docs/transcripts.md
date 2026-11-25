# Read-only terminal transcripts

A transcript displays output from a host-owned producer. A producer can be a
background PTY, a command log, or any other source of UTF-8 text or terminal
bytes. The host is responsible for starting and stopping producers and for
deciding which views a conversation can see.

## Native editor

Include `<softline/transcript.h>` for the C free functions. The same operations
are available on the editor receiver and in [Lua](../lua/README.md#read-only-transcripts).
The C89 interface uses the editor's owner
thread, as do the watch and idle callbacks. For example, register a viewer
and feed each chunk from a watch on a background PTY.

```c
sl_transcript_id_t id;
int result;

result = sl_transcript_add(editor, "Build", source_rows, source_columns, &id);
if (result == SL_OK)
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
after the last one. The header gives the current viewer's position and total
count, such as `1/2`, followed by key hints. **q**, **Esc**, and **Ctrl+C**
return directly from the viewer. A host binding for Ctrl+T takes precedence
from the chat screen. With no viewers, Ctrl+T transposes characters as usual.
An **Alt+S** handoff restores chat before invoking the host's binding so the
interactive shell can use its own alternate screen.

| Keys | Move through output |
| --- | --- |
| **j** / **k** or **↓** / **↑** | Down / up one row |
| **J** / **K** | Down / up five rows |
| **d** / **u** | Down / up half the visible content height |
| **Space** or **f** or **Page Down** / **b** or **Page Up** | Down / up one page |
| **g** or **Home** / **G** or **End** | Oldest retained row / newest output and resume live follow |
| **←** / **→** | Pan horizontally |

### Search

Press **/** to enter a query and **Enter** to find the first match from the
current view toward newer output. **Esc** while entering a query restores
the previous search and position. Search matches literal UTF-8 text in the
terminal cells, including scrollback and columns outside the viewport.
Queries can use up to 256 bytes. ASCII letters match regardless of case.

Visible matches are highlighted, with a distinct highlight for the selected
occurrence. **n** and **N** select the next and previous match and bring it
into view. When there is no further match, the header reports it and the
viewport stays in place. Scrolling back or searching holds the viewport as
output arrives. Changes to the selected row and scrollback eviction clear
a stale selection. **G** resumes following live output. **c** clears the query and
all highlights while keeping the current position. The header shows the
clear and match keys while a query is active.

### Output and scrollback

Each viewer emulates cursor moves, erase, styles, and alternate-screen
transitions at the source geometry. Softline redraws changed rows on the
physical alternate screen. The viewport can crop or pan over the source
grid. Softline sends no input or terminal query replies to the producer.
Each editor accepts up to eight viewers, each up to 200 rows by 320 columns.
Normal-screen scrollback retains up to 2,000 rows or 2 MiB per viewer,
whichever fills first.

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
are strings. Register up to eight views with a stable nonempty `id` of up
to 80 characters within the conversation session. `title` labels the
browser tab. Source rows and columns use the same bounds as the native viewer.

| Event | Fields |
| --- | --- |
| `transcript_open` | `id`, `title`, `rows`, `columns` |
| `transcript_data` for text | `id`, `text` |
| `transcript_data` for VT | `id`, `encoding=base64`, `data` |
| `transcript_resize` | `id`, `rows`, `columns` |
| `transcript_reset` | `id` |
| `transcript_remove` | `id` |

The browser accepts up to 16 KiB of decoded data per chunk. When sending with
`sl_www_emit()`, all field values together must also fit `SL_WWW_CHUNK_MAX`
(8192 bytes), including `id`, `encoding`, and the base64 string. Chunks of up
to 5 KiB of raw VT data fit this budget with any supported transcript ID. Text
events carry complete UTF-8 characters. VT events can split UTF-8 and terminal
sequences across chunks. The browser keeps its own screen for each viewer.
Re-announcing an existing ID leaves that viewer intact. Send
`transcript_resize` to change its source dimensions or `transcript_reset`
before replaying from an empty screen. After a conversation reset or session
change, announce the current registry again before replaying any chunks.

**F8** or the **Transcripts** button cycles through viewers and closes the
pane after the last one. Select a tab to jump directly to a viewer. Use the
arrow keys to switch tabs when the tab bar has focus. **Page Up** and
**Page Down** scroll the viewer when its screen or a tab has focus. The
right scrollbar and mouse wheel scroll output. A bottom scrollbar pans
across wide terminal rows. The X closes the pane.
The pane is to the right of the conversation on wide screens and above it
on narrow screens. Drag its divider or focus it and use the arrow keys to
adjust the space shared by the transcript and the conversation, including any
interactive terminal under chat. Each read-only xterm.js instance uses the
producer's declared rows and columns and retains up to 2,000 scrollback rows.
Retained rows fill the available pane above the source grid with the same
xterm.js formatting. A producer's alternate screen displays its own grid.
If pending input exceeds the viewer's 256 KiB budget, it shows a recovery
message until the next host reset. Host replay should pace chunks as needed.

The C chat examples start the Run log with several screens of numbered rows
and append new rows while chat is idle. Look for `cache hit` to try repeated
matches. Wide rows include an artifact path for horizontal panning. The
Worker screen shows the live step and completed response count on its
alternate screen. Try `make run-chat-web` to see both UIs,
`make run-chat-local` for the native editor, or the remote chat targets.
