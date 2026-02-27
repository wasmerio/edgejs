#include "unode_pipe_wrap.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <uv.h>

#include "unode_runtime.h"
#include "unode_stream_wrap.h"

namespace {

constexpr int kPipeSocket = 0;
constexpr int kPipeServer = 1;

struct PipeWrap;

struct PipeConnectReqWrap {
  uv_connect_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  PipeWrap* pipe = nullptr;
};

struct PipeWriteReqWrap {
  uv_write_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  uv_buf_t buf{};
};

struct PipeShutdownReqWrap {
  uv_shutdown_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  PipeWrap* pipe = nullptr;
};

struct PipeWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref close_cb_ref = nullptr;
  uv_pipe_t handle{};
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  int64_t async_id = 0;
};

napi_ref g_pipe_ctor_ref = nullptr;
int64_t g_next_pipe_async_id = 100000;

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value v = nullptr;
  if (napi_get_reference_value(env, ref, &v) != napi_ok) return nullptr;
  return v;
}

void SetReqError(napi_env env, napi_value req_obj, int status) {
  if (req_obj == nullptr || status >= 0) return;
  const char* err = uv_err_name(status);
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, err != nullptr ? err : "UV_ERROR", NAPI_AUTO_LENGTH, &err_v);
  if (err_v != nullptr) napi_set_named_property(env, req_obj, "error", err_v);
}

int32_t* StreamState() { return UnodeGetStreamBaseState(); }
void SetState(int idx, int32_t value) {
  int32_t* s = StreamState();
  if (s) s[idx] = value;
}

napi_value MakeInt32(napi_env env, int32_t v) {
  napi_value out = nullptr;
  napi_create_int32(env, v, &out);
  return out;
}

napi_value MakeBool(napi_env env, bool v) {
  napi_value out = nullptr;
  napi_get_boolean(env, v, &out);
  return out;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  size_t len = 0;
  if (napi_coerce_to_string(env, value, &value) != napi_ok ||
      napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) {
    return {};
  }
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

void InvokeReqOnComplete(napi_env env, napi_value req_obj, int status, napi_value* argv, size_t argc) {
  if (req_obj == nullptr) return;
  SetReqError(env, req_obj, status);
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || oncomplete == nullptr) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, oncomplete, &t);
  if (t != napi_function) return;
  napi_value ignored = nullptr;
  UnodeMakeCallback(env, req_obj, oncomplete, argc, argv, &ignored);
}

void OnWriteDone(uv_write_t* req, int status) {
  auto* wr = static_cast<PipeWriteReqWrap*>(req->data);
  if (wr == nullptr) return;
  napi_value req_obj = GetRefValue(wr->env, wr->req_obj_ref);
  napi_value argv[1] = {MakeInt32(wr->env, status)};
  InvokeReqOnComplete(wr->env, req_obj, status, argv, 1);
  if (wr->buf.base) free(wr->buf.base);
  if (wr->req_obj_ref) napi_delete_reference(wr->env, wr->req_obj_ref);
  delete wr;
}

void OnShutdownDone(uv_shutdown_t* req, int status) {
  auto* sr = static_cast<PipeShutdownReqWrap*>(req->data);
  if (sr == nullptr) return;
  napi_value req_obj = GetRefValue(sr->env, sr->req_obj_ref);
  napi_value pipe_obj = sr->pipe ? GetRefValue(sr->env, sr->pipe->wrapper_ref) : nullptr;
  napi_value argv[3] = {MakeInt32(sr->env, status), pipe_obj, req_obj};
  InvokeReqOnComplete(sr->env, req_obj, status, argv, 3);
  if (sr->req_obj_ref) napi_delete_reference(sr->env, sr->req_obj_ref);
  delete sr;
}

void OnConnectDone(uv_connect_t* req, int status) {
  auto* cr = static_cast<PipeConnectReqWrap*>(req->data);
  if (cr == nullptr) return;
  napi_value req_obj = GetRefValue(cr->env, cr->req_obj_ref);
  napi_value pipe_obj = cr->pipe ? GetRefValue(cr->env, cr->pipe->wrapper_ref) : nullptr;
  napi_value argv[5] = {
      MakeInt32(cr->env, status),
      pipe_obj,
      req_obj,
      MakeBool(cr->env, true),
      MakeBool(cr->env, true),
  };
  InvokeReqOnComplete(cr->env, req_obj, status, argv, 5);
  if (cr->req_obj_ref) napi_delete_reference(cr->env, cr->req_obj_ref);
  delete cr;
}

void OnAlloc(uv_handle_t* /*h*/, size_t suggested_size, uv_buf_t* buf) {
  char* base = static_cast<char*>(malloc(suggested_size));
  *buf = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
}

void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* wrap = static_cast<PipeWrap*>(stream->data);
  if (wrap == nullptr) {
    if (buf && buf->base) free(buf->base);
    return;
  }
  SetState(kUnodeReadBytesOrError, static_cast<int32_t>(nread));
  SetState(kUnodeArrayBufferOffset, 0);
  if (nread > 0) wrap->bytes_read += static_cast<uint64_t>(nread);

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = nullptr;
  if (self != nullptr && napi_get_named_property(wrap->env, self, "onread", &onread) == napi_ok) {
    napi_valuetype t = napi_undefined;
    napi_typeof(wrap->env, onread, &t);
    if (t == napi_function) {
      napi_value argv[1] = {nullptr};
      if (nread > 0 && buf && buf->base) {
        void* out = nullptr;
        napi_value ab = nullptr;
        if (napi_create_arraybuffer(wrap->env, nread, &out, &ab) == napi_ok && out && ab) {
          memcpy(out, buf->base, nread);
          argv[0] = ab;
        }
      } else {
        napi_get_undefined(wrap->env, &argv[0]);
      }
      napi_value ignored = nullptr;
      UnodeMakeCallback(wrap->env, self, onread, 1, argv, &ignored);
    }
  }
  if (buf && buf->base) free(buf->base);
}

void OnClosed(uv_handle_t* h) {
  auto* wrap = static_cast<PipeWrap*>(h->data);
  if (wrap == nullptr) return;
  wrap->closed = true;
  if (!wrap->finalized && wrap->close_cb_ref) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetRefValue(wrap->env, wrap->close_cb_ref);
    if (cb) {
      napi_value ignored = nullptr;
      UnodeMakeCallback(wrap->env, self, cb, 0, nullptr, &ignored);
    }
    napi_delete_reference(wrap->env, wrap->close_cb_ref);
    wrap->close_cb_ref = nullptr;
  }
  if (wrap->delete_on_close || wrap->finalized) {
    delete wrap;
  }
}

void PipeFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<PipeWrap*>(data);
  if (!wrap) return;
  wrap->finalized = true;
  if (wrap->wrapper_ref) {
    napi_delete_reference(env, wrap->wrapper_ref);
    wrap->wrapper_ref = nullptr;
  }
  if (wrap->close_cb_ref) {
    napi_delete_reference(env, wrap->close_cb_ref);
    wrap->close_cb_ref = nullptr;
  }
  uv_handle_t* h = reinterpret_cast<uv_handle_t*>(&wrap->handle);
  if (!wrap->closed) {
    wrap->delete_on_close = true;
    if (!uv_is_closing(h)) {
      uv_close(h, OnClosed);
    }
    return;
  }
  delete wrap;
}

napi_value PipeCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  auto* wrap = new PipeWrap();
  wrap->env = env;
  wrap->async_id = g_next_pipe_async_id++;
  int ipc = 0;
  uv_pipe_init(uv_default_loop(), &wrap->handle, ipc);
  wrap->handle.data = wrap;
  napi_wrap(env, self, wrap, PipeFinalize, nullptr, &wrap->wrapper_ref);
  return self;
}

napi_value PipeOpen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t fd = -1;
  napi_get_value_int32(env, argv[0], &fd);
  int rc = uv_pipe_open(&wrap->handle, static_cast<uv_file>(fd));
  return MakeInt32(env, rc);
}

napi_value PipeBind(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  std::string path = ValueToUtf8(env, argv[0]);
#if !defined(_WIN32)
  unlink(path.c_str());
#endif
  int rc = uv_pipe_bind(&wrap->handle, path.c_str());
  return MakeInt32(env, rc);
}

void OnConnection(uv_stream_t* server, int status) {
  auto* server_wrap = static_cast<PipeWrap*>(server->data);
  if (!server_wrap) return;
  napi_env env = server_wrap->env;
  napi_value server_obj = GetRefValue(env, server_wrap->wrapper_ref);
  napi_value onconnection = nullptr;
  if (!server_obj || napi_get_named_property(env, server_obj, "onconnection", &onconnection) != napi_ok) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, onconnection, &t);
  if (t != napi_function) return;

  napi_value argv[2] = {MakeInt32(env, status), nullptr};
  if (status == 0) {
    napi_value ctor = GetRefValue(env, g_pipe_ctor_ref);
    napi_value arg0 = nullptr;
    napi_create_int32(env, kPipeSocket, &arg0);
    napi_value client_obj = nullptr;
    napi_new_instance(env, ctor, 1, &arg0, &client_obj);
    PipeWrap* client_wrap = nullptr;
    napi_unwrap(env, client_obj, reinterpret_cast<void**>(&client_wrap));
    int rc = uv_accept(server, reinterpret_cast<uv_stream_t*>(&client_wrap->handle));
    argv[0] = MakeInt32(env, rc);
    argv[1] = client_obj;
  }
  napi_value ignored = nullptr;
  UnodeMakeCallback(env, server_obj, onconnection, 2, argv, &ignored);
}

napi_value PipeListen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t backlog = 511;
  napi_get_value_int32(env, argv[0], &backlog);
  int rc = uv_listen(reinterpret_cast<uv_stream_t*>(&wrap->handle), backlog, OnConnection);
  return MakeInt32(env, rc);
}

napi_value PipeConnect(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 2) return MakeInt32(env, UV_EINVAL);
  auto* cr = new PipeConnectReqWrap();
  cr->env = env;
  cr->pipe = wrap;
  cr->req.data = cr;
  napi_create_reference(env, argv[0], 1, &cr->req_obj_ref);
  std::string path = ValueToUtf8(env, argv[1]);
  uv_pipe_connect(&cr->req, &wrap->handle, path.c_str(), OnConnectDone);
  int rc = 0;
  // uv_pipe_connect is async and returns void in libuv.
  return MakeInt32(env, rc);
}

napi_value PipeReadStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap) return MakeInt32(env, UV_EINVAL);
  int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&wrap->handle), OnAlloc, OnRead);
  napi_set_named_property(env, self, "reading", MakeBool(env, rc == 0));
  return MakeInt32(env, rc);
}

napi_value PipeReadStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap) return MakeInt32(env, UV_EINVAL);
  int rc = uv_read_stop(reinterpret_cast<uv_stream_t*>(&wrap->handle));
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return MakeInt32(env, rc);
}

napi_value PipeWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 2) return MakeInt32(env, UV_EINVAL);

  size_t length = 0;
  void* raw = nullptr;
  bool is_typed = false;
  napi_is_typedarray(env, argv[1], &is_typed);
  if (is_typed) {
    napi_typedarray_type tt;
    napi_value ab;
    size_t off;
    napi_get_typedarray_info(env, argv[1], &tt, &length, &raw, &ab, &off);
  } else {
    std::string s = ValueToUtf8(env, argv[1]);
    length = s.size();
    raw = const_cast<char*>(s.data());
  }

  auto* wr = new PipeWriteReqWrap();
  wr->env = env;
  napi_create_reference(env, argv[0], 1, &wr->req_obj_ref);
  char* copy = static_cast<char*>(malloc(length));
  if (length > 0 && raw != nullptr) memcpy(copy, raw, length);
  wr->buf = uv_buf_init(copy, static_cast<unsigned int>(length));
  wr->req.data = wr;
  wrap->bytes_written += length;
  SetState(kUnodeBytesWritten, static_cast<int32_t>(length));
  SetState(kUnodeLastWriteWasAsync, 1);
  int rc = uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&wrap->handle), &wr->buf, 1, OnWriteDone);
  if (rc != 0) {
    napi_value req_obj = GetRefValue(env, wr->req_obj_ref);
    SetReqError(env, req_obj, rc);
    if (wr->buf.base) free(wr->buf.base);
    if (wr->req_obj_ref) napi_delete_reference(env, wr->req_obj_ref);
    delete wr;
  }
  return MakeInt32(env, rc);
}

napi_value PipeWriteString(napi_env env, napi_callback_info info) {
  return PipeWriteBuffer(env, info);
}

napi_value PipeWritev(napi_env env, napi_callback_info info) {
  // Minimal fallback: write first chunk only.
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  if (argc < 2) return MakeInt32(env, UV_EINVAL);
  napi_value first = nullptr;
  uint32_t len = 0;
  napi_get_array_length(env, argv[1], &len);
  if (len == 0) return MakeInt32(env, 0);
  napi_get_element(env, argv[1], 0, &first);
  napi_value fake_argv[2] = {argv[0], first};
  napi_value ignored = nullptr;
  size_t fake_argc = 2;
  // Re-route through writeBuffer contract.
  napi_callback_info fake_info = info;
  (void)fake_info;
  // Not possible to synthesize napi_callback_info; call directly logic instead.
  // Duplicate minimal path:
  std::string s = ValueToUtf8(env, first);
  auto* wr = new PipeWriteReqWrap();
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  wr->env = env;
  napi_create_reference(env, argv[0], 1, &wr->req_obj_ref);
  char* copy = static_cast<char*>(malloc(s.size()));
  if (!s.empty()) memcpy(copy, s.data(), s.size());
  wr->buf = uv_buf_init(copy, static_cast<unsigned int>(s.size()));
  wr->req.data = wr;
  wrap->bytes_written += s.size();
  SetState(kUnodeBytesWritten, static_cast<int32_t>(s.size()));
  SetState(kUnodeLastWriteWasAsync, 1);
  int rc = uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&wrap->handle), &wr->buf, 1, OnWriteDone);
  (void)ignored;
  (void)fake_argv;
  (void)fake_argc;
  return MakeInt32(env, rc);
}

napi_value PipeShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  auto* sr = new PipeShutdownReqWrap();
  sr->env = env;
  sr->pipe = wrap;
  sr->req.data = sr;
  napi_create_reference(env, argv[0], 1, &sr->req_obj_ref);
  int rc = uv_shutdown(&sr->req, reinterpret_cast<uv_stream_t*>(&wrap->handle), OnShutdownDone);
  if (rc != 0) {
    napi_value req_obj = GetRefValue(env, sr->req_obj_ref);
    SetReqError(env, req_obj, rc);
    napi_delete_reference(env, sr->req_obj_ref);
    delete sr;
  }
  return MakeInt32(env, rc);
}

napi_value PipeClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }
  if (argc > 0 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t == napi_function) {
      if (wrap->close_cb_ref) napi_delete_reference(env, wrap->close_cb_ref);
      napi_create_reference(env, argv[0], 1, &wrap->close_cb_ref);
    }
  }
  if (!wrap->closed && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&wrap->handle))) {
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->handle), OnClosed);
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeSetPendingInstances(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t n = 0;
  napi_get_value_int32(env, argv[0], &n);
  uv_pipe_pending_instances(&wrap->handle, n);
  return MakeInt32(env, 0);
}

napi_value PipeFchmod(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
#ifdef UV_VERSION_MAJOR
  int32_t mode = 0;
  napi_get_value_int32(env, argv[0], &mode);
  int rc = uv_pipe_chmod(&wrap->handle, mode);
  return MakeInt32(env, rc);
#else
  return MakeInt32(env, UV_ENOTSUP);
#endif
}

napi_value PipeRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap) uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeUnref(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap) uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_int64(env, wrap ? wrap->async_id : -1, &out);
  return out;
}

napi_value PipeGetProviderType(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, kPipeSocket);
}

napi_value PipeAsyncReset(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap) wrap->async_id = g_next_pipe_async_id++;
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value PipeGetSockName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  size_t len = 0;
  uv_pipe_getsockname(&wrap->handle, nullptr, &len);
  std::string name(len, '\0');
  int rc = uv_pipe_getsockname(&wrap->handle, name.data(), &len);
  if (rc == 0) {
    name.resize(len);
    napi_value s = nullptr;
    napi_create_string_utf8(env, name.c_str(), name.size(), &s);
    if (s) napi_set_named_property(env, argv[0], "address", s);
  }
  return MakeInt32(env, rc);
}

napi_value PipeGetPeerName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (!wrap || argc < 1) return MakeInt32(env, UV_EINVAL);
  size_t len = 0;
  uv_pipe_getpeername(&wrap->handle, nullptr, &len);
  std::string name(len, '\0');
  int rc = uv_pipe_getpeername(&wrap->handle, name.data(), &len);
  if (rc == 0) {
    name.resize(len);
    napi_value s = nullptr;
    napi_create_string_utf8(env, name.c_str(), name.size(), &s);
    if (s) napi_set_named_property(env, argv[0], "address", s);
  }
  return MakeInt32(env, rc);
}

napi_value PipeBytesReadGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(wrap ? wrap->bytes_read : 0), &out);
  return out;
}

napi_value PipeBytesWrittenGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(wrap ? wrap->bytes_written : 0), &out);
  return out;
}

napi_value PipeFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  PipeWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  int32_t fd = -1;
  if (wrap != nullptr) {
    uv_os_fd_t raw = -1;
    if (uv_fileno(reinterpret_cast<const uv_handle_t*>(&wrap->handle), &raw) == 0) {
      fd = static_cast<int32_t>(raw);
    }
  }
  return MakeInt32(env, fd);
}

napi_value PipeConnectWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  return self;
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  if (v) napi_set_named_property(env, obj, key, v);
}

}  // namespace

void UnodeInstallPipeWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;

  napi_property_descriptor pipe_props[] = {
      {"open", nullptr, PipeOpen, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bind", nullptr, PipeBind, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"listen", nullptr, PipeListen, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"connect", nullptr, PipeConnect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"close", nullptr, PipeClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readStart", nullptr, PipeReadStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readStop", nullptr, PipeReadStop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeBuffer", nullptr, PipeWriteBuffer, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writev", nullptr, PipeWritev, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeLatin1String", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeUtf8String", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeAsciiString", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeUcs2String", nullptr, PipeWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"shutdown", nullptr, PipeShutdown, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setPendingInstances", nullptr, PipeSetPendingInstances, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"fchmod", nullptr, PipeFchmod, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getsockname", nullptr, PipeGetSockName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getpeername", nullptr, PipeGetPeerName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, PipeRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unref", nullptr, PipeUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getAsyncId", nullptr, PipeGetAsyncId, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getProviderType", nullptr, PipeGetProviderType, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"asyncReset", nullptr, PipeAsyncReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bytesRead", nullptr, nullptr, PipeBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, PipeBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, PipeFdGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value pipe_ctor = nullptr;
  if (napi_define_class(env,
                        "Pipe",
                        NAPI_AUTO_LENGTH,
                        PipeCtor,
                        nullptr,
                        sizeof(pipe_props) / sizeof(pipe_props[0]),
                        pipe_props,
                        &pipe_ctor) != napi_ok ||
      pipe_ctor == nullptr) {
    return;
  }
  if (g_pipe_ctor_ref != nullptr) napi_delete_reference(env, g_pipe_ctor_ref);
  napi_create_reference(env, pipe_ctor, 1, &g_pipe_ctor_ref);

  napi_value connect_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "PipeConnectWrap",
                        NAPI_AUTO_LENGTH,
                        PipeConnectWrapCtor,
                        nullptr,
                        0,
                        nullptr,
                        &connect_wrap_ctor) != napi_ok ||
      connect_wrap_ctor == nullptr) {
    return;
  }

  napi_value constants = nullptr;
  napi_create_object(env, &constants);
  SetNamedU32(env, constants, "SOCKET", kPipeSocket);
  SetNamedU32(env, constants, "SERVER", kPipeServer);

  napi_set_named_property(env, binding, "Pipe", pipe_ctor);
  napi_set_named_property(env, binding, "PipeConnectWrap", connect_wrap_ctor);
  napi_set_named_property(env, binding, "constants", constants);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_set_named_property(env, global, "__unode_pipe_wrap", binding);
}
