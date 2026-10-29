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

/// @file make_companion_resolver.cpp
/// @brief ModuleManager::make_companion_resolver
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

pe::ImportCallback ModuleManager::make_companion_resolver(std::string companion_dir,
                                                          bool* dep_failed) {
    // Mirror the resolver used by setup_imports() for the main executable so
    // that any module loaded through PeLoader can locate bundled sibling DLLs
    // (siblings loaded from the same directory)
    // when they are not part of the host thunk registry.
    //
    // Search order:
    //   1) modules already loaded by short name (cache hit)
    //   2) <companion_dir>\<dll_name>  (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)
    //   then Learn's standard (SafeDllSearchMode) order, as load_library uses
    //   for a bare name:
    //   3) <main_module_dir>\<dll_name> (application directory)
    //   4) the SetDllDirectory directory, when one is set
    //   5) C:\Windows\System32 (system), C:\Windows\System (16-bit system),
    //      C:\Windows (Windows directory)
    //   6) the current directory, unless SetDllDirectory("") removed it
    //   7) each directory in PATH
    //
    // Microsoft Learn, "Dynamic-link library search order": dependencies are
    // searched by module name even when their parent was loaded by full path.
    // Omitting the system directories makes an installed redistributable
    // invisible to dynamically loaded plug-ins; omitting PATH hides a
    // dependency a launcher puts on its child's PATH (a game's
    // UplinkSteamAuth.dll importing Steam.dll from the Steam directory).
    // Returns 0 to signal "not handled here" — PeLoader then falls back to
    // its thunk-registry path. Recursion is bounded because load_library()
    // returns the cached base on a second call.
    std::string main_dir;
    if (!main_module_path_.empty()) {
        main_dir = parent_windows_dir(main_module_path_);
    }

    return [this, companion_dir = std::move(companion_dir), main_dir = std::move(main_dir), dep_failed](
        std::string_view dll_name,
        std::string_view func_name,
        u16 ordinal) -> u64 {

        auto try_dir = [&](const std::string& dir) -> GuestAddr {
            if (dir.empty()) return 0;
            std::string full = dir + "\\" + std::string(dll_name);
            return load_library(full);
        };

        // If we already know this dependency previously failed to initialize,
        // try to resolve the export from the still-mapped PE before bailing.
        // Real installed DLLs keep their exports accessible even when DllMain
        // returns FALSE; only missing-export stubs are avoided this way.
        std::string short_key = normalize_name(dll_name);
        if (failed_modules_.count(short_key)) {
            GuestAddr failed_base = get_module_handle(dll_name);
            if (failed_base != 0) {
                if (!func_name.empty()) {
                    GuestAddr addr = get_proc_address(failed_base, func_name);
                    if (addr != 0) return addr;
                }
                if (ordinal != 0) {
                    GuestAddr addr = get_proc_address_by_ordinal(failed_base, ordinal);
                    if (addr != 0) return addr;
                }
            }
            if (dep_failed) *dep_failed = true;
            fprintf(stderr,
                    "[loader] companion-resolver: dependency \"%.*s\" previously "
                    "failed DllMain \u2014 failing import \"%.*s\"\n",
                    static_cast<int>(dll_name.size()), dll_name.data(),
                    static_cast<int>(func_name.size()), func_name.data());
            return 0;
        }

        GuestAddr mod_base = get_module_handle(dll_name);
        if (mod_base == 0) {
            // Mapped but not yet Loaded: either an import cycle back into a
            // module whose own imports are still being bound, or a module
            // whose initializer is running. Bind against its exports; do not
            // load it a second time (see import_graph.cpp).
            mod_base = in_progress_module_base(dll_name);
        }
        bool from_cache = (mod_base != 0);
        if (mod_base == 0) {
            mod_base = try_dir(companion_dir);
        }
        if (mod_base == 0 && !main_dir.empty() && main_dir != companion_dir) {
            mod_base = try_dir(main_dir);
        }
        if (mod_base == 0 && m_dll_directory.has_value() && !m_dll_directory->empty()) {
            mod_base = try_dir(*m_dll_directory);
        }
        if (mod_base == 0) {
            mod_base = try_dir("C:\\Windows\\System32");
        }
        if (mod_base == 0) {
            mod_base = try_dir("C:\\Windows\\System");
        }
        if (mod_base == 0) {
            mod_base = try_dir("C:\\Windows");
        }
        if (mod_base == 0 && (!m_dll_directory.has_value() || !m_dll_directory->empty())) {
            mod_base = try_dir(m_current_directory);
        }
        if (mod_base == 0 && m_path_variable_provider) {
            const std::string path_variable = m_path_variable_provider();
            std::string_view rest = path_variable;
            while (mod_base == 0 && !rest.empty()) {
                const auto semicolon = rest.find(';');
                std::string_view entry = rest.substr(0, semicolon);
                rest = semicolon == std::string_view::npos
                    ? std::string_view{} : rest.substr(semicolon + 1);
                if (entry.size() >= 2 && entry.front() == '"' && entry.back() == '"') {
                    entry = entry.substr(1, entry.size() - 2);
                }
                while (!entry.empty() && (entry.back() == '\\' || entry.back() == '/')) {
                    entry.remove_suffix(1);
                }
                mod_base = try_dir(std::string(entry));
            }
        }
        // If the load attempts failed AND the failure was due to DllMain
        // (recorded in failed_modules_), propagate the dependency-failed
        // signal so the parent load can be aborted instead of silently
        // stubbed.
        if (mod_base == 0 && failed_modules_.count(short_key)) {
            GuestAddr failed_base = get_module_handle(dll_name);
            if (failed_base != 0) {
                if (!func_name.empty()) {
                    GuestAddr addr = get_proc_address(failed_base, func_name);
                    if (addr != 0) return addr;
                }
                if (ordinal != 0) {
                    GuestAddr addr = get_proc_address_by_ordinal(failed_base, ordinal);
                    if (addr != 0) return addr;
                }
            }
            if (dep_failed) *dep_failed = true;
            fprintf(stderr,
                    "[loader] companion-resolver: dependency \"%.*s\" failed DllMain "
                    "during this load \u2014 failing import \"%.*s\"\n",
                    static_cast<int>(dll_name.size()), dll_name.data(),
                    static_cast<int>(func_name.size()), func_name.data());
            return 0;
        }
        if (mod_base == 0) {
            fprintf(stderr,
                    "[loader] companion-resolver: could not load \"%.*s\" "
                    "(companion_dir=\"%s\" main_dir=\"%s\") needed by import \"%.*s\" ord=%u\n",
                    static_cast<int>(dll_name.size()), dll_name.data(),
                    companion_dir.c_str(), main_dir.c_str(),
                    static_cast<int>(func_name.size()), func_name.data(),
                    static_cast<unsigned>(ordinal));
            return 0;
        }

        if (!func_name.empty()) {
            GuestAddr addr = get_proc_address(mod_base, func_name);
            if (addr != 0) return addr;
        }
        if (ordinal != 0) {
            GuestAddr addr = get_proc_address_by_ordinal(mod_base, ordinal);
            if (addr != 0) return addr;
        }
        fprintf(stderr,
                "[loader] companion-resolver: \"%.*s\" loaded at 0x%llx (cached=%d) "
                "but export \"%.*s\" ord=%u not found\n",
                static_cast<int>(dll_name.size()), dll_name.data(),
                static_cast<unsigned long long>(mod_base), from_cache ? 1 : 0,
                static_cast<int>(func_name.size()), func_name.data(),
                static_cast<unsigned>(ordinal));
        return 0;
    };
}

} // namespace vitreo::winapi
