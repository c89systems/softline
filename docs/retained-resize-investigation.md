# Retained-frame resize regression

This is the historical investigation of the reconciliation repair first shipped
in v0.11.0. Its original ABI and test counts below are historical evidence, not
current release proof. The current API uses ABI 4; see the
[ABI contract](softline-mdf-stream-design.md#shared-library-abi).

The standalone C fixture and real tmux cell assertions reproduce the reported
stale status suffix after a settled resize between output writes. The fixture
links only Softline, with no renderer, provider or adjacent checkout dependency.
Its producer acknowledges a FIFO barrier after all 500 partial-line characters
have been emitted. The driver verifies those cells before resizing and releasing
the next write, excluding a resize racing the producer.

The repair is `sl_output_stream_reconcile()`, called on the owner thread after
resize settles and before synchronous output resumes. It observes and repaints
the owned frame without entering/cancelling readline, changing input state,
waiting inside feeds or retaining/replaying transcript bytes. Check its result:
a missing cursor report returns `SL_ERROR_IO`. C receiver/free-function and Lua
methods are equivalent. The repair originally required rebuilding consumers for
ABI 1, one increment from released v0.10.0's ABI 0, because the receiver gained
a method. Current consumers must use the current matching header/library pair.

Run the required regressions with the prepared native toolchain and tmux:

```sh
cmake --preset debug
cmake --build --preset debug --target retained_resize_fixture test_reconcile
ctest --test-dir build/debug -R '^(softline_retained_.*|softline_reconcile|softline_feed_latency)$' --output-on-failure --no-tests=error
```

These tests cover partial/complete ASCII producer lines, shrink/growth, custom
prompts, status, early/late drafts, watch/yield controls and all 120 original
producer rows appearing unchanged exactly once. PTY protocol checks cover draft
and cursor, queue, history, readline status, terminal modes, type-ahead before a
cursor reply, split UTF-8/SGR, missing replies, retry and off-TTY forwarding.
The feed-latency gate continues to reject any cursor request inside a feed.

For diagnosis of callers omitting reconciliation, the standalone suite remains:

```sh
cmake -S cmake/retained-resize-tests -B build/retained-resize-tests -G Ninja \
  -DSOFTLINE_RESIZE_FIXTURE="$PWD/build/debug/tests/retained_resize_fixture"
ctest --test-dir build/retained-resize-tests --output-on-failure --no-tests=error
```

This explicitly selected diagnostic suite includes raw, unobserved writes that
still return failure under the documented fallback limitation. Cases are neither
disabled nor inverted with `WILL_FAIL`; repaired cases are required in the main
suite. The `retained-observed` fixture now invokes the production reconciliation
API directly, rather than the experimental readline/cancel step.
Failed cases retain before/after-resize cells, final screen and scrollback under
the printed owned build directory. Successful cases remove their scratch.

## Observations before the fix (9f5ce33)

| Case | Result on the unchanged library |
| --- | --- |
| Retained, partial line, 120x30 to 60x18 | Fails: `* Edited snake.cize reproduction` |
| Retained, complete line, 120x30 to 60x18 | Fails: first patch row contains stale status text |
| Retained, partial line, width-only shrink to 60x30 | Fails: contaminated heading |
| Retained, partial line, height-only shrink to 120x18 | Passes |
| Watch/yield ownership, partial/complete lines, early/late draft controls | Four cases pass |
| Input-owned step before continuation, each of the three failing variants | Three cases pass |

The complete diagnostic inventory is eight passing controls and three genuine
failures. Existing feed-latency, native-resize and tmux producer-resize checks
also passed (3/3); that reproduction commit changed no library source or API.

## Mechanism

`sl_output_stream_write_method` sets `output_write_active` before
`sl_prepare_native_output` reconciles retained geometry. `sl_query_cursor_row`
refuses observations while that flag is set, preserving the zero-wait feed
contract. The fallback in `sl_native_reconcile_rendered_frame` therefore estimates
where retained status/input cells moved. A width shrink can expose stale status
suffixes in subsequent producer rows, even without a simultaneous height change.
This localizes the path; it does not prove one arithmetic expression is the cause.

An experimental control re-enters the existing input owner through `sl_readline`
and cancels that input step before continuing output. All three failing variants
then pass. This establishes an input-owned reconciliation/repaint as a promising
repair path, not a production workaround or proof that a cursor query alone is
sufficient.

The implemented operation follows this owner-thread boundary. An already-open
`sl_output_stream_begin` continues to return `SL_ERROR_INVALID`. Physical resizes
racing a write and terminal-specific Unicode/tab reflow ambiguities retain their
documented limits. CAI's dependency update and adoption of the new call are
separate work outside this repository.

## Original repair verification

The completed repair passed the full native suite (57/57), host/API checks
(24/24), ASan/UBSan (14/14), Lua (16/16), and installed static/shared/both
CMake and pkg-config consumers, including lib64. The installed consumer calls
both forms of the reconciliation API. SONAME is `libsoftline.so.1`; generated
package metadata reports ABI 1. Formatting and diff checks also pass.

Original logs used `build/retained-resize-{native,host,asan,lua,consumers}.log`.
These generated logs are disposable and removed by `make clean`.
Old sanitizer and consumer build caches referenced the retired toolchain root;
only those owned build directories were moved aside before reconfiguration.
No release/tag/publish operation is part of this repair.
