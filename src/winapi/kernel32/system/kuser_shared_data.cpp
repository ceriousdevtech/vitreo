/// @file kuser_shared_data.cpp
/// @brief KUSER_SHARED_DATA page at 0x7FFE0000.

#include "core/pe/os_version.hpp"
#include "kuser_shared_data.hpp"

#include "core/memory/guest_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "winapi/kernel32/system/time.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"
#include "winapi/kernel32/system/processor_topology.hpp"
#include "platform/host_hardware.hpp"
#include <cstdio>
#include <cstdlib>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace vitreo::winapi {
namespace {

constexpr u64 kEpochOffset = 116444736000000000ULL;
constexpr u32 kOffTickCountMultiplier = 0x004;
constexpr u32 kOffInterruptTime = 0x008;
constexpr u32 kOffSystemTime = 0x014;
constexpr u32 kOffTimeZoneBias = 0x020;
constexpr u32 kOffImageNumberLow = 0x02C;
constexpr u32 kOffImageNumberHigh = 0x02E;
constexpr u32 kOffNtSystemRoot = 0x030;
constexpr u32 kOffNtBuildNumber = 0x260;
constexpr u32 kOffNtMajorVersion = 0x26C;
constexpr u32 kOffNtMinorVersion = 0x270;
constexpr u32 kOffProcessorFeatures = 0x274;
// Offsets from the SDK's ntddk.h KUSER_SHARED_DATA (identical on x86 and x64).
// 0x2D4 is KdDebuggerEnabled and 0x2D5 NXSupportPolicy, which the page count
// used to overwrite.
constexpr u32 kOffNumberOfPhysicalPages = 0x2E8;
constexpr u32 kOffActiveProcessorCount = 0x3C0;
constexpr u32 kOffActiveGroupCount = 0x3C4;
constexpr u32 kOffQpcFrequency = 0x300;
constexpr u32 kOffSystemCall = 0x308;
constexpr u32 kOffTickCountQuad = 0x320;
constexpr u32 kOffCookie = 0x330;

// TickCount counts 15.625 ms clock ticks and TickCountMultiplier converts
// them for GetTickCount: ms = (ticks * multiplier) >> 24. Measured on Windows
// 11 x86 and x64 (tools/oracle_probe/kuser_clock_probe.expected), as is the
// deprecated 32-bit TickCountLow at +0x000 staying zero.
constexpr u32 kTickCountMultiplier = 0x0FA00000u;  // 15.625 * 2^24
constexpr u64 kTickInterval100ns = 156250;

// The kernel advances InterruptTime, SystemTime and TickCount from its clock
// interrupt, independent of every user thread: readers that never enter the
// system (inlined GetTickCount, QueryInterruptTime) still see them move, in
// steps of at most one 15.625 ms tick (kuser_clock_probe.expected). A host
// thread stands in for that interrupt. 4 ms keeps every step well inside one
// tick at 250 wakeups/s per process.
constexpr auto kClockPeriod = std::chrono::milliseconds(4);

constexpr u16 kImageFileMachineI386 = 0x014C;
constexpr u16 kImageFileMachineAmd64 = 0x8664;

// The address space whose page the clock source keeps current, and its host
// view. Guarded by clock_mutex(), which also serialises writers. The mutex is
// never destroyed: the clock thread is detached and keeps running while
// static destructors execute at process exit.
std::mutex& clock_mutex() {
    static std::mutex* const mutex = new std::mutex;
    return *mutex;
}
GuestMemory* g_memory = nullptr;
u8* g_page = nullptr;

// KSYSTEM_TIME writer protocol: High2Time, then LowPart and High1Time. A
// reader takes High1Time, LowPart, High2Time and retries until the highs
// agree, so it can never accept a torn value; x64 readers that load
// LowPart:High1Time as one quadword see a single 64-bit store.
void write_ksystem_time(u8* page, u32 offset, u64 time_100ns) {
    const u32 high = static_cast<u32>(time_100ns >> 32);
    __atomic_store_n(reinterpret_cast<u32*>(page + offset + 8), high,
                     __ATOMIC_RELEASE);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(page + offset, &time_100ns, sizeof(time_100ns));
}

u64 system_time_filetime() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto hns = std::chrono::duration_cast<
        std::chrono::duration<u64, std::ratio<1, 10000000>>>(now);
    return hns.count() + kEpochOffset;
}

// Caller holds clock_mutex(). InterruptTime is the coarse clock
// QueryInterruptTime returns; TickCount is the same clock in whole ticks.
void write_clocks_locked() {
    if (g_page == nullptr) return;
    const u64 interrupt_100ns = tick_count_ms() * 10000ULL;
    write_ksystem_time(g_page, kOffInterruptTime, interrupt_100ns);
    write_ksystem_time(g_page, kOffSystemTime, system_time_filetime());
    write_ksystem_time(g_page, kOffTickCountQuad,
                       interrupt_100ns / kTickInterval100ns);
}

void forget_memory(GuestMemory* memory) {
    std::lock_guard<std::mutex> lock(clock_mutex());
    if (g_memory == memory) {
        g_memory = nullptr;
        g_page = nullptr;
    }
}

void start_clock_source() {
    static std::once_flag once;
    std::call_once(once, [] {
        (void)GuestMemory::add_teardown_hook(&forget_memory);
        std::thread([] {
            for (;;) {
                {
                    std::lock_guard<std::mutex> lock(clock_mutex());
                    write_clocks_locked();
                }
                std::this_thread::sleep_for(kClockPeriod);
            }
        }).detach();
    });
}

}  // namespace

bool install_kuser_shared_data(GuestMemory& memory, bool long_mode) {
    auto mapped = memory.map_region(pe::SHARED_USER_DATA_ADDRESS,
                                    pe::SHARED_USER_DATA_SIZE,
                                    MemoryProtection::ReadWrite);
    if (mapped.is_err()) {
        return false;
    }

    for (u32 i = 0; i < pe::SHARED_USER_DATA_SIZE; ++i) {
        (void)memory.write_u8(pe::SHARED_USER_DATA_ADDRESS + i, 0);
    }

    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffTickCountMultiplier,
                           kTickCountMultiplier);

    (void)memory.write_u16(pe::SHARED_USER_DATA_ADDRESS + kOffImageNumberLow,
                           kImageFileMachineI386);
    (void)memory.write_u16(pe::SHARED_USER_DATA_ADDRESS + kOffImageNumberHigh,
                           kImageFileMachineAmd64);

    const char root[] = "C:\\Windows";
    for (u32 i = 0; root[i] != '\0'; ++i) {
        (void)memory.write_u16(pe::SHARED_USER_DATA_ADDRESS + kOffNtSystemRoot + i * 2,
                               static_cast<u16>(root[i]));
    }

    // SystemCall selects which entry instruction ntdll's x64 service veneers
    // use: 0 for `syscall`, 1 for the `int 2Eh` fallback. Windows leaves it 0.
    // On an x86_64 host the guest veneer runs on the real CPU, where macOS
    // ignores a `syscall` carrying a Windows service id -- the call would
    // return the id as its NTSTATUS having done nothing. Select the trapping
    // entry there, which the native64 fault path dispatches. A decoding
    // backend recognises the syscall itself and needs no redirection.
#if defined(__x86_64__)
    (void)memory.write_u8(pe::SHARED_USER_DATA_ADDRESS + kOffSystemCall, 1);
#endif

    // NtBuildNumber carries the plain build, no flag bits
    // (os_version_sources_probe "KUSER": raw high=0).
    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffNtBuildNumber, pe::kOsBuildNumber);
    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffNtMajorVersion, pe::kOsMajorVersion);
    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffNtMinorVersion, pe::kOsMinorVersion);

    // ProcessorFeatures[PROCESSOR_FEATURE_MAX]: the same answers
    // IsProcessorFeaturePresent gives (Windows publishes one array for both;
    // tools/oracle_probe/processor_features_probe.expected "ksd-eq").
    for (u32 index = 0; index < pf::PROCESSOR_FEATURE_MAX; ++index) {
        (void)memory.write_u8(
            pe::SHARED_USER_DATA_ADDRESS + kOffProcessorFeatures + index,
            processor_feature_present_for_cpu(index, long_mode) ? 1 : 0);
    }

    // The host's memory in 4 KiB pages, and its processors (one group).
    const u64 pages = platform::host_hardware().memory.total_bytes / 4096u;
    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffNumberOfPhysicalPages,
                           static_cast<u32>(std::min<u64>(pages, 0xFFFFFFFFu)));
    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffActiveProcessorCount,
                           processor_topology::processor_count());
    (void)memory.write_u8(pe::SHARED_USER_DATA_ADDRESS + kOffActiveGroupCount, 1);

    (void)memory.write_u64(pe::SHARED_USER_DATA_ADDRESS + kOffQpcFrequency,
                           kQpcHz);
    (void)memory.write_u32(pe::SHARED_USER_DATA_ADDRESS + kOffCookie, 0xA5A5A5A5u);

    u8* const page = reinterpret_cast<u8*>(
        memory.host_ptr_mut(pe::SHARED_USER_DATA_ADDRESS,
                            pe::SHARED_USER_DATA_SIZE));
    if (page == nullptr) return false;
    {
        std::lock_guard<std::mutex> lock(clock_mutex());
        g_memory = &memory;
        g_page = page;
        write_clocks_locked();
    }
    start_clock_source();
    return true;
}

void refresh_kuser_shared_data() {
    std::lock_guard<std::mutex> lock(clock_mutex());
    write_clocks_locked();
}

}  // namespace vitreo::winapi
