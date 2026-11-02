# Makefile for softline - multiline readline replacement derived from linenoise
#
# Lifecycle spine: deps -> configure -> build -> test -> hardening -> package -> verify -> release

SHELL := bash
.SHELLFLAGS := -euo pipefail -c
.NOTPARALLEL:
export CMAKE_BUILD_PARALLEL_LEVEL ?= 4

ROOT_DIR := $(CURDIR)
BUILD_DIR := $(ROOT_DIR)/build
DIST_DIR  := $(ROOT_DIR)/dist
CACHE_DIR := $(ROOT_DIR)/.cache

PRESET ?= debug
CHAT_APPROVAL_DEMO ?= 1
CHAT_RECONNECT_MS ?= 2000
OSC8 ?= auto
CHAT_DEMO_ENV = SOFTLINE_CHAT_APPROVAL_DEMO="$(CHAT_APPROVAL_DEMO)" SOFTLINE_CHAT_RECONNECT_MS="$(CHAT_RECONNECT_MS)" SOFTLINE_OSC8="$(OSC8)"
CHAT_QUEUE ?= local
QUEUE_HOST ?= 127.0.0.1
QUEUE_PORT ?= 7635
ifeq ($(CHAT_QUEUE),local)
CHAT_COMMAND = ./build/debug/examples/example_chat
else ifeq ($(CHAT_QUEUE),remote)
CHAT_COMMAND = ./build/debug/examples/example_chat_remote "$(QUEUE_HOST)" "$(QUEUE_PORT)"
else
$(error CHAT_QUEUE must be local or remote)
endif

ifneq ($(strip $(THEME)),)
EXAMPLE_THEME_ENV := SOFTLINE_PROMPT_THEME="$(THEME)"
endif

.PHONY: help
help: ## Show this help
	@echo "softline -- C89 multiline readline replacement derived from linenoise"
	@echo ""
	@echo "Lifecycle targets:"
	@grep -E '^[a-zA-Z0-9_-]+:.*?## .*$$' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-30s\033[0m %s\n", $$1, $$2}'

.PHONY: format format-check
format: ## Format project-owned C sources with host clang-format
	@./scripts/format.sh

format-check: ## Assert formatting without changing source
	@./scripts/format.sh check

.PHONY: deps-debug
deps-debug: ## Configure debug build dependencies
	@cmake --preset debug

.PHONY: deps-release
deps-release: ## Configure release build dependencies
	@cmake --preset x86_64-linux-gnu-release

.PHONY: deps-cross
deps-cross: ## Provision and inspect all pinned cross toolchains
	@source ./scripts/require-workspace.sh && ./scripts/cpkt-toolchains.sh ensure all

.PHONY: deps
deps: ## Configure one dependency closure (DEPENDENCY=libmdf|lua PRESET=debug)
	@./scripts/deps.sh "$(DEPENDENCY)" "$(PRESET)"

.PHONY: build
build: ## Build debug target
	@./scripts/build.sh build debug

.PHONY: build-debug
build-debug: build ## Build debug target

.PHONY: run-simple
run-simple: build-debug ## Run C simple example (THEME=default|plain|...)
	@$(EXAMPLE_THEME_ENV) ./build/debug/examples/example_simple

.PHONY: run-chat
run-chat: build-debug ## Run C chat example (Gruvbox by default; THEME=... overrides)
	@SOFTLINE_PROMPT_THEME="$(if $(strip $(THEME)),$(THEME),gruvbox)" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-local run-chat-remote run-chat-queue-server
run-chat-local: ## Run chat with the native local queue
	@$(MAKE) run-chat CHAT_QUEUE=local

run-chat-remote: ## Attach chat to the separate TCP queue owner
	@$(MAKE) run-chat CHAT_QUEUE=remote

run-chat-queue-server: build-debug ## Run volatile queue owner on loopback (QUEUE_PORT=7635)
	@$(CHAT_DEMO_ENV) ./build/debug/examples/example_queue_owner "$(QUEUE_PORT)"

.PHONY: run-chat-default
run-chat-default: build-debug ## Run C chat example with the default ANSI theme
	@SOFTLINE_PROMPT_THEME="default" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-riced
run-chat-riced: build-debug ## Run C chat example with the riced theme
	@SOFTLINE_PROMPT_THEME="riced" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-without-delay
run-chat-without-delay: build-debug ## Run default chat with no simulated producer delay
	@SOFTLINE_CHAT_CHAR_MS=0 SOFTLINE_PROMPT_THEME=default $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-without-delay-riced
run-chat-without-delay-riced: build-debug ## Run riced chat with no simulated producer delay
	@SOFTLINE_CHAT_CHAR_MS=0 SOFTLINE_PROMPT_THEME=riced $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-plain
run-chat-plain: build-debug ## Run C chat example with the uncoloured plain theme
	@SOFTLINE_PROMPT_THEME="plain" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-monogreen
run-chat-monogreen: build-debug ## Run C chat example with the monogreen theme
	@SOFTLINE_PROMPT_THEME="monogreen" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-monochrome
run-chat-monochrome: build-debug ## Run C chat example with the monochrome theme
	@SOFTLINE_PROMPT_THEME="monochrome" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: run-chat-synthwave
run-chat-synthwave: build-debug ## Run C chat example with the synthwave theme
	@SOFTLINE_PROMPT_THEME="synthwave" $(CHAT_DEMO_ENV) $(CHAT_COMMAND)

.PHONY: build-release
build-release: ## Build release target
	@cmake --preset x86_64-linux-gnu-release
	@cmake --build --preset x86_64-linux-gnu-release

.PHONY: test
test: ## Run debug tests with cached native terminal tools
	@./scripts/test.sh

.PHONY: test-native
test-native: ## Run debug tests in the prepared test environment
	@./scripts/build.sh test debug

.PHONY: deps-terminal-tests
deps-terminal-tests: ## Cache pinned native GTK/VTE/Xvfb test tools without host installation
	@./scripts/terminal-test-env.sh

.PHONY: test-terminal
test-terminal: ## Run native debug tests with lifecycle-cached terminal test tools
	@$(MAKE) test

.PHONY: test-debug
test-debug: test ## Run debug tests

.PHONY: test-all
test-all: test test-host asan valgrind-portable fuzz-portable lua-test ## Complete missing native, host, hardening, fuzz and Lua proof

.PHONY: test-host
test-host: ## Run host lifecycle fixtures
	@./scripts/test-host.sh

.PHONY: test-terminal-cache
test-terminal-cache: ## Verify offline reconstruction of cached terminal test tools
	@./scripts/test-host.sh -R "^(terminal_cache)$$"

.PHONY: test-lua-platform
test-lua-platform: ## Verify local Lua builds with Darwin platform settings
	@./scripts/test-host.sh -R "^(lua_platform)$$"

.PHONY: test-lua-env
test-lua-env: ## Verify Lua environments reject missing local interpreters
	@./scripts/test-host.sh -R "^(lua_env|lua_runtime_selection)$$"

.PHONY: test-artifact-runtime
test-artifact-runtime: ## Reject local runtime paths in release artifacts
	@./scripts/test-host.sh -R "^(artifact_portability|artifact_runtime)$$"

.PHONY: test-runtime-config
test-runtime-config: ## Verify missing-runtime and archive cache failures
	@./scripts/test-host.sh -R "^(runtime_config)$$"

.PHONY: asan
asan: ## Run the selected ASan+UBSan core tests
	@./scripts/build.sh test asan

.PHONY: asan-native
asan-native: ## Run ASan+UBSan in the prepared test environment
	@./scripts/build.sh test asan

.PHONY: valgrind
valgrind: ## Run native C executable CTests under Valgrind Memcheck
	@./scripts/valgrind.sh

.PHONY: valgrind-portable
valgrind-portable: ## Run native Valgrind on supported Linux hosts, skip where unsupported
	@case "$$(uname -s):$$(uname -m)" in \
		Linux:x86_64|Linux:amd64) $(MAKE) valgrind ;; \
		*) echo "SKIP: valgrind requires native x86_64 Linux" ;; \
	esac

.PHONY: deps-fuzz
deps-fuzz: ## Explicitly provision pinned native AFL++ instrumentation
	@source ./scripts/require-workspace.sh && ./scripts/cpkt-aflpp.sh ensure

.PHONY: fuzz-smoke
fuzz-smoke: ## Build and execute a bounded native AFL++ smoke run (prepare with deps-fuzz)
	@./scripts/fuzz.sh

.PHONY: fuzz-portable
fuzz-portable: ## Run native fuzz smoke on x86_64 hosts, skip where unsupported
	@case "$$(uname -s):$$(uname -m)" in \
		Linux:x86_64|Linux:amd64) $(MAKE) fuzz-smoke ;; \
		*) echo "SKIP: fuzz-smoke requires native x86_64 Linux" ;; \
	esac

.PHONY: fuzz
fuzz: fuzz-smoke ## Run the standard bounded native AFL++ fuzz gate

.PHONY: fuzz-long
fuzz-long: ## Run a longer opt-in native AFL++ fuzz session
	@./scripts/fuzz.sh long

.PHONY: lua-rock
lua-rock: ## Build and install Lua facade into repo-local LuaRocks tree
	@./scripts/lua-test.sh

.PHONY: lua-test
lua-test: ## Run Lua facade smoke tests
	@./scripts/lua-test.sh

.PHONY: lua-test-lib64
lua-test-lib64: ## Run Lua facade smoke tests with a lib64 SDK install
	@SOFTLINE_LUA_INSTALL_LIBDIR=lib64 ./scripts/lua-test.sh

.PHONY: lua-env
lua-env: ## Print shell exports for repo-local Lua facade
	@./scripts/lua-env.sh

.PHONY: lua-debug-test
lua-debug-test: ## Run Lua facade and examples against build/debug/libsoftline
	@./scripts/lua-debug.sh test

.PHONY: lua-debug-env
lua-debug-env: ## Print shell exports for Lua facade against build/debug/libsoftline
	@./scripts/lua-debug.sh env

.PHONY: lua-debug-simple
lua-debug-simple: ## Run examples/simple.lua against build/debug/libsoftline
	@$(EXAMPLE_THEME_ENV) ./scripts/lua-debug.sh simple

.PHONY: lua-debug-chat
lua-debug-chat: ## Run examples/chat.lua against build/debug/libsoftline
	@$(EXAMPLE_THEME_ENV) ./scripts/lua-debug.sh chat

.PHONY: package
package: ## Build release packages
	@./scripts/package.sh

.PHONY: package-checksums
package-checksums: ## Generate the selected binary inventory under build/
	@./scripts/package-checksums.sh binary

.PHONY: package-verify
package-verify: package-checksums ## Verify release packages
	@SOFTLINE_CHECKSUM_FILE="$(BUILD_DIR)/package-state/softline-$$(./scripts/release_version.sh)-BINARY-CHECKSUMS" ./scripts/package-verify.sh

.PHONY: package-source
package-source: ## Create source archive
	@./scripts/package-source.sh

.PHONY: package-source-smoke
package-source-smoke: package-source ## Verify source archive builds
	@./scripts/package-source-smoke.sh

.PHONY: package-consumer-smoke
package-consumer-smoke: ## Verify installed CMake package from an external consumer
	@./scripts/package-consumer-smoke.sh

.PHONY: test-tool-discovery
test-tool-discovery: ## Verify cross-target tool discovery
	@./scripts/test-host.sh -R "^(tool_discovery)$$"

.PHONY: test-darwin-linker-route
test-darwin-linker-route: ## Verify osxcross Darwin links use the target linker
	@./scripts/test-host.sh -R "^(darwin_linker_route)$$"

.PHONY: test-release-version
test-release-version: ## Verify release version source precedence
	@./scripts/test-host.sh -R "^(release_version)$$"

.PHONY: test-package-source-worktree
test-package-source-worktree: ## Verify source packaging from a linked Git worktree
	@./scripts/test-host.sh -R "^(package_source_worktree)$$"

.PHONY: test-toolchain-contract
test-toolchain-contract: ## Verify toolchain provisioning and environment contracts
	@./scripts/test-host.sh -R "^(toolchain_contract)$$"

.PHONY: test-lifecycle-surface
test-lifecycle-surface: ## Verify standard lifecycle command and preset surfaces
	@./scripts/test-host.sh -R "^(lifecycle_surface|deps_target|source_workspace)$$"

.PHONY: test-clangd
test-clangd: deps-debug ## Verify clangd project configuration and semantic parsing
	@./scripts/test_clangd.sh

.PHONY: test-public-header-docs
test-public-header-docs: ## Verify public headers have API documentation comments
	@./scripts/test-host.sh -R "^(public_header_docs)$$"

.PHONY: release-lua-artifacts
release-lua-artifacts: ## Build Lua source package, release rockspec, and source rock
	@./scripts/release_lua_artifacts.sh

.PHONY: validate-luarocks
validate-luarocks: ## Verify LuaRocks release artifacts
	@./scripts/validate_luarocks.sh

.PHONY: test-lua-artifact-privacy
test-lua-artifact-privacy: ## Verify Lua release artifacts fail on local path leaks
	@./scripts/test-host.sh -R "^(lua_artifact_privacy)$$"

.PHONY: verify-release-archives
verify-release-archives: ## Verify the complete release manifest and all archives
	@SOFTLINE_CHECKSUM_FILE= ./scripts/package-verify.sh

.PHONY: verify-release-privacy
verify-release-privacy: ## Scan release artifacts for local paths
	@./scripts/verify-release-privacy.sh

.PHONY: release-matrix
release-matrix: ## Build, package, checksum, and verify all release targets
	@./scripts/run_linux_release_matrix.sh

.PHONY: finalize-slice
finalize-slice: format test format-check ## Pre-commit gate: format + debug tests

.PHONY: prerelease
prerelease: prerelease-checks release-matrix release-lua-artifacts validate-luarocks ## Incremental preparation and binary/Lua rehearsal

.PHONY: prerelease-hardening
prerelease-hardening: prerelease ## Run the deterministic preparation gate

.PHONY: release
release: ## Final tagged release only: clean, reconstruct source, verify complete artifacts
	@$(MAKE) lifecycle-version-contract
	@./scripts/release.sh preflight
	@$(MAKE) clean
	@./scripts/release.sh

.PHONY: prerelease-checks prerelease-runtime-checks
prerelease-checks: prerelease-runtime-checks package-consumer-smoke
prerelease-runtime-checks: format-check test-all test-clangd

.PHONY: lifecycle-version-contract
lifecycle-version-contract: ## Verify exact lightweight-tag release version behavior
	@./scripts/lifecycle-version-contract.sh

.PHONY: print-release-version
print-release-version: ## Print the current release version
	@./scripts/release_version.sh

.PHONY: clean
clean: ## Remove all generated build and dist artifacts
	@./scripts/clean.sh

.PHONY: clean-dist
clean-dist: ## Remove dist/ artifacts only
	@./scripts/clean.sh dist
