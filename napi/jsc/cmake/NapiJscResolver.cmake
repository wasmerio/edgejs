include(CheckCXXSourceCompiles)

function(_napi_jsc_read_value out_var canonical_name)
  if(DEFINED ${canonical_name} AND NOT "${${canonical_name}}" STREQUAL "")
    set(${out_var} "${${canonical_name}}" PARENT_SCOPE)
    return()
  endif()
  if(DEFINED ENV{${canonical_name}} AND NOT "$ENV{${canonical_name}}" STREQUAL "")
    set(${out_var} "$ENV{${canonical_name}}" PARENT_SCOPE)
    return()
  endif()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

function(napi_jsc_resolve_configuration)
  _napi_jsc_read_value(_include NAPI_JSC_INCLUDE_DIR)
  _napi_jsc_read_value(_library NAPI_JSC_LIBRARY)
  _napi_jsc_read_value(_extra NAPI_JSC_EXTRA_LIBS)
  _napi_jsc_read_value(_defines NAPI_JSC_DEFINES)
  _napi_jsc_read_value(_bun_root BUN_WEBKIT_ROOT)
  set(_auto_extra)
  set(_auto_defines)
  set(_bun_sdk_root "")

  if(NOT _bun_root STREQUAL "" AND
     (_include STREQUAL "" OR _library STREQUAL ""))
    if(NOT EXISTS "${_bun_root}/include/JavaScriptCore/JavaScript.h" OR
       NOT EXISTS "${_bun_root}/lib/libJavaScriptCore.a" OR
       NOT EXISTS "${_bun_root}/lib/libWTF.a" OR
       NOT EXISTS "${_bun_root}/lib/libbmalloc.a")
      message(FATAL_ERROR
        "BUN_WEBKIT_ROOT='${_bun_root}' does not look like a Bun WebKit SDK.")
    endif()
    set(_bun_sdk_root "${_bun_root}")
    if(_include STREQUAL "")
      set(_include "${_bun_root}/include")
    endif()
    if(_library STREQUAL "")
      set(_library "${_bun_root}/lib/libJavaScriptCore.a")
    endif()
  endif()

  if(APPLE)
    if(_library STREQUAL "")
      find_library(_framework JavaScriptCore)
      if(_framework)
        set(_library "${_framework}")
      endif()
    endif()
    if(_include STREQUAL "")
      execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _sdk_root
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(_sdk_root AND
         EXISTS "${_sdk_root}/System/Library/Frameworks/JavaScriptCore.framework/Headers")
        set(_include "${_sdk_root}/System/Library/Frameworks/JavaScriptCore.framework/Headers")
      endif()
    endif()
  endif()

  if(_bun_sdk_root STREQUAL "" AND NOT _library STREQUAL "")
    get_filename_component(_library_name "${_library}" NAME)
    if(_library_name STREQUAL "libJavaScriptCore.a")
      get_filename_component(_library_dir "${_library}" DIRECTORY)
      get_filename_component(_candidate_root "${_library_dir}" DIRECTORY)
      if(EXISTS "${_candidate_root}/include/JavaScriptCore/JavaScript.h" AND
         EXISTS "${_candidate_root}/lib/libWTF.a" AND
         EXISTS "${_candidate_root}/lib/libbmalloc.a")
        set(_bun_sdk_root "${_candidate_root}")
        if(_include STREQUAL "" AND
           EXISTS "${_candidate_root}/include/JavaScriptCore/JavaScript.h")
          set(_include "${_candidate_root}/include")
        endif()
      endif()
    endif()
  endif()

  if(NOT _bun_sdk_root STREQUAL "")
    list(APPEND _auto_extra
      "${_bun_sdk_root}/lib/libWTF.a"
      "${_bun_sdk_root}/lib/libbmalloc.a"
    )
    list(APPEND _auto_defines
      NAPI_JSC_ENABLE_PRIVATE_RUNTIME_OPTIONS=1
      NAPI_JSC_STATIC_BUN_SDK=1
    )
    if(APPLE)
      list(APPEND _auto_extra icucore)
    endif()
    message(STATUS "Detected Bun WebKit static SDK at ${_bun_sdk_root}")
  endif()

  if(_include STREQUAL "")
    message(FATAL_ERROR "JavaScriptCore headers not found. Set NAPI_JSC_INCLUDE_DIR.")
  endif()
  if(_library STREQUAL "")
    message(FATAL_ERROR "JavaScriptCore library not found. Set NAPI_JSC_LIBRARY.")
  endif()

  set(_resolved_extra ${_extra})
  set(_resolved_defines ${_defines})
  list(APPEND _resolved_extra ${_auto_extra})
  list(APPEND _resolved_defines ${_auto_defines})
  list(REMOVE_DUPLICATES _resolved_extra)
  list(REMOVE_DUPLICATES _resolved_defines)

  set(CMAKE_REQUIRED_INCLUDES "${_include}")
  set(CMAKE_REQUIRED_LIBRARIES "${_library};${_resolved_extra}")

  check_cxx_source_compiles([=[
    #include <JavaScriptCore/JavaScript.h>
    extern "C" {
      void Bun__errorInstance__finalize(void*) {}
      void Bun__reportUnhandledError(void*, unsigned long long) {}
      void* WTFTimer__create(void*) { return nullptr; }
      void WTFTimer__update(void*, double, bool) {}
      void WTFTimer__deinit(void*) {}
      bool WTFTimer__isActive(const void*) { return false; }
      void WTFTimer__cancel(void*) {}
      double WTFTimer__secondsUntilTimer(void*) { return 0.0; }
    }
    int main() {
      JSGlobalContextRef ctx = JSGlobalContextCreate(nullptr);
      JSValueRef exception = nullptr;
      JSObjectRef resolve = nullptr;
      JSObjectRef reject = nullptr;
      JSObjectRef promise = JSObjectMakeDeferredPromise(ctx, &resolve, &reject, &exception);
      JSValueRef bigint = JSBigIntCreateWithInt64(ctx, 1, &exception);
      JSGlobalContextRelease(ctx);
      return (promise && bigint && !exception) ? 0 : 1;
    }
  ]=] NAPI_JSC_HAS_REQUIRED_APIS)

  if(NOT NAPI_JSC_HAS_REQUIRED_APIS)
    message(FATAL_ERROR
      "The configured JavaScriptCore build does not expose required BigInt/Promise APIs.")
  endif()

  set(NAPI_JSC_INCLUDE_DIR "${_include}" CACHE PATH "Path to JavaScriptCore headers" FORCE)
  set(NAPI_JSC_LIBRARY "${_library}" CACHE FILEPATH "Path to JavaScriptCore library" FORCE)
  set(NAPI_JSC_EXTRA_LIBS "${_resolved_extra}" CACHE STRING "Extra JavaScriptCore libraries" FORCE)
  set(NAPI_JSC_DEFINES "${_resolved_defines}" CACHE STRING "Extra JavaScriptCore compile definitions" FORCE)
endfunction()
