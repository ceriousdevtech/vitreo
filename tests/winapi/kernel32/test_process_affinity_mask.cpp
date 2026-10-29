/// @file test_process_affinity_mask.cpp
/// @brief GetProcessAffinityMask must report the real processor topology.
///
/// The mask is a decision input, not a cosmetic one: a guest that sizes its
/// job system from popcount(mask) builds a single-threaded pipeline when told
/// the machine is a uniprocessor. Vitreo previously hardcoded 1, and TS3.exe
/// (which imports this export) ran its whole load single-file as a result.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"
#include "winapi/core/marshaler.hpp"

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kProcMaskPtr = 0x30000;
constexpr GuestAddr kSysMaskPtr  = 0x30010;

struct Fixture {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr{memory, vfs};
    cpu::CpuState cpu;
    Marshaler m{memory};

    Fixture() {
        (void)memory.map_region(0x30000, 0x10000,
                                MemoryProtection::ReadWrite);
        cpu.init_long_mode();
        m.attach_cpu(cpu);
    }

    void get(u64 hproc = 0x80) {
        (void)memory.write_u64(kProcMaskPtr, 0xDEADBEEFDEADBEEFull);
        (void)memory.write_u64(kSysMaskPtr, 0xDEADBEEFDEADBEEFull);
        cpu.gpr.rcx = hproc;
        cpu.gpr.rdx = kProcMaskPtr;
        cpu.gpr.r8 = kSysMaskPtr;
        mgr.impl_GetProcessAffinityMask(m);
    }

    u64 set(u64 mask) {
        cpu.gpr.rcx = 0x80;
        cpu.gpr.rdx = mask;
        mgr.impl_SetProcessAffinityMask(m);
        return cpu.gpr.rax;
    }
};

} // namespace

// "A system affinity mask is a bit vector in which each bit represents the
// processors that are configured into a system." One bit per logical CPU, not
// a hardcoded 1.
TEST(ProcessAffinityMaskTest, ReportsOneBitPerLogicalProcessor) {
    Fixture f;
    f.get();
    ASSERT_EQ(f.cpu.gpr.rax, 1u);

    const u64 expected = host_active_processor_mask();
    EXPECT_EQ(f.memory.read_u64(kSysMaskPtr).value(), expected);
    EXPECT_EQ(__builtin_popcountll(expected), host_active_processor_count());
}

// "A process affinity mask is a subset of the system affinity mask" and equals
// it until the process restricts itself.
TEST(ProcessAffinityMaskTest, UnrestrictedProcessMaskEqualsSystemMask) {
    Fixture f;
    f.get();
    EXPECT_EQ(f.memory.read_u64(kProcMaskPtr).value(),
              f.memory.read_u64(kSysMaskPtr).value());
}

TEST(ProcessAffinityMaskTest, SetNarrowsTheReportedProcessMask) {
    Fixture f;
    ASSERT_EQ(f.set(1), 1u);
    f.get();
    EXPECT_EQ(f.memory.read_u64(kProcMaskPtr).value(), 1u);
    // The system mask is unaffected by a process restricting itself.
    EXPECT_EQ(f.memory.read_u64(kSysMaskPtr).value(),
              host_active_processor_mask());
}

// "The process affinity mask cannot specify a 1 bit for a processor when the
// system affinity mask specifies a 0 bit for that processor."
TEST(ProcessAffinityMaskTest, SetRejectsProcessorsNotInSystemMask) {
    Fixture f;
    const u64 system_mask = host_active_processor_mask();
    if (system_mask != ~0ull) {
        EXPECT_EQ(f.set(system_mask | (system_mask + 1)), 0u);
    }
    EXPECT_EQ(f.set(0), 0u);

    // A rejected request leaves the process unrestricted.
    f.get();
    EXPECT_EQ(f.memory.read_u64(kProcMaskPtr).value(), system_mask);
}

// DWORD_PTR is pointer-sized: a 32-bit guest gets 4 bytes, and the write must
// not smear over the adjacent variable.
TEST(ProcessAffinityMaskTest, ThirtyTwoBitGuestGetsFourByteMasks) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    ASSERT_TRUE(memory.map_region(0x30000, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    (void)memory.write_u64(kProcMaskPtr, 0xDEADBEEFDEADBEEFull);
    // stdcall args on the 32-bit stack.
    const GuestAddr stack = 0x30800;
    cpu.gpr.rsp = stack;
    (void)memory.write_u32(stack + 4, 0x80);
    (void)memory.write_u32(stack + 8, kProcMaskPtr);
    (void)memory.write_u32(stack + 12, 0);
    mgr.impl_GetProcessAffinityMask(m);

    const u64 expected32 = host_active_processor_mask() & 0xFFFFFFFFull;
    EXPECT_EQ(memory.read_u32(kProcMaskPtr).value(), expected32);
    // High half untouched.
    EXPECT_EQ(memory.read_u32(kProcMaskPtr + 4).value(), 0xDEADBEEFu);
}
