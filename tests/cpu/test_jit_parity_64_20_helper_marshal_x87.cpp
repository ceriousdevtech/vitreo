// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): 64-bit EAs
// through the helper marshal, CMPXCHG and x87.

#include "jit_parity_64_fixture.hpp"

// ---------------------------------------------------------------------------
// 64-bit effective addresses must survive the helper marshal.
//
// Two lowerings passed a full 64-bit EA to a C helper through a 32-bit MOV,
// which zero-extends and silently drops bits 63:32. Both only misbehave above
// 4 GB, which is where x64 images and heaps actually live, so every existing
// test missed them (the oracle harness runs at 0x04000000).
// ---------------------------------------------------------------------------

// CMPXCHG r/m32, r32 marshalled its address with MOV W2, W2 — a self-move.
// This is the instruction behind every InterlockedCompareExchange and spinlock
// in x64 Windows code, so a truncated address is silent memory corruption.
TEST_F(JitParity64Test, Cmpxchg32MemoryUsesFullEffectiveAddress) {
    constexpr GuestAddr kHigh = 0x140006000ull;
    map_tracked_region(kHigh, 0x1000);
    ASSERT_TRUE(m_memory->write_u32(kHigh, 0x11223344).is_ok());
    // RAX == [mem] so the exchange takes the store path and writes ECX back.
    check_native_parity({0x0F, 0xB1, 0x0B},          // cmpxchg [rbx], ecx
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kHigh);
            cpu.gpr.set64(RAX, 0x11223344);
            cpu.gpr.set64(RCX, 0x55667788);
        });
}

// The mismatching case must load [mem] into EAX rather than storing.
TEST_F(JitParity64Test, Cmpxchg32MemoryMismatchLoadsAccumulator) {
    constexpr GuestAddr kHigh = 0x140006000ull;
    map_tracked_region(kHigh, 0x1000);
    ASSERT_TRUE(m_memory->write_u32(kHigh, 0xAABBCCDD).is_ok());
    check_native_parity({0x0F, 0xB1, 0x0B},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, kHigh);
            cpu.gpr.set64(RAX, 0x11223344);          // != [mem]
            cpu.gpr.set64(RCX, 0x55667788);
        });
}

TEST_F(JitParity64Test, LockCmpxchg32RetryBranchAfterClearingLowBit) {
    constexpr GuestAddr cell = 0x140006000ull;
    map_tracked_region(cell, 0x1000);
    for (const u32 initial : {0u, 1u, 3u, 0x80000001u}) {
        SCOPED_TRACE(initial);
        ASSERT_TRUE(m_memory->write_u32(cell, initial).is_ok());
        // mov ecx,eax; and ecx,-2; lock cmpxchg [r14],ecx; jne +2.
        // Both CAS outcomes must replace the AND's flags, and success must
        // retain the old accumulator even though the stored low bit is zero.
        check_native_parity(
            {0x89, 0xC1, 0x83, 0xE1, 0xFE,
             0xF0, 0x41, 0x0F, 0xB1, 0x0E, 0x75, 0x02},
            [](CpuState& cpu) {
                cpu.gpr.set64(14, cell);
                cpu.gpr.set64(RAX, 1);
            });
    }
}

TEST_F(JitParity64Test, LockCmpxchgRetryLoopPreservesAccumulatorForSuccessorTest) {
    // Intel SDM Vol. 2 CMPXCHG: success preserves EAX; failure loads DEST
    // into EAX. The subsequent TEST AL must see that accumulator, not ECX.
    constexpr GuestAddr cell = 0x140006000ull;
    map_tracked_region(cell, 0x1000);
    const u8 loop[] = {0x89, 0xC1, 0x83, 0xE1, 0xFE,
                       0xF0, 0x41, 0x0F, 0xB1, 0x0E, 0x75, 0xF4};
    const u8 tail[] = {0xA8, 0x01, 0x74, 0x02}; // test al,1; je +2
    auto loop_block = build_block(loop, sizeof(loop));
    auto tail_block = build_block(tail, sizeof(tail));
    tail_block.start_addr = 0x1000 + sizeof(loop);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, loop, sizeof(loop)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(tail_block.start_addr, tail, sizeof(tail)).is_ok());
    auto* compiled_loop = m_jit->compile(loop_block);
    auto* compiled_tail = m_jit->compile(tail_block);
    ASSERT_NE(compiled_loop, nullptr);
    ASSERT_NE(compiled_tail, nullptr);
    for (const u32 initial : {1u, 3u, 0x80000001u}) {
        SCOPED_TRACE(initial);
        ASSERT_TRUE(m_memory->write_u32(cell, initial).is_ok());
        CpuState state;
        init_cpu(state);
        state.gpr.set64(14, cell);
        state.gpr.set64(RAX, 0); // Force a failed CAS before the retry succeeds.
        for (unsigned attempt = 0; attempt < 3 && state.rip == 0x1000; ++attempt)
            test::execute_like_dispatcher(*m_jit, *compiled_loop, state);
        ASSERT_EQ(state.rip, tail_block.start_addr);
        EXPECT_EQ(state.gpr.get64(RAX), initial);
        EXPECT_EQ(m_memory->read_u32(cell).value(), initial & ~1u);
        test::execute_like_dispatcher(*m_jit, *compiled_tail, state);
        state.materialize_lazy_flags();
        EXPECT_FALSE(state.flags.zf());
        EXPECT_EQ(state.rip, tail_block.start_addr + sizeof(tail));
    }
}

// FILD m64 has no native lowering, so it reaches the generic x87 helper tail
// where the EA was moved with a 32-bit MOV. FISTP m64 does have one, so a
// round-trip isolates the failure to the load.
TEST_F(JitParity64Test, X87FildM64UsesFullEffectiveAddress) {
    constexpr GuestAddr kSrc = 0x140006000ull;
    constexpr GuestAddr kDst = 0x140007000ull;
    map_tracked_region(kSrc, 0x1000);
    map_tracked_region(kDst, 0x1000);
    constexpr u64 kValue = 0xFEDCBA9876543210ull;
    ASSERT_TRUE(m_memory->write_u64(kSrc, kValue).is_ok());
    ASSERT_TRUE(m_memory->write_u64(kDst, 0).is_ok());
    check_native_parity({0xDF, 0x28, 0xDF, 0x39},    // fild [rax]; fistp [rcx]
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kSrc);
            cpu.gpr.set64(RCX, kDst);
        },
        /*check_flags=*/false);
    const auto stored = m_memory->read_u64(kDst);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), kValue);
}

TEST_F(JitParity64Test, X87FldFaddFstpM64ForwardingPreservesFullState) {
    // Intel/AMD x87 contract: FLD pushes, FADD updates ST(0) under FCW.PC/RC,
    // and FSTP stores before popping.  Exercise every precision-control mode;
    // only PC=53 may take the D0 fast chain, while PC=24/64 must invalidate it
    // and retain the interpreter-backed arithmetic semantics.
    constexpr GuestAddr kData = 0x140006000ull;
    const double lhs = 1.5;
    const double rhs = 2.25;
    map_tracked_region(kData, 0x1000);
    m_compare_memory = true;
    for (u16 cw : {u16{0x007F}, u16{0x027F}, u16{0x037F}}) {
        ASSERT_TRUE(m_memory->write_bytes(
            kData, reinterpret_cast<const Byte*>(&lhs), sizeof(lhs)).is_ok());
        ASSERT_TRUE(m_memory->write_bytes(
            kData + 8, reinterpret_cast<const Byte*>(&rhs), sizeof(rhs)).is_ok());
        ASSERT_TRUE(m_memory->write_u64(kData + 16, 0).is_ok());
        check_native_parity({
                0xDD, 0x00,        // fld  qword ptr [rax]
                0xDC, 0x40, 0x08,  // fadd qword ptr [rax+8]
                0xDD, 0x58, 0x10,  // fstp qword ptr [rax+16]
            },
            [cw](CpuState& cpu) {
                cpu.gpr.set64(RAX, kData);
                cpu.fpu.control_word = cw;
                cpu.flags.rflags = 0xA43;
            });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_interp_cpu.fpu.control_word,
                  m_last_jit_cpu.fpu.control_word);
        EXPECT_EQ(m_last_interp_cpu.fpu.status_word,
                  m_last_jit_cpu.fpu.status_word);
        EXPECT_EQ(m_last_interp_cpu.fpu.tag_word,
                  m_last_jit_cpu.fpu.tag_word);
        // Empty physical stack slots contain architecturally unobservable
        // stale bits after FSTP.  Compare every live slot byte-for-byte while
        // the tag word separately proves the same slots are live.
        for (u8 physical = 0; physical < 8; ++physical) {
            const u16 tag = static_cast<u16>(
                (m_last_interp_cpu.fpu.tag_word >> (physical * 2)) & 3u);
            if (tag != static_cast<u16>(FpuTag::Empty)) {
                EXPECT_EQ(std::memcmp(&m_last_interp_cpu.fpu.st[physical],
                                      &m_last_jit_cpu.fpu.st[physical],
                                      sizeof(Float80)), 0)
                    << "live ST physical slot " << unsigned{physical}
                    << " diverged at FCW=0x" << std::hex << cw;
            }
        }
        const auto stored = m_memory->read_u64(kData + 16);
        ASSERT_TRUE(stored.is_ok());
        u64 expected = 0;
        const double sum = 3.75;
        std::memcpy(&expected, &sum, sizeof(expected));
        EXPECT_EQ(stored.value(), expected);
    }
}

TEST_F(JitParity64Test, X87FldFaddFstpM64EmitsAdjacentD0Consumers) {
    static const u8 code[] = {
        0xDD, 0x00,        // fld  qword ptr [rax]
        0xDC, 0x40, 0x08,  // fadd qword ptr [rax+8]
        0xDD, 0x58, 0x10,  // fstp qword ptr [rax+16]
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)).is_ok());
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(compiled->native_insns, 3u);

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize count = compiled->code_size / sizeof(u32);
    const auto d0_valid_consumers = std::count_if(
        words, words + count, [](u32 word) {
            // CBNZ W6, label: imm19 is intentionally masked out.
            return (word & 0xFF00001Fu) == 0x35000006u;
        });
    EXPECT_EQ(d0_valid_consumers, 2)
        << "FADD and FSTP must each expose a runtime-guarded D0 fast edge";
}

TEST_F(JitParity64Test, X87Fld1SelfFaddFstpForwardingPreservesFullState) {
    // Intel SDM Vol. 2: FLD1 pushes exact +1, D8 C0 computes
    // ST(0)+ST(0), and FSTP stores before popping. Only PC=53 may use the
    // binary64 D0 mirror; PC=24/64 must take the Float80 helper path.
    constexpr GuestAddr kData = 0x140006000ull;
    map_tracked_region(kData, 0x1000);
    m_compare_memory = true;
    for (u16 cw : {u16{0x007F}, u16{0x027F}, u16{0x067F},
                   u16{0x0A7F}, u16{0x0E7F}, u16{0x037F}}) {
        ASSERT_TRUE(m_memory->write_u64(kData, 0).is_ok());
        check_native_parity({
                0xD9, 0xE8,              // fld1
                0xD8, 0xC0,              // fadd st(0),st(0)
                0xDD, 0x18,              // fstp qword ptr [rax]
            },
            [cw](CpuState& cpu) {
                cpu.gpr.set64(RAX, kData);
                cpu.fpu.control_word = cw;
                cpu.fpu.status_word = FPU_SW_DE | FPU_SW_PE;
                cpu.flags.rflags = 0xA43;
            });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.fpu.control_word,
                  m_last_interp_cpu.fpu.control_word);
        EXPECT_EQ(m_last_jit_cpu.fpu.status_word,
                  m_last_interp_cpu.fpu.status_word);
        EXPECT_EQ(m_last_jit_cpu.fpu.tag_word,
                  m_last_interp_cpu.fpu.tag_word);
        const auto stored = m_memory->read_u64(kData);
        ASSERT_TRUE(stored.is_ok());
        u64 expected = 0;
        const double two = 2.0;
        std::memcpy(&expected, &two, sizeof(expected));
        EXPECT_EQ(stored.value(), expected);
    }
}

TEST_F(JitParity64Test, X87ConstantPushMaskedOverflowMatchesInterpreter) {
    for (const u8 opcode : {u8{0xE8}, u8{0xEE}}) {
        SCOPED_TRACE(opcode == 0xE8 ? "FLD1" : "FLDZ");
        check_native_parity({0xD9, opcode}, [](CpuState& cpu) {
            cpu.fpu.control_word = 0x027F;
            cpu.fpu.status_word = 0;
            cpu.fpu.tag_word = 0x0000;
            cpu.flags.rflags = 0xA43;
            for (u8 i = 0; i < 8; ++i) {
                cpu.fpu.st[i] = Float80(double(i + 1));
            }
        });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x3A41u);
        EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x8000u);
        const Float80 indefinite = FpuState::indefinite();
        EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.st[7], &indefinite,
                              sizeof(indefinite)), 0);
        for (u8 i = 0; i < 7; ++i) {
            EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.st[i],
                                  &m_last_interp_cpu.fpu.st[i],
                                  sizeof(Float80)), 0);
        }
        EXPECT_EQ(m_last_jit_cpu.flags.rflags & 0x8D5u, 0xA43u & 0x8D5u);
    }
}

TEST_F(JitParity64Test, X87ConstantPushUnmaskedOverflowDefersAndFwaitFaults) {
    for (const u8 opcode : {u8{0xE8}, u8{0xEE}}) {
        SCOPED_TRACE(opcode == 0xE8 ? "FLD1" : "FLDZ");
        check_native_parity({0xD9, opcode, 0x9B}, [](CpuState& cpu) {
            cpu.fpu.control_word = 0x027E;
            cpu.fpu.status_word = 0;
            cpu.fpu.tag_word = 0x0000;
            cpu.flags.rflags = 0xA43;
            for (u8 i = 0; i < 8; ++i) {
                cpu.fpu.st[i] = Float80(double(i + 1));
            }
        });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x82C1u);
        EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x0000u);
        EXPECT_EQ(m_last_jit_cpu.fpu.top(), 0u);
        for (u8 i = 0; i < 8; ++i) {
            EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.st[i],
                                  &m_last_interp_cpu.fpu.st[i],
                                  sizeof(Float80)), 0);
        }
        EXPECT_TRUE(m_last_jit_cpu.pending_raise.active);
        EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000092u);
        EXPECT_EQ(m_last_jit_cpu.rip, 0x1002u);
        EXPECT_EQ(m_last_jit_cpu.flags.rflags & 0x8D5u, 0xA43u & 0x8D5u);
    }
}

TEST_F(JitParity64Test, X87UnmaskedSourceAndZeroDivideMatchWindowsDeferral) {
    check_native_parity({0xD8, 0xC0, 0x9B}, [](CpuState& cpu) {
        cpu.fpu.control_word = 0x027E;
        cpu.fpu.status_word = 0;
        cpu.fpu.tag_word = 0xFFFF;
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x80C1u);
    EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0xFFFFu);
    ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000092u);

    check_native_parity({0xD8, 0xF1, 0x9B}, [](CpuState& cpu) {
        cpu.fpu.control_word = 0x027B;
        cpu.fpu.status_word = 0;
        cpu.fpu.tag_word = 0xFFF4;
        cpu.fpu.st[0] = Float80(1.0);
        cpu.fpu.st[1] = Float80(0.0);
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x8084u);
    EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0xFFF4u);
    EXPECT_DOUBLE_EQ(m_last_jit_cpu.fpu.st[0].to_double(), 1.0);
    ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC000008Eu);
}

TEST_F(JitParity64Test, X87DenormalOperandMaskAndDeferredFaultMatchInterpreter) {
    const auto seed = [](CpuState& cpu, u16 cw) {
        cpu.fpu.control_word = cw;
        cpu.fpu.status_word = 0;
        cpu.fpu.tag_word = 0xFFFC;
        cpu.fpu.st[0] = Float80::from_extended(1, 0);
        cpu.flags.rflags = 0xA43;
    };

    check_native_parity({0xD8, 0xC0}, [&](CpuState& cpu) {
        seed(cpu, 0x037F); // #D masked
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word & 0x80BFu, FPU_SW_DE);
    EXPECT_EQ(m_last_jit_cpu.fpu.st[0].mantissa, 2u);
    EXPECT_EQ(m_last_jit_cpu.fpu.st[0].exponent_sign, 0u);

    check_native_parity({0xD8, 0xC0, 0x9B}, [&](CpuState& cpu) {
        seed(cpu, 0x037D); // #D unmasked
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_DE));
    EXPECT_EQ(m_last_jit_cpu.fpu.st[0].mantissa, 1u);
    EXPECT_EQ(m_last_jit_cpu.fpu.st[0].exponent_sign, 0u);
    ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000090u);
    EXPECT_EQ(m_last_jit_cpu.rip, 0x1002u);
    EXPECT_EQ(m_last_jit_cpu.flags.rflags & 0x8D5u, 0xA43u & 0x8D5u);
}

TEST_F(JitParity64Test, X87OrderedNanCompareHonorsInvalidMask) {
    check_native_parity({0xD8, 0xD0, 0x9B}, [](CpuState& cpu) {
        cpu.fpu.control_word = 0x027E;
        cpu.fpu.status_word = FPU_SW_C3;
        cpu.fpu.tag_word = 0xFFFE;
        cpu.fpu.st[0] = FpuState::indefinite();
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word,
              static_cast<u16>(FPU_SW_B | FPU_SW_ES | FPU_SW_IE | FPU_SW_C3));
    ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000090u);

    check_native_parity({0xD8, 0xD0}, [](CpuState& cpu) {
        cpu.fpu.control_word = 0x027F;
        cpu.fpu.status_word = 0;
        cpu.fpu.tag_word = 0xFFFE;
        cpu.fpu.st[0] = FpuState::indefinite();
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_FALSE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word,
              static_cast<u16>(FPU_SW_IE | FPU_SW_C0 |
                               FPU_SW_C2 | FPU_SW_C3));
}

TEST_F(JitParity64Test, X87MultiResultUnmaskedOverflowIsAtomic) {
    for (const std::array<u8, 2> producer : {
             std::array<u8, 2>{0xD9, 0xFB}, // FSINCOS
             std::array<u8, 2>{0xD9, 0xF2}, // FPTAN
             std::array<u8, 2>{0xD9, 0xF4}, // FXTRACT
         }) {
        SCOPED_TRACE(static_cast<unsigned>(producer[1]));
        check_native_parity({producer[0], producer[1], 0x9B},
            [](CpuState& cpu) {
                cpu.fpu.control_word = 0x027E;
                cpu.fpu.status_word = 0;
                cpu.fpu.tag_word = 0x0000;
                for (u8 i = 0; i < 8; ++i) {
                    cpu.fpu.st[i] = Float80(double(i + 1));
                }
            });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x82C1u);
        EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x0000u);
        EXPECT_EQ(m_last_jit_cpu.fpu.top(), 0u);
        for (u8 i = 0; i < 8; ++i) {
            EXPECT_DOUBLE_EQ(m_last_jit_cpu.fpu.st[i].to_double(), double(i + 1));
        }
        ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
        EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000092u);
    }
}

TEST_F(JitParity64Test, X87MultiResultMaskedOverflowMatchesWindowsCompletion) {
    const Float80 indefinite = FpuState::indefinite();
    for (const std::array<u8, 2> producer : {
             std::array<u8, 2>{0xD9, 0xFB},
             std::array<u8, 2>{0xD9, 0xF2},
             std::array<u8, 2>{0xD9, 0xF4},
         }) {
        check_native_parity({producer[0], producer[1]}, [](CpuState& cpu) {
            cpu.fpu.control_word = 0x027F;
            cpu.fpu.status_word = 0;
            cpu.fpu.tag_word = 0x0000;
            for (u8 i = 0; i < 8; ++i) cpu.fpu.st[i] = Float80(double(i + 1));
        });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_FALSE(m_last_jit_cpu.pending_raise.active);
        EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x3A41u);
        EXPECT_EQ(m_last_jit_cpu.fpu.top(), 7u);
        EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x8002u);
        EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.get_st(0), &indefinite,
                              sizeof(indefinite)), 0);
        EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.get_st(1), &indefinite,
                              sizeof(indefinite)), 0);
    }
}

TEST_F(JitParity64Test, X87PendingOverflowFaultsWaitingFaddBeforeEffects) {
    check_native_parity({
            0xD9, 0xEE, // fldz records unmasked stack overflow
            0xD8, 0xC0, // fadd st(0),st(0) delivers it
        }, [](CpuState& cpu) {
            cpu.fpu.control_word = 0x027E;
            cpu.fpu.status_word = 0;
            cpu.fpu.tag_word = 0x0000;
            cpu.flags.rflags = 0xA43;
            for (u8 i = 0; i < 8; ++i) {
                cpu.fpu.st[i] = Float80(double(i + 1));
            }
        });
    ASSERT_TRUE(m_last_parity_ok);
    ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000092u);
    EXPECT_EQ(m_last_jit_cpu.rip, 0x1002u);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x82C1u);
    EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x0000u);
    EXPECT_DOUBLE_EQ(m_last_jit_cpu.fpu.st[0].to_double(), 1.0);
    EXPECT_EQ(m_last_jit_cpu.flags.rflags & 0x8D5u, 0xA43u & 0x8D5u);
}

TEST_F(JitParity64Test, X87FnclexAndFninitDiscardPendingWithoutWaiting) {
    const auto run = [&](std::initializer_list<u8> code, bool fnclex) {
        check_native_parity(code, [](CpuState& cpu) {
            cpu.fpu.control_word = 0x027E;
            cpu.fpu.status_word = 0;
            cpu.fpu.tag_word = 0x0000;
            for (u8 i = 0; i < 8; ++i) {
                cpu.fpu.st[i] = Float80(double(i + 1));
            }
        });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_FALSE(m_last_jit_cpu.pending_raise.active);
        if (fnclex) {
            // FNCLEX preserves the full stack, so FLD1 now gets a masked
            // overflow only because IE remains unmasked: it records pending.
            EXPECT_EQ(m_last_jit_cpu.fpu.status_word & FPU_SW_ES, FPU_SW_ES);
        } else {
            EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x3800u);
            EXPECT_DOUBLE_EQ(m_last_jit_cpu.fpu.get_st(0).to_double(), 1.0);
        }
    };
    run({0xD9, 0xEE, 0xDB, 0xE2, 0xD9, 0xE8}, true);
    run({0xD9, 0xEE, 0xDB, 0xE3, 0xD9, 0xE8}, false);
}

TEST_F(JitParity64Test, X87FnstswIgnoresPendingAndFnopDeliversSpecificStatus) {
    check_native_parity({
            0xDF, 0xE0, // fnstsw ax is non-waiting
            0xD9, 0xD0, // fnop is waiting
        }, [](CpuState& cpu) {
            cpu.fpu.control_word = 0x037B; // divide-by-zero unmasked
            cpu.fpu.status_word = FPU_SW_ES | FPU_SW_ZE;
        });
    ASSERT_TRUE(m_last_parity_ok);
    ASSERT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC000008Eu);
    EXPECT_EQ(m_last_jit_cpu.rip, 0x1002u);
    EXPECT_EQ(m_last_jit_cpu.gpr.get16(RAX), FPU_SW_ES | FPU_SW_ZE);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word,
              FPU_SW_B | FPU_SW_ES | FPU_SW_ZE);
}

TEST_F(JitParity64Test, X87StaleEsWithMaskedStickyFlagDoesNotDeliver) {
    check_native_parity({0xD9, 0xD0}, [](CpuState& cpu) {
        cpu.fpu.control_word = 0x037F;
        cpu.fpu.status_word = FPU_SW_ES | FPU_SW_ZE;
        cpu.flags.rflags = 0xA43;
    });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_FALSE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, FPU_SW_ES | FPU_SW_ZE);
    EXPECT_EQ(m_last_jit_cpu.flags.rflags & 0x8D5u, 0xA43u & 0x8D5u);
}

TEST_F(JitParity64Test, X87Fld1SelfFaddFstpEmitsTwoGuardedD0Edges) {
    static const u8 code[] = {
        0xD9, 0xE8,  // fld1
        0xD8, 0xC0,  // fadd st(0),st(0)
        0xDD, 0x18,  // fstp qword ptr [rax]
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)).is_ok());
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize count = compiled->code_size / sizeof(u32);
    const auto d0_valid_consumers = std::count_if(
        words, words + count, [](u32 word) {
            return (word & 0xFF00001Fu) == 0x35000006u;  // CBNZ W6
        });
    EXPECT_EQ(d0_valid_consumers, 2)
        << "register FADD and FSTP must each guard their adjacent D0 edge";
}

TEST_F(JitParity64Test, X87Fld1SelfFaddFstpFaultDoesNotPop) {
    constexpr GuestAddr kUnmapped = 0x14000F000ull;
    check_native_parity({
            0xD9, 0xE8,  // fld1
            0xD8, 0xC0,  // fadd st(0),st(0)
            0xDD, 0x18,  // fstp qword ptr [rax] -- faults before pop
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kUnmapped);
            cpu.fpu.control_word = 0x027F;
            cpu.flags.rflags = 0xA43;
        });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.args[1], kUnmapped);
    EXPECT_EQ(m_last_jit_cpu.rip, 0x1004u);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x3800u);
    EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x3FFFu);
    const Float80 expected(2.0);
    EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.st[7], &expected,
                          sizeof(expected)), 0);
}

TEST_F(JitParity64Test, X87FldFmulFstpM64ForwardingPreservesFullContract) {
    // Intel SDM Vol. 2 FMUL and AMD APM Vol. 5 FMUL: the m64real operand is
    // converted to extended precision, multiplied with ST(0), and written
    // back to ST(0); EFLAGS are unaffected. Only PC=53 finite binary64 work
    // may consume the D0 mirror. Other PC modes and exceptional operands must
    // invalidate it and retain the interpreter-backed x87 contract.
    constexpr GuestAddr kData = 0x140006000ull;
    struct Case {
        double lhs;
        double rhs;
    };
    struct Trial {
        u16 cw;
        Case operands;
    };
    const std::array<Trial, 5> trials{{
        {0x007F, {1.5, 2.25}},
        {0x027F, {1.5, 2.25}},
        {0x037F, {1.5, 2.25}},
        {0x027F, {0.0, std::numeric_limits<double>::infinity()}},
        {0x027F, {1.0, std::numeric_limits<double>::denorm_min()}},
    }};
    map_tracked_region(kData, 0x1000);
    m_compare_memory = true;
    for (const Trial& trial : trials) {
        const Case& test = trial.operands;
        ASSERT_TRUE(m_memory->write_bytes(
            kData, reinterpret_cast<const Byte*>(&test.lhs),
            sizeof(test.lhs)).is_ok());
        ASSERT_TRUE(m_memory->write_bytes(
            kData + 8, reinterpret_cast<const Byte*>(&test.rhs),
            sizeof(test.rhs)).is_ok());
        ASSERT_TRUE(m_memory->write_u64(kData + 16, 0).is_ok());
        check_native_parity({
                0xDD, 0x00,        // fld  qword ptr [rax]
                0xDC, 0x48, 0x08,  // fmul qword ptr [rax+8]
                0xDD, 0x58, 0x10,  // fstp qword ptr [rax+16]
            },
            [cw = trial.cw](CpuState& cpu) {
                cpu.gpr.set64(RAX, kData);
                cpu.fpu.control_word = cw;
                cpu.flags.rflags = 0xA43;
            });
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_last_interp_cpu.fpu.control_word,
                  m_last_jit_cpu.fpu.control_word);
        EXPECT_EQ(m_last_interp_cpu.fpu.status_word,
                  m_last_jit_cpu.fpu.status_word);
        EXPECT_EQ(m_last_interp_cpu.fpu.tag_word,
                  m_last_jit_cpu.fpu.tag_word);
        for (u8 physical = 0; physical < 8; ++physical) {
            const u16 tag = static_cast<u16>(
                (m_last_interp_cpu.fpu.tag_word >> (physical * 2)) & 3u);
            if (tag != static_cast<u16>(FpuTag::Empty)) {
                EXPECT_EQ(std::memcmp(&m_last_interp_cpu.fpu.st[physical],
                                      &m_last_jit_cpu.fpu.st[physical],
                                      sizeof(Float80)), 0)
                    << "live ST physical slot " << unsigned{physical}
                    << " diverged at FCW=0x" << std::hex << trial.cw;
            }
        }
    }
}

TEST_F(JitParity64Test, X87FldFmulFstpM64FaultInvalidatesForwarding) {
    constexpr GuestAddr kData = 0x140006000ull;
    constexpr GuestAddr kUnmapped = 0x14000F000ull;
    const double lhs = 1.5;
    map_tracked_region(kData, 0x1000);
    ASSERT_TRUE(m_memory->write_bytes(
        kData, reinterpret_cast<const Byte*>(&lhs), sizeof(lhs)).is_ok());
    check_native_parity({
            0xDD, 0x00,        // fld  qword ptr [rax]
            0xDC, 0x0B,        // fmul qword ptr [rbx] -- faults
            0xDD, 0x58, 0x08,  // fstp qword ptr [rax+8] -- not reached
        },
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kData);
            cpu.gpr.set64(RBX, kUnmapped);
            cpu.fpu.control_word = 0x027F;
            cpu.flags.rflags = 0xA43;
        });
    ASSERT_TRUE(m_last_parity_ok);
    EXPECT_TRUE(m_last_jit_cpu.pending_raise.active);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(m_last_jit_cpu.pending_raise.args[1], kUnmapped);
    EXPECT_EQ(m_last_jit_cpu.rip, 0x1002u);
    EXPECT_EQ(m_last_jit_cpu.flags.rflags & 0x8D5u, 0x841u);
    // FLD committed before the FMUL operand fault; FSTP was not reached.
    EXPECT_EQ(m_last_jit_cpu.fpu.control_word, 0x027Fu);
    EXPECT_EQ(m_last_jit_cpu.fpu.status_word, 0x3800u);
    EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0x3FFFu);
    const Float80 expected(lhs);
    EXPECT_EQ(std::memcmp(&m_last_jit_cpu.fpu.st[7], &expected,
                          sizeof(expected)), 0);
    const auto destination = m_memory->read_u64(kData + 8);
    ASSERT_TRUE(destination.is_ok());
    EXPECT_EQ(destination.value(), 0u);
}

TEST_F(JitParity64Test, X87FldFmulFstpM64EmitsAdjacentD0Consumers) {
    static const u8 code[] = {
        0xDD, 0x00,        // fld  qword ptr [rax]
        0xDC, 0x48, 0x08,  // fmul qword ptr [rax+8]
        0xDD, 0x58, 0x10,  // fstp qword ptr [rax+16]
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)).is_ok());
    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.instruction_count, 3u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_EQ(compiled->native_insns, 3u);

    const auto* words = static_cast<const u32*>(compiled->code);
    const usize count = compiled->code_size / sizeof(u32);
    EXPECT_EQ(std::count_if(words, words + count, [](u32 word) {
        return (word & 0xFF00001Fu) == 0x35000006u;  // CBNZ W6,label
    }), 2) << "FMUL and FSTP must each expose a guarded D0 fast edge";
}

TEST_F(JitParity64Test, X87UnmaskedNarrowingStoreSuppressesWriteAndPop) {
    constexpr GuestAddr kDestination = 0x140008000ull;
    constexpr u64 kSentinel = 0xA5A55A5AC3C33C3Cull;
    map_tracked_region(kDestination, 0x1000);
    m_compare_memory = true;

    struct Trial {
        Float80 value;
        u16 exception;
        bool m64;
    };
    const std::array<Trial, 4> trials{{
        {Float80::from_extended(0x8000000000000001ull, 0x7FFFu),
         FPU_SW_IE, false},
        {Float80::from_extended(0xFFFFFFFFFFFFFFFFull, 0x7FFEu),
         FPU_SW_OE, false},
        {Float80::from_extended(0x8000000000000000ull, 0x0001u),
         FPU_SW_UE, false},
        {Float80::from_extended(0x8000000000000400ull, 0x3FFFu),
         FPU_SW_PE, true},
    }};

    for (const Trial& trial : trials) {
        SCOPED_TRACE(::testing::Message()
                     << "exception=0x" << std::hex << trial.exception
                     << " width=" << std::dec << (trial.m64 ? 64 : 32));
        ASSERT_TRUE(m_memory->write_u64(kDestination, kSentinel).is_ok());
        const auto setup = [trial](CpuState& cpu) {
            cpu.gpr.set64(RAX, kDestination);
            cpu.fpu.control_word = static_cast<u16>(
                0x037Fu & ~trial.exception);
            cpu.fpu.status_word = 0;
            cpu.fpu.tag_word = 0xFFFCu;
            cpu.fpu.st[0] = trial.value;
            cpu.flags.rflags = 0xA43u;
        };
        if (trial.m64) {
            check_native_parity({0xDD, 0x18}, setup); // FSTP m64 [RAX]
        } else {
            check_native_parity({0xD9, 0x18}, setup); // FSTP m32 [RAX]
        }
        ASSERT_TRUE(m_last_parity_ok);
        EXPECT_EQ(m_memory->read_u64(kDestination).value(), kSentinel);
        EXPECT_EQ(m_last_jit_cpu.fpu.top(), 0u);
        EXPECT_EQ(m_last_jit_cpu.fpu.tag_word, 0xFFFCu);
        EXPECT_NE(m_last_jit_cpu.fpu.status_word & trial.exception, 0u);
        EXPECT_NE(m_last_jit_cpu.fpu.status_word & FPU_SW_ES, 0u);
        EXPECT_NE(m_last_jit_cpu.fpu.status_word & FPU_SW_B, 0u);
        EXPECT_EQ(m_last_interp_cpu.fpu.status_word,
                  m_last_jit_cpu.fpu.status_word);
    }
}

TEST_F(JitParity64Test, X87FnstcwUsesFullEffectiveAddress) {
    constexpr GuestAddr kDst = 0x140008000ull;
    map_tracked_region(kDst, 0x1000);
    ASSERT_TRUE(m_memory->write_u32(kDst, 0xA5A5A5A5u).is_ok());

    // FNSTCW word ptr [RAX] stores exactly the 16-bit control word and leaves
    // the adjacent bytes untouched. Requiring a native block prevents the
    // broken helper-address reconstruction from satisfying this by fallback.
    check_native_parity({0xD9, 0x38},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kDst);
            cpu.fpu.control_word = 0x027Fu;
        },
        /*check_flags=*/false);

    const auto stored = m_memory->read_u32(kDst);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0xA5A5027Fu);
}
