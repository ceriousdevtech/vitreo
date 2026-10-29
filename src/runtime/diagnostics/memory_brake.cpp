// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "runtime/diagnostics/memory_brake.hpp"

#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace vitreo::diagnostics {

bool memory_brake_should_fire(u64 host_available, u64 floor, u64 footprint,
                              u64 physical) noexcept {
    if (floor == 0 || physical == 0) return false;
    return host_available < floor / 2 && footprint > physical / 4;
}

namespace {

u64 own_footprint() noexcept {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO,
                  reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return info.phys_footprint;
}

u64 physical_bytes() noexcept {
    u64 bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0) return 0;
    return bytes;
}

}  // namespace

void install_memory_brake() {
    const u64 physical = physical_bytes();
    if (memory::host_memory_floor_bytes() == 0 || physical == 0) return;
    std::thread([physical] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const u64 floor = memory::host_memory_floor_bytes();
            const u64 available = memory::host_available_bytes();
            if (floor == 0 || available >= floor / 2) continue;
            const u64 footprint = own_footprint();
            if (!memory_brake_should_fire(available, floor, footprint,
                                          physical)) {
                continue;
            }
            std::fprintf(stderr,
                         "[vitreo] MEMORY BRAKE: host available %llu MiB is "
                         "below half the %llu MiB floor and this process "
                         "holds %llu MiB of %llu MiB RAM -- terminating it "
                         "before macOS freezes the machine\n",
                         static_cast<unsigned long long>(available >> 20),
                         static_cast<unsigned long long>(floor >> 20),
                         static_cast<unsigned long long>(footprint >> 20),
                         static_cast<unsigned long long>(physical >> 20));
            std::fflush(stderr);
            // STATUS_NO_MEMORY's low byte; _exit skips atexit handlers that
            // could allocate while the host has nothing left to give.
            ::_exit(0x17);
        }
    }).detach();
}

}  // namespace vitreo::diagnostics
