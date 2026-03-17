#include <JavaScriptCore/JSObjectRef.h>
#include <JavaScriptCore/JSTypedArray.h>
#include <JavaScriptCore/JSValueRef.h>
#include <JavaScriptCore/JavaScript.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include "internal/napi_jsc_env.h"

namespace {

struct ScopedJSString {
  explicit ScopedJSString(const char* utf8)
      : value(JSStringCreateWithUTF8CString(utf8 == nullptr ? "" : utf8)) {}

  ~ScopedJSString() {
    if (value != nullptr) JSStringRelease(value);
  }

  JSStringRef value = nullptr;
};

std::string ValueToUtf8(JSContextRef context, JSValueRef value) {
  if (context == nullptr || value == nullptr) return {};
  JSValueRef exception = nullptr;
  JSStringRef string = JSValueToStringCopy(context, value, &exception);
  if (string == nullptr || exception != nullptr) return {};
  const size_t max_size = JSStringGetMaximumUTF8CStringSize(string);
  std::string utf8(max_size, '\0');
  const size_t actual = JSStringGetUTF8CString(string, utf8.data(), max_size);
  JSStringRelease(string);
  if (actual == 0) return {};
  utf8.resize(actual - 1);
  return utf8;
}

bool PrintFailure(const char* step, JSContextRef context, JSValueRef exception) {
  std::cerr << "JSC runtime probe failed at " << step;
  const std::string detail = ValueToUtf8(context, exception);
  if (!detail.empty()) std::cerr << ": " << detail;
  std::cerr << '\n';
  return false;
}

bool EvaluateBool(JSGlobalContextRef context, const char* source, bool* out) {
  if (context == nullptr || source == nullptr || out == nullptr) return false;
  *out = false;
  JSValueRef exception = nullptr;
  ScopedJSString script(source);
  JSValueRef result = JSEvaluateScript(context, script.value, nullptr, nullptr, 0, &exception);
  if (result == nullptr || exception != nullptr) return PrintFailure(source, context, exception);
  *out = JSValueToBoolean(context, result);
  return true;
}

bool CheckCapability(JSGlobalContextRef context, const char* source, const char* label) {
  bool value = false;
  if (!EvaluateBool(context, source, &value)) return false;
  std::cout << label << ": " << (value ? "yes" : "no") << '\n';
  if (!value) {
    std::cerr << "Required capability missing: " << label << '\n';
    return false;
  }
  return true;
}

bool CheckDeferredPromiseAndBigInt(JSGlobalContextRef context) {
  JSValueRef exception = nullptr;
  JSObjectRef resolve = nullptr;
  JSObjectRef reject = nullptr;
  JSObjectRef promise = JSObjectMakeDeferredPromise(context, &resolve, &reject, &exception);
  if (promise == nullptr || resolve == nullptr || reject == nullptr || exception != nullptr) {
    return PrintFailure("JSObjectMakeDeferredPromise", context, exception);
  }

  exception = nullptr;
  JSValueRef bigint = JSBigIntCreateWithInt64(context, 1, &exception);
  if (bigint == nullptr || exception != nullptr) {
    return PrintFailure("JSBigIntCreateWithInt64", context, exception);
  }

  std::cout << "Deferred Promise API: yes\n";
  std::cout << "BigInt C API: yes\n";
  return true;
}

bool CheckSharedArrayBufferBytes(JSGlobalContextRef context) {
  static constexpr const char kCreateView[] =
      "(function(){"
      "  const sab = new SharedArrayBuffer(8);"
      "  const view = new Uint8Array(sab);"
      "  view[0] = 123;"
      "  view[7] = 45;"
      "  return view;"
      "})()";

  JSValueRef exception = nullptr;
  ScopedJSString script(kCreateView);
  JSValueRef result = JSEvaluateScript(context, script.value, nullptr, nullptr, 0, &exception);
  if (result == nullptr || exception != nullptr) {
    return PrintFailure("SharedArrayBuffer probe", context, exception);
  }

  const JSTypedArrayType type = JSValueGetTypedArrayType(context, result, &exception);
  if (exception != nullptr) return PrintFailure("JSValueGetTypedArrayType", context, exception);
  if (type != kJSTypedArrayTypeUint8Array) {
    std::cerr << "Expected Uint8Array view, got type " << static_cast<int>(type) << '\n';
    return false;
  }

  JSObjectRef view = JSValueToObject(context, result, &exception);
  if (view == nullptr || exception != nullptr) {
    return PrintFailure("JSValueToObject(Uint8Array)", context, exception);
  }

  exception = nullptr;
  void* bytes = JSObjectGetTypedArrayBytesPtr(context, view, &exception);
  if (bytes == nullptr || exception != nullptr) {
    return PrintFailure("JSObjectGetTypedArrayBytesPtr", context, exception);
  }

  exception = nullptr;
  const size_t byte_length = JSObjectGetTypedArrayByteLength(context, view, &exception);
  if (exception != nullptr) return PrintFailure("JSObjectGetTypedArrayByteLength", context, exception);
  if (byte_length != 8) {
    std::cerr << "Expected SharedArrayBuffer-backed Uint8Array byte length 8, got "
              << byte_length << '\n';
    return false;
  }

  auto* raw = static_cast<std::uint8_t*>(bytes);
  if (raw[0] != 123 || raw[7] != 45) {
    std::cerr << "Unexpected SharedArrayBuffer byte contents: [0]="
              << static_cast<unsigned>(raw[0]) << " [7]="
              << static_cast<unsigned>(raw[7]) << '\n';
    return false;
  }

  std::cout << "SharedArrayBuffer raw backing-store access: yes\n";
  return true;
}

void PrintLoadedImage() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(JSGlobalContextCreate), &info) != 0 &&
      info.dli_fname != nullptr) {
    std::cout << "Loaded JavaScriptCore image: " << info.dli_fname << '\n';
  } else {
    std::cout << "Loaded JavaScriptCore image: <unavailable>\n";
  }
}

}  // namespace

int main() {
  PrintLoadedImage();

  napi_jsc_prepare_runtime_for_context_creation();
  JSGlobalContextRef context = JSGlobalContextCreate(nullptr);
  if (context == nullptr) {
    std::cerr << "Failed to create JavaScriptCore global context\n";
    return EXIT_FAILURE;
  }

  const bool ok =
      CheckCapability(context, "typeof BigInt === 'function'", "BigInt") &&
      CheckCapability(context,
                      "typeof Uint8Array === 'function' && typeof DataView === 'function'",
                      "TypedArray/DataView") &&
      CheckCapability(context,
                      "typeof Promise === 'function' && typeof Promise.resolve === 'function'",
                      "Promise") &&
      CheckCapability(context, "typeof SharedArrayBuffer === 'function'", "SharedArrayBuffer") &&
      CheckDeferredPromiseAndBigInt(context) &&
      CheckSharedArrayBufferBytes(context);

  JSGlobalContextRelease(context);
  if (!ok) return EXIT_FAILURE;

  std::cout << "JSC runtime probe passed\n";
  return EXIT_SUCCESS;
}
