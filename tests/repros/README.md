# Regression reproductions

These tests assert the expected behavior and fail on the implementation they
were written against. They run on Linux with the static library and use
tmux for assertions about terminal cells. The default suite excludes them
until the corresponding fixes land.

```sh
cmake --preset debug -DSOFTLINE_TEST_KNOWN_REGRESSIONS=ON
cmake --build --preset debug --target test_native_regressions native_output_fixture
ctest --test-dir build/debug -L '^known-regression$' --output-on-failure
cmake --preset debug -DSOFTLINE_TEST_KNOWN_REGRESSIONS=OFF
```

While enabled, select one case with
`ctest --test-dir build/debug -R '^softline_repro_grow-cleared-history$' --output-on-failure`.
The fixture reports setup errors separately from failed assertions.

## Resize and handoff review

| Case | Behavior checked |
| --- | --- |
| `grow-cleared-history` | After clearing tmux history and growing from 8 to 12 rows, the active prompt, draft, caret, and prior output remain aligned. |
| `clipped-handoff-reopen` | After a height shrink clips the producer endpoint and a handoff write returns EAGAIN, the stream reopens at the same size once the terminal is writable. |
| `relay-after-close` | Output written after a relay returns starts separately from the child’s unfinished line, even when the prior stream closed before relay entry. |
| `zero-width-wrap` | A combining mark at a full producer row survives height growth. The following LF advances exactly one row. |

## Additional failures

| Case | Behavior checked |
| --- | --- |
| `disconnected-stream-close` | Closing after losing a separate output TTY reports I/O failure and restores the surviving input TTY. |
| `disconnected-idle-relay` | An idle relay detects loss of its output TTY and ends without repeatedly calling STEP. |
| `raw-enter` | Enter submits the draft when the TTY previously used IGNCR. Original terminal settings are restored. |
| `quote-without-onlcr` | With ONLCR disabled, a multiline quote still starts at column zero on each row. |
| `utf8-following-key` | A malformed UTF-8 lead byte leaves the next printable keystroke available. |
| `literal-paste-nul` | A NUL inside bracketed paste cannot cause silent submission of only the prefix. |
| `split-crlf` | CR and LF arriving on opposite sides of a readline return do not create an extra empty submission. |
| `history-record-boundary` | A record ending in a single backslash leaves the next imported history record intact. |
| `history-recall-after-eviction` | Adding history at capacity while browsing leaves Up able to reach the previous surviving entry. |
| `insert-borrowed-buffer` | Inserting a suffix borrowed from the current draft preserves its original bytes. |
| `insert-cluster-boundary` | Inserting a base letter before a leading combining mark leaves the caret after the newly joined cluster. |
