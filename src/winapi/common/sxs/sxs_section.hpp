// SPDX-License-Identifier: LGPL-2.1-only
/// @file sxs_section.hpp
/// @brief The activation-context DLL redirection section that
/// FindActCtxSectionString returns (ACTCTX_SECTION_KEYED_DATA lpSectionBase /
/// lpData).
///
/// Layout measured on Windows 11 26100 for x86 and x64 (identical bytes):
/// tests/oracle_data/actctx_probe.{x86,x64}.expected, produced by
/// tools/oracle_probe/actctx_probe.cpp. Microsoft documents only the keyed
/// data structure around it. The element pseudo-key is
/// RtlHashUnicodeString(HASH_STRING_ALGORITHM_X65599) of the upper-cased
/// name, which the measured keys match.
///
///   0x00 string section header (0x2c bytes)
///        'SsHd', header size 0x2c, format 1, data format 1, flags 3,
///        element count, element list offset 0x2c, hash algorithm 1 (X65599),
///        search structure 0, user data offset 0, user data size 0
///   0x2c element list, sorted by pseudo-key: 6 x u32 each
///        {pseudo key, key offset, key length in bytes, data offset,
///         data length, assembly roster index}
///   then per element: the key (UTF-16 as the manifest spells it, NUL,
///        padded to 4) followed by its 20-byte data entry
///        {size 0x14, flags 2 (path omits assembly root), total path length,
///         path segment count, path segment offset} = all zero beyond flags.

#pragma once

#include "winapi/common/sxs/sxs_context.hpp"

#include <map>
#include <string>
#include <vector>

namespace vitreo::winapi::sxs {

struct SectionEntry {
    u32 data_offset = 0;
    u32 data_length = 0;
    u32 roster_index = 0;   ///< 1-based: 1 is the root manifest
};

struct DllRedirectionSection {
    std::vector<u8> bytes;
    std::map<std::string, SectionEntry> entries;   ///< key: lower-cased file name
};

/// X65599 hash of the upper-cased UTF-8 name, as UTF-16 code units.
[[nodiscard]] u32 pseudo_key(std::string_view name);

[[nodiscard]] DllRedirectionSection build_dll_redirection_section(const ContextData& context);

}  // namespace vitreo::winapi::sxs
