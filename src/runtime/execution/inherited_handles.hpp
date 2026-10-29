#pragma once

#include "winapi/kernel32/handle.hpp"

#include <array>
#include <charconv>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vitreo::runtime {

// Every inheritance transport starts with <kind>,<decimal guest handle>,...
// Reserve those values before any child manager allocates a local handle.
// This does not import an object: its owning manager still validates/imports
// the complete record and publishes the handle only after successful import.
inline std::optional<std::vector<winapi::Handle>> inherited_handle_values(
    std::string_view pipes, std::string_view sync, std::string_view mappings) {
    std::vector<winapi::Handle> values;
    std::unordered_set<winapi::Handle> seen;
    const std::array streams{pipes, sync, mappings};
    const std::array<std::string_view, 3> kinds{"FNA", "IEP", "M"};
    for (usize family = 0; family < streams.size(); ++family) {
        auto records = streams[family];
        while (!records.empty()) {
            const auto end = records.find(';');
            const auto record = records.substr(0, end);
            const auto comma = record.find(',', 2);
            if (record.size() < 4 || record[1] != ',' ||
                kinds[family].find(record[0]) == std::string_view::npos ||
                comma == std::string_view::npos) return std::nullopt;
            winapi::Handle value{};
            const auto digits = record.substr(2, comma - 2);
            const auto parsed = std::from_chars(
                digits.data(), digits.data() + digits.size(), value);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != digits.data() + digits.size() ||
                value == winapi::NULL_HANDLE || value == winapi::INVALID_HANDLE ||
                !seen.insert(value).second) return std::nullopt;
            values.push_back(value);
            if (end == std::string_view::npos) break;
            records.remove_prefix(end + 1);
            if (records.empty()) return std::nullopt;
        }
    }
    return values;
}

} // namespace vitreo::runtime
