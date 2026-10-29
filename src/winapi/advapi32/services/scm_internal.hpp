// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file scm_internal.hpp
/// @brief Private surface shared by the scm_*.cpp translation units.
///
/// scm.cpp was split by concern (status file, service database, process
/// lifecycle, service side, and four groups of advapi32 exports). The helpers
/// that more than one of those files needs were file-local in an anonymous
/// namespace before the split. They live here as `inline` functions so every
/// call site still inlines them across the new translation-unit boundaries,
/// and so no new external symbol appears. Consumers open
/// `namespace vitreo::winapi` and write `using namespace scm_detail;` to get
/// the same lookup the anonymous namespace gave.
///
/// Private to advapi32's SCM implementation; not part of scm.hpp's surface.

#include "winapi/advapi32/services/scm.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace vitreo::winapi {

namespace scm_detail {

namespace fs = std::filesystem;

inline std::wstring to_wide(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

inline std::string to_narrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        out.push_back(ch < 0x80 ? static_cast<char>(ch) : '?');
    }
    return out;
}

inline std::string encode_utf16_hex(const std::wstring& value) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size() * 4);
    for (const wchar_t ch : value) {
        const u16 unit = static_cast<u16>(ch);
        encoded.push_back(digits[(unit >> 12) & 0xFu]);
        encoded.push_back(digits[(unit >> 8) & 0xFu]);
        encoded.push_back(digits[(unit >> 4) & 0xFu]);
        encoded.push_back(digits[unit & 0xFu]);
    }
    return encoded;
}

inline std::optional<std::wstring> decode_utf16_hex(std::string_view encoded) {
    if ((encoded.size() % 4u) != 0) return std::nullopt;
    const auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    };
    std::wstring decoded;
    decoded.reserve(encoded.size() / 4);
    for (usize offset = 0; offset < encoded.size(); offset += 4) {
        u16 unit = 0;
        for (usize nibble = 0; nibble < 4; ++nibble) {
            const int value = digit(encoded[offset + nibble]);
            if (value < 0) return std::nullopt;
            unit = static_cast<u16>((unit << 4) | static_cast<u16>(value));
        }
        decoded.push_back(static_cast<wchar_t>(unit));
    }
    return decoded;
}

inline std::wstring to_lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c + 32) : c;
    });
    return s;
}

/// Read a guest string, wide or ANSI. Returns nullopt for a NULL pointer so
/// callers can distinguish "omitted" from "empty", which several service APIs
/// treat differently.
inline std::optional<std::wstring> read_guest_string(Marshaler& m, GuestAddr ptr,
                                                     bool wide) {
    if (ptr == 0) return std::nullopt;
    if (wide) {
        auto r = m.read_wide_string(ptr);
        if (r.is_err()) return std::nullopt;
        return to_wide(r.value());
    }
    auto r = m.read_ansi_string(ptr, 32768);
    if (r.is_err()) return std::nullopt;
    return to_wide(r.value());
}

/// Write SERVICE_STATUS (7 DWORDs) at @p addr, optionally followed by the two
/// extra SERVICE_STATUS_PROCESS fields.
inline void write_service_status(Marshaler& m, GuestAddr addr,
                                 const ServiceStatus& s, bool with_process_info) {
    (void)m.write_u32(addr + 0,  s.service_type);
    (void)m.write_u32(addr + 4,  s.current_state);
    (void)m.write_u32(addr + 8,  s.controls_accepted);
    (void)m.write_u32(addr + 12, s.win32_exit_code);
    (void)m.write_u32(addr + 16, s.service_specific_exit_code);
    (void)m.write_u32(addr + 20, s.check_point);
    (void)m.write_u32(addr + 24, s.wait_hint);
    if (with_process_info) {
        (void)m.write_u32(addr + 28, s.process_id);
        (void)m.write_u32(addr + 32, 0);  // dwServiceFlags
    }
}

inline ServiceStatus read_service_status(Marshaler& m, GuestAddr addr) {
    ServiceStatus s;
    auto rd = [&m, addr](u32 offset) -> u32 {
        auto r = m.read_u32(addr + offset);
        return r.is_ok() ? r.value() : 0u;
    };
    s.service_type               = rd(0);
    s.current_state              = rd(4);
    s.controls_accepted          = rd(8);
    s.win32_exit_code            = rd(12);
    s.service_specific_exit_code = rd(16);
    s.check_point                = rd(20);
    s.wait_hint                  = rd(24);
    return s;
}

/// Resolve a service handle argument, reporting the Windows error the guest
/// expects. Returns nullopt (with `error` set and the caller expected to
/// return FALSE) for anything that is not a live service handle.
inline std::optional<std::wstring> resolve_service_arg(ScmManager& scm, u32 handle) {
    auto name = scm.handle_service_name(handle);
    if (!name) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    return name;
}

/// Sidecar next to the status file. StartService waits for this before
/// returning success so it can distinguish "process exists" from "the
/// service control dispatcher created the ServiceMain thread".
inline std::string dispatcher_handshake_path(const std::string& status_path) {
    if (status_path.size() >= 7 &&
        status_path.compare(status_path.size() - 7, 7, ".status") == 0) {
        return status_path.substr(0, status_path.size() - 7) + ".dispatcher";
    }
    return {};
}

inline bool write_dispatcher_handshake(const std::string& status_path) {
    const std::string path = dispatcher_handshake_path(status_path);
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    if (ec) return false;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(out);
}

inline bool dispatcher_handshake_exists(const std::string& status_path) {
    const std::string path = dispatcher_handshake_path(status_path);
    if (path.empty()) return false;
    std::error_code ec;
    return fs::exists(path, ec);
}

inline void remove_dispatcher_handshake(const std::string& status_path) {
    const std::string path = dispatcher_handshake_path(status_path);
    if (path.empty()) return;
    std::error_code ec;
    fs::remove(path, ec);
}

} // namespace scm_detail

// The advapi32 export table, in the groups create_advapi32_scm_thunks
// registers them. Each appends its entries to `exports` in the order the
// single function used to, so the table order is unchanged.
void register_scm_open_start_exports(ScmManager& scm, DllExports& exports);
void register_scm_status_control_exports(ScmManager& scm, DllExports& exports);
void register_scm_config_exports(ScmManager& scm, DllExports& exports);
void register_scm_service_side_exports(ScmManager& scm, DllExports& exports);

} // namespace vitreo::winapi
