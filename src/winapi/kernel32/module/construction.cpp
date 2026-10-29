// SPDX-License-Identifier: LGPL-2.1-only

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
#include "core/pe/loader/teb_setup.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "core/memory/x64_high_arena.hpp"
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

/// @file construction.cpp
/// @brief Construction
///
/// Closed cut from module.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Construction
// =============================================================================

ModuleManager::ModuleManager(ThunkRegistry& registry)
    : registry_(registry) {}

void ModuleManager::attach_dynamic_loader(GuestMemory& memory, capsule::Vfs& vfs,
                                          bool is_64bit_process) {
    m_memory = &memory;
    m_vfs = &vfs;
    m_is_64bit_process = is_64bit_process;
    m_pe_loader = std::make_unique<pe::PeLoader>(memory, registry_);
    // The x64 defaults are host-probed: on a host whose own dylibs occupy the
    // preferred 0x7FFA window (x86_64 macOS), the whole three-arena block
    // moves down together. See core/memory/x64_high_arena.hpp.
    m_dynamic_base_start = is_64bit_process
        ? ::vitreo::memory::x64_dynamic_base()
        : 0x18000000ULL;
    m_synthetic_module_arena = is_64bit_process
        ? ::vitreo::memory::x64_synthetic_arena_base()
        : SYNTHETIC_MODULE_BASE_X86;
    const GuestAddr ceiling = memory.maximum_user_address();
    if (is_64bit_process && ceiling != 0 &&
        ceiling < 0x00007FFF'FFFEFFFFULL) {
        // Keep the established layout relative to the top of user space:
        // dynamic DLLs at -24 GiB, thunks at -20 GiB, synthetic modules at
        // -16 GiB. Linear64's smallest window is 512 GiB.
        constexpr GuestAddr kDynamicHeadroom = 0x00000006'00000000ULL;
        constexpr GuestAddr kSyntheticHeadroom = 0x00000004'00000000ULL;
        const GuestAddr user_end = ceiling + 1;
        m_dynamic_base_start =
            (user_end - kDynamicHeadroom) & ~GuestAddr{0xFFFF};
        m_synthetic_module_arena =
            (user_end - kSyntheticHeadroom) & ~GuestAddr{0xFFFF};
    }
    m_next_dynamic_base = m_dynamic_base_start;
}

bool ModuleManager::publish_loader_entry(const ModuleInfo& info) {
    if (m_main_peb == 0) {
        // setup_teb() snapshots modules already registered during process
        // construction. Runtime loads only need incremental publication after
        // run.cpp hands this manager the initialized process PEB.
        return true;
    }
    if (m_memory == nullptr) return false;
    pe::TebModuleInfo entry{info.base_address, info.path};
    entry.entry_point = info.entry_point;
    // Synthetic modules have no PE image, but they do own their slot; a zero
    // SizeOfImage hides them from every address-to-module lookup.
    entry.size_of_image = info.image_end > info.base_address
        ? info.image_end - info.base_address
        : SYNTHETIC_MODULE_SLOT_SIZE;
    return pe::append_peb_loader_module(
        *m_memory, m_main_peb, m_is_64bit_process, entry);
}

void ModuleManager::unpublish_loader_entry(const ModuleInfo& info) {
    if (m_main_peb == 0 || m_memory == nullptr) return;
    (void)pe::remove_peb_loader_module(
        *m_memory, m_main_peb, m_is_64bit_process, info.base_address);
}

void ModuleManager::set_path_variable_provider(
    std::function<std::string()> provider) {
    m_path_variable_provider = std::move(provider);
}

void ModuleManager::set_current_directory(std::string_view directory) {
    m_current_directory = directory.empty() ? "C:\\" : std::string(directory);
}

void ModuleManager::set_crt_data_resolver(
        std::function<u64(std::string_view)> resolver) {
    m_crt_data_resolver = std::move(resolver);
}

u64 ModuleManager::resolve_crt_data(std::string_view name) const {
    return m_crt_data_resolver ? m_crt_data_resolver(name) : 0;
}

void ModuleManager::set_dll_directory(std::optional<std::string> directory) {
    m_dll_directory = std::move(directory);
}

void ModuleManager::set_default_dll_directories(u32 flags) {
    m_default_dll_directory_flags = flags;
}

u64 ModuleManager::add_dll_directory(std::string directory) {
    if (directory.empty()) {
        return 0;
    }
    std::unique_lock<HostRwLock> tables(m_tables_mutex);
    const u64 cookie = m_next_dll_directory_cookie++;
    m_user_dll_directories.emplace(cookie, std::move(directory));
    return cookie;
}

bool ModuleManager::remove_dll_directory(u64 cookie) {
    std::unique_lock<HostRwLock> tables(m_tables_mutex);
    return m_user_dll_directories.erase(cookie) != 0;
}

} // namespace vitreo::winapi
