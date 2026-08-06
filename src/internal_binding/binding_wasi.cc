#include "internal_binding/binding_initializers.h"

#include "internal_binding/helpers.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "uvwasi.h"
#include "unofficial_napi.h"
#include "ncrypto.h"

namespace internal_binding {
namespace {

struct WasmMemory {
  napi_env env = nullptr;
  napi_value buffer = nullptr;
  char *data = nullptr;
  size_t size = 0;
  bool host_backed = false;

  bool Read(size_t offset, void *destination, size_t length) const {
    if (!uvwasi_serdes_check_bounds(offset, size, length))
      return false;
    if (host_backed) {
      return unofficial_napi_read_host_arraybuffer(
                 env, buffer, offset, destination, length) == napi_ok;
    }
    if (length != 0)
      std::memcpy(destination, data + offset, length);
    return true;
  }

  bool Write(size_t offset, const void *source, size_t length) const {
    if (!uvwasi_serdes_check_bounds(offset, size, length))
      return false;
    if (host_backed) {
      return unofficial_napi_write_host_arraybuffer(
                 env, buffer, offset, source, length) == napi_ok;
    }
    if (length != 0)
      std::memcpy(data + offset, source, length);
    return true;
  }
};

template <size_t Size, typename Serialize>
bool SerializeToMemory(const WasmMemory &memory, size_t offset,
                       Serialize serialize) {
  std::array<uint8_t, Size> bytes{};
  serialize(bytes.data());
  return memory.Write(offset, bytes.data(), bytes.size());
}

template <size_t Size, typename Deserialize>
bool DeserializeFromMemory(const WasmMemory &memory, size_t offset,
                           Deserialize deserialize) {
  std::array<uint8_t, Size> bytes{};
  if (!memory.Read(offset, bytes.data(), bytes.size()))
    return false;
  deserialize(bytes.data());
  return true;
}

uint32_t ReadUint32(const uint8_t *bytes) {
  return static_cast<uint32_t>(bytes[0]) |
         (static_cast<uint32_t>(bytes[1]) << 8) |
         (static_cast<uint32_t>(bytes[2]) << 16) |
         (static_cast<uint32_t>(bytes[3]) << 24);
}

bool ReadIovecDescriptor(const WasmMemory &memory, size_t offset,
                         uint32_t *buffer_offset, uint32_t *buffer_length) {
  std::array<uint8_t, UVWASI_SERDES_SIZE_iovec_t> descriptor{};
  if (!memory.Read(offset, descriptor.data(), descriptor.size()))
    return false;
  *buffer_offset = ReadUint32(descriptor.data());
  *buffer_length = ReadUint32(descriptor.data() + 4);
  return uvwasi_serdes_check_bounds(*buffer_offset, memory.size,
                                    *buffer_length);
}

bool LoadInputIovecs(const WasmMemory &memory, uint32_t descriptors_offset,
                     uint32_t count, std::vector<uvwasi_ciovec_t> *iovs,
                     std::vector<std::vector<uint8_t>> *buffers) {
  iovs->resize(count);
  buffers->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t offset = 0;
    uint32_t length = 0;
    if (!ReadIovecDescriptor(memory,
                             descriptors_offset +
                                 i * UVWASI_SERDES_SIZE_ciovec_t,
                             &offset, &length))
      return false;
    (*buffers)[i].resize(length);
    if (!memory.Read(offset, (*buffers)[i].data(), length))
      return false;
    (*iovs)[i].buf = (*buffers)[i].data();
    (*iovs)[i].buf_len = length;
  }
  return true;
}

bool LoadOutputIovecs(const WasmMemory &memory, uint32_t descriptors_offset,
                      uint32_t count, std::vector<uvwasi_iovec_t> *iovs,
                      std::vector<std::vector<uint8_t>> *buffers,
                      std::vector<uint32_t> *offsets) {
  iovs->resize(count);
  buffers->resize(count);
  offsets->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t length = 0;
    if (!ReadIovecDescriptor(memory,
                             descriptors_offset + i * UVWASI_SERDES_SIZE_iovec_t,
                             &(*offsets)[i], &length))
      return false;
    (*buffers)[i].resize(length);
    (*iovs)[i].buf = (*buffers)[i].data();
    (*iovs)[i].buf_len = length;
  }
  return true;
}

bool CommitOutputIovecs(const WasmMemory &memory,
                        const std::vector<std::vector<uint8_t>> &buffers,
                        const std::vector<uint32_t> &offsets,
                        size_t bytes_written) {
  for (size_t i = 0; i < buffers.size() && bytes_written != 0; ++i) {
    const size_t length = std::min(bytes_written, buffers[i].size());
    if (!memory.Write(offsets[i], buffers[i].data(), length))
      return false;
    bytes_written -= length;
  }
  return true;
}

bool ReadBytes(const WasmMemory &memory, size_t offset, size_t length,
               std::vector<uint8_t> *bytes) {
  bytes->resize(length);
  return memory.Read(offset, bytes->data(), length);
}

bool ReadChars(const WasmMemory &memory, size_t offset, size_t length,
               std::vector<char> *bytes) {
  bytes->resize(length);
  return memory.Read(offset, bytes->data(), length);
}

#define CHECK_BOUNDS_OR_RETURN(mem_size, offset, buf_size)                     \
  do {                                                                         \
    if (!uvwasi_serdes_check_bounds((offset), (mem_size), (buf_size))) {       \
      return UVWASI_EOVERFLOW;                                                 \
    }                                                                          \
  } while (0)

std::string ValueToString(napi_env env, napi_value value) {
  napi_value string = nullptr;
  if (napi_coerce_to_string(env, value, &string) != napi_ok ||
      string == nullptr) {
    return {};
  }
  size_t length = 0;
  if (napi_get_value_string_utf8(env, string, nullptr, 0, &length) != napi_ok) {
    return {};
  }
  std::vector<char> buffer(length + 1, '\0');
  size_t written = 0;
  if (napi_get_value_string_utf8(env, string, buffer.data(), buffer.size(),
                                 &written) != napi_ok) {
    return {};
  }
  return std::string(buffer.data(), written);
}

void SetErrorProperty(napi_env env, napi_value error, const char *name,
                      napi_value value) {
  if (error != nullptr && value != nullptr) {
    (void)napi_set_named_property(env, error, name, value);
  }
}

void ThrowWasiError(napi_env env, int errorno, const char *syscall) {
  const char *code = uvwasi_embedder_err_code_to_string(errorno);
  if (code == nullptr)
    code = "UVWASI_EUNKNOWN";
  const std::string message = std::string(code) + ", " + syscall;
  napi_value message_value = nullptr;
  napi_value error = nullptr;
  napi_create_string_utf8(env, message.c_str(), message.size(), &message_value);
  napi_create_error(env, nullptr, message_value, &error);
  if (error == nullptr) {
    napi_throw_error(env, code, message.c_str());
    return;
  }
  napi_value errno_value = nullptr;
  napi_value code_value = nullptr;
  napi_value syscall_value = nullptr;
  napi_create_int32(env, errorno, &errno_value);
  napi_create_string_utf8(env, code, NAPI_AUTO_LENGTH, &code_value);
  napi_create_string_utf8(env, syscall, NAPI_AUTO_LENGTH, &syscall_value);
  SetErrorProperty(env, error, "errno", errno_value);
  SetErrorProperty(env, error, "code", code_value);
  SetErrorProperty(env, error, "syscall", syscall_value);
  napi_throw(env, error);
}

void ThrowWasiNotStarted(napi_env env) {
  napi_value message = nullptr;
  napi_value code = nullptr;
  napi_value error = nullptr;
  napi_create_string_utf8(env, "wasi.start() has not been called",
                          NAPI_AUTO_LENGTH, &message);
  napi_create_string_utf8(env, "ERR_WASI_NOT_STARTED", NAPI_AUTO_LENGTH, &code);
  napi_create_error(env, code, message, &error);
  SetErrorProperty(env, error, "code", code);
  napi_throw(env, error);
}

class WasiState {
public:
  explicit WasiState(napi_env env_in) : env(env_in) {
    uvwasi_options_init(&options);
  }

  ~WasiState() {
    if (memory_ref != nullptr)
      napi_delete_reference(env, memory_ref);
    if (initialized)
      uvwasi_destroy(&uvwasi);
  }

  bool
  Initialize(const std::vector<std::string> &args,
             const std::vector<std::string> &environment,
             const std::vector<std::pair<std::string, std::string>> &preopens,
             const std::array<int32_t, 3> &stdio) {
    std::vector<const char *> argv;
    argv.reserve(args.size());
    for (const auto &arg : args)
      argv.push_back(arg.c_str());

    std::vector<const char *> envp;
    envp.reserve(environment.size() + 1);
    for (const auto &entry : environment)
      envp.push_back(entry.c_str());
    envp.push_back(nullptr);

    std::vector<uvwasi_preopen_t> uv_preopens(preopens.size());
    for (size_t i = 0; i < preopens.size(); ++i) {
      uv_preopens[i].mapped_path = preopens[i].first.c_str();
      uv_preopens[i].real_path = preopens[i].second.c_str();
    }

    options.in = stdio[0];
    options.out = stdio[1];
    options.err = stdio[2];
    options.fd_table_size = 3;
    options.argc = static_cast<uvwasi_size_t>(argv.size());
    options.argv = argv.empty() ? nullptr : argv.data();
    options.envp = envp.data();
    options.preopenc = static_cast<uvwasi_size_t>(uv_preopens.size());
    options.preopens = uv_preopens.empty() ? nullptr : uv_preopens.data();

    const uvwasi_errno_t error = uvwasi_init(&uvwasi, &options);
    if (error != UVWASI_ESUCCESS) {
      ThrowWasiError(env, error, "uvwasi_init");
      return false;
    }
    initialized = true;
    return true;
  }

  bool SetMemory(napi_value memory) {
    if (memory == nullptr) {
      napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE",
                            "\"instance.exports.memory\" property must be a "
                            "WebAssembly.Memory object");
      return false;
    }
    napi_value buffer = nullptr;
    const napi_status buffer_status =
        napi_get_named_property(env, memory, "buffer", &buffer);
    if (buffer_status != napi_ok)
      return false;
    if (buffer == nullptr) {
      napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE",
                            "\"instance.exports.memory\" property must be a "
                            "WebAssembly.Memory object");
      return false;
    }
    void *data = nullptr;
    size_t size = 0;
    if (napi_get_arraybuffer_info(env, buffer, &data, &size) != napi_ok ||
        data == nullptr) {
      napi_throw_type_error(env, "ERR_INVALID_ARG_TYPE",
                            "\"instance.exports.memory\" property must be a "
                            "WebAssembly.Memory object");
      return false;
    }
    if (memory_ref != nullptr) {
      napi_delete_reference(env, memory_ref);
      memory_ref = nullptr;
    }
    return napi_create_reference(env, memory, 1, &memory_ref) == napi_ok;
  }

  bool GetMemory(WasmMemory *out) {
    if (out == nullptr || memory_ref == nullptr) {
      ThrowWasiNotStarted(env);
      return false;
    }
    napi_value memory = nullptr;
    napi_value buffer = nullptr;
    const napi_status memory_status =
        napi_get_reference_value(env, memory_ref, &memory);
    if (memory_status != napi_ok)
      return false;
    if (memory == nullptr) {
      ThrowWasiNotStarted(env);
      return false;
    }
    const napi_status buffer_status =
        napi_get_named_property(env, memory, "buffer", &buffer);
    if (buffer_status != napi_ok)
      return false;
    if (buffer == nullptr) {
      ThrowWasiNotStarted(env);
      return false;
    }
    void *data = nullptr;
    size_t size = 0;
    if (napi_get_arraybuffer_info(env, buffer, &data, &size) != napi_ok ||
        data == nullptr) {
      ThrowWasiNotStarted(env);
      return false;
    }
    out->data = static_cast<char *>(data);
    out->size = size;
    out->env = env;
    out->buffer = buffer;
    if (unofficial_napi_is_host_arraybuffer(env, buffer, &out->host_backed) !=
        napi_ok) {
      return false;
    }
    return true;
  }

  static uint32_t ArgsGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t ArgsSizesGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t ClockResGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t ClockTimeGet(WasiState &, WasmMemory, uint32_t, uint64_t,
                               uint32_t);
  static uint32_t EnvironGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t EnvironSizesGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdAdvise(WasiState &, WasmMemory, uint32_t, uint64_t,
                           uint64_t, uint32_t);
  static uint32_t FdAllocate(WasiState &, WasmMemory, uint32_t, uint64_t,
                             uint64_t);
  static uint32_t FdClose(WasiState &, WasmMemory, uint32_t);
  static uint32_t FdDatasync(WasiState &, WasmMemory, uint32_t);
  static uint32_t FdFdstatGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdFdstatSetFlags(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdFdstatSetRights(WasiState &, WasmMemory, uint32_t, uint64_t,
                                    uint64_t);
  static uint32_t FdFilestatGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdFilestatSetSize(WasiState &, WasmMemory, uint32_t,
                                    uint64_t);
  static uint32_t FdFilestatSetTimes(WasiState &, WasmMemory, uint32_t,
                                     uint64_t, uint64_t, uint32_t);
  static uint32_t FdPread(WasiState &, WasmMemory, uint32_t, uint32_t, uint32_t,
                          uint64_t, uint32_t);
  static uint32_t FdPrestatGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdPrestatDirName(WasiState &, WasmMemory, uint32_t, uint32_t,
                                   uint32_t);
  static uint32_t FdPwrite(WasiState &, WasmMemory, uint32_t, uint32_t,
                           uint32_t, uint64_t, uint32_t);
  static uint32_t FdRead(WasiState &, WasmMemory, uint32_t, uint32_t, uint32_t,
                         uint32_t);
  static uint32_t FdReaddir(WasiState &, WasmMemory, uint32_t, uint32_t,
                            uint32_t, uint64_t, uint32_t);
  static uint32_t FdRenumber(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdSeek(WasiState &, WasmMemory, uint32_t, int64_t, uint32_t,
                         uint32_t);
  static uint32_t FdSync(WasiState &, WasmMemory, uint32_t);
  static uint32_t FdTell(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t FdWrite(WasiState &, WasmMemory, uint32_t, uint32_t, uint32_t,
                          uint32_t);
  static uint32_t PathCreateDirectory(WasiState &, WasmMemory, uint32_t,
                                      uint32_t, uint32_t);
  static uint32_t PathFilestatGet(WasiState &, WasmMemory, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t);
  static uint32_t PathFilestatSetTimes(WasiState &, WasmMemory, uint32_t,
                                       uint32_t, uint32_t, uint32_t, uint64_t,
                                       uint64_t, uint32_t);
  static uint32_t PathLink(WasiState &, WasmMemory, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
  static uint32_t PathOpen(WasiState &, WasmMemory, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint64_t, uint64_t,
                           uint32_t, uint32_t);
  static uint32_t PathReadlink(WasiState &, WasmMemory, uint32_t, uint32_t,
                               uint32_t, uint32_t, uint32_t, uint32_t);
  static uint32_t PathRemoveDirectory(WasiState &, WasmMemory, uint32_t,
                                      uint32_t, uint32_t);
  static uint32_t PathRename(WasiState &, WasmMemory, uint32_t, uint32_t,
                             uint32_t, uint32_t, uint32_t, uint32_t);
  static uint32_t PathSymlink(WasiState &, WasmMemory, uint32_t, uint32_t,
                              uint32_t, uint32_t, uint32_t);
  static uint32_t PathUnlinkFile(WasiState &, WasmMemory, uint32_t, uint32_t,
                                 uint32_t);
  static uint32_t PollOneoff(WasiState &, WasmMemory, uint32_t, uint32_t,
                             uint32_t, uint32_t);
  static void ProcExit(WasiState &, WasmMemory, uint32_t);
  static uint32_t ProcRaise(WasiState &, WasmMemory, uint32_t);
  static uint32_t RandomGet(WasiState &, WasmMemory, uint32_t, uint32_t);
  static uint32_t SchedYield(WasiState &, WasmMemory);
  static uint32_t SockAccept(WasiState &, WasmMemory, uint32_t, uint32_t,
                             uint32_t);
  static uint32_t SockRecv(WasiState &, WasmMemory, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t);
  static uint32_t SockSend(WasiState &, WasmMemory, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t);
  static uint32_t SockShutdown(WasiState &, WasmMemory, uint32_t, uint32_t);

  napi_env env = nullptr;
  uvwasi_t uvwasi{};

private:
  uvwasi_options_t options{};
  napi_ref memory_ref = nullptr;
  bool initialized = false;
};

template <typename T> bool ReadArgument(napi_env env, napi_value value, T *out);

template <>
bool ReadArgument<uint32_t>(napi_env env, napi_value value, uint32_t *out) {
  return napi_get_value_uint32(env, value, out) == napi_ok;
}

template <>
bool ReadArgument<uint64_t>(napi_env env, napi_value value, uint64_t *out) {
  bool lossless = false;
  return napi_get_value_bigint_uint64(env, value, out, &lossless) == napi_ok &&
         lossless;
}

template <>
bool ReadArgument<int64_t>(napi_env env, napi_value value, int64_t *out) {
  bool lossless = false;
  return napi_get_value_bigint_int64(env, value, out, &lossless) == napi_ok &&
         lossless;
}

template <typename Tuple, size_t... Indices>
bool ReadArguments(napi_env env, napi_value *argv, Tuple *values,
                   std::index_sequence<Indices...>) {
  return (ReadArgument(env, argv[Indices], &std::get<Indices>(*values)) && ...);
}

template <auto Function> struct WasiCallback;

template <typename Return, typename... Args,
          Return (*Function)(WasiState &, WasmMemory, Args...)>
struct WasiCallback<Function> {
  static napi_value Invoke(napi_env env, napi_callback_info info) {
    constexpr size_t kArgc = sizeof...(Args);
    std::array<napi_value, kArgc == 0 ? 1 : kArgc> argv{};
    size_t argc = kArgc;
    napi_value this_arg = nullptr;
    if (napi_get_cb_info(env, info, &argc, argv.data(), &this_arg, nullptr) !=
        napi_ok) {
      return nullptr;
    }
    if (argc != kArgc) {
      napi_value result = nullptr;
      napi_create_uint32(env, UVWASI_EINVAL, &result);
      return result;
    }
    WasiState *state = nullptr;
    if (napi_unwrap(env, this_arg, reinterpret_cast<void **>(&state)) !=
            napi_ok ||
        state == nullptr) {
      napi_value result = nullptr;
      napi_create_uint32(env, UVWASI_EINVAL, &result);
      return result;
    }
    WasmMemory memory;
    if (!state->GetMemory(&memory))
      return nullptr;
    std::tuple<Args...> values;
    if (!ReadArguments(env, argv.data(), &values,
                       std::index_sequence_for<Args...>{})) {
      napi_value result = nullptr;
      napi_create_uint32(env, UVWASI_EINVAL, &result);
      return result;
    }
    if constexpr (std::is_void_v<Return>) {
      std::apply(
          [&](Args... unpacked) { Function(*state, memory, unpacked...); },
          values);
      return Undefined(env);
    } else {
      const Return value = std::apply(
          [&](Args... unpacked) {
            return Function(*state, memory, unpacked...);
          },
          values);
      napi_value result = nullptr;
      napi_create_uint32(env, static_cast<uint32_t>(value), &result);
      return result;
    }
  }
};

void FinalizeWasi(napi_env, void *data, void *) {
  delete static_cast<WasiState *>(data);
}

bool ReadStringArray(napi_env env, napi_value array,
                     std::vector<std::string> *out) {
  uint32_t length = 0;
  if (out == nullptr || napi_get_array_length(env, array, &length) != napi_ok)
    return false;
  out->clear();
  out->reserve(length);
  for (uint32_t i = 0; i < length; ++i) {
    napi_value value = nullptr;
    if (napi_get_element(env, array, i, &value) != napi_ok || value == nullptr)
      return false;
    out->push_back(ValueToString(env, value));
  }
  return true;
}

napi_value WasiConstructor(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4]{};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok ||
      argc != 4) {
    napi_throw_type_error(env, nullptr,
                          "WASI internal constructor expects four arguments");
    return nullptr;
  }

  std::vector<std::string> args;
  std::vector<std::string> environment;
  std::vector<std::string> flat_preopens;
  if (!ReadStringArray(env, argv[0], &args) ||
      !ReadStringArray(env, argv[1], &environment) ||
      !ReadStringArray(env, argv[2], &flat_preopens) ||
      flat_preopens.size() % 2 != 0) {
    napi_throw_type_error(env, nullptr, "Invalid WASI constructor arguments");
    return nullptr;
  }
  std::vector<std::pair<std::string, std::string>> preopens;
  preopens.reserve(flat_preopens.size() / 2);
  for (size_t i = 0; i < flat_preopens.size(); i += 2) {
    preopens.emplace_back(flat_preopens[i], flat_preopens[i + 1]);
  }

  uint32_t stdio_length = 0;
  std::array<int32_t, 3> stdio{};
  if (napi_get_array_length(env, argv[3], &stdio_length) != napi_ok ||
      stdio_length != 3) {
    napi_throw_type_error(env, nullptr, "Invalid WASI stdio descriptors");
    return nullptr;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    napi_value value = nullptr;
    if (napi_get_element(env, argv[3], i, &value) != napi_ok ||
        napi_get_value_int32(env, value, &stdio[i]) != napi_ok) {
      napi_throw_type_error(env, nullptr, "Invalid WASI stdio descriptor");
      return nullptr;
    }
  }

  auto *state = new WasiState(env);
  if (!state->Initialize(args, environment, preopens, stdio)) {
    delete state;
    return nullptr;
  }
  if (napi_wrap(env, this_arg, state, FinalizeWasi, nullptr, nullptr) !=
      napi_ok) {
    delete state;
    return nullptr;
  }
  return this_arg;
}

napi_value SetMemoryCallback(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1]{};
  napi_value this_arg = nullptr;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, nullptr) != napi_ok ||
      argc != 1) {
    return nullptr;
  }
  WasiState *state = nullptr;
  if (napi_unwrap(env, this_arg, reinterpret_cast<void **>(&state)) !=
          napi_ok ||
      state == nullptr) {
    napi_throw_type_error(env, nullptr, "Invalid WASI receiver");
    return nullptr;
  }
  return state->SetMemory(argv[0]) ? Undefined(env) : nullptr;
}

uint32_t WasiState::ArgsGet(WasiState &wasi, WasmMemory memory,
                            uint32_t argv_offset, uint32_t argv_buf_offset) {
  CHECK_BOUNDS_OR_RETURN(memory.size, argv_buf_offset,
                         wasi.uvwasi.argv_buf_size);
  CHECK_BOUNDS_OR_RETURN(memory.size, argv_offset,
                         static_cast<size_t>(wasi.uvwasi.argc) *
                             UVWASI_SERDES_SIZE_uint32_t);
  std::vector<char *> argv(wasi.uvwasi.argc);
  std::vector<char> argv_storage(wasi.uvwasi.argv_buf_size);
  char *argv_buf = argv_storage.data();
  const uvwasi_errno_t error =
      uvwasi_args_get(&wasi.uvwasi, argv.data(), argv_buf);
  if (error == UVWASI_ESUCCESS &&
      !memory.Write(argv_buf_offset, argv_storage.data(), argv_storage.size()))
    return UVWASI_EFAULT;
  if (error == UVWASI_ESUCCESS && !argv.empty()) {
    for (size_t i = 0; i < argv.size(); ++i) {
      const uint32_t offset =
          static_cast<uint32_t>(argv_buf_offset + (argv[i] - argv[0]));
      if (!SerializeToMemory<UVWASI_SERDES_SIZE_uint32_t>(
              memory, argv_offset + (i * UVWASI_SERDES_SIZE_uint32_t),
              [&](void *bytes) {
                uvwasi_serdes_write_uint32_t(bytes, 0, offset);
              }))
        return UVWASI_EFAULT;
    }
  }
  return error;
}

uint32_t WasiState::ArgsSizesGet(WasiState &wasi, WasmMemory memory,
                                 uint32_t argc_offset,
                                 uint32_t argv_buf_offset) {
  CHECK_BOUNDS_OR_RETURN(memory.size, argc_offset, UVWASI_SERDES_SIZE_size_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, argv_buf_offset,
                         UVWASI_SERDES_SIZE_size_t);
  uvwasi_size_t argc = 0;
  uvwasi_size_t size = 0;
  const uvwasi_errno_t error =
      uvwasi_args_sizes_get(&wasi.uvwasi, &argc, &size);
  if (error == UVWASI_ESUCCESS) {
    if (!SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
            memory, argc_offset,
            [&](void *bytes) { uvwasi_serdes_write_size_t(bytes, 0, argc); }) ||
        !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
            memory, argv_buf_offset,
            [&](void *bytes) { uvwasi_serdes_write_size_t(bytes, 0, size); }))
      return UVWASI_EFAULT;
  }
  return error;
}

uint32_t WasiState::ClockResGet(WasiState &wasi, WasmMemory memory,
                                uint32_t clock_id, uint32_t resolution_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, resolution_ptr,
                         UVWASI_SERDES_SIZE_timestamp_t);
  uvwasi_timestamp_t resolution = 0;
  const uvwasi_errno_t error =
      uvwasi_clock_res_get(&wasi.uvwasi, clock_id, &resolution);
  if (error == UVWASI_ESUCCESS) {
    if (!SerializeToMemory<UVWASI_SERDES_SIZE_timestamp_t>(
            memory, resolution_ptr, [&](void *bytes) {
              uvwasi_serdes_write_timestamp_t(bytes, 0, resolution);
            }))
      return UVWASI_EFAULT;
  }
  return error;
}

uint32_t WasiState::ClockTimeGet(WasiState &wasi, WasmMemory memory,
                                 uint32_t clock_id, uint64_t precision,
                                 uint32_t time_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, time_ptr, UVWASI_SERDES_SIZE_timestamp_t);
  uvwasi_timestamp_t time = 0;
  const uvwasi_errno_t error =
      uvwasi_clock_time_get(&wasi.uvwasi, clock_id, precision, &time);
  if (error == UVWASI_ESUCCESS) {
    if (!SerializeToMemory<UVWASI_SERDES_SIZE_timestamp_t>(
            memory, time_ptr, [&](void *bytes) {
              uvwasi_serdes_write_timestamp_t(bytes, 0, time);
            }))
      return UVWASI_EFAULT;
  }
  return error;
}

uint32_t WasiState::EnvironGet(WasiState &wasi, WasmMemory memory,
                               uint32_t environ_offset,
                               uint32_t environ_buf_offset) {
  CHECK_BOUNDS_OR_RETURN(memory.size, environ_buf_offset,
                         wasi.uvwasi.env_buf_size);
  CHECK_BOUNDS_OR_RETURN(memory.size, environ_offset,
                         static_cast<size_t>(wasi.uvwasi.envc) *
                             UVWASI_SERDES_SIZE_uint32_t);
  std::vector<char *> environment(wasi.uvwasi.envc);
  std::vector<char> environ_storage(wasi.uvwasi.env_buf_size);
  char *environ_buf = environ_storage.data();
  const uvwasi_errno_t error =
      uvwasi_environ_get(&wasi.uvwasi, environment.data(), environ_buf);
  if (error == UVWASI_ESUCCESS &&
      !memory.Write(environ_buf_offset, environ_storage.data(),
                    environ_storage.size()))
    return UVWASI_EFAULT;
  if (error == UVWASI_ESUCCESS && !environment.empty()) {
    for (size_t i = 0; i < environment.size(); ++i) {
      const uint32_t offset = static_cast<uint32_t>(
          environ_buf_offset + (environment[i] - environment[0]));
      if (!SerializeToMemory<UVWASI_SERDES_SIZE_uint32_t>(
              memory, environ_offset + (i * UVWASI_SERDES_SIZE_uint32_t),
              [&](void *bytes) {
                uvwasi_serdes_write_uint32_t(bytes, 0, offset);
              }))
        return UVWASI_EFAULT;
    }
  }
  return error;
}

uint32_t WasiState::EnvironSizesGet(WasiState &wasi, WasmMemory memory,
                                    uint32_t envc_offset,
                                    uint32_t env_buf_offset) {
  CHECK_BOUNDS_OR_RETURN(memory.size, envc_offset, UVWASI_SERDES_SIZE_size_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, env_buf_offset,
                         UVWASI_SERDES_SIZE_size_t);
  uvwasi_size_t envc = 0;
  uvwasi_size_t size = 0;
  const uvwasi_errno_t error =
      uvwasi_environ_sizes_get(&wasi.uvwasi, &envc, &size);
  if (error == UVWASI_ESUCCESS) {
    if (!SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
            memory, envc_offset,
            [&](void *bytes) { uvwasi_serdes_write_size_t(bytes, 0, envc); }) ||
        !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
            memory, env_buf_offset,
            [&](void *bytes) { uvwasi_serdes_write_size_t(bytes, 0, size); }))
      return UVWASI_EFAULT;
  }
  return error;
}

uint32_t WasiState::FdAdvise(WasiState &wasi, WasmMemory, uint32_t fd,
                             uint64_t offset, uint64_t len, uint32_t advice) {
  return uvwasi_fd_advise(&wasi.uvwasi, fd, offset, len, advice);
}

uint32_t WasiState::FdAllocate(WasiState &wasi, WasmMemory, uint32_t fd,
                               uint64_t offset, uint64_t len) {
  return uvwasi_fd_allocate(&wasi.uvwasi, fd, offset, len);
}

uint32_t WasiState::FdClose(WasiState &wasi, WasmMemory, uint32_t fd) {
  return uvwasi_fd_close(&wasi.uvwasi, fd);
}

uint32_t WasiState::FdDatasync(WasiState &wasi, WasmMemory, uint32_t fd) {
  return uvwasi_fd_datasync(&wasi.uvwasi, fd);
}

uint32_t WasiState::FdFdstatGet(WasiState &wasi, WasmMemory memory, uint32_t fd,
                                uint32_t buf) {
  CHECK_BOUNDS_OR_RETURN(memory.size, buf, UVWASI_SERDES_SIZE_fdstat_t);
  uvwasi_fdstat_t stats{};
  const uvwasi_errno_t error = uvwasi_fd_fdstat_get(&wasi.uvwasi, fd, &stats);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_fdstat_t>(
          memory, buf, [&](void *bytes) {
            uvwasi_serdes_write_fdstat_t(bytes, 0, &stats);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdFdstatSetFlags(WasiState &wasi, WasmMemory, uint32_t fd,
                                     uint32_t flags) {
  return uvwasi_fd_fdstat_set_flags(&wasi.uvwasi, fd, flags);
}

uint32_t WasiState::FdFdstatSetRights(WasiState &wasi, WasmMemory, uint32_t fd,
                                      uint64_t base, uint64_t inheriting) {
  return uvwasi_fd_fdstat_set_rights(&wasi.uvwasi, fd, base, inheriting);
}

uint32_t WasiState::FdFilestatGet(WasiState &wasi, WasmMemory memory,
                                  uint32_t fd, uint32_t buf) {
  CHECK_BOUNDS_OR_RETURN(memory.size, buf, UVWASI_SERDES_SIZE_filestat_t);
  uvwasi_filestat_t stats{};
  const uvwasi_errno_t error = uvwasi_fd_filestat_get(&wasi.uvwasi, fd, &stats);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_filestat_t>(
          memory, buf, [&](void *bytes) {
            uvwasi_serdes_write_filestat_t(bytes, 0, &stats);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdFilestatSetSize(WasiState &wasi, WasmMemory, uint32_t fd,
                                      uint64_t size) {
  return uvwasi_fd_filestat_set_size(&wasi.uvwasi, fd, size);
}

uint32_t WasiState::FdFilestatSetTimes(WasiState &wasi, WasmMemory, uint32_t fd,
                                       uint64_t atime, uint64_t mtime,
                                       uint32_t flags) {
  return uvwasi_fd_filestat_set_times(&wasi.uvwasi, fd, atime, mtime, flags);
}

uint32_t WasiState::FdPread(WasiState &wasi, WasmMemory memory, uint32_t fd,
                            uint32_t iovs_ptr, uint32_t iovs_len,
                            uint64_t offset, uint32_t nread_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, iovs_ptr,
                         static_cast<size_t>(iovs_len) *
                             UVWASI_SERDES_SIZE_iovec_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, nread_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<uvwasi_iovec_t> iovs(iovs_len);
  std::vector<std::vector<uint8_t>> buffers;
  std::vector<uint32_t> offsets;
  if (!LoadOutputIovecs(memory, iovs_ptr, iovs_len, &iovs, &buffers, &offsets))
    return UVWASI_EFAULT;
  uvwasi_size_t nread = 0;
  uvwasi_errno_t error =
      uvwasi_fd_pread(&wasi.uvwasi, fd, iovs.data(), iovs_len, offset, &nread);
  if (error == UVWASI_ESUCCESS &&
      (!CommitOutputIovecs(memory, buffers, offsets, nread) ||
       !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
           memory, nread_ptr, [&](void *bytes) {
             uvwasi_serdes_write_size_t(bytes, 0, nread);
           })))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdPrestatGet(WasiState &wasi, WasmMemory memory,
                                 uint32_t fd, uint32_t buf) {
  CHECK_BOUNDS_OR_RETURN(memory.size, buf, UVWASI_SERDES_SIZE_prestat_t);
  uvwasi_prestat_t prestat{};
  const uvwasi_errno_t error =
      uvwasi_fd_prestat_get(&wasi.uvwasi, fd, &prestat);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_prestat_t>(
          memory, buf, [&](void *bytes) {
            uvwasi_serdes_write_prestat_t(bytes, 0, &prestat);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdPrestatDirName(WasiState &wasi, WasmMemory memory,
                                     uint32_t fd, uint32_t path_ptr,
                                     uint32_t path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  std::vector<char> path(path_len);
  const uvwasi_errno_t error = uvwasi_fd_prestat_dir_name(
      &wasi.uvwasi, fd, path.data(), path_len);
  if (error == UVWASI_ESUCCESS &&
      !memory.Write(path_ptr, path.data(), path.size()))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdPwrite(WasiState &wasi, WasmMemory memory, uint32_t fd,
                             uint32_t iovs_ptr, uint32_t iovs_len,
                             uint64_t offset, uint32_t nwritten_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, iovs_ptr,
                         static_cast<size_t>(iovs_len) *
                             UVWASI_SERDES_SIZE_ciovec_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, nwritten_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<uvwasi_ciovec_t> iovs(iovs_len);
  std::vector<std::vector<uint8_t>> buffers;
  if (!LoadInputIovecs(memory, iovs_ptr, iovs_len, &iovs, &buffers))
    return UVWASI_EFAULT;
  uvwasi_size_t nwritten = 0;
  const uvwasi_errno_t error = uvwasi_fd_pwrite(
      &wasi.uvwasi, fd, iovs.data(), iovs_len, offset, &nwritten);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
          memory, nwritten_ptr, [&](void *bytes) {
            uvwasi_serdes_write_size_t(bytes, 0, nwritten);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdRead(WasiState &wasi, WasmMemory memory, uint32_t fd,
                           uint32_t iovs_ptr, uint32_t iovs_len,
                           uint32_t nread_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, iovs_ptr,
                         static_cast<size_t>(iovs_len) *
                             UVWASI_SERDES_SIZE_iovec_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, nread_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<uvwasi_iovec_t> iovs(iovs_len);
  std::vector<std::vector<uint8_t>> buffers;
  std::vector<uint32_t> offsets;
  if (!LoadOutputIovecs(memory, iovs_ptr, iovs_len, &iovs, &buffers, &offsets))
    return UVWASI_EFAULT;
  uvwasi_size_t nread = 0;
  const uvwasi_errno_t error =
      uvwasi_fd_read(&wasi.uvwasi, fd, iovs.data(), iovs_len, &nread);
  if (error == UVWASI_ESUCCESS &&
      (!CommitOutputIovecs(memory, buffers, offsets, nread) ||
       !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
           memory, nread_ptr, [&](void *bytes) {
             uvwasi_serdes_write_size_t(bytes, 0, nread);
           })))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdReaddir(WasiState &wasi, WasmMemory memory, uint32_t fd,
                              uint32_t buf_ptr, uint32_t buf_len,
                              uint64_t cookie, uint32_t bufused_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, buf_ptr, buf_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, bufused_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<uint8_t> buffer(buf_len);
  uvwasi_size_t used = 0;
  const uvwasi_errno_t error = uvwasi_fd_readdir(
      &wasi.uvwasi, fd, buffer.data(), buf_len, cookie, &used);
  if (error == UVWASI_ESUCCESS &&
      (!memory.Write(buf_ptr, buffer.data(), used) ||
       !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
           memory, bufused_ptr, [&](void *bytes) {
             uvwasi_serdes_write_size_t(bytes, 0, used);
           })))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdRenumber(WasiState &wasi, WasmMemory, uint32_t from,
                               uint32_t to) {
  return uvwasi_fd_renumber(&wasi.uvwasi, from, to);
}

uint32_t WasiState::FdSeek(WasiState &wasi, WasmMemory memory, uint32_t fd,
                           int64_t offset, uint32_t whence,
                           uint32_t newoffset_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, newoffset_ptr,
                         UVWASI_SERDES_SIZE_filesize_t);
  uvwasi_filesize_t newoffset = 0;
  const uvwasi_errno_t error =
      uvwasi_fd_seek(&wasi.uvwasi, fd, offset, whence, &newoffset);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_filesize_t>(
          memory, newoffset_ptr, [&](void *bytes) {
            uvwasi_serdes_write_filesize_t(bytes, 0, newoffset);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdSync(WasiState &wasi, WasmMemory, uint32_t fd) {
  return uvwasi_fd_sync(&wasi.uvwasi, fd);
}

uint32_t WasiState::FdTell(WasiState &wasi, WasmMemory memory, uint32_t fd,
                           uint32_t offset_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, offset_ptr,
                         UVWASI_SERDES_SIZE_filesize_t);
  uvwasi_filesize_t offset = 0;
  const uvwasi_errno_t error = uvwasi_fd_tell(&wasi.uvwasi, fd, &offset);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_filesize_t>(
          memory, offset_ptr, [&](void *bytes) {
            uvwasi_serdes_write_filesize_t(bytes, 0, offset);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::FdWrite(WasiState &wasi, WasmMemory memory, uint32_t fd,
                            uint32_t iovs_ptr, uint32_t iovs_len,
                            uint32_t nwritten_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, iovs_ptr,
                         static_cast<size_t>(iovs_len) *
                             UVWASI_SERDES_SIZE_ciovec_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, nwritten_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<std::array<uint8_t, UVWASI_SERDES_SIZE_ciovec_t>> descriptors(
      iovs_len);
  std::vector<std::vector<uint8_t>> buffers(iovs_len);
  std::vector<uvwasi_ciovec_t> iovs(iovs_len);
  for (uint32_t i = 0; i < iovs_len; ++i) {
    auto &descriptor = descriptors[i];
    if (!memory.Read(iovs_ptr + i * descriptor.size(), descriptor.data(),
                     descriptor.size()))
      return UVWASI_EFAULT;
    const uint32_t buffer_offset =
        static_cast<uint32_t>(descriptor[0]) |
        (static_cast<uint32_t>(descriptor[1]) << 8) |
        (static_cast<uint32_t>(descriptor[2]) << 16) |
        (static_cast<uint32_t>(descriptor[3]) << 24);
    const uint32_t buffer_length =
        static_cast<uint32_t>(descriptor[4]) |
        (static_cast<uint32_t>(descriptor[5]) << 8) |
        (static_cast<uint32_t>(descriptor[6]) << 16) |
        (static_cast<uint32_t>(descriptor[7]) << 24);
    if (!uvwasi_serdes_check_bounds(buffer_offset, memory.size, buffer_length))
      return UVWASI_EOVERFLOW;
    buffers[i].resize(buffer_length);
    if (!memory.Read(buffer_offset, buffers[i].data(), buffer_length))
      return UVWASI_EFAULT;
    iovs[i].buf = buffers[i].data();
    iovs[i].buf_len = buffer_length;
  }
  uvwasi_size_t nwritten = 0;
  const uvwasi_errno_t error =
      uvwasi_fd_write(&wasi.uvwasi, fd, iovs.data(), iovs_len, &nwritten);
  if (error == UVWASI_ESUCCESS) {
    std::array<uint8_t, UVWASI_SERDES_SIZE_size_t> encoded{};
    uvwasi_serdes_write_size_t(encoded.data(), 0, nwritten);
    if (!memory.Write(nwritten_ptr, encoded.data(), encoded.size()))
      return UVWASI_EFAULT;
  }
  return error;
}

uint32_t WasiState::PathCreateDirectory(WasiState &wasi, WasmMemory memory,
                                        uint32_t fd, uint32_t path_ptr,
                                        uint32_t path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  std::vector<char> path;
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  return uvwasi_path_create_directory(&wasi.uvwasi, fd, path.data(), path_len);
}

uint32_t WasiState::PathFilestatGet(WasiState &wasi, WasmMemory memory,
                                    uint32_t fd, uint32_t flags,
                                    uint32_t path_ptr, uint32_t path_len,
                                    uint32_t buf_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, buf_ptr, UVWASI_SERDES_SIZE_filestat_t);
  std::vector<char> path;
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  uvwasi_filestat_t stats{};
  const uvwasi_errno_t error = uvwasi_path_filestat_get(
      &wasi.uvwasi, fd, flags, path.data(), path_len, &stats);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_filestat_t>(
          memory, buf_ptr, [&](void *bytes) {
            uvwasi_serdes_write_filestat_t(bytes, 0, &stats);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::PathFilestatSetTimes(WasiState &wasi, WasmMemory memory,
                                         uint32_t fd, uint32_t flags,
                                         uint32_t path_ptr, uint32_t path_len,
                                         uint64_t atime, uint64_t mtime,
                                         uint32_t fst_flags) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  std::vector<char> path;
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  return uvwasi_path_filestat_set_times(&wasi.uvwasi, fd, flags, path.data(),
                                        path_len, atime, mtime, fst_flags);
}

uint32_t WasiState::PathLink(WasiState &wasi, WasmMemory memory,
                             uint32_t old_fd, uint32_t old_flags,
                             uint32_t old_path_ptr, uint32_t old_path_len,
                             uint32_t new_fd, uint32_t new_path_ptr,
                             uint32_t new_path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, old_path_ptr, old_path_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, new_path_ptr, new_path_len);
  std::vector<char> old_path;
  std::vector<char> new_path;
  if (!ReadChars(memory, old_path_ptr, old_path_len, &old_path) ||
      !ReadChars(memory, new_path_ptr, new_path_len, &new_path))
    return UVWASI_EFAULT;
  return uvwasi_path_link(&wasi.uvwasi, old_fd, old_flags, old_path.data(),
                          old_path_len, new_fd, new_path.data(), new_path_len);
}

uint32_t WasiState::PathOpen(WasiState &wasi, WasmMemory memory, uint32_t dirfd,
                             uint32_t dirflags, uint32_t path_ptr,
                             uint32_t path_len, uint32_t oflags,
                             uint64_t rights_base, uint64_t rights_inheriting,
                             uint32_t fdflags, uint32_t fd_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, fd_ptr, UVWASI_SERDES_SIZE_fd_t);
  std::vector<char> path;
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  uvwasi_fd_t fd = 0;
  const uvwasi_errno_t error = uvwasi_path_open(
      &wasi.uvwasi, dirfd, dirflags, path.data(), path_len,
      static_cast<uvwasi_oflags_t>(oflags), rights_base, rights_inheriting,
      static_cast<uvwasi_fdflags_t>(fdflags), &fd);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_fd_t>(
          memory, fd_ptr,
          [&](void *bytes) { uvwasi_serdes_write_size_t(bytes, 0, fd); }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::PathReadlink(WasiState &wasi, WasmMemory memory,
                                 uint32_t fd, uint32_t path_ptr,
                                 uint32_t path_len, uint32_t buf_ptr,
                                 uint32_t buf_len, uint32_t bufused_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, buf_ptr, buf_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, bufused_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<char> path;
  std::vector<char> buffer(buf_len);
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  uvwasi_size_t used = 0;
  const uvwasi_errno_t error = uvwasi_path_readlink(
      &wasi.uvwasi, fd, path.data(), path_len, buffer.data(), buf_len, &used);
  if (error == UVWASI_ESUCCESS &&
      (!memory.Write(buf_ptr, buffer.data(), used) ||
       !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
           memory, bufused_ptr, [&](void *bytes) {
             uvwasi_serdes_write_size_t(bytes, 0, used);
           })))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::PathRemoveDirectory(WasiState &wasi, WasmMemory memory,
                                        uint32_t fd, uint32_t path_ptr,
                                        uint32_t path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  std::vector<char> path;
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  return uvwasi_path_remove_directory(&wasi.uvwasi, fd, path.data(), path_len);
}

uint32_t WasiState::PathRename(WasiState &wasi, WasmMemory memory,
                               uint32_t old_fd, uint32_t old_path_ptr,
                               uint32_t old_path_len, uint32_t new_fd,
                               uint32_t new_path_ptr, uint32_t new_path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, old_path_ptr, old_path_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, new_path_ptr, new_path_len);
  std::vector<char> old_path;
  std::vector<char> new_path;
  if (!ReadChars(memory, old_path_ptr, old_path_len, &old_path) ||
      !ReadChars(memory, new_path_ptr, new_path_len, &new_path))
    return UVWASI_EFAULT;
  return uvwasi_path_rename(&wasi.uvwasi, old_fd, old_path.data(), old_path_len,
                            new_fd, new_path.data(), new_path_len);
}

uint32_t WasiState::PathSymlink(WasiState &wasi, WasmMemory memory,
                                uint32_t old_path_ptr, uint32_t old_path_len,
                                uint32_t fd, uint32_t new_path_ptr,
                                uint32_t new_path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, old_path_ptr, old_path_len);
  CHECK_BOUNDS_OR_RETURN(memory.size, new_path_ptr, new_path_len);
  std::vector<char> old_path;
  std::vector<char> new_path;
  if (!ReadChars(memory, old_path_ptr, old_path_len, &old_path) ||
      !ReadChars(memory, new_path_ptr, new_path_len, &new_path))
    return UVWASI_EFAULT;
  return uvwasi_path_symlink(&wasi.uvwasi, old_path.data(), old_path_len, fd,
                             new_path.data(), new_path_len);
}

uint32_t WasiState::PathUnlinkFile(WasiState &wasi, WasmMemory memory,
                                   uint32_t fd, uint32_t path_ptr,
                                   uint32_t path_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, path_ptr, path_len);
  std::vector<char> path;
  if (!ReadChars(memory, path_ptr, path_len, &path))
    return UVWASI_EFAULT;
  return uvwasi_path_unlink_file(&wasi.uvwasi, fd, path.data(), path_len);
}

uint32_t WasiState::PollOneoff(WasiState &wasi, WasmMemory memory,
                               uint32_t in_ptr, uint32_t out_ptr,
                               uint32_t subscriptions, uint32_t events_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, in_ptr,
                         static_cast<size_t>(subscriptions) *
                             UVWASI_SERDES_SIZE_subscription_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, out_ptr,
                         static_cast<size_t>(subscriptions) *
                             UVWASI_SERDES_SIZE_event_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, events_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<uvwasi_subscription_t> in(subscriptions);
  std::vector<uvwasi_event_t> out(subscriptions);
  uint32_t cursor = in_ptr;
  for (uint32_t i = 0; i < subscriptions; ++i) {
    if (!DeserializeFromMemory<UVWASI_SERDES_SIZE_subscription_t>(
            memory, cursor, [&](const void *bytes) {
              uvwasi_serdes_read_subscription_t(bytes, 0, &in[i]);
            }))
      return UVWASI_EFAULT;
    cursor += UVWASI_SERDES_SIZE_subscription_t;
  }
  uvwasi_size_t events = 0;
  const uvwasi_errno_t error = uvwasi_poll_oneoff(
      &wasi.uvwasi, in.data(), out.data(), subscriptions, &events);
  if (error == UVWASI_ESUCCESS) {
    if (!SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
            memory, events_ptr, [&](void *bytes) {
              uvwasi_serdes_write_size_t(bytes, 0, events);
            }))
      return UVWASI_EFAULT;
    cursor = out_ptr;
    for (uint32_t i = 0; i < events; ++i) {
      if (!SerializeToMemory<UVWASI_SERDES_SIZE_event_t>(
              memory, cursor, [&](void *bytes) {
                uvwasi_serdes_write_event_t(bytes, 0, &out[i]);
              }))
        return UVWASI_EFAULT;
      cursor += UVWASI_SERDES_SIZE_event_t;
    }
  }
  return error;
}

void WasiState::ProcExit(WasiState &wasi, WasmMemory, uint32_t code) {
  (void)uvwasi_proc_exit(&wasi.uvwasi, code);
}

uint32_t WasiState::ProcRaise(WasiState &wasi, WasmMemory, uint32_t signal) {
  return uvwasi_proc_raise(&wasi.uvwasi, signal);
}

uint32_t WasiState::RandomGet(WasiState &wasi, WasmMemory memory,
                              uint32_t buf_ptr, uint32_t buf_len) {
  CHECK_BOUNDS_OR_RETURN(memory.size, buf_ptr, buf_len);
  std::vector<uint8_t> buffer(buf_len);
  if (!ncrypto::CSPRNG(buffer.data(), buffer.size()))
    return UVWASI_EIO;
  if (!memory.Write(buf_ptr, buffer.data(), buffer.size()))
    return UVWASI_EFAULT;
  return UVWASI_ESUCCESS;
}

uint32_t WasiState::SchedYield(WasiState &wasi, WasmMemory) {
  return uvwasi_sched_yield(&wasi.uvwasi);
}

uint32_t WasiState::SockAccept(WasiState &wasi, WasmMemory memory,
                               uint32_t socket, uint32_t flags,
                               uint32_t fd_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, fd_ptr, UVWASI_SERDES_SIZE_fd_t);
  uvwasi_fd_t fd = 0;
  const uvwasi_errno_t error = uvwasi_sock_accept(
      &wasi.uvwasi, socket, static_cast<uvwasi_fdflags_t>(flags), &fd);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_fd_t>(
          memory, fd_ptr,
          [&](void *bytes) { uvwasi_serdes_write_size_t(bytes, 0, fd); }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::SockRecv(WasiState &wasi, WasmMemory memory,
                             uint32_t socket, uint32_t data_ptr,
                             uint32_t data_len, uint32_t flags,
                             uint32_t datalen_ptr, uint32_t flags_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, data_ptr,
                         static_cast<size_t>(data_len) *
                             UVWASI_SERDES_SIZE_iovec_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, datalen_ptr, UVWASI_SERDES_SIZE_size_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, flags_ptr, UVWASI_SERDES_SIZE_roflags_t);
  std::vector<uvwasi_iovec_t> data(data_len);
  std::vector<std::vector<uint8_t>> buffers;
  std::vector<uint32_t> offsets;
  if (!LoadOutputIovecs(memory, data_ptr, data_len, &data, &buffers, &offsets))
    return UVWASI_EFAULT;
  uvwasi_size_t read = 0;
  uvwasi_roflags_t out_flags = 0;
  const uvwasi_errno_t error = uvwasi_sock_recv(
      &wasi.uvwasi, socket, data.data(), data_len, flags, &read, &out_flags);
  if (error == UVWASI_ESUCCESS) {
    if (!CommitOutputIovecs(memory, buffers, offsets, read) ||
        !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
            memory, datalen_ptr, [&](void *bytes) {
              uvwasi_serdes_write_size_t(bytes, 0, read);
            }) ||
        !SerializeToMemory<UVWASI_SERDES_SIZE_roflags_t>(
            memory, flags_ptr, [&](void *bytes) {
              uvwasi_serdes_write_roflags_t(bytes, 0, out_flags);
            }))
      return UVWASI_EFAULT;
  }
  return error;
}

uint32_t WasiState::SockSend(WasiState &wasi, WasmMemory memory,
                             uint32_t socket, uint32_t data_ptr,
                             uint32_t data_len, uint32_t flags,
                             uint32_t datalen_ptr) {
  CHECK_BOUNDS_OR_RETURN(memory.size, data_ptr,
                         static_cast<size_t>(data_len) *
                             UVWASI_SERDES_SIZE_ciovec_t);
  CHECK_BOUNDS_OR_RETURN(memory.size, datalen_ptr, UVWASI_SERDES_SIZE_size_t);
  std::vector<uvwasi_ciovec_t> data(data_len);
  std::vector<std::vector<uint8_t>> buffers;
  if (!LoadInputIovecs(memory, data_ptr, data_len, &data, &buffers))
    return UVWASI_EFAULT;
  uvwasi_size_t written = 0;
  const uvwasi_errno_t error = uvwasi_sock_send(
      &wasi.uvwasi, socket, data.data(), data_len, flags, &written);
  if (error == UVWASI_ESUCCESS &&
      !SerializeToMemory<UVWASI_SERDES_SIZE_size_t>(
          memory, datalen_ptr, [&](void *bytes) {
            uvwasi_serdes_write_size_t(bytes, 0, written);
          }))
    return UVWASI_EFAULT;
  return error;
}

uint32_t WasiState::SockShutdown(WasiState &wasi, WasmMemory, uint32_t socket,
                                 uint32_t how) {
  return uvwasi_sock_shutdown(&wasi.uvwasi, socket, how);
}

#define WASI_METHOD(function, name)                                            \
  {name,                                                                       \
   nullptr,                                                                    \
   WasiCallback<&WasiState::function>::Invoke,                                 \
   nullptr,                                                                    \
   nullptr,                                                                    \
   nullptr,                                                                    \
   napi_default_jsproperty,                                                    \
   nullptr}

constexpr napi_property_descriptor kWasiMethods[] = {
    WASI_METHOD(ArgsGet, "args_get"),
    WASI_METHOD(ArgsSizesGet, "args_sizes_get"),
    WASI_METHOD(ClockResGet, "clock_res_get"),
    WASI_METHOD(ClockTimeGet, "clock_time_get"),
    WASI_METHOD(EnvironGet, "environ_get"),
    WASI_METHOD(EnvironSizesGet, "environ_sizes_get"),
    WASI_METHOD(FdAdvise, "fd_advise"),
    WASI_METHOD(FdAllocate, "fd_allocate"),
    WASI_METHOD(FdClose, "fd_close"),
    WASI_METHOD(FdDatasync, "fd_datasync"),
    WASI_METHOD(FdFdstatGet, "fd_fdstat_get"),
    WASI_METHOD(FdFdstatSetFlags, "fd_fdstat_set_flags"),
    WASI_METHOD(FdFdstatSetRights, "fd_fdstat_set_rights"),
    WASI_METHOD(FdFilestatGet, "fd_filestat_get"),
    WASI_METHOD(FdFilestatSetSize, "fd_filestat_set_size"),
    WASI_METHOD(FdFilestatSetTimes, "fd_filestat_set_times"),
    WASI_METHOD(FdPread, "fd_pread"),
    WASI_METHOD(FdPrestatGet, "fd_prestat_get"),
    WASI_METHOD(FdPrestatDirName, "fd_prestat_dir_name"),
    WASI_METHOD(FdPwrite, "fd_pwrite"),
    WASI_METHOD(FdRead, "fd_read"),
    WASI_METHOD(FdReaddir, "fd_readdir"),
    WASI_METHOD(FdRenumber, "fd_renumber"),
    WASI_METHOD(FdSeek, "fd_seek"),
    WASI_METHOD(FdSync, "fd_sync"),
    WASI_METHOD(FdTell, "fd_tell"),
    WASI_METHOD(FdWrite, "fd_write"),
    WASI_METHOD(PathCreateDirectory, "path_create_directory"),
    WASI_METHOD(PathFilestatGet, "path_filestat_get"),
    WASI_METHOD(PathFilestatSetTimes, "path_filestat_set_times"),
    WASI_METHOD(PathLink, "path_link"),
    WASI_METHOD(PathOpen, "path_open"),
    WASI_METHOD(PathReadlink, "path_readlink"),
    WASI_METHOD(PathRemoveDirectory, "path_remove_directory"),
    WASI_METHOD(PathRename, "path_rename"),
    WASI_METHOD(PathSymlink, "path_symlink"),
    WASI_METHOD(PathUnlinkFile, "path_unlink_file"),
    WASI_METHOD(PollOneoff, "poll_oneoff"),
    WASI_METHOD(ProcExit, "proc_exit"),
    WASI_METHOD(ProcRaise, "proc_raise"),
    WASI_METHOD(RandomGet, "random_get"),
    WASI_METHOD(SchedYield, "sched_yield"),
    WASI_METHOD(SockAccept, "sock_accept"),
    WASI_METHOD(SockRecv, "sock_recv"),
    WASI_METHOD(SockSend, "sock_send"),
    WASI_METHOD(SockShutdown, "sock_shutdown"),
    {"_setMemory", nullptr, SetMemoryCallback, nullptr, nullptr, nullptr,
     napi_default_jsproperty, nullptr},
};

#undef WASI_METHOD

} // namespace

napi_value InitWasi(napi_env env) {
  napi_value constructor = nullptr;
  if (napi_define_class(env, "WASI", NAPI_AUTO_LENGTH, WasiConstructor, nullptr,
                        sizeof(kWasiMethods) / sizeof(kWasiMethods[0]),
                        kWasiMethods, &constructor) != napi_ok ||
      constructor == nullptr) {
    return Undefined(env);
  }
  napi_value binding = nullptr;
  if (napi_create_object(env, &binding) != napi_ok || binding == nullptr ||
      napi_set_named_property(env, binding, "WASI", constructor) != napi_ok) {
    return Undefined(env);
  }
  return binding;
}

} // namespace internal_binding
