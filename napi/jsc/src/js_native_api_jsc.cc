#include "internal/napi_jsc_env.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct napi_callback_info__ {
  napi_env env = nullptr;
  napi_value this_arg = nullptr;
  napi_value new_target = nullptr;
  std::vector<napi_value> args;
  void* data = nullptr;
};

namespace {

inline JSObjectRef ToJSObjectRef(JSValueRef value) {
  return reinterpret_cast<JSObjectRef>(const_cast<OpaqueJSValue*>(value));
}

std::mutex g_env_mutex;
std::unordered_map<JSGlobalContextRef, napi_env> g_envs;

constexpr const char* kErrorMessages[] = {
    nullptr,
    "Invalid argument",
    "An object was expected",
    "A string was expected",
    "A string or symbol was expected",
    "A function was expected",
    "A number was expected",
    "A boolean was expected",
    "An array was expected",
    "Unknown failure",
    "An exception is pending",
    "The async work item was cancelled",
    "napi_escape_handle already called on scope",
    "Invalid handle scope usage",
    "Invalid callback scope usage",
    "Thread-safe function queue is full",
    "Thread-safe function handle is closing",
    "A bigint was expected",
    "A date was expected",
    "An ArrayBuffer was expected",
    "A detachable ArrayBuffer was expected",
    "This platform does not allow external buffers",
    "Cannot run JavaScript in finalizer",
};

size_t JsCharLength(const JSChar* str) {
  size_t len = 0;
  while (str != nullptr && str[len] != 0) {
    ++len;
  }
  return len;
}

class JscString {
 public:
  JscString() = default;
  explicit JscString(JSStringRef string) : string_(string) {}
  JscString(const char* string, size_t length = NAPI_AUTO_LENGTH)
      : string_(CreateUtf8(string, length)) {}
  JscString(const char16_t* string, size_t length = NAPI_AUTO_LENGTH)
      : string_(JSStringCreateWithCharacters(
            reinterpret_cast<const JSChar*>(string),
            length == NAPI_AUTO_LENGTH ? JsCharLength(reinterpret_cast<const JSChar*>(string))
                                       : length)) {}

  JscString(const JscString&) = delete;
  JscString& operator=(const JscString&) = delete;

  JscString(JscString&& other) noexcept : string_(other.string_) {
    other.string_ = nullptr;
  }

  JscString& operator=(JscString&& other) noexcept {
    if (this == &other) return *this;
    if (string_ != nullptr) JSStringRelease(string_);
    string_ = other.string_;
    other.string_ = nullptr;
    return *this;
  }

  ~JscString() {
    if (string_ != nullptr) JSStringRelease(string_);
  }

  operator JSStringRef() const { return string_; }
  JSStringRef get() const { return string_; }

  size_t Length() const { return string_ == nullptr ? 0 : JSStringGetLength(string_); }

  size_t CopyUtf8(char* out, size_t out_size) const {
    if (string_ == nullptr || out == nullptr || out_size == 0) return 0;
    return JSStringGetUTF8CString(string_, out, out_size);
  }

 private:
  static std::u16string Utf8ToUtf16(const char* str, size_t len) {
    std::u16string result;
    if (str == nullptr) return result;
    result.reserve(len);
    const auto* s = reinterpret_cast<const unsigned char*>(str);
    const auto* end = s + len;
    while (s < end) {
      uint32_t cp = 0;
      int trail = 0;
      unsigned char lead = *s++;
      if (lead < 0x80) {
        cp = lead;
      } else if ((lead >> 5) == 0x6) {
        cp = lead & 0x1F;
        trail = 1;
      } else if ((lead >> 4) == 0xE) {
        cp = lead & 0x0F;
        trail = 2;
      } else if ((lead >> 3) == 0x1E) {
        cp = lead & 0x07;
        trail = 3;
      } else {
        result.push_back(0xFFFD);
        continue;
      }
      if (s + trail > end) {
        result.push_back(0xFFFD);
        break;
      }
      bool valid = true;
      for (int i = 0; i < trail; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
          valid = false;
          break;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
      }
      if (!valid) {
        result.push_back(0xFFFD);
        continue;
      }
      s += trail;
      if ((trail == 1 && cp < 0x80) || (trail == 2 && cp < 0x800) ||
          (trail == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) ||
          cp > 0x10FFFF) {
        result.push_back(0xFFFD);
        continue;
      }
      if (cp <= 0xFFFF) {
        result.push_back(static_cast<char16_t>(cp));
      } else {
        cp -= 0x10000;
        result.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
        result.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
      }
    }
    return result;
  }

  static JSStringRef CreateUtf8(const char* string, size_t length) {
    if (string == nullptr) {
      static const char empty[] = "";
      string = empty;
      length = 0;
    }
    if (length == NAPI_AUTO_LENGTH) {
      return JSStringCreateWithUTF8CString(string);
    }
    std::u16string u16 = Utf8ToUtf16(string, length);
    return JSStringCreateWithCharacters(
        reinterpret_cast<const JSChar*>(u16.data()), u16.size());
  }

  JSStringRef string_ = nullptr;
};

bool IsArrayIndexString(std::string_view value) {
  if (value.empty()) return false;
  if (value.size() > 1 && value[0] == '0') return false;
  uint64_t out = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') return false;
    out = out * 10 + static_cast<uint64_t>(ch - '0');
    if (out > std::numeric_limits<uint32_t>::max()) return false;
  }
  return true;
}

uint32_t EcmaScriptToUint32(double number) {
  if (!std::isfinite(number) || number == 0) return 0;
  constexpr double kTwoTo32 = 4294967296.0;
  double truncated = std::trunc(number);
  double wrapped = std::fmod(truncated, kTwoTo32);
  if (wrapped < 0) wrapped += kTwoTo32;
  return static_cast<uint32_t>(wrapped);
}

int32_t EcmaScriptToInt32(double number) {
  const uint32_t unsigned_value = EcmaScriptToUint32(number);
  if (unsigned_value >= 0x80000000u) {
    return static_cast<int32_t>(static_cast<int64_t>(unsigned_value) - 0x100000000ll);
  }
  return static_cast<int32_t>(unsigned_value);
}

std::string ToUtf8(JSStringRef string) {
  if (string == nullptr) return {};
  const size_t max_size = JSStringGetMaximumUTF8CStringSize(string);
  std::string out(max_size, '\0');
  const size_t written = JSStringGetUTF8CString(string, out.data(), out.size());
  if (written == 0) return {};
  out.resize(written - 1);
  return out;
}

JSStringRef CreateLatin1String(const char* string, size_t length) {
  if (string == nullptr) {
    static const char empty[] = "";
    string = empty;
    length = 0;
  }
  std::u16string u16;
  u16.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    u16.push_back(static_cast<unsigned char>(string[i]));
  }
  return JSStringCreateWithCharacters(
      reinterpret_cast<const JSChar*>(u16.data()), u16.size());
}

JSObjectRef ToJSObject(napi_env env, napi_value value) {
  return ToJSObjectRef(napi_jsc_to_js_value(value));
}

bool IsObject(napi_env env, napi_value value) {
  return value != nullptr &&
         JSValueIsObject(env->context, napi_jsc_to_js_value(value));
}

bool IsFunction(napi_env env, napi_value value) {
  return value != nullptr &&
         JSValueIsObject(env->context, napi_jsc_to_js_value(value)) &&
         JSObjectIsFunction(env->context, ToJSObject(env, value));
}

bool IsConstructor(napi_env env, napi_value value) {
  return value != nullptr &&
         JSValueIsObject(env->context, napi_jsc_to_js_value(value)) &&
         JSObjectIsConstructor(env->context, ToJSObject(env, value));
}

JSPropertyAttributes ToAttributes(napi_property_attributes attributes) {
  JSPropertyAttributes out = kJSPropertyAttributeNone;
  if ((attributes & napi_writable) == 0) out |= kJSPropertyAttributeReadOnly;
  if ((attributes & napi_enumerable) == 0) out |= kJSPropertyAttributeDontEnum;
  if ((attributes & napi_configurable) == 0) out |= kJSPropertyAttributeDontDelete;
  return out;
}

void FinalizeWithBasicEnv(napi_env env,
                          node_api_basic_finalize finalize_cb,
                          void* data,
                          void* hint) {
  if (finalize_cb == nullptr) return;
  finalize_cb(env, data, hint);
}

struct CallbackPayload {
  napi_env env = nullptr;
  napi_callback cb = nullptr;
  void* data = nullptr;
};

struct BufferDeallocatorContext {
  napi_env env = nullptr;
  void* data = nullptr;
  node_api_basic_finalize finalize_cb = nullptr;
  void* finalize_hint = nullptr;
  bool free_data = false;
};

void BufferBytesDeallocator(void* bytes, void* deallocator_context) {
  auto* context = static_cast<BufferDeallocatorContext*>(deallocator_context);
  if (context == nullptr) return;
  if (context->free_data) {
    std::free(bytes);
  } else if (context->finalize_cb != nullptr && context->env != nullptr) {
    const bool previous = context->env->in_gc_finalizer;
    context->env->in_gc_finalizer = true;
    FinalizeWithBasicEnv(context->env, context->finalize_cb, bytes, context->finalize_hint);
    context->env->in_gc_finalizer = previous;
  }
  delete context;
}

enum class NativeType : uint8_t {
  kConstructor,
  kFunction,
  kExternal,
  kReference,
  kWrapper,
};

class NativeInfo {
 public:
  explicit NativeInfo(NativeType type) : type_(type) {}
  virtual ~NativeInfo() = default;

  NativeType Type() const { return type_; }

  template <typename T>
  static napi_status Link(napi_env env, JSObjectRef target, JSObjectRef sentinel) {
    JSValueRef exception = nullptr;
    JSObjectSetPropertyForKey(
        env->context,
        target,
        T::GetKey(env),
        sentinel,
        static_cast<JSPropertyAttributes>(kJSPropertyAttributeReadOnly |
                                          kJSPropertyAttributeDontEnum |
                                          kJSPropertyAttributeDontDelete),
        &exception);
    return napi_jsc_check_exception(env, exception) ? napi_ok
                                                    : env->last_error.error_code;
  }

  template <typename T>
  static T* Get(JSObjectRef sentinel) {
    return static_cast<T*>(JSObjectGetPrivate(sentinel));
  }

  template <typename T>
  static T* QueryUnchecked(napi_env env, JSObjectRef target, JSValueRef* exception) {
    if (target == nullptr) return nullptr;
    if (!JSObjectHasPropertyForKey(env->context, target, T::GetKey(env), exception)) {
      return nullptr;
    }
    if (*exception != nullptr) return nullptr;
    JSValueRef value = JSObjectGetPropertyForKey(env->context, target, T::GetKey(env), exception);
    if (*exception != nullptr || value == nullptr || !JSValueIsObject(env->context, value)) {
      return nullptr;
    }
    return Get<T>(ToJSObjectRef(value));
  }

  template <typename T>
  static napi_status Query(napi_env env, JSObjectRef target, T** result) {
    if (result == nullptr) return napi_invalid_arg;
    JSValueRef exception = nullptr;
    *result = QueryUnchecked<T>(env, target, &exception);
    return napi_jsc_check_exception(env, exception) ? napi_ok
                                                    : env->last_error.error_code;
  }

 private:
  NativeType type_;
};

template <typename T, NativeType TType>
class BaseInfoT : public NativeInfo {
 public:
  using Finalizer = std::function<void(T*)>;

  ~BaseInfoT() override {
    if (class_ != nullptr) JSClassRelease(class_);
  }

  static const NativeType StaticType = TType;

  void SetData(void* data) { data_ = data; }
  void* Data() const { return data_; }
  napi_env Env() const { return env_; }
  void SetSentinel(JSObjectRef sentinel) { sentinel_ = sentinel; }

  void AddFinalizer(Finalizer finalizer) { finalizers_.push_back(std::move(finalizer)); }
  void ClearFinalizers() { finalizers_.clear(); }
  void FinalizeNowOrSchedule() {
    if (finalization_scheduled_) return;
    finalization_scheduled_ = true;
    T* self = static_cast<T*>(this);
    if (env_ != nullptr) {
      env_->EnqueueDeferredFinalizer([self]() {
        self->DetachSentinel();
        for (auto& finalizer : self->finalizers_) {
          finalizer(self);
        }
        delete self;
      });
      return;
    }
    DetachSentinel();
    delete self;
  }

 protected:
  BaseInfoT(napi_env env, const char* class_name, bool auto_finalize = true)
      : NativeInfo(TType), env_(env), auto_finalize_(auto_finalize) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = class_name;
    def.finalize = Finalize;
    class_ = JSClassCreate(&def);
  }

  static void Finalize(JSObjectRef object) {
    auto* info = NativeInfo::Get<T>(object);
    if (info == nullptr) return;
    JSObjectSetPrivate(object, nullptr);
    info->sentinel_ = nullptr;
    if (!info->auto_finalize_) return;
    info->FinalizeNowOrSchedule();
  }

  void DetachSentinel() {
    if (sentinel_ != nullptr) {
      JSObjectSetPrivate(sentinel_, nullptr);
      sentinel_ = nullptr;
    }
  }

  napi_env env_ = nullptr;
  void* data_ = nullptr;
  std::vector<Finalizer> finalizers_;
  JSClassRef class_ = nullptr;
  JSObjectRef sentinel_ = nullptr;
  bool auto_finalize_ = true;
  bool finalization_scheduled_ = false;
};

class ExternalInfo : public BaseInfoT<ExternalInfo, NativeType::kExternal> {
 public:
  static JSValueRef GetKey(napi_env env) { return env->external_info_symbol; }

  static napi_status Create(napi_env env,
                            void* data,
                            node_api_basic_finalize finalize_cb,
                            void* finalize_hint,
                            napi_value* result) {
    auto* info = new (std::nothrow) ExternalInfo(env);
    if (info == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
    info->SetData(data);
    if (finalize_cb != nullptr) {
      info->AddFinalizer([finalize_cb, finalize_hint](ExternalInfo* self) {
        FinalizeWithBasicEnv(self->Env(), finalize_cb, self->Data(), finalize_hint);
      });
    }
    JSObjectRef sentinel = JSObjectMake(env->context, info->class_, info);
    info->SetSentinel(sentinel);
    *result = napi_jsc_to_napi(sentinel);
    return napi_ok;
  }

 private:
  explicit ExternalInfo(napi_env env)
      : BaseInfoT(env, "EdgeJSCExternal") {}
};

class ReferenceInfo : public BaseInfoT<ReferenceInfo, NativeType::kReference> {
 public:
  static JSValueRef GetKey(napi_env env) { return env->reference_info_symbol; }

  static napi_status GetObjectId(napi_env env, napi_value value, std::uintptr_t* id) {
    if (id == nullptr) return napi_invalid_arg;
    if (!IsObject(env, value)) {
      *id = 0;
      return napi_ok;
    }
    ReferenceInfo* info = nullptr;
    napi_status status = NativeInfo::Query(env, ToJSObject(env, value), &info);
    if (status != napi_ok) return status;
    *id = info == nullptr ? 0 : info->ObjectId();
    return napi_ok;
  }

  static napi_status Initialize(napi_env env,
                                napi_value value,
                                BaseInfoT<ReferenceInfo, NativeType::kReference>::Finalizer finalizer) {
    auto* info = new (std::nothrow) ReferenceInfo(env);
    if (info == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
    JSObjectRef sentinel = JSObjectMake(env->context, info->class_, info);
    info->SetSentinel(sentinel);
    napi_status status = NativeInfo::Link<ReferenceInfo>(env, ToJSObject(env, value), sentinel);
    if (status != napi_ok) {
      delete info;
      return status;
    }
    info->AddFinalizer(std::move(finalizer));
    return napi_ok;
  }

  std::uintptr_t ObjectId() const {
    return reinterpret_cast<std::uintptr_t>(this);
  }

 private:
  explicit ReferenceInfo(napi_env env)
      : BaseInfoT(env, "EdgeJSCReference") {}
};

class WrapperInfo : public BaseInfoT<WrapperInfo, NativeType::kWrapper> {
 public:
  static JSValueRef GetKey(napi_env env) { return env->wrapper_info_symbol; }

  static napi_status Wrap(napi_env env, napi_value object, WrapperInfo** result) {
    if (result == nullptr) return napi_invalid_arg;
    WrapperInfo* info = nullptr;
    napi_status status = Unwrap(env, object, &info);
    if (status != napi_ok) return status;
    if (info != nullptr) {
      *result = info;
      return napi_ok;
    }
    info = new (std::nothrow) WrapperInfo(env);
    if (info == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
    JSObjectRef sentinel = JSObjectMake(env->context, info->class_, info);
    info->SetSentinel(sentinel);
    status = NativeInfo::Link<WrapperInfo>(env, ToJSObject(env, object), sentinel);
    if (status != napi_ok) {
      delete info;
      return status;
    }
    env->tracked_wrapper_infos.push_back(info);
    info->AddFinalizer([](WrapperInfo* self) {
      napi_env env = self->Env();
      if (env == nullptr) return;
      auto& tracked = env->tracked_wrapper_infos;
      tracked.erase(std::remove(tracked.begin(), tracked.end(), self), tracked.end());
    });
    *result = info;
    return napi_ok;
  }

  static napi_status Unwrap(napi_env env, napi_value object, WrapperInfo** result) {
    if (result == nullptr) return napi_invalid_arg;
    *result = nullptr;
    if (!IsObject(env, object)) return napi_ok;
    return NativeInfo::Query(env, ToJSObject(env, object), result);
  }

 private:
  explicit WrapperInfo(napi_env env)
      : BaseInfoT(env, "EdgeJSCWrapper") {}
};

class ConstructorInfo : public NativeInfo {
 public:
  static JSValueRef GetKey(napi_env env) { return env->constructor_info_symbol; }

  static napi_status Create(napi_env env,
                            const char* utf8name,
                            size_t length,
                            napi_callback cb,
                            void* data,
                            napi_value* result) {
    auto* info = new (std::nothrow) ConstructorInfo(env, utf8name, length, cb, data);
    if (info == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
    JSObjectRef ctor = JSObjectMake(env->context, info->class_, info);
    JSObjectRef prototype = JSObjectMake(env->context, nullptr, nullptr);
    JSValueRef exception = nullptr;
    JSObjectSetProperty(env->context,
                        ctor,
                        JscString("prototype"),
                        prototype,
                        static_cast<JSPropertyAttributes>(kJSPropertyAttributeDontDelete),
                        &exception);
    if (!napi_jsc_check_exception(env, exception)) {
      delete info;
      return env->last_error.error_code;
    }
    if (!info->name_.empty()) {
      JSObjectSetProperty(env->context,
                          ctor,
                          JscString("name"),
                          JSValueMakeString(env->context, JscString(info->name_.c_str())),
                          static_cast<JSPropertyAttributes>(kJSPropertyAttributeReadOnly |
                                                            kJSPropertyAttributeDontEnum),
                          &exception);
      if (!napi_jsc_check_exception(env, exception)) {
        delete info;
        return env->last_error.error_code;
      }
    }
    JSObjectSetProperty(env->context,
                        prototype,
                        JscString("constructor"),
                        ctor,
                        static_cast<JSPropertyAttributes>(kJSPropertyAttributeReadOnly |
                                                          kJSPropertyAttributeDontDelete),
                        &exception);
    if (!napi_jsc_check_exception(env, exception)) {
      delete info;
      return env->last_error.error_code;
    }
    *result = napi_jsc_to_napi(ctor);
    return napi_ok;
  }

 private:
  ConstructorInfo(napi_env env,
                  const char* name,
                  size_t length,
                  napi_callback cb,
                  void* data)
      : NativeInfo(NativeType::kConstructor),
        env_(env),
        name_(name == nullptr ? "" : name,
              name == nullptr ? 0
                              : (length == NAPI_AUTO_LENGTH ? std::strlen(name) : length)),
        cb_(cb),
        data_(data) {
    JSClassDefinition def = kJSClassDefinitionEmpty;
    def.className = name_.c_str();
    def.callAsFunction = CallAsFunction;
    def.callAsConstructor = CallAsConstructor;
    def.finalize = Finalize;
    class_ = JSClassCreate(&def);
  }

  ~ConstructorInfo() override {
    if (class_ != nullptr) JSClassRelease(class_);
  }

  static JSValueRef Invoke(napi_env env,
                           ConstructorInfo* info,
                           JSObjectRef function,
                           JSObjectRef this_object,
                           napi_value new_target,
                           size_t argument_count,
                           const JSValueRef arguments[],
                           JSValueRef* exception) {
    napi_jsc_clear_last_error(env);

    auto* cbinfo = new napi_callback_info__();
    cbinfo->env = env;
    cbinfo->this_arg = napi_jsc_to_napi(this_object);
    cbinfo->new_target = new_target;
    cbinfo->data = info->data_;
    cbinfo->args.reserve(argument_count);
    for (size_t i = 0; i < argument_count; ++i) {
      cbinfo->args.push_back(napi_jsc_to_napi(arguments[i]));
    }
    napi_value result = info->cb_(env, cbinfo);
    delete cbinfo;

    if (env->last_exception != nullptr) {
      *exception = env->last_exception;
      env->ClearLastException();
      return nullptr;
    }
    if (result == nullptr) return JSValueMakeUndefined(env->context);
    return napi_jsc_to_js_value(result);
  }

  static JSValueRef CallAsFunction(JSContextRef ctx,
                                   JSObjectRef function,
                                   JSObjectRef this_object,
                                   size_t argument_count,
                                   const JSValueRef arguments[],
                                   JSValueRef* exception) {
    napi_env env = napi_env__::Get(JSContextGetGlobalContext(ctx));
    auto* info = static_cast<ConstructorInfo*>(JSObjectGetPrivate(function));
    if (env == nullptr || info == nullptr) {
      return nullptr;
    }
    return Invoke(env,
                  info,
                  function,
                  this_object,
                  nullptr,
                  argument_count,
                  arguments,
                  exception);
  }

  static JSObjectRef CallAsConstructor(JSContextRef ctx,
                                       JSObjectRef constructor,
                                       size_t argument_count,
                                       const JSValueRef arguments[],
                                       JSValueRef* exception) {
    napi_env env = napi_env__::Get(JSContextGetGlobalContext(ctx));
    auto* info = static_cast<ConstructorInfo*>(JSObjectGetPrivate(constructor));
    if (env == nullptr || info == nullptr) return nullptr;

    JSObjectRef instance = JSObjectMake(ctx, nullptr, nullptr);
    JSValueRef prototype_value =
        JSObjectGetProperty(ctx, constructor, JscString("prototype"), exception);
    if (*exception != nullptr) return nullptr;
    if (prototype_value != nullptr && JSValueIsObject(ctx, prototype_value)) {
      JSObjectSetPrototype(ctx, instance, ToJSObjectRef(prototype_value));
    }
    JSValueRef result = Invoke(env,
                               info,
                               constructor,
                               instance,
                               napi_jsc_to_napi(constructor),
                               argument_count,
                               arguments,
                               exception);
    if (result != nullptr && JSValueIsObject(ctx, result)) {
      return ToJSObjectRef(result);
    }
    return result == nullptr ? nullptr : instance;
  }

  static void Finalize(JSObjectRef object) {
    delete static_cast<ConstructorInfo*>(JSObjectGetPrivate(object));
  }

  napi_env env_ = nullptr;
  std::string name_;
  napi_callback cb_ = nullptr;
  void* data_ = nullptr;
  JSClassRef class_ = nullptr;
};

class FunctionInfo : public BaseInfoT<FunctionInfo, NativeType::kFunction> {
 public:
  static JSValueRef GetKey(napi_env env) { return env->function_info_symbol; }

  static napi_status Create(napi_env env,
                            const char* utf8name,
                            size_t length,
                            napi_callback cb,
                            void* data,
                            napi_value* result) {
    auto* info = new (std::nothrow) FunctionInfo(env, utf8name, length, cb, data);
    if (info == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);

    JSValueRef exception = nullptr;
    JSObjectRef dispatch =
        JSObjectMakeFunctionWithCallback(env->context, JscString("EdgeJSCDispatch"), Dispatch);
    JSValueRef factory_value = JSEvaluateScript(
        env->context,
        JscString(
            "(function(__dispatch, __name) {"
            "  return Object.defineProperty(function(...args) {"
            "    return __dispatch(this, new.target, args);"
            "  }, 'name', { value: __name, configurable: true });"
            "})"),
        nullptr,
        nullptr,
        0,
        &exception);
    if (!napi_jsc_check_exception(env, exception)) {
      delete info;
      return env->last_error.error_code;
    }
    if (factory_value == nullptr || !JSValueIsObject(env->context, factory_value) ||
        !JSObjectIsFunction(env->context, ToJSObjectRef(factory_value))) {
      delete info;
      return napi_jsc_set_last_error(env, napi_generic_failure);
    }

    JSValueRef argv[2] = {
        dispatch,
        JSValueMakeString(env->context, JscString(info->name_.c_str())),
    };
    JSValueRef wrapper_value = nullptr;
    if (!napi_jsc_call_function(env,
                                ToJSObjectRef(factory_value),
                                JSContextGetGlobalObject(env->context),
                                2,
                                argv,
                                &wrapper_value)) {
      delete info;
      return env->last_error.error_code;
    }
    if (wrapper_value == nullptr || !JSValueIsObject(env->context, wrapper_value) ||
        !JSObjectIsFunction(env->context, ToJSObjectRef(wrapper_value))) {
      delete info;
      return napi_jsc_set_last_error(env, napi_generic_failure);
    }

    JSObjectRef sentinel = JSObjectMake(env->context, info->class_, info);
    info->SetSentinel(sentinel);
    napi_status status = NativeInfo::Link<FunctionInfo>(env, dispatch, sentinel);
    if (status != napi_ok) {
      delete info;
      return status;
    }

    *result = napi_jsc_to_napi(wrapper_value);
    return napi_ok;
  }

 private:
  FunctionInfo(napi_env env,
               const char* name,
               size_t length,
               napi_callback cb,
               void* data)
      : BaseInfoT(env, "EdgeJSCFunctionInfo"),
        name_(name == nullptr ? "" : name,
              name == nullptr ? 0
                              : (length == NAPI_AUTO_LENGTH ? std::strlen(name) : length)),
        cb_(cb),
        data_(data) {}

  static JSValueRef Dispatch(JSContextRef ctx,
                             JSObjectRef function,
                             JSObjectRef /*this_object*/,
                             size_t argument_count,
                             const JSValueRef arguments[],
                             JSValueRef* exception) {
    napi_env env = napi_env__::Get(JSContextGetGlobalContext(ctx));
    if (env == nullptr) return nullptr;

    FunctionInfo* info = nullptr;
    napi_status status = NativeInfo::Query(env, function, &info);
    if (status != napi_ok || info == nullptr) return nullptr;
    napi_jsc_clear_last_error(env);

    JSValueRef callback_this = JSValueMakeUndefined(ctx);
    if (argument_count >= 1 && arguments[0] != nullptr) {
      callback_this = arguments[0];
    }

    napi_value new_target = nullptr;
    if (argument_count >= 2 && arguments[1] != nullptr &&
        !JSValueIsUndefined(ctx, arguments[1]) &&
        !JSValueIsNull(ctx, arguments[1])) {
      new_target = napi_jsc_to_napi(arguments[1]);
    }

    std::vector<napi_value> argv;
    if (argument_count >= 3 && arguments[2] != nullptr && JSValueIsObject(ctx, arguments[2])) {
      JSObjectRef args_array = ToJSObjectRef(arguments[2]);
      JSValueRef length_value =
          JSObjectGetProperty(ctx, args_array, JscString("length"), exception);
      if (*exception != nullptr) return nullptr;
      double length_number = JSValueToNumber(ctx, length_value, exception);
      if (*exception != nullptr) return nullptr;
      size_t argc = length_number <= 0 ? 0 : static_cast<size_t>(length_number);
      argv.reserve(argc);
      for (size_t i = 0; i < argc; ++i) {
        JSValueRef arg = JSObjectGetPropertyAtIndex(
            ctx, args_array, static_cast<unsigned>(i), exception);
        if (*exception != nullptr) return nullptr;
        argv.push_back(napi_jsc_to_napi(arg));
      }
    }

    auto* cbinfo = new napi_callback_info__();
    cbinfo->env = env;
    cbinfo->this_arg = napi_jsc_to_napi(callback_this);
    cbinfo->new_target = new_target;
    cbinfo->data = info->data_;
    cbinfo->args = std::move(argv);
    napi_value result = info->cb_(env, cbinfo);
    delete cbinfo;

    if (env->last_exception != nullptr) {
      *exception = env->last_exception;
      env->ClearLastException();
      return nullptr;
    }
    return result == nullptr ? JSValueMakeUndefined(ctx) : napi_jsc_to_js_value(result);
  }

  std::string name_;
  napi_callback cb_ = nullptr;
  void* data_ = nullptr;
};

}  // namespace

struct napi_ref__ {
  napi_status init(napi_env env, napi_value value, uint32_t count) {
    value_ = value;
    count_ = count;
    if (count_ != 0) {
      iter_ = env->strong_refs.insert(env->strong_refs.end(), this);
      JSValueProtect(env->context, napi_jsc_to_js_value(value_));
    }
    if (IsObject(env, value_)) {
      napi_status status = ReferenceInfo::GetObjectId(env, value_, &object_id_);
      if (status != napi_ok) return status;
      if (object_id_ == 0) {
        status = ReferenceInfo::Initialize(
            env,
            value_,
            [value = value_](ReferenceInfo* info) {
              auto it = info->Env()->active_ref_values.find(value);
              if (it != info->Env()->active_ref_values.end() &&
                  it->second == info->ObjectId()) {
                info->Env()->active_ref_values.erase(it);
              }
            });
        if (status != napi_ok) return status;
        status = ReferenceInfo::GetObjectId(env, value_, &object_id_);
        if (status != napi_ok) return status;
        env->active_ref_values[value_] = object_id_;
      }
    }
    return napi_ok;
  }

  void deinit(napi_env env) {
    if (count_ != 0 && value_ != nullptr) {
      env->strong_refs.erase(iter_);
      JSValueUnprotect(env->context, napi_jsc_to_js_value(value_));
    }
    value_ = nullptr;
    count_ = 0;
    object_id_ = 0;
  }

  void ref(napi_env env) {
    if (count_++ == 0 && value_ != nullptr) {
      iter_ = env->strong_refs.insert(env->strong_refs.end(), this);
      JSValueProtect(env->context, napi_jsc_to_js_value(value_));
    }
  }

  void unref(napi_env env) {
    if (count_ == 0) return;
    if (--count_ == 0 && value_ != nullptr) {
      env->strong_refs.erase(iter_);
      JSValueUnprotect(env->context, napi_jsc_to_js_value(value_));
    }
  }

  uint32_t count() const { return count_; }

  napi_status value(napi_env env, napi_value* result) const {
    if (result == nullptr) return napi_invalid_arg;
    *result = nullptr;
    if (value_ == nullptr) return napi_ok;
    if (!IsObject(env, value_) || count_ != 0) {
      *result = value_;
      return napi_ok;
    }
    auto it = env->active_ref_values.find(value_);
    if (it != env->active_ref_values.end() && it->second == object_id_) {
      *result = value_;
    }
    return napi_ok;
  }

  napi_value value_ = nullptr;
  uint32_t count_ = 0;
  std::uintptr_t object_id_ = 0;
  std::list<napi_ref>::iterator iter_{};
};

napi_env__::napi_env__(JSGlobalContextRef context_in,
                       int32_t module_api_version_in,
                       bool owns_context_in)
    : context(JSGlobalContextRetain(context_in)),
      module_api_version(module_api_version_in),
      owns_context(owns_context_in),
      hash_seed(reinterpret_cast<uintptr_t>(this) ^ 0x9e3779b97f4a7c15ULL) {
  last_error.error_code = napi_ok;
  {
    std::lock_guard<std::mutex> lock(g_env_mutex);
    g_envs[context] = this;
  }
  InitSymbol(constructor_info_symbol, "EdgeJSCConstructorInfo");
  InitSymbol(function_info_symbol, "EdgeJSCFunctionInfo");
  InitSymbol(external_info_symbol, "EdgeJSCExternalInfo");
  InitSymbol(reference_info_symbol, "EdgeJSCReferenceInfo");
  InitSymbol(wrapper_info_symbol, "EdgeJSCWrapperInfo");
  InitSymbol(buffer_marker_symbol, "EdgeJSCBufferMarker");
  InitSymbol(detached_arraybuffer_symbol, "EdgeJSCDetachedArrayBuffer");
  InitSymbol(contextify_context_symbol, "EdgeJSCContextifyContext");
}

napi_env__::~napi_env__() {
  DrainDeferredFinalizers();
  RunEnvCleanupHooks();
  while (!strong_refs.empty()) {
    strong_refs.front()->deinit(this);
  }
  napi_jsc_sweep_wrapper_finalizers(this, true);
  DrainDeferredFinalizers();
  if (instance_data_finalize_cb != nullptr) {
    instance_data_finalize_cb(this, instance_data, instance_data_finalize_hint);
  }
  if (context_token_unassign_callback != nullptr) {
    context_token_unassign_callback(this, context, context_token_callback_data);
  }
  if (env_destroy_callback != nullptr) {
    env_destroy_callback(this, env_destroy_callback_data);
  }
  for (auto& entry : type_tag_entries) {
    Unprotect(entry.value);
    entry.value = nullptr;
  }
  type_tag_entries.clear();
  for (auto& entry : private_symbols) {
    Unprotect(entry.second);
    entry.second = nullptr;
  }
  private_symbols.clear();
  ClearLastException();
  DeinitSymbol(contextify_context_symbol);
  DeinitSymbol(detached_arraybuffer_symbol);
  DeinitSymbol(buffer_marker_symbol);
  DeinitSymbol(wrapper_info_symbol);
  DeinitSymbol(reference_info_symbol);
  DeinitSymbol(external_info_symbol);
  DeinitSymbol(function_info_symbol);
  DeinitSymbol(constructor_info_symbol);
  {
    std::lock_guard<std::mutex> lock(g_env_mutex);
    g_envs.erase(context);
  }
  JSGlobalContextRelease(context);
}

napi_env napi_env__::Get(JSGlobalContextRef context) {
  std::lock_guard<std::mutex> lock(g_env_mutex);
  auto it = g_envs.find(context);
  return it == g_envs.end() ? nullptr : it->second;
}

void napi_env__::SetLastException(JSValueRef exception) {
  ClearLastException();
  last_exception = exception;
  if (last_exception != nullptr) JSValueProtect(context, last_exception);
}

void napi_env__::ClearLastException() {
  if (last_exception != nullptr) {
    JSValueUnprotect(context, last_exception);
    last_exception = nullptr;
  }
}

void napi_env__::Protect(JSValueRef value) {
  if (value != nullptr) JSValueProtect(context, value);
}

void napi_env__::Unprotect(JSValueRef value) {
  if (value != nullptr) JSValueUnprotect(context, value);
}

void napi_env__::InitSymbol(JSValueRef& symbol, const char* description) {
  symbol = JSValueMakeSymbol(context, JscString(description));
  Protect(symbol);
}

void napi_env__::DeinitSymbol(JSValueRef& symbol) {
  Unprotect(symbol);
  symbol = nullptr;
}

void napi_env__::EnqueueDeferredFinalizer(std::function<void()> finalizer) {
  if (!finalizer) return;
  deferred_finalizers.push_back(std::move(finalizer));
}

void napi_env__::DrainDeferredFinalizers() {
  if (draining_deferred_finalizers) return;
  draining_deferred_finalizers = true;
  while (!deferred_finalizers.empty()) {
    std::vector<std::function<void()>> pending;
    pending.swap(deferred_finalizers);
    for (auto& finalizer : pending) {
      if (finalizer) finalizer();
    }
  }
  draining_deferred_finalizers = false;
}

napi_status napi_jsc_sweep_wrapper_finalizers(napi_env env, bool force) {
  if (env == nullptr) return napi_invalid_arg;
  if (force) {
    auto tracked = env->tracked_wrapper_infos;
    env->tracked_wrapper_infos.clear();
    for (void* raw : tracked) {
      auto* info = static_cast<WrapperInfo*>(raw);
      if (info != nullptr) info->FinalizeNowOrSchedule();
    }
  }
  env->DrainDeferredFinalizers();
  return napi_jsc_clear_last_error(env);
}

void napi_env__::RunEnvCleanupHooks() {
  for (auto it = env_cleanup_hooks.rbegin(); it != env_cleanup_hooks.rend(); ++it) {
    auto* entry = static_cast<napi_env_cleanup_hook__*>(*it);
    if (entry != nullptr && entry->hook != nullptr) entry->hook(entry->arg);
  }
  for (void* raw : env_cleanup_hooks) {
    delete static_cast<napi_env_cleanup_hook__*>(raw);
  }
  env_cleanup_hooks.clear();
}

napi_status napi_jsc_set_last_error(napi_env env,
                                    napi_status status,
                                    const char* message) {
  if (env == nullptr) return status;
  env->last_error.error_code = status;
  env->last_error.engine_error_code = 0;
  env->last_error.engine_reserved = nullptr;
  env->last_error_message =
      message != nullptr ? message
                         : ((status >= 0 &&
                             static_cast<size_t>(status) < std::size(kErrorMessages))
                                ? (kErrorMessages[status] != nullptr ? kErrorMessages[status] : "")
                                : "");
  env->last_error.error_message =
      env->last_error_message.empty() ? nullptr : env->last_error_message.c_str();
  return status;
}

napi_status napi_jsc_clear_last_error(napi_env env) {
  return napi_jsc_set_last_error(env, napi_ok, nullptr);
}

napi_status napi_jsc_set_pending_exception(napi_env env,
                                          JSValueRef exception,
                                          const char* message) {
  if (env == nullptr) return napi_pending_exception;
  env->SetLastException(exception);
  return napi_jsc_set_last_error(env, napi_pending_exception, message);
}

bool napi_jsc_can_run_js(napi_env env) {
  return env != nullptr && !env->in_gc_finalizer;
}

bool napi_jsc_check_exception(napi_env env, JSValueRef exception) {
  if (exception == nullptr) return true;
  napi_jsc_set_pending_exception(env, exception, "JavaScript exception");
  return false;
}

JSValueRef napi_jsc_make_string_utf8(napi_env env, const char* str, size_t length) {
  (void)env;
  return JSValueMakeString(env->context, JscString(str, length));
}

JSValueRef napi_jsc_make_string_utf16(napi_env env, const char16_t* str, size_t length) {
  (void)env;
  return JSValueMakeString(env->context, JscString(str, length));
}

std::string napi_jsc_value_to_utf8(napi_env env, napi_value value, JSValueRef* exception) {
  if (value == nullptr) return {};
  JSValueRef local_exception = nullptr;
  JSStringRef string = JSValueToStringCopy(env->context, napi_jsc_to_js_value(value), &local_exception);
  if (exception != nullptr) *exception = local_exception;
  if (local_exception != nullptr || string == nullptr) return {};
  JscString holder(string);
  return ToUtf8(holder.get());
}

bool napi_jsc_get_global_function(napi_env env, const char* name, JSObjectRef* result_out) {
  if (result_out == nullptr) return false;
  *result_out = nullptr;
  JSObjectRef global = JSContextGetGlobalObject(env->context);
  JSValueRef value = nullptr;
  if (!napi_jsc_get_named_property(env, global, name, &value) || value == nullptr ||
      !JSValueIsObject(env->context, value)) {
    return false;
  }
  JSObjectRef object = ToJSObjectRef(value);
  if (!JSObjectIsFunction(env->context, object)) return false;
  *result_out = object;
  return true;
}

bool napi_jsc_call_function(napi_env env,
                            JSObjectRef function,
                            JSObjectRef this_object,
                            size_t argc,
                            const JSValueRef argv[],
                            JSValueRef* result_out) {
  JSValueRef exception = nullptr;
  JSValueRef value = JSObjectCallAsFunction(
      env->context,
      function,
      this_object == nullptr ? JSContextGetGlobalObject(env->context) : this_object,
      argc,
      argv,
      &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (result_out != nullptr) *result_out = value;
  return true;
}

bool napi_jsc_call_constructor(napi_env env,
                               JSObjectRef constructor,
                               size_t argc,
                               const JSValueRef argv[],
                               JSObjectRef* result_out) {
  JSValueRef exception = nullptr;
  JSObjectRef object =
      JSObjectCallAsConstructor(env->context, constructor, argc, argv, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (result_out != nullptr) *result_out = object;
  return true;
}

bool napi_jsc_set_named_property(napi_env env,
                                 JSObjectRef object,
                                 const char* name,
                                 JSValueRef value,
                                 JSPropertyAttributes attributes) {
  JSValueRef exception = nullptr;
  JSObjectSetProperty(
      env->context, object, JscString(name), value, attributes, &exception);
  return napi_jsc_check_exception(env, exception);
}

bool napi_jsc_get_named_property(napi_env env,
                                 JSObjectRef object,
                                 const char* name,
                                 JSValueRef* result_out) {
  JSValueRef exception = nullptr;
  JSValueRef value = JSObjectGetProperty(env->context, object, JscString(name), &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (result_out != nullptr) *result_out = value;
  return true;
}

bool napi_jsc_define_property(napi_env env,
                              JSObjectRef object,
                              JSValueRef key,
                              JSValueRef value,
                              JSValueRef getter,
                              JSValueRef setter,
                              napi_property_attributes attributes) {
  JSObjectRef object_ctor = nullptr;
  if (!napi_jsc_get_global_function(env, "Object", &object_ctor)) return false;
  JSValueRef define_property_value = nullptr;
  if (!napi_jsc_get_named_property(env, object_ctor, "defineProperty", &define_property_value) ||
      define_property_value == nullptr ||
      !JSValueIsObject(env->context, define_property_value) ||
      !JSObjectIsFunction(env->context, ToJSObjectRef(define_property_value))) {
    return false;
  }
  JSObjectRef descriptor = JSObjectMake(env->context, nullptr, nullptr);
  if (value != nullptr) {
    if (!napi_jsc_set_named_property(env, descriptor, "value", value)) return false;
    if (!napi_jsc_set_named_property(
            env,
            descriptor,
            "writable",
            JSValueMakeBoolean(env->context, (attributes & napi_writable) != 0))) {
      return false;
    }
  }
  if (getter != nullptr &&
      !napi_jsc_set_named_property(env, descriptor, "get", getter)) return false;
  if (setter != nullptr &&
      !napi_jsc_set_named_property(env, descriptor, "set", setter)) return false;
  if (!napi_jsc_set_named_property(
          env,
          descriptor,
          "enumerable",
          JSValueMakeBoolean(env->context, (attributes & napi_enumerable) != 0))) {
    return false;
  }
  if (!napi_jsc_set_named_property(
          env,
          descriptor,
          "configurable",
          JSValueMakeBoolean(env->context, (attributes & napi_configurable) != 0))) {
    return false;
  }
  JSValueRef argv[3] = {object, key, descriptor};
  return napi_jsc_call_function(
      env,
      ToJSObjectRef(define_property_value),
      object_ctor,
      3,
      argv,
      nullptr);
}

bool napi_jsc_create_uint8_array_from_bytes(napi_env env,
                                            void* data,
                                            size_t byte_length,
                                            JSTypedArrayBytesDeallocator deallocator,
                                            void* deallocator_context,
                                            napi_value* result_out,
                                            napi_value* arraybuffer_out) {
  JSValueRef exception = nullptr;
  JSObjectRef ab = JSObjectMakeArrayBufferWithBytesNoCopy(
      env->context, data, byte_length, deallocator, deallocator_context, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  JSObjectRef view = JSObjectMakeTypedArrayWithArrayBuffer(
      env->context, kJSTypedArrayTypeUint8Array, ab, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  JSObjectSetPropertyForKey(
      env->context,
      view,
      env->buffer_marker_symbol,
      JSValueMakeBoolean(env->context, true),
      static_cast<JSPropertyAttributes>(kJSPropertyAttributeDontEnum |
                                        kJSPropertyAttributeDontDelete),
      &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (arraybuffer_out != nullptr) *arraybuffer_out = napi_jsc_to_napi(ab);
  if (result_out != nullptr) *result_out = napi_jsc_to_napi(view);
  return true;
}

bool napi_jsc_create_uint8_array_for_buffer(napi_env env,
                                            JSObjectRef arraybuffer,
                                            size_t byte_offset,
                                            size_t byte_length,
                                            napi_value* result_out) {
  JSValueRef exception = nullptr;
  JSObjectRef view = JSObjectMakeTypedArrayWithArrayBufferAndOffset(
      env->context, kJSTypedArrayTypeUint8Array, arraybuffer, byte_offset, byte_length, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  JSObjectSetPropertyForKey(
      env->context,
      view,
      env->buffer_marker_symbol,
      JSValueMakeBoolean(env->context, true),
      static_cast<JSPropertyAttributes>(kJSPropertyAttributeDontEnum |
                                        kJSPropertyAttributeDontDelete),
      &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (result_out != nullptr) *result_out = napi_jsc_to_napi(view);
  return true;
}

bool napi_jsc_get_typed_array_bytes(napi_env env,
                                    JSObjectRef array,
                                    void** data_out,
                                    size_t* byte_length_out,
                                    size_t* byte_offset_out,
                                    JSObjectRef* buffer_out) {
  JSValueRef exception = nullptr;
  if (data_out != nullptr) *data_out = JSObjectGetTypedArrayBytesPtr(env->context, array, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (byte_length_out != nullptr) {
    *byte_length_out = JSObjectGetTypedArrayByteLength(env->context, array, &exception);
    if (!napi_jsc_check_exception(env, exception)) return false;
  }
  if (byte_offset_out != nullptr) {
    *byte_offset_out = JSObjectGetTypedArrayByteOffset(env->context, array, &exception);
    if (!napi_jsc_check_exception(env, exception)) return false;
  }
  if (buffer_out != nullptr) {
    *buffer_out = JSObjectGetTypedArrayBuffer(env->context, array, &exception);
    if (!napi_jsc_check_exception(env, exception)) return false;
  }
  return true;
}

bool napi_jsc_value_is_buffer(napi_env env, napi_value value) {
  if (!IsObject(env, value)) return false;
  JSValueRef exception = nullptr;
  return JSObjectHasPropertyForKey(
      env->context, ToJSObject(env, value), env->buffer_marker_symbol, &exception);
}
static napi_status CreateErrorValue(napi_env env,
                                    const char* ctor_name,
                                    napi_value code,
                                    napi_value msg,
                                    napi_value* result) {
  if (msg == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSObjectRef ctor = nullptr;
  if (!napi_jsc_get_global_function(env, ctor_name, &ctor)) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "Error constructor not available");
  }
  JSValueRef argv[1] = {napi_jsc_to_js_value(msg)};
  JSObjectRef error = nullptr;
  if (!napi_jsc_call_constructor(env, ctor, 1, argv, &error)) {
    return env->last_error.error_code;
  }
  if (code != nullptr) {
    JSValueRef exception = nullptr;
    JSObjectSetProperty(env->context,
                        error,
                        JscString("code"),
                        napi_jsc_to_js_value(code),
                        kJSPropertyAttributeNone,
                        &exception);
    if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  }
  *result = napi_jsc_to_napi(error);
  return napi_jsc_clear_last_error(env);
}

static napi_status ThrowErrorCommon(napi_env env,
                                    const char* ctor_name,
                                    const char* code,
                                    const char* msg) {
  napi_value message = nullptr;
  napi_status status =
      napi_create_string_utf8(env, msg != nullptr ? msg : ctor_name, NAPI_AUTO_LENGTH, &message);
  if (status != napi_ok) return status;
  napi_value code_value = nullptr;
  if (code != nullptr) {
    status = napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value);
    if (status != napi_ok) return status;
  }
  napi_value error = nullptr;
  status = CreateErrorValue(env, ctor_name, code_value, message, &error);
  if (status != napi_ok) return status;
  env->SetLastException(napi_jsc_to_js_value(error));
  return napi_jsc_set_last_error(env, napi_pending_exception, msg);
}

static napi_valuetype TypeOfValue(napi_env env, napi_value value) {
  JSValueRef raw = napi_jsc_to_js_value(value);
  if (raw == nullptr) return napi_undefined;
  if (JSValueIsUndefined(env->context, raw)) return napi_undefined;
  if (JSValueIsNull(env->context, raw)) return napi_null;
  if (JSValueIsBoolean(env->context, raw)) return napi_boolean;
  if (JSValueIsBigInt(env->context, raw)) return napi_bigint;
  if (JSValueIsNumber(env->context, raw)) return napi_number;
  if (JSValueIsString(env->context, raw)) return napi_string;
  if (JSValueIsSymbol(env->context, raw)) return napi_symbol;
  if (JSValueIsObject(env->context, raw)) {
    if (IsFunction(env, value)) return napi_function;
    ExternalInfo* external = nullptr;
    if (NativeInfo::Query(env, ToJSObject(env, value), &external) == napi_ok &&
        external != nullptr && external->Type() == NativeType::kExternal) {
      return napi_external;
    }
    return napi_object;
  }
  return napi_undefined;
}

static bool GetTypedArrayTypeForObject(JSContextRef ctx,
                                       JSObjectRef value,
                                       napi_typedarray_type* out_type) {
  if (out_type == nullptr) return false;
  JSValueRef exception = nullptr;
  switch (JSValueGetTypedArrayType(ctx, value, &exception)) {
    case kJSTypedArrayTypeInt8Array:
      *out_type = napi_int8_array;
      return true;
    case kJSTypedArrayTypeUint8Array:
      *out_type = napi_uint8_array;
      return true;
    case kJSTypedArrayTypeUint8ClampedArray:
      *out_type = napi_uint8_clamped_array;
      return true;
    case kJSTypedArrayTypeInt16Array:
      *out_type = napi_int16_array;
      return true;
    case kJSTypedArrayTypeUint16Array:
      *out_type = napi_uint16_array;
      return true;
    case kJSTypedArrayTypeInt32Array:
      *out_type = napi_int32_array;
      return true;
    case kJSTypedArrayTypeUint32Array:
      *out_type = napi_uint32_array;
      return true;
    case kJSTypedArrayTypeFloat32Array:
      *out_type = napi_float32_array;
      return true;
    case kJSTypedArrayTypeFloat64Array:
      *out_type = napi_float64_array;
      return true;
    case kJSTypedArrayTypeBigInt64Array:
      *out_type = napi_bigint64_array;
      return true;
    case kJSTypedArrayTypeBigUint64Array:
      *out_type = napi_biguint64_array;
      return true;
    default:
      return false;
  }
}

static JSTypedArrayType ToJscTypedArrayType(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array: return kJSTypedArrayTypeInt8Array;
    case napi_uint8_array: return kJSTypedArrayTypeUint8Array;
    case napi_uint8_clamped_array: return kJSTypedArrayTypeUint8ClampedArray;
    case napi_int16_array: return kJSTypedArrayTypeInt16Array;
    case napi_uint16_array: return kJSTypedArrayTypeUint16Array;
    case napi_int32_array: return kJSTypedArrayTypeInt32Array;
    case napi_uint32_array: return kJSTypedArrayTypeUint32Array;
    case napi_float32_array: return kJSTypedArrayTypeFloat32Array;
    case napi_float64_array: return kJSTypedArrayTypeFloat64Array;
    case napi_bigint64_array: return kJSTypedArrayTypeBigInt64Array;
    case napi_biguint64_array: return kJSTypedArrayTypeBigUint64Array;
    default: return kJSTypedArrayTypeNone;
  }
}

static bool GetSharedArrayBufferInfo(napi_env env,
                                     JSObjectRef sab,
                                     void** data_out,
                                     size_t* byte_length_out) {
  JSObjectRef uint8_ctor = nullptr;
  if (!napi_jsc_get_global_function(env, "Uint8Array", &uint8_ctor)) return false;
  JSValueRef argv[1] = {sab};
  JSObjectRef view = nullptr;
  if (!napi_jsc_call_constructor(env, uint8_ctor, 1, argv, &view)) return false;
  size_t length = 0;
  void* data = nullptr;
  if (!napi_jsc_get_typed_array_bytes(env, view, &data, &length, nullptr, nullptr)) return false;
  if (data_out != nullptr) *data_out = data;
  if (byte_length_out != nullptr) {
    JSValueRef byte_length_value = nullptr;
    if (!napi_jsc_get_named_property(env, sab, "byteLength", &byte_length_value) ||
        byte_length_value == nullptr) {
      return false;
    }
    JSValueRef exception = nullptr;
    *byte_length_out = static_cast<size_t>(
        JSValueToNumber(env->context, byte_length_value, &exception));
    if (!napi_jsc_check_exception(env, exception)) return false;
  } else {
    (void)length;
  }
  return true;
}

static bool IsSharedArrayBuffer(napi_env env, napi_value value) {
  if (!IsObject(env, value)) return false;
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return false;
  napi_value ctor = nullptr;
  if (napi_get_named_property(env, global, "SharedArrayBuffer", &ctor) != napi_ok ||
      ctor == nullptr || !IsFunction(env, ctor)) {
    return false;
  }
  bool result = false;
  (void)napi_instanceof(env, value, ctor, &result);
  return result;
}

static bool BigIntToHexString(napi_env env, napi_value value, std::string* out, bool* negative) {
  if (out == nullptr || negative == nullptr) return false;
  *out = {};
  *negative = false;
  JSValueRef source = napi_jsc_to_js_value(value);
  JSObjectRef helper = nullptr;
  JSValueRef helper_value = JSEvaluateScript(
      env->context,
      JscString("(function(v){return [v < 0n, (v < 0n ? (-v) : v).toString(16)];})"),
      nullptr,
      nullptr,
      0,
      nullptr);
  if (helper_value == nullptr || !JSValueIsObject(env->context, helper_value)) return false;
  helper = ToJSObjectRef(helper_value);
  JSValueRef argv[1] = {source};
  JSValueRef result_value = nullptr;
  if (!napi_jsc_call_function(env, helper, JSContextGetGlobalObject(env->context), 1, argv, &result_value) ||
      result_value == nullptr || !JSValueIsObject(env->context, result_value) ||
      !JSValueIsArray(env->context, result_value)) {
    return false;
  }
  JSObjectRef result_array = ToJSObjectRef(result_value);
  JSValueRef exception = nullptr;
  *negative = JSValueToBoolean(env->context, JSObjectGetPropertyAtIndex(env->context, result_array, 0, &exception));
  if (!napi_jsc_check_exception(env, exception)) return false;
  JSValueRef hex_value = JSObjectGetPropertyAtIndex(env->context, result_array, 1, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  *out = napi_jsc_value_to_utf8(env, napi_jsc_to_napi(hex_value), &exception);
  return napi_jsc_check_exception(env, exception);
}

static JSValueRef CreateBigIntFromWords(napi_env env,
                                        int sign_bit,
                                        size_t word_count,
                                        const uint64_t* words,
                                        JSValueRef* exception_out) {
  std::string hex;
  bool non_zero = false;
  for (size_t i = word_count; i > 0; --i) {
    const uint64_t word = words[i - 1];
    if (!non_zero) {
      if (word == 0) continue;
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(word));
      hex += buf;
      non_zero = true;
    } else {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(word));
      hex += buf;
    }
  }
  if (!non_zero) {
    hex = "0";
  }
  std::string source;
  if (sign_bit && hex != "0") {
    source = "(-0x" + hex + "n)";
  } else {
    source = "(0x" + hex + "n)";
  }
  return JSEvaluateScript(env->context,
                          JscString(source.c_str(), source.size()),
                          nullptr,
                          nullptr,
                          0,
                          exception_out);
}

static bool GetArrayLikeLength(napi_env env, JSObjectRef object, size_t* length_out) {
  if (length_out == nullptr) return false;
  JSValueRef length_value = nullptr;
  if (!napi_jsc_get_named_property(env, object, "length", &length_value) ||
      length_value == nullptr) {
    return false;
  }
  JSValueRef exception = nullptr;
  double number = JSValueToNumber(env->context, length_value, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (number < 0) number = 0;
  *length_out = static_cast<size_t>(number);
  return true;
}

static bool GetOwnKeys(napi_env env, JSObjectRef object, JSObjectRef* result_out) {
  if (result_out == nullptr) return false;
  *result_out = nullptr;
  napi_value global = nullptr;
  napi_value reflect = nullptr;
  napi_value own_keys = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return false;
  status = napi_get_named_property(env, global, "Reflect", &reflect);
  if (status != napi_ok) return false;
  status = napi_get_named_property(env, reflect, "ownKeys", &own_keys);
  if (status != napi_ok) return false;
  napi_value object_value = napi_jsc_to_napi(object);
  napi_value argv[1] = {object_value};
  napi_value keys = nullptr;
  status = napi_call_function(env, reflect, own_keys, 1, argv, &keys);
  if (status != napi_ok || keys == nullptr ||
      !JSValueIsObject(env->context, napi_jsc_to_js_value(keys))) {
    return false;
  }
  *result_out = ToJSObjectRef(napi_jsc_to_js_value(keys));
  return true;
}

static bool GetOwnPropertyDescriptor(napi_env env,
                                     JSObjectRef object,
                                     JSValueRef key,
                                     JSValueRef* result_out) {
  if (result_out == nullptr) return false;
  *result_out = nullptr;
  napi_value global = nullptr;
  napi_value object_ctor = nullptr;
  napi_value getter = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return false;
  status = napi_get_named_property(env, global, "Object", &object_ctor);
  if (status != napi_ok) return false;
  status =
      napi_get_named_property(env, object_ctor, "getOwnPropertyDescriptor", &getter);
  if (status != napi_ok) return false;
  napi_value argv[2] = {napi_jsc_to_napi(object), napi_jsc_to_napi(key)};
  napi_value descriptor = nullptr;
  status = napi_call_function(env, object_ctor, getter, 2, argv, &descriptor);
  if (status != napi_ok) return false;
  *result_out = napi_jsc_to_js_value(descriptor);
  return true;
}

static bool GetDescriptorBool(napi_env env,
                              JSValueRef descriptor,
                              const char* name,
                              bool* result_out) {
  if (result_out == nullptr) return false;
  *result_out = false;
  if (descriptor == nullptr || JSValueIsUndefined(env->context, descriptor) ||
      JSValueIsNull(env->context, descriptor) ||
      !JSValueIsObject(env->context, descriptor)) {
    return true;
  }
  JSValueRef value = nullptr;
  if (!napi_jsc_get_named_property(
          env, ToJSObjectRef(descriptor), name, &value)) {
    return false;
  }
  if (value == nullptr || JSValueIsUndefined(env->context, value)) return true;
  *result_out = JSValueToBoolean(env->context, value);
  return true;
}

static bool KeysEqual(napi_env env, JSValueRef lhs, JSValueRef rhs) {
  return JSValueIsStrictEqual(env->context, lhs, rhs);
}

static bool ContainsKey(napi_env env,
                        const std::vector<JSValueRef>& values,
                        JSValueRef key) {
  return std::any_of(values.begin(),
                     values.end(),
                     [env, key](JSValueRef existing) { return KeysEqual(env, existing, key); });
}

static bool MaybeConvertKey(napi_env env,
                            JSValueRef key,
                            napi_key_conversion conversion,
                            JSValueRef* result_out) {
  if (result_out == nullptr) return false;
  *result_out = key;
  if (conversion != napi_key_keep_numbers || key == nullptr ||
      !JSValueIsString(env->context, key)) {
    return true;
  }
  std::string utf8 = napi_jsc_value_to_utf8(env, napi_jsc_to_napi(key));
  if (!IsArrayIndexString(utf8)) return true;
  *result_out = JSValueMakeNumber(env->context, static_cast<double>(std::stoul(utf8)));
  return true;
}

static bool GetAllPropertyNamesImpl(napi_env env,
                                    JSObjectRef object,
                                    napi_key_collection_mode key_mode,
                                    napi_key_filter key_filter,
                                    napi_key_conversion key_conversion,
                                    JSObjectRef* result_out) {
  if (result_out == nullptr) return false;
  *result_out = nullptr;

  std::vector<JSValueRef> keys;
  JSValueRef cursor = object;
  while (cursor != nullptr && JSValueIsObject(env->context, cursor)) {
    auto* current = ToJSObjectRef(cursor);
    JSObjectRef own_keys = nullptr;
    if (!GetOwnKeys(env, current, &own_keys) || own_keys == nullptr) return false;

    size_t length = 0;
    if (!GetArrayLikeLength(env, own_keys, &length)) return false;

    for (size_t i = 0; i < length; ++i) {
      JSValueRef exception = nullptr;
      JSValueRef key = JSObjectGetPropertyAtIndex(
          env->context, own_keys, static_cast<unsigned>(i), &exception);
      if (!napi_jsc_check_exception(env, exception)) return false;
      if (key == nullptr || ContainsKey(env, keys, key)) continue;

      if ((key_filter & napi_key_skip_strings) != 0 &&
          JSValueIsString(env->context, key)) {
        continue;
      }
      if ((key_filter & napi_key_skip_symbols) != 0 &&
          JSValueIsSymbol(env->context, key)) {
        continue;
      }

      JSValueRef descriptor = nullptr;
      if (!GetOwnPropertyDescriptor(env, current, key, &descriptor)) return false;

      bool writable = false;
      bool enumerable = false;
      bool configurable = false;
      if (!GetDescriptorBool(env, descriptor, "writable", &writable) ||
          !GetDescriptorBool(env, descriptor, "enumerable", &enumerable) ||
          !GetDescriptorBool(env, descriptor, "configurable", &configurable)) {
        return false;
      }

      if ((key_filter & napi_key_writable) != 0 && !writable) continue;
      if ((key_filter & napi_key_enumerable) != 0 && !enumerable) continue;
      if ((key_filter & napi_key_configurable) != 0 && !configurable) continue;

      JSValueRef converted = nullptr;
      if (!MaybeConvertKey(env, key, key_conversion, &converted) || converted == nullptr) {
        return false;
      }
      keys.push_back(converted);
    }

    if (key_mode == napi_key_own_only) break;
    cursor = JSObjectGetPrototype(env->context, current);
    if (cursor == nullptr || JSValueIsNull(env->context, cursor) ||
        JSValueIsUndefined(env->context, cursor)) {
      break;
    }
  }

  JSValueRef exception = nullptr;
  JSObjectRef array =
      JSObjectMakeArray(env->context, keys.size(), keys.data(), &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  *result_out = array;
  return true;
}

static const char* TypedArrayConstructorName(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array: return "Int8Array";
    case napi_uint8_array: return "Uint8Array";
    case napi_uint8_clamped_array: return "Uint8ClampedArray";
    case napi_int16_array: return "Int16Array";
    case napi_uint16_array: return "Uint16Array";
    case napi_int32_array: return "Int32Array";
    case napi_uint32_array: return "Uint32Array";
    case napi_float16_array: return "Float16Array";
    case napi_float32_array: return "Float32Array";
    case napi_float64_array: return "Float64Array";
    case napi_bigint64_array: return "BigInt64Array";
    case napi_biguint64_array: return "BigUint64Array";
    default: return nullptr;
  }
}

static size_t TypedArrayElementSize(napi_typedarray_type type) {
  switch (type) {
    case napi_int8_array:
    case napi_uint8_array:
    case napi_uint8_clamped_array:
      return 1;
    case napi_int16_array:
    case napi_uint16_array:
    case napi_float16_array:
      return 2;
    case napi_int32_array:
    case napi_uint32_array:
    case napi_float32_array:
      return 4;
    case napi_float64_array:
    case napi_bigint64_array:
    case napi_biguint64_array:
      return 8;
    default:
      return 0;
  }
}

static bool IsTypedArrayByName(napi_env env, napi_value value, const char* name) {
  napi_value global = nullptr;
  napi_value ctor = nullptr;
  if (napi_get_global(env, &global) != napi_ok) return false;
  if (napi_get_named_property(env, global, name, &ctor) != napi_ok || ctor == nullptr ||
      !IsFunction(env, ctor)) {
    return false;
  }
  bool result = false;
  (void)napi_instanceof(env, value, ctor, &result);
  return result;
}

static napi_status CallElementHelper(napi_env env,
                                     const char* helper_source,
                                     size_t argc,
                                     const JSValueRef argv[],
                                     napi_value* result_out) {
  JSValueRef exception = nullptr;
  JSValueRef helper_value =
      JSEvaluateScript(env->context, JscString(helper_source), nullptr, nullptr, 0, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  if (helper_value == nullptr || !JSValueIsObject(env->context, helper_value) ||
      !JSObjectIsFunction(env->context, ToJSObjectRef(helper_value))) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }
  JSValueRef result = nullptr;
  if (!napi_jsc_call_function(env,
                              ToJSObjectRef(helper_value),
                              JSContextGetGlobalObject(env->context),
                              argc,
                              argv,
                              &result)) {
    return env->last_error.error_code;
  }
  if (result_out != nullptr) *result_out = napi_jsc_to_napi(result);
  return napi_jsc_clear_last_error(env);
}

static bool GetTypedArrayInfoImpl(napi_env env,
                                  napi_value value,
                                  napi_typedarray_type* type_out,
                                  size_t* length_out,
                                  void** data_out,
                                  napi_value* arraybuffer_out,
                                  size_t* byte_offset_out) {
  if (!IsObject(env, value)) return false;

  JSValueRef exception = nullptr;
  JSObjectRef object = ToJSObject(env, value);
  JSTypedArrayType raw_type = JSValueGetTypedArrayType(env->context, object, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;

  bool is_float16 = false;
  if (raw_type == kJSTypedArrayTypeNone || raw_type == kJSTypedArrayTypeArrayBuffer) {
    is_float16 = IsTypedArrayByName(env, value, "Float16Array");
    if (!is_float16) return false;
  }

  if (type_out != nullptr) {
    if (is_float16) {
      *type_out = napi_float16_array;
    } else if (!GetTypedArrayTypeForObject(env->context, object, type_out)) {
      return false;
    }
  }

  if (is_float16) {
    if (length_out != nullptr) {
      JSValueRef length_value = nullptr;
      if (!napi_jsc_get_named_property(env, object, "length", &length_value) ||
          length_value == nullptr) {
        return false;
      }
      double number = JSValueToNumber(env->context, length_value, &exception);
      if (!napi_jsc_check_exception(env, exception)) return false;
      *length_out = static_cast<size_t>(number);
    }

    size_t byte_offset = 0;
    JSValueRef byte_offset_value = nullptr;
    if (!napi_jsc_get_named_property(env, object, "byteOffset", &byte_offset_value) ||
        byte_offset_value == nullptr) {
      return false;
    }
    double byte_offset_number =
        JSValueToNumber(env->context, byte_offset_value, &exception);
    if (!napi_jsc_check_exception(env, exception)) return false;
    byte_offset = static_cast<size_t>(byte_offset_number);
    if (byte_offset_out != nullptr) *byte_offset_out = byte_offset;

    napi_value buffer = nullptr;
    if (napi_get_named_property(env, value, "buffer", &buffer) != napi_ok || buffer == nullptr) {
      return false;
    }
    if (arraybuffer_out != nullptr) *arraybuffer_out = buffer;
    if (data_out != nullptr) {
      void* buffer_bytes = nullptr;
      if (napi_get_arraybuffer_info(env, buffer, &buffer_bytes, nullptr) != napi_ok) {
        return false;
      }
      *data_out = buffer_bytes == nullptr
                      ? nullptr
                      : static_cast<void*>(static_cast<uint8_t*>(buffer_bytes) + byte_offset);
    }
    return true;
  }

  JSObjectRef buffer = nullptr;
  if (!napi_jsc_get_typed_array_bytes(
          env, object, data_out, nullptr, byte_offset_out, &buffer)) {
    return false;
  }
  if (length_out != nullptr) {
    *length_out = JSObjectGetTypedArrayLength(env->context, object, &exception);
    if (!napi_jsc_check_exception(env, exception)) return false;
  }
  if (arraybuffer_out != nullptr) *arraybuffer_out = napi_jsc_to_napi(buffer);
  return true;
}

static bool IsMarkedDetachedArrayBuffer(napi_env env, JSObjectRef object) {
  JSValueRef exception = nullptr;
  return JSObjectHasPropertyForKey(
      env->context, object, env->detached_arraybuffer_symbol, &exception);
}

static bool MarkDetachedArrayBuffer(napi_env env, JSObjectRef object) {
  JSValueRef exception = nullptr;
  JSObjectSetPropertyForKey(
      env->context,
      object,
      env->detached_arraybuffer_symbol,
      JSValueMakeBoolean(env->context, true),
      static_cast<JSPropertyAttributes>(kJSPropertyAttributeDontEnum |
                                        kJSPropertyAttributeDontDelete),
      &exception);
  return napi_jsc_check_exception(env, exception);
}

static bool DetachArrayBufferWithStructuredClone(napi_env env, JSObjectRef object) {
  if (IsMarkedDetachedArrayBuffer(env, object)) return true;

  static constexpr const char kDetachHelper[] =
      "(function(buffer){"
      "  let lastError;"
      "  const proto = Object.getPrototypeOf(buffer);"
      "  const byteLength = buffer.byteLength >>> 0;"
      "  if (typeof structuredClone === 'function') {"
      "    try {"
      "      structuredClone(buffer, { transfer: [buffer] });"
      "      return true;"
      "    } catch (e) {"
      "      lastError = e;"
      "    }"
      "  }"
      "  if (proto && typeof proto.transferToFixedLength === 'function') {"
      "    try {"
      "      proto.transferToFixedLength.call(buffer, byteLength);"
      "      return true;"
      "    } catch (e) {"
      "      lastError = e;"
      "    }"
      "  }"
      "  if (proto && typeof proto.transfer === 'function') {"
      "    try {"
      "      proto.transfer.call(buffer, byteLength);"
      "      return true;"
      "    } catch (e) {"
      "      lastError = e;"
      "    }"
      "    try {"
      "      proto.transfer.call(buffer);"
      "      return true;"
      "    } catch (e) {"
      "      lastError = e;"
      "    }"
      "  }"
      "  if (lastError) throw lastError;"
      "  return false;"
      "})";

  JSValueRef exception = nullptr;
  JSValueRef helper_value =
      JSEvaluateScript(env->context, JscString(kDetachHelper), nullptr, nullptr, 0, &exception);
  if (!napi_jsc_check_exception(env, exception)) return false;
  if (helper_value == nullptr || !JSValueIsObject(env->context, helper_value) ||
      !JSObjectIsFunction(env->context, ToJSObjectRef(helper_value))) {
    return false;
  }

  JSValueRef argv[1] = {object};
  JSValueRef helper_result = nullptr;
  if (!napi_jsc_call_function(env,
                              ToJSObjectRef(helper_value),
                              JSContextGetGlobalObject(env->context),
                              1,
                              argv,
                              &helper_result)) {
    return false;
  }
  if (helper_result == nullptr || !JSValueToBoolean(env->context, helper_result)) return false;
  return MarkDetachedArrayBuffer(env, object);
}

static bool SetIntegrityLevel(napi_env env, JSObjectRef object, const char* method_name) {
  napi_value global = nullptr;
  napi_value object_ctor = nullptr;
  napi_value method = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return false;
  status = napi_get_named_property(env, global, "Object", &object_ctor);
  if (status != napi_ok) return false;
  status = napi_get_named_property(env, object_ctor, method_name, &method);
  if (status != napi_ok || method == nullptr || !IsFunction(env, method)) return false;
  napi_value argv[1] = {napi_jsc_to_napi(object)};
  return napi_call_function(env, object_ctor, method, 1, argv, nullptr) == napi_ok;
}

static void DeleteDeferred(napi_deferred deferred) {
  if (deferred == nullptr || deferred->env == nullptr) {
    delete deferred;
    return;
  }
  deferred->env->Unprotect(deferred->promise);
  deferred->env->Unprotect(deferred->resolve);
  deferred->env->Unprotect(deferred->reject);
  delete deferred;
}

static bool ParseHexUint64(const std::string& hex, uint64_t* result_out) {
  if (result_out == nullptr) return false;
  uint64_t out = 0;
  for (char ch : hex) {
    uint8_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<uint8_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<uint8_t>(10 + ch - 'a');
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<uint8_t>(10 + ch - 'A');
    } else {
      return false;
    }
    out = static_cast<uint64_t>((out << 4) | digit);
  }
  *result_out = out;
  return true;
}

static napi_status GetTruncatedBigIntInt64(napi_env env,
                                           napi_value value,
                                           int64_t* result_out,
                                           bool* lossless_out) {
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result_out);
  NAPI_JSC_CHECK_ARG(env, lossless_out);
  if (!JSValueIsBigInt(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  }

  JSValueRef helper_value = JSEvaluateScript(
      env->context,
      JscString(
          "(function(v){const t=BigInt.asIntN(64,v);"
          "return [t===v,t<0n,(t<0n?(-t):t).toString(16)];})"),
      nullptr,
      nullptr,
      0,
      nullptr);
  if (helper_value == nullptr || !JSValueIsObject(env->context, helper_value)) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }

  JSValueRef argv[1] = {napi_jsc_to_js_value(value)};
  JSValueRef result_value = nullptr;
  if (!napi_jsc_call_function(env,
                              ToJSObjectRef(helper_value),
                              JSContextGetGlobalObject(env->context),
                              1,
                              argv,
                              &result_value) ||
      result_value == nullptr || !JSValueIsObject(env->context, result_value)) {
    return env->last_error.error_code;
  }

  auto* result_array = ToJSObjectRef(result_value);
  JSValueRef exception = nullptr;
  *lossless_out = JSValueToBoolean(
      env->context, JSObjectGetPropertyAtIndex(env->context, result_array, 0, &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  bool negative = JSValueToBoolean(
      env->context, JSObjectGetPropertyAtIndex(env->context, result_array, 1, &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  JSValueRef hex_value =
      JSObjectGetPropertyAtIndex(env->context, result_array, 2, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;

  std::string hex = napi_jsc_value_to_utf8(env, napi_jsc_to_napi(hex_value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  uint64_t magnitude = 0;
  if (!ParseHexUint64(hex, &magnitude)) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }

  if (!negative) {
    *result_out = static_cast<int64_t>(magnitude);
  } else if (magnitude == (uint64_t{1} << 63)) {
    *result_out = std::numeric_limits<int64_t>::min();
  } else {
    *result_out = -static_cast<int64_t>(magnitude);
  }
  return napi_jsc_clear_last_error(env);
}

static napi_status GetTruncatedBigIntUint64(napi_env env,
                                            napi_value value,
                                            uint64_t* result_out,
                                            bool* lossless_out) {
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result_out);
  NAPI_JSC_CHECK_ARG(env, lossless_out);
  if (!JSValueIsBigInt(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  }

  JSValueRef helper_value = JSEvaluateScript(
      env->context,
      JscString(
          "(function(v){const t=BigInt.asUintN(64,v);return [t===v,t.toString(16)];})"),
      nullptr,
      nullptr,
      0,
      nullptr);
  if (helper_value == nullptr || !JSValueIsObject(env->context, helper_value)) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }

  JSValueRef argv[1] = {napi_jsc_to_js_value(value)};
  JSValueRef result_value = nullptr;
  if (!napi_jsc_call_function(env,
                              ToJSObjectRef(helper_value),
                              JSContextGetGlobalObject(env->context),
                              1,
                              argv,
                              &result_value) ||
      result_value == nullptr || !JSValueIsObject(env->context, result_value)) {
    return env->last_error.error_code;
  }

  auto* result_array = ToJSObjectRef(result_value);
  JSValueRef exception = nullptr;
  *lossless_out = JSValueToBoolean(
      env->context, JSObjectGetPropertyAtIndex(env->context, result_array, 0, &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  JSValueRef hex_value =
      JSObjectGetPropertyAtIndex(env->context, result_array, 1, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;

  std::string hex = napi_jsc_value_to_utf8(env, napi_jsc_to_napi(hex_value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  if (!ParseHexUint64(hex, result_out)) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }
  return napi_jsc_clear_last_error(env);
}

extern "C" {

napi_status NAPI_CDECL napi_get_last_error_info(
    node_api_basic_env basic_env, const napi_extended_error_info** result) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  env->last_error.error_message =
      env->last_error_message.empty() ? nullptr : env->last_error_message.c_str();
  *result = &env->last_error;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_undefined(napi_env env, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_to_napi(JSValueMakeUndefined(env->context));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_null(napi_env env, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_to_napi(JSValueMakeNull(env->context));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_global(napi_env env, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_to_napi(JSContextGetGlobalObject(env->context));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_boolean(napi_env env, bool value, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_to_napi(JSValueMakeBoolean(env->context, value));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_object(napi_env env, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  *result = napi_jsc_to_napi(JSObjectMake(env->context, nullptr, nullptr));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_array(napi_env env, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSValueRef exception = nullptr;
  JSObjectRef array = JSObjectMakeArray(env->context, 0, nullptr, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(array);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_array_with_length(
    napi_env env, size_t length, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  const uint32_t array_length =
      length > std::numeric_limits<uint32_t>::max()
          ? std::numeric_limits<uint32_t>::max()
          : static_cast<uint32_t>(length);
  JSValueRef exception = nullptr;
  JSValueRef argv[1] = {
      JSValueMakeNumber(env->context, static_cast<double>(array_length)),
  };
  JSObjectRef array = JSObjectMakeArray(env->context, 1, argv, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(array);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_double(napi_env env, double value, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_to_napi(JSValueMakeNumber(env->context, value));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_int32(napi_env env, int32_t value, napi_value* result) {
  return napi_create_double(env, static_cast<double>(value), result);
}

napi_status NAPI_CDECL napi_create_uint32(napi_env env, uint32_t value, napi_value* result) {
  return napi_create_double(env, static_cast<double>(value), result);
}

napi_status NAPI_CDECL napi_create_int64(napi_env env, int64_t value, napi_value* result) {
  return napi_create_double(env, static_cast<double>(value), result);
}

napi_status NAPI_CDECL napi_create_string_latin1(
    napi_env env, const char* str, size_t length, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (str == nullptr) {
    if (length != 0) {
      return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    str = "";
  }
  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(str);
  }
  if (length > static_cast<size_t>(INT_MAX)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  *result = napi_jsc_to_napi(JSValueMakeString(env->context, JscString(CreateLatin1String(str, length))));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_string_utf8(
    napi_env env, const char* str, size_t length, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (str == nullptr) {
    if (length != 0) {
      return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    str = "";
  }
  if (length == NAPI_AUTO_LENGTH) {
    length = std::strlen(str);
  }
  if (length > static_cast<size_t>(INT_MAX)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  *result = napi_jsc_to_napi(JSValueMakeString(env->context, JscString(str, length)));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_string_utf16(
    napi_env env, const char16_t* str, size_t length, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (str == nullptr) {
    if (length != 0) {
      return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    static const char16_t empty[] = {0};
    str = empty;
  }
  if (length == NAPI_AUTO_LENGTH) {
    const char16_t* p = str;
    while (*p != 0) ++p;
    length = static_cast<size_t>(p - str);
  }
  if (length > static_cast<size_t>(INT_MAX)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  *result = napi_jsc_to_napi(JSValueMakeString(env->context, JscString(str, length)));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_external_string_latin1(
    napi_env env,
    char* str,
    size_t length,
    node_api_basic_finalize finalize_callback,
    void* finalize_hint,
    napi_value* result,
    bool* copied) {
  napi_status status = napi_create_string_latin1(env, str, length, result);
  if (status != napi_ok) return status;
  if (copied != nullptr) *copied = false;
  FinalizeWithBasicEnv(env, finalize_callback, str, finalize_hint);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_external_string_utf16(
    napi_env env,
    char16_t* str,
    size_t length,
    node_api_basic_finalize finalize_callback,
    void* finalize_hint,
    napi_value* result,
    bool* copied) {
  napi_status status = napi_create_string_utf16(env, str, length, result);
  if (status != napi_ok) return status;
  if (copied != nullptr) *copied = false;
  FinalizeWithBasicEnv(env, finalize_callback, str, finalize_hint);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_property_key_latin1(
    napi_env env, const char* str, size_t length, napi_value* result) {
  return napi_create_string_latin1(env, str, length, result);
}

napi_status NAPI_CDECL node_api_create_property_key_utf8(
    napi_env env, const char* str, size_t length, napi_value* result) {
  return napi_create_string_utf8(env, str, length, result);
}

napi_status NAPI_CDECL node_api_create_property_key_utf16(
    napi_env env, const char16_t* str, size_t length, napi_value* result) {
  return napi_create_string_utf16(env, str, length, result);
}

napi_status NAPI_CDECL napi_create_symbol(
    napi_env env, napi_value description, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSValueRef exception = nullptr;
  JSStringRef desc = nullptr;
  if (description != nullptr) {
    desc = JSValueToStringCopy(env->context, napi_jsc_to_js_value(description), &exception);
    if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  }
  *result = napi_jsc_to_napi(JSValueMakeSymbol(env->context, desc));
  if (desc != nullptr) JSStringRelease(desc);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_symbol_for(
    napi_env env, const char* utf8description, size_t length, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSObjectRef symbol_ctor = nullptr;
  if (!napi_jsc_get_global_function(env, "Symbol", &symbol_ctor)) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "Symbol constructor not available");
  }
  JSValueRef for_value = nullptr;
  if (!napi_jsc_get_named_property(env, symbol_ctor, "for", &for_value) ||
      for_value == nullptr || !JSValueIsObject(env->context, for_value)) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "Symbol.for not available");
  }
  JSValueRef argv[1] = {napi_jsc_make_string_utf8(env, utf8description, length)};
  JSValueRef out = nullptr;
  if (!napi_jsc_call_function(
          env, ToJSObjectRef(for_value), symbol_ctor, 1, argv, &out)) {
    return env->last_error.error_code;
  }
  *result = napi_jsc_to_napi(out);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_typeof(napi_env env,
                                   napi_value value,
                                   napi_valuetype* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = TypeOfValue(env, value);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_double(
    napi_env env, napi_value value, double* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!JSValueIsNumber(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_number_expected, "A number was expected");
  }
  JSValueRef exception = nullptr;
  *result = JSValueToNumber(env->context, napi_jsc_to_js_value(value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_int32(
    napi_env env, napi_value value, int32_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  double number = 0;
  napi_status status = napi_get_value_double(env, value, &number);
  if (status != napi_ok) return status;
  *result = EcmaScriptToInt32(number);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_uint32(
    napi_env env, napi_value value, uint32_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  double number = 0;
  napi_status status = napi_get_value_double(env, value, &number);
  if (status != napi_ok) return status;
  *result = EcmaScriptToUint32(number);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_int64(
    napi_env env, napi_value value, int64_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!JSValueIsNumber(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_number_expected, "A number was expected");
  }
  JSValueRef exception = nullptr;
  const double number =
      JSValueToNumber(env->context, napi_jsc_to_js_value(value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  if (!std::isfinite(number)) {
    *result = 0;
    return napi_jsc_clear_last_error(env);
  }
  if (number <= static_cast<double>(std::numeric_limits<int64_t>::min())) {
    *result = std::numeric_limits<int64_t>::min();
    return napi_jsc_clear_last_error(env);
  }
  if (number >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
    *result = std::numeric_limits<int64_t>::max();
    return napi_jsc_clear_last_error(env);
  }
  *result = static_cast<int64_t>(std::trunc(number));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_bool(
    napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!JSValueIsBoolean(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_boolean_expected, "A boolean was expected");
  }
  *result = JSValueToBoolean(env->context, napi_jsc_to_js_value(value));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_latin1(
    napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  if (!JSValueIsString(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_string_expected, "A string was expected");
  }
  if (buf == nullptr && result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  JSValueRef exception = nullptr;
  JscString string(JSValueToStringCopy(env->context, napi_jsc_to_js_value(value), &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  const size_t length = string.Length();
  if (buf == nullptr) {
    *result = length;
    return napi_jsc_clear_last_error(env);
  }
  if (bufsize > 0) {
    const JSChar* chars = JSStringGetCharactersPtr(string.get());
    const size_t written = std::min(length, bufsize - 1);
    for (size_t i = 0; i < written; ++i) {
      const JSChar ch = chars[i];
      buf[i] = (ch < 256) ? static_cast<char>(ch) : '?';
    }
    buf[written] = '\0';
    if (result != nullptr) *result = written;
  } else if (result != nullptr) {
    *result = 0;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_utf8(
    napi_env env, napi_value value, char* buf, size_t bufsize, size_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  if (!JSValueIsString(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_string_expected, "A string was expected");
  }
  if (buf == nullptr && result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  JSValueRef exception = nullptr;
  JscString string(JSValueToStringCopy(env->context, napi_jsc_to_js_value(value), &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  if (buf == nullptr) {
    *result = ToUtf8(string.get()).size();
    return napi_jsc_clear_last_error(env);
  }
  if (bufsize == 0) {
    if (result != nullptr) *result = 0;
    return napi_jsc_clear_last_error(env);
  }
  const size_t written = string.CopyUtf8(buf, bufsize);
  if (result != nullptr) *result = written == 0 ? 0 : written - 1;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_string_utf16(
    napi_env env, napi_value value, char16_t* buf, size_t bufsize, size_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  if (!JSValueIsString(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_string_expected, "A string was expected");
  }
  if (buf == nullptr && result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  JSValueRef exception = nullptr;
  JscString string(JSValueToStringCopy(env->context, napi_jsc_to_js_value(value), &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  const size_t length = string.Length();
  if (buf == nullptr) {
    *result = length;
    return napi_jsc_clear_last_error(env);
  }
  if (bufsize > 0) {
    const JSChar* chars = JSStringGetCharactersPtr(string.get());
    const size_t written = std::min(length, bufsize - 1);
    std::memcpy(buf, chars, written * sizeof(JSChar));
    buf[written] = 0;
    if (result != nullptr) *result = written;
  } else if (result != nullptr) {
    *result = 0;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_coerce_to_bool(napi_env env, napi_value value, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_to_napi(JSValueMakeBoolean(env->context, JSValueToBoolean(env->context, napi_jsc_to_js_value(value))));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_coerce_to_number(
    napi_env env, napi_value value, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (JSValueIsSymbol(env->context, napi_jsc_to_js_value(value))) {
    return ThrowErrorCommon(
        env, "TypeError", nullptr, "Cannot convert a Symbol value to a number");
  }
  JSValueRef exception = nullptr;
  const double number = JSValueToNumber(env->context, napi_jsc_to_js_value(value), &exception);
  if (exception != nullptr) {
    return napi_jsc_set_pending_exception(
        env, exception, "Exception during number coercion");
  }
  *result = napi_jsc_to_napi(JSValueMakeNumber(env->context, number));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_coerce_to_object(
    napi_env env, napi_value value, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSValueRef exception = nullptr;
  JSObjectRef object = JSValueToObject(env->context, napi_jsc_to_js_value(value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(object);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_coerce_to_string(
    napi_env env, napi_value value, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (JSValueIsSymbol(env->context, napi_jsc_to_js_value(value))) {
    return ThrowErrorCommon(
        env, "TypeError", nullptr, "Cannot convert a Symbol value to a string");
  }
  JSValueRef exception = nullptr;
  JscString string(JSValueToStringCopy(env->context, napi_jsc_to_js_value(value), &exception));
  if (exception != nullptr) {
    return napi_jsc_set_pending_exception(
        env, exception, "Exception during string coercion");
  }
  *result = napi_jsc_to_napi(JSValueMakeString(env->context, string));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_set_prototype(
    napi_env env, napi_value object, napi_value prototype) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, prototype);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object) || !IsObject(env, prototype)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  JSObjectSetPrototype(env->context, ToJSObject(env, object), ToJSObject(env, prototype));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_prototype(
    napi_env env, napi_value object, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  *result = napi_jsc_to_napi(JSObjectGetPrototype(env->context, ToJSObject(env, object)));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_strict_equals(
    napi_env env, napi_value lhs, napi_value rhs, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, lhs);
  NAPI_JSC_CHECK_ARG(env, rhs);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = JSValueIsStrictEqual(
      env->context, napi_jsc_to_js_value(lhs), napi_jsc_to_js_value(rhs));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_set_property(
    napi_env env, napi_value object, napi_value key, napi_value value) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, key);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  JSValueRef exception = nullptr;
  JSObjectSetPropertyForKey(
      env->context,
      ToJSObject(env, object),
      napi_jsc_to_js_value(key),
      napi_jsc_to_js_value(value),
      kJSPropertyAttributeNone,
      &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_has_property(
    napi_env env, napi_value object, napi_value key, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, key);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  JSValueRef exception = nullptr;
  *result = JSObjectHasPropertyForKey(
      env->context, ToJSObject(env, object), napi_jsc_to_js_value(key), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_property(
    napi_env env, napi_value object, napi_value key, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, key);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  JSValueRef exception = nullptr;
  JSValueRef property = JSObjectGetPropertyForKey(
      env->context, ToJSObject(env, object), napi_jsc_to_js_value(key), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(property);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_delete_property(
    napi_env env, napi_value object, napi_value key, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, key);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  JSValueRef exception = nullptr;
  bool deleted = JSObjectDeletePropertyForKey(
      env->context, ToJSObject(env, object), napi_jsc_to_js_value(key), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  if (result != nullptr) *result = deleted;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_has_own_property(
    napi_env env, napi_value object, napi_value key, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, key);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  if (!JSValueIsString(env->context, napi_jsc_to_js_value(key)) &&
      !JSValueIsSymbol(env->context, napi_jsc_to_js_value(key))) {
    return napi_jsc_set_last_error(
        env, napi_name_expected, "A string or symbol was expected");
  }
  napi_value global = nullptr;
  napi_value object_ctor = nullptr;
  napi_value prototype = nullptr;
  napi_value has_own_property = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, "Object", &object_ctor);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, object_ctor, "prototype", &prototype);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, prototype, "hasOwnProperty", &has_own_property);
  if (status != napi_ok) return status;
  napi_value call_result = nullptr;
  napi_value argv[1] = {key};
  status = napi_call_function(env, object, has_own_property, 1, argv, &call_result);
  if (status != napi_ok) return status;
  return napi_get_value_bool(env, call_result, result);
}

napi_status NAPI_CDECL napi_set_named_property(
    napi_env env, napi_value object, const char* utf8name, napi_value value) {
  napi_value key = nullptr;
  napi_status status = napi_create_string_utf8(env, utf8name, NAPI_AUTO_LENGTH, &key);
  if (status != napi_ok) return status;
  return napi_set_property(env, object, key, value);
}

napi_status NAPI_CDECL napi_has_named_property(
    napi_env env, napi_value object, const char* utf8name, bool* result) {
  napi_value key = nullptr;
  napi_status status = napi_create_string_utf8(env, utf8name, NAPI_AUTO_LENGTH, &key);
  if (status != napi_ok) return status;
  return napi_has_property(env, object, key, result);
}

napi_status NAPI_CDECL napi_get_named_property(
    napi_env env, napi_value object, const char* utf8name, napi_value* result) {
  napi_value key = nullptr;
  napi_status status = napi_create_string_utf8(env, utf8name, NAPI_AUTO_LENGTH, &key);
  if (status != napi_ok) return status;
  return napi_get_property(env, object, key, result);
}

napi_status NAPI_CDECL napi_set_element(
    napi_env env, napi_value object, uint32_t index, napi_value value) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  static constexpr const char kSetElementHelper[] =
      "(function(object, index, value) { 'use strict'; object[index] = value; })";
  JSValueRef argv[3] = {
      napi_jsc_to_js_value(object),
      JSValueMakeNumber(env->context, static_cast<double>(index)),
      napi_jsc_to_js_value(value),
  };
  return CallElementHelper(env, kSetElementHelper, 3, argv, nullptr);
}

napi_status NAPI_CDECL napi_has_element(
    napi_env env, napi_value object, uint32_t index, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  static constexpr const char kHasElementHelper[] =
      "(function(object, index) { return index in object; })";
  JSValueRef argv[2] = {
      napi_jsc_to_js_value(object),
      JSValueMakeNumber(env->context, static_cast<double>(index)),
  };
  napi_value helper_result = nullptr;
  napi_status status = CallElementHelper(env, kHasElementHelper, 2, argv, &helper_result);
  if (status != napi_ok) return status;
  return napi_get_value_bool(env, helper_result, result);
}

napi_status NAPI_CDECL napi_get_element(
    napi_env env, napi_value object, uint32_t index, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  static constexpr const char kGetElementHelper[] =
      "(function(object, index) { return object[index]; })";
  JSValueRef argv[2] = {
      napi_jsc_to_js_value(object),
      JSValueMakeNumber(env->context, static_cast<double>(index)),
  };
  return CallElementHelper(env, kGetElementHelper, 2, argv, result);
}

napi_status NAPI_CDECL napi_delete_element(
    napi_env env, napi_value object, uint32_t index, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  static constexpr const char kDeleteElementHelper[] =
      "(function(object, index) { 'use strict'; return delete object[index]; })";
  JSValueRef argv[2] = {
      napi_jsc_to_js_value(object),
      JSValueMakeNumber(env->context, static_cast<double>(index)),
  };
  napi_value helper_result = nullptr;
  napi_status status = CallElementHelper(env, kDeleteElementHelper, 2, argv, &helper_result);
  if (status != napi_ok) return status;
  if (result != nullptr) {
    status = napi_get_value_bool(env, helper_result, result);
    if (status != napi_ok) return status;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_define_properties(
    napi_env env,
    napi_value object,
    size_t property_count,
    const napi_property_descriptor* properties) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  if (property_count > 0 && properties == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  for (size_t i = 0; i < property_count; ++i) {
    JSValueRef key = nullptr;
    if (properties[i].utf8name != nullptr) {
      key = napi_jsc_make_string_utf8(env, properties[i].utf8name, NAPI_AUTO_LENGTH);
    } else if (properties[i].name != nullptr) {
      key = napi_jsc_to_js_value(properties[i].name);
    } else {
      return napi_jsc_set_last_error(env, napi_name_expected, "A string or symbol was expected");
    }
    napi_value method = nullptr;
    napi_value getter = nullptr;
    napi_value setter = nullptr;
    if (properties[i].method != nullptr) {
      napi_status status = napi_create_function(
          env,
          properties[i].utf8name,
          NAPI_AUTO_LENGTH,
          properties[i].method,
          properties[i].data,
          &method);
      if (status != napi_ok) return status;
    }
    if (properties[i].getter != nullptr) {
      napi_status status = napi_create_function(
          env,
          properties[i].utf8name,
          NAPI_AUTO_LENGTH,
          properties[i].getter,
          properties[i].data,
          &getter);
      if (status != napi_ok) return status;
    }
    if (properties[i].setter != nullptr) {
      napi_status status = napi_create_function(
          env,
          properties[i].utf8name,
          NAPI_AUTO_LENGTH,
          properties[i].setter,
          properties[i].data,
          &setter);
      if (status != napi_ok) return status;
    }
    JSValueRef value = method != nullptr ? napi_jsc_to_js_value(method)
                                         : napi_jsc_to_js_value(properties[i].value);
    if (!napi_jsc_define_property(
            env,
            ToJSObject(env, object),
            key,
            value,
            getter != nullptr ? napi_jsc_to_js_value(getter) : nullptr,
            setter != nullptr ? napi_jsc_to_js_value(setter) : nullptr,
            properties[i].attributes)) {
      return env->last_error.error_code;
    }
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_is_array(napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = JSValueIsArray(env->context, napi_jsc_to_js_value(value));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_array_length(
    napi_env env, napi_value value, uint32_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  bool is_array = false;
  napi_status status = napi_is_array(env, value, &is_array);
  if (status != napi_ok) return status;
  if (!is_array) return napi_jsc_set_last_error(env, napi_array_expected, "An array was expected");
  napi_value length_value = nullptr;
  status = napi_get_named_property(env, value, "length", &length_value);
  if (status != napi_ok) return status;
  return napi_get_value_uint32(env, length_value, result);
}

napi_status NAPI_CDECL napi_get_property_names(
    napi_env env, napi_value object, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSObjectRef names = nullptr;
  if (!GetAllPropertyNamesImpl(env,
                               ToJSObject(env, object),
                               napi_key_include_prototypes,
                               static_cast<napi_key_filter>(napi_key_enumerable |
                                                            napi_key_skip_symbols),
                               napi_key_numbers_to_strings,
                               &names)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  *result = napi_jsc_to_napi(names);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_cb_info(
    napi_env env,
    napi_callback_info cbinfo,
    size_t* argc,
    napi_value* argv,
    napi_value* this_arg,
    void** data) {
  NAPI_JSC_CHECK_ENV(env);
  if (cbinfo == nullptr) return napi_invalid_arg;
  const size_t provided = argc == nullptr ? 0 : *argc;
  if (argc != nullptr) *argc = cbinfo->args.size();
  if (argv != nullptr) {
    const size_t count = std::min(provided, cbinfo->args.size());
    for (size_t i = 0; i < count; ++i) argv[i] = cbinfo->args[i];
    for (size_t i = count; i < provided; ++i) {
      argv[i] = napi_jsc_to_napi(JSValueMakeUndefined(env->context));
    }
  }
  if (this_arg != nullptr) *this_arg = cbinfo->this_arg;
  if (data != nullptr) *data = cbinfo->data;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_new_target(
    napi_env env, napi_callback_info cbinfo, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (cbinfo == nullptr || result == nullptr) return napi_invalid_arg;
  *result = cbinfo->new_target;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_function(
    napi_env env,
    const char* utf8name,
    size_t length,
    napi_callback cb,
    void* callback_data,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (cb == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  return FunctionInfo::Create(env, utf8name, length, cb, callback_data, result);
}

napi_status NAPI_CDECL napi_define_class(
    napi_env env,
    const char* utf8name,
    size_t length,
    napi_callback cb,
    void* data,
    size_t property_count,
    const napi_property_descriptor* properties,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (utf8name == nullptr || cb == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (property_count > 0 && properties == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  napi_status status = FunctionInfo::Create(env, utf8name, length, cb, data, result);
  if (status != napi_ok) return status;
  napi_value prototype = nullptr;
  status = napi_get_named_property(env, *result, "prototype", &prototype);
  if (status != napi_ok) return status;
  for (size_t i = 0; i < property_count; ++i) {
    napi_value target = (properties[i].attributes & napi_static) ? *result : prototype;
    napi_property_descriptor desc = properties[i];
    desc.attributes = static_cast<napi_property_attributes>(desc.attributes & ~napi_static);
    status = napi_define_properties(env, target, 1, &desc);
    if (status != napi_ok) return status;
  }
  return napi_ok;
}

napi_status NAPI_CDECL napi_call_function(
    napi_env env,
    napi_value recv,
    napi_value func,
    size_t argc,
    const napi_value* argv,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (recv == nullptr || func == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsFunction(env, func)) {
    return napi_jsc_set_last_error(env, napi_function_expected, "A function was expected");
  }
  std::vector<JSValueRef> args;
  args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) args.push_back(napi_jsc_to_js_value(argv[i]));
  JSValueRef out = nullptr;
  if (!napi_jsc_call_function(
          env, ToJSObject(env, func), ToJSObject(env, recv), argc, args.data(), &out)) {
    return env->last_error.error_code;
  }
  if (result != nullptr) *result = napi_jsc_to_napi(out);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_new_instance(
    napi_env env,
    napi_value constructor,
    size_t argc,
    const napi_value* argv,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (constructor == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsConstructor(env, constructor)) {
    return napi_jsc_set_last_error(env, napi_function_expected, "A function was expected");
  }
  std::vector<JSValueRef> args;
  args.reserve(argc);
  for (size_t i = 0; i < argc; ++i) args.push_back(napi_jsc_to_js_value(argv[i]));
  JSObjectRef object = nullptr;
  if (!napi_jsc_call_constructor(env, ToJSObject(env, constructor), argc, args.data(), &object)) {
    return env->last_error.error_code;
  }
  *result = napi_jsc_to_napi(object);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_instanceof(
    napi_env env, napi_value object, napi_value constructor, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (object == nullptr || constructor == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (!IsConstructor(env, constructor)) {
    return napi_jsc_set_last_error(env, napi_function_expected, "A function was expected");
  }
  JSValueRef exception = nullptr;
  *result = JSValueIsInstanceOfConstructor(
      env->context, napi_jsc_to_js_value(object), ToJSObject(env, constructor), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_wrap(
    napi_env env,
    napi_value js_object,
    void* native_object,
    node_api_basic_finalize finalize_cb,
    void* finalize_hint,
    napi_ref* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (js_object == nullptr || !IsObject(env, js_object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  WrapperInfo* info = nullptr;
  napi_status status = WrapperInfo::Wrap(env, js_object, &info);
  if (status != napi_ok) return status;
  if (info->Data() != nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  info->SetData(native_object);
  if (finalize_cb != nullptr) {
    info->AddFinalizer([finalize_cb, finalize_hint](WrapperInfo* self) {
      FinalizeWithBasicEnv(self->Env(), finalize_cb, self->Data(), finalize_hint);
    });
  }
  if (result != nullptr) {
    return napi_create_reference(env, js_object, 0, result);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_unwrap(
    napi_env env, napi_value js_object, void** result) {
  NAPI_JSC_CHECK_ENV(env);
  if (js_object == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  WrapperInfo* info = nullptr;
  napi_status status = WrapperInfo::Unwrap(env, js_object, &info);
  if (status != napi_ok) return status;
  if (info == nullptr || info->Data() == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  *result = info->Data();
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_remove_wrap(
    napi_env env, napi_value js_object, void** result) {
  NAPI_JSC_CHECK_ENV(env);
  if (js_object == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  WrapperInfo* info = nullptr;
  napi_status status = WrapperInfo::Unwrap(env, js_object, &info);
  if (status != napi_ok) return status;
  if (info == nullptr || info->Data() == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  *result = info->Data();
  info->SetData(nullptr);
  info->ClearFinalizers();
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_external(
    napi_env env,
    void* data,
    node_api_basic_finalize finalize_cb,
    void* finalize_hint,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  return ExternalInfo::Create(env, data, finalize_cb, finalize_hint, result);
}

napi_status NAPI_CDECL napi_get_value_external(
    napi_env env, napi_value value, void** result) {
  NAPI_JSC_CHECK_ENV(env);
  if (value == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (!IsObject(env, value)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  ExternalInfo* info = nullptr;
  napi_status status = NativeInfo::Query(env, ToJSObject(env, value), &info);
  if (status != napi_ok) return status;
  if (info == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  *result = info->Data();
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_reference(
    napi_env env, napi_value value, uint32_t initial_refcount, napi_ref* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (value == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  auto* ref = new (std::nothrow) napi_ref__();
  if (ref == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  napi_status status = ref->init(env, value, initial_refcount);
  if (status != napi_ok) {
    delete ref;
    return status;
  }
  *result = ref;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_delete_reference(node_api_basic_env basic_env, napi_ref ref) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  if (ref == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  ref->deinit(env);
  delete ref;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_reference_ref(
    napi_env env, napi_ref ref, uint32_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (ref == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  ref->ref(env);
  if (result != nullptr) *result = ref->count();
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_reference_unref(
    napi_env env, napi_ref ref, uint32_t* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (ref == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  if (ref->count() == 0) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  ref->unref(env);
  if (result != nullptr) *result = ref->count();
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_reference_value(
    napi_env env, napi_ref ref, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (ref == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  napi_status status = ref->value(env, result);
  if (status != napi_ok) return status;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_open_handle_scope(
    napi_env env, napi_handle_scope* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  auto* scope = new (std::nothrow) napi_handle_scope__();
  if (scope == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  scope->env = env;
  *result = scope;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_close_handle_scope(
    napi_env env, napi_handle_scope scope) {
  NAPI_JSC_CHECK_ENV(env);
  if (scope == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  delete scope;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_open_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  auto* scope = new (std::nothrow) napi_escapable_handle_scope__();
  if (scope == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  scope->env = env;
  *result = scope;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_close_escapable_handle_scope(
    napi_env env, napi_escapable_handle_scope scope) {
  NAPI_JSC_CHECK_ENV(env);
  if (scope == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  delete scope;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_escape_handle(
    napi_env env,
    napi_escapable_handle_scope scope,
    napi_value escapee,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (scope == nullptr || escapee == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (scope->escaped) {
    return napi_jsc_set_last_error(env, napi_escape_called_twice);
  }
  scope->escaped = true;
  *result = escapee;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_error(
    napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return CreateErrorValue(env, "Error", code, msg, result);
}

napi_status NAPI_CDECL napi_create_type_error(
    napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return CreateErrorValue(env, "TypeError", code, msg, result);
}

napi_status NAPI_CDECL napi_create_range_error(
    napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return CreateErrorValue(env, "RangeError", code, msg, result);
}

napi_status NAPI_CDECL node_api_create_syntax_error(
    napi_env env, napi_value code, napi_value msg, napi_value* result) {
  return CreateErrorValue(env, "SyntaxError", code, msg, result);
}

napi_status NAPI_CDECL napi_throw(napi_env env, napi_value error) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, error);
  env->SetLastException(napi_jsc_to_js_value(error));
  return napi_jsc_set_last_error(env, napi_pending_exception);
}

napi_status NAPI_CDECL napi_throw_error(
    napi_env env, const char* code, const char* msg) {
  return ThrowErrorCommon(env, "Error", code, msg != nullptr ? msg : "Error");
}

napi_status NAPI_CDECL napi_throw_type_error(
    napi_env env, const char* code, const char* msg) {
  return ThrowErrorCommon(env, "TypeError", code, msg != nullptr ? msg : "TypeError");
}

napi_status NAPI_CDECL napi_throw_range_error(
    napi_env env, const char* code, const char* msg) {
  return ThrowErrorCommon(env, "RangeError", code, msg != nullptr ? msg : "RangeError");
}

napi_status NAPI_CDECL node_api_throw_syntax_error(
    napi_env env, const char* code, const char* msg) {
  return ThrowErrorCommon(env, "SyntaxError", code, msg != nullptr ? msg : "SyntaxError");
}

napi_status NAPI_CDECL napi_is_error(napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  if (value == nullptr || result == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  napi_value global = nullptr;
  napi_value ctor = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, "Error", &ctor);
  if (status != napi_ok) return status;
  return napi_instanceof(env, value, ctor, result);
}

napi_status NAPI_CDECL napi_is_exception_pending(napi_env env, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = env->last_exception != nullptr;
  return napi_ok;
}

napi_status NAPI_CDECL napi_get_and_clear_last_exception(
    napi_env env, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  if (env->last_exception == nullptr) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "No pending exception");
  }
  *result = napi_jsc_to_napi(env->last_exception);
  env->ClearLastException();
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL
node_api_create_object_with_properties(napi_env env,
                                       napi_value prototype_or_null,
                                       napi_value* property_names,
                                       napi_value* property_values,
                                       size_t property_count,
                                       napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  if (property_count > 0 && (property_names == nullptr || property_values == nullptr)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  JSObjectRef object = JSObjectMake(env->context, nullptr, nullptr);
  if (prototype_or_null != nullptr) {
    JSValueRef prototype = napi_jsc_to_js_value(prototype_or_null);
    if (!JSValueIsNull(env->context, prototype) &&
        !JSValueIsObject(env->context, prototype)) {
      return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
    }
    JSObjectSetPrototype(env->context, object, prototype);
  }

  for (size_t i = 0; i < property_count; ++i) {
    if (property_names[i] == nullptr || property_values[i] == nullptr) {
      return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
    }
    JSValueRef exception = nullptr;
    JSObjectSetPropertyForKey(env->context,
                              object,
                              napi_jsc_to_js_value(property_names[i]),
                              napi_jsc_to_js_value(property_values[i]),
                              kJSPropertyAttributeNone,
                              &exception);
    if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  }

  *result = napi_jsc_to_napi(object);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_is_arraybuffer(napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, value)) {
    *result = false;
    return napi_jsc_clear_last_error(env);
  }
  JSValueRef exception = nullptr;
  *result =
      JSValueGetTypedArrayType(env->context, napi_jsc_to_js_value(value), &exception) ==
      kJSTypedArrayTypeArrayBuffer;
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_arraybuffer(napi_env env,
                                               size_t byte_length,
                                               void** data,
                                               napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  void* bytes = byte_length == 0 ? nullptr : std::malloc(byte_length);
  if (byte_length > 0 && bytes == nullptr) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }

  auto* context = new (std::nothrow) BufferDeallocatorContext();
  if (context == nullptr) {
    std::free(bytes);
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }
  context->env = env;
  context->free_data = true;
  JSValueRef exception = nullptr;
  JSObjectRef arraybuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
      env->context, bytes, byte_length, BufferBytesDeallocator, context, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;

  if (data != nullptr) *data = bytes;
  *result = napi_jsc_to_napi(arraybuffer);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_external_arraybuffer(
    napi_env env,
    void* external_data,
    size_t byte_length,
    node_api_basic_finalize finalize_cb,
    void* finalize_hint,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  if (external_data == nullptr && byte_length != 0) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  JSValueRef exception = nullptr;
  if (external_data == nullptr && byte_length == 0) {
    JSObjectRef arraybuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
        env->context, nullptr, 0, nullptr, nullptr, &exception);
    if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
    if (!MarkDetachedArrayBuffer(env, arraybuffer)) return env->last_error.error_code;
    *result = napi_jsc_to_napi(arraybuffer);
    return napi_jsc_clear_last_error(env);
  }

  auto* context = new (std::nothrow) BufferDeallocatorContext();
  if (context == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  context->env = env;
  context->data = external_data;
  context->finalize_cb = finalize_cb;
  context->finalize_hint = finalize_hint;
  JSObjectRef arraybuffer = JSObjectMakeArrayBufferWithBytesNoCopy(
      env->context,
      external_data,
      byte_length,
      BufferBytesDeallocator,
      context,
      &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(arraybuffer);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_arraybuffer_info(
    napi_env env, napi_value arraybuffer, void** data, size_t* byte_length) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, arraybuffer);

  if (IsSharedArrayBuffer(env, arraybuffer)) {
    if (!GetSharedArrayBufferInfo(env, ToJSObject(env, arraybuffer), data, byte_length)) {
      return env->last_error.error_code == napi_ok
                 ? napi_jsc_set_last_error(env, napi_generic_failure)
                 : env->last_error.error_code;
    }
    return napi_jsc_clear_last_error(env);
  }

  bool is_arraybuffer = false;
  napi_status status = napi_is_arraybuffer(env, arraybuffer, &is_arraybuffer);
  if (status != napi_ok) return status;
  if (!is_arraybuffer) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  JSObjectRef object = ToJSObject(env, arraybuffer);
  if (IsMarkedDetachedArrayBuffer(env, object)) {
    if (data != nullptr) *data = nullptr;
    if (byte_length != nullptr) *byte_length = 0;
    return napi_jsc_clear_last_error(env);
  }

  JSValueRef exception = nullptr;
  if (data != nullptr) {
    *data = JSObjectGetArrayBufferBytesPtr(env->context, object, &exception);
    if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  }
  if (byte_length != nullptr) {
    *byte_length = JSObjectGetArrayBufferByteLength(env->context, object, &exception);
    if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_is_typedarray(napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, value)) {
    *result = false;
    return napi_jsc_clear_last_error(env);
  }
  JSValueRef exception = nullptr;
  JSTypedArrayType type =
      JSValueGetTypedArrayType(env->context, napi_jsc_to_js_value(value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = (type != kJSTypedArrayTypeNone && type != kJSTypedArrayTypeArrayBuffer) ||
            IsTypedArrayByName(env, value, "Float16Array");
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_typedarray(napi_env env,
                                              napi_typedarray_type type,
                                              size_t length,
                                              napi_value arraybuffer,
                                              size_t byte_offset,
                                              napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, arraybuffer);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  const char* ctor_name = TypedArrayConstructorName(type);
  if (ctor_name == nullptr || TypedArrayElementSize(type) == 0) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  bool is_arraybuffer = false;
  napi_status status = napi_is_arraybuffer(env, arraybuffer, &is_arraybuffer);
  if (status != napi_ok) return status;
  if (!is_arraybuffer && !IsSharedArrayBuffer(env, arraybuffer)) {
    return napi_jsc_set_last_error(env, napi_arraybuffer_expected, "An ArrayBuffer was expected");
  }

  napi_value global = nullptr;
  napi_value ctor = nullptr;
  status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, ctor_name, &ctor);
  if (status != napi_ok || ctor == nullptr || !IsFunction(env, ctor)) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "TypedArray constructor not available");
  }

  napi_value offset_value = nullptr;
  napi_value length_value = nullptr;
  status = napi_create_double(env, static_cast<double>(byte_offset), &offset_value);
  if (status != napi_ok) return status;
  status = napi_create_double(env, static_cast<double>(length), &length_value);
  if (status != napi_ok) return status;
  napi_value argv[3] = {arraybuffer, offset_value, length_value};
  return napi_new_instance(env, ctor, 3, argv, result);
}

napi_status NAPI_CDECL napi_get_typedarray_info(napi_env env,
                                                napi_value typedarray,
                                                napi_typedarray_type* type,
                                                size_t* length,
                                                void** data,
                                                napi_value* arraybuffer,
                                                size_t* byte_offset) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, typedarray);
  if (!GetTypedArrayInfoImpl(env, typedarray, type, length, data, arraybuffer, byte_offset)) {
    return env->last_error.error_code != napi_ok
               ? env->last_error.error_code
               : napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_dataview(napi_env env,
                                            size_t length,
                                            napi_value arraybuffer,
                                            size_t byte_offset,
                                            napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, arraybuffer);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  bool is_arraybuffer = false;
  napi_status status = napi_is_arraybuffer(env, arraybuffer, &is_arraybuffer);
  if (status != napi_ok) return status;
  if (!is_arraybuffer && !IsSharedArrayBuffer(env, arraybuffer)) {
    return napi_jsc_set_last_error(env, napi_arraybuffer_expected, "An ArrayBuffer was expected");
  }

  napi_value global = nullptr;
  napi_value ctor = nullptr;
  status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, "DataView", &ctor);
  if (status != napi_ok || ctor == nullptr || !IsFunction(env, ctor)) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "DataView constructor not available");
  }

  napi_value offset_value = nullptr;
  napi_value length_value = nullptr;
  status = napi_create_double(env, static_cast<double>(byte_offset), &offset_value);
  if (status != napi_ok) return status;
  status = napi_create_double(env, static_cast<double>(length), &length_value);
  if (status != napi_ok) return status;
  napi_value argv[3] = {arraybuffer, offset_value, length_value};
  return napi_new_instance(env, ctor, 3, argv, result);
}

napi_status NAPI_CDECL napi_is_dataview(napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, value)) {
    *result = false;
    return napi_jsc_clear_last_error(env);
  }
  napi_value global = nullptr;
  napi_value ctor = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, "DataView", &ctor);
  if (status != napi_ok || ctor == nullptr || !IsFunction(env, ctor)) {
    *result = false;
    return napi_jsc_clear_last_error(env);
  }
  return napi_instanceof(env, value, ctor, result);
}

napi_status NAPI_CDECL napi_get_dataview_info(napi_env env,
                                              napi_value dataview,
                                              size_t* bytelength,
                                              void** data,
                                              napi_value* arraybuffer,
                                              size_t* byte_offset) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, dataview);

  bool is_dataview = false;
  napi_status status = napi_is_dataview(env, dataview, &is_dataview);
  if (status != napi_ok) return status;
  if (!is_dataview) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  napi_value buffer = nullptr;
  status = napi_get_named_property(env, dataview, "buffer", &buffer);
  if (status != napi_ok) return status;
  if (arraybuffer != nullptr) *arraybuffer = buffer;

  size_t offset = 0;
  napi_value offset_value = nullptr;
  status = napi_get_named_property(env, dataview, "byteOffset", &offset_value);
  if (status != napi_ok) return status;
  double offset_number = 0;
  status = napi_get_value_double(env, offset_value, &offset_number);
  if (status != napi_ok) return status;
  offset = static_cast<size_t>(offset_number);
  if (byte_offset != nullptr) *byte_offset = offset;

  if (bytelength != nullptr) {
    napi_value length_value = nullptr;
    double length_number = 0;
    status = napi_get_named_property(env, dataview, "byteLength", &length_value);
    if (status != napi_ok) return status;
    status = napi_get_value_double(env, length_value, &length_number);
    if (status != napi_ok) return status;
    *bytelength = static_cast<size_t>(length_number);
  }

  if (data != nullptr) {
    void* buffer_bytes = nullptr;
    status = napi_get_arraybuffer_info(env, buffer, &buffer_bytes, nullptr);
    if (status != napi_ok) return status;
    *data = buffer_bytes == nullptr
                ? nullptr
                : static_cast<void*>(static_cast<uint8_t*>(buffer_bytes) + offset);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_is_sharedarraybuffer(
    napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = IsSharedArrayBuffer(env, value);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_sharedarraybuffer(
    napi_env env, size_t byte_length, void** data, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  napi_value global = nullptr;
  napi_value ctor = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, "SharedArrayBuffer", &ctor);
  if (status != napi_ok || ctor == nullptr || !IsFunction(env, ctor)) {
    return napi_jsc_set_last_error(
        env, napi_generic_failure, "SharedArrayBuffer constructor not available");
  }

  napi_value length_value = nullptr;
  status = napi_create_double(env, static_cast<double>(byte_length), &length_value);
  if (status != napi_ok) return status;
  napi_value argv[1] = {length_value};
  status = napi_new_instance(env, ctor, 1, argv, result);
  if (status != napi_ok) return status;

  if (data != nullptr &&
      !GetSharedArrayBufferInfo(env, ToJSObject(env, *result), data, nullptr)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_version(node_api_basic_env basic_env, uint32_t* result) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = NAPI_VERSION;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_promise(napi_env env,
                                           napi_deferred* deferred,
                                           napi_value* promise) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, deferred);
  NAPI_JSC_CHECK_ARG(env, promise);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  auto* out = new (std::nothrow) napi_deferred__();
  if (out == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  out->env = env;

  JSValueRef exception = nullptr;
  out->promise = JSObjectMakeDeferredPromise(env->context, &out->resolve, &out->reject, &exception);
  if (!napi_jsc_check_exception(env, exception) || out->promise == nullptr ||
      out->resolve == nullptr || out->reject == nullptr) {
    delete out;
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }

  env->Protect(out->promise);
  env->Protect(out->resolve);
  env->Protect(out->reject);
  *deferred = out;
  *promise = napi_jsc_to_napi(out->promise);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_resolve_deferred(napi_env env,
                                             napi_deferred deferred,
                                             napi_value resolution) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, deferred);
  NAPI_JSC_CHECK_ARG(env, resolution);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  JSValueRef argv[1] = {napi_jsc_to_js_value(resolution)};
  if (!napi_jsc_call_function(
          env, deferred->resolve, JSContextGetGlobalObject(env->context), 1, argv, nullptr)) {
    return env->last_error.error_code;
  }
  DeleteDeferred(deferred);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_reject_deferred(napi_env env,
                                            napi_deferred deferred,
                                            napi_value rejection) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, deferred);
  NAPI_JSC_CHECK_ARG(env, rejection);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  JSValueRef argv[1] = {napi_jsc_to_js_value(rejection)};
  if (!napi_jsc_call_function(
          env, deferred->reject, JSContextGetGlobalObject(env->context), 1, argv, nullptr)) {
    return env->last_error.error_code;
  }
  DeleteDeferred(deferred);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_is_promise(napi_env env,
                                       napi_value value,
                                       bool* is_promise) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, is_promise);
  *is_promise = IsTypedArrayByName(env, value, "Promise");
  if (!*is_promise) {
    napi_value global = nullptr;
    napi_value ctor = nullptr;
    if (napi_get_global(env, &global) == napi_ok &&
        napi_get_named_property(env, global, "Promise", &ctor) == napi_ok &&
        ctor != nullptr && IsFunction(env, ctor)) {
      bool result = false;
      if (napi_instanceof(env, value, ctor, &result) == napi_ok) *is_promise = result;
    }
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_run_script(napi_env env,
                                       napi_value script,
                                       napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, script);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!JSValueIsString(env->context, napi_jsc_to_js_value(script))) {
    return napi_jsc_set_last_error(env, napi_string_expected, "A string was expected");
  }
  JSValueRef exception = nullptr;
  JscString source(JSValueToStringCopy(env->context, napi_jsc_to_js_value(script), &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result =
      napi_jsc_to_napi(JSEvaluateScript(env->context, source, nullptr, nullptr, 0, &exception));
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_adjust_external_memory(
    node_api_basic_env basic_env, int64_t change_in_bytes, int64_t* adjusted_value) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, adjusted_value);
  *adjusted_value = change_in_bytes;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_date(napi_env env, double time, napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  napi_value global = nullptr;
  napi_value ctor = nullptr;
  napi_status status = napi_get_global(env, &global);
  if (status != napi_ok) return status;
  status = napi_get_named_property(env, global, "Date", &ctor);
  if (status != napi_ok || ctor == nullptr || !IsFunction(env, ctor)) {
    return napi_jsc_set_last_error(env, napi_generic_failure, "Date constructor not available");
  }
  napi_value time_value = nullptr;
  status = napi_create_double(env, time, &time_value);
  if (status != napi_ok) return status;
  napi_value argv[1] = {time_value};
  return napi_new_instance(env, ctor, 1, argv, result);
}

napi_status NAPI_CDECL napi_is_date(napi_env env, napi_value value, bool* is_date) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, is_date);
  *is_date = JSValueIsDate(env->context, napi_jsc_to_js_value(value));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_date_value(napi_env env,
                                           napi_value value,
                                           double* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!JSValueIsDate(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_date_expected, "A date was expected");
  }
  JSValueRef exception = nullptr;
  *result = JSValueToNumber(env->context, napi_jsc_to_js_value(value), &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_add_finalizer(napi_env env,
                                          napi_value js_object,
                                          void* finalize_data,
                                          node_api_basic_finalize finalize_cb,
                                          void* finalize_hint,
                                          napi_ref* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, js_object);
  if (finalize_cb == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (!IsObject(env, js_object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  WrapperInfo* info = nullptr;
  napi_status status = WrapperInfo::Wrap(env, js_object, &info);
  if (status != napi_ok) return status;
  info->AddFinalizer([finalize_cb, finalize_data, finalize_hint](WrapperInfo* self) {
    FinalizeWithBasicEnv(self->Env(), finalize_cb, finalize_data, finalize_hint);
  });
  if (result != nullptr) return napi_create_reference(env, js_object, 0, result);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_bigint_int64(napi_env env,
                                                int64_t value,
                                                napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  JSValueRef exception = nullptr;
  JSValueRef bigint = JSBigIntCreateWithInt64(env->context, value, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(bigint);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_bigint_uint64(napi_env env,
                                                 uint64_t value,
                                                 napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  JSValueRef exception = nullptr;
  JSValueRef bigint = JSBigIntCreateWithUInt64(env->context, value, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(bigint);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_bigint_words(napi_env env,
                                                int sign_bit,
                                                size_t word_count,
                                                const uint64_t* words,
                                                napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  if ((sign_bit != 0 && sign_bit != 1) || word_count > static_cast<size_t>(INT_MAX)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (word_count > 0 && words == nullptr) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (word_count == static_cast<size_t>(INT_MAX)) {
    return napi_throw_range_error(env, nullptr, "Maximum BigInt size exceeded");
  }
  JSValueRef exception = nullptr;
  JSValueRef bigint = CreateBigIntFromWords(env, sign_bit, word_count, words, &exception);
  if (!napi_jsc_check_exception(env, exception)) return env->last_error.error_code;
  *result = napi_jsc_to_napi(bigint);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_value_bigint_int64(napi_env env,
                                                   napi_value value,
                                                   int64_t* result,
                                                   bool* lossless) {
  NAPI_JSC_CHECK_ENV(env);
  return GetTruncatedBigIntInt64(env, value, result, lossless);
}

napi_status NAPI_CDECL napi_get_value_bigint_uint64(napi_env env,
                                                    napi_value value,
                                                    uint64_t* result,
                                                    bool* lossless) {
  NAPI_JSC_CHECK_ENV(env);
  return GetTruncatedBigIntUint64(env, value, result, lossless);
}

napi_status NAPI_CDECL napi_get_value_bigint_words(napi_env env,
                                                   napi_value value,
                                                   int* sign_bit,
                                                   size_t* word_count,
                                                   uint64_t* words) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, word_count);
  if (!JSValueIsBigInt(env->context, napi_jsc_to_js_value(value))) {
    return napi_jsc_set_last_error(env, napi_bigint_expected, "A bigint was expected");
  }

  std::string hex;
  bool negative = false;
  if (!BigIntToHexString(env, value, &hex, &negative)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }

  if (hex == "0") {
    if (sign_bit != nullptr) *sign_bit = 0;
    *word_count = 0;
    return napi_jsc_clear_last_error(env);
  }

  const size_t required = (hex.size() + 15) / 16;
  const size_t available = *word_count;
  *word_count = required;
  if (sign_bit != nullptr) *sign_bit = negative ? 1 : 0;

  if (words == nullptr || available == 0) return napi_jsc_clear_last_error(env);
  const size_t to_write = std::min(available, required);
  for (size_t i = 0; i < to_write; ++i) {
    const size_t end = hex.size() - (i * 16);
    const size_t start = end > 16 ? end - 16 : 0;
    uint64_t word = 0;
    if (!ParseHexUint64(hex.substr(start, end - start), &word)) {
      return napi_jsc_set_last_error(env, napi_generic_failure);
    }
    words[i] = word;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_all_property_names(napi_env env,
                                                   napi_value object,
                                                   napi_key_collection_mode key_mode,
                                                   napi_key_filter key_filter,
                                                   napi_key_conversion key_conversion,
                                                   napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  JSObjectRef names = nullptr;
  if (!GetAllPropertyNamesImpl(
          env, ToJSObject(env, object), key_mode, key_filter, key_conversion, &names)) {
    return env->last_error.error_code == napi_ok
               ? napi_jsc_set_last_error(env, napi_generic_failure)
               : env->last_error.error_code;
  }
  *result = napi_jsc_to_napi(names);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_set_instance_data(node_api_basic_env basic_env,
                                              void* data,
                                              napi_finalize finalize_cb,
                                              void* finalize_hint) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  env->instance_data = data;
  env->instance_data_finalize_cb = finalize_cb;
  env->instance_data_finalize_hint = finalize_hint;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_instance_data(node_api_basic_env basic_env, void** data) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, data);
  *data = env->instance_data;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_detach_arraybuffer(napi_env env, napi_value arraybuffer) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, arraybuffer);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  bool is_arraybuffer = false;
  napi_status status = napi_is_arraybuffer(env, arraybuffer, &is_arraybuffer);
  if (status != napi_ok) return status;
  if (!is_arraybuffer) {
    return napi_jsc_set_last_error(
        env, napi_arraybuffer_expected, "An ArrayBuffer was expected");
  }
  if (!DetachArrayBufferWithStructuredClone(env, ToJSObject(env, arraybuffer))) {
    return env->last_error.error_code != napi_ok
               ? env->last_error.error_code
               : napi_jsc_set_last_error(env, napi_generic_failure);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_is_detached_arraybuffer(napi_env env,
                                                    napi_value value,
                                                    bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  bool is_arraybuffer = false;
  napi_status status = napi_is_arraybuffer(env, value, &is_arraybuffer);
  if (status != napi_ok) return status;
  if (!is_arraybuffer) {
    return napi_jsc_set_last_error(
        env, napi_arraybuffer_expected, "An ArrayBuffer was expected");
  }
  *result = IsMarkedDetachedArrayBuffer(env, ToJSObject(env, value));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_type_tag_object(napi_env env,
                                            napi_value value,
                                            const napi_type_tag* type_tag) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, type_tag);
  if (!IsObject(env, value)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  for (auto& entry : env->type_tag_entries) {
    if (entry.value != nullptr && KeysEqual(env, entry.value, napi_jsc_to_js_value(value))) {
      entry.tag = *type_tag;
      return napi_jsc_clear_last_error(env);
    }
  }

  napi_env__::TypeTagEntry entry;
  entry.value = napi_jsc_to_js_value(value);
  entry.tag = *type_tag;
  env->Protect(entry.value);
  env->type_tag_entries.push_back(entry);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_check_object_type_tag(napi_env env,
                                                  napi_value value,
                                                  const napi_type_tag* type_tag,
                                                  bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, type_tag);
  NAPI_JSC_CHECK_ARG(env, result);
  if (!IsObject(env, value)) {
    *result = false;
    return napi_jsc_clear_last_error(env);
  }
  for (auto& entry : env->type_tag_entries) {
    if (entry.value != nullptr && KeysEqual(env, entry.value, napi_jsc_to_js_value(value))) {
      *result = entry.tag.lower == type_tag->lower && entry.tag.upper == type_tag->upper;
      return napi_jsc_clear_last_error(env);
    }
  }
  *result = false;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_object_freeze(napi_env env, napi_value object) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  if (!SetIntegrityLevel(env, ToJSObject(env, object), "freeze")) {
    return env->last_error.error_code != napi_ok
               ? env->last_error.error_code
               : napi_jsc_set_last_error(env, napi_generic_failure);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_object_seal(napi_env env, napi_value object) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, object);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (!IsObject(env, object)) {
    return napi_jsc_set_last_error(env, napi_object_expected, "An object was expected");
  }
  if (!SetIntegrityLevel(env, ToJSObject(env, object), "seal")) {
    return env->last_error.error_code != napi_ok
               ? env->last_error.error_code
               : napi_jsc_set_last_error(env, napi_generic_failure);
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_buffer(napi_env env,
                                          size_t length,
                                          void** data,
                                          napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, data);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  void* bytes = length == 0 ? nullptr : std::malloc(length);
  if (length > 0 && bytes == nullptr) {
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }

  auto* context = new (std::nothrow) BufferDeallocatorContext();
  if (context == nullptr) {
    std::free(bytes);
    return napi_jsc_set_last_error(env, napi_generic_failure);
  }
  context->env = env;
  context->free_data = true;
  if (!napi_jsc_create_uint8_array_from_bytes(
          env, bytes, length, BufferBytesDeallocator, context, result)) {
    return env->last_error.error_code;
  }
  *data = bytes;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_external_buffer(napi_env env,
                                                   size_t length,
                                                   void* data,
                                                   node_api_basic_finalize finalize_cb,
                                                   void* finalize_hint,
                                                   napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);
  if (data == nullptr && length != 0) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  auto* context = new (std::nothrow) BufferDeallocatorContext();
  if (context == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  context->env = env;
  context->data = data;
  context->finalize_cb = finalize_cb;
  context->finalize_hint = finalize_hint;
  if (!napi_jsc_create_uint8_array_from_bytes(
          env, data, length, BufferBytesDeallocator, context, result)) {
    return env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL node_api_create_buffer_from_arraybuffer(
    napi_env env,
    napi_value arraybuffer,
    size_t byte_offset,
    size_t byte_length,
    napi_value* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, arraybuffer);
  NAPI_JSC_CHECK_ARG(env, result);
  NAPI_JSC_CHECK_CAN_RUN_JS(env);

  bool is_arraybuffer = false;
  napi_status status = napi_is_arraybuffer(env, arraybuffer, &is_arraybuffer);
  if (status != napi_ok) return status;
  if (!is_arraybuffer) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }

  size_t total_length = 0;
  status = napi_get_arraybuffer_info(env, arraybuffer, nullptr, &total_length);
  if (status != napi_ok) return status;
  if (byte_offset > total_length || byte_length > total_length - byte_offset) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (!napi_jsc_create_uint8_array_for_buffer(
          env, ToJSObject(env, arraybuffer), byte_offset, byte_length, result)) {
    return env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_create_buffer_copy(napi_env env,
                                               size_t length,
                                               const void* data,
                                               void** result_data,
                                               napi_value* result) {
  void* out = nullptr;
  napi_status status = napi_create_buffer(env, length, &out, result);
  if (status != napi_ok) return status;
  if (length > 0 && data != nullptr) std::memcpy(out, data, length);
  if (result_data != nullptr) *result_data = out;
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_is_buffer(napi_env env, napi_value value, bool* result) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  NAPI_JSC_CHECK_ARG(env, result);
  *result = napi_jsc_value_is_buffer(env, value);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_get_buffer_info(napi_env env,
                                            napi_value value,
                                            void** data,
                                            size_t* length) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, value);
  if (!napi_jsc_value_is_buffer(env, value)) {
    return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  }
  if (!napi_jsc_get_typed_array_bytes(env, ToJSObject(env, value), data, length, nullptr, nullptr)) {
    return env->last_error.error_code;
  }
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_fatal_exception(napi_env env, napi_value err) {
  NAPI_JSC_CHECK_ENV(env);
  NAPI_JSC_CHECK_ARG(env, err);
  env->SetLastException(napi_jsc_to_js_value(err));
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_add_env_cleanup_hook(node_api_basic_env basic_env,
                                                 napi_cleanup_hook fun,
                                                 void* arg) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  if (fun == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  auto* entry = new (std::nothrow) napi_env_cleanup_hook__();
  if (entry == nullptr) return napi_jsc_set_last_error(env, napi_generic_failure);
  entry->hook = fun;
  entry->arg = arg;
  env->env_cleanup_hooks.push_back(entry);
  return napi_jsc_clear_last_error(env);
}

napi_status NAPI_CDECL napi_remove_env_cleanup_hook(node_api_basic_env basic_env,
                                                    napi_cleanup_hook fun,
                                                    void* arg) {
  napi_env env = const_cast<napi_env>(basic_env);
  NAPI_JSC_CHECK_ENV(env);
  if (fun == nullptr) return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
  auto& hooks = env->env_cleanup_hooks;
  for (auto it = hooks.begin(); it != hooks.end(); ++it) {
    auto* entry = static_cast<napi_env_cleanup_hook__*>(*it);
    if (entry != nullptr && entry->hook == fun && entry->arg == arg) {
      delete entry;
      hooks.erase(it);
      return napi_jsc_clear_last_error(env);
    }
  }
  return napi_jsc_set_last_error(env, napi_invalid_arg, "Invalid argument");
}

void NAPI_CDECL napi_module_register(napi_module* mod) {
  (void)mod;
}

void NAPI_CDECL napi_fatal_error(const char* location,
                                 size_t location_len,
                                 const char* message,
                                 size_t message_len) {
  const char* loc = location == nullptr ? "" : location;
  const char* msg = message == nullptr ? "" : message;
  size_t loc_len = location_len == NAPI_AUTO_LENGTH ? std::strlen(loc) : location_len;
  size_t msg_len = message_len == NAPI_AUTO_LENGTH ? std::strlen(msg) : message_len;
  std::fprintf(stderr,
               "FATAL ERROR: %.*s %.*s\n",
               static_cast<int>(loc_len),
               loc,
               static_cast<int>(msg_len),
               msg);
  std::fflush(stderr);
  std::abort();
}

}  // extern "C"
