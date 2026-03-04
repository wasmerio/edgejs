#include "ubi_cares_wrap.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <uv.h>

#include "ubi_runtime.h"

namespace {

constexpr int kDnsOrderVerbatim = 0;
constexpr int kDnsOrderIpv4First = 1;
constexpr int kDnsOrderIpv6First = 2;

struct CaresReqWrap {
  napi_env env = nullptr;
  napi_ref req_obj_ref = nullptr;
  uv_getaddrinfo_t ga{};
  uv_getnameinfo_t gn{};
  bool used_ga = false;
  bool used_gn = false;
  bool in_flight = false;
  bool pinned_ref = false;
  bool finalized = false;
  bool orphaned = false;
  std::string hostname;
};

struct ChannelWrap {
  napi_ref wrapper_ref = nullptr;
  std::vector<std::pair<std::string, int>> servers;
  std::string local_ipv4;
  std::string local_ipv6;
  int32_t timeout = -1;
  int32_t tries = 4;
  int32_t max_timeout = 0;
};

std::unordered_map<napi_env, std::unordered_set<CaresReqWrap*>> g_pending_reqs_by_env;
std::unordered_set<napi_env> g_cleanup_hook_registered;
std::unordered_map<napi_env, napi_ref> g_dns_fallback_module_refs;
void OnCaresEnvCleanup(void* arg);

void EnsureCaresCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnCaresEnvCleanup, env) != napi_ok) {
    g_cleanup_hook_registered.erase(it);
  }
}

void UntrackPendingReq(CaresReqWrap* req) {
  if (req == nullptr || req->env == nullptr) return;
  auto it = g_pending_reqs_by_env.find(req->env);
  if (it == g_pending_reqs_by_env.end()) return;
  it->second.erase(req);
  if (it->second.empty()) {
    g_pending_reqs_by_env.erase(it);
  }
}

void MarkReqComplete(CaresReqWrap* req) {
  if (req == nullptr) return;
  req->in_flight = false;
  req->used_ga = false;
  req->used_gn = false;
}

void OnCaresEnvCleanup(void* arg) {
  napi_env env = static_cast<napi_env>(arg);
  auto it = g_pending_reqs_by_env.find(env);
  if (it == g_pending_reqs_by_env.end()) {
    auto module_it = g_dns_fallback_module_refs.find(env);
    if (module_it != g_dns_fallback_module_refs.end()) {
      if (module_it->second != nullptr) {
        napi_delete_reference(env, module_it->second);
      }
      g_dns_fallback_module_refs.erase(module_it);
    }
    g_cleanup_hook_registered.erase(env);
    return;
  }

  for (CaresReqWrap* req : it->second) {
    if (req == nullptr) continue;
    req->orphaned = true;
    req->finalized = true;
    if (req->used_ga) {
      (void)uv_cancel(reinterpret_cast<uv_req_t*>(&req->ga));
    }
    if (req->used_gn) {
      (void)uv_cancel(reinterpret_cast<uv_req_t*>(&req->gn));
    }
    if (req->pinned_ref && req->req_obj_ref != nullptr) {
      uint32_t ref_count = 0;
      (void)napi_reference_unref(env, req->req_obj_ref, &ref_count);
      req->pinned_ref = false;
    }
    req->env = nullptr;
  }

  g_pending_reqs_by_env.erase(it);
  auto module_it = g_dns_fallback_module_refs.find(env);
  if (module_it != g_dns_fallback_module_refs.end()) {
    if (module_it->second != nullptr) {
      napi_delete_reference(env, module_it->second);
    }
    g_dns_fallback_module_refs.erase(module_it);
  }
  g_cleanup_hook_registered.erase(env);
}

void TrackPendingReq(napi_env env, CaresReqWrap* req) {
  if (env == nullptr || req == nullptr) return;
  EnsureCaresCleanupHook(env);
  g_pending_reqs_by_env[env].insert(req);
}

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

napi_value MakeInt32(napi_env env, int32_t v) {
  napi_value out = nullptr;
  napi_create_int32(env, v, &out);
  return out;
}

void ReqFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* req = static_cast<CaresReqWrap*>(data);
  if (!req) return;
  req->finalized = true;
  if (req->pinned_ref && req->req_obj_ref != nullptr) {
    uint32_t ref_count = 0;
    (void)napi_reference_unref(env, req->req_obj_ref, &ref_count);
    req->pinned_ref = false;
  }
  if (req->req_obj_ref) {
    napi_delete_reference(env, req->req_obj_ref);
    req->req_obj_ref = nullptr;
  }
  if (req->in_flight) {
    req->orphaned = true;
    req->env = nullptr;
    return;
  }
  delete req;
}

void ChannelFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* ch = static_cast<ChannelWrap*>(data);
  if (!ch) return;
  if (ch->wrapper_ref) napi_delete_reference(env, ch->wrapper_ref);
  delete ch;
}

napi_value ReqCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* req = new CaresReqWrap();
  req->env = env;
  napi_wrap(env, self, req, ReqFinalize, nullptr, &req->req_obj_ref);
  return self;
}

napi_value ChannelCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  auto* ch = new ChannelWrap();
  ch->servers.emplace_back("127.0.0.1", 53);
  if (argc >= 1 && argv[0] != nullptr) {
    (void)napi_get_value_int32(env, argv[0], &ch->timeout);
  }
  if (argc >= 2 && argv[1] != nullptr) {
    (void)napi_get_value_int32(env, argv[1], &ch->tries);
  }
  if (argc >= 3 && argv[2] != nullptr) {
    (void)napi_get_value_int32(env, argv[2], &ch->max_timeout);
  }
  EnsureCaresCleanupHook(env);
  napi_wrap(env, self, ch, ChannelFinalize, nullptr, &ch->wrapper_ref);
  return self;
}

void InvokeOnComplete(napi_env env, napi_value req_obj, size_t argc, napi_value* argv) {
  if (!req_obj) return;
  napi_value oncomplete = nullptr;
  if (napi_get_named_property(env, req_obj, "oncomplete", &oncomplete) != napi_ok || oncomplete == nullptr) return;
  napi_valuetype t = napi_undefined;
  napi_typeof(env, oncomplete, &t);
  if (t != napi_function) return;
  napi_value ignored = nullptr;
  UbiMakeCallback(env, req_obj, oncomplete, argc, argv, &ignored);
}

void OnGetAddrInfo(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
  auto* r = static_cast<CaresReqWrap*>(req->data);
  if (r == nullptr) return;
  napi_env env = r->env;
  UntrackPendingReq(r);
  MarkReqComplete(r);
  if (env == nullptr) {
    if (res) uv_freeaddrinfo(res);
    if (r->orphaned || r->finalized) delete r;
    return;
  }
  napi_value req_obj = GetRefValue(env, r->req_obj_ref);
  napi_value status_v = MakeInt32(env, status);

  std::vector<std::string> addrs;
  if (status == 0) {
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
      char host[NI_MAXHOST] = {0};
      if (getnameinfo(p->ai_addr, p->ai_addrlen, host, sizeof(host), nullptr, 0, NI_NUMERICHOST) == 0) {
        addrs.emplace_back(host);
      }
    }
  }
  napi_value arr = nullptr;
  napi_create_array_with_length(env, addrs.size(), &arr);
  for (uint32_t i = 0; i < addrs.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, addrs[i].c_str(), addrs[i].size(), &s);
    if (s) napi_set_element(env, arr, i, s);
  }
  napi_value argv[2] = {status_v, arr};
  InvokeOnComplete(env, req_obj, 2, argv);
  if (r->pinned_ref && r->req_obj_ref != nullptr) {
    uint32_t ref_count = 0;
    (void)napi_reference_unref(env, r->req_obj_ref, &ref_count);
    r->pinned_ref = false;
  }
  if (res) uv_freeaddrinfo(res);
  if (r->orphaned || r->finalized) delete r;
}

void OnGetNameInfo(uv_getnameinfo_t* req, int status, const char* hostname, const char* service) {
  auto* r = static_cast<CaresReqWrap*>(req->data);
  if (r == nullptr) return;
  napi_env env = r->env;
  UntrackPendingReq(r);
  MarkReqComplete(r);
  if (env == nullptr) {
    if (r->orphaned || r->finalized) delete r;
    return;
  }
  napi_value req_obj = GetRefValue(env, r->req_obj_ref);
  napi_value status_v = MakeInt32(env, status);
  napi_value host_v = nullptr;
  napi_value service_v = nullptr;
  napi_create_string_utf8(env, hostname ? hostname : "", NAPI_AUTO_LENGTH, &host_v);
  napi_create_string_utf8(env, service ? service : "", NAPI_AUTO_LENGTH, &service_v);
  napi_value argv[3] = {status_v, host_v, service_v};
  InvokeOnComplete(env, req_obj, 3, argv);
  if (r->pinned_ref && r->req_obj_ref != nullptr) {
    uint32_t ref_count = 0;
    (void)napi_reference_unref(env, r->req_obj_ref, &ref_count);
    r->pinned_ref = false;
  }
  if (r->orphaned || r->finalized) delete r;
}

napi_value CaresGetAddrInfo(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  if (argc < 4) return MakeInt32(env, UV_EINVAL);
  CaresReqWrap* req = nullptr;
  napi_unwrap(env, argv[0], reinterpret_cast<void**>(&req));
  if (!req) return MakeInt32(env, UV_EINVAL);
  if (req->in_flight) return MakeInt32(env, UV_EBUSY);
  req->used_ga = true;
  req->ga.data = req;
  req->hostname = ValueToUtf8(env, argv[1]);
  req->env = env;
  req->in_flight = true;
  req->orphaned = false;
  if (req->req_obj_ref != nullptr) {
    uint32_t ref_count = 0;
    if (napi_reference_ref(env, req->req_obj_ref, &ref_count) == napi_ok) {
      req->pinned_ref = true;
    }
  }
  TrackPendingReq(env, req);
  int32_t family = 0;
  int32_t hints_bits = 0;
  napi_get_value_int32(env, argv[2], &family);
  napi_get_value_int32(env, argv[3], &hints_bits);

  addrinfo hints{};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = family == 4 ? AF_INET : (family == 6 ? AF_INET6 : AF_UNSPEC);
  hints.ai_flags = hints_bits;
  int rc = uv_getaddrinfo(uv_default_loop(), &req->ga, OnGetAddrInfo, req->hostname.c_str(), nullptr, &hints);
  if (rc != 0) {
    UntrackPendingReq(req);
    MarkReqComplete(req);
    if (req->pinned_ref && req->req_obj_ref != nullptr) {
      uint32_t ref_count = 0;
      (void)napi_reference_unref(env, req->req_obj_ref, &ref_count);
      req->pinned_ref = false;
    }
  }
  return MakeInt32(env, rc);
}

napi_value CaresGetNameInfo(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  if (argc < 3) return MakeInt32(env, UV_EINVAL);
  CaresReqWrap* req = nullptr;
  napi_unwrap(env, argv[0], reinterpret_cast<void**>(&req));
  if (!req) return MakeInt32(env, UV_EINVAL);
  if (req->in_flight) return MakeInt32(env, UV_EBUSY);
  req->used_gn = true;
  req->gn.data = req;
  req->env = env;
  req->in_flight = true;
  req->orphaned = false;
  if (req->req_obj_ref != nullptr) {
    uint32_t ref_count = 0;
    if (napi_reference_ref(env, req->req_obj_ref, &ref_count) == napi_ok) {
      req->pinned_ref = true;
    }
  }
  TrackPendingReq(env, req);
  std::string host = ValueToUtf8(env, argv[1]);
  int32_t port = 0;
  napi_get_value_int32(env, argv[2], &port);

  sockaddr_storage storage{};
  int rc = 0;
  if (host.find(':') != std::string::npos) {
    auto* a6 = reinterpret_cast<sockaddr_in6*>(&storage);
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(port);
    rc = uv_inet_pton(AF_INET6, host.c_str(), &a6->sin6_addr);
  } else {
    auto* a4 = reinterpret_cast<sockaddr_in*>(&storage);
    a4->sin_family = AF_INET;
    a4->sin_port = htons(port);
    rc = uv_inet_pton(AF_INET, host.c_str(), &a4->sin_addr);
  }
  if (rc != 0) return MakeInt32(env, rc);
  rc = uv_getnameinfo(uv_default_loop(),
                      &req->gn,
                      OnGetNameInfo,
                      reinterpret_cast<const sockaddr*>(&storage),
                      NI_NAMEREQD);
  if (rc != 0) {
    UntrackPendingReq(req);
    MarkReqComplete(req);
    if (req->pinned_ref && req->req_obj_ref != nullptr) {
      uint32_t ref_count = 0;
      (void)napi_reference_unref(env, req->req_obj_ref, &ref_count);
      req->pinned_ref = false;
    }
  }
  return MakeInt32(env, rc);
}

napi_value ConvertIpv6StringToBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napi_value n = nullptr;
    napi_get_null(env, &n);
    return n;
  }
  std::string input = ValueToUtf8(env, argv[0]);
  uint8_t out[16] = {0};
  if (uv_inet_pton(AF_INET6, input.c_str(), out) != 0) {
    napi_value n = nullptr;
    napi_get_null(env, &n);
    return n;
  }
  void* data = nullptr;
  napi_value ab = nullptr;
  napi_create_arraybuffer(env, 16, &data, &ab);
  if (data == nullptr || ab == nullptr) {
    napi_value n = nullptr;
    napi_get_null(env, &n);
    return n;
  }
  memcpy(data, out, 16);
  napi_value arr = nullptr;
  napi_create_typedarray(env, napi_uint8_array, 16, ab, 0, &arr);
  return arr;
}

napi_value CanonicalizeIP(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  std::string input = argc > 0 ? ValueToUtf8(env, argv[0]) : std::string();
  if (!input.empty()) {
    in_addr a4{};
    if (uv_inet_pton(AF_INET, input.c_str(), &a4) == 0) {
      char out4[INET_ADDRSTRLEN] = {0};
      uv_inet_ntop(AF_INET, &a4, out4, sizeof(out4));
      napi_value out = nullptr;
      napi_create_string_utf8(env, out4, NAPI_AUTO_LENGTH, &out);
      return out;
    }
    in6_addr a6{};
    if (uv_inet_pton(AF_INET6, input.c_str(), &a6) == 0) {
      char out6[INET6_ADDRSTRLEN] = {0};
      uv_inet_ntop(AF_INET6, &a6, out6, sizeof(out6));
      napi_value out = nullptr;
      napi_create_string_utf8(env, out6, NAPI_AUTO_LENGTH, &out);
      return out;
    }
  }
  napi_value out = nullptr;
  napi_create_string_utf8(env, "", NAPI_AUTO_LENGTH, &out);
  return out;
}

napi_value CaresStrError(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t code = 0;
  if (argc > 0) napi_get_value_int32(env, argv[0], &code);
  const char* msg = nullptr;
  if (code == 1) {
    msg = "There are pending queries.";
  } else {
    msg = uv_strerror(code);
  }
  napi_value out = nullptr;
  napi_create_string_utf8(env, msg ? msg : "Unknown error", NAPI_AUTO_LENGTH, &out);
  return out;
}

bool GetDnsFallbackModule(napi_env env, napi_value* module_out) {
  if (module_out == nullptr) return false;
  *module_out = nullptr;
  EnsureCaresCleanupHook(env);

  auto it = g_dns_fallback_module_refs.find(env);
  if (it != g_dns_fallback_module_refs.end() && it->second != nullptr) {
    if (napi_get_reference_value(env, it->second, module_out) == napi_ok && *module_out != nullptr) {
      return true;
    }
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return false;

  napi_value require_fn = nullptr;
  if (napi_get_named_property(env, global, "__ubi_bootstrap_require", &require_fn) != napi_ok ||
      require_fn == nullptr) {
    if (napi_get_named_property(env, global, "require", &require_fn) != napi_ok || require_fn == nullptr) {
      return false;
    }
  }
  napi_valuetype require_type = napi_undefined;
  if (napi_typeof(env, require_fn, &require_type) != napi_ok || require_type != napi_function) return false;

  napi_value specifier = nullptr;
  napi_create_string_utf8(env, "internal/dns/fallback_resolver", NAPI_AUTO_LENGTH, &specifier);
  napi_value argv[1] = {specifier};
  napi_value module = nullptr;
  if (napi_call_function(env, global, require_fn, 1, argv, &module) != napi_ok || module == nullptr) {
    bool has_pending = false;
    if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
      napi_value ignored = nullptr;
      (void)napi_get_and_clear_last_exception(env, &ignored);
    }
    return false;
  }

  napi_ref module_ref = nullptr;
  if (napi_create_reference(env, module, 1, &module_ref) != napi_ok || module_ref == nullptr) return false;
  auto old_it = g_dns_fallback_module_refs.find(env);
  if (old_it != g_dns_fallback_module_refs.end() && old_it->second != nullptr) {
    napi_delete_reference(env, old_it->second);
  }
  g_dns_fallback_module_refs[env] = module_ref;
  *module_out = module;
  return true;
}

bool HasPendingFallbackQueries(napi_env env, napi_value self) {
  napi_value module = nullptr;
  if (!GetDnsFallbackModule(env, &module) || module == nullptr) return false;
  napi_value method = nullptr;
  if (napi_get_named_property(env, module, "hasNativeQueries", &method) != napi_ok || method == nullptr) {
    return false;
  }
  napi_valuetype method_type = napi_undefined;
  if (napi_typeof(env, method, &method_type) != napi_ok || method_type != napi_function) return false;
  napi_value argv[1] = {self};
  napi_value result = nullptr;
  if (napi_call_function(env, module, method, 1, argv, &result) != napi_ok || result == nullptr) return false;
  bool has_pending = false;
  (void)napi_get_value_bool(env, result, &has_pending);
  return has_pending;
}

void CancelFallbackQueries(napi_env env, napi_value self) {
  napi_value module = nullptr;
  if (!GetDnsFallbackModule(env, &module) || module == nullptr) return;
  napi_value method = nullptr;
  if (napi_get_named_property(env, module, "cancelNativeQueries", &method) != napi_ok || method == nullptr) return;
  napi_valuetype method_type = napi_undefined;
  if (napi_typeof(env, method, &method_type) != napi_ok || method_type != napi_function) return;
  napi_value argv[1] = {self};
  napi_value ignored = nullptr;
  (void)napi_call_function(env, module, method, 1, argv, &ignored);
}

struct QueryMethodData {
  const char* binding_name;
};

napi_value ChannelCancel(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  CancelFallbackQueries(env, self);
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value ChannelGetServers(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  ChannelWrap* ch = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&ch));
  napi_value arr = nullptr;
  napi_create_array_with_length(env, ch ? ch->servers.size() : 0, &arr);
  if (ch != nullptr) {
    for (uint32_t i = 0; i < ch->servers.size(); i++) {
      napi_value pair = nullptr;
      napi_create_array_with_length(env, 2, &pair);
      napi_value host = nullptr;
      napi_value port = nullptr;
      napi_create_string_utf8(env, ch->servers[i].first.c_str(), NAPI_AUTO_LENGTH, &host);
      napi_create_int32(env, ch->servers[i].second, &port);
      if (host) napi_set_element(env, pair, 0, host);
      if (port) napi_set_element(env, pair, 1, port);
      napi_set_element(env, arr, i, pair);
    }
  }
  return arr;
}

napi_value ChannelSetServers(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  ChannelWrap* ch = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&ch));
  if (ch == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  if (HasPendingFallbackQueries(env, self)) return MakeInt32(env, 1);
  ch->servers.clear();
  uint32_t n = 0;
  napi_get_array_length(env, argv[0], &n);
  for (uint32_t i = 0; i < n; i++) {
    napi_value entry = nullptr;
    napi_get_element(env, argv[0], i, &entry);
    uint32_t en = 0;
    napi_get_array_length(env, entry, &en);
    if (en >= 3) {
      napi_value host_v = nullptr;
      napi_value port_v = nullptr;
      napi_get_element(env, entry, 1, &host_v);
      napi_get_element(env, entry, 2, &port_v);
      int32_t port = 53;
      napi_get_value_int32(env, port_v, &port);
      ch->servers.emplace_back(ValueToUtf8(env, host_v), port);
    } else if (en >= 2) {
      napi_value host_v = nullptr;
      napi_value port_v = nullptr;
      napi_get_element(env, entry, 0, &host_v);
      napi_get_element(env, entry, 1, &port_v);
      int32_t port = 53;
      napi_get_value_int32(env, port_v, &port);
      ch->servers.emplace_back(ValueToUtf8(env, host_v), port);
    }
  }
  return MakeInt32(env, 0);
}

napi_value ChannelSetLocalAddress(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  napi_value self = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, nullptr);
  ChannelWrap* ch = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&ch));
  if (ch != nullptr) {
    if (argc < 1 || argv[0] == nullptr) {
      napi_throw_error(env, nullptr, "Invalid IPv4 address");
      return nullptr;
    }
    const std::string first = ValueToUtf8(env, argv[0]);
    in_addr a4{};
    in6_addr a6_first{};
    const bool first_is_v4 = uv_inet_pton(AF_INET, first.c_str(), &a4) == 0;
    const bool first_is_v6 = uv_inet_pton(AF_INET6, first.c_str(), &a6_first) == 0;
    napi_valuetype second_type = napi_undefined;
    if (argc > 1 && argv[1] != nullptr) {
      napi_typeof(env, argv[1], &second_type);
    }
    if (argc > 1 && argv[1] != nullptr && second_type != napi_undefined && second_type != napi_null) {
      if (!first_is_v4) {
        napi_throw_error(env, nullptr, "Invalid IPv4 address");
        return nullptr;
      }
      ch->local_ipv4 = first;
      const std::string ipv6 = ValueToUtf8(env, argv[1]);
      if (!ipv6.empty()) {
        in6_addr a6{};
        if (uv_inet_pton(AF_INET6, ipv6.c_str(), &a6) != 0) {
          napi_throw_error(env, nullptr, "Invalid IPv6 address");
          return nullptr;
        }
      }
      ch->local_ipv6 = ipv6;
    } else {
      if (!first_is_v4 && !first_is_v6) {
        napi_throw_error(env, nullptr, "Invalid IP address");
        return nullptr;
      }
      if (first_is_v4) {
        ch->local_ipv4 = first;
        ch->local_ipv6.clear();
      } else {
        ch->local_ipv6 = first;
        ch->local_ipv4.clear();
      }
    }
  }
  napi_value u = nullptr;
  napi_get_undefined(env, &u);
  return u;
}

napi_value ChannelQueryViaNativeFallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value self = nullptr;
  void* callback_data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &self, &callback_data);
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) return MakeInt32(env, UV_EINVAL);

  ChannelWrap* ch = nullptr;
  napi_unwrap(env, self, reinterpret_cast<void**>(&ch));
  if (ch == nullptr) return MakeInt32(env, UV_EINVAL);

  const auto* method_data = static_cast<const QueryMethodData*>(callback_data);
  if (method_data == nullptr || method_data->binding_name == nullptr) return MakeInt32(env, UV_EINVAL);

  napi_value module = nullptr;
  if (!GetDnsFallbackModule(env, &module) || module == nullptr) return MakeInt32(env, UV_ENOSYS);
  napi_value start_query_fn = nullptr;
  if (napi_get_named_property(env, module, "startNativeQuery", &start_query_fn) != napi_ok ||
      start_query_fn == nullptr) {
    return MakeInt32(env, UV_ENOSYS);
  }
  napi_valuetype fn_type = napi_undefined;
  if (napi_typeof(env, start_query_fn, &fn_type) != napi_ok || fn_type != napi_function) {
    return MakeInt32(env, UV_ENOSYS);
  }

  bool ttl = false;
  napi_value ttl_value = nullptr;
  if (napi_get_named_property(env, argv[0], "ttl", &ttl_value) == napi_ok && ttl_value != nullptr) {
    (void)napi_get_value_bool(env, ttl_value, &ttl);
  }
  const std::string hostname = ValueToUtf8(env, argv[1]);

  napi_value binding_name = nullptr;
  napi_value hostname_value = nullptr;
  napi_value ttl_value_arg = nullptr;
  napi_value timeout_value = nullptr;
  napi_value tries_value = nullptr;
  napi_value max_timeout_value = nullptr;
  napi_create_string_utf8(env, method_data->binding_name, NAPI_AUTO_LENGTH, &binding_name);
  napi_create_string_utf8(env, hostname.c_str(), hostname.size(), &hostname_value);
  napi_get_boolean(env, ttl, &ttl_value_arg);
  napi_create_int32(env, ch->timeout, &timeout_value);
  napi_create_int32(env, ch->tries, &tries_value);
  napi_create_int32(env, ch->max_timeout, &max_timeout_value);

  napi_value call_argv[8] = {
      self,
      argv[0],
      binding_name,
      hostname_value,
      ttl_value_arg,
      timeout_value,
      tries_value,
      max_timeout_value,
  };
  napi_value call_result = nullptr;
  if (napi_call_function(env, module, start_query_fn, 8, call_argv, &call_result) != napi_ok || call_result == nullptr) {
    bool has_pending = false;
    if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
      napi_value ignored = nullptr;
      (void)napi_get_and_clear_last_exception(env, &ignored);
    }
    return MakeInt32(env, UV_EINVAL);
  }
  int32_t status = 0;
  if (napi_get_value_int32(env, call_result, &status) != napi_ok) status = 0;
  return MakeInt32(env, status);
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

void SetNamedU32(napi_env env, napi_value obj, const char* name, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  if (v) napi_set_named_property(env, obj, name, v);
}

constexpr QueryMethodData kQueryAnyMethod{"queryAny"};
constexpr QueryMethodData kQueryAMethod{"queryA"};
constexpr QueryMethodData kQueryAaaaMethod{"queryAaaa"};
constexpr QueryMethodData kQueryCaaMethod{"queryCaa"};
constexpr QueryMethodData kQueryCnameMethod{"queryCname"};
constexpr QueryMethodData kQueryMxMethod{"queryMx"};
constexpr QueryMethodData kQueryNsMethod{"queryNs"};
constexpr QueryMethodData kQueryTlsaMethod{"queryTlsa"};
constexpr QueryMethodData kQueryTxtMethod{"queryTxt"};
constexpr QueryMethodData kQuerySrvMethod{"querySrv"};
constexpr QueryMethodData kQueryPtrMethod{"queryPtr"};
constexpr QueryMethodData kQueryNaptrMethod{"queryNaptr"};
constexpr QueryMethodData kQuerySoaMethod{"querySoa"};
constexpr QueryMethodData kGetHostByAddrMethod{"getHostByAddr"};

}  // namespace

void UbiInstallCaresWrapBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;

  napi_value req_ctor = nullptr;
  if (napi_define_class(env, "QueryReqWrap", NAPI_AUTO_LENGTH, ReqCtor, nullptr, 0, nullptr, &req_ctor) != napi_ok) return;
  napi_value req2_ctor = nullptr;
  if (napi_define_class(env, "GetAddrInfoReqWrap", NAPI_AUTO_LENGTH, ReqCtor, nullptr, 0, nullptr, &req2_ctor) != napi_ok) return;
  napi_value req3_ctor = nullptr;
  if (napi_define_class(env, "GetNameInfoReqWrap", NAPI_AUTO_LENGTH, ReqCtor, nullptr, 0, nullptr, &req3_ctor) != napi_ok) return;

  napi_property_descriptor channel_props[] = {
      {"cancel", nullptr, ChannelCancel, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"getServers", nullptr, ChannelGetServers, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"setServers", nullptr, ChannelSetServers, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"setLocalAddress", nullptr, ChannelSetLocalAddress, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), nullptr},
      {"queryAny", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryAnyMethod)},
      {"queryA", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryAMethod)},
      {"queryAaaa", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryAaaaMethod)},
      {"queryCaa", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryCaaMethod)},
      {"queryCname", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryCnameMethod)},
      {"queryMx", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryMxMethod)},
      {"queryNs", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryNsMethod)},
      {"queryTlsa", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryTlsaMethod)},
      {"queryTxt", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryTxtMethod)},
      {"querySrv", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQuerySrvMethod)},
      {"queryPtr", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryPtrMethod)},
      {"queryNaptr", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQueryNaptrMethod)},
      {"querySoa", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kQuerySoaMethod)},
      {"getHostByAddr", nullptr, ChannelQueryViaNativeFallback, nullptr, nullptr, nullptr, static_cast<napi_property_attributes>(napi_writable | napi_configurable), const_cast<QueryMethodData*>(&kGetHostByAddrMethod)},
  };
  napi_value channel_ctor = nullptr;
  if (napi_define_class(env,
                        "ChannelWrap",
                        NAPI_AUTO_LENGTH,
                        ChannelCtor,
                        nullptr,
                        sizeof(channel_props) / sizeof(channel_props[0]),
                        channel_props,
                        &channel_ctor) != napi_ok) {
    return;
  }

  napi_set_named_property(env, binding, "QueryReqWrap", req_ctor);
  napi_set_named_property(env, binding, "GetAddrInfoReqWrap", req2_ctor);
  napi_set_named_property(env, binding, "GetNameInfoReqWrap", req3_ctor);
  napi_set_named_property(env, binding, "ChannelWrap", channel_ctor);

  SetMethod(env, binding, "getaddrinfo", CaresGetAddrInfo);
  SetMethod(env, binding, "getnameinfo", CaresGetNameInfo);
  SetMethod(env, binding, "strerror", CaresStrError);
  SetMethod(env, binding, "convertIpv6StringToBuffer", ConvertIpv6StringToBuffer);
  SetMethod(env, binding, "canonicalizeIP", CanonicalizeIP);

  SetNamedU32(env, binding, "AI_ADDRCONFIG", AI_ADDRCONFIG);
  SetNamedU32(env, binding, "AI_ALL", AI_ALL);
  SetNamedU32(env, binding, "AI_V4MAPPED", AI_V4MAPPED);

  SetNamedU32(env, binding, "DNS_ORDER_VERBATIM", kDnsOrderVerbatim);
  SetNamedU32(env, binding, "DNS_ORDER_IPV4_FIRST", kDnsOrderIpv4First);
  SetNamedU32(env, binding, "DNS_ORDER_IPV6_FIRST", kDnsOrderIpv6First);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_set_named_property(env, global, "__ubi_cares_wrap", binding);
}
