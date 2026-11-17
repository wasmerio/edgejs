function(edge_configure_napi_provider)
  set(_edge_napi_provider_default "bundled-v8")
  if(EDGE_IS_WASIX_TARGET)
    set(_edge_napi_provider_default "imports")
  endif()
  set(EDGE_NAPI_PROVIDER
    "${_edge_napi_provider_default}"
    CACHE STRING "N-API provider backend: bundled-v8|imports|quickjs"
  )
  set_property(CACHE EDGE_NAPI_PROVIDER PROPERTY STRINGS bundled-v8 imports quickjs)
  if(EDGE_EXTERNAL_NAPI_V8)
    message(WARNING
      "EDGE_EXTERNAL_NAPI_V8 is deprecated; use EDGE_NAPI_PROVIDER=imports.")
    set(EDGE_NAPI_PROVIDER "imports" CACHE STRING
      "N-API provider backend: bundled-v8|imports|quickjs" FORCE)
  endif()
  string(TOLOWER "${EDGE_NAPI_PROVIDER}" EDGE_NAPI_PROVIDER)
  if(NOT EDGE_NAPI_PROVIDER MATCHES "^(bundled-v8|imports|quickjs)$")
    message(FATAL_ERROR
      "Invalid EDGE_NAPI_PROVIDER='${EDGE_NAPI_PROVIDER}'. "
      "Valid values: bundled-v8, imports, quickjs.")
  endif()
  set(EDGE_NAPI_PROVIDER "${EDGE_NAPI_PROVIDER}" CACHE STRING
    "N-API provider backend: bundled-v8|imports|quickjs" FORCE)
  if(EDGE_IS_WASIX_TARGET AND EDGE_NAPI_PROVIDER STREQUAL "imports")
    set(EDGE_ALLOW_UNDEFINED_IMPORTS ON CACHE BOOL "" FORCE)
  elseif(EDGE_IS_WASIX_TARGET AND
         EDGE_NAPI_PROVIDER STREQUAL "quickjs" AND
         EDGE_QUICKJS_WEBASSEMBLY)
    set(EDGE_ALLOW_UNDEFINED_IMPORTS ON CACHE BOOL "" FORCE)
  elseif(EDGE_NAPI_PROVIDER STREQUAL "quickjs")
    set(EDGE_ALLOW_UNDEFINED_IMPORTS OFF CACHE BOOL "" FORCE)
  endif()

  set(EDGE_NAPI_PROVIDER "${EDGE_NAPI_PROVIDER}" PARENT_SCOPE)
  set(EDGE_ALLOW_UNDEFINED_IMPORTS "${EDGE_ALLOW_UNDEFINED_IMPORTS}" PARENT_SCOPE)
endfunction()

# The Linux V8 prebuilt (wasmerio/v8-custom-builds, napi's default
# NAPI_V8_BUILD_METHOD) holds ELF objects with compact relocations (CREL),
# which only LLD 20 or newer reads: GNU ld rejects them ("unknown
# architecture", .eh_frame errors). Unless the build already names a linker
# (-fuse-ld= or --ld-path= in the linker flags or LDFLAGS, or
# CMAKE_LINKER_TYPE), link with an installed LLD that is new enough. With
# none, stop at configure time for the prebuilt, and warn for a V8 library of
# one's own, which may not need it.
function(edge_select_v8_linker)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING)
    return()
  endif()
  set(_flags
    "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_SHARED_LINKER_FLAGS} ${CMAKE_MODULE_LINKER_FLAGS} $ENV{LDFLAGS}")
  if(_flags MATCHES "-fuse-ld=|--ld-path=" OR CMAKE_LINKER_TYPE)
    return()
  endif()

  find_program(EDGE_LLD_LINKER NAMES ld.lld-22 ld.lld-21 ld.lld-20 ld.lld
    DOC "LLD, which reads the V8 prebuilt's compact relocations")
  set(_version 0)
  if(EDGE_LLD_LINKER)
    execute_process(COMMAND "${EDGE_LLD_LINKER}" --version
      OUTPUT_VARIABLE _out ERROR_QUIET)
    if(_out MATCHES "LLD ([0-9]+)")
      set(_version "${CMAKE_MATCH_1}")
    endif()
  endif()

  if(_version GREATER_EQUAL 20)
    get_filename_component(_name "${EDGE_LLD_LINKER}" NAME)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      add_link_options("--ld-path=${EDGE_LLD_LINKER}")
      message(STATUS "EDGE: linking with ${EDGE_LLD_LINKER} (LLD ${_version}) for V8")
      return()
    elseif(_name STREQUAL "ld.lld")
      # GCC takes only -fuse-ld=lld, which runs ld.lld from PATH.
      add_link_options("-fuse-ld=lld")
      message(STATUS "EDGE: linking with ld.lld (LLD ${_version}) for V8")
      return()
    endif()
  endif()

  set(_message
    "The Linux V8 prebuilt uses ELF compact relocations (CREL), which need LLD 20 "
    "or newer to link; GNU ld cannot. Install it (apt-get install lld-20) and "
    "reconfigure, or choose a linker that reads CREL with LDFLAGS=-fuse-ld=... "
    "(with GCC, ld.lld 20+ must be on PATH as ld.lld).")
  if(EDGE_LLD_LINKER)
    list(APPEND _message " Found ${EDGE_LLD_LINKER}, LLD ${_version}.")
    # Look again next time: one installed later is found then.
    unset(EDGE_LLD_LINKER CACHE)
  endif()
  string(CONCAT _message ${_message})

  set(_method "prebuilt")
  if(DEFINED NAPI_V8_BUILD_METHOD AND NOT NAPI_V8_BUILD_METHOD STREQUAL "")
    set(_method "${NAPI_V8_BUILD_METHOD}")
  elseif(DEFINED ENV{NAPI_V8_BUILD_METHOD} AND NOT "$ENV{NAPI_V8_BUILD_METHOD}" STREQUAL "")
    set(_method "$ENV{NAPI_V8_BUILD_METHOD}")
  endif()
  string(TOLOWER "${_method}" _method)
  if((_method STREQUAL "prebuilt" AND (NOT DEFINED NAPI_V8_LIBRARY OR NAPI_V8_LIBRARY STREQUAL ""))
     OR NAPI_V8_LIBRARY MATCHES "/_v8_cache/")
    message(FATAL_ERROR "${_message}")
  endif()
  message(WARNING "${_message}")
endfunction()
