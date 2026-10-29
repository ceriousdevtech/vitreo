// SPDX-License-Identifier: LGPL-2.1-only

/// @file resource.cpp
/// @brief Resource enumeration and loading APIs.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

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

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
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
// Resource
// =============================================================================

void Kernel32ExtManager::impl_FindResourceA(Marshaler& m) {
    // HRSRC FindResourceA(HMODULE hModule, LPCSTR lpName, LPCSTR lpType)
    m.set_return(find_resource_ansi(m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
}

void Kernel32ExtManager::impl_FindResourceExA(Marshaler& m) {
    // HRSRC FindResourceExA(HMODULE hModule, LPCSTR lpType, LPCSTR lpName,
    //                       WORD wLanguage)
    // Note the argument order differs from FindResourceA: type precedes name.
    // wLanguage is a real preference: modules can ship many localized copies of
    // the same resource id, and the walker's fallback order (exact -> primary
    // language -> neutral -> first entry) only lands on the right one when the
    // caller's LANGID is actually threaded through. wLanguage 0 means
    // LANG_NEUTRAL, which keeps the historical 0x0409 default.
    const GuestAddr hModule = m.arg_ptr(0);
    const GuestAddr lpType  = m.arg_ptr(1);
    const GuestAddr lpName  = m.arg_ptr(2);
    const u32 wLanguage = static_cast<u32>(m.arg(3));
    VITREO_TRACE("[k32ext] FindResourceExA(hModule=0x%llx, lang=0x%04X)\n",
                 static_cast<unsigned long long>(hModule), wLanguage);
    m.set_return(find_resource_ansi(
        m, hModule, lpName, lpType,
        wLanguage != 0 ? static_cast<u16>(wLanguage) : static_cast<u16>(0x0409)));
}

u32 Kernel32ExtManager::find_resource_ansi(Marshaler& m, GuestAddr hModule,
                                           GuestAddr lpName, GuestAddr lpType,
                                           u16 language) {
    // For MAKEINTRESOURCE values, high bits above 16 are zero (IS_INTRESOURCE).

    // Resolve the resource directory for the requested module. NULL or the exe
    // HINSTANCE means the main image; any other HMODULE is a loaded DLL that
    // wants its OWN resources, resolved through the loader by base address.
    GuestAddr module_base = m_rsrc_image_base;
    u32 rsrc_rva  = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;
    if (hModule != 0 && hModule != m_rsrc_image_base) {
        u32 rva = 0;
        u32 size = 0;
        GuestAddr untagged = hModule;
        if (m_module_manager &&
            m_module_manager->resource_directory_for_base(hModule, rva, size,
                                                         &untagged)) {
            module_base = untagged;
            rsrc_rva    = rva;
            rsrc_size   = size;
        } else {
            VITREO_TRACE(
                "[k32ext] FindResource: unknown/resourceless hModule=0x%llx\n",
                static_cast<unsigned long long>(hModule));
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            return 0;
        }
    }

    if (rsrc_rva == 0 || rsrc_size == 0) {
        VITREO_TRACE( "[k32ext] FindResourceA: no resource directory info\n");
        return 0;
    }

    // Either the type or the name may be an integer ID (IS_INTRESOURCE: high
    // 16 bits zero) or a pointer to a string, so all four combinations are
    // valid. Resource strings compare case-insensitively, so both are
    // upper-cased to UTF-16 for the directory walk.
    auto read_ansi_resource_string =
        [&m](GuestAddr value, std::u16string& out) -> bool {
        auto r = m.read_ansi_string(value);
        if (r.is_err()) return false;
        const std::string ansi = r.value();
        out.reserve(ansi.size());
        for (char c : ansi) {
            out.push_back(static_cast<char16_t>(
                static_cast<unsigned char>(c >= 'a' && c <= 'z' ? c - 32 : c)));
        }
        return true;
    };

    u16 type_id = 0;
    bool type_is_string = false;
    std::u16string type_str;
    if ((lpType >> 16) == 0) {
        type_id = static_cast<u16>(lpType);
    } else {
        if (!read_ansi_resource_string(lpType, type_str)) {
            VITREO_TRACE(
                "[k32ext] FindResourceA: failed to read string type at 0x%llx\n",
                static_cast<unsigned long long>(lpType));
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            return 0;
        }
        type_is_string = true;
    }

    u16 name_id = 0;
    bool name_is_string = false;
    std::u16string name_str;
    if ((lpName >> 16) == 0) {
        name_id = static_cast<u16>(lpName);
    } else {
        if (!read_ansi_resource_string(lpName, name_str)) {
            VITREO_TRACE(
                "[k32ext] FindResourceA: failed to read string name at 0x%llx\n",
                static_cast<unsigned long long>(lpName));
            set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
            return 0;
        }
        name_is_string = true;
    }

    windowing::ResourceEntry entry;
    if (type_is_string && name_is_string) {
        entry = windowing::find_resource_by_named_type_and_name(
            m_memory, module_base, rsrc_rva, rsrc_size,
            type_str.c_str(), static_cast<u16>(type_str.size()),
            name_str.c_str(), static_cast<u16>(name_str.size()));
    } else if (type_is_string) {
        entry = windowing::find_resource_by_named_type(
            m_memory, module_base, rsrc_rva, rsrc_size,
            type_str.c_str(), static_cast<u16>(type_str.size()), name_id);
    } else if (name_is_string) {
        entry = windowing::find_resource_by_string_name(
            m_memory, module_base, rsrc_rva, rsrc_size,
            type_id, name_str.c_str(), static_cast<u16>(name_str.size()));
    } else {
        entry = windowing::find_resource(m_memory, module_base, rsrc_rva, rsrc_size,
                                         type_id, name_id, language);
    }
    if (!entry.data || entry.size == 0) {
        VITREO_TRACE("[k32ext] FindResourceA: type=%u/%s name=%u/%s NOT FOUND\n",
                     type_id, type_is_string ? "<string>" : "-",
                     name_id, name_is_string ? "<string>" : "-");
        set_last_error(type_is_string ? error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND
                                      : error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }

    // Use the guest address computed directly by the resource walker
    GuestAddr guest_data_addr = entry.guest_addr;

    u32 handle = 0;
    {
        std::lock_guard<std::mutex> resources(m_resource_mutex);
        handle = m_next_hrsrc++;
        m_resource_handles[handle] = ResourceInfo{guest_data_addr, entry.size};
    }

    VITREO_TRACE( "[k32ext] FindResourceA: type=%u id=%u => hrsrc=0x%08X (addr=0x%08X, size=%u)\n",
                 type_id, name_id, handle, static_cast<unsigned>(guest_data_addr), entry.size);
    return handle;
}

u32 Kernel32ExtManager::find_resource_wide(Marshaler& m, GuestAddr hModule,
                                           GuestAddr lpName, GuestAddr lpType,
                                           u16 language) {
    GuestAddr module_base = m_rsrc_image_base;
    u32 rsrc_rva  = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;
    if (hModule != 0 && hModule != m_rsrc_image_base) {
        u32 module_rva = 0;
        u32 module_size = 0;
        GuestAddr untagged = hModule;
        if (m_module_manager &&
            m_module_manager->resource_directory_for_base(hModule, module_rva,
                                                         module_size, &untagged)) {
            module_base = untagged;
            rsrc_rva = module_rva;
            rsrc_size = module_size;
        } else {
            VITREO_TRACE(
                "[k32ext] FindResourceW: unknown/resourceless "
                "hModule=0x%llx\n",
                static_cast<unsigned long long>(hModule));
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            return 0;
        }
    }

    if (rsrc_rva == 0 || rsrc_size == 0) {
        VITREO_TRACE( "[k32ext] FindResourceW: no resource directory info\n");
        set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
        return 0;
    }

    // Both the type and the name may be integer IDs or UTF-16 strings.
    // Resource strings compare case-insensitively.
    auto read_wide_resource_string =
        [&m](GuestAddr value, std::u16string& out) -> bool {
        auto r = m.read_wide_string(value);
        if (r.is_err()) return false;
        const std::string utf8 = r.value();
        out.reserve(utf8.size());
        for (char c : utf8) {
            out.push_back(static_cast<char16_t>(
                static_cast<unsigned char>(c >= 'a' && c <= 'z' ? c - 32 : c)));
        }
        return true;
    };

    u16 type_id = 0;
    bool type_is_string = false;
    std::u16string type_str;
    if ((lpType >> 16) == 0) {
        type_id = static_cast<u16>(lpType);
    } else {
        if (!read_wide_resource_string(lpType, type_str)) {
            VITREO_TRACE(
                "[k32ext] FindResourceW: failed to read string type at 0x%llx\n",
                static_cast<unsigned long long>(lpType));
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            return 0;
        }
        type_is_string = true;
    }

    u16 name_id = 0;
    bool name_is_string = false;
    std::u16string name_str;
    if ((lpName >> 16) == 0) {
        name_id = static_cast<u16>(lpName);
    } else {
        if (!read_wide_resource_string(lpName, name_str)) {
            VITREO_TRACE(
                "[k32ext] FindResourceW: failed to read string name at 0x%llx\n",
                static_cast<unsigned long long>(lpName));
            set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
            return 0;
        }
        name_is_string = true;
    }

    windowing::ResourceEntry entry;
    if (type_is_string && name_is_string) {
        entry = windowing::find_resource_by_named_type_and_name(
            m_memory, module_base, rsrc_rva, rsrc_size,
            type_str.c_str(), static_cast<u16>(type_str.size()),
            name_str.c_str(), static_cast<u16>(name_str.size()));
    } else if (type_is_string) {
        entry = windowing::find_resource_by_named_type(
            m_memory, module_base, rsrc_rva, rsrc_size,
            type_str.c_str(), static_cast<u16>(type_str.size()), name_id);
    } else if (name_is_string) {
        entry = windowing::find_resource_by_string_name(
            m_memory, module_base, rsrc_rva, rsrc_size,
            type_id, name_str.c_str(), static_cast<u16>(name_str.size()));
    } else {
        entry = windowing::find_resource(m_memory, module_base, rsrc_rva, rsrc_size,
                                         type_id, name_id, language);
    }
    if ((!entry.data || entry.size == 0) && !type_is_string && !name_is_string &&
        type_id == windowing::RT::STRING) {
        // Compatibility: some apps pass a string-table entry id directly to
        // FindResource instead of the Win32 block id (16 strings per block).
        const u16 block_id = static_cast<u16>((name_id / 16u) + 1u);
        auto block_entry = windowing::find_resource(m_memory, module_base, rsrc_rva, rsrc_size,
                                                    type_id, block_id, language);
        if (block_entry.data && block_entry.size > 0) {
            VITREO_TRACE("[k32ext] FindResourceW: RT_STRING id=%u remapped to block=%u\n",
                         name_id, block_id);
            entry = block_entry;
        }
    }
    if (!entry.data || entry.size == 0) {
        VITREO_TRACE( "[k32ext] FindResourceW: type=%u id=%u NOT FOUND\n", type_id, name_id);
        set_last_error(type_is_string ? error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND
                                      : error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
        return 0;
    }

    GuestAddr guest_data_addr = entry.guest_addr;
    u32 handle = 0;
    {
        std::lock_guard<std::mutex> resources(m_resource_mutex);
        handle = m_next_hrsrc++;
        m_resource_handles[handle] = ResourceInfo{guest_data_addr, entry.size};
    }

    VITREO_TRACE( "[k32ext] FindResourceW: type=%u id=%u => hrsrc=0x%08X (addr=0x%08X, size=%u)\n",
                 type_id, name_id, handle, static_cast<unsigned>(guest_data_addr), entry.size);
    return handle;
}

void Kernel32ExtManager::impl_FindResourceW(Marshaler& m) {
    // HRSRC FindResourceW(HMODULE hModule, LPCWSTR lpName, LPCWSTR lpType)
    m.set_return(find_resource_wide(m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
}

void Kernel32ExtManager::impl_LoadStringW(Marshaler& m) {
    // Learn winuser.h / libloaderapi: int LoadStringW(HINSTANCE, UINT, LPWSTR, int)
    // Missing string → 0. A fail-thunk returning 0xFFFFFFFF is treated as a
    // huge character count and becomes std::bad_alloc.
    const GuestAddr instance = m.arg_ptr(0);
    const u32 id = static_cast<u32>(m.arg(1));
    const GuestAddr buf = m.arg_ptr(2);
    const u32 cch_max = static_cast<u32>(m.arg(3));
    if (buf == 0 || cch_max == 0) {
        m.set_return(0);
        return;
    }

    GuestAddr module_base = m_rsrc_image_base;
    u32 rsrc_rva = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;
    if (instance != 0 && instance != m_rsrc_image_base) {
        u32 module_rva = 0;
        u32 module_size = 0;
        GuestAddr untagged = instance;
        if (m_module_manager == nullptr ||
            !m_module_manager->resource_directory_for_base(
                instance, module_rva, module_size, &untagged)) {
            m.set_return(0);
            return;
        }
        module_base = untagged;
        rsrc_rva = module_rva;
        rsrc_size = module_size;
    }
    if (rsrc_rva == 0 || rsrc_size == 0) {
        m.set_return(0);
        return;
    }

    const u16 block_id = static_cast<u16>((id / 16u) + 1u);
    const u16 index_in_block = static_cast<u16>(id % 16u);
    const auto entry = windowing::find_resource(
        m_memory, module_base, rsrc_rva, rsrc_size,
        windowing::RT::STRING, block_id);
    if (!entry.data || entry.size < 2) {
        m.set_return(0);
        return;
    }

    const u8* p = entry.data;
    const u8* end = entry.data + entry.size;
    for (u16 i = 0; i < 16; ++i) {
        if (p + 2 > end) {
            m.set_return(0);
            return;
        }
        const u16 len = static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
        p += 2;
        const u32 bytes = static_cast<u32>(len) * 2u;
        if (p + bytes > end) {
            m.set_return(0);
            return;
        }
        if (i == index_in_block) {
            const u32 copy_len = std::min<u32>(static_cast<u32>(len), cch_max - 1);
            for (u32 j = 0; j < copy_len; ++j) {
                const u16 ch = static_cast<u16>(
                    p[j * 2] | (static_cast<u16>(p[j * 2 + 1]) << 8));
                (void)m_memory.write_u16(buf + j * 2, ch);
            }
            (void)m_memory.write_u16(buf + copy_len * 2, 0);
            m.set_return(copy_len);
            return;
        }
        p += bytes;
    }
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetPrivateProfileIntW(Marshaler& m) {
    // UINT GetPrivateProfileIntW(LPCWSTR lpAppName, LPCWSTR lpKeyName,
    //   INT nDefault, LPCWSTR lpFileName)
    GuestAddr app_ptr = m.arg_ptr(0);
    GuestAddr key_ptr = m.arg_ptr(1);
    u32 n_default = static_cast<u32>(m.arg(2));
    GuestAddr file_ptr = m.arg_ptr(3);
    std::string app_name, key_name, file_name;
    if (auto r = m.read_wide_string(app_ptr); r.is_ok()) app_name = r.value();
    if (auto r = m.read_wide_string(key_ptr); r.is_ok()) key_name = r.value();
    if (auto r = m.read_wide_string(file_ptr); r.is_ok()) file_name = r.value();
    const u32 value =
        lookup_profile_int(app_name, key_name, n_default, file_name);
    VITREO_TRACE("[k32ext] GetPrivateProfileIntW('%s','%s', default=%u, '%s') → %u\n",
                 app_name.c_str(), key_name.c_str(), n_default, file_name.c_str(),
                 value);
    m.set_return(value);
}

namespace {

/// Resource directory of @p hmodule: NULL or the main image's base is the
/// main image; anything else goes through the loader (tagged datafile
/// handles included).
bool module_resource_directory(GuestAddr hmodule, GuestAddr main_base,
                               u32 main_rva, u32 main_size,
                               const ModuleManager* modules, GuestAddr& base,
                               u32& rva, u32& size) {
    if (hmodule == 0 || hmodule == main_base) {
        base = main_base;
        rva = main_rva;
        size = main_size;
    } else {
        GuestAddr untagged = hmodule;
        if (modules == nullptr ||
            !modules->resource_directory_for_base(hmodule, rva, size, &untagged)) {
            return false;
        }
        base = untagged;
    }
    return rva != 0 && size != 0;
}

constexpr u16 kMessageTableName = 1;

} // namespace

bool Kernel32ExtManager::message_table_languages(GuestAddr hmodule,
                                                 std::vector<u16>& out) const {
    GuestAddr base = 0;
    u32 rva = 0;
    u32 size = 0;
    if (!module_resource_directory(hmodule, m_rsrc_image_base, m_rsrc_rva,
                                   m_rsrc_size, m_module_manager, base, rva,
                                   size)) {
        return false;
    }
    const auto langs = windowing::enumerate_resource_languages(
        m_memory, base, rva, size, windowing::RT::MESSAGETABLE, false, nullptr,
        0, kMessageTableName, false, nullptr, 0);
    if (langs.status != windowing::ResourceLanguageEnum::Status::Ok ||
        langs.languages.empty()) {
        return false;
    }
    out = langs.languages;
    return true;
}

bool Kernel32ExtManager::message_table_data(GuestAddr hmodule, u16 language,
                                            std::vector<u8>& out) const {
    GuestAddr base = 0;
    u32 rva = 0;
    u32 size = 0;
    if (!module_resource_directory(hmodule, m_rsrc_image_base, m_rsrc_rva,
                                   m_rsrc_size, m_module_manager, base, rva,
                                   size)) {
        return false;
    }
    // Only an exact language is asked for (it came from the enumeration), so
    // the walker's own fallback never substitutes another table.
    const auto entry = windowing::find_resource(
        m_memory, base, rva, size, windowing::RT::MESSAGETABLE,
        kMessageTableName, language);
    if (entry.guest_addr == 0 || entry.size == 0) return false;
    out.resize(entry.size);
    return m_memory
        .read_bytes(entry.guest_addr, reinterpret_cast<Byte*>(out.data()),
                    out.size())
        .is_ok();
}

void Kernel32ExtManager::impl_LoadResource(Marshaler& m) {
    // HGLOBAL LoadResource(HMODULE hModule, HRSRC hResInfo)
    // Win32 exposes the mapped resource address as HGLOBAL. LockResource is
    // effectively an identity operation on that value, and legacy installers
    // commonly dereference the LoadResource result directly.
    // GuestAddr must stay pointer-width: truncating a resource above 4 GiB
    // (a resource above 4 GiB, at ImageBase+RVA) yields a useless 32-bit pointer.
    (void)m.arg(0); // hModule (ignored)
    const u32 hrsrc = static_cast<u32>(m.arg(1));

    std::lock_guard<std::mutex> resources(m_resource_mutex);
    auto it = m_resource_handles.find(hrsrc);
    if (it == m_resource_handles.end()) {
        VITREO_TRACE( "[k32ext] LoadResource: unknown hrsrc=0x%08X => NULL\n", hrsrc);
        m.set_return(0);
        return;
    }

    const GuestAddr address = it->second.guest_addr;
    VITREO_TRACE(
        "[k32ext] LoadResource: hrsrc=0x%08X => 0x%llx\n",
        hrsrc, static_cast<unsigned long long>(address));
    m.set_return(address);
}

void Kernel32ExtManager::impl_LockResource(Marshaler& m) {
    // LPVOID LockResource(HGLOBAL hResData)
    // Returns a pointer to the first byte of the resource data.
    const GuestAddr hglobal = m.arg_ptr(0);

    std::lock_guard<std::mutex> resources(m_resource_mutex);
    // Synthetic HRSRC handles are small integers (IS_INTRESOURCE-shaped).
    if ((hglobal >> 16) == 0) {
        const u32 hrsrc = static_cast<u32>(hglobal);
        auto it = m_resource_handles.find(hrsrc);
        if (it != m_resource_handles.end()) {
            const GuestAddr address = it->second.guest_addr;
            VITREO_TRACE(
                "[k32ext] LockResource: legacy hrsrc=0x%08X => 0x%llx\n",
                hrsrc, static_cast<unsigned long long>(address));
            m.set_return(address);
            return;
        }
    }

    const auto loaded = std::find_if(
        m_resource_handles.begin(), m_resource_handles.end(),
        [hglobal](const auto& entry) {
            return entry.second.guest_addr == hglobal;
        });
    if (loaded != m_resource_handles.end()) {
        VITREO_TRACE(
            "[k32ext] LockResource: hglobal=0x%llx => 0x%llx\n",
            static_cast<unsigned long long>(hglobal),
            static_cast<unsigned long long>(hglobal));
        m.set_return(hglobal);
        return;
    }

    VITREO_TRACE(
        "[k32ext] LockResource: unknown handle=0x%llx => NULL\n",
        static_cast<unsigned long long>(hglobal));
    m.set_return(0);
}

void Kernel32ExtManager::impl_SizeofResource(Marshaler& m) {
    // DWORD SizeofResource(HMODULE hModule, HRSRC hResInfo)
    (void)m.arg(0); // hModule
    const u32 hrsrc = static_cast<u32>(m.arg(1));

    std::lock_guard<std::mutex> resources(m_resource_mutex);
    auto it = m_resource_handles.find(hrsrc);
    if (it == m_resource_handles.end()) {
        VITREO_TRACE( "[k32ext] SizeofResource: unknown hrsrc=0x%08X => 0\n", hrsrc);
        m.set_return(0);
        return;
    }

    VITREO_TRACE( "[k32ext] SizeofResource: hrsrc=0x%08X => %u\n", hrsrc, it->second.size);
    m.set_return(it->second.size);
}

} // namespace vitreo::winapi
