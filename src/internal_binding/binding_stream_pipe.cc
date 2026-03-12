#include "internal_binding/dispatch.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "internal_binding/helpers.h"
#include "../edge_stream_wrap.h"
#include "uv.h"

namespace internal_binding {

namespace {

bool IsStreamPipeDebugEnabled() {
  const char* value = std::getenv("EDGE_STREAM_PIPE_DEBUG");
  return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
}

struct StreamPipeBindingState {
  napi_ref binding_ref = nullptr;
};

struct StreamPipeWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref source_ref = nullptr;
  napi_ref sink_ref = nullptr;
  bool closed = false;
  bool pump_scheduled = false;
  bool source_kind_known = false;
  bool source_is_fifo = false;
  bool saw_source_data = false;
  bool eof_pending = false;
  uint32_t pending_writes = 0;
};

std::unordered_map<napi_env, StreamPipeBindingState> g_stream_pipe_states;
std::unordered_set<napi_env> g_stream_pipe_cleanup_hooks;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void CleanupStreamPipeEnv(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_stream_pipe_cleanup_hooks.erase(env);
  auto it = g_stream_pipe_states.find(env);
  if (it == g_stream_pipe_states.end()) return;
  DeleteRefIfPresent(env, &it->second.binding_ref);
  g_stream_pipe_states.erase(it);
}

StreamPipeBindingState& EnsureState(napi_env env) {
  auto& state = g_stream_pipe_states[env];
  if (g_stream_pipe_cleanup_hooks.emplace(env).second) {
    if (napi_add_env_cleanup_hook(env, CleanupStreamPipeEnv, env) != napi_ok) {
      g_stream_pipe_cleanup_hooks.erase(env);
    }
  }
  return state;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return nullptr;
  return value;
}

bool IsFunction(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

bool GetInt32Property(napi_env env, napi_value obj, const char* name, int32_t* out) {
  if (out == nullptr) return false;
  *out = 0;
  if (env == nullptr || obj == nullptr || name == nullptr) return false;
  napi_value value = nullptr;
  return napi_get_named_property(env, obj, name, &value) == napi_ok &&
         value != nullptr &&
         napi_get_value_int32(env, value, out) == napi_ok;
}

bool GetInt64Property(napi_env env, napi_value obj, const char* name, int64_t* out) {
  if (out == nullptr) return false;
  *out = 0;
  if (env == nullptr || obj == nullptr || name == nullptr) return false;
  napi_value value = nullptr;
  return napi_get_named_property(env, obj, name, &value) == napi_ok &&
         value != nullptr &&
         napi_get_value_int64(env, value, out) == napi_ok;
}

bool CallNamedIntMethod(napi_env env,
                        napi_value recv,
                        const char* name,
                        size_t argc,
                        napi_value* argv,
                        int32_t* out) {
  if (out == nullptr) return false;
  *out = 0;
  if (env == nullptr || recv == nullptr || name == nullptr) return false;
  napi_value fn = nullptr;
  napi_value result = nullptr;
  return napi_get_named_property(env, recv, name, &fn) == napi_ok &&
         IsFunction(env, fn) &&
         napi_call_function(env, recv, fn, argc, argv, &result) == napi_ok &&
         result != nullptr &&
         napi_get_value_int32(env, result, out) == napi_ok;
}

StreamPipeWrap* UnwrapPipe(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  StreamPipeWrap* wrap = nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok) return nullptr;
  return wrap;
}

void StreamPipeFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<StreamPipeWrap*>(data);
  if (wrap == nullptr) return;
  DeleteRefIfPresent(env, &wrap->source_ref);
  DeleteRefIfPresent(env, &wrap->sink_ref);
  DeleteRefIfPresent(env, &wrap->wrapper_ref);
  delete wrap;
}

void CallOnUnpipe(napi_env env, napi_value self) {
  if (env == nullptr || self == nullptr) return;
  napi_value onunpipe = nullptr;
  if (napi_get_named_property(env, self, "onunpipe", &onunpipe) != napi_ok || !IsFunction(env, onunpipe)) return;
  napi_value ignored = nullptr;
  (void)napi_call_function(env, self, onunpipe, 0, nullptr, &ignored);
}

void ClearLinks(napi_env env, napi_value self, StreamPipeWrap* wrap) {
  napi_value null_value = nullptr;
  napi_get_null(env, &null_value);
  if (self != nullptr) {
    (void)napi_set_named_property(env, self, "source", null_value);
    (void)napi_set_named_property(env, self, "sink", null_value);
  }
  if (wrap == nullptr) return;
  DeleteRefIfPresent(env, &wrap->source_ref);
  DeleteRefIfPresent(env, &wrap->sink_ref);
}

void ClosePipe(napi_env env, napi_value self, StreamPipeWrap* wrap) {
  if (wrap == nullptr || wrap->closed) return;
  wrap->closed = true;
  wrap->pump_scheduled = false;
  CallOnUnpipe(env, self);
  ClearLinks(env, self, wrap);
}

void NotifySourceRead(napi_env env, napi_value source, int32_t status) {
  if (env == nullptr || source == nullptr) return;
  if (IsStreamPipeDebugEnabled()) {
    std::fprintf(stderr, "STREAM_PIPE notify source status=%d\n", status);
  }
  int32_t* state = EdgeGetStreamBaseState(env);
  if (state != nullptr) {
    state[kEdgeReadBytesOrError] = status;
    state[kEdgeArrayBufferOffset] = 0;
  }
  napi_value onread = nullptr;
  if (napi_get_named_property(env, source, "onread", &onread) != napi_ok || !IsFunction(env, onread)) return;
  napi_value ignored = nullptr;
  (void)napi_call_function(env, source, onread, 0, nullptr, &ignored);
}

void MaybeFinishPipeAfterWrites(napi_env env, napi_value self, StreamPipeWrap* wrap) {
  if (env == nullptr || self == nullptr || wrap == nullptr || wrap->closed || !wrap->eof_pending ||
      wrap->pending_writes != 0) {
    return;
  }
  napi_value sink = GetRefValue(env, wrap->sink_ref);
  napi_value source = GetRefValue(env, wrap->source_ref);
  int32_t rc = 0;
  if (sink != nullptr) {
    napi_value shutdown_req = EdgeCreateStreamReqObject(env);
    napi_value argv[1] = {shutdown_req != nullptr ? shutdown_req : Undefined(env)};
    (void)CallNamedIntMethod(env, sink, "shutdown", 1, argv, &rc);
  }
  if (source != nullptr) {
    NotifySourceRead(env, source, UV_EOF);
  }
  ClosePipe(env, self, wrap);
}

napi_value StreamPipeWriteComplete(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  napi_value self = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, &self, &data);
  auto* wrap = static_cast<StreamPipeWrap*>(data);
  if (wrap == nullptr || wrap->pending_writes == 0) return Undefined(env);
  wrap->pending_writes--;
  napi_value pipe_self = GetRefValue(env, wrap->wrapper_ref);
  if (pipe_self == nullptr) pipe_self = self;
  MaybeFinishPipeAfterWrites(env, pipe_self, wrap);
  return Undefined(env);
}

bool PipeFileHandleToHttp2Stream(napi_env env, napi_value source, napi_value sink, napi_value self, StreamPipeWrap* wrap) {
  if (env == nullptr || source == nullptr || sink == nullptr || self == nullptr || wrap == nullptr) return false;

  int32_t fd = -1;
  int64_t offset = 0;
  int64_t remaining = -1;
  if (!GetInt32Property(env, source, "fd", &fd) || fd < 0) {
    ClosePipe(env, self, wrap);
    return false;
  }
  if (!wrap->source_kind_known) {
    struct stat st;
    if (fstat(fd, &st) == 0) {
      wrap->source_is_fifo = S_ISFIFO(st.st_mode);
    }
    wrap->source_kind_known = true;
  }
  (void)GetInt64Property(env, source, "offset", &offset);
  (void)GetInt64Property(env, source, "length", &remaining);

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  constexpr size_t kChunkSize = 64 * 1024;
  int32_t rc = 0;
  for (;;) {
    if (wrap->closed) return true;
    size_t read_len = kChunkSize;
    if (remaining >= 0) {
      if (remaining == 0) break;
      read_len = static_cast<size_t>(std::min<int64_t>(remaining, static_cast<int64_t>(kChunkSize)));
    }

    std::vector<char> storage(read_len);
    uv_buf_t buf = uv_buf_init(storage.data(), static_cast<unsigned int>(storage.size()));
    uv_fs_t req;
    std::memset(&req, 0, sizeof(req));
    const int64_t read_offset = wrap->source_is_fifo ? -1 : offset;
    const int read_rc = static_cast<int>(uv_fs_read(nullptr, &req, fd, &buf, 1, read_offset, nullptr));
    uv_fs_req_cleanup(&req);
    if (read_rc == UV_EAGAIN) {
      if (IsStreamPipeDebugEnabled()) {
        std::fprintf(stderr, "STREAM_PIPE read rc=UV_EAGAIN offset=%lld remaining=%lld saw_data=%d fifo=%d\n",
                     static_cast<long long>(offset),
                     static_cast<long long>(remaining),
                     wrap->saw_source_data ? 1 : 0,
                     wrap->source_is_fifo ? 1 : 0);
      }
      return true;
    }
    if (read_rc < 0) {
      if (IsStreamPipeDebugEnabled()) {
        std::fprintf(stderr, "STREAM_PIPE read rc=%d offset=%lld remaining=%lld\n",
                     read_rc,
                     static_cast<long long>(offset),
                     static_cast<long long>(remaining));
      }
      NotifySourceRead(env, source, read_rc);
      ClosePipe(env, self, wrap);
      return false;
    }
    if (read_rc == 0) {
      if (IsStreamPipeDebugEnabled()) {
        std::fprintf(stderr, "STREAM_PIPE read rc=0 offset=%lld remaining=%lld saw_data=%d fifo=%d\n",
                     static_cast<long long>(offset),
                     static_cast<long long>(remaining),
                     wrap->saw_source_data ? 1 : 0,
                     wrap->source_is_fifo ? 1 : 0);
      }
      if (wrap->source_is_fifo && !wrap->saw_source_data) {
        return true;
      }
      break;
    }

    if (!wrap->source_is_fifo) {
      offset += read_rc;
    }
    if (remaining >= 0) remaining -= read_rc;
    wrap->saw_source_data = true;

    napi_value req_obj = EdgeCreateStreamReqObject(env);
    napi_value chunk = nullptr;
    void* copy = nullptr;
    napi_value oncomplete = nullptr;
    if (req_obj == nullptr ||
        napi_create_function(env,
                             "__ubiStreamPipeWriteComplete",
                             NAPI_AUTO_LENGTH,
                             StreamPipeWriteComplete,
                             wrap,
                             &oncomplete) != napi_ok ||
        oncomplete == nullptr ||
        napi_set_named_property(env, req_obj, "oncomplete", oncomplete) != napi_ok ||
        napi_create_buffer_copy(env, read_rc, storage.data(), &copy, &chunk) != napi_ok ||
        chunk == nullptr) {
      ClosePipe(env, self, wrap);
      return false;
    }
    wrap->pending_writes++;
    napi_value argv[2] = {req_obj, chunk};
    if (!CallNamedIntMethod(env, sink, "writeBuffer", 2, argv, &rc) || rc < 0) {
      if (IsStreamPipeDebugEnabled()) {
        std::fprintf(stderr, "STREAM_PIPE sink write rc=%d\n", rc);
      }
      if (wrap->pending_writes > 0) wrap->pending_writes--;
      NotifySourceRead(env, source, rc < 0 ? rc : UV_EIO);
      ClosePipe(env, self, wrap);
      return false;
    }
    int32_t* state = EdgeGetStreamBaseState(env);
    if (state == nullptr || state[kEdgeLastWriteWasAsync] == 0) {
      if (wrap->pending_writes > 0) wrap->pending_writes--;
    }
  }

  wrap->eof_pending = true;
  MaybeFinishPipeAfterWrites(env, self, wrap);
  return true;
}

void SchedulePipePump(StreamPipeWrap* wrap);

napi_value DeferredPipePump(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  auto* wrap = static_cast<StreamPipeWrap*>(data);
  if (wrap == nullptr || wrap->closed) return Undefined(env);
  wrap->pump_scheduled = false;
  napi_value self = GetRefValue(env, wrap->wrapper_ref);
  napi_value source = GetRefValue(env, wrap->source_ref);
  napi_value sink = GetRefValue(env, wrap->sink_ref);
  if (self == nullptr || source == nullptr || sink == nullptr) {
    ClosePipe(env, self, wrap);
    return Undefined(env);
  }
  const bool done = PipeFileHandleToHttp2Stream(env, source, sink, self, wrap);
  if (!wrap->closed && !wrap->eof_pending && done) {
    SchedulePipePump(wrap);
  }
  return Undefined(env);
}

void SchedulePipePump(StreamPipeWrap* wrap) {
  if (wrap == nullptr || wrap->closed || wrap->pump_scheduled || wrap->env == nullptr) return;
  napi_value callback = nullptr;
  if (napi_create_function(wrap->env,
                           "__ubiStreamPipePump",
                           NAPI_AUTO_LENGTH,
                           DeferredPipePump,
                           wrap,
                           &callback) != napi_ok ||
      callback == nullptr) {
    return;
  }
  napi_value global = GetGlobal(wrap->env);
  napi_value set_immediate = nullptr;
  napi_valuetype type = napi_undefined;
  if (global == nullptr ||
      napi_get_named_property(wrap->env, global, "setImmediate", &set_immediate) != napi_ok ||
      set_immediate == nullptr ||
      napi_typeof(wrap->env, set_immediate, &type) != napi_ok ||
      type != napi_function) {
    return;
  }
  wrap->pump_scheduled = true;
  napi_value argv[1] = {callback};
  napi_value ignored = nullptr;
  (void)napi_call_function(wrap->env, global, set_immediate, 1, argv, &ignored);
}

napi_value StreamPipeCtor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);

  auto* wrap = new StreamPipeWrap();
  wrap->env = env;
  if (napi_wrap(env, self, wrap, StreamPipeFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    delete wrap;
    return nullptr;
  }

  napi_value null_value = nullptr;
  napi_get_null(env, &null_value);
  napi_value source = argc >= 1 && argv[0] != nullptr ? argv[0] : null_value;
  napi_value sink = argc >= 2 && argv[1] != nullptr ? argv[1] : null_value;
  if (source != nullptr && !IsUndefined(env, source)) {
    (void)napi_create_reference(env, source, 1, &wrap->source_ref);
  }
  if (sink != nullptr && !IsUndefined(env, sink)) {
    (void)napi_create_reference(env, sink, 1, &wrap->sink_ref);
  }
  (void)napi_set_named_property(env, self, "source", source != nullptr ? source : null_value);
  (void)napi_set_named_property(env, self, "sink", sink != nullptr ? sink : null_value);
  (void)napi_set_named_property(env, self, "onunpipe", Undefined(env));
  return self;
}

 napi_value StreamPipeStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  StreamPipeWrap* wrap = UnwrapPipe(env, self);
  if (wrap == nullptr || wrap->closed) return Undefined(env);
  napi_value source = GetRefValue(env, wrap->source_ref);
  napi_value sink = GetRefValue(env, wrap->sink_ref);
  if (source == nullptr || sink == nullptr) {
    ClosePipe(env, self, wrap);
    return Undefined(env);
  }
  const bool needs_retry = PipeFileHandleToHttp2Stream(env, source, sink, self, wrap);
  if (!wrap->closed && !wrap->eof_pending && needs_retry) {
    SchedulePipePump(wrap);
  }
  return Undefined(env);
}

napi_value StreamPipeUnpipe(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  StreamPipeWrap* wrap = UnwrapPipe(env, self);
  if (wrap != nullptr && !wrap->closed) {
    ClosePipe(env, self, wrap);
  }
  return Undefined(env);
}

napi_value StreamPipeIsClosed(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  StreamPipeWrap* wrap = UnwrapPipe(env, self);
  napi_value out = nullptr;
  napi_get_boolean(env, wrap != nullptr ? wrap->closed : true, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value StreamPipePendingWrites(napi_env env, napi_callback_info /*info*/) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

}  // namespace

napi_value ResolveStreamPipe(napi_env env, const ResolveOptions& /*options*/) {
  StreamPipeBindingState& state = EnsureState(env);
  napi_value cached = GetRefValue(env, state.binding_ref);
  if (cached != nullptr) return cached;

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return Undefined(env);

  napi_property_descriptor props[] = {
      {"unpipe", nullptr, StreamPipeUnpipe, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"start", nullptr, StreamPipeStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"isClosed", nullptr, StreamPipeIsClosed, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"pendingWrites", nullptr, StreamPipePendingWrites, nullptr, nullptr, nullptr, napi_default_method, nullptr},
  };
  napi_value ctor = nullptr;
  if (napi_define_class(env,
                        "StreamPipe",
                        NAPI_AUTO_LENGTH,
                        StreamPipeCtor,
                        nullptr,
                        sizeof(props) / sizeof(props[0]),
                        props,
                        &ctor) != napi_ok ||
      ctor == nullptr) {
    return Undefined(env);
  }

  napi_set_named_property(env, binding, "StreamPipe", ctor);
  DeleteRefIfPresent(env, &state.binding_ref);
  (void)napi_create_reference(env, binding, 1, &state.binding_ref);
  return binding;
}

}  // namespace internal_binding
