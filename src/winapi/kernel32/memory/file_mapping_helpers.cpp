// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_mapping_helpers.cpp
/// @brief Shared FileMappingManager helpers.

#include "file_mapping_internal.hpp"
#include "memory.hpp"

#include <charconv>
#include <unistd.h>

namespace vitreo::winapi {

usize host_mapping_page_size() {
    static const usize size = static_cast<usize>(::getpagesize());
    return size;
}

GuestAddr view_base_alignment() {
    const auto host = static_cast<GuestAddr>(host_mapping_page_size());
    return host > kWindowsAllocationGranularity ? host
                                                : kWindowsAllocationGranularity;
}

bool is_writable_protect(u32 protect) {
    const u32 page_protection = protect & 0xFFu;
    return (page_protection == PAGE_READWRITE) ||
           (page_protection == PAGE_EXECUTE_READWRITE);
}

bool is_copy_on_write_access(u32 desired_access) {
    return (desired_access & FILE_MAP_COPY) != 0 &&
           (desired_access & (FILE_MAP_WRITE | FILE_MAP_READ)) == 0;
}

/// Strip a leading "Local\" so "Local\X" and "X" are the same session-local
/// name (Learn BaseNamedObjects / CreateFileMappingW).
[[nodiscard]] std::string normalize_mapping_name(std::string name) {
    constexpr std::string_view kLocal = "Local\\";
    if (name.size() >= kLocal.size() &&
        name.compare(0, kLocal.size(), kLocal) == 0) {
        name.erase(0, kLocal.size());
    }
    return name;
}

bool valid_file_mapping_access(u32 access) {
    return (access & ~FILE_MAPPING_ACCESS_MASK) == 0;
}

std::string hex_encode(std::string_view value) {
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

std::optional<std::string> hex_decode(std::string_view value) {
    if ((value.size() & 1u) != 0) return std::nullopt;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
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

std::optional<u32> parse_u32(std::string_view value) {
    u32 parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace vitreo::winapi
