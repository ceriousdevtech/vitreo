/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win32_full_path.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

#include <vitreo/diagnostics.hpp>

/// @file get_module_handle.cpp
/// @brief ModuleManager::get_module_handle
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

GuestAddr ModuleManager::get_module_handle(std::string_view name) const {
    // NULL / empty name → main executable
    if (name.empty()) {
        return main_module_base_;
    }

    auto alive = [](const ModuleInfo& info) {
        return info.state == ModuleInfo::State::Loaded && info.ref_count > 0;
    };

    // Every branch below only walks the tables and pure name helpers (no
    // nested ModuleManager call), so one shared hold covers the lookup while
    // another thread's LoadLibrary / FreeLibrary inserts or erases.
    std::shared_lock<HostRwLock> tables(m_tables_mutex);

    // Learn GetModuleHandle: a path string matches only a module loaded from
    // that path. Basename-only lookup must not satisfy C:\nowhere\kernel32.dll
    // (loader_contract_probe.expected → null err=7e).
    if (looks_like_path(name)) {
        // Full-path identity, as LoadLibrary (loader_path_canon_probe).
        std::string full_name(name);
        if (is_absolute_windows_path(name)) {
            if (auto full = capsule::win32_full_path(name, m_current_directory)) full_name = std::move(full->path);
        }
        const std::string want = normalize_path_key(full_name);
        for (const auto& [loaded_key, info] : modules_by_name_) {
            (void)loaded_key;
            if (!alive(info) || info.path.empty()) continue;
            if (normalize_path_key(info.path) == want) {
                return info.base_address;
            }
        }
        return 0;
    }

    std::string key = normalize_name(name);

    auto it = modules_by_name_.find(key);
    if (it != modules_by_name_.end() && alive(it->second)) {
        return it->second.base_address;
    }

    // Fallback: if the module was loaded via explicit path, it may be keyed by
    // normalized path rather than base name.  Windows' GetModuleHandle matches
    // by module name, so scan the loaded set for a matching short name.
    for (const auto& [loaded_key, info] : modules_by_name_) {
        (void)loaded_key;
        if (alive(info) && normalized_name_equals(info.name, key)) {
            return info.base_address;
        }
    }

    // API set schema names are not registered under their api-ms-* key when
    // LoadLibrary resolves them to a host DLL. Match Windows: GetModuleHandle
    // of an API set returns the already-loaded host module (e.g. advapi32)
    // when that host is present, otherwise NULL.
    auto resolved = pe::resolve_api_set(name);
    if (resolved != name) {
        const std::string resolved_key = normalize_name(resolved);
        auto rit = modules_by_name_.find(resolved_key);
        if (rit != modules_by_name_.end() && alive(rit->second)) {
            return rit->second.base_address;
        }
        for (const auto& [loaded_key, info] : modules_by_name_) {
            (void)loaded_key;
            if (alive(info) && normalized_name_equals(info.name, resolved_key)) {
                return info.base_address;
            }
        }
    }
    return 0;
}

} // namespace vitreo::winapi
