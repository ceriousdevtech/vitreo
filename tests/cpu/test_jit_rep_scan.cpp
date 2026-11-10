// SPDX-License-Identifier: LGPL-2.1-only
// Intel SDM Vol. 2 CMPS, SCAS and REP: final comparisons and restart state.
#include <gtest/gtest.h>
#include "jit_exec_test_fixture.hpp"
#include <vector>
#include <array>
#include <limits>
#include "core/cpu/jit/jit_helpers.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {
constexpr u8 ax = static_cast<u8>(RegIndex::RAX);
constexpr u8 cx = static_cast<u8>(RegIndex::RCX);
constexpr u8 si = static_cast<u8>(RegIndex::RSI);
constexpr u8 di = static_cast<u8>(RegIndex::RDI);
using RepHelper = void (*)(CpuState*, GuestMemory*);
}

TEST(JitRepScan, FaultPreservesEntryFlagsAndCommitsOnlyCompletedComparisons) {
    // Intel SDM Vol. 2B REP (4-564): EFLAGS restores to instruction entry,
    // whereas indices/count retain completed iterations, including first fault.
    for (const bool wide : {false, true}) {
        for (const bool cmps : {false, true}) {
            for (const bool repe : {false, true}) {
                for (const u64 completed : {0u, 32u}) {
                    SCOPED_TRACE(::testing::Message() << wide << cmps << repe << completed);
                    GuestMemory memory;
                    ASSERT_TRUE(memory.map_region(0x2000, 0x1000, MemoryProtection::ReadWrite));
                    ASSERT_TRUE(memory.map_region(0x4000, 0x1000, MemoryProtection::ReadWrite));
                    const std::vector<u8> source(0x1000, 0x11);
                    const std::vector<u8> target(0x1000, repe ? 0x11 : 0x22);
                    ASSERT_TRUE(memory.write_bytes(0x2000, source.data(), source.size()));
                    ASSERT_TRUE(memory.write_bytes(0x4000, target.data(), target.size()));
                    CpuState cpu;
                    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                    cpu.flags.rflags = 0x8D7;
                    cpu.set_flag_op(CpuState::FlagOp::Sub32);
                    cpu.flag_src = 3;
                    cpu.flag_dst = 0xFFFFFFFE;
                    cpu.cf_overlay = 2;
                    cpu.of_overlay = 1;
                    const CpuState entry = cpu;
                    cpu.gpr.set64(ax, 0x11);
                    cpu.gpr.set64(cx, 64);
                    cpu.gpr.set64(si, 0x2000);
                    cpu.gpr.set64(di, 0x5000 - completed);
                    const RepHelper helper = cmps
                        ? (repe ? jit_repe_cmpsb : jit_repne_cmpsb)
                        : (repe ? jit_repe_scasb : jit_repne_scasb);
                    helper(&cpu, &memory);
                    ASSERT_TRUE(cpu.pending_raise.active);
                    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
                    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
                    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
                    EXPECT_EQ(cpu.pending_raise.args[1], 0x5000u);
                    EXPECT_EQ(cpu.gpr.get64(di), 0x5000u);
                    EXPECT_EQ(cpu.gpr.get64(cx), 64 - completed);
                    EXPECT_EQ(cpu.gpr.get64(si), 0x2000 + (cmps ? completed : 0));
                    EXPECT_EQ(cpu.flags.rflags, entry.flags.rflags);
                    EXPECT_EQ(cpu.flag_op, entry.flag_op);
                    EXPECT_EQ(cpu.flag_src, entry.flag_src);
                    EXPECT_EQ(cpu.flag_dst, entry.flag_dst);
                    EXPECT_EQ(cpu.cf_overlay, entry.cf_overlay);
                    EXPECT_EQ(cpu.of_overlay, entry.of_overlay);
                }
            }
        }
    }
}

namespace {
constexpr std::array<RepHelper, 4> cmps_equal = {
    jit_repe_cmpsb, jit_repe_cmpsw, jit_repe_cmpsd, jit_repe_cmpsq};
constexpr std::array<RepHelper, 4> cmps_unequal = {
    jit_repne_cmpsb, jit_repne_cmpsw, jit_repne_cmpsd, jit_repne_cmpsq};
constexpr std::array<RepHelper, 4> scas_equal = {
    jit_repe_scasb, jit_repe_scasw, jit_repe_scasd, jit_repe_scasq};
constexpr std::array<RepHelper, 4> scas_unequal = {
    jit_repne_scasb, jit_repne_scasw, jit_repne_scasd, jit_repne_scasq};
RepHelper select_helper(usize width, bool cmps, bool repe) {
    return cmps ? (repe ? cmps_equal[width] : cmps_unequal[width])
                : (repe ? scas_equal[width] : scas_unequal[width]);
}
}

TEST(JitRepScan, ForwardScansStopAtLastReadableElementForEveryWidthAndPrefix) {
    for (const bool wide : {false, true}) {
        for (usize width = 0; width < (wide ? 4u : 3u); ++width) {
            const usize elem = usize{1} << width;
            for (const bool cmps : {false, true}) {
                for (const bool repe : {false, true}) {
                    for (const usize bytes : {4096u, 8192u}) {
                        for (const bool unbounded : {false, true}) {
                            SCOPED_TRACE(::testing::Message() << wide << width << cmps << repe << bytes << unbounded);
                            GuestMemory memory;
                            ASSERT_TRUE(memory.map_region(0x20000, bytes, MemoryProtection::ReadWrite));
                            ASSERT_TRUE(memory.map_region(0x30000, bytes, MemoryProtection::ReadWrite));
                            const std::vector<u8> source(bytes, 0x11);
                            std::vector<u8> target(bytes, repe ? 0x11 : 0x22);
                            std::fill(target.end() - elem, target.end(), repe ? 0x22 : 0x11);
                            ASSERT_TRUE(memory.write_bytes(0x20000, source.data(), bytes));
                            ASSERT_TRUE(memory.write_bytes(0x30000, target.data(), bytes));
                            CpuState cpu;
                            if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                            cpu.gpr.set64(ax, 0x1111111111111111ull);
                            const u64 count = unbounded
                                ? (wide ? std::numeric_limits<u64>::max() : std::numeric_limits<u32>::max())
                                : bytes / elem;
                            cpu.gpr.set64(cx, count);
                            cpu.gpr.set64(si, 0x20000);
                            cpu.gpr.set64(di, 0x30000);
                            cpu.rip = 0x123456;
                            select_helper(width, cmps, repe)(&cpu, &memory);
                            ASSERT_FALSE(cpu.pending_raise.active);
                            EXPECT_EQ(cpu.gpr.get64(cx), count - bytes / elem);
                            EXPECT_EQ(cpu.gpr.get64(di), 0x30000 + bytes);
                            EXPECT_EQ(cpu.gpr.get64(si), 0x20000 + (cmps ? bytes : 0));
                            EXPECT_EQ(cpu.gpr.get64(ax), 0x1111111111111111ull);
                            EXPECT_EQ(cpu.rip, 0x123456u);
                            // Last comparison: 11..11 - 22..22 for REPE;
                            // 11..11 - 11..11 for REPNE (Intel subtraction).
                            EXPECT_EQ(cpu.flags.zf(), !repe);
                            EXPECT_EQ(cpu.flags.cf(), repe);
                            EXPECT_EQ(cpu.flags.sf(), repe);
                            EXPECT_EQ(cpu.flags.af(), repe);
                            EXPECT_EQ(cpu.flags.pf(), !repe);
                            EXPECT_FALSE(cpu.flags.of());
                            EXPECT_FALSE(cpu.flags.df());
                        }
                    }
                }
            }
        }
    }
}

TEST(JitRepScan, CrossingElementFaultNamesFirstUnreadableByteAndPreservesRestartState) {
    for (const bool wide : {false, true}) {
        for (usize width = 1; width < (wide ? 4u : 3u); ++width) {
            const u64 elem = u64{1} << width;
            for (u64 available = 1; available < elem; ++available) {
                for (int fault_side = 0; fault_side < 3; ++fault_side) {
                    const bool cmps = fault_side != 0;
                    for (const bool repe : {false, true}) {
                        for (const u64 completed : {0u, 2u}) {
                            SCOPED_TRACE(::testing::Message() << wide << width << available << fault_side << repe << completed);
                            GuestMemory memory;
                            ASSERT_TRUE(memory.map_region(0x2000, 0x1000, MemoryProtection::ReadWrite));
                            ASSERT_TRUE(memory.map_region(0x4000, 0x1000, MemoryProtection::ReadWrite));
                            const std::vector<u8> source(0x1000, 0x11);
                            const std::vector<u8> target(0x1000, repe ? 0x11 : 0x22);
                            ASSERT_TRUE(memory.write_bytes(0x2000, source.data(), source.size()));
                            ASSERT_TRUE(memory.write_bytes(0x4000, target.data(), target.size()));
                            CpuState cpu;
                            if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                            cpu.flags.rflags = 0x8D7;
                            const u64 entry_flags = cpu.flags.rflags;
                            cpu.gpr.set64(ax, 0x1111111111111111ull);
                            cpu.gpr.set64(cx, 17);
                            const u64 bad_start = 0x5000 - available;
                            const u64 initial = bad_start - completed * elem;
                            cpu.gpr.set64(si, fault_side == 1 ? initial : 0x2000);
                            cpu.gpr.set64(di, fault_side == 1 ? 0x2000 : initial);
                            // For source faults with REPNE, ensure successful
                            // pairs differ before the failing source element.
                            if (fault_side == 1) {
                                std::vector<u8> good(0x1000, repe ? 0x11 : 0x33);
                                ASSERT_TRUE(memory.write_bytes(0x2000, good.data(), good.size()));
                                const std::vector<u8> bad_source(0x1000, 0x11);
                                ASSERT_TRUE(memory.write_bytes(0x4000, bad_source.data(), bad_source.size()));
                            }
                            select_helper(width, cmps, repe)(&cpu, &memory);
                            ASSERT_TRUE(cpu.pending_raise.active);
                            EXPECT_EQ(cpu.pending_raise.args[0], 0u);
                            EXPECT_EQ(cpu.pending_raise.args[1], 0x5000u);
                            EXPECT_EQ(cpu.gpr.get64(cx), 17 - completed);
                            EXPECT_EQ(cpu.gpr.get64(si), fault_side == 1 ? bad_start : 0x2000 + (cmps ? completed * elem : 0));
                            EXPECT_EQ(cpu.gpr.get64(di), fault_side == 1 ? 0x2000 + completed * elem : bad_start);
                            EXPECT_EQ(cpu.flags.rflags, entry_flags);
                        }
                    }
                }
            }
        }
    }
}

TEST(JitRepScan, DefaultSegmentsTranslateAddressesButNeverStoredOffsets) {
    for (const bool wide : {false, true}) {
        for (const bool cmps : {false, true}) {
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x9000, 0x1000, MemoryProtection::ReadWrite));
            ASSERT_TRUE(memory.map_region(0xD000, 0x1000, MemoryProtection::ReadWrite));
            const std::vector<u8> source(0x1000, 0x11);
            std::vector<u8> target(0x1000, 0x22);
            target[128] = 0x11;
            ASSERT_TRUE(memory.write_bytes(0x9000, source.data(), source.size()));
            ASSERT_TRUE(memory.write_bytes(0xD000, target.data(), target.size()));
            CpuState cpu;
            if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
            cpu.seg.ds_desc.base = 0x8000;
            cpu.seg.es_desc.base = 0xC000;
            const u64 src = wide ? 0x9000 : 0x1000;
            const u64 dst = wide ? 0xD000 : 0x1000;
            cpu.gpr.set64(si, src);
            cpu.gpr.set64(di, dst);
            cpu.gpr.set64(cx, 1000);
            cpu.gpr.set64(ax, 0x11);
            select_helper(0, cmps, false)(&cpu, &memory);
            ASSERT_FALSE(cpu.pending_raise.active);
            EXPECT_EQ(cpu.gpr.get64(cx), 1000 - 129);
            EXPECT_EQ(cpu.gpr.get64(di), dst + 129);
            EXPECT_EQ(cpu.gpr.get64(si), src + (cmps ? 129 : 0));
            EXPECT_TRUE(cpu.flags.zf());
        }
    }
}

namespace {
BasicBlock scan_block(const std::vector<u8>& code, bool wide) {
    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = wide;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining() != 0) {
        DecodedInstruction instruction;
        const auto error = decode_instruction(reader, wide ? DecodeMode::Mode64 : DecodeMode::Mode32, instruction);
        EXPECT_EQ(error, DecodeError::Success);
        if (error != DecodeError::Success) break;
        block.instructions.push_back(instruction);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}
}

TEST_F(JitExecTest, NonRepCmpsScasRetainNativeCoverageAndApplyDefaultSegments) {
    for (usize width = 0; width < 3; ++width) {
        for (const bool cmps : {false, true}) {
            SCOPED_TRACE(::testing::Message() << width << cmps);
            const usize elem = usize{1} << width;
            std::vector<u8> code;
            if (width == 1) code.push_back(0x66);
            code.push_back(cmps ? (width ? 0xA7 : 0xA6) : (width ? 0xAF : 0xAE));
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            const std::vector<u8> source(elem, 0x11), target(elem, 0x22);
            ASSERT_TRUE(m_memory->write_bytes(0x2100, source.data(), elem));
            ASSERT_TRUE(m_memory->write_bytes(0x3200, target.data(), elem));
            m_cpu->init_protected_mode();
            m_cpu->rip = 0x1000;
            m_cpu->seg.ds_desc.base = 0x2000;
            m_cpu->seg.es_desc.base = 0x3000;
            m_cpu->gpr.set64(ax, 0x1111111111111111ull);
            m_cpu->gpr.set32(si, 0x100);
            m_cpu->gpr.set32(di, 0x200);
            m_cpu->gpr.set32(cx, 57);
            auto block = scan_block(code, false);
            auto* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_EQ(compiled->native_insns, 1u);
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
            m_cpu->materialize_lazy_flags();
            EXPECT_EQ(m_cpu->gpr.get32(si), 0x100 + (cmps ? elem : 0));
            EXPECT_EQ(m_cpu->gpr.get32(di), 0x200 + elem);
            EXPECT_EQ(m_cpu->gpr.get32(cx), 57u);
            EXPECT_TRUE(m_cpu->flags.cf());
            EXPECT_FALSE(m_cpu->flags.zf());
        }
    }
}

TEST_F(JitExecTest, RepQwordComparisonFaultExitsBeforeFollowingNativeInstruction) {
    for (const bool cmps : {false, true}) {
        for (const bool repe : {false, true}) {
            SCOPED_TRACE(::testing::Message() << cmps << repe);
            const std::vector<u8> code = {
                static_cast<u8>(repe ? 0xF3 : 0xF2), 0x48,
                static_cast<u8>(cmps ? 0xA7 : 0xAF),
                0xBB, 0x78, 0x56, 0x34, 0x12}; // MOV EBX,12345678 after REP
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            ASSERT_TRUE(m_memory->write_u64(0x10FF8, 0));
            ASSERT_TRUE(m_memory->write_u64(0x2000, repe ? 0 : 0x11));
            ASSERT_TRUE(m_memory->write_u64(0x2008, repe ? 0 : 0x11));
            m_cpu->init_long_mode();
            m_cpu->rip = 0x1000;
            m_cpu->flags.rflags = 0x8D7;
            m_cpu->gpr.set64(ax, repe ? 0 : 0x11);
            m_cpu->gpr.set64(cx, 2);
            m_cpu->gpr.set64(si, 0x2000);
            m_cpu->gpr.set64(di, 0x10FF8);
            m_cpu->gpr.set32(EBX, 0xDEADBEEF);
            auto block = scan_block(code, true);
            auto* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_EQ(compiled->native_insns, 2u);
            test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu);
            ASSERT_TRUE(m_cpu->pending_raise.active);
            EXPECT_EQ(m_cpu->pending_raise.args[1], 0x11000u);
            EXPECT_EQ(m_cpu->gpr.get64(di), 0x11000u);
            EXPECT_EQ(m_cpu->gpr.get64(cx), 1u);
            EXPECT_EQ(m_cpu->gpr.get32(EBX), 0xDEADBEEFu);
            EXPECT_EQ(m_cpu->rip, 0x1000u);
            m_cpu->materialize_lazy_flags();
            EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
        }
    }
}

TEST_F(JitExecTest, CmpsScasAlternateAddressSizesAndSourceOverridesDelegatePrecisely) {
    if (environment_flag_enabled("VITREO_JIT_NO_DELEGATE")) GTEST_SKIP();
    for (int variant = 0; variant < 4; ++variant) {
        SCOPED_TRACE(variant);
        const bool wide = variant >= 2;
        std::vector<u8> code;
        if (variant == 0) code = {0xF3, 0x67, 0x66, 0xAF}; // addr16 SCASW
        if (variant == 1) code = {0xF3, 0x64, 0xA6};       // FS CMPSB
        if (variant == 2) code = {0xF3, 0x67, 0x48, 0xAF}; // addr32 SCASQ
        if (variant == 3) code = {0xF3, 0x65, 0x48, 0xA7}; // GS CMPSQ
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        if (wide) m_cpu->init_long_mode(); else m_cpu->init_protected_mode();
        m_cpu->rip = 0x1000;
        m_cpu->gpr.set64(ax, 0x1111111111111111ull);
        m_cpu->gpr.set64(si, 0x2000);
        m_cpu->gpr.set64(di, 0x2000);
        m_cpu->gpr.set64(cx, 1);
        m_cpu->seg.es_desc.base = 0x3000;
        m_cpu->seg.ds_desc.base = 0x1000;
        m_cpu->seg.fs_desc.base = 0x4000;
        m_cpu->seg.gs_desc.base = 0x4000;
        if (variant == 0) {
            m_cpu->gpr.set32(di, 0x12342000);
            m_cpu->gpr.set32(cx, 0x56780001);
            ASSERT_TRUE(m_memory->write_u16(0x5000, 0x1111));
        } else if (variant == 1) {
            ASSERT_TRUE(m_memory->write_u8(0x6000, 0x11));
            ASSERT_TRUE(m_memory->write_u8(0x5000, 0x11));
        } else if (variant == 2) {
            m_cpu->gpr.set64(di, 0x123400002000ull);
            m_cpu->gpr.set64(cx, 0x123400000001ull);
            ASSERT_TRUE(m_memory->write_u64(0x2000, 0x1111111111111111ull));
        } else {
            m_cpu->gpr.set64(di, 0x5000);
            ASSERT_TRUE(m_memory->write_u64(0x6000, 0x1111111111111111ull));
            ASSERT_TRUE(m_memory->write_u64(0x5000, 0x1111111111111111ull));
        }
        auto block = scan_block(code, wide);
        auto* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        EXPECT_EQ(compiled->native_insns, 0u);
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
        EXPECT_FALSE(m_cpu->pending_raise.active);
        m_cpu->materialize_lazy_flags();
        EXPECT_TRUE(m_cpu->flags.zf());
        if (variant == 0) {
            EXPECT_EQ(m_cpu->gpr.get32(di), 0x12342002u);
            EXPECT_EQ(m_cpu->gpr.get32(cx), 0x56780000u);
        } else if (variant == 1) {
            EXPECT_EQ(m_cpu->gpr.get32(si), 0x2001u);
            EXPECT_EQ(m_cpu->gpr.get32(di), 0x2001u);
            EXPECT_EQ(m_cpu->gpr.get32(cx), 0u);
        } else if (variant == 2) {
            EXPECT_EQ(m_cpu->gpr.get64(di), 0x2008u);
            EXPECT_EQ(m_cpu->gpr.get64(cx), 0u);
        } else {
            EXPECT_EQ(m_cpu->gpr.get64(si), 0x2008u);
            EXPECT_EQ(m_cpu->gpr.get64(di), 0x5008u);
            EXPECT_EQ(m_cpu->gpr.get64(cx), 0u);
        }
    }
}

TEST_F(JitExecTest, NonRepCmpsPreservesFirstOperandAcrossTwoColdMemoryHelpers) {
    // Both operands miss the stack/last-region caches. The second helper's
    // ABI return register X0 must not overwrite the first operand.
    ASSERT_TRUE(m_memory->map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x30000, 0x1000, MemoryProtection::ReadWrite));
    for (const bool wide : {false, true}) {
        for (usize width = 0; width < (wide ? 4u : 3u); ++width) {
            SCOPED_TRACE(::testing::Message() << wide << width);
            const usize elem = usize{1} << width;
            const std::vector<u8> source(elem, 0x11), destination(elem, 0x22);
            ASSERT_TRUE(m_memory->write_bytes(0x20020, source.data(), elem));
            ASSERT_TRUE(m_memory->write_bytes(0x30040, destination.data(), elem));
            std::vector<u8> code;
            if (width == 1) code.push_back(0x66);
            if (width == 3) code.push_back(0x48);
            code.push_back(width ? 0xA7 : 0xA6);
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            if (wide) m_cpu->init_long_mode(); else m_cpu->init_protected_mode();
            m_cpu->rip = 0x1000;
            m_cpu->gpr.set64(si, 0x20020);
            m_cpu->gpr.set64(di, 0x30040);
            m_cpu->gpr.set64(cx, 57);
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            auto block = scan_block(code, wide);
            auto* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            EXPECT_EQ(compiled->native_insns, 1u);
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu), JitExecResult::Continue);
            ASSERT_FALSE(m_cpu->pending_raise.active);
            m_cpu->materialize_lazy_flags();
            // Intel CMPS computes first-source minus second-source:
            // 11..11 - 22..22 is negative, with borrow, and never zero.
            EXPECT_TRUE(m_cpu->flags.cf());
            EXPECT_TRUE(m_cpu->flags.sf());
            EXPECT_FALSE(m_cpu->flags.zf());
            EXPECT_EQ(m_cpu->gpr.get64(si), 0x20020 + elem);
            EXPECT_EQ(m_cpu->gpr.get64(di), 0x30040 + elem);
            EXPECT_EQ(m_cpu->gpr.get64(cx), 57u);
        }
    }
}

TEST(JitRepScan, RepeCmpsBulkMismatchLocatesTheFirstElementNotItsDifferingByte) {
    constexpr usize elements = 64;
    for (const bool wide : {false, true}) {
        for (usize width = 0; width < (wide ? 4u : 3u); ++width) {
            const usize elem = usize{1} << width;
            for (const usize mismatch : {0u, 1u, 5u, 17u, 63u}) {
                for (const usize byte : {usize{0}, elem - 1}) {
                    SCOPED_TRACE(::testing::Message() << wide << width << mismatch << byte);
                    GuestMemory memory;
                    ASSERT_TRUE(memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite));
                    ASSERT_TRUE(memory.map_region(0x30000, 0x1000, MemoryProtection::ReadWrite));
                    const std::vector<u8> source(elements * elem, 0x11);
                    auto target = source;
                    target[mismatch * elem + byte] = 0x91;
                    ASSERT_TRUE(memory.write_bytes(0x20000, source.data(), source.size()));
                    ASSERT_TRUE(memory.write_bytes(0x30000, target.data(), target.size()));
                    CpuState cpu;
                    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                    cpu.gpr.set64(si, 0x20000);
                    cpu.gpr.set64(di, 0x30000);
                    cpu.gpr.set64(cx, elements);
                    cmps_equal[width](&cpu, &memory);
                    ASSERT_FALSE(cpu.pending_raise.active);
                    EXPECT_EQ(cpu.gpr.get64(si), 0x20000 + (mismatch + 1) * elem);
                    EXPECT_EQ(cpu.gpr.get64(di), 0x30000 + (mismatch + 1) * elem);
                    EXPECT_EQ(cpu.gpr.get64(cx), elements - mismatch - 1);
                    EXPECT_TRUE(cpu.flags.cf());
                    EXPECT_TRUE(cpu.flags.sf());
                    EXPECT_FALSE(cpu.flags.zf());
                    // 11..11 - (11..11 with bit7 of this byte set).
                    EXPECT_EQ(cpu.flags.of(), byte == elem - 1);
                    EXPECT_EQ(cpu.flags.pf(), byte != 0);
                    EXPECT_FALSE(cpu.flags.af());
                }
            }
        }
    }
}

TEST(JitRepScan, NoncanonicalReadFaultUsesSharedGpRecordAndPreservesEntryState) {
    // Same Windows #GP record as high_address_fault_probe.expected: read -1.
    for (usize width = 0; width < 4; ++width) {
        const u64 elem = u64{1} << width;
        for (int side = 0; side < 3; ++side) {
            const bool cmps = side != 0;
            for (const bool repe : {false, true}) {
                const std::vector<u64> bad_addresses = elem == 1
                    ? std::vector<u64>{0x0000800000000000ull, 0xFFFF7FFFFFFFFFFFull}
                    : std::vector<u64>{0x0000800000000000ull, 0xFFFF7FFFFFFFFFFFull,
                                       0x0000800000000000ull - elem + 1};
                for (const u64 bad : bad_addresses) {
                    SCOPED_TRACE(::testing::Message() << width << side << repe << std::hex << bad);
                    GuestMemory memory;
                    ASSERT_TRUE(memory.map_region(0x2000, 0x1000, MemoryProtection::ReadWrite));
                    CpuState cpu;
                    cpu.init_long_mode();
                    cpu.flags.rflags = 0x8D7;
                    cpu.set_flag_op(CpuState::FlagOp::Sub64);
                    cpu.flag_src = 3;
                    cpu.flag_dst = 0xFFFFFFFFFFFFFFFEull;
                    cpu.cf_overlay = 2;
                    cpu.of_overlay = 1;
                    const CpuState entry = cpu;
                    cpu.gpr.set64(si, side == 1 ? bad : 0x2000);
                    cpu.gpr.set64(di, side == 1 ? 0x2000 : bad);
                    cpu.gpr.set64(cx, 17);
                    cpu.gpr.set64(ax, 0x11);
                    select_helper(width, cmps, repe)(&cpu, &memory);
                    ASSERT_TRUE(cpu.pending_raise.active);
                    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
                    EXPECT_EQ(cpu.pending_raise.num_args, 2u);
                    EXPECT_EQ(cpu.pending_raise.args[0], 0u);
                    EXPECT_EQ(cpu.pending_raise.args[1], std::numeric_limits<u64>::max());
                    EXPECT_EQ(cpu.gpr.get64(cx), 17u);
                    EXPECT_EQ(cpu.gpr.get64(si), side == 1 ? bad : 0x2000);
                    EXPECT_EQ(cpu.gpr.get64(di), side == 1 ? 0x2000 : bad);
                    EXPECT_EQ(cpu.flags.rflags, entry.flags.rflags);
                    EXPECT_EQ(cpu.flag_op, entry.flag_op);
                    EXPECT_EQ(cpu.flag_src, entry.flag_src);
                    EXPECT_EQ(cpu.flag_dst, entry.flag_dst);
                    EXPECT_EQ(cpu.cf_overlay, entry.cf_overlay);
                    EXPECT_EQ(cpu.of_overlay, entry.of_overlay);
                }
            }
        }
    }
}

TEST_F(JitExecTest, Rep32ComparisonFaultPublishesItsInstructionRipAfterPrecedingNativeMove) {
    for (usize width = 0; width < 3; ++width) {
        const usize elem = usize{1} << width;
        for (const bool cmps : {false, true}) {
            for (const bool repe : {false, true}) {
                SCOPED_TRACE(::testing::Message() << width << cmps << repe);
                std::vector<u8> code = {0xB8, static_cast<u8>(repe ? 0 : 0x11), 0, 0, 0};
                const u64 rep_rip = 0x1000 + code.size();
                code.push_back(repe ? 0xF3 : 0xF2);
                if (width == 1) code.push_back(0x66);
                code.push_back(cmps ? (width ? 0xA7 : 0xA6) : (width ? 0xAF : 0xAE));
                code.insert(code.end(), {0xBB, 0x78, 0x56, 0x34, 0x12});
                ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
                const std::vector<u8> source(elem * 2, repe ? 0 : 0x11), destination(elem, 0);
                ASSERT_TRUE(m_memory->write_bytes(0x2000, source.data(), source.size()));
                ASSERT_TRUE(m_memory->write_bytes(0x11000 - elem, destination.data(), elem));
                m_cpu->init_protected_mode();
                m_cpu->rip = 0x1000;
                m_cpu->flags.rflags = 0x8D7;
                m_cpu->gpr.set32(cx, 2);
                m_cpu->gpr.set32(si, 0x2000);
                m_cpu->gpr.set32(di, static_cast<u32>(0x11000 - elem));
                m_cpu->gpr.set32(EBX, 0xDEADBEEF);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                auto block = scan_block(code, false);
                auto* compiled = m_jit->compile(block);
                ASSERT_NE(compiled, nullptr);
                EXPECT_EQ(compiled->native_insns, 3u);
                test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu);
                ASSERT_TRUE(m_cpu->pending_raise.active);
                EXPECT_EQ(m_cpu->pending_raise.args[1], 0x11000u);
                EXPECT_EQ(m_cpu->gpr.get32(cx), 1u);
                EXPECT_EQ(m_cpu->gpr.get32(di), 0x11000u);
                EXPECT_EQ(m_cpu->gpr.get32(EBX), 0xDEADBEEFu);
                EXPECT_EQ(m_cpu->rip, rep_rip);
                m_cpu->materialize_lazy_flags();
                EXPECT_EQ(m_cpu->flags.rflags, 0x8D7u);
            }
        }
    }
}

TEST(JitRepScan, RepeCmpsFindsLastMismatchInOneAndTwoNativeWordSpans) {
    for (const bool wide : {false, true}) {
        for (usize width = 0; width < (wide ? 4u : 3u); ++width) {
            const usize elem = usize{1} << width;
            for (const usize bytes : {sizeof(u64), 2 * sizeof(u64)}) {
                SCOPED_TRACE(::testing::Message() << wide << width << bytes);
                GuestMemory memory;
                ASSERT_TRUE(memory.map_region(0x2000, 0x1000, MemoryProtection::ReadWrite));
                ASSERT_TRUE(memory.map_region(0x4000, 0x1000, MemoryProtection::ReadWrite));
                const std::vector<u8> source(bytes, 0x11);
                auto target = source;
                std::fill(target.end() - elem, target.end(), 0x22);
                ASSERT_TRUE(memory.write_bytes(0x2000, source.data(), source.size()));
                ASSERT_TRUE(memory.write_bytes(0x4000, target.data(), target.size()));
                CpuState cpu;
                if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
                cpu.gpr.set64(si, 0x2000);
                cpu.gpr.set64(di, 0x4000);
                cpu.gpr.set64(cx, bytes / elem);
                cmps_equal[width](&cpu, &memory);
                ASSERT_FALSE(cpu.pending_raise.active);
                EXPECT_EQ(cpu.gpr.get64(si), 0x2000 + bytes);
                EXPECT_EQ(cpu.gpr.get64(di), 0x4000 + bytes);
                EXPECT_EQ(cpu.gpr.get64(cx), 0u);
                EXPECT_TRUE(cpu.flags.cf());
                EXPECT_TRUE(cpu.flags.sf());
                EXPECT_TRUE(cpu.flags.af());
                EXPECT_FALSE(cpu.flags.zf());
                EXPECT_FALSE(cpu.flags.pf());
                EXPECT_FALSE(cpu.flags.of());
            }
        }
    }
}

TEST_F(JitExecTest, RepComparisonFaultPreservesFlagsFromPrecedingNativeArithmetic) {
    // Intel SUB: 80000000h - 1 = 7fffffffh, so OF/AF/PF are set
    // (defined status 0814h); CF/ZF/SF are clear. REP faults restore these
    // entry flags, not the successful comparison immediately before fault.
    constexpr u64 status = FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF;
    constexpr u64 expected = FLAG_OF | FLAG_AF | FLAG_PF;
    for (const bool wide : {false, true}) {
        for (const bool cmps : {false, true}) {
            for (const bool repe : {false, true}) {
                SCOPED_TRACE(::testing::Message() << wide << cmps << repe);
                std::vector<u8> code = {
                    0xB8, 0x00, 0x00, 0x00, 0x80, // MOV EAX,80000000h
                    0x83, 0xE8, 0x01};            // SUB EAX,1 (native producer)
                const u64 rep_rip = 0x1000 + code.size();
                code.push_back(repe ? 0xF3 : 0xF2);
                code.push_back(cmps ? 0xA6 : 0xAE);
                code.insert(code.end(), {0xBB, 0x78, 0x56, 0x34, 0x12});
                ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
                const u8 source = 0x11;
                const u8 destination = cmps ? (repe ? source : u8{0x22})
                                            : (repe ? u8{0xFF} : u8{0});
                ASSERT_TRUE(m_memory->write_u8(0x2000, source));
                ASSERT_TRUE(m_memory->write_u8(0x2001, source));
                ASSERT_TRUE(m_memory->write_u8(0x10FFF, destination));
                if (wide) m_cpu->init_long_mode(); else m_cpu->init_protected_mode();
                m_cpu->rip = 0x1000;
                m_cpu->gpr.set64(cx, 2);
                m_cpu->gpr.set64(si, 0x2000);
                m_cpu->gpr.set64(di, 0x10FFF);
                m_cpu->gpr.set32(EBX, 0xDEADBEEF);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                auto block = scan_block(code, wide);
                auto* compiled = m_jit->compile(block);
                ASSERT_NE(compiled, nullptr);
                EXPECT_EQ(compiled->native_insns, 4u);
                test::execute_like_dispatcher(*m_jit, *compiled, *m_cpu);
                ASSERT_TRUE(m_cpu->pending_raise.active);
                EXPECT_EQ(m_cpu->pending_raise.args[1], 0x11000u);
                EXPECT_EQ(m_cpu->gpr.get64(cx), 1u);
                EXPECT_EQ(m_cpu->gpr.get64(di), 0x11000u);
                EXPECT_EQ(m_cpu->gpr.get32(ax), 0x7FFFFFFFu);
                EXPECT_EQ(m_cpu->gpr.get32(EBX), 0xDEADBEEFu);
                EXPECT_EQ(m_cpu->rip, rep_rip);
                m_cpu->materialize_lazy_flags();
                EXPECT_EQ(m_cpu->flags.rflags & status, expected);
            }
        }
    }
}
