#ifndef EDGE_BYTECODE_H_
#define EDGE_BYTECODE_H_

#include <cstddef>
#include <cstdint>
#include <utility>

#include "unofficial_napi.h"

// Move-only ownership for a provider bytecode artifact. Opening owns the whole
// cache-validation transaction: a supplied cache is either accepted or the
// same provider compiles the source fallback before returning.
class EdgeBytecode {
 public:
  EdgeBytecode() = default;
  EdgeBytecode(const EdgeBytecode&) = delete;
  EdgeBytecode& operator=(const EdgeBytecode&) = delete;

  EdgeBytecode(EdgeBytecode&& other) noexcept { MoveFrom(std::move(other)); }
  EdgeBytecode& operator=(EdgeBytecode&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(std::move(other));
    }
    return *this;
  }

  ~EdgeBytecode() { Reset(); }

  napi_status Open(napi_env env,
                   napi_value source_text,
                   napi_value filename,
                   int32_t shape,
                   napi_value params_or_undefined,
                   napi_value host_defined_option_id,
                   int32_t line_offset,
                   int32_t column_offset,
                   const uint8_t* cache_bytes = nullptr,
                   size_t cache_byte_length = 0,
                   bool has_cache = false,
                   bool* cache_rejected_out = nullptr,
                   bool* can_parse_as_module_out = nullptr,
                   bool compile_on_cache_rejection = true,
                   unofficial_napi_realm realm = nullptr) {
    Reset();
    if (cache_rejected_out != nullptr) *cache_rejected_out = false;
    if (can_parse_as_module_out != nullptr) *can_parse_as_module_out = false;

    unofficial_napi_bytecode_open_options options{};
    options.size = sizeof(options);
    options.version = UNOFFICIAL_NAPI_BYTECODE_OPEN_OPTIONS_VERSION;
    options.realm_or_null = realm;
    options.source_text = source_text;
    options.filename = filename;
    options.shape = shape;
    options.params_or_undefined = params_or_undefined;
    options.host_defined_option_id = host_defined_option_id;
    options.line_offset = line_offset;
    options.column_offset = column_offset;
    options.cache_bytes = cache_bytes;
    options.cache_byte_length = cache_byte_length;
    options.has_cache = has_cache ? 1 : 0;
    options.cache_policy =
        compile_on_cache_rejection
            ? unofficial_napi_bytecode_cache_compile_on_reject
            : unofficial_napi_bytecode_cache_validate_only;

    unofficial_napi_bytecode_open_result result{};
    const napi_status status = unofficial_napi_bytecode_open(env, &options, &result);
    if (cache_rejected_out != nullptr) *cache_rejected_out = result.cache_rejected != 0;
    if (can_parse_as_module_out != nullptr) {
      *can_parse_as_module_out = result.can_parse_as_module != 0;
    }
    if (status == napi_ok && result.bytecode != nullptr) {
      env_ = env;
      bytecode_ = result.bytecode;
    } else if (result.bytecode != nullptr) {
      (void)unofficial_napi_bytecode_release(env, result.bytecode);
    }
    return status;
  }

  napi_status Serialize(napi_value* buffer_out) const {
    if (env_ == nullptr || bytecode_ == nullptr || buffer_out == nullptr) {
      return napi_invalid_arg;
    }
    return unofficial_napi_bytecode_serialize(env_, bytecode_, buffer_out);
  }

  unofficial_napi_bytecode get() const { return bytecode_; }
  explicit operator bool() const { return bytecode_ != nullptr; }

  unofficial_napi_js_source source() const {
    return unofficial_napi_js_source_from_bytecode(bytecode_);
  }

  void Reset() {
    if (env_ != nullptr && bytecode_ != nullptr) {
      (void)unofficial_napi_bytecode_release(env_, bytecode_);
    }
    env_ = nullptr;
    bytecode_ = nullptr;
  }

 private:
  void MoveFrom(EdgeBytecode&& other) {
    env_ = other.env_;
    bytecode_ = other.bytecode_;
    other.env_ = nullptr;
    other.bytecode_ = nullptr;
  }

  napi_env env_ = nullptr;
  unofficial_napi_bytecode bytecode_ = nullptr;
};

#endif  // EDGE_BYTECODE_H_
