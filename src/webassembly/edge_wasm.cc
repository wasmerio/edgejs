#include "webassembly/edge_wasm.h"

#include "edge_environment.h"
#include "internal_binding/helpers.h"
#include "unofficial_napi.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if defined(EDGE_QUICKJS_WEBASSEMBLY_IMPORTS) && !defined(WASM_API_EXTERN)
#define WASM_API_EXTERN __attribute__((import_module("wasm_c_api_v0")))
#endif

#include <wasm.h>

// Wasmer extensions needed because the version of wasm.h used by the C API
// does not carry the shared bit in wasm_limits_t and only standardizes shared
// module handles, not shared memories.
extern "C" {
typedef struct wasm_shared_memory_t wasm_shared_memory_t;
WASM_API_EXTERN wasm_memorytype_t *
wasm_shared_memorytype_new(const wasm_limits_t *limits);
WASM_API_EXTERN bool
wasm_memorytype_is_shared(const wasm_memorytype_t *memory_type);
#if defined(__wasi__)
WASM_API_EXTERN bool wasm_memory_read(const wasm_memory_t *memory,
                                      uint64_t offset, uint8_t *destination,
                                      size_t length);
WASM_API_EXTERN bool wasm_memory_write(const wasm_memory_t *memory,
                                       uint64_t offset,
                                       const uint8_t *source,
                                       size_t length);
WASM_API_EXTERN bool wasm_memory_atomic(
    const wasm_memory_t *memory, uint64_t offset, int operation, int width,
    uint64_t value, uint64_t replacement, uint64_t *result);
WASM_API_EXTERN int wasm_memory_atomic_wait(const wasm_memory_t *memory,
                                            uint64_t offset, int width,
                                            int64_t expected,
                                            int64_t timeout_nanos);
WASM_API_EXTERN int wasm_memory_atomic_notify(const wasm_memory_t *memory,
                                              uint64_t offset,
                                              uint32_t count);
#endif
WASM_API_EXTERN wasm_shared_memory_t *
wasm_memory_share(const wasm_memory_t *memory);
WASM_API_EXTERN wasm_memory_t *
wasm_memory_obtain(wasm_store_t *store,
                   const wasm_shared_memory_t *shared_memory);
WASM_API_EXTERN void
wasm_shared_memory_delete(wasm_shared_memory_t *shared_memory);
}

namespace {

constexpr uint32_t kInternalCreateMagic = 0x45574153; // EWAS
constexpr uint32_t kCloneBackingMagic = 0x4557434c;  // EWCL

enum class WasmObjectKind : uint32_t {
  kModule = 1,
  kInstance,
  kMemory,
  kTable,
  kGlobal,
  kFunction,
};

struct WasmState;

struct WasmObjectBase {
  WasmObjectKind kind;
  WasmState *state;
};

struct WasmModuleObject {
  WasmObjectBase base;
  wasm_module_t *module = nullptr;
};

struct WasmInstanceObject {
  WasmObjectBase base;
  wasm_instance_t *instance = nullptr;
  napi_ref exports_ref = nullptr;
};

struct WasmMemoryObject {
  WasmObjectBase base;
  wasm_memory_t *memory = nullptr;
  // Cached .buffer ArrayBuffer, detached (and re-minted on next access) when
  // the backing wasm memory grows/moves — mirroring the JS API's
  // detach-on-grow semantics that wasm-bindgen's view caching relies on.
  napi_ref buffer_ref = nullptr;
  void *buffer_data = nullptr;
  size_t buffer_size = 0;
  bool buffer_host_backed = false;
  bool shared = false;
};

struct WasmTableObject {
  WasmObjectBase base;
  wasm_table_t *table = nullptr;
};

struct WasmGlobalObject {
  WasmObjectBase base;
  wasm_global_t *global = nullptr;
};

struct WasmFunctionObject {
  WasmObjectBase base;
  wasm_func_t *func = nullptr;
};

struct InternalCreate {
  uint32_t magic = kInternalCreateMagic;
  WasmObjectKind kind = WasmObjectKind::kModule;
  void *ptr = nullptr;
};

struct WasmCloneBacking {
  uint32_t magic = kCloneBackingMagic;
  WasmObjectKind kind = WasmObjectKind::kModule;
  void *shared = nullptr;
};

struct HostMemoryBacking {
  wasm_memory_t *memory = nullptr;
};

void DeleteRefIfPresent(napi_env env, napi_ref *ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr)
    return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

struct WasmState {
  explicit WasmState(napi_env env_in) : env(env_in) {}

  ~WasmState() {
    // Environment cleanup destroys slot-owned state before N-API releases the
    // remaining JavaScript wrappers. Those wrappers may therefore be finalized
    // after this destructor. Detach their non-owning state pointers first so a
    // late MemoryFinalize cannot touch this destroyed live-memory registry.
    for (WasmMemoryObject *memory : live_memories) {
      if (memory != nullptr)
        memory->base.state = nullptr;
    }
    live_memories.clear();
    DeleteRefIfPresent(env, &externref_values_ref);
    DeleteRefIfPresent(env, &pending_import_exception_ref);
    DeleteRefIfPresent(env, &webassembly_ref);
    DeleteRefIfPresent(env, &module_ctor_ref);
    DeleteRefIfPresent(env, &instance_ctor_ref);
    DeleteRefIfPresent(env, &memory_ctor_ref);
    DeleteRefIfPresent(env, &table_ctor_ref);
    DeleteRefIfPresent(env, &global_ctor_ref);
    if (store != nullptr)
      wasm_store_delete(store);
    if (engine != nullptr)
      wasm_engine_delete(engine);
  }

  bool Initialize(std::string *error_out) {
    if (engine != nullptr && store != nullptr)
      return true;
    engine = wasm_engine_new();
    if (engine == nullptr) {
      if (error_out != nullptr)
        *error_out = "wasm_engine_new failed";
      return false;
    }
    store = wasm_store_new(engine);
    if (store == nullptr) {
      if (error_out != nullptr)
        *error_out = "wasm_store_new failed";
      return false;
    }
    return true;
  }

  napi_env env = nullptr;
  wasm_engine_t *engine = nullptr;
  wasm_store_t *store = nullptr;
  napi_ref webassembly_ref = nullptr;
  napi_ref module_ctor_ref = nullptr;
  napi_ref instance_ctor_ref = nullptr;
  napi_ref memory_ctor_ref = nullptr;
  napi_ref table_ctor_ref = nullptr;
  napi_ref global_ctor_ref = nullptr;
  napi_ref pending_import_exception_ref = nullptr;
  // Externref registry: JS values passed into wasm as externrefs are rooted
  // in a JS array; the array index rides inside the wasm-side foreign object
  // as host info. Entries are never released — the wasmer store has no
  // reference lifetime management yet (WARP-70 Part B), so the extern objects
  // leak until store death regardless.
  napi_ref externref_values_ref = nullptr;
  uint32_t next_externref_id = 1;
  // Live memory objects whose cached buffers must be revalidated at JS↔wasm
  // boundaries (wasm-internal memory.grow has no JS-side hook).
  std::vector<WasmMemoryObject *> live_memories;
};

struct ImportFuncData {
  WasmState *state = nullptr;
  napi_env env = nullptr;
  napi_ref function_ref = nullptr;
  std::vector<wasm_valkind_t> result_kinds;
};

napi_value Undefined(napi_env env) { return internal_binding::Undefined(env); }

napi_value Null(napi_env env) {
  napi_value value = nullptr;
  napi_get_null(env, &value);
  return value;
}

bool IsUndefined(napi_env env, napi_value value) {
  return value == nullptr || internal_binding::IsUndefined(env, value);
}

bool IsNullOrUndefined(napi_env env, napi_value value) {
  if (IsUndefined(env, value))
    return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_null;
}

std::string GetString(napi_env env, napi_value value) {
  if (value == nullptr)
    return std::string();
  size_t length = 0;
  if (napi_get_value_string_utf8(env, value, nullptr, 0, &length) != napi_ok) {
    return std::string();
  }
  std::vector<char> buffer(length + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, value, buffer.data(), buffer.size(),
                                 &copied) != napi_ok) {
    return std::string();
  }
  return std::string(buffer.data(), copied);
}

std::string NameToString(const wasm_name_t *name) {
  if (name == nullptr || name->data == nullptr || name->size == 0)
    return std::string();
  size_t length = name->size;
  if (length > 0 && name->data[length - 1] == '\0')
    --length;
  return std::string(name->data, name->data + length);
}

napi_value MakeString(napi_env env, const std::string &value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value.data(), value.size(), &out) !=
      napi_ok) {
    return Undefined(env);
  }
  return out;
}

napi_value MakeString(napi_env env, const char *value) {
  napi_value out = nullptr;
  if (napi_create_string_utf8(env, value, NAPI_AUTO_LENGTH, &out) != napi_ok) {
    return Undefined(env);
  }
  return out;
}

bool GetNamed(napi_env env, napi_value object, const char *name,
              napi_value *out) {
  if (out == nullptr)
    return false;
  *out = nullptr;
  if (object == nullptr || name == nullptr)
    return false;
  bool has = false;
  if (napi_has_named_property(env, object, name, &has) != napi_ok || !has) {
    *out = Undefined(env);
    return true;
  }
  return napi_get_named_property(env, object, name, out) == napi_ok &&
         *out != nullptr;
}

bool SetNamed(napi_env env, napi_value object, const char *name,
              napi_value value) {
  return object != nullptr && name != nullptr && value != nullptr &&
         napi_set_named_property(env, object, name, value) == napi_ok;
}

bool SetNamedString(napi_env env, napi_value object, const char *name,
                    const char *value) {
  return SetNamed(env, object, name, MakeString(env, value));
}

bool SetNamedString(napi_env env, napi_value object, const char *name,
                    const std::string &value) {
  return SetNamed(env, object, name, MakeString(env, value));
}

bool SetNamedUint32(napi_env env, napi_value object, const char *name,
                    uint32_t value) {
  napi_value js_value = nullptr;
  if (napi_create_uint32(env, value, &js_value) != napi_ok ||
      js_value == nullptr)
    return false;
  return SetNamed(env, object, name, js_value);
}

bool GetRequiredUint32(napi_env env, napi_value object, const char *name,
                       uint32_t *out) {
  napi_value value = nullptr;
  if (!GetNamed(env, object, name, &value) || IsUndefined(env, value))
    return false;
  return napi_get_value_uint32(env, value, out) == napi_ok;
}

bool GetOptionalUint32(napi_env env, napi_value object, const char *name,
                       uint32_t fallback, uint32_t *out) {
  napi_value value = nullptr;
  if (!GetNamed(env, object, name, &value) || IsUndefined(env, value)) {
    *out = fallback;
    return true;
  }
  return napi_get_value_uint32(env, value, out) == napi_ok;
}

bool GetOptionalBool(napi_env env, napi_value object, const char *name,
                     bool fallback, bool *out) {
  napi_value value = nullptr;
  if (!GetNamed(env, object, name, &value) || IsUndefined(env, value)) {
    *out = fallback;
    return true;
  }
  return napi_get_value_bool(env, value, out) == napi_ok;
}

napi_value CreateErrorObject(napi_env env, const char *name,
                             const std::string &message) {
  napi_value message_value = MakeString(env, message);
  napi_value error = nullptr;
  napi_value global = nullptr;
  napi_value webassembly = nullptr;
  napi_value ctor = nullptr;
  bool used_wasm_ctor = false;
  if (napi_get_global(env, &global) == napi_ok &&
      GetNamed(env, global, "WebAssembly", &webassembly) &&
      !IsUndefined(env, webassembly) &&
      GetNamed(env, webassembly, name, &ctor) && !IsUndefined(env, ctor)) {
    napi_valuetype ctor_type = napi_undefined;
    if (napi_typeof(env, ctor, &ctor_type) == napi_ok &&
        ctor_type == napi_function) {
      napi_value argv[1] = {message_value};
      used_wasm_ctor =
          napi_new_instance(env, ctor, 1, argv, &error) == napi_ok &&
          error != nullptr;
    }
  }
  if (!used_wasm_ctor &&
      napi_create_error(env, nullptr, message_value, &error) != napi_ok) {
    return nullptr;
  }
  if (error != nullptr) {
    SetNamedString(env, error, "name", name);
  }
  return error;
}

void ThrowWasmError(napi_env env, const char *name,
                    const std::string &message) {
  napi_value error = CreateErrorObject(env, name, message);
  if (error != nullptr) {
    napi_throw(env, error);
  } else {
    napi_throw_error(env, nullptr, message.c_str());
  }
}

void RejectDeferredWithWasmError(napi_env env, napi_deferred deferred,
                                 const char *name, const std::string &message) {
  napi_value error = CreateErrorObject(env, name, message);
  if (error == nullptr) {
    napi_create_error(env, nullptr, MakeString(env, message), &error);
  }
  napi_reject_deferred(env, deferred, error);
}

bool ReadBufferSource(napi_env env, napi_value value, const wasm_byte_t **data,
                      size_t *length) {
  if (data == nullptr || length == nullptr)
    return false;
  *data = nullptr;
  *length = 0;

  bool is_typed_array = false;
  if (napi_is_typedarray(env, value, &is_typed_array) == napi_ok &&
      is_typed_array) {
    napi_typedarray_type type = napi_uint8_array;
    size_t element_count = 0;
    void *raw = nullptr;
    if (napi_get_typedarray_info(env, value, &type, &element_count, &raw,
                                 nullptr, nullptr) != napi_ok) {
      return false;
    }
    size_t element_size = 1;
    switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      element_size = 1;
      break;
    case napi_int16_array:
    case napi_uint16_array:
    case napi_float16_array:
      element_size = 2;
      break;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      element_size = 4;
      break;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      element_size = 8;
      break;
    }
    *data = static_cast<const wasm_byte_t *>(raw);
    *length = element_count * element_size;
    return true;
  }

  bool is_data_view = false;
  if (napi_is_dataview(env, value, &is_data_view) == napi_ok && is_data_view) {
    void *raw = nullptr;
    if (napi_get_dataview_info(env, value, length, &raw, nullptr, nullptr) !=
        napi_ok) {
      return false;
    }
    *data = static_cast<const wasm_byte_t *>(raw);
    return true;
  }

  bool is_array_buffer = false;
  if (napi_is_arraybuffer(env, value, &is_array_buffer) == napi_ok &&
      is_array_buffer) {
    void *raw = nullptr;
    if (napi_get_arraybuffer_info(env, value, &raw, length) != napi_ok) {
      return false;
    }
    *data = static_cast<const wasm_byte_t *>(raw);
    return true;
  }

  return false;
}

wasm_byte_vec_t BorrowedByteVec(const wasm_byte_t *data, size_t length) {
  wasm_byte_vec_t bytes;
  bytes.size = length;
  bytes.data = const_cast<wasm_byte_t *>(data);
  return bytes;
}

WasmState *GetState(napi_env env) {
  return EdgeEnvironmentGetSlotData<WasmState>(
      env, kEdgeEnvironmentSlotQuickJsWebAssemblyState);
}

WasmState *EnsureState(napi_env env, std::string *error_out) {
  if (auto *existing = GetState(env); existing != nullptr)
    return existing;
  auto *state = new WasmState(env);
  if (!state->Initialize(error_out)) {
    delete state;
    return nullptr;
  }
  EdgeEnvironmentSetOpaqueSlot(
      env, kEdgeEnvironmentSlotQuickJsWebAssemblyState, state,
      [](void *data) { delete static_cast<WasmState *>(data); });
  return state;
}

template <typename T>
T *Unwrap(napi_env env, napi_value value, WasmObjectKind kind) {
  if (value == nullptr)
    return nullptr;
  void *data = nullptr;
  if (napi_unwrap(env, value, &data) != napi_ok || data == nullptr)
    return nullptr;
  auto *base = static_cast<WasmObjectBase *>(data);
  if (base->kind != kind)
    return nullptr;
  return static_cast<T *>(data);
}

bool GetCallback(napi_env env, napi_callback_info info, size_t *argc,
                 napi_value *argv, napi_value *this_arg, void **data) {
  return napi_get_cb_info(env, info, argc, argv, this_arg, data) == napi_ok;
}

bool TryConsumeInternalCreate(napi_env env, napi_value value,
                              WasmObjectKind expected_kind, void **out) {
  if (out == nullptr)
    return false;
  *out = nullptr;
  void *external = nullptr;
  if (value == nullptr ||
      napi_get_value_external(env, value, &external) != napi_ok ||
      external == nullptr) {
    return false;
  }
  auto *init = static_cast<InternalCreate *>(external);
  if (init->magic != kInternalCreateMagic || init->kind != expected_kind ||
      init->ptr == nullptr) {
    return false;
  }
  *out = init->ptr;
  init->ptr = nullptr;
  return true;
}

napi_value GetConstructor(WasmState *state, WasmObjectKind kind) {
  napi_ref ref = nullptr;
  switch (kind) {
  case WasmObjectKind::kModule:
    ref = state->module_ctor_ref;
    break;
  case WasmObjectKind::kInstance:
    ref = state->instance_ctor_ref;
    break;
  case WasmObjectKind::kMemory:
    ref = state->memory_ctor_ref;
    break;
  case WasmObjectKind::kTable:
    ref = state->table_ctor_ref;
    break;
  case WasmObjectKind::kGlobal:
    ref = state->global_ctor_ref;
    break;
  case WasmObjectKind::kFunction:
    break;
  }
  napi_value ctor = nullptr;
  if (ref != nullptr)
    napi_get_reference_value(state->env, ref, &ctor);
  return ctor;
}

void DeleteOwnedByKind(WasmObjectKind kind, void *ptr) {
  if (ptr == nullptr)
    return;
  switch (kind) {
  case WasmObjectKind::kModule:
    wasm_module_delete(static_cast<wasm_module_t *>(ptr));
    break;
  case WasmObjectKind::kInstance:
    wasm_instance_delete(static_cast<wasm_instance_t *>(ptr));
    break;
  case WasmObjectKind::kMemory:
    wasm_memory_delete(static_cast<wasm_memory_t *>(ptr));
    break;
  case WasmObjectKind::kTable:
    wasm_table_delete(static_cast<wasm_table_t *>(ptr));
    break;
  case WasmObjectKind::kGlobal:
    wasm_global_delete(static_cast<wasm_global_t *>(ptr));
    break;
  case WasmObjectKind::kFunction:
    wasm_func_delete(static_cast<wasm_func_t *>(ptr));
    break;
  }
}

napi_value CreateObjectFromOwned(WasmState *state, WasmObjectKind kind,
                                 void *ptr) {
  if (state == nullptr || ptr == nullptr)
    return nullptr;
  napi_env env = state->env;
  napi_value ctor = GetConstructor(state, kind);
  if (ctor == nullptr)
    return nullptr;

  InternalCreate init;
  init.kind = kind;
  init.ptr = ptr;

  napi_value external = nullptr;
  if (napi_create_external(env, &init, nullptr, nullptr, &external) !=
          napi_ok ||
      external == nullptr) {
    return nullptr;
  }
  napi_value argv[1] = {external};
  napi_value object = nullptr;
  napi_status status = napi_new_instance(env, ctor, 1, argv, &object);
  if (init.ptr != nullptr) {
    DeleteOwnedByKind(kind, init.ptr);
    init.ptr = nullptr;
  }
  return status == napi_ok ? object : nullptr;
}

const char *ExternKindName(wasm_externkind_t kind) {
  switch (kind) {
  case WASM_EXTERN_FUNC:
    return "function";
  case WASM_EXTERN_GLOBAL:
    return "global";
  case WASM_EXTERN_TABLE:
    return "table";
  case WASM_EXTERN_MEMORY:
    return "memory";
  default:
    return "unknown";
  }
}

bool ParseValueKind(napi_env env, napi_value value, wasm_valkind_t *out) {
  if (out == nullptr || value == nullptr)
    return false;
  std::string kind = GetString(env, value);
  if (kind == "i32") {
    *out = WASM_I32;
    return true;
  }
  if (kind == "i64") {
    *out = WASM_I64;
    return true;
  }
  if (kind == "f32") {
    *out = WASM_F32;
    return true;
  }
  if (kind == "f64") {
    *out = WASM_F64;
    return true;
  }
  if (kind == "externref") {
    *out = WASM_EXTERNREF;
    return true;
  }
  if (kind == "funcref" || kind == "anyfunc") {
    *out = WASM_FUNCREF;
    return true;
  }
  return false;
}

napi_value CreateFunctionObject(WasmState *state, const std::string &name,
                                wasm_func_t *owned_func);

napi_value ExternrefRegistry(WasmState *state) {
  if (state == nullptr)
    return nullptr;
  napi_env env = state->env;
  napi_value registry = nullptr;
  if (state->externref_values_ref != nullptr) {
    if (napi_get_reference_value(env, state->externref_values_ref,
                                 &registry) != napi_ok)
      return nullptr;
    return registry;
  }
  if (napi_create_array(env, &registry) != napi_ok || registry == nullptr)
    return nullptr;
  if (napi_create_reference(env, registry, 1, &state->externref_values_ref) !=
      napi_ok) {
    state->externref_values_ref = nullptr;
    return nullptr;
  }
  return registry;
}

// Mints an owned wasm reference for an arbitrary JS value. Only JS null (and
// an absent value) maps to the null reference; undefined is a real externref
// value (wasm-bindgen roots it in a sentinel table slot).
bool JsToExternRef(WasmState *state, napi_value value, wasm_ref_t **out) {
  if (state == nullptr || out == nullptr)
    return false;
  *out = nullptr;
  if (value == nullptr)
    return true;
  napi_env env = state->env;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok)
    return false;
  if (type == napi_null)
    return true;
  napi_value registry = ExternrefRegistry(state);
  if (registry == nullptr)
    return false;
  uint32_t id = state->next_externref_id++;
  if (napi_set_element(env, registry, id, value) != napi_ok)
    return false;
  wasm_foreign_t *foreign = wasm_foreign_new(state->store);
  if (foreign == nullptr)
    return false;
  wasm_ref_t *ref = wasm_foreign_as_ref(foreign);
  wasm_ref_set_host_info(ref,
                         reinterpret_cast<void *>(static_cast<uintptr_t>(id)));
  *out = ref;
  return true;
}

bool JsToFuncRef(WasmState *state, napi_value value, wasm_ref_t **out) {
  if (state == nullptr || out == nullptr)
    return false;
  *out = nullptr;
  if (value == nullptr || IsNullOrUndefined(state->env, value))
    return true;
  auto *wrapped =
      Unwrap<WasmFunctionObject>(state->env, value, WasmObjectKind::kFunction);
  if (wrapped == nullptr || wrapped->func == nullptr)
    return false;
  *out = wasm_func_as_ref(wrapped->func);
  return *out != nullptr;
}

bool JsToRef(WasmState *state, napi_value value, wasm_valkind_t kind,
             wasm_ref_t **out) {
  return kind == WASM_FUNCREF ? JsToFuncRef(state, value, out)
                              : JsToExternRef(state, value, out);
}

// Maps a wasm reference back to JS. Externrefs minted by JsToExternRef
// round-trip to the exact same JS value via the registry; funcrefs wrap into
// fresh callable function objects (identity across round-trips is not
// preserved, which the JS API spec permits). Borrows `ref`.
napi_value RefToJs(WasmState *state, wasm_ref_t *ref) {
  if (state == nullptr)
    return nullptr;
  napi_env env = state->env;
  if (ref == nullptr)
    return Null(env);
  if (wasm_func_t *func = wasm_ref_as_func(ref); func != nullptr)
    return CreateFunctionObject(state, std::string(), func);
  uintptr_t id = reinterpret_cast<uintptr_t>(wasm_ref_get_host_info(ref));
  if (id == 0)
    return Null(env);
  napi_value registry = ExternrefRegistry(state);
  napi_value out = nullptr;
  if (registry == nullptr ||
      napi_get_element(env, registry, static_cast<uint32_t>(id), &out) !=
          napi_ok)
    return Null(env);
  return out;
}

void RefreshMemoryView(WasmMemoryObject *object) {
  if (object == nullptr || object->buffer_ref == nullptr ||
      object->memory == nullptr)
    return;
  void *data = wasm_memory_data(object->memory);
  const size_t size = object->shared
                          ? static_cast<size_t>(wasm_memory_size(object->memory)) *
                                65'536
                          : wasm_memory_data_size(object->memory);
  const bool same_backing = object->buffer_host_backed
                                ? data == nullptr
                                : data == object->buffer_data;
  if (same_backing && size == object->buffer_size)
    return;
  napi_env env = object->base.state->env;
  napi_value buffer = nullptr;
  if (!object->shared &&
      napi_get_reference_value(env, object->buffer_ref, &buffer) == napi_ok &&
      buffer != nullptr)
    napi_detach_arraybuffer(env, buffer);
  DeleteRefIfPresent(env, &object->buffer_ref);
  object->buffer_data = nullptr;
  object->buffer_size = 0;
  object->buffer_host_backed = false;
}

bool IsSharedMemory(wasm_memory_t *memory) {
  if (memory == nullptr)
    return false;
  wasm_memorytype_t *type = wasm_memory_type(memory);
  if (type == nullptr)
    return false;
  const bool shared = wasm_memorytype_is_shared(type);
  wasm_memorytype_delete(type);
  return shared;
}

void SharedMemoryBufferFinalize(napi_env, void *, void *hint) {
  auto *shared = static_cast<wasm_shared_memory_t *>(hint);
  if (shared != nullptr)
    wasm_shared_memory_delete(shared);
}

int NAPI_CDECL HostMemoryRead(void *hint, size_t offset, void *destination,
                              size_t length) {
  auto *backing = static_cast<HostMemoryBacking *>(hint);
#if defined(__wasi__)
  return backing != nullptr && backing->memory != nullptr &&
                 wasm_memory_read(backing->memory, offset,
                                  static_cast<uint8_t *>(destination), length)
             ? 0
             : -1;
#else
  if (backing == nullptr || backing->memory == nullptr ||
      offset > wasm_memory_data_size(backing->memory) ||
      length > wasm_memory_data_size(backing->memory) - offset)
    return -1;
  std::memcpy(destination, wasm_memory_data(backing->memory) + offset, length);
  return 0;
#endif
}

int NAPI_CDECL HostMemoryWrite(void *hint, size_t offset, const void *source,
                               size_t length) {
  auto *backing = static_cast<HostMemoryBacking *>(hint);
#if defined(__wasi__)
  return backing != nullptr && backing->memory != nullptr &&
                 wasm_memory_write(backing->memory, offset,
                                   static_cast<const uint8_t *>(source),
                                   length)
             ? 0
             : -1;
#else
  if (backing == nullptr || backing->memory == nullptr ||
      offset > wasm_memory_data_size(backing->memory) ||
      length > wasm_memory_data_size(backing->memory) - offset)
    return -1;
  std::memcpy(wasm_memory_data(backing->memory) + offset, source, length);
  return 0;
#endif
}

int NAPI_CDECL HostMemoryAtomic(void *hint, size_t offset, int operation,
                               int width, uint64_t value, uint64_t replacement,
                               uint64_t *result) {
  auto *backing = static_cast<HostMemoryBacking *>(hint);
#if defined(__wasi__)
  return backing != nullptr && backing->memory != nullptr && result != nullptr &&
                 wasm_memory_atomic(backing->memory, offset, operation, width,
                                    value, replacement, result)
             ? 0
             : -1;
#else
  return -1;
#endif
}

int NAPI_CDECL HostMemoryWait(void *hint, size_t offset, int width,
                             int64_t expected, int64_t timeout_nanos) {
  auto *backing = static_cast<HostMemoryBacking *>(hint);
#if defined(__wasi__)
  return backing != nullptr && backing->memory != nullptr
             ? wasm_memory_atomic_wait(backing->memory, offset, width, expected,
                                       timeout_nanos)
             : -1;
#else
  return -1;
#endif
}

int NAPI_CDECL HostMemoryNotify(void *hint, size_t offset, uint32_t count) {
  auto *backing = static_cast<HostMemoryBacking *>(hint);
#if defined(__wasi__)
  return backing != nullptr && backing->memory != nullptr
             ? wasm_memory_atomic_notify(backing->memory, offset, count)
             : -1;
#else
  return -1;
#endif
}

void HostMemoryBufferFinalize(napi_env, void *, void *hint) {
  auto *backing = static_cast<HostMemoryBacking *>(hint);
  if (backing == nullptr)
    return;
  if (backing->memory != nullptr)
    wasm_memory_delete(backing->memory);
  delete backing;
}

void WasmCloneBackingFinalize(napi_env, void *, void *hint) {
  auto *backing = static_cast<WasmCloneBacking *>(hint);
  if (backing == nullptr)
    return;
  if (backing->magic == kCloneBackingMagic && backing->shared != nullptr) {
    if (backing->kind == WasmObjectKind::kModule) {
      wasm_shared_module_delete(
          static_cast<wasm_shared_module_t *>(backing->shared));
    } else if (backing->kind == WasmObjectKind::kMemory) {
      wasm_shared_memory_delete(
          static_cast<wasm_shared_memory_t *>(backing->shared));
    }
  }
  backing->magic = 0;
  delete backing;
}

void RefreshMemoryViews(WasmState *state) {
  if (state == nullptr)
    return;
  for (auto *object : state->live_memories)
    RefreshMemoryView(object);
}

bool TableElementKind(wasm_table_t *table, wasm_valkind_t *out) {
  if (table == nullptr || out == nullptr)
    return false;
  wasm_tabletype_t *type = wasm_table_type(table);
  if (type == nullptr)
    return false;
  *out = wasm_valtype_kind(wasm_tabletype_element(type));
  wasm_tabletype_delete(type);
  return true;
}

bool JsToWasmVal(WasmState *state, napi_env env, napi_value value,
                 wasm_valkind_t kind, wasm_val_t *out) {
  if (out == nullptr)
    return false;
  out->kind = kind;
  switch (kind) {
  case WASM_I32: {
    int32_t number = 0;
    if (napi_get_value_int32(env, value, &number) != napi_ok) {
      // ToInt32 coercion, as the JS API demands (wasm-bindgen glue returns
      // booleans from predicate imports typed i32).
      napi_value coerced = nullptr;
      if (napi_coerce_to_number(env, value, &coerced) != napi_ok ||
          napi_get_value_int32(env, coerced, &number) != napi_ok)
        return false;
    }
    out->of.i32 = number;
    return true;
  }
  case WASM_I64: {
    int64_t number = 0;
    bool lossless = false;
    if (napi_get_value_bigint_int64(env, value, &number, &lossless) !=
        napi_ok) {
      if (napi_get_value_int64(env, value, &number) != napi_ok)
        return false;
    }
    out->of.i64 = number;
    return true;
  }
  case WASM_F32:
  case WASM_F64: {
    double number = 0;
    if (napi_get_value_double(env, value, &number) != napi_ok) {
      napi_value coerced = nullptr;
      if (napi_coerce_to_number(env, value, &coerced) != napi_ok ||
          napi_get_value_double(env, coerced, &number) != napi_ok)
        return false;
    }
    if (kind == WASM_F32) {
      out->of.f32 = static_cast<float>(number);
    } else {
      out->of.f64 = number;
    }
    return true;
  }
  case WASM_EXTERNREF:
  case WASM_FUNCREF: {
    wasm_ref_t *ref = nullptr;
    if (!JsToRef(state, value, kind, &ref))
      return false;
    // The wasm_val_t owns the boxed reference; wasm_val_delete /
    // wasm_val_vec_delete frees it.
    out->of.ref = ref;
    return true;
  }
  default:
    return false;
  }
}

napi_value WasmValToJs(WasmState *state, napi_env env,
                       const wasm_val_t *value) {
  if (value == nullptr)
    return Undefined(env);
  napi_value out = nullptr;
  switch (value->kind) {
  case WASM_I32:
    napi_create_int32(env, value->of.i32, &out);
    break;
  case WASM_I64:
    napi_create_bigint_int64(env, value->of.i64, &out);
    break;
  case WASM_F32:
    napi_create_double(env, static_cast<double>(value->of.f32), &out);
    break;
  case WASM_F64:
    napi_create_double(env, value->of.f64, &out);
    break;
  case WASM_EXTERNREF:
  case WASM_FUNCREF:
    out = RefToJs(state, value->of.ref);
    break;
  default:
    out = Undefined(env);
    break;
  }
  return out == nullptr ? Undefined(env) : out;
}

wasm_trap_t *MakeTrap(WasmState *state, const char *message) {
  if (state == nullptr || state->store == nullptr)
    return nullptr;
  wasm_message_t wasm_message;
  wasm_name_new_from_string_nt(
      &wasm_message, message == nullptr ? "WebAssembly trap" : message);
  wasm_trap_t *trap = wasm_trap_new(state->store, &wasm_message);
  wasm_name_delete(&wasm_message);
  return trap;
}

std::string TrapMessage(wasm_trap_t *trap) {
  if (trap == nullptr)
    return "WebAssembly trap";
  wasm_message_t message;
  wasm_trap_message(trap, &message);
  std::string out = NameToString(&message);
  wasm_name_delete(&message);
  if (out.empty())
    return "WebAssembly trap";
  return out;
}

bool TakePendingImportException(WasmState *state, napi_value *out) {
  if (state == nullptr || out == nullptr ||
      state->pending_import_exception_ref == nullptr)
    return false;
  napi_value exception = nullptr;
  if (napi_get_reference_value(state->env, state->pending_import_exception_ref,
                               &exception) != napi_ok ||
      exception == nullptr) {
    DeleteRefIfPresent(state->env, &state->pending_import_exception_ref);
    return false;
  }
  DeleteRefIfPresent(state->env, &state->pending_import_exception_ref);
  *out = exception;
  return true;
}

wasm_trap_t *JsImportCallback(void *raw, const wasm_val_vec_t *args,
                              wasm_val_vec_t *results) {
  auto *data = static_cast<ImportFuncData *>(raw);
  if (data == nullptr || data->env == nullptr ||
      data->function_ref == nullptr) {
    return MakeTrap(data == nullptr ? nullptr : data->state,
                    "Invalid WebAssembly import callback");
  }

  napi_env env = data->env;
  // Wasm may have grown its memory since the last JS↔wasm crossing; stale
  // cached buffers must read as detached before glue code touches them.
  RefreshMemoryViews(data->state);
  napi_value function = nullptr;
  if (napi_get_reference_value(env, data->function_ref, &function) != napi_ok ||
      function == nullptr) {
    return MakeTrap(data->state, "WebAssembly import callback was collected");
  }

  std::vector<napi_value> js_args(args == nullptr ? 0 : args->size);
  for (size_t i = 0; i < js_args.size(); ++i) {
    js_args[i] = WasmValToJs(data->state, env, &args->data[i]);
  }

  napi_value global = nullptr;
  napi_get_global(env, &global);
  napi_value result = nullptr;
  napi_status status =
      napi_call_function(env, global, function, js_args.size(),
                         js_args.empty() ? nullptr : js_args.data(), &result);
  if (status != napi_ok) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value exception = nullptr;
      if (napi_get_and_clear_last_exception(env, &exception) == napi_ok &&
          exception != nullptr) {
        DeleteRefIfPresent(env, &data->state->pending_import_exception_ref);
        napi_create_reference(env, exception, 1,
                              &data->state->pending_import_exception_ref);
      }
    }
    return MakeTrap(data->state, "WebAssembly import function threw");
  }

  if (results != nullptr && results->size > 0) {
    if (data->result_kinds.size() < results->size) {
      return MakeTrap(
          data->state,
          "WebAssembly import function result type metadata is missing");
    }
    if (!JsToWasmVal(data->state, env, result, data->result_kinds[0],
                     &results->data[0])) {
      return MakeTrap(
          data->state,
          "WebAssembly import function returned an incompatible value");
    }
    for (size_t i = 1; i < results->size; ++i) {
      results->data[i].kind = data->result_kinds[i];
      results->data[i].of.ref = nullptr;
    }
  }
  return nullptr;
}

void ImportFuncDataFinalizer(void *raw) {
  auto *data = static_cast<ImportFuncData *>(raw);
  if (data == nullptr)
    return;
  DeleteRefIfPresent(data->env, &data->function_ref);
  delete data;
}

void ModuleFinalize(napi_env, void *data, void *) {
  auto *object = static_cast<WasmModuleObject *>(data);
  if (object == nullptr)
    return;
  if (object->module != nullptr)
    wasm_module_delete(object->module);
  delete object;
}

void InstanceFinalize(napi_env env, void *data, void *) {
  auto *object = static_cast<WasmInstanceObject *>(data);
  if (object == nullptr)
    return;
  DeleteRefIfPresent(env, &object->exports_ref);
  if (object->instance != nullptr)
    wasm_instance_delete(object->instance);
  delete object;
}

void MemoryFinalize(napi_env env, void *data, void *) {
  auto *object = static_cast<WasmMemoryObject *>(data);
  if (object == nullptr)
    return;
  if (object->base.state != nullptr) {
    auto &memories = object->base.state->live_memories;
    memories.erase(std::remove(memories.begin(), memories.end(), object),
                   memories.end());
  }
  DeleteRefIfPresent(env, &object->buffer_ref);
  if (object->memory != nullptr)
    wasm_memory_delete(object->memory);
  delete object;
}

void TableFinalize(napi_env, void *data, void *) {
  auto *object = static_cast<WasmTableObject *>(data);
  if (object == nullptr)
    return;
  if (object->table != nullptr)
    wasm_table_delete(object->table);
  delete object;
}

void GlobalFinalize(napi_env, void *data, void *) {
  auto *object = static_cast<WasmGlobalObject *>(data);
  if (object == nullptr)
    return;
  if (object->global != nullptr)
    wasm_global_delete(object->global);
  delete object;
}

void FunctionFinalize(napi_env, void *data, void *) {
  auto *object = static_cast<WasmFunctionObject *>(data);
  if (object == nullptr)
    return;
  if (object->func != nullptr)
    wasm_func_delete(object->func);
  delete object;
}

void ExternalArrayBufferNoopFinalize(node_api_basic_env, void *, void *) {}

napi_value CreateFunctionObject(WasmState *state, const std::string &name,
                                wasm_func_t *owned_func) {
  if (state == nullptr || owned_func == nullptr)
    return nullptr;
  napi_env env = state->env;
  auto *object = new WasmFunctionObject();
  object->base.kind = WasmObjectKind::kFunction;
  object->base.state = state;
  object->func = owned_func;

  napi_value fn = nullptr;
  if (napi_create_function(
          env, name.empty() ? nullptr : name.c_str(),
          name.empty() ? 0 : name.size(),
          [](napi_env env, napi_callback_info info) -> napi_value {
            size_t argc = 32;
            napi_value argv[32] = {};
            napi_value this_arg = nullptr;
            void *raw = nullptr;
            if (!GetCallback(env, info, &argc, argv, &this_arg, &raw))
              return nullptr;
            auto *function = static_cast<WasmFunctionObject *>(raw);
            if (function == nullptr || function->func == nullptr) {
              napi_throw_type_error(env, nullptr,
                                    "Invalid WebAssembly function");
              return nullptr;
            }

            wasm_functype_t *type = wasm_func_type(function->func);
            if (type == nullptr) {
              napi_throw_error(env, nullptr,
                               "Failed to read WebAssembly function type");
              return nullptr;
            }
            const wasm_valtype_vec_t *params = wasm_functype_params(type);
            const wasm_valtype_vec_t *result_types =
                wasm_functype_results(type);
            size_t param_count = params == nullptr ? 0 : params->size;
            size_t result_count =
                result_types == nullptr ? 0 : result_types->size;
            if (argc < param_count) {
              wasm_functype_delete(type);
              napi_throw_type_error(
                  env, nullptr, "Too few arguments for WebAssembly function");
              return nullptr;
            }

            wasm_val_vec_t wasm_args;
            wasm_val_vec_t wasm_results;
            wasm_val_vec_new_uninitialized(&wasm_args, param_count);
            wasm_val_vec_new_uninitialized(&wasm_results, result_count);
            // Zero-fill: wasm_val_delete on a ref-kind val frees of.ref, so
            // no slot may hold uninitialized garbage on an early-exit path.
            if (param_count > 0)
              std::memset(wasm_args.data, 0, param_count * sizeof(wasm_val_t));
            bool ok = true;
            for (size_t i = 0; i < param_count; ++i) {
              wasm_valkind_t kind = wasm_valtype_kind(params->data[i]);
              if (!JsToWasmVal(function->base.state, env, argv[i], kind,
                               &wasm_args.data[i])) {
                ok = false;
                break;
              }
            }
            for (size_t i = 0; i < result_count; ++i) {
              wasm_results.data[i].kind =
                  wasm_valtype_kind(result_types->data[i]);
              wasm_results.data[i].of.ref = nullptr;
            }
            wasm_functype_delete(type);
            if (!ok) {
              wasm_val_vec_delete(&wasm_args);
              wasm_val_vec_delete(&wasm_results);
              napi_throw_type_error(
                  env, nullptr, "Invalid argument for WebAssembly function");
              return nullptr;
            }

            wasm_trap_t *trap =
                wasm_func_call(function->func, &wasm_args, &wasm_results);
            wasm_val_vec_delete(&wasm_args);
            RefreshMemoryViews(function->base.state);
            if (trap != nullptr) {
              napi_value pending_exception = nullptr;
              if (TakePendingImportException(function->base.state,
                                             &pending_exception)) {
                wasm_trap_delete(trap);
                wasm_val_vec_delete(&wasm_results);
                napi_throw(env, pending_exception);
                return nullptr;
              }
              std::string message = TrapMessage(trap);
              wasm_trap_delete(trap);
              wasm_val_vec_delete(&wasm_results);
              ThrowWasmError(env, "RuntimeError", message);
              return nullptr;
            }

            napi_value out = nullptr;
            if (result_count == 0) {
              out = Undefined(env);
            } else if (result_count == 1) {
              out = WasmValToJs(function->base.state, env,
                                &wasm_results.data[0]);
            } else {
              // Multi-value results surface as a JS array, as in the JS API
              // (wasm-bindgen's externref ABI relies on this).
              if (napi_create_array_with_length(env, result_count, &out) !=
                  napi_ok) {
                out = nullptr;
              } else {
                for (size_t i = 0; i < result_count; ++i) {
                  napi_set_element(env, out, static_cast<uint32_t>(i),
                                   WasmValToJs(function->base.state, env,
                                               &wasm_results.data[i]));
                }
              }
            }
            wasm_val_vec_delete(&wasm_results);
            return out;
          },
          object, &fn) != napi_ok ||
      fn == nullptr) {
    delete object;
    return nullptr;
  }
  if (napi_wrap(env, fn, object, FunctionFinalize, nullptr, nullptr) !=
      napi_ok) {
    delete object;
    return nullptr;
  }
  return fn;
}

napi_value CreateExternObject(WasmState *state, wasm_extern_t *ext,
                              const std::string &name) {
  if (state == nullptr || ext == nullptr)
    return nullptr;
  switch (wasm_extern_kind(ext)) {
  case WASM_EXTERN_FUNC: {
    wasm_func_t *func = wasm_extern_as_func(ext);
    return func == nullptr
               ? nullptr
               : CreateFunctionObject(state, name, wasm_func_copy(func));
  }
  case WASM_EXTERN_GLOBAL: {
    wasm_global_t *global = wasm_extern_as_global(ext);
    return global == nullptr
               ? nullptr
               : CreateObjectFromOwned(state, WasmObjectKind::kGlobal,
                                       wasm_global_copy(global));
  }
  case WASM_EXTERN_TABLE: {
    wasm_table_t *table = wasm_extern_as_table(ext);
    return table == nullptr
               ? nullptr
               : CreateObjectFromOwned(state, WasmObjectKind::kTable,
                                       wasm_table_copy(table));
  }
  case WASM_EXTERN_MEMORY: {
    wasm_memory_t *memory = wasm_extern_as_memory(ext);
    return memory == nullptr
               ? nullptr
               : CreateObjectFromOwned(state, WasmObjectKind::kMemory,
                                       wasm_memory_copy(memory));
  }
  default:
    return Undefined(state->env);
  }
}

napi_value ModuleConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  napi_value this_arg = nullptr;
  void *raw_state = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, &raw_state))
    return nullptr;
  auto *state = static_cast<WasmState *>(raw_state);
  if (state == nullptr) {
    napi_throw_error(env, nullptr, "WebAssembly state is not initialized");
    return nullptr;
  }

  void *internal_module = nullptr;
  if (argc >= 1 &&
      TryConsumeInternalCreate(env, argv[0], WasmObjectKind::kModule,
                               &internal_module)) {
    auto *object = new WasmModuleObject();
    object->base.kind = WasmObjectKind::kModule;
    object->base.state = state;
    object->module = static_cast<wasm_module_t *>(internal_module);
    if (napi_wrap(env, this_arg, object, ModuleFinalize, nullptr, nullptr) !=
        napi_ok) {
      ModuleFinalize(env, object, nullptr);
      return nullptr;
    }
    return this_arg;
  }

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok ||
      new_target == nullptr) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Module must be called with new");
    return nullptr;
  }
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Module requires a buffer source");
    return nullptr;
  }

  const wasm_byte_t *data = nullptr;
  size_t length = 0;
  if (!ReadBufferSource(env, argv[0], &data, &length)) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Module expects a BufferSource");
    return nullptr;
  }
  wasm_byte_vec_t bytes = BorrowedByteVec(data, length);
  wasm_module_t *module = wasm_module_new(state->store, &bytes);
  if (module == nullptr) {
    ThrowWasmError(env, "CompileError",
                   "WebAssembly.Module compilation failed");
    return nullptr;
  }

  auto *object = new WasmModuleObject();
  object->base.kind = WasmObjectKind::kModule;
  object->base.state = state;
  object->module = module;
  if (napi_wrap(env, this_arg, object, ModuleFinalize, nullptr, nullptr) !=
      napi_ok) {
    ModuleFinalize(env, object, nullptr);
    napi_throw_error(env, nullptr, "Failed to wrap WebAssembly.Module");
    return nullptr;
  }
  return this_arg;
}

bool BuildImportExtern(WasmState *state, napi_value import_object,
                       const wasm_importtype_t *import_type,
                       std::vector<wasm_func_t *> *owned_funcs,
                       wasm_extern_t **out) {
  if (state == nullptr || import_type == nullptr || owned_funcs == nullptr ||
      out == nullptr)
    return false;
  *out = nullptr;
  napi_env env = state->env;
  const std::string module_name =
      NameToString(wasm_importtype_module(import_type));
  const std::string import_name =
      NameToString(wasm_importtype_name(import_type));

  napi_value module_object = nullptr;
  if (import_object == nullptr ||
      !GetNamed(env, import_object, module_name.c_str(), &module_object) ||
      IsUndefined(env, module_object)) {
    ThrowWasmError(env, "LinkError",
                   "Missing WebAssembly import: " + module_name + "." +
                       import_name);
    return false;
  }
  napi_value value = nullptr;
  if (!GetNamed(env, module_object, import_name.c_str(), &value) ||
      IsUndefined(env, value)) {
    ThrowWasmError(env, "LinkError",
                   "Missing WebAssembly import: " + module_name + "." +
                       import_name);
    return false;
  }

  const wasm_externtype_t *ext_type = wasm_importtype_type(import_type);
  switch (wasm_externtype_kind(ext_type)) {
  case WASM_EXTERN_FUNC: {
    if (auto *wrapped =
            Unwrap<WasmFunctionObject>(env, value, WasmObjectKind::kFunction);
        wrapped != nullptr) {
      wasm_func_t *func = wasm_func_copy(wrapped->func);
      owned_funcs->push_back(func);
      *out = wasm_func_as_extern(func);
      return *out != nullptr;
    }

    napi_valuetype value_type = napi_undefined;
    if (napi_typeof(env, value, &value_type) != napi_ok ||
        value_type != napi_function) {
      ThrowWasmError(env, "LinkError",
                     "WebAssembly function import must be callable: " +
                         module_name + "." + import_name);
      return false;
    }

    const wasm_functype_t *expected_type =
        wasm_externtype_as_functype_const(ext_type);
    wasm_functype_t *copied_type =
        wasm_functype_copy(const_cast<wasm_functype_t *>(expected_type));
    auto *callback_data = new ImportFuncData();
    callback_data->state = state;
    callback_data->env = env;
    const wasm_valtype_vec_t *result_types =
        wasm_functype_results(expected_type);
    if (result_types != nullptr) {
      callback_data->result_kinds.reserve(result_types->size);
      for (size_t i = 0; i < result_types->size; ++i) {
        callback_data->result_kinds.push_back(
            wasm_valtype_kind(result_types->data[i]));
      }
    }
    if (napi_create_reference(env, value, 1, &callback_data->function_ref) !=
        napi_ok) {
      delete callback_data;
      wasm_functype_delete(copied_type);
      napi_throw_error(env, nullptr,
                       "Failed to retain WebAssembly import function");
      return false;
    }
    wasm_func_t *func =
        wasm_func_new_with_env(state->store, copied_type, JsImportCallback,
                               callback_data, ImportFuncDataFinalizer);
    wasm_functype_delete(copied_type);
    if (func == nullptr) {
      ImportFuncDataFinalizer(callback_data);
      ThrowWasmError(env, "LinkError",
                     "Failed to create WebAssembly import function");
      return false;
    }
    owned_funcs->push_back(func);
    *out = wasm_func_as_extern(func);
    return *out != nullptr;
  }
  case WASM_EXTERN_GLOBAL: {
    auto *global =
        Unwrap<WasmGlobalObject>(env, value, WasmObjectKind::kGlobal);
    if (global == nullptr || global->global == nullptr) {
      ThrowWasmError(env, "LinkError",
                     "WebAssembly global import has incompatible value: " +
                         module_name + "." + import_name);
      return false;
    }
    *out = wasm_global_as_extern(global->global);
    return *out != nullptr;
  }
  case WASM_EXTERN_TABLE: {
    auto *table = Unwrap<WasmTableObject>(env, value, WasmObjectKind::kTable);
    if (table == nullptr || table->table == nullptr) {
      ThrowWasmError(env, "LinkError",
                     "WebAssembly table import has incompatible value: " +
                         module_name + "." + import_name);
      return false;
    }
    *out = wasm_table_as_extern(table->table);
    return *out != nullptr;
  }
  case WASM_EXTERN_MEMORY: {
    auto *memory =
        Unwrap<WasmMemoryObject>(env, value, WasmObjectKind::kMemory);
    if (memory == nullptr || memory->memory == nullptr) {
      ThrowWasmError(env, "LinkError",
                     "WebAssembly memory import has incompatible value: " +
                         module_name + "." + import_name);
      return false;
    }
    *out = wasm_memory_as_extern(memory->memory);
    return *out != nullptr;
  }
  default:
    ThrowWasmError(env, "LinkError", "Unsupported WebAssembly import type");
    return false;
  }
}

napi_value InstanceConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  napi_value this_arg = nullptr;
  void *raw_state = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, &raw_state))
    return nullptr;
  auto *state = static_cast<WasmState *>(raw_state);
  if (state == nullptr) {
    napi_throw_error(env, nullptr, "WebAssembly state is not initialized");
    return nullptr;
  }

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok ||
      new_target == nullptr) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Instance must be called with new");
    return nullptr;
  }
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Instance requires a WebAssembly.Module");
    return nullptr;
  }
  auto *module =
      Unwrap<WasmModuleObject>(env, argv[0], WasmObjectKind::kModule);
  if (module == nullptr || module->module == nullptr) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Instance expects a WebAssembly.Module");
    return nullptr;
  }

  wasm_importtype_vec_t import_types;
  wasm_module_imports(module->module, &import_types);
  std::vector<wasm_extern_t *> import_externs(import_types.size);
  std::vector<wasm_func_t *> owned_import_funcs;
  bool ok = true;
  for (size_t i = 0; i < import_types.size; ++i) {
    if (!BuildImportExtern(state, argc >= 2 ? argv[1] : nullptr,
                           import_types.data[i], &owned_import_funcs,
                           &import_externs[i])) {
      ok = false;
      break;
    }
  }
  if (!ok) {
    for (wasm_func_t *func : owned_import_funcs)
      wasm_func_delete(func);
    wasm_importtype_vec_delete(&import_types);
    return nullptr;
  }

  wasm_extern_vec_t imports;
  imports.size = import_externs.size();
  imports.data = import_externs.empty() ? nullptr : import_externs.data();
  wasm_trap_t *trap = nullptr;
  wasm_instance_t *instance =
      wasm_instance_new(state->store, module->module, &imports, &trap);
  for (wasm_func_t *func : owned_import_funcs)
    wasm_func_delete(func);
  wasm_importtype_vec_delete(&import_types);

  if (trap != nullptr) {
    napi_value pending_exception = nullptr;
    if (TakePendingImportException(state, &pending_exception)) {
      wasm_trap_delete(trap);
      napi_throw(env, pending_exception);
      return nullptr;
    }
    std::string message = TrapMessage(trap);
    wasm_trap_delete(trap);
    ThrowWasmError(env, "RuntimeError", message);
    return nullptr;
  }
  if (instance == nullptr) {
    ThrowWasmError(env, "LinkError",
                   "WebAssembly.Instance instantiation failed");
    return nullptr;
  }

  wasm_extern_vec_t exports;
  wasm_exporttype_vec_t export_types;
  wasm_instance_exports(instance, &exports);
  wasm_module_exports(module->module, &export_types);

  napi_value exports_object = nullptr;
  napi_create_object(env, &exports_object);
  const size_t export_count = std::min(exports.size, export_types.size);
  for (size_t i = 0; i < export_count; ++i) {
    std::string name = NameToString(wasm_exporttype_name(export_types.data[i]));
    napi_value js_export = CreateExternObject(state, exports.data[i], name);
    if (js_export != nullptr) {
      SetNamed(env, exports_object, name.c_str(), js_export);
    }
  }
  wasm_extern_vec_delete(&exports);
  wasm_exporttype_vec_delete(&export_types);

  auto *object = new WasmInstanceObject();
  object->base.kind = WasmObjectKind::kInstance;
  object->base.state = state;
  object->instance = instance;
  napi_create_reference(env, exports_object, 1, &object->exports_ref);
  if (napi_wrap(env, this_arg, object, InstanceFinalize, nullptr, nullptr) !=
      napi_ok) {
    InstanceFinalize(env, object, nullptr);
    napi_throw_error(env, nullptr, "Failed to wrap WebAssembly.Instance");
    return nullptr;
  }
  return this_arg;
}

napi_value InstanceExportsGetter(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, nullptr, &this_arg, nullptr))
    return nullptr;
  auto *object =
      Unwrap<WasmInstanceObject>(env, this_arg, WasmObjectKind::kInstance);
  if (object == nullptr || object->exports_ref == nullptr)
    return Undefined(env);
  napi_value exports = nullptr;
  if (napi_get_reference_value(env, object->exports_ref, &exports) != napi_ok ||
      exports == nullptr) {
    return Undefined(env);
  }
  return exports;
}

bool BuildLimitsFromDescriptor(napi_env env, napi_value descriptor,
                               wasm_limits_t *limits) {
  if (limits == nullptr)
    return false;
  uint32_t initial = 0;
  uint32_t maximum = wasm_limits_max_default;
  if (!GetRequiredUint32(env, descriptor, "initial", &initial) ||
      !GetOptionalUint32(env, descriptor, "maximum", wasm_limits_max_default,
                         &maximum)) {
    return false;
  }
  if (maximum != wasm_limits_max_default && initial > maximum)
    return false;
  limits->min = initial;
  limits->max = maximum;
  return true;
}

napi_value MemoryConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  napi_value this_arg = nullptr;
  void *raw_state = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, &raw_state))
    return nullptr;
  auto *state = static_cast<WasmState *>(raw_state);
  if (state == nullptr)
    return nullptr;

  void *internal_memory = nullptr;
  if (argc >= 1 &&
      TryConsumeInternalCreate(env, argv[0], WasmObjectKind::kMemory,
                               &internal_memory)) {
    auto *object = new WasmMemoryObject();
    object->base.kind = WasmObjectKind::kMemory;
    object->base.state = state;
    object->memory = static_cast<wasm_memory_t *>(internal_memory);
    object->shared = IsSharedMemory(object->memory);
    if (napi_wrap(env, this_arg, object, MemoryFinalize, nullptr, nullptr) !=
        napi_ok) {
      MemoryFinalize(env, object, nullptr);
      return nullptr;
    }
    state->live_memories.push_back(object);
    return this_arg;
  }

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok ||
      new_target == nullptr) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Memory must be called with new");
    return nullptr;
  }
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Memory requires a descriptor");
    return nullptr;
  }
  bool shared = false;
  if (!GetOptionalBool(env, argv[0], "shared", false, &shared)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Memory shared value");
    return nullptr;
  }

  wasm_limits_t limits;
  if (!BuildLimitsFromDescriptor(env, argv[0], &limits)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Memory descriptor");
    return nullptr;
  }
  if (shared && limits.max == wasm_limits_max_default) {
    napi_throw_type_error(
        env, nullptr,
        "Shared WebAssembly.Memory requires a maximum page count");
    return nullptr;
  }
  wasm_memorytype_t *type = shared ? wasm_shared_memorytype_new(&limits)
                                   : wasm_memorytype_new(&limits);
  if (type == nullptr) {
    napi_throw_range_error(env, nullptr, "Invalid WebAssembly.Memory limits");
    return nullptr;
  }
  wasm_memory_t *memory = wasm_memory_new(state->store, type);
  wasm_memorytype_delete(type);
  if (memory == nullptr) {
    napi_throw_range_error(env, nullptr, "Failed to create WebAssembly.Memory");
    return nullptr;
  }

  auto *object = new WasmMemoryObject();
  object->base.kind = WasmObjectKind::kMemory;
  object->base.state = state;
  object->memory = memory;
  object->shared = shared;
  if (napi_wrap(env, this_arg, object, MemoryFinalize, nullptr, nullptr) !=
      napi_ok) {
    MemoryFinalize(env, object, nullptr);
    return nullptr;
  }
  state->live_memories.push_back(object);
  return this_arg;
}

napi_value MemoryBufferGetter(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, nullptr, &this_arg, nullptr))
    return nullptr;
  auto *object =
      Unwrap<WasmMemoryObject>(env, this_arg, WasmObjectKind::kMemory);
  if (object == nullptr || object->memory == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Memory");
    return nullptr;
  }
  RefreshMemoryView(object);
  if (object->buffer_ref != nullptr) {
    napi_value cached = nullptr;
    if (napi_get_reference_value(env, object->buffer_ref, &cached) ==
            napi_ok &&
        cached != nullptr)
      return cached;
  }
  void *data = wasm_memory_data(object->memory);
  size_t size = object->shared
                    ? static_cast<size_t>(wasm_memory_size(object->memory)) *
                          65'536
                    : wasm_memory_data_size(object->memory);
  if (data == nullptr && size != 0) {
    if (!object->shared) {
      napi_throw_error(env, nullptr,
                       "Failed to access WebAssembly.Memory storage");
      return nullptr;
    }

    wasm_memory_t *retained = wasm_memory_copy(object->memory);
    auto *backing = retained == nullptr
                        ? nullptr
                        : new (std::nothrow) HostMemoryBacking{retained};
    if (backing == nullptr) {
      if (retained != nullptr)
        wasm_memory_delete(retained);
      napi_throw_error(env, nullptr,
                       "Failed to retain host WebAssembly.Memory storage");
      return nullptr;
    }
    napi_value host_buffer = nullptr;
    const napi_status status = unofficial_napi_create_host_sharedarraybuffer(
        env, backing, size, HostMemoryRead, HostMemoryWrite, HostMemoryAtomic,
        HostMemoryWait, HostMemoryNotify,
        HostMemoryBufferFinalize, backing, &host_buffer);
    if (status != napi_ok || host_buffer == nullptr)
      return nullptr;
    napi_create_reference(env, host_buffer, 1, &object->buffer_ref);
    object->buffer_data = backing;
    object->buffer_size = size;
    object->buffer_host_backed = true;
    return host_buffer;
  }
  napi_value array_buffer = nullptr;
  napi_status buffer_status = napi_generic_failure;
  if (object->shared) {
    wasm_shared_memory_t *shared = wasm_memory_share(object->memory);
    if (shared != nullptr) {
      buffer_status = unofficial_napi_create_external_sharedarraybuffer(
          env, data, size, SharedMemoryBufferFinalize, shared, &array_buffer);
    }
  } else {
    buffer_status = napi_create_external_arraybuffer(
        env, data, size, ExternalArrayBufferNoopFinalize, nullptr,
        &array_buffer);
  }
  if (buffer_status != napi_ok || array_buffer == nullptr) {
    napi_throw_error(env, nullptr,
                     "Failed to create WebAssembly.Memory buffer");
    return nullptr;
  }
  if (napi_create_reference(env, array_buffer, 1, &object->buffer_ref) ==
      napi_ok) {
    object->buffer_data = data;
    object->buffer_size = size;
    object->buffer_host_backed = false;
  } else {
    object->buffer_ref = nullptr;
  }
  return array_buffer;
}

napi_value MemoryGrow(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, nullptr))
    return nullptr;
  auto *object =
      Unwrap<WasmMemoryObject>(env, this_arg, WasmObjectKind::kMemory);
  if (object == nullptr || object->memory == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Memory");
    return nullptr;
  }
  uint32_t delta = 0;
  if (argc < 1 || napi_get_value_uint32(env, argv[0], &delta) != napi_ok) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Memory.grow expects a page count");
    return nullptr;
  }
  uint32_t previous = wasm_memory_size(object->memory);
  if (!wasm_memory_grow(object->memory, delta)) {
    napi_throw_range_error(env, nullptr, "WebAssembly.Memory.grow failed");
    return nullptr;
  }
  RefreshMemoryView(object);
  napi_value out = nullptr;
  napi_create_uint32(env, previous, &out);
  return out;
}

napi_value TableConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  napi_value this_arg = nullptr;
  void *raw_state = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, &raw_state))
    return nullptr;
  auto *state = static_cast<WasmState *>(raw_state);
  if (state == nullptr)
    return nullptr;

  void *internal_table = nullptr;
  if (argc >= 1 && TryConsumeInternalCreate(
                       env, argv[0], WasmObjectKind::kTable, &internal_table)) {
    auto *object = new WasmTableObject();
    object->base.kind = WasmObjectKind::kTable;
    object->base.state = state;
    object->table = static_cast<wasm_table_t *>(internal_table);
    if (napi_wrap(env, this_arg, object, TableFinalize, nullptr, nullptr) !=
        napi_ok) {
      TableFinalize(env, object, nullptr);
      return nullptr;
    }
    return this_arg;
  }

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok ||
      new_target == nullptr) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Table must be called with new");
    return nullptr;
  }
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Table requires a descriptor");
    return nullptr;
  }

  napi_value element_value = nullptr;
  wasm_valkind_t element_kind = WASM_FUNCREF;
  if (!GetNamed(env, argv[0], "element", &element_value) ||
      IsUndefined(env, element_value) ||
      !ParseValueKind(env, element_value, &element_kind) ||
      (element_kind != WASM_FUNCREF && element_kind != WASM_EXTERNREF)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Table element type");
    return nullptr;
  }
  wasm_limits_t limits;
  if (!BuildLimitsFromDescriptor(env, argv[0], &limits)) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Table descriptor");
    return nullptr;
  }

  wasm_ref_t *init_ref = nullptr;
  if (argc >= 2 && !IsNullOrUndefined(env, argv[1]) &&
      !JsToRef(state, argv[1], element_kind, &init_ref)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Table initial value");
    return nullptr;
  }

  wasm_tabletype_t *table_type =
      wasm_tabletype_new(wasm_valtype_new(element_kind), &limits);
  wasm_table_t *table =
      table_type == nullptr ? nullptr
                            : wasm_table_new(state->store, table_type, init_ref);
  if (table_type != nullptr)
    wasm_tabletype_delete(table_type);
  if (init_ref != nullptr)
    wasm_ref_delete(init_ref);
  if (table == nullptr) {
    napi_throw_error(env, nullptr, "Failed to create WebAssembly.Table");
    return nullptr;
  }

  auto *object = new WasmTableObject();
  object->base.kind = WasmObjectKind::kTable;
  object->base.state = state;
  object->table = table;
  if (napi_wrap(env, this_arg, object, TableFinalize, nullptr, nullptr) !=
      napi_ok) {
    TableFinalize(env, object, nullptr);
    return nullptr;
  }
  return this_arg;
}

napi_value TableLengthGetter(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, nullptr, &this_arg, nullptr))
    return nullptr;
  auto *object = Unwrap<WasmTableObject>(env, this_arg, WasmObjectKind::kTable);
  if (object == nullptr || object->table == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Table");
    return nullptr;
  }
  napi_value out = nullptr;
  napi_create_uint32(env, wasm_table_size(object->table), &out);
  return out;
}

napi_value TableGet(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, nullptr))
    return nullptr;
  auto *object = Unwrap<WasmTableObject>(env, this_arg, WasmObjectKind::kTable);
  if (object == nullptr || object->table == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Table");
    return nullptr;
  }
  uint32_t index = 0;
  if (argc < 1 || napi_get_value_uint32(env, argv[0], &index) != napi_ok) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Table.get expects an index");
    return nullptr;
  }
  if (index >= wasm_table_size(object->table)) {
    napi_throw_range_error(env, nullptr,
                           "WebAssembly.Table.get index is out of range");
    return nullptr;
  }
  wasm_ref_t *ref = wasm_table_get(object->table, index);
  napi_value out = RefToJs(object->base.state, ref);
  if (ref != nullptr)
    wasm_ref_delete(ref);
  return out;
}

napi_value TableSet(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, nullptr))
    return nullptr;
  auto *object = Unwrap<WasmTableObject>(env, this_arg, WasmObjectKind::kTable);
  if (object == nullptr || object->table == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Table");
    return nullptr;
  }
  uint32_t index = 0;
  if (argc < 1 || napi_get_value_uint32(env, argv[0], &index) != napi_ok) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Table.set expects an index");
    return nullptr;
  }
  if (index >= wasm_table_size(object->table)) {
    napi_throw_range_error(env, nullptr,
                           "WebAssembly.Table.set index is out of range");
    return nullptr;
  }
  wasm_valkind_t element_kind = WASM_FUNCREF;
  if (!TableElementKind(object->table, &element_kind)) {
    napi_throw_error(env, nullptr, "Failed to read WebAssembly.Table type");
    return nullptr;
  }
  wasm_ref_t *ref = nullptr;
  if (argc >= 2 &&
      !JsToRef(object->base.state, argv[1], element_kind, &ref)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid value for WebAssembly.Table.set");
    return nullptr;
  }
  bool ok = wasm_table_set(object->table, index, ref);
  if (ref != nullptr)
    wasm_ref_delete(ref);
  if (!ok) {
    napi_throw_range_error(env, nullptr, "WebAssembly.Table.set failed");
    return nullptr;
  }
  return Undefined(env);
}

napi_value TableGrow(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, nullptr))
    return nullptr;
  auto *object = Unwrap<WasmTableObject>(env, this_arg, WasmObjectKind::kTable);
  if (object == nullptr || object->table == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Table");
    return nullptr;
  }
  uint32_t delta = 0;
  if (argc < 1 || napi_get_value_uint32(env, argv[0], &delta) != napi_ok) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Table.grow expects a count");
    return nullptr;
  }
  wasm_valkind_t element_kind = WASM_FUNCREF;
  if (!TableElementKind(object->table, &element_kind)) {
    napi_throw_error(env, nullptr, "Failed to read WebAssembly.Table type");
    return nullptr;
  }
  wasm_ref_t *init_ref = nullptr;
  if (argc >= 2 &&
      !JsToRef(object->base.state, argv[1], element_kind, &init_ref)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid initial value for WebAssembly.Table.grow");
    return nullptr;
  }
  uint32_t previous = wasm_table_size(object->table);
  bool ok = wasm_table_grow(object->table, delta, init_ref);
  if (init_ref != nullptr)
    wasm_ref_delete(init_ref);
  if (!ok) {
    napi_throw_range_error(env, nullptr, "WebAssembly.Table.grow failed");
    return nullptr;
  }
  napi_value out = nullptr;
  napi_create_uint32(env, previous, &out);
  return out;
}

napi_value GlobalConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  napi_value this_arg = nullptr;
  void *raw_state = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, &raw_state))
    return nullptr;
  auto *state = static_cast<WasmState *>(raw_state);
  if (state == nullptr)
    return nullptr;

  void *internal_global = nullptr;
  if (argc >= 1 &&
      TryConsumeInternalCreate(env, argv[0], WasmObjectKind::kGlobal,
                               &internal_global)) {
    auto *object = new WasmGlobalObject();
    object->base.kind = WasmObjectKind::kGlobal;
    object->base.state = state;
    object->global = static_cast<wasm_global_t *>(internal_global);
    if (napi_wrap(env, this_arg, object, GlobalFinalize, nullptr, nullptr) !=
        napi_ok) {
      GlobalFinalize(env, object, nullptr);
      return nullptr;
    }
    return this_arg;
  }

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok ||
      new_target == nullptr) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Global must be called with new");
    return nullptr;
  }
  if (argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.Global requires a descriptor");
    return nullptr;
  }
  napi_value value_kind_name = nullptr;
  wasm_valkind_t value_kind = WASM_I32;
  if (!GetNamed(env, argv[0], "value", &value_kind_name) ||
      IsUndefined(env, value_kind_name) ||
      !ParseValueKind(env, value_kind_name, &value_kind)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Global value type");
    return nullptr;
  }
  bool is_mutable = false;
  if (!GetOptionalBool(env, argv[0], "mutable", false, &is_mutable)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Global mutability");
    return nullptr;
  }

  napi_value initial = argc >= 2 ? argv[1] : nullptr;
  napi_value zero = nullptr;
  if (IsUndefined(env, initial)) {
    napi_create_int32(env, 0, &zero);
    initial = zero;
  }
  wasm_val_t initial_value;
  if (!JsToWasmVal(state, env, initial, value_kind, &initial_value)) {
    napi_throw_type_error(env, nullptr,
                          "Invalid WebAssembly.Global initial value");
    return nullptr;
  }
  wasm_valtype_t *content = wasm_valtype_new(value_kind);
  wasm_globaltype_t *type =
      wasm_globaltype_new(content, is_mutable ? WASM_VAR : WASM_CONST);
  wasm_global_t *global = wasm_global_new(state->store, type, &initial_value);
  wasm_globaltype_delete(type);
  wasm_val_delete(&initial_value);
  if (global == nullptr) {
    napi_throw_error(env, nullptr, "Failed to create WebAssembly.Global");
    return nullptr;
  }

  auto *object = new WasmGlobalObject();
  object->base.kind = WasmObjectKind::kGlobal;
  object->base.state = state;
  object->global = global;
  if (napi_wrap(env, this_arg, object, GlobalFinalize, nullptr, nullptr) !=
      napi_ok) {
    GlobalFinalize(env, object, nullptr);
    return nullptr;
  }
  return this_arg;
}

napi_value GlobalValueGetter(napi_env env, napi_callback_info info) {
  size_t argc = 0;
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, nullptr, &this_arg, nullptr))
    return nullptr;
  auto *object =
      Unwrap<WasmGlobalObject>(env, this_arg, WasmObjectKind::kGlobal);
  if (object == nullptr || object->global == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Global");
    return nullptr;
  }
  wasm_val_t value;
  wasm_global_get(object->global, &value);
  napi_value out = WasmValToJs(object->base.state, env, &value);
  wasm_val_delete(&value);
  return out;
}

napi_value GlobalValueSetter(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  napi_value this_arg = nullptr;
  if (!GetCallback(env, info, &argc, argv, &this_arg, nullptr))
    return nullptr;
  auto *object =
      Unwrap<WasmGlobalObject>(env, this_arg, WasmObjectKind::kGlobal);
  if (object == nullptr || object->global == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Global");
    return nullptr;
  }
  wasm_globaltype_t *type = wasm_global_type(object->global);
  if (type == nullptr)
    return nullptr;
  if (wasm_globaltype_mutability(type) != WASM_VAR) {
    wasm_globaltype_delete(type);
    napi_throw_type_error(env, nullptr, "WebAssembly.Global is immutable");
    return nullptr;
  }
  wasm_valkind_t kind = wasm_valtype_kind(wasm_globaltype_content(type));
  wasm_val_t value;
  bool ok = argc >= 1 &&
            JsToWasmVal(object->base.state, env, argv[0], kind, &value);
  wasm_globaltype_delete(type);
  if (!ok) {
    napi_throw_type_error(env, nullptr, "Invalid WebAssembly.Global value");
    return nullptr;
  }
  wasm_global_set(object->global, &value);
  wasm_val_delete(&value);
  return Undefined(env);
}

napi_value ValidateCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  void *raw_state = nullptr;
  if (!GetCallback(env, info, &argc, argv, nullptr, &raw_state))
    return nullptr;
  auto *state = static_cast<WasmState *>(raw_state);
  if (state == nullptr || argc < 1) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.validate expects a BufferSource");
    return nullptr;
  }
  const wasm_byte_t *data = nullptr;
  size_t length = 0;
  if (!ReadBufferSource(env, argv[0], &data, &length)) {
    napi_throw_type_error(env, nullptr,
                          "WebAssembly.validate expects a BufferSource");
    return nullptr;
  }
  wasm_byte_vec_t bytes = BorrowedByteVec(data, length);
  bool valid = wasm_module_validate(state->store, &bytes);
  napi_value out = nullptr;
  napi_get_boolean(env, valid, &out);
  return out;
}

napi_value ModuleImportsCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  if (!GetCallback(env, info, &argc, argv, nullptr, nullptr))
    return nullptr;
  auto *module = argc >= 1 ? Unwrap<WasmModuleObject>(env, argv[0],
                                                      WasmObjectKind::kModule)
                           : nullptr;
  if (module == nullptr || module->module == nullptr) {
    napi_throw_type_error(
        env, nullptr,
        "WebAssembly.Module.imports expects a WebAssembly.Module");
    return nullptr;
  }
  wasm_importtype_vec_t imports;
  wasm_module_imports(module->module, &imports);
  napi_value array = nullptr;
  napi_create_array_with_length(env, imports.size, &array);
  for (size_t i = 0; i < imports.size; ++i) {
    napi_value desc = nullptr;
    napi_create_object(env, &desc);
    SetNamedString(env, desc, "module",
                   NameToString(wasm_importtype_module(imports.data[i])));
    SetNamedString(env, desc, "name",
                   NameToString(wasm_importtype_name(imports.data[i])));
    SetNamedString(env, desc, "kind",
                   ExternKindName(wasm_externtype_kind(
                       wasm_importtype_type(imports.data[i]))));
    napi_set_element(env, array, static_cast<uint32_t>(i), desc);
  }
  wasm_importtype_vec_delete(&imports);
  return array;
}

napi_value ModuleExportsCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {};
  if (!GetCallback(env, info, &argc, argv, nullptr, nullptr))
    return nullptr;
  auto *module = argc >= 1 ? Unwrap<WasmModuleObject>(env, argv[0],
                                                      WasmObjectKind::kModule)
                           : nullptr;
  if (module == nullptr || module->module == nullptr) {
    napi_throw_type_error(
        env, nullptr,
        "WebAssembly.Module.exports expects a WebAssembly.Module");
    return nullptr;
  }
  wasm_exporttype_vec_t exports;
  wasm_module_exports(module->module, &exports);
  napi_value array = nullptr;
  napi_create_array_with_length(env, exports.size, &array);
  for (size_t i = 0; i < exports.size; ++i) {
    napi_value desc = nullptr;
    napi_create_object(env, &desc);
    SetNamedString(env, desc, "name",
                   NameToString(wasm_exporttype_name(exports.data[i])));
    SetNamedString(env, desc, "kind",
                   ExternKindName(wasm_externtype_kind(
                       wasm_exporttype_type(exports.data[i]))));
    napi_set_element(env, array, static_cast<uint32_t>(i), desc);
  }
  wasm_exporttype_vec_delete(&exports);
  return array;
}

napi_value ModuleCustomSectionsCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {};
  if (!GetCallback(env, info, &argc, argv, nullptr, nullptr))
    return nullptr;
  auto *module = argc >= 1 ? Unwrap<WasmModuleObject>(env, argv[0],
                                                      WasmObjectKind::kModule)
                           : nullptr;
  if (module == nullptr || module->module == nullptr) {
    napi_throw_type_error(
        env, nullptr,
        "WebAssembly.Module.customSections expects a WebAssembly.Module");
    return nullptr;
  }
  napi_value array = nullptr;
  napi_create_array_with_length(env, 0, &array);
  return array;
}

napi_status DefineValue(
    napi_env env, napi_value object, const char *name, napi_value value,
    napi_property_attributes attributes = static_cast<napi_property_attributes>(
        napi_writable | napi_configurable)) {
  napi_property_descriptor desc = {};
  desc.utf8name = name;
  desc.value = value;
  desc.attributes = attributes;
  return napi_define_properties(env, object, 1, &desc);
}

bool InstallJsWrappers(napi_env env, std::string *error_out) {
  static const char *kScript = R"JS(
(function(WA) {
  'use strict';
  function makeError(name) {
    function WasmError(message) {
      var err = new Error(message === undefined ? '' : String(message));
      if (new.target) Object.setPrototypeOf(err, new.target.prototype);
      Object.defineProperty(err, 'name', { value: name, configurable: true });
      return err;
    }
    Object.setPrototypeOf(WasmError, Error);
    WasmError.prototype = Object.create(Error.prototype, {
      constructor: { value: WasmError, writable: true, configurable: true },
      name: { value: name, configurable: true }
    });
    return WasmError;
  }
  if (typeof WA.CompileError !== 'function') WA.CompileError = makeError('CompileError');
  if (typeof WA.LinkError !== 'function') WA.LinkError = makeError('LinkError');
  if (typeof WA.RuntimeError !== 'function') WA.RuntimeError = makeError('RuntimeError');

  var NativeModule = WA.Module;
  var NativeInstance = WA.Instance;
  WA.validate = WA.__edgeValidate;
  NativeModule.imports = WA.__edgeModuleImports;
  NativeModule.exports = WA.__edgeModuleExports;
  NativeModule.customSections = WA.__edgeModuleCustomSections;

  WA.compile = function compile(bytes) {
    return Promise.resolve().then(function() {
      return new NativeModule(bytes);
    });
  };

  WA.instantiate = function instantiate(bytesOrModule, imports) {
    return Promise.resolve().then(function() {
      if (bytesOrModule instanceof NativeModule) {
        return new NativeInstance(bytesOrModule, imports);
      }
      var module = new NativeModule(bytesOrModule);
      var instance = new NativeInstance(module, imports);
      return { module: module, instance: instance };
    });
  };

  WA.compileStreaming = function compileStreaming(source) {
    return Promise.resolve(source).then(function(response) {
      if (response == null || typeof response.arrayBuffer !== 'function') {
        throw new TypeError('WebAssembly.compileStreaming expects a Response or Response-like object');
      }
      return response.arrayBuffer();
    }).then(WA.compile);
  };

  WA.instantiateStreaming = function instantiateStreaming(source, imports) {
    return Promise.resolve(source).then(function(response) {
      if (response == null || typeof response.arrayBuffer !== 'function') {
        throw new TypeError('WebAssembly.instantiateStreaming expects a Response or Response-like object');
      }
      return response.arrayBuffer();
    }).then(function(bytes) {
      return WA.instantiate(bytes, imports);
    });
  };

  delete WA.__edgeValidate;
  delete WA.__edgeModuleImports;
  delete WA.__edgeModuleExports;
  delete WA.__edgeModuleCustomSections;
  if (typeof Symbol === 'function' && Symbol.toStringTag) {
    Object.defineProperty(WA, Symbol.toStringTag, { value: 'WebAssembly', configurable: true });
  }
})(globalThis.WebAssembly);
)JS";
  napi_value source = MakeString(env, kScript);
  napi_value result = nullptr;
  if (napi_run_script(env, source, &result) == napi_ok)
    return true;
  if (error_out != nullptr) {
    *error_out = "Failed to install QuickJS WebAssembly JS wrappers";
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value exception = nullptr;
      if (napi_get_and_clear_last_exception(env, &exception) == napi_ok &&
          exception != nullptr) {
        napi_value text = nullptr;
        if (napi_coerce_to_string(env, exception, &text) == napi_ok &&
            text != nullptr) {
          *error_out += ": ";
          *error_out += GetString(env, text);
        }
      }
    }
  }
  return false;
}

bool StoreConstructorRef(napi_env env, napi_value ctor, napi_ref *ref) {
  DeleteRefIfPresent(env, ref);
  return napi_create_reference(env, ctor, 1, ref) == napi_ok;
}

} // namespace

bool EdgeInstallQuickJsWebAssembly(napi_env env, std::string *error_out) {
  if (env == nullptr) {
    if (error_out != nullptr)
      *error_out = "Invalid environment";
    return false;
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    if (error_out != nullptr)
      *error_out = "Failed to fetch global object";
    return false;
  }
  napi_value existing = nullptr;
  if (GetNamed(env, global, "WebAssembly", &existing) &&
      !IsUndefined(env, existing)) {
    return true;
  }

  WasmState *state = EnsureState(env, error_out);
  if (state == nullptr)
    return false;

  napi_value webassembly = nullptr;
  if (napi_create_object(env, &webassembly) != napi_ok ||
      webassembly == nullptr) {
    if (error_out != nullptr)
      *error_out = "Failed to create WebAssembly object";
    return false;
  }

  napi_property_descriptor memory_props[] = {
      {"grow", nullptr, MemoryGrow, nullptr, nullptr, nullptr,
       napi_default_method, nullptr},
      {"buffer", nullptr, nullptr, MemoryBufferGetter, nullptr, nullptr,
       napi_default, nullptr},
  };
  napi_property_descriptor table_props[] = {
      {"get", nullptr, TableGet, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"set", nullptr, TableSet, nullptr, nullptr, nullptr, napi_default_method,
       nullptr},
      {"grow", nullptr, TableGrow, nullptr, nullptr, nullptr,
       napi_default_method, nullptr},
      {"length", nullptr, nullptr, TableLengthGetter, nullptr, nullptr,
       napi_default, nullptr},
  };
  napi_property_descriptor global_props[] = {
      {"valueOf", nullptr, GlobalValueGetter, nullptr, nullptr, nullptr,
       napi_default_method, nullptr},
      {"value", nullptr, nullptr, GlobalValueGetter, GlobalValueSetter, nullptr,
       napi_default, nullptr},
  };
  napi_property_descriptor instance_props[] = {
      {"exports", nullptr, nullptr, InstanceExportsGetter, nullptr, nullptr,
       napi_default, nullptr},
  };

  napi_value module_ctor = nullptr;
  napi_value instance_ctor = nullptr;
  napi_value memory_ctor = nullptr;
  napi_value table_ctor = nullptr;
  napi_value global_ctor = nullptr;
  if (napi_define_class(env, "Module", NAPI_AUTO_LENGTH, ModuleConstructor,
                        state, 0, nullptr, &module_ctor) != napi_ok ||
      napi_define_class(env, "Instance", NAPI_AUTO_LENGTH, InstanceConstructor,
                        state,
                        sizeof(instance_props) / sizeof(instance_props[0]),
                        instance_props, &instance_ctor) != napi_ok ||
      napi_define_class(env, "Memory", NAPI_AUTO_LENGTH, MemoryConstructor,
                        state, sizeof(memory_props) / sizeof(memory_props[0]),
                        memory_props, &memory_ctor) != napi_ok ||
      napi_define_class(env, "Table", NAPI_AUTO_LENGTH, TableConstructor, state,
                        sizeof(table_props) / sizeof(table_props[0]),
                        table_props, &table_ctor) != napi_ok ||
      napi_define_class(env, "Global", NAPI_AUTO_LENGTH, GlobalConstructor,
                        state, sizeof(global_props) / sizeof(global_props[0]),
                        global_props, &global_ctor) != napi_ok) {
    if (error_out != nullptr)
      *error_out = "Failed to define WebAssembly constructors";
    return false;
  }

  if (!StoreConstructorRef(env, module_ctor, &state->module_ctor_ref) ||
      !StoreConstructorRef(env, instance_ctor, &state->instance_ctor_ref) ||
      !StoreConstructorRef(env, memory_ctor, &state->memory_ctor_ref) ||
      !StoreConstructorRef(env, table_ctor, &state->table_ctor_ref) ||
      !StoreConstructorRef(env, global_ctor, &state->global_ctor_ref)) {
    if (error_out != nullptr)
      *error_out = "Failed to retain WebAssembly constructors";
    return false;
  }

  napi_value validate = nullptr;
  napi_value imports = nullptr;
  napi_value exports = nullptr;
  napi_value custom_sections = nullptr;
  if (napi_create_function(env, "__edgeValidate", NAPI_AUTO_LENGTH,
                           ValidateCallback, state, &validate) != napi_ok ||
      napi_create_function(env, "__edgeModuleImports", NAPI_AUTO_LENGTH,
                           ModuleImportsCallback, state, &imports) != napi_ok ||
      napi_create_function(env, "__edgeModuleExports", NAPI_AUTO_LENGTH,
                           ModuleExportsCallback, state, &exports) != napi_ok ||
      napi_create_function(env, "__edgeModuleCustomSections", NAPI_AUTO_LENGTH,
                           ModuleCustomSectionsCallback, state,
                           &custom_sections) != napi_ok) {
    if (error_out != nullptr)
      *error_out = "Failed to create WebAssembly native methods";
    return false;
  }

  DefineValue(env, webassembly, "Module", module_ctor);
  DefineValue(env, webassembly, "Instance", instance_ctor);
  DefineValue(env, webassembly, "Memory", memory_ctor);
  DefineValue(env, webassembly, "Table", table_ctor);
  DefineValue(env, webassembly, "Global", global_ctor);
  DefineValue(env, webassembly, "__edgeValidate", validate);
  DefineValue(env, webassembly, "__edgeModuleImports", imports);
  DefineValue(env, webassembly, "__edgeModuleExports", exports);
  DefineValue(env, webassembly, "__edgeModuleCustomSections", custom_sections);

  if (DefineValue(env, global, "WebAssembly", webassembly,
                  static_cast<napi_property_attributes>(
                      napi_writable | napi_configurable)) != napi_ok) {
    if (error_out != nullptr)
      *error_out = "Failed to install global WebAssembly";
    return false;
  }
  DeleteRefIfPresent(env, &state->webassembly_ref);
  napi_create_reference(env, webassembly, 1, &state->webassembly_ref);

  return InstallJsWrappers(env, error_out);
}

napi_value EdgePrepareQuickJsWebAssemblyClone(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr)
    return nullptr;

  WasmObjectKind kind;
  void *shared = nullptr;
  if (auto *module =
          Unwrap<WasmModuleObject>(env, value, WasmObjectKind::kModule);
      module != nullptr && module->module != nullptr) {
    kind = WasmObjectKind::kModule;
    shared = wasm_module_share(module->module);
  } else if (auto *memory =
                 Unwrap<WasmMemoryObject>(env, value, WasmObjectKind::kMemory);
             memory != nullptr && memory->memory != nullptr && memory->shared) {
    kind = WasmObjectKind::kMemory;
    shared = wasm_memory_share(memory->memory);
  } else {
    return nullptr;
  }
  if (shared == nullptr)
    return nullptr;

  auto *backing = new (std::nothrow) WasmCloneBacking();
  if (backing == nullptr) {
    if (kind == WasmObjectKind::kModule)
      wasm_shared_module_delete(static_cast<wasm_shared_module_t *>(shared));
    else
      wasm_shared_memory_delete(static_cast<wasm_shared_memory_t *>(shared));
    return nullptr;
  }
  backing->kind = kind;
  backing->shared = shared;

  napi_value clone_data = nullptr;
  if (unofficial_napi_create_external_sharedarraybuffer(
          env, backing, sizeof(*backing), WasmCloneBackingFinalize, backing,
          &clone_data) != napi_ok ||
      clone_data == nullptr) {
    // The external-buffer API invokes the finalizer when construction fails.
    return nullptr;
  }

  napi_value marker = nullptr;
  napi_value true_value = nullptr;
  if (napi_create_object(env, &marker) != napi_ok || marker == nullptr ||
      napi_get_boolean(env, true, &true_value) != napi_ok ||
      napi_set_named_property(env, marker,
                              "__ubiWebAssemblyCloneMarker",
                              true_value) != napi_ok ||
      napi_set_named_property(env, marker, "data", clone_data) != napi_ok) {
    return nullptr;
  }
  return marker;
}

bool EdgeIsQuickJsWebAssemblyCloneMarker(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr)
    return false;
  bool has_marker = false;
  if (napi_has_named_property(env, value, "__ubiWebAssemblyCloneMarker",
                              &has_marker) != napi_ok ||
      !has_marker) {
    return false;
  }
  napi_value marker = nullptr;
  bool enabled = false;
  return napi_get_named_property(env, value, "__ubiWebAssemblyCloneMarker",
                                 &marker) == napi_ok &&
         marker != nullptr &&
         napi_get_value_bool(env, marker, &enabled) == napi_ok && enabled;
}

napi_value EdgeRestoreQuickJsWebAssemblyClone(napi_env env,
                                               napi_value marker) {
  if (!EdgeIsQuickJsWebAssemblyCloneMarker(env, marker))
    return nullptr;
  napi_value clone_data = nullptr;
  if (napi_get_named_property(env, marker, "data", &clone_data) != napi_ok ||
      clone_data == nullptr) {
    return nullptr;
  }

  void *raw = nullptr;
  size_t size = 0;
  if (unofficial_napi_get_external_sharedarraybuffer_info(
          env, clone_data, &raw, &size) != napi_ok ||
      raw == nullptr || size != sizeof(WasmCloneBacking)) {
    return nullptr;
  }
  auto *backing = static_cast<WasmCloneBacking *>(raw);
  if (backing->magic != kCloneBackingMagic || backing->shared == nullptr)
    return nullptr;

  std::string error;
  WasmState *state = EnsureState(env, &error);
  if (state == nullptr)
    return nullptr;

  if (backing->kind == WasmObjectKind::kModule) {
    wasm_module_t *module = wasm_module_obtain(
        state->store, static_cast<wasm_shared_module_t *>(backing->shared));
    return CreateObjectFromOwned(state, WasmObjectKind::kModule, module);
  }
  if (backing->kind == WasmObjectKind::kMemory) {
    wasm_memory_t *memory = wasm_memory_obtain(
        state->store, static_cast<wasm_shared_memory_t *>(backing->shared));
    return CreateObjectFromOwned(state, WasmObjectKind::kMemory, memory);
  }
  return nullptr;
}
