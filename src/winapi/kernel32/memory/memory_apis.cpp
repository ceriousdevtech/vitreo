// SPDX-License-Identifier: LGPL-2.1-only

/// @file memory_apis.cpp
/// @brief Memory banner APIs from kernel32_ext.
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
// Memory
// =============================================================================

void Kernel32ExtManager::impl_IsBadWritePtr(Marshaler& m) {
    // BOOL IsBadWritePtr(LPVOID lp, UINT_PTR ucb)
    (void)m.arg_ptr(0);
    (void)m.arg(1);
    m.set_return(0); // FALSE — memory is writable
}

void Kernel32ExtManager::impl_GlobalMemoryStatusEx(Marshaler& m) {
    // BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX lpBuffer)
    GuestAddr status_ptr = m.arg_ptr(0);
    if (status_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const auto length = m_memory.read_u32(status_ptr);
    if (length.is_err() || length.value() != 64) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    u64 total_phys = 0;
    size_t total_size = sizeof(total_phys);
    if (sysctlbyname("hw.memsize", &total_phys, &total_size,
                     nullptr, 0) != 0 ||
        total_phys == 0) {
        set_last_error(error_codes::ERROR_GEN_FAILURE);
        m.set_return(0);
        return;
    }

    vm_statistics64_data_t vm_stat{};
    mach_msg_type_number_t stat_count = HOST_VM_INFO64_COUNT;
    u64 available_phys = 0;
    if (host_statistics64(
            mach_host_self(), HOST_VM_INFO64,
            reinterpret_cast<host_info64_t>(&vm_stat),
            &stat_count) == KERN_SUCCESS) {
        available_phys =
            (static_cast<u64>(vm_stat.free_count) +
             static_cast<u64>(vm_stat.inactive_count)) *
            static_cast<u64>(vm_page_size);
    }
    available_phys = std::min(available_phys, total_phys);

    struct xsw_usage swap{};
    size_t swap_size = sizeof(swap);
    (void)sysctlbyname(
        "vm.swapusage", &swap, &swap_size, nullptr, 0);
    const u64 total_page = total_phys + swap.xsu_total;
    const u64 available_page = available_phys + swap.xsu_avail;
    const u32 memory_load = total_phys == 0
        ? 0
        : static_cast<u32>(
              ((total_phys - available_phys) * 100) / total_phys);

    // ullTotalVirtual is the SIZE of the user-mode VA range (Learn), not the
    // maximum address. Windows x64 ≈ 0x7FFFFFFE0000 (= max 0x7FFFFFFEFFFF -
    // min 0x10000 + 1).
    const u64 max_va =
        m_memory.maximum_user_address() != 0
            ? static_cast<u64>(m_memory.maximum_user_address())
            : (m.cpu_is_64bit() ? 0x00007FFFFFFEFFFFull
                                : 0x000000007FFEFFFFull);
    constexpr u64 kMinUserVa = 0x10000ull;
    const u64 total_virtual =
        max_va >= kMinUserVa ? (max_va - kMinUserVa + 1) : max_va;

    // ullAvailVirtual is "the amount of unreserved and uncommitted memory
    // currently in the user-mode portion of the virtual address space of the
    // calling process" (Microsoft Learn) -- NOT the size of that space.
    //
    // This used to report total_virtual, i.e. a permanently empty address
    // space. An application that sizes its pools from the value then asks for
    // memory it cannot possibly have. A 32-bit guest that is not
    // large-address-aware has a 2 GiB window; reporting the whole window as
    // available makes those allocations fail and retry.
    const u64 mapped = static_cast<u64>(m_memory.total_mapped_size());
    const u64 available_virtual = mapped >= total_virtual
                                      ? 0u
                                      : total_virtual - mapped;
    (void)m_memory.write_u32(status_ptr + 0, 64);
    (void)m_memory.write_u32(status_ptr + 4, memory_load);
    (void)m_memory.write_u64(status_ptr + 8, total_phys);
    (void)m_memory.write_u64(status_ptr + 16, available_phys);
    (void)m_memory.write_u64(status_ptr + 24, total_page);
    (void)m_memory.write_u64(status_ptr + 32, available_page);
    (void)m_memory.write_u64(status_ptr + 40, total_virtual);
    (void)m_memory.write_u64(status_ptr + 48, available_virtual);
    (void)m_memory.write_u64(status_ptr + 56, 0);

    VITREO_TRACE(
        "[k32ext] GlobalMemoryStatusEx: total=%llu avail=%llu "
        "swap_total=%llu load=%u%% virt_total=%llu virt_avail=%llu\n",
        static_cast<unsigned long long>(total_phys),
        static_cast<unsigned long long>(available_phys),
        static_cast<unsigned long long>(swap.xsu_total), memory_load,
        static_cast<unsigned long long>(total_virtual),
        static_cast<unsigned long long>(available_virtual));
    m.set_return(1); // TRUE
}

} // namespace vitreo::winapi
