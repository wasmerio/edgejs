#include "ubi_tls_wrap.h"

#include <cstdint>
#include <cstring>
#include <deque>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <uv.h>

#include "crypto/ubi_secure_context_bridge.h"
#include "ncrypto.h"
#include "internal_binding/helpers.h"
#include "ubi_async_wrap.h"
#include "ubi_env_loop.h"
#include "ubi_handle_wrap.h"
#include "ubi_module_loader.h"
#include "ubi_runtime.h"
#include "ubi_stream_base.h"
#include "ubi_stream_wrap.h"

namespace {

struct PendingAppWrite {
  napi_ref req_ref = nullptr;
  std::vector<uint8_t> data;
};

struct PendingEncryptedWrite {
  std::vector<uint8_t> data;
  napi_ref completion_req_ref = nullptr;
  bool force_parent_turn = false;
};

struct KeepaliveHandle {
  uv_idle_t idle{};
};

struct TlsWrap;
using TlsCertCb = void (*)(void*);
TlsWrap* FindWrapBySelf(napi_env env, napi_value self);
void OnInternalWriteDone(TlsWrap* wrap, int status);
bool ScheduleReqCompletion(TlsWrap* wrap, napi_ref* req_ref, int status);
int32_t CallParentMethodInt(TlsWrap* wrap,
                            const char* method,
                            size_t argc,
                            napi_value* argv,
                            napi_value* result_out);
int TLSExtStatusCallback(SSL* ssl, void* arg);

struct ClientHelloData {
  const uint8_t* session_id = nullptr;
  uint8_t session_size = 0;
  const uint8_t* servername = nullptr;
  uint16_t servername_size = 0;
  bool has_ticket = false;
};

class ClientHelloParser {
 public:
  using OnHelloCb = void (*)(TlsWrap* wrap, const ClientHelloData& hello);

  void Start(OnHelloCb on_hello) {
    if (!IsEnded()) return;
    Reset();
    state_ = kWaiting;
    on_hello_ = on_hello;
  }

  void End() {
    state_ = kEnded;
  }

  bool IsEnded() const {
    return state_ == kEnded;
  }

  bool IsPaused() const {
    return state_ == kPaused;
  }

  void Parse(TlsWrap* wrap, const uint8_t* data, size_t avail) {
    if (wrap == nullptr || data == nullptr) return;
    switch (state_) {
      case kWaiting:
        if (!ParseRecordHeader(data, avail)) return;
        [[fallthrough]];
      case kTLSHeader:
        ParseHeader(wrap, data, avail);
        break;
      case kPaused:
      case kEnded:
        break;
    }
  }

 private:
  enum ParseState { kWaiting, kTLSHeader, kPaused, kEnded };

  static constexpr size_t kMaxTLSFrameLen = 16 * 1024 + 5;
  static constexpr uint8_t kChangeCipherSpec = 20;
  static constexpr uint8_t kAlert = 21;
  static constexpr uint8_t kHandshake = 22;
  static constexpr uint8_t kApplicationData = 23;
  static constexpr uint8_t kClientHello = 1;
  static constexpr uint16_t kServerName = 0;
  static constexpr uint16_t kTLSSessionTicket = 35;
  static constexpr uint8_t kServernameHostname = 0;

  void Reset() {
    state_ = kEnded;
    frame_len_ = 0;
    session_id_ = nullptr;
    session_size_ = 0;
    servername_ = nullptr;
    servername_size_ = 0;
    tls_ticket_ = nullptr;
    tls_ticket_size_ = static_cast<uint16_t>(-1);
    on_hello_ = nullptr;
  }

  bool ParseRecordHeader(const uint8_t* data, size_t avail) {
    if (avail < 5) return false;
    switch (data[0]) {
      case kChangeCipherSpec:
      case kAlert:
      case kHandshake:
      case kApplicationData:
        frame_len_ = (static_cast<size_t>(data[3]) << 8) + data[4];
        state_ = kTLSHeader;
        body_offset_ = 5;
        break;
      default:
        End();
        return false;
    }
    if (frame_len_ >= kMaxTLSFrameLen) {
      End();
      return false;
    }
    return true;
  }

  void ParseHeader(TlsWrap* wrap, const uint8_t* data, size_t avail) {
    if (frame_len_ < 6) {
      End();
      return;
    }
    if (body_offset_ + frame_len_ > avail) return;
    if (data[body_offset_ + 4] != 0x03 ||
        data[body_offset_ + 5] < 0x01 ||
        data[body_offset_ + 5] > 0x03) {
      End();
      return;
    }
    if (data[body_offset_] == kClientHello && !ParseTLSClientHello(data, avail)) {
      End();
      return;
    }
    if (session_id_ == nullptr ||
        session_size_ > 32 ||
        session_id_ + session_size_ > data + avail) {
      End();
      return;
    }
    state_ = kPaused;
    if (on_hello_ != nullptr) {
      ClientHelloData hello;
      hello.session_id = session_id_;
      hello.session_size = session_size_;
      hello.servername = servername_;
      hello.servername_size = servername_size_;
      hello.has_ticket = tls_ticket_ != nullptr && tls_ticket_size_ != 0;
      on_hello_(wrap, hello);
    }
  }

  void ParseExtension(uint16_t type, const uint8_t* data, size_t len) {
    switch (type) {
      case kServerName: {
        if (len < 2) return;
        const uint32_t server_names_len = (static_cast<uint32_t>(data[0]) << 8) + data[1];
        if (server_names_len + 2 > len) return;
        for (size_t offset = 2; offset < 2 + server_names_len;) {
          if (offset + 3 > len) return;
          if (data[offset] != kServernameHostname) return;
          const uint16_t name_len = (static_cast<uint16_t>(data[offset + 1]) << 8) + data[offset + 2];
          offset += 3;
          if (offset + name_len > len) return;
          servername_ = data + offset;
          servername_size_ = name_len;
          offset += name_len;
        }
        break;
      }
      case kTLSSessionTicket:
        tls_ticket_size_ = static_cast<uint16_t>(len);
        tls_ticket_ = data + len;
        break;
      default:
        break;
    }
  }

  bool ParseTLSClientHello(const uint8_t* data, size_t avail) {
    const size_t session_offset = body_offset_ + 4 + 2 + 32;
    if (session_offset + 1 >= avail) return false;
    session_size_ = data[session_offset];
    session_id_ = data + session_offset + 1;

    const size_t cipher_offset = session_offset + 1 + session_size_;
    if (cipher_offset + 1 >= avail) return false;
    const uint16_t cipher_len = (static_cast<uint16_t>(data[cipher_offset]) << 8) + data[cipher_offset + 1];

    const size_t comp_offset = cipher_offset + 2 + cipher_len;
    if (comp_offset >= avail) return false;
    const uint8_t comp_len = data[comp_offset];
    const size_t extension_offset = comp_offset + 1 + comp_len;
    if (extension_offset > avail) return false;
    if (extension_offset == avail) return true;

    size_t ext_off = extension_offset + 2;
    while (ext_off < avail) {
      if (ext_off + 4 > avail) return false;
      const uint16_t ext_type = (static_cast<uint16_t>(data[ext_off]) << 8) + data[ext_off + 1];
      const uint16_t ext_len = (static_cast<uint16_t>(data[ext_off + 2]) << 8) + data[ext_off + 3];
      ext_off += 4;
      if (ext_off + ext_len > avail) return false;
      ParseExtension(ext_type, data + ext_off, ext_len);
      ext_off += ext_len;
    }
    return ext_off <= avail;
  }

  ParseState state_ = kEnded;
  OnHelloCb on_hello_ = nullptr;
  size_t frame_len_ = 0;
  size_t body_offset_ = 0;
  const uint8_t* session_id_ = nullptr;
  uint8_t session_size_ = 0;
  const uint8_t* servername_ = nullptr;
  uint16_t servername_size_ = 0;
  uint16_t tls_ticket_size_ = static_cast<uint16_t>(-1);
  const uint8_t* tls_ticket_ = nullptr;
};

struct TlsWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref parent_ref = nullptr;
  napi_ref context_ref = nullptr;
  napi_ref pending_shutdown_req_ref = nullptr;
  napi_ref active_write_req_ref = nullptr;
  napi_ref user_read_buffer_ref = nullptr;
  bool is_server = false;
  bool started = false;
  bool established = false;
  bool eof = false;
  bool parent_write_in_progress = false;
  bool has_active_write_issued_by_prev_listener = false;
  bool waiting_cert_cb = false;
  bool cert_cb_running = false;
  TlsCertCb cert_cb = nullptr;
  void* cert_cb_arg = nullptr;
  bool alpn_callback_enabled = false;
  bool session_callbacks_enabled = false;
  bool awaiting_new_session = false;
  bool client_session_fallback_emitted = false;
  bool client_session_fallback_scheduled = false;
  uint32_t client_session_event_count = 0;
  bool pending_client_ocsp_event = false;
  bool client_ocsp_event_emitted = false;
  bool request_cert = false;
  bool reject_unauthorized = false;
  bool shutdown_started = false;
  bool write_callback_scheduled = false;
  bool defer_req_callbacks = false;
  bool handshake_done_emitted = false;
  bool handshake_done_pending = false;
  bool refed = true;
  bool keepalive_needed = false;
  int64_t async_id = 0;
  int cycle_depth = 0;
  SSL* ssl = nullptr;
  BIO* enc_in = nullptr;
  BIO* enc_out = nullptr;
  ubi::crypto::SecureContextHolder* secure_context = nullptr;
  std::deque<PendingAppWrite> pending_app_writes;
  std::deque<PendingEncryptedWrite> pending_encrypted_writes;
  std::vector<uint8_t> pending_session;
  std::vector<uint8_t> deferred_parent_input;
  std::vector<uint8_t> ocsp_response;
  std::vector<unsigned char> alpn_protos;
  ClientHelloParser hello_parser;
  SSL_SESSION* next_session = nullptr;
  KeepaliveHandle* keepalive = nullptr;
  char* user_buffer_base = nullptr;
  size_t user_buffer_len = 0;
};

struct TlsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref tls_wrap_ctor_ref = nullptr;
  int64_t next_async_id = 300000;
  std::vector<TlsWrap*> wraps;
};

struct ClientSessionFallbackTask {
  napi_ref self_ref = nullptr;
};

struct DeferredReqCompletionTask {
  napi_ref self_ref = nullptr;
  napi_ref req_ref = nullptr;
  int status = 0;
};

std::unordered_map<napi_env, TlsBindingState> g_tls_states;
std::unordered_set<napi_env> g_tls_cleanup_hook_registered;

napi_value Undefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

napi_value Null(napi_env env) {
  napi_value out = nullptr;
  napi_get_null(env, &out);
  return out;
}

napi_value MakeInt32(napi_env env, int32_t value) {
  napi_value out = nullptr;
  napi_create_int32(env, value, &out);
  return out;
}

napi_value MakeInt64(napi_env env, int64_t value) {
  napi_value out = nullptr;
  napi_create_int64(env, value, &out);
  return out;
}

napi_value MakeBool(napi_env env, bool value) {
  napi_value out = nullptr;
  napi_get_boolean(env, value, &out);
  return out;
}

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void KeepaliveNoop(uv_idle_t* /*handle*/) {}

void EnsureKeepaliveHandle(TlsWrap* wrap) {
  if (wrap == nullptr || !wrap->keepalive_needed || wrap->keepalive != nullptr || wrap->env == nullptr) return;
  uv_loop_t* loop = UbiGetEnvLoop(wrap->env);
  if (loop == nullptr) return;

  auto* keepalive = new KeepaliveHandle();
  if (uv_idle_init(loop, &keepalive->idle) != 0) {
    delete keepalive;
    return;
  }
  keepalive->idle.data = keepalive;
  if (uv_idle_start(&keepalive->idle, KeepaliveNoop) != 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&keepalive->idle),
             [](uv_handle_t* handle) {
               delete static_cast<KeepaliveHandle*>(handle->data);
             });
    return;
  }
  wrap->keepalive = keepalive;
}

void SyncKeepaliveRef(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->keepalive == nullptr) return;
  uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&wrap->keepalive->idle);
  if (wrap->refed) {
    uv_ref(handle);
  } else {
    uv_unref(handle);
  }
}

void ReleaseKeepaliveHandle(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->keepalive == nullptr) return;
  auto* keepalive = wrap->keepalive;
  wrap->keepalive = nullptr;
  uv_idle_stop(&keepalive->idle);
  uv_close(reinterpret_cast<uv_handle_t*>(&keepalive->idle),
           [](uv_handle_t* handle) {
             delete static_cast<KeepaliveHandle*>(handle->data);
           });
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok) return nullptr;
  return out;
}

napi_value GetNamedValue(napi_env env, napi_value obj, const char* key) {
  if (env == nullptr || obj == nullptr || key == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, obj, key, &out) != napi_ok) return nullptr;
  return out;
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

napi_value GetInternalBindingSymbol(napi_env env, const char* name) {
  napi_value symbols = ResolveInternalBinding(env, "symbols");
  if (symbols == nullptr || name == nullptr) return nullptr;
  return GetNamedValue(env, symbols, name);
}

napi_value GetPropertyBySymbol(napi_env env, napi_value obj, const char* symbol_name) {
  if (env == nullptr || obj == nullptr || symbol_name == nullptr) return nullptr;
  napi_value symbol = GetInternalBindingSymbol(env, symbol_name);
  if (symbol == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_property(env, obj, symbol, &out) != napi_ok) return nullptr;
  return out;
}

void InvokeReqWithStatus(TlsWrap* wrap, napi_ref* req_ref, int status) {
  if (wrap == nullptr || wrap->env == nullptr || req_ref == nullptr || *req_ref == nullptr) return;
  napi_value req_obj = GetRefValue(wrap->env, *req_ref);
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (req_obj != nullptr && status < 0) {
    UbiStreamBaseSetReqError(wrap->env, req_obj, status);
  }
  napi_value argv[3] = {
      MakeInt32(wrap->env, status),
      self != nullptr ? self : Undefined(wrap->env),
      status < 0 && req_obj != nullptr ? GetNamedValue(wrap->env, req_obj, "error") : Undefined(wrap->env),
  };
  if (req_obj != nullptr) {
    UbiStreamBaseInvokeReqOnComplete(wrap->env, req_obj, status, argv, 3);
  }
  DeleteRefIfPresent(wrap->env, req_ref);
}

bool IsFunction(napi_env env, napi_value value) {
  napi_valuetype type = napi_undefined;
  return value != nullptr && napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

bool GetArrayBufferViewSpan(napi_env env, napi_value value, const uint8_t** data, size_t* len) {
  static uint8_t kEmptySentinel = 0;
  if (env == nullptr || value == nullptr || data == nullptr || len == nullptr) return false;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* raw = nullptr;
    size_t byte_len = 0;
    if (napi_get_buffer_info(env, value, &raw, &byte_len) != napi_ok) return false;
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<const uint8_t*>(raw) : &kEmptySentinel;
    *len = byte_len;
    return true;
  }

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type ta_type = napi_uint8_array;
    size_t element_len = 0;
    void* raw = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(
            env, value, &ta_type, &element_len, &raw, &arraybuffer, &byte_offset) != napi_ok) {
      return false;
    }
    const size_t byte_len = element_len * UbiTypedArrayElementSize(ta_type);
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<const uint8_t*>(raw) : &kEmptySentinel;
    *len = byte_len;
    return true;
  }

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) {
    size_t byte_len = 0;
    void* raw = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_dataview_info(env, value, &byte_len, &raw, &arraybuffer, &byte_offset) != napi_ok) {
      return false;
    }
    if (raw == nullptr && byte_len != 0) return false;
    *data = raw != nullptr ? static_cast<const uint8_t*>(raw) : &kEmptySentinel;
    *len = byte_len;
    return true;
  }

  return false;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return "";
  size_t len = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &len) != napi_ok) return "";
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, out.data(), out.size(), &copied) != napi_ok) return "";
  out.resize(copied);
  return out;
}

void SetState(napi_env env, int idx, int32_t value) {
  int32_t* state = UbiGetStreamBaseState(env);
  if (state == nullptr) return;
  state[idx] = value;
}

TlsBindingState& EnsureState(napi_env env) {
  if (env != nullptr) {
    auto [it, inserted] = g_tls_cleanup_hook_registered.emplace(env);
    if (inserted && napi_add_env_cleanup_hook(
                        env,
                        [](void* data) {
                          napi_env cleanup_env = static_cast<napi_env>(data);
                          g_tls_cleanup_hook_registered.erase(cleanup_env);

                          auto state_it = g_tls_states.find(cleanup_env);
                          if (state_it == g_tls_states.end()) return;
                          for (TlsWrap* wrap : state_it->second.wraps) {
                            if (wrap == nullptr) continue;
                            DeleteRefIfPresent(cleanup_env, &wrap->wrapper_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->parent_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->context_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->pending_shutdown_req_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->active_write_req_ref);
                            DeleteRefIfPresent(cleanup_env, &wrap->user_read_buffer_ref);
                            for (auto& pending : wrap->pending_app_writes) {
                              DeleteRefIfPresent(cleanup_env, &pending.req_ref);
                            }
                            for (auto& pending : wrap->pending_encrypted_writes) {
                              DeleteRefIfPresent(cleanup_env, &pending.completion_req_ref);
                            }
                          }
                          DeleteRefIfPresent(cleanup_env, &state_it->second.binding_ref);
                          DeleteRefIfPresent(cleanup_env, &state_it->second.tls_wrap_ctor_ref);
                          state_it->second.wraps.clear();
                          g_tls_states.erase(state_it);
                        },
                        env) != napi_ok) {
      g_tls_cleanup_hook_registered.erase(it);
    }
  }
  return g_tls_states[env];
}

TlsWrap* UnwrapTlsWrap(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return nullptr;
  TlsWrap* wrap = nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok) return nullptr;
  return wrap;
}

TlsWrap* FindWrapByParent(napi_env env, napi_value parent) {
  auto it = g_tls_states.find(env);
  if (it == g_tls_states.end()) return nullptr;
  for (TlsWrap* wrap : it->second.wraps) {
    if (wrap == nullptr) continue;
    napi_value candidate = GetRefValue(env, wrap->parent_ref);
    bool same = false;
    if (candidate != nullptr && napi_strict_equals(env, candidate, parent, &same) == napi_ok && same) {
      return wrap;
    }
  }
  return nullptr;
}

TlsWrap* FindWrapBySelf(napi_env env, napi_value self) {
  auto it = g_tls_states.find(env);
  if (it == g_tls_states.end()) return nullptr;
  for (TlsWrap* wrap : it->second.wraps) {
    if (wrap == nullptr) continue;
    napi_value candidate = GetRefValue(env, wrap->wrapper_ref);
    bool same = false;
    if (candidate != nullptr && napi_strict_equals(env, candidate, self, &same) == napi_ok && same) {
      return wrap;
    }
  }
  return nullptr;
}

TlsWrap* UnwrapThis(napi_env env,
                    napi_callback_info info,
                    size_t* argc,
                    napi_value* argv,
                    napi_value* self_out) {
  size_t local_argc = argc != nullptr ? *argc : 0;
  napi_value self = nullptr;
  if (napi_get_cb_info(env, info, &local_argc, argv, &self, nullptr) != napi_ok) return nullptr;
  if (argc != nullptr) *argc = local_argc;
  if (self_out != nullptr) *self_out = self;
  return UnwrapTlsWrap(env, self);
}

void RemoveWrapFromState(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  auto it = g_tls_states.find(wrap->env);
  if (it == g_tls_states.end()) return;
  auto& wraps = it->second.wraps;
  for (auto vec_it = wraps.begin(); vec_it != wraps.end(); ++vec_it) {
    if (*vec_it == wrap) {
      wraps.erase(vec_it);
      break;
    }
  }
}

std::vector<uint8_t> ReadAllPendingBio(BIO* bio) {
  std::vector<uint8_t> out;
  if (bio == nullptr) return out;
  const size_t pending = static_cast<size_t>(BIO_ctrl_pending(bio));
  if (pending == 0) return out;
  out.resize(pending);
  const int read = BIO_read(bio, out.data(), static_cast<int>(pending));
  if (read <= 0) {
    out.clear();
    return out;
  }
  out.resize(static_cast<size_t>(read));
  return out;
}

napi_value CreateBufferCopy(napi_env env, const uint8_t* data, size_t len) {
  napi_value out = nullptr;
  void* copied = nullptr;
  if (napi_create_buffer_copy(env, len, len > 0 ? data : nullptr, &copied, &out) != napi_ok) return nullptr;
  return out;
}

void EmitOnReadData(TlsWrap* wrap, const uint8_t* data, size_t len) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = GetNamedValue(wrap->env, self, "onread");
  if (!IsFunction(wrap->env, onread)) return;
  if (wrap->user_buffer_base != nullptr && wrap->user_buffer_len != 0) {
    size_t offset = 0;
    while (offset < len) {
      const size_t chunk_len = std::min(wrap->user_buffer_len, len - offset);
      std::memcpy(wrap->user_buffer_base, data + offset, chunk_len);
      SetState(wrap->env, kUbiReadBytesOrError, static_cast<int32_t>(chunk_len));
      SetState(wrap->env, kUbiArrayBufferOffset, 0);
      napi_value argv[1] = {Undefined(wrap->env)};
      napi_value result = nullptr;
      if (UbiAsyncWrapMakeCallback(
              wrap->env, wrap->async_id, self, self, onread, 1, argv, &result, kUbiMakeCallbackNone) != napi_ok) {
        return;
      }
      if (result != nullptr) {
        napi_valuetype type = napi_undefined;
        if (napi_typeof(wrap->env, result, &type) == napi_ok &&
            type != napi_undefined &&
            type != napi_null) {
          if (type == napi_boolean) {
            bool keep_reading = true;
            if (napi_get_value_bool(wrap->env, result, &keep_reading) == napi_ok && !keep_reading) {
              (void)CallParentMethodInt(wrap, "readStop", 0, nullptr, nullptr);
              napi_set_named_property(wrap->env, self, "reading", MakeBool(wrap->env, false));
              break;
            }
          }
          bool is_buffer = false;
          if (napi_is_buffer(wrap->env, result, &is_buffer) == napi_ok && is_buffer) {
            void* raw = nullptr;
            size_t raw_len = 0;
            if (napi_get_buffer_info(wrap->env, result, &raw, &raw_len) == napi_ok &&
                raw != nullptr &&
                raw_len != 0) {
              DeleteRefIfPresent(wrap->env, &wrap->user_read_buffer_ref);
              if (napi_create_reference(wrap->env, result, 1, &wrap->user_read_buffer_ref) == napi_ok) {
                wrap->user_buffer_base = static_cast<char*>(raw);
                wrap->user_buffer_len = raw_len;
              }
            }
          } else {
            bool is_typedarray = false;
            if (napi_is_typedarray(wrap->env, result, &is_typedarray) == napi_ok && is_typedarray) {
              napi_typedarray_type ta_type = napi_uint8_array;
              size_t element_len = 0;
              void* raw = nullptr;
              napi_value arraybuffer = nullptr;
              size_t byte_offset = 0;
              if (napi_get_typedarray_info(
                      wrap->env, result, &ta_type, &element_len, &raw, &arraybuffer, &byte_offset) == napi_ok &&
                  raw != nullptr &&
                  element_len != 0) {
                DeleteRefIfPresent(wrap->env, &wrap->user_read_buffer_ref);
                if (napi_create_reference(wrap->env, result, 1, &wrap->user_read_buffer_ref) == napi_ok) {
                  wrap->user_buffer_base = static_cast<char*>(raw);
                  wrap->user_buffer_len = element_len * UbiTypedArrayElementSize(ta_type);
                }
              }
            }
          }
        }
      }
      offset += chunk_len;
      if (wrap->user_buffer_base == nullptr || wrap->user_buffer_len == 0) break;
    }
    return;
  }
  napi_value arraybuffer = nullptr;
  void* raw = nullptr;
  if (napi_create_arraybuffer(wrap->env, len, &raw, &arraybuffer) != napi_ok || arraybuffer == nullptr) return;
  if (len > 0 && raw != nullptr) {
    std::memcpy(raw, data, len);
  }
  SetState(wrap->env, kUbiReadBytesOrError, static_cast<int32_t>(len));
  SetState(wrap->env, kUbiArrayBufferOffset, 0);
  napi_value argv[1] = {arraybuffer};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, onread, 1, argv, &ignored, kUbiMakeCallbackNone);
}

void EmitOnReadStatus(TlsWrap* wrap, int32_t status) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onread = GetNamedValue(wrap->env, self, "onread");
  if (!IsFunction(wrap->env, onread)) return;
  SetState(wrap->env, kUbiReadBytesOrError, status);
  SetState(wrap->env, kUbiArrayBufferOffset, 0);
  napi_value argv[1] = {Undefined(wrap->env)};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, onread, 1, argv, &ignored, kUbiMakeCallbackNone);
}

napi_value CreateErrorWithCode(napi_env env, const char* code, const std::string& message) {
  napi_value code_v = nullptr;
  napi_value msg_v = nullptr;
  napi_value err_v = nullptr;
  if (code != nullptr) {
    napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_v);
  } else {
    napi_get_undefined(env, &code_v);
  }
  napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &msg_v);
  napi_create_error(env, code_v, msg_v, &err_v);
  if (err_v != nullptr && code != nullptr && code_v != nullptr) {
    napi_set_named_property(env, err_v, "code", code_v);
  }
  return err_v;
}

std::string DeriveSslCodeFromReason(const char* reason) {
  if (reason == nullptr || reason[0] == '\0') return {};
  std::string code = "ERR_SSL_";
  for (const unsigned char ch : std::string(reason)) {
    if (ch == ' ') {
      code.push_back('_');
    } else {
      code.push_back(static_cast<char>(std::toupper(ch)));
    }
  }
  return code;
}

std::string DeriveOpenSslCode(unsigned long err, const char* reason) {
  if (reason == nullptr || reason[0] == '\0') return {};
  std::string normalized(reason);
  for (char& ch : normalized) {
    if (ch == ' ') {
      ch = '_';
    } else {
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
  }

#define OSSL_ERROR_CODES_MAP(V)                                               \
  V(SYS)                                                                      \
  V(BN)                                                                       \
  V(RSA)                                                                      \
  V(DH)                                                                       \
  V(EVP)                                                                      \
  V(BUF)                                                                      \
  V(OBJ)                                                                      \
  V(PEM)                                                                      \
  V(DSA)                                                                      \
  V(X509)                                                                     \
  V(ASN1)                                                                     \
  V(CONF)                                                                     \
  V(CRYPTO)                                                                   \
  V(EC)                                                                       \
  V(SSL)                                                                      \
  V(BIO)                                                                      \
  V(PKCS7)                                                                    \
  V(X509V3)                                                                   \
  V(PKCS12)                                                                   \
  V(RAND)                                                                     \
  V(DSO)                                                                      \
  V(ENGINE)                                                                   \
  V(OCSP)                                                                     \
  V(UI)                                                                       \
  V(COMP)                                                                     \
  V(ECDSA)                                                                    \
  V(ECDH)                                                                     \
  V(OSSL_STORE)                                                               \
  V(FIPS)                                                                     \
  V(CMS)                                                                      \
  V(TS)                                                                       \
  V(HMAC)                                                                     \
  V(CT)                                                                       \
  V(ASYNC)                                                                    \
  V(KDF)                                                                      \
  V(SM2)                                                                      \
  V(USER)

  const char* lib = "";
  const char* prefix = "OSSL_";
  switch (ERR_GET_LIB(err)) {
#define V(name) case ERR_LIB_##name: lib = #name "_"; break;
    OSSL_ERROR_CODES_MAP(V)
#undef V
    default:
      break;
  }
#undef OSSL_ERROR_CODES_MAP

  if (std::strcmp(lib, "SSL_") == 0) prefix = "";
  std::string code = "ERR_";
  code += prefix;
  code += lib;
  code += normalized;
  return code;
}

void SetErrorStackProperty(napi_env env, napi_value err, const std::vector<unsigned long>& errors) {
  if (err == nullptr || errors.size() <= 1) return;
  napi_value stack = nullptr;
  if (napi_create_array_with_length(env, errors.size() - 1, &stack) != napi_ok || stack == nullptr) return;
  uint32_t index = 0;
  for (size_t i = 1; i < errors.size(); ++i, ++index) {
    char buf[256];
    ERR_error_string_n(errors[i], buf, sizeof(buf));
    napi_value entry = nullptr;
    if (napi_create_string_utf8(env, buf, NAPI_AUTO_LENGTH, &entry) == napi_ok && entry != nullptr) {
      napi_set_element(env, stack, index, entry);
    }
  }
  napi_set_named_property(env, err, "opensslErrorStack", stack);
}

void SetErrorStringProperty(napi_env env, napi_value err, const char* name, const char* value) {
  if (err == nullptr || name == nullptr || value == nullptr || value[0] == '\0') return;
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, err, name, out);
  }
}

napi_value CreateLastOpenSslError(napi_env env, const char* fallback_code, const char* fallback_message) {
  std::vector<unsigned long> errors;
  while (const unsigned long err = ERR_get_error()) {
    errors.push_back(err);
  }
  if (errors.empty()) {
    return CreateErrorWithCode(env, fallback_code, fallback_message != nullptr ? fallback_message : "OpenSSL error");
  }

  const unsigned long err = errors.front();
  char buf[256];
  ERR_error_string_n(err, buf, sizeof(buf));
  const char* library = ERR_lib_error_string(err);
  const char* reason = ERR_reason_error_string(err);
  std::string derived_code;
  if (fallback_code == nullptr || std::strncmp(fallback_code, "ERR_TLS_", 8) == 0) {
    derived_code = DeriveOpenSslCode(err, reason);
  }
  const char* code = !derived_code.empty() ? derived_code.c_str() : fallback_code;
  napi_value error = CreateErrorWithCode(env, code, buf);
  SetErrorStringProperty(env, error, "library", library);
#if OPENSSL_VERSION_NUMBER < 0x30000000L
  SetErrorStringProperty(env, error, "function", ERR_func_error_string(err));
#endif
  SetErrorStringProperty(env, error, "reason", reason);
  SetErrorStackProperty(env, error, errors);
  return error;
}

void EmitError(TlsWrap* wrap, napi_value error) {
  if (wrap == nullptr || wrap->env == nullptr || error == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value onerror = GetNamedValue(wrap->env, self, "onerror");
  if (!IsFunction(wrap->env, onerror)) return;
  napi_value argv[1] = {error};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, onerror, 1, argv, &ignored, kUbiMakeCallbackNone);
}

void CompleteReq(TlsWrap* wrap, napi_ref* req_ref, int status) {
  if (wrap == nullptr || wrap->env == nullptr || req_ref == nullptr || *req_ref == nullptr) return;
  if (wrap->defer_req_callbacks && ScheduleReqCompletion(wrap, req_ref, status)) {
    return;
  }
  napi_value req_obj = GetRefValue(wrap->env, *req_ref);
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value argv[3] = {
      MakeInt32(wrap->env, status),
      self != nullptr ? self : Undefined(wrap->env),
      status < 0 ? GetNamedValue(wrap->env, req_obj, "error") : Undefined(wrap->env),
  };
  UbiStreamBaseInvokeReqOnComplete(wrap->env, req_obj, status, argv, 3);
  DeleteRefIfPresent(wrap->env, req_ref);
}

void MaybeFinishActiveWrite(TlsWrap* wrap, int status) {
  if (wrap == nullptr || wrap->active_write_req_ref == nullptr) return;
  if (status == 0) {
    if (!wrap->write_callback_scheduled ||
        wrap->parent_write_in_progress ||
        !wrap->pending_encrypted_writes.empty() ||
        !wrap->pending_app_writes.empty()) {
      return;
    }
  }
  wrap->write_callback_scheduled = false;
  CompleteReq(wrap, &wrap->active_write_req_ref, status);
}

bool GetArrayBufferBytes(napi_env env,
                         napi_value value,
                         const uint8_t** data,
                         size_t* len,
                         size_t* offset_out) {
  if (data == nullptr || len == nullptr || offset_out == nullptr) return false;
  *data = nullptr;
  *len = 0;
  *offset_out = 0;
  if (value == nullptr) return false;
  bool is_ab = false;
  if (napi_is_arraybuffer(env, value, &is_ab) == napi_ok && is_ab) {
    void* raw = nullptr;
    size_t byte_len = 0;
    if (napi_get_arraybuffer_info(env, value, &raw, &byte_len) != napi_ok || raw == nullptr) return false;
    *data = static_cast<const uint8_t*>(raw);
    *len = byte_len;
    return true;
  }
  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) {
    napi_typedarray_type type = napi_uint8_array;
    size_t element_len = 0;
    void* raw = nullptr;
    napi_value ab = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(env, value, &type, &element_len, &raw, &ab, &byte_offset) != napi_ok ||
        raw == nullptr) {
      return false;
    }
    *data = static_cast<const uint8_t*>(raw);
    *len = element_len * UbiTypedArrayElementSize(type);
    *offset_out = byte_offset;
    return true;
  }
  return false;
}

int32_t CallParentMethodInt(TlsWrap* wrap, const char* method, size_t argc, napi_value* argv, napi_value* result_out) {
  if (wrap == nullptr || method == nullptr) return UV_EINVAL;
  napi_value parent = GetRefValue(wrap->env, wrap->parent_ref);
  if (parent == nullptr) return UV_EINVAL;
  napi_value fn = GetNamedValue(wrap->env, parent, method);
  if (!IsFunction(wrap->env, fn)) return UV_EINVAL;
  napi_value result = nullptr;
  if (napi_call_function(wrap->env, parent, fn, argc, argv, &result) != napi_ok || result == nullptr) {
    return UV_EINVAL;
  }
  if (result_out != nullptr) *result_out = result;
  int32_t out = 0;
  if (napi_get_value_int32(wrap->env, result, &out) != napi_ok) return 0;
  return out;
}

bool SetSecureContextOnSsl(TlsWrap* wrap, ubi::crypto::SecureContextHolder* holder) {
  if (wrap == nullptr || wrap->ssl == nullptr || holder == nullptr || holder->ctx == nullptr) return false;
  SSL_CTX_set_tlsext_status_cb(holder->ctx, TLSExtStatusCallback);
  SSL_CTX_set_tlsext_status_arg(holder->ctx, nullptr);
  if (SSL_set_SSL_CTX(wrap->ssl, holder->ctx) == nullptr) return false;
  wrap->secure_context = holder;
  X509_STORE* store = SSL_CTX_get_cert_store(holder->ctx);
  if (store != nullptr && SSL_set1_verify_cert_store(wrap->ssl, store) != 1) return false;
  STACK_OF(X509_NAME)* list = SSL_dup_CA_list(SSL_CTX_get_client_CA_list(holder->ctx));
  SSL_set_client_CA_list(wrap->ssl, list);
  return true;
}

void InitSsl(TlsWrap* wrap);
void Cycle(TlsWrap* wrap);
bool TryHandshake(TlsWrap* wrap);
void TryStartParentWrite(TlsWrap* wrap);
void MaybeStartParentShutdown(TlsWrap* wrap);
void MaybeStartTlsShutdown(TlsWrap* wrap);
bool ReadCleartext(TlsWrap* wrap);

void ResumeCertCallback(void* arg) {
  Cycle(static_cast<TlsWrap*>(arg));
}

void CleanupPendingWrites(TlsWrap* wrap, int status) {
  if (wrap == nullptr) return;
  while (!wrap->pending_encrypted_writes.empty()) {
    PendingEncryptedWrite pending = std::move(wrap->pending_encrypted_writes.front());
    wrap->pending_encrypted_writes.pop_front();
    CompleteReq(wrap, &pending.completion_req_ref, status);
  }
  while (!wrap->pending_app_writes.empty()) {
    PendingAppWrite pending = std::move(wrap->pending_app_writes.front());
    wrap->pending_app_writes.pop_front();
    CompleteReq(wrap, &pending.req_ref, status);
  }
  CompleteReq(wrap, &wrap->active_write_req_ref, status);
  InvokeReqWithStatus(wrap, &wrap->pending_shutdown_req_ref, status);
}

void DestroySsl(TlsWrap* wrap) {
  if (wrap == nullptr) return;
  if (wrap->ssl != nullptr) {
    CleanupPendingWrites(wrap, UV_ECANCELED);
    SSL_free(wrap->ssl);
    wrap->ssl = nullptr;
    wrap->enc_in = nullptr;
    wrap->enc_out = nullptr;
    wrap->parent_write_in_progress = false;
    wrap->write_callback_scheduled = false;
    wrap->refed = false;
  }
  if (wrap->next_session != nullptr) {
    SSL_SESSION_free(wrap->next_session);
    wrap->next_session = nullptr;
  }
  DeleteRefIfPresent(wrap->env, &wrap->user_read_buffer_ref);
  wrap->user_buffer_base = nullptr;
  wrap->user_buffer_len = 0;
  ReleaseKeepaliveHandle(wrap);
}

void TlsWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<TlsWrap*>(data);
  if (wrap == nullptr) return;
  DestroySsl(wrap);
  ReleaseKeepaliveHandle(wrap);
  RemoveWrapFromState(wrap);
  DeleteRefIfPresent(env, &wrap->parent_ref);
  DeleteRefIfPresent(env, &wrap->context_ref);
  DeleteRefIfPresent(env, &wrap->active_write_req_ref);
  DeleteRefIfPresent(env, &wrap->user_read_buffer_ref);
  DeleteRefIfPresent(env, &wrap->wrapper_ref);
  delete wrap;
}

void EmitHandshakeCallback(TlsWrap* wrap, const char* name, size_t argc, napi_value* argv) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, name);
  if (!IsFunction(wrap->env, cb)) return;
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, argc, argv, &ignored, kUbiMakeCallbackNone);
}

void EmitHandshakeDoneIfPending(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr || !wrap->handshake_done_pending || wrap->handshake_done_emitted) {
    return;
  }
  if (wrap->parent_write_in_progress || !wrap->pending_encrypted_writes.empty()) {
    return;
  }
  wrap->handshake_done_pending = false;
  wrap->handshake_done_emitted = true;
  if (wrap->keepalive_needed) {
    ReleaseKeepaliveHandle(wrap);
  }
  EmitHandshakeCallback(wrap, "onhandshakedone", 0, nullptr);
  MaybeStartTlsShutdown(wrap);
}

bool HandshakeCallbacksReady(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr) return false;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return false;
  return IsFunction(wrap->env, GetNamedValue(wrap->env, self, "onhandshakedone")) &&
         IsFunction(wrap->env, GetNamedValue(wrap->env, self, "onerror"));
}

void OnClientHello(TlsWrap* wrap, const ClientHelloData& hello) {
  if (wrap == nullptr || wrap->env == nullptr) return;
  const uint64_t ctx_options =
      wrap->ssl != nullptr && SSL_get_SSL_CTX(wrap->ssl) != nullptr ? SSL_CTX_get_options(SSL_get_SSL_CTX(wrap->ssl))
                                                                    : 0;
  const bool has_ticket = hello.has_ticket &&
                          (ctx_options & SSL_OP_NO_TICKET) == 0;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;
  napi_value cb = GetNamedValue(wrap->env, self, "onclienthello");
  if (!IsFunction(wrap->env, cb)) return;

  napi_value hello_obj = nullptr;
  napi_create_object(wrap->env, &hello_obj);
  napi_value session_id = CreateBufferCopy(wrap->env, hello.session_id, hello.session_size);
  napi_value servername = nullptr;
  if (hello.servername == nullptr) {
    napi_create_string_utf8(wrap->env, "", 0, &servername);
  } else {
    napi_create_string_utf8(wrap->env,
                            reinterpret_cast<const char*>(hello.servername),
                            hello.servername_size,
                            &servername);
  }
  napi_value tls_ticket = MakeBool(wrap->env, has_ticket);
  if (hello_obj == nullptr || session_id == nullptr || servername == nullptr || tls_ticket == nullptr) return;
  napi_set_named_property(wrap->env, hello_obj, "sessionId", session_id);
  napi_set_named_property(wrap->env, hello_obj, "servername", servername);
  napi_set_named_property(wrap->env, hello_obj, "tlsTicket", tls_ticket);

  napi_value argv[1] = {hello_obj};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, 1, argv, &ignored, kUbiMakeCallbackNone);
}

void FlushDeferredParentInput(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->deferred_parent_input.empty()) return;
  (void)BIO_write(wrap->enc_in,
                  wrap->deferred_parent_input.data(),
                  static_cast<int>(wrap->deferred_parent_input.size()));
  wrap->deferred_parent_input.clear();
}

void MaybeProcessDeferredParentInput(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->deferred_parent_input.empty()) return;
  if (!wrap->hello_parser.IsEnded()) {
    wrap->hello_parser.Parse(wrap, wrap->deferred_parent_input.data(), wrap->deferred_parent_input.size());
    if (!wrap->hello_parser.IsEnded()) return;
  }
  FlushDeferredParentInput(wrap);
}

int NewSessionCallback(SSL* ssl, SSL_SESSION* session);

void SslInfoCallback(const SSL* ssl, int where, int /*ret*/) {
  if ((where & (SSL_CB_HANDSHAKE_START | SSL_CB_HANDSHAKE_DONE)) == 0) return;
  TlsWrap* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr) return;
  if ((where & SSL_CB_HANDSHAKE_START) != 0) {
    const int64_t now_ms = static_cast<int64_t>(uv_hrtime() / 1000000ULL);
    napi_value argv[1] = {MakeInt64(wrap->env, now_ms)};
    EmitHandshakeCallback(wrap, "onhandshakestart", 1, argv);
  }
  if ((where & SSL_CB_HANDSHAKE_DONE) != 0 && SSL_renegotiate_pending(const_cast<SSL*>(ssl)) == 0) {
    wrap->established = true;
    wrap->handshake_done_pending = true;
    if (!wrap->is_server && wrap->session_callbacks_enabled && wrap->client_session_event_count == 0 &&
        SSL_version(const_cast<SSL*>(ssl)) < TLS1_3_VERSION &&
        SSL_session_reused(const_cast<SSL*>(ssl)) != 1) {
      SSL_SESSION* session = SSL_get_session(const_cast<SSL*>(ssl));
      if (session != nullptr) {
        (void)NewSessionCallback(const_cast<SSL*>(ssl), session);
      }
    }
  }
}

void KeylogCallback(const SSL* ssl, const char* line) {
  TlsWrap* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || line == nullptr) return;
  const size_t len = std::strlen(line);
  std::vector<uint8_t> bytes(len + 1, 0);
  if (len > 0) std::memcpy(bytes.data(), line, len);
  bytes[len] = '\n';
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, "onkeylog");
  if (!IsFunction(wrap->env, cb)) return;
  napi_value buffer = CreateBufferCopy(wrap->env, bytes.data(), bytes.size());
  if (buffer == nullptr) return;
  napi_value argv[1] = {buffer};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, 1, argv, &ignored, kUbiMakeCallbackNone);
}

int VerifyCallback(int /*preverify_ok*/, X509_STORE_CTX* /*ctx*/) {
  return 1;
}

unsigned int PskServerCallback(SSL* ssl,
                               const char* identity,
                               unsigned char* psk,
                               unsigned int max_psk_len) {
  TlsWrap* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || wrap->env == nullptr) return 0;

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetPropertyBySymbol(wrap->env, self, "onpskexchange");
  if (!IsFunction(wrap->env, cb)) return 0;

  napi_value identity_v = nullptr;
  if (identity == nullptr) {
    napi_get_null(wrap->env, &identity_v);
  } else {
    napi_create_string_utf8(wrap->env, identity, NAPI_AUTO_LENGTH, &identity_v);
  }
  napi_value max_psk_v = MakeInt32(wrap->env, static_cast<int32_t>(max_psk_len));
  napi_value argv[2] = {identity_v, max_psk_v};
  napi_value out = nullptr;
  if (UbiAsyncWrapMakeCallback(
          wrap->env, wrap->async_id, self, self, cb, 2, argv, &out, kUbiMakeCallbackNone) != napi_ok ||
      out == nullptr) {
    return 0;
  }

  const uint8_t* data = nullptr;
  size_t len = 0;
  if (!GetArrayBufferViewSpan(wrap->env, out, &data, &len) || data == nullptr || len > max_psk_len) {
    return 0;
  }
  std::memcpy(psk, data, len);
  return static_cast<unsigned int>(len);
}

unsigned int PskClientCallback(SSL* ssl,
                               const char* hint,
                               char* identity,
                               unsigned int max_identity_len,
                               unsigned char* psk,
                               unsigned int max_psk_len) {
  TlsWrap* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || wrap->env == nullptr) return 0;

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetPropertyBySymbol(wrap->env, self, "onpskexchange");
  if (!IsFunction(wrap->env, cb)) return 0;

  napi_value hint_v = nullptr;
  if (hint == nullptr) {
    napi_get_null(wrap->env, &hint_v);
  } else {
    napi_create_string_utf8(wrap->env, hint, NAPI_AUTO_LENGTH, &hint_v);
  }
  napi_value argv[3] = {
      hint_v,
      MakeInt32(wrap->env, static_cast<int32_t>(max_psk_len)),
      MakeInt32(wrap->env, static_cast<int32_t>(max_identity_len)),
  };
  napi_value result = nullptr;
  if (UbiAsyncWrapMakeCallback(
          wrap->env, wrap->async_id, self, self, cb, 3, argv, &result, kUbiMakeCallbackNone) != napi_ok ||
      result == nullptr) {
    return 0;
  }

  napi_value identity_v = GetNamedValue(wrap->env, result, "identity");
  napi_value psk_v = GetNamedValue(wrap->env, result, "psk");
  if (identity_v == nullptr || psk_v == nullptr) return 0;

  napi_valuetype identity_type = napi_undefined;
  if (napi_typeof(wrap->env, identity_v, &identity_type) != napi_ok || identity_type != napi_string) {
    return 0;
  }
  const std::string identity_str = ValueToUtf8(wrap->env, identity_v);
  if (identity_str.size() > max_identity_len) return 0;

  const uint8_t* psk_data = nullptr;
  size_t psk_len = 0;
  if (!GetArrayBufferViewSpan(wrap->env, psk_v, &psk_data, &psk_len) || psk_data == nullptr ||
      psk_len > max_psk_len) {
    return 0;
  }

  std::memcpy(identity, identity_str.data(), identity_str.size());
  if (identity_str.size() < max_identity_len) {
    identity[identity_str.size()] = '\0';
  }
  std::memcpy(psk, psk_data, psk_len);
  return static_cast<unsigned int>(psk_len);
}

int CertCallback(SSL* ssl, void* arg) {
  auto* wrap = static_cast<TlsWrap*>(arg);
  if (wrap == nullptr || !wrap->is_server || !wrap->waiting_cert_cb) return 1;
  if (wrap->cert_cb_running) return -1;

  wrap->cert_cb_running = true;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value owner = UbiHandleWrapGetActiveOwner(wrap->env, wrap->wrapper_ref);
  napi_value info = nullptr;
  napi_create_object(wrap->env, &info);
  const char* servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (servername != nullptr) {
    napi_value sn = nullptr;
    napi_create_string_utf8(wrap->env, servername, NAPI_AUTO_LENGTH, &sn);
    if (sn != nullptr) {
      napi_set_named_property(wrap->env, info, "servername", sn);
      if (owner != nullptr) {
        napi_set_named_property(wrap->env, owner, "servername", sn);
      }
    }
  }
  napi_value ocsp = MakeBool(wrap->env, SSL_get_tlsext_status_type(ssl) == TLSEXT_STATUSTYPE_ocsp);
  if (ocsp != nullptr) napi_set_named_property(wrap->env, info, "OCSPRequest", ocsp);

  napi_value argv[1] = {info};
  EmitHandshakeCallback(wrap, "oncertcb", 1, argv);
  return wrap->cert_cb_running ? -1 : 1;
}

napi_value GetSSLOCSPResponse(TlsWrap* wrap, SSL* ssl) {
  if (wrap == nullptr || wrap->env == nullptr || ssl == nullptr) return nullptr;
  const unsigned char* resp = nullptr;
  const int len = SSL_get_tlsext_status_ocsp_resp(ssl, &resp);
  if (resp == nullptr || len < 0) {
    return Null(wrap->env);
  }
  napi_value buffer = CreateBufferCopy(wrap->env, resp, static_cast<size_t>(len));
  if (buffer == nullptr) return nullptr;
  return buffer;
}

bool MaybeEmitClientOcspResponse(TlsWrap* wrap, bool allow_null) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->ssl == nullptr || wrap->is_server ||
      wrap->client_ocsp_event_emitted) {
    return false;
  }

  const unsigned char* resp = nullptr;
  const int len = SSL_get_tlsext_status_ocsp_resp(wrap->ssl, &resp);
  if ((resp == nullptr || len < 0) && !allow_null) {
    return false;
  }

  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, "onocspresponse");
  if (!IsFunction(wrap->env, cb)) {
    wrap->pending_client_ocsp_event = false;
    wrap->client_ocsp_event_emitted = true;
    return false;
  }

  napi_value arg = nullptr;
  if (resp != nullptr && len >= 0) {
    arg = GetSSLOCSPResponse(wrap, wrap->ssl);
  }
  if (arg == nullptr) {
    arg = Null(wrap->env);
  }

  napi_value argv[1] = {arg};
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, 1, argv, &ignored, kUbiMakeCallbackNone);
  wrap->pending_client_ocsp_event = false;
  wrap->client_ocsp_event_emitted = true;
  return true;
}

bool ApplyPendingServerOcspResponse(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr || !wrap->is_server || wrap->ocsp_response.empty()) {
    return true;
  }

  const unsigned char* existing = nullptr;
  if (SSL_get_tlsext_status_ocsp_resp(wrap->ssl, &existing) >= 0 && existing != nullptr) {
    wrap->ocsp_response.clear();
    return true;
  }

  const size_t len = wrap->ocsp_response.size();
  unsigned char* data = static_cast<unsigned char*>(OPENSSL_malloc(len));
  if (data == nullptr) {
    return false;
  }
  if (len > 0) {
    std::memcpy(data, wrap->ocsp_response.data(), len);
  }
  if (!SSL_set_tlsext_status_ocsp_resp(wrap->ssl, data, static_cast<int>(len))) {
    OPENSSL_free(data);
    return false;
  }
  wrap->ocsp_response.clear();
  return true;
}

int TLSExtStatusCallback(SSL* ssl, void* /*arg*/) {
  auto* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || wrap->env == nullptr) return 1;

  if (!wrap->is_server) {
    const unsigned char* resp = nullptr;
    const int len = SSL_get_tlsext_status_ocsp_resp(ssl, &resp);
    if (resp == nullptr || len < 0) {
      wrap->pending_client_ocsp_event = true;
    } else {
      (void)MaybeEmitClientOcspResponse(wrap, true);
    }
    return 1;
  }

  const unsigned char* existing = nullptr;
  if (SSL_get_tlsext_status_ocsp_resp(ssl, &existing) >= 0 && existing != nullptr) {
    wrap->ocsp_response.clear();
    return SSL_TLSEXT_ERR_OK;
  }

  if (wrap->ocsp_response.empty()) return SSL_TLSEXT_ERR_NOACK;
  const size_t len = wrap->ocsp_response.size();
  unsigned char* data = static_cast<unsigned char*>(OPENSSL_malloc(len));
  if (data == nullptr) return SSL_TLSEXT_ERR_NOACK;
  if (len > 0) {
    std::memcpy(data, wrap->ocsp_response.data(), len);
  }
  if (!SSL_set_tlsext_status_ocsp_resp(ssl, data, static_cast<int>(len))) {
    OPENSSL_free(data);
    return SSL_TLSEXT_ERR_NOACK;
  }
  wrap->ocsp_response.clear();
  return SSL_TLSEXT_ERR_OK;
}

int SelectALPNCallback(SSL* ssl,
                       const unsigned char** out,
                       unsigned char* outlen,
                       const unsigned char* in,
                       unsigned int inlen,
                       void* /*arg*/) {
  auto* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr) return SSL_TLSEXT_ERR_NOACK;

  if (wrap->alpn_callback_enabled) {
    napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
    napi_value cb = GetNamedValue(wrap->env, self, "ALPNCallback");
    if (!IsFunction(wrap->env, cb)) return SSL_TLSEXT_ERR_ALERT_FATAL;
    napi_value buffer = CreateBufferCopy(wrap->env, in, inlen);
    napi_value argv[1] = {buffer};
    napi_value result = nullptr;
    if (UbiAsyncWrapMakeCallback(
            wrap->env, wrap->async_id, self, self, cb, 1, argv, &result, kUbiMakeCallbackNone) != napi_ok ||
        result == nullptr) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    napi_valuetype type = napi_undefined;
    if (napi_typeof(wrap->env, result, &type) != napi_ok || type == napi_undefined) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    uint32_t offset = 0;
    if (napi_get_value_uint32(wrap->env, result, &offset) != napi_ok || offset >= inlen) {
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    *outlen = *(in + offset);
    *out = in + offset + 1;
    return SSL_TLSEXT_ERR_OK;
  }

  if (wrap->alpn_protos.empty()) return SSL_TLSEXT_ERR_NOACK;
  const int rc =
      SSL_select_next_proto(const_cast<unsigned char**>(out),
                            outlen,
                            wrap->alpn_protos.data(),
                            static_cast<unsigned int>(wrap->alpn_protos.size()),
                            in,
                            inlen);
  return rc == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
}

SSL_SESSION* GetSessionCallback(SSL* ssl,
                                const unsigned char* /*key*/,
                                int /*len*/,
                                int* copy) {
  auto* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (copy != nullptr) *copy = 0;
  if (wrap == nullptr) return nullptr;
  SSL_SESSION* session = wrap->next_session;
  wrap->next_session = nullptr;
  return session;
}

int NewSessionCallback(SSL* ssl, SSL_SESSION* session) {
  auto* wrap = static_cast<TlsWrap*>(SSL_get_app_data(ssl));
  if (wrap == nullptr || wrap->env == nullptr || session == nullptr) return 1;
  if (!wrap->session_callbacks_enabled) return 0;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value cb = GetNamedValue(wrap->env, self, "onnewsession");
  if (!IsFunction(wrap->env, cb)) return 0;

  unsigned int id_len = 0;
  const unsigned char* id = SSL_SESSION_get_id(session, &id_len);
  napi_value id_buffer = CreateBufferCopy(wrap->env, id, id_len);

  const int encoded_len = i2d_SSL_SESSION(session, nullptr);
  if (encoded_len <= 0 || id_buffer == nullptr) return 0;
  std::vector<uint8_t> encoded(static_cast<size_t>(encoded_len));
  unsigned char* ptr = encoded.data();
  if (i2d_SSL_SESSION(session, &ptr) != encoded_len) return 0;

  napi_value session_buffer = CreateBufferCopy(wrap->env, encoded.data(), encoded.size());
  if (session_buffer == nullptr) return 0;

  napi_value argv[2] = {id_buffer, session_buffer};
  if (wrap->is_server) wrap->awaiting_new_session = true;
  if (!wrap->is_server) wrap->client_session_event_count++;
  napi_value ignored = nullptr;
  (void)UbiAsyncWrapMakeCallback(
      wrap->env, wrap->async_id, self, self, cb, 2, argv, &ignored, kUbiMakeCallbackNone);
  return 0;
}

void DeleteClientSessionFallbackTask(napi_env env, ClientSessionFallbackTask* task) {
  if (task == nullptr) return;
  DeleteRefIfPresent(env, &task->self_ref);
  delete task;
}

void DeleteDeferredReqCompletionTask(napi_env env, DeferredReqCompletionTask* task) {
  if (task == nullptr) return;
  DeleteRefIfPresent(env, &task->self_ref);
  DeleteRefIfPresent(env, &task->req_ref);
  delete task;
}

napi_value RunClientSessionFallbackTask(napi_env env, napi_callback_info info) {
  void* data = nullptr;
  napi_get_cb_info(env, info, nullptr, nullptr, nullptr, &data);
  auto* task = static_cast<ClientSessionFallbackTask*>(data);
  if (env == nullptr || task == nullptr || task->self_ref == nullptr) return Undefined(env);
  napi_value self = GetRefValue(env, task->self_ref);
  TlsWrap* wrap = self != nullptr ? FindWrapBySelf(env, self) : nullptr;
  if (wrap != nullptr) {
    wrap->client_session_fallback_scheduled = false;
  }
  if (wrap == nullptr || wrap->ssl == nullptr) {
    DeleteClientSessionFallbackTask(env, task);
    return Undefined(env);
  }
  if (wrap->is_server || !wrap->established || !wrap->session_callbacks_enabled ||
      wrap->client_session_fallback_emitted) {
    DeleteClientSessionFallbackTask(env, task);
    return Undefined(env);
  }
  if (SSL_version(wrap->ssl) >= TLS1_3_VERSION || SSL_session_reused(wrap->ssl) == 1) {
    DeleteClientSessionFallbackTask(env, task);
    return Undefined(env);
  }
  SSL_SESSION* session = SSL_get_session(wrap->ssl);
  if (session != nullptr) {
    wrap->client_session_fallback_emitted = true;
    (void)NewSessionCallback(wrap->ssl, session);
  }
  DeleteClientSessionFallbackTask(env, task);
  return Undefined(env);
}

napi_value RunDeferredReqCompletionTask(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, nullptr, nullptr, &data);
  auto* task = static_cast<DeferredReqCompletionTask*>(data);
  if (task == nullptr) return Undefined(env);

  napi_value self = GetRefValue(env, task->self_ref);
  napi_value req_obj = GetRefValue(env, task->req_ref);
  if (self != nullptr && req_obj != nullptr) {
    napi_value argv[3] = {
        MakeInt32(env, task->status),
        self,
        task->status < 0 ? GetNamedValue(env, req_obj, "error") : Undefined(env),
    };
    UbiStreamBaseInvokeReqOnComplete(env, req_obj, task->status, argv, 3);
  }
  DeleteDeferredReqCompletionTask(env, task);
  return Undefined(env);
}

void ScheduleClientSessionFallback(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->ssl == nullptr || wrap->is_server ||
      !wrap->established || !wrap->session_callbacks_enabled || wrap->client_session_fallback_emitted ||
      wrap->client_session_fallback_scheduled) {
    return;
  }
  if (SSL_version(wrap->ssl) >= TLS1_3_VERSION || SSL_session_reused(wrap->ssl) == 1) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;
  auto* task = new ClientSessionFallbackTask();
  if (napi_create_reference(wrap->env, self, 1, &task->self_ref) != napi_ok || task->self_ref == nullptr) {
    delete task;
    return;
  }
  napi_value callback = nullptr;
  if (napi_create_function(
          wrap->env, "__ubiTlsSessionFallback", NAPI_AUTO_LENGTH, RunClientSessionFallbackTask, task, &callback) !=
          napi_ok ||
      callback == nullptr) {
    DeleteClientSessionFallbackTask(wrap->env, task);
    return;
  }
  napi_value global = internal_binding::GetGlobal(wrap->env);
  napi_value process = nullptr;
  napi_value next_tick = nullptr;
  napi_valuetype next_tick_type = napi_undefined;
  if (global == nullptr ||
      napi_get_named_property(wrap->env, global, "process", &process) != napi_ok ||
      process == nullptr ||
      napi_get_named_property(wrap->env, process, "nextTick", &next_tick) != napi_ok ||
      next_tick == nullptr ||
      napi_typeof(wrap->env, next_tick, &next_tick_type) != napi_ok ||
      next_tick_type != napi_function) {
    DeleteClientSessionFallbackTask(wrap->env, task);
    return;
  }
  napi_value argv[1] = {callback};
  napi_value ignored = nullptr;
  if (napi_call_function(wrap->env, process, next_tick, 1, argv, &ignored) != napi_ok) {
    DeleteClientSessionFallbackTask(wrap->env, task);
    return;
  }
  wrap->client_session_fallback_scheduled = true;
}

bool ScheduleReqCompletion(TlsWrap* wrap, napi_ref* req_ref, int status) {
  if (wrap == nullptr || wrap->env == nullptr || req_ref == nullptr || *req_ref == nullptr) return false;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  napi_value req_obj = GetRefValue(wrap->env, *req_ref);
  if (self == nullptr || req_obj == nullptr) return false;

  auto* task = new DeferredReqCompletionTask();
  task->status = status;
  task->req_ref = *req_ref;
  *req_ref = nullptr;

  if (napi_create_reference(wrap->env, self, 1, &task->self_ref) != napi_ok || task->self_ref == nullptr) {
    task->self_ref = nullptr;
    task->req_ref = nullptr;
    delete task;
    return false;
  }

  napi_value callback = nullptr;
  if (napi_create_function(wrap->env,
                           "__ubiTlsDeferredReqCompletion",
                           NAPI_AUTO_LENGTH,
                           RunDeferredReqCompletionTask,
                           task,
                           &callback) != napi_ok ||
      callback == nullptr) {
    DeleteDeferredReqCompletionTask(wrap->env, task);
    return false;
  }

  napi_value global = internal_binding::GetGlobal(wrap->env);
  napi_value set_immediate = nullptr;
  napi_valuetype set_immediate_type = napi_undefined;
  if (global == nullptr ||
      napi_get_named_property(wrap->env, global, "setImmediate", &set_immediate) != napi_ok ||
      set_immediate == nullptr ||
      napi_typeof(wrap->env, set_immediate, &set_immediate_type) != napi_ok ||
      set_immediate_type != napi_function) {
    DeleteDeferredReqCompletionTask(wrap->env, task);
    return false;
  }

  napi_value argv[1] = {callback};
  napi_value ignored = nullptr;
  if (napi_call_function(wrap->env, global, set_immediate, 1, argv, &ignored) != napi_ok) {
    DeleteDeferredReqCompletionTask(wrap->env, task);
    return false;
  }
  return true;
}

void QueueEncryptedWrite(TlsWrap* wrap,
                         std::vector<uint8_t> bytes,
                         napi_ref completion_req_ref,
                         bool force_parent_turn = false);

bool HandleSslError(TlsWrap* wrap, int ssl_result, const char* fallback_code, const char* fallback_message) {
  if (wrap == nullptr || wrap->ssl == nullptr) return true;
  const int err = SSL_get_error(wrap->ssl, ssl_result);
  if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_X509_LOOKUP) {
    return false;
  }
  if (err == SSL_ERROR_ZERO_RETURN) return false;
  std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
  if (!encrypted.empty()) {
    QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
    TryStartParentWrite(wrap);
  }
  EmitError(wrap, CreateLastOpenSslError(wrap->env, fallback_code, fallback_message));
  return true;
}

void QueueEncryptedWrite(TlsWrap* wrap,
                         std::vector<uint8_t> bytes,
                         napi_ref completion_req_ref,
                         bool force_parent_turn) {
  if (wrap == nullptr) return;
  if (bytes.empty() && !force_parent_turn) {
    if (completion_req_ref != nullptr) {
      CompleteReq(wrap, &completion_req_ref, 0);
    }
    return;
  }
  PendingEncryptedWrite pending;
  pending.data = std::move(bytes);
  pending.completion_req_ref = completion_req_ref;
  pending.force_parent_turn = force_parent_turn;
  wrap->pending_encrypted_writes.push_back(std::move(pending));
}

napi_value CreateInternalWriteReq(TlsWrap* wrap);

void OnInternalWriteDone(TlsWrap* wrap, int status) {
  if (wrap == nullptr) return;
  wrap->parent_write_in_progress = false;
  if (wrap->pending_encrypted_writes.empty()) return;

  PendingEncryptedWrite pending = std::move(wrap->pending_encrypted_writes.front());
  wrap->pending_encrypted_writes.pop_front();
  int effective_status = status;
  if (wrap->ssl == nullptr) {
    effective_status = UV_ECANCELED;
  }
  CompleteReq(wrap, &pending.completion_req_ref, effective_status);
  if (effective_status != 0) {
    MaybeFinishActiveWrite(wrap, effective_status);
  }
  MaybeStartTlsShutdown(wrap);
  MaybeStartParentShutdown(wrap);
  Cycle(wrap);
  if (effective_status == 0) {
    MaybeFinishActiveWrite(wrap, 0);
    MaybeStartTlsShutdown(wrap);
  }
}

napi_value InternalWriteOnComplete(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  void* data = nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, &data);
  auto* wrap = static_cast<TlsWrap*>(data);
  int32_t status = 0;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &status);
  OnInternalWriteDone(wrap, status);
  return Undefined(env);
}

napi_value CreateInternalWriteReq(TlsWrap* wrap) {
  napi_value req = UbiCreateStreamReqObject(wrap->env);
  if (req == nullptr) return nullptr;
  napi_value oncomplete = nullptr;
  if (napi_create_function(
          wrap->env, "__ubiTlsInternalWriteDone", NAPI_AUTO_LENGTH, InternalWriteOnComplete, wrap, &oncomplete) !=
          napi_ok ||
      oncomplete == nullptr) {
    return nullptr;
  }
  napi_set_named_property(wrap->env, req, "oncomplete", oncomplete);
  return req;
}

void TryStartParentWrite(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->parent_write_in_progress ||
      wrap->has_active_write_issued_by_prev_listener || wrap->awaiting_new_session ||
      !wrap->hello_parser.IsEnded() ||
      wrap->pending_encrypted_writes.empty()) {
    return;
  }

  const PendingEncryptedWrite& pending = wrap->pending_encrypted_writes.front();
  napi_value req = CreateInternalWriteReq(wrap);
  const uint8_t* payload_data = pending.data.empty() ? nullptr : pending.data.data();
  napi_value payload = CreateBufferCopy(wrap->env, payload_data, pending.data.size());
  if (req == nullptr || payload == nullptr) {
    OnInternalWriteDone(wrap, UV_ENOMEM);
    return;
  }

  napi_value argv[2] = {req, payload};
  napi_value result = nullptr;
  const int32_t rc = CallParentMethodInt(wrap, "writeBuffer", 2, argv, &result);
  if (rc != 0) {
    OnInternalWriteDone(wrap, rc);
    return;
  }
  int32_t* state = UbiGetStreamBaseState(wrap->env);
  const bool async = state != nullptr && state[kUbiLastWriteWasAsync] != 0;
  if (!async) {
    wrap->defer_req_callbacks = true;
    OnInternalWriteDone(wrap, 0);
    wrap->defer_req_callbacks = false;
    return;
  }
  wrap->parent_write_in_progress = true;
}

void MaybeStartParentShutdown(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->pending_shutdown_req_ref == nullptr || wrap->shutdown_started ||
      wrap->parent_write_in_progress || !wrap->pending_encrypted_writes.empty()) {
    return;
  }

  wrap->shutdown_started = true;
  napi_value req_obj = GetRefValue(wrap->env, wrap->pending_shutdown_req_ref);
  if (req_obj == nullptr) {
    DeleteRefIfPresent(wrap->env, &wrap->pending_shutdown_req_ref);
    return;
  }

  napi_value argv[1] = {req_obj};
  const int32_t rc = CallParentMethodInt(wrap, "shutdown", 1, argv, nullptr);
  if (rc != 0) {
    wrap->shutdown_started = false;
    InvokeReqWithStatus(wrap, &wrap->pending_shutdown_req_ref, rc);
    return;
  }

  DeleteRefIfPresent(wrap->env, &wrap->pending_shutdown_req_ref);
}

void MaybeStartTlsShutdown(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->pending_shutdown_req_ref == nullptr || wrap->shutdown_started ||
      !wrap->established) {
    return;
  }
  if (wrap->active_write_req_ref != nullptr ||
      wrap->write_callback_scheduled ||
      !wrap->pending_app_writes.empty() ||
      wrap->parent_write_in_progress ||
      !wrap->pending_encrypted_writes.empty()) {
    return;
  }

  ncrypto::MarkPopErrorOnReturn mark_pop_error_on_return;
  int shutdown_rc = SSL_shutdown(wrap->ssl);
  if (shutdown_rc == 0) {
    shutdown_rc = SSL_shutdown(wrap->ssl);
  }
  std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
  if (!encrypted.empty()) {
    QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
    TryStartParentWrite(wrap);
  }
  MaybeStartParentShutdown(wrap);
}

bool TryHandshake(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return false;
  if (!wrap->is_server && !wrap->started) return false;
  if (wrap->established && SSL_renegotiate_pending(wrap->ssl) == 0) return false;

  ncrypto::MarkPopErrorOnReturn mark_pop_error_on_return;
  const int rc = SSL_do_handshake(wrap->ssl);
  if (rc != 1 && HandleSslError(wrap, rc, "ERR_TLS_HANDSHAKE", "TLS handshake failed")) {
    return false;
  }

  std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
  if (!encrypted.empty()) {
    QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
    TryStartParentWrite(wrap);
    return true;
  }
  return rc == 1;
}

bool PumpPendingAppWrites(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->parent_write_in_progress ||
      wrap->has_active_write_issued_by_prev_listener) {
    return false;
  }
  if (!wrap->established) return false;

  ncrypto::MarkPopErrorOnReturn mark_pop_error_on_return;
  bool made_progress = false;
  while (!wrap->pending_app_writes.empty() && !wrap->parent_write_in_progress &&
         !wrap->has_active_write_issued_by_prev_listener) {
    PendingAppWrite& pending = wrap->pending_app_writes.front();
    bool is_current_write = false;
    if (wrap->active_write_req_ref == nullptr && pending.req_ref != nullptr) {
      wrap->active_write_req_ref = pending.req_ref;
      pending.req_ref = nullptr;
      is_current_write = true;
    } else {
      is_current_write = pending.req_ref == nullptr;
    }
    if (wrap->active_write_req_ref == nullptr || (wrap->write_callback_scheduled && is_current_write)) {
      break;
    }

    if (pending.data.empty()) {
      napi_ref completion_req_ref = nullptr;
      if (!is_current_write) {
        completion_req_ref = pending.req_ref;
        pending.req_ref = nullptr;
      }
      wrap->pending_app_writes.pop_front();
      (void)ReadCleartext(wrap);
      std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
      if (is_current_write) wrap->write_callback_scheduled = true;
      if (!encrypted.empty()) {
        QueueEncryptedWrite(wrap, std::move(encrypted), completion_req_ref);
        TryStartParentWrite(wrap);
      } else {
        QueueEncryptedWrite(wrap, std::vector<uint8_t>(), completion_req_ref, true);
      }
      made_progress = true;
      continue;
    }

    const int rc = SSL_write(wrap->ssl, pending.data.data(), static_cast<int>(pending.data.size()));
    if (rc == static_cast<int>(pending.data.size())) {
      napi_ref completion_req_ref = nullptr;
      if (!is_current_write) {
        completion_req_ref = pending.req_ref;
        pending.req_ref = nullptr;
      }
      wrap->pending_app_writes.pop_front();
      std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
      if (is_current_write) wrap->write_callback_scheduled = true;
      if (!encrypted.empty()) {
        QueueEncryptedWrite(wrap, std::move(encrypted), completion_req_ref);
        TryStartParentWrite(wrap);
      } else {
        QueueEncryptedWrite(wrap, std::vector<uint8_t>(), completion_req_ref, true);
      }
      made_progress = true;
      continue;
    }

    std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
    if (!encrypted.empty()) {
      QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
      TryStartParentWrite(wrap);
      made_progress = true;
    }

    if (HandleSslError(wrap, rc, "ERR_TLS_WRITE", "TLS write failed")) {
      if (!is_current_write) {
        CompleteReq(wrap, &pending.req_ref, UV_EPROTO);
      }
      wrap->pending_app_writes.pop_front();
      if (is_current_write) {
        MaybeFinishActiveWrite(wrap, UV_EPROTO);
      }
      made_progress = true;
      continue;
    }
    break;
  }

  return made_progress;
}

bool ReadCleartext(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return false;
  ncrypto::MarkPopErrorOnReturn mark_pop_error_on_return;
  bool made_progress = false;
  char buffer[16 * 1024];
  int last_err = SSL_ERROR_NONE;
  for (;;) {
    const int rc = SSL_read(wrap->ssl, buffer, sizeof(buffer));
    if (rc > 0) {
      EmitOnReadData(wrap, reinterpret_cast<const uint8_t*>(buffer), static_cast<size_t>(rc));
      made_progress = true;
      continue;
    }
    const int err = SSL_get_error(wrap->ssl, rc);
    last_err = err;
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_X509_LOOKUP) break;
    if (err == SSL_ERROR_ZERO_RETURN) {
      if (!wrap->eof) {
        wrap->eof = true;
        EmitOnReadStatus(wrap, UV_EOF);
      }
      break;
    }
    if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
      EmitError(wrap, CreateLastOpenSslError(wrap->env, "ERR_TLS_READ", "TLS read failed"));
      break;
    }
    break;
  }

  if (wrap->ssl != nullptr && last_err != SSL_ERROR_ZERO_RETURN) {
    std::vector<uint8_t> encrypted = ReadAllPendingBio(wrap->enc_out);
    if (!encrypted.empty()) {
      QueueEncryptedWrite(wrap, std::move(encrypted), nullptr);
      TryStartParentWrite(wrap);
      made_progress = true;
    }
  }
  return made_progress;
}

void Cycle(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->ssl == nullptr) return;
  if (wrap->cycle_depth++ > 0) {
    wrap->cycle_depth--;
    return;
  }

  bool keep_going = false;
  do {
    keep_going = false;
    if (TryHandshake(wrap)) keep_going = true;
    if (ReadCleartext(wrap)) keep_going = true;
    if (PumpPendingAppWrites(wrap)) keep_going = true;
    if (MaybeEmitClientOcspResponse(wrap, wrap->established)) keep_going = true;
    if (wrap->ssl != nullptr) {
      TryStartParentWrite(wrap);
      EmitHandshakeDoneIfPending(wrap);
      MaybeStartTlsShutdown(wrap);
    }
    if (!wrap->parent_write_in_progress && wrap->pending_encrypted_writes.empty()) {
      MaybeFinishActiveWrite(wrap, 0);
    }
  } while (keep_going && wrap->ssl != nullptr);

  wrap->cycle_depth--;
}

void InitSsl(TlsWrap* wrap) {
  if (wrap == nullptr || wrap->secure_context == nullptr || wrap->secure_context->ctx == nullptr) return;
  SSL_CTX_set_tlsext_status_cb(wrap->secure_context->ctx, TLSExtStatusCallback);
  SSL_CTX_set_tlsext_status_arg(wrap->secure_context->ctx, nullptr);
  wrap->ssl = SSL_new(wrap->secure_context->ctx);
  if (wrap->ssl == nullptr) return;
  SSL_CTX_sess_set_new_cb(wrap->secure_context->ctx, NewSessionCallback);
  SSL_CTX_sess_set_get_cb(wrap->secure_context->ctx, GetSessionCallback);
  wrap->enc_in = BIO_new(BIO_s_mem());
  wrap->enc_out = BIO_new(BIO_s_mem());
  BIO_set_mem_eof_return(wrap->enc_in, -1);
  BIO_set_mem_eof_return(wrap->enc_out, -1);
  SSL_set_bio(wrap->ssl, wrap->enc_in, wrap->enc_out);
  SSL_set_app_data(wrap->ssl, wrap);
  SSL_set_info_callback(wrap->ssl, SslInfoCallback);
  SSL_set_verify(wrap->ssl, SSL_VERIFY_NONE, VerifyCallback);
#ifdef SSL_MODE_RELEASE_BUFFERS
  SSL_set_mode(wrap->ssl, SSL_MODE_RELEASE_BUFFERS);
#endif
  SSL_set_mode(wrap->ssl, SSL_MODE_AUTO_RETRY);
  SSL_set_cert_cb(wrap->ssl, CertCallback, wrap);
  if (wrap->is_server) {
    SSL_set_accept_state(wrap->ssl);
  } else {
    SSL_set_connect_state(wrap->ssl);
  }
}

napi_value ForwardParentRead(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value parent = nullptr;
  napi_get_cb_info(env, info, &argc, argv, &parent, nullptr);
  TlsWrap* wrap = FindWrapByParent(env, parent);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);

  int32_t* state = UbiGetStreamBaseState(env);
  const int32_t nread = state != nullptr ? state[kUbiReadBytesOrError] : 0;
  const int32_t offset = state != nullptr ? state[kUbiArrayBufferOffset] : 0;

  if (nread <= 0) {
    if (HandshakeCallbacksReady(wrap)) {
      MaybeProcessDeferredParentInput(wrap);
    }
    if (nread < 0 && wrap->ssl != nullptr) {
      if (nread == UV_EOF) {
        wrap->eof = true;
      }
      EmitOnReadStatus(wrap, nread);
      Cycle(wrap);
    }
    return Undefined(env);
  }

  const uint8_t* bytes = nullptr;
  size_t len = 0;
  size_t byte_offset = 0;
  if (!GetArrayBufferBytes(env, argv[0], &bytes, &len, &byte_offset) || bytes == nullptr) return Undefined(env);

  size_t final_offset = byte_offset + static_cast<size_t>(offset >= 0 ? offset : 0);
  if (final_offset > len) return Undefined(env);
  size_t final_len = static_cast<size_t>(nread);
  if (final_offset + final_len > len) {
    final_len = len - final_offset;
  }

  if (!HandshakeCallbacksReady(wrap)) {
    if (final_len > 0) {
      wrap->deferred_parent_input.insert(
          wrap->deferred_parent_input.end(), bytes + final_offset, bytes + final_offset + final_len);
    }
    return Undefined(env);
  }

  if (!wrap->hello_parser.IsEnded()) {
    if (final_len > 0) {
      wrap->deferred_parent_input.insert(
          wrap->deferred_parent_input.end(), bytes + final_offset, bytes + final_offset + final_len);
      wrap->hello_parser.Parse(wrap, wrap->deferred_parent_input.data(), wrap->deferred_parent_input.size());
      if (wrap->hello_parser.IsEnded()) {
        FlushDeferredParentInput(wrap);
        Cycle(wrap);
      }
    }
    return Undefined(env);
  }

  if (final_len > 0) {
    (void)BIO_write(wrap->enc_in, bytes + final_offset, static_cast<int>(final_len));
  }
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  if (self == nullptr) return Undefined(env);

  auto* wrap = new TlsWrap();
  wrap->env = env;
  wrap->async_id = EnsureState(env).next_async_id++;
  if (napi_wrap(env, self, wrap, TlsWrapFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    delete wrap;
    return Undefined(env);
  }

  EnsureState(env).wraps.push_back(wrap);
  napi_set_named_property(env, self, "isStreamBase", MakeBool(env, true));
  napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return self;
}

napi_value TlsWrapReadStart(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, &self);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  if (HandshakeCallbacksReady(wrap)) {
    FlushDeferredParentInput(wrap);
    Cycle(wrap);
  }
  const int32_t rc = CallParentMethodInt(wrap, "readStart", 0, nullptr, nullptr);
  if (self != nullptr) napi_set_named_property(env, self, "reading", MakeBool(env, rc == 0));
  return MakeInt32(env, rc);
}

napi_value TlsWrapReadStop(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, &self);
  if (wrap == nullptr) return MakeInt32(env, UV_EINVAL);
  const int32_t rc = CallParentMethodInt(wrap, "readStop", 0, nullptr, nullptr);
  if (self != nullptr) napi_set_named_property(env, self, "reading", MakeBool(env, false));
  return MakeInt32(env, rc);
}

int32_t QueueAppWrite(TlsWrap* wrap, napi_value req_obj, const uint8_t* data, size_t len) {
  if (wrap == nullptr || wrap->ssl == nullptr) return UV_EINVAL;
  PendingAppWrite pending;
  if (req_obj != nullptr) napi_create_reference(wrap->env, req_obj, 1, &pending.req_ref);
  pending.data.assign(data, data + len);
  wrap->pending_app_writes.push_back(std::move(pending));
  SetState(wrap->env, kUbiBytesWritten, static_cast<int32_t>(len));
  SetState(wrap->env, kUbiLastWriteWasAsync, 1);
  Cycle(wrap);
  SetState(wrap->env, kUbiBytesWritten, static_cast<int32_t>(len));
  SetState(wrap->env, kUbiLastWriteWasAsync, 1);
  return 0;
}

napi_value TlsWrapWriteBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (!UbiStreamBaseExtractByteSpan(env, argv[1], &data, &len, &refable, &temp_utf8) || data == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  return MakeInt32(env, QueueAppWrite(wrap, argv[0], data, len));
}

napi_value TlsWrapWriteString(napi_env env, napi_callback_info info, const char* encoding_name) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  napi_value payload = argv[1];
  if (encoding_name != nullptr) {
    napi_value encoding = nullptr;
    if (napi_create_string_utf8(env, encoding_name, NAPI_AUTO_LENGTH, &encoding) == napi_ok && encoding != nullptr) {
      payload = UbiStreamBufferFromWithEncoding(env, argv[1], encoding);
    }
  }
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (!UbiStreamBaseExtractByteSpan(env, payload, &data, &len, &refable, &temp_utf8) || data == nullptr) {
    return MakeInt32(env, UV_EINVAL);
  }
  return MakeInt32(env, QueueAppWrite(wrap, argv[0], data, len));
}

napi_value TlsWrapWriteLatin1String(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "latin1");
}

napi_value TlsWrapWriteUtf8String(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "utf8");
}

napi_value TlsWrapWriteAsciiString(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "ascii");
}

napi_value TlsWrapWriteUcs2String(napi_env env, napi_callback_info info) {
  return TlsWrapWriteString(env, info, "ucs2");
}

napi_value TlsWrapWritev(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 2) return MakeInt32(env, UV_EINVAL);
  bool all_buffers = false;
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_bool(env, argv[2], &all_buffers);
  uint32_t raw_len = 0;
  if (napi_get_array_length(env, argv[1], &raw_len) != napi_ok) return MakeInt32(env, UV_EINVAL);
  const uint32_t count = all_buffers ? raw_len : (raw_len / 2);
  std::vector<uint8_t> combined;
  for (uint32_t i = 0; i < count; ++i) {
    napi_value chunk = nullptr;
    napi_get_element(env, argv[1], all_buffers ? i : (i * 2), &chunk);
    const uint8_t* data = nullptr;
    size_t len = 0;
    bool refable = false;
    std::string temp_utf8;
    if (!UbiStreamBaseExtractByteSpan(env, chunk, &data, &len, &refable, &temp_utf8) || data == nullptr) {
      return MakeInt32(env, UV_EINVAL);
    }
    combined.insert(combined.end(), data, data + len);
  }
  return MakeInt32(env, QueueAppWrite(wrap, argv[0], combined.data(), combined.size()));
}

napi_value TlsWrapShutdown(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 1) return MakeInt32(env, UV_EINVAL);
  if (wrap->shutdown_started || wrap->pending_shutdown_req_ref != nullptr) {
    return MakeInt32(env, 0);
  }
  if (wrap->ssl == nullptr) {
    wrap->shutdown_started = true;
    const int32_t rc = CallParentMethodInt(wrap, "shutdown", 1, argv, nullptr);
    if (rc != 0) wrap->shutdown_started = false;
    return MakeInt32(env, rc);
  }

  if (napi_create_reference(env, argv[0], 1, &wrap->pending_shutdown_req_ref) != napi_ok) {
    return MakeInt32(env, UV_ENOMEM);
  }
  MaybeStartTlsShutdown(wrap);
  return MakeInt32(env, 0);
}

napi_value TlsWrapClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr) return Undefined(env);
  DestroySsl(wrap);
  (void)CallParentMethodInt(wrap, "close", argc, argv, nullptr);
  return Undefined(env);
}

napi_value TlsWrapRef(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->refed = true;
    SyncKeepaliveRef(wrap);
    (void)CallParentMethodInt(wrap, "ref", 0, nullptr, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapUnref(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->refed = false;
    SyncKeepaliveRef(wrap);
    (void)CallParentMethodInt(wrap, "unref", 0, nullptr, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapGetAsyncId(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  return MakeInt64(env, wrap != nullptr ? wrap->async_id : -1);
}

napi_value TlsWrapGetProviderType(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value result = nullptr;
  int32_t rc = wrap == nullptr ? UV_EINVAL : CallParentMethodInt(wrap, "getProviderType", 0, nullptr, &result);
  if (wrap == nullptr || rc == UV_EINVAL || result == nullptr) return MakeInt32(env, 0);
  return result;
}

napi_value TlsWrapAsyncReset(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->async_id = EnsureState(env).next_async_id++;
  }
  return Undefined(env);
}

napi_value TlsWrapUseUserBuffer(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap != nullptr && argc >= 1) {
    bool is_buffer = false;
    if (napi_is_buffer(env, argv[0], &is_buffer) == napi_ok && is_buffer) {
      void* raw = nullptr;
      size_t len = 0;
      if (napi_get_buffer_info(env, argv[0], &raw, &len) == napi_ok && raw != nullptr && len != 0) {
        DeleteRefIfPresent(env, &wrap->user_read_buffer_ref);
        if (napi_create_reference(env, argv[0], 1, &wrap->user_read_buffer_ref) == napi_ok) {
          wrap->user_buffer_base = static_cast<char*>(raw);
          wrap->user_buffer_len = len;
        }
      }
    } else {
      bool is_typedarray = false;
      if (napi_is_typedarray(env, argv[0], &is_typedarray) == napi_ok && is_typedarray) {
        napi_typedarray_type ta_type = napi_uint8_array;
        size_t element_len = 0;
        void* raw = nullptr;
        napi_value arraybuffer = nullptr;
        size_t byte_offset = 0;
        if (napi_get_typedarray_info(
                env, argv[0], &ta_type, &element_len, &raw, &arraybuffer, &byte_offset) == napi_ok &&
            raw != nullptr &&
            element_len != 0) {
          DeleteRefIfPresent(env, &wrap->user_read_buffer_ref);
          if (napi_create_reference(env, argv[0], 1, &wrap->user_read_buffer_ref) == napi_ok) {
            wrap->user_buffer_base = static_cast<char*>(raw);
            wrap->user_buffer_len = element_len * UbiTypedArrayElementSize(ta_type);
          }
        }
      }
    }
  }
  return Undefined(env);
}

napi_value TlsWrapBytesReadGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  napi_value parent = wrap != nullptr ? GetRefValue(env, wrap->parent_ref) : nullptr;
  napi_value out = GetNamedValue(env, parent, "bytesRead");
  return out != nullptr ? out : MakeInt32(env, 0);
}

napi_value TlsWrapBytesWrittenGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  napi_value parent = wrap != nullptr ? GetRefValue(env, wrap->parent_ref) : nullptr;
  napi_value out = GetNamedValue(env, parent, "bytesWritten");
  return out != nullptr ? out : MakeInt32(env, 0);
}

napi_value TlsWrapFdGetter(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);
  napi_value parent = wrap != nullptr ? GetRefValue(env, wrap->parent_ref) : nullptr;
  napi_value out = GetNamedValue(env, parent, "fd");
  return out != nullptr ? out : MakeInt32(env, -1);
}

napi_value TlsWrapSetVerifyMode(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 2) return Undefined(env);
  bool request_cert = false;
  bool reject_unauthorized = false;
  napi_get_value_bool(env, argv[0], &request_cert);
  napi_get_value_bool(env, argv[1], &reject_unauthorized);
  wrap->request_cert = request_cert;
  wrap->reject_unauthorized = reject_unauthorized;

  int verify_mode = SSL_VERIFY_NONE;
  if (wrap->is_server) {
    if (request_cert) {
      verify_mode = SSL_VERIFY_PEER;
      if (reject_unauthorized) verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }
  }
  SSL_set_verify(wrap->ssl, verify_mode, VerifyCallback);
  return Undefined(env);
}

napi_value TlsWrapEnableTrace(napi_env env, napi_callback_info info) {
  (void)env;
  (void)info;
  return Undefined(env);
}

napi_value TlsWrapEnableSessionCallbacks(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->session_callbacks_enabled) return Undefined(env);
  wrap->session_callbacks_enabled = true;
  if (wrap->is_server) {
    wrap->hello_parser.Start(OnClientHello);
  } else if (wrap->established && wrap->client_session_event_count == 0 &&
             SSL_version(wrap->ssl) < TLS1_3_VERSION &&
             SSL_session_reused(wrap->ssl) != 1) {
    SSL_SESSION* session = SSL_get_session(wrap->ssl);
    if (session != nullptr) {
      (void)NewSessionCallback(wrap->ssl, session);
    }
  }
  return Undefined(env);
}

napi_value TlsWrapEnableCertCb(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->waiting_cert_cb = true;
    wrap->cert_cb = ResumeCertCallback;
    wrap->cert_cb_arg = wrap;
  }
  return Undefined(env);
}

napi_value TlsWrapEnableALPNCb(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr) {
    wrap->alpn_callback_enabled = true;
    SSL_CTX_set_alpn_select_cb(SSL_get_SSL_CTX(wrap->ssl), SelectALPNCallback, nullptr);
  }
  return Undefined(env);
}

napi_value TlsWrapEnablePskCallback(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr) {
    SSL_set_psk_server_callback(wrap->ssl, PskServerCallback);
    SSL_set_psk_client_callback(wrap->ssl, PskClientCallback);
  }
  return Undefined(env);
}

napi_value TlsWrapSetPskIdentityHint(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const std::string hint = ValueToUtf8(env, argv[0]);
  if (!hint.empty() && SSL_use_psk_identity_hint(wrap->ssl, hint.c_str()) != 1) {
    EmitError(wrap,
              CreateErrorWithCode(env,
                                  "ERR_TLS_PSK_SET_IDENTITY_HINT_FAILED",
                                  "Failed to set PSK identity hint"));
  }
  return Undefined(env);
}

napi_value TlsWrapEnableKeylogCallback(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr) {
    SSL_CTX_set_keylog_callback(SSL_get_SSL_CTX(wrap->ssl), KeylogCallback);
  }
  return Undefined(env);
}

napi_value TlsWrapWritesIssuedByPrevListenerDone(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr) {
    wrap->has_active_write_issued_by_prev_listener = false;
    TryStartParentWrite(wrap);
    Cycle(wrap);
  }
  return Undefined(env);
}

napi_value TlsWrapSetALPNProtocols(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (!UbiStreamBaseExtractByteSpan(env, argv[0], &data, &len, &refable, &temp_utf8) || data == nullptr) {
    napi_throw_type_error(env, nullptr, "Must give a Buffer as first argument");
    return nullptr;
  }
  if (wrap->is_server) {
    wrap->alpn_protos.assign(data, data + len);
    SSL_CTX_set_alpn_select_cb(SSL_get_SSL_CTX(wrap->ssl), SelectALPNCallback, nullptr);
  } else {
    (void)SSL_set_alpn_protos(wrap->ssl, data, static_cast<unsigned int>(len));
  }
  return Undefined(env);
}

napi_value TlsWrapRequestOCSP(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap != nullptr && wrap->ssl != nullptr && !wrap->is_server) {
    SSL_set_tlsext_status_type(wrap->ssl, TLSEXT_STATUSTYPE_ocsp);
  }
  return Undefined(env);
}

napi_value TlsWrapStart(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr) return Undefined(env);
  wrap->started = true;
  EnsureKeepaliveHandle(wrap);
  SyncKeepaliveRef(wrap);
  if (wrap->is_server && !wrap->session_callbacks_enabled) {
    napi_value self = GetRefValue(env, wrap->wrapper_ref);
    if (self != nullptr &&
        (IsFunction(env, GetNamedValue(env, self, "onclienthello")) ||
         IsFunction(env, GetNamedValue(env, self, "onnewsession")))) {
      wrap->session_callbacks_enabled = true;
      wrap->hello_parser.Start(OnClientHello);
    }
  }
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapRenegotiate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  ncrypto::ClearErrorOnReturn clear_error_on_return;
#ifndef OPENSSL_IS_BORINGSSL
  if (SSL_renegotiate(wrap->ssl) != 1) {
    napi_throw(env, CreateLastOpenSslError(env, nullptr, "TLS renegotiation failed"));
    return nullptr;
  }
#endif
  return Undefined(env);
}

napi_value TlsWrapSetServername(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const std::string servername = ValueToUtf8(env, argv[0]);
  if (!servername.empty()) {
    (void)SSL_set_tlsext_host_name(wrap->ssl, servername.c_str());
  }
  return Undefined(env);
}

napi_value TlsWrapGetServername(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return MakeBool(env, false);
  const char* servername = SSL_get_servername(wrap->ssl, TLSEXT_NAMETYPE_host_name);
  if (servername == nullptr) return MakeBool(env, false);
  napi_value out = nullptr;
  napi_create_string_utf8(env, servername, NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : MakeBool(env, false);
}

bool LoadSessionBytes(TlsWrap* wrap, const uint8_t* data, size_t len) {
  if (wrap == nullptr || wrap->ssl == nullptr || data == nullptr || len == 0) return false;
  const unsigned char* ptr = data;
  SSL_SESSION* session = d2i_SSL_SESSION(nullptr, &ptr, static_cast<long>(len));
  if (session == nullptr) return false;
  const int rc = SSL_set_session(wrap->ssl, session);
  SSL_SESSION_free(session);
  return rc == 1;
}

napi_value TlsWrapSetSession(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 1) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  bool refable = false;
  std::string temp_utf8;
  if (UbiStreamBaseExtractByteSpan(env, argv[0], &data, &len, &refable, &temp_utf8) && data != nullptr) {
    wrap->pending_session.assign(data, data + len);
    if (wrap->is_server) {
      const unsigned char* ptr = wrap->pending_session.data();
      SSL_SESSION* session = d2i_SSL_SESSION(nullptr, &ptr, static_cast<long>(wrap->pending_session.size()));
      if (wrap->next_session != nullptr) SSL_SESSION_free(wrap->next_session);
      wrap->next_session = session;
    } else if (wrap->ssl != nullptr) {
      (void)LoadSessionBytes(wrap, wrap->pending_session.data(), wrap->pending_session.size());
    }
  }
  return Undefined(env);
}

napi_value TlsWrapLoadSession(napi_env env, napi_callback_info info) {
  return TlsWrapSetSession(env, info);
}

napi_value TlsWrapGetSession(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  SSL_SESSION* session = SSL_get_session(wrap->ssl);
  if (session == nullptr) return Undefined(env);
  const int size = i2d_SSL_SESSION(session, nullptr);
  if (size <= 0) return Undefined(env);
  std::vector<uint8_t> out(static_cast<size_t>(size));
  unsigned char* ptr = out.data();
  if (i2d_SSL_SESSION(session, &ptr) != size) return Undefined(env);
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapExportKeyingMaterial(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 2) return Undefined(env);
  uint32_t length = 0;
  napi_get_value_uint32(env, argv[0], &length);
  const std::string label = ValueToUtf8(env, argv[1]);
  std::vector<uint8_t> context_bytes;
  const uint8_t* context_data = nullptr;
  size_t context_len = 0;
  if (argc >= 3 && argv[2] != nullptr) {
    bool refable = false;
    std::string temp_utf8;
    if (UbiStreamBaseExtractByteSpan(env, argv[2], &context_data, &context_len, &refable, &temp_utf8) &&
        context_data != nullptr) {
      context_bytes.assign(context_data, context_data + context_len);
      context_data = context_bytes.data();
    }
  }
  std::vector<uint8_t> out(length, 0);
  if (SSL_export_keying_material(wrap->ssl,
                                 out.data(),
                                 out.size(),
                                 label.c_str(),
                                 label.size(),
                                 context_bytes.empty() ? nullptr : context_data,
                                 context_len,
                                 context_bytes.empty() ? 0 : 1) != 1) {
    EmitError(wrap, CreateLastOpenSslError(env, "ERR_TLS_EXPORT_KEYING_MATERIAL", "Key export failed"));
    return Undefined(env);
  }
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapSetMaxSendFragment(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return MakeInt32(env, 0);
#ifdef SSL_set_max_send_fragment
  uint32_t value = 0;
  napi_get_value_uint32(env, argv[0], &value);
  return MakeInt32(env, SSL_set_max_send_fragment(wrap->ssl, value) == 1 ? 1 : 0);
#else
  return MakeInt32(env, 1);
#endif
}

napi_value TlsWrapGetALPNNegotiatedProtocol(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return MakeBool(env, false);
  const unsigned char* data = nullptr;
  unsigned int len = 0;
  SSL_get0_alpn_selected(wrap->ssl, &data, &len);
  if (data == nullptr || len == 0) return MakeBool(env, false);
  napi_value out = nullptr;
  napi_create_string_utf8(env, reinterpret_cast<const char*>(data), len, &out);
  return out != nullptr ? out : MakeBool(env, false);
}

napi_value TlsWrapGetCipher(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  const SSL_CIPHER* cipher = SSL_get_current_cipher(wrap->ssl);
  if (cipher == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_object(env, &out);
  napi_value name = nullptr;
  napi_value standard_name = nullptr;
  napi_value version = nullptr;
  const char* cipher_name = SSL_CIPHER_get_name(cipher);
  const char* cipher_standard_name = SSL_CIPHER_standard_name(cipher);
  const char* cipher_version = SSL_CIPHER_get_version(cipher);
  if (cipher_name != nullptr) napi_create_string_utf8(env, cipher_name, NAPI_AUTO_LENGTH, &name);
  if (cipher_standard_name != nullptr) {
    napi_create_string_utf8(env, cipher_standard_name, NAPI_AUTO_LENGTH, &standard_name);
  } else if (cipher_name != nullptr) {
    napi_create_string_utf8(env, cipher_name, NAPI_AUTO_LENGTH, &standard_name);
  }
  if (cipher_version != nullptr) napi_create_string_utf8(env, cipher_version, NAPI_AUTO_LENGTH, &version);
  if (name != nullptr) napi_set_named_property(env, out, "name", name);
  if (standard_name != nullptr) napi_set_named_property(env, out, "standardName", standard_name);
  if (version != nullptr) napi_set_named_property(env, out, "version", version);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetSharedSigalgs(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value out = nullptr;
  napi_create_array(env, &out);
  if (wrap == nullptr || wrap->ssl == nullptr) return out != nullptr ? out : Undefined(env);
  int nsig = SSL_get_shared_sigalgs(wrap->ssl, 0, nullptr, nullptr, nullptr, nullptr, nullptr);
  for (int i = 0; i < nsig; ++i) {
    int sign_nid = NID_undef;
    int hash_nid = NID_undef;
    if (SSL_get_shared_sigalgs(wrap->ssl, i, &sign_nid, &hash_nid, nullptr, nullptr, nullptr) <= 0) continue;

    std::string sig_with_md;
    switch (sign_nid) {
      case EVP_PKEY_RSA:
        sig_with_md = "RSA+";
        break;
      case EVP_PKEY_RSA_PSS:
        sig_with_md = "RSA-PSS+";
        break;
      case EVP_PKEY_DSA:
        sig_with_md = "DSA+";
        break;
      case EVP_PKEY_EC:
        sig_with_md = "ECDSA+";
        break;
      case NID_ED25519:
        sig_with_md = "Ed25519+";
        break;
      case NID_ED448:
        sig_with_md = "Ed448+";
        break;
#ifndef OPENSSL_NO_GOST
      case NID_id_GostR3410_2001:
        sig_with_md = "gost2001+";
        break;
      case NID_id_GostR3410_2012_256:
        sig_with_md = "gost2012_256+";
        break;
      case NID_id_GostR3410_2012_512:
        sig_with_md = "gost2012_512+";
        break;
#endif
      default: {
        const char* sign_name = OBJ_nid2sn(sign_nid);
        sig_with_md = sign_name != nullptr ? std::string(sign_name) + "+" : "UNDEF+";
        break;
      }
    }

    const char* hash_name = OBJ_nid2sn(hash_nid);
    sig_with_md += hash_name != nullptr ? hash_name : "UNDEF";

    napi_value value = nullptr;
    napi_create_string_utf8(env, sig_with_md.c_str(), NAPI_AUTO_LENGTH, &value);
    napi_set_element(env, out, i, value);
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetEphemeralKeyInfo(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  napi_value out = nullptr;
  napi_create_object(env, &out);
  if (wrap == nullptr || wrap->ssl == nullptr) return out != nullptr ? out : Undefined(env);
  if (wrap->is_server) return Null(env);
  EVP_PKEY* key = nullptr;
  if (SSL_get_peer_tmp_key(wrap->ssl, &key) != 1 || key == nullptr) return out != nullptr ? out : Undefined(env);
  const int key_id = EVP_PKEY_id(key);
  const int bits = EVP_PKEY_bits(key);
  const char* key_type = nullptr;
  const char* key_name = nullptr;
  if (key_id == EVP_PKEY_DH) {
    key_type = "DH";
  } else if (key_id == EVP_PKEY_EC || key_id == EVP_PKEY_X25519 || key_id == EVP_PKEY_X448) {
    key_type = "ECDH";
    if (key_id == EVP_PKEY_EC) {
      EC_KEY* ec = EVP_PKEY_get1_EC_KEY(key);
      if (ec != nullptr) {
        const EC_GROUP* group = EC_KEY_get0_group(ec);
        if (group != nullptr) {
          key_name = OBJ_nid2sn(EC_GROUP_get_curve_name(group));
        }
        EC_KEY_free(ec);
      }
    } else {
      key_name = OBJ_nid2sn(key_id);
    }
  }
  if (key_type != nullptr) {
    napi_value type = nullptr;
    napi_create_string_utf8(env, key_type, NAPI_AUTO_LENGTH, &type);
    if (type != nullptr) napi_set_named_property(env, out, "type", type);
  }
  if (key_name != nullptr && key_name[0] != '\0') {
    napi_value name = nullptr;
    napi_create_string_utf8(env, key_name, NAPI_AUTO_LENGTH, &name);
    if (name != nullptr) napi_set_named_property(env, out, "name", name);
  }
  if (bits > 0) {
    napi_set_named_property(env, out, "size", MakeInt32(env, bits));
  }
  EVP_PKEY_free(key);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetFinished(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  char dummy[1];
  const size_t len = SSL_get_finished(wrap->ssl, dummy, sizeof(dummy));
  if (len == 0) return Undefined(env);
  std::vector<uint8_t> out(len);
  (void)SSL_get_finished(wrap->ssl, out.data(), out.size());
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapGetPeerFinished(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  char dummy[1];
  const size_t len = SSL_get_peer_finished(wrap->ssl, dummy, sizeof(dummy));
  if (len == 0) return Undefined(env);
  std::vector<uint8_t> out(len);
  (void)SSL_get_peer_finished(wrap->ssl, out.data(), out.size());
  napi_value buffer = CreateBufferCopy(env, out.data(), out.size());
  return buffer != nullptr ? buffer : Undefined(env);
}

napi_value TlsWrapGetProtocol(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  const char* version = SSL_get_version(wrap->ssl);
  if (version == nullptr) return Undefined(env);
  napi_value out = nullptr;
  napi_create_string_utf8(env, version, NAPI_AUTO_LENGTH, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetTLSTicket(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);

  SSL_SESSION* session = SSL_get_session(wrap->ssl);
  if (session == nullptr) return Undefined(env);

  const unsigned char* ticket = nullptr;
  size_t length = 0;
  SSL_SESSION_get0_ticket(session, &ticket, &length);
  if (ticket == nullptr) return Undefined(env);

  napi_value out = nullptr;
  if (napi_create_buffer_copy(env, length, ticket, nullptr, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  return out;
}

napi_value TlsWrapIsSessionReused(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  return MakeBool(env, wrap != nullptr && wrap->ssl != nullptr && SSL_session_reused(wrap->ssl) == 1);
}

bool AppendX509NameEntry(napi_env env, napi_value target, int nid, const std::string& value) {
  const char* key = OBJ_nid2sn(nid);
  if (key == nullptr) return true;
  napi_value current = GetNamedValue(env, target, key);
  napi_value str = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), value.size(), &str) != napi_ok || str == nullptr) return false;
  if (current == nullptr || current == Undefined(env) || current == Null(env)) {
    return napi_set_named_property(env, target, key, str) == napi_ok;
  }
  bool is_array = false;
  if (napi_is_array(env, current, &is_array) == napi_ok && is_array) {
    uint32_t length = 0;
    napi_get_array_length(env, current, &length);
    napi_set_element(env, current, length, str);
    return true;
  }
  napi_value arr = nullptr;
  if (napi_create_array_with_length(env, 2, &arr) != napi_ok || arr == nullptr) return false;
  napi_set_element(env, arr, 0, current);
  napi_set_element(env, arr, 1, str);
  return napi_set_named_property(env, target, key, arr) == napi_ok;
}

napi_value CreateX509NameObject(napi_env env, X509_NAME* name) {
  napi_value out = nullptr;
  napi_create_object(env, &out);
  if (out == nullptr || name == nullptr) return out;
  const int count = X509_NAME_entry_count(name);
  for (int i = 0; i < count; ++i) {
    X509_NAME_ENTRY* entry = X509_NAME_get_entry(name, i);
    if (entry == nullptr) continue;
    ASN1_OBJECT* object = X509_NAME_ENTRY_get_object(entry);
    ASN1_STRING* data = X509_NAME_ENTRY_get_data(entry);
    unsigned char* utf8 = nullptr;
    const int utf8_len = ASN1_STRING_to_UTF8(&utf8, data);
    if (utf8_len < 0 || utf8 == nullptr) continue;
    std::string value(reinterpret_cast<char*>(utf8), static_cast<size_t>(utf8_len));
    OPENSSL_free(utf8);
    (void)AppendX509NameEntry(env, out, OBJ_obj2nid(object), value);
  }
  return out;
}

std::string GetSubjectAltNameString(X509* cert) {
  std::string out;
  if (cert == nullptr) return out;
  GENERAL_NAMES* names =
      static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
  if (names == nullptr) return out;
  const int count = sk_GENERAL_NAME_num(names);
  for (int i = 0; i < count; ++i) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    if (name == nullptr) continue;
    if (!out.empty()) out.append(", ");
    if (name->type == GEN_DNS) {
      const auto* dns = ASN1_STRING_get0_data(name->d.dNSName);
      const int dns_len = ASN1_STRING_length(name->d.dNSName);
      out.append("DNS:");
      out.append(reinterpret_cast<const char*>(dns), static_cast<size_t>(dns_len));
    } else if (name->type == GEN_IPADD) {
      out.append("IP Address:");
      const unsigned char* ip = ASN1_STRING_get0_data(name->d.iPAddress);
      const int ip_len = ASN1_STRING_length(name->d.iPAddress);
      char buf[INET6_ADDRSTRLEN] = {0};
      if (ip_len == 4) {
        uv_inet_ntop(AF_INET, ip, buf, sizeof(buf));
      } else if (ip_len == 16) {
        uv_inet_ntop(AF_INET6, ip, buf, sizeof(buf));
      }
      out.append(buf);
    } else {
      out.pop_back();
      out.pop_back();
    }
  }
  GENERAL_NAMES_free(names);
  return out;
}

napi_value CreateLegacyCertObject(napi_env env, X509* cert) {
  if (cert == nullptr) return Undefined(env);
  napi_value crypto_binding = ResolveInternalBinding(env, "crypto");
  if (crypto_binding == nullptr) return Undefined(env);

  napi_value parse_x509 = nullptr;
  if (napi_get_named_property(env, crypto_binding, "parseX509", &parse_x509) != napi_ok || parse_x509 == nullptr) {
    return Undefined(env);
  }

  const int der_len = i2d_X509(cert, nullptr);
  if (der_len <= 0) return Undefined(env);
  std::vector<uint8_t> der(static_cast<size_t>(der_len));
  unsigned char* ptr = der.data();
  if (i2d_X509(cert, &ptr) != der_len) return Undefined(env);

  napi_value raw = CreateBufferCopy(env, der.data(), der.size());
  if (raw == nullptr) return Undefined(env);
  napi_value handle_argv[1] = {raw};
  napi_value handle = nullptr;
  if (napi_call_function(env, crypto_binding, parse_x509, 1, handle_argv, &handle) != napi_ok || handle == nullptr) {
    return Undefined(env);
  }

  napi_value to_legacy = nullptr;
  if (napi_get_named_property(env, handle, "toLegacy", &to_legacy) != napi_ok || to_legacy == nullptr) {
    return Undefined(env);
  }
  napi_value out = nullptr;
  if (napi_call_function(env, handle, to_legacy, 0, nullptr, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }

  if (X509_check_issued(cert, cert) == X509_V_OK) {
    napi_set_named_property(env, out, "issuerCertificate", out);
  }

  return out;
}

napi_value BuildDetailedPeerCertificateObject(napi_env env, SSL* ssl, bool is_server) {
  if (ssl == nullptr) return Undefined(env);

  ncrypto::X509Pointer cert;
  if (is_server) {
    cert.reset(SSL_get_peer_certificate(ssl));
  }

  STACK_OF(X509)* ssl_certs = SSL_get_peer_cert_chain(ssl);
  if (!cert && (ssl_certs == nullptr || sk_X509_num(ssl_certs) == 0)) {
    return Undefined(env);
  }

  std::vector<ncrypto::X509Pointer> peer_certs;
  if (!cert) {
    cert.reset(X509_dup(sk_X509_value(ssl_certs, 0)));
    if (!cert) return Undefined(env);
    for (int i = 1; i < sk_X509_num(ssl_certs); ++i) {
      ncrypto::X509Pointer dup(X509_dup(sk_X509_value(ssl_certs, i)));
      if (!dup) return Undefined(env);
      peer_certs.push_back(std::move(dup));
    }
  } else if (ssl_certs != nullptr) {
    for (int i = 0; i < sk_X509_num(ssl_certs); ++i) {
      ncrypto::X509Pointer dup(X509_dup(sk_X509_value(ssl_certs, i)));
      if (!dup) return Undefined(env);
      peer_certs.push_back(std::move(dup));
    }
  }

  napi_value result = CreateLegacyCertObject(env, cert.get());
  if (result == nullptr || internal_binding::IsUndefined(env, result)) return Undefined(env);

  napi_value issuer_object = result;
  for (;;) {
    size_t match_index = peer_certs.size();
    for (size_t i = 0; i < peer_certs.size(); ++i) {
      if (cert.view().isIssuedBy(peer_certs[i].view())) {
        match_index = i;
        break;
      }
    }

    if (match_index == peer_certs.size()) break;

    napi_value next = CreateLegacyCertObject(env, peer_certs[match_index].get());
    if (next == nullptr || internal_binding::IsUndefined(env, next)) return Undefined(env);
    napi_set_named_property(env, issuer_object, "issuerCertificate", next);
    issuer_object = next;
    cert = std::move(peer_certs[match_index]);
    peer_certs.erase(peer_certs.begin() + static_cast<std::ptrdiff_t>(match_index));
  }

  while (!cert.view().isIssuedBy(cert.view())) {
    X509* prev = cert.get();
    auto issuer = ncrypto::X509Pointer::IssuerFrom(SSL_get_SSL_CTX(ssl), cert.view());
    if (!issuer) break;
    napi_value next = CreateLegacyCertObject(env, issuer.get());
    if (next == nullptr || internal_binding::IsUndefined(env, next)) return Undefined(env);
    napi_set_named_property(env, issuer_object, "issuerCertificate", next);
    issuer_object = next;
    if (issuer.get() == prev) break;
    cert = std::move(issuer);
  }

  return result;
}

napi_value TlsWrapVerifyError(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Null(env);
  const long verify_error = SSL_get_verify_result(wrap->ssl);
  if (verify_error == X509_V_OK) return Null(env);
  const char* code = ncrypto::X509Pointer::ErrorCode(static_cast<int32_t>(verify_error));
  const char* reason = X509_verify_cert_error_string(verify_error);
  return CreateErrorWithCode(env, code != nullptr ? code : "ERR_TLS_CERT", reason != nullptr ? reason
                                                                                              : "Certificate verification failed");
}

napi_value TlsWrapGetPeerCertificate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  bool detailed = false;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_bool(env, argv[0], &detailed);
  }
  if (detailed) {
    return BuildDetailedPeerCertificateObject(env, wrap->ssl, wrap->is_server);
  }

  X509* cert = SSL_get_peer_certificate(wrap->ssl);
  if (cert == nullptr) {
    STACK_OF(X509)* chain = SSL_get_peer_cert_chain(wrap->ssl);
    if (chain == nullptr || sk_X509_num(chain) == 0) return Undefined(env);
    cert = X509_dup(sk_X509_value(chain, 0));
  }
  if (cert == nullptr) return Undefined(env);
  napi_value out = CreateLegacyCertObject(env, cert);
  X509_free(cert);
  return out != nullptr ? out : Undefined(env);
}

napi_value TlsWrapGetCertificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  X509* cert = SSL_get_certificate(wrap->ssl);
  return CreateLegacyCertObject(env, cert);
}

napi_value TlsWrapGetPeerX509Certificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  X509* cert = SSL_get_peer_certificate(wrap->ssl);
  if (cert == nullptr) return Undefined(env);

  napi_value crypto_binding = ResolveInternalBinding(env, "crypto");
  if (crypto_binding == nullptr) {
    X509_free(cert);
    return Undefined(env);
  }

  napi_value parse_x509 = nullptr;
  if (napi_get_named_property(env, crypto_binding, "parseX509", &parse_x509) != napi_ok || parse_x509 == nullptr) {
    X509_free(cert);
    return Undefined(env);
  }

  const int der_len = i2d_X509(cert, nullptr);
  if (der_len <= 0) {
    X509_free(cert);
    return Undefined(env);
  }
  std::vector<uint8_t> der(static_cast<size_t>(der_len));
  unsigned char* ptr = der.data();
  if (i2d_X509(cert, &ptr) != der_len) {
    X509_free(cert);
    return Undefined(env);
  }

  std::vector<uint8_t> issuer_der;
  STACK_OF(X509)* chain = SSL_get_peer_cert_chain(wrap->ssl);
  if (chain == nullptr) {
#if OPENSSL_VERSION_MAJOR >= 3
    chain = SSL_get0_verified_chain(wrap->ssl);
#endif
  }
  if (chain != nullptr) {
    const int count = sk_X509_num(chain);
    for (int i = 0; i < count; ++i) {
      X509* candidate = sk_X509_value(chain, i);
      if (candidate == nullptr) continue;
      if (X509_NAME_cmp(X509_get_subject_name(candidate), X509_get_issuer_name(cert)) != 0) continue;
      const int issuer_len = i2d_X509(candidate, nullptr);
      if (issuer_len <= 0) continue;
      issuer_der.resize(static_cast<size_t>(issuer_len));
      unsigned char* issuer_ptr = issuer_der.data();
      if (i2d_X509(candidate, &issuer_ptr) != issuer_len) {
        issuer_der.clear();
      }
      break;
    }
  }
  X509_free(cert);

  napi_value raw = CreateBufferCopy(env, der.data(), der.size());
  if (raw == nullptr) return Undefined(env);
  napi_value handle_argv[2] = {raw, Undefined(env)};
  size_t handle_argc = 1;
  if (!issuer_der.empty()) {
    handle_argv[1] = CreateBufferCopy(env, issuer_der.data(), issuer_der.size());
    if (handle_argv[1] != nullptr) {
      handle_argc = 2;
    }
  }
  napi_value out = nullptr;
  if (napi_call_function(env, crypto_binding, parse_x509, handle_argc, handle_argv, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  return out;
}

napi_value TlsWrapGetX509Certificate(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  X509* cert = SSL_get_certificate(wrap->ssl);
  if (cert == nullptr) return Undefined(env);

  napi_value crypto_binding = ResolveInternalBinding(env, "crypto");
  if (crypto_binding == nullptr) return Undefined(env);

  napi_value parse_x509 = nullptr;
  if (napi_get_named_property(env, crypto_binding, "parseX509", &parse_x509) != napi_ok || parse_x509 == nullptr) {
    return Undefined(env);
  }

  const int der_len = i2d_X509(cert, nullptr);
  if (der_len <= 0) return Undefined(env);
  std::vector<uint8_t> der(static_cast<size_t>(der_len));
  unsigned char* ptr = der.data();
  if (i2d_X509(cert, &ptr) != der_len) return Undefined(env);

  napi_value raw = CreateBufferCopy(env, der.data(), der.size());
  if (raw == nullptr) return Undefined(env);
  napi_value handle_argv[1] = {raw};
  napi_value out = nullptr;
  if (napi_call_function(env, crypto_binding, parse_x509, 1, handle_argv, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }
  return out;
}

napi_value TlsWrapSetKeyCert(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || wrap->is_server == false || argc < 1) return Undefined(env);
  ubi::crypto::SecureContextHolder* holder = nullptr;
  if (internal_binding::UbiCryptoGetSecureContextHolderFromObject(env, argv[0], &holder) && holder != nullptr) {
    if (!SetSecureContextOnSsl(wrap, holder)) {
      EmitError(wrap, CreateLastOpenSslError(env, "ERR_TLS_INVALID_CONTEXT", "Failed to update secure context"));
    }
  } else {
    napi_throw_type_error(env, nullptr, "Must give a SecureContext as first argument");
    return nullptr;
  }
  return Undefined(env);
}

napi_value TlsWrapDestroySSL(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  DestroySsl(wrap);
  return Undefined(env);
}

napi_value TlsWrapEndParser(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr) return Undefined(env);
  wrap->hello_parser.End();
  MaybeProcessDeferredParentInput(wrap);
  TryStartParentWrite(wrap);
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapSetOCSPResponse(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  if (!GetArrayBufferViewSpan(env, argv[0], &data, &len) || data == nullptr) {
    napi_throw_type_error(env, nullptr, "OCSP response must be a Buffer");
    return nullptr;
  }
  wrap->ocsp_response.assign(data, data + len);
  return Undefined(env);
}

napi_value TlsWrapCertCbDone(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr) return Undefined(env);
  napi_value self = GetRefValue(env, wrap->wrapper_ref);
  napi_value sni_context = GetNamedValue(env, self, "sni_context");
  ubi::crypto::SecureContextHolder* holder = nullptr;
  napi_valuetype sni_type = napi_undefined;
  if (sni_context != nullptr &&
      napi_typeof(env, sni_context, &sni_type) == napi_ok &&
      sni_type == napi_object) {
    if (internal_binding::UbiCryptoGetSecureContextHolderFromObject(env, sni_context, &holder) && holder != nullptr) {
      if (!SetSecureContextOnSsl(wrap, holder)) {
        EmitError(wrap, CreateLastOpenSslError(env, "ERR_TLS_INVALID_CONTEXT", "Failed to set SNI context"));
        return Undefined(env);
      }
    } else {
      napi_value code = nullptr;
      napi_value message = nullptr;
      napi_value error = nullptr;
      napi_create_string_utf8(env, "ERR_TLS_INVALID_CONTEXT", NAPI_AUTO_LENGTH, &code);
      napi_create_string_utf8(env, "Invalid SNI context", NAPI_AUTO_LENGTH, &message);
      napi_create_type_error(env, code, message, &error);
      if (error != nullptr && code != nullptr) {
        napi_set_named_property(env, error, "code", code);
      }
      EmitError(wrap, error);
      return Undefined(env);
    }
  }
  TlsCertCb cb = wrap->cert_cb;
  void* cb_arg = wrap->cert_cb_arg;
  wrap->cert_cb_running = false;
  wrap->waiting_cert_cb = false;
  wrap->cert_cb = nullptr;
  wrap->cert_cb_arg = nullptr;
  if (cb != nullptr) {
    cb(cb_arg);
  } else {
    Cycle(wrap);
  }
  return Undefined(env);
}

napi_value TlsWrapNewSessionDone(napi_env env, napi_callback_info info) {
  TlsWrap* wrap = UnwrapThis(env, info, nullptr, nullptr, nullptr);
  if (wrap == nullptr) return Undefined(env);
  wrap->awaiting_new_session = false;
  TryStartParentWrite(wrap);
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapReceive(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  TlsWrap* wrap = UnwrapThis(env, info, &argc, argv, nullptr);
  if (wrap == nullptr || wrap->ssl == nullptr || argc < 1) return Undefined(env);
  const uint8_t* data = nullptr;
  size_t len = 0;
  size_t offset = 0;
  if (!GetArrayBufferBytes(env, argv[0], &data, &len, &offset) || data == nullptr) return Undefined(env);
  if (len > offset) {
    (void)BIO_write(wrap->enc_in, data + offset, static_cast<int>(len - offset));
  }
  Cycle(wrap);
  return Undefined(env);
}

napi_value TlsWrapGetWriteQueueSize(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  TlsWrap* wrap = UnwrapTlsWrap(env, self);

  uint32_t size = 0;
  if (wrap != nullptr) {
    for (const auto& pending : wrap->pending_encrypted_writes) {
      size += static_cast<uint32_t>(pending.data.size());
    }
  }

  napi_value out = nullptr;
  napi_create_uint32(env, size, &out);
  return out;
}

napi_value TlsWrapWrap(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4] = {nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2 || argv[0] == nullptr || argv[1] == nullptr) return Undefined(env);

  ubi::crypto::SecureContextHolder* holder = nullptr;
  if (!internal_binding::UbiCryptoGetSecureContextHolderFromObject(env, argv[1], &holder) || holder == nullptr) {
    napi_throw_type_error(env, nullptr, "SecureContext required");
    return nullptr;
  }

  napi_value ctor = GetRefValue(env, EnsureState(env).tls_wrap_ctor_ref);
  napi_value out = nullptr;
  if (ctor == nullptr || napi_new_instance(env, ctor, 0, nullptr, &out) != napi_ok || out == nullptr) {
    return Undefined(env);
  }

  TlsWrap* wrap = UnwrapTlsWrap(env, out);
  if (wrap == nullptr) return Undefined(env);
  napi_create_reference(env, argv[0], 1, &wrap->parent_ref);
  napi_create_reference(env, argv[1], 1, &wrap->context_ref);
  wrap->secure_context = holder;
  if (argc >= 3 && argv[2] != nullptr) {
    bool is_server = false;
    napi_get_value_bool(env, argv[2], &is_server);
    wrap->is_server = is_server;
  }
  if (argc >= 4 && argv[3] != nullptr) {
    bool has_active = false;
    napi_get_value_bool(env, argv[3], &has_active);
    wrap->has_active_write_issued_by_prev_listener = has_active;
  }
  wrap->keepalive_needed = UbiStreamBaseGetLibuvStream(env, argv[0]) == nullptr;

  InitSsl(wrap);
  if (!wrap->pending_session.empty()) {
    (void)LoadSessionBytes(wrap, wrap->pending_session.data(), wrap->pending_session.size());
  }

  napi_value parent_reading = GetNamedValue(env, argv[0], "reading");
  if (parent_reading != nullptr) {
    napi_set_named_property(env, out, "reading", parent_reading);
  }

  napi_value onread = nullptr;
  if (napi_create_function(env, "__ubiTlsParentOnRead", NAPI_AUTO_LENGTH, ForwardParentRead, nullptr, &onread) ==
          napi_ok &&
      onread != nullptr) {
    napi_set_named_property(env, argv[0], "onread", onread);
  }

  return out;
}

napi_value UbiInstallTlsWrapBindingInternal(napi_env env) {
  TlsBindingState& state = EnsureState(env);
  napi_value cached = GetRefValue(env, state.binding_ref);
  if (cached != nullptr) return cached;

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  constexpr napi_property_attributes kMutableMethod =
      static_cast<napi_property_attributes>(napi_writable | napi_configurable);
  napi_property_descriptor tls_wrap_props[] = {
      {"readStart", nullptr, TlsWrapReadStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"readStop", nullptr, TlsWrapReadStop, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeBuffer", nullptr, TlsWrapWriteBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writev", nullptr, TlsWrapWritev, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeLatin1String", nullptr, TlsWrapWriteLatin1String, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"writeUtf8String", nullptr, TlsWrapWriteUtf8String, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeAsciiString", nullptr, TlsWrapWriteAsciiString, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeUcs2String", nullptr, TlsWrapWriteUcs2String, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"shutdown", nullptr, TlsWrapShutdown, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"close", nullptr, TlsWrapClose, nullptr, nullptr, nullptr, kMutableMethod, nullptr},
      {"ref", nullptr, TlsWrapRef, nullptr, nullptr, nullptr, kMutableMethod, nullptr},
      {"unref", nullptr, TlsWrapUnref, nullptr, nullptr, nullptr, kMutableMethod, nullptr},
      {"getAsyncId", nullptr, TlsWrapGetAsyncId, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProviderType", nullptr, TlsWrapGetProviderType, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"asyncReset", nullptr, TlsWrapAsyncReset, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"useUserBuffer", nullptr, TlsWrapUseUserBuffer, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setVerifyMode", nullptr, TlsWrapSetVerifyMode, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableTrace", nullptr, TlsWrapEnableTrace, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableSessionCallbacks", nullptr, TlsWrapEnableSessionCallbacks, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"enableCertCb", nullptr, TlsWrapEnableCertCb, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enableALPNCb", nullptr, TlsWrapEnableALPNCb, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"enablePskCallback", nullptr, TlsWrapEnablePskCallback, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"setPskIdentityHint", nullptr, TlsWrapSetPskIdentityHint, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"enableKeylogCallback", nullptr, TlsWrapEnableKeylogCallback, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"writesIssuedByPrevListenerDone", nullptr, TlsWrapWritesIssuedByPrevListenerDone, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"setALPNProtocols", nullptr, TlsWrapSetALPNProtocols, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"requestOCSP", nullptr, TlsWrapRequestOCSP, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"start", nullptr, TlsWrapStart, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"renegotiate", nullptr, TlsWrapRenegotiate, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setServername", nullptr, TlsWrapSetServername, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getServername", nullptr, TlsWrapGetServername, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setSession", nullptr, TlsWrapSetSession, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getSession", nullptr, TlsWrapGetSession, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getCipher", nullptr, TlsWrapGetCipher, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getSharedSigalgs", nullptr, TlsWrapGetSharedSigalgs, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getEphemeralKeyInfo", nullptr, TlsWrapGetEphemeralKeyInfo, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"getFinished", nullptr, TlsWrapGetFinished, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerFinished", nullptr, TlsWrapGetPeerFinished, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getProtocol", nullptr, TlsWrapGetProtocol, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getTLSTicket", nullptr, TlsWrapGetTLSTicket, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"isSessionReused", nullptr, TlsWrapIsSessionReused, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerX509Certificate", nullptr, TlsWrapGetPeerX509Certificate, nullptr, nullptr, nullptr,
       napi_default_method, nullptr},
      {"getX509Certificate", nullptr, TlsWrapGetX509Certificate, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"exportKeyingMaterial", nullptr, TlsWrapExportKeyingMaterial, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"setMaxSendFragment", nullptr, TlsWrapSetMaxSendFragment, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"getALPNNegotiatedProtocol", nullptr, TlsWrapGetALPNNegotiatedProtocol, nullptr, nullptr, nullptr,
       napi_default, nullptr},
      {"verifyError", nullptr, TlsWrapVerifyError, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"getPeerCertificate", nullptr, TlsWrapGetPeerCertificate, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"getCertificate", nullptr, TlsWrapGetCertificate, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setKeyCert", nullptr, TlsWrapSetKeyCert, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"destroySSL", nullptr, TlsWrapDestroySSL, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"loadSession", nullptr, TlsWrapLoadSession, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"endParser", nullptr, TlsWrapEndParser, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"setOCSPResponse", nullptr, TlsWrapSetOCSPResponse, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"certCbDone", nullptr, TlsWrapCertCbDone, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"newSessionDone", nullptr, TlsWrapNewSessionDone, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"receive", nullptr, TlsWrapReceive, nullptr, nullptr, nullptr, napi_default_method, nullptr},
      {"writeQueueSize", nullptr, nullptr, TlsWrapGetWriteQueueSize, nullptr, nullptr, napi_default, nullptr},
      {"bytesRead", nullptr, nullptr, TlsWrapBytesReadGetter, nullptr, nullptr, napi_default, nullptr},
      {"bytesWritten", nullptr, nullptr, TlsWrapBytesWrittenGetter, nullptr, nullptr, napi_default, nullptr},
      {"fd", nullptr, nullptr, TlsWrapFdGetter, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value tls_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "TLSWrap",
                        NAPI_AUTO_LENGTH,
                        TlsWrapCtor,
                        nullptr,
                        sizeof(tls_wrap_props) / sizeof(tls_wrap_props[0]),
                        tls_wrap_props,
                        &tls_wrap_ctor) != napi_ok ||
      tls_wrap_ctor == nullptr) {
    return nullptr;
  }

  DeleteRefIfPresent(env, &state.tls_wrap_ctor_ref);
  napi_create_reference(env, tls_wrap_ctor, 1, &state.tls_wrap_ctor_ref);

  napi_value wrap_fn = nullptr;
  if (napi_create_function(env, "wrap", NAPI_AUTO_LENGTH, TlsWrapWrap, nullptr, &wrap_fn) != napi_ok ||
      wrap_fn == nullptr) {
    return nullptr;
  }

  napi_set_named_property(env, binding, "TLSWrap", tls_wrap_ctor);
  napi_set_named_property(env, binding, "wrap", wrap_fn);

  DeleteRefIfPresent(env, &state.binding_ref);
  napi_create_reference(env, binding, 1, &state.binding_ref);
  return binding;
}

}  // namespace

napi_value UbiInstallTlsWrapBinding(napi_env env) {
  if (env == nullptr) return nullptr;
  return UbiInstallTlsWrapBindingInternal(env);
}
