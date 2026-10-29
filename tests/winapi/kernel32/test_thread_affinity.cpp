/// @file test_thread_affinity.cpp
/// @brief Thread affinity, ideal processor and current processor (kernel32).
///
/// Every expected value is a row of tools/oracle_probe/processor_topology_probe
/// {32,64}.expected ("sta", "gtga", "stga", "stip", "stipex"), measured on
/// Windows 11, replayed on a 14-processor host (an M4 Pro profile), so masks
/// read 0x3FFF where the 16-processor oracle printed 0xFFFF.

#include <gtest/gtest.h>

#include "capsule/vfs/vfs.hpp"
#include "core/cpu/cpuid_synth.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "platform/host_hardware.hpp"
#include "platform/host_hardware_fixtures.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kData = 0x20000;
constexpr GuestAddr kStack = 0x30000;
constexpr u64 kCurrentThread = 0xFFFFFFFFFFFFFFFEull;  // GetCurrentThread()

const ExportEntry* find_export(const DllExports& e, std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return it == e.entries.end() ? nullptr : &*it;
}

class ThreadAffinityTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        platform::set_host_hardware_for_testing(&m_host);
        ASSERT_TRUE(memory.map_region(kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
        if (wide()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = kStack + 0x800;
        marshaler.attach_cpu(cpu);
        mgr.set_handle_table(&handles);
        mgr.set_thread_manager(&threads);
        exports = create_kernel32_ext_thunks(mgr);
    }
    void TearDown() override {
        platform::bind_current_thread_affinity(nullptr);
        platform::set_host_hardware_for_testing(nullptr);
    }

    bool wide() const { return GetParam(); }
    u64 outside() const { return wide() ? (1ull << 63) : (1ull << 31); }

    /// Calls @p name with up to three arguments in the guest's ABI.
    u64 call(std::string_view name, u64 a0 = 0, u64 a1 = 0, u64 a2 = 0) {
        const ExportEntry* entry = find_export(exports, name);
        EXPECT_NE(entry, nullptr) << name;
        if (entry == nullptr) return 0;
        if (wide()) {
            cpu.gpr.rcx = a0; cpu.gpr.rdx = a1; cpu.gpr.r8 = a2;
        } else {
            (void)memory.write_u32(kStack + 0x804, static_cast<u32>(a0));
            (void)memory.write_u32(kStack + 0x808, static_cast<u32>(a1));
            (void)memory.write_u32(kStack + 0x80C, static_cast<u32>(a2));
        }
        entry->handler(marshaler);
        return wide() ? cpu.gpr.rax : (cpu.gpr.rax & 0xFFFFFFFFull);
    }

    u64 mask_at(GuestAddr at) {
        return wide() ? memory.read_u64(at).value() : memory.read_u32(at).value();
    }
    u16 group_at(GuestAddr at) { return memory.read_u16(at + (wide() ? 8 : 4)).value(); }
    void put_affinity(GuestAddr at, u64 mask, u16 group, u16 reserved0 = 0) {
        std::vector<u8> zero(16, 0);
        (void)memory.write_bytes(at, zero.data(), zero.size());
        if (wide()) (void)memory.write_u64(at, mask); else (void)memory.write_u32(at, static_cast<u32>(mask));
        (void)memory.write_u16(at + (wide() ? 8 : 4), group);
        (void)memory.write_u16(at + (wide() ? 10 : 6), reserved0);
    }
    u64 current_mask() {
        EXPECT_EQ(call("GetThreadGroupAffinity", kCurrentThread, kData + 0x100), 1u);
        return mask_at(kData + 0x100);
    }

    platform::HostHardwareProfile m_host = test::m4_pro_profile();
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    ThreadManager threads{handles, 1};
    Kernel32ExtManager mgr{memory, vfs};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

TEST_P(ThreadAffinityTest, SetThreadAffinityMask) {
    set_last_error(0x1234);
    EXPECT_EQ(call("SetThreadAffinityMask", kCurrentThread, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    set_last_error(0x1234);
    EXPECT_EQ(call("SetThreadAffinityMask", kCurrentThread, outside()), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    set_last_error(0x1234);
    EXPECT_EQ(call("SetThreadAffinityMask", kCurrentThread, 0x2), 0x3FFFu);  // previous: the process mask
    EXPECT_EQ(get_last_error(), 0x1234u);
    EXPECT_EQ(call("SetThreadAffinityMask", kCurrentThread, 0x3FFF | outside()), 0x2u);
    EXPECT_EQ(current_mask(), 0x3FFFu);  // the part inside the process affinity
    set_last_error(0x1234);
    EXPECT_EQ(call("SetThreadAffinityMask", 0x4444, 0x1), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_P(ThreadAffinityTest, GroupAffinityLayoutAndRules) {
    // GetThreadGroupAffinity: KAFFINITY then WORD Group at 4 (x86) / 8 (x64).
    (void)memory.write_u16(kData + 0x100 + (wide() ? 8 : 4), 0xBEEF);
    EXPECT_EQ(current_mask(), 0x3FFFu);
    EXPECT_EQ(group_at(kData + 0x100), 0u);

    struct Case { const char* name; u64 mask; u16 group; u16 reserved; bool ok; u64 now; };
    const Case cases[] = {
        {"zero", 0, 0, 0, true, 0x3FFF},          // succeeds, changes nothing
        {"group1", 0x1, 1, 0, false, 0x3FFF},
        {"reserved", 0x1, 0, 1, false, 0x3FFF},
        {"four", 0x4, 0, 0, true, 0x4},
        {"outside", outside(), 0, 0, false, 0x3FFF},
        {"partly_outside", 0x5 | (1ull << 20), 0, 0, false, 0x3FFF},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        ASSERT_NE(call("SetThreadAffinityMask", kCurrentThread, 0x3FFF), 0u);
        put_affinity(kData, c.mask, c.group, c.reserved);
        put_affinity(kData + 0x40, 0xCCCCCCCCull, 0xCCCC);
        set_last_error(0x1234);
        EXPECT_EQ(call("SetThreadGroupAffinity", kCurrentThread, kData, kData + 0x40), c.ok ? 1u : 0u);
        EXPECT_EQ(get_last_error(), c.ok ? 0x1234u : error_codes::ERROR_INVALID_PARAMETER);
        EXPECT_EQ(mask_at(kData + 0x40), c.ok ? 0x3FFFu : 0xCCCCCCCCu);  // prior only on success
        EXPECT_EQ(current_mask(), c.now);
    }
    put_affinity(kData, 0x8, 0);
    EXPECT_EQ(call("SetThreadGroupAffinity", kCurrentThread, kData, 0), 1u);  // NULL prior
    EXPECT_EQ(current_mask(), 0x8u);
}

TEST_P(ThreadAffinityTest, IdealProcessor) {
    const u64 maximum = wide() ? 64 : 32;  // MAXIMUM_PROCESSORS
    const u64 initial = call("SetThreadIdealProcessor", kCurrentThread, maximum);
    set_last_error(0x1234);
    EXPECT_EQ(call("SetThreadIdealProcessor", kCurrentThread, 3), initial);
    EXPECT_EQ(call("SetThreadIdealProcessor", kCurrentThread, maximum), 3u);
    EXPECT_EQ(call("SetThreadIdealProcessor", kCurrentThread, 63), 3u);  // out of range: unchanged
    EXPECT_EQ(call("SetThreadIdealProcessor", kCurrentThread, 14), 3u);
    EXPECT_EQ(call("SetThreadIdealProcessor", kCurrentThread, 13), 3u);
    EXPECT_EQ(get_last_error(), 0x1234u);
    EXPECT_EQ(call("GetThreadIdealProcessorEx", kCurrentThread, kData), 1u);
    EXPECT_EQ(memory.read_u8(kData + 2).value(), 13u);
    EXPECT_EQ(call("SetThreadIdealProcessor", 0, 1), 0xFFFFFFFFu);

    struct Case { const char* name; u16 group; u8 number; u8 reserved; bool ok; };
    for (const Case c : {Case{"five", 0, 5, 0, true}, Case{"count", 0, 14, 0, false},
                         Case{"max", 0, 64, 0, false}, Case{"group1", 1, 1, 0, false},
                         Case{"reserved", 0, 2, 1, false}}) {
        SCOPED_TRACE(c.name);
        (void)memory.write_u16(kData, c.group);
        (void)memory.write_u8(kData + 2, c.number);
        (void)memory.write_u8(kData + 3, c.reserved);
        (void)memory.write_u32(kData + 0x10, 0xCCCCCCCCu);
        set_last_error(0x1234);
        EXPECT_EQ(call("SetThreadIdealProcessorEx", kCurrentThread, kData, kData + 0x10), c.ok ? 1u : 0u);
        EXPECT_EQ(get_last_error(), c.ok ? 0x1234u : error_codes::ERROR_INVALID_PARAMETER);
        EXPECT_EQ(memory.read_u32(kData + 0x10).value(), c.ok ? 0x000D0000u : 0xCCCCCCCCu);  // previous 0/13/0
        ASSERT_EQ(call("GetThreadIdealProcessorEx", kCurrentThread, kData + 0x20), 1u);
        EXPECT_EQ(memory.read_u8(kData + 0x22).value(), 5u);
    }
    (void)memory.write_u32(kData, 0x00010000u);  // group 0, number 1
    EXPECT_EQ(call("SetThreadIdealProcessorEx", kCurrentThread, kData, 0), 1u);  // NULL previous
}

TEST_P(ThreadAffinityTest, PinnedThreadReportsItsProcessor) {
    // Pinned to processor 9 (the first core of the second P cluster): Windows
    // runs the thread there, so GetCurrentProcessorNumber(Ex) and the CPUID
    // APIC ID name it wherever macOS actually runs the host thread.
    ASSERT_NE(call("SetThreadAffinityMask", kCurrentThread, 1u << 9), 0u);
    threads.apply_current_thread_scheduling();  // binds this host thread's guest affinity
    EXPECT_EQ(call("GetCurrentProcessorNumber"), 9u);
    EXPECT_EQ(call("GetCurrentProcessorNumberEx", kData), 1u);
    EXPECT_EQ(memory.read_u32(kData).value(), 0x00090000u);  // group 0, number 9, reserved 0
    u32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    cpu::ops::synth_cpuid(1, 0, true, eax, ebx, ecx, edx);
    EXPECT_EQ(ebx >> 24, 16u);  // APIC ID of processor 9: cluster 2 block, core 0
}

INSTANTIATE_TEST_SUITE_P(BothAbis, ThreadAffinityTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

} // namespace
