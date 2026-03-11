#include "ubi_stream_base.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "internal_binding/helpers.h"
#include "ubi_active_resource.h"
#include "ubi_async_wrap.h"
#include "ubi_module_loader.h"
#include "ubi_pipe_wrap.h"
#include "ubi_runtime.h"
#include "ubi_js_stream.h"
#include "ubi_stream_wrap.h"
#include "ubi_tcp_wrap.h"
#include "ubi_tty_wrap.h"

namespace {

struct StreamSymbolCache {
  napi_ref symbols_ref = nullptr;
  napi_ref owner_symbol_ref = nullptr;
  napi_ref handle_onclose_symbol_ref = nullptr;
};

napi_value GetOwnerSymbol(napi_env env);

bool StreamBaseHasRefForTracking(void* data) {
  return UbiStreamBaseHasRef(static_cast<UbiStreamBase*>(data));
}

napi_value StreamBaseGetActiveOwner(napi_env env, void* data) {
  auto* base = static_cast<UbiStreamBase*>(data);
  if (base == nullptr) return nullptr;
  napi_value wrapper = UbiStreamBaseGetWrapper(base);
  if (wrapper == nullptr) return nullptr;

  napi_value owner_symbol = GetOwnerSymbol(env);
  if (owner_symbol == nullptr) return nullptr;

  napi_value owner = nullptr;
  if (napi_get_property(env, wrapper, owner_symbol, &owner) != napi_ok || owner == nullptr) return nullptr;
  napi_valuetype owner_type = napi_undefined;
  if (napi_typeof(env, owner, &owner_type) != napi_ok) return nullptr;
  if (owner_type == napi_undefined || owner_type == napi_null) return nullptr;
  return owner;
}

const char* ActiveResourceNameForProvider(int32_t provider_type) {
  switch (provider_type) {
    case kUbiProviderTcpWrap:
      return "TCPSocketWrap";
    case kUbiProviderTcpServerWrap:
      return "TCPServerWrap";
    case kUbiProviderPipeWrap:
      return "PipeWrap";
    case kUbiProviderPipeServerWrap:
      return "PipeServerWrap";
    case kUbiProviderJsStream:
      return "JSSTREAM";
    default:
      return "STREAM";
  }
}

struct LibuvWriteReq {
  uv_write_t req{};
  napi_env env = nullptr;
  UbiStreamBase* base = nullptr;
  napi_ref req_obj_ref = nullptr;
  napi_ref send_handle_ref = nullptr;
  uv_buf_t* bufs = nullptr;
  uv_buf_t* bufs_storage = nullptr;
  napi_ref* bufs_refs = nullptr;
  char** bufs_allocs = nullptr;
  uint32_t nbufs = 0;
  uint32_t nbufs_storage = 0;
};

struct LibuvShutdownReq {
  uv_shutdown_t req{};
  napi_env env = nullptr;
  UbiStreamBase* base = nullptr;
  napi_ref req_obj_ref = nullptr;
};

std::unordered_map<napi_env, StreamSymbolCache> g_stream_symbols;
std::unordered_set<napi_env> g_stream_symbol_cleanup_hook_registered;

void OnStreamSymbolsEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_stream_symbol_cleanup_hook_registered.erase(env);

  auto it = g_stream_symbols.find(env);
  if (it == g_stream_symbols.end()) return;
  if (it->second.symbols_ref != nullptr) napi_delete_reference(env, it->second.symbols_ref);
  if (it->second.owner_symbol_ref != nullptr) napi_delete_reference(env, it->second.owner_symbol_ref);
  if (it->second.handle_onclose_symbol_ref != nullptr) {
    napi_delete_reference(env, it->second.handle_onclose_symbol_ref);
  }
  g_stream_symbols.erase(it);
}

void EnsureStreamSymbolsCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_stream_symbol_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnStreamSymbolsEnvCleanup, env) != napi_ok) {
    g_stream_symbol_cleanup_hook_registered.erase(it);
  }
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) {
    return nullptr;
  }
  return value;
}

napi_value ResolveInternalBinding(napi_env env, const char* name) {
  if (env == nullptr || name == nullptr) return nullptr;

  napi_value global = internal_binding::GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value internal_binding = UbiGetInternalBinding(env);
  if (internal_binding == nullptr) {
    if (napi_get_named_property(env, global, "internalBinding", &internal_binding) != napi_ok ||
        internal_binding == nullptr) {
      return nullptr;
    }
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, internal_binding, &type) != napi_ok || type != napi_function) {
    return nullptr;
  }

  napi_value binding_name = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &binding_name) != napi_ok ||
      binding_name == nullptr) {
    return nullptr;
  }

  napi_value binding = nullptr;
  napi_value argv[1] = {binding_name};
  if (napi_call_function(env, global, internal_binding, 1, argv, &binding) != napi_ok ||
      binding == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return nullptr;
  }
  return binding;
}

StreamSymbolCache& GetSymbolCache(napi_env env) {
  EnsureStreamSymbolsCleanupHook(env);
  return g_stream_symbols[env];
}

napi_value GetSymbolsBinding(napi_env env) {
  StreamSymbolCache& cache = GetSymbolCache(env);
  napi_value binding = GetRefValue(env, cache.symbols_ref);
  if (binding != nullptr) return binding;

  binding = ResolveInternalBinding(env, "symbols");
  if (binding == nullptr) return nullptr;

  if (cache.symbols_ref != nullptr) {
    napi_delete_reference(env, cache.symbols_ref);
    cache.symbols_ref = nullptr;
  }
  napi_create_reference(env, binding, 1, &cache.symbols_ref);
  return binding;
}

napi_value GetNamedCachedSymbol(napi_env env, const char* key, napi_ref* slot) {
  if (slot == nullptr) return nullptr;
  napi_value symbol = GetRefValue(env, *slot);
  if (symbol != nullptr) return symbol;

  napi_value symbols = GetSymbolsBinding(env);
  if (symbols == nullptr) return nullptr;

  if (napi_get_named_property(env, symbols, key, &symbol) != napi_ok || symbol == nullptr) {
    return nullptr;
  }

  if (*slot != nullptr) {
    napi_delete_reference(env, *slot);
    *slot = nullptr;
  }
  napi_create_reference(env, symbol, 1, slot);
  return symbol;
}

napi_value GetOwnerSymbol(napi_env env) {
  StreamSymbolCache& cache = GetSymbolCache(env);
  return GetNamedCachedSymbol(env, "owner_symbol", &cache.owner_symbol_ref);
}

napi_value GetHandleOnCloseSymbol(napi_env env) {
  StreamSymbolCache& cache = GetSymbolCache(env);
  return GetNamedCachedSymbol(env, "handle_onclose", &cache.handle_onclose_symbol_ref);
}

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void FreeExternalArrayBuffer(napi_env /*env*/, void* data, void* /*hint*/) {
  free(data);
}

void SetStreamState(napi_env env, int index, int32_t value) {
  int32_t* state = UbiGetStreamBaseState(env);
  if (state == nullptr) return;
  state[index] = value;
}

bool IsFunction(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

void DefineValueProperty(napi_env env,
                         napi_value object,
                         const char* name,
                         napi_value value,
                         napi_property_attributes attrs) {
  if (env == nullptr || object == nullptr || name == nullptr || value == nullptr) return;
  napi_property_descriptor desc = {
      name,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      value,
      attrs,
      nullptr,
  };
  napi_define_properties(env, object, 1, &desc);
}

// Mirror Node's LibuvStreamWrap::DoTryWrite() contract by slicing consumed
// buffers in place and returning 0 for EAGAIN/ENOSYS.
int UbiLibuvStreamDoTryWrite(uv_stream_t* stream, uv_buf_t** bufs, size_t* count) {
  if (stream == nullptr || bufs == nullptr || count == nullptr || *bufs == nullptr) return UV_EINVAL;

  int err = uv_try_write(stream, *bufs, *count);
  if (err == UV_ENOSYS || err == UV_EAGAIN) return 0;
  if (err < 0) return err;

  size_t written = static_cast<size_t>(err);
  uv_buf_t* vbufs = *bufs;
  size_t vcount = *count;

  for (; vcount > 0; vbufs++, vcount--) {
    if (vbufs[0].len > written) {
      vbufs[0].base += written;
      vbufs[0].len -= written;
      written = 0;
      break;
    }
    written -= vbufs[0].len;
  }

  *bufs = vbufs;
  *count = vcount;
  return 0;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return {};
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

void SetPropertyIfPresent(napi_env env, napi_value obj, napi_value key, napi_value value) {
  if (env == nullptr || obj == nullptr || key == nullptr || value == nullptr) return;
  napi_set_property(env, obj, key, value);
}

napi_value GetNamedPropertyValue(napi_env env, napi_value obj, const char* key) {
  if (env == nullptr || obj == nullptr || key == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_named_property(env, obj, key, &value) != napi_ok) return nullptr;
  return value;
}

bool UpdateUserReadBuffer(UbiStreamBase* base, napi_value value) {
  if (base == nullptr || base->env == nullptr || value == nullptr) return false;

  bool is_buffer = false;
  if (napi_is_buffer(base->env, value, &is_buffer) == napi_ok && is_buffer) {
    void* data = nullptr;
    size_t len = 0;
    if (napi_get_buffer_info(base->env, value, &data, &len) != napi_ok ||
        data == nullptr ||
        len == 0) {
      return false;
    }
    DeleteRefIfPresent(base->env, &base->user_read_buffer_ref);
    if (napi_create_reference(base->env, value, 1, &base->user_read_buffer_ref) != napi_ok ||
        base->user_read_buffer_ref == nullptr) {
      return false;
    }
    base->user_buffer_base = static_cast<char*>(data);
    base->user_buffer_len = len;
    return true;
  }

  bool is_typedarray = false;
  if (napi_is_typedarray(base->env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type ta_type = napi_uint8_array;
    size_t length = 0;
    void* data = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(base->env,
                                 value,
                                 &ta_type,
                                 &length,
                                 &data,
                                 &arraybuffer,
                                 &byte_offset) != napi_ok ||
        data == nullptr ||
        length == 0) {
      return false;
    }
    DeleteRefIfPresent(base->env, &base->user_read_buffer_ref);
    if (napi_create_reference(base->env, value, 1, &base->user_read_buffer_ref) != napi_ok ||
        base->user_read_buffer_ref == nullptr) {
      return false;
    }
    base->user_buffer_base = static_cast<char*>(data);
    base->user_buffer_len = length * UbiTypedArrayElementSize(ta_type);
    return true;
  }

  return false;
}

bool CallJsOnRead(UbiStreamBase* base,
                  ssize_t nread,
                  napi_value arraybuffer,
                  size_t offset,
                  napi_value* result) {
  if (base == nullptr || base->env == nullptr) return false;

  SetStreamState(base->env, kUbiReadBytesOrError, static_cast<int32_t>(nread));
  SetStreamState(base->env, kUbiArrayBufferOffset, static_cast<int32_t>(offset));

  napi_value callback = GetRefValue(base->env, base->onread_ref);
  if (!IsFunction(base->env, callback)) return false;

  napi_value self = UbiStreamBaseGetWrapper(base);
  if (self == nullptr) return false;

  napi_value argv[1] = {arraybuffer != nullptr ? arraybuffer : UbiStreamBaseUndefined(base->env)};
  napi_value ignored = nullptr;
  napi_value* out = result != nullptr ? result : &ignored;
  if (UbiAsyncWrapMakeCallback(
          base->env, base->async_id, self, self, callback, 1, argv, out, kUbiMakeCallbackNone) != napi_ok) {
    *out = nullptr;
  }
  (void)UbiHandlePendingExceptionNow(base->env, nullptr);
  return true;
}

bool DefaultOnAlloc(UbiStreamListener* listener, size_t suggested_size, uv_buf_t* out) {
  if (listener == nullptr || out == nullptr) return false;
  char* base = static_cast<char*>(malloc(suggested_size));
  if (base == nullptr && suggested_size > 0) return false;
  *out = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
  return true;
}

bool DefaultOnRead(UbiStreamListener* listener, ssize_t nread, const uv_buf_t* buf) {
  if (listener == nullptr) return false;
  auto* base = static_cast<UbiStreamBase*>(listener->data);
  if (base == nullptr) return false;

  const char* data = (buf != nullptr) ? buf->base : nullptr;
  size_t length = (buf != nullptr) ? buf->len : 0;

  if (nread <= 0) {
    if (nread < 0) {
      (void)CallJsOnRead(base, nread, nullptr, 0, nullptr);
    }
    if (data != nullptr) free(const_cast<char*>(data));
    return true;
  }

  napi_value ab = nullptr;
  if (static_cast<size_t>(nread) == length) {
    if (napi_create_external_arraybuffer(base->env,
                                         const_cast<char*>(data),
                                         static_cast<size_t>(nread),
                                         FreeExternalArrayBuffer,
                                         nullptr,
                                         &ab) != napi_ok ||
        ab == nullptr) {
      ab = nullptr;
    }
  }

  if (ab == nullptr) {
    void* out = nullptr;
    if (napi_create_arraybuffer(base->env, static_cast<size_t>(nread), &out, &ab) != napi_ok ||
        out == nullptr ||
        ab == nullptr) {
      if (data != nullptr) free(const_cast<char*>(data));
      return true;
    }
    if (nread > 0 && data != nullptr) memcpy(out, data, static_cast<size_t>(nread));
    if (data != nullptr) free(const_cast<char*>(data));
  }

  (void)CallJsOnRead(base, nread, ab, 0, nullptr);
  return true;
}

bool UserBufferOnAlloc(UbiStreamListener* listener, size_t /*suggested_size*/, uv_buf_t* out) {
  if (listener == nullptr || out == nullptr) return false;
  auto* base = static_cast<UbiStreamBase*>(listener->data);
  if (base == nullptr || base->user_buffer_base == nullptr || base->user_buffer_len == 0) {
    return false;
  }
  *out = uv_buf_init(base->user_buffer_base, static_cast<unsigned int>(base->user_buffer_len));
  return true;
}

bool UserBufferOnRead(UbiStreamListener* listener, ssize_t nread, const uv_buf_t* buf) {
  if (listener == nullptr) return false;
  auto* base = static_cast<UbiStreamBase*>(listener->data);
  if (base == nullptr) return false;

  if (nread < 0 && (buf == nullptr || buf->base == nullptr)) {
    (void)CallJsOnRead(base, nread, nullptr, 0, nullptr);
    return true;
  }

  napi_value next_buffer = nullptr;
  (void)CallJsOnRead(base, nread, nullptr, 0, &next_buffer);
  if (next_buffer != nullptr) {
    napi_valuetype type = napi_undefined;
    if (napi_typeof(base->env, next_buffer, &type) == napi_ok &&
        type != napi_undefined &&
        type != napi_null) {
      (void)UpdateUserReadBuffer(base, next_buffer);
    }
  }
  return true;
}

bool DefaultOnAfterReqFinished(UbiStreamBase* base,
                               napi_value req_obj,
                               int status) {
  if (base == nullptr || base->env == nullptr || req_obj == nullptr) return true;
  napi_value stream_obj = UbiStreamBaseGetWrapper(base);
  napi_value argv[3] = {
      UbiStreamBaseMakeInt32(base->env, status),
      stream_obj != nullptr ? stream_obj : UbiStreamBaseUndefined(base->env),
      status < 0 ? GetNamedPropertyValue(base->env, req_obj, "error") : UbiStreamBaseUndefined(base->env),
  };
  UbiStreamBaseInvokeReqOnComplete(base->env, req_obj, status, argv, 3);
  return true;
}

bool DefaultOnAfterWrite(UbiStreamListener* listener,
                         napi_value req_obj,
                         int status) {
  if (listener == nullptr) return false;
  return DefaultOnAfterReqFinished(static_cast<UbiStreamBase*>(listener->data), req_obj, status);
}

bool DefaultOnAfterShutdown(UbiStreamListener* listener,
                            napi_value req_obj,
                            int status) {
  if (listener == nullptr) return false;
  return DefaultOnAfterReqFinished(static_cast<UbiStreamBase*>(listener->data), req_obj, status);
}

void DeleteOnReadRefs(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return;
  DeleteRefIfPresent(base->env, &base->onread_ref);
  DeleteRefIfPresent(base->env, &base->user_read_buffer_ref);
  base->user_buffer_base = nullptr;
  base->user_buffer_len = 0;
}

void MaybeCallHandleOnClose(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr || base->finalized) return;
  napi_value self = UbiStreamBaseGetWrapper(base);
  if (self == nullptr) return;
  napi_value symbol = GetHandleOnCloseSymbol(base->env);
  if (symbol == nullptr) return;

  bool has_callback = false;
  if (napi_has_property(base->env, self, symbol, &has_callback) != napi_ok || !has_callback) {
    return;
  }

  napi_value callback = nullptr;
  if (napi_get_property(base->env, self, symbol, &callback) != napi_ok || !IsFunction(base->env, callback)) {
    return;
  }

  napi_value ignored = nullptr;
  UbiAsyncWrapMakeCallback(
      base->env, base->async_id, self, self, callback, 0, nullptr, &ignored, kUbiMakeCallbackNone);
  napi_value undefined = UbiStreamBaseUndefined(base->env);
  SetPropertyIfPresent(base->env, self, symbol, undefined);
}

void DestroyBase(UbiStreamBase* base) {
  if (base == nullptr || base->ops == nullptr || base->ops->destroy_self == nullptr) return;
  base->ops->destroy_self(base);
}

void FreeWriteReq(LibuvWriteReq* wr) {
  if (wr == nullptr) return;
  if (wr->bufs_refs != nullptr) {
    for (uint32_t i = 0; i < wr->nbufs_storage; ++i) {
      DeleteRefIfPresent(wr->env, &wr->bufs_refs[i]);
    }
    delete[] wr->bufs_refs;
    wr->bufs_refs = nullptr;
  }
  if (wr->bufs_allocs != nullptr) {
    for (uint32_t i = 0; i < wr->nbufs_storage; ++i) {
      free(wr->bufs_allocs[i]);
    }
    delete[] wr->bufs_allocs;
    wr->bufs_allocs = nullptr;
  }
  delete[] wr->bufs_storage;
  wr->bufs_storage = nullptr;
  DeleteRefIfPresent(wr->env, &wr->req_obj_ref);
  DeleteRefIfPresent(wr->env, &wr->send_handle_ref);
  delete wr;
}

void OnWriteDone(uv_write_t* req, int status) {
  auto* wr = static_cast<LibuvWriteReq*>(req->data);
  if (wr == nullptr) return;
  napi_value req_obj = GetRefValue(wr->env, wr->req_obj_ref);
  UbiStreamBaseEmitAfterWrite(wr->base, req_obj, status);
  FreeWriteReq(wr);
}

void OnShutdownDone(uv_shutdown_t* req, int status) {
  auto* sr = static_cast<LibuvShutdownReq*>(req->data);
  if (sr == nullptr) return;
  napi_value req_obj = GetRefValue(sr->env, sr->req_obj_ref);
  UbiStreamBaseEmitAfterShutdown(sr->base, req_obj, status);
  DeleteRefIfPresent(sr->env, &sr->req_obj_ref);
  delete sr;
}

}  // namespace

void UbiStreamBaseInit(UbiStreamBase* base,
                       napi_env env,
                       const UbiStreamBaseOps* ops,
                       int32_t provider_type) {
  if (base == nullptr) return;
  base->env = env;
  base->ops = ops;
  base->provider_type = provider_type;
  base->async_id = UbiAsyncWrapNextId(env);

  base->default_listener.on_alloc = DefaultOnAlloc;
  base->default_listener.on_read = DefaultOnRead;
  base->default_listener.on_after_write = DefaultOnAfterWrite;
  base->default_listener.on_after_shutdown = DefaultOnAfterShutdown;
  base->default_listener.data = base;

  base->user_buffer_listener.on_alloc = UserBufferOnAlloc;
  base->user_buffer_listener.on_read = UserBufferOnRead;
  base->user_buffer_listener.data = base;

  UbiInitStreamListenerState(&base->listener_state, &base->default_listener);
}

void UbiStreamBaseSetWrapperRef(UbiStreamBase* base, napi_ref wrapper_ref) {
  if (base == nullptr) return;
  base->wrapper_ref = wrapper_ref;
  if (base->env == nullptr || wrapper_ref == nullptr) return;
  napi_value owner = UbiStreamBaseGetWrapper(base);
  if (owner == nullptr) return;
  if (base->active_handle_token == nullptr && base->provider_type != kUbiProviderJsStream) {
    base->active_handle_token = UbiRegisterActiveHandle(base->env,
                                                        owner,
                                                        ActiveResourceNameForProvider(base->provider_type),
                                                        StreamBaseHasRefForTracking,
                                                        StreamBaseGetActiveOwner,
                                                        base);
  }
  if (!base->async_init_emitted && base->async_id > 0) {
    UbiAsyncWrapEmitInit(
        base->env, base->async_id, base->provider_type, UbiAsyncWrapExecutionAsyncId(base->env), owner);
    base->async_init_emitted = true;
  }
}

napi_value UbiStreamBaseGetWrapper(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return nullptr;
  return GetRefValue(base->env, base->wrapper_ref);
}

void UbiStreamBaseSetInitialStreamProperties(UbiStreamBase* base,
                                             bool set_owner_symbol,
                                             bool set_onconnection) {
  if (base == nullptr || base->env == nullptr) return;
  napi_value self = UbiStreamBaseGetWrapper(base);
  if (self == nullptr) return;
  const auto writable_js_property = static_cast<napi_property_attributes>(
      napi_writable | napi_enumerable | napi_configurable);

  DefineValueProperty(base->env,
                      self,
                      "isStreamBase",
                      UbiStreamBaseMakeBool(base->env, true),
                      napi_default);
  DefineValueProperty(base->env,
                      self,
                      "reading",
                      UbiStreamBaseMakeBool(base->env, false),
                      writable_js_property);

  if (set_onconnection) {
    DefineValueProperty(base->env,
                        self,
                        "onconnection",
                        UbiStreamBaseUndefined(base->env),
                        writable_js_property);
  }

  if (set_owner_symbol) {
    napi_value owner_symbol = GetOwnerSymbol(base->env);
    if (owner_symbol != nullptr) {
      napi_value null_value = nullptr;
      napi_get_null(base->env, &null_value);
      SetPropertyIfPresent(base->env, self, owner_symbol, null_value);
    }
  }
}

void UbiStreamBaseFinalize(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return;
  base->finalized = true;
  DeleteOnReadRefs(base);
  DeleteRefIfPresent(base->env, &base->wrapper_ref);

  uv_handle_t* handle = (base->ops != nullptr && base->ops->get_handle != nullptr)
                            ? base->ops->get_handle(base)
                            : nullptr;
  if (handle == nullptr) {
    if (base->active_handle_token != nullptr) {
      UbiUnregisterActiveHandle(base->env, base->active_handle_token);
      base->active_handle_token = nullptr;
    }
    DestroyBase(base);
    return;
  }

  if (!base->closed) {
    base->delete_on_close = true;
    if (!base->closing && !uv_is_closing(handle) && base->ops != nullptr && base->ops->on_close != nullptr) {
      base->closing = true;
      uv_close(handle, base->ops->on_close);
    }
    return;
  }

  DestroyBase(base);
}

void UbiStreamBaseOnClosed(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return;
  base->closing = false;
  base->closed = true;

  if (!base->destroy_notified) {
    base->destroy_notified = true;
    UbiAsyncWrapQueueDestroyId(base->env, base->async_id);
    base->async_id = -1;
    UbiStreamNotifyClosed(&base->listener_state);
  }

  MaybeCallHandleOnClose(base);

  if (base->active_handle_token != nullptr) {
    UbiUnregisterActiveHandle(base->env, base->active_handle_token);
    base->active_handle_token = nullptr;
  }

  if (base->delete_on_close || base->finalized) {
    DeleteOnReadRefs(base);
    DestroyBase(base);
  }
}

bool UbiStreamBasePushListener(UbiStreamBase* base, UbiStreamListener* listener) {
  if (base == nullptr || listener == nullptr) return false;
  UbiPushStreamListener(&base->listener_state, listener);
  return true;
}

bool UbiStreamBaseRemoveListener(UbiStreamBase* base, UbiStreamListener* listener) {
  if (base == nullptr || listener == nullptr) return false;
  return UbiRemoveStreamListener(&base->listener_state, listener);
}

bool UbiStreamBaseOnUvAlloc(UbiStreamBase* base, size_t suggested_size, uv_buf_t* out) {
  if (base == nullptr || out == nullptr) return false;
  if (UbiStreamEmitAlloc(&base->listener_state, suggested_size, out)) return true;
  char* raw = static_cast<char*>(malloc(suggested_size));
  *out = uv_buf_init(raw, static_cast<unsigned int>(suggested_size));
  return true;
}

void UbiStreamBaseOnUvRead(UbiStreamBase* base, ssize_t nread, const uv_buf_t* buf) {
  if (base == nullptr) {
    if (buf != nullptr && buf->base != nullptr) free(buf->base);
    return;
  }

  if (nread == UV_EOF) {
    base->eof_emitted = true;
  }

  if (base->ops != nullptr && base->ops->accept_pending_handle != nullptr) {
    napi_value self = UbiStreamBaseGetWrapper(base);
    napi_value pending_handle = base->ops->accept_pending_handle(base);
    if (self != nullptr && pending_handle != nullptr && !internal_binding::IsUndefined(base->env, pending_handle)) {
      napi_set_named_property(base->env, self, "pendingHandle", pending_handle);
    }
  }

  if (nread > 0) {
    base->bytes_read += static_cast<uint64_t>(nread);
  }

  if (!UbiStreamEmitRead(&base->listener_state, nread, buf) && buf != nullptr && buf->base != nullptr) {
    free(buf->base);
  }
}

void UbiStreamBaseEmitAfterWrite(UbiStreamBase* base, napi_value req_obj, int status) {
  if (base == nullptr) return;
  if (!UbiStreamEmitAfterWrite(&base->listener_state, req_obj, status) && req_obj != nullptr) {
    UbiStreamBaseInvokeReqOnComplete(base->env, req_obj, status, nullptr, 0);
  }
}

void UbiStreamBaseEmitAfterShutdown(UbiStreamBase* base, napi_value req_obj, int status) {
  if (base == nullptr) return;
  if (!UbiStreamEmitAfterShutdown(&base->listener_state, req_obj, status) && req_obj != nullptr) {
    UbiStreamBaseInvokeReqOnComplete(base->env, req_obj, status, nullptr, 0);
  }
}

void UbiStreamBaseSetReading(UbiStreamBase* base, bool reading) {
  if (base == nullptr || base->env == nullptr) return;
  napi_value self = UbiStreamBaseGetWrapper(base);
  if (self == nullptr) return;
  napi_set_named_property(base->env, self, "reading", UbiStreamBaseMakeBool(base->env, reading));
}

napi_value UbiStreamBaseClose(UbiStreamBase* base, napi_value close_callback) {
  if (base == nullptr || base->env == nullptr) return UbiStreamBaseUndefined(base != nullptr ? base->env : nullptr);
  uv_handle_t* handle = (base->ops != nullptr && base->ops->get_handle != nullptr)
                            ? base->ops->get_handle(base)
                            : nullptr;
  if (handle == nullptr || base->closed || base->closing || uv_is_closing(handle)) {
    return UbiStreamBaseUndefined(base->env);
  }

  UbiStreamBaseSetCloseCallback(base, close_callback);
  if (base->ops != nullptr && base->ops->get_stream != nullptr) {
    if (uv_stream_t* stream = base->ops->get_stream(base)) {
      (void)uv_read_stop(stream);
    }
  }
  UbiStreamBaseSetReading(base, false);

  base->closing = true;
  if (base->ops != nullptr && base->ops->on_close != nullptr) {
    uv_close(handle, base->ops->on_close);
  }
  return UbiStreamBaseUndefined(base->env);
}

void UbiStreamBaseSetCloseCallback(UbiStreamBase* base, napi_value close_callback) {
  if (base == nullptr || base->env == nullptr || close_callback == nullptr ||
      !IsFunction(base->env, close_callback)) {
    return;
  }
  napi_value self = UbiStreamBaseGetWrapper(base);
  napi_value symbol = GetHandleOnCloseSymbol(base->env);
  if (self != nullptr && symbol != nullptr) {
    SetPropertyIfPresent(base->env, self, symbol, close_callback);
  }
}

bool UbiStreamBaseHasRef(UbiStreamBase* base) {
  if (base == nullptr || base->ops == nullptr || base->ops->get_handle == nullptr) return false;
  uv_handle_t* handle = base->ops->get_handle(base);
  return handle != nullptr && !base->closed && uv_has_ref(handle) != 0;
}

void UbiStreamBaseRef(UbiStreamBase* base) {
  if (base == nullptr || base->ops == nullptr || base->ops->get_handle == nullptr) return;
  uv_handle_t* handle = base->ops->get_handle(base);
  if (handle != nullptr && !base->closed) uv_ref(handle);
}

void UbiStreamBaseUnref(UbiStreamBase* base) {
  if (base == nullptr || base->ops == nullptr || base->ops->get_handle == nullptr) return;
  uv_handle_t* handle = base->ops->get_handle(base);
  if (handle != nullptr && !base->closed) uv_unref(handle);
}

napi_value UbiStreamBaseGetOnRead(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return nullptr;
  napi_value callback = GetRefValue(base->env, base->onread_ref);
  return callback != nullptr ? callback : UbiStreamBaseUndefined(base->env);
}

napi_value UbiStreamBaseSetOnRead(UbiStreamBase* base, napi_value value) {
  if (base == nullptr || base->env == nullptr) return nullptr;

  napi_valuetype type = napi_undefined;
  if (value == nullptr || napi_typeof(base->env, value, &type) != napi_ok ||
      type == napi_undefined || type == napi_null) {
    DeleteRefIfPresent(base->env, &base->onread_ref);
    return UbiStreamBaseUndefined(base->env);
  }

  if (type != napi_function) {
    return UbiStreamBaseUndefined(base->env);
  }

  DeleteRefIfPresent(base->env, &base->onread_ref);
  napi_create_reference(base->env, value, 1, &base->onread_ref);
  return UbiStreamBaseUndefined(base->env);
}

napi_value UbiStreamBaseUseUserBuffer(UbiStreamBase* base, napi_value value) {
  if (base == nullptr || base->env == nullptr || value == nullptr) {
    return UbiStreamBaseMakeInt32(base != nullptr ? base->env : nullptr, UV_EINVAL);
  }

  if (!UpdateUserReadBuffer(base, value)) return UbiStreamBaseMakeInt32(base->env, UV_EINVAL);
  if (!base->user_buffer_listener_active) {
    UbiPushStreamListener(&base->listener_state, &base->user_buffer_listener);
    base->user_buffer_listener_active = true;
  }
  return UbiStreamBaseMakeInt32(base->env, 0);
}

napi_value UbiStreamBaseGetBytesRead(UbiStreamBase* base) {
  return UbiStreamBaseMakeDouble(base != nullptr ? base->env : nullptr,
                                 static_cast<double>(base != nullptr ? base->bytes_read : 0));
}

napi_value UbiStreamBaseGetBytesWritten(UbiStreamBase* base) {
  return UbiStreamBaseMakeDouble(base != nullptr ? base->env : nullptr,
                                 static_cast<double>(base != nullptr ? base->bytes_written : 0));
}

napi_value UbiStreamBaseGetFd(UbiStreamBase* base) {
  if (base == nullptr || base->ops == nullptr || base->ops->get_handle == nullptr) {
    return UbiStreamBaseMakeInt32(base != nullptr ? base->env : nullptr, -1);
  }
  uv_handle_t* handle = base->ops->get_handle(base);
  uv_os_fd_t fd = -1;
  if (handle == nullptr || uv_fileno(handle, &fd) != 0) fd = -1;
  return UbiStreamBaseMakeInt32(base->env, static_cast<int32_t>(fd));
}

napi_value UbiStreamBaseGetExternal(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return nullptr;
  napi_value external = nullptr;
  napi_create_external(base->env, base, nullptr, nullptr, &external);
  return external;
}

napi_value UbiStreamBaseGetAsyncId(UbiStreamBase* base) {
  return UbiStreamBaseMakeInt64(base != nullptr ? base->env : nullptr,
                                base != nullptr ? base->async_id : -1);
}

napi_value UbiStreamBaseGetProviderType(UbiStreamBase* base) {
  return UbiStreamBaseMakeInt32(base != nullptr ? base->env : nullptr,
                                base != nullptr ? base->provider_type : kUbiProviderNone);
}

napi_value UbiStreamBaseAsyncReset(UbiStreamBase* base) {
  if (base == nullptr || base->env == nullptr) return nullptr;
  UbiAsyncWrapReset(base->env, &base->async_id);
  base->async_init_emitted = false;
  napi_value self = UbiStreamBaseGetWrapper(base);
  if (self != nullptr && base->async_id > 0) {
    UbiAsyncWrapEmitInit(
        base->env, base->async_id, base->provider_type, UbiAsyncWrapExecutionAsyncId(base->env), self);
    base->async_init_emitted = true;
  }
  return UbiStreamBaseUndefined(base->env);
}

napi_value UbiStreamBaseHasRefValue(UbiStreamBase* base) {
  return UbiStreamBaseMakeBool(base != nullptr ? base->env : nullptr, UbiStreamBaseHasRef(base));
}

napi_value UbiStreamBaseGetWriteQueueSize(UbiStreamBase* base) {
  if (base == nullptr || base->ops == nullptr || base->ops->get_stream == nullptr) {
    return UbiStreamBaseMakeInt32(base != nullptr ? base->env : nullptr, 0);
  }
  uv_stream_t* stream = base->ops->get_stream(base);
  const uint32_t size = stream != nullptr ? stream->write_queue_size : 0;
  napi_value out = nullptr;
  napi_create_uint32(base->env, size, &out);
  return out;
}

uv_stream_t* UbiStreamBaseGetLibuvStream(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  if (uv_stream_t* stream = UbiPipeWrapGetStream(env, value)) return stream;
  if (uv_stream_t* stream = UbiTcpWrapGetStream(env, value)) return stream;
  if (uv_stream_t* stream = UbiTtyWrapGetStream(env, value)) return stream;
  return nullptr;
}

UbiStreamBase* UbiStreamBaseFromValue(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return nullptr;
  if (type == napi_external) {
    void* data = nullptr;
    if (napi_get_value_external(env, value, &data) == napi_ok) {
      return static_cast<UbiStreamBase*>(data);
    }
  }

  if (type != napi_object && type != napi_function) {
    return nullptr;
  }

  napi_value external = nullptr;
  if (napi_get_named_property(env, value, "_externalStream", &external) != napi_ok || external == nullptr) {
    return nullptr;
  }
  void* data = nullptr;
  if (napi_get_value_external(env, external, &data) != napi_ok) return nullptr;
  return static_cast<UbiStreamBase*>(data);
}

napi_value UbiStreamBaseMakeInt32(napi_env env, int32_t value) {
  if (env == nullptr) return nullptr;
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out;
}

napi_value UbiStreamBaseMakeInt64(napi_env env, int64_t value) {
  if (env == nullptr) return nullptr;
  napi_value out = nullptr;
  napi_create_int64(env, value, &out);
  return out;
}

napi_value UbiStreamBaseMakeDouble(napi_env env, double value) {
  if (env == nullptr) return nullptr;
  napi_value out = nullptr;
  napi_create_double(env, value, &out);
  return out;
}

napi_value UbiStreamBaseMakeBool(napi_env env, bool value) {
  if (env == nullptr) return nullptr;
  napi_value out = nullptr;
  napi_get_boolean(env, value, &out);
  return out;
}

napi_value UbiStreamBaseUndefined(napi_env env) {
  if (env == nullptr) return nullptr;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

void UbiStreamBaseSetReqError(napi_env env, napi_value req_obj, int status) {
  if (env == nullptr || req_obj == nullptr || status >= 0) return;
  napi_value undefined = nullptr;
  if (napi_get_undefined(env, &undefined) == napi_ok && undefined != nullptr) {
    napi_set_named_property(env, req_obj, "error", undefined);
  }
}

void UbiStreamBaseInvokeReqOnComplete(napi_env env,
                                      napi_value req_obj,
                                      int status,
                                      napi_value* argv,
                                      size_t argc) {
  if (env == nullptr || req_obj == nullptr) return;

  UbiStreamBaseSetReqError(env, req_obj, status);
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok ||
      !IsFunction(env, oncomplete)) {
    UbiStreamReqMarkDone(env, req_obj);
    return;
  }

  napi_value local_argv[1] = {UbiStreamBaseMakeInt32(env, status)};
  if (argv == nullptr || argc == 0) {
    argv = local_argv;
    argc = 1;
  }

  napi_value ignored = nullptr;
  UbiAsyncWrapMakeCallback(env,
                           UbiStreamReqGetAsyncId(env, req_obj),
                           req_obj,
                           req_obj,
                           oncomplete,
                           argc,
                           argv,
                           &ignored,
                           kUbiMakeCallbackNone);
  UbiStreamReqMarkDone(env, req_obj);
}

int UbiStreamBaseWriteBufferDirect(UbiStreamBase* base,
                                   napi_value req_obj,
                                   napi_value payload,
                                   bool* async_out) {
  if (async_out != nullptr) *async_out = false;
  if (base == nullptr || base->env == nullptr) return UV_EBADF;

  if (base->provider_type == kUbiProviderJsStream) {
    return UbiJsStreamWriteBuffer(base, req_obj, payload, async_out);
  }

  napi_value status_value = UbiLibuvStreamWriteBuffer(base, req_obj, payload, nullptr, nullptr);
  int32_t status = UV_EINVAL;
  if (status_value == nullptr || napi_get_value_int32(base->env, status_value, &status) != napi_ok) {
    return UV_EINVAL;
  }

  if (async_out != nullptr && status == 0) {
    int32_t* state = UbiGetStreamBaseState(base->env);
    *async_out = state != nullptr && state[kUbiLastWriteWasAsync] != 0;
  }

  return status;
}

size_t UbiTypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 1;
  }
}

bool UbiStreamBaseExtractByteSpan(napi_env env,
                                  napi_value value,
                                  const uint8_t** data,
                                  size_t* len,
                                  bool* refable,
                                  std::string* temp_utf8) {
  if (data == nullptr || len == nullptr || refable == nullptr || temp_utf8 == nullptr) return false;
  *data = nullptr;
  *len = 0;
  *refable = false;
  temp_utf8->clear();

  if (env == nullptr || value == nullptr) return true;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* raw = nullptr;
    size_t length = 0;
    if (napi_get_buffer_info(env, value, &raw, &length) == napi_ok && raw != nullptr) {
      *data = static_cast<const uint8_t*>(raw);
      *len = length;
      *refable = true;
      return true;
    }
  }

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type ta_type = napi_uint8_array;
    size_t length = 0;
    void* raw = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env,
                                 value,
                                 &ta_type,
                                 &length,
                                 &raw,
                                 &arraybuffer,
                                 &byte_offset) == napi_ok &&
        raw != nullptr) {
      *data = static_cast<const uint8_t*>(raw);
      *len = length * UbiTypedArrayElementSize(ta_type);
      *refable = true;
      return true;
    }
  }

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* raw = nullptr;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, value, &raw, &length) == napi_ok && raw != nullptr) {
      *data = static_cast<const uint8_t*>(raw);
      *len = length;
      *refable = true;
      return true;
    }
  }

  *temp_utf8 = ValueToUtf8(env, value);
  *data = reinterpret_cast<const uint8_t*>(temp_utf8->data());
  *len = temp_utf8->size();
  return true;
}

napi_value UbiStreamBufferFromWithEncoding(napi_env env,
                                          napi_value value,
                                          napi_value encoding) {
  if (env == nullptr || value == nullptr) return value;

  napi_value global = internal_binding::GetGlobal(env);
  if (global == nullptr) return value;

  napi_value buffer_ctor = nullptr;
  napi_value from_fn = nullptr;
  if (napi_get_named_property(env, global, "Buffer", &buffer_ctor) != napi_ok ||
      buffer_ctor == nullptr ||
      napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok ||
      !IsFunction(env, from_fn)) {
    return value;
  }

  napi_value argv[2] = {value, nullptr};
  size_t argc = 1;
  if (encoding != nullptr && !internal_binding::IsUndefined(env, encoding)) {
    argv[1] = encoding;
    argc = 2;
  }

  napi_value out = nullptr;
  if (napi_call_function(env, buffer_ctor, from_fn, argc, argv, &out) != napi_ok || out == nullptr) {
    return value;
  }
  return out;
}

napi_value UbiLibuvStreamWriteBuffer(UbiStreamBase* base,
                                     napi_value req_obj,
                                     napi_value payload,
                                     uv_stream_t* send_handle,
                                     napi_value send_handle_obj) {
  if (base == nullptr || base->env == nullptr || base->ops == nullptr || base->ops->get_stream == nullptr) {
    return nullptr;
  }

  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  UbiStreamBaseExtractByteSpan(base->env, payload, &data, &len, &refable, &temp_utf8);

  uv_stream_t* stream = base->ops->get_stream(base);
  base->bytes_written += len;
  uv_buf_t write_buf{};
  uv_buf_t* write_bufs = &write_buf;
  size_t write_count = 1;

  if (send_handle == nullptr && len > 0 && data != nullptr) {
    write_buf =
        uv_buf_init(const_cast<char*>(reinterpret_cast<const char*>(data)), static_cast<unsigned int>(len));
    const int try_rc = UbiLibuvStreamDoTryWrite(stream, &write_bufs, &write_count);
    if (try_rc != 0 || write_count == 0) {
      SetStreamState(base->env, kUbiBytesWritten, static_cast<int32_t>(len));
      SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
      if (try_rc != 0) UbiStreamBaseSetReqError(base->env, req_obj, try_rc);
      return UbiStreamBaseMakeInt32(base->env, try_rc);
    }
  }

  const char* remaining_base =
      (send_handle == nullptr && len > 0 && data != nullptr) ? write_bufs[0].base
                                                              : const_cast<char*>(reinterpret_cast<const char*>(data));
  const size_t remaining =
      (send_handle == nullptr && len > 0 && data != nullptr) ? write_bufs[0].len : len;

  UbiStreamReqActivate(base->env, req_obj, kUbiProviderWriteWrap, base->async_id);

  auto* wr = new LibuvWriteReq();
  wr->env = base->env;
  wr->base = base;
  napi_create_reference(base->env, req_obj, 1, &wr->req_obj_ref);
  wr->nbufs = 1;
  wr->nbufs_storage = 1;
  wr->bufs_storage = new uv_buf_t[1];
  wr->bufs_refs = new napi_ref[1]();
  wr->bufs_allocs = new char*[1]();
  wr->bufs = wr->bufs_storage;

  if (refable && payload != nullptr && remaining_base != nullptr &&
      napi_create_reference(base->env, payload, 1, &wr->bufs_refs[0]) == napi_ok &&
      wr->bufs_refs[0] != nullptr) {
    wr->bufs_storage[0] = uv_buf_init(const_cast<char*>(remaining_base), static_cast<unsigned int>(remaining));
  } else {
    char* copy = static_cast<char*>(malloc(remaining));
    if (copy == nullptr && remaining > 0) {
      SetStreamState(base->env, kUbiBytesWritten, static_cast<int32_t>(len));
      SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
      UbiStreamBaseSetReqError(base->env, req_obj, UV_ENOMEM);
      FreeWriteReq(wr);
      return UbiStreamBaseMakeInt32(base->env, UV_ENOMEM);
    }
    if (remaining > 0 && copy != nullptr && remaining_base != nullptr) {
      memcpy(copy, remaining_base, remaining);
    }
    wr->bufs_allocs[0] = copy;
    wr->bufs_storage[0] = uv_buf_init(copy, static_cast<unsigned int>(remaining));
  }

  if (send_handle != nullptr && send_handle_obj != nullptr) {
    napi_create_reference(base->env, send_handle_obj, 1, &wr->send_handle_ref);
  }

  wr->req.data = wr;
  SetStreamState(base->env, kUbiBytesWritten, static_cast<int32_t>(len));
  SetStreamState(base->env, kUbiLastWriteWasAsync, 1);

  int rc = 0;
  if (send_handle != nullptr) {
    rc = uv_write2(&wr->req, stream, wr->bufs, wr->nbufs, send_handle, OnWriteDone);
  } else {
    rc = uv_write(&wr->req, stream, wr->bufs, wr->nbufs, OnWriteDone);
  }
  if (rc != 0) {
    SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
    UbiStreamBaseSetReqError(base->env, req_obj, rc);
    UbiStreamReqMarkDone(base->env, req_obj);
    FreeWriteReq(wr);
  }
  return UbiStreamBaseMakeInt32(base->env, rc);
}

napi_value UbiLibuvStreamWriteString(UbiStreamBase* base,
                                     napi_value req_obj,
                                     napi_value payload,
                                     const char* encoding_name,
                                     uv_stream_t* send_handle,
                                     napi_value send_handle_obj) {
  if (base == nullptr || base->env == nullptr) return nullptr;

  napi_value encoded = payload;
  if (encoding_name != nullptr && payload != nullptr) {
    napi_value encoding = nullptr;
    if (napi_create_string_utf8(base->env, encoding_name, NAPI_AUTO_LENGTH, &encoding) == napi_ok &&
        encoding != nullptr) {
      encoded = UbiStreamBufferFromWithEncoding(base->env, payload, encoding);
    }
  }

  return UbiLibuvStreamWriteBuffer(base, req_obj, encoded, send_handle, send_handle_obj);
}

napi_value UbiLibuvStreamWriteV(UbiStreamBase* base,
                                napi_value req_obj,
                                napi_value chunks,
                                bool all_buffers,
                                uv_stream_t* send_handle,
                                napi_value send_handle_obj) {
  if (base == nullptr || base->env == nullptr || base->ops == nullptr || base->ops->get_stream == nullptr) {
    return nullptr;
  }

  uint32_t raw_len = 0;
  if (napi_get_array_length(base->env, chunks, &raw_len) != napi_ok) {
    return UbiStreamBaseMakeInt32(base->env, UV_EINVAL);
  }
  const uint32_t nbufs = all_buffers ? raw_len : (raw_len / 2);
  if (nbufs == 0) {
    SetStreamState(base->env, kUbiBytesWritten, 0);
    SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
    return UbiStreamBaseMakeInt32(base->env, 0);
  }

  auto* wr = new LibuvWriteReq();
  wr->env = base->env;
  wr->base = base;
  napi_create_reference(base->env, req_obj, 1, &wr->req_obj_ref);
  wr->nbufs = nbufs;
  wr->nbufs_storage = nbufs;
  wr->bufs_storage = new uv_buf_t[nbufs];
  wr->bufs_refs = new napi_ref[nbufs]();
  wr->bufs_allocs = new char*[nbufs]();
  wr->bufs = wr->bufs_storage;

  size_t total = 0;
  for (uint32_t i = 0; i < nbufs; ++i) {
    napi_value chunk = nullptr;
    napi_get_element(base->env, chunks, all_buffers ? i : (i * 2), &chunk);

    if (!all_buffers) {
      bool is_buffer = false;
      bool is_typedarray = false;
      napi_is_buffer(base->env, chunk, &is_buffer);
      if (!is_buffer) napi_is_typedarray(base->env, chunk, &is_typedarray);
      if (!is_buffer && !is_typedarray) {
        napi_value encoding = nullptr;
        napi_get_element(base->env, chunks, i * 2 + 1, &encoding);
        chunk = UbiStreamBufferFromWithEncoding(base->env, chunk, encoding);
      }
    }

    const uint8_t* data = nullptr;
    size_t len = 0;
    bool refable = false;
    std::string temp_utf8;
    UbiStreamBaseExtractByteSpan(base->env, chunk, &data, &len, &refable, &temp_utf8);

    if (refable &&
        chunk != nullptr &&
        data != nullptr &&
        napi_create_reference(base->env, chunk, 1, &wr->bufs_refs[i]) == napi_ok &&
        wr->bufs_refs[i] != nullptr) {
      wr->bufs_storage[i] = uv_buf_init(const_cast<char*>(reinterpret_cast<const char*>(data)),
                                        static_cast<unsigned int>(len));
    } else {
      char* copy = static_cast<char*>(malloc(len));
      if (copy == nullptr && len > 0) {
        SetStreamState(base->env, kUbiBytesWritten, 0);
        SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
        UbiStreamBaseSetReqError(base->env, req_obj, UV_ENOMEM);
        FreeWriteReq(wr);
        return UbiStreamBaseMakeInt32(base->env, UV_ENOMEM);
      }
      if (len > 0 && copy != nullptr && data != nullptr) memcpy(copy, data, len);
      wr->bufs_allocs[i] = copy;
      wr->bufs_storage[i] = uv_buf_init(copy, static_cast<unsigned int>(len));
    }
    total += len;
  }

  base->bytes_written += total;
  uv_stream_t* stream = base->ops->get_stream(base);
  if (send_handle == nullptr && total > 0) {
    uv_buf_t* try_bufs = wr->bufs;
    size_t try_count = wr->nbufs;
    const int try_rc = UbiLibuvStreamDoTryWrite(stream, &try_bufs, &try_count);
    if (try_rc != 0 || try_count == 0) {
      SetStreamState(base->env, kUbiBytesWritten, static_cast<int32_t>(total));
      SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
      if (try_rc != 0) UbiStreamBaseSetReqError(base->env, req_obj, try_rc);
      FreeWriteReq(wr);
      return UbiStreamBaseMakeInt32(base->env, try_rc);
    }
    wr->bufs = try_bufs;
    wr->nbufs = static_cast<uint32_t>(try_count);
  }

  if (wr->nbufs == 0) {
    SetStreamState(base->env, kUbiBytesWritten, static_cast<int32_t>(total));
    SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
    FreeWriteReq(wr);
    return UbiStreamBaseMakeInt32(base->env, 0);
  }

  UbiStreamReqActivate(base->env, req_obj, kUbiProviderWriteWrap, base->async_id);

  if (send_handle != nullptr && send_handle_obj != nullptr) {
    napi_create_reference(base->env, send_handle_obj, 1, &wr->send_handle_ref);
  }

  wr->req.data = wr;
  SetStreamState(base->env, kUbiBytesWritten, static_cast<int32_t>(total));
  SetStreamState(base->env, kUbiLastWriteWasAsync, 1);

  int rc = 0;
  if (send_handle != nullptr) {
    rc = uv_write2(&wr->req, stream, wr->bufs, wr->nbufs, send_handle, OnWriteDone);
  } else {
    rc = uv_write(&wr->req, stream, wr->bufs, wr->nbufs, OnWriteDone);
  }
  if (rc != 0) {
    SetStreamState(base->env, kUbiLastWriteWasAsync, 0);
    UbiStreamBaseSetReqError(base->env, req_obj, rc);
    UbiStreamReqMarkDone(base->env, req_obj);
    FreeWriteReq(wr);
  }
  return UbiStreamBaseMakeInt32(base->env, rc);
}

napi_value UbiLibuvStreamShutdown(UbiStreamBase* base, napi_value req_obj) {
  if (base == nullptr || base->env == nullptr || base->ops == nullptr || base->ops->get_stream == nullptr) {
    return nullptr;
  }

  auto* sr = new LibuvShutdownReq();
  sr->env = base->env;
  sr->base = base;
  UbiStreamReqActivate(base->env, req_obj, kUbiProviderShutdownWrap, base->async_id);
  napi_create_reference(base->env, req_obj, 1, &sr->req_obj_ref);
  sr->req.data = sr;

  int rc = uv_shutdown(&sr->req, base->ops->get_stream(base), OnShutdownDone);
  if (rc != 0) {
    UbiStreamBaseSetReqError(base->env, req_obj, rc);
    UbiStreamReqMarkDone(base->env, req_obj);
    DeleteRefIfPresent(base->env, &sr->req_obj_ref);
    delete sr;
  }
  return UbiStreamBaseMakeInt32(base->env, rc);
}
