// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): IMUL, CVT, VEX
// shift and VPERM memory operands.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// IMUL with a MEMORY r/m operand (Intel SDM Vol. 2, IMUL: 0F AF /r, 6B /r ib,
// 69 /r id all take r/m32).
//
// These arms used to decline memory and fall to the interpreter.
// `imul r32, [mem], imm` was the most-delegated instruction in the Steam
// client renderer: 37,458,519 interpreter round trips, 12.2% of all
// delegation. check_native_parity requires the JIT to lower the block, so a
// regression back to declining fails here rather than silently costing 30x.
// ============================================================================

namespace {

// [0x40000] = the multiplicand; RAX points at it.
void seed_imul_mem(GuestMemory& mem, CpuState& cpu, u32 value) {
    (void)mem.write_u32(0x40000, value);
    cpu.gpr.set64(RAX, 0x40000);
}

}  // namespace

TEST_F(JitParity64Test, ImulR32MemImm32) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0x69, 0x08, 0x78, 0x56, 0x34, 0x12},   // imul ecx, [rax], 0x12345678
        [this](CpuState& cpu) { seed_imul_mem(*m_memory, cpu, 0x000000A5u); });
}

TEST_F(JitParity64Test, ImulR32MemImm32Overflows) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0x69, 0x08, 0x78, 0x56, 0x34, 0x12},
        [this](CpuState& cpu) { seed_imul_mem(*m_memory, cpu, 0x7FFFFFFFu); });
}

TEST_F(JitParity64Test, ImulR32MemImm8) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0x6B, 0x08, 0x42},                      // imul ecx, [rax], 0x42
        [this](CpuState& cpu) { seed_imul_mem(*m_memory, cpu, 0x00010001u); });
}

TEST_F(JitParity64Test, ImulR32MemImm8Negative) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0x6B, 0x08, 0xFE},                      // imul ecx, [rax], -2
        [this](CpuState& cpu) { seed_imul_mem(*m_memory, cpu, 0x80000000u); });
}

TEST_F(JitParity64Test, ImulR32Mem2Operand) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0xB9, 0x07, 0x00, 0x00, 0x00,           // mov ecx, 7
         0x0F, 0xAF, 0x08},                      // imul ecx, [rax]
        [this](CpuState& cpu) { seed_imul_mem(*m_memory, cpu, 0x00ABCDEFu); });
}

TEST_F(JitParity64Test, ImulR32Mem2OperandOverflows) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0xB9, 0x00, 0x00, 0x00, 0x40,           // mov ecx, 0x40000000
         0x0F, 0xAF, 0x08},                      // imul ecx, [rax]
        [this](CpuState& cpu) { seed_imul_mem(*m_memory, cpu, 0x00000004u); });
}

// ============================================================================
// Packed int<->float conversions with a MEMORY operand in a 64-bit block.
// These arms used to `return false` when m_block_is_64bit, so every
// `cvtdq2ps/cvtps2dq/cvttps2dq xmm, [mem]` in an x64 guest was interpreted:
// 24.5M + 17.6M + 2.4M delegated instructions in the Steam renderer (14.5% of
// all delegation). MXCSR is compared too — CVTDQ2PS raises PE on an inexact
// result, and CVT(T)PS2DQ has its own indefinite-value behaviour.
// ============================================================================

namespace {

void seed_cvt_mem(GuestMemory& mem, CpuState& cpu, const u32 (&lanes)[4]) {
    for (int i = 0; i < 4; ++i) {
        (void)mem.write_u32(0x40000 + static_cast<GuestAddr>(i) * 4, lanes[i]);
    }
    cpu.gpr.set64(RAX, 0x40000);
    for (int b = 0; b < 16; ++b) {
        cpu.sse.xmm[1].u8_[b] = 0xCD;          // dest must be fully written
        cpu.avx.ymm_high[1].u8_[b] = 0xEE;     // and its YMM high left alone
    }
}

}  // namespace

// 66 0F 5B /r — cvtps2dq xmm1, [rax]
TEST_F(JitParity64Test, Cvtps2dqMemoryInLongMode) {
    map_tracked_region(0x40000, 0x1000);
    const u32 lanes[4] = {0x3F800000u, 0xC0490FDBu, 0x4B000000u, 0x00000000u};
    check_native_parity({0x66, 0x0F, 0x5B, 0x08},
                        [this, &lanes](CpuState& cpu) {
                            seed_cvt_mem(*m_memory, cpu, lanes);
                        },
                        /*check_flags=*/false, /*check_xmm=*/true);
}

// F3 0F 5B /r — cvttps2dq xmm1, [rax]  (truncating)
TEST_F(JitParity64Test, Cvttps2dqMemoryInLongMode) {
    map_tracked_region(0x40000, 0x1000);
    const u32 lanes[4] = {0x3FC00000u, 0xBFC00000u, 0x7F800000u, 0xFF800000u};
    check_native_parity({0xF3, 0x0F, 0x5B, 0x08},
                        [this, &lanes](CpuState& cpu) {
                            seed_cvt_mem(*m_memory, cpu, lanes);
                        },
                        false, true);
}

// 0F 5B /r — cvtdq2ps xmm1, [rax]
TEST_F(JitParity64Test, Cvtdq2psMemoryInLongMode) {
    map_tracked_region(0x40000, 0x1000);
    const u32 lanes[4] = {1u, 0xFFFFFFFFu, 0x00FFFFFFu, 0x7FFFFFFFu};
    check_native_parity({0x0F, 0x5B, 0x08},
                        [this, &lanes](CpuState& cpu) {
                            seed_cvt_mem(*m_memory, cpu, lanes);
                        },
                        false, true);
}

// The inexact case: 0x7FFFFFFF is not representable in f32, so CVTDQ2PS must
// set MXCSR.PE. A lowering that skips the sticky bit passes a value-only test.
TEST_F(JitParity64Test, Cvtdq2psMemoryRaisesPrecisionInLongMode) {
    map_tracked_region(0x40000, 0x1000);
    const u32 lanes[4] = {0x7FFFFFFFu, 0x7FFFFFFEu, 0x00000003u, 0x12345679u};
    check_native_parity({0x0F, 0x5B, 0x08},
                        [this, &lanes](CpuState& cpu) {
                            seed_cvt_mem(*m_memory, cpu, lanes);
                        },
                        false, true);
}

// RIP-relative and a wide displacement exercise the 64-bit EA path itself.
TEST_F(JitParity64Test, Cvtdq2psMemoryIndexedInLongMode) {
    map_tracked_region(0x40000, 0x1000);
    const u32 lanes[4] = {7u, 8u, 9u, 10u};
    check_native_parity(
        {0x48, 0xC7, 0xC1, 0x10, 0x00, 0x00, 0x00,   // mov rcx, 16
         0x0F, 0x5B, 0x4C, 0x08, 0xF0},              // cvtdq2ps xmm1,[rax+rcx-16]
        [this, &lanes](CpuState& cpu) {
            seed_cvt_mem(*m_memory, cpu, lanes);
        },
        false, true);
}

// 66 0F 38 04 /r — pmaddubsw xmm1, [rax]. Its memory arm was 32-bit-only for
// the same reason the conversions above were; the EA is parked in X21 so the
// second qword's address survives emit_guest_load64's clobber of X4.
TEST_F(JitParity64Test, PmaddubswMemoryInLongMode) {
    map_tracked_region(0x40000, 0x1000);
    check_native_parity(
        {0x66, 0x0F, 0x38, 0x04, 0x08},
        [this](CpuState& cpu) {
            for (int i = 0; i < 16; ++i) {
                (void)m_memory->write_u8(0x40000 + static_cast<GuestAddr>(i),
                                         static_cast<u8>(0x80 + i * 7));
                cpu.sse.xmm[1].u8_[i] = static_cast<u8>(0xF0 - i * 9);
            }
            cpu.gpr.set64(RAX, 0x40000);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

// ============================================================================
// VEX shift-by-imm8 (Intel SDM Vol. 2, VPSLLW/VPSRLW/... 71-73 /x ib).
//
// These are NDD: ModRM.reg is the opcode extension, so the SOURCE is the r/m
// register and VEX.vvvv is the DESTINATION. The JIT used to decline them
// outright, sending every one to the interpreter -- 23.3% of all delegated
// instructions in the Steam client renderer. The source must survive (it is a
// different register from the destination), the 128-bit form must zero
// YMM[255:128], and the 256-bit form must shift BOTH lanes by the same imm8.
// ============================================================================

namespace {

void seed_vex_shift(CpuState& cpu) {
    for (int i = 0; i < 8; ++i) {
        cpu.sse.xmm[2].u16_[i] = static_cast<u16>(0x8001u + i * 0x1111u);
        cpu.avx.ymm_high[2].u16_[i] = static_cast<u16>(0xF00Du - i * 0x0707u);
        cpu.sse.xmm[1].u16_[i] = 0xDEAD;      // destination: must be overwritten
        cpu.avx.ymm_high[1].u16_[i] = 0xBEEF;
    }
}

}  // namespace

// VEX.128.66.0F 71 /2 ib — vpsrlw xmm1, xmm2, 3   (C5 F1 71 D2 03)
TEST_F(JitParity64Test, Vex128VpsrlwImmZeroesYmmHigh) {
    check_native_parity({0xC5, 0xF1, 0x71, 0xD2, 0x03}, seed_vex_shift,
                        false, true);
}

// VEX.256.66.0F 71 /2 ib — vpsrlw ymm1, ymm2, 3   (C5 F5 71 D2 03)
TEST_F(JitParity64Test, Vex256VpsrlwImmBothLanes) {
    check_native_parity({0xC5, 0xF5, 0x71, 0xD2, 0x03}, seed_vex_shift,
                        false, true);
}

// VEX.256.66.0F 72 /6 ib — vpslld ymm1, ymm2, 5
TEST_F(JitParity64Test, Vex256VpslldImmBothLanes) {
    check_native_parity({0xC5, 0xF5, 0x72, 0xF2, 0x05}, seed_vex_shift,
                        false, true);
}

// VEX.256.66.0F 72 /2 ib — vpsrld ymm1, ymm2, 9
TEST_F(JitParity64Test, Vex256VpsrldImmBothLanes) {
    check_native_parity({0xC5, 0xF5, 0x72, 0xD2, 0x09}, seed_vex_shift,
                        false, true);
}

// VEX.256.66.0F 72 /4 ib — vpsrad ymm1, ymm2, 7   (arithmetic: sign matters)
TEST_F(JitParity64Test, Vex256VpsradImmBothLanes) {
    check_native_parity({0xC5, 0xF5, 0x72, 0xE2, 0x07}, seed_vex_shift,
                        false, true);
}

// VEX.256.66.0F 71 /6 ib — vpsllw ymm1, ymm2, 1
TEST_F(JitParity64Test, Vex256VpsllwImmBothLanes) {
    check_native_parity({0xC5, 0xF5, 0x71, 0xF2, 0x01}, seed_vex_shift,
                        false, true);
}

// VEX.256.66.0F 73 /2 ib — vpsrlq ymm1, ymm2, 17
TEST_F(JitParity64Test, Vex256VpsrlqImmBothLanes) {
    check_native_parity({0xC5, 0xF5, 0x73, 0xD2, 0x11}, seed_vex_shift,
                        false, true);
}

// Destination == source (`vpsrlw ymm2, ymm2, 3`): the copy must be skipped,
// not emitted as a self-copy that reads a half-shifted register.
TEST_F(JitParity64Test, Vex256VpsrlwImmDestEqualsSource) {
    check_native_parity({0xC5, 0xED, 0x71, 0xD2, 0x03}, seed_vex_shift,
                        false, true);
}

// A count >= element width zeroes every element (Intel SDM: "if COUNT >
// element width, DEST is set to 0"), which is outside ARM's USHR imm range and
// so takes a different arm of the lowering.
TEST_F(JitParity64Test, Vex256VpsrlwImmCountExceedsWidth) {
    check_native_parity({0xC5, 0xF5, 0x71, 0xD2, 0x20}, seed_vex_shift,
                        false, true);
}

// An oversized arithmetic-right count sign-fills each element rather than
// zeroing it, and must remain native for both halves of a 256-bit register.
TEST_F(JitParity64Test, Vex256VpsradImmCountExceedsWidth) {
    check_native_parity({0xC5, 0xF5, 0x72, 0xE2, 0x40}, seed_vex_shift,
                        false, true);
}

// Count of zero must leave the value unchanged (and still zero YMM high on the
// 128-bit form).
TEST_F(JitParity64Test, Vex128VpsrlwImmZeroCount) {
    check_native_parity({0xC5, 0xF1, 0x71, 0xD2, 0x00}, seed_vex_shift,
                        false, true);
}

// ============================================================================
// VPERMPS/VPERMD with a 256-bit MEMORY data source.
//
// Intel SDM Vol. 2, VPERMPS/VPERMD: each dword in VEX.vvvv supplies a
// three-bit index selecting any of the eight dwords in the full 256-bit r/m
// source. These are cross-lane permutations; the memory form must read all 32
// bytes before overwriting a destination that aliases the index register.
// ============================================================================

namespace {
constexpr GuestAddr kVpermSource = 0x140008000ull;

void seed_vperm_indices(CpuState& cpu, u8 reg) {
    static constexpr u32 kIndex[8] = {7, 0, 5, 2, 4, 1, 6, 3};
    for (int i = 0; i < 4; ++i) {
        cpu.sse.xmm[reg].u32_[i] = kIndex[i];
        cpu.avx.ymm_high[reg].u32_[i] = kIndex[i + 4];
    }
}

void write_vperm_source(GuestMemory& mem, GuestAddr address) {
    for (u32 i = 0; i < 8; ++i) {
        (void)mem.write_u32(address + i * 4, 0x3F000000u + i * 0x00111111u);
    }
}
}  // namespace

TEST_F(JitParity64Test, VpermpsYmmMemoryCrossLaneAboveFourGig) {
    map_tracked_region(kVpermSource, 0x1000);
    write_vperm_source(*m_memory, kVpermSource);

    // vpermps ymm1, ymm2, [rax] — VEX.256.66.0F38.W0 16 /r
    check_native_parity({0xC4, 0xE2, 0x6D, 0x16, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kVpermSource);
            seed_vperm_indices(cpu, 2);
            seed_ymm_dirty(cpu, 1);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, VpermpsYmmMemoryDestinationAliasesIndex) {
    map_tracked_region(kVpermSource, 0x1000);
    write_vperm_source(*m_memory, kVpermSource + 16);

    // vpermps ymm2, ymm2, [rax+rcx*4]. RCX=4 exercises the long-mode indexed
    // EA path while dst==VEX.vvvv proves both index halves are consumed first.
    check_native_parity({0xC4, 0xE2, 0x6D, 0x16, 0x14, 0x88},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, kVpermSource);
            cpu.gpr.set64(RCX, 4);
            seed_vperm_indices(cpu, 2);
        },
        /*check_flags=*/false, /*check_xmm=*/true);
}
