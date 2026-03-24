#include "edge_napi_embedder_hooks.h"

#include <mutex>

#include <uv.h>

#include "unofficial_napi.h"

namespace {

napi_status GetEmbedderMemoryInfo(void* /*target*/,
                                  unofficial_napi_embedder_memory_info* info_out) {
  if (info_out == nullptr) return napi_invalid_arg;
  info_out->total_memory = uv_get_total_memory();
  info_out->constrained_memory = uv_get_constrained_memory();
  return napi_ok;
}

napi_status PumpShutdownLoop(void* /*target*/, void* handle) {
  if (handle == nullptr) return napi_ok;
  (void)uv_run(static_cast<uv_loop_t*>(handle), UV_RUN_ONCE);
  return napi_ok;
}

}  // namespace

void EdgeInstallNapiEmbedderHooks() {
  static std::once_flag once;
  std::call_once(once, []() {
    unofficial_napi_embedder_hooks hooks{};
    hooks.memory_info_callback = GetEmbedderMemoryInfo;
    hooks.shutdown_pump_callback = PumpShutdownLoop;
    (void)unofficial_napi_set_embedder_hooks(&hooks);
  });
}
