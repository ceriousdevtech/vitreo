// SPDX-License-Identifier: LGPL-2.1-only
/// @file psapi.cpp
/// @brief Implementation of PSAPI.dll API functions

#include "psapi.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <mach/mach.h>
#include <mach/task_info.h>

namespace vitreo::winapi {

// =============================================================================
// Constants
// =============================================================================

// =============================================================================
// PsapiManager Implementation
// =============================================================================

u32 psapi_get_module_information(Marshaler& m, const ModuleManager* modules,
                                 u32 h_process, GuestAddr h_module,
                                 GuestAddr output, u32 cb) {
    // Measured (s7_psapi_probe): a cb below sizeof(MODULEINFO) is
    // ERROR_INSUFFICIENT_BUFFER; hModule NULL means the main image.
    const u32 required = m.cpu_is_64bit() ? 24u : 12u;
    if (cb < required) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (!output) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return 0;
    }
    const ModuleInfo* info = modules
        ? modules->get_module_info_by_handle(h_module) : nullptr;
    if (h_process != 0xffffffffu || !info) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return 0;
    }
    // MODULEINFO size and entry point are defined by the loaded PE header.
    const u64 base = info->base_address;
    const u32 pe_offset = m.memory().read_u32(base + 0x3c).value_or(0);
    const u64 pe = base + pe_offset;
    if (m.memory().read_u16(base).value_or(0) != 0x5a4d ||
        m.memory().read_u32(pe).value_or(0) != 0x4550) {
        set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
        return 0;
    }
    const u32 size = m.memory().read_u32(pe + 24 + 56).value_or(0);
    const u32 entry_rva = m.memory().read_u32(pe + 24 + 16).value_or(0);
    const u64 entry = entry_rva ? base + entry_rva : 0;
    bool ok;
    if (m.cpu_is_64bit()) {
        ok = m.memory().write_u64(output, base).is_ok() &&
             m.memory().write_u32(output + 8, size).is_ok() &&
             m.memory().write_u32(output + 12, 0).is_ok() &&
             m.memory().write_u64(output + 16, entry).is_ok();
    } else {
        ok = m.memory().write_u32(output, static_cast<u32>(base)).is_ok() &&
             m.memory().write_u32(output + 4, size).is_ok() &&
             m.memory().write_u32(output + 8, static_cast<u32>(entry)).is_ok();
    }
    if (!ok) set_last_error(error_codes::ERROR_NOACCESS);
    return ok ? 1u : 0u;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

// =============================================================================
// Export Table Factory
// =============================================================================

void PsapiManager::get_mapped_file_name(Marshaler& m, bool wide) {
    const u64 process = m.arg(0);
    const GuestAddr address = m.arg_ptr(1), output = m.arg_ptr(2);
    const u32 capacity = static_cast<u32>(m.arg(3));
    if (process != ~u64{0} && process != 0xffffffffu) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (!output || !capacity) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::string path = m_mapped_path ? m_mapped_path(address) : std::string{};
    if (path.empty()) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    }
    // The resolver reports the NT device path QueryDosDevice maps back
    // (win32_to_nt_device_path); anything else has no device to name.
    if (!path.starts_with("\\Device\\")) {
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    const auto text = utf8_to_utf16(path);
    if (!wide) path = utf8_to_acp(path);
    const usize length = wide ? text.size() : path.size();
    const usize count = std::min<usize>(length, capacity - 1);
    bool ok = true;
    for (usize i=0; i<count && ok; ++i) {
        ok = wide ? m.memory().write_u16(output + i*2, text[i]).is_ok()
                  : m.memory().write_u8(output + i, static_cast<u8>(path[i])).is_ok();
    }
    ok = ok && (wide ? m.memory().write_u16(output + count*2, 0).is_ok()
                     : m.memory().write_u8(output + count, 0).is_ok());
    if (!ok) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
    } else if (length >= capacity) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(capacity);
    } else m.set_return(length);
}

DllExports create_psapi_thunks(PsapiManager& mgr, Kernel32ExtManager& k32) {
    DllExports exports;
    exports.entries.reserve(12);
    // Ordinals from the Windows 10/11 psapi.dll export table (identical in
    // System32 and SysWOW64; the NT4/2000 numbering differs).
    exports.entries.push_back({"GetMappedFileNameW", 13,
        [&mgr](Marshaler& m) { mgr.get_mapped_file_name(m, true); }, 16});
    exports.entries.push_back({"GetMappedFileNameA", 12,
        [&mgr](Marshaler& m) { mgr.get_mapped_file_name(m, false); }, 16});
    // GetProcessImageFileNameA/W forward to kernel32's K32 functions.
    exports.entries.push_back(ExportEntry{
        "GetProcessImageFileNameA", 20,
        [&k32](Marshaler& m) { k32.impl_K32GetProcessImageFileName(m, false); }, 12});
    exports.entries.push_back(ExportEntry{
        "GetProcessImageFileNameW", 21,
        [&k32](Marshaler& m) { k32.impl_K32GetProcessImageFileName(m, true); }, 12});

    // On Windows 7+ these psapi.dll exports are forwarders to kernel32's
    // K32* functions: bind them to that one implementation so the two export
    // names cannot diverge (the psapi copy used to write the x86
    // PROCESS_MEMORY_COUNTERS layout into x64 callers).
    const auto k32_entry = [&](const char* name, u16 ordinal,
                               void (Kernel32ExtManager::*fn)(Marshaler&),
                               u16 stdcall_bytes) {
        exports.entries.push_back(ExportEntry{
            name, ordinal,
            [&k32, fn](Marshaler& m) { (k32.*fn)(m); }, stdcall_bytes});
    };
    k32_entry("EnumProcessModules", 5,
              &Kernel32ExtManager::impl_K32EnumProcessModules, 16);
    // LIST_MODULES_* selects among one same-bitness module list.
    k32_entry("EnumProcessModulesEx", 6,
              &Kernel32ExtManager::impl_K32EnumProcessModules, 20);
    k32_entry("EnumProcesses", 7,
              &Kernel32ExtManager::impl_K32EnumProcesses, 12);
    k32_entry("GetModuleBaseNameA", 14,
              &Kernel32ExtManager::impl_K32GetModuleBaseNameA, 16);
    k32_entry("GetModuleBaseNameW", 15,
              &Kernel32ExtManager::impl_K32GetModuleBaseNameW, 16);
    k32_entry("GetModuleFileNameExA", 16,
              &Kernel32ExtManager::impl_K32GetModuleFileNameExA, 16);
    k32_entry("GetModuleFileNameExW", 17,
              &Kernel32ExtManager::impl_K32GetModuleFileNameExW, 16);
    k32_entry("GetModuleInformation", 18,
              &Kernel32ExtManager::impl_K32GetModuleInformation, 16);
    k32_entry("GetProcessMemoryInfo", 22,
              &Kernel32ExtManager::impl_K32GetProcessMemoryInfo, 12);
    k32_entry("GetPerformanceInfo", 19,
              &Kernel32ExtManager::impl_K32GetPerformanceInfo, 8);
    return exports;
}

} // namespace vitreo::winapi
