#include "runtime_checkpoint.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace vitreo::checkpoint {
namespace {

static_assert(std::is_trivially_copyable_v<cpu::CpuState>,
              "Checkpoint CPU payload requires a trivially copyable CpuState");

bool page_end(GuestAddr address, GuestAddr& end) {
    if (address > std::numeric_limits<GuestAddr>::max() - kGuestPageSize) {
        return false;
    }
    end = address + kGuestPageSize;
    return true;
}

} // namespace

bool capture(GuestMemory& memory, const cpu::CpuState& cpu,
             const CaptureRequest& request, Checkpoint& checkpoint,
             std::string& error) {
    Checkpoint result;
    result.architecture = request.architecture;
    result.determinism = request.determinism;
    result.modules = request.modules;
    result.handles_complete = false;

    ThreadState thread;
    thread.guest_thread_id = request.guest_thread_id;
    thread.cpu_state_abi_size = sizeof(cpu::CpuState);
    thread.cpu_state.resize(sizeof(cpu::CpuState));
    std::memcpy(thread.cpu_state.data(), &cpu, sizeof(cpu));
    result.threads.push_back(std::move(thread));

    GuestAddr search = 1;
    while (true) {
        const GuestAddr region_base = memory.next_region_base(search);
        if (region_base == 0) break;

        GuestAddr queried_base = 0;
        usize region_size = 0;
        MemoryProtection ignored = MemoryProtection::None;
        if (!memory.query_region(region_base, queried_base, region_size, ignored) ||
            queried_base != region_base || region_size == 0) {
            error = "guest memory region changed during checkpoint capture";
            return false;
        }

        const GuestAddr aligned_end =
            region_base + static_cast<GuestAddr>(
                (region_size + kGuestPageSize - 1) &
                ~(kGuestPageSize - 1));
        if (aligned_end < region_base) {
            error = "guest memory region overflows the address space";
            return false;
        }

        for (GuestAddr page_address = region_base;
             page_address < aligned_end;
             page_address += kGuestPageSize) {
            GuestAddr page_region_base = 0;
            usize page_region_size = 0;
            MemoryProtection protection = MemoryProtection::None;
            if (!memory.query_region(page_address, page_region_base,
                                     page_region_size, protection)) {
                error = "guest memory mapping changed during checkpoint capture";
                return false;
            }

            MemoryPage page;
            page.address = page_address;
            page.protection = protection;
            page.has_contents = has_flag(protection, MemoryProtection::Read);
            if (page.has_contents) {
                page.contents.resize(kGuestPageSize);
                const usize available = std::min<usize>(
                    kGuestPageSize,
                    region_size - static_cast<usize>(page_address - region_base));
                const auto read = memory.read_bytes(
                    page_address, reinterpret_cast<Byte*>(page.contents.data()),
                    available);
                if (read.is_err()) {
                    error = "failed to read mapped guest page";
                    return false;
                }
                std::fill(page.contents.begin() + static_cast<isize>(available),
                          page.contents.end(), 0);
            }
            result.pages.push_back(std::move(page));
        }

        search = aligned_end;
        if (search == 0) break;
    }

    checkpoint = std::move(result);
    error.clear();
    return true;
}

bool restore(const Checkpoint& checkpoint,
             GuestArchitecture expected_architecture,
             GuestMemory& memory, cpu::CpuState& cpu,
             std::string& error) {
    if (checkpoint.architecture != expected_architecture) {
        error = "checkpoint guest architecture does not match executable";
        return false;
    }
    if (checkpoint.threads.size() != 1) {
        error = "this checkpoint runtime supports exactly one guest thread";
        return false;
    }
    if (checkpoint.handles_complete) {
        error = "complete handle restore has no registered provider";
        return false;
    }
    const auto& thread = checkpoint.threads.front();
    if (thread.cpu_state_abi_size != sizeof(cpu::CpuState) ||
        thread.cpu_state.size() != sizeof(cpu::CpuState)) {
        error = "checkpoint CpuState ABI does not match this Vitreo build";
        return false;
    }

    for (const auto& page : checkpoint.pages) {
        GuestAddr ignored_end = 0;
        if (!page_end(page.address, ignored_end)) {
            error = "checkpoint page address overflows guest address space";
            return false;
        }

        if (!memory.is_mapped(page.address, kGuestPageSize)) {
            const auto mapped = memory.map_region(
                page.address, kGuestPageSize,
                page.has_contents ? MemoryProtection::ReadWrite
                                  : page.protection);
            if (mapped.is_err()) {
                error = "failed to recreate checkpoint memory mapping";
                return false;
            }
        } else if (page.has_contents) {
            const auto writable = memory.protect_region(
                page.address, kGuestPageSize, MemoryProtection::ReadWrite);
            if (writable.is_err()) {
                error = "failed to make checkpoint page writable";
                return false;
            }
        }

        if (page.has_contents) {
            const auto written = memory.write_bytes(
                page.address,
                reinterpret_cast<const Byte*>(page.contents.data()),
                page.contents.size());
            if (written.is_err()) {
                error = "failed to restore checkpoint page contents";
                return false;
            }
        }
        const auto protected_result = memory.protect_region(
            page.address, kGuestPageSize, page.protection);
        if (protected_result.is_err()) {
            error = "failed to restore checkpoint page protection";
            return false;
        }
    }

    std::memcpy(&cpu, thread.cpu_state.data(), sizeof(cpu));
    error.clear();
    return true;
}

} // namespace vitreo::checkpoint
