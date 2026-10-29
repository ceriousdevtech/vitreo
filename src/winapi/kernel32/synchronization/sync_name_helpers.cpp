// SPDX-License-Identifier: LGPL-2.1-only

/// @file sync_name_helpers.cpp
/// @brief Named-sync transfer encoding helpers shared across SyncManager TUs.

#include "sync_internal.hpp"
#include "winapi/kernel32/named_object_directory.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <unistd.h>

namespace vitreo::winapi {

std::string sync_object_key(const std::string& name) {
    std::string key = normalize_named_object_name(name);
    return key.empty() ? name : key;
}

std::string make_sync_transfer_name(char kind) {
    static std::atomic<u64> next_id{1};
    return "VitreoTransferredSync_" + std::string(1, kind) + "_" +
           std::to_string(::getpid()) + "_" +
           std::to_string(next_id.fetch_add(1, std::memory_order_relaxed));
}

std::string hex_encode_sync_name(std::string_view value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        encoded.push_back(digits[byte >> 4]);
        encoded.push_back(digits[byte & 0x0f]);
    }
    return encoded;
}

std::optional<std::string> hex_decode_sync_name(std::string_view value) {
    if ((value.size() & 1u) != 0) return std::nullopt;
    auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        return -1;
    };
    std::string decoded;
    decoded.reserve(value.size() / 2);
    for (usize index = 0; index < value.size(); index += 2) {
        const int high = nibble(value[index]);
        const int low = nibble(value[index + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

std::optional<u32> parse_sync_u32(std::string_view value) {
    u32 parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::string format_parent_chain(
    const std::array<GuestAddr, 4>& parents, u8 parent_count) {
    std::string formatted{"["};
    const usize count = std::min<usize>(parent_count, parents.size());
    for (usize index = 0; index < count; ++index) {
        char address[24]{};
        std::snprintf(
            address, sizeof(address), "0x%llx",
            static_cast<unsigned long long>(parents[index]));
        if (index != 0) formatted.push_back(',');
        formatted += address;
    }
    formatted.push_back(']');
    return formatted;
}

} // namespace vitreo::winapi
