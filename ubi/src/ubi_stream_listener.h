#ifndef UBI_STREAM_LISTENER_H_
#define UBI_STREAM_LISTENER_H_

#include <cstddef>

#include <uv.h>

#include "node_api.h"

struct UbiStreamListener;

using UbiStreamOnAlloc = bool (*)(UbiStreamListener* listener,
                                  size_t suggested_size,
                                  uv_buf_t* out);
using UbiStreamOnRead = bool (*)(UbiStreamListener* listener,
                                 ssize_t nread,
                                 const uv_buf_t* buf);
using UbiStreamOnAfterWrite = bool (*)(UbiStreamListener* listener,
                                       napi_value req_obj,
                                       int status);
using UbiStreamOnAfterShutdown = bool (*)(UbiStreamListener* listener,
                                          napi_value req_obj,
                                          int status);
using UbiStreamOnClose = void (*)(UbiStreamListener* listener);

struct UbiStreamListener {
  UbiStreamListener* previous = nullptr;
  UbiStreamOnAlloc on_alloc = nullptr;
  UbiStreamOnRead on_read = nullptr;
  UbiStreamOnAfterWrite on_after_write = nullptr;
  UbiStreamOnAfterShutdown on_after_shutdown = nullptr;
  UbiStreamOnClose on_close = nullptr;
  void* data = nullptr;
};

struct UbiStreamListenerState {
  UbiStreamListener* current = nullptr;
};

void UbiInitStreamListenerState(UbiStreamListenerState* state,
                                UbiStreamListener* initial);
void UbiPushStreamListener(UbiStreamListenerState* state,
                           UbiStreamListener* listener);
bool UbiRemoveStreamListener(UbiStreamListenerState* state,
                             UbiStreamListener* listener);
bool UbiStreamEmitAlloc(UbiStreamListenerState* state,
                        size_t suggested_size,
                        uv_buf_t* out);
bool UbiStreamEmitRead(UbiStreamListenerState* state,
                       ssize_t nread,
                       const uv_buf_t* buf);
bool UbiStreamEmitAfterWrite(UbiStreamListenerState* state,
                             napi_value req_obj,
                             int status);
bool UbiStreamEmitAfterShutdown(UbiStreamListenerState* state,
                                napi_value req_obj,
                                int status);
bool UbiStreamPassAfterWrite(UbiStreamListener* listener,
                             napi_value req_obj,
                             int status);
bool UbiStreamPassAfterShutdown(UbiStreamListener* listener,
                                napi_value req_obj,
                                int status);
void UbiStreamNotifyClosed(UbiStreamListenerState* state);

#endif  // UBI_STREAM_LISTENER_H_
