#include "internal/napi_jsc_env.h"

#if defined(NAPI_JSC_ENABLE_PRIVATE_RUNTIME_OPTIONS)

#include <dlfcn.h>

#include <mutex>

namespace {

using JscInitializeFn = void (*)();
using JscSetOptionsFn = bool (*)(const char*);
using JscNotifyOptionsChangedFn = void (*)();

constexpr char kEnableSharedArrayBufferOption[] = "useSharedArrayBuffer=true";

#if defined(NAPI_JSC_STATIC_BUN_SDK)
extern void napi_jsc_private_initialize() __asm__("__ZN3JSC10initializeEv");
extern bool napi_jsc_private_set_options(const char*) __asm__("__ZN3JSC7Options10setOptionsEPKc");
extern void napi_jsc_private_notify_options_changed()
    __asm__("__ZN3JSC7Options20notifyOptionsChangedEv");
#endif

std::once_flag g_runtime_init_once;

void ConfigureRuntimeOptions() {
#if defined(NAPI_JSC_STATIC_BUN_SDK)
  napi_jsc_private_initialize();
  if (napi_jsc_private_set_options(kEnableSharedArrayBufferOption)) {
    napi_jsc_private_notify_options_changed();
  }
#else
  auto* initialize =
      reinterpret_cast<JscInitializeFn>(dlsym(RTLD_DEFAULT, "__ZN3JSC10initializeEv"));
  auto* set_options = reinterpret_cast<JscSetOptionsFn>(
      dlsym(RTLD_DEFAULT, "__ZN3JSC7Options10setOptionsEPKc"));
  auto* notify_options_changed = reinterpret_cast<JscNotifyOptionsChangedFn>(
      dlsym(RTLD_DEFAULT, "__ZN3JSC7Options20notifyOptionsChangedEv"));
  if (initialize == nullptr || set_options == nullptr || notify_options_changed == nullptr) {
    return;
  }
  initialize();
  if (set_options(kEnableSharedArrayBufferOption)) {
    notify_options_changed();
  }
#endif
}

}  // namespace

#endif

void napi_jsc_prepare_runtime_for_context_creation() {
#if defined(NAPI_JSC_ENABLE_PRIVATE_RUNTIME_OPTIONS)
  std::call_once(g_runtime_init_once, ConfigureRuntimeOptions);
#endif
}
