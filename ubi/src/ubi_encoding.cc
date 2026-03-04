#include "ubi_encoding.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "ada.h"
#include "simdutf.h"

namespace {

napi_ref g_encode_into_results_ref = nullptr;

bool IsBase64Char(char c, bool url_mode) {
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
    return true;
  }
  if (c == '=') return true;
  if (c == '+' || c == '/') return true;
  if (url_mode && (c == '-' || c == '_')) return true;
  return false;
}

std::string NormalizeForgivingBase64(const std::string& input, bool url_mode) {
  std::string compact;
  compact.reserve(input.size());
  for (char c : input) {
    if (c == '=') break;
    if (!IsBase64Char(c, url_mode)) continue;
    if (url_mode) {
      if (c == '-') c = '+';
      else if (c == '_') c = '/';
    }
    compact.push_back(c);
  }

  const size_t rem = compact.size() % 4;
  if (rem == 1) return "";
  if (rem == 2) compact.append("==");
  else if (rem == 3) compact.push_back('=');
  return compact;
}

napi_value GetUndefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

bool ExtractBytesFromValue(napi_env env, napi_value value, const char** data, size_t* len) {
  if (value == nullptr || data == nullptr || len == nullptr) return false;

  bool is_buffer = false;
  if (napi_is_buffer(env, value, &is_buffer) == napi_ok && is_buffer) {
    void* ptr = nullptr;
    if (napi_get_buffer_info(env, value, &ptr, len) != napi_ok || ptr == nullptr) return false;
    *data = static_cast<const char*>(ptr);
    return true;
  }

  bool is_arraybuffer = false;
  if (napi_is_arraybuffer(env, value, &is_arraybuffer) == napi_ok && is_arraybuffer) {
    void* ptr = nullptr;
    if (napi_get_arraybuffer_info(env, value, &ptr, len) != napi_ok) return false;
    *data = static_cast<const char*>(ptr);
    return true;
  }

  bool is_typed = false;
  if (napi_is_typedarray(env, value, &is_typed) == napi_ok && is_typed) {
    napi_typedarray_type type = napi_uint8_array;
    size_t element_len = 0;
    void* ptr = nullptr;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_typedarray_info(
            env, value, &type, &element_len, &ptr, &arraybuffer, &byte_offset) != napi_ok ||
        ptr == nullptr) {
      return false;
    }

    size_t bytes_per_element = 1;
    switch (type) {
      case napi_int16_array:
      case napi_uint16_array:
        bytes_per_element = 2;
        break;
      case napi_int32_array:
      case napi_uint32_array:
      case napi_float32_array:
        bytes_per_element = 4;
        break;
      case napi_float64_array:
        bytes_per_element = 8;
        break;
      default:
        bytes_per_element = 1;
        break;
    }
    *data = static_cast<const char*>(ptr);
    *len = element_len * bytes_per_element;
    return true;
  }

  bool is_dataview = false;
  if (napi_is_dataview(env, value, &is_dataview) == napi_ok && is_dataview) {
    void* ptr = nullptr;
    size_t byte_length = 0;
    napi_value arraybuffer = nullptr;
    size_t byte_offset = 0;
    if (napi_get_dataview_info(env, value, &byte_length, &ptr, &arraybuffer, &byte_offset) != napi_ok ||
        ptr == nullptr) {
      return false;
    }
    *data = static_cast<const char*>(ptr);
    *len = byte_length;
    return true;
  }

  return false;
}

napi_value MakeUint8Array(napi_env env, const char* data, size_t len) {
  napi_value arraybuffer = nullptr;
  void* out = nullptr;
  if (napi_create_arraybuffer(env, len, &out, &arraybuffer) != napi_ok || arraybuffer == nullptr) {
    return nullptr;
  }
  if (len > 0 && out != nullptr && data != nullptr) {
    std::memcpy(out, data, len);
  }
  napi_value typed = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, len, arraybuffer, 0, &typed) != napi_ok) {
    return nullptr;
  }
  return typed;
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

void SetNamedString(napi_env env, napi_value obj, const char* key, std::string_view value) {
  napi_value v = nullptr;
  if (napi_create_string_utf8(env, value.data(), value.size(), &v) == napi_ok && v != nullptr) {
    napi_set_named_property(env, obj, key, v);
  }
}

void ThrowTypeErrorWithCode(napi_env env, std::string_view message, std::string_view code) {
  napi_value msg = nullptr;
  napi_value err = nullptr;
  if (napi_create_string_utf8(env, message.data(), message.size(), &msg) != napi_ok ||
      napi_create_type_error(env, nullptr, msg, &err) != napi_ok || err == nullptr) {
    return;
  }
  SetNamedString(env, err, "code", code);
  napi_throw(env, err);
}

napi_value GetEncodeIntoResultsArray(napi_env env) {
  if (g_encode_into_results_ref == nullptr) return nullptr;
  napi_value arr = nullptr;
  if (napi_get_reference_value(env, g_encode_into_results_ref, &arr) != napi_ok || arr == nullptr) return nullptr;
  return arr;
}

void UpdateEncodeIntoResults(napi_env env, uint32_t read, uint32_t written) {
  napi_value arr = GetEncodeIntoResultsArray(env);
  if (arr == nullptr) return;
  napi_value v_read = nullptr;
  napi_value v_written = nullptr;
  if (napi_create_uint32(env, read, &v_read) == napi_ok && v_read != nullptr) {
    napi_set_element(env, arr, 0, v_read);
  }
  if (napi_create_uint32(env, written, &v_written) == napi_ok && v_written != nullptr) {
    napi_set_element(env, arr, 1, v_written);
  }
}

size_t WriteCodePointUtf8(uint32_t cp, char* out) {
  if (cp <= 0x7F) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp <= 0x7FF) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

size_t Utf8LengthForCodePoint(uint32_t cp) {
  if (cp <= 0x7F) return 1;
  if (cp <= 0x7FF) return 2;
  if (cp <= 0xFFFF) return 3;
  return 4;
}

void DecodeUtf8Lossy(const uint8_t* data, size_t len, std::vector<char16_t>* out) {
  out->clear();
  out->reserve(len);
  size_t i = 0;
  while (i < len) {
    uint32_t cp = 0xFFFD;
    uint8_t b0 = data[i];

    if (b0 < 0x80) {
      cp = b0;
      i += 1;
    } else if (b0 >= 0xC2 && b0 <= 0xDF) {
      if (i + 1 < len) {
        uint8_t b1 = data[i + 1];
        if ((b1 & 0xC0) == 0x80) {
          cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
          i += 2;
        } else {
          i += 1;
        }
      } else {
        i += 1;
      }
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      if (i + 2 < len) {
        uint8_t b1 = data[i + 1];
        uint8_t b2 = data[i + 2];
        bool ok = (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80;
        ok = ok && !((b0 == 0xE0) && (b1 < 0xA0));
        ok = ok && !((b0 == 0xED) && (b1 > 0x9F));
        if (ok) {
          cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
          i += 3;
        } else {
          i += 1;
        }
      } else {
        i += 1;
      }
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      if (i + 3 < len) {
        uint8_t b1 = data[i + 1];
        uint8_t b2 = data[i + 2];
        uint8_t b3 = data[i + 3];
        bool ok = (b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80;
        ok = ok && !((b0 == 0xF0) && (b1 < 0x90));
        ok = ok && !((b0 == 0xF4) && (b1 > 0x8F));
        if (ok) {
          cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
          i += 4;
        } else {
          i += 1;
        }
      } else {
        i += 1;
      }
    } else {
      i += 1;
    }

    if (cp <= 0xFFFF) {
      out->push_back(static_cast<char16_t>(cp));
    } else {
      cp -= 0x10000;
      out->push_back(static_cast<char16_t>(0xD800 + ((cp >> 10) & 0x3FF)));
      out->push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
  }
}

bool DecodeUTF8ToString(
    napi_env env,
    const char* bytes,
    size_t len,
    bool ignore_bom,
    bool fatal,
    napi_value* out) {
  if (bytes == nullptr || out == nullptr) return false;
  const char* data = bytes;
  size_t length = len;

  if (!ignore_bom && length >= 3 &&
      static_cast<unsigned char>(data[0]) == 0xEF &&
      static_cast<unsigned char>(data[1]) == 0xBB &&
      static_cast<unsigned char>(data[2]) == 0xBF) {
    data += 3;
    length -= 3;
  }

  if (fatal && !simdutf::validate_utf8(data, length)) {
    ThrowTypeErrorWithCode(env, "The encoded data was not valid for encoding utf-8",
                           "ERR_ENCODING_INVALID_ENCODED_DATA");
    return false;
  }

  if (length == 0) {
    napi_create_string_utf8(env, "", 0, out);
    return true;
  }

  if (simdutf::validate_utf8(data, length)) {
    const size_t utf16_len = simdutf::utf16_length_from_utf8(data, length);
    std::vector<char16_t> utf16(utf16_len);
    const size_t written = simdutf::convert_utf8_to_utf16(data, length, utf16.data());
    if (written == utf16_len && napi_create_string_utf16(env, utf16.data(), utf16.size(), out) == napi_ok) {
      return true;
    }
  }

  std::vector<char16_t> utf16_lossy;
  DecodeUtf8Lossy(reinterpret_cast<const uint8_t*>(data), length, &utf16_lossy);
  return napi_create_string_utf16(env, utf16_lossy.data(), utf16_lossy.size(), out) == napi_ok;
}

napi_value BindingEncodeUtf8String(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }

  size_t utf16_len = 0;
  if (napi_get_value_string_utf16(env, argv[0], nullptr, 0, &utf16_len) != napi_ok) {
    return GetUndefined(env);
  }
  std::vector<char16_t> input(utf16_len + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf16(env, argv[0], input.data(), input.size(), &copied) != napi_ok) {
    return GetUndefined(env);
  }

  const size_t out_len = simdutf::utf8_length_from_utf16(input.data(), copied);
  std::vector<char> out(out_len);
  const size_t written = simdutf::convert_utf16_to_utf8(input.data(), copied, out.data());
  if (written != out_len) return GetUndefined(env);
  return MakeUint8Array(env, out.data(), out.size());
}

napi_value BindingEncodeInto(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 2) {
    return GetUndefined(env);
  }

  size_t utf16_len = 0;
  if (napi_get_value_string_utf16(env, argv[0], nullptr, 0, &utf16_len) != napi_ok) {
    return GetUndefined(env);
  }
  std::vector<char16_t> input(utf16_len + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf16(env, argv[0], input.data(), input.size(), &copied) != napi_ok) {
    return GetUndefined(env);
  }

  bool is_typed = false;
  if (napi_is_typedarray(env, argv[1], &is_typed) != napi_ok || !is_typed) {
    return GetUndefined(env);
  }
  napi_typedarray_type type = napi_uint8_array;
  size_t element_len = 0;
  void* ptr = nullptr;
  napi_value arraybuffer = nullptr;
  size_t byte_offset = 0;
  if (napi_get_typedarray_info(env, argv[1], &type, &element_len, &ptr, &arraybuffer, &byte_offset) != napi_ok ||
      ptr == nullptr) {
    return GetUndefined(env);
  }

  char* dest = static_cast<char*>(ptr);
  size_t dest_len = element_len;
  uint32_t read = 0;
  uint32_t written = 0;

  for (size_t i = 0; i < copied;) {
    uint32_t cp = input[i];
    size_t consumed = 1;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
      if (i + 1 < copied) {
        uint32_t lo = input[i + 1];
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          consumed = 2;
        } else {
          cp = 0xFFFD;
        }
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }

    const size_t bytes_needed = Utf8LengthForCodePoint(cp);
    if (written + bytes_needed > dest_len) break;
    WriteCodePointUtf8(cp, dest + written);
    written += static_cast<uint32_t>(bytes_needed);
    read += static_cast<uint32_t>(consumed);
    i += consumed;
  }

  UpdateEncodeIntoResults(env, read, written);
  return GetUndefined(env);
}

napi_value BindingUtf8ByteLength(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }
  size_t utf16_len = 0;
  if (napi_get_value_string_utf16(env, argv[0], nullptr, 0, &utf16_len) != napi_ok) {
    return GetUndefined(env);
  }
  std::vector<char16_t> input(utf16_len + 1);
  size_t copied = 0;
  if (napi_get_value_string_utf16(env, argv[0], input.data(), input.size(), &copied) != napi_ok) {
    return GetUndefined(env);
  }
  const size_t out_len = simdutf::utf8_length_from_utf16(input.data(), copied);
  napi_value out = nullptr;
  if (napi_create_uint32(env, static_cast<uint32_t>(out_len), &out) != napi_ok) return GetUndefined(env);
  return out;
}

napi_value BindingValidateUtf8(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }
  const char* data = nullptr;
  size_t len = 0;
  if (!ExtractBytesFromValue(env, argv[0], &data, &len)) return GetUndefined(env);
  napi_value out = nullptr;
  if (napi_get_boolean(env, simdutf::validate_utf8(data, len), &out) != napi_ok) return GetUndefined(env);
  return out;
}

napi_value BindingDecodeUTF8(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }

  const char* data = nullptr;
  size_t len = 0;
  if (!ExtractBytesFromValue(env, argv[0], &data, &len)) {
    ThrowTypeErrorWithCode(
        env,
        "The \"list\" argument must be an instance of SharedArrayBuffer, ArrayBuffer or ArrayBufferView.",
        "ERR_INVALID_ARG_TYPE");
    return GetUndefined(env);
  }

  bool ignore_bom = false;
  bool fatal = false;
  if (argc > 1) napi_get_value_bool(env, argv[1], &ignore_bom);
  if (argc > 2) napi_get_value_bool(env, argv[2], &fatal);

  napi_value out = nullptr;
  if (!DecodeUTF8ToString(env, data, len, ignore_bom, fatal, &out)) return GetUndefined(env);
  return out;
}

napi_value BindingDecodeUtf8(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }

  const char* data = nullptr;
  size_t len = 0;
  if (!ExtractBytesFromValue(env, argv[0], &data, &len)) return GetUndefined(env);
  napi_value out = nullptr;
  if (!DecodeUTF8ToString(env, data, len, true, false, &out)) return GetUndefined(env);
  return out;
}

napi_value BindingEncodeBase64(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }
  const char* data = nullptr;
  size_t len = 0;
  if (!ExtractBytesFromValue(env, argv[0], &data, &len)) return GetUndefined(env);
  bool is_url = false;
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_bool(env, argv[1], &is_url);

  const size_t out_len = is_url
                             ? simdutf::base64_length_from_binary(len, simdutf::base64_url)
                             : simdutf::base64_length_from_binary(len);
  std::string out(out_len, '\0');
  const size_t written = is_url
                             ? simdutf::binary_to_base64(data, len, out.data(), simdutf::base64_url)
                             : simdutf::binary_to_base64(data, len, out.data());
  if (written != out_len) return GetUndefined(env);
  napi_value str = nullptr;
  if (napi_create_string_utf8(env, out.data(), written, &str) != napi_ok) return GetUndefined(env);
  return str;
}

napi_value BindingDecodeBase64(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }

  size_t in_len = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &in_len) != napi_ok) return GetUndefined(env);
  std::string input(in_len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[0], input.data(), input.size(), &copied) != napi_ok) return GetUndefined(env);
  input.resize(copied);

  bool is_url = false;
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_bool(env, argv[1], &is_url);

  const size_t max_len = simdutf::maximal_binary_length_from_base64(input.data(), input.size());
  std::vector<char> out(max_len);
  size_t out_len = max_len;
  simdutf::result result = is_url
                               ? simdutf::base64_to_binary_safe(
                                     input.data(), input.size(), out.data(), out_len, simdutf::base64_url)
                               : simdutf::base64_to_binary_safe(
                                     input.data(), input.size(), out.data(), out_len);
  if (result.error != simdutf::error_code::SUCCESS) {
    const std::string forgiving = NormalizeForgivingBase64(input, is_url);
    if (forgiving.empty()) return MakeUint8Array(env, nullptr, 0);
    const size_t fallback_max = simdutf::maximal_binary_length_from_base64(forgiving.data(), forgiving.size());
    std::vector<char> fallback_out(fallback_max);
    size_t fallback_len = fallback_max;
    simdutf::result fallback_result =
        simdutf::base64_to_binary_safe(forgiving.data(), forgiving.size(), fallback_out.data(), fallback_len);
    if (fallback_result.error != simdutf::error_code::SUCCESS) {
      return MakeUint8Array(env, nullptr, 0);
    }
    return MakeUint8Array(env, fallback_out.data(), fallback_len);
  }
  return MakeUint8Array(env, out.data(), out_len);
}

napi_value BindingToASCII(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }
  size_t in_len = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &in_len) != napi_ok) return GetUndefined(env);
  std::string input(in_len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[0], input.data(), input.size(), &copied) != napi_ok) return GetUndefined(env);
  input.resize(copied);

  napi_value out = nullptr;
  if (input.empty()) {
    napi_create_string_utf8(env, "", 0, &out);
    return out;
  }
  const std::string result = ada::idna::to_ascii(input);
  napi_create_string_utf8(env, result.c_str(), result.size(), &out);
  return out;
}

napi_value BindingToUnicode(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok || argc < 1) {
    return GetUndefined(env);
  }
  size_t in_len = 0;
  if (napi_get_value_string_utf8(env, argv[0], nullptr, 0, &in_len) != napi_ok) return GetUndefined(env);
  std::string input(in_len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, argv[0], input.data(), input.size(), &copied) != napi_ok) return GetUndefined(env);
  input.resize(copied);

  napi_value out = nullptr;
  if (input.empty()) {
    napi_create_string_utf8(env, "", 0, &out);
    return out;
  }
  const std::string result = ada::idna::to_unicode(input);
  napi_create_string_utf8(env, result.c_str(), result.size(), &out);
  return out;
}

}  // namespace

void UbiInstallEncodingBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return;

  napi_value encode_into_results = nullptr;
  if (napi_create_array_with_length(env, 2, &encode_into_results) == napi_ok && encode_into_results != nullptr) {
    napi_value zero = nullptr;
    napi_create_uint32(env, 0, &zero);
    if (zero != nullptr) {
      napi_set_element(env, encode_into_results, 0, zero);
      napi_set_element(env, encode_into_results, 1, zero);
    }
    napi_set_named_property(env, binding, "encodeIntoResults", encode_into_results);
    if (g_encode_into_results_ref != nullptr) {
      napi_delete_reference(env, g_encode_into_results_ref);
      g_encode_into_results_ref = nullptr;
    }
    napi_create_reference(env, encode_into_results, 1, &g_encode_into_results_ref);
  }

  SetMethod(env, binding, "encodeInto", BindingEncodeInto);
  SetMethod(env, binding, "encodeUtf8String", BindingEncodeUtf8String);
  SetMethod(env, binding, "decodeUTF8", BindingDecodeUTF8);
  SetMethod(env, binding, "toASCII", BindingToASCII);
  SetMethod(env, binding, "toUnicode", BindingToUnicode);

  // Legacy helpers used by existing ubi builtins.
  SetMethod(env, binding, "encodeUtf8", BindingEncodeUtf8String);
  SetMethod(env, binding, "decodeUtf8", BindingDecodeUtf8);
  SetMethod(env, binding, "utf8ByteLength", BindingUtf8ByteLength);
  SetMethod(env, binding, "validateUtf8", BindingValidateUtf8);
  SetMethod(env, binding, "encodeBase64", BindingEncodeBase64);
  SetMethod(env, binding, "decodeBase64", BindingDecodeBase64);

  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return;
  napi_set_named_property(env, global, "__ubi_encoding", binding);
}
