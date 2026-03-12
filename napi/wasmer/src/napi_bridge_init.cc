// N-API bridge for the WASM host.
//
// Uses unofficial_napi_create_env() from napi-v8 to obtain a proper
// napi_env with all V8 scopes managed correctly.  Each N-API function
// is wrapped with an extern "C" bridge that takes/returns u32 handle IDs
// instead of opaque pointers, so the Rust host can translate between
// WASM guest memory and native N-API calls.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef NAPI_EXPERIMENTAL
#define NAPI_EXPERIMENTAL
#endif

#include "node_api.h"
#include "unofficial_napi.h"

void ResetCallbackBridgeStateLocked();

namespace {

std::recursive_mutex g_mu;
napi_env g_env = nullptr;
void* g_scope = nullptr;  // opaque scope handle from unofficial_napi_create_env
constexpr uint32_t kUnofficialEnvHandle = 1;
constexpr uint32_t kUnofficialScopeHandle = 1;

// Handle table: maps u32 IDs to persistent napi_value references.
//
// Raw napi_value handles are only valid within the originating handle scope.
// The guest stores these IDs across calls, so we must hold a reference on the
// host side and re-resolve it when loading the value again.
std::unordered_map<uint32_t, napi_ref> g_values;
uint32_t g_next_id = 1;

uint32_t StoreValue(napi_value val) {
  if (val == nullptr) return 0;
  napi_ref ref = nullptr;
  if (napi_create_reference(g_env, val, 1, &ref) != napi_ok || ref == nullptr) {
    return 0;
  }
  uint32_t id = g_next_id++;
  g_values[id] = ref;
  return id;
}

napi_value LoadValue(uint32_t id) {
  if (id == 0) return nullptr;
  auto it = g_values.find(id);
  if (it == g_values.end() || it->second == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(g_env, it->second, &value) != napi_ok || value == nullptr) {
    return nullptr;
  }
  return value;
}

// Handle table for napi_ref (references)
std::unordered_map<uint32_t, napi_ref> g_refs;
uint32_t g_next_ref_id = 1;

uint32_t StoreRef(napi_ref ref) {
  if (ref == nullptr) return 0;
  uint32_t id = g_next_ref_id++;
  g_refs[id] = ref;
  return id;
}

napi_ref LoadRef(uint32_t id) {
  if (id == 0) return nullptr;
  auto it = g_refs.find(id);
  return it != g_refs.end() ? it->second : nullptr;
}

void RemoveRef(uint32_t id) { g_refs.erase(id); }

// Handle table for napi_deferred (promise deferreds)
std::unordered_map<uint32_t, napi_deferred> g_deferreds;
uint32_t g_next_deferred_id = 1;

uint32_t StoreDeferred(napi_deferred d) {
  if (d == nullptr) return 0;
  uint32_t id = g_next_deferred_id++;
  g_deferreds[id] = d;
  return id;
}

napi_deferred LoadDeferred(uint32_t id) {
  if (id == 0) return nullptr;
  auto it = g_deferreds.find(id);
  return it != g_deferreds.end() ? it->second : nullptr;
}

void RemoveDeferred(uint32_t id) { g_deferreds.erase(id); }

// Handle table for napi_escapable_handle_scope
std::unordered_map<uint32_t, napi_escapable_handle_scope> g_esc_scopes;
uint32_t g_next_esc_scope_id = 1;

uint32_t StoreEscScope(napi_escapable_handle_scope s) {
  if (s == nullptr) return 0;
  uint32_t id = g_next_esc_scope_id++;
  g_esc_scopes[id] = s;
  return id;
}

napi_escapable_handle_scope LoadEscScope(uint32_t id) {
  if (id == 0) return nullptr;
  auto it = g_esc_scopes.find(id);
  return it != g_esc_scopes.end() ? it->second : nullptr;
}

void RemoveEscScope(uint32_t id) { g_esc_scopes.erase(id); }

// Handle table for opaque module_wrap handles.
std::unordered_map<uint32_t, void*> g_module_wrap_handles;
uint32_t g_next_module_wrap_handle_id = 1;

uint32_t StoreModuleWrapHandle(void* handle) {
  if (handle == nullptr) return 0;
  uint32_t id = g_next_module_wrap_handle_id++;
  g_module_wrap_handles[id] = handle;
  return id;
}

void* LoadModuleWrapHandle(uint32_t id) {
  if (id == 0) return nullptr;
  auto it = g_module_wrap_handles.find(id);
  return it != g_module_wrap_handles.end() ? it->second : nullptr;
}

void RemoveModuleWrapHandle(uint32_t id) { g_module_wrap_handles.erase(id); }

napi_status DisposeBridgeStateLocked() {
  for (auto& entry : g_values) {
    if (entry.second != nullptr) {
      napi_delete_reference(g_env, entry.second);
    }
  }
  g_values.clear();
  g_next_id = 1;
  g_refs.clear();
  g_next_ref_id = 1;
  g_deferreds.clear();
  g_next_deferred_id = 1;
  g_esc_scopes.clear();
  g_next_esc_scope_id = 1;
  g_module_wrap_handles.clear();
  g_next_module_wrap_handle_id = 1;
  ::ResetCallbackBridgeStateLocked();
  if (g_scope) {
    napi_status s = unofficial_napi_release_env(g_scope);
    g_scope = nullptr;
    g_env = nullptr;
    return s;
  }
  g_env = nullptr;
  return napi_ok;
}

}  // namespace

// ============================================================
// Initialization
// ============================================================

extern "C" int snapi_bridge_init() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  // Intentionally do not create a N-API env here.
  // Env creation is deferred until the guest explicitly calls
  // `unofficial_napi_create_env`, so init happens on the execution thread.
  (void)lock;
  return 1;
}

// ============================================================
// Value creation
// ============================================================

extern "C" int snapi_bridge_get_undefined(uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_get_undefined(g_env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_null(uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_get_null(g_env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_boolean(int value, uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_get_boolean(g_env, value != 0, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_global(uint32_t* out_id) {
  if (g_env == nullptr) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_global(g_env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_string_utf8(const char* str,
                                               uint32_t wasm_length,
                                               uint32_t* out_id) {
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : (size_t)wasm_length;
  napi_value result;
  napi_status s = napi_create_string_utf8(g_env, str, length, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_string_latin1(const char* str,
                                                 uint32_t wasm_length,
                                                 uint32_t* out_id) {
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : (size_t)wasm_length;
  napi_value result;
  napi_status s = napi_create_string_latin1(g_env, str, length, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_int32(int32_t value, uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_int32(g_env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_uint32(uint32_t value, uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_uint32(g_env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_double(double value, uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_double(g_env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_int64(int64_t value, uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_int64(g_env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_object(uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_object(g_env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_array(uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_array(g_env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_array_with_length(uint32_t length,
                                                     uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_array_with_length(g_env, (size_t)length, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Value reading
// ============================================================

extern "C" int snapi_bridge_get_value_string_utf8(uint32_t id, char* buf,
                                                  size_t bufsize,
                                                  size_t* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_string_utf8(g_env, val, buf, bufsize, result);
}

extern "C" int snapi_bridge_get_value_string_latin1(uint32_t id, char* buf,
                                                    size_t bufsize,
                                                    size_t* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_string_latin1(g_env, val, buf, bufsize, result);
}

extern "C" int snapi_bridge_get_value_int32(uint32_t id, int32_t* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_int32(g_env, val, result);
}

extern "C" int snapi_bridge_get_value_uint32(uint32_t id, uint32_t* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_uint32(g_env, val, result);
}

extern "C" int snapi_bridge_get_value_double(uint32_t id, double* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_double(g_env, val, result);
}

extern "C" int snapi_bridge_get_value_int64(uint32_t id, int64_t* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_int64(g_env, val, result);
}

extern "C" int snapi_bridge_get_value_bool(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool b;
  napi_status s = napi_get_value_bool(g_env, val, &b);
  if (s != napi_ok) return s;
  *result = b ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Type checking
// ============================================================

extern "C" int snapi_bridge_typeof(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  napi_valuetype vtype;
  napi_status s = napi_typeof(g_env, val, &vtype);
  if (s != napi_ok) return s;
  *result = (int)vtype;
  return napi_ok;
}

extern "C" int snapi_bridge_is_array(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_array(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_error(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_error(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_arraybuffer(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_arraybuffer(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_typedarray(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_typedarray(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_dataview(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_dataview(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_date(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_date(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_is_promise(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_promise(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_instanceof(uint32_t obj_id, uint32_t ctor_id,
                                       int* result) {
  napi_value obj = LoadValue(obj_id);
  napi_value ctor = LoadValue(ctor_id);
  if (!obj || !ctor) return napi_invalid_arg;
  bool is;
  napi_status s = napi_instanceof(g_env, obj, ctor, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Coercion
// ============================================================

extern "C" int snapi_bridge_coerce_to_bool(uint32_t id, uint32_t* out_id) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_bool(g_env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_coerce_to_number(uint32_t id, uint32_t* out_id) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_number(g_env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_coerce_to_string(uint32_t id, uint32_t* out_id) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_string(g_env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_coerce_to_object(uint32_t id, uint32_t* out_id) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_coerce_to_object(g_env, val, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Object operations
// ============================================================

extern "C" int snapi_bridge_set_property(uint32_t obj_id, uint32_t key_id,
                                         uint32_t val_id) {
  napi_value obj = LoadValue(obj_id);
  napi_value key = LoadValue(key_id);
  napi_value val = LoadValue(val_id);
  if (!obj || !key || !val) return napi_invalid_arg;
  return napi_set_property(g_env, obj, key, val);
}

extern "C" int snapi_bridge_get_property(uint32_t obj_id, uint32_t key_id,
                                         uint32_t* out_id) {
  napi_value obj = LoadValue(obj_id);
  napi_value key = LoadValue(key_id);
  if (!obj || !key) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_property(g_env, obj, key, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_has_property(uint32_t obj_id, uint32_t key_id,
                                         int* result) {
  napi_value obj = LoadValue(obj_id);
  napi_value key = LoadValue(key_id);
  if (!obj || !key) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_property(g_env, obj, key, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_has_own_property(uint32_t obj_id, uint32_t key_id,
                                             int* result) {
  napi_value obj = LoadValue(obj_id);
  napi_value key = LoadValue(key_id);
  if (!obj || !key) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_own_property(g_env, obj, key, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_delete_property(uint32_t obj_id, uint32_t key_id,
                                            int* result) {
  napi_value obj = LoadValue(obj_id);
  napi_value key = LoadValue(key_id);
  if (!obj || !key) return napi_invalid_arg;
  bool deleted;
  napi_status s = napi_delete_property(g_env, obj, key, &deleted);
  if (s != napi_ok) return s;
  *result = deleted ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_set_named_property(uint32_t obj_id,
                                               const char* name,
                                               uint32_t val_id) {
  napi_value obj = LoadValue(obj_id);
  napi_value val = LoadValue(val_id);
  if (!obj || !val || !name) return napi_invalid_arg;
  return napi_set_named_property(g_env, obj, name, val);
}

extern "C" int snapi_bridge_get_named_property(uint32_t obj_id,
                                               const char* name,
                                               uint32_t* out_id) {
  if (g_env == nullptr) return napi_invalid_arg;
  napi_value obj = LoadValue(obj_id);
  if (!obj || !name) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_named_property(g_env, obj, name, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_has_named_property(uint32_t obj_id,
                                               const char* name,
                                               int* result) {
  napi_value obj = LoadValue(obj_id);
  if (!obj || !name) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_named_property(g_env, obj, name, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_set_element(uint32_t obj_id, uint32_t index,
                                        uint32_t val_id) {
  napi_value obj = LoadValue(obj_id);
  napi_value val = LoadValue(val_id);
  if (!obj || !val) return napi_invalid_arg;
  return napi_set_element(g_env, obj, index, val);
}

extern "C" int snapi_bridge_get_element(uint32_t obj_id, uint32_t index,
                                        uint32_t* out_id) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_element(g_env, obj, index, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_has_element(uint32_t obj_id, uint32_t index,
                                        int* result) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  bool has;
  napi_status s = napi_has_element(g_env, obj, index, &has);
  if (s != napi_ok) return s;
  *result = has ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_delete_element(uint32_t obj_id, uint32_t index,
                                           int* result) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  bool deleted;
  napi_status s = napi_delete_element(g_env, obj, index, &deleted);
  if (s != napi_ok) return s;
  *result = deleted ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_array_length(uint32_t arr_id,
                                             uint32_t* result) {
  napi_value arr = LoadValue(arr_id);
  if (!arr) return napi_invalid_arg;
  return napi_get_array_length(g_env, arr, result);
}

extern "C" int snapi_bridge_get_property_names(uint32_t obj_id,
                                               uint32_t* out_id) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_property_names(g_env, obj, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_all_property_names(uint32_t obj_id,
                                                   int mode, int filter,
                                                   int conversion,
                                                   uint32_t* out_id) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_all_property_names(
      g_env, obj, (napi_key_collection_mode)mode, (napi_key_filter)filter,
      (napi_key_conversion)conversion, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_prototype(uint32_t obj_id, uint32_t* out_id) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_prototype(g_env, obj, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_object_freeze(uint32_t obj_id) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  return napi_object_freeze(g_env, obj);
}

extern "C" int snapi_bridge_object_seal(uint32_t obj_id) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  return napi_object_seal(g_env, obj);
}

// ============================================================
// Comparison
// ============================================================

extern "C" int snapi_bridge_strict_equals(uint32_t a_id, uint32_t b_id,
                                          int* result) {
  napi_value a = LoadValue(a_id);
  napi_value b = LoadValue(b_id);
  if (!a || !b) return napi_invalid_arg;
  bool eq;
  napi_status s = napi_strict_equals(g_env, a, b, &eq);
  if (s != napi_ok) return s;
  *result = eq ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Error handling
// ============================================================

extern "C" int snapi_bridge_create_error(uint32_t code_id, uint32_t msg_id,
                                         uint32_t* out_id) {
  napi_value code = LoadValue(code_id);  // can be null (0)
  napi_value msg = LoadValue(msg_id);
  if (!msg) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_error(g_env, code, msg, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_type_error(uint32_t code_id,
                                              uint32_t msg_id,
                                              uint32_t* out_id) {
  napi_value code = LoadValue(code_id);
  napi_value msg = LoadValue(msg_id);
  if (!msg) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_type_error(g_env, code, msg, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_range_error(uint32_t code_id,
                                               uint32_t msg_id,
                                               uint32_t* out_id) {
  napi_value code = LoadValue(code_id);
  napi_value msg = LoadValue(msg_id);
  if (!msg) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_range_error(g_env, code, msg, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_throw(uint32_t error_id) {
  napi_value error = LoadValue(error_id);
  if (!error) return napi_invalid_arg;
  return napi_throw(g_env, error);
}

extern "C" int snapi_bridge_throw_error(const char* code, const char* msg) {
  return napi_throw_error(g_env, code, msg);
}

extern "C" int snapi_bridge_throw_type_error(const char* code,
                                             const char* msg) {
  return napi_throw_type_error(g_env, code, msg);
}

extern "C" int snapi_bridge_throw_range_error(const char* code,
                                              const char* msg) {
  return napi_throw_range_error(g_env, code, msg);
}

extern "C" int snapi_bridge_is_exception_pending(int* result) {
  bool pending;
  napi_status s = napi_is_exception_pending(g_env, &pending);
  if (s != napi_ok) return s;
  *result = pending ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_and_clear_last_exception(uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_get_and_clear_last_exception(g_env, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Symbol
// ============================================================

extern "C" int snapi_bridge_create_symbol(uint32_t description_id,
                                          uint32_t* out_id) {
  napi_value description = LoadValue(description_id);  // can be null (0)
  napi_value result;
  napi_status s = napi_create_symbol(g_env, description, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// BigInt
// ============================================================

extern "C" int snapi_bridge_create_bigint_int64(int64_t value,
                                                uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_bigint_int64(g_env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_bigint_uint64(uint64_t value,
                                                 uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_bigint_uint64(g_env, value, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_bigint_int64(uint32_t id, int64_t* value,
                                                   int* lossless) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool loss;
  napi_status s = napi_get_value_bigint_int64(g_env, val, value, &loss);
  if (s != napi_ok) return s;
  *lossless = loss ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_bigint_uint64(uint32_t id,
                                                    uint64_t* value,
                                                    int* lossless) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool loss;
  napi_status s = napi_get_value_bigint_uint64(g_env, val, value, &loss);
  if (s != napi_ok) return s;
  *lossless = loss ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Date
// ============================================================

extern "C" int snapi_bridge_create_date(double time, uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_date(g_env, time, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_date_value(uint32_t id, double* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_date_value(g_env, val, result);
}

// ============================================================
// Promise
// ============================================================

extern "C" int snapi_bridge_create_promise(uint32_t* deferred_out,
                                           uint32_t* out_id) {
  napi_deferred deferred;
  napi_value promise;
  napi_status s = napi_create_promise(g_env, &deferred, &promise);
  if (s != napi_ok) return s;
  *deferred_out = StoreDeferred(deferred);
  *out_id = StoreValue(promise);
  return napi_ok;
}

extern "C" int snapi_bridge_resolve_deferred(uint32_t deferred_id,
                                             uint32_t value_id) {
  napi_deferred d = LoadDeferred(deferred_id);
  napi_value val = LoadValue(value_id);
  if (!d || !val) return napi_invalid_arg;
  napi_status s = napi_resolve_deferred(g_env, d, val);
  if (s == napi_ok) RemoveDeferred(deferred_id);
  return s;
}

extern "C" int snapi_bridge_reject_deferred(uint32_t deferred_id,
                                            uint32_t value_id) {
  napi_deferred d = LoadDeferred(deferred_id);
  napi_value val = LoadValue(value_id);
  if (!d || !val) return napi_invalid_arg;
  napi_status s = napi_reject_deferred(g_env, d, val);
  if (s == napi_ok) RemoveDeferred(deferred_id);
  return s;
}

// ============================================================
// ArrayBuffer
// ============================================================

extern "C" int snapi_bridge_create_arraybuffer(uint32_t byte_length,
                                               uint32_t* out_id) {
  void* data;
  napi_value result;
  napi_status s =
      napi_create_arraybuffer(g_env, (size_t)byte_length, &data, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_external_arraybuffer(uint64_t data_addr,
                                                        uint32_t byte_length,
                                                        uint32_t* out_id) {
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_arraybuffer(
      g_env, data, (size_t)byte_length, nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_create_external_buffer(uint64_t data_addr,
                                                   uint32_t byte_length,
                                                   uint32_t* out_id) {
  void* data = (void*)(uintptr_t)data_addr;
  napi_value result;
  napi_status s = napi_create_external_buffer(
      g_env, (size_t)byte_length, data, nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_is_sharedarraybuffer(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is_sab = false;
  napi_status s = node_api_is_sharedarraybuffer(g_env, val, &is_sab);
  if (s != napi_ok) return s;
  *result = is_sab ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_create_sharedarraybuffer(uint32_t byte_length,
                                                     uint64_t* data_out,
                                                     uint32_t* out_id) {
  void* data = nullptr;
  napi_value result;
  napi_status s =
      node_api_create_sharedarraybuffer(g_env, (size_t)byte_length, &data, &result);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_node_api_set_prototype(uint32_t object_id,
                                                   uint32_t prototype_id) {
  napi_value object = LoadValue(object_id);
  napi_value prototype = LoadValue(prototype_id);
  if (!object || !prototype) return napi_invalid_arg;
  return node_api_set_prototype(g_env, object, prototype);
}

extern "C" int snapi_bridge_get_arraybuffer_info(uint32_t id,
                                                 uint64_t* data_out,
                                                 uint32_t* byte_length) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  void* data;
  size_t len;
  napi_status s = napi_get_arraybuffer_info(g_env, val, &data, &len);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  *byte_length = (uint32_t)len;
  return napi_ok;
}

extern "C" int snapi_bridge_detach_arraybuffer(uint32_t id) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_detach_arraybuffer(g_env, val);
}

extern "C" int snapi_bridge_is_detached_arraybuffer(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is;
  napi_status s = napi_is_detached_arraybuffer(g_env, val, &is);
  if (s != napi_ok) return s;
  *result = is ? 1 : 0;
  return napi_ok;
}

// ============================================================
// TypedArray
// ============================================================

extern "C" int snapi_bridge_create_typedarray(int type, uint32_t length,
                                              uint32_t arraybuffer_id,
                                              uint32_t byte_offset,
                                              uint32_t* out_id) {
  napi_value arraybuffer = LoadValue(arraybuffer_id);
  if (!arraybuffer) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_typedarray(
      g_env, (napi_typedarray_type)type, (size_t)length, arraybuffer,
      (size_t)byte_offset, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_typedarray_info(uint32_t id, int* type_out,
                                                uint32_t* length_out,
                                                uint64_t* data_out,
                                                uint32_t* arraybuffer_out,
                                                uint32_t* byte_offset_out) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  napi_typedarray_type type;
  size_t length;
  void* data = nullptr;
  napi_value arraybuffer;
  size_t byte_offset;
  napi_status s = napi_get_typedarray_info(g_env, val, &type, &length, &data,
                                           &arraybuffer, &byte_offset);
  if (s != napi_ok) return s;
  if (type_out) *type_out = (int)type;
  if (length_out) *length_out = (uint32_t)length;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  if (arraybuffer_out) *arraybuffer_out = StoreValue(arraybuffer);
  if (byte_offset_out) *byte_offset_out = (uint32_t)byte_offset;
  return napi_ok;
}

// ============================================================
// DataView
// ============================================================

extern "C" int snapi_bridge_create_dataview(uint32_t byte_length,
                                            uint32_t arraybuffer_id,
                                            uint32_t byte_offset,
                                            uint32_t* out_id) {
  napi_value arraybuffer = LoadValue(arraybuffer_id);
  if (!arraybuffer) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_create_dataview(g_env, (size_t)byte_length, arraybuffer,
                                       (size_t)byte_offset, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_dataview_info(uint32_t id,
                                              uint32_t* byte_length_out,
                                              uint64_t* data_out,
                                              uint32_t* arraybuffer_out,
                                              uint32_t* byte_offset_out) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  size_t byte_length;
  void* data = nullptr;
  napi_value arraybuffer;
  size_t byte_offset;
  napi_status s = napi_get_dataview_info(g_env, val, &byte_length, &data,
                                         &arraybuffer, &byte_offset);
  if (s != napi_ok) return s;
  if (byte_length_out) *byte_length_out = (uint32_t)byte_length;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  if (arraybuffer_out) *arraybuffer_out = StoreValue(arraybuffer);
  if (byte_offset_out) *byte_offset_out = (uint32_t)byte_offset;
  return napi_ok;
}

// ============================================================
// External values
// ============================================================

extern "C" int snapi_bridge_create_external(uint64_t data_val,
                                            uint32_t* out_id) {
  // Store arbitrary u64 data value as a void*. No finalizer.
  napi_value result;
  napi_status s = napi_create_external(g_env, (void*)(uintptr_t)data_val,
                                       nullptr, nullptr, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_external(uint32_t id,
                                               uint64_t* data_out) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  void* data;
  napi_status s = napi_get_value_external(g_env, val, &data);
  if (s != napi_ok) return s;
  *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

// ============================================================
// References
// ============================================================

extern "C" int snapi_bridge_create_reference(uint32_t value_id,
                                             uint32_t initial_refcount,
                                             uint32_t* ref_out) {
  napi_value val = LoadValue(value_id);
  if (!val) return napi_invalid_arg;
  napi_ref ref;
  napi_status s = napi_create_reference(g_env, val, initial_refcount, &ref);
  if (s != napi_ok) return s;
  *ref_out = StoreRef(ref);
  return napi_ok;
}

extern "C" int snapi_bridge_delete_reference(uint32_t ref_id) {
  napi_ref ref = LoadRef(ref_id);
  if (!ref) return napi_invalid_arg;
  napi_status s = napi_delete_reference(g_env, ref);
  if (s == napi_ok) RemoveRef(ref_id);
  return s;
}

extern "C" int snapi_bridge_reference_ref(uint32_t ref_id, uint32_t* result) {
  napi_ref ref = LoadRef(ref_id);
  if (!ref) return napi_invalid_arg;
  return napi_reference_ref(g_env, ref, result);
}

extern "C" int snapi_bridge_reference_unref(uint32_t ref_id, uint32_t* result) {
  napi_ref ref = LoadRef(ref_id);
  if (!ref) return napi_invalid_arg;
  return napi_reference_unref(g_env, ref, result);
}

extern "C" int snapi_bridge_get_reference_value(uint32_t ref_id,
                                                uint32_t* out_id) {
  napi_ref ref = LoadRef(ref_id);
  if (!ref) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_get_reference_value(g_env, ref, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Handle scopes (escapable)
// ============================================================

extern "C" int snapi_bridge_open_escapable_handle_scope(uint32_t* scope_out) {
  napi_escapable_handle_scope scope;
  napi_status s = napi_open_escapable_handle_scope(g_env, &scope);
  if (s != napi_ok) return s;
  *scope_out = StoreEscScope(scope);
  return napi_ok;
}

extern "C" int snapi_bridge_close_escapable_handle_scope(uint32_t scope_id) {
  napi_escapable_handle_scope scope = LoadEscScope(scope_id);
  if (!scope) return napi_invalid_arg;
  napi_status s = napi_close_escapable_handle_scope(g_env, scope);
  if (s == napi_ok) RemoveEscScope(scope_id);
  return s;
}

extern "C" int snapi_bridge_escape_handle(uint32_t scope_id,
                                          uint32_t escapee_id,
                                          uint32_t* out_id) {
  napi_escapable_handle_scope scope = LoadEscScope(scope_id);
  napi_value escapee = LoadValue(escapee_id);
  if (!scope || !escapee) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_escape_handle(g_env, scope, escapee, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Type tagging
// ============================================================

extern "C" int snapi_bridge_type_tag_object(uint32_t obj_id,
                                            uint64_t tag_lower,
                                            uint64_t tag_upper) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_type_tag tag;
  tag.lower = tag_lower;
  tag.upper = tag_upper;
  return napi_type_tag_object(g_env, obj, &tag);
}

extern "C" int snapi_bridge_check_object_type_tag(uint32_t obj_id,
                                                  uint64_t tag_lower,
                                                  uint64_t tag_upper,
                                                  int* result) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_type_tag tag;
  tag.lower = tag_lower;
  tag.upper = tag_upper;
  bool matches;
  napi_status s = napi_check_object_type_tag(g_env, obj, &tag, &matches);
  if (s != napi_ok) return s;
  *result = matches ? 1 : 0;
  return napi_ok;
}

// ============================================================
// Function calling (call JS functions from native)
// ============================================================

extern "C" int snapi_bridge_call_function(uint32_t recv_id, uint32_t func_id,
                                          uint32_t argc,
                                          const uint32_t* argv_ids,
                                          uint32_t* out_id) {
  napi_value recv = LoadValue(recv_id);
  napi_value func = LoadValue(func_id);
  if (!recv || !func) return napi_invalid_arg;
  std::vector<napi_value> argv(argc);
  for (uint32_t i = 0; i < argc; i++) {
    argv[i] = LoadValue(argv_ids[i]);
    if (!argv[i]) return napi_invalid_arg;
  }
  napi_value result;
  napi_status s =
      napi_call_function(g_env, recv, func, argc, argv.data(), &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Script execution
// ============================================================

extern "C" int snapi_bridge_run_script(uint32_t script_id,
                                       uint32_t* out_value_id) {
  napi_value script_val = LoadValue(script_id);
  if (!script_val) return napi_invalid_arg;
  napi_value result;
  napi_status s = napi_run_script(g_env, script_val, &result);
  if (s != napi_ok) return s;
  *out_value_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// UTF-16 strings
// ============================================================

extern "C" int snapi_bridge_create_string_utf16(const uint16_t* str,
                                                uint32_t wasm_length,
                                                uint32_t* out_id) {
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : (size_t)wasm_length;
  napi_value result;
  napi_status s = napi_create_string_utf16(g_env, (const char16_t*)str, length,
                                           &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_string_utf16(uint32_t id,
                                                   uint16_t* buf,
                                                   size_t bufsize,
                                                   size_t* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_string_utf16(g_env, val, (char16_t*)buf, bufsize,
                                     result);
}

// ============================================================
// BigInt words (arbitrary precision)
// ============================================================

extern "C" int snapi_bridge_create_bigint_words(int sign_bit,
                                                uint32_t word_count,
                                                const uint64_t* words,
                                                uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_bigint_words(g_env, sign_bit, (size_t)word_count,
                                           words, &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_get_value_bigint_words(uint32_t id,
                                                   int* sign_bit,
                                                   size_t* word_count,
                                                   uint64_t* words) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  return napi_get_value_bigint_words(g_env, val, sign_bit, word_count, words);
}

// ============================================================
// Instance data
// ============================================================

extern "C" int snapi_bridge_set_instance_data(uint64_t data_val) {
  return napi_set_instance_data(g_env, (void*)(uintptr_t)data_val,
                                nullptr, nullptr);
}

extern "C" int snapi_bridge_get_instance_data(uint64_t* data_out) {
  void* data = nullptr;
  napi_status s = napi_get_instance_data(g_env, &data);
  if (s != napi_ok) return s;
  *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_adjust_external_memory(int64_t change,
                                                   int64_t* adjusted) {
  return napi_adjust_external_memory(g_env, change, adjusted);
}

// ============================================================
// Node Buffers
// ============================================================

extern "C" int snapi_bridge_create_buffer(uint32_t length,
                                          uint64_t* data_out,
                                          uint32_t* out_id) {
  napi_value buffer;
  void* data = nullptr;
  napi_status s = napi_create_buffer(g_env, (size_t)length, &data, &buffer);
  if (s != napi_ok) return s;
  *out_id = StoreValue(buffer);
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_create_buffer_copy(uint32_t length,
                                               const void* src_data,
                                               uint64_t* result_data_out,
                                               uint32_t* out_id) {
  napi_value buffer;
  void* result_data = nullptr;
  napi_status s = napi_create_buffer_copy(g_env, (size_t)length, src_data,
                                          &result_data, &buffer);
  if (s != napi_ok) return s;
  *out_id = StoreValue(buffer);
  if (result_data_out) *result_data_out = (uint64_t)(uintptr_t)result_data;
  return napi_ok;
}

extern "C" int snapi_bridge_is_buffer(uint32_t id, int* result) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  bool is_buffer = false;
  napi_status s = napi_is_buffer(g_env, val, &is_buffer);
  if (s != napi_ok) return s;
  *result = is_buffer ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_get_buffer_info(uint32_t id,
                                            uint64_t* data_out,
                                            uint32_t* length_out) {
  napi_value val = LoadValue(id);
  if (!val) return napi_invalid_arg;
  void* data = nullptr;
  size_t length = 0;
  napi_status s = napi_get_buffer_info(g_env, val, &data, &length);
  if (s != napi_ok) return s;
  if (length_out) *length_out = (uint32_t)length;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

// ============================================================
// Node version (stub — we're not running in Node, return fake version)
// ============================================================

extern "C" int snapi_bridge_get_node_version(uint32_t* major,
                                             uint32_t* minor,
                                             uint32_t* patch) {
  // Return a reasonable fake version since we're running on pure V8
  if (major) *major = 22;
  if (minor) *minor = 0;
  if (patch) *patch = 0;
  return napi_ok;
}

// ============================================================
// Object wrapping
// ============================================================

extern "C" int snapi_bridge_wrap(uint32_t obj_id, uint64_t native_data,
                                 uint32_t* ref_out) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  napi_ref ref = nullptr;
  napi_status s = napi_wrap(g_env, obj, (void*)(uintptr_t)native_data,
                            nullptr, nullptr, ref_out ? &ref : nullptr);
  if (s != napi_ok) return s;
  if (ref_out) *ref_out = StoreRef(ref);
  return napi_ok;
}

extern "C" int snapi_bridge_unwrap(uint32_t obj_id, uint64_t* data_out) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  void* data = nullptr;
  napi_status s = napi_unwrap(g_env, obj, &data);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_remove_wrap(uint32_t obj_id, uint64_t* data_out) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  void* data = nullptr;
  napi_status s = napi_remove_wrap(g_env, obj, &data);
  if (s != napi_ok) return s;
  if (data_out) *data_out = (uint64_t)(uintptr_t)data;
  return napi_ok;
}

extern "C" int snapi_bridge_add_finalizer(uint32_t obj_id, uint64_t data_val,
                                          uint32_t* ref_out) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  // No actual WASM callback for finalizer; just register with nullptr callback
  napi_ref ref = nullptr;
  napi_status s = napi_add_finalizer(g_env, obj, (void*)(uintptr_t)data_val,
                                     nullptr, nullptr, ref_out ? &ref : nullptr);
  if (s != napi_ok) return s;
  if (ref_out) *ref_out = StoreRef(ref);
  return napi_ok;
}

// ============================================================
// napi_new_instance
// ============================================================

extern "C" int snapi_bridge_new_instance(uint32_t ctor_id, uint32_t argc,
                                         const uint32_t* argv_ids,
                                         uint32_t* out_id) {
  napi_value ctor = LoadValue(ctor_id);
  if (!ctor) return napi_invalid_arg;
  std::vector<napi_value> argv(argc);
  for (uint32_t i = 0; i < argc; i++) {
    argv[i] = LoadValue(argv_ids[i]);
    if (!argv[i]) return napi_invalid_arg;
  }
  napi_value result;
  napi_status s = napi_new_instance(g_env, ctor, argc, argv.data(), &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// napi_define_properties
// ============================================================

// Forward declaration (defined below in callback system section)
static napi_value generic_wasm_callback(napi_env env, napi_callback_info info);

extern "C" int snapi_bridge_define_properties(uint32_t obj_id,
                                              uint32_t prop_count,
                                              const char** utf8names,
                                              const uint32_t* name_ids,
                                              const uint32_t* prop_types,
                                              const uint32_t* value_ids,
                                              const uint32_t* method_reg_ids,
                                              const uint32_t* getter_reg_ids,
                                              const uint32_t* setter_reg_ids,
                                              const int32_t* attributes) {
  napi_value obj = LoadValue(obj_id);
  if (!obj) return napi_invalid_arg;
  std::vector<napi_property_descriptor> descs(prop_count);
  for (uint32_t i = 0; i < prop_count; i++) {
    memset(&descs[i], 0, sizeof(napi_property_descriptor));
    descs[i].utf8name = utf8names != nullptr ? utf8names[i] : nullptr;
    descs[i].name = (name_ids != nullptr && name_ids[i] != 0) ? LoadValue(name_ids[i]) : nullptr;
    descs[i].attributes = (napi_property_attributes)attributes[i];

    switch (prop_types[i]) {
      case 0:
        descs[i].value = LoadValue(value_ids[i]);
        break;
      case 1:
        descs[i].method = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)method_reg_ids[i];
        break;
      case 2:
        descs[i].getter = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)getter_reg_ids[i];
        break;
      case 3:
        descs[i].setter = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)setter_reg_ids[i];
        break;
      case 4:
        descs[i].getter = generic_wasm_callback;
        descs[i].setter = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)getter_reg_ids[i];
        break;
    }
  }
  return napi_define_properties(g_env, obj, prop_count, descs.data());
}

// ============================================================
// napi_define_class
// ============================================================

// Property descriptor layout passed from Rust:
// For each property (i), we pass:
//   utf8names[i]   - property name (C string)
//   types[i]       - 0=value, 1=method, 2=getter, 3=setter, 4=getter+setter
//   value_ids[i]   - if type==0, the value handle ID
//   method_reg_ids[i]  - if type==1, the callback reg_id for the method
//   getter_reg_ids[i]  - if type==2 or 4, the callback reg_id for getter
//   setter_reg_ids[i]  - if type==3 or 4, the callback reg_id for setter
//   attributes[i]  - napi_property_attributes

extern "C" int snapi_bridge_define_class(
    const char* utf8name, uint32_t name_len,
    uint32_t ctor_reg_id,
    uint32_t prop_count,
    const char** prop_names,
    const uint32_t* prop_name_ids,
    const uint32_t* prop_types,
    const uint32_t* prop_value_ids,
    const uint32_t* prop_method_reg_ids,
    const uint32_t* prop_getter_reg_ids,
    const uint32_t* prop_setter_reg_ids,
    const int32_t* prop_attributes,
    uint32_t* out_id) {

  // Build property descriptors
  std::vector<napi_property_descriptor> descs(prop_count);
  for (uint32_t i = 0; i < prop_count; i++) {
    memset(&descs[i], 0, sizeof(napi_property_descriptor));
    descs[i].utf8name = prop_names != nullptr ? prop_names[i] : nullptr;
    descs[i].name = (prop_name_ids != nullptr && prop_name_ids[i] != 0) ? LoadValue(prop_name_ids[i]) : nullptr;
    descs[i].attributes = (napi_property_attributes)prop_attributes[i];

    switch (prop_types[i]) {
      case 0: // value
        descs[i].value = LoadValue(prop_value_ids[i]);
        break;
      case 1: // method
        descs[i].method = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)prop_method_reg_ids[i];
        break;
      case 2: // getter only
        descs[i].getter = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)prop_getter_reg_ids[i];
        break;
      case 3: // setter only
        descs[i].setter = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)prop_setter_reg_ids[i];
        break;
      case 4: // getter + setter
        descs[i].getter = generic_wasm_callback;
        descs[i].setter = generic_wasm_callback;
        descs[i].data = (void*)(uintptr_t)prop_getter_reg_ids[i];
        // Note: N-API uses the same data pointer for both getter and setter.
        // The setter_reg_id is stored in the getter_reg_id for now.
        break;
    }
  }

  napi_value result;
  napi_status s = napi_define_class(
      g_env, utf8name,
      name_len == 0xFFFFFFFFu ? NAPI_AUTO_LENGTH : (size_t)name_len,
      generic_wasm_callback,
      (void*)(uintptr_t)ctor_reg_id,
      prop_count, descs.data(),
      &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Callback system (napi_create_function + napi_get_cb_info)
// ============================================================

// ---- Callback context stack (supports re-entrant / nested callbacks) ----
struct CbContext {
  uint32_t this_id;
  uint32_t argc;
  uint32_t argv_ids[64];
  uint64_t data_val;
  napi_callback_info original_info;  // For napi_get_new_target
};
static std::vector<CbContext> g_cb_stack;

extern "C" void snapi_bridge_set_cb_context(uint32_t this_id, uint32_t argc,
                                            const uint32_t* argv_ids,
                                            uint64_t data_val) {
  CbContext ctx;
  ctx.this_id = this_id;
  ctx.argc = argc;
  for (uint32_t i = 0; i < argc && i < 64; i++) ctx.argv_ids[i] = argv_ids[i];
  ctx.data_val = data_val;
  ctx.original_info = nullptr;
  g_cb_stack.push_back(ctx);
}

extern "C" void snapi_bridge_clear_cb_context() {
  if (!g_cb_stack.empty()) g_cb_stack.pop_back();
}

extern "C" int snapi_bridge_get_cb_info(uint32_t* argc_ptr, uint32_t* argv_out,
                                        uint32_t max_argv,
                                        uint32_t* this_out, uint64_t* data_out) {
  if (g_cb_stack.empty()) return napi_generic_failure;
  const CbContext& ctx = g_cb_stack.back();
  uint32_t actual = ctx.argc;
  uint32_t wanted = *argc_ptr;
  *argc_ptr = actual;
  if (this_out) *this_out = ctx.this_id;
  if (data_out) *data_out = ctx.data_val;
  uint32_t to_copy = (wanted < actual) ? wanted : actual;
  if (argv_out) {
    for (uint32_t i = 0; i < to_copy; i++) argv_out[i] = ctx.argv_ids[i];
  }
  return napi_ok;
}

// napi_get_new_target — only valid inside a constructor callback
extern "C" int snapi_bridge_get_new_target(uint32_t* out_id) {
  if (g_cb_stack.empty()) return napi_generic_failure;
  const CbContext& ctx = g_cb_stack.back();
  if (!ctx.original_info) {
    // Not inside a V8-triggered callback (shouldn't happen with trampoline)
    *out_id = 0;
    return napi_ok;
  }
  napi_value result;
  napi_status s = napi_get_new_target(g_env, ctx.original_info, &result);
  if (s != napi_ok) return s;
  *out_id = result ? StoreValue(result) : 0;
  return napi_ok;
}

// ---- Callback registry and V8 trampoline ----
// Each registered callback maps reg_id → (wasm_fn_ptr, data_val).
struct CbRegistration {
  uint32_t wasm_fn_ptr;
  uint32_t wasm_setter_fn_ptr;
  uint64_t data_val;
};
static std::unordered_map<uint32_t, CbRegistration> g_cb_registry;
static uint32_t g_next_cb_reg_id = 1;

// Forward-declare the Rust trampoline (defined in lib.rs via #[no_mangle] extern "C")
extern "C" uint32_t snapi_host_invoke_wasm_callback(uint32_t wasm_fn_ptr, uint64_t data_val);

void ResetCallbackBridgeStateLocked() {
  g_cb_stack.clear();
  g_cb_registry.clear();
  g_next_cb_reg_id = 1;
}

// Generic C++ callback invoked by V8 for all napi_create_function functions.
// Stores the V8 call args in the context stack, then calls the Rust trampoline
// which dispatches to the WASM callback.
static napi_value generic_wasm_callback(napi_env env, napi_callback_info info) {
  void* raw_data;
  size_t argc = 64;
  napi_value argv[64];
  napi_value this_arg;
  napi_get_cb_info(env, info, &argc, argv, &this_arg, &raw_data);

  uint32_t reg_id = (uint32_t)(uintptr_t)raw_data;
  auto it = g_cb_registry.find(reg_id);
  if (it == g_cb_registry.end()) {
    napi_value undef;
    napi_get_undefined(env, &undef);
    return undef;
  }

  // Push context onto stack
  CbContext ctx;
  ctx.this_id = StoreValue(this_arg);
  ctx.argc = (uint32_t)argc;
  for (uint32_t i = 0; i < argc && i < 64; i++) {
    ctx.argv_ids[i] = StoreValue(argv[i]);
  }
  ctx.data_val = it->second.data_val;
  ctx.original_info = info;  // Store for napi_get_new_target
  g_cb_stack.push_back(ctx);

  // Call Rust trampoline → WASM callback
  const uint32_t wasm_fn_ptr =
      (it->second.wasm_setter_fn_ptr != 0 && argc > 0)
          ? it->second.wasm_setter_fn_ptr
          : it->second.wasm_fn_ptr;

  uint32_t result_id =
      snapi_host_invoke_wasm_callback(wasm_fn_ptr, it->second.data_val);

  g_cb_stack.pop_back();

  napi_value result = LoadValue(result_id);
  if (!result) {
    napi_get_undefined(env, &result);
  }
  return result;
}

// Allocate a registration ID for a new callback
extern "C" uint32_t snapi_bridge_alloc_cb_reg_id() {
  return g_next_cb_reg_id++;
}

// Register callback data for a registration ID
extern "C" void snapi_bridge_register_callback(uint32_t reg_id,
                                               uint32_t wasm_fn_ptr,
                                               uint64_t data_val) {
  g_cb_registry[reg_id] = { wasm_fn_ptr, 0, data_val };
}

extern "C" void snapi_bridge_register_callback_pair(uint32_t reg_id,
                                                    uint32_t wasm_getter_fn_ptr,
                                                    uint32_t wasm_setter_fn_ptr,
                                                    uint64_t data_val) {
  g_cb_registry[reg_id] = { wasm_getter_fn_ptr, wasm_setter_fn_ptr, data_val };
}

// Create a JS function with generic_wasm_callback as its native callback.
// The reg_id is passed as the data pointer so the callback can look up
// which WASM function to invoke.
extern "C" int snapi_bridge_create_function(const char* utf8name, uint32_t name_len,
                                            uint32_t reg_id,
                                            uint32_t* out_id) {
  napi_value result;
  napi_status s = napi_create_function(g_env, utf8name,
                                       name_len == 0xFFFFFFFFu ? NAPI_AUTO_LENGTH : (size_t)name_len,
                                       generic_wasm_callback,
                                       (void*)(uintptr_t)reg_id,
                                       &result);
  if (s != napi_ok) return s;
  *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_create_env(int32_t module_api_version,
                                                  uint32_t* env_out,
                                                  uint32_t* scope_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr) {
    napi_status s = unofficial_napi_create_env(module_api_version, &g_env, &g_scope);
    if (s != napi_ok) return s;
  }
  if (env_out != nullptr) *env_out = kUnofficialEnvHandle;
  if (scope_out != nullptr) *scope_out = kUnofficialScopeHandle;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_release_env(uint32_t scope_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr && g_scope == nullptr) return napi_ok;
  if (scope_handle != kUnofficialScopeHandle) return napi_invalid_arg;
  return DisposeBridgeStateLocked();
}

extern "C" int snapi_bridge_unofficial_process_microtasks(uint32_t env_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr) return napi_invalid_arg;
  if (env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  return unofficial_napi_process_microtasks(g_env);
}

extern "C" int snapi_bridge_unofficial_request_gc_for_testing(uint32_t env_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr) return napi_invalid_arg;
  if (env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  return unofficial_napi_request_gc_for_testing(g_env);
}

extern "C" int snapi_bridge_unofficial_get_promise_details(uint32_t env_handle,
                                                           uint32_t promise_id,
                                                           int32_t* state_out,
                                                           uint32_t* result_out,
                                                           int* has_result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value promise = LoadValue(promise_id);
  if (promise == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  bool has_result = false;
  napi_status s =
      unofficial_napi_get_promise_details(g_env, promise, state_out, &result, &has_result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  if (has_result_out != nullptr) *has_result_out = has_result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_proxy_details(uint32_t env_handle,
                                                         uint32_t proxy_id,
                                                         uint32_t* target_out,
                                                         uint32_t* handler_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value proxy = LoadValue(proxy_id);
  if (proxy == nullptr) return napi_invalid_arg;
  napi_value target = nullptr;
  napi_value handler = nullptr;
  napi_status s = unofficial_napi_get_proxy_details(g_env, proxy, &target, &handler);
  if (s != napi_ok) return s;
  if (target_out != nullptr) *target_out = StoreValue(target);
  if (handler_out != nullptr) *handler_out = StoreValue(handler);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_preview_entries(uint32_t env_handle,
                                                       uint32_t value_id,
                                                       uint32_t* entries_out,
                                                       int* is_key_value_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value value = LoadValue(value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value entries = nullptr;
  bool is_key_value = false;
  napi_status s = unofficial_napi_preview_entries(g_env, value, &entries, &is_key_value);
  if (s != napi_ok) return s;
  if (entries_out != nullptr) *entries_out = StoreValue(entries);
  if (is_key_value_out != nullptr) *is_key_value_out = is_key_value ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_call_sites(uint32_t env_handle,
                                                      uint32_t frames,
                                                      uint32_t* callsites_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value callsites = nullptr;
  napi_status s = unofficial_napi_get_call_sites(g_env, frames, &callsites);
  if (s != napi_ok) return s;
  if (callsites_out != nullptr) *callsites_out = StoreValue(callsites);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_caller_location(uint32_t env_handle,
                                                           uint32_t* location_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value location = nullptr;
  napi_status s = unofficial_napi_get_caller_location(g_env, &location);
  if (s != napi_ok) return s;
  if (location_out != nullptr) *location_out = StoreValue(location);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_arraybuffer_view_has_buffer(uint32_t env_handle,
                                                                   uint32_t value_id,
                                                                   int* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value value = LoadValue(value_id);
  if (value == nullptr) return napi_invalid_arg;
  bool result = false;
  napi_status s = unofficial_napi_arraybuffer_view_has_buffer(g_env, value, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_constructor_name(uint32_t env_handle,
                                                            uint32_t value_id,
                                                            uint32_t* name_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value value = LoadValue(value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value name = nullptr;
  napi_status s = unofficial_napi_get_constructor_name(g_env, value, &name);
  if (s != napi_ok) return s;
  if (name_out != nullptr) *name_out = StoreValue(name);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_create_private_symbol(uint32_t env_handle,
                                                             const char* utf8description,
                                                             uint32_t wasm_length,
                                                             uint32_t* out_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  size_t length =
      (wasm_length == 0xFFFFFFFFu) ? NAPI_AUTO_LENGTH : static_cast<size_t>(wasm_length);
  napi_value result = nullptr;
  napi_status s =
      unofficial_napi_create_private_symbol(g_env, utf8description, length, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_continuation_preserved_embedder_data(
    uint32_t env_handle,
    uint32_t* out_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s =
      unofficial_napi_get_continuation_preserved_embedder_data(g_env, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_set_continuation_preserved_embedder_data(
    uint32_t env_handle,
    uint32_t value_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value value = value_id == 0 ? nullptr : LoadValue(value_id);
  if (value_id != 0 && value == nullptr) return napi_invalid_arg;
  return unofficial_napi_set_continuation_preserved_embedder_data(g_env, value);
}

extern "C" int snapi_bridge_unofficial_set_enqueue_foreground_task_callback(
    uint32_t env_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  // Keep parity with the previous bridge behavior (no custom foreground task hook).
  // The runtime still drives microtasks via unofficial_napi_process_microtasks.
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_set_fatal_error_callbacks(
    uint32_t env_handle,
    uint32_t /*fatal_callback_id*/,
    uint32_t /*oom_callback_id*/) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  return unofficial_napi_set_fatal_error_callbacks(g_env, nullptr, nullptr);
}

extern "C" int snapi_bridge_unofficial_terminate_execution(uint32_t env_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  return unofficial_napi_terminate_execution(g_env);
}

extern "C" int snapi_bridge_unofficial_enqueue_microtask(uint32_t env_handle,
                                                         uint32_t callback_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value callback = LoadValue(callback_id);
  if (callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_enqueue_microtask(g_env, callback);
}

extern "C" int snapi_bridge_unofficial_set_promise_reject_callback(uint32_t env_handle,
                                                                   uint32_t callback_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value callback = callback_id == 0 ? nullptr : LoadValue(callback_id);
  if (callback_id != 0 && callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_set_promise_reject_callback(g_env, callback);
}

extern "C" int snapi_bridge_unofficial_get_own_non_index_properties(
    uint32_t env_handle,
    uint32_t value_id,
    uint32_t filter_bits,
    uint32_t* out_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value value = LoadValue(value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s =
      unofficial_napi_get_own_non_index_properties(g_env, value, filter_bits, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_get_process_memory_info(
    uint32_t env_handle,
    double* heap_total_out,
    double* heap_used_out,
    double* external_out,
    double* array_buffers_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  return unofficial_napi_get_process_memory_info(
      g_env, heap_total_out, heap_used_out, external_out, array_buffers_out);
}

extern "C" int snapi_bridge_unofficial_structured_clone(uint32_t env_handle,
                                                        uint32_t value_id,
                                                        uint32_t* out_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value value = LoadValue(value_id);
  if (value == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_structured_clone(g_env, value, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_notify_datetime_configuration_change(
    uint32_t env_handle) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  return unofficial_napi_notify_datetime_configuration_change(g_env);
}

extern "C" int snapi_bridge_unofficial_create_serdes_binding(uint32_t env_handle,
                                                             uint32_t* out_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_create_serdes_binding(g_env, &result);
  if (s != napi_ok) return s;
  if (out_id != nullptr) *out_id = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_contains_module_syntax(
    uint32_t env_handle,
    uint32_t code_id,
    uint32_t filename_id,
    uint32_t resource_name_id,
    int cjs_var_in_scope,
    int* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value code = LoadValue(code_id);
  napi_value filename = LoadValue(filename_id);
  napi_value resource_name = resource_name_id == 0 ? nullptr : LoadValue(resource_name_id);
  if (code == nullptr || filename == nullptr) return napi_invalid_arg;
  if (resource_name_id != 0 && resource_name == nullptr) return napi_invalid_arg;
  bool result = false;
  napi_status s = unofficial_napi_contextify_contains_module_syntax(
      g_env, code, filename, resource_name, cjs_var_in_scope != 0, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_make_context(
    uint32_t env_handle,
    uint32_t sandbox_or_symbol_id,
    uint32_t name_id,
    uint32_t origin_id,
    int allow_code_gen_strings,
    int allow_code_gen_wasm,
    int own_microtask_queue,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value sandbox_or_symbol = LoadValue(sandbox_or_symbol_id);
  napi_value name = LoadValue(name_id);
  napi_value origin = origin_id == 0 ? nullptr : LoadValue(origin_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(host_defined_option_id);
  if (sandbox_or_symbol == nullptr || name == nullptr) return napi_invalid_arg;
  if (origin_id != 0 && origin == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_make_context(
      g_env,
      sandbox_or_symbol,
      name,
      origin,
      allow_code_gen_strings != 0,
      allow_code_gen_wasm != 0,
      own_microtask_queue != 0,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_run_script(
    uint32_t env_handle,
    uint32_t sandbox_or_null_id,
    uint32_t source_id,
    uint32_t filename_id,
    int32_t line_offset,
    int32_t column_offset,
    int64_t timeout,
    int display_errors,
    int break_on_sigint,
    int break_on_first_line,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value sandbox_or_null = sandbox_or_null_id == 0 ? nullptr : LoadValue(sandbox_or_null_id);
  napi_value source = LoadValue(source_id);
  napi_value filename = LoadValue(filename_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(host_defined_option_id);
  if (sandbox_or_null_id != 0 && sandbox_or_null == nullptr) return napi_invalid_arg;
  if (source == nullptr || filename == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_run_script(
      g_env,
      sandbox_or_null,
      source,
      filename,
      line_offset,
      column_offset,
      timeout,
      display_errors != 0,
      break_on_sigint != 0,
      break_on_first_line != 0,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_dispose_context(
    uint32_t env_handle,
    uint32_t sandbox_or_context_global_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value sandbox_or_context_global = LoadValue(sandbox_or_context_global_id);
  if (sandbox_or_context_global == nullptr) return napi_invalid_arg;
  return unofficial_napi_contextify_dispose_context(g_env, sandbox_or_context_global);
}

extern "C" int snapi_bridge_unofficial_contextify_compile_function(
    uint32_t env_handle,
    uint32_t code_id,
    uint32_t filename_id,
    int32_t line_offset,
    int32_t column_offset,
    uint32_t cached_data_id,
    int produce_cached_data,
    uint32_t parsing_context_id,
    uint32_t context_extensions_id,
    uint32_t params_id,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value code = LoadValue(code_id);
  napi_value filename = LoadValue(filename_id);
  napi_value cached_data = cached_data_id == 0 ? nullptr : LoadValue(cached_data_id);
  napi_value parsing_context = parsing_context_id == 0 ? nullptr : LoadValue(parsing_context_id);
  napi_value context_extensions =
      context_extensions_id == 0 ? nullptr : LoadValue(context_extensions_id);
  napi_value params = params_id == 0 ? nullptr : LoadValue(params_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(host_defined_option_id);
  if (code == nullptr || filename == nullptr) return napi_invalid_arg;
  if (cached_data_id != 0 && cached_data == nullptr) return napi_invalid_arg;
  if (parsing_context_id != 0 && parsing_context == nullptr) return napi_invalid_arg;
  if (context_extensions_id != 0 && context_extensions == nullptr) return napi_invalid_arg;
  if (params_id != 0 && params == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_compile_function(
      g_env,
      code,
      filename,
      line_offset,
      column_offset,
      cached_data,
      produce_cached_data != 0,
      parsing_context,
      context_extensions,
      params,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_compile_function_for_cjs_loader(
    uint32_t env_handle,
    uint32_t code_id,
    uint32_t filename_id,
    int is_sea_main,
    int should_detect_module,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value code = LoadValue(code_id);
  napi_value filename = LoadValue(filename_id);
  if (code == nullptr || filename == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_compile_function_for_cjs_loader(
      g_env, code, filename, is_sea_main != 0, should_detect_module != 0, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_contextify_create_cached_data(
    uint32_t env_handle,
    uint32_t code_id,
    uint32_t filename_id,
    int32_t line_offset,
    int32_t column_offset,
    uint32_t host_defined_option_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value code = LoadValue(code_id);
  napi_value filename = LoadValue(filename_id);
  napi_value host_defined_option =
      host_defined_option_id == 0 ? nullptr : LoadValue(host_defined_option_id);
  if (code == nullptr || filename == nullptr) return napi_invalid_arg;
  if (host_defined_option_id != 0 && host_defined_option == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_contextify_create_cached_data(
      g_env,
      code,
      filename,
      line_offset,
      column_offset,
      host_defined_option,
      &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_create_source_text(
    uint32_t env_handle,
    uint32_t wrapper_id,
    uint32_t url_id,
    uint32_t context_id,
    uint32_t source_id,
    int32_t line_offset,
    int32_t column_offset,
    uint32_t cached_data_or_id,
    uint32_t* handle_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value wrapper = LoadValue(wrapper_id);
  napi_value url = LoadValue(url_id);
  napi_value context = context_id == 0 ? nullptr : LoadValue(context_id);
  napi_value source = LoadValue(source_id);
  napi_value cached_data = cached_data_or_id == 0 ? nullptr : LoadValue(cached_data_or_id);
  if (wrapper == nullptr || url == nullptr || source == nullptr) return napi_invalid_arg;
  if (context_id != 0 && context == nullptr) return napi_invalid_arg;
  if (cached_data_or_id != 0 && cached_data == nullptr) return napi_invalid_arg;
  void* handle = nullptr;
  napi_status s = unofficial_napi_module_wrap_create_source_text(
      g_env, wrapper, url, context, source, line_offset, column_offset, cached_data, &handle);
  if (s != napi_ok) return s;
  if (handle_out != nullptr) *handle_out = StoreModuleWrapHandle(handle);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_create_synthetic(
    uint32_t env_handle,
    uint32_t wrapper_id,
    uint32_t url_id,
    uint32_t context_id,
    uint32_t export_names_id,
    uint32_t synthetic_eval_steps_id,
    uint32_t* handle_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value wrapper = LoadValue(wrapper_id);
  napi_value url = LoadValue(url_id);
  napi_value context = context_id == 0 ? nullptr : LoadValue(context_id);
  napi_value export_names = LoadValue(export_names_id);
  napi_value synthetic_eval_steps = LoadValue(synthetic_eval_steps_id);
  if (wrapper == nullptr || url == nullptr || export_names == nullptr ||
      synthetic_eval_steps == nullptr) {
    return napi_invalid_arg;
  }
  if (context_id != 0 && context == nullptr) return napi_invalid_arg;
  void* handle = nullptr;
  napi_status s = unofficial_napi_module_wrap_create_synthetic(
      g_env, wrapper, url, context, export_names, synthetic_eval_steps, &handle);
  if (s != napi_ok) return s;
  if (handle_out != nullptr) *handle_out = StoreModuleWrapHandle(handle);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_destroy(uint32_t env_handle,
                                                           uint32_t handle_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_status s = unofficial_napi_module_wrap_destroy(g_env, handle);
  if (s == napi_ok) RemoveModuleWrapHandle(handle_id);
  return s;
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_module_requests(
    uint32_t env_handle,
    uint32_t handle_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_get_module_requests(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_link(uint32_t env_handle,
                                                        uint32_t handle_id,
                                                        uint32_t count,
                                                        const uint32_t* linked_handle_ids) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  std::vector<void*> linked_handles(count, nullptr);
  for (uint32_t i = 0; i < count; ++i) {
    void* linked = linked_handle_ids != nullptr ? LoadModuleWrapHandle(linked_handle_ids[i]) : nullptr;
    if (linked == nullptr) return napi_invalid_arg;
    linked_handles[i] = linked;
  }
  return unofficial_napi_module_wrap_link(
      g_env, handle, count, count == 0 ? nullptr : linked_handles.data());
}

extern "C" int snapi_bridge_unofficial_module_wrap_instantiate(uint32_t env_handle,
                                                               uint32_t handle_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_instantiate(g_env, handle);
}

extern "C" int snapi_bridge_unofficial_module_wrap_evaluate(uint32_t env_handle,
                                                            uint32_t handle_id,
                                                            int64_t timeout,
                                                            int break_on_sigint,
                                                            uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_evaluate(
      g_env, handle, timeout, break_on_sigint != 0, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_evaluate_sync(uint32_t env_handle,
                                                                 uint32_t handle_id,
                                                                 uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_evaluate_sync(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_namespace(uint32_t env_handle,
                                                                 uint32_t handle_id,
                                                                 uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_get_namespace(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_status(uint32_t env_handle,
                                                              uint32_t handle_id,
                                                              int32_t* status_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_get_status(g_env, handle, status_out);
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_error(uint32_t env_handle,
                                                             uint32_t handle_id,
                                                             uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_get_error(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_has_top_level_await(
    uint32_t env_handle,
    uint32_t handle_id,
    int* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  bool result = false;
  napi_status s = unofficial_napi_module_wrap_has_top_level_await(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_has_async_graph(uint32_t env_handle,
                                                                   uint32_t handle_id,
                                                                   int* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  bool result = false;
  napi_status s = unofficial_napi_module_wrap_has_async_graph(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = result ? 1 : 0;
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_set_export(uint32_t env_handle,
                                                              uint32_t handle_id,
                                                              uint32_t export_name_id,
                                                              uint32_t export_value_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  napi_value export_name = LoadValue(export_name_id);
  napi_value export_value = export_value_id == 0 ? nullptr : LoadValue(export_value_id);
  if (handle == nullptr || export_name == nullptr) return napi_invalid_arg;
  if (export_value_id != 0 && export_value == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_set_export(g_env, handle, export_name, export_value);
}

extern "C" int snapi_bridge_unofficial_module_wrap_set_module_source_object(
    uint32_t env_handle,
    uint32_t handle_id,
    uint32_t source_object_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  napi_value source_object = source_object_id == 0 ? nullptr : LoadValue(source_object_id);
  if (handle == nullptr) return napi_invalid_arg;
  if (source_object_id != 0 && source_object == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_set_module_source_object(g_env, handle, source_object);
}

extern "C" int snapi_bridge_unofficial_module_wrap_get_module_source_object(
    uint32_t env_handle,
    uint32_t handle_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_get_module_source_object(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_create_cached_data(
    uint32_t env_handle,
    uint32_t handle_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_create_cached_data(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_set_import_module_dynamically_callback(
    uint32_t env_handle,
    uint32_t callback_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value callback = callback_id == 0 ? nullptr : LoadValue(callback_id);
  if (callback_id != 0 && callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_set_import_module_dynamically_callback(g_env, callback);
}

extern "C" int
snapi_bridge_unofficial_module_wrap_set_initialize_import_meta_object_callback(
    uint32_t env_handle,
    uint32_t callback_id) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  napi_value callback = callback_id == 0 ? nullptr : LoadValue(callback_id);
  if (callback_id != 0 && callback == nullptr) return napi_invalid_arg;
  return unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(g_env, callback);
}

extern "C" int snapi_bridge_unofficial_module_wrap_import_module_dynamically(
    uint32_t env_handle,
    uint32_t argc,
    const uint32_t* argv_ids,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  std::vector<napi_value> argv(argc, nullptr);
  for (uint32_t i = 0; i < argc; ++i) {
    napi_value value = LoadValue(argv_ids[i]);
    if (value == nullptr) return napi_invalid_arg;
    argv[i] = value;
  }
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_import_module_dynamically(
      g_env, argc, argc == 0 ? nullptr : argv.data(), &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

extern "C" int snapi_bridge_unofficial_module_wrap_create_required_module_facade(
    uint32_t env_handle,
    uint32_t handle_id,
    uint32_t* result_out) {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  if (g_env == nullptr || env_handle != kUnofficialEnvHandle) return napi_invalid_arg;
  void* handle = LoadModuleWrapHandle(handle_id);
  if (handle == nullptr) return napi_invalid_arg;
  napi_value result = nullptr;
  napi_status s = unofficial_napi_module_wrap_create_required_module_facade(g_env, handle, &result);
  if (s != napi_ok) return s;
  if (result_out != nullptr) *result_out = StoreValue(result);
  return napi_ok;
}

// ============================================================
// Cleanup
// ============================================================

extern "C" void snapi_bridge_dispose() {
  std::lock_guard<std::recursive_mutex> lock(g_mu);
  (void)DisposeBridgeStateLocked();
}
