#ifndef EDGE_HANDLE_WRAP_H_
#define EDGE_HANDLE_WRAP_H_

#include <cstdint>

#include <uv.h>

#include "node_api.h"

enum EdgeHandleState : uint8_t {
  kEdgeHandleUninitialized = 0,
  kEdgeHandleInitialized,
  kEdgeHandleClosing,
  kEdgeHandleClosed,
};

using EdgeHandleWrapCloseCallback = void (*)(void* data);

struct EdgeHandleWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  void* active_handle_token = nullptr;
  void* close_data = nullptr;
  uv_handle_t* uv_handle = nullptr;
  EdgeHandleWrapCloseCallback close_callback = nullptr;
  EdgeHandleWrap* prev = nullptr;
  EdgeHandleWrap* next = nullptr;
  bool attached = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool wrapper_ref_held = false;
  EdgeHandleState state = kEdgeHandleUninitialized;
};

void EdgeHandleWrapInit(EdgeHandleWrap* wrap, napi_env env);
void EdgeHandleWrapAttach(EdgeHandleWrap* wrap,
                         void* close_data,
                         uv_handle_t* handle,
                         EdgeHandleWrapCloseCallback close_callback);
void EdgeHandleWrapDetach(EdgeHandleWrap* wrap);
napi_value EdgeHandleWrapGetRefValue(napi_env env, napi_ref ref);
void EdgeHandleWrapDeleteRefIfPresent(napi_env env, napi_ref* ref);
void EdgeHandleWrapHoldWrapperRef(EdgeHandleWrap* wrap);
void EdgeHandleWrapReleaseWrapperRef(EdgeHandleWrap* wrap);
bool EdgeHandleWrapCancelFinalizer(EdgeHandleWrap* wrap, void* native_object);
napi_value EdgeHandleWrapGetActiveOwner(napi_env env, napi_ref wrapper_ref);
void EdgeHandleWrapSetOnCloseCallback(napi_env env, napi_value wrapper, napi_value callback);
void EdgeHandleWrapMaybeCallOnClose(EdgeHandleWrap* wrap);
bool EdgeHandleWrapHasRef(const EdgeHandleWrap* wrap, const uv_handle_t* handle);
bool EdgeHandleWrapEnvCleanupStarted(napi_env env);
void EdgeHandleWrapRunEnvCleanup(napi_env env);

#endif  // EDGE_HANDLE_WRAP_H_
