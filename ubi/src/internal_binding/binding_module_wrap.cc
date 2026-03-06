#include "internal_binding/dispatch.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "internal_binding/helpers.h"
#include "unofficial_napi.h"
#include "../ubi_module_loader.h"

namespace internal_binding {

namespace {

enum ModuleWrapStatus : int32_t {
  kUninstantiated = 0,
  kInstantiating = 1,
  kInstantiated = 2,
  kEvaluating = 3,
  kEvaluated = 4,
  kErrored = 5,
};

constexpr int32_t kSourcePhase = 1;
constexpr int32_t kEvaluationPhase = 2;

struct ModuleWrapInstance {
  napi_ref wrapper_ref = nullptr;
  napi_ref namespace_ref = nullptr;
  napi_ref source_object_ref = nullptr;
  napi_ref url_ref = nullptr;
  napi_ref source_text_ref = nullptr;
  napi_ref synthetic_eval_steps_ref = nullptr;
  napi_ref linker_ref = nullptr;
  napi_ref error_ref = nullptr;
  void* module_handle = nullptr;
  int32_t status = kUninstantiated;
  int32_t phase = kEvaluationPhase;
  bool has_top_level_await = false;
  bool is_source_text_module = false;
};

struct ModuleWrapBindingState {
  napi_ref binding_ref = nullptr;
  napi_ref module_wrap_ctor_ref = nullptr;
  napi_ref import_module_dynamically_ref = nullptr;
  napi_ref initialize_import_meta_ref = nullptr;
};

std::unordered_map<napi_env, ModuleWrapBindingState> g_module_wrap_states;

ModuleWrapBindingState* GetBindingState(napi_env env) {
  auto it = g_module_wrap_states.find(env);
  if (it == g_module_wrap_states.end()) return nullptr;
  return &it->second;
}

napi_value GetRefValue(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value out = nullptr;
  if (napi_get_reference_value(env, ref, &out) != napi_ok || out == nullptr) return nullptr;
  return out;
}

void ResetRef(napi_env env, napi_ref* ref_ptr) {
  if (ref_ptr == nullptr || *ref_ptr == nullptr) return;
  napi_delete_reference(env, *ref_ptr);
  *ref_ptr = nullptr;
}

void SetRef(napi_env env, napi_ref* ref_ptr, napi_value value, napi_valuetype required) {
  if (ref_ptr == nullptr) return;
  ResetRef(env, ref_ptr);
  if (value == nullptr) return;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type != required) return;
  napi_create_reference(env, value, 1, ref_ptr);
}

ModuleWrapInstance* UnwrapModuleWrap(napi_env env, napi_value this_arg) {
  if (this_arg == nullptr) return nullptr;
  void* data = nullptr;
  if (napi_unwrap(env, this_arg, &data) != napi_ok || data == nullptr) return nullptr;
  return static_cast<ModuleWrapInstance*>(data);
}

void ModuleWrapFinalize(napi_env env, void* data, void* /*hint*/) {
  auto* instance = static_cast<ModuleWrapInstance*>(data);
  if (instance == nullptr) return;
  if (instance->module_handle != nullptr) {
    (void)unofficial_napi_module_wrap_destroy(env, instance->module_handle);
    instance->module_handle = nullptr;
  }
  ResetRef(env, &instance->wrapper_ref);
  ResetRef(env, &instance->namespace_ref);
  ResetRef(env, &instance->source_object_ref);
  ResetRef(env, &instance->url_ref);
  ResetRef(env, &instance->source_text_ref);
  ResetRef(env, &instance->synthetic_eval_steps_ref);
  ResetRef(env, &instance->linker_ref);
  ResetRef(env, &instance->error_ref);
  delete instance;
}

void SetNamedInt(napi_env env, napi_value obj, const char* key, int32_t value) {
  napi_value out = nullptr;
  if (napi_create_int32(env, value, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, obj, key, out);
  }
}

void SetNamedBool(napi_env env, napi_value obj, const char* key, bool value) {
  napi_value out = nullptr;
  if (napi_get_boolean(env, value, &out) == napi_ok && out != nullptr) {
    napi_set_named_property(env, obj, key, out);
  }
}

void SetNamedValue(napi_env env, napi_value obj, const char* key, napi_value value) {
  if (obj == nullptr || key == nullptr || value == nullptr) return;
  napi_set_named_property(env, obj, key, value);
}

void SetNamedMethod(napi_env env, napi_value obj, const char* key, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, key, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, key, fn);
  }
}

bool IsFunctionValue(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

std::string ValueToUtf8(napi_env env, napi_value value) {
  if (value == nullptr) return {};
  napi_value str = nullptr;
  if (napi_coerce_to_string(env, value, &str) != napi_ok || str == nullptr) return {};
  size_t len = 0;
  if (napi_get_value_string_utf8(env, str, nullptr, 0, &len) != napi_ok) return {};
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, str, out.data(), out.size(), &copied) != napi_ok) return {};
  out.resize(copied);
  return out;
}

bool StartsWithNodeScheme(const std::string& specifier) {
  return specifier.rfind("node:", 0) == 0;
}

std::string TrimAscii(std::string_view value) {
  size_t start = 0;
  size_t end = value.size();
  while (start < end && (value[start] == ' ' || value[start] == '\t' || value[start] == '\r')) start++;
  while (end > start &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == ';')) {
    end--;
  }
  return std::string(value.substr(start, end - start));
}

bool ParseQuotedLiteral(const std::string& text, std::string* out) {
  if (out == nullptr) return false;
  if (text.size() < 2) return false;
  const char quote = text.front();
  if ((quote != '\'' && quote != '"') || text.back() != quote) return false;
  *out = text.substr(1, text.size() - 2);
  return true;
}

bool ParseImportSideEffect(const std::string& trimmed, std::string* specifier_out) {
  // import 'specifier'
  constexpr std::string_view kPrefix = "import ";
  if (trimmed.rfind(kPrefix, 0) != 0) return false;
  const std::string rest = TrimAscii(trimmed.substr(kPrefix.size()));
  if (rest.empty() || rest[0] == '{' || rest.find(" from ") != std::string::npos) return false;
  return ParseQuotedLiteral(rest, specifier_out);
}

bool ParseImportNamed(const std::string& trimmed, std::string* destructuring_out, std::string* specifier_out) {
  // import { a, b as c } from 'specifier'
  constexpr std::string_view kPrefix = "import ";
  if (trimmed.rfind(kPrefix, 0) != 0) return false;
  const std::string rest = TrimAscii(trimmed.substr(kPrefix.size()));
  if (rest.empty() || rest.front() != '{') return false;
  const size_t close_brace = rest.find('}');
  if (close_brace == std::string::npos) return false;
  const std::string bindings = rest.substr(1, close_brace - 1);
  std::string after = TrimAscii(rest.substr(close_brace + 1));
  constexpr std::string_view kFrom = "from ";
  if (after.rfind(kFrom, 0) != 0) return false;
  after = TrimAscii(after.substr(kFrom.size()));
  std::string specifier;
  if (!ParseQuotedLiteral(after, &specifier)) return false;

  std::stringstream ss(bindings);
  std::string item;
  std::vector<std::string> parts;
  while (std::getline(ss, item, ',')) {
    item = TrimAscii(item);
    if (item.empty()) continue;
    const size_t as_pos = item.find(" as ");
    if (as_pos == std::string::npos) {
      parts.push_back(item);
    } else {
      const std::string imported = TrimAscii(item.substr(0, as_pos));
      const std::string local = TrimAscii(item.substr(as_pos + 4));
      if (imported.empty() || local.empty()) continue;
      parts.push_back(imported + ": " + local);
    }
  }
  if (parts.empty()) return false;

  std::string destructuring;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) destructuring += ", ";
    destructuring += parts[i];
  }
  if (destructuring_out != nullptr) *destructuring_out = destructuring;
  if (specifier_out != nullptr) *specifier_out = specifier;
  return true;
}

bool IsErrorCode(napi_env env, napi_value err, const char* code) {
  if (env == nullptr || err == nullptr || code == nullptr) return false;
  napi_value code_value = nullptr;
  if (napi_get_named_property(env, err, "code", &code_value) != napi_ok || code_value == nullptr) return false;
  const std::string actual = ValueToUtf8(env, code_value);
  return actual == code;
}

bool ReadTextFile(const std::string& path, std::string* out) {
  if (out == nullptr) return false;
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

napi_value BuildNamespaceFromExports(napi_env env, napi_value exports_value) {
  napi_value namespace_obj = nullptr;
  if (napi_create_object(env, &namespace_obj) != napi_ok || namespace_obj == nullptr) return nullptr;
  napi_set_named_property(env, namespace_obj, "default", exports_value);

  napi_valuetype exports_type = napi_undefined;
  if (napi_typeof(env, exports_value, &exports_type) != napi_ok ||
      (exports_type != napi_object && exports_type != napi_function)) {
    return namespace_obj;
  }

  napi_value keys = nullptr;
  if (napi_get_property_names(env, exports_value, &keys) != napi_ok || keys == nullptr) return namespace_obj;
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) return namespace_obj;
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    const std::string key_utf8 = ValueToUtf8(env, key);
    if (key_utf8.empty() || key_utf8 == "default") continue;
    napi_value value = nullptr;
    if (napi_get_property(env, exports_value, key, &value) != napi_ok || value == nullptr) continue;
    napi_set_property(env, namespace_obj, key, value);
  }
  return namespace_obj;
}

napi_value CreateRequireForUrl(napi_env env, napi_value url_value) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value require_fn = nullptr;
  if (napi_get_named_property(env, global, "require", &require_fn) != napi_ok || !IsFunctionValue(env, require_fn)) {
    return nullptr;
  }

  napi_value module_name = nullptr;
  if (napi_create_string_utf8(env, "module", NAPI_AUTO_LENGTH, &module_name) != napi_ok || module_name == nullptr) {
    return require_fn;
  }
  napi_value module_exports = nullptr;
  if (napi_call_function(env, global, require_fn, 1, &module_name, &module_exports) != napi_ok ||
      module_exports == nullptr) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return require_fn;
  }

  napi_value create_require = nullptr;
  if (napi_get_named_property(env, module_exports, "createRequire", &create_require) != napi_ok ||
      !IsFunctionValue(env, create_require)) {
    return require_fn;
  }

  napi_value created = nullptr;
  napi_value argv[1] = {url_value};
  if (napi_call_function(env, module_exports, create_require, 1, argv, &created) != napi_ok ||
      !IsFunctionValue(env, created)) {
    bool pending = false;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
      napi_value ignored = nullptr;
      napi_get_and_clear_last_exception(env, &ignored);
    }
    return require_fn;
  }
  return created;
}

bool EvaluateSimpleEsmFallback(napi_env env,
                               napi_value require_fn,
                               const std::string& module_url,
                               const std::string& source_text,
                               size_t depth,
                               napi_value* namespace_out);

bool RunSideEffectImport(napi_env env, napi_value require_fn, const std::string& specifier, size_t depth) {
  napi_value specifier_value = nullptr;
  if (napi_create_string_utf8(env, specifier.c_str(), NAPI_AUTO_LENGTH, &specifier_value) != napi_ok ||
      specifier_value == nullptr) {
    return false;
  }

  napi_value ignored_result = nullptr;
  if (napi_call_function(env, require_fn, require_fn, 1, &specifier_value, &ignored_result) == napi_ok) {
    return true;
  }

  bool has_pending = false;
  if (napi_is_exception_pending(env, &has_pending) != napi_ok || !has_pending) return false;

  napi_value err = nullptr;
  if (napi_get_and_clear_last_exception(env, &err) != napi_ok || err == nullptr) return false;
  if (!IsErrorCode(env, err, "ERR_REQUIRE_ESM")) {
    napi_throw(env, err);
    return false;
  }

  napi_value resolve_fn = nullptr;
  if (napi_get_named_property(env, require_fn, "resolve", &resolve_fn) != napi_ok || !IsFunctionValue(env, resolve_fn)) {
    napi_throw(env, err);
    return false;
  }

  napi_value resolved_value = nullptr;
  if (napi_call_function(env, require_fn, resolve_fn, 1, &specifier_value, &resolved_value) != napi_ok ||
      resolved_value == nullptr) {
    napi_throw(env, err);
    return false;
  }
  const std::string resolved_path = ValueToUtf8(env, resolved_value);
  if (resolved_path.empty()) {
    napi_throw(env, err);
    return false;
  }

  std::string nested_source;
  if (!ReadTextFile(resolved_path, &nested_source)) {
    napi_throw(env, err);
    return false;
  }

  napi_value resolved_url_value = nullptr;
  if (napi_create_string_utf8(env, resolved_path.c_str(), NAPI_AUTO_LENGTH, &resolved_url_value) != napi_ok ||
      resolved_url_value == nullptr) {
    napi_throw(env, err);
    return false;
  }
  napi_value nested_require = CreateRequireForUrl(env, resolved_url_value);
  if (!IsFunctionValue(env, nested_require)) {
    napi_throw(env, err);
    return false;
  }

  napi_value nested_namespace = nullptr;
  if (!EvaluateSimpleEsmFallback(
          env, nested_require, resolved_path, nested_source, depth + 1, &nested_namespace)) {
    return false;
  }
  return true;
}

bool EvaluateSimpleEsmFallback(napi_env env,
                               napi_value require_fn,
                               const std::string& module_url,
                               const std::string& source_text,
                               size_t depth,
                               napi_value* namespace_out) {
  if (namespace_out != nullptr) *namespace_out = nullptr;
  constexpr size_t kMaxFallbackDepth = 64;
  if (depth > kMaxFallbackDepth) {
    napi_throw_error(env, nullptr, "ModuleWrap ESM fallback exceeded recursion depth");
    return false;
  }

  std::vector<std::string> side_effect_imports;
  std::string transformed_body;
  bool unsupported = false;

  std::stringstream input(source_text);
  std::string line;
  while (std::getline(input, line)) {
    const std::string trimmed = TrimAscii(line);
    if (trimmed.rfind("import ", 0) == 0) {
      std::string side_effect_specifier;
      if (ParseImportSideEffect(trimmed, &side_effect_specifier)) {
        side_effect_imports.push_back(side_effect_specifier);
        continue;
      }

      std::string destructuring;
      std::string specifier;
      if (ParseImportNamed(trimmed, &destructuring, &specifier)) {
        transformed_body += "const { " + destructuring + " } = require(";
        transformed_body += "'";
        transformed_body += specifier;
        transformed_body += "'";
        transformed_body += ");\n";
        continue;
      }

      unsupported = true;
      break;
    }

    if (trimmed.rfind("export ", 0) == 0) {
      unsupported = true;
      break;
    }

    transformed_body += line;
    transformed_body.push_back('\n');
  }

  if (unsupported) {
    napi_value empty_exports = nullptr;
    napi_create_object(env, &empty_exports);
    napi_value ns = BuildNamespaceFromExports(env, empty_exports != nullptr ? empty_exports : Undefined(env));
    if (namespace_out != nullptr) *namespace_out = ns;
    return true;
  }

  for (const auto& side_effect_specifier : side_effect_imports) {
    if (!RunSideEffectImport(env, require_fn, side_effect_specifier, depth)) return false;
  }

  napi_value global = GetGlobal(env);
  if (global == nullptr) return false;
  napi_value function_ctor = nullptr;
  if (napi_get_named_property(env, global, "Function", &function_ctor) != napi_ok ||
      !IsFunctionValue(env, function_ctor)) {
    return false;
  }

  napi_value ctor_argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_create_string_utf8(env, "exports", NAPI_AUTO_LENGTH, &ctor_argv[0]);
  napi_create_string_utf8(env, "require", NAPI_AUTO_LENGTH, &ctor_argv[1]);
  napi_create_string_utf8(env, "module", NAPI_AUTO_LENGTH, &ctor_argv[2]);
  napi_create_string_utf8(env, "__filename", NAPI_AUTO_LENGTH, &ctor_argv[3]);
  napi_create_string_utf8(env, "__dirname", NAPI_AUTO_LENGTH, &ctor_argv[4]);
  std::string wrapped_body = "'use strict';\n" + transformed_body;
  if (!module_url.empty()) {
    wrapped_body += "\n//# sourceURL=" + module_url + "\n";
  }
  napi_create_string_utf8(env, wrapped_body.c_str(), wrapped_body.size(), &ctor_argv[5]);

  napi_value compiled_fn = nullptr;
  if (napi_new_instance(env, function_ctor, 6, ctor_argv, &compiled_fn) != napi_ok || compiled_fn == nullptr) {
    return false;
  }

  napi_value module_obj = nullptr;
  napi_value exports_obj = nullptr;
  if (napi_create_object(env, &module_obj) != napi_ok || module_obj == nullptr ||
      napi_create_object(env, &exports_obj) != napi_ok || exports_obj == nullptr) {
    return false;
  }
  napi_set_named_property(env, module_obj, "exports", exports_obj);

  std::string dirname = ".";
  if (!module_url.empty()) {
    std::error_code ec;
    std::filesystem::path p(module_url);
    auto parent = p.parent_path();
    if (!parent.empty()) dirname = parent.string();
    if (dirname.empty()) dirname = ".";
  }

  napi_value filename_value = nullptr;
  napi_value dirname_value = nullptr;
  napi_create_string_utf8(
      env, module_url.empty() ? "<module>" : module_url.c_str(), NAPI_AUTO_LENGTH, &filename_value);
  napi_create_string_utf8(env, dirname.c_str(), NAPI_AUTO_LENGTH, &dirname_value);

  napi_value run_argv[5] = {exports_obj, require_fn, module_obj, filename_value, dirname_value};
  napi_value ignored_result = nullptr;
  if (napi_call_function(env, global, compiled_fn, 5, run_argv, &ignored_result) != napi_ok) {
    return false;
  }

  napi_value final_exports = nullptr;
  if (napi_get_named_property(env, module_obj, "exports", &final_exports) != napi_ok || final_exports == nullptr) {
    final_exports = exports_obj;
  }

  napi_value ns = BuildNamespaceFromExports(env, final_exports);
  if (namespace_out != nullptr) *namespace_out = ns;
  return true;
}

napi_value EvaluateSourceTextModuleFallback(napi_env env, ModuleWrapInstance* instance) {
  if (instance == nullptr || instance->source_text_ref == nullptr) return nullptr;
  napi_value source_value = GetRefValue(env, instance->source_text_ref);
  napi_value url_value = GetRefValue(env, instance->url_ref);
  if (source_value == nullptr) return nullptr;

  const std::string source_text = ValueToUtf8(env, source_value);
  const std::string module_url = ValueToUtf8(env, url_value);
  napi_value require_fn = CreateRequireForUrl(env, url_value != nullptr ? url_value : source_value);
  if (!IsFunctionValue(env, require_fn)) return nullptr;

  napi_value namespace_obj = nullptr;
  if (!EvaluateSimpleEsmFallback(env, require_fn, module_url, source_text, 0, &namespace_obj)) {
    return nullptr;
  }

  if (namespace_obj == nullptr) {
    napi_value empty = nullptr;
    napi_create_object(env, &empty);
    namespace_obj = BuildNamespaceFromExports(env, empty != nullptr ? empty : Undefined(env));
  }
  return namespace_obj;
}

napi_value CreateCjsNamespaceFromRequire(napi_env env, const std::string& specifier) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return nullptr;

  napi_value require_fn = nullptr;
  if (napi_get_named_property(env, global, "require", &require_fn) != napi_ok || !IsFunctionValue(env, require_fn)) {
    return nullptr;
  }

  napi_value specifier_value = nullptr;
  if (napi_create_string_utf8(env, specifier.c_str(), NAPI_AUTO_LENGTH, &specifier_value) != napi_ok ||
      specifier_value == nullptr) {
    return nullptr;
  }

  napi_value exports_value = nullptr;
  if (napi_call_function(env, global, require_fn, 1, &specifier_value, &exports_value) != napi_ok ||
      exports_value == nullptr) {
    return nullptr;
  }

  napi_value namespace_obj = nullptr;
  if (napi_create_object(env, &namespace_obj) != napi_ok || namespace_obj == nullptr) {
    return nullptr;
  }
  napi_set_named_property(env, namespace_obj, "default", exports_value);

  napi_valuetype exports_type = napi_undefined;
  if (napi_typeof(env, exports_value, &exports_type) != napi_ok ||
      (exports_type != napi_object && exports_type != napi_function)) {
    return namespace_obj;
  }

  napi_value keys = nullptr;
  if (napi_get_property_names(env, exports_value, &keys) != napi_ok || keys == nullptr) {
    return namespace_obj;
  }
  uint32_t key_count = 0;
  if (napi_get_array_length(env, keys, &key_count) != napi_ok) {
    return namespace_obj;
  }
  for (uint32_t i = 0; i < key_count; ++i) {
    napi_value key = nullptr;
    if (napi_get_element(env, keys, i, &key) != napi_ok || key == nullptr) continue;
    const std::string key_utf8 = ValueToUtf8(env, key);
    if (key_utf8.empty() || key_utf8 == "default") continue;
    napi_value value = nullptr;
    if (napi_get_property(env, exports_value, key, &value) != napi_ok || value == nullptr) continue;
    napi_set_property(env, namespace_obj, key, value);
  }
  return namespace_obj;
}

napi_value GetVmDynamicImportDefaultInternalSymbol(napi_env env) {
  napi_value global = GetGlobal(env);
  if (global == nullptr) return Undefined(env);

  napi_value internal_binding = UbiGetInternalBinding(env);
  if (!IsFunctionValue(env, internal_binding) &&
      (napi_get_named_property(env, global, "internalBinding", &internal_binding) != napi_ok ||
       !IsFunctionValue(env, internal_binding))) {
    return Undefined(env);
  }

  napi_value symbols_name = nullptr;
  if (napi_create_string_utf8(env, "symbols", NAPI_AUTO_LENGTH, &symbols_name) != napi_ok ||
      symbols_name == nullptr) {
    return Undefined(env);
  }

  napi_value symbols_binding = nullptr;
  if (napi_call_function(env, global, internal_binding, 1, &symbols_name, &symbols_binding) != napi_ok ||
      symbols_binding == nullptr) {
    return Undefined(env);
  }

  napi_value symbol_value = nullptr;
  if (napi_get_named_property(
          env, symbols_binding, "vm_dynamic_import_default_internal", &symbol_value) != napi_ok ||
      symbol_value == nullptr) {
    return Undefined(env);
  }
  return symbol_value;
}

napi_value ModuleWrapCtor(napi_env env, napi_callback_info info) {
  size_t argc = 6;
  napi_value argv[6] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok || this_arg == nullptr) {
    return nullptr;
  }

  auto* instance = new ModuleWrapInstance();

  napi_value namespace_obj = nullptr;
  if (napi_create_object(env, &namespace_obj) == napi_ok && namespace_obj != nullptr) {
    napi_create_reference(env, namespace_obj, 1, &instance->namespace_ref);
  }

  if (argc >= 1 && argv[0] != nullptr) {
    napi_valuetype url_type = napi_undefined;
    if (napi_typeof(env, argv[0], &url_type) == napi_ok && url_type == napi_string) {
      napi_create_reference(env, argv[0], 1, &instance->url_ref);
      SetNamedValue(env, this_arg, "url", argv[0]);
    }
  }

  const bool has_exports_array = argc >= 3 && argv[2] != nullptr;
  bool is_array = false;
  if (has_exports_array && napi_is_array(env, argv[2], &is_array) == napi_ok && is_array) {
    SetNamedBool(env, this_arg, "synthetic", true);
    if (argc >= 4 && argv[3] != nullptr) {
      napi_valuetype t = napi_undefined;
      if (napi_typeof(env, argv[3], &t) == napi_ok && t == napi_function) {
        napi_create_reference(env, argv[3], 1, &instance->synthetic_eval_steps_ref);
      }
    }
    if (argc >= 4 && argv[3] != nullptr) {
      (void)unofficial_napi_module_wrap_create_synthetic(env,
                                                         this_arg,
                                                         argc >= 1 ? argv[0] : nullptr,
                                                         argc >= 2 ? argv[1] : nullptr,
                                                         argv[2],
                                                         argv[3],
                                                         &instance->module_handle);
    }
    uint32_t exports_len = 0;
    napi_get_array_length(env, argv[2], &exports_len);
    for (uint32_t i = 0; i < exports_len; ++i) {
      napi_value export_name = nullptr;
      if (napi_get_element(env, argv[2], i, &export_name) != napi_ok || export_name == nullptr) continue;
      napi_value undefined = Undefined(env);
      napi_set_property(env, namespace_obj, export_name, undefined);
    }
  } else if (has_exports_array) {
    SetNamedBool(env, this_arg, "synthetic", false);
    napi_valuetype source_type = napi_undefined;
    if (napi_typeof(env, argv[2], &source_type) == napi_ok && source_type == napi_string) {
      instance->is_source_text_module = true;
      napi_create_reference(env, argv[2], 1, &instance->source_text_ref);
      int32_t line_offset = 0;
      int32_t column_offset = 0;
      if (argc >= 4 && argv[3] != nullptr) (void)napi_get_value_int32(env, argv[3], &line_offset);
      if (argc >= 5 && argv[4] != nullptr) (void)napi_get_value_int32(env, argv[4], &column_offset);
      (void)unofficial_napi_module_wrap_create_source_text(env,
                                                           this_arg,
                                                           argc >= 1 ? argv[0] : nullptr,
                                                           argc >= 2 ? argv[1] : nullptr,
                                                           argv[2],
                                                           line_offset,
                                                           column_offset,
                                                           argc >= 6 ? argv[5] : nullptr,
                                                           &instance->module_handle);
      if (instance->module_handle != nullptr) {
        bool has_tla = false;
        if (unofficial_napi_module_wrap_has_top_level_await(env, instance->module_handle, &has_tla) == napi_ok) {
          instance->has_top_level_await = has_tla;
        }
      }
    }
  }

  SetNamedBool(env, this_arg, "hasTopLevelAwait", instance->has_top_level_await);

  napi_wrap(env, this_arg, instance, ModuleWrapFinalize, nullptr, &instance->wrapper_ref);
  return this_arg;
}

napi_value ModuleWrapLink(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);

  if (instance->module_handle != nullptr) {
    bool is_array = false;
    if (argc < 1 || argv[0] == nullptr || napi_is_array(env, argv[0], &is_array) != napi_ok || !is_array) {
      napi_throw_error(env, "ERR_INVALID_ARG_TYPE", "link() expects an array");
      return nullptr;
    }
    uint32_t length = 0;
    napi_get_array_length(env, argv[0], &length);
    std::vector<void*> linked_handles(length, nullptr);
    for (uint32_t i = 0; i < length; ++i) {
      napi_value module_value = nullptr;
      if (napi_get_element(env, argv[0], i, &module_value) != napi_ok || module_value == nullptr) {
        napi_throw_error(env, "ERR_VM_MODULE_LINK_FAILURE", "linked module missing");
        return nullptr;
      }
      ModuleWrapInstance* linked = UnwrapModuleWrap(env, module_value);
      linked_handles[i] = linked != nullptr ? linked->module_handle : nullptr;
    }
    if (unofficial_napi_module_wrap_link(env, instance->module_handle, length, linked_handles.data()) != napi_ok) {
      return nullptr;
    }
    return Undefined(env);
  }

  if (argc >= 1) SetRef(env, &instance->linker_ref, argv[0], napi_function);
  instance->status = kInstantiating;
  return Undefined(env);
}

napi_value ModuleWrapGetModuleRequests(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance != nullptr && instance->module_handle != nullptr) {
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_get_module_requests(env, instance->module_handle, &out) == napi_ok &&
        out != nullptr) {
      return out;
    }
    return Undefined(env);
  }
  napi_value out = nullptr;
  napi_create_array_with_length(env, 0, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapInstantiate(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance != nullptr && instance->module_handle != nullptr) {
    if (unofficial_napi_module_wrap_instantiate(env, instance->module_handle) != napi_ok) {
      return nullptr;
    }
    int32_t status = kInstantiated;
    (void)unofficial_napi_module_wrap_get_status(env, instance->module_handle, &status);
    instance->status = status;
    return Undefined(env);
  }
  if (instance != nullptr && instance->status <= kInstantiating) instance->status = kInstantiated;
  return Undefined(env);
}

napi_value ModuleWrapInstantiateSync(napi_env env, napi_callback_info info) {
  return ModuleWrapInstantiate(env, info);
}

napi_value ModuleWrapEvaluateSync(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);

  if (instance->module_handle != nullptr) {
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_evaluate_sync(env, instance->module_handle, &out) != napi_ok) {
      return nullptr;
    }
    int32_t status = kEvaluated;
    (void)unofficial_napi_module_wrap_get_status(env, instance->module_handle, &status);
    instance->status = status;
    return out != nullptr ? out : Undefined(env);
  }

  if (instance->is_source_text_module) {
    instance->status = kEvaluating;
    napi_value ns = EvaluateSourceTextModuleFallback(env, instance);
    if (ns == nullptr) {
      bool has_pending = false;
      if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
        napi_value err = nullptr;
        if (napi_get_and_clear_last_exception(env, &err) == napi_ok && err != nullptr) {
          ResetRef(env, &instance->error_ref);
          napi_create_reference(env, err, 1, &instance->error_ref);
          instance->status = kErrored;
          napi_throw(env, err);
          return nullptr;
        }
      }
      instance->status = kErrored;
      return nullptr;
    }
    ResetRef(env, &instance->namespace_ref);
    napi_create_reference(env, ns, 1, &instance->namespace_ref);
    instance->status = kEvaluated;
    return ns;
  }

  instance->status = kEvaluating;
  if (instance->synthetic_eval_steps_ref != nullptr) {
    napi_value fn = GetRefValue(env, instance->synthetic_eval_steps_ref);
    if (fn != nullptr) {
      napi_value ignored = nullptr;
      if (napi_call_function(env, this_arg, fn, 0, nullptr, &ignored) != napi_ok) {
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
          napi_value err = nullptr;
          napi_get_and_clear_last_exception(env, &err);
          ResetRef(env, &instance->error_ref);
          if (err != nullptr) napi_create_reference(env, err, 1, &instance->error_ref);
          instance->status = kErrored;
          napi_throw(env, err);
          return nullptr;
        }
      }
    }
  }
  instance->status = kEvaluated;
  return Undefined(env);
}

napi_value ModuleWrapEvaluate(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);

  if (instance->module_handle != nullptr) {
    int64_t timeout = -1;
    bool break_on_sigint = false;
    if (argc >= 1 && argv[0] != nullptr) (void)napi_get_value_int64(env, argv[0], &timeout);
    if (argc >= 2 && argv[1] != nullptr) (void)napi_get_value_bool(env, argv[1], &break_on_sigint);
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_evaluate(env, instance->module_handle, timeout, break_on_sigint, &out) != napi_ok) {
      return nullptr;
    }
    int32_t status = kEvaluated;
    (void)unofficial_napi_module_wrap_get_status(env, instance->module_handle, &status);
    instance->status = status;
    return out != nullptr ? out : Undefined(env);
  }

  if (instance->is_source_text_module) {
    napi_deferred deferred = nullptr;
    napi_value promise = nullptr;
    if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);

    instance->status = kEvaluating;
    napi_value ns = EvaluateSourceTextModuleFallback(env, instance);
    if (ns == nullptr) {
      bool has_pending = false;
      napi_value err = nullptr;
      if (napi_is_exception_pending(env, &has_pending) == napi_ok && has_pending) {
        napi_get_and_clear_last_exception(env, &err);
      }
      if (err == nullptr) err = Undefined(env);
      ResetRef(env, &instance->error_ref);
      if (err != nullptr && !IsUndefined(env, err)) {
        napi_create_reference(env, err, 1, &instance->error_ref);
      }
      instance->status = kErrored;
      napi_reject_deferred(env, deferred, err);
      return promise;
    }

    ResetRef(env, &instance->namespace_ref);
    napi_create_reference(env, ns, 1, &instance->namespace_ref);
    instance->status = kEvaluated;
    napi_resolve_deferred(env, deferred, Undefined(env));
    return promise;
  }

  napi_deferred deferred = nullptr;
  napi_value promise = nullptr;
  if (napi_create_promise(env, &deferred, &promise) != napi_ok || promise == nullptr) return Undefined(env);

  instance->status = kEvaluating;
  bool failed = false;
  napi_value err = nullptr;
  if (instance->synthetic_eval_steps_ref != nullptr) {
    napi_value fn = GetRefValue(env, instance->synthetic_eval_steps_ref);
    if (fn != nullptr) {
      napi_value ignored = nullptr;
      if (napi_call_function(env, this_arg, fn, 0, nullptr, &ignored) != napi_ok) {
        bool pending = false;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
          napi_get_and_clear_last_exception(env, &err);
          failed = true;
        }
      }
    }
  }

  if (failed) {
    ResetRef(env, &instance->error_ref);
    if (err != nullptr) napi_create_reference(env, err, 1, &instance->error_ref);
    instance->status = kErrored;
    napi_reject_deferred(env, deferred, err != nullptr ? err : Undefined(env));
    return promise;
  }

  instance->status = kEvaluated;
  napi_resolve_deferred(env, deferred, Undefined(env));
  return promise;
}

napi_value ModuleWrapSetExport(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr || argc < 1 || argv[0] == nullptr) return Undefined(env);
  if (instance->module_handle != nullptr) {
    if (unofficial_napi_module_wrap_set_export(
            env, instance->module_handle, argv[0], argc >= 2 ? argv[1] : Undefined(env)) != napi_ok) {
      return nullptr;
    }
    return Undefined(env);
  }
  napi_value namespace_obj = GetRefValue(env, instance->namespace_ref);
  if (namespace_obj == nullptr) return Undefined(env);
  napi_value value = argc >= 2 && argv[1] != nullptr ? argv[1] : Undefined(env);
  napi_set_property(env, namespace_obj, argv[0], value);
  return Undefined(env);
}

napi_value ModuleWrapSetModuleSourceObject(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  if (instance->module_handle != nullptr) {
    if (unofficial_napi_module_wrap_set_module_source_object(
            env, instance->module_handle, argc >= 1 ? argv[0] : nullptr) != napi_ok) {
      return nullptr;
    }
  }
  ResetRef(env, &instance->source_object_ref);
  if (argc >= 1 && argv[0] != nullptr) napi_create_reference(env, argv[0], 1, &instance->source_object_ref);
  instance->phase = kSourcePhase;
  return Undefined(env);
}

napi_value ModuleWrapGetModuleSourceObject(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  if (instance->module_handle != nullptr) {
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_get_module_source_object(env, instance->module_handle, &out) == napi_ok &&
        out != nullptr) {
      return out;
    }
  }
  napi_value out = GetRefValue(env, instance->source_object_ref);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapCreateCachedData(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance != nullptr && instance->module_handle != nullptr) {
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_create_cached_data(env, instance->module_handle, &out) == napi_ok &&
        out != nullptr) {
      return out;
    }
  }
  napi_value arraybuffer = nullptr;
  void* data = nullptr;
  if (napi_create_arraybuffer(env, 0, &data, &arraybuffer) != napi_ok || arraybuffer == nullptr) {
    return Undefined(env);
  }
  napi_value typed_array = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, 0, arraybuffer, 0, &typed_array) != napi_ok ||
      typed_array == nullptr) {
    return Undefined(env);
  }
  return typed_array;
}

napi_value ModuleWrapGetNamespace(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  if (instance->module_handle != nullptr) {
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_get_namespace(env, instance->module_handle, &out) == napi_ok && out != nullptr) {
      return out;
    }
  }
  napi_value out = GetRefValue(env, instance->namespace_ref);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapGetNamespaceSync(napi_env env, napi_callback_info info) {
  return ModuleWrapGetNamespace(env, info);
}

napi_value ModuleWrapGetStatus(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  if (instance->module_handle != nullptr) {
    int32_t status = kUninstantiated;
    if (unofficial_napi_module_wrap_get_status(env, instance->module_handle, &status) == napi_ok) {
      instance->status = status;
    }
  }
  napi_value out = nullptr;
  napi_create_int32(env, instance->status, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapGetError(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  if (instance == nullptr) return Undefined(env);
  if (instance->module_handle != nullptr) {
    napi_value out = nullptr;
    if (unofficial_napi_module_wrap_get_error(env, instance->module_handle, &out) == napi_ok && out != nullptr) {
      return out;
    }
  }
  napi_value out = GetRefValue(env, instance->error_ref);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapHasTopLevelAwait(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  bool value = instance != nullptr && instance->has_top_level_await;
  if (instance != nullptr && instance->module_handle != nullptr) {
    (void)unofficial_napi_module_wrap_has_top_level_await(env, instance->module_handle, &value);
  }
  napi_value out = nullptr;
  napi_get_boolean(env, value, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapHasAsyncGraph(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &this_arg, nullptr);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, this_arg);
  bool value = false;
  if (instance != nullptr && instance->module_handle != nullptr) {
    if (unofficial_napi_module_wrap_has_async_graph(env, instance->module_handle, &value) != napi_ok) {
      return nullptr;
    }
  }
  napi_value out = nullptr;
  napi_get_boolean(env, value, &out);
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapSetImportModuleDynamicallyCallback(napi_env env, napi_callback_info info) {
  auto* state = GetBindingState(env);
  if (state == nullptr) return Undefined(env);
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  SetRef(env, &state->import_module_dynamically_ref, argc >= 1 ? argv[0] : nullptr, napi_function);
  (void)unofficial_napi_module_wrap_set_import_module_dynamically_callback(env, argc >= 1 ? argv[0] : nullptr);
  return Undefined(env);
}

napi_value ModuleWrapSetInitializeImportMetaObjectCallback(napi_env env, napi_callback_info info) {
  auto* state = GetBindingState(env);
  if (state == nullptr) return Undefined(env);
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  SetRef(env, &state->initialize_import_meta_ref, argc >= 1 ? argv[0] : nullptr, napi_function);
  (void)unofficial_napi_module_wrap_set_initialize_import_meta_object_callback(env, argc >= 1 ? argv[0] : nullptr);
  return Undefined(env);
}

napi_value ModuleWrapImportModuleDynamically(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  napi_value result = nullptr;
  if (unofficial_napi_module_wrap_import_module_dynamically(env, argc, argv, &result) != napi_ok) {
    return nullptr;
  }
  return result != nullptr ? result : Undefined(env);
}

napi_value ModuleWrapCreateRequiredModuleFacade(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) return Undefined(env);
  ModuleWrapInstance* instance = UnwrapModuleWrap(env, argv[0]);
  if (instance == nullptr || instance->module_handle == nullptr) return argv[0];
  napi_value out = nullptr;
  if (unofficial_napi_module_wrap_create_required_module_facade(env, instance->module_handle, &out) != napi_ok) {
    return nullptr;
  }
  return out != nullptr ? out : Undefined(env);
}

napi_value ModuleWrapThrowIfPromiseRejected(napi_env env, napi_callback_info /*info*/) {
  return Undefined(env);
}

}  // namespace

napi_value ResolveModuleWrap(napi_env env, const ResolveOptions& /*options*/) {
  auto& state = g_module_wrap_states[env];
  if (state.binding_ref != nullptr) {
    napi_value existing = GetRefValue(env, state.binding_ref);
    if (existing != nullptr) return existing;
  }

  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return Undefined(env);

  SetNamedInt(env, binding, "kUninstantiated", kUninstantiated);
  SetNamedInt(env, binding, "kInstantiating", kInstantiating);
  SetNamedInt(env, binding, "kInstantiated", kInstantiated);
  SetNamedInt(env, binding, "kEvaluating", kEvaluating);
  SetNamedInt(env, binding, "kEvaluated", kEvaluated);
  SetNamedInt(env, binding, "kErrored", kErrored);
  SetNamedInt(env, binding, "kSourcePhase", kSourcePhase);
  SetNamedInt(env, binding, "kEvaluationPhase", kEvaluationPhase);

  napi_property_descriptor proto[] = {
      {"link", nullptr, ModuleWrapLink, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getModuleRequests", nullptr, ModuleWrapGetModuleRequests, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"instantiate", nullptr, ModuleWrapInstantiate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"instantiateSync", nullptr, ModuleWrapInstantiateSync, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"evaluate", nullptr, ModuleWrapEvaluate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"evaluateSync", nullptr, ModuleWrapEvaluateSync, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setExport", nullptr, ModuleWrapSetExport, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"setModuleSourceObject", nullptr, ModuleWrapSetModuleSourceObject, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"getModuleSourceObject", nullptr, ModuleWrapGetModuleSourceObject, nullptr, nullptr, nullptr, napi_default,
       nullptr},
      {"createCachedData", nullptr, ModuleWrapCreateCachedData, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getNamespace", nullptr, ModuleWrapGetNamespace, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getNamespaceSync", nullptr, ModuleWrapGetNamespaceSync, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getStatus", nullptr, ModuleWrapGetStatus, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getError", nullptr, ModuleWrapGetError, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"hasAsyncGraph", nullptr, nullptr, ModuleWrapHasAsyncGraph, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value module_wrap_ctor = nullptr;
  if (napi_define_class(env,
                        "ModuleWrap",
                        NAPI_AUTO_LENGTH,
                        ModuleWrapCtor,
                        nullptr,
                        sizeof(proto) / sizeof(proto[0]),
                        proto,
                        &module_wrap_ctor) != napi_ok ||
      module_wrap_ctor == nullptr) {
    return Undefined(env);
  }
  napi_set_named_property(env, binding, "ModuleWrap", module_wrap_ctor);
  napi_create_reference(env, module_wrap_ctor, 1, &state.module_wrap_ctor_ref);

  SetNamedMethod(env, binding, "setImportModuleDynamicallyCallback", ModuleWrapSetImportModuleDynamicallyCallback);
  SetNamedMethod(
      env, binding, "setInitializeImportMetaObjectCallback", ModuleWrapSetInitializeImportMetaObjectCallback);
  SetNamedMethod(env, binding, "importModuleDynamically", ModuleWrapImportModuleDynamically);
  SetNamedMethod(env, binding, "createRequiredModuleFacade", ModuleWrapCreateRequiredModuleFacade);
  SetNamedMethod(env, binding, "throwIfPromiseRejected", ModuleWrapThrowIfPromiseRejected);

  napi_create_reference(env, binding, 1, &state.binding_ref);
  return binding;
}

}  // namespace internal_binding
