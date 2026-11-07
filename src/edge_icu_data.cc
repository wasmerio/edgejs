#include "edge_icu_data.h"

#include <cstddef>
#include <mutex>
#include <string>

// ICU is built with U_DISABLE_RENAMING=1 (see cmake/EdgeICU.cmake), so consumers
// must match to avoid referencing versioned symbols (udata_setCommonData_78).
#define U_DISABLE_RENAMING 1
#include <unicode/udata.h>
#include <unicode/utypes.h>

#if defined(EDGE_ICU_DATA_EXTERNAL)
#include <unicode/putil.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>
#else
// The embedded ICU common-data blob. cmake/embed_binary.py emits it as a raw
// (already bz2-decompressed) byte array plus its length, linked via
// edge_icu_embedded_data on both the native and WASIX targets.
extern "C" {
extern const unsigned char ubi_icudt78l_dat[];
extern const std::size_t ubi_icudt78l_dat_len;
}
#endif

namespace {
std::once_flag g_icu_data_once;
bool g_icu_data_ok = false;
// A warning or error for the caller to print once; empty when ICU data was
// activated from the selected directory without any problem.
std::string g_icu_data_message;

#if defined(EDGE_ICU_DATA_EXTERNAL)
// Where packaged WASIX builds mount ICU's individual data items.
constexpr const char kDefaultIcuDataDir[] = "/usr/share/edge-icu";

struct IcuDataDirChoice {
  std::string dir;
  std::string source;  // What selected it, for diagnostics.
};

// --icu-data-dir, then EDGE_ICU_DATA (Edge's own override, so runners can pin
// the data regardless of a guest's Node settings), then NODE_ICU_DATA (Node's
// variable), then ICU_DATA, then the package mount.
IcuDataDirChoice ChooseIcuDataDirectory(const char* cli_icu_data_dir) {
  if (cli_icu_data_dir != nullptr && *cli_icu_data_dir != '\0') {
    return {cli_icu_data_dir, "--icu-data-dir"};
  }
  for (const char* name : {"EDGE_ICU_DATA", "NODE_ICU_DATA", "ICU_DATA"}) {
    const char* value = std::getenv(name);
    if (value != nullptr && *value != '\0') return {value, name};
  }
  return {kDefaultIcuDataDir, "default"};
}

// ICU resolves the directory on every lazy load, so pin it to an absolute
// path that a later process.chdir() cannot change.
std::filesystem::path AbsoluteDirectory(const std::string& dir) {
  std::error_code ec;
  std::filesystem::path out = std::filesystem::absolute(std::filesystem::path(dir), ec);
  if (ec) out = std::filesystem::path(dir);
  return out.lexically_normal();
}

// Every ICU data file, whether a single item (res_index.res, root.res, ...) or
// a whole common-data package (icudt78l.dat), starts with a MappedData header:
// a 16-bit header size followed by the magic bytes 0xDA 0x27.
bool HasIcuDataHeader(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  unsigned char header[4] = {};
  if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) return false;
  return header[2] == 0xDA && header[3] == 0x27;
}

// A usable directory holds either the per-item tree (<dir>/icudt78l/, as the
// package ships it, with at least the bundle index and the root locale) or a
// Node-style common-data file (<dir>/icudt78l.dat). Checking the headers, not
// just the names, keeps an empty or foreign file from silently disabling Intl.
bool IsUsableIcuDataDirectory(const std::filesystem::path& dir) {
  const std::filesystem::path tree = dir / U_ICUDATA_NAME;
  return (HasIcuDataHeader(tree / "res_index.res") && HasIcuDataHeader(tree / "root.res")) ||
         HasIcuDataHeader(dir / (U_ICUDATA_NAME ".dat"));
}

// ICU_DATA and NODE_ICU_DATA may hold a ':'-separated list, which ICU searches
// in order. Returns the usable entries as an absolute list, or an empty string
// when none holds ICU data.
std::string UsableIcuDataPath(const std::string& value) {
  std::string usable;
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(':', start);
    if (end == std::string::npos) end = value.size();
    const std::string entry = value.substr(start, end - start);
    if (!entry.empty()) {
      const std::filesystem::path dir = AbsoluteDirectory(entry);
      if (IsUsableIcuDataDirectory(dir)) {
        if (!usable.empty()) usable += ':';
        usable += dir.string();
      }
    }
    start = end + 1;
  }
  return usable;
}
#endif
}  // namespace

bool EdgeActivateIcuData(std::string* message_out, const char* cli_icu_data_dir) {
  std::call_once(g_icu_data_once, [cli_icu_data_dir]() {
#if defined(EDGE_ICU_DATA_EXTERNAL)
    // This build does not embed the ICU common data (EDGE_EMBED_ICU_DATA=OFF):
    // the package ships ICU's individual items (<dir>/icudt78l/coll/de.res,
    // *.cnv, ...) and ICU opens only the ones it needs, so unused locale data
    // never enters linear memory.
    const IcuDataDirChoice choice = ChooseIcuDataDirectory(cli_icu_data_dir);
    const std::string selected = UsableIcuDataPath(choice.dir);
    if (!selected.empty()) {
      u_setDataDirectory(selected.c_str());
      g_icu_data_ok = true;
      return;
    }
    constexpr const char kExpected[] = "no usable " U_ICUDATA_NAME "/ or " U_ICUDATA_NAME ".dat";
    constexpr const char kUnavailable[] =
        "Intl locale data and ICU-backed text encodings are unavailable; mount the package's "
        "/usr/share/edge-icu directory or set EDGE_ICU_DATA to a directory holding " U_ICUDATA_NAME
        "/ or " U_ICUDATA_NAME ".dat";
    if (choice.source == "default") {
      g_icu_data_ok = false;
      g_icu_data_message = std::string("ICU data not found: ") + kDefaultIcuDataDir + " holds " +
                           kExpected + ". " + kUnavailable;
      return;
    }
    // An explicit setting that does not point at ICU data (for example a
    // NODE_ICU_DATA meant for the host) must not disable Intl: fall back to
    // the data the package mounts.
    const std::string fallback = UsableIcuDataPath(kDefaultIcuDataDir);
    if (!fallback.empty()) {
      u_setDataDirectory(fallback.c_str());
      g_icu_data_ok = true;
      g_icu_data_message = "ignoring ICU data directory " + choice.dir + " (from " + choice.source +
                           "): it holds " + kExpected + "; using " + fallback + " instead";
      return;
    }
    g_icu_data_ok = false;
    g_icu_data_message = "ICU data not found: " + choice.dir + " (from " + choice.source +
                         ") holds " + kExpected + ", and the package mount " + kDefaultIcuDataDir +
                         " is missing too. " + kUnavailable;
#else
    (void)cli_icu_data_dir;
    // Silence the unused-symbol warning while keeping the length referenced so
    // the linker retains the blob; udata_setCommonData reads the length from the
    // ICU data header itself.
    (void)ubi_icudt78l_dat_len;
    UErrorCode status = U_ZERO_ERROR;
    udata_setCommonData(reinterpret_cast<const void*>(ubi_icudt78l_dat), &status);
    if (U_FAILURE(status)) {
      g_icu_data_ok = false;
      g_icu_data_message =
          std::string("Failed to activate embedded ICU data: ") + u_errorName(status);
      return;
    }
    g_icu_data_ok = true;
#endif
  });

  if (message_out != nullptr) {
    *message_out = g_icu_data_message;
  }
  return g_icu_data_ok;
}
