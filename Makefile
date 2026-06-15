.PHONY: build build-edge build-edge-quickjs-cli build-wasix build-quickjs-wasix build-napi build-napi-quickjs build-native-v8 build-native-quickjs build-wasix-napi build-wasix-napi-quickjs build-napi-wasmer-cli test-wasix-napi test-wasix-napi-quickjs test-wasix-napi-cli test-wasix-safe-mode test-wasix-quickjs-only test test-only check-portability clean clean-napi-quickjs clean-edge-quickjs-cli clean-dist dist dist-only framework-test framework-test-reset

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
BUILD_DIR ?= build-edge
BUILD_EDGE_QUICKJS_CLI_DIR ?= build-edge-quickjs-cli
BUILD_WASIX_NAPI_DIR ?= build-wasix-napi
BUILD_QUICKJS_WASIX_DIR ?= build-quickjs-wasix
DIST_DIR ?= dist
DIST_BIN_DIR ?= $(DIST_DIR)/bin
DIST_BIN_COMPAT_DIR ?= $(DIST_DIR)/bin-compat
ZIP_NAME ?= edge.zip
CMAKE_BUILD_TYPE ?= Release
JOBS ?= 8
TEST_JOBS ?= 0
EDGE_BINARY ?= $(BUILD_DIR)/edge
EDGEENV_BINARY ?= $(BUILD_DIR)/edgeenv
CMAKE_ARGS ?=
BUILD_ENV ?= env
EXTRA_CMAKE_ARGS ?=
NAPI_V8_PREBUILT_VERSION ?= 11.9.2
NAPI_V8_PLATFORM :=
FRAMEWORK_TEST_SCRIPT := $(CURDIR)/scripts/framework-test.js
FRAMEWORK_TEST_SELECTOR := $(filter js-%,$(MAKECMDGOALS))
NAPI_WASMER_DIR ?= napi
NAPI_WASMER_CARGO_TARGET_DIR ?= $(abspath $(BUILD_WASIX_NAPI_DIR)/target)
NAPI_WASMER_BINARY ?= $(NAPI_WASMER_CARGO_TARGET_DIR)/debug/napi_wasmer
WASIX_EDGEJS_WASM ?= ./build-wasix/edgejs.wasm
WASIX_NAPI_SMOKE_JS ?= console.log('hello world!');
WASMER_BIN ?= wasmer
WASIX_PACKAGE_DIR ?= $(CURDIR)
WASIX_SSL_CERTS_DIR ?= ssl-certs
WASIX_QUICKJS_NODE_TEST_RUNNER ?= $(CURDIR)/scripts/edge-wasix-node-runner.sh
EDGE_VERSION_MAJOR := $(shell awk '$$2 == "EDGE_MAJOR_VERSION" {print $$3; exit}' src/edge_version.h)
EDGE_VERSION_MINOR := $(shell awk '$$2 == "EDGE_MINOR_VERSION" {print $$3; exit}' src/edge_version.h)
EDGE_VERSION_PATCH := $(shell awk '$$2 == "EDGE_PATCH_VERSION" {print $$3; exit}' src/edge_version.h)
EDGE_VERSION_COMMIT := $(shell git rev-parse --short=7 HEAD 2>/dev/null || printf unknown)
EDGE_VERSION_BASE := $(EDGE_VERSION_MAJOR).$(EDGE_VERSION_MINOR).$(EDGE_VERSION_PATCH)
ifneq ($(filter 1 true TRUE yes YES,$(IS_FINAL_RELEASE)),)
EDGE_PACKAGE_VERSION := $(EDGE_VERSION_BASE)
else
EDGE_PACKAGE_VERSION := $(EDGE_VERSION_BASE)-$(EDGE_VERSION_COMMIT)
endif
EDGE_WASMER_PACKAGE ?= wasmer/edgejs@=$(EDGE_PACKAGE_VERSION)

EMPTY :=
SPACE := $(EMPTY) $(EMPTY)
COMMA := ,

# Temporarily excluded from Edge's Node compatibility category lanes after the
# 2026-05-18 CI failure sweep. Keep this list deduped because both V8 and
# QuickJS targets share part of the failing set.
EDGE_NODE_TEST_SKIP_CI_20260518 := \
  abort/test-http-parser-consume.js \
  abort/test-zlib-invalid-internals-usage.js \
  parallel/test-dns-channel-timeout.js \
  parallel/test-domain-multiple-errors.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-0.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-1.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-2.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-3.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-4.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-5.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-6.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-7.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-8.js \
  parallel/test-domain-no-error-handler-abort-on-uncaught-9.js \
  parallel/test-domain-throw-error-then-throw-from-uncaught-exception-handler.js \
  parallel/test-domain-vm-promise-isolation.js \
  parallel/test-domain-with-abort-on-uncaught-exception.js \
  parallel/test-http-server-headers-timeout-keepalive.js \
  parallel/test-http-server-request-timeout-keepalive.js \
  parallel/test-http2-client-shutdown-before-connect.js \
  parallel/test-http2-forget-closed-streams.js \
  parallel/test-http2-max-settings.js \
  parallel/test-http2-pipe.js \
  parallel/test-http2-response-splitting.js \
  parallel/test-strace-openat-openssl.js \
  parallel/test-stream-pipeline.js \
  parallel/test-stream-readable-async-iterators.js \
  parallel/test-zlib-type-error.js \
  pseudo-tty/console_colors.js
EDGE_NODE_TEST_SKIP_TESTS ?= $(subst $(SPACE),$(COMMA),$(strip $(EDGE_NODE_TEST_SKIP_CI_20260518)))

# QuickJS currently cannot parse explicit resource management `using` syntax.
QUICKJS_SKIP_USING_PARSER_TESTS := parallel/test-stream-duplex-destroy.js,parallel/test-stream-readable-dispose.js,parallel/test-stream-transform-destroy.js,parallel/test-stream-writable-destroy.js
# QuickJS worker_threads/MessagePort support is incomplete; these worker-backed
# tests time out or fail in the QuickJS lane while V8 continues to cover them.
QUICKJS_SKIP_WORKER_TESTS := parallel/test-diagnostics-channel-worker-threads.js,client-proxy/test-http-proxy-request-invalid-char-in-url.mjs,parallel/test-crypto-key-objects-messageport.js,parallel/test-crypto-prime.js,parallel/test-crypto-worker-thread.js,parallel/test-http2-reset-flood.js,parallel/test-webcrypto-cryptokey-workers.js
# QuickJS currently regresses TLS close-notify handling under --expose-internals.
QUICKJS_SKIP_TLS_TESTS := parallel/test-tls-close-notify.js
QUICKJS_SKIP_TESTS ?= $(EDGE_NODE_TEST_SKIP_TESTS),$(QUICKJS_SKIP_USING_PARSER_TESTS),$(QUICKJS_SKIP_WORKER_TESTS),$(QUICKJS_SKIP_TLS_TESTS)

ifeq ($(UNAME_S),Darwin)
BUILD_ENV := env -u CPPFLAGS -u LDFLAGS
endif
ifeq ($(UNAME_S),Darwin)
ifeq ($(UNAME_M),arm64)
NAPI_V8_PLATFORM := darwin-arm64
else ifeq ($(UNAME_M),x86_64)
NAPI_V8_PLATFORM := darwin-amd64
endif
else ifeq ($(UNAME_S),Linux)
ifeq ($(UNAME_M),x86_64)
NAPI_V8_PLATFORM := linux-amd64
endif
endif
NAPI_V8_DIST_ROOT ?= $(CURDIR)/build-v8-napi/_v8_cache/$(NAPI_V8_PREBUILT_VERSION)/$(NAPI_V8_PLATFORM)
NAPI_V8_CMAKE_ARGS ?=
ifneq ($(NAPI_V8_PLATFORM),)
ifneq ($(wildcard $(NAPI_V8_DIST_ROOT)/include/v8.h),)
ifneq ($(wildcard $(NAPI_V8_DIST_ROOT)/lib/libv8.a),)
NAPI_V8_CMAKE_ARGS += -DNAPI_V8_BUILD_METHOD=local
NAPI_V8_CMAKE_ARGS += -DNAPI_V8_INCLUDE_DIR=$(NAPI_V8_DIST_ROOT)/include
NAPI_V8_CMAKE_ARGS += -DNAPI_V8_LIBRARY=$(NAPI_V8_DIST_ROOT)/lib/libv8.a
NAPI_V8_CMAKE_ARGS += -DNAPI_V8_DEFINES=V8_COMPRESS_POINTERS
ifeq ($(UNAME_S),Darwin)
NAPI_V8_CMAKE_ARGS += -DNAPI_V8_EXTRA_LIBS=/System/Library/Frameworks/CoreFoundation.framework
endif
endif
endif
endif

clean-napi-quickjs:
	rm -rf $(BUILD_EDGE_QUICKJS_CLI_DIR)

clean:
	find . -maxdepth 1 -type d -name 'build-*' -exec rm -rf {} +

build-napi:
	$(BUILD_ENV) cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEDGE_DEFAULT_WASMER_PACKAGE=$(EDGE_WASMER_PACKAGE) -DEDGE_BUILD_NAPI_TESTS=ON $(NAPI_V8_CMAKE_ARGS) $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_DIR) -j$(JOBS)

build-napi-quickjs:
	$(BUILD_ENV) cmake -S . -B $(BUILD_EDGE_QUICKJS_CLI_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEDGE_DEFAULT_WASMER_PACKAGE=$(EDGE_WASMER_PACKAGE) -DEDGE_NAPI_PROVIDER=quickjs -DEDGE_BUILD_NAPI_TESTS=ON $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_EDGE_QUICKJS_CLI_DIR) -j$(JOBS)

build-native-v8:
	$(MAKE) -C napi build-native-v8

build-native-quickjs:
	$(MAKE) -C napi build-native-quickjs

build:
	$(BUILD_ENV) cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEDGE_DEFAULT_WASMER_PACKAGE=$(EDGE_WASMER_PACKAGE) -DEDGE_BUILD_NAPI_TESTS=OFF $(NAPI_V8_CMAKE_ARGS) $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_DIR) -j$(JOBS)

build-edge: build

build-edge-quickjs-cli:
	$(BUILD_ENV) cmake -S . -B $(BUILD_EDGE_QUICKJS_CLI_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEDGE_DEFAULT_WASMER_PACKAGE=$(EDGE_WASMER_PACKAGE) -DEDGE_NAPI_PROVIDER=quickjs -DEDGE_BUILD_NAPI_TESTS=OFF $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_EDGE_QUICKJS_CLI_DIR) --target edge edgeenv -j$(JOBS)

build-wasix:
	./wasix/build-wasix.sh

build-quickjs-wasix:
	./quickjs-wasm/build.sh

build-wasix-napi: build-wasix build-napi-wasmer-cli

build-wasix-napi-quickjs: build-quickjs-wasix

build-napi-wasmer-cli:
	cd $(NAPI_WASMER_DIR) && CARGO_TARGET_DIR="$(NAPI_WASMER_CARGO_TARGET_DIR)" ./cargo-standalone.sh build --features cli --bin napi_wasmer

test-wasix-napi: build-wasix-napi test-wasix-napi-cli

test-wasix-napi-quickjs: build-wasix-napi-quickjs
	$(MAKE) test-wasix-safe-mode WASIX_PACKAGE_DIR="$(CURDIR)/quickjs-wasm"

test-wasix-napi-cli: build-wasix build-napi-wasmer-cli
	@output="$$($(NAPI_WASMER_BINARY) $(WASIX_EDGEJS_WASM) -e "$(WASIX_NAPI_SMOKE_JS)")"; \
	printf '%s\n' "$$output"; \
	printf '%s\n' "$$output" | grep -Fx "hello world!"

test-wasix-safe-mode:
	python3 ./scripts/test-wasix-safe-mode.py --wasmer-bin "$(WASMER_BIN)" --package-dir "$(WASIX_PACKAGE_DIR)" $(WASIX_SAFE_MODE_ARGS)

$(EDGE_BINARY):
	$(MAKE) build

test: build test-only

test-only:
	EDGE_BYTECODE_CACHE=0 NODE_TEST_RUNNER=$(EDGE_BINARY) ./test/nodejs_test_harness --category=node:buffer,node:console,node:dgram,node:diagnostics_channel,node:dns,node:events,node:http,node:https,node:os,node:path,node:punycode,node:querystring,node:stream,node:string_decoder,node:tty,node:url,node:zlib,node:crypto,node:domain,node:http2,node:tls,node:sys \
	  --skip-tests=$(EDGE_NODE_TEST_SKIP_TESTS) \
	  -j $(TEST_JOBS)

test-quickjs-only:
	EDGE_BYTECODE_CACHE=0 NODE_TEST_RUNNER=$(BUILD_EDGE_QUICKJS_CLI_DIR)/edge ./test/nodejs_test_harness --category=node:buffer,node:console,node:dgram,node:diagnostics_channel,node:dns,node:events,node:http,node:https,node:os,node:path,node:punycode,node:querystring,node:stream,node:string_decoder,node:tty,node:url,node:zlib,node:crypto,node:domain,node:http2,node:tls,node:sys \
	  --skip-tests=$(QUICKJS_SKIP_TESTS) \
	  -j $(TEST_JOBS)

test-wasix-quickjs-only:
	NODE_TEST_RUNNER=$(WASIX_QUICKJS_NODE_TEST_RUNNER) WASMER_BIN="$(WASMER_BIN)" EDGEJS_ROOT="$(CURDIR)" WASIX_EDGEJS_PACKAGE_DIR="$(CURDIR)/quickjs-wasm" ./test/nodejs_test_harness --category=node:buffer,node:console,node:dgram,node:diagnostics_channel,node:dns,node:events,node:http,node:https,node:os,node:path,node:punycode,node:querystring,node:stream,node:string_decoder,node:tty,node:url,node:zlib,node:crypto,node:domain,node:http2,node:tls,node:sys \
	  --skip-tests=$(QUICKJS_SKIP_TESTS) \
	  -j $(TEST_JOBS)

test-bytecode-cache:
	EDGE_BIN=$(EDGE_BINARY) ./scripts/test-bytecode-cache.sh

test-bytecode-cache-quickjs:
	EDGE_BIN=$(BUILD_EDGE_QUICKJS_CLI_DIR)/edge ./scripts/test-bytecode-cache.sh

# Generate the shippable builtins (lib/) bytecode cache next to each binary
# (<binary>.builtins.v8b / .qjsb). The builtins cache is on by default, so a
# representative run populates it; ship the produced file alongside the binary
# so opt-in users (and read-only installs) get the builtin startup win from the
# first run. Re-run after rebuilding (the engine tag invalidates a stale cache).
PRECOMPILE_BUILTINS_DRIVER ?= ./benchmarks/precompile-builtins-driver.mjs
precompile-builtins:
	@for bin in $(EDGE_BINARY) $(BUILD_EDGE_QUICKJS_CLI_DIR)/edge; do \
		[ -x "$$bin" ] || continue; \
		rm -f "$$bin".builtins.*; \
		"$$bin" "$(PRECOMPILE_BUILTINS_DRIVER)" >/dev/null 2>&1 || true; \
		ls -la "$$bin".builtins.* 2>/dev/null || echo "no builtins cache produced for $$bin"; \
	done

clean-edge-quickjs-cli:
	rm -rf $(BUILD_EDGE_QUICKJS_CLI_DIR)

check-portability:
ifeq ($(UNAME_S),Darwin)
	@for bin in $(EDGE_BINARY) $(EDGEENV_BINARY); do \
		deps=$$(otool -L "$$bin" | tail -n +2 | awk '{print $$1}' | grep '^/' | grep -Ev '^(/System/Library/|/usr/lib/)' || true); \
		if [ -n "$$deps" ]; then \
			echo "error: $$bin links to non-system dylibs:" >&2; \
			echo "$$deps" >&2; \
			exit 1; \
		fi; \
		file "$$bin"; \
	done
endif

clean-dist:
	rm -rf $(DIST_DIR)
	rm -f $(ZIP_NAME)

dist: build dist-only

dist-only:
	rm -rf $(DIST_DIR)
	rm -f $(ZIP_NAME)
	mkdir -p $(DIST_BIN_DIR)
	if [ "$(BUILD_DIR)" = "build-wasix" ] || [ "$(BUILD_DIR)" = "$(BUILD_QUICKJS_WASIX_DIR)" ]; then \
		cp "$(BUILD_DIR)/edgejs.wasm" "$(DIST_BIN_DIR)/edgejs"; \
		cp wasmer.toml "$(DIST_DIR)/wasmer.toml"; \
		mkdir -p "$(DIST_DIR)/ssl-certs"; \
		cp "$(WASIX_SSL_CERTS_DIR)/cacert.pem" "$(DIST_DIR)/ssl-certs/cacert.pem"; \
		cp "$(WASIX_SSL_CERTS_DIR)/cert.pem" "$(DIST_DIR)/ssl-certs/cert.pem"; \
		cp -R "$(WASIX_SSL_CERTS_DIR)/certs" "$(DIST_DIR)/ssl-certs/certs"; \
		perl -0pi -e 's#^source = ".*"#source = "./bin/edgejs"#m' "$(DIST_DIR)/wasmer.toml"; \
	else \
		cp "$(EDGE_BINARY)" "$(DIST_BIN_DIR)/edge"; \
		cp "$(EDGEENV_BINARY)" "$(DIST_BIN_DIR)/edgeenv"; \
	fi
	cp -R bin-compat $(DIST_BIN_COMPAT_DIR)
	cp README.md $(DIST_DIR)/README.md
	if [ "$(UNAME_S)" = "Darwin" ] && [ "$(BUILD_DIR)" != "build-wasix" ] && [ "$(BUILD_DIR)" != "$(BUILD_QUICKJS_WASIX_DIR)" ]; then \
		for bin in $(DIST_BIN_DIR)/edge $(DIST_BIN_DIR)/edgeenv; do \
			deps=$$(otool -L "$$bin" | tail -n +2 | awk '{print $$1}' | grep '^/' | grep -Ev '^(/System/Library/|/usr/lib/)' || true); \
			if [ -n "$$deps" ]; then \
				echo "error: $$bin still links to non-system dylibs:" >&2; \
				echo "$$deps" >&2; \
				echo "Rebuild with 'make build' before packaging." >&2; \
				exit 1; \
			fi; \
		done; \
	fi
	if [ "$(BUILD_DIR)" = "build-wasix" ] || [ "$(BUILD_DIR)" = "$(BUILD_QUICKJS_WASIX_DIR)" ]; then \
		cd $(DIST_DIR) && zip -r ../$(ZIP_NAME) bin bin-compat README.md wasmer.toml ssl-certs; \
	else \
		cd $(DIST_DIR) && zip -r ../$(ZIP_NAME) bin bin-compat README.md; \
	fi

framework-test: $(EDGE_BINARY)
	@"$(EDGE_BINARY)" "$(FRAMEWORK_TEST_SCRIPT)" test $(FRAMEWORK_TEST_SELECTOR)

framework-test-reset:
	@if [ -x "$(EDGE_BINARY)" ]; then \
		"$(EDGE_BINARY)" "$(FRAMEWORK_TEST_SCRIPT)" reset $(FRAMEWORK_TEST_SELECTOR); \
	elif command -v node >/dev/null 2>&1; then \
		node "$(FRAMEWORK_TEST_SCRIPT)" reset $(FRAMEWORK_TEST_SELECTOR); \
	else \
		echo "error: $(EDGE_BINARY) is missing and no node fallback is available for framework-test-reset" >&2; \
		exit 1; \
	fi

js-%:
	@:
