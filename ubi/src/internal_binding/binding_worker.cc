#include "internal_binding/dispatch.h"
#include "internal_binding/binding_messaging.h"

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <uv.h>

#include "internal_binding/helpers.h"
#include "unofficial_napi.h"
#include "ubi_env_loop.h"
#include "ubi_runtime.h"
#include "ubi_worker_env.h"

namespace internal_binding {
namespace {

bool DebugWorkerEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("UBI_DEBUG_WORKER");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
  }();
  return enabled;
}

void DebugWorkerLog(const std::string& message) {
  if (!DebugWorkerEnabled()) return;
  std::cerr << "[ubi-worker] " << message << std::endl;
}

std::atomic<int32_t> g_next_worker_thread_id{1};

struct WorkerImplWrap {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  napi_ref message_port_ref = nullptr;
  int32_t thread_id = 0;
  std::string thread_name = "WorkerThread";
  std::vector<std::string> exec_argv;
  UbiWorkerEnvConfig worker_config;
  std::thread thread;
  std::mutex mutex;
  napi_env worker_env = nullptr;
  void* worker_scope = nullptr;
  uv_async_t exit_async{};
  uv_async_t stop_async{};
  std::atomic<bool> exit_async_initialized{false};
  std::atomic<bool> stop_async_initialized{false};
  std::atomic<bool> started{false};
  std::atomic<bool> has_ref{true};
  std::atomic<bool> stop_requested{false};
  int32_t requested_exit_code = 1;
  int32_t exit_code = 0;
  std::string custom_err;
  std::string custom_err_reason;
};

napi_value GetNamed(napi_env env, napi_value obj, const char* key) {
  napi_value out = nullptr;
  if (obj == nullptr || napi_get_named_property(env, obj, key, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

bool IsFunction(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

bool IsNullOrUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && (type == napi_null || type == napi_undefined);
}

bool IsNullValue(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_null;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  napi_value as_string = nullptr;
  if (napi_coerce_to_string(env, value, &as_string) != napi_ok || as_string == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, as_string, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, as_string, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

void SetRefValue(napi_env env, napi_ref* slot, napi_value value) {
  if (slot == nullptr) return;
  DeleteRefIfPresent(env, slot);
  if (value != nullptr) {
    napi_create_reference(env, value, 1, slot);
  }
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (env == nullptr || ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

WorkerImplWrap* UnwrapWorkerImpl(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<WorkerImplWrap*>(data);
}

std::string FileUrlToPath(const std::string& maybe_url) {
  if (maybe_url.rfind("file://", 0) != 0) return maybe_url;
  std::string path = maybe_url.substr(7);
  if (path.size() >= 3 && path[0] == '/' && path[2] == ':') return path.substr(1);
  return path;
}

bool SnapshotStringMapFromObject(napi_env env, napi_value object, std::map<std::string, std::string>* out) {
  if (env == nullptr || object == nullptr || out == nullptr) return false;
  napi_value keys = nullptr;
  if (napi_get_property_names(env, object, &keys) != napi_ok || keys == nullptr) return false;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return false;
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    const std::string key_string = ValueToUtf8(env, key);
    if (key_string.empty()) continue;
    napi_value value = nullptr;
    if (napi_get_property(env, object, key, &value) != napi_ok || value == nullptr) continue;
    napi_value string_value = nullptr;
    if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) continue;
    (*out)[key_string] = ValueToUtf8(env, string_value);
  }
  return true;
}

std::map<std::string, std::string> SnapshotCurrentProcessEnv(napi_env env) {
  std::map<std::string, std::string> out;
  napi_value global = GetGlobal(env);
  napi_value process = GetNamed(env, global, "process");
  napi_value process_env = GetNamed(env, process, "env");
  if (process_env != nullptr) {
    (void)SnapshotStringMapFromObject(env, process_env, &out);
  }
  return out;
}

std::vector<std::string> ReadStringArray(napi_env env, napi_value value) {
  std::vector<std::string> out;
  bool is_array = false;
  if (value == nullptr || napi_is_array(env, value, &is_array) != napi_ok || !is_array) return out;
  uint32_t len = 0;
  if (napi_get_array_length(env, value, &len) != napi_ok) return out;
  out.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value element = nullptr;
    if (napi_get_element(env, value, i, &element) != napi_ok || element == nullptr) continue;
    const std::string arg = ValueToUtf8(env, element);
    if (arg == "--") break;
    out.push_back(arg);
  }
  return out;
}

bool IsAllowedNodeEnvironmentFlag(napi_env env, const std::string& flag) {
  napi_value global = GetGlobal(env);
  napi_value process = GetNamed(env, global, "process");
  napi_value allowed = GetNamed(env, process, "allowedNodeEnvironmentFlags");
  napi_value has = GetNamed(env, allowed, "has");
  if (!IsFunction(env, has)) return true;
  napi_value flag_v = nullptr;
  if (napi_create_string_utf8(env, flag.c_str(), NAPI_AUTO_LENGTH, &flag_v) != napi_ok || flag_v == nullptr) {
    return false;
  }
  napi_value result = nullptr;
  if (napi_call_function(env, allowed, has, 1, &flag_v, &result) != napi_ok || result == nullptr) {
    return false;
  }
  bool is_allowed = false;
  return napi_get_value_bool(env, result, &is_allowed) == napi_ok && is_allowed;
}

bool IsDisallowedWorkerExecArgvFlag(const std::string& flag) {
  return flag == "--expose-gc" ||
         flag == "--expose_gc" ||
         flag == "--title" ||
         flag == "--redirect-warnings";
}

std::vector<std::string> ValidateExecArgv(napi_env env,
                                          const std::vector<std::string>& args,
                                          bool explicitly_provided) {
  std::vector<std::string> invalid;
  if (!explicitly_provided) return invalid;
  for (const std::string& arg : args) {
    if (arg.empty() || arg[0] != '-') continue;
    std::string flag = arg;
    const size_t eq = flag.find('=');
    if (eq != std::string::npos) flag.resize(eq);
    if (IsDisallowedWorkerExecArgvFlag(flag) || !IsAllowedNodeEnvironmentFlag(env, flag)) {
      invalid.push_back(arg);
    }
  }
  return invalid;
}

std::vector<std::string> ValidateNodeOptionsEnv(napi_env env, const std::map<std::string, std::string>& entries) {
  auto it = entries.find("NODE_OPTIONS");
  if (it == entries.end()) return {};
  std::vector<std::string> invalid;
  std::string current;
  const std::string& options = it->second;
  for (size_t i = 0; i <= options.size(); ++i) {
    if (i == options.size() || options[i] == ' ') {
      if (!current.empty()) {
        std::string flag = current;
        const size_t eq = flag.find('=');
        if (eq != std::string::npos) flag.resize(eq);
        if (IsDisallowedWorkerExecArgvFlag(flag) || !IsAllowedNodeEnvironmentFlag(env, flag)) invalid.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(options[i]);
  }
  return invalid;
}

std::array<double, 4> ReadResourceLimits(napi_env env, napi_value value) {
  std::array<double, 4> limits = {-1, -1, -1, -1};
  bool is_typed_array = false;
  napi_typedarray_type typed_array_type = napi_int8_array;
  size_t length = 0;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (value == nullptr ||
      napi_is_typedarray(env, value, &is_typed_array) != napi_ok ||
      !is_typed_array ||
      napi_get_typedarray_info(env, value, &typed_array_type, &length, nullptr, &arraybuffer, &byte_offset) != napi_ok ||
      typed_array_type != napi_float64_array) {
    return limits;
  }
  void* data = nullptr;
  size_t byte_length = 0;
  if (napi_get_arraybuffer_info(env, arraybuffer, &data, &byte_length) != napi_ok || data == nullptr) return limits;
  const double* values = reinterpret_cast<const double*>(static_cast<uint8_t*>(data) + byte_offset);
  const size_t count = length < limits.size() ? length : limits.size();
  for (size_t i = 0; i < count; ++i) limits[i] = values[i];
  return limits;
}

napi_value BuildResourceLimitsArray(napi_env env, const std::array<double, 4>& limits) {
  napi_value ab = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, sizeof(double) * limits.size(), &data, &ab) != napi_ok || ab == nullptr ||
      data == nullptr) {
    return Undefined(env);
  }
  double* values = static_cast<double*>(data);
  for (size_t i = 0; i < limits.size(); ++i) values[i] = limits[i];
  napi_value typed = nullptr;
  if (napi_create_typedarray(env, napi_float64_array, limits.size(), ab, 0, &typed) != napi_ok || typed == nullptr) {
    return Undefined(env);
  }
  return typed;
}

void CallWorkerOnExit(WorkerImplWrap* wrap) {
  if (wrap == nullptr || wrap->env == nullptr || wrap->wrapper_ref == nullptr) return;
  napi_value self = GetRefValue(wrap->env, wrap->wrapper_ref);
  if (self == nullptr) return;
  napi_value onexit = GetNamed(wrap->env, self, "onexit");
  if (!IsFunction(wrap->env, onexit)) return;
  napi_value argv[3] = {nullptr, Undefined(wrap->env), Undefined(wrap->env)};
  napi_create_int32(wrap->env, wrap->exit_code, &argv[0]);
  if (!wrap->custom_err.empty()) {
    napi_create_string_utf8(wrap->env, wrap->custom_err.c_str(), NAPI_AUTO_LENGTH, &argv[1]);
  }
  if (!wrap->custom_err_reason.empty()) {
    napi_create_string_utf8(wrap->env, wrap->custom_err_reason.c_str(), NAPI_AUTO_LENGTH, &argv[2]);
  }
  napi_value ignored = nullptr;
  (void)UbiMakeCallback(wrap->env, self, onexit, 3, argv, &ignored);
}

void OnWorkerExitAsyncClosed(uv_handle_t* handle) {
  auto* wrap = static_cast<WorkerImplWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  DebugWorkerLog("OnWorkerExitAsyncClosed thread_id=" + std::to_string(wrap->thread_id));
  if (wrap->thread.joinable()) wrap->thread.join();
  DeleteRefIfPresent(wrap->env, &wrap->message_port_ref);
  if (wrap->wrapper_ref != nullptr) {
    uint32_t ref_count = 0;
    (void)napi_reference_unref(wrap->env, wrap->wrapper_ref, &ref_count);
  }
}

void OnWorkerExitAsync(uv_async_t* handle) {
  auto* wrap = static_cast<WorkerImplWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr) return;
  DebugWorkerLog("OnWorkerExitAsync thread_id=" + std::to_string(wrap->thread_id) +
                 " exit_code=" + std::to_string(wrap->exit_code));
  CallWorkerOnExit(wrap);
  uv_close(reinterpret_cast<uv_handle_t*>(&wrap->exit_async), OnWorkerExitAsyncClosed);
}

void OnWorkerStopAsync(uv_async_t* handle) {
  auto* wrap = static_cast<WorkerImplWrap*>(handle != nullptr ? handle->data : nullptr);
  if (wrap == nullptr || !wrap->stop_requested.load(std::memory_order_acquire)) return;
  DebugWorkerLog("OnWorkerStopAsync thread_id=" + std::to_string(wrap->thread_id));
  uv_loop_t* loop = handle->loop;
  if (loop != nullptr) uv_stop(loop);
}

void FinalizeWorkerThread(WorkerImplWrap* wrap, int exit_code, const std::string& custom_err, const std::string& custom_err_reason) {
  if (wrap == nullptr) return;
  DebugWorkerLog("FinalizeWorkerThread thread_id=" + std::to_string(wrap->thread_id) +
                 " exit_code=" + std::to_string(exit_code) +
                 (custom_err.empty() ? "" : " err=" + custom_err) +
                 (custom_err_reason.empty() ? "" : " reason=" + custom_err_reason));
  {
    std::lock_guard<std::mutex> lock(wrap->mutex);
    wrap->worker_env = nullptr;
    wrap->worker_scope = nullptr;
    wrap->exit_code = exit_code;
    wrap->custom_err = custom_err;
    wrap->custom_err_reason = custom_err_reason;
  }
  if (wrap->exit_async_initialized.load(std::memory_order_acquire)) {
    uv_async_send(&wrap->exit_async);
  }
}

void CloseWorkerLoopHandles(uv_loop_t* loop, uv_async_t* stop_async) {
  if (loop == nullptr) return;
  uv_walk(
      loop,
      [](uv_handle_t* handle, void* arg) {
        if (handle == nullptr || uv_is_closing(handle)) return;
        auto* stop_handle = static_cast<uv_handle_t*>(arg);
        if (handle == stop_handle) return;
        uv_close(handle, nullptr);
      },
      stop_async != nullptr ? reinterpret_cast<void*>(stop_async) : nullptr);
}

void WorkerThreadMain(WorkerImplWrap* wrap) {
  DebugWorkerLog("WorkerThreadMain start thread_id=" + std::to_string(wrap != nullptr ? wrap->thread_id : -1));
  int exit_code = 0;
  std::string custom_err;
  std::string custom_err_reason;
  napi_env worker_env = nullptr;
  void* worker_scope = nullptr;
  if (unofficial_napi_create_env(8, &worker_env, &worker_scope) != napi_ok || worker_env == nullptr ||
      worker_scope == nullptr) {
    DebugWorkerLog("WorkerThreadMain failed create_env");
    FinalizeWorkerThread(wrap, 1, "ERR_WORKER_INIT_FAILED", "Failed to create worker env");
    return;
  }

  UbiWorkerEnvConfigure(worker_env, wrap->worker_config);
  {
    std::lock_guard<std::mutex> lock(wrap->mutex);
    wrap->worker_env = worker_env;
    wrap->worker_scope = worker_scope;
  }

  uv_loop_t* loop = UbiGetEnvLoop(worker_env);
  if (loop == nullptr || uv_async_init(loop, &wrap->stop_async, OnWorkerStopAsync) != 0) {
    DebugWorkerLog("WorkerThreadMain failed stop_async init");
    custom_err = "ERR_WORKER_INIT_FAILED";
    custom_err_reason = "Failed to initialize worker stop handle";
    exit_code = 1;
  } else {
    wrap->stop_async.data = wrap;
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->stop_async));
    wrap->stop_async_initialized.store(true, std::memory_order_release);

    std::string run_error;
    exit_code = UbiRunWorkerThreadMain(worker_env, wrap->exec_argv, &run_error);
    DebugWorkerLog("WorkerThreadMain run returned thread_id=" + std::to_string(wrap->thread_id) +
                   " exit_code=" + std::to_string(exit_code) +
                   (run_error.empty() ? "" : " run_error=" + run_error));
    if (exit_code != 0 && !run_error.empty() && wrap->custom_err.empty()) {
      custom_err = "ERR_WORKER_INIT_FAILED";
      custom_err_reason = run_error;
    }
    if (wrap->stop_requested.load(std::memory_order_acquire)) {
      exit_code = wrap->requested_exit_code;
      custom_err.clear();
      custom_err_reason.clear();
    }
  }

  if (wrap->stop_async_initialized.exchange(false, std::memory_order_acq_rel)) {
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->stop_async), nullptr);
  }

  CloseWorkerLoopHandles(loop, nullptr);
  for (int i = 0; loop != nullptr && i < 32; ++i) {
    if (uv_run(loop, UV_RUN_NOWAIT) == 0) break;
  }

  DebugWorkerLog("WorkerThreadMain releasing env thread_id=" + std::to_string(wrap->thread_id));
  (void)unofficial_napi_release_env(worker_scope);
  FinalizeWorkerThread(wrap, exit_code, custom_err, custom_err_reason);
}

void WorkerImplFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* wrap = static_cast<WorkerImplWrap*>(data);
  if (wrap == nullptr) return;
  if (wrap->thread.joinable()) wrap->thread.join();
  DeleteRefIfPresent(env, &wrap->message_port_ref);
  DeleteRefIfPresent(env, &wrap->wrapper_ref);
  delete wrap;
}

napi_value CreateMessageChannel(napi_env env, napi_value* parent_port, UbiMessagePortDataPtr* worker_port_data) {
  if (parent_port == nullptr || worker_port_data == nullptr) return nullptr;
  *parent_port = nullptr;
  *worker_port_data = nullptr;
  UbiMessagePortDataPtr first = UbiCreateMessagePortData();
  UbiMessagePortDataPtr second = UbiCreateMessagePortData();
  UbiEntangleMessagePortData(first, second);
  napi_value parent = UbiCreateMessagePortForData(env, first);
  if (parent == nullptr || IsNullOrUndefinedValue(env, parent)) return nullptr;
  *parent_port = parent;
  *worker_port_data = second;
  return parent;
}

napi_value WorkerImplCtor(napi_env env, napi_callback_info info) {
  size_t argc = 7;
  napi_value argv[7] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto* wrap = new WorkerImplWrap();
  wrap->env = env;
  wrap->thread_id = g_next_worker_thread_id.fetch_add(1, std::memory_order_relaxed);
  if (argc >= 7 && argv[6] != nullptr && !IsNullOrUndefinedValue(env, argv[6])) {
    const std::string name = ValueToUtf8(env, argv[6]);
    if (!name.empty()) wrap->thread_name = name;
  }

  wrap->worker_config.is_main_thread = false;
  wrap->worker_config.owns_process_state = false;
  wrap->worker_config.thread_id = wrap->thread_id;
  wrap->worker_config.thread_name = wrap->thread_name;
  {
    bool is_internal_thread = false;
    if (argc >= 6 && argv[5] != nullptr && !IsNullOrUndefinedValue(env, argv[5])) {
      (void)napi_get_value_bool(env, argv[5], &is_internal_thread);
    }
    wrap->worker_config.is_internal_thread = is_internal_thread;
  }
  if (argc >= 4 && argv[3] != nullptr) {
    wrap->worker_config.resource_limits = ReadResourceLimits(env, argv[3]);
  }

  const bool explicit_exec_argv = argc >= 3 && argv[2] != nullptr && !IsNullOrUndefinedValue(env, argv[2]);
  napi_value process = GetNamed(env, GetGlobal(env), "process");
  napi_value process_exec_argv = GetNamed(env, process, "execArgv");
  wrap->exec_argv = explicit_exec_argv ? ReadStringArray(env, argv[2]) : ReadStringArray(env, process_exec_argv);
  const std::vector<std::string> invalid_exec_argv = ValidateExecArgv(env, wrap->exec_argv, explicit_exec_argv);
  if (!invalid_exec_argv.empty()) {
    napi_value invalid = nullptr;
    if (napi_create_array_with_length(env, invalid_exec_argv.size(), &invalid) == napi_ok && invalid != nullptr) {
      for (size_t i = 0; i < invalid_exec_argv.size(); ++i) {
        napi_value item = nullptr;
        if (napi_create_string_utf8(env, invalid_exec_argv[i].c_str(), NAPI_AUTO_LENGTH, &item) == napi_ok && item != nullptr) {
          napi_set_element(env, invalid, static_cast<uint32_t>(i), item);
        }
      }
      napi_set_named_property(env, this_arg, "invalidExecArgv", invalid);
    }
  }

  if (argc >= 2 && argv[1] != nullptr) {
    napi_valuetype env_type = napi_undefined;
    if (napi_typeof(env, argv[1], &env_type) == napi_ok && env_type == napi_object && !IsNullOrUndefinedValue(env, argv[1])) {
      wrap->worker_config.share_env = false;
      (void)SnapshotStringMapFromObject(env, argv[1], &wrap->worker_config.env_vars);
      const std::vector<std::string> invalid_node_options = ValidateNodeOptionsEnv(env, wrap->worker_config.env_vars);
      if (!invalid_node_options.empty()) {
        napi_value invalid = nullptr;
        if (napi_create_array_with_length(env, invalid_node_options.size(), &invalid) == napi_ok && invalid != nullptr) {
          for (size_t i = 0; i < invalid_node_options.size(); ++i) {
            napi_value item = nullptr;
            if (napi_create_string_utf8(env, invalid_node_options[i].c_str(), NAPI_AUTO_LENGTH, &item) == napi_ok && item != nullptr) {
              napi_set_element(env, invalid, static_cast<uint32_t>(i), item);
            }
          }
          napi_set_named_property(env, this_arg, "invalidNodeOptions", invalid);
        }
      }
    } else if (IsNullValue(env, argv[1])) {
      wrap->worker_config.share_env = false;
      wrap->worker_config.env_vars = SnapshotCurrentProcessEnv(env);
    } else {
      wrap->worker_config.share_env = true;
    }
  } else {
    wrap->worker_config.share_env = false;
    wrap->worker_config.env_vars = SnapshotCurrentProcessEnv(env);
  }

  napi_value parent_port = nullptr;
  if (CreateMessageChannel(env, &parent_port, &wrap->worker_config.env_message_port_data) == nullptr ||
      parent_port == nullptr) {
    delete wrap;
    return nullptr;
  }
  SetRefValue(env, &wrap->message_port_ref, parent_port);

  uv_loop_t* loop = UbiGetEnvLoop(env);
  if (loop == nullptr || uv_async_init(loop, &wrap->exit_async, OnWorkerExitAsync) != 0) {
    delete wrap;
    return nullptr;
  }
  wrap->exit_async.data = wrap;
  wrap->exit_async_initialized.store(true, std::memory_order_release);

  if (napi_wrap(env, this_arg, wrap, WorkerImplFinalize, nullptr, &wrap->wrapper_ref) != napi_ok) {
    uv_close(reinterpret_cast<uv_handle_t*>(&wrap->exit_async), nullptr);
    delete wrap;
    return nullptr;
  }

  napi_set_named_property(env, this_arg, "messagePort", parent_port);
  SetInt32(env, this_arg, "threadId", wrap->thread_id);
  SetString(env, this_arg, "threadName", wrap->thread_name.c_str());
  return this_arg;
}

napi_value WorkerImplStartThread(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  bool expected = false;
  if (!wrap->started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return Undefined(env);
  }

  uint32_t ref_count = 0;
  if (wrap->wrapper_ref != nullptr) (void)napi_reference_ref(env, wrap->wrapper_ref, &ref_count);
  try {
    wrap->thread = std::thread(WorkerThreadMain, wrap);
  } catch (const std::exception& ex) {
    wrap->started.store(false, std::memory_order_release);
    if (wrap->wrapper_ref != nullptr) (void)napi_reference_unref(env, wrap->wrapper_ref, &ref_count);
    napi_throw_error(env, "ERR_WORKER_INIT_FAILED", ex.what());
  } catch (...) {
    wrap->started.store(false, std::memory_order_release);
    if (wrap->wrapper_ref != nullptr) (void)napi_reference_unref(env, wrap->wrapper_ref, &ref_count);
    napi_throw_error(env, "ERR_WORKER_INIT_FAILED", "Failed to create worker thread");
  }
  return Undefined(env);
}

napi_value WorkerImplStopThread(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap == nullptr || !wrap->started.load(std::memory_order_acquire)) return Undefined(env);
  wrap->stop_requested.store(true, std::memory_order_release);
  DebugWorkerLog("stopThread thread_id=" + std::to_string(wrap->thread_id));
  napi_env worker_env = nullptr;
  {
    std::lock_guard<std::mutex> lock(wrap->mutex);
    worker_env = wrap->worker_env;
  }
  if (worker_env != nullptr) {
    UbiWorkerEnvRequestStop(worker_env);
    (void)unofficial_napi_terminate_execution(worker_env);
  }
  if (wrap->stop_async_initialized.load(std::memory_order_acquire)) {
    uv_async_send(&wrap->stop_async);
  }
  return Undefined(env);
}

napi_value WorkerImplRef(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap != nullptr && wrap->exit_async_initialized.load(std::memory_order_acquire)) {
    wrap->has_ref.store(true, std::memory_order_release);
    uv_ref(reinterpret_cast<uv_handle_t*>(&wrap->exit_async));
  }
  return Undefined(env);
}

napi_value WorkerImplUnref(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap != nullptr && wrap->exit_async_initialized.load(std::memory_order_acquire)) {
    wrap->has_ref.store(false, std::memory_order_release);
    uv_unref(reinterpret_cast<uv_handle_t*>(&wrap->exit_async));
  }
  return Undefined(env);
}

napi_value WorkerImplGetResourceLimits(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  WorkerImplWrap* wrap = UnwrapWorkerImpl(env, this_arg);
  if (wrap == nullptr) return Undefined(env);
  return BuildResourceLimitsArray(env, wrap->worker_config.resource_limits);
}

napi_value WorkerImplLoopStartTime(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, -1, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value WorkerImplLoopIdleTime(napi_env env, napi_callback_info info) {
  napi_value out = nullptr;
  napi_create_int32(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value WorkerImplNoopTaker(napi_env env, napi_callback_info info) {
  return Undefined(env);
}

napi_value CreateWorkerCtor(napi_env env) {
  static constexpr napi_property_descriptor kProps[] = {
      {"startThread", nullptr, WorkerImplStartThread, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"stopThread", nullptr, WorkerImplStopThread, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"ref", nullptr, WorkerImplRef, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unref", nullptr, WorkerImplUnref, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getResourceLimits", nullptr, WorkerImplGetResourceLimits, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loopStartTime", nullptr, WorkerImplLoopStartTime, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loopIdleTime", nullptr, WorkerImplLoopIdleTime, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"takeHeapSnapshot", nullptr, WorkerImplNoopTaker, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getHeapStatistics", nullptr, WorkerImplNoopTaker, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"cpuUsage", nullptr, WorkerImplNoopTaker, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startCpuProfile", nullptr, WorkerImplNoopTaker, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"startHeapProfile", nullptr, WorkerImplNoopTaker, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value ctor = nullptr;
  if (napi_define_class(env,
                        "Worker",
                        NAPI_AUTO_LENGTH,
                        WorkerImplCtor,
                        nullptr,
                        sizeof(kProps) / sizeof(kProps[0]),
                        kProps,
                        &ctor) != napi_ok) {
    return nullptr;
  }
  return ctor;
}

napi_value GetOrCreateEnvMessagePort(napi_env env) {
  napi_value cached = UbiWorkerEnvGetEnvMessagePort(env);
  if (cached != nullptr && !IsNullOrUndefinedValue(env, cached)) return cached;
  UbiMessagePortDataPtr data = UbiWorkerEnvGetEnvMessagePortData(env);
  if (!data) return Undefined(env);
  napi_value port = UbiCreateMessagePortForData(env, data);
  if (port == nullptr || IsNullOrUndefinedValue(env, port)) return Undefined(env);
  UbiWorkerEnvSetEnvMessagePort(env, port);
  return port;
}

napi_value WorkerGetEnvMessagePort(napi_env env, napi_callback_info /*info*/) {
  return GetOrCreateEnvMessagePort(env);
}

}  // namespace

napi_value ResolveWorker(napi_env env, const ResolveOptions& /*options*/) {
  napi_value cached = UbiWorkerEnvGetBinding(env);
  if (cached != nullptr) return cached;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return Undefined(env);

  const bool is_main_thread = UbiWorkerEnvIsMainThread(env);
  SetBool(env, out, "isMainThread", is_main_thread);
  SetBool(env, out, "isInternalThread", UbiWorkerEnvIsInternalThread(env));
  SetBool(env, out, "ownsProcessState", UbiWorkerEnvOwnsProcessState(env));
  SetInt32(env, out, "threadId", UbiWorkerEnvThreadId(env));
  SetString(env, out, "threadName", UbiWorkerEnvThreadName(env).c_str());

  napi_value resource_limits = is_main_thread ? nullptr : BuildResourceLimitsArray(env, UbiWorkerEnvResourceLimits(env));
  if (resource_limits == nullptr) {
    napi_create_object(env, &resource_limits);
  }
  if (resource_limits != nullptr) napi_set_named_property(env, out, "resourceLimits", resource_limits);

  SetInt32(env, out, "kMaxYoungGenerationSizeMb", 0);
  SetInt32(env, out, "kMaxOldGenerationSizeMb", 1);
  SetInt32(env, out, "kCodeRangeSizeMb", 2);
  SetInt32(env, out, "kStackSizeMb", 3);
  SetInt32(env, out, "kTotalResourceLimitCount", 4);

  napi_value worker_ctor = CreateWorkerCtor(env);
  if (worker_ctor != nullptr) napi_set_named_property(env, out, "Worker", worker_ctor);

  napi_value get_env_message_port = nullptr;
  if (napi_create_function(env,
                           "getEnvMessagePort",
                           NAPI_AUTO_LENGTH,
                           WorkerGetEnvMessagePort,
                           nullptr,
                           &get_env_message_port) == napi_ok &&
      get_env_message_port != nullptr) {
    napi_set_named_property(env, out, "getEnvMessagePort", get_env_message_port);
  }

  UbiWorkerEnvSetBinding(env, out);
  return out;
}

}  // namespace internal_binding
