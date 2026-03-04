#include "internal_binding/dispatch.h"

#include "internal_binding/helpers.h"

namespace internal_binding {

napi_value ResolveSymbols(napi_env env, const ResolveOptions& /*options*/) {
  const napi_value undefined = Undefined(env);
  napi_value global = GetGlobal(env);
  if (global == nullptr) return undefined;

  bool has_binding = false;
  if (napi_has_named_property(env, global, "__ubi_symbols_binding", &has_binding) == napi_ok && has_binding) {
    napi_value existing = nullptr;
    if (napi_get_named_property(env, global, "__ubi_symbols_binding", &existing) == napi_ok &&
        existing != nullptr) {
      return existing;
    }
  }

  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok || out == nullptr) return undefined;

  auto set_symbol = [&](const char* key, const char* description) {
    napi_value desc = nullptr;
    napi_value sym = nullptr;
    if (napi_create_string_utf8(env, description, NAPI_AUTO_LENGTH, &desc) == napi_ok &&
        desc != nullptr &&
        napi_create_symbol(env, desc, &sym) == napi_ok &&
        sym != nullptr) {
      napi_set_named_property(env, out, key, sym);
    }
  };

  set_symbol("vm_dynamic_import_default_internal", "vm_dynamic_import_default_internal");
  set_symbol("vm_dynamic_import_main_context_default", "vm_dynamic_import_main_context_default");
  set_symbol("vm_dynamic_import_no_callback", "vm_dynamic_import_no_callback");
  set_symbol("vm_dynamic_import_missing_flag", "vm_dynamic_import_missing_flag");
  set_symbol("vm_context_no_contextify", "vm_context_no_contextify");
  set_symbol("source_text_module_default_hdo", "source_text_module_default_hdo");
  set_symbol("constructor_key_symbol", "constructor_key_symbol");
  set_symbol("fs_use_promises_symbol", "fs_use_promises_symbol");
  set_symbol("handle_onclose", "handle_onclose");
  set_symbol("resource_symbol", "resource_symbol");
  set_symbol("owner_symbol", "owner_symbol");
  set_symbol("async_id_symbol", "async_id_symbol");
  set_symbol("trigger_async_id_symbol", "trigger_async_id_symbol");
  set_symbol("oninit", "oninit");
  set_symbol("onpskexchange", "onpskexchange");
  set_symbol("messaging_deserialize_symbol", "messaging_deserialize_symbol");
  set_symbol("messaging_transfer_symbol", "messaging_transfer_symbol");
  set_symbol("messaging_clone_symbol", "messaging_clone_symbol");
  set_symbol("messaging_transfer_list_symbol", "messaging_transfer_list_symbol");
  set_symbol("no_message_symbol", "no_message_symbol");
  set_symbol("imported_cjs_symbol", "imported_cjs_symbol");

  napi_set_named_property(env, global, "__ubi_symbols_binding", out);
  return out;
}

}  // namespace internal_binding
