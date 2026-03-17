#include "internal/napi_jsc_env.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

struct ContextRecord {
  napi_ref sandbox_ref = nullptr;
  bool own_microtask_queue = false;
  bool allow_code_gen_strings = true;
  bool allow_code_gen_wasm = true;
};

std::mutex g_contextify_mu;
std::unordered_map<napi_env, std::vector<std::unique_ptr<ContextRecord>>> g_context_records;

bool DeleteRefIfPresent(napi_env env, napi_ref* ref_ptr) {
  if (env == nullptr || ref_ptr == nullptr || *ref_ptr == nullptr) return true;
  if (napi_delete_reference(env, *ref_ptr) != napi_ok) return false;
  *ref_ptr = nullptr;
  return true;
}

bool IsNullish(napi_env env, napi_value value) {
  if (env == nullptr || value == nullptr) return true;
  JSValueRef raw = napi_jsc_to_js_value(value);
  return JSValueIsNull(env->context, raw) || JSValueIsUndefined(env->context, raw);
}

bool IsObjectLike(napi_env env, napi_value value) {
  return env != nullptr && value != nullptr &&
         JSValueIsObject(env->context, napi_jsc_to_js_value(value));
}

bool GetContextMarkerSymbol(napi_env env, napi_value* result_out) {
  return napi_jsc_get_interned_private_symbol(
             env, "node:contextify:context", NAPI_AUTO_LENGTH, result_out) == napi_ok &&
         result_out != nullptr && *result_out != nullptr;
}

ContextRecord* FindRecordBySandbox(napi_env env, napi_value sandbox) {
  auto env_it = g_context_records.find(env);
  if (env_it == g_context_records.end()) return nullptr;
  for (const auto& record : env_it->second) {
    if (record == nullptr || record->sandbox_ref == nullptr) continue;
    napi_value current = nullptr;
    if (napi_get_reference_value(env, record->sandbox_ref, &current) != napi_ok || current == nullptr) {
      continue;
    }
    bool same = false;
    if (napi_strict_equals(env, current, sandbox, &same) == napi_ok && same) {
      return record.get();
    }
  }
  return nullptr;
}

bool MarkContextified(napi_env env, napi_value sandbox) {
  napi_value symbol = nullptr;
  napi_value marker = nullptr;
  return GetContextMarkerSymbol(env, &symbol) &&
         napi_get_boolean(env, true, &marker) == napi_ok &&
         marker != nullptr &&
         napi_set_property(env, sandbox, symbol, marker) == napi_ok;
}

bool UnmarkContextified(napi_env env, napi_value sandbox) {
  napi_value symbol = nullptr;
  bool deleted = false;
  return GetContextMarkerSymbol(env, &symbol) &&
         napi_delete_property(env, sandbox, symbol, &deleted) == napi_ok;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  JSValueRef exception = nullptr;
  return napi_jsc_value_to_utf8(env, value, &exception);
}

bool CreateString(napi_env env, const std::string& value, napi_value* result_out) {
  return napi_create_string_utf8(env, value.c_str(), value.size(), result_out) == napi_ok &&
         result_out != nullptr && *result_out != nullptr;
}

bool SetNamed(napi_env env, napi_value object, const char* name, napi_value value) {
  return napi_set_named_property(env, object, name, value) == napi_ok;
}

std::string StripCommentsAndStrings(std::string_view input) {
  enum class State {
    kCode,
    kSingleQuote,
    kDoubleQuote,
    kTemplate,
    kLineComment,
    kBlockComment,
  };

  State state = State::kCode;
  std::string out;
  out.reserve(input.size());

  for (size_t i = 0; i < input.size(); ++i) {
    const char ch = input[i];
    const char next = (i + 1 < input.size()) ? input[i + 1] : '\0';

    switch (state) {
      case State::kCode:
        if (ch == '\'' ) {
          state = State::kSingleQuote;
          out.push_back(' ');
        } else if (ch == '"') {
          state = State::kDoubleQuote;
          out.push_back(' ');
        } else if (ch == '`') {
          state = State::kTemplate;
          out.push_back(' ');
        } else if (ch == '/' && next == '/') {
          state = State::kLineComment;
          out.push_back(' ');
          ++i;
          out.push_back(' ');
        } else if (ch == '/' && next == '*') {
          state = State::kBlockComment;
          out.push_back(' ');
          ++i;
          out.push_back(' ');
        } else {
          out.push_back(ch);
        }
        break;
      case State::kSingleQuote:
        if (ch == '\\' && i + 1 < input.size()) {
          out.push_back(' ');
          ++i;
          out.push_back(' ');
        } else if (ch == '\'') {
          state = State::kCode;
          out.push_back(' ');
        } else {
          out.push_back(ch == '\n' ? '\n' : ' ');
        }
        break;
      case State::kDoubleQuote:
        if (ch == '\\' && i + 1 < input.size()) {
          out.push_back(' ');
          ++i;
          out.push_back(' ');
        } else if (ch == '"') {
          state = State::kCode;
          out.push_back(' ');
        } else {
          out.push_back(ch == '\n' ? '\n' : ' ');
        }
        break;
      case State::kTemplate:
        if (ch == '\\' && i + 1 < input.size()) {
          out.push_back(' ');
          ++i;
          out.push_back(' ');
        } else if (ch == '`') {
          state = State::kCode;
          out.push_back(' ');
        } else {
          out.push_back(ch == '\n' ? '\n' : ' ');
        }
        break;
      case State::kLineComment:
        if (ch == '\n') {
          state = State::kCode;
          out.push_back('\n');
        } else {
          out.push_back(' ');
        }
        break;
      case State::kBlockComment:
        if (ch == '*' && next == '/') {
          state = State::kCode;
          out.push_back(' ');
          ++i;
          out.push_back(' ');
        } else {
          out.push_back(ch == '\n' ? '\n' : ' ');
        }
        break;
    }
  }

  return out;
}

bool ContainsModuleSyntaxText(std::string_view source, bool cjs_var_in_scope) {
  const std::string stripped = StripCommentsAndStrings(source);
  const auto contains_word = [&](std::string_view needle) {
    return stripped.find(needle) != std::string::npos;
  };

  if (contains_word("import.meta")) return true;
  if (contains_word("export ")) return true;
  if (contains_word("export{")) return true;

  for (size_t i = 0; i < stripped.size(); ++i) {
    if (std::strncmp(stripped.c_str() + i, "import", 6) == 0) {
      const bool left_ok = (i == 0) || !std::isalnum(static_cast<unsigned char>(stripped[i - 1]));
      const char right = (i + 6 < stripped.size()) ? stripped[i + 6] : '\0';
      const bool right_ok = right == ' ' || right == '\t' || right == '\n' || right == '{' ||
                            right == '*' || right == '(' || right == '"' || right == '\'';
      if (left_ok && right_ok) return true;
    }
  }

  if (!cjs_var_in_scope && contains_word("await ")) return true;
  return false;
}

bool CreateSourceMapUrlValue(napi_env env, std::string_view code, napi_value* result_out) {
  constexpr std::string_view kNeedle1 = "//# sourceMappingURL=";
  constexpr std::string_view kNeedle2 = "//@ sourceMappingURL=";
  size_t pos = code.rfind(kNeedle1);
  size_t skip = kNeedle1.size();
  if (pos == std::string_view::npos) {
    pos = code.rfind(kNeedle2);
    skip = kNeedle2.size();
  }
  if (pos == std::string_view::npos) {
    return napi_get_undefined(env, result_out) == napi_ok;
  }
  std::string_view rest = code.substr(pos + skip);
  size_t end = rest.find_first_of("\r\n");
  rest = rest.substr(0, end);
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front()))) {
    rest.remove_prefix(1);
  }
  while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back()))) {
    rest.remove_suffix(1);
  }
  return CreateString(env, std::string(rest), result_out);
}

bool RunScriptInSandbox(napi_env env,
                        napi_value sandbox,
                        napi_value source,
                        napi_value filename,
                        napi_value* result_out) {
  static constexpr const char kHelper[] =
      "(function(sandbox, source, filename){"
      "  const outerGlobal = globalThis;"
      "  const sourceKey = '__napiJscContextifySource__';"
      "  if (sandbox == null || (typeof sandbox !== 'object' && typeof sandbox !== 'function')) {"
      "    throw new TypeError('Sandbox must be an object');"
      "  }"
      "  const proxy = new Proxy(sandbox, {"
      "    has(target, key) {"
      "      return key !== Symbol.unscopables;"
      "    },"
      "    get(target, key, receiver) {"
      "      if (key === 'globalThis' || key === 'self') return receiver;"
      "      if (Reflect.has(target, key)) return Reflect.get(target, key, receiver);"
      "      return outerGlobal[key];"
      "    },"
      "    set(target, key, value, receiver) {"
      "      return Reflect.set(target, key, value, receiver);"
      "    },"
      "    getOwnPropertyDescriptor(target, key) {"
      "      const own = Reflect.getOwnPropertyDescriptor(target, key);"
      "      if (own) return own;"
      "      const outer = Reflect.getOwnPropertyDescriptor(outerGlobal, key);"
      "      if (!outer) return undefined;"
      "      return { configurable: true, enumerable: !!outer.enumerable, writable: true, value: outerGlobal[key] };"
      "    }"
      "  });"
      "  sandbox.globalThis = proxy;"
      "  sandbox.self = proxy;"
      "  proxy[sourceKey] = String(source) + '\\n//# sourceURL=' + String(filename);"
      "  try {"
      "    return (function() { with (proxy) { return eval(__napiJscContextifySource__); } }).call(proxy);"
      "  } finally {"
      "    delete proxy[sourceKey];"
      "  }"
      "})";

  JSValueRef exception = nullptr;
  JSStringRef script = JSStringCreateWithUTF8CString(kHelper);
  JSValueRef helper_value = JSEvaluateScript(env->context, script, nullptr, nullptr, 0, &exception);
  JSStringRelease(script);
  if (!napi_jsc_check_exception(env, exception) || helper_value == nullptr ||
      !JSValueIsObject(env->context, helper_value)) {
    return false;
  }

  JSValueRef argv[3] = {napi_jsc_to_js_value(sandbox),
                        napi_jsc_to_js_value(source),
                        napi_jsc_to_js_value(filename)};
  JSValueRef raw_result = nullptr;
  if (!napi_jsc_call_function(env,
                              reinterpret_cast<JSObjectRef>(const_cast<OpaqueJSValue*>(helper_value)),
                              JSContextGetGlobalObject(env->context),
                              3,
                              argv,
                              &raw_result)) {
    return false;
  }
  *result_out = napi_jsc_to_napi(raw_result);
  return true;
}

bool BuildCompiledFunction(napi_env env,
                           napi_value code,
                           napi_value params,
                           napi_value* function_out) {
  if (env == nullptr || code == nullptr || function_out == nullptr) return false;
  napi_value global = nullptr;
  napi_value function_ctor = nullptr;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_named_property(env, global, "Function", &function_ctor) != napi_ok ||
      function_ctor == nullptr) {
    return false;
  }

  std::vector<napi_value> ctor_args;
  uint32_t param_count = 0;
  bool is_array = false;
  if (params != nullptr && !IsNullish(env, params) &&
      napi_is_array(env, params, &is_array) == napi_ok && is_array &&
      napi_get_array_length(env, params, &param_count) == napi_ok) {
    ctor_args.reserve(static_cast<size_t>(param_count) + 1);
    for (uint32_t i = 0; i < param_count; ++i) {
      napi_value param = nullptr;
      if (napi_get_element(env, params, i, &param) != napi_ok || param == nullptr ||
          napi_coerce_to_string(env, param, &param) != napi_ok) {
        return false;
      }
      ctor_args.push_back(param);
    }
  }
  ctor_args.push_back(code);
  return napi_new_instance(env,
                           function_ctor,
                           ctor_args.size(),
                           ctor_args.empty() ? nullptr : ctor_args.data(),
                           function_out) == napi_ok;
}

bool CreateCompileResultObject(napi_env env,
                               napi_value function,
                               napi_value cached_data,
                               bool cached_data_produced,
                               bool cached_data_rejected,
                               napi_value* result_out) {
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return false;

  napi_value produced = nullptr;
  napi_value rejected = nullptr;
  if (napi_get_boolean(env, cached_data_produced, &produced) != napi_ok ||
      napi_get_boolean(env, cached_data_rejected, &rejected) != napi_ok) {
    return false;
  }

  if (!SetNamed(env, out, "function", function) ||
      !SetNamed(env, out, "cachedDataProduced", produced) ||
      !SetNamed(env, out, "cachedDataRejected", rejected)) {
    return false;
  }
  if (cached_data != nullptr && !IsNullish(env, cached_data) && !SetNamed(env, out, "cachedData", cached_data)) {
    return false;
  }
  *result_out = out;
  return true;
}

}  // namespace

void napi_jsc_contextify_cleanup_env(napi_env env) {
  if (env == nullptr) return;
  std::lock_guard<std::mutex> lock(g_contextify_mu);
  auto env_it = g_context_records.find(env);
  if (env_it == g_context_records.end()) return;
  for (const auto& record : env_it->second) {
    if (record == nullptr) continue;
    if (env->context_token_unassign_callback != nullptr) {
      env->context_token_unassign_callback(env,
                                           const_cast<ContextRecord*>(record.get()),
                                           env->context_token_callback_data);
    }
    DeleteRefIfPresent(env, &record->sandbox_ref);
  }
  g_context_records.erase(env_it);
}

extern "C" {

napi_status NAPI_CDECL unofficial_napi_contextify_make_context(
    napi_env env,
    napi_value sandbox_or_symbol,
    napi_value /*name*/,
    napi_value /*origin_or_undefined*/,
    bool allow_code_gen_strings,
    bool allow_code_gen_wasm,
    bool own_microtask_queue,
    napi_value /*host_defined_option_id*/,
    napi_value* result_out) {
  if (env == nullptr || sandbox_or_symbol == nullptr || result_out == nullptr) return napi_invalid_arg;
  *result_out = nullptr;

  napi_value sandbox = sandbox_or_symbol;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, sandbox_or_symbol, &type) != napi_ok) return napi_generic_failure;
  if (type == napi_symbol) {
    if (napi_create_object(env, &sandbox) != napi_ok || sandbox == nullptr) {
      return napi_generic_failure;
    }
  } else if (type != napi_object && type != napi_function) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "An object was expected");
  }

  std::lock_guard<std::mutex> lock(g_contextify_mu);
  if (FindRecordBySandbox(env, sandbox) != nullptr) {
    *result_out = sandbox;
    return napi_jsc_clear_last_error(env);
  }

  auto record = std::make_unique<ContextRecord>();
  record->own_microtask_queue = own_microtask_queue;
  record->allow_code_gen_strings = allow_code_gen_strings;
  record->allow_code_gen_wasm = allow_code_gen_wasm;
  if (napi_create_reference(env, sandbox, 1, &record->sandbox_ref) != napi_ok ||
      record->sandbox_ref == nullptr) {
    return napi_generic_failure;
  }
  if (!MarkContextified(env, sandbox)) {
    DeleteRefIfPresent(env, &record->sandbox_ref);
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  if (env->context_token_assign_callback != nullptr) {
    env->context_token_assign_callback(env, record.get(), env->context_token_callback_data);
  }
  g_context_records[env].push_back(std::move(record));
  *result_out = sandbox;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_contextify_run_script(
    napi_env env,
    napi_value sandbox_or_null,
    napi_value source,
    napi_value filename,
    int32_t /*line_offset*/,
    int32_t /*column_offset*/,
    int64_t /*timeout*/,
    bool /*display_errors*/,
    bool /*break_on_sigint*/,
    bool /*break_on_first_line*/,
    napi_value /*host_defined_option_id*/,
    napi_value* result_out) {
  if (env == nullptr || source == nullptr || filename == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }

  if (IsNullish(env, sandbox_or_null)) {
    return napi_run_script(env, source, result_out);
  }
  if (!IsObjectLike(env, sandbox_or_null)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "An object was expected");
  }

  {
    std::lock_guard<std::mutex> lock(g_contextify_mu);
    if (FindRecordBySandbox(env, sandbox_or_null) == nullptr) {
      return napi_jsc_set_last_error(env, napi_invalid_arg, "Context has been disposed");
    }
  }

  napi_value source_string = source;
  napi_value filename_string = filename;
  if (napi_coerce_to_string(env, source_string, &source_string) != napi_ok ||
      napi_coerce_to_string(env, filename_string, &filename_string) != napi_ok) {
    return env->last_error.error_code;
  }

  if (!RunScriptInSandbox(env, sandbox_or_null, source_string, filename_string, result_out)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_contextify_dispose_context(
    napi_env env,
    napi_value sandbox_or_context_global) {
  if (env == nullptr || sandbox_or_context_global == nullptr) return napi_invalid_arg;
  std::lock_guard<std::mutex> lock(g_contextify_mu);
  auto env_it = g_context_records.find(env);
  if (env_it == g_context_records.end()) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Context has been disposed");
  }

  auto& records = env_it->second;
  for (auto it = records.begin(); it != records.end(); ++it) {
    napi_value current = nullptr;
    if ((*it) == nullptr || (*it)->sandbox_ref == nullptr ||
        napi_get_reference_value(env, (*it)->sandbox_ref, &current) != napi_ok ||
        current == nullptr) {
      continue;
    }
    bool same = false;
    if (napi_strict_equals(env, current, sandbox_or_context_global, &same) != napi_ok || !same) {
      continue;
    }
    (void)UnmarkContextified(env, current);
    if (env->context_token_unassign_callback != nullptr) {
      env->context_token_unassign_callback(env, it->get(), env->context_token_callback_data);
    }
    DeleteRefIfPresent(env, &(*it)->sandbox_ref);
    records.erase(it);
    if (records.empty()) g_context_records.erase(env_it);
    return napi_jsc_clear_last_error(env);
  }

  return napi_jsc_set_last_error(env, napi_invalid_arg, "Context has been disposed");
}

napi_status NAPI_CDECL unofficial_napi_contextify_compile_function(
    napi_env env,
    napi_value code,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value cached_data_or_undefined,
    bool produce_cached_data,
    napi_value /*parsing_context_or_undefined*/,
    napi_value /*context_extensions_or_undefined*/,
    napi_value params_or_undefined,
    napi_value host_defined_option_id,
    napi_value* result_out) {
  if (env == nullptr || code == nullptr || result_out == nullptr) return napi_invalid_arg;
  napi_value code_string = code;
  napi_value filename_string = filename;
  if (napi_coerce_to_string(env, code_string, &code_string) != napi_ok) return env->last_error.error_code;
  if (filename_string != nullptr && !IsNullish(env, filename_string) &&
      napi_coerce_to_string(env, filename_string, &filename_string) != napi_ok) {
    return env->last_error.error_code;
  }

  napi_value fn = nullptr;
  if (!BuildCompiledFunction(env, code_string, params_or_undefined, &fn) || fn == nullptr) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }

  napi_value cached_data = nullptr;
  if (produce_cached_data) {
    if (unofficial_napi_contextify_create_cached_data(env,
                                                      code_string,
                                                      filename_string != nullptr ? filename_string : code_string,
                                                      line_offset,
                                                      column_offset,
                                                      host_defined_option_id,
                                                      &cached_data) != napi_ok) {
      return env->last_error.error_code;
    }
  }

  const bool cached_data_rejected =
      cached_data_or_undefined != nullptr && !IsNullish(env, cached_data_or_undefined);
  if (!CreateCompileResultObject(
          env, fn, cached_data, produce_cached_data, cached_data_rejected, result_out)) {
    return napi_generic_failure;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_contextify_compile_function_for_cjs_loader(
    napi_env env,
    napi_value code,
    napi_value filename,
    bool /*is_sea_main*/,
    bool should_detect_module,
    napi_value* result_out) {
  if (env == nullptr || code == nullptr || filename == nullptr || result_out == nullptr) {
    return napi_invalid_arg;
  }

  napi_value params = nullptr;
  if (napi_create_array_with_length(env, 5, &params) != napi_ok || params == nullptr) {
    return napi_generic_failure;
  }
  const char* param_names[] = {"exports", "require", "module", "__filename", "__dirname"};
  for (uint32_t i = 0; i < 5; ++i) {
    napi_value name = nullptr;
    if (napi_create_string_utf8(env, param_names[i], NAPI_AUTO_LENGTH, &name) != napi_ok ||
        napi_set_element(env, params, i, name) != napi_ok) {
      return napi_generic_failure;
    }
  }

  napi_value code_string = code;
  napi_value filename_string = filename;
  if (napi_coerce_to_string(env, code_string, &code_string) != napi_ok ||
      napi_coerce_to_string(env, filename_string, &filename_string) != napi_ok) {
    return env->last_error.error_code;
  }

  bool can_parse_as_esm = false;
  if (should_detect_module) {
    const std::string text = ValueToUtf8(env, code_string);
    can_parse_as_esm = ContainsModuleSyntaxText(text, true);
  }

  napi_value compile_body = code_string;
  if (can_parse_as_esm) {
    if (napi_create_string_utf8(env, "return undefined;", NAPI_AUTO_LENGTH, &compile_body) != napi_ok ||
        compile_body == nullptr) {
      return env->last_error.error_code == napi_ok
                 ? napi_jsc_set_last_error(env, napi_generic_failure)
                 : env->last_error.error_code;
    }
  }

  napi_value fn = nullptr;
  if (!BuildCompiledFunction(env, compile_body, params, &fn) || fn == nullptr) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }

  napi_value out = nullptr;
  napi_value can_parse = nullptr;
  napi_value cached_data_rejected = nullptr;
  napi_value source_map_url = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr ||
      napi_get_boolean(env, can_parse_as_esm, &can_parse) != napi_ok ||
      napi_get_boolean(env, false, &cached_data_rejected) != napi_ok ||
      !CreateSourceMapUrlValue(env, ValueToUtf8(env, code_string), &source_map_url)) {
    return napi_generic_failure;
  }

  if (!SetNamed(env, out, "function", fn) ||
      !SetNamed(env, out, "canParseAsESM", can_parse) ||
      !SetNamed(env, out, "cachedDataRejected", cached_data_rejected) ||
      !SetNamed(env, out, "sourceURL", filename_string) ||
      !SetNamed(env, out, "sourceMapURL", source_map_url)) {
    return napi_generic_failure;
  }

  *result_out = out;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_contextify_contains_module_syntax(
    napi_env env,
    napi_value code,
    napi_value /*filename*/,
    napi_value /*resource_name_or_undefined*/,
    bool cjs_var_in_scope,
    bool* result_out) {
  if (env == nullptr || code == nullptr || result_out == nullptr) return napi_invalid_arg;
  napi_value code_string = code;
  if (napi_coerce_to_string(env, code_string, &code_string) != napi_ok) return env->last_error.error_code;
  *result_out = ContainsModuleSyntaxText(ValueToUtf8(env, code_string), cjs_var_in_scope);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL unofficial_napi_contextify_create_cached_data(
    napi_env env,
    napi_value code,
    napi_value filename,
    int32_t line_offset,
    int32_t column_offset,
    napi_value /*host_defined_option_id*/,
    napi_value* cached_data_buffer_out) {
  if (env == nullptr || code == nullptr || filename == nullptr || cached_data_buffer_out == nullptr) {
    return napi_invalid_arg;
  }

  std::string payload = "jsc-cache:";
  payload += ValueToUtf8(env, filename);
  payload.push_back(':');
  payload += std::to_string(line_offset);
  payload.push_back(':');
  payload += std::to_string(column_offset);
  payload.push_back(':');
  payload += ValueToUtf8(env, code);

  return napi_create_buffer_copy(env,
                                 payload.size(),
                                 payload.data(),
                                 nullptr,
                                 cached_data_buffer_out);
}

napi_status NAPI_CDECL unofficial_napi_contextify_start_sigint_watchdog(
    napi_env env,
    bool* result_out) {
  if (result_out != nullptr) *result_out = false;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_contextify_start_sigint_watchdog");
}

napi_status NAPI_CDECL unofficial_napi_contextify_stop_sigint_watchdog(
    napi_env env,
    bool* had_pending_signal_out) {
  if (had_pending_signal_out != nullptr) *had_pending_signal_out = false;
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_contextify_stop_sigint_watchdog");
}

napi_status NAPI_CDECL unofficial_napi_contextify_watchdog_has_pending_sigint(
    napi_env env,
    bool* result_out) {
  if (result_out != nullptr) *result_out = false;
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_contextify_watchdog_has_pending_sigint");
}

#define NAPI_JSC_UNSUPPORTED_MODULE_WRAP(api_name, ...)                        \
  napi_status NAPI_CDECL api_name(__VA_ARGS__) {                               \
    return napi_jsc_unofficial_unsupported(nullptr, #api_name);                \
  }

napi_status NAPI_CDECL unofficial_napi_module_wrap_create_source_text(
    napi_env env,
    napi_value /*wrapper*/,
    napi_value /*url*/,
    napi_value /*context_or_undefined*/,
    napi_value /*source*/,
    int32_t /*line_offset*/,
    int32_t /*column_offset*/,
    napi_value /*cached_data_or_id*/,
    void** /*handle_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_create_source_text");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_create_synthetic(
    napi_env env,
    napi_value /*wrapper*/,
    napi_value /*url*/,
    napi_value /*context_or_undefined*/,
    napi_value /*export_names*/,
    napi_value /*synthetic_eval_steps*/,
    void** /*handle_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_create_synthetic");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_destroy(napi_env env, void* /*handle*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_destroy");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_requests(
    napi_env env,
    void* /*handle*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_get_module_requests");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_link(
    napi_env env,
    void* /*handle*/,
    size_t /*count*/,
    void* const* /*linked_handles*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_link");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_instantiate(napi_env env, void* /*handle*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_instantiate");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_evaluate(
    napi_env env,
    void* /*handle*/,
    int64_t /*timeout*/,
    bool /*break_on_sigint*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_evaluate");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_evaluate_sync(
    napi_env env,
    void* /*handle*/,
    napi_value /*filename*/,
    napi_value /*parent_filename*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_evaluate_sync");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_get_namespace(
    napi_env env,
    void* /*handle*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_get_namespace");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_get_status(
    napi_env env,
    void* /*handle*/,
    int32_t* /*status_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_get_status");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_get_error(
    napi_env env,
    void* /*handle*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_get_error");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_has_top_level_await(
    napi_env env,
    void* /*handle*/,
    bool* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_has_top_level_await");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_has_async_graph(
    napi_env env,
    void* /*handle*/,
    bool* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_has_async_graph");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_check_unsettled_top_level_await(
    napi_env env,
    napi_value /*module_wrap*/,
    bool /*warnings*/,
    bool* /*settled_out*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_check_unsettled_top_level_await");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_set_export(
    napi_env env,
    void* /*handle*/,
    napi_value /*export_name*/,
    napi_value /*export_value*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_set_export");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_set_module_source_object(
    napi_env env,
    void* /*handle*/,
    napi_value /*source_object*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_set_module_source_object");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_get_module_source_object(
    napi_env env,
    void* /*handle*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_get_module_source_object");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_create_cached_data(
    napi_env env,
    void* /*handle*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(env, "unofficial_napi_module_wrap_create_cached_data");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_set_import_module_dynamically_callback(
    napi_env env,
    napi_value /*callback*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_set_import_module_dynamically_callback");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(
    napi_env env,
    napi_value /*callback*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_set_initialize_import_meta_object_callback");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_import_module_dynamically(
    napi_env env,
    size_t /*argc*/,
    napi_value* /*argv*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_import_module_dynamically");
}

napi_status NAPI_CDECL unofficial_napi_module_wrap_create_required_module_facade(
    napi_env env,
    void* /*handle*/,
    napi_value* /*result_out*/) {
  return napi_jsc_unofficial_unsupported(
      env, "unofficial_napi_module_wrap_create_required_module_facade");
}

}  // extern "C"
