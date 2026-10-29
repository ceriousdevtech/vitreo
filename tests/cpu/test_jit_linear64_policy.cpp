// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// VITREO_JIT_LINEAR64 policy selection and linear reservation registration.
// Split from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

BasicBlock build_x64_policy_load(GuestAddr code_addr) {
    const std::array<u8, 2> code = {0x8B, 0x03}; // mov eax,[rbx]
    BasicBlock block;
    block.start_addr = code_addr;
    block.is_64bit = true;
    MemoryByteReader reader(code.data(), code.size());
    DecodedInstruction instruction;
    if (decode_instruction(reader, DecodeMode::Mode64, instruction) ==
        DecodeError::Success) {
        block.instructions.push_back(instruction);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

bool emits_linear64_policy_load(GuestMemory& memory, GuestAddr code_addr) {
    ScopedCompileMemoIsolation memo_isolation;
    JitCompiler jit(memory);
    BasicBlock block = build_x64_policy_load(code_addr);
    EXPECT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = jit.compile(block);
    EXPECT_NE(compiled, nullptr);
    if (compiled == nullptr) return false;

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    return has_guarded_linear64_load32(emitted);
}

TEST(JitLinearRegistrationTest, MoreThanEightSequentialLifetimesReuseSlots) {
    ASSERT_TRUE(install_linear_fault_handler());
    constexpr usize kSmallReservation = usize{1} << 20;
    for (int lifetime = 0; lifetime < 16; ++lifetime) {
        GuestMemory memory;
        ASSERT_TRUE(memory.enable_linear_reservation(kSmallReservation));
        {
            LinearReservationRegistration registration;
            ASSERT_TRUE(registration.acquire(memory.linear_base(),
                                             kSmallReservation))
                << "lifetime=" << lifetime;
            EXPECT_TRUE(linear_reservation_registered(memory.linear_base(),
                                                       kSmallReservation));
        }
        EXPECT_FALSE(linear_reservation_registered(memory.linear_base(),
                                                    kSmallReservation));
    }
}

// Unset selects linear64 only when the reservation carries guest protection
// in hardware. A host that cannot provide that property stays on region JIT.
TEST(JitLinear64PolicyTest, UnsetEnablesEligibleHardwareReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", nullptr);
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    ASSERT_TRUE(memory.enable_linear_hardware_protection());
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_TRUE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, UnsetRejectsSoftwareOnlyReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", nullptr);
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    ASSERT_FALSE(memory.linear_hardware_protection());
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_FALSE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, UnsetSoftwareOnlyReservationExecutesRegionLoad) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", nullptr);
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;
    constexpr GuestAddr data_addr = code_addr + 0x10000;
    constexpr u32 marker = 0x89ABCDEFu;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    ASSERT_FALSE(memory.linear_hardware_protection());
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(data_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(data_addr, marker));

    ScopedCompileMemoIsolation memo_isolation;
    JitCompiler jit(memory);
    BasicBlock block = build_x64_policy_load(code_addr);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    const usize word_count = compiled->code_size / sizeof(u32);
    std::vector<u32> emitted(word_count);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    ASSERT_FALSE(has_guarded_linear64_load32(emitted));

    CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = code_addr;
    cpu.gpr.rbx = data_addr;
    EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, marker);
    EXPECT_EQ(cpu.rip, code_addr + code.size());
}

TEST(JitLinear64PolicyTest, ExplicitOneEnablesRegisteredPowerOfTwoReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", "1");
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_TRUE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, ExplicitZeroDisablesEligibleReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", "0");
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_FALSE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, ExplicitZeroDisablesHardwareReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", "0");
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    ASSERT_TRUE(memory.enable_linear_hardware_protection());
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_FALSE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, ExplicitOneEnablesHardwareReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", "1");
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    ASSERT_TRUE(memory.enable_linear_hardware_protection());
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_TRUE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, UnsetFailsClosedWithoutRegisteredReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", nullptr);
    constexpr usize size = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;
    constexpr GuestAddr data_addr = code_addr + 0x10000;
    constexpr u32 marker = 0x13579BDFu;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    ASSERT_FALSE(linear_reservation_registered(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    ASSERT_TRUE(memory.map_region(data_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));
    ASSERT_TRUE(memory.write_u32(data_addr, marker));

    ScopedCompileMemoIsolation memo_isolation;
    JitCompiler jit(memory);
    BasicBlock block = build_x64_policy_load(code_addr);
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);
    const usize word_count = compiled->code_size / sizeof(u32);
    std::vector<u32> emitted(word_count);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    EXPECT_FALSE(has_guarded_linear64_load32(emitted));

    CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = code_addr;
    cpu.gpr.rbx = data_addr;
    EXPECT_EQ(test::execute_like_dispatcher(jit, *compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.gpr.rax, marker);
    EXPECT_EQ(cpu.rip, code_addr + code.size());
}

TEST(JitLinear64PolicyTest, UnsetFailsClosedWithoutLinearReservation) {
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", nullptr);
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_FALSE(memory.is_linear());
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_FALSE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinear64PolicyTest, UnsetFailsClosedForNonPowerOfTwoReservation) {
    ASSERT_TRUE(install_linear_fault_handler());
    ScopedEnvironmentFlag linear64("VITREO_JIT_LINEAR64", nullptr);
    constexpr usize size = usize{6} * 1024 * 1024 * 1024;
    constexpr GuestAddr code_addr = 0x0000000140000000ull;

    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(size));
    LinearReservationRegistration registration;
    ASSERT_TRUE(registration.acquire(memory.linear_base(), size));
    ASSERT_TRUE(memory.map_region(code_addr, 0x4000,
                                  MemoryProtection::ReadWrite));
    const std::array<u8, 2> code = {0x8B, 0x03};
    ASSERT_TRUE(memory.write_bytes(code_addr, code.data(), code.size()));

    EXPECT_FALSE(emits_linear64_policy_load(memory, code_addr));
}

TEST(JitLinearRegistrationTest, UnregisteredReservationFailsClosedToCheckedPath) {
    constexpr usize kUnregisteredSize = usize{8} * 1024 * 1024 * 1024;
    constexpr GuestAddr kUnregisteredCode = 0x0000000140000000ull;
    GuestMemory memory;
    ASSERT_TRUE(memory.enable_linear_reservation(kUnregisteredSize));
    ASSERT_TRUE(memory.map_region(kUnregisteredCode, 0x4000,
                                  MemoryProtection::ReadWrite));
    EXPECT_FALSE(linear_reservation_registered(memory.linear_base(),
                                               kUnregisteredSize));

    const char* prior = std::getenv("VITREO_JIT_LINEAR64");
    const std::string saved = prior ? prior : "";
    setenv("VITREO_JIT_LINEAR64", "1", 1);
    JitCompiler jit(memory);
    if (prior) setenv("VITREO_JIT_LINEAR64", saved.c_str(), 1);
    else unsetenv("VITREO_JIT_LINEAR64");

    const std::vector<u8> code = {0x8B, 0x03}; // mov eax,[rbx]
    BasicBlock block;
    block.start_addr = kUnregisteredCode;
    block.is_64bit = true;
    MemoryByteReader reader(code.data(), code.size());
    DecodedInstruction instruction;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, instruction),
              DecodeError::Success);
    block.instructions.push_back(instruction);
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = 1;
    CompiledBlock* compiled = jit.compile(block);
    ASSERT_NE(compiled, nullptr);

    const u32 words = compiled->code_size / 4;
    std::vector<u32> emitted(words);
    std::memcpy(emitted.data(), compiled->code, compiled->code_size);
    EXPECT_FALSE(has_guarded_linear64_load32(emitted));
}

}  // namespace
