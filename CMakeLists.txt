cmake_minimum_required(VERSION 3.20)
project(edge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(EDGE_ROOT "${PROJECT_ROOT}")
set(NAPI_V8_ROOT "${PROJECT_ROOT}/napi/v8")
set(NAPI_INCLUDE_ROOT "${PROJECT_ROOT}/napi/include")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Git QUIET)

set(EDGE_VERSION_COMMIT "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${PROJECT_ROOT}"
    OUTPUT_VARIABLE EDGE_VERSION_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
endif()
if(EDGE_VERSION_COMMIT STREQUAL "")
  set(EDGE_VERSION_COMMIT "unknown")
endif()
set(EDGE_VERSION_SUFFIX "")
if(NOT EDGE_VERSION_COMMIT STREQUAL "unknown")
  set(EDGE_VERSION_SUFFIX "-${EDGE_VERSION_COMMIT}")
endif()
set(EDGE_DEFAULT_WASMER_PACKAGE
  "$ENV{EDGE_WASMER_PACKAGE}"
  CACHE STRING "Default Wasmer package used by edge --safe"
)

include("${PROJECT_ROOT}/napi/v8/cmake/NapiBuildCache.cmake")
napi_enable_sccache_if_available()

set(NAPI_V8_NODE_ROOT_DEFAULT "${PROJECT_ROOT}/node")
if(DEFINED ENV{NAPI_V8_NODE_ROOT})
  set(NAPI_V8_NODE_ROOT_DEFAULT "$ENV{NAPI_V8_NODE_ROOT}")
endif()
set(NAPI_V8_NODE_ROOT
  "${NAPI_V8_NODE_ROOT_DEFAULT}"
  CACHE PATH "Path to Node source root (for raw Node tests/fixtures)"
)
option(EDGE_EXTERNAL_NAPI_V8
  "Deprecated alias. Use EDGE_NAPI_PROVIDER=imports."
  OFF
)
option(EDGE_ALLOW_UNDEFINED_IMPORTS
  "Allow undefined imports at link time (useful for external runtime deps in wasm/wasix)"
  OFF
)
option(EDGE_PREFER_REPO_LOCAL_V8
  "Prefer the repo-local V8 dist bundle when it exists"
  ON
)
option(EDGE_SHARED_OPENSSL
  "Link against a shared/system OpenSSL instead of the vendored OpenSSL"
  OFF
)
option(EDGE_ENABLE_STARTUP_PROFILE
  "Embed the internal startup profiler (EDGE_STARTUP_PROFILE) into the binary"
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
  set(EDGE_ALLOW_UNDEFINED_IMPORTS ON CACHE BOOL "" FORCE)
endif()

function(edge_detect_native_openssl_target_os out_var)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_value "linux")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_value "mac")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(_value "win")
  else()
    message(FATAL_ERROR
      "Vendored OpenSSL is not yet wired for native target system '${CMAKE_SYSTEM_NAME}'. "
      "Use -DEDGE_SHARED_OPENSSL=ON for this platform.")
  endif()
  set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(edge_detect_native_openssl_target_arch out_var)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(_value "x64")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86|i[3-6]86|X86)$")
    set(_value "x86")
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(_value "arm64")
  else()
    message(FATAL_ERROR
      "Vendored OpenSSL is not yet wired for native target architecture '${CMAKE_SYSTEM_PROCESSOR}'. "
      "Use -DEDGE_SHARED_OPENSSL=ON for this target.")
  endif()
  set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(edge_require_shared_openssl_target target_name display_name)
  set(_shared FALSE)
  foreach(_prop
      IMPORTED_IMPLIB
      IMPORTED_IMPLIB_RELEASE
      IMPORTED_IMPLIB_DEBUG
      IMPORTED_IMPLIB_RELWITHDEBINFO
      IMPORTED_IMPLIB_MINSIZEREL
      IMPORTED_IMPLIB_NOCONFIG)
    get_target_property(_value "${target_name}" "${_prop}")
    if(_value AND NOT _value MATCHES "-NOTFOUND$")
      set(_shared TRUE)
    endif()
  endforeach()
  if(NOT _shared)
    foreach(_prop
        IMPORTED_LOCATION
        IMPORTED_LOCATION_RELEASE
        IMPORTED_LOCATION_DEBUG
        IMPORTED_LOCATION_RELWITHDEBINFO
        IMPORTED_LOCATION_MINSIZEREL
        IMPORTED_LOCATION_NOCONFIG)
      get_target_property(_value "${target_name}" "${_prop}")
      if(NOT _value OR _value MATCHES "-NOTFOUND$")
        continue()
      endif()
      if(_value MATCHES "\\.(so(\\..*)?|dylib|dll)$")
        set(_shared TRUE)
      endif()
    endforeach()
  endif()
  if(NOT _shared)
    message(FATAL_ERROR
      "${display_name} resolved to a non-shared OpenSSL library. "
      "Shared mode requires shared/system OpenSSL libraries.")
  endif()
endfunction()

set(_edge_napi_provider_default "bundled-v8")
if(EDGE_IS_WASIX_TARGET)
  set(_edge_napi_provider_default "imports")
endif()
set(EDGE_NAPI_PROVIDER
  "${_edge_napi_provider_default}"
  CACHE STRING "N-API provider backend: bundled-v8|imports"
)
set_property(CACHE EDGE_NAPI_PROVIDER PROPERTY STRINGS bundled-v8 imports)
if(EDGE_EXTERNAL_NAPI_V8)
  message(WARNING
    "EDGE_EXTERNAL_NAPI_V8 is deprecated; use EDGE_NAPI_PROVIDER=imports.")
  set(EDGE_NAPI_PROVIDER "imports" CACHE STRING
    "N-API provider backend: bundled-v8|imports" FORCE)
endif()
string(TOLOWER "${EDGE_NAPI_PROVIDER}" EDGE_NAPI_PROVIDER)
if(NOT EDGE_NAPI_PROVIDER MATCHES "^(bundled-v8|imports)$")
  message(FATAL_ERROR
    "Invalid EDGE_NAPI_PROVIDER='${EDGE_NAPI_PROVIDER}'. "
    "Valid values: bundled-v8, imports.")
endif()
set(EDGE_NAPI_PROVIDER "${EDGE_NAPI_PROVIDER}" CACHE STRING
  "N-API provider backend: bundled-v8|imports" FORCE)

# Hard gate: edge runtime/library sources must never directly reference V8.
file(GLOB_RECURSE EDGE_PURE_SRC_FILES
  "${EDGE_ROOT}/src/*.h"
  "${EDGE_ROOT}/src/*.cc"
  "${EDGE_ROOT}/src/*.c"
)
foreach(EDGE_CODE_FILE IN LISTS EDGE_PURE_SRC_FILES)
  file(READ "${EDGE_CODE_FILE}" EDGE_CODE_CONTENTS)
  if(EDGE_CODE_CONTENTS MATCHES "#include <v8\\.h>" OR
     EDGE_CODE_CONTENTS MATCHES "#include <libplatform/libplatform\\.h>" OR
     EDGE_CODE_CONTENTS MATCHES "v8::")
    message(FATAL_ERROR
      "V8 usage is forbidden by policy in src. "
      "Found in: ${EDGE_CODE_FILE}")
  endif()
endforeach()

if(EDGE_NAPI_PROVIDER STREQUAL "bundled-v8")
  if(EDGE_PREFER_REPO_LOCAL_V8)
    set(_edge_repo_local_v8_include "${EDGE_REPO_LOCAL_V8_DIST_ROOT}/include")
    set(_edge_repo_local_v8_library "${EDGE_REPO_LOCAL_V8_DIST_ROOT}/lib/libv8.a")
    if(EXISTS "${_edge_repo_local_v8_include}" AND EXISTS "${_edge_repo_local_v8_library}")
      message(STATUS "EDGE: preferring repo-local V8 bundle at ${EDGE_REPO_LOCAL_V8_DIST_ROOT}")
      set(NAPI_V8_BUILD_METHOD "local" CACHE STRING
        "V8 resolution method: prebuilt|source|local" FORCE)
      set(NAPI_V8_INCLUDE_DIR "${_edge_repo_local_v8_include}" CACHE PATH
        "Path to V8 headers" FORCE)
      set(NAPI_V8_LIBRARY "${_edge_repo_local_v8_library}" CACHE FILEPATH
        "Path to V8 runtime library (monolith/static/dynamic)" FORCE)
      set(NAPI_V8_DEFINES "V8_COMPRESS_POINTERS" CACHE STRING
        "Semicolon-separated V8 embedder compile definitions" FORCE)
      if(APPLE)
        set(NAPI_V8_EXTRA_LIBS "/System/Library/Frameworks/CoreFoundation.framework" CACHE STRING
          "Semicolon-separated extra V8 libraries (e.g. libplatform/libbase)" FORCE)
      endif()
    endif()
  endif()
  set(NAPI_V8_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${NAPI_V8_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/napi-v8")
elseif(EDGE_NAPI_PROVIDER STREQUAL "imports")
  add_library(edge_napi_headers INTERFACE)
  target_include_directories(edge_napi_headers INTERFACE "${NAPI_INCLUDE_ROOT}")
endif()

if(EDGE_IS_WASIX_TARGET)
  set(EDGE_LIBUV_ROOT "${PROJECT_ROOT}/deps/libuv-wasix")
else()
  set(EDGE_LIBUV_ROOT "${PROJECT_ROOT}/deps/uv")
endif()
if(NOT EXISTS "${EDGE_LIBUV_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "libuv CMake project not found at ${EDGE_LIBUV_ROOT}. "
    "For WASIX, run wasix/setup-wasix-deps.sh first.")
endif()

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory("${EDGE_LIBUV_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/deps/uv")

set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_ROOT}/deps/cares" "${CMAKE_CURRENT_BINARY_DIR}/deps/cares")

set(BUILD_UNITTESTS OFF CACHE BOOL "" FORCE)
set(BUILD_MINIZIP_BIN OFF CACHE BOOL "" FORCE)
set(BUILD_ZPIPE OFF CACHE BOOL "" FORCE)
set(BUILD_MINIGZIP OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_ROOT}/deps/zlib" "${CMAKE_CURRENT_BINARY_DIR}/deps/zlib")
if(EDGE_IS_WASIX_TARGET AND TARGET zlib_bench)
  # zlib_bench links against the shared zlib target, which fails under WASIX
  # static-linking constraints. Keep the libraries but skip the benchmark tool.
  set_target_properties(zlib_bench PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

find_package(Threads REQUIRED)

add_library(edge_brotli STATIC
  "${PROJECT_ROOT}/deps/brotli/c/common/constants.c"
  "${PROJECT_ROOT}/deps/brotli/c/common/context.c"
  "${PROJECT_ROOT}/deps/brotli/c/common/dictionary.c"
  "${PROJECT_ROOT}/deps/brotli/c/common/platform.c"
  "${PROJECT_ROOT}/deps/brotli/c/common/shared_dictionary.c"
  "${PROJECT_ROOT}/deps/brotli/c/common/transform.c"
  "${PROJECT_ROOT}/deps/brotli/c/dec/bit_reader.c"
  "${PROJECT_ROOT}/deps/brotli/c/dec/decode.c"
  "${PROJECT_ROOT}/deps/brotli/c/dec/huffman.c"
  "${PROJECT_ROOT}/deps/brotli/c/dec/prefix.c"
  "${PROJECT_ROOT}/deps/brotli/c/dec/state.c"
  "${PROJECT_ROOT}/deps/brotli/c/dec/static_init.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/backward_references.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/backward_references_hq.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/bit_cost.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/block_splitter.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/brotli_bit_stream.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/cluster.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/command.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/compound_dictionary.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/compress_fragment.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/compress_fragment_two_pass.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/dictionary_hash.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/encode.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/encoder_dict.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/entropy_encode.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/fast_log.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/histogram.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/literal_cost.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/memory.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/metablock.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/static_dict.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/static_dict_lut.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/static_init.c"
  "${PROJECT_ROOT}/deps/brotli/c/enc/utf8_util.c"
)
target_include_directories(edge_brotli
  PUBLIC
    "${PROJECT_ROOT}/deps/brotli/c/include"
)
if(APPLE)
  target_link_libraries(edge_brotli PUBLIC m)
elseif(UNIX)
  target_link_libraries(edge_brotli PUBLIC m)
endif()

add_library(edge_zstd STATIC
  "${PROJECT_ROOT}/deps/zstd/lib/common/debug.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/entropy_common.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/fse_decompress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/fse_compress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/huf_compress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/decompress/huf_decompress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/pool.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/threading.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/xxhash.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/zstd_common.c"
  "${PROJECT_ROOT}/deps/zstd/lib/common/error_private.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/hist.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_compress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_compress_literals.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_compress_sequences.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_compress_superblock.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_double_fast.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_fast.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_lazy.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_ldm.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_opt.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstd_preSplit.c"
  "${PROJECT_ROOT}/deps/zstd/lib/compress/zstdmt_compress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/decompress/zstd_ddict.c"
  "${PROJECT_ROOT}/deps/zstd/lib/decompress/zstd_decompress.c"
  "${PROJECT_ROOT}/deps/zstd/lib/decompress/zstd_decompress_block.c"
)
target_include_directories(edge_zstd
  PUBLIC
    "${PROJECT_ROOT}/deps/zstd/lib"
)
target_compile_definitions(edge_zstd
  PUBLIC
    XXH_NAMESPACE=ZSTD_
    ZSTD_MULTITHREAD
    ZSTD_DISABLE_ASM
)
target_link_libraries(edge_zstd
  PUBLIC
    Threads::Threads
)

add_library(edge_nghttp2 STATIC
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_buf.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_callbacks.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_debug.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_extpri.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_frame.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_hd.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_hd_huffman.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_hd_huffman_data.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_helper.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_http.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_map.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_mem.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_alpn.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_option.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_outbound_item.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_pq.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_priority_spec.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_queue.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_ratelim.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_rcbuf.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_session.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_stream.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_submit.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_time.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/nghttp2_version.c"
  "${PROJECT_ROOT}/deps/nghttp2/lib/sfparse.c"
)
target_include_directories(edge_nghttp2
  PUBLIC
    "${PROJECT_ROOT}/deps/nghttp2/lib/includes"
  PRIVATE
    "${PROJECT_ROOT}/deps/nghttp2/lib"
)
target_compile_definitions(edge_nghttp2
  PUBLIC
    NGHTTP2_STATICLIB
  PRIVATE
    BUILDING_NGHTTP2
    HAVE_CONFIG_H
    _U_=
)

if(EDGE_IS_WASIX_TARGET)
  set(EDGE_OPENSSL_WASIX_ROOT
    "${PROJECT_ROOT}/deps/openssl-wasix"
    CACHE PATH "Path to wasix-org OpenSSL source/build tree"
  )
  set(_edge_openssl_include_dir "${EDGE_OPENSSL_WASIX_ROOT}/include")
  if(NOT EXISTS "${_edge_openssl_include_dir}")
    message(FATAL_ERROR
      "OpenSSL include directory not found: ${_edge_openssl_include_dir}. "
      "For WASIX, clone deps with wasix/setup-wasix-deps.sh.")
  endif()

  set(_edge_openssl_crypto "")
  foreach(_candidate
      "${EDGE_OPENSSL_WASIX_ROOT}/lib/libcrypto.a"
      "${EDGE_OPENSSL_WASIX_ROOT}/libcrypto.a")
    if(EXISTS "${_candidate}")
      set(_edge_openssl_crypto "${_candidate}")
      break()
    endif()
  endforeach()

  set(_edge_openssl_ssl "")
  foreach(_candidate
      "${EDGE_OPENSSL_WASIX_ROOT}/lib/libssl.a"
      "${EDGE_OPENSSL_WASIX_ROOT}/libssl.a")
    if(EXISTS "${_candidate}")
      set(_edge_openssl_ssl "${_candidate}")
      break()
    endif()
  endforeach()

  if(_edge_openssl_crypto STREQUAL "" OR _edge_openssl_ssl STREQUAL "")
    message(FATAL_ERROR
      "WASIX OpenSSL static libraries not found under ${EDGE_OPENSSL_WASIX_ROOT}. "
      "Expected libcrypto.a and libssl.a (either root or lib/).")
  endif()

  add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${_edge_openssl_crypto}"
    INTERFACE_INCLUDE_DIRECTORIES "${_edge_openssl_include_dir}"
  )
  add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
  set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${_edge_openssl_ssl}"
    INTERFACE_INCLUDE_DIRECTORIES "${_edge_openssl_include_dir}"
  )
  target_link_libraries(OpenSSL::SSL INTERFACE OpenSSL::Crypto)
else()
  if(NOT EDGE_SHARED_OPENSSL)
    if(NOT EDGE_SHARED_OPENSSL_INCLUDES STREQUAL "" OR
       NOT EDGE_SHARED_OPENSSL_LIBPATH STREQUAL "" OR
       NOT EDGE_SHARED_OPENSSL_LIBNAME STREQUAL "crypto;ssl")
      message(FATAL_ERROR
        "EDGE_SHARED_OPENSSL_* overrides require -DEDGE_SHARED_OPENSSL=ON.")
    endif()

    edge_detect_native_openssl_target_os(EDGE_OPENSSL_TARGET_OS)
    edge_detect_native_openssl_target_arch(EDGE_OPENSSL_TARGET_ARCH)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      set(_edge_openssl_compiler_family "clang")
    elseif(MSVC)
      set(_edge_openssl_compiler_family "msvc")
    else()
      set(_edge_openssl_compiler_family "gcc")
    endif()

    set(EDGE_OPENSSL_MODULES_DIR
      "${CMAKE_CURRENT_BINARY_DIR}/deps/openssl/lib/openssl-modules"
    )
    file(MAKE_DIRECTORY "${EDGE_OPENSSL_MODULES_DIR}")

    set(_edge_vendored_openssl_manifest
      "${CMAKE_CURRENT_BINARY_DIR}/edge_vendored_openssl.cmake"
    )
    execute_process(
      COMMAND
        "${Python3_EXECUTABLE}"
        "${PROJECT_ROOT}/scripts/generate_vendored_openssl_cmake.py"
        --project-root "${PROJECT_ROOT}"
        --target-os "${EDGE_OPENSSL_TARGET_OS}"
        --target-arch "${EDGE_OPENSSL_TARGET_ARCH}"
        --compiler-family "${_edge_openssl_compiler_family}"
        --modules-dir "${EDGE_OPENSSL_MODULES_DIR}"
        --output "${_edge_vendored_openssl_manifest}"
      RESULT_VARIABLE _edge_vendored_openssl_status
      OUTPUT_VARIABLE _edge_vendored_openssl_stdout
      ERROR_VARIABLE _edge_vendored_openssl_stderr
    )
    if(NOT _edge_vendored_openssl_status EQUAL 0)
      message(FATAL_ERROR
        "Failed to generate vendored OpenSSL build manifest:\n${_edge_vendored_openssl_stderr}")
    endif()
    include("${_edge_vendored_openssl_manifest}")

    set(_edge_vendored_openssl_asm_sources
      ${EDGE_VENDORED_OPENSSL_SOURCES}
      ${EDGE_VENDORED_OPENSSL_CLI_SOURCES}
    )
    list(FILTER _edge_vendored_openssl_asm_sources INCLUDE REGEX "\\.(S|s|asm)$")
    if(_edge_vendored_openssl_asm_sources)
      if(WIN32)
        enable_language(ASM_MASM)
      else()
        enable_language(ASM)
      endif()
    endif()

    add_library(edge_vendored_openssl STATIC
      ${EDGE_VENDORED_OPENSSL_SOURCES}
    )
    target_include_directories(edge_vendored_openssl
      PUBLIC
        ${EDGE_VENDORED_OPENSSL_INCLUDE_DIRS}
    )
    target_compile_definitions(edge_vendored_openssl
      PRIVATE
        ${EDGE_VENDORED_OPENSSL_DEFINES}
    )
    target_compile_options(edge_vendored_openssl
      PRIVATE
        ${EDGE_VENDORED_OPENSSL_COMPILE_OPTIONS}
    )
    target_link_libraries(edge_vendored_openssl
      PUBLIC
        ${EDGE_VENDORED_OPENSSL_LINK_LIBRARIES}
    )

    add_library(edge_vendored_openssl_ssl INTERFACE)
    target_link_libraries(edge_vendored_openssl_ssl
      INTERFACE
        edge_vendored_openssl
    )

    add_library(OpenSSL::Crypto ALIAS edge_vendored_openssl)
    add_library(OpenSSL::SSL ALIAS edge_vendored_openssl_ssl)

    add_executable(edge_openssl_cli
      ${EDGE_VENDORED_OPENSSL_CLI_SOURCES}
    )
    set_target_properties(edge_openssl_cli PROPERTIES OUTPUT_NAME "openssl-cli")
    target_include_directories(edge_openssl_cli
      PRIVATE
        ${EDGE_VENDORED_OPENSSL_CLI_INCLUDE_DIRS}
    )
    target_compile_definitions(edge_openssl_cli
      PRIVATE
        ${EDGE_VENDORED_OPENSSL_CLI_DEFINES}
    )
    target_compile_options(edge_openssl_cli
      PRIVATE
        ${EDGE_VENDORED_OPENSSL_CLI_COMPILE_OPTIONS}
    )
    target_link_libraries(edge_openssl_cli
      PRIVATE
        OpenSSL::SSL
        ${EDGE_VENDORED_OPENSSL_CLI_LINK_LIBRARIES}
    )
  else()
    if(DEFINED OPENSSL_USE_STATIC_LIBS AND OPENSSL_USE_STATIC_LIBS)
      message(FATAL_ERROR
        "EDGE_SHARED_OPENSSL=ON is incompatible with OPENSSL_USE_STATIC_LIBS=TRUE.")
    endif()

    if(EDGE_SHARED_OPENSSL_INCLUDES)
      set(OPENSSL_INCLUDE_DIR "${EDGE_SHARED_OPENSSL_INCLUDES}" CACHE PATH "" FORCE)
    endif()

    list(LENGTH EDGE_SHARED_OPENSSL_LIBNAME _edge_shared_openssl_libname_count)
    if(NOT _edge_shared_openssl_libname_count EQUAL 2)
      message(FATAL_ERROR
        "EDGE_SHARED_OPENSSL_LIBNAME must contain exactly two entries: crypto;ssl.")
    endif()
    list(GET EDGE_SHARED_OPENSSL_LIBNAME 0 _edge_shared_openssl_crypto_name)
    list(GET EDGE_SHARED_OPENSSL_LIBNAME 1 _edge_shared_openssl_ssl_name)

    if(EDGE_SHARED_OPENSSL_LIBPATH)
      find_library(_edge_shared_openssl_crypto_lib
        NAMES "${_edge_shared_openssl_crypto_name}"
        PATHS "${EDGE_SHARED_OPENSSL_LIBPATH}"
        NO_DEFAULT_PATH
      )
      find_library(_edge_shared_openssl_ssl_lib
        NAMES "${_edge_shared_openssl_ssl_name}"
        PATHS "${EDGE_SHARED_OPENSSL_LIBPATH}"
        NO_DEFAULT_PATH
      )
      if(NOT _edge_shared_openssl_crypto_lib OR NOT _edge_shared_openssl_ssl_lib)
        message(FATAL_ERROR
          "Could not locate shared OpenSSL libraries '${EDGE_SHARED_OPENSSL_LIBNAME}' under "
          "${EDGE_SHARED_OPENSSL_LIBPATH}.")
      endif()
      set(OPENSSL_CRYPTO_LIBRARY "${_edge_shared_openssl_crypto_lib}" CACHE FILEPATH "" FORCE)
      set(OPENSSL_SSL_LIBRARY "${_edge_shared_openssl_ssl_lib}" CACHE FILEPATH "" FORCE)
    endif()

    set(OPENSSL_USE_STATIC_LIBS FALSE CACHE BOOL "" FORCE)
    find_package(OpenSSL REQUIRED)
    edge_require_shared_openssl_target(OpenSSL::Crypto "OpenSSL::Crypto")
    edge_require_shared_openssl_target(OpenSSL::SSL "OpenSSL::SSL")
  endif()
endif()

set(EDGE_EMBEDDED_V8_VERSION "0.0.0-node.0")
if(DEFINED NAPI_V8_INCLUDE_DIR AND EXISTS "${NAPI_V8_INCLUDE_DIR}/v8-version.h")
  file(STRINGS "${NAPI_V8_INCLUDE_DIR}/v8-version.h" _v8_major_line
       REGEX "^#define V8_MAJOR_VERSION [0-9]+$")
  file(STRINGS "${NAPI_V8_INCLUDE_DIR}/v8-version.h" _v8_minor_line
       REGEX "^#define V8_MINOR_VERSION [0-9]+$")
  file(STRINGS "${NAPI_V8_INCLUDE_DIR}/v8-version.h" _v8_build_line
       REGEX "^#define V8_BUILD_NUMBER [0-9]+$")
  file(STRINGS "${NAPI_V8_INCLUDE_DIR}/v8-version.h" _v8_patch_line
       REGEX "^#define V8_PATCH_LEVEL [0-9]+$")
  if(_v8_major_line AND _v8_minor_line AND _v8_build_line AND _v8_patch_line)
    string(REGEX REPLACE "^#define V8_MAJOR_VERSION ([0-9]+)$" "\\1"
           _v8_major "${_v8_major_line}")
    string(REGEX REPLACE "^#define V8_MINOR_VERSION ([0-9]+)$" "\\1"
           _v8_minor "${_v8_minor_line}")
    string(REGEX REPLACE "^#define V8_BUILD_NUMBER ([0-9]+)$" "\\1"
           _v8_build "${_v8_build_line}")
    string(REGEX REPLACE "^#define V8_PATCH_LEVEL ([0-9]+)$" "\\1"
           _v8_patch "${_v8_patch_line}")
    set(EDGE_EMBEDDED_V8_VERSION
      "${_v8_major}.${_v8_minor}.${_v8_build}.${_v8_patch}-node.0")
  endif()
endif()

add_library(edge_node_api STATIC
  src/node_api.cc
  src/edge_napi_embedder_hooks.cc
  src/unofficial_napi_testing_until_gc.cc
)

add_library(edge_environment_core STATIC
  src/edge_environment.cc
)
target_include_directories(edge_environment_core
  PUBLIC
    "${EDGE_ROOT}/src"
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
)
target_link_libraries(edge_environment_core PUBLIC uv_a)
if(EDGE_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_compile_definitions(edge_environment_core PUBLIC EDGE_BUNDLED_NAPI_V8=1)
endif()

set(EDGE_ICU_ROOT "${PROJECT_ROOT}/deps/icu-small")
set(EDGE_ICU_INCLUDE_DIRS
  "${EDGE_ICU_ROOT}/source/common"
  "${EDGE_ICU_ROOT}/source/i18n"
  "${EDGE_ICU_ROOT}/source/stubdata"
)
set(EDGE_ICU_LINK_LIBS "")
file(GLOB_RECURSE EDGE_ICU_COMMON_SOURCES CONFIGURE_DEPENDS
  "${EDGE_ICU_ROOT}/source/common/*.cpp"
)
file(GLOB_RECURSE EDGE_ICU_I18N_SOURCES CONFIGURE_DEPENDS
  "${EDGE_ICU_ROOT}/source/i18n/*.cpp"
)
if(EDGE_IS_WASIX_TARGET)
  include("${PROJECT_ROOT}/wasix/cmake/icu_wasix.cmake")
else()
  set(EDGE_ICU_BASE_DEFINES
    U_ATTRIBUTE_DEPRECATED=
    UCONFIG_NO_SERVICE=1
    U_ENABLE_DYLOAD=0
    U_STATIC_IMPLEMENTATION=1
    U_HAVE_STD_STRING=1
    UCONFIG_NO_BREAK_ITERATION=0
    U_DISABLE_RENAMING=1
  )

  add_library(edge_icu_stubdata STATIC
    "${EDGE_ICU_ROOT}/source/stubdata/stubdata.cpp"
  )
  target_include_directories(edge_icu_stubdata
    PUBLIC
      "${EDGE_ICU_ROOT}/source/common"
      "${EDGE_ICU_ROOT}/source/stubdata"
  )
  target_compile_definitions(edge_icu_stubdata
    PRIVATE
      ${EDGE_ICU_BASE_DEFINES}
      U_COMMON_IMPLEMENTATION=1
  )

  add_library(edge_icu_common STATIC
    ${EDGE_ICU_COMMON_SOURCES}
  )
  target_include_directories(edge_icu_common
    PUBLIC
      "${EDGE_ICU_ROOT}/source/common"
  )
  target_compile_definitions(edge_icu_common
    PRIVATE
      ${EDGE_ICU_BASE_DEFINES}
      U_COMMON_IMPLEMENTATION=1
  )
  target_link_libraries(edge_icu_common
    PUBLIC
      edge_icu_stubdata
  )

  add_library(edge_icu_i18n STATIC
    ${EDGE_ICU_I18N_SOURCES}
  )
  target_include_directories(edge_icu_i18n
    PUBLIC
      "${EDGE_ICU_ROOT}/source/common"
      "${EDGE_ICU_ROOT}/source/i18n"
  )
  target_compile_definitions(edge_icu_i18n
    PRIVATE
      ${EDGE_ICU_BASE_DEFINES}
      U_I18N_IMPLEMENTATION=1
  )
  target_link_libraries(edge_icu_i18n
    PUBLIC
      edge_icu_common
  )

  list(APPEND EDGE_ICU_LINK_LIBS
    edge_icu_i18n
    edge_icu_common
  )
endif()

add_library(edge_ncrypto STATIC
  "${PROJECT_ROOT}/deps/ncrypto/ncrypto.cc"
  "${PROJECT_ROOT}/deps/ncrypto/engine.cc"
)
target_include_directories(edge_ncrypto
  PUBLIC
    "${PROJECT_ROOT}/deps/ncrypto"
)
target_link_libraries(edge_ncrypto
  PUBLIC
    OpenSSL::Crypto
    OpenSSL::SSL
)

add_library(edge_simdutf STATIC
  "${PROJECT_ROOT}/deps/simdutf/simdutf.cpp"
)
add_library(edge_simdjson STATIC
  "${PROJECT_ROOT}/deps/simdjson/simdjson.cpp"
)
add_library(edge_ada STATIC
  "${PROJECT_ROOT}/deps/ada/ada.cpp"
)
target_compile_definitions(edge_ada PUBLIC ADA_USE_UNSAFE_STD_REGEX_PROVIDER)
target_include_directories(edge_ada
  PUBLIC
    "${PROJECT_ROOT}/deps/ada"
)

target_include_directories(edge_simdjson
  PUBLIC
    "${PROJECT_ROOT}/deps"
)

target_include_directories(edge_simdutf
  PUBLIC
    "${PROJECT_ROOT}/deps/simdutf"
)

target_include_directories(edge_node_api
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
)

target_link_libraries(edge_node_api
  PUBLIC
    edge_environment_core
  PRIVATE
    uv_a
)
if(EDGE_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_compile_definitions(napi_v8 PUBLIC EDGE_BUNDLED_NAPI_V8=1)
  target_link_libraries(edge_node_api PUBLIC napi_v8)
endif()

add_library(edge_runtime STATIC
  src/builtin_catalog.cc
  src/edge_environment_runtime.cc
  src/edge_runtime.cc
  src/edge_runtime_platform_v8.cc
  src/edge_node_compat.cc
  src/edge_process.cc
  src/edge_active_resource.cc
  src/edge_module_loader.cc
  src/internal_binding/dispatch.cc
  src/internal_binding/binding_async_context_frame.cc
  src/internal_binding/binding_async_wrap.cc
  src/internal_binding/binding_block_list.cc
  src/internal_binding/binding_blob.cc
  src/internal_binding/binding_buffer.cc
  src/internal_binding/binding_builtins.cc
  src/internal_binding/binding_cares_wrap.cc
  src/internal_binding/binding_config.cc
  src/internal_binding/binding_constants.cc
  src/internal_binding/binding_contextify.cc
  src/internal_binding/binding_credentials.cc
  src/internal_binding/binding_crypto.cc
  src/internal_binding/binding_encoding_binding.cc
  src/internal_binding/binding_errors.cc
  src/internal_binding/binding_fs_event_wrap.cc
  src/internal_binding/binding_fs.cc
  src/internal_binding/binding_fs_dir.cc
  src/internal_binding/binding_heap_utils.cc
  src/internal_binding/binding_http2.cc
  src/internal_binding/binding_http_parser.cc
  src/internal_binding/binding_icu.cc
  src/internal_binding/binding_js_udp_wrap.cc
  src/internal_binding/binding_js_stream.cc
  src/internal_binding/binding_internal_only_v8.cc
  src/internal_binding/binding_module_wrap.cc
  src/internal_binding/binding_modules.cc
  src/internal_binding/binding_messaging.cc
  src/internal_binding/binding_mksnapshot.cc
  src/internal_binding/binding_options.cc
  src/internal_binding/binding_os.cc
  src/internal_binding/binding_performance.cc
  src/internal_binding/binding_permission.cc
  src/internal_binding/binding_pipe_wrap.cc
  src/internal_binding/binding_process_methods.cc
  src/internal_binding/binding_process_wrap.cc
  src/internal_binding/binding_report.cc
  src/internal_binding/binding_sea.cc
  src/internal_binding/binding_serdes.cc
  src/internal_binding/binding_signal_wrap.cc
  src/internal_binding/binding_spawn_sync.cc
  src/internal_binding/binding_stream_pipe.cc
  src/internal_binding/binding_stream_wrap.cc
  src/internal_binding/binding_string_decoder.cc
  src/internal_binding/binding_symbols.cc
  src/internal_binding/binding_task_queue.cc
  src/internal_binding/binding_tcp_wrap.cc
  src/internal_binding/binding_tls_wrap.cc
  src/internal_binding/binding_timers.cc
  src/internal_binding/binding_trace_events.cc
  src/internal_binding/binding_tty_wrap.cc
  src/internal_binding/binding_types.cc
  src/internal_binding/binding_udp_wrap.cc
  src/internal_binding/binding_url.cc
  src/internal_binding/binding_url_pattern.cc
  src/internal_binding/binding_util.cc
  src/internal_binding/binding_v8.cc
  src/internal_binding/binding_uv.cc
  src/internal_binding/binding_watchdog.cc
  src/internal_binding/binding_wasm_web_api.cc
  src/internal_binding/binding_worker.cc
  src/internal_binding/binding_zlib.cc
  src/edge_errors_binding.cc
  "${PROJECT_ROOT}/deps/llhttp/src/api.c"
  "${PROJECT_ROOT}/deps/llhttp/src/http.c"
  "${PROJECT_ROOT}/deps/llhttp/src/llhttp.c"
  src/edge_fs.cc
  src/edge_buffer.cc
  src/edge_os.cc
  src/edge_path.cc
  src/edge_encoding.cc
  src/edge_string_decoder.cc
  src/edge_handle_wrap.cc
  src/edge_async_wrap.cc
  src/edge_http_parser.cc
  src/edge_js_udp_wrap.cc
  src/edge_js_stream.cc
  src/edge_stream_base.cc
  src/edge_stream_listener.cc
  src/edge_udp_listener.cc
  src/edge_stream_wrap.cc
  src/edge_process_wrap.cc
  src/edge_task_queue.cc
  src/edge_tcp_wrap.cc
  src/edge_tls_wrap.cc
  src/edge_tty_wrap.cc
  src/edge_pipe_wrap.cc
  src/edge_signal_wrap.cc
  src/edge_cares_wrap.cc
  src/edge_udp_wrap.cc
  src/edge_url.cc
  src/edge_url_pattern.cc
  src/edge_util.cc
  src/edge_worker_env.cc
  src/edge_spawn_sync.cc
  src/edge_timers_host.cc
  src/crypto/edge_crypto_bio.cc
  src/crypto/edge_crypto_binding.cc
  src/edge_crypto.cc
)

file(GLOB_RECURSE EDGE_BUILTIN_LIB_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_ROOT}/lib/*.js"
)
file(GLOB_RECURSE EDGE_BUILTIN_DEP_SOURCES CONFIGURE_DEPENDS
  "${PROJECT_ROOT}/deps/*.js"
  "${PROJECT_ROOT}/deps/*.mjs"
)
set(EDGE_BUILTIN_CATALOG_HEADER "${CMAKE_CURRENT_BINARY_DIR}/edge_builtin_catalog_data.h")
add_custom_command(
  OUTPUT "${EDGE_BUILTIN_CATALOG_HEADER}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_ROOT}/scripts/generate_builtin_catalog_header.py"
    --project-root "${PROJECT_ROOT}"
    --output "${EDGE_BUILTIN_CATALOG_HEADER}"
  DEPENDS
    "${PROJECT_ROOT}/scripts/generate_builtin_catalog_header.py"
    ${EDGE_BUILTIN_LIB_SOURCES}
    ${EDGE_BUILTIN_DEP_SOURCES}
  COMMENT "Generating Edge builtin catalog"
  VERBATIM
)
add_custom_target(edge_builtin_catalog_gen DEPENDS "${EDGE_BUILTIN_CATALOG_HEADER}")
add_dependencies(edge_runtime edge_builtin_catalog_gen)
target_include_directories(edge_runtime
  PUBLIC
    "${EDGE_ROOT}/src"
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${PROJECT_ROOT}/deps"
    "${PROJECT_ROOT}/deps/llhttp/include"
    "${PROJECT_ROOT}/deps/ncrypto"
    ${EDGE_ICU_INCLUDE_DIRS}
)
target_link_libraries(edge_runtime
  PUBLIC
    edge_environment_core
    edge_node_api
    ${EDGE_ICU_LINK_LIBS}
  PRIVATE
    edge_simdjson
    edge_simdutf
    edge_ada
    edge_ncrypto
    edge_brotli
    edge_nghttp2
    edge_zstd
    uv_a
    c-ares::cares
    OpenSSL::Crypto
    zlibstatic
)
if(EDGE_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_link_libraries(edge_runtime PUBLIC napi_v8)
endif()
set(EDGE_NODE_SHARED_OPENSSL 0)
if(NOT EDGE_IS_WASIX_TARGET AND EDGE_SHARED_OPENSSL)
  set(EDGE_NODE_SHARED_OPENSSL 1)
endif()
target_compile_definitions(edge_runtime
  PRIVATE
    EDGE_HAS_ICU=1
    EDGE_HAS_SMALL_ICU=1
    EDGE_NODE_SHARED_OPENSSL=${EDGE_NODE_SHARED_OPENSSL}
    EDGE_ENABLE_STARTUP_PROFILE=$<BOOL:${EDGE_ENABLE_STARTUP_PROFILE}>
    EDGE_EMBEDDED_V8_VERSION="${EDGE_EMBEDDED_V8_VERSION}"
    EDGE_VERSION_COMMIT="${EDGE_VERSION_COMMIT}"
    EDGE_VERSION_SUFFIX="${EDGE_VERSION_SUFFIX}"
)
if(EDGE_IS_WASIX_TARGET)
  target_sources(edge_runtime PRIVATE
    wasix/src/wasix_compat.cc
  )
endif()
if(EDGE_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_compile_definitions(edge_runtime PRIVATE EDGE_BUNDLED_NAPI_V8=1)
endif()

add_library(edge_cli STATIC
  src/edge_cli.cc
  src/edge_compat_exec.cc
)
target_include_directories(edge_cli
  PUBLIC
    "${EDGE_ROOT}/src"
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
)
target_link_libraries(edge_cli
  PUBLIC
    edge_runtime
  PRIVATE
    uv_a
)
target_compile_definitions(edge_cli
  PRIVATE
    EDGE_ENABLE_STARTUP_PROFILE=$<BOOL:${EDGE_ENABLE_STARTUP_PROFILE}>
    EDGE_VERSION_COMMIT="${EDGE_VERSION_COMMIT}"
    EDGE_VERSION_SUFFIX="${EDGE_VERSION_SUFFIX}"
)
if(NOT EDGE_DEFAULT_WASMER_PACKAGE STREQUAL "")
target_compile_definitions(edge_cli
  PRIVATE
    EDGE_DEFAULT_WASMER_PACKAGE="${EDGE_DEFAULT_WASMER_PACKAGE}"
)
endif()

option(EDGE_BUILD_CLI "Build edge CLI binary" ON)
if(EDGE_BUILD_CLI)
  set(EDGE_CLI_SOURCES
    "${EDGE_ROOT}/src/edge_main.cc"
  )
  add_executable(edge
    ${EDGE_CLI_SOURCES}
  )
  set_target_properties(edge PROPERTIES ENABLE_EXPORTS ON)
  target_include_directories(edge
    PRIVATE
      "${EDGE_ROOT}/src"
      "${NAPI_INCLUDE_ROOT}"
  )
  target_link_libraries(edge
    PRIVATE
      edge_cli
  )
  if(EDGE_NAPI_PROVIDER STREQUAL "bundled-v8" AND NAPI_V8_DEFINES)
    target_compile_definitions(edge PRIVATE ${NAPI_V8_DEFINES})
  endif()
  if(EDGE_ALLOW_UNDEFINED_IMPORTS)
    target_link_options(edge PRIVATE
      "-Wl,--allow-undefined"
      "-Wl,--import-undefined"
    )
  endif()

  if(NOT EDGE_IS_WASIX_TARGET)
    add_executable(edgeenv
      "${EDGE_ROOT}/src/edgeenv_main.cc"
    )
    target_include_directories(edgeenv
      PRIVATE
        "${EDGE_ROOT}/src"
    )
  endif()
endif()

if(NOT EDGE_IS_WASIX_TARGET)
  enable_testing()
  add_subdirectory(tests)
endif()
