#include "internal_binding/binding_messaging.h"
#include "internal_binding/dispatch.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <uv.h>

#include "internal_binding/helpers.h"
#include "unofficial_napi.h"
#include "../ubi_module_loader.h"
#include "ubi_active_resource.h"
#include "ubi_async_wrap.h"
#include "ubi_env_loop.h"
#include "ubi_handle_wrap.h"
#include "ubi_runtime.h"

namespace internal_binding {

struct MessagePortWrap;
struct BroadcastChannelGroup;

struct QueuedMessage {
  void* payload_data = nullptr;
  bool is_close = false;
  MessagePortWrap* close_source_wrap = nullptr;
  struct TransferredPortEntry {
    napi_ref source_port_ref = nullptr;
    UbiMessagePortDataPtr data;
  };
  std::vector<TransferredPortEntry> transferred_ports;
};

struct UbiMessagePortData {
  std::mutex mutex;
  std::weak_ptr<UbiMessagePortData> sibling;
  std::shared_ptr<BroadcastChannelGroup> broadcast_group;
  std::deque<QueuedMessage> queued_messages;
  bool close_message_enqueued = false;
  bool closed = false;
  MessagePortWrap* attached_wrap = nullptr;
};

struct BroadcastChannelGroup {
  explicit BroadcastChannelGroup(std::string group_name) : name(std::move(group_name)) {}

  std::mutex mutex;
  std::string name;
  std::vector<std::weak_ptr<UbiMessagePortData>> members;
};

struct MessagePortWrap {
  UbiHandleWrap handle_wrap{};
  UbiMessagePortDataPtr data;
  uv_async_t async{};
  int64_t async_id = 0;
  bool closing_has_ref = false;
  bool receiving_messages = false;
};

namespace {

struct MessagingState {
  napi_ref binding_ref = nullptr;
  napi_ref deserializer_create_object_ref = nullptr;
  napi_ref emit_message_ref = nullptr;
  napi_ref message_port_ctor_ref = nullptr;
  napi_ref no_message_symbol_ref = nullptr;
  napi_ref oninit_symbol_ref = nullptr;
  uint32_t next_shared_handle_id = 1;
  std::unordered_map<uint32_t, napi_ref> shared_handle_refs;
};

std::unordered_map<napi_env, MessagingState> g_messaging_states;
std::unordered_set<napi_env> g_messaging_cleanup_hook_registered;
std::mutex g_broadcast_groups_mutex;
std::unordered_map<std::string, std::weak_ptr<BroadcastChannelGroup>> g_broadcast_groups;

napi_value ResolveDOMExceptionValue(napi_env env);
napi_value ResolveEmitMessageValue(napi_env env);
MessagePortWrap* UnwrapMessagePort(napi_env env, napi_value value);
napi_value GetTransferListValue(napi_env env, napi_value value);
bool ApplyArrayBufferTransfers(napi_env env, napi_value options);
napi_value TryRequireModule(napi_env env, const char* module_name);

napi_value GetNamed(napi_env env, napi_value obj, const char* key) {
  napi_value out = nullptr;
  if (obj == nullptr || napi_get_named_property(env, obj, key, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

bool IsFunction(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

bool IsObjectLike(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && (type == napi_object || type == napi_function);
}

bool IsUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_undefined;
}

bool IsNullOrUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok &&
         (type == napi_undefined || type == napi_null);
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

void ClearPendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
    napi_value ignored = nullptr;
    napi_get_and_clear_last_exception(env, &ignored);
  }
}

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void DeleteTransferredPortRefs(napi_env env,
                               std::vector<QueuedMessage::TransferredPortEntry>* transferred_ports) {
  if (transferred_ports == nullptr) return;
  for (auto& entry : *transferred_ports) {
    DeleteRefIfPresent(env, &entry.source_port_ref);
  }
}

void OnMessagingEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_messaging_cleanup_hook_registered.erase(env);

  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return;
  for (auto& entry : it->second.shared_handle_refs) {
    if (entry.second != nullptr) {
      napi_delete_reference(env, entry.second);
    }
  }
  DeleteRefIfPresent(env, &it->second.binding_ref);
  DeleteRefIfPresent(env, &it->second.deserializer_create_object_ref);
  DeleteRefIfPresent(env, &it->second.emit_message_ref);
  DeleteRefIfPresent(env, &it->second.message_port_ctor_ref);
  DeleteRefIfPresent(env, &it->second.no_message_symbol_ref);
  DeleteRefIfPresent(env, &it->second.oninit_symbol_ref);
  g_messaging_states.erase(it);
}

void EnsureMessagingCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_messaging_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnMessagingEnvCleanup, env) != napi_ok) {
    g_messaging_cleanup_hook_registered.erase(it);
  }
}

MessagingState& EnsureMessagingState(napi_env env) {
  EnsureMessagingCleanupHook(env);
  return g_messaging_states[env];
}

void ThrowTypeErrorWithCode(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value) != napi_ok ||
      code_value == nullptr ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      message_value == nullptr ||
      napi_create_type_error(env, code_value, message_value, &error_value) != napi_ok ||
      error_value == nullptr) {
    napi_throw_type_error(env, code, message);
    return;
  }
  napi_throw(env, error_value);
}

napi_value CreateDataCloneError(napi_env env, const char* message) {
  napi_value dom_exception = ResolveDOMExceptionValue(env);
  if (IsFunction(env, dom_exception)) {
    napi_value argv[2] = {nullptr, nullptr};
    napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &argv[0]);
    napi_create_string_utf8(env, "DataCloneError", NAPI_AUTO_LENGTH, &argv[1]);
    napi_value err = nullptr;
    if (napi_new_instance(env, dom_exception, 2, argv, &err) == napi_ok && err != nullptr) {
      return err;
    }
    ClearPendingException(env);
  }

  napi_value msg = nullptr;
  napi_value err = nullptr;
  napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &msg);
  napi_create_error(env, nullptr, msg, &err);
  if (err == nullptr) return nullptr;

  napi_value code = nullptr;
  napi_create_int32(env, 25, &code);
  if (code != nullptr) napi_set_named_property(env, err, "code", code);

  napi_value name = nullptr;
  napi_create_string_utf8(env, "DataCloneError", NAPI_AUTO_LENGTH, &name);
  if (name != nullptr) napi_set_named_property(env, err, "name", name);
  return err;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value value = nullptr;
  if (napi_get_reference_value(env, ref, &value) != napi_ok || value == nullptr) return nullptr;
  return value;
}

napi_value GetInternalBindingValue(napi_env env, const char* name) {
  if (name == nullptr) return nullptr;
  napi_value global = GetGlobal(env);
  napi_value internal_binding = UbiGetInternalBinding(env);
  if (!IsFunction(env, internal_binding)) {
    internal_binding = GetNamed(env, global, "internalBinding");
  }
  if (!IsFunction(env, internal_binding)) return nullptr;

  napi_value name_value = nullptr;
  if (napi_create_string_utf8(env, name, NAPI_AUTO_LENGTH, &name_value) != napi_ok || name_value == nullptr) {
    return nullptr;
  }

  napi_value argv[1] = {name_value};
  napi_value out = nullptr;
  if (napi_call_function(env, global, internal_binding, 1, argv, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  return out;
}

napi_value GetUtilPrivateSymbol(napi_env env, const char* key) {
  napi_value util_binding = GetInternalBindingValue(env, "util");
  napi_value private_symbols = GetNamed(env, util_binding, "privateSymbols");
  return GetNamed(env, private_symbols, key);
}

napi_value GetMessagingSymbol(napi_env env, const char* key) {
  napi_value symbols = GetInternalBindingValue(env, "symbols");
  return GetNamed(env, symbols, key);
}

napi_value TakePendingException(napi_env env) {
  bool pending = false;
  if (napi_is_exception_pending(env, &pending) != napi_ok || !pending) return nullptr;

  napi_value exception = nullptr;
  if (napi_get_and_clear_last_exception(env, &exception) != napi_ok || exception == nullptr) return nullptr;
  return exception;
}

napi_value CreateErrorWithMessage(napi_env env, const char* code, const char* message) {
  napi_value code_value = nullptr;
  napi_value message_value = nullptr;
  napi_value error_value = nullptr;
  if (message == nullptr ||
      napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &message_value) != napi_ok ||
      message_value == nullptr) {
    return nullptr;
  }
  if (code != nullptr) {
    napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value);
  }
  napi_create_error(env, code_value, message_value, &error_value);
  return error_value;
}

void SetRefToValue(napi_env env, napi_ref* slot, napi_value value) {
  if (slot == nullptr) return;
  DeleteRefIfPresent(env, slot);
  if (value == nullptr) return;
  napi_create_reference(env, value, 1, slot);
}

bool IsCloneableTransferableValue(napi_env env, napi_value value) {
  if (!IsObjectLike(env, value)) return false;

  napi_value transfer_mode_symbol = GetUtilPrivateSymbol(env, "transfer_mode_private_symbol");
  napi_value clone_symbol = GetMessagingSymbol(env, "messaging_clone_symbol");
  if (transfer_mode_symbol == nullptr || clone_symbol == nullptr) return false;

  bool has_mode = false;
  if (napi_has_property(env, value, transfer_mode_symbol, &has_mode) != napi_ok || !has_mode) return false;

  napi_value transfer_mode = nullptr;
  if (napi_get_property(env, value, transfer_mode_symbol, &transfer_mode) != napi_ok || transfer_mode == nullptr) {
    return false;
  }

  uint32_t mode = 0;
  if (napi_get_value_uint32(env, transfer_mode, &mode) != napi_ok || (mode & 2u) == 0) return false;

  napi_value clone_method = nullptr;
  return napi_get_property(env, value, clone_symbol, &clone_method) == napi_ok && IsFunction(env, clone_method);
}

bool IsTransferableValue(napi_env env, napi_value value) {
  if (!IsObjectLike(env, value)) return false;

  napi_value transfer_mode_symbol = GetUtilPrivateSymbol(env, "transfer_mode_private_symbol");
  napi_value transfer_symbol = GetMessagingSymbol(env, "messaging_transfer_symbol");
  if (transfer_mode_symbol == nullptr || transfer_symbol == nullptr) return false;

  bool has_mode = false;
  if (napi_has_property(env, value, transfer_mode_symbol, &has_mode) != napi_ok || !has_mode) return false;

  napi_value transfer_mode = nullptr;
  if (napi_get_property(env, value, transfer_mode_symbol, &transfer_mode) != napi_ok || transfer_mode == nullptr) {
    return false;
  }

  uint32_t mode = 0;
  if (napi_get_value_uint32(env, transfer_mode, &mode) != napi_ok || (mode & 1u) == 0) return false;

  napi_value transfer_method = nullptr;
  return napi_get_property(env, value, transfer_symbol, &transfer_method) == napi_ok &&
         IsFunction(env, transfer_method);
}

bool IsBlobHandleValue(napi_env env, napi_value value) {
  if (!IsObjectLike(env, value)) return false;
  bool has_blob_data = false;
  return napi_has_named_property(env, value, "__ubi_blob_data", &has_blob_data) == napi_ok && has_blob_data;
}

bool IsInstanceOfValue(napi_env env, napi_value value, napi_value ctor) {
  if (!IsObjectLike(env, value) || !IsFunction(env, ctor)) return false;
  bool is_instance = false;
  if (napi_instanceof(env, value, ctor, &is_instance) != napi_ok) {
    ClearPendingException(env);
    return false;
  }
  return is_instance;
}

napi_value GetBlockListBindingValue(napi_env env) {
  return GetInternalBindingValue(env, "block_list");
}

napi_value GetBlockListBindingCtor(napi_env env, const char* name) {
  return GetNamed(env, GetBlockListBindingValue(env), name);
}

bool IsSocketAddressHandleValue(napi_env env, napi_value value) {
  return IsInstanceOfValue(env, value, GetBlockListBindingCtor(env, "SocketAddress"));
}

bool IsBlockListHandleValue(napi_env env, napi_value value) {
  return IsInstanceOfValue(env, value, GetBlockListBindingCtor(env, "BlockList"));
}

napi_value CreateHandleCloneMarker(napi_env env, const char* marker_name, napi_value data) {
  if (marker_name == nullptr || data == nullptr) return nullptr;
  napi_value marker = nullptr;
  if (napi_create_object(env, &marker) != napi_ok || marker == nullptr) return nullptr;
  napi_value true_value = nullptr;
  if (napi_get_boolean(env, true, &true_value) != napi_ok || true_value == nullptr ||
      napi_set_named_property(env, marker, marker_name, true_value) != napi_ok ||
      napi_set_named_property(env, marker, "data", data) != napi_ok) {
    return nullptr;
  }
  return marker;
}

bool IsHandleCloneMarker(napi_env env,
                         napi_value value,
                         const char* marker_name,
                         napi_value* data_out) {
  if (data_out != nullptr) *data_out = nullptr;
  if (!IsObjectLike(env, value) || marker_name == nullptr) return false;

  bool has_marker = false;
  if (napi_has_named_property(env, value, marker_name, &has_marker) != napi_ok || !has_marker) {
    return false;
  }

  napi_value marker_value = GetNamed(env, value, marker_name);
  bool is_marker = false;
  if (marker_value == nullptr || napi_get_value_bool(env, marker_value, &is_marker) != napi_ok || !is_marker) {
    return false;
  }

  napi_value data = GetNamed(env, value, "data");
  if (data == nullptr) return false;
  if (data_out != nullptr) *data_out = data;
  return true;
}

napi_value CreateBlobHandleCloneMarker(napi_env env, napi_value handle) {
  napi_value blob_data = GetNamed(env, handle, "__ubi_blob_data");
  if (blob_data == nullptr) return nullptr;

  return CreateHandleCloneMarker(env, "__ubiBlobHandleCloneMarker", blob_data);
}

bool IsBlobHandleCloneMarker(napi_env env, napi_value value, napi_value* data_out) {
  return IsHandleCloneMarker(env, value, "__ubiBlobHandleCloneMarker", data_out);
}

napi_value CreateSocketAddressHandleCloneMarker(napi_env env, napi_value handle) {
  napi_value detail_fn = GetNamed(env, handle, "detail");
  if (!IsFunction(env, detail_fn)) return nullptr;

  napi_value detail = nullptr;
  if (napi_create_object(env, &detail) != napi_ok || detail == nullptr) return nullptr;

  napi_value argv[1] = {detail};
  napi_value detail_out = nullptr;
  if (napi_call_function(env, handle, detail_fn, 1, argv, &detail_out) != napi_ok || detail_out == nullptr) {
    return nullptr;
  }

  return CreateHandleCloneMarker(env, "__ubiSocketAddressHandleCloneMarker", detail_out);
}

bool IsSocketAddressHandleCloneMarker(napi_env env, napi_value value, napi_value* data_out) {
  return IsHandleCloneMarker(env, value, "__ubiSocketAddressHandleCloneMarker", data_out);
}

napi_value CreateBlockListHandleCloneMarker(napi_env env, napi_value handle) {
  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return nullptr;

  const uint32_t id = it->second.next_shared_handle_id++;
  napi_ref handle_ref = nullptr;
  if (napi_create_reference(env, handle, 1, &handle_ref) != napi_ok || handle_ref == nullptr) {
    return nullptr;
  }
  it->second.shared_handle_refs[id] = handle_ref;

  napi_value id_value = nullptr;
  if (napi_create_uint32(env, id, &id_value) != napi_ok || id_value == nullptr) {
    napi_delete_reference(env, handle_ref);
    it->second.shared_handle_refs.erase(id);
    return nullptr;
  }

  return CreateHandleCloneMarker(env, "__ubiBlockListHandleCloneMarker", id_value);
}

bool IsBlockListHandleCloneMarker(napi_env env, napi_value value, napi_value* data_out) {
  return IsHandleCloneMarker(env, value, "__ubiBlockListHandleCloneMarker", data_out);
}

bool IsJSTransferableCloneMarker(napi_env env, napi_value value, napi_value* data_out, napi_value* info_out) {
  if (data_out != nullptr) *data_out = nullptr;
  if (info_out != nullptr) *info_out = nullptr;
  if (!IsObjectLike(env, value)) return false;

  bool has_marker = false;
  if (napi_has_named_property(env, value, "__ubiJSTransferableCloneMarker", &has_marker) != napi_ok || !has_marker) {
    return false;
  }

  napi_value marker_value = GetNamed(env, value, "__ubiJSTransferableCloneMarker");
  bool is_marker = false;
  if (marker_value == nullptr || napi_get_value_bool(env, marker_value, &is_marker) != napi_ok || !is_marker) {
    return false;
  }

  napi_value data = GetNamed(env, value, "data");
  napi_value info = GetNamed(env, value, "deserializeInfo");
  if (data == nullptr || info == nullptr) return false;
  if (data_out != nullptr) *data_out = data;
  if (info_out != nullptr) *info_out = info;
  return true;
}

bool IsStructuredClonePassThroughValue(napi_env env, napi_value value) {
  if (!IsObjectLike(env, value)) return true;

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) return true;

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) return true;

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) return true;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) return true;

  return false;
}

bool IsProcessEnvValue(napi_env env, napi_value value) {
  if (!IsObjectLike(env, value)) return false;

  napi_value global = GetGlobal(env);
  if (global == nullptr) return false;
  napi_value process = GetNamed(env, global, "process");
  if (!IsObjectLike(env, process)) return false;
  napi_value process_env = GetNamed(env, process, "env");
  if (!IsObjectLike(env, process_env)) return false;

  bool same = false;
  return napi_strict_equals(env, value, process_env, &same) == napi_ok && same;
}

napi_value PrepareTransferableDataForStructuredClone(napi_env env, napi_value value);
napi_value RestoreTransferableDataAfterStructuredClone(napi_env env, napi_value value);
struct ValueTransformPair {
  napi_value source = nullptr;
  napi_value target = nullptr;
};
bool PrepareJSTransferableCloneData(
    napi_env env, napi_value value, napi_value* data_out, napi_value* deserialize_info_out);
napi_value CreateJSTransferableCloneMarker(napi_env env, napi_value value);
napi_value DeserializeJSTransferableCloneMarker(napi_env env, napi_value data, napi_value deserialize_info);
napi_value CloneRootJSTransferableValueForQueue(napi_env env, napi_value value);
napi_value TransformTransferredPortsForQueue(
    napi_env env,
    napi_value value,
    const std::vector<QueuedMessage::TransferredPortEntry>& transferred_ports,
    std::vector<ValueTransformPair>* seen_pairs);
bool CollectTransferredPorts(
    napi_env env,
    napi_value transfer_list,
    std::vector<QueuedMessage::TransferredPortEntry>* out);

napi_value CloneArrayEntriesForStructuredClone(napi_env env, napi_value array) {
  uint32_t length = 0;
  if (napi_get_array_length(env, array, &length) != napi_ok) return nullptr;

  napi_value out = nullptr;
  if (napi_create_array_with_length(env, length, &out) != napi_ok || out == nullptr) return nullptr;

  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, array, i, &item) != napi_ok) return nullptr;
    napi_value cloned = PrepareTransferableDataForStructuredClone(env, item);
    if (cloned == nullptr) return nullptr;
    if (napi_set_element(env, out, i, cloned) != napi_ok) return nullptr;
  }
  return out;
}

napi_value CloneObjectPropertiesForStructuredClone(napi_env env, napi_value object) {
  napi_value keys = nullptr;
  if (napi_get_property_names(env, object, &keys) != napi_ok || keys == nullptr) return nullptr;

  uint32_t length = 0;
  if (napi_get_array_length(env, keys, &length) != napi_ok) return nullptr;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return nullptr;

  for (uint32_t i = 0; i < length; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) return nullptr;
    napi_value item = nullptr;
    if (napi_get_property(env, object, key, &item) != napi_ok) return nullptr;
    napi_value cloned = PrepareTransferableDataForStructuredClone(env, item);
    if (cloned == nullptr) return nullptr;
    if (napi_set_property(env, out, key, cloned) != napi_ok) return nullptr;
  }

  return out;
}

napi_value PrepareTransferableDataForStructuredClone(napi_env env, napi_value value) {
  if (value == nullptr || IsNullOrUndefinedValue(env, value) || IsStructuredClonePassThroughValue(env, value)) {
    return value;
  }
  if (IsBlobHandleValue(env, value)) {
    return CreateBlobHandleCloneMarker(env, value);
  }
  if (IsSocketAddressHandleValue(env, value)) {
    return CreateSocketAddressHandleCloneMarker(env, value);
  }
  if (IsBlockListHandleValue(env, value)) {
    return CreateBlockListHandleCloneMarker(env, value);
  }
  if (!IsObjectLike(env, value)) return value;

  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    return CloneArrayEntriesForStructuredClone(env, value);
  }
  return CloneObjectPropertiesForStructuredClone(env, value);
}

napi_value CreateBlobHandleFromCloneData(napi_env env, napi_value blob_data) {
  if (blob_data == nullptr) return nullptr;

  napi_value blob_binding = GetInternalBindingValue(env, "blob");
  napi_value create_blob = GetNamed(env, blob_binding, "createBlob");
  if (!IsFunction(env, create_blob)) return nullptr;

  size_t byte_length = 0;
  void* raw = nullptr;
  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, blob_data, &is_arraybuffer) != napi_ok || !is_arraybuffer ||
      napi_get_arraybuffer_info(env, blob_data, &raw, &byte_length) != napi_ok) {
    return nullptr;
  }

  napi_value sources = nullptr;
  if (napi_create_array_with_length(env, 1, &sources) != napi_ok || sources == nullptr) return nullptr;
  if (napi_set_element(env, sources, 0, blob_data) != napi_ok) return nullptr;

  napi_value length_value = nullptr;
  if (napi_create_uint32(
          env,
          static_cast<uint32_t>(std::min<size_t>(byte_length, std::numeric_limits<uint32_t>::max())),
          &length_value) != napi_ok ||
      length_value == nullptr) {
    return nullptr;
  }

  napi_value argv[2] = {sources, length_value};
  napi_value handle = nullptr;
  if (napi_call_function(env, blob_binding, create_blob, 2, argv, &handle) != napi_ok || handle == nullptr) {
    return nullptr;
  }
  return handle;
}

napi_value CreateSocketAddressHandleFromCloneData(napi_env env, napi_value data) {
  napi_value ctor = GetBlockListBindingCtor(env, "SocketAddress");
  if (!IsFunction(env, ctor) || data == nullptr) return nullptr;

  napi_value argv[4] = {
      GetNamed(env, data, "address"),
      GetNamed(env, data, "port"),
      GetNamed(env, data, "family"),
      GetNamed(env, data, "flowlabel"),
  };
  if (argv[0] == nullptr || argv[1] == nullptr || argv[2] == nullptr || argv[3] == nullptr) return nullptr;

  napi_value handle = nullptr;
  if (napi_new_instance(env, ctor, 4, argv, &handle) != napi_ok || handle == nullptr) {
    return nullptr;
  }
  return handle;
}

napi_value ExtractHandleFromTransferableClone(napi_env env, napi_value value) {
  if (value == nullptr) return nullptr;
  napi_value clone_symbol = GetMessagingSymbol(env, "messaging_clone_symbol");
  napi_value clone_method = nullptr;
  if (clone_symbol == nullptr ||
      napi_get_property(env, value, clone_symbol, &clone_method) != napi_ok ||
      !IsFunction(env, clone_method)) {
    return nullptr;
  }

  napi_value clone_result = nullptr;
  if (napi_call_function(env, value, clone_method, 0, nullptr, &clone_result) != napi_ok || clone_result == nullptr) {
    return nullptr;
  }

  napi_value clone_data = GetNamed(env, clone_result, "data");
  return GetNamed(env, clone_data, "handle");
}

napi_value CreateBlockListHandleFromCloneData(napi_env env, napi_value rules) {
  uint32_t handle_id = 0;
  if (rules != nullptr && napi_get_value_uint32(env, rules, &handle_id) == napi_ok) {
    auto it = g_messaging_states.find(env);
    if (it != g_messaging_states.end()) {
      auto ref_it = it->second.shared_handle_refs.find(handle_id);
      if (ref_it != it->second.shared_handle_refs.end()) {
        return GetRefValue(env, ref_it->second);
      }
    }
    return nullptr;
  }

  napi_value blocklist_module = TryRequireModule(env, "internal/blocklist");
  napi_value blocklist_ctor = GetNamed(env, blocklist_module, "BlockList");
  if (!IsFunction(env, blocklist_ctor)) return nullptr;

  napi_value blocklist = nullptr;
  if (napi_new_instance(env, blocklist_ctor, 0, nullptr, &blocklist) != napi_ok || blocklist == nullptr) {
    return nullptr;
  }

  napi_value from_json = GetNamed(env, blocklist, "fromJSON");
  if (!IsFunction(env, from_json)) return nullptr;

  napi_value argv[1] = {rules};
  napi_value ignored = nullptr;
  if (napi_call_function(env, blocklist, from_json, 1, argv, &ignored) != napi_ok) {
    return nullptr;
  }

  return ExtractHandleFromTransferableClone(env, blocklist);
}

napi_value RestoreTransferableDataAfterStructuredClone(napi_env env, napi_value value) {
  if (value == nullptr || IsNullOrUndefinedValue(env, value) || IsStructuredClonePassThroughValue(env, value)) {
    return value;
  }

  napi_value transferable_data = nullptr;
  napi_value deserialize_info = nullptr;
  if (IsJSTransferableCloneMarker(env, value, &transferable_data, &deserialize_info)) {
    napi_value restored_data = RestoreTransferableDataAfterStructuredClone(env, transferable_data);
    if (restored_data == nullptr) return nullptr;
    return DeserializeJSTransferableCloneMarker(env, restored_data, deserialize_info);
  }

  napi_value blob_data = nullptr;
  if (IsBlobHandleCloneMarker(env, value, &blob_data)) {
    return CreateBlobHandleFromCloneData(env, blob_data);
  }

  napi_value socket_address_data = nullptr;
  if (IsSocketAddressHandleCloneMarker(env, value, &socket_address_data)) {
    return CreateSocketAddressHandleFromCloneData(env, socket_address_data);
  }

  napi_value block_list_data = nullptr;
  if (IsBlockListHandleCloneMarker(env, value, &block_list_data)) {
    return CreateBlockListHandleFromCloneData(env, block_list_data);
  }

  if (!IsObjectLike(env, value)) return value;

  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    uint32_t length = 0;
    if (napi_get_array_length(env, value, &length) != napi_ok) return nullptr;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value item = nullptr;
      if (napi_get_element(env, value, i, &item) != napi_ok) return nullptr;
      napi_value restored = RestoreTransferableDataAfterStructuredClone(env, item);
      if (restored == nullptr || napi_set_element(env, value, i, restored) != napi_ok) return nullptr;
    }
    return value;
  }

  napi_value keys = nullptr;
  if (napi_get_property_names(env, value, &keys) != napi_ok || keys == nullptr) return nullptr;

  uint32_t length = 0;
  if (napi_get_array_length(env, keys, &length) != napi_ok) return nullptr;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) return nullptr;
    napi_value item = nullptr;
    if (napi_get_property(env, value, key, &item) != napi_ok) return nullptr;
    napi_value restored = RestoreTransferableDataAfterStructuredClone(env, item);
    if (restored == nullptr || napi_set_property(env, value, key, restored) != napi_ok) return nullptr;
  }
  return value;
}

napi_value DeserializeJSTransferableCloneMarker(napi_env env, napi_value data, napi_value deserialize_info) {
  if (data == nullptr || deserialize_info == nullptr) return nullptr;

  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end() || it->second.deserializer_create_object_ref == nullptr) return nullptr;

  napi_value deserializer_factory = GetRefValue(env, it->second.deserializer_create_object_ref);
  if (!IsFunction(env, deserializer_factory)) return nullptr;

  napi_value receiver = Undefined(env);
  napi_value factory_argv[1] = {deserialize_info};
  napi_value out = nullptr;
  if (napi_call_function(env, receiver, deserializer_factory, 1, factory_argv, &out) != napi_ok || out == nullptr) {
    return nullptr;
  }

  napi_value deserialize_symbol = GetMessagingSymbol(env, "messaging_deserialize_symbol");
  napi_value deserialize_method = nullptr;
  if (deserialize_symbol == nullptr ||
      napi_get_property(env, out, deserialize_symbol, &deserialize_method) != napi_ok ||
      !IsFunction(env, deserialize_method)) {
    return nullptr;
  }

  napi_value deserialize_argv[1] = {data};
  napi_value ignored = nullptr;
  if (napi_call_function(env, out, deserialize_method, 1, deserialize_argv, &ignored) != napi_ok) {
    return nullptr;
  }

  return out;
}

bool PrepareJSTransferableCloneData(
    napi_env env, napi_value value, napi_value* data_out, napi_value* deserialize_info_out) {
  if (data_out != nullptr) *data_out = nullptr;
  if (deserialize_info_out != nullptr) *deserialize_info_out = nullptr;
  if (!IsCloneableTransferableValue(env, value)) return false;

  napi_value clone_symbol = GetMessagingSymbol(env, "messaging_clone_symbol");
  napi_value clone_method = nullptr;
  if (clone_symbol == nullptr ||
      napi_get_property(env, value, clone_symbol, &clone_method) != napi_ok ||
      !IsFunction(env, clone_method)) {
    return false;
  }

  napi_value clone_result = nullptr;
  if (napi_call_function(env, value, clone_method, 0, nullptr, &clone_result) != napi_ok || clone_result == nullptr) {
    return false;
  }

  napi_value clone_data = GetNamed(env, clone_result, "data");
  napi_value deserialize_info = GetNamed(env, clone_result, "deserializeInfo");
  if (clone_data == nullptr || deserialize_info == nullptr) return false;

  napi_value prepared_data = PrepareTransferableDataForStructuredClone(env, clone_data);
  if (prepared_data == nullptr) return false;

  if (data_out != nullptr) *data_out = prepared_data;
  if (deserialize_info_out != nullptr) *deserialize_info_out = deserialize_info;
  return true;
}

bool PrepareJSTransferableTransferData(napi_env env,
                                       napi_value value,
                                       napi_value* data_out,
                                       napi_value* deserialize_info_out,
                                       napi_value* transfer_list_out) {
  if (data_out != nullptr) *data_out = nullptr;
  if (deserialize_info_out != nullptr) *deserialize_info_out = nullptr;
  if (transfer_list_out != nullptr) *transfer_list_out = nullptr;
  if (!IsTransferableValue(env, value)) return false;

  napi_value transfer_list = nullptr;
  napi_value transfer_list_symbol = GetMessagingSymbol(env, "messaging_transfer_list_symbol");
  napi_value transfer_list_method = nullptr;
  if (transfer_list_symbol != nullptr &&
      napi_get_property(env, value, transfer_list_symbol, &transfer_list_method) == napi_ok &&
      IsFunction(env, transfer_list_method)) {
    napi_value list_value = nullptr;
    if (napi_call_function(env, value, transfer_list_method, 0, nullptr, &list_value) != napi_ok) {
      return false;
    }
    transfer_list = list_value;
  }

  napi_value transfer_symbol = GetMessagingSymbol(env, "messaging_transfer_symbol");
  napi_value transfer_method = nullptr;
  if (transfer_symbol == nullptr ||
      napi_get_property(env, value, transfer_symbol, &transfer_method) != napi_ok ||
      !IsFunction(env, transfer_method)) {
    return false;
  }

  napi_value transfer_result = nullptr;
  if (napi_call_function(env, value, transfer_method, 0, nullptr, &transfer_result) != napi_ok ||
      transfer_result == nullptr) {
    return false;
  }

  napi_value transfer_data = GetNamed(env, transfer_result, "data");
  napi_value deserialize_info = GetNamed(env, transfer_result, "deserializeInfo");
  if (transfer_data == nullptr || deserialize_info == nullptr) return false;

  if (data_out != nullptr) *data_out = transfer_data;
  if (deserialize_info_out != nullptr) *deserialize_info_out = deserialize_info;
  if (transfer_list_out != nullptr) *transfer_list_out = transfer_list;
  return true;
}

napi_value CreateJSTransferableCloneMarker(napi_env env, napi_value value) {
  napi_value prepared_data = nullptr;
  napi_value deserialize_info = nullptr;
  if (!PrepareJSTransferableCloneData(env, value, &prepared_data, &deserialize_info) ||
      prepared_data == nullptr ||
      deserialize_info == nullptr) {
    return nullptr;
  }

  napi_value marker = nullptr;
  if (napi_create_object(env, &marker) != napi_ok || marker == nullptr) return nullptr;

  napi_value true_value = nullptr;
  if (napi_get_boolean(env, true, &true_value) != napi_ok || true_value == nullptr ||
      napi_set_named_property(env, marker, "__ubiJSTransferableCloneMarker", true_value) != napi_ok ||
      napi_set_named_property(env, marker, "data", prepared_data) != napi_ok ||
      napi_set_named_property(env, marker, "deserializeInfo", deserialize_info) != napi_ok) {
    return nullptr;
  }

  return marker;
}

napi_value CloneRootJSTransferableValueForQueue(napi_env env, napi_value value) {
  napi_value marker = CreateJSTransferableCloneMarker(env, value);
  if (marker == nullptr) return nullptr;

  napi_value cloned = nullptr;
  if (unofficial_napi_structured_clone(env, marker, &cloned) != napi_ok || cloned == nullptr) {
    return nullptr;
  }

  return cloned;
}

bool TransferRootJSTransferableValueForQueue(
    napi_env env,
    napi_value value,
    napi_value* cloned_out,
    std::vector<QueuedMessage::TransferredPortEntry>* transferred_ports_out) {
  if (cloned_out != nullptr) *cloned_out = nullptr;
  if (transferred_ports_out != nullptr) transferred_ports_out->clear();

  napi_value transfer_data = nullptr;
  napi_value deserialize_info = nullptr;
  napi_value nested_transfer_list = nullptr;
  if (!PrepareJSTransferableTransferData(
          env, value, &transfer_data, &deserialize_info, &nested_transfer_list) ||
      transfer_data == nullptr ||
      deserialize_info == nullptr) {
    return false;
  }

  if (transferred_ports_out != nullptr &&
      !CollectTransferredPorts(env, nested_transfer_list, transferred_ports_out)) {
    return false;
  }

  std::vector<ValueTransformPair> seen_pairs;
  const std::vector<QueuedMessage::TransferredPortEntry> empty_transferred_ports;
  const auto& transferred_ports =
      transferred_ports_out != nullptr ? *transferred_ports_out : empty_transferred_ports;
  napi_value transformed_data =
      TransformTransferredPortsForQueue(env, transfer_data, transferred_ports, &seen_pairs);
  if (transformed_data == nullptr) return false;

  napi_value prepared_data = PrepareTransferableDataForStructuredClone(env, transformed_data);
  if (prepared_data == nullptr) return false;

  napi_value marker = nullptr;
  if (napi_create_object(env, &marker) != napi_ok || marker == nullptr) return false;
  napi_value true_value = nullptr;
  if (napi_get_boolean(env, true, &true_value) != napi_ok || true_value == nullptr ||
      napi_set_named_property(env, marker, "__ubiJSTransferableCloneMarker", true_value) != napi_ok ||
      napi_set_named_property(env, marker, "data", prepared_data) != napi_ok ||
      napi_set_named_property(env, marker, "deserializeInfo", deserialize_info) != napi_ok) {
    return false;
  }

  napi_value cloned = nullptr;
  if (unofficial_napi_structured_clone(env, marker, &cloned) != napi_ok || cloned == nullptr) {
    return false;
  }

  if (cloned_out != nullptr) *cloned_out = cloned;
  return true;
}

napi_value StructuredCloneJSTransferableValue(napi_env env, napi_value value) {
  napi_value prepared_data = nullptr;
  napi_value deserialize_info = nullptr;
  if (!PrepareJSTransferableCloneData(env, value, &prepared_data, &deserialize_info) ||
      prepared_data == nullptr ||
      deserialize_info == nullptr) {
    return nullptr;
  }

  napi_value cloned_data = nullptr;
  if (unofficial_napi_structured_clone(env, prepared_data, &cloned_data) != napi_ok || cloned_data == nullptr) {
    return nullptr;
  }

  cloned_data = RestoreTransferableDataAfterStructuredClone(env, cloned_data);
  if (cloned_data == nullptr) return nullptr;

  return DeserializeJSTransferableCloneMarker(env, cloned_data, deserialize_info);
}

napi_value TryRequireModule(napi_env env, const char* module_name) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;
  napi_value require_fn = UbiGetRequireFunction(env);
  if (!IsFunction(env, require_fn)) {
    require_fn = GetNamed(env, global, "require");
  }
  if (!IsFunction(env, require_fn)) return nullptr;

  napi_value module_name_v = nullptr;
  if (napi_create_string_utf8(env, module_name, NAPI_AUTO_LENGTH, &module_name_v) != napi_ok ||
      module_name_v == nullptr) {
    return nullptr;
  }

  napi_value out = nullptr;
  napi_value argv[1] = {module_name_v};
  if (napi_call_function(env, global, require_fn, 1, argv, &out) != napi_ok || out == nullptr) {
    ClearPendingException(env);
    return nullptr;
  }
  return out;
}

napi_value GetUntransferableObjectPrivateSymbol(napi_env env) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value internal_binding = UbiGetInternalBinding(env);
  if (!IsFunction(env, internal_binding)) {
    internal_binding = GetNamed(env, global, "internalBinding");
  }
  if (!IsFunction(env, internal_binding)) return nullptr;

  napi_value util_name = nullptr;
  if (napi_create_string_utf8(env, "util", NAPI_AUTO_LENGTH, &util_name) != napi_ok || util_name == nullptr) {
    return nullptr;
  }

  napi_value util_binding = nullptr;
  napi_value argv[1] = {util_name};
  if (napi_call_function(env, global, internal_binding, 1, argv, &util_binding) != napi_ok || util_binding == nullptr) {
    ClearPendingException(env);
    return nullptr;
  }

  napi_value private_symbols = GetNamed(env, util_binding, "privateSymbols");
  if (private_symbols == nullptr) return nullptr;
  return GetNamed(env, private_symbols, "untransferable_object_private_symbol");
}

bool TransferListContainsMarkedUntransferable(napi_env env, napi_value transfer_list) {
  if (transfer_list == nullptr || IsUndefinedValue(env, transfer_list)) return false;

  bool is_array = false;
  if (napi_is_array(env, transfer_list, &is_array) != napi_ok || !is_array) return false;

  napi_value marker = GetUntransferableObjectPrivateSymbol(env);
  if (marker == nullptr || IsUndefinedValue(env, marker)) return false;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer_list, &length) != napi_ok || length == 0) return false;

  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer_list, i, &item) != napi_ok || item == nullptr) continue;

    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, item, &type) != napi_ok || (type != napi_object && type != napi_function)) continue;

    bool has_marker = false;
    if (napi_has_property(env, item, marker, &has_marker) == napi_ok && has_marker) {
      napi_value marker_value = nullptr;
      if (napi_get_property(env, item, marker, &marker_value) == napi_ok && !IsUndefinedValue(env, marker_value)) {
        return true;
      }
    }
  }

  return false;
}

bool IsMessagePortValue(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_object) return false;
  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return false;
  napi_value ctor = GetRefValue(env, it->second.message_port_ctor_ref);
  if (!IsFunction(env, ctor)) return false;
  bool is_instance = false;
  if (napi_instanceof(env, value, ctor, &is_instance) != napi_ok || !is_instance) {
    ClearPendingException(env);
    return false;
  }
  return UnwrapMessagePort(env, value) != nullptr;
}

bool TransferListContainsValue(napi_env env, napi_value transfer_list, napi_value candidate) {
  if (transfer_list == nullptr || candidate == nullptr) return false;
  bool is_array = false;
  if (napi_is_array(env, transfer_list, &is_array) != napi_ok || !is_array) return false;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer_list, &length) != napi_ok || length == 0) return false;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer_list, i, &item) != napi_ok || item == nullptr) continue;
    bool same = false;
    if (napi_strict_equals(env, item, candidate, &same) == napi_ok && same) {
      return true;
    }
  }
  return false;
}

bool TransferListContainsMessagePort(napi_env env, napi_value transfer_list, napi_value candidate) {
  return TransferListContainsValue(env, transfer_list, candidate);
}

bool TransferListContainsDuplicateMessagePort(napi_env env, napi_value transfer_list) {
  if (transfer_list == nullptr) return false;
  bool is_array = false;
  if (napi_is_array(env, transfer_list, &is_array) != napi_ok || !is_array) return false;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer_list, &length) != napi_ok || length < 2) return false;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value first = nullptr;
    if (napi_get_element(env, transfer_list, i, &first) != napi_ok || first == nullptr ||
        !IsMessagePortValue(env, first)) {
      continue;
    }
    for (uint32_t j = i + 1; j < length; ++j) {
      napi_value second = nullptr;
      if (napi_get_element(env, transfer_list, j, &second) != napi_ok || second == nullptr) continue;
      bool same = false;
      if (napi_strict_equals(env, first, second, &same) == napi_ok && same) {
        return true;
      }
    }
  }
  return false;
}

bool ValidateTransferListMessagePorts(napi_env env, napi_value transfer_list) {
  if (transfer_list == nullptr) return true;
  bool is_array = false;
  if (napi_is_array(env, transfer_list, &is_array) != napi_ok || !is_array) return true;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer_list, &length) != napi_ok || length == 0) return true;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer_list, i, &item) != napi_ok || item == nullptr) continue;
    MessagePortWrap* wrap = UnwrapMessagePort(env, item);
    if (wrap == nullptr) continue;

    bool detached = wrap->handle_wrap.state != kUbiHandleInitialized || !wrap->data;
    if (!detached) {
      std::lock_guard<std::mutex> lock(wrap->data->mutex);
      detached = wrap->data->closed || wrap->data->sibling.expired();
    }
    if (!detached) continue;

    napi_value err = CreateDataCloneError(env, "MessagePort in transfer list is already detached");
    if (err != nullptr) napi_throw(env, err);
    return false;
  }
  return true;
}

napi_value CreateVisitedSet(napi_env env) {
  napi_value global = GetGlobal(env);
  napi_value set_ctor = GetNamed(env, global, "Set");
  if (!IsFunction(env, set_ctor)) return nullptr;
  napi_value set = nullptr;
  if (napi_new_instance(env, set_ctor, 0, nullptr, &set) != napi_ok || set == nullptr) return nullptr;
  return set;
}

bool VisitedSetHas(napi_env env, napi_value visited, napi_value value) {
  if (visited == nullptr || value == nullptr) return false;
  napi_value has_fn = GetNamed(env, visited, "has");
  if (!IsFunction(env, has_fn)) return false;
  napi_value result = nullptr;
  napi_value argv[1] = {value};
  if (napi_call_function(env, visited, has_fn, 1, argv, &result) != napi_ok || result == nullptr) return false;
  bool has = false;
  (void)napi_get_value_bool(env, result, &has);
  return has;
}

void VisitedSetAdd(napi_env env, napi_value visited, napi_value value) {
  if (visited == nullptr || value == nullptr) return;
  napi_value add_fn = GetNamed(env, visited, "add");
  if (!IsFunction(env, add_fn)) return;
  napi_value ignored = nullptr;
  napi_value argv[1] = {value};
  (void)napi_call_function(env, visited, add_fn, 1, argv, &ignored);
}

bool ValueRequiresMessagePortTransfer(napi_env env,
                                      napi_value value,
                                      napi_value transfer_list,
                                      napi_value visited) {
  if (value == nullptr || IsNullOrUndefinedValue(env, value)) return false;

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok) return false;
  if (type != napi_object && type != napi_function) return false;

  if (VisitedSetHas(env, visited, value)) return false;
  VisitedSetAdd(env, visited, value);

  if (IsMessagePortValue(env, value)) {
    return !TransferListContainsMessagePort(env, transfer_list, value);
  }

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) return false;

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) return false;

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) return false;

  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) return false;

  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    uint32_t length = 0;
    if (napi_get_array_length(env, value, &length) != napi_ok) return false;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value item = nullptr;
      if (napi_get_element(env, value, i, &item) != napi_ok || item == nullptr) continue;
      if (ValueRequiresMessagePortTransfer(env, item, transfer_list, visited)) return true;
    }
    return false;
  }

  napi_value keys = nullptr;
  if (unofficial_napi_get_own_non_index_properties(env, value, napi_key_all_properties, &keys) != napi_ok ||
      keys == nullptr) {
    return false;
  }
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return false;
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    napi_value child = nullptr;
    if (napi_get_property(env, value, key, &child) != napi_ok || child == nullptr) continue;
    if (ValueRequiresMessagePortTransfer(env, child, transfer_list, visited)) return true;
  }
  return false;
}

bool EnsureNoMissingTransferredMessagePorts(napi_env env, napi_value value, napi_value transfer_arg) {
  napi_value transfer_list = GetTransferListValue(env, transfer_arg);
  napi_value visited = CreateVisitedSet(env);
  if (visited == nullptr) return true;
  if (!ValueRequiresMessagePortTransfer(env, value, transfer_list, visited)) return true;

  napi_value err =
      CreateDataCloneError(env, "Object that needs transfer was found in message but not listed in transferList");
  if (err != nullptr) napi_throw(env, err);
  return false;
}

napi_value GetNoMessageSymbol(napi_env env) {
  const auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return nullptr;
  return GetRefValue(env, it->second.no_message_symbol_ref);
}

napi_value GetOnInitSymbol(napi_env env) {
  const auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return nullptr;
  return GetRefValue(env, it->second.oninit_symbol_ref);
}

MessagePortWrap* UnwrapMessagePort(napi_env env, napi_value value) {
  MessagePortWrap* wrap = nullptr;
  if (value == nullptr) return nullptr;
  if (napi_unwrap(env, value, reinterpret_cast<void**>(&wrap)) != napi_ok || wrap == nullptr) return nullptr;
  return wrap;
}

std::shared_ptr<BroadcastChannelGroup> GetOrCreateBroadcastChannelGroup(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_broadcast_groups_mutex);
  auto it = g_broadcast_groups.find(name);
  if (it != g_broadcast_groups.end()) {
    if (auto existing = it->second.lock()) {
      return existing;
    }
  }

  auto group = std::make_shared<BroadcastChannelGroup>(name);
  g_broadcast_groups[name] = group;
  return group;
}

void AttachToBroadcastChannelGroup(const UbiMessagePortDataPtr& data,
                                   const std::shared_ptr<BroadcastChannelGroup>& group) {
  if (!data || !group) return;

  {
    std::lock_guard<std::mutex> lock(data->mutex);
    data->broadcast_group = group;
    data->sibling.reset();
    data->closed = false;
    data->close_message_enqueued = false;
  }

  std::lock_guard<std::mutex> group_lock(group->mutex);
  group->members.erase(
      std::remove_if(
          group->members.begin(),
          group->members.end(),
          [&](const std::weak_ptr<UbiMessagePortData>& entry) {
            auto member = entry.lock();
            return !member || member.get() == data.get();
          }),
      group->members.end());
  group->members.push_back(data);
}

void RemoveFromBroadcastChannelGroup(const UbiMessagePortDataPtr& data) {
  if (!data) return;

  std::shared_ptr<BroadcastChannelGroup> group;
  {
    std::lock_guard<std::mutex> lock(data->mutex);
    group = data->broadcast_group;
    data->broadcast_group.reset();
  }
  if (!group) return;

  std::lock_guard<std::mutex> group_lock(group->mutex);
  group->members.erase(
      std::remove_if(
          group->members.begin(),
          group->members.end(),
          [&](const std::weak_ptr<UbiMessagePortData>& entry) {
            auto member = entry.lock();
            return !member || member.get() == data.get();
          }),
      group->members.end());
}

std::vector<UbiMessagePortDataPtr> GetBroadcastChannelTargets(const UbiMessagePortDataPtr& source) {
  std::vector<UbiMessagePortDataPtr> targets;
  if (!source) return targets;

  std::shared_ptr<BroadcastChannelGroup> group;
  {
    std::lock_guard<std::mutex> lock(source->mutex);
    group = source->broadcast_group;
  }
  if (!group) return targets;

  std::lock_guard<std::mutex> group_lock(group->mutex);
  group->members.erase(
      std::remove_if(
          group->members.begin(),
          group->members.end(),
          [&](const std::weak_ptr<UbiMessagePortData>& entry) {
            auto member = entry.lock();
            if (!member) return true;
            if (member.get() == source.get()) return false;

            bool closed = false;
            {
              std::lock_guard<std::mutex> member_lock(member->mutex);
              closed = member->closed;
            }
            if (!closed) targets.push_back(member);
            return false;
          }),
      group->members.end());
  return targets;
}

UbiMessagePortDataPtr InternalCreateMessagePortData() {
  return std::make_shared<UbiMessagePortData>();
}

void InternalEntangleMessagePortData(const UbiMessagePortDataPtr& first,
                                     const UbiMessagePortDataPtr& second) {
  if (!first || !second || first.get() == second.get()) return;
  {
    std::lock_guard<std::mutex> first_lock(first->mutex);
    first->closed = false;
    first->close_message_enqueued = false;
    first->broadcast_group.reset();
    first->sibling = second;
  }
  {
    std::lock_guard<std::mutex> second_lock(second->mutex);
    second->closed = false;
    second->close_message_enqueued = false;
    second->broadcast_group.reset();
    second->sibling = first;
  }
}

bool MessagePortHasRefActive(void* data) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  if (wrap == nullptr) return false;
  if (wrap->handle_wrap.state == kUbiHandleClosing) return wrap->closing_has_ref;
  if (wrap->handle_wrap.state != kUbiHandleInitialized) return false;
  return UbiHandleWrapHasRef(&wrap->handle_wrap, reinterpret_cast<const uv_handle_t*>(&wrap->async));
}

napi_value MessagePortGetActiveOwner(napi_env env, void* data) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  return wrap != nullptr ? UbiHandleWrapGetActiveOwner(env, wrap->handle_wrap.wrapper_ref) : nullptr;
}

void DeleteQueuedMessages(napi_env env, MessagePortWrap* wrap) {
  if (wrap == nullptr) return;
  std::deque<QueuedMessage> queued;
  {
    if (!wrap->data) return;
    std::lock_guard<std::mutex> lock(wrap->data->mutex);
    queued.swap(wrap->data->queued_messages);
    wrap->data->close_message_enqueued = false;
  }
  for (auto& entry : queued) {
    if (entry.payload_data != nullptr) {
      unofficial_napi_release_serialized_value(entry.payload_data);
      entry.payload_data = nullptr;
    }
    for (auto& port_entry : entry.transferred_ports) {
      DeleteRefIfPresent(env, &port_entry.source_port_ref);
    }
  }
}

void TriggerPortAsync(MessagePortWrap* wrap) {
  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized ||
      uv_is_closing(reinterpret_cast<uv_handle_t*>(&wrap->async))) {
    return;
  }
  uv_async_send(&wrap->async);
}

void TriggerPortAsync(const UbiMessagePortDataPtr& data) {
  if (!data) return;
  MessagePortWrap* wrap = nullptr;
  {
    std::lock_guard<std::mutex> lock(data->mutex);
    wrap = data->attached_wrap;
  }
  TriggerPortAsync(wrap);
}

napi_value CreateStructuredCloneOptions(napi_env env, napi_value value) {
  if (value == nullptr || IsUndefinedValue(env, value) || IsNullOrUndefinedValue(env, value)) return nullptr;
  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    napi_value options = nullptr;
    if (napi_create_object(env, &options) != napi_ok || options == nullptr) return nullptr;
    napi_set_named_property(env, options, "transfer", value);
    return options;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) == napi_ok && type == napi_object) {
    bool has_transfer = false;
    if (napi_has_named_property(env, value, "transfer", &has_transfer) == napi_ok && has_transfer) {
      return value;
    }
    return nullptr;
  }

  napi_value options = nullptr;
  if (napi_create_object(env, &options) != napi_ok || options == nullptr) return nullptr;
  napi_set_named_property(env, options, "transfer", value);
  return options;
}

napi_value GetTransferListValue(napi_env env, napi_value value) {
  if (value == nullptr || IsUndefinedValue(env, value) || IsNullOrUndefinedValue(env, value)) return nullptr;
  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) return value;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) == napi_ok && type == napi_object) {
    bool has_transfer = false;
    if (napi_has_named_property(env, value, "transfer", &has_transfer) != napi_ok || !has_transfer) {
      return nullptr;
    }
    napi_value transfer = GetNamed(env, value, "transfer");
    if (transfer != nullptr && !IsUndefinedValue(env, transfer) && !IsNullOrUndefinedValue(env, transfer)) {
      return transfer;
    }
    return nullptr;
  }
  return value;
}

bool CoerceTransferIterable(napi_env env,
                            napi_value value,
                            const char* error_message,
                            napi_value* out) {
  if (out != nullptr) *out = nullptr;
  if (value == nullptr || IsUndefinedValue(env, value)) return true;

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || (type != napi_object && type != napi_function)) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", error_message);
    return false;
  }

  napi_value global = GetGlobal(env);
  napi_value symbol_ctor = GetNamed(env, global, "Symbol");
  napi_value iterator_symbol = GetNamed(env, symbol_ctor, "iterator");
  napi_value iterator_fn = nullptr;
  if (iterator_symbol == nullptr ||
      napi_get_property(env, value, iterator_symbol, &iterator_fn) != napi_ok ||
      !IsFunction(env, iterator_fn)) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", error_message);
    return false;
  }

  napi_value array_ctor = GetNamed(env, global, "Array");
  napi_value from_fn = GetNamed(env, array_ctor, "from");
  if (!IsFunction(env, from_fn)) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", error_message);
    return false;
  }

  napi_value argv[1] = {value};
  napi_value result = nullptr;
  if (napi_call_function(env, array_ctor, from_fn, 1, argv, &result) != napi_ok || result == nullptr) {
    ClearPendingException(env);
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", error_message);
    return false;
  }

  if (out != nullptr) *out = result;
  return true;
}

bool HasCallableIterator(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || (type != napi_object && type != napi_function)) {
    return false;
  }
  napi_value global = GetGlobal(env);
  napi_value symbol_ctor = GetNamed(env, global, "Symbol");
  napi_value iterator_symbol = GetNamed(env, symbol_ctor, "iterator");
  napi_value iterator_fn = nullptr;
  return iterator_symbol != nullptr &&
         napi_get_property(env, value, iterator_symbol, &iterator_fn) == napi_ok &&
         IsFunction(env, iterator_fn);
}

bool NormalizePostMessageTransferArg(napi_env env,
                                     napi_value arg,
                                     napi_value* normalized_arg,
                                     napi_value* transfer_list) {
  if (normalized_arg != nullptr) *normalized_arg = nullptr;
  if (transfer_list != nullptr) *transfer_list = nullptr;
  if (arg == nullptr || IsUndefinedValue(env, arg) || IsNullOrUndefinedValue(env, arg)) {
    return true;
  }

  bool is_array = false;
  if (napi_is_array(env, arg, &is_array) == napi_ok && is_array) {
    if (normalized_arg != nullptr) *normalized_arg = arg;
    if (transfer_list != nullptr) *transfer_list = arg;
    return true;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, arg, &type) == napi_ok && type == napi_object) {
    bool has_transfer = false;
    if (napi_has_named_property(env, arg, "transfer", &has_transfer) == napi_ok && has_transfer) {
      napi_value transfer = GetNamed(env, arg, "transfer");
      napi_value normalized_transfer = nullptr;
      if (transfer != nullptr && !IsUndefinedValue(env, transfer)) {
        if (!CoerceTransferIterable(
                env, transfer, "Optional options.transfer argument must be an iterable", &normalized_transfer)) {
          return false;
        }
      }
      if (normalized_transfer == nullptr) return true;
      napi_value options = nullptr;
      if (napi_create_object(env, &options) != napi_ok || options == nullptr) return false;
      napi_set_named_property(env, options, "transfer", normalized_transfer);
      if (normalized_arg != nullptr) *normalized_arg = options;
      if (transfer_list != nullptr) *transfer_list = normalized_transfer;
      return true;
    }
    if (HasCallableIterator(env, arg)) {
      napi_value normalized_transfer = nullptr;
      if (!CoerceTransferIterable(
              env, arg, "Optional transferList argument must be an iterable", &normalized_transfer)) {
        return false;
      }
      if (normalized_arg != nullptr) *normalized_arg = normalized_transfer;
      if (transfer_list != nullptr) *transfer_list = normalized_transfer;
    }
    return true;
  }

  napi_value normalized_transfer = nullptr;
  if (!CoerceTransferIterable(
          env, arg, "Optional transferList argument must be an iterable", &normalized_transfer)) {
    return false;
  }
  if (normalized_arg != nullptr) *normalized_arg = normalized_transfer;
  if (transfer_list != nullptr) *transfer_list = normalized_transfer;
  return true;
}

napi_value CloneMessageValue(napi_env env, napi_value value, napi_value transfer_arg) {
  napi_value global = GetGlobal(env);
  napi_value structured_clone = GetNamed(env, global, "structuredClone");
  if (!IsFunction(env, structured_clone)) return value;

  napi_value argv[2] = {value, nullptr};
  size_t argc = 1;
  napi_value options = CreateStructuredCloneOptions(env, transfer_arg);
  if (options != nullptr) {
    argv[1] = options;
    argc = 2;
  }

  napi_value cloned = nullptr;
  if (napi_call_function(env, global, structured_clone, argc, argv, &cloned) != napi_ok || cloned == nullptr) {
    return nullptr;
  }
  return cloned;
}

void EmitProcessWarning(napi_env env, const char* message) {
  if (env == nullptr || message == nullptr) return;
  napi_value global = GetGlobal(env);
  napi_value process = GetNamed(env, global, "process");
  napi_value emit_warning = GetNamed(env, process, "emitWarning");
  if (!IsFunction(env, emit_warning)) return;

  napi_value warning = nullptr;
  if (napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &warning) != napi_ok || warning == nullptr) return;

  napi_value ignored = nullptr;
  if (napi_call_function(env, process, emit_warning, 1, &warning, &ignored) != napi_ok) {
    ClearPendingException(env);
  }
}

void ThrowClosedMessagePortError(napi_env env) {
  napi_throw_error(env, "ERR_CLOSED_MESSAGE_PORT", "Cannot send data on closed MessagePort");
}

void OnMessagePortClosed(uv_handle_t* handle);

bool IsCloneByReferenceValue(napi_env env, napi_value value) {
  if (value == nullptr || IsNullOrUndefinedValue(env, value)) return true;
  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) return true;
  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) return true;
  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) return true;
  bool is_typedarray = false;
  if (napi_is_typedarray(env, value, &is_typedarray) == napi_ok && is_typedarray) return true;
  return false;
}

std::string GetObjectTag(napi_env env, napi_value value) {
  napi_value global = GetGlobal(env);
  napi_value object_ctor = GetNamed(env, global, "Object");
  napi_value prototype = GetNamed(env, object_ctor, "prototype");
  napi_value to_string = GetNamed(env, prototype, "toString");
  if (!IsFunction(env, to_string)) return {};
  napi_value out = nullptr;
  if (napi_call_function(env, value, to_string, 0, nullptr, &out) != napi_ok || out == nullptr) {
    ClearPendingException(env);
    return {};
  }
  return ValueToUtf8(env, out);
}

bool IsInstanceOfGlobalCtor(napi_env env, napi_value value, const char* ctor_name) {
  if (value == nullptr || ctor_name == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || (type != napi_object && type != napi_function)) {
    return false;
  }
  napi_value global = GetGlobal(env);
  napi_value ctor = GetNamed(env, global, ctor_name);
  if (!IsFunction(env, ctor)) return false;
  bool is_instance = false;
  return napi_instanceof(env, value, ctor, &is_instance) == napi_ok && is_instance;
}

bool IsMapValue(napi_env env, napi_value value) {
  const std::string tag = GetObjectTag(env, value);
  return tag == "[object Map]" || IsInstanceOfGlobalCtor(env, value, "Map");
}

bool IsSetValue(napi_env env, napi_value value) {
  const std::string tag = GetObjectTag(env, value);
  return tag == "[object Set]" || IsInstanceOfGlobalCtor(env, value, "Set");
}

napi_value CreateGlobalInstance(napi_env env, const char* ctor_name) {
  napi_value global = GetGlobal(env);
  napi_value ctor = GetNamed(env, global, ctor_name);
  if (!IsFunction(env, ctor)) return nullptr;
  napi_value out = nullptr;
  if (napi_new_instance(env, ctor, 0, nullptr, &out) != napi_ok || out == nullptr) {
    ClearPendingException(env);
    return nullptr;
  }
  return out;
}

bool FindTransferredPortIndex(napi_env env,
                              napi_value value,
                              const std::vector<QueuedMessage::TransferredPortEntry>& transferred_ports,
                              uint32_t* index_out) {
  if (index_out != nullptr) *index_out = 0;
  for (uint32_t i = 0; i < transferred_ports.size(); ++i) {
    napi_value source_port = GetRefValue(env, transferred_ports[i].source_port_ref);
    if (source_port == nullptr) continue;
    bool same = false;
    if (napi_strict_equals(env, source_port, value, &same) == napi_ok && same) {
      if (index_out != nullptr) *index_out = i;
      return true;
    }
  }
  return false;
}

napi_value CreateTransferPlaceholder(napi_env env, uint32_t index) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return nullptr;
  SetInt32(env, out, "__ubiMessagePortTransferIndex", static_cast<int32_t>(index));
  return out;
}

bool ReadTransferPlaceholderIndex(napi_env env, napi_value value, uint32_t* index_out) {
  if (index_out != nullptr) *index_out = 0;
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != napi_object) return false;
  bool has_index = false;
  if (napi_has_named_property(env, value, "__ubiMessagePortTransferIndex", &has_index) != napi_ok || !has_index) {
    return false;
  }
  napi_value index_value = GetNamed(env, value, "__ubiMessagePortTransferIndex");
  if (index_value == nullptr) return false;
  int32_t index = 0;
  if (napi_get_value_int32(env, index_value, &index) != napi_ok || index < 0) return false;
  if (index_out != nullptr) *index_out = static_cast<uint32_t>(index);
  return true;
}

napi_value CreateCloneTargetObject(napi_env env, napi_value source) {
  napi_value prototype = nullptr;
  if (napi_get_prototype(env, source, &prototype) != napi_ok || prototype == nullptr) {
    prototype = nullptr;
  }

  napi_value global = GetGlobal(env);
  napi_value object_ctor = GetNamed(env, global, "Object");
  napi_value create_fn = GetNamed(env, object_ctor, "create");
  if (!IsFunction(env, create_fn) || prototype == nullptr) {
    napi_value out = nullptr;
    napi_create_object(env, &out);
    return out;
  }

  napi_value out = nullptr;
  napi_value argv[1] = {prototype};
  if (napi_call_function(env, object_ctor, create_fn, 1, argv, &out) != napi_ok || out == nullptr) {
    ClearPendingException(env);
    napi_create_object(env, &out);
  }
  return out;
}

napi_value FindTransformedValue(napi_env env,
                                napi_value source,
                                const std::vector<ValueTransformPair>& pairs) {
  for (const auto& pair : pairs) {
    bool same = false;
    if (pair.source != nullptr && napi_strict_equals(env, pair.source, source, &same) == napi_ok && same) {
      return pair.target;
    }
  }
  return nullptr;
}

napi_value TransformTransferredPortsForQueue(
    napi_env env,
    napi_value value,
    const std::vector<QueuedMessage::TransferredPortEntry>& transferred_ports,
    std::vector<ValueTransformPair>* seen_pairs) {
  uint32_t transfer_index = 0;
  if (FindTransferredPortIndex(env, value, transferred_ports, &transfer_index)) {
    return CreateTransferPlaceholder(env, transfer_index);
  }

  if (value == nullptr || IsNullOrUndefinedValue(env, value) || IsCloneByReferenceValue(env, value)) {
    return value;
  }
  if (IsCloneableTransferableValue(env, value)) {
    return value;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || (type != napi_object && type != napi_function)) {
    return value;
  }

  if (seen_pairs != nullptr) {
    napi_value existing = FindTransformedValue(env, value, *seen_pairs);
    if (existing != nullptr) return existing;
  }

  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    uint32_t length = 0;
    if (napi_get_array_length(env, value, &length) != napi_ok) return value;
    napi_value out = nullptr;
    if (napi_create_array_with_length(env, length, &out) != napi_ok || out == nullptr) return value;
    if (seen_pairs != nullptr) seen_pairs->push_back({value, out});
    for (uint32_t i = 0; i < length; ++i) {
      napi_value item = nullptr;
      if (napi_get_element(env, value, i, &item) != napi_ok || item == nullptr) continue;
      napi_value transformed =
          TransformTransferredPortsForQueue(env, item, transferred_ports, seen_pairs);
      napi_set_element(env, out, i, transformed != nullptr ? transformed : item);
    }
    return out;
  }

  if (IsMapValue(env, value)) {
    napi_value entries = nullptr;
    napi_value global = GetGlobal(env);
    napi_value array_ctor = GetNamed(env, global, "Array");
    napi_value from_fn = GetNamed(env, array_ctor, "from");
    napi_value out = CreateGlobalInstance(env, "Map");
    napi_value set_fn = GetNamed(env, out, "set");
    if (!IsFunction(env, from_fn) || out == nullptr || !IsFunction(env, set_fn)) return value;
    if (seen_pairs != nullptr) seen_pairs->push_back({value, out});
    napi_value argv[1] = {value};
    if (napi_call_function(env, array_ctor, from_fn, 1, argv, &entries) != napi_ok || entries == nullptr) {
      ClearPendingException(env);
      return out;
    }
    uint32_t length = 0;
    if (napi_get_array_length(env, entries, &length) != napi_ok) return out;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value pair = nullptr;
      if (napi_get_element(env, entries, i, &pair) != napi_ok || pair == nullptr) continue;
      napi_value key = nullptr;
      napi_value map_value = nullptr;
      if (napi_get_element(env, pair, 0, &key) != napi_ok || key == nullptr) continue;
      if (napi_get_element(env, pair, 1, &map_value) != napi_ok) map_value = Undefined(env);
      napi_value transformed_key =
          TransformTransferredPortsForQueue(env, key, transferred_ports, seen_pairs);
      napi_value transformed_value =
          TransformTransferredPortsForQueue(env, map_value, transferred_ports, seen_pairs);
      napi_value set_argv[2] = {transformed_key != nullptr ? transformed_key : key,
                                transformed_value != nullptr ? transformed_value : map_value};
      napi_value ignored = nullptr;
      if (napi_call_function(env, out, set_fn, 2, set_argv, &ignored) != napi_ok) {
        ClearPendingException(env);
      }
    }
    return out;
  }

  if (IsSetValue(env, value)) {
    napi_value entries = nullptr;
    napi_value global = GetGlobal(env);
    napi_value array_ctor = GetNamed(env, global, "Array");
    napi_value from_fn = GetNamed(env, array_ctor, "from");
    napi_value out = CreateGlobalInstance(env, "Set");
    napi_value add_fn = GetNamed(env, out, "add");
    if (!IsFunction(env, from_fn) || out == nullptr || !IsFunction(env, add_fn)) return value;
    if (seen_pairs != nullptr) seen_pairs->push_back({value, out});
    napi_value argv[1] = {value};
    if (napi_call_function(env, array_ctor, from_fn, 1, argv, &entries) != napi_ok || entries == nullptr) {
      ClearPendingException(env);
      return out;
    }
    uint32_t length = 0;
    if (napi_get_array_length(env, entries, &length) != napi_ok) return out;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value item = nullptr;
      if (napi_get_element(env, entries, i, &item) != napi_ok || item == nullptr) continue;
      napi_value transformed =
          TransformTransferredPortsForQueue(env, item, transferred_ports, seen_pairs);
      napi_value add_argv[1] = {transformed != nullptr ? transformed : item};
      napi_value ignored = nullptr;
      if (napi_call_function(env, out, add_fn, 1, add_argv, &ignored) != napi_ok) {
        ClearPendingException(env);
      }
    }
    return out;
  }

  napi_value out = CreateCloneTargetObject(env, value);
  if (out == nullptr) return value;
  if (seen_pairs != nullptr) seen_pairs->push_back({value, out});

  napi_value keys = nullptr;
  if (unofficial_napi_get_own_non_index_properties(env, value, napi_key_all_properties, &keys) != napi_ok ||
      keys == nullptr) {
    return out;
  }
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return out;
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    napi_value child = nullptr;
    if (napi_get_property(env, value, key, &child) != napi_ok || child == nullptr) continue;
    napi_value transformed =
        TransformTransferredPortsForQueue(env, child, transferred_ports, seen_pairs);
    napi_set_property(env, out, key, transformed != nullptr ? transformed : child);
  }
  return out;
}

bool CollectTransferredPorts(
    napi_env env,
    napi_value transfer_list,
    std::vector<QueuedMessage::TransferredPortEntry>* out) {
  if (out == nullptr || transfer_list == nullptr) return true;
  bool is_array = false;
  if (napi_is_array(env, transfer_list, &is_array) != napi_ok || !is_array) return true;

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer_list, &length) != napi_ok) return false;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer_list, i, &item) != napi_ok || item == nullptr) continue;
    MessagePortWrap* wrap = UnwrapMessagePort(env, item);
    if (wrap == nullptr || wrap->data == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized) continue;
    QueuedMessage::TransferredPortEntry entry;
    if (napi_create_reference(env, item, 1, &entry.source_port_ref) != napi_ok) return false;
    entry.data = wrap->data;
    out->push_back(std::move(entry));
  }
  return true;
}

bool DetachTransferredPort(napi_env env, MessagePortWrap* wrap) {
  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized || !wrap->data) return false;
  RemoveFromBroadcastChannelGroup(wrap->data);
  {
    std::lock_guard<std::mutex> lock(wrap->data->mutex);
    if (wrap->data->attached_wrap == wrap) {
      wrap->data->attached_wrap = nullptr;
    }
  }
  wrap->closing_has_ref =
      UbiHandleWrapHasRef(&wrap->handle_wrap, reinterpret_cast<const uv_handle_t*>(&wrap->async));
  wrap->data.reset();
  wrap->handle_wrap.state = kUbiHandleClosing;
  uv_close(reinterpret_cast<uv_handle_t*>(&wrap->async), OnMessagePortClosed);
  return true;
}

void DetachTransferredPorts(napi_env env,
                            std::vector<QueuedMessage::TransferredPortEntry>* transferred_ports) {
  if (transferred_ports == nullptr) return;
  for (auto& entry : *transferred_ports) {
    napi_value source_port = GetRefValue(env, entry.source_port_ref);
    MessagePortWrap* transferred_wrap = UnwrapMessagePort(env, source_port);
    if (transferred_wrap != nullptr) {
      DetachTransferredPort(env, transferred_wrap);
    }
    DeleteRefIfPresent(env, &entry.source_port_ref);
  }
}

struct ReceivedTransferredPortState {
  std::vector<napi_value> ports;
};

napi_value GetOrCreateReceivedTransferredPort(napi_env env,
                                              const QueuedMessage& message,
                                              ReceivedTransferredPortState* state,
                                              uint32_t index) {
  if (state == nullptr || index >= message.transferred_ports.size()) return nullptr;
  if (state->ports.size() < message.transferred_ports.size()) {
    state->ports.resize(message.transferred_ports.size(), nullptr);
  }
  if (state->ports[index] != nullptr) return state->ports[index];
  napi_value port = UbiCreateMessagePortForData(env, message.transferred_ports[index].data);
  state->ports[index] = port;
  return port;
}

napi_value RestoreTransferredPortsInValue(napi_env env,
                                          napi_value value,
                                          const QueuedMessage& message,
                                          ReceivedTransferredPortState* state,
                                          std::vector<ValueTransformPair>* seen_pairs) {
  uint32_t placeholder_index = 0;
  if (ReadTransferPlaceholderIndex(env, value, &placeholder_index)) {
    return GetOrCreateReceivedTransferredPort(env, message, state, placeholder_index);
  }

  if (value == nullptr || IsNullOrUndefinedValue(env, value) || IsCloneByReferenceValue(env, value)) {
    return value;
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || (type != napi_object && type != napi_function)) {
    return value;
  }

  if (seen_pairs != nullptr) {
    napi_value existing = FindTransformedValue(env, value, *seen_pairs);
    if (existing != nullptr) return existing;
    seen_pairs->push_back({value, value});
  }

  bool is_array = false;
  if (napi_is_array(env, value, &is_array) == napi_ok && is_array) {
    uint32_t length = 0;
    if (napi_get_array_length(env, value, &length) != napi_ok) return value;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value item = nullptr;
      if (napi_get_element(env, value, i, &item) != napi_ok || item == nullptr) continue;
      napi_value restored = RestoreTransferredPortsInValue(env, item, message, state, seen_pairs);
      if (restored != nullptr) napi_set_element(env, value, i, restored);
    }
    return value;
  }

  if (IsMapValue(env, value)) {
    napi_value entries = nullptr;
    napi_value global = GetGlobal(env);
    napi_value array_ctor = GetNamed(env, global, "Array");
    napi_value from_fn = GetNamed(env, array_ctor, "from");
    napi_value clear_fn = GetNamed(env, value, "clear");
    napi_value set_fn = GetNamed(env, value, "set");
    if (!IsFunction(env, from_fn) || !IsFunction(env, clear_fn) || !IsFunction(env, set_fn)) {
      return value;
    }
    napi_value argv[1] = {value};
    if (napi_call_function(env, array_ctor, from_fn, 1, argv, &entries) != napi_ok || entries == nullptr) {
      ClearPendingException(env);
      return value;
    }
    napi_value ignored = nullptr;
    if (napi_call_function(env, value, clear_fn, 0, nullptr, &ignored) != napi_ok) {
      ClearPendingException(env);
      return value;
    }
    uint32_t length = 0;
    if (napi_get_array_length(env, entries, &length) != napi_ok) return value;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value pair = nullptr;
      if (napi_get_element(env, entries, i, &pair) != napi_ok || pair == nullptr) continue;
      napi_value key = nullptr;
      napi_value map_value = nullptr;
      if (napi_get_element(env, pair, 0, &key) != napi_ok || key == nullptr) continue;
      if (napi_get_element(env, pair, 1, &map_value) != napi_ok) map_value = Undefined(env);
      napi_value restored_key = RestoreTransferredPortsInValue(env, key, message, state, seen_pairs);
      napi_value restored_value = RestoreTransferredPortsInValue(env, map_value, message, state, seen_pairs);
      napi_value set_argv[2] = {restored_key != nullptr ? restored_key : key,
                                restored_value != nullptr ? restored_value : map_value};
      if (napi_call_function(env, value, set_fn, 2, set_argv, &ignored) != napi_ok) {
        ClearPendingException(env);
      }
    }
    return value;
  }

  if (IsSetValue(env, value)) {
    napi_value entries = nullptr;
    napi_value global = GetGlobal(env);
    napi_value array_ctor = GetNamed(env, global, "Array");
    napi_value from_fn = GetNamed(env, array_ctor, "from");
    napi_value clear_fn = GetNamed(env, value, "clear");
    napi_value add_fn = GetNamed(env, value, "add");
    if (!IsFunction(env, from_fn) || !IsFunction(env, clear_fn) || !IsFunction(env, add_fn)) {
      return value;
    }
    napi_value argv[1] = {value};
    if (napi_call_function(env, array_ctor, from_fn, 1, argv, &entries) != napi_ok || entries == nullptr) {
      ClearPendingException(env);
      return value;
    }
    napi_value ignored = nullptr;
    if (napi_call_function(env, value, clear_fn, 0, nullptr, &ignored) != napi_ok) {
      ClearPendingException(env);
      return value;
    }
    uint32_t length = 0;
    if (napi_get_array_length(env, entries, &length) != napi_ok) return value;
    for (uint32_t i = 0; i < length; ++i) {
      napi_value item = nullptr;
      if (napi_get_element(env, entries, i, &item) != napi_ok || item == nullptr) continue;
      napi_value restored = RestoreTransferredPortsInValue(env, item, message, state, seen_pairs);
      napi_value add_argv[1] = {restored != nullptr ? restored : item};
      if (napi_call_function(env, value, add_fn, 1, add_argv, &ignored) != napi_ok) {
        ClearPendingException(env);
      }
    }
    return value;
  }

  napi_value keys = nullptr;
  if (unofficial_napi_get_own_non_index_properties(env, value, napi_key_all_properties, &keys) != napi_ok ||
      keys == nullptr) {
    return value;
  }
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return value;
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    napi_value child = nullptr;
    if (napi_get_property(env, value, key, &child) != napi_ok || child == nullptr) continue;
    napi_value restored = RestoreTransferredPortsInValue(env, child, message, state, seen_pairs);
    if (restored != nullptr) napi_set_property(env, value, key, restored);
  }
  return value;
}

napi_value BuildTransferredPortsArray(napi_env env,
                                      const QueuedMessage& message,
                                      ReceivedTransferredPortState* state) {
  napi_value out = nullptr;
  if (napi_create_array_with_length(env, message.transferred_ports.size(), &out) != napi_ok || out == nullptr) {
    return nullptr;
  }
  for (uint32_t i = 0; i < message.transferred_ports.size(); ++i) {
    napi_value port = GetOrCreateReceivedTransferredPort(env, message, state, i);
    napi_set_element(env, out, i, port != nullptr ? port : Undefined(env));
  }
  return out;
}

void EnqueueMessageToPort(napi_env env,
                          const UbiMessagePortDataPtr& target,
                          napi_value payload,
                          bool is_close,
                          MessagePortWrap* close_source_wrap = nullptr,
                          std::vector<QueuedMessage::TransferredPortEntry> transferred_ports = {}) {
  if (!target) return;
  QueuedMessage queued;
  queued.is_close = is_close;
  queued.close_source_wrap = close_source_wrap;
  queued.transferred_ports = std::move(transferred_ports);
  if (!is_close && payload != nullptr) {
    if (unofficial_napi_serialize_value(env, payload, &queued.payload_data) != napi_ok || queued.payload_data == nullptr) {
      for (auto& entry : queued.transferred_ports) {
        DeleteRefIfPresent(env, &entry.source_port_ref);
      }
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(target->mutex);
    if (is_close) {
      if (target->closed || target->close_message_enqueued) {
        if (queued.payload_data != nullptr) {
          unofficial_napi_release_serialized_value(queued.payload_data);
          queued.payload_data = nullptr;
        }
        return;
      }
      target->close_message_enqueued = true;
    }
    if (target->closed) {
      if (queued.payload_data != nullptr) {
        unofficial_napi_release_serialized_value(queued.payload_data);
        queued.payload_data = nullptr;
      }
      for (auto& entry : queued.transferred_ports) {
        DeleteRefIfPresent(env, &entry.source_port_ref);
      }
      return;
    }
    target->queued_messages.push_back(queued);
  }
  TriggerPortAsync(target);
}

void BeginClosePort(napi_env env, MessagePortWrap* wrap, bool notify_peer);
void OnMessagePortClosed(uv_handle_t* handle);
void EmitMessageToPort(napi_env env,
                       napi_value port,
                       napi_value payload,
                       const char* type = "message",
                       napi_value ports = nullptr);

void ProcessQueuedMessages(MessagePortWrap* wrap, bool force) {
  if (wrap == nullptr || wrap->handle_wrap.env == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized) {
    return;
  }

  for (;;) {
    QueuedMessage next;
    bool have_message = false;
    {
      if (!wrap->data) break;
      std::lock_guard<std::mutex> lock(wrap->data->mutex);
      if (wrap->data->queued_messages.empty()) break;
      if (!force && !wrap->receiving_messages && !wrap->data->queued_messages.front().is_close) break;
      next = wrap->data->queued_messages.front();
      wrap->data->queued_messages.pop_front();
      if (next.is_close) wrap->data->close_message_enqueued = false;
      have_message = true;
    }
    if (!have_message) break;

    if (next.is_close) {
      if (next.close_source_wrap != nullptr) {
        next.close_source_wrap->closing_has_ref = false;
      }
      wrap->closing_has_ref = false;
      if (next.payload_data != nullptr) {
        unofficial_napi_release_serialized_value(next.payload_data);
        next.payload_data = nullptr;
      }
      BeginClosePort(wrap->handle_wrap.env, wrap, false);
      break;
    }

    napi_value self = UbiHandleWrapGetRefValue(wrap->handle_wrap.env, wrap->handle_wrap.wrapper_ref);
    napi_value payload = nullptr;
    napi_value message_error = nullptr;
    if (next.payload_data != nullptr) {
      if (unofficial_napi_deserialize_value(wrap->handle_wrap.env, next.payload_data, &payload) != napi_ok) {
        payload = nullptr;
        message_error = TakePendingException(wrap->handle_wrap.env);
        if (message_error == nullptr) {
          message_error = CreateErrorWithMessage(wrap->handle_wrap.env, nullptr, "Message could not be deserialized");
        }
      }
      unofficial_napi_release_serialized_value(next.payload_data);
      next.payload_data = nullptr;
    }
    if (self != nullptr && message_error == nullptr) {
      ReceivedTransferredPortState received_ports;
      std::vector<ValueTransformPair> seen_pairs;
      payload = RestoreTransferredPortsInValue(
          wrap->handle_wrap.env,
          payload != nullptr ? payload : Undefined(wrap->handle_wrap.env),
          next,
          &received_ports,
          &seen_pairs);
      message_error = TakePendingException(wrap->handle_wrap.env);
      if (message_error == nullptr) {
        payload = RestoreTransferableDataAfterStructuredClone(
            wrap->handle_wrap.env,
            payload != nullptr ? payload : Undefined(wrap->handle_wrap.env));
        message_error = TakePendingException(wrap->handle_wrap.env);
      }
      if (message_error == nullptr) {
        napi_value ports = BuildTransferredPortsArray(wrap->handle_wrap.env, next, &received_ports);
        DeleteTransferredPortRefs(wrap->handle_wrap.env, &next.transferred_ports);
        EmitMessageToPort(wrap->handle_wrap.env,
                          self,
                          payload != nullptr ? payload : Undefined(wrap->handle_wrap.env),
                          "message",
                          ports);
        continue;
      }
    }
    DeleteTransferredPortRefs(wrap->handle_wrap.env, &next.transferred_ports);
    if (self != nullptr) {
      EmitMessageToPort(wrap->handle_wrap.env,
                        self,
                        message_error != nullptr ? message_error : Undefined(wrap->handle_wrap.env),
                        "messageerror");
    }
  }
}

void OnMessagePortClosed(uv_handle_t* handle) {
  auto* wrap = static_cast<MessagePortWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  wrap->handle_wrap.state = kUbiHandleClosed;
  UbiHandleWrapReleaseWrapperRef(&wrap->handle_wrap);
  UbiHandleWrapMaybeCallOnClose(&wrap->handle_wrap);
  if (wrap->handle_wrap.active_handle_token != nullptr) {
    UbiUnregisterActiveHandle(wrap->handle_wrap.env, wrap->handle_wrap.active_handle_token);
    wrap->handle_wrap.active_handle_token = nullptr;
  }
  if (wrap->data) {
    std::lock_guard<std::mutex> lock(wrap->data->mutex);
    if (wrap->data->attached_wrap == wrap) {
      wrap->data->attached_wrap = nullptr;
    }
  }
  if (wrap->handle_wrap.finalized || wrap->handle_wrap.delete_on_close) {
    if (wrap->async_id > 0) {
      UbiAsyncWrapQueueDestroyId(wrap->handle_wrap.env, wrap->async_id);
      wrap->async_id = 0;
    }
    DeleteQueuedMessages(wrap->handle_wrap.env, wrap);
    UbiHandleWrapDeleteRefIfPresent(wrap->handle_wrap.env, &wrap->handle_wrap.wrapper_ref);
    delete wrap;
    return;
  }
  if (wrap->async_id > 0) {
    UbiAsyncWrapQueueDestroyId(wrap->handle_wrap.env, wrap->async_id);
    wrap->async_id = 0;
  }
}

void OnMessagePortAsync(uv_async_t* handle) {
  auto* wrap = static_cast<MessagePortWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  ProcessQueuedMessages(wrap, false);
}

void DisentanglePeer(napi_env env, MessagePortWrap* wrap, bool enqueue_close) {
  if (wrap == nullptr) return;
  RemoveFromBroadcastChannelGroup(wrap->data);
  UbiMessagePortDataPtr peer;
  if (wrap->data) {
    std::lock_guard<std::mutex> lock(wrap->data->mutex);
    peer = wrap->data->sibling.lock();
    wrap->data->sibling.reset();
    wrap->data->closed = true;
  }
  if (peer) {
    {
      std::lock_guard<std::mutex> peer_lock(peer->mutex);
      peer->sibling.reset();
    }
    if (enqueue_close) {
      EnqueueMessageToPort(env, peer, nullptr, true, wrap);
    }
  }
}

void BeginClosePort(napi_env env, MessagePortWrap* wrap, bool notify_peer) {
  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized) return;
  if (notify_peer) {
    DisentanglePeer(env, wrap, true);
  }
  wrap->closing_has_ref =
      UbiHandleWrapHasRef(&wrap->handle_wrap, reinterpret_cast<const uv_handle_t*>(&wrap->async));
  wrap->handle_wrap.state = kUbiHandleClosing;
  uv_close(reinterpret_cast<uv_handle_t*>(&wrap->async), OnMessagePortClosed);
}

void MessagePortFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<MessagePortWrap*>(data);
  if (wrap == nullptr) return;
  wrap->handle_wrap.finalized = true;
  UbiHandleWrapDeleteRefIfPresent(env, &wrap->handle_wrap.wrapper_ref);
  if (wrap->handle_wrap.state == kUbiHandleUninitialized || wrap->handle_wrap.state == kUbiHandleClosed) {
    DeleteQueuedMessages(env, wrap);
    if (wrap->handle_wrap.active_handle_token != nullptr) {
      UbiUnregisterActiveHandle(env, wrap->handle_wrap.active_handle_token);
      wrap->handle_wrap.active_handle_token = nullptr;
    }
    delete wrap;
    return;
  }
  wrap->handle_wrap.delete_on_close = true;
  DisentanglePeer(env, wrap, true);
  if (wrap->handle_wrap.state == kUbiHandleInitialized &&
      !uv_is_closing(reinterpret_cast<uv_handle_t*>(&wrap->async))) {
    wrap->handle_wrap.state = kUbiHandleClosing;
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->async), OnMessagePortClosed);
  }
}

void InvokePortSymbolHook(napi_env env, napi_value port, napi_value symbol) {
  if (port == nullptr || symbol == nullptr) return;
  napi_value hook = nullptr;
  if (napi_get_property(env, port, symbol, &hook) != napi_ok || !IsFunction(env, hook)) return;
  napi_value ignored = nullptr;
  if (UbiMakeCallback(env, port, hook, 0, nullptr, &ignored) != napi_ok) {
    ClearPendingException(env);
  }
}

void EmitMessageToPort(napi_env env, napi_value port, napi_value payload, const char* type, napi_value ports) {
  if (port == nullptr) return;

  napi_value emit_message = ResolveEmitMessageValue(env);
  if (IsFunction(env, emit_message)) {
    napi_value type_value = nullptr;
    if (ports == nullptr) napi_create_array_with_length(env, 0, &ports);
    napi_create_string_utf8(env, type != nullptr ? type : "message", NAPI_AUTO_LENGTH, &type_value);
    napi_value ignored = nullptr;
    napi_value argv[3] = {payload != nullptr ? payload : Undefined(env),
                          ports != nullptr ? ports : Undefined(env),
                          type_value != nullptr ? type_value : Undefined(env)};
    if (UbiMakeCallback(env, port, emit_message, 3, argv, &ignored) == napi_ok) {
      return;
    }
    ClearPendingException(env);
  }

  napi_value event = nullptr;
  if (napi_create_object(env, &event) != napi_ok || event == nullptr) return;
  napi_set_named_property(env, event, "data", payload != nullptr ? payload : Undefined(env));

  napi_value dispatch_event = GetNamed(env, port, "dispatchEvent");
  if (IsFunction(env, dispatch_event)) {
    napi_value ignored = nullptr;
    napi_value argv[1] = {event};
    if (UbiMakeCallback(env, port, dispatch_event, 1, argv, &ignored) == napi_ok) return;
    ClearPendingException(env);
  }

  const char* handler_name = (type != nullptr && strcmp(type, "messageerror") == 0)
                                 ? "onmessageerror"
                                 : "onmessage";
  napi_value handler = GetNamed(env, port, handler_name);
  if (IsFunction(env, handler)) {
    napi_value ignored = nullptr;
    napi_value argv[1] = {event};
    if (UbiMakeCallback(env, port, handler, 1, argv, &ignored) != napi_ok) {
      ClearPendingException(env);
    }
  }
}

void ConnectPorts(napi_env env, napi_value first, napi_value second) {
  MessagePortWrap* first_wrap = UnwrapMessagePort(env, first);
  MessagePortWrap* second_wrap = UnwrapMessagePort(env, second);
  if (first_wrap == nullptr || second_wrap == nullptr) return;
  if (!first_wrap->data) first_wrap->data = InternalCreateMessagePortData();
  if (!second_wrap->data) second_wrap->data = InternalCreateMessagePortData();
  InternalEntangleMessagePortData(first_wrap->data, second_wrap->data);
}

bool EnsureMessagingSymbols(napi_env env, const ResolveOptions& options) {
  auto& state = EnsureMessagingState(env);
  if (state.no_message_symbol_ref != nullptr &&
      state.oninit_symbol_ref != nullptr) {
    return true;
  }
  napi_value symbols = nullptr;
  if (options.callbacks.resolve_binding != nullptr) {
    symbols = options.callbacks.resolve_binding(env, options.state, "symbols");
  }
  if (symbols == nullptr || IsUndefined(env, symbols)) {
    napi_value global = GetGlobal(env);
    napi_value internal_binding = UbiGetInternalBinding(env);
    if (!IsFunction(env, internal_binding)) {
      internal_binding = GetNamed(env, global, "internalBinding");
    }
    if (IsFunction(env, internal_binding)) {
      napi_value name = nullptr;
      if (napi_create_string_utf8(env, "symbols", NAPI_AUTO_LENGTH, &name) == napi_ok && name != nullptr) {
        napi_value argv[1] = {name};
        napi_call_function(env, global, internal_binding, 1, argv, &symbols);
      }
    }
  }
  if (symbols == nullptr || IsUndefined(env, symbols)) return false;

  SetRefToValue(env, &state.no_message_symbol_ref, GetNamed(env, symbols, "no_message_symbol"));
  SetRefToValue(env, &state.oninit_symbol_ref, GetNamed(env, symbols, "oninit"));

  return state.no_message_symbol_ref != nullptr;
}

napi_value MessagePortConstructorCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto* wrap = new MessagePortWrap();
  UbiHandleWrapInit(&wrap->handle_wrap, env);
  wrap->data = InternalCreateMessagePortData();
  if (napi_wrap(env, this_arg, wrap, MessagePortFinalize, nullptr, &wrap->handle_wrap.wrapper_ref) != napi_ok) {
    delete wrap;
    return nullptr;
  }

  uv_loop_t* loop = UbiGetEnvLoop(env);
  const int rc = loop != nullptr ? uv_async_init(loop, &wrap->async, OnMessagePortAsync) : UV_EINVAL;
  if (rc == 0) {
    wrap->async.data = wrap;
    wrap->handle_wrap.state = kUbiHandleInitialized;
    UbiHandleWrapHoldWrapperRef(&wrap->handle_wrap);
    wrap->async_id = UbiAsyncWrapNextId(env);
    UbiAsyncWrapEmitInit(
        env, wrap->async_id, kUbiProviderMessagePort, UbiAsyncWrapExecutionAsyncId(env), this_arg);
    {
      std::lock_guard<std::mutex> lock(wrap->data->mutex);
      wrap->data->attached_wrap = wrap;
    }
    wrap->handle_wrap.active_handle_token =
        UbiRegisterActiveHandle(env, this_arg, "MESSAGEPORT", MessagePortHasRefActive, MessagePortGetActiveOwner, wrap);
  }

  const napi_value oninit_symbol = GetOnInitSymbol(env);
  if (rc == 0 && oninit_symbol != nullptr) {
    InvokePortSymbolHook(env, this_arg, oninit_symbol);
  }
  return this_arg;
}

napi_value MessagePortPostMessageCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  napi_value normalized_transfer_arg = nullptr;
  napi_value transfer_list = nullptr;
  if (argc >= 2 && argv[1] != nullptr &&
      !NormalizePostMessageTransferArg(env, argv[1], &normalized_transfer_arg, &transfer_list)) {
    return nullptr;
  }
  if (transfer_list != nullptr && TransferListContainsMarkedUntransferable(env, transfer_list)) {
    napi_value err = CreateDataCloneError(env, "An ArrayBuffer is marked as untransferable");
    if (err != nullptr) {
      napi_throw(env, err);
    }
    return nullptr;
  }
  if (!ValidateTransferListMessagePorts(env, transfer_list)) {
    return nullptr;
  }
  if (TransferListContainsDuplicateMessagePort(env, transfer_list)) {
    napi_value err = CreateDataCloneError(env, "Transfer list contains duplicate MessagePort");
    if (err != nullptr) napi_throw(env, err);
    return nullptr;
  }
  if (TransferListContainsMessagePort(env, transfer_list, this_arg)) {
    napi_value err = CreateDataCloneError(env, "Transfer list contains source port");
    if (err != nullptr) napi_throw(env, err);
    return nullptr;
  }

  napi_value payload = (argc >= 1 && argv[0] != nullptr) ? argv[0] : Undefined(env);
  if (!EnsureNoMissingTransferredMessagePorts(env, payload, normalized_transfer_arg)) {
    return nullptr;
  }

  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  std::vector<QueuedMessage::TransferredPortEntry> transferred_ports;
  napi_value cloned_payload = nullptr;
  if (transfer_list != nullptr &&
      TransferListContainsValue(env, transfer_list, payload) &&
      IsTransferableValue(env, payload)) {
    if (!TransferRootJSTransferableValueForQueue(env, payload, &cloned_payload, &transferred_ports)) {
      DeleteTransferredPortRefs(env, &transferred_ports);
      return nullptr;
    }
  } else {
    if (!CollectTransferredPorts(env, transfer_list, &transferred_ports)) {
      return nullptr;
    }

    std::vector<ValueTransformPair> seen_pairs;
    napi_value transformed_payload =
        TransformTransferredPortsForQueue(env, payload, transferred_ports, &seen_pairs);

    if (IsCloneableTransferableValue(env, transformed_payload)) {
      cloned_payload = CloneRootJSTransferableValueForQueue(env, transformed_payload);
    } else {
      cloned_payload = CloneMessageValue(env, transformed_payload, normalized_transfer_arg);
    }
    if (cloned_payload == nullptr) {
      bool pending = false;
      if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        DeleteTransferredPortRefs(env, &transferred_ports);
        return nullptr;
      }
      cloned_payload = payload;
    }
  }
  if (normalized_transfer_arg != nullptr && !ApplyArrayBufferTransfers(env, normalized_transfer_arg)) {
    DeleteTransferredPortRefs(env, &transferred_ports);
    return nullptr;
  }

  if (wrap == nullptr || wrap->handle_wrap.state != kUbiHandleInitialized || !wrap->data) {
    DetachTransferredPorts(env, &transferred_ports);
    return Undefined(env);
  }

  UbiMessagePortDataPtr peer;
  {
    std::lock_guard<std::mutex> lock(wrap->data->mutex);
    peer = wrap->data->sibling.lock();
  }
  if (peer) {
    bool target_in_transfer_list = false;
    for (const auto& entry : transferred_ports) {
      if (entry.data && entry.data.get() == peer.get()) {
        target_in_transfer_list = true;
        break;
      }
    }
    if (target_in_transfer_list) {
      DetachTransferredPorts(env, &transferred_ports);
      EmitProcessWarning(env, "The target port was posted to itself, and the communication channel was lost");
      BeginClosePort(env, wrap, true);
      napi_value true_value = nullptr;
      napi_get_boolean(env, true, &true_value);
      return true_value;
    }

    DetachTransferredPorts(env, &transferred_ports);
    EnqueueMessageToPort(env, peer, cloned_payload, false, nullptr, std::move(transferred_ports));
    napi_value true_value = nullptr;
    napi_get_boolean(env, true, &true_value);
    return true_value;
  }

  std::vector<UbiMessagePortDataPtr> broadcast_targets = GetBroadcastChannelTargets(wrap->data);
  if (!broadcast_targets.empty()) {
    if (broadcast_targets.size() > 1 && !transferred_ports.empty()) {
      DeleteTransferredPortRefs(env, &transferred_ports);
      napi_value err = CreateDataCloneError(env, "Transferables cannot be used with multiple destinations");
      if (err != nullptr) napi_throw(env, err);
      return nullptr;
    }
    if (!transferred_ports.empty()) {
      DetachTransferredPorts(env, &transferred_ports);
    }
    for (size_t i = 0; i < broadcast_targets.size(); ++i) {
      std::vector<QueuedMessage::TransferredPortEntry> per_target_ports;
      if (i == 0) {
        per_target_ports = std::move(transferred_ports);
      }
      EnqueueMessageToPort(env, broadcast_targets[i], cloned_payload, false, nullptr, std::move(per_target_ports));
    }
    napi_value true_value = nullptr;
    napi_get_boolean(env, true, &true_value);
    return true_value;
  }

  if (!transferred_ports.empty()) {
    DetachTransferredPorts(env, &transferred_ports);
  }
  napi_value false_value = nullptr;
  napi_get_boolean(env, false, &false_value);
  return false_value;
}

napi_value MessagePortStartCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr && wrap->handle_wrap.state == kUbiHandleInitialized) {
    wrap->receiving_messages = true;
    TriggerPortAsync(wrap);
  }
  return Undefined(env);
}

napi_value MessagePortCloseCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  BeginClosePort(env, wrap, true);
  return Undefined(env);
}

napi_value MessagePortRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr && wrap->handle_wrap.state == kUbiHandleInitialized) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->async));
  }
  return this_arg;
}

napi_value MessagePortUnrefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  if (wrap != nullptr && wrap->handle_wrap.state == kUbiHandleInitialized) {
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->async));
  }
  return this_arg;
}

napi_value MessagePortHasRefCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, this_arg);
  napi_value out = nullptr;
  const bool has_ref = wrap != nullptr && MessagePortHasRefActive(wrap);
  napi_get_boolean(env, has_ref, &out);
  return out;
}

napi_value MessageChannelConstructorCallback(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return this_arg;
  napi_value message_port_ctor = GetRefValue(env, it->second.message_port_ctor_ref);
  if (!IsFunction(env, message_port_ctor)) return this_arg;

  napi_value port1 = nullptr;
  napi_value port2 = nullptr;
  if (napi_new_instance(env, message_port_ctor, 0, nullptr, &port1) != napi_ok || port1 == nullptr) {
    return this_arg;
  }
  if (napi_new_instance(env, message_port_ctor, 0, nullptr, &port2) != napi_ok || port2 == nullptr) {
    return this_arg;
  }

  ConnectPorts(env, port1, port2);
  napi_set_named_property(env, this_arg, "port1", port1);
  napi_set_named_property(env, this_arg, "port2", port2);
  return this_arg;
}

napi_value ExposeLazyDOMExceptionPropertyCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  napi_valuetype target_type = napi_undefined;
  if (napi_typeof(env, argv[0], &target_type) != napi_ok || target_type != napi_object) return Undefined(env);

  napi_value global = GetGlobal(env);
  napi_value dom_exception = GetNamed(env, global, "DOMException");
  if (dom_exception == nullptr || IsUndefined(env, dom_exception)) {
    dom_exception = ResolveDOMExceptionValue(env);
  }
  if (dom_exception == nullptr || IsUndefined(env, dom_exception)) return Undefined(env);

  napi_property_descriptor desc = {};
  desc.utf8name = "DOMException";
  desc.value = dom_exception;
  desc.attributes = static_cast<napi_property_attributes>(napi_writable | napi_configurable);
  napi_define_properties(env, argv[0], 1, &desc);
  return Undefined(env);
}

napi_value SetDeserializerCreateObjectFunctionCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;

  auto& state = EnsureMessagingState(env);
  DeleteRefIfPresent(env, &state.deserializer_create_object_ref);
  if (argc >= 1 && IsFunction(env, argv[0])) {
    napi_create_reference(env, argv[0], 1, &state.deserializer_create_object_ref);
  }
  return Undefined(env);
}

bool ApplyArrayBufferTransfers(napi_env env, napi_value options) {
  if (options == nullptr || IsUndefined(env, options)) return true;
  napi_value transfer = options;
  bool is_array = false;
  if (napi_is_array(env, transfer, &is_array) != napi_ok || !is_array) {
    napi_valuetype options_type = napi_undefined;
    if (napi_typeof(env, options, &options_type) != napi_ok || options_type != napi_object) return true;
    transfer = GetNamed(env, options, "transfer");
    if (transfer == nullptr || napi_is_array(env, transfer, &is_array) != napi_ok || !is_array) return true;
  }

  uint32_t length = 0;
  if (napi_get_array_length(env, transfer, &length) != napi_ok) return true;

  for (uint32_t i = 0; i < length; ++i) {
    napi_value item = nullptr;
    if (napi_get_element(env, transfer, i, &item) != napi_ok || item == nullptr) continue;
    bool is_arraybuffer = false;
    if (napi_is_arraybuffer(env, item, &is_arraybuffer) != napi_ok || !is_arraybuffer) continue;
    const napi_status detach_status = napi_detach_arraybuffer(env, item);
    if (detach_status == napi_ok) continue;
    bool already_detached = false;
    if (napi_is_detached_arraybuffer(env, item, &already_detached) == napi_ok && already_detached) continue;
    napi_throw_error(env, "ERR_INVALID_STATE", "Failed to transfer detached ArrayBuffer");
    return false;
  }

  return true;
}

napi_value StructuredCloneCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);

  if (!EnsureNoMissingTransferredMessagePorts(env, argv[0], argc >= 2 ? argv[1] : nullptr)) {
    return nullptr;
  }

  napi_value clone_input = argv[0];
  if (IsProcessEnvValue(env, clone_input)) {
    clone_input = CloneObjectPropertiesForStructuredClone(env, clone_input);
    if (clone_input == nullptr) return nullptr;
  }

  napi_value out = StructuredCloneJSTransferableValue(env, clone_input);
  const napi_status clone_status =
      out != nullptr ? napi_ok : unofficial_napi_structured_clone(env, clone_input, &out);
  if (clone_status != napi_ok || out == nullptr) {
    bool has_pending = false;
    if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) return nullptr;
    napi_value err = CreateDataCloneError(env, "The object could not be cloned.");
    if (err != nullptr) napi_throw(env, err);
    return nullptr;
  }

  if (argc >= 2 && !ApplyArrayBufferTransfers(env, argv[1])) {
    return nullptr;
  }
  return out;
}

napi_value BroadcastChannelCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return Undefined(env);

  const std::string name = (argc >= 1 && argv[0] != nullptr) ? ValueToUtf8(env, argv[0]) : std::string();
  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return Undefined(env);
  napi_value message_port_ctor = GetRefValue(env, it->second.message_port_ctor_ref);
  if (!IsFunction(env, message_port_ctor)) return Undefined(env);

  napi_value handle = nullptr;
  if (napi_new_instance(env, message_port_ctor, 0, nullptr, &handle) != napi_ok || handle == nullptr) {
    return Undefined(env);
  }

  MessagePortWrap* wrap = UnwrapMessagePort(env, handle);
  if (wrap == nullptr || !wrap->data) return Undefined(env);
  AttachToBroadcastChannelGroup(wrap->data, GetOrCreateBroadcastChannelGroup(name));
  return handle;
}

napi_value DrainMessagePortCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
  if (wrap == nullptr) return Undefined(env);
  ProcessQueuedMessages(wrap, true);
  return Undefined(env);
}

napi_value MoveMessagePortToContextCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;

  if (argc < 1 || argv[0] == nullptr) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"port\" argument must be a MessagePort instance");
    return nullptr;
  }

  MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
  if (wrap == nullptr) {
    napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE", "The \"port\" argument must be a MessagePort instance");
    return nullptr;
  }
  if (wrap->handle_wrap.state != kUbiHandleInitialized || !wrap->data) {
    ThrowClosedMessagePortError(env);
    return nullptr;
  }

  bool closed = false;
  {
    std::lock_guard<std::mutex> lock(wrap->data->mutex);
    closed = wrap->data->closed;
  }
  if (closed) {
    ThrowClosedMessagePortError(env);
    return nullptr;
  }

  if (argc < 2 || argv[1] == nullptr) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "Invalid context argument");
    return nullptr;
  }
  napi_valuetype context_type = napi_undefined;
  if (napi_typeof(env, argv[1], &context_type) != napi_ok || context_type != napi_object) {
    ThrowTypeErrorWithCode(env, "ERR_INVALID_ARG_TYPE", "Invalid context argument");
    return nullptr;
  }

  return argv[0];
}

napi_value ReceiveMessageOnPortCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc < 1 || argv[0] == nullptr) {
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, argv[0], &type) != napi_ok || type != napi_object) {
    napi_throw_type_error(env,
                          "ERR_INVALID_ARG_TYPE",
                          "The \"port\" argument must be a MessagePort instance");
    return nullptr;
  }
  MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
  if (wrap == nullptr) {
    napi_throw_type_error(env,
                          "ERR_INVALID_ARG_TYPE",
                          "The \"port\" argument must be a MessagePort instance");
    return nullptr;
  }

  QueuedMessage next;
  bool have_message = false;
  {
    if (wrap->data) {
      std::lock_guard<std::mutex> lock(wrap->data->mutex);
      if (!wrap->data->queued_messages.empty()) {
        next = wrap->data->queued_messages.front();
        wrap->data->queued_messages.pop_front();
        if (next.is_close) wrap->data->close_message_enqueued = false;
        have_message = true;
      }
    }
  }
  if (!have_message) {
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  if (next.is_close) {
    if (next.payload_data != nullptr) {
      unofficial_napi_release_serialized_value(next.payload_data);
      next.payload_data = nullptr;
    }
    for (auto& entry : next.transferred_ports) {
      DeleteRefIfPresent(env, &entry.source_port_ref);
    }
    BeginClosePort(env, wrap, false);
    napi_value symbol = GetNoMessageSymbol(env);
    return symbol != nullptr ? symbol : Undefined(env);
  }

  napi_value value = nullptr;
  if (next.payload_data != nullptr) {
    if (unofficial_napi_deserialize_value(env, next.payload_data, &value) != napi_ok) {
      value = nullptr;
    }
    unofficial_napi_release_serialized_value(next.payload_data);
    next.payload_data = nullptr;
  }
  if (value == nullptr) {
    napi_value exception = TakePendingException(env);
    if (exception == nullptr) {
      exception = CreateErrorWithMessage(env, nullptr, "Message could not be deserialized");
    }
    DeleteTransferredPortRefs(env, &next.transferred_ports);
    if (exception != nullptr) napi_throw(env, exception);
    return nullptr;
  }
  ReceivedTransferredPortState received_ports;
  std::vector<ValueTransformPair> seen_pairs;
  value = RestoreTransferredPortsInValue(
      env,
      value != nullptr ? value : Undefined(env),
      next,
      &received_ports,
      &seen_pairs);
  napi_value exception = TakePendingException(env);
  if (exception != nullptr) {
    DeleteTransferredPortRefs(env, &next.transferred_ports);
    napi_throw(env, exception);
    return nullptr;
  }
  DeleteTransferredPortRefs(env, &next.transferred_ports);
  return value != nullptr ? value : Undefined(env);
}

napi_value StopMessagePortCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    MessagePortWrap* wrap = UnwrapMessagePort(env, argv[0]);
    if (wrap != nullptr) wrap->receiving_messages = false;
  }
  return Undefined(env);
}

napi_value FallbackDOMExceptionConstructorCallback(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  const std::string message =
      (argc >= 1 && argv[0] != nullptr) ? ValueToUtf8(env, argv[0]) : std::string();
  const std::string name =
      (argc >= 2 && argv[1] != nullptr) ? ValueToUtf8(env, argv[1]) : std::string("Error");

  napi_value message_value = nullptr;
  napi_value name_value = nullptr;
  napi_create_string_utf8(env, message.c_str(), NAPI_AUTO_LENGTH, &message_value);
  napi_create_string_utf8(env, name.c_str(), NAPI_AUTO_LENGTH, &name_value);
  if (message_value != nullptr) napi_set_named_property(env, this_arg, "message", message_value);
  if (name_value != nullptr) napi_set_named_property(env, this_arg, "name", name_value);
  SetInt32(env, this_arg, "code", 0);
  return this_arg;
}

napi_value CreateFallbackDOMExceptionConstructor(napi_env env) {
  napi_value ctor = nullptr;
  if (napi_define_class(env,
                        "DOMException",
                        NAPI_AUTO_LENGTH,
                        FallbackDOMExceptionConstructorCallback,
                        nullptr,
                        0,
                        nullptr,
                        &ctor) != napi_ok ||
      ctor == nullptr) {
    return nullptr;
  }

  napi_value global = GetGlobal(env);
  napi_value object_ctor = GetNamed(env, global, "Object");
  napi_value set_prototype_of = GetNamed(env, object_ctor, "setPrototypeOf");
  napi_value error_ctor = GetNamed(env, global, "Error");
  napi_value error_prototype = GetNamed(env, error_ctor, "prototype");
  napi_value dom_prototype = GetNamed(env, ctor, "prototype");
  if (IsFunction(env, set_prototype_of) && dom_prototype != nullptr && error_prototype != nullptr) {
    napi_value argv[2] = {dom_prototype, error_prototype};
    napi_value ignored = nullptr;
    if (napi_call_function(env, object_ctor, set_prototype_of, 2, argv, &ignored) != napi_ok) {
      ClearPendingException(env);
    }
  }
  return ctor;
}

napi_value ResolveDOMExceptionValue(napi_env env) {
  napi_value global = GetGlobal(env);
  napi_value dom_exception = GetNamed(env, global, "DOMException");
  if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) return dom_exception;

  napi_value dom_module = TryRequireModule(env, "internal/per_context/domexception");
  if (dom_module != nullptr && !IsUndefined(env, dom_module)) {
    dom_exception = GetNamed(env, dom_module, "DOMException");
    if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) return dom_exception;
    dom_exception = GetNamed(env, dom_module, "default");
    if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) return dom_exception;
  }

  dom_exception = CreateFallbackDOMExceptionConstructor(env);
  if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) {
    napi_set_named_property(env, global, "DOMException", dom_exception);
    return dom_exception;
  }

  return Undefined(env);
}

napi_value ResolveEmitMessageValue(napi_env env) {
  auto& state = EnsureMessagingState(env);
  napi_value cached = GetRefValue(env, state.emit_message_ref);
  if (IsFunction(env, cached)) return cached;

  napi_value messageport_module = TryRequireModule(env, "internal/per_context/messageport");
  if (messageport_module == nullptr || IsUndefined(env, messageport_module)) return nullptr;

  napi_value emit_message = GetNamed(env, messageport_module, "emitMessage");
  if (!IsFunction(env, emit_message)) return nullptr;

  SetRefToValue(env, &state.emit_message_ref, emit_message);
  return emit_message;
}

napi_value GetCachedMessaging(napi_env env) {
  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end() || it->second.binding_ref == nullptr) return nullptr;
  return GetRefValue(env, it->second.binding_ref);
}

}  // namespace

UbiMessagePortDataPtr UbiCreateMessagePortData() {
  return InternalCreateMessagePortData();
}

void UbiEntangleMessagePortData(const UbiMessagePortDataPtr& first,
                                const UbiMessagePortDataPtr& second) {
  InternalEntangleMessagePortData(first, second);
}

UbiMessagePortDataPtr UbiGetMessagePortData(napi_env env, napi_value value) {
  MessagePortWrap* wrap = UnwrapMessagePort(env, value);
  if (wrap == nullptr) return nullptr;
  return wrap->data;
}

napi_value UbiCreateMessagePortForData(napi_env env, const UbiMessagePortDataPtr& data) {
  if (!data) return Undefined(env);
  auto it = g_messaging_states.find(env);
  if (it == g_messaging_states.end()) return Undefined(env);
  napi_value ctor = GetRefValue(env, it->second.message_port_ctor_ref);
  if (!IsFunction(env, ctor)) return Undefined(env);

  napi_value port = nullptr;
  if (napi_new_instance(env, ctor, 0, nullptr, &port) != napi_ok || port == nullptr) {
    return Undefined(env);
  }

  MessagePortWrap* wrap = UnwrapMessagePort(env, port);
  if (wrap == nullptr) return Undefined(env);

  if (wrap->data) {
    std::lock_guard<std::mutex> old_lock(wrap->data->mutex);
    if (wrap->data->attached_wrap == wrap) {
      wrap->data->attached_wrap = nullptr;
    }
  }

  wrap->data = data;
  bool has_queued_messages = false;
  {
    std::lock_guard<std::mutex> lock(data->mutex);
    data->attached_wrap = wrap;
    data->closed = false;
    has_queued_messages = !data->queued_messages.empty();
  }
  if (has_queued_messages) {
    TriggerPortAsync(wrap);
  }
  return port;
}

napi_value ResolveMessaging(napi_env env, const ResolveOptions& options) {
  const napi_value undefined = Undefined(env);
  napi_value cached = GetCachedMessaging(env);
  if (cached != nullptr) return cached;

  auto& state = EnsureMessagingState(env);
  EnsureMessagingSymbols(env, options);

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  napi_value message_port_ctor = nullptr;
  if (napi_define_class(env,
                        "MessagePort",
                        NAPI_AUTO_LENGTH,
                        MessagePortConstructorCallback,
                        nullptr,
                        0,
                        nullptr,
                        &message_port_ctor) == napi_ok &&
      message_port_ctor != nullptr) {
    constexpr napi_property_attributes kMutableMethodAttrs =
        static_cast<napi_property_attributes>(napi_writable | napi_configurable);
    napi_property_descriptor methods[] = {
        {"postMessage", nullptr, MessagePortPostMessageCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"start", nullptr, MessagePortStartCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"close", nullptr, MessagePortCloseCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"ref", nullptr, MessagePortRefCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"unref", nullptr, MessagePortUnrefCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
        {"hasRef", nullptr, MessagePortHasRefCallback, nullptr, nullptr, nullptr, kMutableMethodAttrs, nullptr},
    };
    napi_value prototype = nullptr;
    if (napi_get_named_property(env, message_port_ctor, "prototype", &prototype) == napi_ok && prototype != nullptr) {
      napi_define_properties(env, prototype, sizeof(methods) / sizeof(methods[0]), methods);
    }
    napi_set_named_property(env, out, "MessagePort", message_port_ctor);
    SetRefToValue(env, &state.message_port_ctor_ref, message_port_ctor);
  }

  napi_value message_channel_ctor = nullptr;
  if (napi_define_class(env,
                        "MessageChannel",
                        NAPI_AUTO_LENGTH,
                        MessageChannelConstructorCallback,
                        nullptr,
                        0,
                        nullptr,
                        &message_channel_ctor) == napi_ok &&
      message_channel_ctor != nullptr) {
    napi_set_named_property(env, out, "MessageChannel", message_channel_ctor);
  }

  napi_value broadcast_channel_fn = nullptr;
  if (napi_create_function(env,
                           "broadcastChannel",
                           NAPI_AUTO_LENGTH,
                           BroadcastChannelCallback,
                           nullptr,
                           &broadcast_channel_fn) == napi_ok &&
      broadcast_channel_fn != nullptr) {
    napi_set_named_property(env, out, "broadcastChannel", broadcast_channel_fn);
  }

  napi_value drain_fn = nullptr;
  if (napi_create_function(env,
                           "drainMessagePort",
                           NAPI_AUTO_LENGTH,
                           DrainMessagePortCallback,
                           nullptr,
                           &drain_fn) == napi_ok &&
      drain_fn != nullptr) {
    napi_set_named_property(env, out, "drainMessagePort", drain_fn);
  }

  napi_value move_fn = nullptr;
  if (napi_create_function(env,
                           "moveMessagePortToContext",
                           NAPI_AUTO_LENGTH,
                           MoveMessagePortToContextCallback,
                           nullptr,
                           &move_fn) == napi_ok &&
      move_fn != nullptr) {
    napi_set_named_property(env, out, "moveMessagePortToContext", move_fn);
  }

  napi_value receive_fn = nullptr;
  if (napi_create_function(env,
                           "receiveMessageOnPort",
                           NAPI_AUTO_LENGTH,
                           ReceiveMessageOnPortCallback,
                           nullptr,
                           &receive_fn) == napi_ok &&
      receive_fn != nullptr) {
    napi_set_named_property(env, out, "receiveMessageOnPort", receive_fn);
  }

  napi_value stop_fn = nullptr;
  if (napi_create_function(env,
                           "stopMessagePort",
                           NAPI_AUTO_LENGTH,
                           StopMessagePortCallback,
                           nullptr,
                           &stop_fn) == napi_ok &&
      stop_fn != nullptr) {
    napi_set_named_property(env, out, "stopMessagePort", stop_fn);
  }

  napi_value expose_fn = nullptr;
  if (napi_create_function(env,
                           "exposeLazyDOMExceptionProperty",
                           NAPI_AUTO_LENGTH,
                           ExposeLazyDOMExceptionPropertyCallback,
                           nullptr,
                           &expose_fn) == napi_ok &&
      expose_fn != nullptr) {
    napi_set_named_property(env, out, "exposeLazyDOMExceptionProperty", expose_fn);
  }

  napi_value set_deserializer = nullptr;
  if (napi_create_function(env,
                           "setDeserializerCreateObjectFunction",
                           NAPI_AUTO_LENGTH,
                           SetDeserializerCreateObjectFunctionCallback,
                           nullptr,
                           &set_deserializer) == napi_ok &&
      set_deserializer != nullptr) {
    napi_set_named_property(env, out, "setDeserializerCreateObjectFunction", set_deserializer);
  }

  napi_value structured_clone = nullptr;
  if (napi_create_function(env,
                           "structuredClone",
                           NAPI_AUTO_LENGTH,
                           StructuredCloneCallback,
                           nullptr,
                           &structured_clone) == napi_ok &&
      structured_clone != nullptr) {
    napi_set_named_property(env, out, "structuredClone", structured_clone);
  }

  napi_value dom_exception = ResolveDOMExceptionValue(env);
  if (dom_exception != nullptr && !IsUndefined(env, dom_exception)) {
    napi_set_named_property(env, out, "DOMException", dom_exception);
  }

  SetRefToValue(env, &state.binding_ref, out);
  return out;
}

}  // namespace internal_binding
