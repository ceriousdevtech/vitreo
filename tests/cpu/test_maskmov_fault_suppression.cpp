// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// VMASKMOV / VPMASKMOV must not touch memory for masked-out elements.
//
// Intel SDM Vol. 2 VMASKMOV: "elements with the mask bit clear are not
// accessed and do not signal a fault". Guests rely on this to walk the tail of
// an array where the next element would cross into an unmapped page — the
// canonical vectorised-loop epilogue.
//
// The oracle harness cannot test this: its scratch page is fully mapped and
// the protocol has no way to express "the next page is unmapped", so a
// lowering that loaded the full 128/256 bits and then blended would pass every
// oracle value comparison and fault exactly where a guest expects safety.
// Hence a unit test against a real mapping boundary.

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u64 kCode = 0x1000;
// A single mapped page; everything at or past kEdge is unmapped.
constexpr u64 kDataPage = 0x40000;
constexpr u64 kPageSize = 0x1000;
constexpr u64 kEdge = kDataPage + kPageSize;

class MaskMovFaultTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCode, 0x2000,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kDataPage, kPageSize,
                                         MemoryProtection::ReadWrite));
        // Deliberately leave [kEdge, ...) unmapped.
    }

    static CpuState fresh() {
        CpuState cpu;
        cpu.init_long_mode();
        cpu.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x1800);
        return cpu;
    }

    ExecResult run_interp(const std::vector<u8>& code, CpuState& cpu) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        cpu.rip = kCode;
        Interpreter interp(cpu, *m_memory);
        return interp.step();
    }

    // Execute the same snippet through the JIT.
    bool run_jit(const std::vector<u8>& code, CpuState& cpu) {
        EXPECT_TRUE(m_memory->write_bytes(kCode, code.data(), code.size()));
        BasicBlock block;
        block.start_addr = kCode;
        block.is_64bit = true;
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction inst;
        if (decode_instruction(reader, DecodeMode::Mode64, inst)
            != DecodeError::Success) {
            return false;
        }
        block.instructions.push_back(inst);
        block.instruction_count = 1;
        block.byte_length = inst.length;
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        if (!cb) return false;
        cpu.rip = kCode;
        jit.execute(*cb, cpu);
        return true;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

// VPMASKMOVD xmm0, xmm1, [rax] — C4 E2 71 8C 00.
// RAX is placed so that elements 2 and 3 land on the unmapped page. With their
// mask bits clear, the instruction must complete and zero them.
TEST_F(MaskMovFaultTest, LoadDoesNotFaultOnMaskedOutElementsPastMapping) {
    const std::vector<u8> code{0xC4, 0xE2, 0x71, 0x8C, 0x00};
    const u64 base = kEdge - 8;  // elements 0,1 mapped; 2,3 unmapped

    auto seed = [&](CpuState& cpu) {
        cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), base);
        cpu.sse.xmm[1].u32_[0] = 0x80000000u;  // enabled
        cpu.sse.xmm[1].u32_[1] = 0x80000000u;  // enabled
        cpu.sse.xmm[1].u32_[2] = 0;            // masked out (unmapped)
        cpu.sse.xmm[1].u32_[3] = 0;            // masked out (unmapped)
        for (int i = 0; i < 4; ++i) cpu.sse.xmm[0].u32_[i] = 0xCCCCCCCCu;
    };
    const u32 a = 0x11111111u, b = 0x22222222u;
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base), a));
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base + 4), b));

    CpuState ic = fresh();
    seed(ic);
    ASSERT_EQ(run_interp(code, ic), ExecResult::Continue)
        << "interpreter faulted on a masked-out element";
    EXPECT_EQ(ic.sse.xmm[0].u32_[0], a);
    EXPECT_EQ(ic.sse.xmm[0].u32_[1], b);
    EXPECT_EQ(ic.sse.xmm[0].u32_[2], 0u);  // masked out -> zeroed
    EXPECT_EQ(ic.sse.xmm[0].u32_[3], 0u);

    CpuState jc = fresh();
    seed(jc);
    ASSERT_TRUE(run_jit(code, jc)) << "JIT failed to compile the snippet";
    EXPECT_FALSE(jc.pending_raise.active)
        << "JIT raised a fault for a masked-out element past the mapping";
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(jc.sse.xmm[0].u32_[i], ic.sse.xmm[0].u32_[i])
            << "lane " << i << " diverged from the interpreter";
    }
}

// VPMASKMOVD [rax], xmm1, xmm0 — C4 E2 71 8E 00. Masked-out elements must not
// be written, so the unmapped page is never touched.
TEST_F(MaskMovFaultTest, StoreDoesNotFaultOnMaskedOutElementsPastMapping) {
    const std::vector<u8> code{0xC4, 0xE2, 0x71, 0x8E, 0x00};
    const u64 base = kEdge - 8;

    auto seed = [&](CpuState& cpu) {
        cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), base);
        cpu.sse.xmm[1].u32_[0] = 0x80000000u;
        cpu.sse.xmm[1].u32_[1] = 0;
        cpu.sse.xmm[1].u32_[2] = 0;
        cpu.sse.xmm[1].u32_[3] = 0;
        for (int i = 0; i < 4; ++i) cpu.sse.xmm[0].u32_[i] = 0x55555555u;
    };

    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base), 0x99999999u));
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base + 4), 0x99999999u));

    CpuState ic = fresh();
    seed(ic);
    ASSERT_EQ(run_interp(code, ic), ExecResult::Continue)
        << "interpreter faulted on a masked-out store element";
    auto v0 = m_memory->read_u32(static_cast<GuestAddr>(base));
    auto v1 = m_memory->read_u32(static_cast<GuestAddr>(base + 4));
    ASSERT_TRUE(v0.is_ok());
    ASSERT_TRUE(v1.is_ok());
    EXPECT_EQ(v0.value(), 0x55555555u);  // enabled
    EXPECT_EQ(v1.value(), 0x99999999u);  // masked out -> untouched

    // Reset and run the same thing through the JIT.
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base), 0x99999999u));
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base + 4), 0x99999999u));
    CpuState jc = fresh();
    seed(jc);
    ASSERT_TRUE(run_jit(code, jc));
    EXPECT_FALSE(jc.pending_raise.active)
        << "JIT raised a fault for a masked-out store element";
    auto j0 = m_memory->read_u32(static_cast<GuestAddr>(base));
    auto j1 = m_memory->read_u32(static_cast<GuestAddr>(base + 4));
    ASSERT_TRUE(j0.is_ok());
    ASSERT_TRUE(j1.is_ok());
    EXPECT_EQ(j0.value(), 0x55555555u);
    EXPECT_EQ(j1.value(), 0x99999999u);
}

// An all-zero mask must touch nothing at all, even with the base squarely on
// unmapped memory.
TEST_F(MaskMovFaultTest, ZeroMaskTouchesNothingAtAll) {
    const std::vector<u8> code{0xC4, 0xE2, 0x71, 0x8C, 0x00};
    CpuState jc = fresh();
    jc.gpr.set64(static_cast<u8>(RegIndex::RAX), kEdge + 0x800);  // unmapped
    for (int i = 0; i < 4; ++i) jc.sse.xmm[1].u32_[i] = 0;
    ASSERT_TRUE(run_jit(code, jc));
    EXPECT_FALSE(jc.pending_raise.active)
        << "an all-zero mask must not access memory at all";
    for (int i = 0; i < 4; ++i) EXPECT_EQ(jc.sse.xmm[0].u32_[i], 0u);
}

// Gathers carry the same contract: an element whose mask bit is clear is not
// accessed, so an index pointing into unmapped memory must be harmless.
// VPGATHERDD xmm0, [rax + xmm1*4], xmm2 — C4 E2 69 90 04 88.
TEST_F(MaskMovFaultTest, GatherDoesNotFaultOnMaskedOutElements) {
    const std::vector<u8> code{0xC4, 0xE2, 0x69, 0x90, 0x04, 0x88};
    const u64 base = kDataPage;

    auto seed = [&](CpuState& cpu) {
        cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), base);
        // Elements 0,1 index into the mapped page; 2,3 point far past the
        // mapping and are masked out.
        cpu.sse.xmm[1].u32_[0] = 0;
        cpu.sse.xmm[1].u32_[1] = 1;
        cpu.sse.xmm[1].u32_[2] = 0x00100000u;   // way past kEdge
        cpu.sse.xmm[1].u32_[3] = 0x00200000u;
        cpu.sse.xmm[2].u32_[0] = 0x80000000u;   // enabled
        cpu.sse.xmm[2].u32_[1] = 0x80000000u;   // enabled
        cpu.sse.xmm[2].u32_[2] = 0;             // masked out (unmapped)
        cpu.sse.xmm[2].u32_[3] = 0;
        for (int i = 0; i < 4; ++i) cpu.sse.xmm[0].u32_[i] = 0xCCCCCCCCu;
    };
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base), 0xAAAAAAAAu));
    ASSERT_TRUE(m_memory->write_u32(static_cast<GuestAddr>(base + 4), 0xBBBBBBBBu));

    CpuState ic = fresh();
    seed(ic);
    ASSERT_EQ(run_interp(code, ic), ExecResult::Continue)
        << "interpreter faulted on a masked-out gather element";

    CpuState jc = fresh();
    seed(jc);
    ASSERT_TRUE(run_jit(code, jc)) << "JIT failed to compile the gather";
    EXPECT_FALSE(jc.pending_raise.active)
        << "JIT raised a fault for a masked-out gather element";

    EXPECT_EQ(jc.sse.xmm[0].u32_[0], 0xAAAAAAAAu);
    EXPECT_EQ(jc.sse.xmm[0].u32_[1], 0xBBBBBBBBu);
    // Masked-out destination elements keep their previous contents.
    EXPECT_EQ(jc.sse.xmm[0].u32_[2], 0xCCCCCCCCu);
    EXPECT_EQ(jc.sse.xmm[0].u32_[3], 0xCCCCCCCCu);
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(jc.sse.xmm[0].u32_[i], ic.sse.xmm[0].u32_[i])
            << "gather lane " << i << " diverged from the interpreter";
        EXPECT_EQ(jc.sse.xmm[2].u32_[i], 0u) << "mask element " << i
            << " must be zeroed on completion";
    }
}

}  // namespace
