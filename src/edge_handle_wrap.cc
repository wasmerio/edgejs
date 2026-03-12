#include "edge_handle_wrap.h"

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "internal_binding/helpers.h"
#include "edge_env_loop.h"
#include "edge_module_loader.h"
#include "edge_runtime.h"

namespace {

struct HandleSymbolCache {
  napi_ref symbols_ref = nullptr;
  napi_ref owner_symbol_ref = nullptr;
  napi_ref handle_onclose_symbol_ref = nullptr;
};

std::unordered_map<napi_env, HandleSymbolCache> g_handle_symbols;
std::unordered_set<napi_env> g_handle_symbol_cleanup_hook_registered;

struct HandleWrapEnvState {
  EdgeHandleWrap* head = nullptr;
  bool cleanup_hook_registered = false;
  bool cleanup_started = false;
};

std::mutex g_handle_wrap_env_states_mutex;
std::unordered_map<napi_env, HandleWrapEnvState> g_handle_wrap_env_states;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void UnlinkHandleWrapLocked(HandleWrapEnvState* state, EdgeHandleWrap* wrap) {
  if (state == nullptr || wrap == nullptr || !wrap->attached) return;
  if (wrap->prev != nullptr) {
    wrap->prev->next = wrap->next;
  } else if (state->head == wrap) {
    state->head = wrap->next;
  }
  if (wrap->next != nullptr) {
    wrap->next->prev = wrap->prev;
  }
  wrap->prev = nullptr;
  wrap->next = nullptr;
  wrap->attached = false;
}

void MaybeEraseHandleWrapStateLocked(napi_env env) {
  auto it = g_handle_wrap_env_states.find(env);
  if (it == g_handle_wrap_env_states.end()) return;
  if (it->second.head != nullptr || it->second.cleanup_hook_registered) return;
  g_handle_wrap_env_states.erase(it);
}

void RunHandleWrapEnvCleanup(napi_env env) {
  if (env == nullptr) return;
  std::vector<EdgeHandleWrap*> wraps_to_close;
  {
    std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
    auto it = g_handle_wrap_env_states.find(env);
    if (it == g_handle_wrap_env_states.end()) return;
    it->second.cleanup_hook_registered = false;
    it->second.cleanup_started = true;
    for (EdgeHandleWrap* wrap = it->second.head; wrap != nullptr; wrap = wrap->next) {
      wraps_to_close.push_back(wrap);
    }
  }

  for (EdgeHandleWrap* wrap : wraps_to_close) {
    if (wrap == nullptr ||
        !wrap->attached ||
        wrap->state != kEdgeHandleInitialized ||
        wrap->close_callback == nullptr) {
      continue;
    }
    wrap->close_callback(wrap->close_data);
  }

  uv_loop_t* loop = EdgeGetExistingEnvLoop(env);
  while (loop != nullptr) {
    bool empty = false;
    {
      std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
      auto it = g_handle_wrap_env_states.find(env);
      empty = (it == g_handle_wrap_env_states.end()) || (it->second.head == nullptr);
      if (empty) {
        MaybeEraseHandleWrapStateLocked(env);
      }
    }
    if (empty) break;
    (void)uv_run(loop, UV_RUN_ONCE);
  }
}

void OnHandleWrapEnvCleanup(void* data) {
  RunHandleWrapEnvCleanup(static_cast<napi_env>(data));
}

void EnsureHandleWrapCleanupHook(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
  auto& state = g_handle_wrap_env_states[env];
  if (state.cleanup_hook_registered || state.cleanup_started) return;
  if (napi_add_env_cleanup_hook(env, OnHandleWrapEnvCleanup, env) == napi_ok) {
    state.cleanup_hook_registered = true;
  }
}

void OnHandleSymbolsEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_handle_symbol_cleanup_hook_registered.erase(env);

  auto it = g_handle_symbols.find(env);
  if (it == g_handle_symbols.end()) return;
  DeleteRefIfPresent(env, &it->second.symbols_ref);
  DeleteRefIfPresent(env, &it->second.owner_symbol_ref);
  DeleteRefIfPresent(env, &it->second.handle_onclose_symbol_ref);
  g_handle_symbols.erase(it);
}

void EnsureHandleSymbolsCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_handle_symbol_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnHandleSymbolsEnvCleanup, env) != napi_ok) {
    g_handle_symbol_cleanup_hook_registered.erase(it);
  }
}

napi_value ResolveInternalBinding(napi_env env, const char* name) {
  if (env == nullptr || name == nullptr) return nullptr;

  napi_value global = internal_binding::GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value internal_binding = EdgeGetInternalBinding(env);
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

HandleSymbolCache& GetHandleSymbolCache(napi_env env) {
  EnsureHandleSymbolsCleanupHook(env);
  return g_handle_symbols[env];
}

napi_value GetSymbolsBinding(napi_env env) {
  HandleSymbolCache& cache = GetHandleSymbolCache(env);
  napi_value binding = EdgeHandleWrapGetRefValue(env, cache.symbols_ref);
  if (binding != nullptr) return binding;

  binding = ResolveInternalBinding(env, "symbols");
  if (binding == nullptr) return nullptr;

  EdgeHandleWrapDeleteRefIfPresent(env, &cache.symbols_ref);
  napi_create_reference(env, binding, 1, &cache.symbols_ref);
  return binding;
}

napi_value GetNamedCachedSymbol(napi_env env, const char* key, napi_ref* slot) {
  if (slot == nullptr) return nullptr;
  napi_value symbol = EdgeHandleWrapGetRefValue(env, *slot);
  if (symbol != nullptr) return symbol;

  napi_value symbols = GetSymbolsBinding(env);
  if (symbols == nullptr) return nullptr;
  if (napi_get_named_property(env, symbols, key, &symbol) != napi_ok || symbol == nullptr) {
    return nullptr;
  }

  EdgeHandleWrapDeleteRefIfPresent(env, slot);
  napi_create_reference(env, symbol, 1, slot);
  return symbol;
}

napi_value GetOwnerSymbol(napi_env env) {
  HandleSymbolCache& cache = GetHandleSymbolCache(env);
  return GetNamedCachedSymbol(env, "owner_symbol", &cache.owner_symbol_ref);
}

napi_value GetHandleOnCloseSymbol(napi_env env) {
  HandleSymbolCache& cache = GetHandleSymbolCache(env);
  return GetNamedCachedSymbol(env, "handle_onclose", &cache.handle_onclose_symbol_ref);
}

void SetPropertyIfPresent(napi_env env, napi_value obj, napi_value key, napi_value value) {
  if (env == nullptr || obj == nullptr || key == nullptr || value == nullptr) return;
  napi_set_property(env, obj, key, value);
}

}  // namespace

void EdgeHandleWrapInit(EdgeHandleWrap* wrap, napi_env env) {
  if (wrap == nullptr) return;
  wrap->env = env;
  wrap->wrapper_ref = nullptr;
  wrap->active_handle_token = nullptr;
  wrap->close_data = nullptr;
  wrap->uv_handle = nullptr;
  wrap->close_callback = nullptr;
  wrap->prev = nullptr;
  wrap->next = nullptr;
  wrap->attached = false;
  wrap->finalized = false;
  wrap->delete_on_close = false;
  wrap->wrapper_ref_held = false;
  wrap->state = kEdgeHandleUninitialized;
}

void EdgeHandleWrapAttach(EdgeHandleWrap* wrap,
                         void* close_data,
                         uv_handle_t* handle,
                         EdgeHandleWrapCloseCallback close_callback) {
  if (wrap == nullptr || wrap->env == nullptr || handle == nullptr || close_callback == nullptr) return;
  EnsureHandleWrapCleanupHook(wrap->env);
  std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
  auto& state = g_handle_wrap_env_states[wrap->env];
  if (state.cleanup_started) return;
  if (wrap->attached) {
    UnlinkHandleWrapLocked(&state, wrap);
  }
  wrap->close_data = close_data;
  wrap->uv_handle = handle;
  wrap->close_callback = close_callback;
  wrap->prev = nullptr;
  wrap->next = state.head;
  if (state.head != nullptr) {
    state.head->prev = wrap;
  }
  state.head = wrap;
  wrap->attached = true;
}

void EdgeHandleWrapDetach(EdgeHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || !wrap->attached) return;
  std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
  auto it = g_handle_wrap_env_states.find(wrap->env);
  if (it == g_handle_wrap_env_states.end()) {
    wrap->close_data = nullptr;
    wrap->uv_handle = nullptr;
    wrap->close_callback = nullptr;
    wrap->prev = nullptr;
    wrap->next = nullptr;
    wrap->attached = false;
    return;
  }
  UnlinkHandleWrapLocked(&it->second, wrap);
  wrap->close_data = nullptr;
  wrap->uv_handle = nullptr;
  wrap->close_callback = nullptr;
  MaybeEraseHandleWrapStateLocked(wrap->env);
}

napi_value EdgeHandleWrapGetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) {
    return nullptr;
  }
  return value;
}

void EdgeHandleWrapDeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void EdgeHandleWrapHoldWrapperRef(EdgeHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr || wrap->wrapper_ref_held) return;
  uint32_t ref_count = 0;
  if (napi_reference_ref(wrap->env, wrap->wrapper_ref, &ref_count) == napi_ok) {
    wrap->wrapper_ref_held = true;
  }
}

void EdgeHandleWrapReleaseWrapperRef(EdgeHandleWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr || !wrap->wrapper_ref_held) return;
  uint32_t ref_count = 0;
  if (napi_reference_unref(wrap->env, wrap->wrapper_ref, &ref_count) == napi_ok) {
    wrap->wrapper_ref_held = false;
  }
}

bool EdgeHandleWrapCancelFinalizer(EdgeHandleWrap* wrap, void* native_object) {
  if (wrap == nullptr ||
      wrap->env == nullptr ||
      wrap->wrapper_ref == nullptr ||
      native_object == nullptr ||
      wrap->finalized) {
    return false;
  }

  napi_value self = EdgeHandleWrapGetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return false;

  void* removed = nullptr;
  if (napi_remove_wrap(wrap->env, self, &removed) != napi_ok || removed != native_object) {
    return false;
  }

  EdgeHandleWrapDeleteRefIfPresent(wrap->env, &wrap->wrapper_ref);
  wrap->wrapper_ref_held = false;
  return true;
}

napi_value EdgeHandleWrapGetActiveOwner(napi_env env, napi_ref wrapper_ref) {
  napi_value wrapper = EdgeHandleWrapGetRefValue(env, wrapper_ref);
  if (wrapper == nullptr) return nullptr;

  napi_value owner_symbol = GetOwnerSymbol(env);
  if (owner_symbol != nullptr) {
    napi_value owner = nullptr;
    if (napi_get_property(env, wrapper, owner_symbol, &owner) == napi_ok && owner != nullptr) {
      napi_valuetype type = napi_undefined;
      if (napi_typeof(env, owner, &type) == napi_ok && type != napi_undefined && type != napi_null) {
        return owner;
      }
    }
  }
  return wrapper;
}

void EdgeHandleWrapSetOnCloseCallback(napi_env env, napi_value wrapper, napi_value callback) {
  if (env == nullptr || wrapper == nullptr || callback == nullptr) return;
  napi_value symbol = GetHandleOnCloseSymbol(env);
  if (symbol == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, callback, &type) != napi_ok || type != napi_function) return;
  SetPropertyIfPresent(env, wrapper, symbol, callback);
}

void EdgeHandleWrapMaybeCallOnClose(EdgeHandleWrap* wrap) {
  if (wrap == nullptr ||
      wrap->env == nullptr ||
      wrap->finalized ||
      EdgeHandleWrapEnvCleanupStarted(wrap->env)) {
    return;
  }
  napi_value self = EdgeHandleWrapGetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;

  napi_value symbol = GetHandleOnCloseSymbol(wrap->env);
  if (symbol == nullptr) return;

  bool has_callback = false;
  if (napi_has_property(wrap->env, self, symbol, &has_callback) != napi_ok || !has_callback) {
    return;
  }

  napi_value callback = nullptr;
  if (napi_get_property(wrap->env, self, symbol, &callback) != napi_ok || callback == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(wrap->env, callback, &type) != napi_ok || type != napi_function) return;

  napi_value ignored = nullptr;
  EdgeMakeCallback(wrap->env, self, callback, 0, nullptr, &ignored);

  napi_value undefined = nullptr;
  napi_get_undefined(wrap->env, &undefined);
  SetPropertyIfPresent(wrap->env, self, symbol, undefined);
}

bool EdgeHandleWrapHasRef(const EdgeHandleWrap* wrap, const uv_handle_t* handle) {
  if (wrap == nullptr || handle == nullptr || wrap->state != kEdgeHandleInitialized) return false;
  return uv_has_ref(handle) != 0;
}

bool EdgeHandleWrapEnvCleanupStarted(napi_env env) {
  if (env == nullptr) return false;
  std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
  auto it = g_handle_wrap_env_states.find(env);
  return it != g_handle_wrap_env_states.end() && it->second.cleanup_started;
}

void EdgeHandleWrapRunEnvCleanup(napi_env env) {
  if (env == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(g_handle_wrap_env_states_mutex);
    auto it = g_handle_wrap_env_states.find(env);
    if (it == g_handle_wrap_env_states.end()) return;
    if (it->second.cleanup_hook_registered) {
      (void)napi_remove_env_cleanup_hook(env, OnHandleWrapEnvCleanup, env);
      it->second.cleanup_hook_registered = false;
    }
  }
  RunHandleWrapEnvCleanup(env);
}
