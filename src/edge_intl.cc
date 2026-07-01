#include "edge_intl.h"

#include <cstdint>
#include <string>
#include <vector>

// ICU is built with U_DISABLE_RENAMING=1 (see cmake/EdgeICU.cmake); consumers
// must match to avoid referencing versioned symbols (e.g. ulistfmt_open_78).
#define U_DISABLE_RENAMING 1
#include <unicode/uloc.h>
#include <unicode/ulistformatter.h>
#include <unicode/unumberformatter.h>
#include <unicode/ustring.h>
#include <unicode/utypes.h>

namespace {

// ---------------------------------------------------------------------------
// Shared N-API + ICU harness (reused by every constructor below)
// ---------------------------------------------------------------------------

void SetError(std::string* error_out, const std::string& message) {
  if (error_out != nullptr) *error_out = message;
}

napi_value Undefined(napi_env env) {
  napi_value out = nullptr;
  napi_get_undefined(env, &out);
  return out;
}

bool IsFunction(napi_env env, napi_value value) {
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  return napi_typeof(env, value, &type) == napi_ok && type == napi_function;
}

// Throws a JS RangeError and returns nullptr, matching ECMA-402 which reports
// invalid options/locales as RangeError.
napi_value ThrowRange(napi_env env, const std::string& message) {
  // Must be napi_throw_range_error (throws a real RangeError); napi_throw_error's
  // first arg is an error *code*, which would yield a plain Error instead.
  napi_throw_range_error(env, nullptr, message.c_str());
  return nullptr;
}

napi_value ThrowType(napi_env env, const std::string& message) {
  napi_throw_type_error(env, nullptr, message.c_str());
  return nullptr;
}

// JS string/coercible -> UTF-8 std::string. Returns fallback when absent.
std::string ValueToString(napi_env env, napi_value value, const std::string& fallback = "") {
  if (value == nullptr) return fallback;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type == napi_undefined || type == napi_null) {
    return fallback;
  }
  napi_value str = value;
  if (type != napi_string && napi_coerce_to_string(env, value, &str) != napi_ok) {
    return fallback;
  }
  size_t len = 0;
  if (napi_get_value_string_utf8(env, str, nullptr, 0, &len) != napi_ok) return fallback;
  std::string out(len + 1, '\0');
  size_t copied = 0;
  if (napi_get_value_string_utf8(env, str, out.data(), out.size(), &copied) != napi_ok) {
    return fallback;
  }
  out.resize(copied);
  return out;
}

napi_value MakeString(napi_env env, const std::string& value) {
  napi_value out = nullptr;
  napi_create_string_utf8(env, value.c_str(), value.size(), &out);
  return out;
}

napi_value GetNamed(napi_env env, napi_value object, const char* name) {
  if (object == nullptr) return nullptr;
  bool has = false;
  if (napi_has_named_property(env, object, name, &has) != napi_ok || !has) return nullptr;
  napi_value out = nullptr;
  if (napi_get_named_property(env, object, name, &out) != napi_ok) return nullptr;
  return out;
}

// Reads an enumerated string option, returning `fallback` when unset. If a value
// is present but not in `allowed`, returns "" so the caller can raise RangeError.
std::string GetStringOption(napi_env env,
                            napi_value options,
                            const char* name,
                            const std::vector<std::string>& allowed,
                            const std::string& fallback) {
  napi_value value = GetNamed(env, options, name);
  if (value == nullptr) return fallback;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) return fallback;
  const std::string got = ValueToString(env, value, "");
  for (const std::string& candidate : allowed) {
    if (candidate == got) return got;
  }
  return "";  // present-but-invalid sentinel
}

// Reads a free-form (non-enumerated) string option, e.g. a currency code.
std::string GetRawStringOption(napi_env env, napi_value options, const char* name) {
  napi_value value = GetNamed(env, options, name);
  if (value == nullptr) return "";
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) return "";
  return ValueToString(env, value, "");
}

// Reads an integer option; returns false when absent/unreadable.
bool GetIntOption(napi_env env, napi_value options, const char* name, int32_t* out) {
  napi_value value = GetNamed(env, options, name);
  if (value == nullptr) return false;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) return false;
  napi_value num = value;
  if (type != napi_number && napi_coerce_to_number(env, value, &num) != napi_ok) return false;
  double d = 0;
  if (napi_get_value_double(env, num, &d) != napi_ok) return false;
  *out = static_cast<int32_t>(d);
  return true;
}

// Reads a boolean option, defaulting when absent. ECMA-402 also allows string
// values for useGrouping; a present non-false value is treated as true.
bool GetBoolOptionDefault(napi_env env, napi_value options, const char* name, bool fallback) {
  napi_value value = GetNamed(env, options, name);
  if (value == nullptr) return fallback;
  napi_valuetype type = napi_undefined;
  if (napi_typeof(env, value, &type) != napi_ok || type == napi_undefined) return fallback;
  if (type == napi_boolean) {
    bool out = fallback;
    return napi_get_value_bool(env, value, &out) == napi_ok ? out : fallback;
  }
  bool coerced = fallback;
  napi_value b = nullptr;
  if (napi_coerce_to_bool(env, value, &b) == napi_ok && b != nullptr &&
      napi_get_value_bool(env, b, &coerced) == napi_ok) {
    return coerced;
  }
  return fallback;
}

bool DefineMethod(napi_env env, napi_value object, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  return napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok &&
         napi_set_named_property(env, object, name, fn) == napi_ok;
}

bool InstallToStringTag(napi_env env, napi_value prototype, const char* tag) {
  napi_value global = nullptr;
  napi_value symbol = nullptr;
  napi_value key = nullptr;
  napi_value tag_value = nullptr;
  if (napi_get_global(env, &global) != napi_ok ||
      napi_get_named_property(env, global, "Symbol", &symbol) != napi_ok ||
      napi_get_named_property(env, symbol, "toStringTag", &key) != napi_ok ||
      napi_create_string_utf8(env, tag, NAPI_AUTO_LENGTH, &tag_value) != napi_ok) {
    return false;
  }
  napi_property_descriptor desc = {};
  desc.name = key;
  desc.value = tag_value;
  desc.attributes = napi_configurable;
  return napi_define_properties(env, prototype, 1, &desc) == napi_ok;
}

// --- ICU string + locale conversions ---------------------------------------

std::u16string ToUChars(const std::string& utf8) {
  if (utf8.empty()) return std::u16string();
  int32_t cap = static_cast<int32_t>(utf8.size()) + 1;
  std::u16string out(static_cast<size_t>(cap), u'\0');
  int32_t len = 0;
  UErrorCode status = U_ZERO_ERROR;
  u_strFromUTF8(reinterpret_cast<UChar*>(out.data()), cap, &len,
                utf8.data(), static_cast<int32_t>(utf8.size()), &status);
  if (U_FAILURE(status)) return std::u16string();
  out.resize(static_cast<size_t>(len));
  return out;
}

std::string FromUChars(const UChar* buf, int32_t len) {
  if (buf == nullptr || len <= 0) return "";
  int32_t cap = len * 3 + 1;  // worst-case UTF-8 expansion for BMP text
  std::string out(static_cast<size_t>(cap), '\0');
  int32_t out_len = 0;
  UErrorCode status = U_ZERO_ERROR;
  u_strToUTF8(out.data(), cap, &out_len, buf, len, &status);
  if (U_FAILURE(status)) return "";
  out.resize(static_cast<size_t>(out_len));
  return out;
}

// Resolves the first requested locale (BCP-47) to an ICU locale id. Defaults to
// en-US. Never throws; unparseable tags fall back to the default.
std::string ResolveIcuLocale(napi_env env, napi_value locales) {
  std::string tag = "en-US";
  if (locales != nullptr) {
    bool is_array = false;
    if (napi_is_array(env, locales, &is_array) == napi_ok && is_array) {
      uint32_t len = 0;
      napi_value first = nullptr;
      if (napi_get_array_length(env, locales, &len) == napi_ok && len > 0 &&
          napi_get_element(env, locales, 0, &first) == napi_ok) {
        tag = ValueToString(env, first, "en-US");
      }
    } else {
      napi_valuetype type = napi_undefined;
      if (napi_typeof(env, locales, &type) == napi_ok &&
          type != napi_undefined && type != napi_null) {
        tag = ValueToString(env, locales, "en-US");
      }
    }
  }
  char buf[ULOC_FULLNAME_CAPACITY];
  UErrorCode status = U_ZERO_ERROR;
  int32_t len = uloc_forLanguageTag(tag.c_str(), buf, sizeof(buf), nullptr, &status);
  if (U_FAILURE(status) || len <= 0) return "en_US";
  return std::string(buf, static_cast<size_t>(len));
}

// ICU locale id -> canonical BCP-47 tag (for resolvedOptions().locale).
std::string IcuLocaleToBcp47(const std::string& icu_locale) {
  char buf[ULOC_FULLNAME_CAPACITY];
  UErrorCode status = U_ZERO_ERROR;
  int32_t len = uloc_toLanguageTag(icu_locale.c_str(), buf, sizeof(buf), true, &status);
  if (U_FAILURE(status) || len <= 0) return "en-US";
  return std::string(buf, static_cast<size_t>(len));
}

// Generic Intl static supportedLocalesOf: returns the requested locales that
// parse as valid language tags (canonicalized). Never throws on valid input.
napi_value SupportedLocalesOf(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  if (napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr) != napi_ok) return Undefined(env);

  std::vector<std::string> requested;
  if (argc > 0 && argv[0] != nullptr) {
    bool is_array = false;
    if (napi_is_array(env, argv[0], &is_array) == napi_ok && is_array) {
      uint32_t len = 0;
      napi_get_array_length(env, argv[0], &len);
      for (uint32_t i = 0; i < len; i++) {
        napi_value el = nullptr;
        if (napi_get_element(env, argv[0], i, &el) == napi_ok) {
          requested.push_back(ValueToString(env, el, ""));
        }
      }
    } else {
      napi_valuetype type = napi_undefined;
      if (napi_typeof(env, argv[0], &type) == napi_ok &&
          type != napi_undefined && type != napi_null) {
        requested.push_back(ValueToString(env, argv[0], ""));
      }
    }
  }

  napi_value out = nullptr;
  napi_create_array(env, &out);
  uint32_t idx = 0;
  for (const std::string& tag : requested) {
    if (tag.empty()) continue;
    char buf[ULOC_FULLNAME_CAPACITY];
    UErrorCode status = U_ZERO_ERROR;
    int32_t len = uloc_forLanguageTag(tag.c_str(), buf, sizeof(buf), nullptr, &status);
    if (U_FAILURE(status) || len <= 0) continue;
    napi_set_element(env, out, idx++, MakeString(env, IcuLocaleToBcp47(std::string(buf, len))));
  }
  return out;
}

// Reads a JS array of strings into a UTF-8 vector. Returns false if `value` is
// not an array (ECMA-402 accepts any iterable; arrays cover the real call sites).
bool ReadStringArray(napi_env env, napi_value value, std::vector<std::string>* out) {
  bool is_array = false;
  if (value == nullptr || napi_is_array(env, value, &is_array) != napi_ok || !is_array) {
    return false;
  }
  uint32_t len = 0;
  if (napi_get_array_length(env, value, &len) != napi_ok) return false;
  out->reserve(len);
  for (uint32_t i = 0; i < len; i++) {
    napi_value el = nullptr;
    if (napi_get_element(env, value, i, &el) != napi_ok) return false;
    out->push_back(ValueToString(env, el, ""));
  }
  return true;
}

// Installs a constructor built from a callback + prototype method table, wires
// the static supportedLocalesOf, the Symbol.toStringTag, and attaches it to Intl.
bool InstallConstructor(napi_env env,
                        napi_value intl,
                        const char* name,
                        napi_callback constructor,
                        const napi_property_descriptor* methods,
                        size_t method_count,
                        const char* tag,
                        std::string* error_out) {
  napi_value ctor = nullptr;
  napi_status status = napi_define_class(env, name, NAPI_AUTO_LENGTH, constructor, nullptr,
                                         method_count, methods, &ctor);
  if (status != napi_ok || ctor == nullptr) {
    SetError(error_out, std::string("Failed to define Intl.") + name);
    return false;
  }
  DefineMethod(env, ctor, "supportedLocalesOf", SupportedLocalesOf);
  napi_value prototype = nullptr;
  if (napi_get_named_property(env, ctor, "prototype", &prototype) == napi_ok && prototype != nullptr) {
    InstallToStringTag(env, prototype, tag);
  }
  if (napi_set_named_property(env, intl, name, ctor) != napi_ok) {
    SetError(error_out, std::string("Failed to install Intl.") + name);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Intl.ListFormat  (ulistfmt_*)
// ---------------------------------------------------------------------------

struct ListFormatState {
  std::string locale;  // resolved ICU locale id
  std::string type;    // "conjunction" | "disjunction" | "unit"
  std::string style;   // "long" | "short" | "narrow"
  UListFormatter* fmt = nullptr;
};

void ListFormatFinalize(napi_env /*env*/, void* data, void* /*hint*/) {
  auto* state = static_cast<ListFormatState*>(data);
  if (state != nullptr) {
    if (state->fmt != nullptr) ulistfmt_close(state->fmt);
    delete state;
  }
}

ListFormatState* UnwrapListFormat(napi_env env, napi_value this_arg) {
  void* data = nullptr;
  if (this_arg == nullptr || napi_unwrap(env, this_arg, &data) != napi_ok) return nullptr;
  return static_cast<ListFormatState*>(data);
}

UListFormatterType ListTypeFromString(const std::string& type) {
  if (type == "disjunction") return ULISTFMT_TYPE_OR;
  if (type == "unit") return ULISTFMT_TYPE_UNITS;
  return ULISTFMT_TYPE_AND;  // "conjunction" (default)
}

UListFormatterWidth ListWidthFromString(const std::string& style) {
  if (style == "short") return ULISTFMT_WIDTH_SHORT;
  if (style == "narrow") return ULISTFMT_WIDTH_NARROW;
  return ULISTFMT_WIDTH_WIDE;  // "long" (default)
}

napi_value ListFormatConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok) return nullptr;

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok) return nullptr;
  if (new_target == nullptr) {
    return ThrowType(env, "Constructor Intl.ListFormat requires 'new'");
  }

  auto* state = new ListFormatState();
  state->locale = argc > 0 ? ResolveIcuLocale(env, argv[0]) : "en_US";
  napi_value options = argc > 1 ? argv[1] : nullptr;
  state->type = GetStringOption(env, options, "type", {"conjunction", "disjunction", "unit"}, "conjunction");
  state->style = GetStringOption(env, options, "style", {"long", "short", "narrow"}, "long");
  if (state->type.empty()) {
    delete state;
    return ThrowRange(env, "Value for Intl.ListFormat option 'type' is out of range");
  }
  if (state->style.empty()) {
    delete state;
    return ThrowRange(env, "Value for Intl.ListFormat option 'style' is out of range");
  }

  UErrorCode status = U_ZERO_ERROR;
  state->fmt = ulistfmt_openForType(state->locale.c_str(),
                                    ListTypeFromString(state->type),
                                    ListWidthFromString(state->style), &status);
  if (U_FAILURE(status) || state->fmt == nullptr) {
    delete state;
    return ThrowRange(env, std::string("Failed to create Intl.ListFormat: ") + u_errorName(status));
  }

  if (napi_wrap(env, this_arg, state, ListFormatFinalize, nullptr, nullptr) != napi_ok) {
    ListFormatFinalize(env, state, nullptr);
    return nullptr;
  }
  return this_arg;
}

napi_value ListFormatFormat(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok) return nullptr;

  ListFormatState* state = UnwrapListFormat(env, this_arg);
  if (state == nullptr || state->fmt == nullptr) {
    return ThrowType(env, "Intl.ListFormat.prototype.format called on incompatible receiver");
  }

  std::vector<std::string> items;
  if (argc > 0 && argv[0] != nullptr) {
    napi_valuetype type = napi_undefined;
    napi_typeof(env, argv[0], &type);
    if (type != napi_undefined && !ReadStringArray(env, argv[0], &items)) {
      return ThrowType(env, "Intl.ListFormat.prototype.format expects an array of strings");
    }
  }

  if (items.empty()) return MakeString(env, "");

  // Build the parallel UChar* / length arrays ICU expects. Backing storage must
  // outlive the ulistfmt_format call, so keep the u16 strings in a vector.
  std::vector<std::u16string> backing;
  backing.reserve(items.size());
  std::vector<const UChar*> ptrs;
  std::vector<int32_t> lengths;
  ptrs.reserve(items.size());
  lengths.reserve(items.size());
  for (const std::string& item : items) {
    backing.push_back(ToUChars(item));
    ptrs.push_back(reinterpret_cast<const UChar*>(backing.back().data()));
    lengths.push_back(static_cast<int32_t>(backing.back().size()));
  }

  UErrorCode status = U_ZERO_ERROR;
  int32_t needed = ulistfmt_format(state->fmt, ptrs.data(), lengths.data(),
                                   static_cast<int32_t>(items.size()), nullptr, 0, &status);
  if (status != U_BUFFER_OVERFLOW_ERROR && U_FAILURE(status)) {
    return ThrowRange(env, std::string("Intl.ListFormat.format failed: ") + u_errorName(status));
  }
  std::u16string result(static_cast<size_t>(needed), u'\0');
  status = U_ZERO_ERROR;
  ulistfmt_format(state->fmt, ptrs.data(), lengths.data(), static_cast<int32_t>(items.size()),
                  reinterpret_cast<UChar*>(result.data()), needed + 1, &status);
  if (U_FAILURE(status)) {
    return ThrowRange(env, std::string("Intl.ListFormat.format failed: ") + u_errorName(status));
  }
  return MakeString(env, FromUChars(reinterpret_cast<const UChar*>(result.data()),
                                    static_cast<int32_t>(result.size())));
}

napi_value ListFormatResolvedOptions(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok) return nullptr;
  ListFormatState* state = UnwrapListFormat(env, this_arg);
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok) return Undefined(env);
  if (state != nullptr) {
    napi_set_named_property(env, out, "locale", MakeString(env, IcuLocaleToBcp47(state->locale)));
    napi_set_named_property(env, out, "type", MakeString(env, state->type));
    napi_set_named_property(env, out, "style", MakeString(env, state->style));
  }
  return out;
}

bool InstallListFormat(napi_env env, napi_value intl, std::string* error_out) {
  napi_property_descriptor methods[] = {
      {"format", nullptr, ListFormatFormat, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"resolvedOptions", nullptr, ListFormatResolvedOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  return InstallConstructor(env, intl, "ListFormat", ListFormatConstructor, methods,
                            sizeof(methods) / sizeof(methods[0]), "Intl.ListFormat", error_out);
}

// ---------------------------------------------------------------------------
// Intl.NumberFormat  (unumf_* modern number formatter)
// ---------------------------------------------------------------------------

struct NumberFormatState {
  std::string locale;    // resolved ICU locale id
  std::string style;     // "decimal" | "currency" | "percent" | "unit"
  std::string currency;  // ISO 4217, when style == currency
  std::string unit;      // when style == unit
  bool use_grouping = true;
  int32_t min_fraction = -1;  // -1 == unset (ICU default)
  int32_t max_fraction = -1;
  UNumberFormatter* fmt = nullptr;
};

void NumberFormatFinalize(napi_env /*env*/, void* data, void* /*hint*/) {
  auto* state = static_cast<NumberFormatState*>(data);
  if (state != nullptr) {
    if (state->fmt != nullptr) unumf_close(state->fmt);
    delete state;
  }
}

NumberFormatState* UnwrapNumberFormat(napi_env env, napi_value this_arg) {
  void* data = nullptr;
  if (this_arg == nullptr || napi_unwrap(env, this_arg, &data) != napi_ok) return nullptr;
  return static_cast<NumberFormatState*>(data);
}

// Builds an ICU concise number skeleton (unumberformatter.h) from resolved
// options. See https://unicode-org.github.io/icu/userguide/format_parse/numbers/skeletons
std::string BuildNumberSkeleton(const NumberFormatState& s) {
  std::string skel;
  auto add = [&](const std::string& token) {
    if (!skel.empty()) skel += " ";
    skel += token;
  };
  if (s.style == "currency") {
    add("currency/" + s.currency);
  } else if (s.style == "percent") {
    add("percent");
    add("scale/100");  // ECMA-402 percent multiplies the input by 100
  } else if (s.style == "unit" && !s.unit.empty()) {
    add("unit/" + s.unit);
  }
  if (s.min_fraction >= 0 || s.max_fraction >= 0) {
    int32_t lo = s.min_fraction < 0 ? 0 : s.min_fraction;
    int32_t hi = s.max_fraction < 0 ? (lo > 3 ? lo : 3) : s.max_fraction;
    if (hi < lo) hi = lo;
    std::string frac = ".";
    frac.append(static_cast<size_t>(lo), '0');
    frac.append(static_cast<size_t>(hi - lo), '#');
    add(frac);
  }
  if (!s.use_grouping) add("group-off");
  return skel;
}

napi_value NumberFormatConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr, nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok) return nullptr;

  napi_value new_target = nullptr;
  if (napi_get_new_target(env, info, &new_target) != napi_ok) return nullptr;
  if (new_target == nullptr) return ThrowType(env, "Constructor Intl.NumberFormat requires 'new'");

  auto* state = new NumberFormatState();
  state->locale = argc > 0 ? ResolveIcuLocale(env, argv[0]) : "en_US";
  napi_value options = argc > 1 ? argv[1] : nullptr;

  state->style = GetStringOption(env, options, "style", {"decimal", "currency", "percent", "unit"}, "decimal");
  if (state->style.empty()) {
    delete state;
    return ThrowRange(env, "Value for Intl.NumberFormat option 'style' is out of range");
  }
  if (state->style == "currency") {
    state->currency = GetRawStringOption(env, options, "currency");
    if (state->currency.empty()) {
      delete state;
      return ThrowType(env, "Currency code is required with currency style");
    }
  } else if (state->style == "unit") {
    state->unit = GetRawStringOption(env, options, "unit");
    if (state->unit.empty()) {
      delete state;
      return ThrowType(env, "Unit is required with unit style");
    }
  }
  state->use_grouping = GetBoolOptionDefault(env, options, "useGrouping", true);
  GetIntOption(env, options, "minimumFractionDigits", &state->min_fraction);
  GetIntOption(env, options, "maximumFractionDigits", &state->max_fraction);

  const std::string skeleton = BuildNumberSkeleton(*state);
  const std::u16string uskel = ToUChars(skeleton);
  UErrorCode status = U_ZERO_ERROR;
  state->fmt = unumf_openForSkeletonAndLocale(
      reinterpret_cast<const UChar*>(uskel.data()), static_cast<int32_t>(uskel.size()),
      state->locale.c_str(), &status);
  if (U_FAILURE(status) || state->fmt == nullptr) {
    delete state;
    return ThrowRange(env, std::string("Failed to create Intl.NumberFormat: ") + u_errorName(status));
  }

  if (napi_wrap(env, this_arg, state, NumberFormatFinalize, nullptr, nullptr) != napi_ok) {
    NumberFormatFinalize(env, state, nullptr);
    return nullptr;
  }
  return this_arg;
}

napi_value NumberFormatFormat(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1] = {nullptr};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok) return nullptr;

  NumberFormatState* state = UnwrapNumberFormat(env, this_arg);
  if (state == nullptr || state->fmt == nullptr) {
    return ThrowType(env, "Intl.NumberFormat.prototype.format called on incompatible receiver");
  }

  double value = 0;
  if (argc > 0 && argv[0] != nullptr) {
    napi_value num = argv[0];
    napi_valuetype type = napi_undefined;
    napi_typeof(env, argv[0], &type);
    if (type != napi_number && napi_coerce_to_number(env, argv[0], &num) != napi_ok) {
      return MakeString(env, "NaN");
    }
    if (napi_get_value_double(env, num, &value) != napi_ok) return MakeString(env, "NaN");
  }

  UErrorCode status = U_ZERO_ERROR;
  UFormattedNumber* result = unumf_openResult(&status);
  if (U_FAILURE(status)) return ThrowRange(env, std::string("NumberFormat: ") + u_errorName(status));
  unumf_formatDouble(state->fmt, value, result, &status);
  if (U_FAILURE(status)) {
    unumf_closeResult(result);
    return ThrowRange(env, std::string("NumberFormat.format failed: ") + u_errorName(status));
  }

  int32_t needed = unumf_resultToString(result, nullptr, 0, &status);
  status = U_ZERO_ERROR;
  std::u16string out(static_cast<size_t>(needed), u'\0');
  unumf_resultToString(result, reinterpret_cast<UChar*>(out.data()), needed + 1, &status);
  unumf_closeResult(result);
  if (U_FAILURE(status)) {
    return ThrowRange(env, std::string("NumberFormat.format failed: ") + u_errorName(status));
  }
  return MakeString(env, FromUChars(reinterpret_cast<const UChar*>(out.data()),
                                    static_cast<int32_t>(out.size())));
}

napi_value NumberFormatResolvedOptions(napi_env env, napi_callback_info info) {
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, nullptr, nullptr, &this_arg, nullptr) != napi_ok) return nullptr;
  NumberFormatState* state = UnwrapNumberFormat(env, this_arg);
  napi_value out = nullptr;
  if (napi_create_object(env, &out) != napi_ok) return Undefined(env);
  if (state != nullptr) {
    napi_set_named_property(env, out, "locale", MakeString(env, IcuLocaleToBcp47(state->locale)));
    napi_set_named_property(env, out, "numberingSystem", MakeString(env, "latn"));
    napi_set_named_property(env, out, "style", MakeString(env, state->style));
    if (!state->currency.empty()) {
      napi_set_named_property(env, out, "currency", MakeString(env, state->currency));
    }
    if (!state->unit.empty()) {
      napi_set_named_property(env, out, "unit", MakeString(env, state->unit));
    }
    napi_value grouping = nullptr;
    napi_get_boolean(env, state->use_grouping, &grouping);
    napi_set_named_property(env, out, "useGrouping", grouping);
  }
  return out;
}

bool InstallNumberFormat(napi_env env, napi_value intl, std::string* error_out) {
  napi_property_descriptor methods[] = {
      {"format", nullptr, NumberFormatFormat, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"resolvedOptions", nullptr, NumberFormatResolvedOptions, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  return InstallConstructor(env, intl, "NumberFormat", NumberFormatConstructor, methods,
                            sizeof(methods) / sizeof(methods[0]), "Intl.NumberFormat", error_out);
}

// ---------------------------------------------------------------------------
// Install entry point
// ---------------------------------------------------------------------------

napi_value GetOrCreateIntl(napi_env env, napi_value global) {
  napi_value intl = nullptr;
  bool has_intl = false;
  if (napi_has_named_property(env, global, "Intl", &has_intl) == napi_ok && has_intl) {
    napi_get_named_property(env, global, "Intl", &intl);
  }
  napi_valuetype type = napi_undefined;
  if (intl != nullptr && napi_typeof(env, intl, &type) == napi_ok &&
      (type == napi_object || type == napi_function)) {
    return intl;
  }
  if (napi_create_object(env, &intl) != napi_ok) return nullptr;
  if (napi_set_named_property(env, global, "Intl", intl) != napi_ok) return nullptr;
  return intl;
}

}  // namespace

bool EdgeInstallIntl(napi_env env, std::string* error_out) {
  if (env == nullptr) return false;
  napi_value global = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) {
    SetError(error_out, "Failed to fetch global object for Intl");
    return false;
  }
  napi_value intl = GetOrCreateIntl(env, global);
  if (intl == nullptr) {
    SetError(error_out, "Failed to create Intl object");
    return false;
  }

  if (!InstallListFormat(env, intl, error_out)) return false;
  if (!InstallNumberFormat(env, intl, error_out)) return false;

  return true;
}
