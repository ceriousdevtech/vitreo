// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// PAGE_EXECUTE (Learn, "Memory Protection Constants") enables execute access
// with no read access. A PE section flagged IMAGE_SCN_MEM_EXECUTE without
// IMAGE_SCN_MEM_READ (characteristics 0x20000040) is mapped that way, and
// code in it must run. Instruction fetch used to go through the data-read
// check, failed on the first byte and surfaced as STATUS_ILLEGAL_INSTRUCTION
// at a perfectly valid function entry.

#include <gtest/gtest.h>

#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

namespace {

constexpr GuestAddr kCode = 0x140300000;
constexpr GuestAddr kStack = 0x140310000;

/// `sub rsp,0x88 ; add rsp,0x88 ; ret`
const std::vector<u8> kFunction = {0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,
                                   0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,
                                   0xC3};

void map_code(GuestMemory& mem, MemoryProtection prot) {
    ASSERT_TRUE(mem.map_region(kCode, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(mem.write_bytes(kCode, kFunction.data(), kFunction.size()));
    ASSERT_TRUE(mem.protect_region(kCode, 0x1000, prot).is_ok());
}

}  // namespace

TEST(ExecuteOnlyFetch, FetchSucceedsWhereDataReadIsRefused) {
    GuestMemory mem;
    map_code(mem, MemoryProtection::Execute);

    // Data reads keep the PAGE_EXECUTE "no read access" contract.
    ASSERT_FALSE(mem.read_u8(kCode).is_ok());
    EXPECT_EQ(mem.read_u8(kCode).error(), Error::ProtectionFault);
    EXPECT_EQ(mem.host_ptr(kCode, 15), nullptr);

    // Instruction fetch is execute access.
    auto b = mem.fetch_u8(kCode);
    ASSERT_TRUE(b.is_ok());
    EXPECT_EQ(b.value(), 0x48);
    const Byte* p = mem.fetch_ptr(kCode, kFunction.size());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p[kFunction.size() - 1], 0xC3);
}

TEST(ExecuteOnlyFetch, FetchStillRefusesNoAccessAndUnmapped) {
    GuestMemory mem;
    map_code(mem, MemoryProtection::None);
    EXPECT_EQ(mem.fetch_u8(kCode).error(), Error::ProtectionFault);
    EXPECT_EQ(mem.fetch_ptr(kCode, 1), nullptr);
    EXPECT_FALSE(mem.fetch_u8(kCode + 0x10000).is_ok());
}

TEST(ExecuteOnlyFetch, ReadOnlyPageStillFetches) {
    // Unchanged behaviour for readable code: fetch accepts Read as before.
    GuestMemory mem;
    map_code(mem, MemoryProtection::Read);
    EXPECT_TRUE(mem.fetch_u8(kCode).is_ok());
}

TEST(ExecuteOnlyFetch, BlockDecoderDecodesExecuteOnlyCode) {
    GuestMemory mem;
    map_code(mem, MemoryProtection::Execute);
    const auto block = cli::decode_basic_block(mem, kCode, true);
    EXPECT_EQ(block.instruction_count, 3u);
    EXPECT_EQ(block.byte_length, kFunction.size());
}

TEST(ExecuteOnlyFetch, InterpreterExecutesExecuteOnlyCode) {
    GuestMemory mem;
    map_code(mem, MemoryProtection::Execute);
    ASSERT_TRUE(mem.map_region(kStack - 0x1000, 0x2000, MemoryProtection::ReadWrite));

    CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = kCode;
    cpu.gpr.set64(4, kStack);
    Interpreter interp(cpu, mem);

    const ExecResult r = interp.step();
    EXPECT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch)
        << "ExecResult " << int(r);
    EXPECT_EQ(cpu.gpr.get64(4), kStack - 0x88);
    EXPECT_EQ(cpu.rip, kCode + 7);
}
