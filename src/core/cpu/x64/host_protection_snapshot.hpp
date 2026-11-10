// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file host_protection_snapshot.hpp
/// @brief Record a host span's page protections and put them back exactly.
///
/// The native backends patch guest code in place (CPUID -> UD2, Win64 segment
/// overrides): they open the span writable, edit it, and must then leave
/// every page exactly as it was. A page's protection is the guest's own
/// VirtualAlloc/VirtualProtect contract -- PAGE_EXECUTE_READWRITE stays
/// writable -- and under identity mapping it can also carry host-side arming
/// (write-watch read-only, guard-page no-access) that a protection derived
/// from the guest's view would silently disarm. So the restore replays what
/// the kernel reported per VM region, never a blanket read+execute. A blanket
/// restore made a DLL's RWX code buffer, committed before the CPUID sweep,
/// read-only; its next write was an access violation the guest never caused.

#include <mach/mach.h>
#include <mach/mach_vm.h>

#include <cstdint>
#include <vector>

namespace vitreo::cpu::x64 {

class HostProtectionSnapshot {
public:
    /// Capture the current protection of every VM region intersecting
    /// [lo, hi). Unmapped stretches are skipped (nothing to restore there).
    HostProtectionSnapshot(std::uintptr_t lo, std::uintptr_t hi) {
        mach_vm_address_t addr = lo;
        while (addr < hi) {
            mach_vm_address_t region = addr;
            mach_vm_size_t size = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t object = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &region, &size,
                               VM_REGION_BASIC_INFO_64,
                               reinterpret_cast<vm_region_info_t>(&info),
                               &count, &object) != KERN_SUCCESS ||
                size == 0 || region >= hi) {
                break;
            }
            const mach_vm_address_t begin = region > addr ? region : addr;
            const mach_vm_address_t end =
                region + size < hi ? region + size : hi;
            m_spans.push_back({begin, end, info.protection});
            addr = end;
        }
    }

    /// Put back exactly the captured protections.
    void restore() const noexcept {
        for (const Span& s : m_spans) {
            (void)mach_vm_protect(mach_task_self(), s.begin, s.end - s.begin,
                                  FALSE, s.protection);
        }
    }

private:
    struct Span {
        mach_vm_address_t begin;
        mach_vm_address_t end;
        vm_prot_t protection;
    };
    std::vector<Span> m_spans;
};

} // namespace vitreo::cpu::x64
