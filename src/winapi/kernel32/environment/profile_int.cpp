// SPDX-License-Identifier: LGPL-2.1-only

/// @file profile_int.cpp
/// @brief GetProfileInt and FindResourceEx / EnumResourceLanguages stubs.
///
/// CPU-Z Profile / Locale / Resource banner (win.ini GetProfileInt, not PrivateProfile).
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// CPU-Z: Profile / Locale / Resource
// =============================================================================

void Kernel32ExtManager::impl_GetProfileIntA(Marshaler& m) {
    std::string app_name;
    std::string key_name;
    if (auto result = m.read_ansi_string(m.arg_ptr(0)); result.is_ok()) {
        app_name = result.value();
    }
    if (auto result = m.read_ansi_string(m.arg_ptr(1)); result.is_ok()) {
        key_name = result.value();
    }
    const u32 default_val = static_cast<u32>(m.arg(2));
    VITREO_TRACE("[k32ext] GetProfileIntA('%s','%s', default=%u) -> %u\n",
                 app_name.c_str(), key_name.c_str(), default_val, default_val);
    m.set_return(default_val);
}

void Kernel32ExtManager::impl_GetProfileIntW(Marshaler& m) {
    (void)m.arg_ptr(0); // lpAppName
    (void)m.arg_ptr(1); // lpKeyName
    u32 default_val = m.arg(2);
    VITREO_TRACE("[k32ext] GetProfileIntW -> default %u\n", default_val);
    m.set_return(default_val);
}

void Kernel32ExtManager::impl_ConvertDefaultLocale(Marshaler& m) {
    u32 lcid = m.arg(0);
    VITREO_TRACE("[k32ext] ConvertDefaultLocale(0x%X) -> 0x%X\n", lcid, lcid);
    m.set_return(lcid); // return unchanged
}

void Kernel32ExtManager::impl_FindResourceExW(Marshaler& m) {
    // HRSRC FindResourceExW(HMODULE, LPCWSTR lpType, LPCWSTR lpName, WORD lang)
    // Argument order differs from FindResourceW (type before name). Language
    // is a real preference threaded into the resource walker — same policy as
    // FindResourceExA. wLanguage 0 (LANG_NEUTRAL) keeps the 0x0409 default.
    const GuestAddr hModule = m.arg_ptr(0);
    const GuestAddr lpType  = m.arg_ptr(1);
    const GuestAddr lpName  = m.arg_ptr(2);
    const u32 wLanguage = static_cast<u32>(m.arg(3));
    VITREO_TRACE("[k32ext] FindResourceExW(hModule=0x%llx, lang=0x%04X)\n",
                 static_cast<unsigned long long>(hModule), wLanguage);
    m.set_return(find_resource_wide(
        m, hModule, lpName, lpType,
        wLanguage != 0 ? static_cast<u16>(wLanguage) : static_cast<u16>(0x0409)));
}

void Kernel32ExtManager::impl_EnumResourceLanguagesA(Marshaler& m) {
    enum_resource_languages_impl(m, /*wide=*/false);
}

void Kernel32ExtManager::impl_EnumResourceLanguagesW(Marshaler& m) {
    enum_resource_languages_impl(m, /*wide=*/true);
}

} // namespace vitreo::winapi
