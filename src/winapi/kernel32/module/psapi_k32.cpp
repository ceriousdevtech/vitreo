// SPDX-License-Identifier: LGPL-2.1-only

/// @file psapi_k32.cpp
/// @brief Wave 3 Group G: PSAPI K32* exports.
///
/// K32-prefixed process/module query stubs from the Wave 3 banner.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/system/psapi/psapi.hpp"
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

// ---- Group G: PSAPI K32* ----
void Kernel32ExtManager::impl_K32EnumProcesses(Marshaler& m) {
    // BOOL K32EnumProcesses(DWORD *lpidProcess, DWORD cb, LPDWORD lpcbNeeded)
    // https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocesses
    // Oracle (process_contract_probe.expected): NULL + cb=0 succeeds, writes
    // needed=0, leaves last-error untouched. NULL + cb>0 fails ERROR_NOACCESS.
    const GuestAddr ids = m.arg_ptr(0);
    const u32 cb = static_cast<u32>(m.arg(1));
    const GuestAddr needed = m.arg_ptr(2);
    if (ids == 0) {
        if (cb == 0) {
            if (needed != 0) {
                (void)m.write_u32(needed, 0);
            }
            m.set_return(1);
            return;
        }
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }

    std::vector<u32> pids;
    if (m_process_snapshot_provider) {
        for (const auto& proc : m_process_snapshot_provider()) {
            pids.push_back(proc.pid);
        }
    }
    if (pids.empty()) {
        // Idle (0) and System (4) are always present on Windows; include the
        // guest pid so self-enumeration still finds a match without a host
        // snapshot provider.
        pids.push_back(0);
        pids.push_back(4);
        const u32 self = m_process_manager
            ? m_process_manager->get_current_process_id()
            : static_cast<u32>(::getpid());
        if (self != 0 && self != 4) {
            pids.push_back(self);
        }
    }

    const u32 max_pids = cb / static_cast<u32>(sizeof(u32));
    const u32 to_write = std::min(static_cast<u32>(pids.size()), max_pids);
    for (u32 i = 0; i < to_write; ++i) {
        (void)m.write_u32(ids + i * sizeof(u32), pids[i]);
    }
    // Learn: lpcbNeeded is "The number of bytes returned in the pProcessIds
    // array" — not the full enumerate size. Callers retry when needed == cb
    // (05_process_modules.md K32EnumProcesses MEDIUM).
    if (needed != 0) {
        (void)m.write_u32(needed, to_write * static_cast<u32>(sizeof(u32)));
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

// ---- psapi.dll / K32* process and module queries -------------------------
//
// psapi.dll's EnumProcessModules, GetModuleBaseName*, GetModuleFileNameEx*,
// GetModuleInformation and GetProcessMemoryInfo are forwarders to these K32*
// functions on Windows 7+, so one implementation serves both export names.
// Every rule below that Learn leaves open is measured by
// tools/oracle_probe/s7_psapi_probe.c (.expected / .x86.expected):
//   - access: GetProcessMemoryInfo and GetModuleFileNameEx(hModule NULL)
//     need PROCESS_QUERY_[LIMITED_]INFORMATION; every other query also needs
//     PROCESS_VM_READ; otherwise ERROR_ACCESS_DENIED. A non-process handle
//     (or NULL) is ERROR_INVALID_HANDLE, as is an unknown hModule.
//   - truncation: a name that does not fit copies nSize-1 characters plus
//     NUL and returns nSize; one that fits returns its length. Success
//     leaves the last error untouched.
namespace {

constexpr u32 kProcessVmRead = 0x0010u;
constexpr u32 kProcessQueryInformation = 0x0400u;
constexpr u32 kProcessQueryLimitedInformation = 0x1000u;

enum class PsapiProcess { Current, Other, Invalid, AccessDenied };

[[nodiscard]] PsapiProcess classify_psapi_process(HandleTable* handles,
                                                  const ProcessManager* processes,
                                                  u64 raw_handle,
                                                  bool needs_vm_read) {
    const Handle handle = static_cast<Handle>(raw_handle);
    if (handle == CURRENT_PROCESS_PSEUDO) return PsapiProcess::Current;
    if (handles == nullptr ||
        handles->get_type(handle) != HandleType::Process) {
        return PsapiProcess::Invalid;
    }
    auto* process = static_cast<ipc::ProcessObject*>(
        handles->get(handle, HandleType::Process));
    if (process == nullptr) return PsapiProcess::Invalid;
    const u32 access = handles->get_access(handle);
    if (access != HandleAccess::All) {
        const bool query = (access & (kProcessQueryInformation |
                                      kProcessQueryLimitedInformation)) != 0;
        const bool vm_read = (access & kProcessVmRead) != 0;
        if (!query || (needs_vm_read && !vm_read)) {
            return PsapiProcess::AccessDenied;
        }
    }
    if (processes != nullptr &&
        static_cast<u32>(process->pid) == processes->get_current_process_id()) {
        return PsapiProcess::Current;
    }
    return PsapiProcess::Other;
}

/// Fail a query whose target handle is not the current process. Only this
/// process's loader list and counters are visible to Vitreo.
bool fail_psapi_target(PsapiProcess kind, const char* api) {
    switch (kind) {
    case PsapiProcess::Current:
        return false;
    case PsapiProcess::Invalid:
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return true;
    case PsapiProcess::AccessDenied:
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return true;
    case PsapiProcess::Other:
        break;
    }
    static std::mutex once_mutex;
    static std::unordered_map<std::string, bool> reported;
    {
        std::lock_guard lock(once_mutex);
        if (!reported[api]) {
            reported[api] = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED psapi!%s: another process's "
                         "module list / memory counters\n", api);
        }
    }
    set_last_error(error_codes::ERROR_ACCESS_DENIED);
    return true;
}

/// Copy @p units into a caller buffer of @p capacity elements with the
/// measured psapi truncation rule; returns the value the API returns.
template <typename Unit>
u32 copy_psapi_name(Marshaler& m, GuestAddr buffer, u32 capacity,
                    const std::vector<Unit>& units) {
    const u32 length = static_cast<u32>(units.size());
    const u32 copied = length < capacity ? length : capacity - 1;
    for (u32 i = 0; i < copied; ++i) {
        if constexpr (sizeof(Unit) == 2) {
            (void)m.write_u16(buffer + i * 2, units[i]);
        } else {
            (void)m.write_u8(buffer + i, static_cast<u8>(units[i]));
        }
    }
    if constexpr (sizeof(Unit) == 2) {
        (void)m.write_u16(buffer + copied * 2, 0);
    } else {
        (void)m.write_u8(buffer + copied, 0);
    }
    return length < capacity ? length : capacity;
}

[[nodiscard]] std::vector<u8> acp_units(std::string_view utf8) {
    const std::string acp = utf8_to_acp(utf8);
    return std::vector<u8>(acp.begin(), acp.end());
}

[[nodiscard]] std::string_view leaf_name(std::string_view path) {
    const auto slash = path.find_last_of("\\/");
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

std::optional<std::string> Kernel32ExtManager::psapi_module_path(
    Marshaler& m, u64 process, GuestAddr module, bool needs_vm_read,
    const char* api) {
    const PsapiProcess kind = classify_psapi_process(
        m_handle_table, m_process_manager, process, needs_vm_read);
    // The main image of another process is known from the process snapshot
    // (GetModuleFileNameEx(hProcess, NULL) is the documented way to ask).
    if (kind == PsapiProcess::Other && module == 0 && !needs_vm_read) {
        auto image = process_image_path(static_cast<Handle>(process));
        if (image) return image;
    }
    if (fail_psapi_target(kind, api)) return std::nullopt;
    const std::string* path =
        m_module_manager ? m_module_manager->module_path(module) : nullptr;
    if (path == nullptr || path->empty()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    return *path;
}

void Kernel32ExtManager::psapi_module_base_name(Marshaler& m, bool wide) {
    // DWORD GetModuleBaseName{A,W}(HANDLE, HMODULE, LPTSTR, DWORD nSize)
    // https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmodulebasenamew
    const u64 process = m.arg(0);
    const GuestAddr module = m.arg_ptr(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    // Measured: nSize 0 returns 0 without touching the last error for W and
    // with ERROR_INVALID_PARAMETER for A.
    if (size == 0) {
        if (!wide) set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    auto path = psapi_module_path(m, process, module, /*needs_vm_read=*/true,
                                  wide ? "GetModuleBaseNameW"
                                       : "GetModuleBaseNameA");
    if (!path) {
        m.set_return(0u);
        return;
    }
    const std::string_view name = leaf_name(*path);
    m.set_return(wide ? copy_psapi_name(m, buffer, size, utf8_to_utf16(name))
                      : copy_psapi_name(m, buffer, size, acp_units(name)));
}

void Kernel32ExtManager::impl_K32GetModuleBaseNameA(Marshaler& m) {
    psapi_module_base_name(m, false);
}

void Kernel32ExtManager::impl_K32GetModuleBaseNameW(Marshaler& m) {
    psapi_module_base_name(m, true);
}

void Kernel32ExtManager::psapi_module_file_name(Marshaler& m, bool wide) {
    // DWORD GetModuleFileNameEx{A,W}(HANDLE, HMODULE, LPTSTR, DWORD nSize)
    // https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getmodulefilenameexw
    const u64 process = m.arg(0);
    const GuestAddr module = m.arg_ptr(1);
    const GuestAddr buffer = m.arg_ptr(2);
    const u32 size = m.arg32(3);
    if (size == 0) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0u);
        return;
    }
    // hModule NULL asks for the process image, which needs only query access.
    auto path = psapi_module_path(m, process, module,
                                  /*needs_vm_read=*/module != 0,
                                  wide ? "GetModuleFileNameExW"
                                       : "GetModuleFileNameExA");
    if (!path) {
        m.set_return(0u);
        return;
    }
    m.set_return(wide ? copy_psapi_name(m, buffer, size, utf8_to_utf16(*path))
                      : copy_psapi_name(m, buffer, size, acp_units(*path)));
}

void Kernel32ExtManager::impl_K32GetModuleFileNameExA(Marshaler& m) {
    psapi_module_file_name(m, false);
}

void Kernel32ExtManager::impl_K32GetModuleFileNameExW(Marshaler& m) {
    psapi_module_file_name(m, true);
}

void Kernel32ExtManager::impl_K32EnumProcessModules(Marshaler& m) {
    // BOOL EnumProcessModules(HANDLE hProcess, HMODULE* lphModule, DWORD cb,
    //                         LPDWORD lpcbNeeded)
    // https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-enumprocessmodules
    //
    // The contract is load-bearing well beyond process inspection: a
    // process-wide symbol search (Mono's mono_dl_lookup_symbol for one)
    // enumerates every module here and GetProcAddresses each. HMODULEs are
    // pointer-sized. EnumProcessModulesEx shares this entry: Vitreo presents
    // one module list per process, all of that process's own bitness, so
    // every LIST_MODULES_* filter selects the same set.
    const u64 process = m.arg(0);
    const GuestAddr modules_out = m.arg_ptr(1);
    const u32 cb = m.arg32(2);
    const GuestAddr needed = m.arg_ptr(3);

    const PsapiProcess kind = classify_psapi_process(
        m_handle_table, m_process_manager, process, /*needs_vm_read=*/true);
    if (fail_psapi_target(kind, "EnumProcessModules")) {
        m.set_return(0u);
        return;
    }
    const std::vector<GuestAddr> handles =
        m_module_manager ? m_module_manager->get_all_module_handles()
                         : std::vector<GuestAddr>{};
    const u32 handle_size = m.cpu_is_64bit() ? 8u : 4u;
    const u32 writable = std::min(static_cast<u32>(handles.size()),
                                  cb / handle_size);
    for (u32 i = 0; i < writable; ++i) {
        if (m.write_ptr(modules_out + i * handle_size, handles[i]).is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0u);
            return;
        }
    }
    // Measured: a NULL lpcbNeeded faults inside the call (ERROR_NOACCESS),
    // it is not a parameter check.
    if (m.write_u32(needed, static_cast<u32>(handles.size() * handle_size))
            .is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

void Kernel32ExtManager::impl_K32GetModuleInformation(Marshaler& m) {
    // BOOL GetModuleInformation(HANDLE, HMODULE, LPMODULEINFO, DWORD cb)
    const u64 process = m.arg(0);
    const GuestAddr module = m.arg_ptr(1);
    const GuestAddr output = m.arg_ptr(2);
    const u32 cb = m.arg32(3);
    const PsapiProcess kind = classify_psapi_process(
        m_handle_table, m_process_manager, process, /*needs_vm_read=*/true);
    if (fail_psapi_target(kind, "GetModuleInformation")) {
        m.set_return(0u);
        return;
    }
    m.set_return(psapi_get_module_information(
        m, m_module_manager, CURRENT_PROCESS_PSEUDO, module, output, cb));
}
void Kernel32ExtManager::impl_K32GetPerformanceInfo(Marshaler& m) {
    GuestAddr info = m.arg_ptr(0);
    u32 cb = static_cast<u32>(m.arg(1));
    const u32 expected_size = m.cpu_is_64bit() ? 104u : 56u;
    if (info == 0 || cb != expected_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    // macOS does not expose Windows commit accounting, kernel paged/nonpaged
    // pools, or a Windows handle count. Returning a zero-filled structure as
    // success fabricates those counters, so fail until a contract-preserving
    // translation is available.
    VITREO_TRACE(
        "[k32ext] K32GetPerformanceInfo: unsupported host translation\n");
    set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    m.set_return(0);
}
void Kernel32ExtManager::impl_K32GetProcessMemoryInfo(Marshaler& m) {
    // BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD cb)
    // https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo
    //
    // This used to zero the whole structure and return TRUE. A caller cannot
    // tell that apart from a process genuinely using no memory. WorkingSetSize
    // is what callers read as used physical memory, so a zeroed counter makes
    // a memory-driven allocator believe there is nothing to reclaim.
    //
    // PROCESS_MEMORY_COUNTERS is DWORD cb, DWORD PageFaultCount, then eight
    // SIZE_T; _EX adds PrivateUsage, _EX2 adds PrivateWorkingSetSize and a
    // ULONG64 SharedCommitUsage. Measured sizes (s7_psapi_probe): 72/80/96 on
    // x64, 40/44/56 on x86. Windows writes the largest form that fits in cb
    // and stores that size in cb; a cb below the base form is
    // ERROR_INSUFFICIENT_BUFFER, a NULL buffer ERROR_INVALID_PARAMETER.
    const u64 process = m.arg(0);
    const GuestAddr info = m.arg_ptr(1);
    const u32 cb = m.arg32(2);
    const bool is_64bit = m.cpu_is_64bit();
    const u32 field = is_64bit ? 8u : 4u;
    const u32 base_size = is_64bit ? 72u : 40u;
    const u32 ex_size   = is_64bit ? 80u : 44u;
    const u32 ex2_size  = is_64bit ? 96u : 56u;

    if (cb < base_size) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    if (info == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const PsapiProcess kind = classify_psapi_process(
        m_handle_table, m_process_manager, process, /*needs_vm_read=*/false);
    if (fail_psapi_target(kind, "GetProcessMemoryInfo")) {
        m.set_return(0);
        return;
    }

    // Resident size is the honest analogue of the Windows working set, and
    // phys_footprint is what macOS itself reports as a process's memory
    // footprint — the closest thing to Windows' commit charge. Reporting
    // virtual_size instead would be wildly misleading here, because Vitreo
    // reserves a 64 TB guest address space up front.
    u64 working_set = 0, peak_working_set = 0, footprint = 0, private_ws = 0;
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t basic_count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  reinterpret_cast<task_info_t>(&basic), &basic_count) ==
        KERN_SUCCESS) {
        working_set = basic.resident_size;
        peak_working_set = basic.resident_size_max;
    }
    task_vm_info_data_t vm_info{};
    mach_msg_type_number_t vm_count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&vm_info), &vm_count) ==
        KERN_SUCCESS) {
        footprint = vm_info.phys_footprint;
        // Private (anonymous) resident memory: the private part of the
        // working set.
        private_ws = vm_info.internal;
    }
    if (footprint == 0) footprint = working_set;
    if (peak_working_set < working_set) peak_working_set = working_set;
    if (private_ws > working_set) private_ws = working_set;

    u64 page_faults = 0;
    task_events_info_data_t events{};
    mach_msg_type_number_t events_count = TASK_EVENTS_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_EVENTS_INFO,
                  reinterpret_cast<task_info_t>(&events), &events_count) ==
        KERN_SUCCESS) {
        page_faults = static_cast<u64>(events.faults);
    }

    const u32 written = cb >= ex2_size ? ex2_size
                      : cb >= ex_size  ? ex_size
                                       : base_size;
    bool ok = true;
    for (u32 i = 0; i < written && ok; ++i) ok = m.write_u8(info + i, 0).is_ok();
    if (!ok) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    const auto put = [&](u32 offset, u64 value) {
        if (is_64bit) (void)m.write_u64(info + offset, value);
        else (void)m.write_u32(info + offset,
                               static_cast<u32>(std::min<u64>(value, 0xFFFFFFFFull)));
    };
    (void)m.write_u32(info + 0, written);
    (void)m.write_u32(info + 4, static_cast<u32>(
        std::min<u64>(page_faults, 0xFFFFFFFFull)));
    constexpr u32 first = 8u;   // after cb + PageFaultCount, both bitnesses
    put(first + 0 * field, peak_working_set);   // PeakWorkingSetSize
    put(first + 1 * field, working_set);        // WorkingSetSize
    // Quota*PoolUsage are Windows kernel pool accounting with no macOS
    // analogue; zero is the truthful answer rather than an invented number.
    put(first + 6 * field, footprint);          // PagefileUsage
    put(first + 7 * field, footprint);          // PeakPagefileUsage
    if (written >= ex_size) {
        put(first + 8 * field, footprint);      // PrivateUsage
    }
    if (written == ex2_size) {
        put(first + 9 * field, private_ws);     // PrivateWorkingSetSize
        // SharedCommitUsage (ULONG64, 8-aligned) is commit of shared
        // sections; the guest has no pagefile-backed shared commit charge.
        (void)m.write_u64(info + (is_64bit ? 88u : 48u), 0);
    }

    VITREO_TRACE("[psapi] GetProcessMemoryInfo cb=%u -> ws=%llu peak=%llu "
                 "footprint=%llu faults=%llu\n",
                 written, (unsigned long long)working_set,
                 (unsigned long long)peak_working_set,
                 (unsigned long long)footprint,
                 (unsigned long long)page_faults);
    m.set_return(1);
}
void Kernel32ExtManager::impl_K32QueryWorkingSetEx(Marshaler& m) {
    (void)m.arg(0);
    GuestAddr info = m.arg_ptr(1);
    u32 cb = static_cast<u32>(m.arg(2));
    if (info && cb) { for (u32 i = 0; i < cb; ++i) (void)m.write_u8(info + i, 0); }
    m.set_return(1);
}
void Kernel32ExtManager::impl_Module32FirstW(Marshaler& m) {
    (void)write_next_module_entry(m, static_cast<u32>(m.arg(0)),
                                  m.arg_ptr(1), true, true);
}
void Kernel32ExtManager::impl_Module32NextW(Marshaler& m) {
    (void)write_next_module_entry(m, static_cast<u32>(m.arg(0)),
                                  m.arg_ptr(1), true, false);
}

bool Kernel32ExtManager::write_next_module_entry(
    Marshaler& m, u32 snapshot, GuestAddr entry, bool wide, bool first) {
    // MODULEENTRY32 has native-width pointers and handles. On x64 the pointer
    // fields are aligned to 8 bytes, moving the string arrays from 32 to 48.
    constexpr u32 kMaxModuleName32 = 256;
    constexpr u32 kMaxPath = 260;
    const bool is_64bit = m.cpu_is_64bit();
    const u32 expected_size = wide
        ? (is_64bit ? 1080u : 1064u)
        : (is_64bit ? 568u : 548u);

    std::lock_guard<std::mutex> snapshots(m_toolhelp_mutex);
    auto found = m_toolhelp_snapshots.find(snapshot);
    if (found == m_toolhelp_snapshots.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return false;
    }
    if (entry == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return false;
    }
    const u32 supplied_size = m.read_u32(entry).value_or(0);
    if (supplied_size < expected_size) {
        set_last_error(error_codes::ERROR_BAD_LENGTH);
        m.set_return(0);
        return false;
    }

    auto& state = found->second;
    if (first) state.next_module_index = 0;
    if (state.next_module_index >= state.modules.size()) {
        set_last_error(error_codes::ERROR_NO_MORE_FILES);
        m.set_return(0);
        return false;
    }

    const auto& module = state.modules[state.next_module_index++];
    const u32 process_id = m_process_manager
        ? m_process_manager->get_current_process_id()
        : static_cast<u32>(::getpid());
    const u32 base_offset = is_64bit ? 24u : 20u;
    const u32 size_offset = is_64bit ? 32u : 24u;
    const u32 handle_offset = is_64bit ? 40u : 28u;
    const u32 name_offset = is_64bit ? 48u : 32u;
    const u32 path_offset = name_offset + kMaxModuleName32 * (wide ? 2u : 1u);

    (void)m.write_u32(entry + 4, 1);             // th32ModuleID
    (void)m.write_u32(entry + 8, process_id);    // th32ProcessID
    (void)m.write_u32(entry + 12, 0xFFFFu);      // GlblcntUsage
    (void)m.write_u32(entry + 16, 0xFFFFu);      // ProccntUsage
    if (is_64bit) {
        (void)m.write_u64(entry + base_offset, module.base_address);
        (void)m.write_u64(entry + handle_offset, module.base_address);
    } else {
        (void)m.write_u32(entry + base_offset,
                          static_cast<u32>(module.base_address));
        (void)m.write_u32(entry + handle_offset,
                          static_cast<u32>(module.base_address));
    }
    (void)m.write_u32(entry + size_offset, module.image_size);

    auto write_string = [&](GuestAddr destination, u32 capacity,
                            std::string_view value) {
        if (wide) {
            const auto utf16 = utf8_to_utf16(value);
            const usize count = std::min<usize>(utf16.size(), capacity - 1);
            for (usize i = 0; i < count; ++i) {
                (void)m.write_u16(destination + i * 2, utf16[i]);
            }
            (void)m.write_u16(destination + count * 2, 0);
        } else {
            const usize count = std::min<usize>(value.size(), capacity - 1);
            for (usize i = 0; i < count; ++i) {
                (void)m.write_u8(destination + i,
                                 static_cast<u8>(value[i]));
            }
            (void)m.write_u8(destination + count, 0);
        }
    };
    write_string(entry + name_offset, kMaxModuleName32, module.name);
    write_string(entry + path_offset, kMaxPath, module.path);

    VITREO_TRACE(
        "[k32ext] Module32%s -> pid=%u base=0x%llX size=%u name='%s'\n",
        wide ? "W" : "A", process_id,
        static_cast<unsigned long long>(module.base_address),
        module.image_size, module.name.c_str());
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
    return true;
}
namespace {
constexpr u32 kPe32MaxPath = 260;

struct ProcessEntryLayout {
    u32 required_size;
    u32 default_heap_offset;
    u32 module_id_offset;
    u32 thread_count_offset;
    u32 parent_pid_offset;
    u32 priority_offset;
    u32 flags_offset;
    u32 name_offset;
};

[[nodiscard]] ProcessEntryLayout process_entry_layout(bool is_64bit,
                                                       bool wide) {
    if (is_64bit) {
        // ULONG_PTR is 8-byte aligned. The structure itself is rounded to its
        // 8-byte native alignment after the fixed-size filename array.
        return {
            wide ? 568u : 304u, 16u, 24u, 28u, 32u, 36u, 40u, 44u};
    }
    return {
        wide ? 556u : 296u, 12u, 16u, 20u, 24u, 28u, 32u, 36u};
}
} // namespace

void Kernel32ExtManager::write_process_entry(
    Marshaler& m, GuestAddr entry, bool wide,
    const ipc::ProcessSnapshotEntry& process) {
    const ProcessEntryLayout layout =
        process_entry_layout(m.cpu_is_64bit(), wide);
    (void)m.write_u32(entry + 0, layout.required_size); // dwSize
    (void)m.write_u32(entry + 4, 0);      // cntUsage (obsolete)
    (void)m.write_u32(entry + 8, process.pid); // th32ProcessID
    if (m.cpu_is_64bit()) {
        (void)m.write_u64(entry + layout.default_heap_offset, 0);
    } else {
        (void)m.write_u32(entry + layout.default_heap_offset, 0);
    }
    (void)m.write_u32(entry + layout.module_id_offset, 0);
    (void)m.write_u32(entry + layout.thread_count_offset, 1);
    (void)m.write_u32(entry + layout.parent_pid_offset, process.parent_pid);
    (void)m.write_u32(entry + layout.priority_offset, 8);
    (void)m.write_u32(entry + layout.flags_offset, 0);

    const usize n = std::min<usize>(process.image_name.size(), kPe32MaxPath - 1);
    for (usize i = 0; i < n; ++i) {
        const auto c = static_cast<u8>(process.image_name[i]);
        if (wide) {
            (void)m.write_u16(entry + layout.name_offset + i * 2, c);
        } else {
            (void)m.write_u8(entry + layout.name_offset + i, c);
        }
    }
    if (wide) {
        (void)m.write_u16(entry + layout.name_offset + n * 2, 0);
    } else {
        (void)m.write_u8(entry + layout.name_offset + n, 0);
    }

    VITREO_TRACE(
        "[k32ext] Process32%s(size=%u native=%u) -> pid=%u parent=%u "
        "exe='%s'\n",
        wide ? "W" : "A", layout.required_size, layout.required_size,
        process.pid, process.parent_pid, process.image_name.c_str());
}

bool Kernel32ExtManager::write_next_process_entry(
    Marshaler& m, u32 snapshot, GuestAddr entry, bool wide, bool first) {
    std::lock_guard<std::mutex> snapshots(m_toolhelp_mutex);
    auto found = m_toolhelp_snapshots.find(snapshot);
    if (entry == 0 || found == m_toolhelp_snapshots.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return false;
    }

    const ProcessEntryLayout layout =
        process_entry_layout(m.cpu_is_64bit(), wide);
    const auto input_size = m.read_u32(entry);
    if (input_size.is_err() || input_size.value() < layout.required_size) {
        VITREO_TRACE(
            "[k32ext] Process32%s rejected dwSize=%u required=%u\n",
            wide ? "W" : "A", input_size.value_or(0),
            layout.required_size);
        set_last_error(error_codes::ERROR_BAD_LENGTH);
        m.set_return(0);
        return false;
    }
    VITREO_TRACE("[k32ext] Process32%s accepted dwSize=%u required=%u\n",
                 wide ? "W" : "A", input_size.value(),
                 layout.required_size);

    auto& state = found->second;
    if (first) state.next_index = 0;
    if (state.next_index >= state.processes.size()) {
        set_last_error(error_codes::ERROR_NO_MORE_FILES);
        m.set_return(0);
        return false;
    }

    write_process_entry(m, entry, wide, state.processes[state.next_index++]);
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
    return true;
}

bool Kernel32ExtManager::write_next_thread_entry(
    Marshaler& m, u32 snapshot, GuestAddr entry, bool first) {
    // BOOL Thread32First/Next(HANDLE, LPTHREADENTRY32)
    // https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32first
    std::lock_guard<std::mutex> snapshots(m_toolhelp_mutex);
    auto found = m_toolhelp_snapshots.find(snapshot);
    if (entry == 0 || found == m_toolhelp_snapshots.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return false;
    }

    auto& state = found->second;
    if (first) state.next_thread_index = 0;
    if (state.next_thread_index >= state.threads.size()) {
        set_last_error(error_codes::ERROR_NO_MORE_FILES);
        m.set_return(0);
        return false;
    }

    const u32 tid = state.threads[state.next_thread_index++];
    const u32 owner_pid = m_process_manager
        ? m_process_manager->get_current_process_id()
        : 0;
    // THREADENTRY32: dwSize must be at least 28 on input.
    const u32 dw_size = m.read_u32(entry).value_or(0);
    if (dw_size < 28) {
        set_last_error(error_codes::ERROR_BAD_LENGTH);
        m.set_return(0);
        return false;
    }
    (void)m.write_u32(entry + 0, 28);          // dwSize
    (void)m.write_u32(entry + 4, 1);           // cntUsage
    (void)m.write_u32(entry + 8, tid);         // th32ThreadID
    (void)m.write_u32(entry + 12, owner_pid);  // th32OwnerProcessID
    (void)m.write_u32(entry + 16, 8);          // tpBasePri (THREAD_PRIORITY_NORMAL)
    (void)m.write_u32(entry + 20, 0);          // tpDeltaPri
    (void)m.write_u32(entry + 24, 0);          // dwFlags
    VITREO_TRACE("[k32ext] Thread32 -> tid=%u owner=%u\n", tid, owner_pid);
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
    return true;
}

void Kernel32ExtManager::impl_Process32FirstW(Marshaler& m) {
    (void)write_next_process_entry(m, static_cast<u32>(m.arg(0)),
                                   m.arg_ptr(1), true, true);
}

void Kernel32ExtManager::impl_Process32NextW(Marshaler& m) {
    (void)write_next_process_entry(m, static_cast<u32>(m.arg(0)),
                                   m.arg_ptr(1), true, false);
}

} // namespace vitreo::winapi

namespace vitreo::winapi {

// DWORD K32GetProcessImageFileNameA/W(HANDLE hProcess, LPTSTR lpImageFileName,
//                                     DWORD nSize)
// https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessimagefilenamew
void Kernel32ExtManager::impl_K32GetProcessImageFileName(Marshaler& m, bool wide) {
    const auto handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 size = m.arg32(2);
    const auto image = process_image_path(handle);
    if (!image) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const auto device_path = win32_to_nt_device_path(m_vfs, *image);
    if (!device_path) {
        // An image outside every drive has no device path to report.
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    }
    const std::vector<u16> units = wide ? utf8_to_utf16(*device_path) : std::vector<u16>{};
    const std::string bytes = wide ? std::string{} : utf8_to_acp(*device_path);
    const usize length = wide ? units.size() : bytes.size();
    if (buffer == 0 || size <= length) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    bool ok = true;
    for (usize i = 0; i < length && ok; ++i) {
        ok = wide ? m.write_u16(buffer + i * 2, units[i]).is_ok()
                  : m.write_u8(buffer + i, static_cast<u8>(bytes[i])).is_ok();
    }
    ok = ok && (wide ? m.write_u16(buffer + length * 2, 0).is_ok()
                     : m.write_u8(buffer + length, 0).is_ok());
    if (!ok) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    m.set_return(static_cast<u32>(length));
}

}  // namespace vitreo::winapi
