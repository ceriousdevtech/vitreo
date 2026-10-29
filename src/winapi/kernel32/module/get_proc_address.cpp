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

/// @file get_proc_address.cpp
/// @brief ModuleManager::get_proc_address
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

// A successful lookup leaves the thread's last error untouched (measured on
// Windows 11: tools/oracle_probe/last_error_on_success_probe.expected).
GuestAddr ModuleManager::get_proc_address(GuestAddr module_base,
                                           std::string_view func_name) const {
    // Table lookups under the shared table lock; everything that can take the
    // loader lock (a forwarder resolution loads its target) runs after it is
    // released, on copies.
    std::string dll_key;
    bool image_backed = false;
    std::string forwarder;
    {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        const std::string* dll_name = name_for_base(module_base);
        if (!dll_name) {
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            return 0;
        }
        dll_key = *dll_name;

        auto it = modules_by_name_.find(dll_key);
        if (it != modules_by_name_.end() &&
            (it->second.is_pe_backed() || it->second.synthetic_image)) {
            image_backed = true;
            auto ex = it->second.exports_by_name.find(std::string(func_name));
            if (ex != it->second.exports_by_name.end()) {
                return ex->second;
            }

            auto fwd = it->second.forwarders_by_name.find(std::string(func_name));
            if (fwd != it->second.forwarders_by_name.end()) {
                forwarder = fwd->second;
            }
        }
    }

    if (image_backed) {
        if (!forwarder.empty()) {
            const GuestAddr target = resolve_forwarded_export(forwarder, 0);
            if (target != 0) {
                return target;
            }
        }

        // Compatibility fallback: some real Windows DLLs resolve missing
        // exports via API-set or companion thunk layers. If the mapped PE does
        // not have the symbol, try the thunk registry for this DLL name.
        std::string dll_with_ext = ModuleManager::thunk_registry_dll_key(dll_key);
        auto thunk = registry_.get_thunk_address(dll_with_ext, func_name);
        if (thunk.is_ok()) {
            return registry_.guest_entry(thunk.value());
        }

        set_last_error(error_codes::ERROR_PROC_NOT_FOUND);
        return 0;
    }

    // ThunkRegistry stores DLLs under the full "name.dll" / "name.drv" key.
    std::string dll_with_ext = ModuleManager::thunk_registry_dll_key(dll_key);

    auto result = registry_.get_thunk_address(dll_with_ext, func_name);
    if (result.is_ok()) {
        return registry_.guest_entry(result.value());
    }

    set_last_error(error_codes::ERROR_PROC_NOT_FOUND);
    return 0;
}


GuestAddr ModuleManager::resolve_forwarded_export(std::string_view forwarder,
                                                   unsigned depth) const {
    // Forwarder chains are legal (A forwards to B, which forwards to C), but
    // a malformed image can make one cyclic. Windows caps the walk; so do we.
    constexpr unsigned kMaxForwarderDepth = 8;
    // A chained forwarder re-enters through get_proc_address, so the depth the
    // caller passes only starts the walk; the running count has to live across
    // that re-entry. It is per-thread because module lookups are.
    static thread_local unsigned active_depth = 0;
    if (depth + active_depth >= kMaxForwarderDepth) {
        return 0;
    }
    ++active_depth;
    struct DepthGuard {
        unsigned& d;
        ~DepthGuard() { --d; }
    } depth_guard{active_depth};

    // "TargetDll.ExportName" / "TargetDll.#123". The DLL name carries no
    // extension, and API-set names ("api-ms-win-core-heap-l1-1-0") contain no
    // dots, so the final '.' is the separator.
    const auto dot = forwarder.rfind('.');
    if (dot == std::string_view::npos || dot == 0 ||
        dot + 1 >= forwarder.size()) {
        return 0;
    }
    const std::string_view target_dll = forwarder.substr(0, dot);
    const std::string_view target_sym = forwarder.substr(dot + 1);

    // Resolving a forwarder loads the target module, which is loader state.
    // GetProcAddress is a query from the guest's point of view but not from
    // the loader's; Windows has the same asymmetry (LdrpResolveProcedure-
    // AddressEx loads the forwarded-to module on demand).
    auto& self = const_cast<ModuleManager&>(*this);

    std::string dll_with_ext(target_dll);
    dll_with_ext += ".dll";

    GuestAddr target_base = get_module_handle(dll_with_ext);
    if (target_base == 0) {
        target_base = self.load_library(dll_with_ext);
    }
    if (target_base == 0) {
        return 0;
    }

    if (target_sym.front() == '#') {
        const std::string_view digits = target_sym.substr(1);
        u32 ordinal = 0;
        for (const char c : digits) {
            if (c < '0' || c > '9') {
                return 0;
            }
            ordinal = ordinal * 10 + static_cast<u32>(c - '0');
            if (ordinal > 0xFFFFu) {
                return 0;
            }
        }
        if (digits.empty()) {
            return 0;
        }
        return get_proc_address_by_ordinal(target_base,
                                           static_cast<u16>(ordinal));
    }

    return get_proc_address(target_base, target_sym);
}

} // namespace vitreo::winapi
