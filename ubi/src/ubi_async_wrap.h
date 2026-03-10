#ifndef UBI_ASYNC_WRAP_H_
#define UBI_ASYNC_WRAP_H_

#include <cstdint>

#include "node_api.h"

enum UbiAsyncProviderType : int32_t {
  kUbiProviderNone = 0,
  kUbiProviderHttpClientRequest = 10,
  kUbiProviderHttpIncomingMessage = 11,
  kUbiProviderJsStream = 20,
  kUbiProviderJsUdpWrap = 21,
  kUbiProviderMessagePort = 22,
  kUbiProviderPipeConnectWrap = 23,
  kUbiProviderPipeServerWrap = 24,
  kUbiProviderPipeWrap = 25,
  kUbiProviderProcessWrap = 26,
  kUbiProviderShutdownWrap = 35,
  kUbiProviderTcpConnectWrap = 39,
  kUbiProviderTcpServerWrap = 40,
  kUbiProviderTcpWrap = 41,
  kUbiProviderTtyWrap = 42,
  kUbiProviderUdpSendWrap = 43,
  kUbiProviderUdpWrap = 44,
  kUbiProviderWriteWrap = 52,
  kUbiProviderZlib = 53,
  kUbiProviderWorker = 54,
};

int64_t UbiAsyncWrapNextId(napi_env env);
void UbiAsyncWrapQueueDestroyId(napi_env env, int64_t async_id);
void UbiAsyncWrapReset(napi_env env, int64_t* async_id);
int64_t UbiAsyncWrapExecutionAsyncId(napi_env env);
int64_t UbiAsyncWrapCurrentExecutionAsyncId(napi_env env);
const char* UbiAsyncWrapProviderName(int32_t provider_type);
void UbiAsyncWrapEmitInitString(napi_env env,
                                int64_t async_id,
                                const char* type,
                                int64_t trigger_async_id,
                                napi_value resource);
void UbiAsyncWrapEmitInit(napi_env env,
                          int64_t async_id,
                          int32_t provider_type,
                          int64_t trigger_async_id,
                          napi_value resource);
void UbiAsyncWrapEmitBefore(napi_env env, int64_t async_id);
void UbiAsyncWrapEmitAfter(napi_env env, int64_t async_id);
void UbiAsyncWrapPushContext(napi_env env,
                             int64_t async_id,
                             int64_t trigger_async_id,
                             napi_value resource);
bool UbiAsyncWrapPopContext(napi_env env, int64_t async_id);
napi_status UbiAsyncWrapMakeCallback(napi_env env,
                                     int64_t async_id,
                                     napi_value resource,
                                     napi_value recv,
                                     napi_value callback,
                                     size_t argc,
                                     napi_value* argv,
                                     napi_value* result,
                                     int flags);

#endif  // UBI_ASYNC_WRAP_H_
