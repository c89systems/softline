# Lifecycle Migration Ledger

Softline uses the native pkt.systems C/CMake lifecycle. This ledger records the
implemented contract; successful release evidence comes from the actual tagged
release run and its final checksum-selected artifact bytes.

## Commands and ownership

Make exposes short public recipes. Bash sequences concrete operations in the
foreground. CMake owns configuration, source/data generation, build and install;
CTest owns registered test execution and results. One operation owns each mutable
workspace. Sources, tool collections and configuration stay stable during checks;
ordinary timestamp changes invalidate native build dependencies. Restore/copy
operations or changed execution context require explicit invalidation.

The terminal or job runner cancels the entire operation/job. Parent-PID-only
signals and concurrent in-place changes are outside the lifecycle interface.
There is no helper supervisor, receipt service or active-tag recovery protocol.
Generated scratch stays under `build/`; `make clean` removes local generated
state and `dist/` while preserving shared verified caches.

Workspace checks reject borrowed generated roots, nested staging symlinks and
cache overrides outside owned local state or the declared global cpkt cache.
Direct CMake binary directories use those same owned generated areas, including
when reached through a source-path alias; external and in-source builds fail.
`make clean-dist` uses the same anchored cleanup helper and preserves other local
state; full clean removes hidden entries too. Builds default to four jobs and
honor an explicit `CMAKE_BUILD_PARALLEL_LEVEL`.

## Tool and test cutover

- PATH-selected Bash is at least 4.4. Linux compilation uses complete pinned
  Bootlin collections without host compiler/binutils/libc/header fallback.
  Discovery/configuration consume prepared tools; explicit `ensure` provisions.
- Shared defaults are `~/.cache/cpkt/toolchains` and `~/.cache/cpkt/deps`, with
  explicit cache overrides. Verified archive bytes are reusable by digest across
  naming/URL aliases. Local dependency extraction stays under `.cache/`.
- Project C targets use C89 and warnings as errors. Vendored upstream Lua/c-ares
  keep their selected upstream compilation boundary and recorded licenses.
- Python only generates the private resolver namespace or image inputs, or supplies
  small HTTP/DNS inputs. Test assertions and orchestration use compiled fixtures
  and small Bash/CMake checks. API/documentation checks inspect sources directly
  in CMake; no Python analysis or generated test inventory sits behind them.
  Lua API semantics execute in the selected runtime.
- Debug and native Memcheck share compilation. Instrumented checks retain their
  explicit timeout profile. ASan/UBSan and AFL++ own distinct effective flags.
  Lua SDK installation uses that same Debug library producer. Its facade requires
  the public shared library; static-only SDKs fail before compilation.
  Installed consumer variants reuse matching library objects and inspect real
  CMake/pkg-config imports, ABI metadata and execution.
- Export maps use unchanged-content native generation. The private resolver
  namespace is a CMake-declared output with explicit parallel build dependencies.
  `cmake/softline.exports` is the public dynamic boundary, checked on built and
  extracted shared libraries, including accidental data exports.

## Delivery and final release

Preparation is incremental. `make prerelease` combines missing local and binary/
Lua rehearsal gates; source reconstruction is separately available through
`make package-source-smoke`. Binary rehearsals write a selected inventory under
`build/package-state/`, preserving unrelated identified artifacts.
`make verify-release-archives` instead verifies the existing complete release
manifest without regenerating checksums or narrowing its artifact selection.

`make release` is the final exact lightweight `vX.Y.Z` tagged gate on the signed,
attached default branch resolved through `origin/HEAD`. Implementation and review
use one development branch; squash the verified change onto the local default
branch before engineer amendment/signing. Its read-only signature/branch/tag/tool
preflight runs before the retained `make clean` and rejects toolchain or dependency
caches that cleanup would delete. Source smoke likewise rejects checkout-local
cache overrides before extracting a separate source workspace. Candidate overrides,
dirty worktrees and partial inventory selectors cannot enter that gate. Git tag
variations are checked only in disposable fixture repositories.

Final release starts once from clean local state, shares matching producers and
runs fresh required proof. Its delivered source reconstruction supplies the native
Release lane; cross configurations remain in the owning checkout. The final
installed-consumer matrix reuses that reconstructed Release producer; preparation
retains its separate consumer workspace. Explicit test commands run CTest;
matching preparation results are reused without another result-cache service.
CMake install stages the public SDK under `build/`, and deterministic archives
normalize ordering, timestamps, ownership, gzip/ZIP metadata and intentional modes.

The complete checksum inventory contains four source/Lua artifacts plus all seven
SDK targets. Required payloads must exist and verify; stale/unlisted release-looking
files are rejected. Verification inspects extracted layout, version, public exports,
private dependency notices, ELF/Mach-O metadata, relocatable imports and real
installed static/shared consumer links on every target. Header, CMake, pkg-config
and package-metadata release versions must agree. SDK inventories reject extra
or missing public files and empty private/generated directories. Bundled-dependency
metadata must match the declared target closure. Publication is a separate
explicitly authorized operation. Release failure stops; repairs do not
retag/rebuild/publish inside it.
Default-branch and release-tag pushes remain prohibited until the signed,
tagged commit passes the entire local gate. Hosted builds are optional diagnostics
and must never be triggered by pushing for feedback.

## Product boundaries preserved

libmdf remains an example/integration-test dependency, outside `libsoftline` and
its SDK imports. Private bundled c-ares remains Linux resolver implementation;
there is no inferred cpkt SDK dependency. Lifecycle work preserves public C/Lua
behavior, selected targets and the artifact set. Owner reconciliation first
shipped in v0.11.0 with ABI 1. The current development ABI is 4: constructor-selected
queue backends extend released v0.15.1's ABI 3. C and Lua consumers must rebuild
against matching headers and libraries. Cross
execution and hosted native checks are not implicitly opted in; cross lanes prove
build/link/package metadata, with runtime execution deferred where no selected
native runner exists.

## Installed skill synchronization

The lifecycle helper files and shared resolver fixtures are vendored from the
installed skill. `scripts/cpkt-toolchains.sh` carries local Darwin completeness
fixes: candidates require executable `install_name_tool`, and `osxcross-conf`
must report an existing SDK. The resolver supplies that SDK to CMake, so optional
packaging and final-release preflight use the same availability decision.
The other four helpers and shared resolver fixtures remain exact copies.
Softline's native-host policy lives in
`scripts/native-linux-target.sh`; its Darwin adapter consumes the resolver's
validated SDK and matching installation's target tools.
There is no local fork of the shared resolver interface. Python remains limited
to the declared generators and small input servers described above.
The vendored helpers execute independently of an installed cpkt command, SDK,
checkout or lifecycle skill. Installed-consumer proof rejects cpkt CLI use and
disables cpkt package discovery. Project options use Softline names or native
CMake variables; the toolchain helpers retain their upstream environment contract.
