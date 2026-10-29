#include "runtime/checkpoint/checkpoint_format.hpp"
#include "runtime/checkpoint/runtime_checkpoint.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"

#include <gtest/gtest.h>

namespace vitreo::checkpoint {
namespace {

TEST(CheckpointFormat, RoundTripsAndRejectsCorruption) {
    Checkpoint original;
    original.architecture = GuestArchitecture::X86;
    original.determinism = {123, 456, 789};
    original.pages.push_back(MemoryPage{
        0x1000, MemoryProtection::ReadWrite, true,
        std::vector<u8>(kGuestPageSize, 0xA5)});
    original.threads.push_back(ThreadState{1, 4, {1, 2, 3, 4}});
    original.modules.push_back(
        ModuleMetadata{0x400000, 0x12000, "C:\\test.exe", "test.exe"});
    original.handle_providers.push_back(
        HandleProviderState{"future-provider", 3, {9, 8, 7}});

    std::vector<u8> bytes;
    std::string error;
    ASSERT_TRUE(encode(original, bytes, error)) << error;

    Checkpoint decoded;
    ASSERT_TRUE(decode(bytes, decoded, error)) << error;
    EXPECT_EQ(decoded.format_version, kFormatVersion);
    EXPECT_EQ(decoded.architecture, GuestArchitecture::X86);
    EXPECT_EQ(decoded.determinism.rng_seed, 456u);
    ASSERT_EQ(decoded.pages.size(), 1u);
    EXPECT_EQ(decoded.pages[0].contents[100], 0xA5);
    ASSERT_EQ(decoded.modules.size(), 1u);
    EXPECT_EQ(decoded.modules[0].guest_path, "C:\\test.exe");
    ASSERT_EQ(decoded.handle_providers.size(), 1u);
    EXPECT_FALSE(decoded.handles_complete);

    bytes.back() ^= 1;
    EXPECT_FALSE(decode(bytes, decoded, error));
    EXPECT_NE(error.find("checksum"), std::string::npos);
}

TEST(RuntimeCheckpoint, RestoresMappedMemoryAndCpu) {
    GuestMemory source_memory;
    ASSERT_TRUE(source_memory
                    .map_region(0x10000, kGuestPageSize,
                                MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(source_memory.write_u32(0x10120, 0xAABBCCDD).is_ok());

    cpu::CpuState source_cpu;
    source_cpu.init_protected_mode();
    source_cpu.rip = 0x401000;
    source_cpu.gpr.rax = 0x12345678;

    CaptureRequest request;
    request.architecture = GuestArchitecture::X86;
    request.determinism.rng_seed = 42;
    Checkpoint saved;
    std::string error;
    ASSERT_TRUE(capture(source_memory, source_cpu, request, saved, error))
        << error;
    EXPECT_FALSE(saved.handles_complete);

    GuestMemory restored_memory;
    cpu::CpuState restored_cpu;
    restored_cpu.init_protected_mode();
    ASSERT_TRUE(restore(saved, GuestArchitecture::X86, restored_memory,
                        restored_cpu, error))
        << error;

    ASSERT_TRUE(restored_memory.read_u32(0x10120).is_ok());
    EXPECT_EQ(restored_memory.read_u32(0x10120).value(), 0xAABBCCDDu);
    EXPECT_EQ(restored_cpu.rip, 0x401000u);
    EXPECT_EQ(restored_cpu.gpr.rax, 0x12345678u);
}

TEST(RuntimeCheckpoint, RejectsCpuAbiAndArchitectureMismatch) {
    Checkpoint saved;
    saved.architecture = GuestArchitecture::X64;
    saved.threads.push_back(ThreadState{1, 1, {0}});

    GuestMemory memory;
    cpu::CpuState cpu;
    std::string error;
    EXPECT_FALSE(restore(saved, GuestArchitecture::X86, memory, cpu, error));
    EXPECT_NE(error.find("architecture"), std::string::npos);
}

} // namespace
} // namespace vitreo::checkpoint
