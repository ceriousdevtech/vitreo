// SPDX-License-Identifier: LGPL-2.1-only

/// @file thread_apis.cpp
/// @brief Thread banner APIs from kernel32_ext.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

#include "winapi/kernel32/system/processor_topology.hpp"
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
#include <pthread.h>
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
// Thread
// =============================================================================

void Kernel32ExtManager::impl_SetThreadPriority(Marshaler& m) {
    // BOOL SetThreadPriority(HANDLE hThread, int nPriority)
    const Handle h = static_cast<Handle>(m.arg(0));
    const i32 priority = static_cast<i32>(static_cast<u32>(m.arg(1)));
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const u32 error = m_thread_manager->set_thread_priority(h, priority);
    if (error != 0) set_last_error(error);
    m.set_return(error == 0 ? 1u : 0u);
}

void Kernel32ExtManager::impl_GetThreadPriority(Marshaler& m) {
    // int GetThreadPriority(HANDLE hThread)
    const Handle h = static_cast<Handle>(m.arg(0));
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0x7FFFFFFFu);
        return;
    }
    m.set_return(static_cast<u64>(static_cast<u32>(
        m_thread_manager->get_thread_priority(h))));
}

void Kernel32ExtManager::impl_GetCurrentProcessorNumber(Marshaler& m) {
    // DWORD GetCurrentProcessorNumber(void)
    // Learn: the processor the calling thread is running on. That is the host
    // processor macOS scheduled it on, or the one its affinity pins it to
    // (platform::current_processor), the same processor CPUID reports.
    m.set_return(platform::current_processor());
}

void Kernel32ExtManager::impl_GetCurrentProcessorNumberEx(Marshaler& m) {
    // VOID GetCurrentProcessorNumberEx(PPROCESSOR_NUMBER ProcNumber)
    // PROCESSOR_NUMBER { WORD Group; BYTE Number; BYTE Reserved; }; one group.
    const GuestAddr out = m.arg_ptr(0);
    if (out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    (void)m.write_u16(out + 0, 0);
    (void)m.write_u8(out + 2, static_cast<u8>(platform::current_processor()));
    (void)m.write_u8(out + 3, 0);
    m.set_return(1);
}

// NT_TIB sits at offset 0 of the TEB. StackBase is the high end of the stack
// (one past the last usable byte) and StackLimit the low end; the loader
// writes both in teb_setup.cpp / guest_thread_context.cpp.
//   x64: +0x08 StackBase, +0x10 StackLimit
//   x86: +0x04 StackBase, +0x08 StackLimit
bool Kernel32ExtManager::current_thread_stack_bounds(Marshaler& m, u64& low,
                                                     u64& high) const {
    auto* cpu = m.cpu();
    if (cpu == nullptr) return false;
    const bool is64 = cpu->is_64bit();
    const GuestAddr teb = static_cast<GuestAddr>(
        is64 ? cpu->seg.gs_desc.base : cpu->seg.fs_desc.base);
    if (teb == 0) return false;

    if (is64) {
        auto base = m.memory().read_u64(teb + 0x08);
        auto limit = m.memory().read_u64(teb + 0x10);
        if (base.is_err() || limit.is_err()) return false;
        high = base.value();
        // Learn: LowLimit is the lower boundary of the stack the system
        // ALLOCATED -- TEB DeallocationStack, not StackLimit, which is only
        // the committed bottom and sits above the overflow guard.
        auto allocation_base = m.memory().read_u64(teb + 0x1478);
        low = allocation_base.is_ok() && allocation_base.value() != 0
                  ? allocation_base.value()
                  : limit.value();
    } else {
        auto base = m.memory().read_u32(teb + 0x04);
        auto limit = m.memory().read_u32(teb + 0x08);
        if (base.is_err() || limit.is_err()) return false;
        high = base.value();
        low = limit.value();
    }
    // A TEB whose stack fields were never filled in must not be reported as a
    // zero-length stack at address 0: callers use these bounds to decide
    // whether ESP/RSP is on the thread stack.
    return high > low && low != 0;
}

void Kernel32ExtManager::impl_GetCurrentThreadStackLimits(Marshaler& m) {
    // VOID GetCurrentThreadStackLimits(PULONG_PTR LowLimit,
    //                                  PULONG_PTR HighLimit)
    //
    // Learn: "Retrieves the boundaries of the stack that was allocated by the
    // system for the current thread." No return value and no failure path —
    // callers compare the current stack pointer against the two bounds, so
    // writing nothing (which the missing-export stub did) leaves them
    // comparing against uninitialized stack.
    const GuestAddr low_ptr = m.arg_ptr(0);
    const GuestAddr high_ptr = m.arg_ptr(1);

    u64 low = 0;
    u64 high = 0;
    (void)current_thread_stack_bounds(m, low, high);

    if (low_ptr != 0) (void)m.write_ptr(low_ptr, low);
    if (high_ptr != 0) (void)m.write_ptr(high_ptr, high);
    VITREO_TRACE("[k32ext] GetCurrentThreadStackLimits() -> [0x%llx, 0x%llx)\n",
                 static_cast<unsigned long long>(low),
                 static_cast<unsigned long long>(high));
}

void Kernel32ExtManager::impl_SetThreadStackGuarantee(Marshaler& m) {
    // BOOL SetThreadStackGuarantee(PULONG StackSizeInBytes)
    //
    // Learn: in/out. On return the parameter holds the PREVIOUS guarantee.
    // Zero in queries without changing anything. A request smaller than the
    // current guarantee succeeds but is ignored — the guarantee can never
    // shrink. The value cannot exceed the reserved stack size.
    //
    // The page-rounded per-thread guarantee is recorded, written to the TEB,
    // and -- the part that makes the promise true -- the stack guard is moved
    // up so the overflow leaves the handler that much stack (see below).
    const GuestAddr size_ptr = m.arg_ptr(0);
    if (size_ptr == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    auto requested_read = m.read_u32(size_ptr);
    if (requested_read.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    const u32 requested = requested_read.value();

    // The guarantee is per thread, so key it off the calling thread's TEB.
    auto* cpu = m.cpu();
    const GuestAddr teb =
        cpu == nullptr ? 0
                       : static_cast<GuestAddr>(cpu->is_64bit()
                                                    ? cpu->seg.gs_desc.base
                                                    : cpu->seg.fs_desc.base);

    // "This value cannot be larger than the reserved stack size."
    if (requested != 0) {
        u64 low = 0;
        u64 high = 0;
        if (current_thread_stack_bounds(m, low, high) &&
            static_cast<u64>(requested) >= (high - low)) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
    }

    u32 previous = 0;
    u32 effective = 0;
    {
        std::lock_guard<std::mutex> lock(m_thread_stack_guarantee_mutex);
        auto& stored = m_thread_stack_guarantee[teb];
        previous = stored;
        // Page-granular, and never shrinks.
        if (requested > stored) {
            constexpr u32 kPageSize = 0x1000u;
            stored = (requested + (kPageSize - 1)) & ~(kPageSize - 1);
        }
        effective = stored;
    }
    if (cpu != nullptr) cpu->stack_guarantee = effective;

    // Measured on Windows (tools/oracle_probe/stack_guarantee_probe.c): the
    // value lands in TEB.GuaranteedStackBytes (+0x1748), and the guard region
    // -- where STATUS_STACK_OVERFLOW is raised -- grows to cover it. Its top
    // is DeallocationStack + 0x2000 + max(0x2000, guarantee): + 0x4000 by
    // default, + 0x13000 for a 0x11000 guarantee, both measured, so the
    // handler gets the guarantee plus a page to run on. Recording the value without moving the guard is what let Mono's
    // overflow handler (which asks for this) overrun a 12 KiB recovery area.
    if (cpu != nullptr && cpu->is_64bit() && teb != 0) {
        (void)m.write_u32(teb + 0x1748, effective);
        const u64 deallocation = m.read_u64(teb + 0x1478).value_or(0);
        const u64 limit = m.read_u64(teb + 0x10).value_or(0);
        if (deallocation != 0 && m_virtual_memory_manager != nullptr) {
            const u64 guard_top =
                deallocation + 0x2000 + std::max<u64>(0x2000, effective);
            if (cpu->sp() <= guard_top) {
                // The thread already stands where the guard would go.
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            u32 old_protect = 0;
            if (m_virtual_memory_manager->virtual_protect(
                    deallocation + 0x1000, guard_top - (deallocation + 0x1000),
                    PAGE_NOACCESS, &old_protect)) {
                if (limit < guard_top) (void)m.write_u64(teb + 0x10, guard_top);
            }
        }
    }

    (void)m.write_u32(size_ptr, previous);
    VITREO_TRACE("[k32ext] SetThreadStackGuarantee(%u) -> prev=%u\n",
                 requested, previous);
    m.set_return(1); // TRUE
}

void Kernel32ExtManager::impl_SetThreadAffinityMask(Marshaler& m) {
    // DWORD_PTR SetThreadAffinityMask(HANDLE hThread, DWORD_PTR dwThreadAffinityMask)
    // Measured (tools/oracle_probe/processor_topology_probe "sta", x86 = x64):
    // a mask naming no processor of the process affinity (0 included) fails
    // with ERROR_INVALID_PARAMETER; a mask partly outside it succeeds and keeps
    // the part inside; success returns the previous mask and leaves the last
    // error alone.
    const Handle thread = static_cast<Handle>(m.arg(0));
    const u64 requested =
        processor_topology::guest_affinity(m, m.arg(1)) & process_affinity_mask(m);
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (requested == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    u64 previous = 0;
    if (const u32 error = m_thread_manager->set_thread_affinity(thread, requested, previous); error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    m.set_return(effective_thread_affinity(m, previous));
}

void Kernel32ExtManager::impl_SetThreadIdealProcessor(Marshaler& m) {
    // DWORD SetThreadIdealProcessor(HANDLE hThread, DWORD dwIdealProcessor)
    // Learn: returns the previous ideal processor, (DWORD)-1 on failure;
    // MAXIMUM_PROCESSORS queries. Measured ("stip", x86 = x64): any number at
    // or above the processor count also leaves the ideal processor unchanged
    // and returns it; the last error is untouched.
    const Handle thread = static_cast<Handle>(m.arg(0));
    const u32 requested = static_cast<u32>(m.arg(1));
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    u32 previous = 0;
    const bool set = requested < processor_topology::processor_count();
    if (const u32 error = m_thread_manager->thread_ideal_processor(
            thread, ThreadManager::THREAD_SET_INFORMATION, set, requested, previous);
        error != 0) {
        set_last_error(error);
        m.set_return(0xFFFFFFFFu);
        return;
    }
    m.set_return(previous);
}

void Kernel32ExtManager::impl_GetThreadIdealProcessorEx(Marshaler& m) {
    // BOOL GetThreadIdealProcessorEx(HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor)
    // Learn: the handle needs THREAD_QUERY_INFORMATION.
    const Handle thread = static_cast<Handle>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0 || m_thread_manager == nullptr) {
        set_last_error(out == 0 ? error_codes::ERROR_INVALID_PARAMETER : error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    u32 ideal = 0;
    if (const u32 error = m_thread_manager->thread_ideal_processor(
            thread, ThreadManager::THREAD_QUERY_INFORMATION, false, 0, ideal);
        error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    (void)m.write_u16(out + 0, 0);
    (void)m.write_u8(out + 2, static_cast<u8>(ideal));
    (void)m.write_u8(out + 3, 0);
    m.set_return(1);
}

void Kernel32ExtManager::impl_SetThreadIdealProcessorEx(Marshaler& m) {
    // BOOL SetThreadIdealProcessorEx(HANDLE hThread, PPROCESSOR_NUMBER lpIdealProcessor,
    //                                PPROCESSOR_NUMBER lpPreviousIdealProcessor)
    // Measured ("stipex", x86 = x64): a group other than 0, a number at or
    // above the processor count, or a non-zero Reserved byte fails with
    // ERROR_INVALID_PARAMETER and leaves the previous-processor buffer alone;
    // lpPreviousIdealProcessor may be NULL. A NULL lpIdealProcessor is not
    // measured and is refused the same way.
    const Handle thread = static_cast<Handle>(m.arg(0));
    const GuestAddr wanted = m.arg_ptr(1);
    const GuestAddr previous_out = m.arg_ptr(2);
    if (m_thread_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    bool valid = false;
    u32 number = 0;
    if (wanted != 0) {
        const auto group = m.read_u16(wanted);
        const auto processor = m.read_u8(wanted + 2);
        const auto reserved = m.read_u8(wanted + 3);
        valid = group.is_ok() && processor.is_ok() && reserved.is_ok() && group.value() == 0 &&
                reserved.value() == 0 && processor.value() < processor_topology::processor_count();
        if (valid) number = processor.value();
    }
    if (!valid) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    u32 previous = 0;
    if (const u32 error = m_thread_manager->thread_ideal_processor(
            thread, ThreadManager::THREAD_SET_INFORMATION, true, number, previous);
        error != 0) {
        set_last_error(error);
        m.set_return(0);
        return;
    }
    if (previous_out != 0) {
        (void)m.write_u16(previous_out + 0, 0);
        (void)m.write_u8(previous_out + 2, static_cast<u8>(previous));
        (void)m.write_u8(previous_out + 3, 0);
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_OpenThread(Marshaler& m) {
    // HANDLE OpenThread(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwThreadId)
    // Learn: handle is granted only dwDesiredAccess; bInheritHandle sets
    // HANDLE_FLAG_INHERIT (thread_lifecycle_probe OPEN_INHERIT / SUSPEND_NO_ACCESS).
    const u32 desired_access = static_cast<u32>(m.arg(0));
    const bool inherit = m.arg32(1) != 0;
    const u32 thread_id = static_cast<u32>(m.arg(2));
    const u32 flags = inherit ? HandleFlags::Inherit : 0;

    if (m_thread_manager != nullptr) {
        Handle h = m_thread_manager->open_thread(thread_id, desired_access, flags);
        if (h != 0) {
            VITREO_TRACE("[kernel32_ext] OpenThread(access=0x%x, inherit=%u, tid=%u) -> 0x%x\n",
                        desired_access, inherit ? 1u : 0u, thread_id, h);
            m.set_return(static_cast<u64>(h));
            return;
        }
    }

    // CreateProcess primary threads live outside ThreadManager (child process).
    {
        std::lock_guard lock(m_external_thread_mutex);
        const auto by_tid = m_external_primary_by_tid.find(thread_id);
        if (by_tid != m_external_primary_by_tid.end() &&
            m_handle_table != nullptr) {
            const Handle h = m_handle_table->allocate(
                HandleType::Thread, nullptr, flags, desired_access);
            if (h != NULL_HANDLE) {
                m_external_primary_handles.emplace(h, thread_id);
                VITREO_TRACE(
                    "[kernel32_ext] OpenThread(tid=%u) -> 0x%x (external)\n",
                    thread_id, h);
                m.set_return(static_cast<u64>(h));
                return;
            }
        }
    }

    VITREO_TRACE("[kernel32_ext] OpenThread(tid=%u) -> NULL (not found)\n", thread_id);
    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    m.set_return(0);
}

} // namespace vitreo::winapi
