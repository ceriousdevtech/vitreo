/// @file test_vmem_free.cpp
/// @brief Unit tests for VirtualFree and the MEM_DECOMMIT contract (WIN32-020)

#include "test_vmem_fixture.hpp"
#include <core/cpu/decode/decoder.hpp>
#include <core/cpu/jit/block_cache.hpp>
#include <core/cpu/jit/jit_compiler.hpp>
#include <core/cpu/jit/jit_helpers.hpp>
#include <core/cpu/jit/stencils.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/memory/virtual_query.hpp>
#include <array>

TEST_F(VirtualMemoryTest, FreeDecommit) {
    u64 addr = vmm.virtual_alloc(0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);

    bool ok = vmm.virtual_free(addr, 0x1000, MEM_DECOMMIT);
    EXPECT_TRUE(ok);
    // MEM_DECOMMIT keeps the reservation and makes it inaccessible.
    EXPECT_TRUE(mem.is_mapped(static_cast<GuestAddr>(addr), 0x1000));
    EXPECT_TRUE(mem.read_u8(static_cast<GuestAddr>(addr)).is_err());

    VirtualAllocationQuery info{};
    ASSERT_TRUE(vmm.virtual_query(addr, info));
    EXPECT_EQ(info.state, MEM_RESERVE);
    EXPECT_EQ(info.protect, 0u);
}

TEST_F(VirtualMemoryTest, DecommitSubrangePreservesNeighboursAndRecommitZeros) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr), 0x11).is_ok());
    ASSERT_TRUE(mem.write_u8(
        static_cast<GuestAddr>(addr + 0x1000), 0x22).is_ok());
    ASSERT_TRUE(mem.write_u8(
        static_cast<GuestAddr>(addr + 0x2000), 0x33).is_ok());

    ASSERT_TRUE(vmm.virtual_free(addr + 0x1000, 0x1000, MEM_DECOMMIT));
    EXPECT_EQ(mem.read_u8(static_cast<GuestAddr>(addr)).value(), 0x11u);
    EXPECT_TRUE(mem.read_u8(
        static_cast<GuestAddr>(addr + 0x1000)).is_err());
    EXPECT_EQ(mem.read_u8(
        static_cast<GuestAddr>(addr + 0x2000)).value(), 0x33u);

    ASSERT_EQ(vmm.virtual_alloc(
                  addr + 0x1000, 0x1000, MEM_COMMIT, PAGE_READWRITE),
              addr + 0x1000);
    auto recommitted =
        mem.read_u8(static_cast<GuestAddr>(addr + 0x1000));
    ASSERT_TRUE(recommitted.is_ok());
    EXPECT_EQ(recommitted.value(), 0u);
}

TEST_F(VirtualMemoryTest, DecommitZeroSizeAtBaseCoversEntireAllocation) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x3000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(addr, 0u);
    for (u64 off = 0; off < 0x3000; off += 0x1000) {
        ASSERT_TRUE(mem.write_u8(static_cast<GuestAddr>(addr + off),
                                 static_cast<u8>(0x40 + (off >> 12))).is_ok());
    }

    ASSERT_TRUE(vmm.virtual_free(addr, 0, MEM_DECOMMIT));
    for (u64 off = 0; off < 0x3000; off += 0x1000) {
        EXPECT_TRUE(mem.read_u8(
            static_cast<GuestAddr>(addr + off)).is_err());
        VirtualAllocationQuery info{};
        ASSERT_TRUE(vmm.virtual_query(addr + off, info));
        EXPECT_EQ(info.state, MEM_RESERVE);
        EXPECT_EQ(info.protect, 0u);
    }

    ASSERT_EQ(vmm.virtual_alloc(
                  addr, 0x3000, MEM_COMMIT, PAGE_READWRITE),
              addr);
    for (u64 off = 0; off < 0x3000; off += 0x1000) {
        EXPECT_EQ(mem.read_u8(
            static_cast<GuestAddr>(addr + off)).value_or(0xFF), 0u);
    }
}

TEST_F(VirtualMemoryTest, DecommitInvalidatesCachedExecutableCode) {
    const u64 addr = vmm.virtual_alloc(
        0, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    ASSERT_NE(addr, 0u);

    constexpr std::array<u8, 1> kCode{0x90}; // NOP
    ASSERT_TRUE(mem.write_bytes(static_cast<GuestAddr>(addr), kCode.data(),
                                kCode.size()));
    cpu::jit::BlockCache cache{16};
    cpu::jit::JitCompiler jit{mem};
    jit.set_block_cache(&cache);
    cpu::MemoryByteReader reader{kCode.data(), kCode.size()};
    cpu::DecodedInstruction inst{};
    ASSERT_EQ(cpu::decode_instruction(
                  reader, cpu::DecodeMode::Mode64, inst),
              cpu::DecodeError::Success);
    cpu::jit::BasicBlock block{};
    block.start_addr = static_cast<GuestAddr>(addr);
    block.byte_length = 1;
    block.instruction_count = 1;
    block.is_64bit = true;
    block.instructions.push_back(inst);
    cpu::jit::BasicBlock* cached = cache.insert(block);
    ASSERT_NE(cached, nullptr);
    cpu::jit::CompiledBlock* compiled = jit.compile(*cached);
    ASSERT_NE(compiled, nullptr);
    jit.publish_compiled_block(*cached, *compiled);
    ASSERT_NE(cached->compiled_code, nullptr);

    ASSERT_TRUE(vmm.virtual_free(addr, 0x1000, MEM_DECOMMIT));
    EXPECT_EQ(cache.lookup(static_cast<GuestAddr>(addr)), nullptr)
        << "decommit left a stale translation for inaccessible code";
}

TEST(VirtualFreeThunkTest, ZeroSizeDecommitAtBaseDecommitsWholeAllocation) {
    GuestMemory memory;
    VirtualMemoryManager manager{memory};
    const u64 address = manager.virtual_alloc(
        0, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(address, 0u);
    ASSERT_TRUE(memory.write_u8(static_cast<GuestAddr>(address), 0xA5).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = address;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = MEM_DECOMMIT;
    set_last_error(0xDEADu);
    impl_VirtualFree(marshaler, manager);

    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
    EXPECT_TRUE(memory.read_u8(static_cast<GuestAddr>(address)).is_err());
    EXPECT_TRUE(memory.read_u8(
        static_cast<GuestAddr>(address + 0x1000)).is_err());
}

TEST(VirtualFreeThunkTest, BadAddressSetsInvalidAddressNotInvalidParameter) {
    // Audit 01 VirtualFree #1 / tools/oracle_probe/virtual_free_errors_probe.expected
    GuestMemory memory;
    VirtualMemoryManager manager{memory};
    const u64 address = manager.virtual_alloc(
        0, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(address, 0u);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    auto call = [&](u64 addr, u64 size, u32 type) {
        cpu.gpr.rcx = addr;
        cpu.gpr.rdx = size;
        cpu.gpr.r8 = type;
        set_last_error(0xDEADu);
        impl_VirtualFree(marshaler, manager);
        return std::pair<u64, u32>{cpu.gpr.rax, get_last_error()};
    };

    {
        const auto [ok, err] = call(0x12345000ull, 0, MEM_RELEASE);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_ADDRESS);
    }
    {
        const auto [ok, err] = call(address + 0x1000, 0, MEM_RELEASE);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_ADDRESS);
    }
    {
        const auto [ok, err] = call(address, 0x1000, MEM_RELEASE);
        EXPECT_EQ(ok, 0u);
        EXPECT_EQ(err, error_codes::ERROR_INVALID_PARAMETER);
    }
    EXPECT_TRUE(manager.virtual_free(address, 0, MEM_RELEASE));
}
