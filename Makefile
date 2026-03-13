.PHONY: build test test-only check-portability clean-dist dist dist-only

UNAME_S := $(shell uname -s)
BUILD_DIR ?= build-edge
DIST_DIR ?= dist
DIST_BIN_DIR ?= $(DIST_DIR)/bin
DIST_BIN_COMPAT_DIR ?= $(DIST_DIR)/bin-compat
DIST_LIB_DIR ?= $(DIST_DIR)/lib
ZIP_NAME ?= edge.zip
CMAKE_BUILD_TYPE ?= Release
JOBS ?= 8
TEST_JOBS ?= 0
EDGE_BINARY ?= $(BUILD_DIR)/edge
EDGEENV_BINARY ?= $(BUILD_DIR)/edgeenv
CMAKE_ARGS ?=
BUILD_ENV ?= env
EXTRA_CMAKE_ARGS ?=

ifeq ($(UNAME_S),Darwin)
BUILD_ENV := env -u CPPFLAGS -u LDFLAGS
EXTRA_CMAKE_ARGS += '-UOPENSSL_*' -DOPENSSL_USE_STATIC_LIBS=TRUE -DCMAKE_EXE_LINKER_FLAGS= -DCMAKE_SHARED_LINKER_FLAGS= -DCMAKE_MODULE_LINKER_FLAGS=
endif

build:
	$(BUILD_ENV) cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) $(EXTRA_CMAKE_ARGS) $(CMAKE_ARGS)
	$(BUILD_ENV) cmake --build $(BUILD_DIR) -j$(JOBS)

test: build test-only

test-only:
	NODE_TEST_RUNNER=$(EDGE_BINARY) ./test/nodejs_test_harness --category=node:buffer,node:console,node:dgram,node:diagnostics_channel,node:dns,node:events,node:http,node:https,node:os,node:path,node:punycode,node:querystring,node:stream,node:string_decoder,node:tty,node:url,node:zlib,node:crypto,node:domain,node:http2,node:tls,node:sys \
	  --skip-tests=known_issues/test-stdin-is-always-net.socket.j,parallel/test-dns-perf_hooks.js,

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
	cp $(EDGE_BINARY) $(DIST_BIN_DIR)/edge
	cp $(EDGEENV_BINARY) $(DIST_BIN_DIR)/edgeenv
	cp -R bin-compat $(DIST_BIN_COMPAT_DIR)
	cp README.md $(DIST_DIR)/README.md
	cp -R lib $(DIST_LIB_DIR)
	mkdir -p $(DIST_LIB_DIR)/internal/deps
	for dep in undici acorn minimatch cjs-module-lexer amaro; do \
		mkdir -p "$(DIST_LIB_DIR)/internal/deps/$$(dirname "$$dep")"; \
		cp -R "deps/$$dep" "$(DIST_LIB_DIR)/internal/deps/$$dep"; \
	done
	if [ "$(UNAME_S)" = "Darwin" ]; then \
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
	cd $(DIST_DIR) && zip -r ../$(ZIP_NAME) bin bin-compat README.md lib
