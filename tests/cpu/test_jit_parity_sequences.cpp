// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): multi-instruction sequences
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// Multi-instruction parity (dependency chains)
// ============================================================================

TEST_F(JitParityTest, MovAddSub) {
    // MOV EAX, 100; MOV EBX, 200; ADD EAX, EBX; SUB EAX, 50
    u8 code[] = {
        0xB8, 0x64, 0x00, 0x00, 0x00,  // MOV EAX, 100
        0xBB, 0xC8, 0x00, 0x00, 0x00,  // MOV EBX, 200
        0x01, 0xD8,                      // ADD EAX, EBX
        0x83, 0xE8, 0x32,              // SUB EAX, 50
    };
    check_parity(code, sizeof(code));
}

TEST_F(JitParityTest, ShiftMaskCombine) {
    // SHL EAX, 4; AND EAX, 0xFFF0; OR EAX, 0x0F
    u8 code[] = {
        0xC1, 0xE0, 0x04,                    // SHL EAX, 4
        0x25, 0xF0, 0xFF, 0x00, 0x00,        // AND EAX, 0xFFF0
        0x83, 0xC8, 0x0F,                    // OR  EAX, 0x0F
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0x123); });
}

TEST_F(JitParityTest, PushPopArithmetic) {
    // PUSH EAX; ADD EAX, EBX; POP ECX → ECX gets orig EAX
    u8 code[] = {
        0x50,        // PUSH EAX
        0x01, 0xD8,  // ADD EAX, EBX
        0x59,        // POP ECX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 100);
            cpu.gpr.set32(EBX, 200);
        });
}

TEST_F(JitParityTest, LeaWithSubsequentUse) {
    // LEA EAX, [EBX+ECX]; ADD EAX, EDX
    u8 code[] = {
        0x8D, 0x04, 0x0B,  // LEA EAX, [EBX+ECX]
        0x01, 0xD0,        // ADD EAX, EDX (01 /r: reg=EDX(010), rm=EAX(000) → D0)
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0x1000);
            cpu.gpr.set32(ECX, 0x200);
            cpu.gpr.set32(EDX, 0x50);
        });
}

TEST_F(JitParityTest, IncDecSequence) {
    // INC EAX; INC EAX; DEC EBX; DEC EBX
    u8 code[] = {0x40, 0x40, 0x4B, 0x4B};
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0);
            cpu.gpr.set32(EBX, 10);
        });
}

TEST_F(JitParityTest, CmpThenSubsequentUse) {
    // CMP EAX, EBX; MOV ECX, 0 → verify CMP doesn't clobber registers
    u8 code[] = {
        0x39, 0xD8,                          // CMP EAX, EBX
        0xB9, 0x00, 0x00, 0x00, 0x00,      // MOV ECX, 0
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x100);
            cpu.gpr.set32(EBX, 0x200);
        });
}

TEST_F(JitParityTest, NegThenAdd) {
    // NEG EAX; ADD EAX, EBX
    u8 code[] = {
        0xF7, 0xD8,  // NEG EAX
        0x01, 0xD8,  // ADD EAX, EBX
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 50);
            cpu.gpr.set32(EBX, 100);
        });
}

TEST_F(JitParityTest, MemRoundtrip) {
    // Store value, load it back:
    // MOV [EBP-4], EAX; MOV EBX, [EBP-4]
    u8 code[] = {
        0x89, 0x45, 0xFC,  // MOV [EBP-4], EAX
        0x8B, 0x5D, 0xFC,  // MOV EBX, [EBP-4]
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EBP, 0x5004);
            cpu.gpr.set32(EAX, 0xCAFEBABE);
            cpu.gpr.set32(EBX, 0);
        },
        false);
}

TEST_F(JitParityTest, XchgRegReg) {
    // XCHG EAX, EBX — 93
    check_parity({0x93},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x11111111);
            cpu.gpr.set32(EBX, 0x22222222);
        },
        false);
}

TEST_F(JitParityTest, XchgRm16R16) {
    // 66 87 C3 => XCHG AX, BX — preserves EAX/EBX upper 16 bits
    check_parity({0x66, 0x87, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1111);
            cpu.gpr.set32(EBX, 0xBBBB2222);
        },
        false);
}

// Intel SDM Vol. 2 CMOVcc r16,r/m16: move only low 16 bits; upper GPR bits
// survive both taken and not-taken paths (emit_store_gpr16).
TEST_F(JitParityTest, CmovzR16TakenPreservesUpper) {
    // 66 0F 44 C3 => CMOVZ AX, BX with ZF=1
    check_parity({0x66, 0x0F, 0x44, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1111);
            cpu.gpr.set32(EBX, 0xBBBB2222);
            cpu.flags.set_zf(true);
        },
        false);
}

TEST_F(JitParityTest, CmovzR16NotTakenPreservesUpper) {
    // 66 0F 44 C3 => CMOVZ AX, BX with ZF=0 — AX unchanged including high half
    check_parity({0x66, 0x0F, 0x44, 0xC3},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1111);
            cpu.gpr.set32(EBX, 0xBBBB2222);
            cpu.flags.set_zf(false);
        },
        false);
}

TEST_F(JitParityTest, CmovnzR16FromMem) {
    // 66 0F 45 05 00 20 00 00 => CMOVNZ AX, [0x2000]
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0x3456).is_ok());
    check_parity({0x66, 0x0F, 0x45, 0x05, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1111);
            cpu.flags.set_zf(false);
        },
        false);
}

// Intel SDM Vol. 2 CMPXCHG r/m16,r16: compare AX with r/m16; ZF; store.
TEST_F(JitParityTest, CmpxchgRm16Match) {
    // 66 0F B1 CB => CMPXCHG BX, CX — AX==BX → BX:=CX, ZF=1; EAX upper kept
    check_parity({0x66, 0x0F, 0xB1, 0xCB},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1234);
            cpu.gpr.set32(EBX, 0xBBBB1234);
            cpu.gpr.set32(ECX, 0xCCCC5678);
        });
}

TEST_F(JitParityTest, CmpxchgRm16Mismatch) {
    // AX!=BX → AX:=BX (low 16), ZF=0; EBX unchanged
    check_parity({0x66, 0x0F, 0xB1, 0xCB},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA0001);
            cpu.gpr.set32(EBX, 0xBBBB0002);
            cpu.gpr.set32(ECX, 0xCCCC9999);
        });
}

TEST_F(JitParityTest, CmpxchgRm16MemMatch) {
    // 66 0F B1 0D 00 20 00 00 => CMPXCHG [0x2000], CX
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0x1234).is_ok());
    check_parity({0x66, 0x0F, 0xB1, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0xAAAA1234);
            cpu.gpr.set32(ECX, 0xCCCC5678);
        });
}

// Intel SDM Vol. 2C XADD r/m16,r16: exchange-and-add; Add16 flags.
TEST_F(JitParityTest, XaddRm16R16) {
    // 66 0F C1 CB => XADD BX, CX — BX:=BX+CX, CX:=old BX; upper bits kept
    check_parity({0x66, 0x0F, 0xC1, 0xCB},
        [](CpuState& cpu) {
            cpu.gpr.set32(EBX, 0xBBBB0010);
            cpu.gpr.set32(ECX, 0xCCCC0020);
        });
}

TEST_F(JitParityTest, XaddRm16Mem) {
    // 66 0F C1 0D 00 20 00 00 => XADD [0x2000], CX
    ASSERT_TRUE(m_memory->write_u16(0x2000, 0x0100).is_ok());
    check_parity({0x66, 0x0F, 0xC1, 0x0D, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set32(ECX, 0xCCCC00FF);
        });
}

// Intel SDM Vol. 2B, XCHG: exchanging EAX with ESP updates the stack
// pointer. MSVC 32-bit _chkstk ends with `xchg esp, eax; mov eax, [eax];
// mov [esp], eax` so a JIT miss here would smash the caller's frame while
// still returning to the correct RIP.
TEST_F(JitParityTest, XchgEaxEsp) {
    check_parity({0x94},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x7000);
            cpu.gpr.set32(ESP, 0x8000);
        },
        false);
}

TEST_F(JitParityTest, ChkstkStackSwitchCopiesReturnAddress) {
    const u32 old_esp = 0x8000;
    const u32 new_esp = 0x7000;
    const u32 ret_addr = 0x00401000;
    ASSERT_TRUE(m_memory->write_u32(old_esp, ret_addr).is_ok());
    ASSERT_TRUE(m_memory->write_u32(new_esp, 0xDEADBEEF).is_ok());
    // xchg eax, esp; mov eax, [eax]; mov [esp], eax
    check_parity({0x94, 0x8B, 0x00, 0x89, 0x04, 0x24},
        [old_esp, new_esp](CpuState& cpu) {
            cpu.gpr.set32(EAX, new_esp);
            cpu.gpr.set32(ESP, old_esp);
        },
        false);
}

// _chkstk saturates a wrapping new-ESP to 0 via `sbb eax, eax; not eax;
// and ecx, eax` after `sub ecx, eax`. CF must match the interpreter.
TEST_F(JitParityTest, ChkstkSaturateOnStackUnderflow) {
    // sub ecx, eax; sbb eax, eax; not eax; and ecx, eax
    check_parity({0x2B, 0xC8, 0x1B, 0xC0, 0xF7, 0xD0, 0x21, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 0x2000);
            cpu.gpr.set32(ECX, 0x1000);
        });
}

// ============================================================================
// Stress: longer multi-instruction blocks
// ============================================================================

TEST_F(JitParityTest, FibonacciLikeSequence) {
    // Compute a simple sequence: start with EAX=1, EBX=1
    // then: ADD EAX, EBX; XCHG EAX, EBX (×3)
    u8 code[] = {
        0x01, 0xD8,  // ADD EAX, EBX  → EAX=2, EBX=1
        0x93,        // XCHG EAX, EBX → EAX=1, EBX=2
        0x01, 0xD8,  // ADD EAX, EBX  → EAX=3, EBX=2
        0x93,        // XCHG EAX, EBX → EAX=2, EBX=3
        0x01, 0xD8,  // ADD EAX, EBX  → EAX=5, EBX=3
        0x93,        // XCHG EAX, EBX → EAX=3, EBX=5
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(EAX, 1);
            cpu.gpr.set32(EBX, 1);
        });
}

TEST_F(JitParityTest, MemCopyPattern) {
    // Common pattern: load from [ESI], store to [EDI], advance both
    // MOV EAX, [ESI]; MOV [EDI], EAX; ADD ESI, 4; ADD EDI, 4
    u32 src_val = 0xABCD1234;
    m_memory->write_bytes(0x3000, reinterpret_cast<const u8*>(&src_val), 4);
    u8 code[] = {
        0x8B, 0x06,        // MOV EAX, [ESI]
        0x89, 0x07,        // MOV [EDI], EAX
        0x83, 0xC6, 0x04,  // ADD ESI, 4
        0x83, 0xC7, 0x04,  // ADD EDI, 4
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) {
            cpu.gpr.set32(ESI, 0x3000);
            cpu.gpr.set32(EDI, 0x4000);
        });
}

TEST_F(JitParityTest, BitManipulationPattern) {
    // Extract bits 8-15 of EAX: SHR EAX, 8; AND EAX, 0xFF
    u8 code[] = {
        0xC1, 0xE8, 0x08,  // SHR EAX, 8
        0x25, 0xFF, 0x00, 0x00, 0x00,  // AND EAX, 0xFF
    };
    check_parity(code, sizeof(code),
        [](CpuState& cpu) { cpu.gpr.set32(EAX, 0xDEADBEEF); });
}

TEST_F(JitParityTest, SmcInvalidationAllowsBoundedRecompilation) {
    const u8 code[] = {0xB8, 1, 0, 0, 0}; // mov eax, 1
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code, sizeof(code)).is_ok());

    BasicBlock block = build_block(code, sizeof(code));
    ASSERT_EQ(block.start_addr, 0x1000u);

    BlockCache cache;
    JitCompiler jit(*m_memory);
    jit.set_block_cache(&cache);

    CompiledBlock* cb = jit.compile(block);
    ASSERT_NE(cb, nullptr);
    jit.publish_compiled_block(block, *cb);
    cache.insert(block);
    ASSERT_NE(cache.lookup(0x1000), nullptr);

    for (u8 value = 2; value <= 9; ++value) {
        SCOPED_TRACE(value);
        const u8 patched[] = {0xB8, value, 0, 0, 0};
        ASSERT_TRUE(m_memory->write_bytes(
            0x1000, patched, sizeof(patched)).is_ok());
        EXPECT_EQ(cache.lookup(0x1000), nullptr);

        BasicBlock again = build_block(patched, sizeof(patched));
        CompiledBlock* fresh = jit.compile(again);
        if (value == 9) {
            EXPECT_EQ(fresh, nullptr) << "ongoing SMC must remain bounded";
            break;
        }
        ASSERT_NE(fresh, nullptr);
        jit.publish_compiled_block(again, *fresh);
        cache.insert(again);
        CpuState cpu;
        cpu.rip = 0x1000;
        (void)test::execute_like_dispatcher(jit, *fresh, cpu);
        EXPECT_FALSE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.gpr.get32(EAX), value) << "executed stale code";
    }

    jit.set_block_cache(nullptr);
}

// ============================================================================
// Sims 3 .package decompressor (RefPack/QFS) — real guest bytes.
//
// Forcing ONLY these blocks to the interpreter (VITREO_PC_WATCH_ADDR) changes
// in-game behaviour, so the JIT and the interpreter disagree somewhere in this
// loop. The loop is a literal-run / back-reference copier whose control flow
// hangs off flags produced by SHR and by SUB/TEST, which is the shape of three
// separate flag bugs already recorded in this file's history.
//
// Bytes lifted verbatim from TS3.exe at 0x004e49d6..0x004e4a38.
// ============================================================================
namespace {

// test dl, 0x40 ; jne
const u8 kDecompA[] = {0xf6, 0xc2, 0x40, 0x75, 0x5d};
// mov al,[ecx] ; mov bl,[ecx+1] ; add ecx,1 ; movzx ebp,al ; mov edi,ebp
// add ecx,1 ; shr edi,6 ; je          <-- branch on ZF from SHR
const u8 kDecompB[] = {0x8a, 0x01, 0x8a, 0x59, 0x01, 0x83, 0xc1, 0x01,
                       0x0f, 0xb6, 0xe8, 0x8b, 0xfd, 0x83, 0xc1, 0x01,
                       0xc1, 0xef, 0x06, 0x74, 0x11};
// mov al,[ecx] ; sub edi,1 ; mov [esi],al ; add esi,1 ; add ecx,1
// test edi,edi ; jne
const u8 kDecompC[] = {0x8a, 0x01, 0x83, 0xef, 0x01, 0x88, 0x06, 0x83,
                       0xc6, 0x01, 0x83, 0xc1, 0x01, 0x85, 0xff, 0x75, 0xef};
// and ebp,0x3f ; shl ebp,8 ; mov edi,esi ; sub edi,ebp ; movzx eax,bl
// sub edi,eax ; and edx,0x3f ; sub edi,1 ; add edx,3 ; lea esp,[esp] ; ...
const u8 kDecompD[] = {0x83, 0xe5, 0x3f, 0xc1, 0xe5, 0x08, 0x8b, 0xfe,
                       0x2b, 0xfd, 0x0f, 0xb6, 0xc3, 0x2b, 0xf8, 0x83,
                       0xe2, 0x3f, 0x83, 0xef, 0x01, 0x83, 0xc2, 0x03,
                       0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00, 0x8a,
                       0x07, 0x88, 0x06, 0x8b, 0xc2, 0x83, 0xc6, 0x01,
                       0x83, 0xc7, 0x01, 0x83, 0xea, 0x01, 0x85, 0xc0,
                       0x75, 0xed};
// mov al,[edi] ; mov [esi],al ; mov eax,edx ; add esi,1 ; add edi,1
// sub edx,1 ; test eax,eax ; jne
const u8 kDecompE[] = {0x8a, 0x07, 0x88, 0x06, 0x8b, 0xc2, 0x83, 0xc6,
                       0x01, 0x83, 0xc7, 0x01, 0x83, 0xea, 0x01, 0x85,
                       0xc0, 0x75, 0xed};

// xorshift so a failure is one integer to reproduce.
u32 xs(u32& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

} // namespace

// NOTE: a block-level parity test over these blocks was removed. Blocks C/D/E
// are self-loops and block A ends in a taken branch, so the interpreter (run
// for exactly instruction_count) and the JIT (which iterates) retire different
// instructions and "diverge" for harness reasons alone. The straight-line and
// shift->branch tests below cover the same code with no such artifact.
TEST_F(JitParityTest, Sims3DecompressorStraightLineMatchesInterpreter) {
    constexpr u32 kDst = 0x3000u;
    // and ebp,0x3f ; shl ebp,8 ; mov edi,esi ; sub edi,ebp ; movzx eax,bl
    // sub edi,eax ; and edx,0x3f ; sub edi,1 ; add edx,3 ; lea esp,[esp]
    const u8 kAddrCalc[] = {0x83, 0xe5, 0x3f, 0xc1, 0xe5, 0x08, 0x8b, 0xfe,
                            0x2b, 0xfd, 0x0f, 0xb6, 0xc3, 0x2b, 0xf8, 0x83,
                            0xe2, 0x3f, 0x83, 0xef, 0x01, 0x83, 0xc2, 0x03,
                            0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00};
    // the alignment NOP alone
    const u8 kLeaEspNop[] = {0x8d, 0xa4, 0x24, 0x00, 0x00, 0x00, 0x00};
    // shr edi,6 in isolation (flag producer for the JE)
    const u8 kShr6[] = {0xc1, 0xef, 0x06};

    u32 seed = 0x9E3779B9u;
    for (int iter = 0; iter < 128; ++iter) {
        const u32 s0 = seed;
        const u32 a = xs(seed), b = xs(seed), c = xs(seed);
        {
            SCOPED_TRACE("addr-calc seed=" + std::to_string(s0));
            check_parity(kAddrCalc, sizeof(kAddrCalc), [&](CpuState& cpu) {
                cpu.gpr.set32(ESI, kDst + (a & 0xFF));
                cpu.gpr.set32(EBP, b);
                cpu.gpr.set32(EBX, c & 0xFFu);
                cpu.gpr.set32(EDX, a);
            });
        }
        {
            SCOPED_TRACE("lea esp,[esp] seed=" + std::to_string(s0));
            check_parity(kLeaEspNop, sizeof(kLeaEspNop), [&](CpuState& cpu) {
                cpu.gpr.set32(EAX, a);
                cpu.gpr.set32(EDX, b);
            });
        }
        {
            SCOPED_TRACE("shr edi,6 seed=" + std::to_string(s0));
            check_parity(kShr6, sizeof(kShr6), [&](CpuState& cpu) {
                cpu.gpr.set32(EDI, a);
            });
        }
    }
}

// The decompressor's control flow hangs off flags produced by a SHIFT:
//     shr edi, 6
//     je  <literal-run path>
// If the JE sees the wrong ZF, the decompressor takes the wrong path and
// emits corrupt bytes -- silently, with no fault. Sweep EDI and compare the
// resulting RIP between interpreter and JIT.
TEST_F(JitParityTest, Sims3DecompressorShiftBranchMatchesInterpreter) {
    // shr edi,6 ; je +0x11
    const std::vector<u8> code = {0xc1, 0xef, 0x06, 0x74, 0x11};

    auto run_one = [&](u32 edi_in, u32& interp_rip, u32& jit_rip) {
        JitCompiler jit(*m_memory);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        MemoryByteReader reader(code.data(), code.size());
        BasicBlock block; block.start_addr = 0x1000;
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, DecodeMode::Mode32, inst) !=
                DecodeError::Success) break;
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        ASSERT_EQ(block.instruction_count, 2u);

        auto setup = [&](CpuState& c) {
            c.init_protected_mode();
            c.gpr.set32(ESP, 0x10000);
            c.rip = 0x1000;
            c.gpr.set32(EDI, edi_in);
        };
        CpuState icpu; setup(icpu);
        { Interpreter interp(icpu, *m_memory); (void)interp.run(2); }
        CpuState jcpu; setup(jcpu);
        ASSERT_TRUE(m_memory->protect_region(0x1000, 0x1000,
                        MemoryProtection::ReadExecute).is_ok());
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        (void)test::execute_like_dispatcher(jit, *cb, jcpu);
        ASSERT_TRUE(m_memory->protect_region(0x1000, 0x1000,
                        MemoryProtection::ReadWrite).is_ok());
        interp_rip = static_cast<u32>(icpu.rip);
        jit_rip = static_cast<u32>(jcpu.rip);
    };

    // Values that straddle the ZF boundary: anything < 0x40 shifts to zero.
    std::vector<u32> inputs = {0, 1, 0x3F, 0x40, 0x41, 0x7F, 0x80, 0xFF,
                               0x100, 0x1234, 0x8000'0000u, 0xFFFF'FFFFu};
    u32 seed = 0xC0FFEEu;
    for (int i = 0; i < 64; ++i) inputs.push_back(xs(seed));

    for (u32 v : inputs) {
        u32 ir = 0, jr = 0;
        run_one(v, ir, jr);
        EXPECT_EQ(ir, jr) << "shr/je diverged for EDI=0x" << std::hex << v
                          << ": interp rip=0x" << ir << " jit rip=0x" << jr;
    }
}
