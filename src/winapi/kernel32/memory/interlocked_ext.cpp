// SPDX-License-Identifier: LGPL-2.1-only

/// @file interlocked_ext.cpp
/// @brief Interlocked and GlobalMemoryStatus stubs.
///
/// CPU-Z Interlocked / Memory banner.
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
// CPU-Z: Interlocked / Memory
// =============================================================================

void Kernel32ExtManager::impl_InterlockedExchangeAdd(Marshaler& m) {
    GuestAddr ptr = m.arg_ptr(0);
    u32 value = m.arg(1);

    u32 old_val = 0;
    if (auto* hp = m_memory.host_ptr_mut(ptr, 4)) {
        // Lock-free atomic add (returns previous value to match Win32 semantics).
        old_val = __atomic_fetch_add(reinterpret_cast<u32*>(hp), value, __ATOMIC_SEQ_CST);
    } else {
        auto old_r = m_memory.read_u32(ptr);
        old_val = old_r.is_ok() ? old_r.value() : 0;
        (void)m_memory.write_u32(ptr, old_val + value);
    }

    VITREO_TRACE("[k32ext] InterlockedExchangeAdd(*0x%llX, %u) -> %u\n",
                 static_cast<unsigned long long>(ptr), value, old_val);
    m.set_return(old_val);
}

void Kernel32ExtManager::impl_GlobalMemoryStatus(Marshaler& m) {
    // void GlobalMemoryStatus(LPMEMORYSTATUS lpBuffer)
    // MEMORYSTATUS is 32 bytes (8 DWORDs)
    GuestAddr buf_ptr = m.arg_ptr(0);
    if (buf_ptr == 0) {
        m.set_return(0);
        return;
    }

    // Get real memory info from macOS
    u64 total_phys = 0;
    size_t sz = sizeof(total_phys);
    sysctlbyname("hw.memsize", &total_phys, &sz, nullptr, 0);

    u64 avail_phys = 0;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    vm_statistics64_data_t vm_stat;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm_stat),
                          &count) == KERN_SUCCESS) {
        avail_phys = static_cast<u64>(vm_stat.free_count + vm_stat.inactive_count)
                     * static_cast<u64>(vm_page_size);
    }

    // MEMORYSTATUS fields are DWORD. Learn / KB 274558: a non-LAA 32-bit
    // process on a machine with 2–4 GB sees 2 GB in dwTotalPhys / dwAvailPhys.
    // Above 4 GB the API may store –1 (overflow); that value is signed-less
    // than any real allocation and is documented as incorrect. Cap 32-bit
    // callers at 2 GB and derive load/avail from guest-mapped bytes so host
    // file-cache pressure is not reported as process OOM.
    constexpr u32 kX86NonLaaPhysCap = 0x80000000u;
    u32 total_phys_32 = 0;
    u32 avail_phys_32 = 0;
    u32 mem_load = 50;
    u32 total_page_32 = 0;
    u32 avail_page_32 = 0;
    if (!m.cpu_is_64bit()) {
        const u32 cap = total_phys > kX86NonLaaPhysCap
                            ? kX86NonLaaPhysCap
                            : static_cast<u32>(total_phys);
        const u64 guest_used = m_memory.total_mapped_size();
        const u32 used = guest_used > cap ? cap : static_cast<u32>(guest_used);
        total_phys_32 = cap;
        avail_phys_32 = cap - used;
        mem_load = cap == 0 ? 0 : static_cast<u32>((used * 100ull) / cap);
        total_page_32 = cap;
        avail_page_32 = avail_phys_32;
    } else {
        total_phys_32 = (total_phys > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFFu
                            : static_cast<u32>(total_phys);
        avail_phys_32 = (avail_phys > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFFu
                            : static_cast<u32>(avail_phys);
        mem_load = (total_phys > 0)
            ? static_cast<u32>(((total_phys - avail_phys) * 100) / total_phys)
            : 50;
        const u64 total_page = total_phys * 2;
        total_page_32 = (total_page > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFFu
                            : static_cast<u32>(total_page);
        const u64 avail_page = avail_phys * 2;
        avail_page_32 = (avail_page > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFFu
                            : static_cast<u32>(avail_page);
    }

    // Learn MEMORYSTATUS: dwTotalVirtual is this process's user-mode VA
    // size; dwAvailVirtual is the unreserved/uncommitted remainder — not a
    // hardcoded 2 GB / 1.5 GB pair. A caller that HeapAllocs dwAvailVirtual
    // then retries a failed ~2 GB reserve forever if we lie.
    const u64 total_virtual =
        m_memory.maximum_user_address() != 0
            ? static_cast<u64>(m_memory.maximum_user_address())
            : (m.cpu_is_64bit() ? 0x00007FFFFFFEFFFFull
                                : 0x000000007FFEFFFFull);
    const u64 mapped = static_cast<u64>(m_memory.total_mapped_size());
    const u64 available_virtual =
        mapped >= total_virtual ? 0u : total_virtual - mapped;
    const auto to_dword = [](u64 v) -> u32 {
        return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(v);
    };

    // dwLength (0)
    (void)m_memory.write_u32(buf_ptr + 0, 32);
    // dwMemoryLoad (4)
    (void)m_memory.write_u32(buf_ptr + 4, mem_load);
    // dwTotalPhys (8)
    (void)m_memory.write_u32(buf_ptr + 8, total_phys_32);
    // dwAvailPhys (12)
    (void)m_memory.write_u32(buf_ptr + 12, avail_phys_32);
    // dwTotalPageFile (16)
    (void)m_memory.write_u32(buf_ptr + 16, total_page_32);
    // dwAvailPageFile (20)
    (void)m_memory.write_u32(buf_ptr + 20, avail_page_32);
    (void)m_memory.write_u32(buf_ptr + 24, to_dword(total_virtual));
    (void)m_memory.write_u32(buf_ptr + 28, to_dword(available_virtual));

    VITREO_TRACE("[k32ext] GlobalMemoryStatus: total=%uMB avail=%uMB load=%u%%\n",
                 total_phys_32 / (1024*1024), avail_phys_32 / (1024*1024), mem_load);
    m.set_return(0); // void function, no meaningful return
}

} // namespace vitreo::winapi
