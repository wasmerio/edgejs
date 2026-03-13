#include "internal_binding/dispatch.h"

#include <array>
#include <string_view>

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveAsyncWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveAsyncContextFrame(napi_env env, const ResolveOptions& options);
napi_value ResolveBlockList(napi_env env, const ResolveOptions& options);
napi_value ResolveBlob(napi_env env, const ResolveOptions& options);
napi_value ResolveBuffer(napi_env env, const ResolveOptions& options);
napi_value ResolveBuiltins(napi_env env, const ResolveOptions& options);
napi_value ResolveCaresWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveConfig(napi_env env, const ResolveOptions& options);
napi_value ResolveConstants(napi_env env, const ResolveOptions& options);
napi_value ResolveContextify(napi_env env, const ResolveOptions& options);
napi_value ResolveCredentials(napi_env env, const ResolveOptions& options);
napi_value ResolveCrypto(napi_env env, const ResolveOptions& options);
napi_value ResolveEncodingBinding(napi_env env, const ResolveOptions& options);
napi_value ResolveErrors(napi_env env, const ResolveOptions& options);
napi_value ResolveFsEventWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveFs(napi_env env, const ResolveOptions& options);
napi_value ResolveFsDir(napi_env env, const ResolveOptions& options);
napi_value ResolveHeapUtils(napi_env env, const ResolveOptions& options);
napi_value ResolveHttp2(napi_env env, const ResolveOptions& options);
napi_value ResolveHttpParser(napi_env env, const ResolveOptions& options);
napi_value ResolveIcu(napi_env env, const ResolveOptions& options);
napi_value ResolveJsUdpWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveJsStream(napi_env env, const ResolveOptions& options);
napi_value ResolveInternalOnlyV8(napi_env env, const ResolveOptions& options);
napi_value ResolveModuleWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveModules(napi_env env, const ResolveOptions& options);
napi_value ResolveMksnapshot(napi_env env, const ResolveOptions& options);
napi_value ResolveMessaging(napi_env env, const ResolveOptions& options);
napi_value ResolveOptionsBinding(napi_env env, const ResolveOptions& options);
napi_value ResolveOs(napi_env env, const ResolveOptions& options);
napi_value ResolvePerformance(napi_env env, const ResolveOptions& options);
napi_value ResolvePermission(napi_env env, const ResolveOptions& options);
napi_value ResolvePipeWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveProcessMethods(napi_env env, const ResolveOptions& options);
napi_value ResolveProcessWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveReport(napi_env env, const ResolveOptions& options);
napi_value ResolveSea(napi_env env, const ResolveOptions& options);
napi_value ResolveSignalWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveSerdes(napi_env env, const ResolveOptions& options);
napi_value ResolveSpawnSync(napi_env env, const ResolveOptions& options);
napi_value ResolveStreamPipe(napi_env env, const ResolveOptions& options);
napi_value ResolveStreamWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveStringDecoder(napi_env env, const ResolveOptions& options);
napi_value ResolveSymbols(napi_env env, const ResolveOptions& options);
napi_value ResolveTaskQueue(napi_env env, const ResolveOptions& options);
napi_value ResolveTcpWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveTlsWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveTimers(napi_env env, const ResolveOptions& options);
napi_value ResolveTraceEvents(napi_env env, const ResolveOptions& options);
napi_value ResolveTtyWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveTypes(napi_env env, const ResolveOptions& options);
napi_value ResolveUdpWrap(napi_env env, const ResolveOptions& options);
napi_value ResolveUrl(napi_env env, const ResolveOptions& options);
napi_value ResolveUrlPattern(napi_env env, const ResolveOptions& options);
napi_value ResolveUtil(napi_env env, const ResolveOptions& options);
napi_value ResolveV8(napi_env env, const ResolveOptions& options);
napi_value ResolveUv(napi_env env, const ResolveOptions& options);
napi_value ResolveWatchdog(napi_env env, const ResolveOptions& options);
napi_value ResolveWasmWebApi(napi_env env, const ResolveOptions& options);
napi_value ResolveWorker(napi_env env, const ResolveOptions& options);
napi_value ResolveZlib(napi_env env, const ResolveOptions& options);

namespace {

using ResolverFn = napi_value (*)(napi_env env, const ResolveOptions& options);

struct BindingResolverEntry {
  std::string_view name;
  ResolverFn resolver;
};

constexpr std::array<BindingResolverEntry, 61> kResolvers = {{
    {"async_wrap", ResolveAsyncWrap},
    {"async_context_frame", ResolveAsyncContextFrame},
    {"block_list", ResolveBlockList},
    {"blob", ResolveBlob},
    {"buffer", ResolveBuffer},
    {"builtins", ResolveBuiltins},
    {"cares_wrap", ResolveCaresWrap},
    {"config", ResolveConfig},
    {"constants", ResolveConstants},
    {"contextify", ResolveContextify},
    {"credentials", ResolveCredentials},
    {"crypto", ResolveCrypto},
    {"encoding_binding", ResolveEncodingBinding},
    {"errors", ResolveErrors},
    {"fs_event_wrap", ResolveFsEventWrap},
    {"fs", ResolveFs},
    {"fs_dir", ResolveFsDir},
    {"heap_utils", ResolveHeapUtils},
    {"http2", ResolveHttp2},
    {"http_parser", ResolveHttpParser},
    {"icu", ResolveIcu},
    {"js_udp_wrap", ResolveJsUdpWrap},
    {"js_stream", ResolveJsStream},
    {"internal_only_v8", ResolveInternalOnlyV8},
    {"module_wrap", ResolveModuleWrap},
    {"modules", ResolveModules},
    {"mksnapshot", ResolveMksnapshot},
    {"messaging", ResolveMessaging},
    {"options", ResolveOptionsBinding},
    {"os", ResolveOs},
    {"performance", ResolvePerformance},
    {"permission", ResolvePermission},
    {"pipe_wrap", ResolvePipeWrap},
    {"process_methods", ResolveProcessMethods},
    {"process_wrap", ResolveProcessWrap},
    {"report", ResolveReport},
    {"sea", ResolveSea},
    {"serdes", ResolveSerdes},
    {"signal_wrap", ResolveSignalWrap},
    {"spawn_sync", ResolveSpawnSync},
    {"stream_pipe", ResolveStreamPipe},
    {"stream_wrap", ResolveStreamWrap},
    {"string_decoder", ResolveStringDecoder},
    {"symbols", ResolveSymbols},
    {"task_queue", ResolveTaskQueue},
    {"tcp_wrap", ResolveTcpWrap},
    {"tls_wrap", ResolveTlsWrap},
    {"timers", ResolveTimers},
    {"trace_events", ResolveTraceEvents},
    {"tty_wrap", ResolveTtyWrap},
    {"types", ResolveTypes},
    {"udp_wrap", ResolveUdpWrap},
    {"url", ResolveUrl},
    {"url_pattern", ResolveUrlPattern},
    {"util", ResolveUtil},
    {"v8", ResolveV8},
    {"uv", ResolveUv},
    {"watchdog", ResolveWatchdog},
    {"wasm_web_api", ResolveWasmWebApi},
    {"worker", ResolveWorker},
    {"zlib", ResolveZlib},
}};

}  // namespace

napi_value Resolve(napi_env env, const std::string& name, const ResolveOptions& options) {
  for (const auto& entry : kResolvers) {
    if (entry.name == name) {
      return entry.resolver(env, options);
    }
  }
  return Undefined(env);
}

}  // namespace internal_binding
