#include "edge_errors_binding.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "unofficial_napi.h"
#include "edge_worker_env.h"

namespace {

struct ErrorsBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref prepare_stack_trace_callback_ref = nullptr;
  napi_ref get_source_map_error_source_ref = nullptr;
  napi_ref maybe_cache_generated_source_map_ref = nullptr;
  napi_ref enhance_fatal_stack_before_inspector_ref = nullptr;
  napi_ref enhance_fatal_stack_after_inspector_ref = nullptr;
  bool source_maps_enabled = false;
};

std::unordered_map<napi_env, ErrorsBindingState> g_errors_states;
std::unordered_set<napi_env> g_errors_cleanup_hook_registered;

void DeleteRefIfPresent(napi_env env, napi_ref* ref) {
  if (env == nullptr || ref == nullptr || *ref == nullptr) return;
  napi_delete_reference(env, *ref);
  *ref = nullptr;
}

bool IsNullOrUndefinedValue(napi_env env, napi_value value) {
  if (value == nullptr) return true;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok &&
         (type == napi_undefined || type == napi_null);
}

void OnErrorsEnvCleanup(void* data) {
  napi_env env = static_cast<napi_env>(data);
  g_errors_cleanup_hook_registered.erase(env);

  auto it = g_errors_states.find(env);
  if (it == g_errors_states.end()) return;
  DeleteRefIfPresent(env, &it->second.binding_ref);
  DeleteRefIfPresent(env, &it->second.prepare_stack_trace_callback_ref);
  DeleteRefIfPresent(env, &it->second.get_source_map_error_source_ref);
  DeleteRefIfPresent(env, &it->second.maybe_cache_generated_source_map_ref);
  DeleteRefIfPresent(env, &it->second.enhance_fatal_stack_before_inspector_ref);
  DeleteRefIfPresent(env, &it->second.enhance_fatal_stack_after_inspector_ref);
  g_errors_states.erase(it);
}

void EnsureErrorsCleanupHook(napi_env env) {
  if (env == nullptr) return;
  auto [it, inserted] = g_errors_cleanup_hook_registered.emplace(env);
  if (!inserted) return;
  if (napi_add_env_cleanup_hook(env, OnErrorsEnvCleanup, env) != napi_ok) {
    g_errors_cleanup_hook_registered.erase(it);
  }
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  napi_value string_value = nullptr;
  if (napi_coerce_to_string(env, value, &string_value) != napi_ok || string_value == nullptr) {
    return "";
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string_value, nullptr, 0, &length) != napi_ok) {
    return "";
  }
  std::string out(length + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, string_value, out.data(), out.size(), &copied) != napi_ok) {
    return "";
  }
  out.resize(copied);
  return out;
}

std::string CallRefCallbackAsUtf8(napi_env env, napi_ref ref, napi_value arg) {
  if (env == nullptr || ref == nullptr || arg == nullptr) return "";

  napi_value cb = nullptr;
  if (napi_get_reference_value(env, ref, &cb) != napi_ok || cb == nullptr) {
    return "";
  }
  napi_valuetype cb_type = napi_undefined;
  if (napi_typeof(env, cb, &cb_type) != napi_ok || cb_type != napi_function) {
    return "";
  }

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    return "";
  }

  napi_value argv[1] = {arg};
  napi_value result = nullptr;
  if (napi_call_function(env, global, cb, 1, argv, &result) != napi_ok || result == nullptr) {
    return "";
  }
  return ValueToUtf8(env, result);
}

std::string FormatFatalExceptionAfterInspector(napi_env env, napi_value exception) {
  const auto it = g_errors_states.find(env);
  if (it == g_errors_states.end()) return "";
  return CallRefCallbackAsUtf8(env, it->second.enhance_fatal_stack_after_inspector_ref, exception);
}

napi_value MakeUndefined(napi_env env) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

void SetNamedString(napi_env env, napi_value obj, const char* key, const std::string& value) {
  napi_value js_value = nullptr;
  if (napi_create_string_utf8(env, value.c_str(), NAPI_AUTO_LENGTH, &js_value) == napi_ok &&
      js_value != nullptr) {
    napi_set_named_property(env, obj, key, js_value);
  }
}

void SetNamedInt32(napi_env env, napi_value obj, const char* key, int32_t value) {
  napi_value js_value = nullptr;
  if (napi_create_int32(env, value, &js_value) == napi_ok && js_value != nullptr) {
    napi_set_named_property(env, obj, key, js_value);
  }
}

napi_value ErrorsNoSideEffectsToString(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) return nullptr;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, argv[0], &type) != napi_ok) return MakeUndefined(env);

  // Preserve primitives and avoid invoking user-defined toString/valueOf on objects.
  if (type == napi_string) return argv[0];
  if (type == napi_number || type == napi_boolean || type == napi_bigint || type == napi_symbol ||
      type == napi_undefined || type == napi_null) {
    napi_value out = nullptr;
    if (napi_coerce_to_string(env, argv[0], &out) == napi_ok && out != nullptr) return out;
    return MakeUndefined(env);
  }

  napi_value script = nullptr;
  if (napi_create_string_utf8(
          env,
          "(function(v){ return Object.prototype.toString.call(v); })",
          NAPI_AUTO_LENGTH,
          &script) != napi_ok ||
      script == nullptr) {
    return MakeUndefined(env);
  }

  napi_value fn = nullptr;
  if (napi_run_script(env, script, &fn) != napi_ok || fn == nullptr) return MakeUndefined(env);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return MakeUndefined(env);

  napi_value out = nullptr;
  napi_value call_argv[1] = {argv[0]};
  if (napi_call_function(env, global, fn, 1, call_argv, &out) != napi_ok || out == nullptr) {
    return MakeUndefined(env);
  }
  return out;
}

void ErrorsSetRef(napi_env env, napi_ref* slot, napi_value value) {
  if (slot == nullptr) return;
  DeleteRefIfPresent(env, slot);
  napi_valuetype type = napi_undefined;
  if (value != nullptr && napi_typeof(env, value, &type) == napi_ok && type == napi_function) {
    napi_create_reference(env, value, 1, slot);
  }
}

napi_value ErrorsSetPrepareStackTraceCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  auto& st = g_errors_states[env];
  if (argc >= 1) {
    ErrorsSetRef(env, &st.prepare_stack_trace_callback_ref, argv[0]);
  }
  if (argc >= 1 && argv[0] != nullptr) {
    (void)unofficial_napi_set_prepare_stack_trace_callback(env, argv[0]);
  } else {
    (void)unofficial_napi_set_prepare_stack_trace_callback(env, nullptr);
  }
  return MakeUndefined(env);
}

napi_value ErrorsSetGetSourceMapErrorSource(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  auto& st = g_errors_states[env];
  if (argc >= 1) {
    ErrorsSetRef(env, &st.get_source_map_error_source_ref, argv[0]);
  }
  return MakeUndefined(env);
}

napi_value ErrorsSetSourceMapsEnabled(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  auto& st = g_errors_states[env];
  bool enabled = false;
  if (argc >= 1 && argv[0] != nullptr) {
    napi_get_value_bool(env, argv[0], &enabled);
  }
  st.source_maps_enabled = enabled;
  return MakeUndefined(env);
}

napi_value ErrorsSetMaybeCacheGeneratedSourceMap(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  auto& st = g_errors_states[env];
  if (argc >= 1) {
    ErrorsSetRef(env, &st.maybe_cache_generated_source_map_ref, argv[0]);
  }
  return MakeUndefined(env);
}

napi_value ErrorsSetEnhanceStackForFatalException(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  auto& st = g_errors_states[env];
  if (argc >= 1) {
    ErrorsSetRef(env, &st.enhance_fatal_stack_before_inspector_ref, argv[0]);
  }
  if (argc >= 2) {
    ErrorsSetRef(env, &st.enhance_fatal_stack_after_inspector_ref, argv[1]);
  }
  return MakeUndefined(env);
}

napi_value ErrorsGetErrorSourcePositions(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return nullptr;

  if (argc < 1 || argv[0] == nullptr) {
    SetNamedString(env, out, "sourceLine", "");
    SetNamedString(env, out, "scriptResourceName", "");
    SetNamedInt32(env, out, "lineNumber", 0);
    SetNamedInt32(env, out, "startColumn", 0);
    return out;
  }

  unofficial_napi_error_source_positions positions = {};
  if (unofficial_napi_get_error_source_positions(env, argv[0], &positions) != napi_ok) {
    SetNamedString(env, out, "sourceLine", "");
    SetNamedString(env, out, "scriptResourceName", "");
    SetNamedInt32(env, out, "lineNumber", 0);
    SetNamedInt32(env, out, "startColumn", 0);
    return out;
  }

  if (positions.source_line != nullptr) {
    napi_set_named_property(env, out, "sourceLine", positions.source_line);
  } else {
    SetNamedString(env, out, "sourceLine", "");
  }
  if (positions.script_resource_name != nullptr) {
    napi_set_named_property(env, out, "scriptResourceName", positions.script_resource_name);
  } else {
    SetNamedString(env, out, "scriptResourceName", "");
  }
  SetNamedInt32(env, out, "lineNumber", positions.line_number);
  SetNamedInt32(env, out, "startColumn", positions.start_column);
  return out;
}

napi_value ErrorsTriggerUncaughtException(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return nullptr;
  napi_value exception = nullptr;
  if (argc >= 1 && argv[0] != nullptr) {
    exception = argv[0];
  } else {
    napi_get_undefined(env, &exception);
  }
  bool from_promise = false;
  if (argc >= 2 && argv[1] != nullptr) {
    napi_get_value_bool(env, argv[1], &from_promise);
  }

  if (!EdgeWorkerEnvOwnsProcessState(env) && EdgeWorkerEnvStopRequested(env)) {
    bool has_pending = false;
    if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
      napi_value ignored = nullptr;
      (void)napi_get_and_clear_last_exception(env, &ignored);
    }
    (void)unofficial_napi_cancel_terminate_execution(env);
    return MakeUndefined(env);
  }

  auto invoke_ref_callback = [&](napi_ref ref) {
    if (ref == nullptr || IsNullOrUndefinedValue(env, exception)) return;
    napi_value cb = nullptr;
    if (napi_get_reference_value(env, ref, &cb) != napi_ok || cb == nullptr) return;
    napi_valuetype cb_type = napi_undefined;
    if (napi_typeof(env, cb, &cb_type) != napi_ok || cb_type != napi_function) return;
    napi_value global = nullptr;
    if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
    napi_value cb_argv[1] = {exception};
    napi_value ignored = nullptr;
    if (napi_call_function(env, global, cb, 1, cb_argv, &ignored) != napi_ok) {
      bool has_pending = false;
      if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
        napi_value thrown = nullptr;
        (void)napi_get_and_clear_last_exception(env, &thrown);
      }
    }
  };

  auto st_it = g_errors_states.find(env);
  if (st_it != g_errors_states.end()) {
    invoke_ref_callback(st_it->second.enhance_fatal_stack_before_inspector_ref);
  }

  napi_value global = nullptr;
  napi_value process = nullptr;
  if (napi_get_global(env, &global) == napi_ok &&
      global != nullptr &&
      napi_get_named_property(env, global, "process", &process) == napi_ok &&
      process != nullptr) {
    napi_value fatal_exception = nullptr;
    napi_valuetype t = napi_undefined;
    if (napi_get_named_property(env, process, "_fatalException", &fatal_exception) == napi_ok &&
        fatal_exception != nullptr &&
        napi_typeof(env, fatal_exception, &t) == napi_ok &&
        t == napi_function) {
      napi_value from_promise_value = nullptr;
      napi_get_boolean(env, from_promise, &from_promise_value);
      napi_value fatal_argv[2] = {exception, from_promise_value};
      napi_value fatal_result = nullptr;
      if (napi_call_function(env, process, fatal_exception, 2, fatal_argv, &fatal_result) != napi_ok) {
        if (!EdgeWorkerEnvOwnsProcessState(env) &&
            EdgeWorkerEnvStopRequested(env)) {
          bool has_pending = false;
          if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
            napi_value ignored = nullptr;
            (void)napi_get_and_clear_last_exception(env, &ignored);
          }
          (void)unofficial_napi_cancel_terminate_execution(env);
          if (st_it != g_errors_states.end()) {
            invoke_ref_callback(st_it->second.enhance_fatal_stack_after_inspector_ref);
          }
          return MakeUndefined(env);
        }
        return nullptr;
      }
      if (fatal_result != nullptr) {
        bool handled = false;
        if (napi_get_value_bool(env, fatal_result, &handled) == napi_ok && handled) {
          if (st_it != g_errors_states.end()) {
            invoke_ref_callback(st_it->second.enhance_fatal_stack_after_inspector_ref);
          }
          return MakeUndefined(env);
        }
      }
    }
  }

  if (st_it != g_errors_states.end()) {
    invoke_ref_callback(st_it->second.enhance_fatal_stack_after_inspector_ref);
  }

  // Match Node's native fatal path by preserving the original engine-provided
  // source arrow before the exception crosses back through JS.
  (void)unofficial_napi_preserve_error_source_message(env, exception);
  napi_throw(env, exception);
  return MakeUndefined(env);
}

napi_value GetOrCreateErrorsBinding(napi_env env) {
  EnsureErrorsCleanupHook(env);
  auto& st = g_errors_states[env];
  if (st.binding_ref != nullptr) {
    napi_value existing = nullptr;
    if (napi_get_reference_value(env, st.binding_ref, &existing) == napi_ok && existing != nullptr) {
      return existing;
    }
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;
  auto define_method = [&](const char* name, napi_callback cb) -> bool {
    napi_value fn = nullptr;
    return napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok &&
           fn != nullptr &&
           napi_set_named_property(env, binding, name, fn) == napi_ok;
  };
  if (!define_method("setPrepareStackTraceCallback", ErrorsSetPrepareStackTraceCallback) ||
      !define_method("setGetSourceMapErrorSource", ErrorsSetGetSourceMapErrorSource) ||
      !define_method("setSourceMapsEnabled", ErrorsSetSourceMapsEnabled) ||
      !define_method("setMaybeCacheGeneratedSourceMap", ErrorsSetMaybeCacheGeneratedSourceMap) ||
      !define_method("setEnhanceStackForFatalException", ErrorsSetEnhanceStackForFatalException) ||
      !define_method("noSideEffectsToString", ErrorsNoSideEffectsToString) ||
      !define_method("triggerUncaughtException", ErrorsTriggerUncaughtException) ||
      !define_method("getErrorSourcePositions", ErrorsGetErrorSourcePositions)) {
    return nullptr;
  }

  napi_value exit_codes = nullptr;
  if (napi_create_object(env, &exit_codes) != napi_ok || exit_codes == nullptr) return nullptr;
  auto set_const = [&](const char* name, int32_t value) -> bool {
    napi_value v = nullptr;
    return napi_create_int32(env, value, &v) == napi_ok &&
           v != nullptr &&
           napi_set_named_property(env, exit_codes, name, v) == napi_ok;
  };
  if (!set_const("kNoFailure", 0) ||
      !set_const("kGenericUserError", 1) ||
      !set_const("kInternalJSParseError", 3) ||
      !set_const("kInternalJSEvaluationFailure", 4) ||
      !set_const("kV8FatalError", 5) ||
      !set_const("kInvalidFatalExceptionMonkeyPatching", 6) ||
      !set_const("kExceptionInFatalExceptionHandler", 7) ||
      !set_const("kInvalidCommandLineArgument", 9) ||
      !set_const("kBootstrapFailure", 10) ||
      !set_const("kInvalidCommandLineArgument2", 12) ||
      !set_const("kUnsettledTopLevelAwait", 13) ||
      !set_const("kStartupSnapshotFailure", 14) ||
      !set_const("kAbort", 134)) {
    return nullptr;
  }
  if (napi_set_named_property(env, binding, "exitCodes", exit_codes) != napi_ok) return nullptr;

  DeleteRefIfPresent(env, &st.binding_ref);
  if (napi_create_reference(env, binding, 1, &st.binding_ref) != napi_ok || st.binding_ref == nullptr) {
    return nullptr;
  }
  return binding;
}

}  // namespace

napi_value EdgeGetOrCreateErrorsBinding(napi_env env) {
  return GetOrCreateErrorsBinding(env);
}

std::string EdgeFormatFatalExceptionAfterInspector(napi_env env, napi_value exception) {
  return FormatFatalExceptionAfterInspector(env, exception);
}
