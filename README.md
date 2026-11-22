# softline

softline is a C89 multiline prompt editor derived from [linenoise](https://github.com/antirez/linenoise).
It is meant for shells, chat prompts, REPLs, and other readline-like
interfaces where Enter submits and `Ctrl-J` inserts a newline.

Create one `sl_t` for each editor. It owns the draft, history, terminal
state, rendering and callbacks. Receiver methods and the matching `sl_*`
functions use the same contracts.

```c
#include <softline/softline.h>
#include <stdio.h>

int main(void) {
  sl_t *sl = sl_create();
  char *line;
  int result;
  if (!sl)
    return 1;
  line = sl->readline(sl, "softline> ");
  if (line) {
    puts(line);
    sl->free_string(sl, line);
  }
  result = sl->last_readline_status(sl) == SL_READLINE_ERROR;
  sl->destroy(sl);
  return result;
}
```

Pass `NULL` as the prompt to use the default prompt, `"> "`.

## Editing and integration

Enter submits and Ctrl+J inserts a newline. Ctrl+D exits an empty prompt or
deletes forward within text. Ctrl+C interrupts the editor unless it is
cancelling an image upload or leaving a transcript. Alt+K stashes a draft
with content, Alt+J appends the stash, and Ctrl+_ undoes draft edits.
Up/Down navigate visual rows and history. Ctrl+R searches history and Enter
selects the match for editing.

The editor supports multiline UTF-8, bracketed paste, inline completion,
host-owned status messages and a prompt queue. Hooks run on the editor's
owner thread. When either descriptor is redirected, input falls back to a
silent line reader. Use `sl_last_readline_status()` to distinguish EOF,
cancellation, interruption, suspension and errors.

| Guide | Use it for |
| --- | --- |
| [Editor behavior](docs/softline-spec.md) | Keyboard editing, completion providers, local queue policy, history, and OSC 8 hyperlinks |
| [Live output](docs/softline-mdf-stream-design.md) | Streaming an external renderer above the prompt and handling terminal resize |
| [Queue backends](docs/queue-backends.md) | An external queue authority, reconnects, and the opt-in libcbor integration |
| [Image paste](docs/image-paste.md) | Linux Ctrl+V, streaming uploads, and host acknowledgement |
| [Terminal handoff](docs/terminal-handoff-design.md) | Local and remote shells, PTY sessions, and raw-mode recovery |
| [Transcripts](docs/transcripts.md) | Read-only native and browser views of background output |
| [Web frontend](docs/web-frontend.md) and [browser terminal](docs/web-terminal.md) | HTTP/WebSocket host integration, replay, and xterm.js |
| [Lua API](lua/README.md) | Lua 5.5 editor methods and callback contracts |

The installed `<softline/softline.h>` documents the editor, including ownership,
return values and receiver equivalents. `<softline/terminal.h>` exposes the
host-driven server PTY, `<softline/transcript.h>` the native transcript viewer,
and `<softline/www.h>` the opt-in web handler. `<softline/cbor.h>` is a
convenience include for applications that link libcbor and supply a transport.

## Examples

| Command | What it runs |
| --- | --- |
| `make run-simple` | An ordinary multiline REPL |
| `make run-chat` | Local ANSI chat in the Gruvbox theme |
| `make run-chat-web` | The same local chat with both ANSI and browser views |
| `make run-chat-web-only` | The local example with a browser view |
| `make run-chat-queue-server` | The separate queue owner and its browser listener |
| `make run-chat-remote` | The remote ANSI client and its browser listener |
| `make lua-debug-simple`, `make lua-debug-chat` | Lua examples using the debug library |

For the queue demo, start the server first. Use `THEME=riced` to choose a
theme or `CHAT_QUEUE=remote` to run a themed C chat against the queue owner.
Interactive C targets run under the [terminal recovery launcher](docs/terminal-handoff-design.md#independent-launcher).

The C chat streams Markdown through libmdf and adds a chart to every third
simulated reply. The browser renders the same chart fences, with tile charts
shown as pies. Lua chat demonstrates the queue with plain text. The examples
register their own commands and completions. Try `/q` then Tab for `/quit`,
or `@REA` then Tab for `@README.md`. Right or Ctrl+F commits a directory and
previews its first child. The example removes `@` when the completion ends.

| C chat command | Effect |
| --- | --- |
| `/stream`, `/stream direct`, `/stream llm` | Toggle or select response pacing |
| `/demo` | Toggle simulated prompt typing in the TUI |
| `/status`, `/resume` | Inspect or resume paused work |
| `/approve [on\|off]` | Toggle or set conversation-wide automatic approval |
| `!sh`, `/shell` | Open a shell after the current turn finishes |
| `/quit` | Exit when the command is delivered |

Alt+S opens a shell at a turn boundary and restores the draft after exit.
Enter then `~.` ends a handoff locally. The remote example retries
disconnections while preserving the draft. See the guides for the
[browser commands and settings](docs/web-frontend.md#run-the-examples)
and [remote queue settings](docs/queue-backends.md#two-terminal-demo).

## Terminal-native chat

Use a persistent output session to stream above a bottom-anchored prompt.
The renderer owns formatting and the terminal owns scrollback. Softline owns
prompt placement and updates only its rows. Feed writes consume bounded
chunks without waiting for cursor reports.

```c
sl_output_stream_begin(sl);
/* On the owner thread, pass each renderer sink emission to this call. */
sl_output_stream_write(sl, bytes, length);
/* After finishing the renderer document: */
sl_output_stream_end(sl);
```

Check each return value. At a settled resize boundary, call
`sl_output_stream_reconcile()` before resuming a synchronous producer.
`sl_output_stream_write_quoted_prompt()` writes literal submitted text with
a themed prefix. Softline can also present finite output using
`sl_print_above()`. See the [streaming and resize contract](docs/softline-mdf-stream-design.md).

## Prompt queueing

Enable the local queue and call `sl_next_prompt()` to receive application
work with its source. The default profile binds Tab to queueing and Alt+E
to editing. Use the queued-turns profile with `sl_set_status_busy()` to
queue Enter during a turn and preserve Alt+Enter steering. Set manual
delivery to consume eligible entries at host-selected boundaries.

```c
sl_set_prompt_queue(sl, 1, 64, 3);
sl_set_prompt_queue_profile(sl, SL_PROMPT_QUEUE_PROFILE_QUEUED_TURNS);
sl_set_prompt_queue_delivery(sl, SL_PROMPT_QUEUE_DELIVERY_MANUAL);
```

The 500 ms default preview delay affects display only. A visible queue
shows new arrivals immediately. Approval, edit and connection controls
appear immediately. See the [local queue contract](docs/softline-spec.md#prompt-queue)
or the [external authority guide](docs/queue-backends.md).

## External events while editing

Register an application-owned descriptor with `sl_watch_add()` to receive
nonblocking worker or transport events on the editor owner thread. Use the
callback to drain bounded work, feed output, update status, or advance queue
state. The application owns the descriptor and removes the watch before
closing it.

## Status lines

Status lines and their message row are host-controlled. Enable the status
line, set its elements, and update or clear a reasoning message as work
progresses.

```c
static const char *const status[] = {"model", "ctx 36%", "demo/project"};

sl_set_statusline(sl, 1, 0);
sl_set_status_elements(sl, status, sizeof(status) / sizeof(status[0]));
sl_set_status_message(sl, "Thinking...");
sl_set_status_message(sl, NULL);
```

Set the presentation with `sl_set_prompt_theme()`. The element limit,
prefix, palette roles and busy markers are documented on the corresponding
`sl_set_status_*` declarations.

## Build and test

```sh
make build
make test
make test-terminal-cache
make deps DEPENDENCY=libmdf
make deps DEPENDENCY=lua PRESET=debug
make asan
make valgrind
make package-consumer-smoke
make lua-test
make prerelease
```

On native x86_64 Linux, `make test` provisions checksum-pinned GTK/VTE,
Xvfb, keyboard, font and clipboard test tools from the lifecycle's shared
verified archive cache. `make deps-terminal-tests` prepares them independently.
The cache is selected by `CPKT_DEPENDENCY_CACHE`, otherwise
`${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/deps`; every reused archive is
hashed. Repository-local extraction and runtime staging are disposable under
`.cache/deps-build/x86_64-linux-gnu/terminal-tests` and
`.cache/deps/x86_64-linux-gnu/terminal-tests`. No package manager installation
or maintainer scripts run, and none of these tools enter the shipped libraries.

The pinned native tools use a dated official Ubuntu archive snapshot, an explicit
source exception for test tools recorded with versions, URLs and SHA-256 hashes
in `cmake/terminal-tests/archives.json`, including their libc, compiler runtime
and desktop library closure. Provisioning verifies every ELF dependency and
imported libc symbol version is staged. Host GTK/GLib/X11 installs are not
required. Configured CTest tests retain the prepared environment in the build's
CMake cache, including after ordinary reconfiguration by `make build` or
`make run-chat` and for direct `ctest --preset debug` runs. Terminal tools and
the test interpreter run through the cached loader; fixture children, host
build tools and shell utilities retain their own runtime.
The staged Xvfb keyboard compiler lookup is relocated to the staged `PATH`;
the verified archive remains immutable. Font caches and temporary test workspaces
live under `build/terminal-test-tools`. `make test-terminal-cache` checks offline
reconstruction, corrupt archives, interrupted staging, runtime isolation and
rejection of missing runtime libraries or libc symbol versions.
Transient archive transfer failures are retried up to three times; partial or
unverified downloads never become cache entries. Other hosts retain existing
native test-tool discovery.

The compiled example-test runner uses a fresh private history directory under
its configured build directory on every invocation, including direct and
Valgrind runs. It overrides inherited `SOFTLINE_HISTORY_DIR` and does not touch
the caller's normal chat history. These test stores remain available for
diagnosis until `make clean`.

Every project-owned C target is compiled as C89 with POSIX terminal APIs.
For editor setup, run `make build-debug` to prepare the dependencies, generated
headers and `build/debug/compile_commands.json`. The checked-in `.clangd` uses
that database; public-header Doxygen comments supply API hover documentation.
`make test-clangd` verifies configuration/database discovery and semantic parsing
of `src/softline.c`. Lua API usage and callback contracts are documented in
[lua/README.md](lua/README.md) and the facade's source comments.

Shared builds use `SOFTLINE_ABI_VERSION=5` for SONAME/SOVERSION. Image-paste handover
extends the public configuration and receiver layout. Rebuild consumers against
the new header/library pair. ABI 5 advances exactly once from released v0.17.0's
ABI 4, independently of the project release version.
The ABI is fixed by the source, not a CMake option. Reconfiguring an existing
build discards any obsolete cached ABI value before generating library names
and installed metadata.

Builds require no installed cpkt CLI, cpkt SDK or cpkt checkout. The standalone
lifecycle helper scripts are vendored here; their `cpkt-*` filenames and shared
cache namespace follow the lifecycle skill. Source archives include those helpers
and do not need an installed skill. Installed-consumer checks disable cpkt package
discovery and restrict pkg-config lookup to the installed Softline SDK.

Linux debug, sanitizer, Valgrind, package-consumer, and release package
builds use the pinned native GNU Bootlin toolchain. The current pinned Bootlin
compiler executables are x86_64-hosted, so ordinary Linux lifecycle work is
supported on native x86_64 Linux hosts. Unsupported Linux hosts fail closed
rather than selecting host compilers or binutils. The release matrix ships the supported Linux target artifacts
(`x86_64`, `aarch64`, and `armhf`, each GNU and musl) from those pinned
toolchains. Fuzzing remains native `x86_64-linux-gnu` only.
Host `clangd`, `clang-format`, Valgrind, Lua 5.5, LuaRocks, and packaging
utilities are development tools; the C compiler, linker, archiver, and sysroot
for project-owned Linux release builds come from the lifecycle toolchain
resolver.

Local tests, examples, fuzzers, and temporary SDK consumers on supported x86-64
Linux hosts select the
Bootlin ELF interpreter and runtime libraries at link time, including in Release
builds. Run these executables directly. Their private runtime paths also cover
child executables and indirect library dependencies without exporting
`LD_LIBRARY_PATH` to host tools. Shipped SDK libraries and exported metadata do
not contain these local runtime settings.

The release guard inspects every ELF payload by file signature, including nested
archives, and rejects non-system interpreters, non-`$ORIGIN` runtime paths, and
embedded local or Bootlin collection paths. Malformed archives or failed ELF
inspection block verification. `make test-artifact-runtime` exercises positive
and negative packaged fixtures; it is part of `make test-all`.

Lua verification and examples use a local Lua 5.5.1 interpreter built with the
same selected collection. Its checksum-pinned upstream source archive is reused
from the shared dependency cache; disposable extraction state stays under
`.cache/deps-build/`, while the interpreter build remains under
`build/local-lua` (or `build/local-lua-debug` for debug workflows).
LuaRocks remains a host packaging tool. This runtime selection is not hermetic
execution and does not establish compatibility with older deployment libcs.

`make prerelease` performs incremental preparation: formatting, native Debug,
sanitizer, Memcheck, Lua, host, editor and installed-consumer checks, followed by
binary and Lua artifact rehearsals. Memcheck shares the Debug producer;
consumer install variants share matching library objects. `make release-matrix`
is binary-only, with a scoped checksum inventory under `build/package-state/`.
Its ordinary rehearsal may omit Darwin when osxcross is unavailable.
`make verify-release-archives` verifies the existing complete release manifest
and every source/Lua artifact and SDK it names, without regenerating checksums.

Select candidate artifact versions with `SL_VERSION_OVERRIDE`, for example
`SL_VERSION_OVERRIDE=1.2.3 make prerelease`. Preparation must leave release refs
unchanged; do not reserve synthetic patch versions or create rehearsal tags in
the owning checkout. Only the intended final release receives a tag, after the
engineer signs the default-branch commit. A failed final release stops; return
to authorized development instead of inventing another version to retry it.

`make release` is only for the final exact lightweight `vX.Y.Z` tag on a clean,
signed commit attached to the default branch resolved by `origin/HEAD`, without
a candidate version override. Complete implementation and review on one
development branch, then squash onto the local default branch before engineer
amendment/signing. Keep remote refs unchanged during preparation. The gate
verifies the signature, branch, tag and prepared tools before `make clean`,
including that the selected toolchain and dependency caches survive that cleanup,
then builds fresh local proof and all
seven SDK targets. Delivered source supplies the native Release lane; cross
lanes stay in the checkout. All source/Lua products and SDKs must appear in the
complete checksum manifest. Packaged Darwin bytes require target-correct Mach-O
inspection. A failed release stops; repair is a separate iteration.

Only after that complete local gate passes may an explicitly authorized release
push the default-branch commit and its lightweight tag, then upload the exact
checksum-selected local artifacts. Never push to obtain CI feedback. Hosted
native checks are optional diagnostics; local osxcross supplies Darwin release
proof. Publication remains a separate, explicitly authorized operation.

The `macOS tests` GitHub Actions workflow runs on branch pushes and manual
dispatches using a native Apple Silicon macOS runner. It builds the libraries
and examples, then runs the native CTest suite, including PTY handoff,
transcripts, browser host integration and libmdf streaming. X11 tests run
on Linux. The workflow does not publish releases or upload build artifacts.

Builds default to four jobs through `CMAKE_BUILD_PARALLEL_LEVEL`; set it explicitly
to choose another limit. Operational helpers and direct CMake configuration reject
symlinked `build/`, `dist/`, or `.cache/` roots and borrowed cache overrides.
Direct CMake binary directories must also resolve inside this checkout's
`build/`, `.cache/`, or `dist/`; in-source and external builds are rejected.
Cache overrides use this checkout's `build/` or `.cache/`, or the declared global
`${XDG_CACHE_HOME:-$HOME/.cache}/cpkt/{toolchains,deps}` caches. Staging and local
dependency paths must resolve inside this checkout before cleanup or extraction.
Source reconstruction (`make package-source-smoke` and final release) requires
the declared shared cpkt caches: checkout-local overrides would belong to the
parent checkout and cannot be reused by the extracted source. Incompatible
cache selections fail before extraction or release cleanup.

osxcross discovery selects the newest complete `arm64-apple-darwin25.x`
compiler prefix, including installations with only dotted executable names.
`OSXCROSS_ROOT` selects the installation; `CPKT_OSXCROSS_HOST` pins an exact
prefix and fails if its tools are missing instead of selecting another prefix.

Linux clipboard protocol tests run without an X server. They check request and
reply sequence wraparound, I/O failures, final property deletion for direct and
INCR transfers, and byte preservation for a 17 MiB PNG transferred in small
INCR chunks. Short test deadlines cover silent peers, truncated replies, missing
selection/chunk/terminator events and continuous unrelated events/replies.
The optional X11 tests also exercise real clipboard owners and forwarded
displays.

Package smoke links fully static GNU and musl consumers with fatal linker
warnings. Linux X11 hostname lookup uses privately bundled MIT-licensed c-ares
1.34.9 (with its BSD-3-Clause sorting helper) in both static and shared libraries:
literal IPv4/IPv6 addresses (including IPv6 interface qualifiers), `/etc/hosts`,
then DNS configured by `/etc/resolv.conf` (including search
domains). It does not use libc NSS or load hostname modules. No additional
library is needed when linking `libsoftline.a`. Resolver symbols are private
and namespaced so callers may also link their own c-ares. Source provenance,
the small numeric-service patch, and the license are in `vendor/c-ares/`;
Linux SDKs install notices and provenance under `share/softline/licenses/c-ares/`.
Resolver sources are checked in; configuring/building never downloads c-ares.

Source archive smoke extracts and builds under the repository's `build/`,
independently of the caller's working directory or `TMPDIR`, and removes its
workspace on ordinary success or failure. The terminal/job runner owns the
whole operation and its cancellation; helpers stay in the foreground.

`cmake/softline.exports` is the source-controlled dynamic export contract for
`libsoftline`. Build and extracted-package checks fail if a public symbol is
missing or an accidental symbol becomes linkable. The lifecycle migration record
is maintained in [docs/lifecycle-migration.md](docs/lifecycle-migration.md).

Version/tag variations are tested in disposable Git fixtures without changing
active release refs. The signed release fixtures require Git SSH signing support
and `ssh-keygen`; their temporary keys and trust files stay under ignored `build/`.
No release version is reserved for a lifecycle test. The
Lua facade supports Lua 5.5 only; LuaRocks builds must select Lua 5.5.

Installed CMake consumers should use the canonical imported target:

```cmake
find_package(softline REQUIRED CONFIG)
target_link_libraries(app PRIVATE softline::softline)
```

Plain C consumers can use the installed pkg-config metadata:

```sh
eval "$(./scripts/cpkt-toolchains.sh env x86_64-linux-gnu)"
"$CC" $(pkg-config --cflags softline) app.c $(pkg-config --libs softline)
```

Lua 5.5 consumers can use the Lua facade from the LuaRocks package:

```lua
local softline = require("softline")
local sl = softline.new()
local line = sl:readline("softline> ")
sl:close()
```

For local development:

```sh
make lua-test
softline_lua_env="$(make lua-env)" && eval "${softline_lua_env}"
lua examples/simple.lua
```

To run the Lua facade and examples against the in-tree debug library:

```sh
make lua-debug-test
make lua-debug-simple
make lua-debug-chat
make lua-debug-chat THEME=riced
```

## Lineage

softline no longer vendors linenoise as a separate source file, but its design
and some code are derived from linenoise. The inherited BSD 2-Clause notice is
included in `LICENSE`.

## Persistent prompt history

Recording is opt-in. Attach a native store with a stable application key,
then choose manual `sl_history_add()` or automatic recording at acceptance.

```c
if (sl_history_open(sl, "my-app:project-id", NULL) == SL_OK)
  sl_history_set_auto_add(sl, 1);
```

The default native store is under `$XDG_STATE_HOME/softline/history`, falling
back to `$HOME/.local/state`. Use `sl_history_set_backend()` for custom
storage, or `sl_history_save()` and `sl_history_load()` for explicit
snapshots. See [history storage and recording](docs/softline-spec.md#history-storage-and-recording).
