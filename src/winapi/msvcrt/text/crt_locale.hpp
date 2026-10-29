// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_locale.hpp
/// @brief UCRT locale state exposed to MSVC runtime libraries

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <mutex>

namespace vitreo {

class GuestMemory;

namespace winapi {

class CrtMemoryManager;

/// Owns the guest-visible state for Vitreo's supported CRT "C" locale.
class CrtLocaleManager {
public:
    CrtLocaleManager(GuestMemory& memory, CrtMemoryManager& memory_manager);

    CrtLocaleManager(const CrtLocaleManager&) = delete;
    CrtLocaleManager& operator=(const CrtLocaleManager&) = delete;

    [[nodiscard]] bool valid() const noexcept { return m_valid; }
    [[nodiscard]] GuestAddr pctype_addr() const noexcept {
        return m_pctype_addr;
    }
    [[nodiscard]] GuestAddr locale_names_addr(bool is_64bit) const noexcept {
        return is_64bit ? m_locale_names64_addr : m_locale_names32_addr;
    }
    [[nodiscard]] GuestAddr locale_conventions_addr(bool is_64bit) const noexcept {
        return is_64bit ? m_lconv64_addr : m_lconv32_addr;
    }
    /// UCRT __initialize_lconv_for_unsigned_char (/J startup): set the eight
    /// char members of the "C" locale lconv to (char)UCHAR_MAX. Process-wide.
    [[nodiscard]] bool initialize_lconv_for_unsigned_char();
    [[nodiscard]] GuestAddr allocate_days(bool wide);
    [[nodiscard]] GuestAddr allocate_months(bool wide);

    [[nodiscard]] static constexpr u32 code_page() noexcept { return 0; }
    [[nodiscard]] static constexpr u32 collate_code_page() noexcept { return 0; }
    [[nodiscard]] static constexpr i32 multibyte_max() noexcept { return 1; }

    void lock() { m_locale_lock.lock(); }
    void unlock() { m_locale_lock.unlock(); }

private:
    [[nodiscard]] GuestAddr allocate_ascii_time_string(const char* value,
                                                       bool wide);

    GuestMemory& m_memory;
    CrtMemoryManager& m_memory_manager;
    std::recursive_mutex m_locale_lock;
    GuestAddr m_pctype_addr{0};
    GuestAddr m_locale_names32_addr{0};
    GuestAddr m_locale_names64_addr{0};
    GuestAddr m_lconv32_addr{0};
    GuestAddr m_lconv64_addr{0};
    bool m_valid{false};
};

/// Register locale exports shared by msvcrt.dll and UCRT API-set aliases.
DllExports create_crt_locale_thunks(CrtLocaleManager& mgr);

} // namespace winapi
} // namespace vitreo
