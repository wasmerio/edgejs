#include "unode_tcp_wrap.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <uv.h>

#include "unode_runtime.h"
#include "unode_stream_wrap.h"

namespace {

constexpr int kTcpSocket = 0;
constexpr int kTcpServer = 1;

struct TcpWrap;

struct ConnectReqWrap {
  uv_connect_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  TcpWrap* tcp = nullptr;
};

struct WriteReqWrap {
  uv_write_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  uv_buf_t* bufs = nullptr;
  uint32_t nbufs = 0;
};

struct ShutdownReqWrap {
  uv_shutdown_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  TcpWrap* tcp = nullptr;
};

struct TcpWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref close_cb_ref = nullptr;
  uv_tcp_t handle{};
  bool initialized = false;
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  uint64_t bytes_read = 0;
  uint64_t bytes_written = 0;
  int64_t async_id = 0;
};

napi_ref g_tcp_ctor_ref = nullptr;
napi_ref g_connect_wrap_ctor_ref = nullptr;
int64_t g_next_async_id = 1;

void OnClosed(uv_handle_t* h);

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value v = nullptr;
  if (napi_get_reference_value(env, ref, &v) != napi_ok) return nullptr;
  return v;
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

void SetReqError(napi_env env, napi_value req_obj, int status) {
  if (req_obj == nullptr || status >= 0) return;
  const char* err = uv_err_name(status);
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, err != nullptr ? err : "UV_ERROR", NAPI_AUTO_LENGTH, &err_v);
  if (err_v != nullptr) napi_set_named_property(env, req_obj, "error", err_v);
}

int32_t* StreamState() {
  return UnodeGetStreamBaseState();
}

void SetState(int idx, int32_t value) {
  int32_t* s = StreamState();
  if (s == nullptr) return;
  s[idx] = value;
}

napi_value GetThis(napi_env env, napi_callback_info info, size_t* argc_out, napi_value* argv, TcpWrap** wrap_out) {
  size_t argc = argc_out ? *argc_out : 0;
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  if (argc_out) *argc_out = argc;
  if (wrap_out != nullptr) {
    *wrap_out = nullptr;
    napi_unwrap(env, self, reinterpret_cast<void**>(wrap_out));
  }
  return self;
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

void FreeWriteReq(WriteReqWrap* r) {
  if (r == nullptr) return;
  if (r->bufs != nullptr) {
    for (uint32_t i = 0; i < r->nbufs; i++) free(r->bufs[i].base);
    delete[] r->bufs;
  }
  if (r->req_obj_ref != nullptr) napi_delete_reference(r->env, r->req_obj_ref);
  delete r;
}

void InvokeReqOnComplete(napi_env env, napi_value req_obj, int status, napi_value* argv = nullptr, size_t argc = 1) {
  if (req_obj == nullptr) return;
  SetReqError(env, req_obj, status);
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || oncomplete == nullptr) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, oncomplete, &t);
  if (t != napi_function) return;
  napi_value status_v = MakeInt32(env, status);
  napi_value local_argv[5] = {status_v, nullptr, nullptr, nullptr, nullptr};
  if (argv != nullptr && argc > 0) {
    for (size_t i = 0; i < argc; i++) local_argv[i] = argv[i];
  } else {
    argc = 1;
  }
  napi_value ignored = nullptr;
  UnodeMakeCallback(env, req_obj, oncomplete, argc, local_argv, &ignored);
}

void OnWriteDone(uv_write_t* req, int status) {
  auto* wr = static_cast<WriteReqWrap*>(req->data);
  if (wr == nullptr) return;
  napi_value req_obj = GetRefValue(wr->env, wr->req_obj_ref);
  napi_value argv[1] = {MakeInt32(wr->env, status)};
  InvokeReqOnComplete(wr->env, req_obj, status, argv, 1);
  FreeWriteReq(wr);
}

void OnShutdownDone(uv_shutdown_t* req, int status) {
  auto* sr = static_cast<ShutdownReqWrap*>(req->data);
  if (sr == nullptr) return;
  napi_value req_obj = GetRefValue(sr->env, sr->req_obj_ref);
  napi_value tcp_obj = sr->tcp != nullptr ? GetRefValue(sr->env, sr->tcp->wrapper_ref) : nullptr;
  napi_value argv[3] = {MakeInt32(sr->env, status), tcp_obj, req_obj};
  InvokeReqOnComplete(sr->env, req_obj, status, argv, 3);
  if (sr->req_obj_ref != nullptr) napi_delete_reference(sr->env, sr->req_obj_ref);
  delete sr;
}

void OnConnectDone(uv_connect_t* req, int status) {
  auto* cr = static_cast<ConnectReqWrap*>(req->data);
  if (cr == nullptr) return;
  napi_value req_obj = GetRefValue(cr->env, cr->req_obj_ref);
  napi_value tcp_obj = cr->tcp != nullptr ? GetRefValue(cr->env, cr->tcp->wrapper_ref) : nullptr;
  napi_value argv[5] = {
      MakeInt32(cr->env, status),
      tcp_obj,
      req_obj,
      MakeBool(cr->env, true),
      MakeBool(cr->env, true),
  };
  InvokeReqOnComplete(cr->env, req_obj, status, argv, 5);
  if (cr->req_obj_ref != nullptr) napi_delete_reference(cr->env, cr->req_obj_ref);
  delete cr;
}

void OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size, uv_buf_t* buf) {
  char* base = static_cast<char*>(malloc(suggested_size));
  *buf = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
}

void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  auto* wrap = static_cast<TcpWrap*>(stream->data);
  if (wrap == nullptr) {
    if (buf && buf->base) free(buf->base);
    return;
  }
  SetState(kUnodeReadBytesOrError, static_cast<int32_t>(nread));
  SetState(kUnodeArrayBufferOffset, 0);
  if (nread > 0) wrap->bytes_read += static_cast<uint64_t>(nread);

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = nullptr;
  if (self != nullptr && napi_get_named_property(wrap->env, self, "onread", &onread) == napi_ok && onread != nullptr) {
    napi_valuetype t = napi_undefined;
    napi_typeof(wrap->env, onread, &t);
    if (t == napi_function) {
      napi_value argv[1] = {nullptr};
      if (nread > 0 && buf != nullptr && buf->base != nullptr) {
        void* out = nullptr;
        napi_value ab = nullptr;
        if (napi_create_arraybuffer(wrap->env, nread, &out, &ab) == napi_ok && out != nullptr && ab != nullptr) {
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

  // Safety net: if JS-side teardown is skipped on EOF/error, close the handle
  // to avoid accumulating half-closed sockets under sustained load.
  if (nread < 0 && !wrap->closed) {
    uv_handle_t* h = reinterpret_cast<uv_handle_t*>(&wrap->handle);
    (void)uv_read_stop(stream);
    if (!uv_is_closing(h)) {
      uv_close(h, OnClosed);
    }
  }

  if (buf && buf->base) free(buf->base);
}

void OnClosed(uv_handle_t* h) {
  auto* wrap = static_cast<TcpWrap*>(h->data);
  if (wrap == nullptr) return;
  wrap->closed = true;
  if (!wrap->finalized && wrap->close_cb_ref != nullptr) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetRefValue(wrap->env, wrap->close_cb_ref);
    if (cb != nullptr) {
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

void TcpFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<TcpWrap*>(data);
  if (wrap == nullptr) return;
  wrap->finalized = true;
  if (wrap->wrapper_ref != nullptr) {
    napi_delete_reference(env, wrap->wrapper_ref);
    wrap->wrapper_ref = nullptr;
  }
  if (wrap->close_cb_ref != nullptr) {
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

napi_value TcpCtor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  auto* wrap = new TcpWrap();
  wrap->env = env;
  wrap->async_id = g_next_async_id++;
  uv_tcp_init(uv_default_loop(), &wrap->handle);
  wrap->initialized = true;
  wrap->handle.data = wrap;
  napi_wrap(env, self, wrap, TcpFinalize, nullptr, &wrap->wrapper_ref);
  const char* mutable_methods[] = {"setNoDelay", "setKeepAlive", "ref", "unref"};
  for (const char* key : mutable_methods) {
    napi_value fn = nullptr;
    if (napi_get_named_property(env, self, key, &fn) == napi_ok && fn != nullptr) {
      napi_property_descriptor desc = {key, nullptr, nullptr, nullptr, nullptr, fn,
                                       static_cast<napi_property_attributes>(napi_writable | napi_configurable),
                                       nullptr};
      napi_define_properties(env, self, 1, &desc);
    }
  }
  return self;
}

bool ExtractByteSpan(napi_env env, napi_value v, const uint8_t** data, size_t* len, std::string* temp_utf8) {
  bool is_typed = false;
  napi_is_typedarray(env, v, &is_typed);
  if (is_typed) {
    napi_typedarray_type ta_type;
    size_t length = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t offset = 0;
    if (napi_get_typedarray_info(env, v, &ta_type, &length, &raw, &ab, &offset) == napi_ok && raw != nullptr) {
      *data = static_cast<const uint8_t*>(raw);
      *len = length;
      return true;
    }
  }
  *temp_utf8 = ValueToUtf8(env, v);
  *data = reinterpret_cast<const uint8_t*>(temp_utf8->data());
  *len = temp_utf8->size();
  return true;
}

napi_value TcpWriteBufferLike(napi_env env, TcpWrap* wrap, napi_value req_obj, const uint8_t* data, size_t len) {
  auto* wr = new WriteReqWrap();
  wr->env = env;
  napi_create_reference(env, req_obj, 1, &wr->req_obj_ref);
  wr->nbufs = 1;
  wr->bufs = new uv_buf_t[1];
  char* copy = static_cast<char*>(malloc(len));
  if (len > 0 && copy != nullptr && data != nullptr) memcpy(copy, data, len);
  wr->bufs[0] = uv_buf_init(copy, static_cast<unsigned int>(len));
  wr->req.data = wr;

  wrap->bytes_written += len;
  SetState(kUnodeBytesWritten, static_cast<int32_t>(len));
  SetState(kUnodeLastWriteWasAsync, 1);

  int rc = uv_write(&wr->req, reinterpret_cast<uv_stream_t*>(&wrap->handle), wr->bufs, 1, OnWriteDone);
  if (rc != 0) {
    SetReqError(env, req_obj, rc);
    FreeWriteReq(wr);
  }
  return MakeInt32(env, rc);
}

napi_value TcpWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  const uint8_t* data = nullptr;
  size_t len = 0;
  std::string temp;
  ExtractByteSpan(env, argv[1], &data, &len, &temp);
  return TcpWriteBufferLike(env, wrap, argv[0], data, len);
}

napi_value TcpWriteString(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  std::string s = ValueToUtf8(env, argv[1]);
  return TcpWriteBufferLike(env, wrap, argv[0], reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

napi_value TcpWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  napi_value req_obj = argv[0];
  napi_value chunks = argv[1];
  bool all_buffers = false;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &all_buffers);

  uint32_t n = 0;
  napi_get_array_length(env, chunks, &n);
  auto* wr = new WriteReqWrap();
  wr->env = env;
  napi_create_reference(env, req_obj, 1, &wr->req_obj_ref);
  wr->bufs = new uv_buf_t[n > 0 ? n : 1];
  wr->nbufs = n;
  size_t total = 0;

  for (uint32_t i = 0; i < n; i++) {
    napi_value chunk = nullptr;
    if (all_buffers) {
      napi_get_element(env, chunks, i, &chunk);
    } else {
      napi_get_element(env, chunks, i * 2, &chunk);
    }
    const uint8_t* data = nullptr;
    size_t len = 0;
    std::string temp;
    ExtractByteSpan(env, chunk, &data, &len, &temp);
    char* copy = static_cast<char*>(malloc(len));
    if (len > 0 && copy != nullptr && data != nullptr) memcpy(copy, data, len);
    wr->bufs[i] = uv_buf_init(copy, static_cast<unsigned int>(len));
    total += len;
  }

  wr->req.data = wr;
  wrap->bytes_written += total;
  SetState(kUnodeBytesWritten, static_cast<int32_t>(total));
  SetState(kUnodeLastWriteWasAsync, 1);

  int rc = uv_write(&wr->req,
                    reinterpret_cast<uv_stream_t*>(&wrap->handle),
                    wr->bufs,
                    wr->nbufs,
                    OnWriteDone);
  if (rc != 0) {
    SetReqError(env, req_obj, rc);
    FreeWriteReq(wr);
  }
  return MakeInt32(env, rc);
}

napi_value TcpReadStart(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int rc = uv_read_start(reinterpret_cast<uv_stream_t*>(&wrap->handle), OnAlloc, OnRead);
  napi_value self = GetRefValue(env, wrap->wrapper_ref);
  if (self != nullptr) {
    napi_set_named_property(env, self, "reading", MakeBool(env, rc == 0));
  }
  return MakeInt32(env, rc);
}

napi_value TcpReadStop(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int rc = uv_read_stop(reinterpret_cast<uv_stream_t*>(&wrap->handle));
  napi_value self = GetRefValue(env, wrap->wrapper_ref);
  if (self != nullptr) napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return MakeInt32(env, rc);
}

napi_value TcpClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr) {
    napi_value u = nullptr;
    napi_get_undefined(env, &u);
    return u;
  }
  if (argc > 0 && argv[0] != nullptr) {
    napi_valuetype t = napi_undefined;
    napi_typeof(env, argv[0], &t);
    if (t == napi_function) {
      if (wrap->close_cb_ref != nullptr) napi_delete_reference(env, wrap->close_cb_ref);
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

napi_value TcpOpen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t fd = -1;
  napi_get_value_int32(env, argv[0], &fd);
  int rc = uv_tcp_open(&wrap->handle, static_cast<uv_os_sock_t>(fd));
  return MakeInt32(env, rc);
}

napi_value TcpSetBlocking(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  int rc = uv_stream_set_blocking(reinterpret_cast<uv_stream_t*>(&wrap->handle), on ? 1 : 0);
  return MakeInt32(env, rc);
}

napi_value TcpBind(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  std::string host = ValueToUtf8(env, argv[0]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  sockaddr_in addr{};
  int rc = uv_ip4_addr(host.c_str(), port, &addr);
  if (rc != 0) return MakeInt32(env, rc);
  rc = uv_tcp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&addr), 0);
  return MakeInt32(env, rc);
}

napi_value TcpBind6(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  std::string host = ValueToUtf8(env, argv[0]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  unsigned int flags = 0;
  if (argc > 2 && argv[2] != nullptr) {
    uint32_t tmp = 0;
    napi_get_value_uint32(env, argv[2], &tmp);
    flags = tmp;
  }
  sockaddr_in6 addr{};
  int rc = uv_ip6_addr(host.c_str(), port, &addr);
  if (rc != 0) return MakeInt32(env, rc);
  rc = uv_tcp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&addr), flags);
  return MakeInt32(env, rc);
}

void FillSockAddr(napi_env env, napi_value out, const sockaddr* sa) {
  char ip[INET6_ADDRSTRLEN] = {0};
  const char* family = "IPv4";
  int port = 0;
  if (sa->sa_family == AF_INET6) {
    family = "IPv6";
    const auto* a6 = reinterpret_cast<const sockaddr_in6*>(sa);
    uv_ip6_name(a6, ip, sizeof(ip));
    port = ntohs(a6->sin6_port);
  } else {
    const auto* a4 = reinterpret_cast<const sockaddr_in*>(sa);
    uv_ip4_name(a4, ip, sizeof(ip));
    port = ntohs(a4->sin_port);
  }
  napi_value addr_v = nullptr;
  napi_value fam_v = nullptr;
  napi_value port_v = nullptr;
  napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &addr_v);
  napi_create_string_utf8(env, family, NAPI_AUTO_LENGTH, &fam_v);
  napi_create_int32(env, port, &port_v);
  if (addr_v) napi_set_named_property(env, out, "address", addr_v);
  if (fam_v) napi_set_named_property(env, out, "family", fam_v);
  if (port_v) napi_set_named_property(env, out, "port", port_v);
}

napi_value TcpGetSockName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  sockaddr_storage storage{};
  int len = sizeof(storage);
  int rc = uv_tcp_getsockname(&wrap->handle, reinterpret_cast<sockaddr*>(&storage), &len);
  if (rc == 0) FillSockAddr(env, argv[0], reinterpret_cast<sockaddr*>(&storage));
  return MakeInt32(env, rc);
}

napi_value TcpGetPeerName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  sockaddr_storage storage{};
  int len = sizeof(storage);
  int rc = uv_tcp_getpeername(&wrap->handle, reinterpret_cast<sockaddr*>(&storage), &len);
  if (rc == 0) FillSockAddr(env, argv[0], reinterpret_cast<sockaddr*>(&storage));
  return MakeInt32(env, rc);
}

void OnConnection(uv_stream_t* server, int status) {
  auto* server_wrap = static_cast<TcpWrap*>(server->data);
  if (server_wrap == nullptr) return;
  napi_env env = server_wrap->env;
  napi_value server_obj = GetRefValue(env, server_wrap->wrapper_ref);
  napi_value onconnection = nullptr;
  if (server_obj == nullptr ||
      napi_get_named_property(env, server_obj, "onconnection", &onconnection) != napi_ok ||
      onconnection == nullptr) {
    return;
  }
  napi_valuetype t = napi_undefined;
  napi_typeof(env, onconnection, &t);
  if (t != napi_function) return;

  napi_value undef = nullptr;
  napi_get_undefined(env, &undef);
  napi_value argv[2] = {MakeInt32(env, status), undef};
  if (status == 0) {
    napi_value ctor = GetRefValue(env, g_tcp_ctor_ref);
    if (ctor == nullptr) return;
    napi_value arg0 = MakeInt32(env, kTcpSocket);
    napi_value client_obj = nullptr;
    napi_new_instance(env, ctor, 1, &arg0, &client_obj);
    TcpWrap* client_wrap = nullptr;
    if (client_obj == nullptr ||
        napi_unwrap(env, client_obj, reinterpret_cast<void**>(&client_wrap)) != napi_ok ||
        client_wrap == nullptr) {
      return;
    }
    // Match Node: if uv_accept fails (e.g. EAGAIN), abort this callback path.
    if (uv_accept(server, reinterpret_cast<uv_stream_t*>(&client_wrap->handle)) != 0) {
      return;
    }
    argv[1] = client_obj;
  }
  napi_value ignored = nullptr;
  UnodeMakeCallback(env, server_obj, onconnection, 2, argv, &ignored);
}

napi_value TcpListen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int32_t backlog = 511;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &backlog);
  int rc = uv_listen(reinterpret_cast<uv_stream_t*>(&wrap->handle), backlog, OnConnection);
  return MakeInt32(env, rc);
}

napi_value TcpConnectImpl(napi_env env, TcpWrap* wrap, napi_value req_obj, const std::string& host, int32_t port, bool ipv6) {
  auto* cr = new ConnectReqWrap();
  cr->env = env;
  cr->tcp = wrap;
  cr->req.data = cr;
  napi_create_reference(env, req_obj, 1, &cr->req_obj_ref);

  int rc = 0;
  if (port >= 0) {
    if (ipv6) {
      sockaddr_in6 addr6{};
      rc = uv_ip6_addr(host.c_str(), port, &addr6);
      if (rc == 0) {
        rc = uv_tcp_connect(&cr->req,
                            &wrap->handle,
                            reinterpret_cast<const sockaddr*>(&addr6),
                            OnConnectDone);
      }
    } else {
      sockaddr_in addr4{};
      rc = uv_ip4_addr(host.c_str(), port, &addr4);
      if (rc == 0) {
        rc = uv_tcp_connect(&cr->req,
                            &wrap->handle,
                            reinterpret_cast<const sockaddr*>(&addr4),
                            OnConnectDone);
      }
    }
  } else {
    rc = UV_EINVAL;
  }
  if (rc != 0) {
    SetReqError(env, req_obj, rc);
    if (cr->req_obj_ref != nullptr) napi_delete_reference(env, cr->req_obj_ref);
    delete cr;
  }
  return MakeInt32(env, rc);
}

napi_value TcpConnect(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  napi_value req_obj = argv[0];
  std::string host = ValueToUtf8(env, argv[1]);
  int32_t port = -1;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_int32(env, argv[2], &port);
  return TcpConnectImpl(env, wrap, req_obj, host, port, false);
}

napi_value TcpConnect6(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 3) return MakeInt32(env, UV_EINVAL);
  napi_value req_obj = argv[0];
  std::string host = ValueToUtf8(env, argv[1]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[2], &port);
  return TcpConnectImpl(env, wrap, req_obj, host, port, true);
}

napi_value TcpShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  auto* sr = new ShutdownReqWrap();
  sr->env = env;
  sr->tcp = wrap;
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

napi_value TcpSetNoDelay(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  int rc = uv_tcp_nodelay(&wrap->handle, on ? 1 : 0);
  return MakeInt32(env, rc);
}

napi_value TcpSetKeepAlive(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  TcpWrap* wrap = nullptr;
  GetThis(env, info, &argc, argv, &wrap);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  int32_t delay = 0;
  napi_get_value_bool(env, argv[0], &on);
  napi_get_value_int32(env, argv[1], &delay);
  int rc = uv_tcp_keepalive(&wrap->handle, on ? 1 : 0, static_cast<unsigned int>(delay));
  return MakeInt32(env, rc);
}

napi_value TcpRef(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap != nullptr) uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value TcpUnref(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap != nullptr) uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value TcpGetAsyncId(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  napi_value out = nullptr;
  napi_create_int64(env, wrap != nullptr ? wrap->async_id : -1, &out);
  return out;
}

napi_value TcpGetProviderType(napi_env env, napi_callback_info info) {
  (void)info;
  return MakeInt32(env, kTcpSocket);
}

napi_value TcpAsyncReset(napi_env env, napi_callback_info info) {
  TcpWrap* wrap = nullptr;
  GetThis(env, info, nullptr, nullptr, &wrap);
  if (wrap != nullptr) wrap->async_id = g_next_async_id++;
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value TcpReset(napi_env env, napi_callback_info info) {
  return TcpClose(env, info);
}

napi_value TcpFchmod(napi_env env, napi_callback_info info) {
  return MakeInt32(env, UV_ENOTSUP);
}

napi_value TcpBytesReadGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TcpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(wrap != nullptr ? wrap->bytes_read : 0), &out);
  return out;
}

napi_value TcpBytesWrittenGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TcpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(wrap != nullptr ? wrap->bytes_written : 0), &out);
  return out;
}

napi_value TcpFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TcpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  int32_t fd = -1;
  if (wrap != nullptr) {
    uv_os_fd_t raw = -1;
    if (uv_fileno(reinterpret_cast<const uv_handle_t*>(&wrap->handle), &raw) == 0) {
      fd = static_cast<int32_t>(raw);
    }
  }
  if (fd >= 0) {
    napi_value global = nullptr;
    if (napi_get_global(env, &global) == napi_ok && global != nullptr) {
      napi_value map = nullptr;
      bool has = false;
      if (napi_has_named_property(env, global, "__unode_fd_types", &has) == napi_ok && has) {
        napi_get_named_property(env, global, "__unode_fd_types", &map);
      } else {
        napi_create_object(env, &map);
        if (map != nullptr) napi_set_named_property(env, global, "__unode_fd_types", map);
      }
      if (map != nullptr) {
        napi_value type_v = nullptr;
        napi_create_string_utf8(env, "TCP", NAPI_AUTO_LENGTH, &type_v);
        if (type_v != nullptr) {
          std::string key = std::to_string(fd);
          napi_set_named_property(env, map, key.c_str(), type_v);
        }
      }
    }
  }
  return MakeInt32(env, fd);
}

napi_value ConnectWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  return self;
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  if (v != nullptr) napi_set_named_property(env, obj, key, v);
}

}  // namespace

void UnodeInstallTcpWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;

  napi_property_descriptor tcp_props[] = {
      {"open", nullptr, TcpOpen, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setBlocking", nullptr, TcpSetBlocking, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bind", nullptr, TcpBind, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bind6", nullptr, TcpBind6, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"listen", nullptr, TcpListen, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"connect", nullptr, TcpConnect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"connect6", nullptr, TcpConnect6, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"shutdown", nullptr, TcpShutdown, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"close", nullptr, TcpClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readStart", nullptr, TcpReadStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"readStop", nullptr, TcpReadStop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeBuffer", nullptr, TcpWriteBuffer, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writev", nullptr, TcpWritev, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeLatin1String", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeUtf8String", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeAsciiString", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"writeUcs2String", nullptr, TcpWriteString, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setNoDelay", nullptr, TcpSetNoDelay, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"setKeepAlive", nullptr, TcpSetKeepAlive, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"getsockname", nullptr, TcpGetSockName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getpeername", nullptr, TcpGetPeerName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, TcpRef, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"unref", nullptr, TcpUnref, nullptr, nullptr, nullptr,
       static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"getAsyncId", nullptr, TcpGetAsyncId, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getProviderType", nullptr, TcpGetProviderType, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"asyncReset", nullptr, TcpAsyncReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"reset", nullptr, TcpReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"fchmod", nullptr, TcpFchmod, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bytesRead", nullptr, nullptr, TcpBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, TcpBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, TcpFdGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value tcp_ctor = nullptr;
  if (napi_define_class(env,
                        "TCP",
                        NAPI_AUTO_LENGTH,
                        TcpCtor,
                        nullptr,
                        sizeof(tcp_props) / sizeof(tcp_props[0]),
                        tcp_props,
                        &tcp_ctor) != napi_ok ||
      tcp_ctor == nullptr) {
    return;
  }
  if (g_tcp_ctor_ref != nullptr) {
    napi_delete_reference(env, g_tcp_ctor_ref);
    g_tcp_ctor_ref = nullptr;
  }
  napi_create_reference(env, tcp_ctor, 1, &g_tcp_ctor_ref);

  napi_value connect_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "TCPConnectWrap",
                        NAPI_AUTO_LENGTH,
                        ConnectWrapCtor,
                        nullptr,
                        0,
                        nullptr,
                        &connect_wrap_ctor) != napi_ok ||
      connect_wrap_ctor == nullptr) {
    return;
  }
  if (g_connect_wrap_ctor_ref != nullptr) {
    napi_delete_reference(env, g_connect_wrap_ctor_ref);
    g_connect_wrap_ctor_ref = nullptr;
  }
  napi_create_reference(env, connect_wrap_ctor, 1, &g_connect_wrap_ctor_ref);

  napi_value constants = nullptr;
  napi_create_object(env, &constants);
  SetNamedU32(env, constants, "SOCKET", kTcpSocket);
  SetNamedU32(env, constants, "SERVER", kTcpServer);
#ifdef UV_TCP_IPV6ONLY
  SetNamedU32(env, constants, "UV_TCP_IPV6ONLY", UV_TCP_IPV6ONLY);
#else
  SetNamedU32(env, constants, "UV_TCP_IPV6ONLY", 0);
#endif
#ifdef UV_TCP_REUSEPORT
  SetNamedU32(env, constants, "UV_TCP_REUSEPORT", UV_TCP_REUSEPORT);
#else
  SetNamedU32(env, constants, "UV_TCP_REUSEPORT", 0);
#endif

  napi_set_named_property(env, binding, "TCP", tcp_ctor);
  napi_set_named_property(env, binding, "TCPConnectWrap", connect_wrap_ctor);
  napi_set_named_property(env, binding, "constants", constants);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_set_named_property(env, global, "__unode_tcp_wrap", binding);
}
