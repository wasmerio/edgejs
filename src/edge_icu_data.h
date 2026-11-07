#ifndef EDGE_ICU_DATA_H_
#define EDGE_ICU_DATA_H_

#include <string>

// Points ICU at its data. Without this, the linked ICU has only the empty
// stubdata image and every locale-aware lookup fails with
// U_MISSING_RESOURCE_ERROR.
//
// Embedded builds (EDGE_EMBED_ICU_DATA=ON, the default and every native build)
// register the common-data blob linked into the binary (ubi_icudt78l_dat,
// produced by cmake/embed_binary.py) via udata_setCommonData().
//
// External builds (EDGE_EMBED_ICU_DATA=OFF, the WASIX packages; compiled with
// EDGE_ICU_DATA_EXTERNAL) instead read ICU's data from a directory through
// u_setDataDirectory(). The directory is, in order: cli_icu_data_dir (the
// --icu-data-dir value, when non-null and non-empty), EDGE_ICU_DATA,
// NODE_ICU_DATA, ICU_DATA, or /usr/share/edge-icu (the package mount). It is
// made absolute and must hold either icudt78l/ (individual items, opened
// lazily; res_index.res and root.res must carry ICU data headers) or
// icudt78l.dat (a Node-style common-data file). Environment values may be a
// ':'-separated list, of which the usable entries are kept. An explicit
// setting with no usable entry falls back to the package mount with a
// warning, so a NODE_ICU_DATA meant for another runtime cannot disable Intl.
// cli_icu_data_dir is ignored by embedded builds.
//
// Runs at most once per process (ICU data is process-global), so the first
// call's cli_icu_data_dir wins. Safe to call from any thread and any number of
// times. Returns true when ICU data is active. *message_out (when non-null)
// receives the warning or error to print, or an empty string when there is
// nothing to report; the same message is returned on every call.
bool EdgeActivateIcuData(std::string* message_out, const char* cli_icu_data_dir = nullptr);

#endif  // EDGE_ICU_DATA_H_
