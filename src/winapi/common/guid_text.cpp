// SPDX-License-Identifier: LGPL-2.1-only
/// @file guid_text.cpp
/// @brief Canonical text form of a Windows GUID.

#include "guid_text.hpp"

#include <cstdio>

namespace vitreo::winapi {

std::string format_guid_braced(const std::array<u8, 16>& guid) {
    const u32 data1 = static_cast<u32>(guid[0]) |
                      (static_cast<u32>(guid[1]) << 8) |
                      (static_cast<u32>(guid[2]) << 16) |
                      (static_cast<u32>(guid[3]) << 24);
    const u32 data2 = static_cast<u32>(guid[4]) | (static_cast<u32>(guid[5]) << 8);
    const u32 data3 = static_cast<u32>(guid[6]) | (static_cast<u32>(guid[7]) << 8);

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer),
                  "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
                  data1, data2, data3, guid[8], guid[9], guid[10], guid[11],
                  guid[12], guid[13], guid[14], guid[15]);
    return std::string(buffer);
}

} // namespace vitreo::winapi
