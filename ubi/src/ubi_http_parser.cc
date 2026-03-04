#include "ubi_http_parser.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <uv.h>

#include "ubi_runtime.h"

extern "C" {
#include "llhttp.h"
}

namespace {

constexpr uint32_t kOnMessageBegin = 0;
constexpr uint32_t kOnHeaders = 1;
constexpr uint32_t kOnHeadersComplete = 2;
constexpr uint32_t kOnBody = 3;
constexpr uint32_t kOnMessageComplete = 4;
constexpr uint32_t kOnExecute = 5;
constexpr uint32_t kOnTimeout = 6;
constexpr size_t kMaxHeaderFieldsCount = 32;

constexpr uint32_t kLenientNone = 0;
constexpr uint32_t kLenientHeaders = 1 << 0;
constexpr uint32_t kLenientChunkedLength = 1 << 1;
constexpr uint32_t kLenientKeepAlive = 1 << 2;
constexpr uint32_t kLenientTransferEncoding = 1 << 3;
constexpr uint32_t kLenientVersion = 1 << 4;
constexpr uint32_t kLenientDataAfterClose = 1 << 5;
constexpr uint32_t kLenientOptionalLFAfterCR = 1 << 6;
constexpr uint32_t kLenientOptionalCRLFAfterChunk = 1 << 7;
constexpr uint32_t kLenientOptionalCRBeforeLF = 1 << 8;
constexpr uint32_t kLenientSpacesAfterChunkSize = 1 << 9;
constexpr uint32_t kLenientAll = kLenientHeaders | kLenientChunkedLength | kLenientKeepAlive |
    kLenientTransferEncoding | kLenientVersion | kLenientDataAfterClose |
    kLenientOptionalLFAfterCR | kLenientOptionalCRLFAfterChunk |
    kLenientOptionalCRBeforeLF | kLenientSpacesAfterChunkSize;

struct Parser;

struct ConnectionsList {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  std::set<Parser*> all;
  std::set<Parser*> active;
};

struct Parser {
  napi_env env = nullptr;
  napi_ref wrapper_ref = nullptr;
  llhttp_t parser{};
  llhttp_settings_t settings{};
  std::vector<std::string> fields;
  std::vector<std::string> values;
  std::string url;
  std::string status_message;
  bool last_was_value = false;
  bool have_flushed = false;
  bool got_exception = false;
  bool pending_pause = false;
  bool headers_completed = false;
  uint64_t header_nread = 0;
  uint64_t chunk_extensions_nread = 0;
  uint64_t max_http_header_size = 8 * 1024;
  uint64_t last_message_start = 0;
  const char* current_buffer_data = nullptr;
  size_t current_buffer_len = 0;
  ConnectionsList* list = nullptr;
};

template <typename T>
T* Unwrap(napi_env env, napi_callback_info info, napi_value* this_arg = nullptr) {
  size_t argc = 0;
  napi_value self = nullptr;
  void* data = nullptr;
  if (napi_get_cb_info(env, info, &argc, nullptr, &self, &data) != napi_ok || self == nullptr) return nullptr;
  T* out = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&out)) != napi_ok) return nullptr;
  if (this_arg != nullptr) *this_arg = self;
  return out;
}

napi_value CreateUint8ArrayCopy(napi_env env, const char* data, size_t length);
int FlushHeadersToJs(Parser* p);

napi_value MakeError(napi_env env,
                     const char* message,
                     const char* code,
                     const char* reason,
                     uint32_t bytes_parsed,
                     const char* raw_packet,
                     size_t raw_packet_len) {
  napi_value msg = nullptr;
  napi_value code_v = nullptr;
  napi_value err = nullptr;
  napi_create_string_utf8(env, message, NAPI_AUTO_LENGTH, &msg);
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_v);
  napi_create_error(env, code_v, msg, &err);
  if (err != nullptr) {
    napi_value reason_v = nullptr;
    napi_create_string_utf8(env, reason ? reason : "", NAPI_AUTO_LENGTH, &reason_v);
    if (reason_v != nullptr) napi_set_named_property(env, err, "reason", reason_v);
    napi_set_named_property(env, err, "code", code_v);
    napi_value bytes_v = nullptr;
    napi_create_uint32(env, bytes_parsed, &bytes_v);
    if (bytes_v != nullptr) napi_set_named_property(env, err, "bytesParsed", bytes_v);
    if (raw_packet != nullptr && raw_packet_len > 0) {
      napi_value packet = CreateUint8ArrayCopy(env, raw_packet, raw_packet_len);
      if (packet != nullptr) napi_set_named_property(env, err, "rawPacket", packet);
    }
  }
  return err;
}

napi_value GetWrappedObject(napi_env env, napi_ref ref) {
  if (ref == nullptr) return nullptr;
  napi_value obj = nullptr;
  if (napi_get_reference_value(env, ref, &obj) != napi_ok) return nullptr;
  return obj;
}

napi_value CreateUint8ArrayCopy(napi_env env, const char* data, size_t length) {
  napi_value ab = nullptr;
  void* out = nullptr;
  if (napi_create_arraybuffer(env, length, &out, &ab) != napi_ok || ab == nullptr) return nullptr;
  if (out != nullptr && data != nullptr && length > 0) {
    std::memcpy(out, data, length);
  }
  napi_value view = nullptr;
  if (napi_create_typedarray(env, napi_uint8_array, length, ab, 0, &view) != napi_ok) return nullptr;
  return view;
}

napi_value CreateBufferCopy(napi_env env, const char* data, size_t length) {
  napi_value view = CreateUint8ArrayCopy(env, data, length);
  if (view == nullptr) return nullptr;
  napi_value global = nullptr;
  napi_value buffer_ctor = nullptr;
  napi_value from_fn = nullptr;
  napi_value out = nullptr;
  if (napi_get_global(env, &global) != napi_ok || global == nullptr) return view;
  if (napi_get_named_property(env, global, "Buffer", &buffer_ctor) != napi_ok || buffer_ctor == nullptr) return view;
  if (napi_get_named_property(env, buffer_ctor, "from", &from_fn) != napi_ok || from_fn == nullptr) return view;
  napi_value argv[1] = {view};
  if (napi_call_function(env, buffer_ctor, from_fn, 1, argv, &out) != napi_ok || out == nullptr) return view;
  return out;
}

int CallIndexedNoArgs(Parser* p, uint32_t index, bool skip_task_queues = false) {
  napi_value self = GetWrappedObject(p->env, p->wrapper_ref);
  if (self == nullptr) return 0;
  napi_value cb = nullptr;
  if (napi_get_element(p->env, self, index, &cb) != napi_ok || cb == nullptr) return 0;
  napi_valuetype t = napi_undefined;
  napi_typeof(p->env, cb, &t);
  if (t != napi_function) return 0;
  napi_value result = nullptr;
  bool has_pending = false;
  const int callback_flags =
      skip_task_queues ? kUbiMakeCallbackSkipTaskQueues : kUbiMakeCallbackNone;
  if (UbiMakeCallbackWithFlags(p->env, self, cb, 0, nullptr, &result, callback_flags) != napi_ok ||
      (napi_is_exception_pending(p->env, &has_pending) == napi_ok && has_pending)) {
    p->got_exception = true;
    llhttp_set_error_reason(&p->parser, "HPE_JS_EXCEPTION:JS Exception");
    return HPE_USER;
  }
  return 0;
}

int ParserOnMessageBegin(llhttp_t* llp) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (p->list != nullptr) {
    p->list->all.erase(p);
    p->list->active.erase(p);
  }
  p->fields.clear();
  p->values.clear();
  p->url.clear();
  p->status_message.clear();
  p->last_was_value = false;
  p->headers_completed = false;
  p->chunk_extensions_nread = 0;
  p->have_flushed = false;
  p->last_message_start = uv_hrtime();
  if (p->list != nullptr) {
    p->list->all.insert(p);
    p->list->active.insert(p);
  }
  return CallIndexedNoArgs(p, kOnMessageBegin, true);
}

int TrackHeader(Parser* p, size_t len) {
  p->header_nread += len;
  if (p->header_nread >= p->max_http_header_size) {
    llhttp_set_error_reason(&p->parser, "HPE_HEADER_OVERFLOW:Header overflow");
    return HPE_USER;
  }
  return 0;
}

int ParserOnUrl(llhttp_t* llp, const char* at, size_t length) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (int rv = TrackHeader(p, length); rv != 0) return rv;
  p->url.append(at, length);
  return 0;
}

int ParserOnStatus(llhttp_t* llp, const char* at, size_t length) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (int rv = TrackHeader(p, length); rv != 0) return rv;
  p->status_message.append(at, length);
  return 0;
}

int ParserOnHeaderField(llhttp_t* llp, const char* at, size_t length) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (int rv = TrackHeader(p, length); rv != 0) return rv;
  if (p->fields.empty() || p->last_was_value) {
    if (p->fields.size() >= kMaxHeaderFieldsCount) {
      const int flush_rv = FlushHeadersToJs(p);
      if (flush_rv != 0) return flush_rv;
    }
    p->fields.emplace_back();
    p->last_was_value = false;
  }
  p->fields.back().append(at, length);
  return 0;
}

int ParserOnHeaderValue(llhttp_t* llp, const char* at, size_t length) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (int rv = TrackHeader(p, length); rv != 0) return rv;
  if (p->values.size() < p->fields.size()) p->values.emplace_back();
  p->values.back().append(at, length);
  p->last_was_value = true;
  return 0;
}

napi_value BuildHeadersArray(Parser* p) {
  napi_value arr = nullptr;
  napi_create_array_with_length(p->env, p->values.size() * 2, &arr);
  if (arr == nullptr) return nullptr;
  for (size_t i = 0; i < p->values.size(); i++) {
    napi_value k = nullptr;
    napi_value v = nullptr;
    const std::string& key = i < p->fields.size() ? p->fields[i] : std::string();
    napi_create_string_utf8(p->env, key.c_str(), key.size(), &k);
    napi_create_string_utf8(p->env, p->values[i].c_str(), p->values[i].size(), &v);
    if (k != nullptr) napi_set_element(p->env, arr, static_cast<uint32_t>(i * 2), k);
    if (v != nullptr) napi_set_element(p->env, arr, static_cast<uint32_t>(i * 2 + 1), v);
  }
  return arr;
}

int FlushHeadersToJs(Parser* p) {
  napi_value self = GetWrappedObject(p->env, p->wrapper_ref);
  if (self == nullptr) return 0;
  napi_value cb = nullptr;
  if (napi_get_element(p->env, self, kOnHeaders, &cb) != napi_ok || cb == nullptr) return 0;
  napi_valuetype t = napi_undefined;
  napi_typeof(p->env, cb, &t);
  if (t != napi_function) return 0;

  napi_value headers = BuildHeadersArray(p);
  napi_value url_v = nullptr;
  napi_create_string_utf8(p->env, p->url.c_str(), p->url.size(), &url_v);
  napi_value argv[2] = {headers, url_v};
  napi_value ignored = nullptr;
  bool has_pending = false;
  if (UbiMakeCallback(p->env, self, cb, 2, argv, &ignored) != napi_ok ||
      (napi_is_exception_pending(p->env, &has_pending) == napi_ok && has_pending)) {
    p->got_exception = true;
    llhttp_set_error_reason(&p->parser, "HPE_JS_EXCEPTION:JS Exception");
    return HPE_USER;
  }

  p->have_flushed = true;
  p->fields.clear();
  p->values.clear();
  p->url.clear();
  return 0;
}

int ParserOnHeadersComplete(llhttp_t* llp) {
  Parser* p = static_cast<Parser*>(llp->data);
  p->headers_completed = true;
  p->header_nread = 0;

  napi_value self = GetWrappedObject(p->env, p->wrapper_ref);
  if (self == nullptr) return 0;
  napi_value cb = nullptr;
  if (napi_get_element(p->env, self, kOnHeadersComplete, &cb) != napi_ok || cb == nullptr) return 0;
  napi_valuetype t = napi_undefined;
  napi_typeof(p->env, cb, &t);
  if (t != napi_function) return 0;

  napi_value argv[9] = {nullptr};
  napi_get_undefined(p->env, &argv[0]);
  for (int i = 1; i < 9; i++) argv[i] = argv[0];

  napi_create_uint32(p->env, llhttp_get_http_major(llp), &argv[0]);
  napi_create_uint32(p->env, llhttp_get_http_minor(llp), &argv[1]);
  if (p->have_flushed) {
    if (FlushHeadersToJs(p) != 0) return HPE_USER;
  } else {
    argv[2] = BuildHeadersArray(p);
    if (llhttp_get_type(llp) == HTTP_REQUEST) {
      napi_create_string_utf8(p->env, p->url.c_str(), p->url.size(), &argv[4]);
    }
  }
  if (llhttp_get_type(llp) == HTTP_REQUEST) {
    napi_create_uint32(p->env, llhttp_get_method(llp), &argv[3]);
  } else {
    napi_create_int32(p->env, llhttp_get_status_code(llp), &argv[5]);
    napi_create_string_utf8(p->env, p->status_message.c_str(), p->status_message.size(), &argv[6]);
  }
  p->fields.clear();
  p->values.clear();
  p->url.clear();
  p->status_message.clear();
  napi_get_boolean(p->env, llhttp_get_upgrade(llp) != 0, &argv[7]);
  napi_get_boolean(p->env, llhttp_should_keep_alive(llp) != 0, &argv[8]);

  napi_value result = nullptr;
  bool has_pending = false;
  if (UbiMakeCallbackWithFlags(p->env,
                               self,
                               cb,
                               9,
                               argv,
                               &result,
                               kUbiMakeCallbackSkipTaskQueues) != napi_ok ||
      (napi_is_exception_pending(p->env, &has_pending) == napi_ok && has_pending)) {
    p->got_exception = true;
    llhttp_set_error_reason(&p->parser, "HPE_JS_EXCEPTION:JS Exception");
    return HPE_USER;
  }
  int32_t rv = 0;
  if (result != nullptr) {
    napi_valuetype rt = napi_undefined;
    napi_typeof(p->env, result, &rt);
    if (rt == napi_number) napi_get_value_int32(p->env, result, &rv);
  }
  return rv;
}

int ParserOnBody(llhttp_t* llp, const char* at, size_t length) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (length == 0) return 0;
  napi_value self = GetWrappedObject(p->env, p->wrapper_ref);
  if (self == nullptr) return 0;
  napi_value cb = nullptr;
  if (napi_get_element(p->env, self, kOnBody, &cb) != napi_ok || cb == nullptr) return 0;
  napi_valuetype t = napi_undefined;
  napi_typeof(p->env, cb, &t);
  if (t != napi_function) return 0;
  napi_value buf = CreateBufferCopy(p->env, at, length);
  napi_value argv[1] = {buf};
  napi_value ignored = nullptr;
  bool has_pending = false;
  if (UbiMakeCallback(p->env, self, cb, 1, argv, &ignored) != napi_ok ||
      (napi_is_exception_pending(p->env, &has_pending) == napi_ok && has_pending)) {
    p->got_exception = true;
    llhttp_set_error_reason(&p->parser, "HPE_JS_EXCEPTION:JS Exception");
    return HPE_USER;
  }
  return 0;
}

int ParserOnMessageComplete(llhttp_t* llp) {
  Parser* p = static_cast<Parser*>(llp->data);
  if (p->list != nullptr) {
    p->list->all.erase(p);
    p->list->active.erase(p);
    p->last_message_start = 0;
    p->list->all.insert(p);
  }
  if (!p->fields.empty()) {
    // _http_common tolerates one-shot headers array.
  }
  return CallIndexedNoArgs(p, kOnMessageComplete, true);
}

int ParserOnChunkExtension(llhttp_t* llp, const char* at, size_t length) {
  Parser* p = static_cast<Parser*>(llp->data);
  p->chunk_extensions_nread += length;
  if (p->chunk_extensions_nread > 16384) {
    llhttp_set_error_reason(&p->parser, "HPE_CHUNK_EXTENSIONS_OVERFLOW:Chunk extensions overflow");
    return HPE_USER;
  }
  return 0;
}

int ParserOnChunkHeader(llhttp_t* llp) {
  Parser* p = static_cast<Parser*>(llp->data);
  p->header_nread = 0;
  p->chunk_extensions_nread = 0;
  return 0;
}

int ParserOnChunkComplete(llhttp_t* llp) {
  Parser* p = static_cast<Parser*>(llp->data);
  p->header_nread = 0;
  return 0;
}

void ParserFinalize(napi_env env, void* data, void* hint) {
  Parser* p = static_cast<Parser*>(data);
  if (p->list != nullptr) {
    p->list->all.erase(p);
    p->list->active.erase(p);
  }
  if (p->wrapper_ref != nullptr) napi_delete_reference(env, p->wrapper_ref);
  delete p;
}

void ConnectionsListFinalize(napi_env env, void* data, void* hint) {
  ConnectionsList* list = static_cast<ConnectionsList*>(data);
  if (list->wrapper_ref != nullptr) napi_delete_reference(env, list->wrapper_ref);
  delete list;
}

napi_value ParserCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* p = new Parser();
  p->env = env;
  llhttp_settings_init(&p->settings);
  p->settings.on_message_begin = ParserOnMessageBegin;
  p->settings.on_url = ParserOnUrl;
  p->settings.on_status = ParserOnStatus;
  p->settings.on_header_field = ParserOnHeaderField;
  p->settings.on_header_value = ParserOnHeaderValue;
  p->settings.on_chunk_extension_name = ParserOnChunkExtension;
  p->settings.on_chunk_extension_value = ParserOnChunkExtension;
  p->settings.on_headers_complete = ParserOnHeadersComplete;
  p->settings.on_body = ParserOnBody;
  p->settings.on_message_complete = ParserOnMessageComplete;
  p->settings.on_chunk_header = ParserOnChunkHeader;
  p->settings.on_chunk_complete = ParserOnChunkComplete;
  llhttp_init(&p->parser, HTTP_REQUEST, &p->settings);
  p->parser.data = p;
  napi_wrap(env, self, p, ParserFinalize, nullptr, &p->wrapper_ref);
  return self;
}

napi_value ConnectionsListCtor(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  auto* list = new ConnectionsList();
  list->env = env;
  napi_wrap(env, self, list, ConnectionsListFinalize, nullptr, &list->wrapper_ref);
  return self;
}

napi_value ParserInitialize(napi_env env, napi_callback_info info) {
  size_t argc = 5;
  napi_value argv[5] = {nullptr};
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) return nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  int32_t type = HTTP_REQUEST;
  if (argc >= 1 && argv[0] != nullptr) napi_get_value_int32(env, argv[0], &type);
  uint64_t max_header = 8 * 1024;
  if (argc >= 3 && argv[2] != nullptr) {
    double d = 0;
    if (napi_get_value_double(env, argv[2], &d) == napi_ok && d > 0) max_header = static_cast<uint64_t>(d);
  }
  int32_t lenient = 0;
  if (argc >= 4 && argv[3] != nullptr) napi_get_value_int32(env, argv[3], &lenient);
  llhttp_init(&p->parser, static_cast<llhttp_type_t>(type), &p->settings);
  p->parser.data = p;
  p->max_http_header_size = max_header;
  p->header_nread = 0;
  p->chunk_extensions_nread = 0;
  p->fields.clear();
  p->values.clear();
  p->url.clear();
  p->status_message.clear();
  p->last_was_value = false;
  p->headers_completed = false;
  p->have_flushed = false;
  p->got_exception = false;
  p->list = nullptr;
  if (argc >= 5 && argv[4] != nullptr) {
    bool is_null = false;
    napi_valuetype value_type = napi_undefined;
    if (napi_typeof(env, argv[4], &value_type) == napi_ok) {
      is_null = (value_type == napi_null || value_type == napi_undefined);
    }
    if (!is_null) {
      ConnectionsList* list = nullptr;
      if (napi_unwrap(env, argv[4], reinterpret_cast<void**>(&list)) == napi_ok) {
        p->list = list;
        p->last_message_start = uv_hrtime();
        list->all.insert(p);
        list->active.insert(p);
      }
    }
  }
  if (lenient & static_cast<int32_t>(kLenientHeaders)) llhttp_set_lenient_headers(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientChunkedLength)) llhttp_set_lenient_chunked_length(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientKeepAlive)) llhttp_set_lenient_keep_alive(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientTransferEncoding)) llhttp_set_lenient_transfer_encoding(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientVersion)) llhttp_set_lenient_version(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientDataAfterClose)) llhttp_set_lenient_data_after_close(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientOptionalLFAfterCR)) llhttp_set_lenient_optional_lf_after_cr(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientOptionalCRLFAfterChunk)) llhttp_set_lenient_optional_crlf_after_chunk(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientOptionalCRBeforeLF)) llhttp_set_lenient_optional_cr_before_lf(&p->parser, 1);
  if (lenient & static_cast<int32_t>(kLenientSpacesAfterChunkSize)) llhttp_set_lenient_spaces_after_chunk_size(&p->parser, 1);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserExecuteCommon(Parser* p, const char* data, size_t len) {
  p->current_buffer_data = data;
  p->current_buffer_len = len;
  p->got_exception = false;
  llhttp_errno_t err = data == nullptr ? llhttp_finish(&p->parser) : llhttp_execute(&p->parser, data, len);
  size_t nread = len;
  if (err != HPE_OK && data != nullptr) {
    const char* pos = llhttp_get_error_pos(&p->parser);
    if (pos != nullptr && pos >= data) nread = static_cast<size_t>(pos - data);
    if (err == HPE_PAUSED_UPGRADE) {
      err = HPE_OK;
      llhttp_resume_after_upgrade(&p->parser);
    }
  }
  if (p->pending_pause) {
    p->pending_pause = false;
    llhttp_pause(&p->parser);
  }
  p->current_buffer_data = nullptr;
  p->current_buffer_len = 0;
  if (p->got_exception) {
    napi_value pending = nullptr;
    if (napi_get_and_clear_last_exception(p->env, &pending) == napi_ok && pending != nullptr) {
      napi_throw(p->env, pending);
    }
    return nullptr;
  }
  if (llhttp_get_upgrade(&p->parser) == 0 && err != HPE_OK) {
    const char* reason = llhttp_get_error_reason(&p->parser);
    const char* code = llhttp_errno_name(err);
    std::string message = "Parse Error: ";
    message += (reason != nullptr) ? reason : "Unknown error";
    return MakeError(p->env,
                     message.c_str(),
                     code != nullptr ? code : "HPE_UNKNOWN",
                     reason,
                     static_cast<uint32_t>(nread),
                     data,
                     len);
  }
  if (data == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(p->env, &undefined);
    return undefined;
  }
  napi_value nread_v = nullptr;
  napi_create_uint32(p->env, static_cast<uint32_t>(nread), &nread_v);
  return nread_v;
}

napi_value ParserExecute(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3] = {nullptr, nullptr, nullptr};
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) {
    napi_value msg = nullptr;
    napi_create_string_utf8(env, "Invalid this for HTTPParser.execute", NAPI_AUTO_LENGTH, &msg);
    napi_value err = nullptr;
    napi_create_type_error(env, nullptr, msg, &err);
    if (err != nullptr) napi_throw(env, err);
    return nullptr;
  }
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1 || argv[0] == nullptr) {
    napi_value zero = nullptr;
    napi_create_uint32(env, 0, &zero);
    return zero;
  }
  bool is_typed_array = false;
  napi_is_typedarray(env, argv[0], &is_typed_array);
  if (!is_typed_array) {
    bool is_buffer = false;
    napi_is_buffer(env, argv[0], &is_buffer);
    if (is_buffer) {
      void* data = nullptr;
      size_t total_len = 0;
      napi_get_buffer_info(env, argv[0], &data, &total_len);
      uint32_t start = 0;
      uint32_t len = static_cast<uint32_t>(total_len);
      if (argc >= 2 && argv[1] != nullptr) napi_get_value_uint32(env, argv[1], &start);
      if (argc >= 3 && argv[2] != nullptr) napi_get_value_uint32(env, argv[2], &len);
      if (start > total_len) start = static_cast<uint32_t>(total_len);
      size_t available = total_len - start;
      if (len > available) len = static_cast<uint32_t>(available);
      const char* ptr = static_cast<const char*>(data) + start;
      return ParserExecuteCommon(p, ptr, len);
    }
    napi_value zero = nullptr;
    napi_create_uint32(env, 0, &zero);
    return zero;
  }
  napi_typedarray_type type;
  size_t length = 0;
  void* data = nullptr;
  napi_value ab = nullptr;
  size_t offset = 0;
  napi_get_typedarray_info(env, argv[0], &type, &length, &data, &ab, &offset);
  uint32_t start = 0;
  uint32_t len = static_cast<uint32_t>(length);
  if (argc >= 2 && argv[1] != nullptr) napi_get_value_uint32(env, argv[1], &start);
  if (argc >= 3 && argv[2] != nullptr) napi_get_value_uint32(env, argv[2], &len);
  if (start > length) start = static_cast<uint32_t>(length);
  size_t available = length - start;
  if (len > available) len = static_cast<uint32_t>(available);
  const char* ptr = static_cast<const char*>(data) + start;
  return ParserExecuteCommon(p, ptr, len);
}

napi_value ParserFinish(napi_env env, napi_callback_info info) {
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) return nullptr;
  return ParserExecuteCommon(p, nullptr, 0);
}

napi_value ParserPause(napi_env env, napi_callback_info info) {
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) return nullptr;
  llhttp_pause(&p->parser);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserResume(napi_env env, napi_callback_info info) {
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) return nullptr;
  llhttp_resume(&p->parser);
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserConsume(napi_env env, napi_callback_info info) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserUnconsume(napi_env env, napi_callback_info info) {
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserGetCurrentBuffer(napi_env env, napi_callback_info info) {
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) return nullptr;
  return CreateUint8ArrayCopy(env, p->current_buffer_data, p->current_buffer_len);
}

napi_value ParserRemove(napi_env env, napi_callback_info info) {
  Parser* p = Unwrap<Parser>(env, info);
  if (p == nullptr) return nullptr;
  if (p->list != nullptr) {
    p->list->all.erase(p);
    p->list->active.erase(p);
    p->list = nullptr;
  }
  p->last_message_start = 0;
  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserClose(napi_env env, napi_callback_info info) {
  napi_value self = nullptr;
  size_t argc = 0;
  napi_get_cb_info(env, info, &argc, nullptr, &self, nullptr);
  if (self == nullptr) return nullptr;

  Parser* p = nullptr;
  if (napi_unwrap(env, self, reinterpret_cast<void**>(&p)) != napi_ok || p == nullptr) {
    napi_value undefined = nullptr;
    napi_get_undefined(env, &undefined);
    return undefined;
  }

  if (p->list != nullptr) {
    p->list->all.erase(p);
    p->list->active.erase(p);
    p->list = nullptr;
  }
  p->last_message_start = 0;

  void* removed = nullptr;
  if (napi_remove_wrap(env, self, &removed) == napi_ok && removed != nullptr) {
    Parser* removed_parser = static_cast<Parser*>(removed);
    if (removed_parser->wrapper_ref != nullptr) {
      napi_delete_reference(env, removed_parser->wrapper_ref);
      removed_parser->wrapper_ref = nullptr;
    }
    delete removed_parser;
  }

  napi_value undefined = nullptr;
  napi_get_undefined(env, &undefined);
  return undefined;
}

napi_value ParserFree(napi_env env, napi_callback_info info) {
  return ParserRemove(env, info);
}

napi_value ConnectionsListAll(napi_env env, napi_callback_info info) {
  ConnectionsList* list = Unwrap<ConnectionsList>(env, info);
  if (list == nullptr) return nullptr;
  napi_value arr = nullptr;
  napi_create_array_with_length(env, list->all.size(), &arr);
  uint32_t idx = 0;
  for (Parser* p : list->all) {
    napi_value obj = GetWrappedObject(env, p->wrapper_ref);
    if (obj != nullptr) napi_set_element(env, arr, idx++, obj);
  }
  return arr;
}

napi_value ConnectionsListIdle(napi_env env, napi_callback_info info) {
  ConnectionsList* list = Unwrap<ConnectionsList>(env, info);
  if (list == nullptr) return nullptr;
  napi_value arr = nullptr;
  napi_create_array(env, &arr);
  uint32_t idx = 0;
  for (Parser* p : list->all) {
    if (p->last_message_start == 0) {
      napi_value obj = GetWrappedObject(env, p->wrapper_ref);
      if (obj != nullptr) napi_set_element(env, arr, idx++, obj);
    }
  }
  return arr;
}

napi_value ConnectionsListActive(napi_env env, napi_callback_info info) {
  ConnectionsList* list = Unwrap<ConnectionsList>(env, info);
  if (list == nullptr) return nullptr;
  napi_value arr = nullptr;
  napi_create_array_with_length(env, list->active.size(), &arr);
  uint32_t idx = 0;
  for (Parser* p : list->active) {
    napi_value obj = GetWrappedObject(env, p->wrapper_ref);
    if (obj != nullptr) napi_set_element(env, arr, idx++, obj);
  }
  return arr;
}

napi_value ConnectionsListExpired(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2] = {nullptr};
  ConnectionsList* list = Unwrap<ConnectionsList>(env, info);
  if (list == nullptr) return nullptr;
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  uint32_t headers_timeout_ms = 0;
  uint32_t request_timeout_ms = 0;
  if (argc > 0 && argv[0] != nullptr) napi_get_value_uint32(env, argv[0], &headers_timeout_ms);
  if (argc > 1 && argv[1] != nullptr) napi_get_value_uint32(env, argv[1], &request_timeout_ms);
  uint64_t headers_timeout = static_cast<uint64_t>(headers_timeout_ms) * 1000000ULL;
  uint64_t request_timeout = static_cast<uint64_t>(request_timeout_ms) * 1000000ULL;
  if (request_timeout > 0 && headers_timeout > request_timeout) std::swap(headers_timeout, request_timeout);
  const uint64_t now = uv_hrtime();
  const uint64_t headers_deadline = (headers_timeout > 0 && now > headers_timeout) ? now - headers_timeout : 0;
  const uint64_t request_deadline = (request_timeout > 0 && now > request_timeout) ? now - request_timeout : 0;

  std::vector<Parser*> expired;
  for (Parser* p : list->active) {
    const bool header_expired = !p->headers_completed && headers_deadline > 0 && p->last_message_start < headers_deadline;
    const bool req_expired = request_deadline > 0 && p->last_message_start < request_deadline;
    if (header_expired || req_expired) expired.push_back(p);
  }
  for (Parser* p : expired) list->active.erase(p);

  napi_value arr = nullptr;
  napi_create_array_with_length(env, expired.size(), &arr);
  for (uint32_t i = 0; i < expired.size(); i++) {
    napi_value obj = GetWrappedObject(env, expired[i]->wrapper_ref);
    if (obj != nullptr) napi_set_element(env, arr, i, obj);
  }
  return arr;
}

void SetNamedValue(napi_env env, napi_value obj, const char* key, napi_value val) {
  if (obj != nullptr && val != nullptr) napi_set_named_property(env, obj, key, val);
}

void SetNamedU32(napi_env env, napi_value obj, const char* key, uint32_t value) {
  napi_value v = nullptr;
  napi_create_uint32(env, value, &v);
  SetNamedValue(env, obj, key, v);
}

void SetMethod(napi_env env, napi_value obj, const char* name, napi_callback cb) {
  napi_value fn = nullptr;
  if (napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) == napi_ok && fn != nullptr) {
    napi_set_named_property(env, obj, name, fn);
  }
}

napi_value CreateMethodsArray(napi_env env, bool all_methods) {
  std::vector<const char*> names;
#define ADD_METHOD(num, name, string) names.push_back(#string);
  if (all_methods) {
    HTTP_ALL_METHOD_MAP(ADD_METHOD)
  } else {
    HTTP_METHOD_MAP(ADD_METHOD)
  }
#undef ADD_METHOD
  napi_value arr = nullptr;
  napi_create_array_with_length(env, names.size(), &arr);
  for (uint32_t i = 0; i < names.size(); i++) {
    napi_value s = nullptr;
    napi_create_string_utf8(env, names[i], NAPI_AUTO_LENGTH, &s);
    if (s != nullptr) napi_set_element(env, arr, i, s);
  }
  return arr;
}

}  // namespace

napi_value UbiInstallHttpParserBinding(napi_env env) {
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr) return nullptr;

  napi_property_descriptor parser_props[] = {
      {"close", nullptr, ParserClose, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"free", nullptr, ParserFree, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"remove", nullptr, ParserRemove, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"execute", nullptr, ParserExecute, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"finish", nullptr, ParserFinish, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"initialize", nullptr, ParserInitialize, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"pause", nullptr, ParserPause, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"resume", nullptr, ParserResume, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"consume", nullptr, ParserConsume, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"unconsume", nullptr, ParserUnconsume, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"getCurrentBuffer", nullptr, ParserGetCurrentBuffer, nullptr, nullptr, nullptr, napi_default, nullptr},
  };

  napi_value parser_ctor = nullptr;
  if (napi_define_class(env,
                        "HTTPParser",
                        NAPI_AUTO_LENGTH,
                        ParserCtor,
                        nullptr,
                        sizeof(parser_props) / sizeof(parser_props[0]),
                        parser_props,
                        &parser_ctor) != napi_ok ||
      parser_ctor == nullptr) {
    return nullptr;
  }

  SetNamedU32(env, parser_ctor, "REQUEST", HTTP_REQUEST);
  SetNamedU32(env, parser_ctor, "RESPONSE", HTTP_RESPONSE);
  SetNamedU32(env, parser_ctor, "kOnMessageBegin", kOnMessageBegin);
  SetNamedU32(env, parser_ctor, "kOnHeaders", kOnHeaders);
  SetNamedU32(env, parser_ctor, "kOnHeadersComplete", kOnHeadersComplete);
  SetNamedU32(env, parser_ctor, "kOnBody", kOnBody);
  SetNamedU32(env, parser_ctor, "kOnMessageComplete", kOnMessageComplete);
  SetNamedU32(env, parser_ctor, "kOnExecute", kOnExecute);
  SetNamedU32(env, parser_ctor, "kOnTimeout", kOnTimeout);
  SetNamedU32(env, parser_ctor, "kHasNativeConsume", 0);

  SetNamedU32(env, parser_ctor, "kLenientNone", kLenientNone);
  SetNamedU32(env, parser_ctor, "kLenientHeaders", kLenientHeaders);
  SetNamedU32(env, parser_ctor, "kLenientChunkedLength", kLenientChunkedLength);
  SetNamedU32(env, parser_ctor, "kLenientKeepAlive", kLenientKeepAlive);
  SetNamedU32(env, parser_ctor, "kLenientTransferEncoding", kLenientTransferEncoding);
  SetNamedU32(env, parser_ctor, "kLenientVersion", kLenientVersion);
  SetNamedU32(env, parser_ctor, "kLenientDataAfterClose", kLenientDataAfterClose);
  SetNamedU32(env, parser_ctor, "kLenientOptionalLFAfterCR", kLenientOptionalLFAfterCR);
  SetNamedU32(env, parser_ctor, "kLenientOptionalCRLFAfterChunk", kLenientOptionalCRLFAfterChunk);
  SetNamedU32(env, parser_ctor, "kLenientOptionalCRBeforeLF", kLenientOptionalCRBeforeLF);
  SetNamedU32(env, parser_ctor, "kLenientSpacesAfterChunkSize", kLenientSpacesAfterChunkSize);
  SetNamedU32(env, parser_ctor, "kLenientAll", kLenientAll);

  napi_property_descriptor list_props[] = {
      {"all", nullptr, ConnectionsListAll, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"idle", nullptr, ConnectionsListIdle, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"active", nullptr, ConnectionsListActive, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"expired", nullptr, ConnectionsListExpired, nullptr, nullptr, nullptr, napi_default, nullptr},
  };
  napi_value list_ctor = nullptr;
  if (napi_define_class(env,
                        "ConnectionsList",
                        NAPI_AUTO_LENGTH,
                        ConnectionsListCtor,
                        nullptr,
                        sizeof(list_props) / sizeof(list_props[0]),
                        list_props,
                        &list_ctor) != napi_ok ||
      list_ctor == nullptr) {
    return nullptr;
  }

  SetNamedValue(env, binding, "HTTPParser", parser_ctor);
  SetNamedValue(env, binding, "ConnectionsList", list_ctor);
  SetNamedValue(env, binding, "methods", CreateMethodsArray(env, false));
  SetNamedValue(env, binding, "allMethods", CreateMethodsArray(env, true));

  return binding;
}
