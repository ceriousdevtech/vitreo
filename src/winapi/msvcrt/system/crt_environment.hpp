// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_environment.hpp
/// @brief Guest-visible CRT environment access

#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <map>
#include <vector>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace vitreo {

class GuestMemory;

namespace winapi {

class CrtMemoryManager;
class EnvironmentManager;

/// Bridges the CRT environment functions to the process environment owned by
/// kernel32 while retaining CRT-owned guest buffers for pointer-returning APIs.
class CrtEnvironmentManager {
public:
    CrtEnvironmentManager(EnvironmentManager& environment,
                          GuestMemory& memory,
                          CrtMemoryManager& memory_manager);

    CrtEnvironmentManager(const CrtEnvironmentManager&) = delete;
    CrtEnvironmentManager& operator=(const CrtEnvironmentManager&) = delete;

    [[nodiscard]] std::optional<std::string> value(
        std::string_view name) const;
    [[nodiscard]] GuestAddr getenv_ansi(std::string_view name);
    [[nodiscard]] GuestAddr getenv_wide(std::string_view name);
    void set_errno(u32 value);

    /// The CRT heap / errno / invalid-parameter owner. `_dupenv_s` hands the
    /// guest a buffer it must release with the CRT's `free`, so the copy has
    /// to come from this manager's `crt_malloc`.
    [[nodiscard]] CrtMemoryManager& memory_manager() noexcept {
        return m_memory_manager;
    }

    /// A variable's value as the narrow CRT functions return it: encoded in
    /// the ANSI code page (Learn getenv), without a terminator.
    [[nodiscard]] static std::vector<u8> ansi_value_bytes(
        std::string_view utf8_value);

    /// Set or remove a variable, mirroring `_putenv("NAME=VALUE")`.
    /// An empty value removes the variable, as Windows specifies.
    /// @return true on success
    bool put_variable(std::string_view name, std::string_view value,
                      bool remove);

    /// Address of the `char** _environ` variable. The array it points at is
    /// rebuilt whenever the environment changed since the last call, so a
    /// guest that caches `_environ` across a _putenv sees the same staleness
    /// Windows exposes (the variable is re-read, the old array is not freed).
    /// @param is_64bit Guest pointer width for the array elements.
    [[nodiscard]] GuestAddr environ_variable_addr(bool is_64bit);

    /// The NULL-terminated `char*` environment ARRAY itself (not the variable
    /// cell). _get_initial_narrow_environment hands this straight to the CRT.
    [[nodiscard]] GuestAddr environ_array_addr(bool is_64bit);

    /// The NULL-terminated `wchar_t*` environment array, built as UTF-16
    /// "NAME=VALUE" strings. _get_initial_wide_environment returns this, and a
    /// wide-char program (pwsh.exe, any wmain) reads its whole environment
    /// through it.
    [[nodiscard]] GuestAddr wenviron_array_addr(bool is_64bit);
    [[nodiscard]] GuestAddr wenviron_variable_addr(bool is_64bit,
                                                  bool initialize = false);

    /// Build a NULL-terminated `wchar_t*` table for `args` and return its
    /// address. Used for __wargv; the CRT arena never reuses allocations, so a
    /// caller holding an older table still reads valid strings.
    [[nodiscard]] GuestAddr build_wide_string_table(
        const std::vector<std::wstring>& args, bool is_64bit);

    /// Narrow counterpart: a NULL-terminated `char*` table of the byte
    /// strings in @p args, pointer width per bitness.
    [[nodiscard]] GuestAddr build_narrow_string_table(
        const std::vector<std::string>& args, bool is_64bit);

private:
    struct CachedValue {
        std::string value;
        GuestAddr address{0};
    };

    [[nodiscard]] GuestAddr cache_ansi(std::string_view name,
                                       std::string_view value);
    [[nodiscard]] GuestAddr cache_wide(std::string_view name,
                                       std::string_view value);

    EnvironmentManager& m_environment;
    GuestMemory& m_memory;
    CrtMemoryManager& m_memory_manager;
    std::mutex m_lock;
    std::map<std::string, CachedValue> m_ansi_cache;
    std::map<std::string, CachedValue> m_wide_cache;

    /// Guest address of the `char** _environ` cell (allocated on demand).
    GuestAddr m_environ_variable{0};
    /// Snapshot the currently published array was built from, so the array is
    /// only rebuilt when the environment actually changed.
    std::vector<std::pair<std::string, std::string>> m_environ_snapshot;
    bool m_environ_built{false};
    GuestAddr m_wenviron_variable{0};
    std::vector<std::pair<std::string, std::string>> m_wenviron_snapshot;
    bool m_wenviron_built{false};
    bool m_wenviron_is_64bit{false};
};

/// Register getenv-family exports shared by msvcrt.dll and UCRT aliases.
/// @param vfs Backs _searchenv/_wsearchenv, which must actually look for the
///            file along the named variable's directory list.
DllExports create_crt_environment_thunks(CrtEnvironmentManager& mgr,
                                         capsule::Vfs& vfs);

} // namespace winapi
} // namespace vitreo
