#ifndef UBI_STREAM_BASE_H_
#define UBI_STREAM_BASE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include <uv.h>

#include "node_api.h"
#include "ubi_stream_listener.h"

struct UbiStreamBase;

struct UbiStreamBaseOps {
  uv_handle_t* (*get_handle)(UbiStreamBase* base) = nullptr;
  uv_stream_t* (*get_stream)(UbiStreamBase* base) = nullptr;
  uv_close_cb on_close = nullptr;
  void (*destroy_self)(UbiStreamBase* base) = nullptr;
  napi_value (*accept_pending_handle)(UbiStreamBase* base) = nullptr;
};

struct UbiStreamBase {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref onread_ref = nullptr;
  napi_ref user_read_buffer_ref = nullptr;
  UbiStreamListenerState listener_state{};
  UbiStreamListener default_listener{};
  UbiStreamListener user_buffer_listener{};
  char* user_buffer_base = nullptr;
  size_t user_buffer_len = 0;
  bool user_buffer_listener_active = false;
  bool closing = false;
  bool closed = false;
  bool eof_emitted = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool destroy_notified = false;
  bool async_init_emitted = false;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  int64_t async_id = -1;
  int32_t provider_type = 0;
  void* active_handle_token = nullptr;
  const UbiStreamBaseOps* ops = nullptr;
};

void UbiStreamBaseInit(UbiStreamBase* base,
                       napi_env env,
                       const UbiStreamBaseOps* ops,
                       int32_t provider_type);
void UbiStreamBaseSetWrapperRef(UbiStreamBase* base, napi_ref wrapper_ref);
napi_value UbiStreamBaseGetWrapper(UbiStreamBase* base);
void UbiStreamBaseSetInitialStreamProperties(UbiStreamBase* base,
                                             bool set_owner_symbol,
                                             bool set_onconnection);

void UbiStreamBaseFinalize(UbiStreamBase* base);
void UbiStreamBaseOnClosed(UbiStreamBase* base);
void UbiStreamBaseEmitAfterWrite(UbiStreamBase* base, napi_value req_obj, int status);
void UbiStreamBaseEmitAfterShutdown(UbiStreamBase* base, napi_value req_obj, int status);

bool UbiStreamBasePushListener(UbiStreamBase* base, UbiStreamListener* listener);
bool UbiStreamBaseRemoveListener(UbiStreamBase* base, UbiStreamListener* listener);
bool UbiStreamBaseOnUvAlloc(UbiStreamBase* base, size_t suggested_size, uv_buf_t* out);
void UbiStreamBaseOnUvRead(UbiStreamBase* base, ssize_t nread, const uv_buf_t* buf);

void UbiStreamBaseSetReading(UbiStreamBase* base, bool reading);
napi_value UbiStreamBaseClose(UbiStreamBase* base, napi_value close_callback);
void UbiStreamBaseSetCloseCallback(UbiStreamBase* base, napi_value close_callback);
bool UbiStreamBaseHasRef(UbiStreamBase* base);
void UbiStreamBaseRef(UbiStreamBase* base);
void UbiStreamBaseUnref(UbiStreamBase* base);

napi_value UbiStreamBaseGetOnRead(UbiStreamBase* base);
napi_value UbiStreamBaseSetOnRead(UbiStreamBase* base, napi_value value);
napi_value UbiStreamBaseUseUserBuffer(UbiStreamBase* base, napi_value value);

napi_value UbiStreamBaseGetBytesRead(UbiStreamBase* base);
napi_value UbiStreamBaseGetBytesWritten(UbiStreamBase* base);
napi_value UbiStreamBaseGetFd(UbiStreamBase* base);
napi_value UbiStreamBaseGetExternal(UbiStreamBase* base);
napi_value UbiStreamBaseGetAsyncId(UbiStreamBase* base);
napi_value UbiStreamBaseGetProviderType(UbiStreamBase* base);
napi_value UbiStreamBaseAsyncReset(UbiStreamBase* base);
napi_value UbiStreamBaseHasRefValue(UbiStreamBase* base);
napi_value UbiStreamBaseGetWriteQueueSize(UbiStreamBase* base);
uv_stream_t* UbiStreamBaseGetLibuvStream(napi_env env, napi_value value);
UbiStreamBase* UbiStreamBaseFromValue(napi_env env, napi_value value);

napi_value UbiStreamBaseMakeInt32(napi_env env, int32_t value);
napi_value UbiStreamBaseMakeInt64(napi_env env, int64_t value);
napi_value UbiStreamBaseMakeDouble(napi_env env, double value);
napi_value UbiStreamBaseMakeBool(napi_env env, bool value);
napi_value UbiStreamBaseUndefined(napi_env env);

void UbiStreamBaseSetReqError(napi_env env, napi_value req_obj, int status);
void UbiStreamBaseInvokeReqOnComplete(napi_env env,
                                      napi_value req_obj,
                                      int status,
                                      napi_value* argv,
                                      size_t argc);
int UbiStreamBaseWriteBufferDirect(UbiStreamBase* base,
                                   napi_value req_obj,
                                   napi_value payload,
                                   bool* async_out);

size_t UbiTypedArrayElementSize(napi_typedarray_type type);
bool UbiStreamBaseExtractByteSpan(napi_env env,
                                  napi_value value,
                                  const uint8_t** data,
                                  size_t* len,
                                  bool* refable,
                                  std::string* temp_utf8);
napi_value UbiStreamBufferFromWithEncoding(napi_env env,
                                          napi_value value,
                                          napi_value encoding);

napi_value UbiLibuvStreamWriteBuffer(UbiStreamBase* base,
                                     napi_value req_obj,
                                     napi_value payload,
                                     uv_stream_t* send_handle,
                                     napi_value send_handle_obj);
napi_value UbiLibuvStreamWriteString(UbiStreamBase* base,
                                     napi_value req_obj,
                                     napi_value payload,
                                     const char* encoding_name,
                                     uv_stream_t* send_handle,
                                     napi_value send_handle_obj);
napi_value UbiLibuvStreamWriteV(UbiStreamBase* base,
                                napi_value req_obj,
                                napi_value chunks,
                                bool all_buffers,
                                uv_stream_t* send_handle,
                                napi_value send_handle_obj);
napi_value UbiLibuvStreamShutdown(UbiStreamBase* base, napi_value req_obj);

#endif  // UBI_STREAM_BASE_H_
