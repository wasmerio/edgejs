#include "internal_binding/dispatch.h"

#include <unordered_map>
#include <unordered_set>

#include "internal_binding/helpers.h"

namespace internal_binding {

namespace {

std::unordered_map<napi_env, napi_ref> g_mksnapshot_refs;
std::unordered_set<napi_env> g_mksnapshot_cleanup_hook_registered;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void OnMksnapshotEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_mksnapshot_cleanup_hook_registered.erase(env);

  auto it = g_mksnapshot_refs.find(env);
  if (it == g_mksnapshot_refs.end()) return;
  DeleteRefIfPresent(env, &it->second);
  g_mksnapshot_refs.erase(it);
}

void EnsureMksnapshotCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_mksnapshot_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnMksnapshotEnvCleanup, env) != napi_ok) {
    g_mksnapshot_cleanup_hook_registered.erase(it);
  }
}

napi_value ReturnUndefined(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

napi_value GetCachedMksnapshot(napi_env env) {
  auto it = g_mksnapshot_refs.find(env);
  if (it == g_mksnapshot_refs.end() || it->second == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, it->second, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

}  // namespace

napi_value ResolveMksnapshot(napi_env env, const ResolveOptions& /*options*/) {
  EnsureMksnapshotCleanupHook(env);
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedMksnapshot(env);
  if (cached != nullptr) return cached;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  auto define_noop = [&](const char* name) {
    napi_value fn = nullptr;
    if (napi_create_function(env, name, NAPI_AUTO_LENGTH, ReturnUndefined, nullptr, &fn) == napi_ok &&
        fn != nullptr) {
      napi_set_named_property(env, out, name, fn);
    }
  };
  define_noop("runEmbedderPreload");
  define_noop("compileSerializeMain");
  define_noop("setSerializeCallback");
  define_noop("setDeserializeCallback");
  define_noop("setDeserializeMainFunction");

  void* data = nullptr;
  napi_value ab = nullptr;
  napi_value is_building_snapshot_buffer = nullptr;
  if (napi_create_arraybuffer(env, 1, &data, &ab) == napi_ok && data != nullptr && ab != nullptr) {
    static_cast<uint8_t*>(data)[0] = 0;
    if (napi_create_typedarray(env, napi_uint8_array, 1, ab, 0, &is_building_snapshot_buffer) == napi_ok &&
        is_building_snapshot_buffer != nullptr) {
      napi_set_named_property(env, out, "isBuildingSnapshotBuffer", is_building_snapshot_buffer);
    }
  }

  SetString(env, out, "anonymousMainPath", "<anonymous>");

  auto& ref = g_mksnapshot_refs[env];
  DeleteRefIfPresent(env, &ref);
  napi_create_reference(env, out, 1, &ref);
  return out;
}

}  // namespace internal_binding
