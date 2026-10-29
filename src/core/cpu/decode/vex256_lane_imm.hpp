#pragma once

/// @file vex256_lane_imm.hpp
/// @brief Upper-half immediate for a VEX.256 legacy-form op run as two
///        128-bit halves (interpreter avx_sse_bridge and the JIT lane split).

#include "core/cpu/decode/opcodes.hpp"

#include <vitreo/common.hpp>

namespace vitreo::cpu {

/// Most imm8 operands are a per-lane pattern that the 256-bit form repeats in
/// each 128-bit lane (SHUFPS, PSHUFD/HW/LW, DPPS, ROUNDPS/PD, CMPPS/PD,
/// PBLENDW, PALIGNR), so the same byte is right for both halves. These four
/// address elements across the whole register (Intel SDM Vol. 2), and the
/// upper half must take the upper bits:
///   VBLENDPS ymm: element i selects on imm8[i], i = 0..7   -> imm8[7:4]
///   VBLENDPD ymm: element i selects on imm8[i], i = 0..3   -> imm8[3:2]
///   VSHUFPD  ymm: element i selects on imm8[i], i = 0..3   -> imm8[3:2]
///   VMPSADBW ymm: block offsets imm8[2:0] low lane, imm8[5:3] high lane
/// Measured on silicon: tools/oracle_probe/avx_vlmax_upper_probe.expected and
/// avx2_lane_imm_probe.expected. Both engines repeated the low bits, which
/// made an AVX animation kernel emit NaN poses.
[[nodiscard]] constexpr u8 vex256_high_lane_imm(Opcode op, u8 imm) noexcept {
    switch (op) {
        case Opcode::BLENDPS_XMM_XMMM128_IMM8: return static_cast<u8>(imm >> 4);
        case Opcode::BLENDPD_XMM_XMMM128_IMM8: return static_cast<u8>(imm >> 2);
        case Opcode::SHUFPD_XMM_XMMM128_IMM8:  return static_cast<u8>(imm >> 2);
        case Opcode::MPSADBW_XMM_XMMM128_IMM8: return static_cast<u8>(imm >> 3);
        default: return imm;
    }
}

}  // namespace vitreo::cpu
