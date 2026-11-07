function(edge_configure_options)
  set(EDGE_DEFAULT_WASMER_PACKAGE
    "$ENV{EDGE_WASMER_PACKAGE}"
    CACHE STRING "Default Wasmer package used by edge --safe"
  )

  option(EDGE_EXTERNAL_NAPI_V8
    "Deprecated alias. Use EDGE_NAPI_PROVIDER=imports."
    OFF
  )
  # Static data that native builds keep in the read-only executable image.
  # Wasm has no read-only segment, so embedded data is copied into every
  # instance's linear memory; WASIX builds turn these OFF and ship the data in
  # the package filesystem instead (see wasix/build-wasix.sh).
  option(EDGE_EMBED_BUILTIN_SOURCES
    "Embed builtin JS sources (lib/ and deps/) in the binary; OFF reads them from the lib root (/node-lib or EDGEJS_LIB_DIR)"
    ON
  )
  option(EDGE_EMBED_ICU_DATA
    "Embed the ICU common data blob; OFF loads individual ICU items lazily from a data directory (/usr/share/edge-icu or EDGE_ICU_DATA)"
    ON
  )
  option(EDGE_ALLOW_UNDEFINED_IMPORTS
    "Allow undefined imports at link time (useful for external runtime deps in wasm/wasix)"
    OFF
  )
  option(EDGE_PREFER_REPO_LOCAL_V8
    "Prefer the repo-local V8 dist bundle when it exists"
    ON
  )
  option(EDGE_BUILD_NAPI_TESTS
    "Build the selected embedded N-API provider tests as part of the Edge build"
    OFF
  )
  option(EDGE_SHARED_OPENSSL
    "Link against a shared/system OpenSSL instead of the vendored OpenSSL"
    OFF
  )
  option(EDGE_QUICKJS_WEBASSEMBLY
    "Install a QuickJS-only WebAssembly global backed by Wasmer's wasm_c_api"
    ON
  )
  set(_edge_quickjs_wasmer_version_default "latest-stable")
  if(NOT "$ENV{EDGE_QUICKJS_WASMER_VERSION}" STREQUAL "")
    set(_edge_quickjs_wasmer_version_default "$ENV{EDGE_QUICKJS_WASMER_VERSION}")
  endif()
  set(EDGE_QUICKJS_WASMER_VERSION
    "${_edge_quickjs_wasmer_version_default}"
    CACHE STRING "Wasmer C API version for QuickJS WebAssembly, or latest-stable"
  )
  set(EDGE_QUICKJS_WASMER_DIST_ROOT
    "$ENV{EDGE_QUICKJS_WASMER_DIST_ROOT}"
    CACHE PATH "Path to a Wasmer C API distribution with include/ and lib/libwasmer.a"
  )
  option(EDGE_QUICKJS_WASMER_ALLOW_PRERELEASE
    "Allow prerelease Wasmer C API versions when resolving latest"
    OFF
  )
  option(ENABLE_TRACING
    "Compile startup tracing support"
    OFF
  )
  set(EDGE_SHARED_OPENSSL_INCLUDES
    ""
    CACHE PATH "Directory containing OpenSSL headers when EDGE_SHARED_OPENSSL is ON"
  )
  set(EDGE_SHARED_OPENSSL_LIBPATH
    ""
    CACHE PATH "Directory containing shared OpenSSL libraries when EDGE_SHARED_OPENSSL is ON"
  )
  set(EDGE_SHARED_OPENSSL_LIBNAME
    "crypto;ssl"
    CACHE STRING "Semicolon-separated shared OpenSSL library names (crypto;ssl)"
  )
  set(EDGE_REPO_LOCAL_V8_DIST_ROOT
    "${PROJECT_ROOT}/v8-custom-builds/v8/out/dist"
    CACHE PATH "Path to the repo-local V8 dist bundle used when EDGE_PREFER_REPO_LOCAL_V8 is ON"
  )

  set(EDGE_IS_WASIX_TARGET OFF)
  if(CMAKE_SYSTEM_NAME MATCHES "WASI|WASIX" OR
     CMAKE_C_COMPILER MATCHES "wasixcc" OR
     CMAKE_CXX_COMPILER MATCHES "wasixcc\\+\\+")
    set(EDGE_IS_WASIX_TARGET ON)
  endif()

  if(NOT EDGE_EMBED_BUILTIN_SOURCES OR NOT EDGE_EMBED_ICU_DATA)
    # The binary depends on data staged by scripts/stage_wasix_static_data.py.
    set(_edge_static_data_note
      "Edge static data: builtin JS embedded=${EDGE_EMBED_BUILTIN_SOURCES}, ICU data embedded=${EDGE_EMBED_ICU_DATA}")
    if(EDGE_IS_WASIX_TARGET)
      message(STATUS "${_edge_static_data_note}")
    else()
      message(WARNING
        "${_edge_static_data_note}. Native builds normally embed both; this "
        "binary needs EDGEJS_LIB_DIR / EDGE_ICU_DATA pointing at a tree staged "
        "by scripts/stage_wasix_static_data.py.")
    endif()
  endif()

  set(_edge_webcontainer_version_default "")
  if(EDGE_IS_WASIX_TARGET)
    execute_process(
      COMMAND "${CMAKE_C_COMPILER}" --version
      OUTPUT_VARIABLE _edge_wasixcc_version_output
      ERROR_VARIABLE _edge_wasixcc_version_error
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_STRIP_TRAILING_WHITESPACE
    )
    string(CONCAT _edge_wasixcc_version_text
      "${_edge_wasixcc_version_output}\n${_edge_wasixcc_version_error}")
    if(_edge_wasixcc_version_text MATCHES "wasixcc[ \t]+v?([0-9]+\\.[0-9]+\\.[0-9]+)")
      set(_edge_webcontainer_version_default "wasix-${CMAKE_MATCH_1}")
    else()
      set(_edge_webcontainer_version_default "wasix")
    endif()
  endif()
  if(NOT "$ENV{EDGE_WEBCONTAINER_VERSION}" STREQUAL "")
    set(_edge_webcontainer_version_default "$ENV{EDGE_WEBCONTAINER_VERSION}")
  endif()
  set(EDGE_WEBCONTAINER_VERSION
    "${_edge_webcontainer_version_default}"
    CACHE STRING "Fake process.versions.webcontainer value for WebAssembly builds"
  )

  set(EDGE_DEFAULT_WASMER_PACKAGE "${EDGE_DEFAULT_WASMER_PACKAGE}" PARENT_SCOPE)
  set(EDGE_EXTERNAL_NAPI_V8 "${EDGE_EXTERNAL_NAPI_V8}" PARENT_SCOPE)
  set(EDGE_ALLOW_UNDEFINED_IMPORTS "${EDGE_ALLOW_UNDEFINED_IMPORTS}" PARENT_SCOPE)
  set(EDGE_PREFER_REPO_LOCAL_V8 "${EDGE_PREFER_REPO_LOCAL_V8}" PARENT_SCOPE)
  set(EDGE_BUILD_NAPI_TESTS "${EDGE_BUILD_NAPI_TESTS}" PARENT_SCOPE)
  set(EDGE_SHARED_OPENSSL "${EDGE_SHARED_OPENSSL}" PARENT_SCOPE)
  set(EDGE_QUICKJS_WEBASSEMBLY "${EDGE_QUICKJS_WEBASSEMBLY}" PARENT_SCOPE)
  set(EDGE_QUICKJS_WASMER_VERSION "${EDGE_QUICKJS_WASMER_VERSION}" PARENT_SCOPE)
  set(EDGE_QUICKJS_WASMER_DIST_ROOT "${EDGE_QUICKJS_WASMER_DIST_ROOT}" PARENT_SCOPE)
  set(EDGE_QUICKJS_WASMER_ALLOW_PRERELEASE "${EDGE_QUICKJS_WASMER_ALLOW_PRERELEASE}" PARENT_SCOPE)
  set(ENABLE_TRACING "${ENABLE_TRACING}" PARENT_SCOPE)
  set(EDGE_SHARED_OPENSSL_INCLUDES "${EDGE_SHARED_OPENSSL_INCLUDES}" PARENT_SCOPE)
  set(EDGE_SHARED_OPENSSL_LIBPATH "${EDGE_SHARED_OPENSSL_LIBPATH}" PARENT_SCOPE)
  set(EDGE_SHARED_OPENSSL_LIBNAME "${EDGE_SHARED_OPENSSL_LIBNAME}" PARENT_SCOPE)
  set(EDGE_REPO_LOCAL_V8_DIST_ROOT "${EDGE_REPO_LOCAL_V8_DIST_ROOT}" PARENT_SCOPE)
  set(EDGE_EMBED_BUILTIN_SOURCES "${EDGE_EMBED_BUILTIN_SOURCES}" PARENT_SCOPE)
  set(EDGE_EMBED_ICU_DATA "${EDGE_EMBED_ICU_DATA}" PARENT_SCOPE)
  set(EDGE_IS_WASIX_TARGET "${EDGE_IS_WASIX_TARGET}" PARENT_SCOPE)
  set(EDGE_WEBCONTAINER_VERSION "${EDGE_WEBCONTAINER_VERSION}" PARENT_SCOPE)
endfunction()
