.PHONY: build build-wasix build-napi-wasmer-cli test-wasix-napi-cli test test-only check-portability clean-dist dist dist-only framework-test framework-test-reset

UNAME_S := $(shell uname -s)
BUILD_NAPI_DIR ?= build-v8-napi
BUILD_DIR ?= build-edge
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
FRAMEWORK_TEST_SCRIPT := $(CURDIR)/scripts/framework-test.js
FRAMEWORK_TEST_SELECTOR := $(filter js-%,$(MAKECMDGOALS))
NAPI_WASMER_DIR ?= napi
NAPI_WASMER_BINARY ?= ./$(NAPI_WASMER_DIR)/target/debug/napi_wasmer
WASIX_EDGEJS_WASM ?= ./build-wasix/edgejs.wasm
WASIX_NAPI_SMOKE_JS ?= console.log('hello world!');
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

ifeq ($(UNAME_S),Darwin)
BUILD_ENV := env -u CPPFLAGS -u LDFLAGS
endif

build-napi:
	$(BUILD_ENV) cmake -S napi/v8 -B $(BUILD_NAPI_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_NAPI_DIR) -j$(JOBS)

test-napi: build-napi test-napi-only

test-napi-only:
	$(BUILD_ENV) ctest --test-dir $(BUILD_NAPI_DIR) --output-on-failure -R '^napi_v8\.'

build:
	$(BUILD_ENV) cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) -DEDGE_DEFAULT_WASMER_PACKAGE=$(EDGE_WASMER_PACKAGE) $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_DIR) -j$(JOBS)

build-wasix:
	./wasix/build-wasix.sh

build-napi-wasmer-cli:
	cd $(NAPI_WASMER_DIR) && ./cargo-standalone.sh build --features cli --bin napi_wasmer

test-wasix-napi-cli:
	@output="$$($(NAPI_WASMER_BINARY) $(WASIX_EDGEJS_WASM) -e "$(WASIX_NAPI_SMOKE_JS)")"; \
	printf '%s\n' "$$output"; \
	printf '%s\n' "$$output" | grep -Fx "hello world!"

$(EDGE_BINARY):
	$(MAKE) build

test: build test-only

test-only:
	NODE_TEST_RUNNER=$(EDGE_BINARY) ./test/nodejs_test_harness --category=node:buffer,node:console,node:dgram,node:diagnostics_channel,node:dns,node:events,node:http,node:https,node:os,node:path,node:punycode,node:querystring,node:stream,node:string_decoder,node:tty,node:url,node:zlib,node:crypto,node:domain,node:http2,node:tls,node:sys \
	  --skip-tests=known_issues/test-stdin-is-always-net.socket.js,parallel/test-dns-perf_hooks.js,parallel/test-dns-channel-timeout.js

# 	Tests not working on linux

# 	/parallel/test-dns-channel-timeout.js
# 	/parallel/test-http-server-headers-timeout-keepalive.js
# 	/parallel/test-http-server-request-timeout-keepalive.js
# 	/parallel/test-crypto-argon2-unsupported.js
# 	/parallel/test-crypto-encap-decap.js
# 	/parallel/test-crypto-pqc-key-objects-ml-dsa.js
# 	/parallel/test-crypto-pqc-key-objects-ml-kem.js
# 	/parallel/test-crypto-pqc-key-objects-slh-dsa.js
# 	/parallel/test-crypto-pqc-keygen-ml-dsa.js
# 	/parallel/test-crypto-pqc-keygen-ml-kem.js
# 	/parallel/test-crypto-pqc-keygen-slh-dsa.js
# 	/parallel/test-crypto-rsa-dsa.js
# 	/parallel/test-strace-openat-openssl.js
# 	/parallel/test-webcrypto-supports.mjs
# 	/parallel/test-http2-client-jsstream-destroy.js
# 	/parallel/test-http2-https-fallback.js
# 	/parallel/test-http2-respond-with-file-connection-abort.js
# 	/parallel/test-http2-server-unknown-protocol.js
# 	/parallel/test-tls-alpn-server-client.js
# 	/parallel/test-tls-client-getephemeralkeyinfo.js
# 	/parallel/test-tls-getprotocol.js
# 	/parallel/test-tls-min-max-version.js
# 	/parallel/test-tls-socket-destroy.js

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
	if [ "$(BUILD_DIR)" = "build-wasix" ]; then \
		cp "$(BUILD_DIR)/edgejs.wasm" "$(DIST_BIN_DIR)/edgejs"; \
		cp wasmer.toml "$(DIST_DIR)/wasmer.toml"; \
		perl -0pi -e 's#^source = ".*"#source = "./bin/edgejs"#m' "$(DIST_DIR)/wasmer.toml"; \
	else \
		cp "$(EDGE_BINARY)" "$(DIST_BIN_DIR)/edge"; \
		cp "$(EDGEENV_BINARY)" "$(DIST_BIN_DIR)/edgeenv"; \
	fi
	cp -R bin-compat $(DIST_BIN_COMPAT_DIR)
	cp README.md $(DIST_DIR)/README.md
	if [ "$(UNAME_S)" = "Darwin" ] && [ "$(BUILD_DIR)" != "build-wasix" ]; then \
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
	if [ "$(BUILD_DIR)" = "build-wasix" ]; then \
		cd $(DIST_DIR) && zip -r ../$(ZIP_NAME) bin bin-compat README.md wasmer.toml; \
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

