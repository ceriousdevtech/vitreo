// SPDX-License-Identifier: LGPL-2.1-only
// Apply native Windows export surfaces and ordinals to CRT DllExports tables.
#include "winapi/msvcrt/crt_ordinals.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace vitreo::winapi {
namespace {

void apply_table(
    DllExports& exports,
    const std::pair<const char*, std::uint16_t>* table,
    std::size_t table_len) {
    std::unordered_map<std::string_view, std::uint16_t> by_name;
    by_name.reserve(table_len * 2);
    for (std::size_t i = 0; i < table_len; ++i) {
        by_name.emplace(table[i].first, table[i].second);
    }
    // A shared CRT implementation supplies several DLLs, but GetProcAddress
    // must expose only names in this DLL's native export table. Keeping an
    // unknown name with ordinal zero still incorrectly exports it by name.
    std::erase_if(exports.entries, [&](const auto& entry) {
        return !entry.internal && !by_name.contains(entry.name);
    });
    for (auto& entry : exports.entries) {
        entry.ordinal = 0;
        const auto it = by_name.find(entry.name);
        if (it != by_name.end()) {
            entry.ordinal = it->second;
        }
    }
}

}  // namespace

void apply_msvcrt_windows_ordinals(DllExports& exports, bool guest_is_64bit) {
    if (!guest_is_64bit) {
#include "msvcrt_ordinals32.inc"
        apply_table(exports, kMsvcrtOrdinalEntries, std::size(kMsvcrtOrdinalEntries));
        return;
    }
#include "msvcrt_ordinals.inc"
    apply_table(
        exports, kMsvcrtOrdinalEntries,
        sizeof(kMsvcrtOrdinalEntries) / sizeof(kMsvcrtOrdinalEntries[0]));
}

void apply_ucrtbase_windows_ordinals(DllExports& exports, bool guest_is_64bit) {
    if (!guest_is_64bit) {
#include "ucrtbase_ordinals32.inc"
        apply_table(exports, kUcrtbaseOrdinalEntries, std::size(kUcrtbaseOrdinalEntries));
        return;
    }
#include "ucrtbase_ordinals.inc"
    apply_table(
        exports, kUcrtbaseOrdinalEntries,
        sizeof(kUcrtbaseOrdinalEntries) / sizeof(kUcrtbaseOrdinalEntries[0]));
}

}  // namespace vitreo::winapi
