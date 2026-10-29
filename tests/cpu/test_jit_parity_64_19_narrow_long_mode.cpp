// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): 8/16-bit forms
// and string ops with 64-bit pointers.

#include "jit_parity_64_fixture.hpp"

// ---------------------------------------------------------------------------
// 8/16-bit MOV and ALU forms inside a 64-bit block.
//
// These are lowered in compile_mov / compile_arithmetic, which the 32-bit
// native chain owns; until they were routed into compile_64bit they delegated
// (MOVSX_R16_RM8 and XOR_R16_RM16 were 2.7% of executed instructions each on
// an x64 16-bit-arithmetic loop). The rule these tests exist for is long
// mode's destination width: a 16-bit write preserves bits 63:16 and an 8-bit
// write preserves bits 63:8, unlike a 32-bit write which zero-extends. Every
// setup below seeds the untouched half with a pattern that a wrong-width store
// would destroy.
// ---------------------------------------------------------------------------

TEST_F(JitParity64Test, Xor16InLongModePreservesUpperBits) {
    // XOR AX, CX — 66 33 C1
    check_native_parity({0x66, 0x33, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFEEDFACE1111AAAAull);
            cpu.gpr.set64(RCX, 0x123456780000FFFFull);
        });
}

TEST_F(JitParity64Test, Adc16InLongModeReadsCarryAndPreservesUpper) {
    // ADC AX, CX — 66 13 C1, with CF set on entry.
    check_native_parity({0x66, 0x13, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEF0000FFFFull);
            cpu.gpr.set64(RCX, 0xCAFEBABE00000001ull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, Sbb16InLongModeReadsCarry) {
    // SBB AX, CX — 66 1B C1
    check_native_parity({0x66, 0x1B, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1122334400000000ull);
            cpu.gpr.set64(RCX, 0x5566778800000001ull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, Neg16InLongModePreservesUpperBits) {
    // NEG AX — 66 F7 D8
    check_native_parity({0x66, 0xF7, 0xD8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAABBBB00008000ull);
        });
}

TEST_F(JitParity64Test, Inc16InLongModePreservesCarryAndUpperBits) {
    // INC AX — 66 FF C0. INC must leave CF alone.
    check_native_parity({0x66, 0xFF, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x99887766FFFFFFFFull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, Xor8InLongModePreservesUpperBits) {
    // XOR AL, CL — 30 C8
    check_native_parity({0x30, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0123456789ABCDEFull);
            cpu.gpr.set64(RCX, 0xFEDCBA98765432FFull);
        });
}

TEST_F(JitParity64Test, Add8HighByteRegisterWithoutRex) {
    // XOR AH, CL — 30 CC. Without REX, rm=4 is AH, not SPL.
    check_native_parity({0x30, 0xCC},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFF00FFull);
            cpu.gpr.set64(RCX, 0x00000000000000AAull);
        });
}

TEST_F(JitParity64Test, Add8SplWithRexIsNotAh) {
    // ADD SPL, CL — 40 00 CC. REX (even bare 0x40) makes rm=4 mean SPL.
    check_native_parity({0x40, 0x00, 0xCC},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSP, 0x7FFF000000000010ull);
            cpu.gpr.set64(RCX, 0x0000000000000022ull);
        });
}

TEST_F(JitParity64Test, Neg8AndNot8InLongModePreserveUpperBits) {
    // NEG AL ; NOT AL — F6 D8 / F6 D0
    check_native_parity({0xF6, 0xD8, 0xF6, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1234567890ABCD05ull);
        });
}

TEST_F(JitParity64Test, Movsx16FromByteInLongMode) {
    // MOVSX AX, CL — 66 0F BE C1. Sign-extends into AX, preserves bits 63:16.
    check_native_parity({0x66, 0x0F, 0xBE, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAAAAAAAAAAull);
            cpu.gpr.set64(RCX, 0x00000000000000F0ull);  // negative as int8
        });
}

TEST_F(JitParity64Test, Movzx16FromByteInLongMode) {
    // MOVZX AX, CL — 66 0F B6 C1
    check_native_parity({0x66, 0x0F, 0xB6, 0xC1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x5555555555555555ull);
            cpu.gpr.set64(RCX, 0x00000000000000F0ull);
        });
}

TEST_F(JitParity64Test, CwdeZeroExtendsWhereCbwDoesNot) {
    // CWDE (98) writes EAX and therefore zero-extends bits 63:32;
    // CBW (66 98) writes AX only and must preserve them.
    check_native_parity({0x98},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFFFFFFFF0000FFF0ull); },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, CbwInLongModePreservesUpperBits) {
    check_native_parity({0x66, 0x98},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x123456789ABC00F0ull); },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, CwdWritesDxOnly) {
    // CWD — 66 99: sign of AX into DX, upper DX bits preserved.
    check_native_parity({0x66, 0x99},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x0000000000008001ull);
            cpu.gpr.set64(RDX, 0xCCCCCCCCCCCC0000ull);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Xchg8InLongModeSwapsOnlyLowBytes) {
    // XCHG AL, CL — 86 C8
    check_native_parity({0x86, 0xC8},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1111111111111101ull);
            cpu.gpr.set64(RCX, 0x2222222222222202ull);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Adc8ImmediateAccumulatorFormReadsCarry) {
    // ADC AL, 0x10 — 14 10
    check_native_parity({0x14, 0x10},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xABCDEF01234567FFull);
            cpu.flags.set_cf(true);
        });
}

// 8/16-bit shifts and rotates inside a 64-bit block (routed to compile_shifts).
// Long mode's partial-register rule again: these write 8 or 16 bits and must
// leave the rest of the destination alone. RCL/RCR additionally read CF.
TEST_F(JitParity64Test, Rol8InLongModePreservesUpperBits) {
    // ROL AL, 3 — C0 C0 03
    check_native_parity({0xC0, 0xC0, 0x03},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x123456789ABCDE81ull); });
}

TEST_F(JitParity64Test, Ror16InLongModePreservesUpperBits) {
    // ROR AX, 5 — 66 C1 C8 05
    check_native_parity({0x66, 0xC1, 0xC8, 0x05},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0xFEDCBA9800001357ull); });
}

TEST_F(JitParity64Test, Rcl8InLongModeReadsCarry) {
    // RCL AL, 1 — D0 D0, with CF set on entry.
    check_native_parity({0xD0, 0xD0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAAAAAAAA80ull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, Rcr32InLongModeReadsCarryAndZeroExtends) {
    // RCR ECX, 1 — D1 D9. A 32-bit write still zero-extends bits 63:32.
    check_native_parity({0xD1, 0xD9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RCX, 0xFFFFFFFF00000001ull);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, Sar8InLongModeSignPropagates) {
    // SAR AL, 2 — C0 F8 02
    check_native_parity({0xC0, 0xF8, 0x02},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, 0x1111111111111190ull); });
}

TEST_F(JitParity64Test, Shl16ByClInLongMode) {
    // SHL AX, CL — 66 D3 E0
    check_native_parity({0x66, 0xD3, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xDEADBEEF00001234ull);
            cpu.gpr.set64(RCX, 4);
        });
}

TEST_F(JitParity64Test, LegacySlash6ShlBhCountZeroMatchesInterpreter) {
    // C0 F7 00 — legacy Group-2 /6 alias of SHL BH, 0. The zero count
    // preserves BH and every arithmetic flag.
    check_native_parity({0xC0, 0xF7, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x123456789ABC81EFULL);
            cpu.flags.set_cf(true);
            cpu.flags.set_zf(true);
            cpu.flags.set_of(true);
        });
}

TEST_F(JitParity64Test, LegacySlash6ShlBhMatchesInterpreter) {
    // C0 F7 01 — legacy Group-2 /6 alias of SHL BH, 1.
    check_native_parity({0xC0, 0xF7, 0x01},
        [](CpuState& cpu) {
            cpu.gpr.set64(RBX, 0x123456789ABC81EFULL);
            cpu.flags.set_cf(false);
        });
}

TEST_F(JitParity64Test, Shld16InLongModePreservesUpperBits) {
    // SHLD AX, CX, 4 — 66 0F A4 C8 04
    check_native_parity({0x66, 0x0F, 0xA4, 0xC8, 0x04},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x7777777700001234ull);
            cpu.gpr.set64(RCX, 0x888888880000ABCDull);
        });
}

TEST_F(JitParity64Test, ShiftByClZeroCountLeavesFlagsAlone) {
    // SHL AL, CL with CL == 0 must not touch flags at all.
    check_native_parity({0xD2, 0xE0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x00000000000000FFull);
            cpu.gpr.set64(RCX, 0);
            cpu.flags.set_cf(true);
            cpu.flags.set_of(true);
        });
}

// String ops inside a 64-bit block. The bug these guard against is pointer
// width: RSI/RDI/RCX are 64-bit addresses in long mode, and the inline paths
// used to read them with emit_load_gpr32 (truncating the address) and write
// them back with emit_store_gpr32 (zeroing bits 63:32). Each setup puts the
// buffer at a high address so a truncated pointer cannot accidentally work.
TEST_F(JitParity64Test, Stosd64BitPointerIsNotTruncated) {
    map_tracked_region(0x20000, 0x1000);
    // STOSD — AB
    check_native_parity({0xAB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDI, 0x20040);
            cpu.gpr.set64(RAX, 0xCAFEBABEu);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Stosb64BitPointerDecrementsOnDirectionFlag) {
    map_tracked_region(0x20000, 0x1000);
    check_native_parity({0xAA},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDI, 0x20080);
            cpu.gpr.set64(RAX, 0x5Au);
            cpu.flags.set_df(true);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Lodsd64BitPointerIsNotTruncated) {
    map_tracked_region(0x20000, 0x1000);
    // LODSD — AD
    check_native_parity({0xAD},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x20040);
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Lodsb64BitPreservesUpperRax) {
    map_tracked_region(0x20000, 0x1000);
    check_native_parity({0xAC},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x20040);
            cpu.gpr.set64(RAX, 0x1122334455667788ull);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Movsd64BitPointersAreNotTruncated) {
    map_tracked_region(0x20000, 0x2000);
    // MOVSD — A5
    check_native_parity({0xA5},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x20040);
            cpu.gpr.set64(RDI, 0x21040);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, Scasd64BitSetsFlagsFromComparison) {
    map_tracked_region(0x20000, 0x1000);
    // SCASD — AF
    check_native_parity({0xAF},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDI, 0x20040);
            cpu.gpr.set64(RAX, 0);
            cpu.flags.set_df(false);
        });
}

TEST_F(JitParity64Test, Cmpsb64BitSetsFlagsFromComparison) {
    map_tracked_region(0x20000, 0x2000);
    // CMPSB — A6
    check_native_parity({0xA6},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x20040);
            cpu.gpr.set64(RDI, 0x21040);
            cpu.flags.set_df(false);
        });
}

TEST_F(JitParity64Test, RepStosd64BitUsesFullRcxAndRdi) {
    map_tracked_region(0x20000, 0x1000);
    // REP STOSD — F3 AB
    check_native_parity({0xF3, 0xAB},
        [](CpuState& cpu) {
            cpu.gpr.set64(RDI, 0x20000);
            cpu.gpr.set64(RCX, 16);
            cpu.gpr.set64(RAX, 0xA5A5A5A5u);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, RepMovsb64BitUsesFullPointers) {
    map_tracked_region(0x20000, 0x2000);
    // REP MOVSB — F3 A4
    check_native_parity({0xF3, 0xA4},
        [](CpuState& cpu) {
            cpu.gpr.set64(RSI, 0x20000);
            cpu.gpr.set64(RDI, 0x21000);
            cpu.gpr.set64(RCX, 64);
            cpu.flags.set_df(false);
        },
        /*check_flags=*/false);
}

// 8/16-bit MUL/DIV in a 64-bit block. Implicit operands are AL/AX/DX, so the
// partial-register rules apply: MUL r/m8 writes AX (preserving bits 63:16),
// and the 16-bit forms write AX/DX without disturbing the upper halves.
TEST_F(JitParity64Test, Mul8InLongModeWritesAxOnly) {
    // MUL CL — F6 E1
    check_native_parity({0xF6, 0xE1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x1234567890AB00F0ull);
            cpu.gpr.set64(RCX, 0x00000000000000F0ull);
        });
}

TEST_F(JitParity64Test, Div16InLongModeWritesAxAndDxOnly) {
    // DIV CX — 66 F7 F1
    check_native_parity({0x66, 0xF7, 0xF1},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xAAAAAAAAAAAA1234ull);
            cpu.gpr.set64(RDX, 0xBBBBBBBBBBBB0000ull);
            cpu.gpr.set64(RCX, 0x0000000000000007ull);
        });
}

TEST_F(JitParity64Test, Idiv8InLongModeNegativeDividend) {
    // IDIV CL — F6 F9
    check_native_parity({0xF6, 0xF9},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0x777777777777FF9Cull);  // AX = -100
            cpu.gpr.set64(RCX, 0x0000000000000007ull);
        });
}
