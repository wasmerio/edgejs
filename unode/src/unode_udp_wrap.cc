#include "unode_udp_wrap.h"

#include <arpa/inet.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <uv.h>

#include "unode_runtime.h"

namespace {

struct UdpWrap;

struct UdpSendReqWrap {
  uv_udp_send_t req{};
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  uv_buf_t* bufs = nullptr;
  uint32_t nbufs = 0;
  UdpWrap* udp = nullptr;
};

struct UdpWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref close_cb_ref = nullptr;
  uv_udp_t handle{};
  bool closed = false;
  bool finalized = false;
  bool delete_on_close = false;
  bool has_ref = true;
  int64_t async_id = 200000;
};

struct SendWrap {
  napi_ref wrapper_ref = nullptr;
};

napi_ref g_udp_ctor_ref = nullptr;
int64_t g_next_async_id = 200000;

void OnClosed(uv_handle_t* h);

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value v = nullptr;
  if (napi_get_reference_value(env, ref, &v) != napi_ok) return nullptr;
  return v;
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

void SetReqError(napi_env env, napi_value req_obj, int status) {
  if (req_obj == nullptr || status >= 0) return;
  const char* err = uv_err_name(status);
  napi_value err_v = nullptr;
  napi_create_string_utf8(env, err != nullptr ? err : "UV_ERROR", NAPI_AUTO_LENGTH, &err_v);
  if (err_v != nullptr) napi_set_named_property(env, req_obj, "error", err_v);
}

void SendWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<SendWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->wrapper_ref) napi_delete_reference(env, wrap->wrapper_ref);
  delete wrap;
}

void UdpFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<UdpWrap*>(data);
  if (wrap == nullptr) return;
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

napi_value SendWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* wrap = new SendWrap();
  napi_wrap(env, self, wrap, SendWrapFinalize, nullptr, &wrap->wrapper_ref);
  return self;
}

napi_value UdpCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* wrap = new UdpWrap();
  wrap->env = env;
  wrap->async_id = g_next_async_id++;
  uv_udp_init(uv_default_loop(), &wrap->handle);
  wrap->handle.data = wrap;
  napi_wrap(env, self, wrap, UdpFinalize, nullptr, &wrap->wrapper_ref);
  // Node's internal/dgram mutates selected handle methods for udp6 aliases.
  const char* mutable_methods[] = {"bind", "bind6", "connect", "connect6", "send", "send6"};
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

void InvokeReqOnComplete(napi_env env, napi_value req_obj, int status) {
  if (req_obj == nullptr) return;
  SetReqError(env, req_obj, status);
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || oncomplete == nullptr) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, oncomplete, &t);
  if (t != napi_function) return;
  napi_value argv[1] = {MakeInt32(env, status)};
  napi_value ignored = nullptr;
  UnodeMakeCallback(env, req_obj, oncomplete, 1, argv, &ignored);
}

void OnSendDone(uv_udp_send_t* req, int status) {
  auto* sr = static_cast<UdpSendReqWrap*>(req->data);
  if (sr == nullptr) return;
  napi_value req_obj = GetRefValue(sr->env, sr->req_obj_ref);
  InvokeReqOnComplete(sr->env, req_obj, status);
  if (sr->bufs != nullptr) {
    for (uint32_t i = 0; i < sr->nbufs; i++) free(sr->bufs[i].base);
    delete[] sr->bufs;
  }
  if (sr->req_obj_ref) napi_delete_reference(sr->env, sr->req_obj_ref);
  delete sr;
}

void OnAlloc(uv_handle_t* /*h*/, size_t suggested_size, uv_buf_t* buf) {
  char* base = static_cast<char*>(malloc(suggested_size));
  *buf = uv_buf_init(base, static_cast<unsigned int>(suggested_size));
}

napi_value MakeBufferFromBytes(napi_env env, const char* data, size_t len) {
  void* out = nullptr;
  napi_value ab = nullptr;
  if (napi_create_arraybuffer(env, len, &out, &ab) != napi_ok || out == nullptr || ab == nullptr) return nullptr;
  if (len > 0) memcpy(out, data, len);
  napi_value view = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, len, ab, 0, &view) != napi_ok || view == nullptr) return nullptr;

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return view;
  napi_value buffer_ctor = nullptr;
  if (napi_get_named_property(env, global, "Buffer", &buffer_ctor) != napi_ok || buffer_ctor == nullptr) return view;
  napi_value from_fn = nullptr;
  if (napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok || from_fn == nullptr) return view;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, from_fn, &t);
  if (t != napi_function) return view;
  napi_value argv[1] = {view};
  napi_value buf_obj = nullptr;
  if (napi_call_function(env, buffer_ctor, from_fn, 1, argv, &buf_obj) != napi_ok || buf_obj == nullptr) return view;
  return buf_obj;
}

void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const sockaddr* addr, unsigned /*flags*/) {
  auto* wrap = static_cast<UdpWrap*>(handle->data);
  if (wrap == nullptr) {
    if (buf && buf->base) free(buf->base);
    return;
  }
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onmessage = nullptr;
  if (self != nullptr &&
      napi_get_named_property(wrap->env, self, "onmessage", &onmessage) == napi_ok &&
      onmessage != nullptr) {
    napi_valuetype t = napi_undefined;
    napi_typeof(wrap->env, onmessage, &t);
    if (t == napi_function) {
      napi_value argv[4] = {MakeInt32(wrap->env, static_cast<int32_t>(nread)), self, nullptr, nullptr};
      if (nread > 0 && buf != nullptr && buf->base != nullptr) {
        argv[2] = MakeBufferFromBytes(wrap->env, buf->base, static_cast<size_t>(nread));
      } else {
        napi_get_undefined(wrap->env, &argv[2]);
      }
      napi_value rinfo = nullptr;
      napi_create_object(wrap->env, &rinfo);
      if (addr != nullptr) {
        char ip[INET6_ADDRSTRLEN] = {0};
        int port = 0;
        const char* fam = "IPv4";
        if (addr->sa_family == AF_INET6) {
          auto* a6 = reinterpret_cast<const sockaddr_in6*>(addr);
          uv_ip6_name(a6, ip, sizeof(ip));
          port = ntohs(a6->sin6_port);
          fam = "IPv6";
        } else {
          auto* a4 = reinterpret_cast<const sockaddr_in*>(addr);
          uv_ip4_name(a4, ip, sizeof(ip));
          port = ntohs(a4->sin_port);
        }
        napi_value ip_v = nullptr;
        napi_value fam_v = nullptr;
        napi_value port_v = nullptr;
        napi_create_string_utf8(wrap->env, ip, NAPI_AUTO_LENGTH, &ip_v);
        napi_create_string_utf8(wrap->env, fam, NAPI_AUTO_LENGTH, &fam_v);
        napi_create_int32(wrap->env, port, &port_v);
        if (ip_v) napi_set_named_property(wrap->env, rinfo, "address", ip_v);
        if (fam_v) napi_set_named_property(wrap->env, rinfo, "family", fam_v);
        if (port_v) napi_set_named_property(wrap->env, rinfo, "port", port_v);
      }
      argv[3] = rinfo;
      napi_value ignored = nullptr;
      UnodeMakeCallback(wrap->env, self, onmessage, 4, argv, &ignored);
    }
  }
  if (buf && buf->base) free(buf->base);
}

void OnClosed(uv_handle_t* h) {
  auto* wrap = static_cast<UdpWrap*>(h->data);
  if (wrap == nullptr) return;
  wrap->closed = true;
  if (!wrap->finalized && wrap->close_cb_ref) {
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

napi_value UdpBindImpl(napi_env env, UdpWrap* wrap, napi_value ip_val, int32_t port, bool ipv6, uint32_t flags) {
  std::string ip = ValueToUtf8(env, ip_val);
  int rc = 0;
  if (ipv6) {
    sockaddr_in6 a6{};
    rc = uv_ip6_addr(ip.c_str(), port, &a6);
    if (rc == 0) rc = uv_udp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&a6), flags);
  } else {
    sockaddr_in a4{};
    rc = uv_ip4_addr(ip.c_str(), port, &a4);
    if (rc == 0) rc = uv_udp_bind(&wrap->handle, reinterpret_cast<const sockaddr*>(&a4), flags);
  }
  return MakeInt32(env, rc);
}

napi_value UdpBind(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  uint32_t flags = 0;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_uint32(env, argv[2], &flags);
  return UdpBindImpl(env, wrap, argv[0], port, false, flags);
}

napi_value UdpBind6(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  uint32_t flags = 0;
  if (argc > 2 && argv[2] != nullptr) napi_get_value_uint32(env, argv[2], &flags);
  return UdpBindImpl(env, wrap, argv[0], port, true, flags);
}

napi_value UdpOpen(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t fd = -1;
  napi_get_value_int32(env, argv[0], &fd);
  int rc = uv_udp_open(&wrap->handle, static_cast<uv_os_sock_t>(fd));
  return MakeInt32(env, rc);
}

napi_value UdpRecvStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int rc = uv_udp_recv_start(&wrap->handle, OnAlloc, OnRecv);
  return MakeInt32(env, rc);
}

napi_value UdpRecvStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  int rc = uv_udp_recv_stop(&wrap->handle);
  return MakeInt32(env, rc);
}

napi_value UdpClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) {
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

napi_value UdpSend(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 3) return MakeInt32(env, UV_EINVAL);

  napi_value req_obj = argv[0];
  napi_value list = argv[1];
  uint32_t list_len = 0;
  napi_get_array_length(env, list, &list_len);

  auto* sr = new UdpSendReqWrap();
  sr->env = env;
  sr->udp = wrap;
  sr->req.data = sr;
  sr->nbufs = list_len;
  sr->bufs = new uv_buf_t[list_len > 0 ? list_len : 1];
  napi_create_reference(env, req_obj, 1, &sr->req_obj_ref);

  for (uint32_t i = 0; i < list_len; i++) {
    napi_value chunk = nullptr;
    napi_get_element(env, list, i, &chunk);
    bool is_typed = false;
    napi_is_typedarray(env, chunk, &is_typed);
    const char* src = nullptr;
    size_t len = 0;
    std::string tmp;
    if (is_typed) {
      napi_typedarray_type tt;
      void* data = nullptr;
      napi_value ab;
      size_t off;
      napi_get_typedarray_info(env, chunk, &tt, &len, &data, &ab, &off);
      src = static_cast<const char*>(data);
    } else {
      tmp = ValueToUtf8(env, chunk);
      src = tmp.data();
      len = tmp.size();
    }
    char* copy = static_cast<char*>(malloc(len));
    if (len > 0 && src != nullptr) memcpy(copy, src, len);
    sr->bufs[i] = uv_buf_init(copy, static_cast<unsigned int>(len));
  }

  int rc = 0;
  if (argc >= 5 && argv[3] != nullptr && argv[4] != nullptr) {
    int32_t port = 0;
    napi_get_value_int32(env, argv[3], &port);
    std::string ip = ValueToUtf8(env, argv[4]);
    sockaddr_storage ss{};
    if (ip.find(':') != std::string::npos) {
      auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
      uv_ip6_addr(ip.c_str(), port, a6);
      rc = uv_udp_send(&sr->req, &wrap->handle, sr->bufs, sr->nbufs, reinterpret_cast<const sockaddr*>(a6), OnSendDone);
    } else {
      auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
      uv_ip4_addr(ip.c_str(), port, a4);
      rc = uv_udp_send(&sr->req, &wrap->handle, sr->bufs, sr->nbufs, reinterpret_cast<const sockaddr*>(a4), OnSendDone);
    }
  } else {
    rc = uv_udp_send(&sr->req, &wrap->handle, sr->bufs, sr->nbufs, nullptr, OnSendDone);
  }
  if (rc != 0) {
    SetReqError(env, req_obj, rc);
    for (uint32_t i = 0; i < sr->nbufs; i++) free(sr->bufs[i].base);
    delete[] sr->bufs;
    napi_delete_reference(env, sr->req_obj_ref);
    delete sr;
  }
  return MakeInt32(env, rc);
}

napi_value UdpSend6(napi_env env, napi_callback_info info) {
  return UdpSend(env, info);
}

napi_value UdpGetSockName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  sockaddr_storage ss{};
  int len = sizeof(ss);
  int rc = uv_udp_getsockname(&wrap->handle, reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc == 0) {
    char ip[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    const char* fam = "IPv4";
    if (ss.ss_family == AF_INET6) {
      auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
      uv_ip6_name(a6, ip, sizeof(ip));
      port = ntohs(a6->sin6_port);
      fam = "IPv6";
    } else {
      auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
      uv_ip4_name(a4, ip, sizeof(ip));
      port = ntohs(a4->sin_port);
    }
    napi_value ip_v = nullptr;
    napi_value fam_v = nullptr;
    napi_value port_v = nullptr;
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &ip_v);
    napi_create_string_utf8(env, fam, NAPI_AUTO_LENGTH, &fam_v);
    napi_create_int32(env, port, &port_v);
    if (ip_v) napi_set_named_property(env, argv[0], "address", ip_v);
    if (fam_v) napi_set_named_property(env, argv[0], "family", fam_v);
    if (port_v) napi_set_named_property(env, argv[0], "port", port_v);
  }
  return MakeInt32(env, rc);
}

napi_value UdpRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap != nullptr) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
    wrap->has_ref = true;
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value UdpUnref(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap != nullptr) {
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->handle));
    wrap->has_ref = false;
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value UdpHasRef(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  return MakeBool(env, wrap != nullptr ? wrap->has_ref : false);
}

napi_value UdpGetAsyncId(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  napi_value out = nullptr;
  napi_create_int64(env, wrap != nullptr ? wrap->async_id : -1, &out);
  return out;
}

napi_value UdpFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
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
        napi_create_string_utf8(env, "UDP", NAPI_AUTO_LENGTH, &type_v);
        if (type_v != nullptr) {
          std::string key = std::to_string(fd);
          napi_set_named_property(env, map, key.c_str(), type_v);
        }
      }
    }
  }
  return MakeInt32(env, fd);
}

napi_value UdpNoop0(napi_env env, napi_callback_info info) {
  return MakeInt32(env, 0);
}

napi_value UdpSetBroadcast(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  return MakeInt32(env, uv_udp_set_broadcast(&wrap->handle, on ? 1 : 0));
}

napi_value UdpSetTTL(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t ttl = 0;
  napi_get_value_int32(env, argv[0], &ttl);
  return MakeInt32(env, uv_udp_set_ttl(&wrap->handle, ttl));
}

napi_value UdpSetMulticastTTL(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  int32_t ttl = 0;
  napi_get_value_int32(env, argv[0], &ttl);
  return MakeInt32(env, uv_udp_set_multicast_ttl(&wrap->handle, ttl));
}

napi_value UdpSetMulticastLoopback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  bool on = false;
  napi_get_value_bool(env, argv[0], &on);
  return MakeInt32(env, uv_udp_set_multicast_loop(&wrap->handle, on ? 1 : 0));
}

napi_value UdpSetMulticastInterface(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  std::string iface = ValueToUtf8(env, argv[0]);
  return MakeInt32(env, uv_udp_set_multicast_interface(&wrap->handle, iface.c_str()));
}

napi_value UdpMembershipImpl(napi_env env, napi_callback_info info, uv_membership membership) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1 || argv[0] == nullptr) return MakeInt32(env, UV_EINVAL);
  std::string multicast = ValueToUtf8(env, argv[0]);
  const char* iface = nullptr;
  std::string iface_storage;
  if (argc > 1 && argv[1] != nullptr) {
    iface_storage = ValueToUtf8(env, argv[1]);
    iface = iface_storage.c_str();
  }
  return MakeInt32(env, uv_udp_set_membership(&wrap->handle, multicast.c_str(), iface, membership));
}

napi_value UdpAddMembership(napi_env env, napi_callback_info info) {
  return UdpMembershipImpl(env, info, UV_JOIN_GROUP);
}

napi_value UdpDropMembership(napi_env env, napi_callback_info info) {
  return UdpMembershipImpl(env, info, UV_LEAVE_GROUP);
}

napi_value UdpSourceMembershipImpl(napi_env env,
                                   napi_callback_info info,
                                   uv_membership membership) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2 || argv[0] == nullptr || argv[1] == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 32)
  std::string source = ValueToUtf8(env, argv[0]);
  std::string group = ValueToUtf8(env, argv[1]);
  const char* iface = nullptr;
  std::string iface_storage;
  if (argc > 2 && argv[2] != nullptr) {
    iface_storage = ValueToUtf8(env, argv[2]);
    iface = iface_storage.c_str();
  }
  return MakeInt32(env,
                   uv_udp_set_source_membership(
                       &wrap->handle, group.c_str(), iface, source.c_str(), membership));
#else
  return MakeInt32(env, UV_ENOTSUP);
#endif
}

napi_value UdpAddSourceSpecificMembership(napi_env env, napi_callback_info info) {
  return UdpSourceMembershipImpl(env, info, UV_JOIN_GROUP);
}

napi_value UdpDropSourceSpecificMembership(napi_env env, napi_callback_info info) {
  return UdpSourceMembershipImpl(env, info, UV_LEAVE_GROUP);
}

napi_value UdpConnectImpl(napi_env env, napi_callback_info info, bool ipv6) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  std::string host = ValueToUtf8(env, argv[0]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[1], &port);
  if (ipv6) {
    sockaddr_in6 a6{};
    int rc = uv_ip6_addr(host.c_str(), port, &a6);
    if (rc != 0) return MakeInt32(env, rc);
    return MakeInt32(env, uv_udp_connect(&wrap->handle, reinterpret_cast<const sockaddr*>(&a6)));
  }
  sockaddr_in a4{};
  int rc = uv_ip4_addr(host.c_str(), port, &a4);
  if (rc != 0) return MakeInt32(env, rc);
  return MakeInt32(env, uv_udp_connect(&wrap->handle, reinterpret_cast<const sockaddr*>(&a4)));
}

napi_value UdpConnect(napi_env env, napi_callback_info info) {
  return UdpConnectImpl(env, info, false);
}

napi_value UdpConnect6(napi_env env, napi_callback_info info) {
  return UdpConnectImpl(env, info, true);
}

napi_value UdpDisconnect(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  return MakeInt32(env, uv_udp_connect(&wrap->handle, nullptr));
}

napi_value UdpGetPeerName(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  sockaddr_storage ss{};
  int len = sizeof(ss);
  int rc = uv_udp_getpeername(&wrap->handle, reinterpret_cast<sockaddr*>(&ss), &len);
  if (rc == 0) {
    char ip[INET6_ADDRSTRLEN] = {0};
    int port = 0;
    const char* fam = "IPv4";
    if (ss.ss_family == AF_INET6) {
      auto* a6 = reinterpret_cast<sockaddr_in6*>(&ss);
      uv_ip6_name(a6, ip, sizeof(ip));
      port = ntohs(a6->sin6_port);
      fam = "IPv6";
    } else {
      auto* a4 = reinterpret_cast<sockaddr_in*>(&ss);
      uv_ip4_name(a4, ip, sizeof(ip));
      port = ntohs(a4->sin_port);
    }
    napi_value ip_v = nullptr;
    napi_value fam_v = nullptr;
    napi_value port_v = nullptr;
    napi_create_string_utf8(env, ip, NAPI_AUTO_LENGTH, &ip_v);
    napi_create_string_utf8(env, fam, NAPI_AUTO_LENGTH, &fam_v);
    napi_create_int32(env, port, &port_v);
    if (ip_v) napi_set_named_property(env, argv[0], "address", ip_v);
    if (fam_v) napi_set_named_property(env, argv[0], "family", fam_v);
    if (port_v) napi_set_named_property(env, argv[0], "port", port_v);
  }
  return MakeInt32(env, rc);
}

napi_value UdpBufferSize(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  if (wrap == nullptr || argc < 2) {
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
  }
  int32_t size = 0;
  napi_get_value_int32(env, argv[0], &size);
  bool recv = false;
  napi_get_value_bool(env, argv[1], &recv);
  int value = size;
  const char* syscall = recv ? "uv_recv_buffer_size" : "uv_send_buffer_size";
  int rc = recv
      ? uv_recv_buffer_size(reinterpret_cast<uv_handle_t*>(&wrap->handle), &value)
      : uv_send_buffer_size(reinterpret_cast<uv_handle_t*>(&wrap->handle), &value);
  if (rc != 0) {
    if (argc > 2 && argv[2] != nullptr) {
      napi_value errno_v = nullptr;
      napi_create_int32(env, rc, &errno_v);
      if (errno_v) napi_set_named_property(env, argv[2], "errno", errno_v);
      const char* code = uv_err_name(rc);
      napi_value code_v = nullptr;
      napi_create_string_utf8(env, code ? code : "UV_ERROR", NAPI_AUTO_LENGTH, &code_v);
      if (code_v) napi_set_named_property(env, argv[2], "code", code_v);
      const char* msg = uv_strerror(rc);
      napi_value msg_v = nullptr;
      napi_create_string_utf8(env, msg ? msg : "buffer size error", NAPI_AUTO_LENGTH, &msg_v);
      if (msg_v) napi_set_named_property(env, argv[2], "message", msg_v);
      napi_value syscall_v = nullptr;
      napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &syscall_v);
      if (syscall_v) napi_set_named_property(env, argv[2], "syscall", syscall_v);
    }
    napi_value undef = nullptr;
    napi_get_undefined(env, &undef);
    return undef;
  }
  return MakeInt32(env, value);
}

napi_value UdpGetSendQueueSize(napi_env env, napi_callback_info info) {
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 19)
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  uint64_t value = wrap != nullptr ? uv_udp_get_send_queue_size(&wrap->handle) : 0;
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(value), &out);
  return out;
#else
  return MakeInt32(env, 0);
#endif
}

napi_value UdpGetSendQueueCount(napi_env env, napi_callback_info info) {
#if UV_VERSION_MAJOR > 1 || (UV_VERSION_MAJOR == 1 && UV_VERSION_MINOR >= 19)
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  UdpWrap* wrap = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&wrap));
  size_t value = wrap != nullptr ? uv_udp_get_send_queue_count(&wrap->handle) : 0;
  napi_value out = nullptr;
  napi_create_double(env, static_cast<double>(value), &out);
  return out;
#else
  return MakeInt32(env, 0);
#endif
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  if (v != nullptr) napi_set_named_property(env, obj, key, v);
}

}  // namespace

void UnodeInstallUdpWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;

  napi_property_descriptor udp_props[] = {
      {"open", nullptr, UdpOpen, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bind", nullptr, UdpBind, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bind6", nullptr, UdpBind6, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"send", nullptr, UdpSend, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"send6", nullptr, UdpSend6, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"recvStart", nullptr, UdpRecvStart, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"recvStop", nullptr, UdpRecvStop, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getsockname", nullptr, UdpGetSockName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getpeername", nullptr, UdpGetPeerName, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"close", nullptr, UdpClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setBroadcast", nullptr, UdpSetBroadcast, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setTTL", nullptr, UdpSetTTL, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setMulticastTTL", nullptr, UdpSetMulticastTTL, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setMulticastLoopback", nullptr, UdpSetMulticastLoopback, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setMulticastInterface", nullptr, UdpSetMulticastInterface, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"addMembership", nullptr, UdpAddMembership, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dropMembership", nullptr, UdpDropMembership, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"addSourceSpecificMembership", nullptr, UdpAddSourceSpecificMembership, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dropSourceSpecificMembership", nullptr, UdpDropSourceSpecificMembership, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setMulticastAll", nullptr, UdpNoop0, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"bufferSize", nullptr, UdpBufferSize, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setRecvBufferSize", nullptr, UdpNoop0, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setSendBufferSize", nullptr, UdpNoop0, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getRecvBufferSize", nullptr, UdpNoop0, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getSendBufferSize", nullptr, UdpNoop0, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"connect", nullptr, UdpConnect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"connect6", nullptr, UdpConnect6, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"disconnect", nullptr, UdpDisconnect, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, UdpRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unref", nullptr, UdpUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"hasRef", nullptr, UdpHasRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getAsyncId", nullptr, UdpGetAsyncId, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, UdpFdGetter, nullptr, nullptr, napi_default, nullptr},
      {"getSendQueueSize", nullptr, UdpGetSendQueueSize, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getSendQueueCount", nullptr, UdpGetSendQueueCount, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value udp_ctor = nullptr;
  if (napi_define_class(env,
                        "UDP",
                        NAPI_AUTO_LENGTH,
                        UdpCtor,
                        nullptr,
                        sizeof(udp_props) / sizeof(udp_props[0]),
                        udp_props,
                        &udp_ctor) != napi_ok ||
      udp_ctor == nullptr) {
    return;
  }
  if (g_udp_ctor_ref != nullptr) napi_delete_reference(env, g_udp_ctor_ref);
  napi_create_reference(env, udp_ctor, 1, &g_udp_ctor_ref);

  napi_value send_wrap_ctor = nullptr;
  if (napi_define_class(env, "SendWrap", NAPI_AUTO_LENGTH, SendWrapCtor, nullptr, 0, nullptr, &send_wrap_ctor) != napi_ok ||
      send_wrap_ctor == nullptr) {
    return;
  }

  napi_value constants = nullptr;
  napi_create_object(env, &constants);
#ifdef UV_UDP_IPV6ONLY
  SetNamedU32(env, constants, "UV_UDP_IPV6ONLY", UV_UDP_IPV6ONLY);
#else
  SetNamedU32(env, constants, "UV_UDP_IPV6ONLY", 0);
#endif
#ifdef UV_UDP_REUSEPORT
  SetNamedU32(env, constants, "UV_UDP_REUSEPORT", UV_UDP_REUSEPORT);
#else
  SetNamedU32(env, constants, "UV_UDP_REUSEPORT", 0);
#endif

  napi_set_named_property(env, binding, "UDP", udp_ctor);
  napi_set_named_property(env, binding, "SendWrap", send_wrap_ctor);
  napi_set_named_property(env, binding, "constants", constants);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_set_named_property(env, global, "__unode_udp_wrap", binding);
}
