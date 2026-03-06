cmake_minimum_required(VERSION 3.20)
project(ubi LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(UBI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NAPI_V8_ROOT "${UBI_ROOT}/../napi/v8")
set(PROJECT_ROOT "${UBI_ROOT}/../")
set(NAPI_INCLUDE_ROOT "${PROJECT_ROOT}/napi/include")

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
option(UBI_EXTERNAL_NAPI_V8
  "Deprecated alias. Use UBI_NAPI_PROVIDER=imports."
  OFF
)
option(UBI_ALLOW_UNDEFINED_IMPORTS
  "Allow undefined imports at link time (useful for external runtime deps in wasm/wasix)"
  OFF
)

set(UBI_IS_WASIX_TARGET OFF)
if(CMAKE_SYSTEM_NAME MATCHES "WASI|WASIX" OR
   CMAKE_C_COMPILER MATCHES "wasixcc" OR
   CMAKE_CXX_COMPILER MATCHES "wasixcc\\+\\+")
  set(UBI_IS_WASIX_TARGET ON)
  set(UBI_ALLOW_UNDEFINED_IMPORTS ON CACHE BOOL "" FORCE)
endif()

set(_ubi_napi_provider_default "bundled-v8")
if(UBI_IS_WASIX_TARGET)
  set(_ubi_napi_provider_default "imports")
endif()
set(UBI_NAPI_PROVIDER
  "${_ubi_napi_provider_default}"
  CACHE STRING "N-API provider backend: bundled-v8|imports"
)
set_property(CACHE UBI_NAPI_PROVIDER PROPERTY STRINGS bundled-v8 imports)
if(UBI_EXTERNAL_NAPI_V8)
  message(WARNING
    "UBI_EXTERNAL_NAPI_V8 is deprecated; use UBI_NAPI_PROVIDER=imports.")
  set(UBI_NAPI_PROVIDER "imports" CACHE STRING
    "N-API provider backend: bundled-v8|imports" FORCE)
endif()
string(TOLOWER "${UBI_NAPI_PROVIDER}" UBI_NAPI_PROVIDER)
if(NOT UBI_NAPI_PROVIDER MATCHES "^(bundled-v8|imports)$")
  message(FATAL_ERROR
    "Invalid UBI_NAPI_PROVIDER='${UBI_NAPI_PROVIDER}'. "
    "Valid values: bundled-v8, imports.")
endif()
set(UBI_NAPI_PROVIDER "${UBI_NAPI_PROVIDER}" CACHE STRING
  "N-API provider backend: bundled-v8|imports" FORCE)

# Hard gate: ubi runtime/library sources must never directly reference V8.
file(GLOB_RECURSE UBI_PURE_SRC_FILES
  "${UBI_ROOT}/src/*.h"
  "${UBI_ROOT}/src/*.cc"
  "${UBI_ROOT}/src/*.c"
)
foreach(UBI_CODE_FILE IN LISTS UBI_PURE_SRC_FILES)
  file(READ "${UBI_CODE_FILE}" UBI_CODE_CONTENTS)
  if(UBI_CODE_CONTENTS MATCHES "#include <v8\\.h>" OR
     UBI_CODE_CONTENTS MATCHES "#include <libplatform/libplatform\\.h>" OR
     UBI_CODE_CONTENTS MATCHES "v8::")
    message(FATAL_ERROR
      "V8 usage is forbidden by policy in ubi/src. "
      "Found in: ${UBI_CODE_FILE}")
  endif()
endforeach()

if(UBI_NAPI_PROVIDER STREQUAL "bundled-v8")
  set(NAPI_V8_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory("${NAPI_V8_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/napi-v8")
elseif(UBI_NAPI_PROVIDER STREQUAL "imports")
  add_library(ubi_napi_headers INTERFACE)
  target_include_directories(ubi_napi_headers INTERFACE "${NAPI_INCLUDE_ROOT}")
endif()

if(UBI_IS_WASIX_TARGET)
  set(UBI_LIBUV_ROOT "${PROJECT_ROOT}/deps/libuv-wasix")
else()
  set(UBI_LIBUV_ROOT "${PROJECT_ROOT}/deps/uv")
endif()
if(NOT EXISTS "${UBI_LIBUV_ROOT}/CMakeLists.txt")
  message(FATAL_ERROR
    "libuv CMake project not found at ${UBI_LIBUV_ROOT}. "
    "For WASIX, run ubi/scripts/setup-wasix-deps.sh first.")
endif()

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory("${UBI_LIBUV_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/deps/uv")

set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
add_subdirectory("${PROJECT_ROOT}/deps/cares" "${CMAKE_CURRENT_BINARY_DIR}/deps/cares")

if(UBI_IS_WASIX_TARGET)
  set(UBI_OPENSSL_WASIX_ROOT
    "${PROJECT_ROOT}/deps/openssl-wasix"
    CACHE PATH "Path to wasix-org OpenSSL source/build tree"
  )
  set(_ubi_openssl_include_dir "${UBI_OPENSSL_WASIX_ROOT}/include")
  if(NOT EXISTS "${_ubi_openssl_include_dir}")
    message(FATAL_ERROR
      "OpenSSL include directory not found: ${_ubi_openssl_include_dir}. "
      "For WASIX, clone deps with ubi/scripts/setup-wasix-deps.sh.")
  endif()

  set(_ubi_openssl_crypto "")
  foreach(_candidate
      "${UBI_OPENSSL_WASIX_ROOT}/lib/libcrypto.a"
      "${UBI_OPENSSL_WASIX_ROOT}/libcrypto.a")
    if(EXISTS "${_candidate}")
      set(_ubi_openssl_crypto "${_candidate}")
      break()
    endif()
  endforeach()

  set(_ubi_openssl_ssl "")
  foreach(_candidate
      "${UBI_OPENSSL_WASIX_ROOT}/lib/libssl.a"
      "${UBI_OPENSSL_WASIX_ROOT}/libssl.a")
    if(EXISTS "${_candidate}")
      set(_ubi_openssl_ssl "${_candidate}")
      break()
    endif()
  endforeach()

  if(_ubi_openssl_crypto STREQUAL "" OR _ubi_openssl_ssl STREQUAL "")
    message(FATAL_ERROR
      "WASIX OpenSSL static libraries not found under ${UBI_OPENSSL_WASIX_ROOT}. "
      "Expected libcrypto.a and libssl.a (either root or lib/).")
  endif()

  add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
  set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${_ubi_openssl_crypto}"
    INTERFACE_INCLUDE_DIRECTORIES "${_ubi_openssl_include_dir}"
  )
  add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
  set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${_ubi_openssl_ssl}"
    INTERFACE_INCLUDE_DIRECTORIES "${_ubi_openssl_include_dir}"
  )
  target_link_libraries(OpenSSL::SSL INTERFACE OpenSSL::Crypto)
else()
  find_package(OpenSSL REQUIRED)
endif()

set(UBI_EMBEDDED_V8_VERSION "0.0.0-node.0")
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
    set(UBI_EMBEDDED_V8_VERSION
      "${_v8_major}.${_v8_minor}.${_v8_build}.${_v8_patch}-node.0")
  endif()
endif()

add_library(ubi_node_api STATIC
  src/node_api.cc
  src/unofficial_napi_testing_until_gc.cc
)
if(UBI_NAPI_PROVIDER STREQUAL "imports")
  set_source_files_properties(src/node_api.cc PROPERTIES
    COMPILE_DEFINITIONS "NAPI_EXTERN=")
endif()

add_library(ubi_ncrypto STATIC
  "${PROJECT_ROOT}/deps/ncrypto/ncrypto.cc"
  "${PROJECT_ROOT}/deps/ncrypto/engine.cc"
)
target_include_directories(ubi_ncrypto
  PUBLIC
    "${PROJECT_ROOT}/deps/ncrypto"
)
target_link_libraries(ubi_ncrypto
  PUBLIC
    OpenSSL::Crypto
    OpenSSL::SSL
)

add_library(ubi_simdutf STATIC
  "${PROJECT_ROOT}/deps/simdutf/simdutf.cpp"
)
add_library(ubi_simdjson STATIC
  "${PROJECT_ROOT}/deps/simdjson/simdjson.cpp"
)
add_library(ubi_ada STATIC
  "${PROJECT_ROOT}/deps/ada/ada.cpp"
)
target_include_directories(ubi_ada
  PUBLIC
    "${PROJECT_ROOT}/deps/ada"
)

target_include_directories(ubi_simdjson
  PUBLIC
    "${PROJECT_ROOT}/deps"
)

target_include_directories(ubi_simdutf
  PUBLIC
    "${PROJECT_ROOT}/deps/simdutf"
)

target_include_directories(ubi_node_api
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
)

target_link_libraries(ubi_node_api PRIVATE uv_a)
if(UBI_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_link_libraries(ubi_node_api PUBLIC napi_v8)
endif()

add_library(ubi_runtime STATIC
  src/builtin_catalog.cc
  src/ubi_runtime.cc
  src/ubi_runtime_platform_v8.cc
  src/ubi_process.cc
  src/ubi_module_loader.cc
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
  src/internal_binding/binding_serdes.cc
  src/internal_binding/binding_signal_wrap.cc
  src/internal_binding/binding_spawn_sync.cc
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
  src/internal_binding/binding_wasm_web_api.cc
  src/internal_binding/binding_worker.cc
  src/internal_binding/binding_zlib.cc
  src/ubi_errors_binding.cc
  "${PROJECT_ROOT}/deps/llhttp/src/api.c"
  "${PROJECT_ROOT}/deps/llhttp/src/http.c"
  "${PROJECT_ROOT}/deps/llhttp/src/llhttp.c"
  src/ubi_fs.cc
  src/ubi_buffer.cc
  src/ubi_os.cc
  src/ubi_encoding.cc
  src/ubi_string_decoder.cc
  src/ubi_async_wrap.cc
  src/ubi_http_parser.cc
  src/ubi_js_udp_wrap.cc
  src/ubi_js_stream.cc
  src/ubi_stream_base.cc
  src/ubi_stream_listener.cc
  src/ubi_udp_listener.cc
  src/ubi_stream_wrap.cc
  src/ubi_process_wrap.cc
  src/ubi_tcp_wrap.cc
  src/ubi_tls_wrap.cc
  src/ubi_tty_wrap.cc
  src/ubi_pipe_wrap.cc
  src/ubi_signal_wrap.cc
  src/ubi_cares_wrap.cc
  src/ubi_udp_wrap.cc
  src/ubi_url.cc
  src/ubi_util.cc
  src/ubi_spawn_sync.cc
  src/ubi_timers_host.cc
  src/crypto/ubi_crypto_binding.cc
  src/ubi_crypto.cc
)
target_include_directories(ubi_runtime
  PUBLIC
    "${UBI_ROOT}/src"
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
    "${PROJECT_ROOT}/deps"
    "${PROJECT_ROOT}/deps/llhttp/include"
    "${PROJECT_ROOT}/deps/ncrypto"
    "${PROJECT_ROOT}/deps/icu-small/source/common"
    "${PROJECT_ROOT}/deps/icu-small/source/i18n"
)
target_link_libraries(ubi_runtime
  PUBLIC
    ubi_node_api
  PRIVATE
    ubi_simdjson
    ubi_simdutf
    ubi_ada
    ubi_ncrypto
    uv_a
    c-ares::cares
    OpenSSL::Crypto
)
if(APPLE)
  target_link_libraries(ubi_runtime PRIVATE icucore)
endif()
if(UBI_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_link_libraries(ubi_runtime PUBLIC napi_v8)
endif()
target_compile_definitions(ubi_runtime
  PRIVATE
    UBI_EMBEDDED_V8_VERSION="${UBI_EMBEDDED_V8_VERSION}"
)
if(UBI_NAPI_PROVIDER STREQUAL "bundled-v8")
  target_compile_definitions(ubi_runtime PRIVATE UBI_BUNDLED_NAPI_V8=1)
endif()

add_library(ubi_cli STATIC
  src/ubi_cli.cc
  src/ubi_compat_exec.cc
)
target_include_directories(ubi_cli
  PUBLIC
    "${UBI_ROOT}/src"
  PRIVATE
    "${NAPI_INCLUDE_ROOT}"
)
target_link_libraries(ubi_cli
  PUBLIC
    ubi_runtime
  PRIVATE
    uv_a
)

option(UBI_BUILD_CLI "Build ubi CLI binary" ON)
if(UBI_BUILD_CLI)
  add_executable(ubi
    "${UBI_ROOT}/src/ubi_main.cc"
  )
  target_include_directories(ubi
    PRIVATE
      "${UBI_ROOT}/src"
      "${NAPI_INCLUDE_ROOT}"
  )
  target_link_libraries(ubi
    PRIVATE
      ubi_cli
  )
  if(UBI_NAPI_PROVIDER STREQUAL "bundled-v8" AND NAPI_V8_DEFINES)
    target_compile_definitions(ubi PRIVATE ${NAPI_V8_DEFINES})
  endif()
  if(UBI_ALLOW_UNDEFINED_IMPORTS)
    target_link_options(ubi PRIVATE
      "-Wl,--allow-undefined"
      "-Wl,--import-undefined"
    )
  endif()

  add_executable(ubienv
    "${UBI_ROOT}/src/ubienv_main.cc"
  )
  target_include_directories(ubienv
    PRIVATE
      "${UBI_ROOT}/src"
      "${NAPI_INCLUDE_ROOT}"
  )
  target_link_libraries(ubienv
    PRIVATE
      ubi_cli
  )
  if(UBI_NAPI_PROVIDER STREQUAL "bundled-v8" AND NAPI_V8_DEFINES)
    target_compile_definitions(ubienv PRIVATE ${NAPI_V8_DEFINES})
  endif()
  if(UBI_ALLOW_UNDEFINED_IMPORTS)
    target_link_options(ubienv PRIVATE
      "-Wl,--allow-undefined"
      "-Wl,--import-undefined"
    )
  endif()
endif()

if(NOT UBI_IS_WASIX_TARGET)
  enable_testing()
  add_subdirectory(tests)
endif()
