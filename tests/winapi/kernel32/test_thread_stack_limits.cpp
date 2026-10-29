/// @file test_thread_stack_limits.cpp
/// @brief GetCurrentThreadStackLimits / SetThreadStackGuarantee contracts.
///
/// Both were reaching the generic missing-export stub, which returns
/// 0xFFFFFFFF and writes no out-parameters at all. For
/// GetCurrentThreadStackLimits — a VOID function whose entire result is two
/// out-parameters — that means the caller compares its stack pointer against
/// whatever happened to be in those two words.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kTeb        = 0x00200000;
constexpr GuestAddr kStackLow   = 0x00300000;   // NtTib.StackLimit
constexpr GuestAddr kStackHigh  = 0x00400000;   // NtTib.StackBase
constexpr GuestAddr kScratch    = 0x00100000;

const ExportEntry* find_export(const DllExports& e, std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return it == e.entries.end() ? nullptr : &*it;
}

/// x64 guest with a TEB whose NT_TIB stack fields are filled in the way the
/// loader fills them (teb_setup.cpp: +0x08 StackBase, +0x10 StackLimit).
struct StackLimitsFixture {
    GuestMemory memory;
    capsule::Vfs vfs;
    cpu::CpuState cpu;

    StackLimitsFixture() {
        (void)memory.map_region(kScratch, 0x1000, MemoryProtection::ReadWrite);
        (void)memory.map_region(kTeb, 0x2000, MemoryProtection::ReadWrite);
        cpu.init_long_mode();
        cpu.seg.gs_desc.base = kTeb;
        (void)memory.write_u64(kTeb + 0x08, kStackHigh);
        (void)memory.write_u64(kTeb + 0x10, kStackLow);
    }
};

} // namespace

TEST(GetCurrentThreadStackLimitsTest, RegisteredAsStdcallTwoArgs) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);
    const auto* entry = find_export(exports, "GetCurrentThreadStackLimits");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 8u);
}

TEST(GetCurrentThreadStackLimitsTest, ReportsTheTebStackBounds) {
    StackLimitsFixture f;
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);

    // Poison both out-params: a stub that writes nothing must be detectable.
    (void)f.memory.write_u64(kScratch + 0x00, 0xDEADBEEFDEADBEEFULL);
    (void)f.memory.write_u64(kScratch + 0x08, 0xDEADBEEFDEADBEEFULL);

    f.cpu.gpr.rcx = kScratch + 0x00;  // LowLimit
    f.cpu.gpr.rdx = kScratch + 0x08;  // HighLimit
    mgr.impl_GetCurrentThreadStackLimits(m);

    EXPECT_EQ(f.memory.read_u64(kScratch + 0x00).value(), kStackLow);
    EXPECT_EQ(f.memory.read_u64(kScratch + 0x08).value(), kStackHigh);

    // The documented use: verify the current stack pointer lies inside.
    const u64 low = f.memory.read_u64(kScratch + 0x00).value();
    const u64 high = f.memory.read_u64(kScratch + 0x08).value();
    const u64 sp = kStackHigh - 0x100;
    EXPECT_GE(sp, low);
    EXPECT_LT(sp, high);
}

TEST(GetCurrentThreadStackLimitsTest, LowLimitIsTheAllocationBaseNotStackLimit) {
    // Learn: LowLimit is the lower boundary of the stack the system
    // ALLOCATED. That is TEB DeallocationStack (+0x1478). StackLimit is only
    // the committed bottom, and on an x64 thread it sits above the 16 KiB
    // stack-overflow guard -- so reporting it would tell a caller the stack is
    // 16 KiB smaller than it is, and change after every overflow.
    StackLimitsFixture f;
    constexpr u64 kAllocationBase = kStackLow - 0x4000;
    (void)f.memory.write_u64(kTeb + 0x1478, kAllocationBase);
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);

    f.cpu.gpr.rcx = kScratch + 0x00;
    f.cpu.gpr.rdx = kScratch + 0x08;
    mgr.impl_GetCurrentThreadStackLimits(m);

    EXPECT_EQ(f.memory.read_u64(kScratch + 0x00).value(), kAllocationBase);
    EXPECT_EQ(f.memory.read_u64(kScratch + 0x08).value(), kStackHigh);
}

TEST(GetCurrentThreadStackLimitsTest, NullOutParametersAreTolerated) {
    StackLimitsFixture f;
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);

    (void)f.memory.write_u64(kScratch + 0x08, 0);
    f.cpu.gpr.rcx = 0;                // LowLimit  = NULL
    f.cpu.gpr.rdx = kScratch + 0x08;  // HighLimit
    mgr.impl_GetCurrentThreadStackLimits(m);
    EXPECT_EQ(f.memory.read_u64(kScratch + 0x08).value(), kStackHigh);
}

TEST(SetThreadStackGuaranteeTest, RegisteredAsStdcallOneArg) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);
    const auto* entry = find_export(exports, "SetThreadStackGuarantee");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 4u);
}

// Learn: "On return, this value is set to the size of the previous stack, in
// bytes." Zero queries without changing anything; a smaller request succeeds
// but is ignored, so the guarantee never shrinks.
TEST(SetThreadStackGuaranteeTest, WritesBackThePreviousGuaranteeAndGrowsOnly) {
    StackLimitsFixture f;
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);
    f.cpu.gpr.rcx = kScratch;

    // First query on a fresh thread: no guarantee yet.
    (void)f.memory.write_u32(kScratch, 0);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_NE(f.cpu.gpr.rax, 0u) << "documented success is nonzero";
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0u);

    // Set 8 KB. The out-param still reports the PREVIOUS value, not the new one.
    (void)f.memory.write_u32(kScratch, 0x2000);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_NE(f.cpu.gpr.rax, 0u);
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0u);
    EXPECT_EQ(f.cpu.stack_guarantee, 0x2000u);

    // Query: now reports 8 KB and leaves it alone.
    (void)f.memory.write_u32(kScratch, 0);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_NE(f.cpu.gpr.rax, 0u);
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0x2000u);
    EXPECT_EQ(f.cpu.stack_guarantee, 0x2000u);

    // A smaller request succeeds but is ignored.
    (void)f.memory.write_u32(kScratch, 0x1000);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_NE(f.cpu.gpr.rax, 0u);
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0x2000u);
    (void)f.memory.write_u32(kScratch, 0);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0x2000u)
        << "the guarantee must never shrink";
}

TEST(SetThreadStackGuaranteeTest, RoundsUpToPageGranularity) {
    StackLimitsFixture f;
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);
    f.cpu.gpr.rcx = kScratch;

    (void)f.memory.write_u32(kScratch, 0x1001);
    mgr.impl_SetThreadStackGuarantee(m);
    (void)f.memory.write_u32(kScratch, 0);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0x2000u);
    EXPECT_EQ(f.cpu.stack_guarantee, 0x2000u);
}

// Learn: "This value cannot be larger than the reserved stack size."
TEST(SetThreadStackGuaranteeTest, RejectsAGuaranteeLargerThanTheStack) {
    StackLimitsFixture f;
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);

    f.cpu.gpr.rcx = kScratch;
    (void)f.memory.write_u32(kScratch, (kStackHigh - kStackLow) + 0x1000);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_EQ(f.cpu.gpr.rax, 0u) << "documented failure is zero";

    // The rejected request must not have been recorded.
    (void)f.memory.write_u32(kScratch, 0);
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_NE(f.cpu.gpr.rax, 0u);
    EXPECT_EQ(f.memory.read_u32(kScratch).value(), 0u);
}

TEST(SetThreadStackGuaranteeTest, NullPointerFails) {
    StackLimitsFixture f;
    Kernel32ExtManager mgr(f.memory, f.vfs);
    Marshaler m(f.memory);
    m.attach_cpu(f.cpu);

    f.cpu.gpr.rcx = 0;
    mgr.impl_SetThreadStackGuarantee(m);
    EXPECT_EQ(f.cpu.gpr.rax, 0u);
}
