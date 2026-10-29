// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Opcode decode handler interface

#pragma once

#include "../decoder.hpp"
#include "../modrm.hpp"

namespace vitreo::cpu::decode {

/// Context passed to opcode decode handlers
struct DecodeContext {
    ByteReader& reader;
    DecodeMode mode;
    Prefixes& prefix;
    DecodedInstruction& out;
    u8 opcode_byte;
    bool is_64bit_operand;
    u8 op_size;
    
    DecodeContext(
        ByteReader& r,
        DecodeMode m,
        Prefixes& p,
        DecodedInstruction& o,
        u8 byte
    ) noexcept
        : reader(r), mode(m), prefix(p), out(o), opcode_byte(byte),
          is_64bit_operand(p.rex_w()),
          op_size(default_operand_size(m, p)) {}
};

/// Decode MOV instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_mov(DecodeContext& ctx) noexcept;

/// Decode ADD instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_add(DecodeContext& ctx) noexcept;

/// Decode SUB instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_sub(DecodeContext& ctx) noexcept;

/// Decode XOR instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_xor(DecodeContext& ctx) noexcept;

/// Decode CMP instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_cmp(DecodeContext& ctx) noexcept;

/// Decode TEST instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_test(DecodeContext& ctx) noexcept;

/// Decode PUSH instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_push(DecodeContext& ctx) noexcept;

/// Decode POP instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_pop(DecodeContext& ctx) noexcept;

/// Decode POP r/m instruction (0x8F /0)
[[nodiscard]] DecodeError decode_pop_rm(DecodeContext& ctx) noexcept;

/// Decode JMP instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_jmp(DecodeContext& ctx) noexcept;

/// Decode Jcc instructions (one-byte rel8)
[[nodiscard]] DecodeError decode_jcc_rel8(DecodeContext& ctx) noexcept;

/// Decode LOOP/JCXZ-family instructions (E0-E3)
[[nodiscard]] DecodeError decode_loop_jcxz(DecodeContext& ctx) noexcept;

/// Decode CALL/RET instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_call_ret(DecodeContext& ctx) noexcept;

/// Decode LEA instruction
[[nodiscard]] DecodeError decode_lea(DecodeContext& ctx) noexcept;

/// Decode INT instructions
[[nodiscard]] DecodeError decode_int(DecodeContext& ctx) noexcept;

/// Decode NOP/XCHG instructions
[[nodiscard]] DecodeError decode_nop_xchg(DecodeContext& ctx) noexcept;

/// Decode two-byte opcodes (0F prefix)
[[nodiscard]] DecodeError decode_two_byte(DecodeContext& ctx) noexcept;

/// Decode Group 1 (0x80-0x83) - ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m, imm
[[nodiscard]] DecodeError decode_group1(DecodeContext& ctx) noexcept;

/// Decode Group 3 (0xF6-0xF7) - TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m
[[nodiscard]] DecodeError decode_group3(DecodeContext& ctx) noexcept;

/// Decode Group 5 (0xFF) - INC/DEC/CALL/JMP/PUSH r/m
[[nodiscard]] DecodeError decode_group5(DecodeContext& ctx) noexcept;

/// Decode LEAVE instruction (C9)
[[nodiscard]] DecodeError decode_leave(DecodeContext& ctx) noexcept;

/// Decode ENTER instruction (C8)
[[nodiscard]] DecodeError decode_enter(DecodeContext& ctx) noexcept;

/// Decode AND instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_and(DecodeContext& ctx) noexcept;

/// Decode OR instructions (one-byte opcodes)
[[nodiscard]] DecodeError decode_or(DecodeContext& ctx) noexcept;

/// Decode shift/rotate instructions (Group 2: C0, C1, D0, D1, D2, D3)
[[nodiscard]] DecodeError decode_shifts(DecodeContext& ctx) noexcept;

/// Decode CMPXCHG instructions (0F B0, 0F B1)
[[nodiscard]] DecodeError decode_cmpxchg(DecodeContext& ctx) noexcept;

/// Decode XADD instructions (0F C0, 0F C1)
[[nodiscard]] DecodeError decode_xadd(DecodeContext& ctx) noexcept;

/// Decode XCHG r/m, r instructions (86, 87)
[[nodiscard]] DecodeError decode_xchg_rm(DecodeContext& ctx) noexcept;

/// Decode MOVSXD instruction (63)
[[nodiscard]] DecodeError decode_movsxd(DecodeContext& ctx) noexcept;

/// Decode CMOVcc instructions (0F 40-4F)
[[nodiscard]] DecodeError decode_cmov(DecodeContext& ctx) noexcept;

/// Decode SETcc instructions (0F 90-9F)
[[nodiscard]] DecodeError decode_setcc(DecodeContext& ctx) noexcept;

/// Decode BSF/BSR instructions (0F BC, 0F BD)
[[nodiscard]] DecodeError decode_bsf_bsr(DecodeContext& ctx) noexcept;

/// Decode POPCNT instruction (F3 0F B8)
[[nodiscard]] DecodeError decode_popcnt(DecodeContext& ctx) noexcept;

/// Decode BT/BTS/BTR/BTC register instructions (0F A3, 0F AB, 0F B3, 0F BB)
[[nodiscard]] DecodeError decode_bt(DecodeContext& ctx) noexcept;

/// Decode BT/BTS/BTR/BTC imm8 instructions (0F BA Group 8)
[[nodiscard]] DecodeError decode_bt_imm(DecodeContext& ctx) noexcept;

/// Decode BSWAP instructions (0F C8+rd)
[[nodiscard]] DecodeError decode_bswap(DecodeContext& ctx) noexcept;

/// Decode string operations (REP/REPNE + MOVS/STOS/LODS/etc.)
[[nodiscard]] DecodeError decode_string_ops(DecodeContext& ctx) noexcept;

/// Decode ADC instructions (one-byte opcodes 10-15)
[[nodiscard]] DecodeError decode_adc(DecodeContext& ctx) noexcept;

/// Decode SBB instructions (one-byte opcodes 18-1D)
[[nodiscard]] DecodeError decode_sbb(DecodeContext& ctx) noexcept;

/// Decode Group 4 (0xFE) - INC/DEC r/m8
[[nodiscard]] DecodeError decode_group4(DecodeContext& ctx) noexcept;

/// Decode IMUL two-byte (0F AF) - IMUL r, r/m
[[nodiscard]] DecodeError decode_imul_two_byte(DecodeContext& ctx) noexcept;

/// Decode IMUL three-operand (69, 6B) - IMUL r, r/m, imm
[[nodiscard]] DecodeError decode_imul_three_op(DecodeContext& ctx) noexcept;

/// Decode sign-extend (98, 99) - CBW/CWDE/CDQE, CWD/CDQ/CQO
[[nodiscard]] DecodeError decode_sign_extend(DecodeContext& ctx) noexcept;

/// Decode x87 FPU instructions (D8-DF escape bytes)
[[nodiscard]] DecodeError decode_fpu(DecodeContext& ctx) noexcept;

/// Decode SSE arithmetic instructions (0F 58/5C/59/5E/51/5F/5D/2F/2E/53/52)
[[nodiscard]] DecodeError decode_sse_arith(DecodeContext& ctx) noexcept;

/// Decode SSE data transfer instructions (0F 10/11/12/13/16/17/28/29/50/54-57/6F/7F/AE)
[[nodiscard]] DecodeError decode_sse_data(DecodeContext& ctx) noexcept;

/// Decode SSE conversion instructions (0F 2A/2C/2D/5A/5B/E6)
[[nodiscard]] DecodeError decode_sse_conv(DecodeContext& ctx) noexcept;

/// Decode SSE comparison instructions (CPU-036)
[[nodiscard]] DecodeError decode_sse_cmp(DecodeContext& ctx) noexcept;

/// Decode SSE2 integer arithmetic instructions (CPU-037)
[[nodiscard]] DecodeError decode_sse_int(DecodeContext& ctx) noexcept;

/// Decode SSE shuffle & unpack instructions (0F C6/70/14/15/60-62/68-6A/6C/6D)
[[nodiscard]] DecodeError decode_sse_shuffle(DecodeContext& ctx) noexcept;

/// Decode three-byte opcodes (0F 38 XX): PSHUFB, PMULLD, AES-NI, SHA
[[nodiscard]] DecodeError decode_three_byte_38(DecodeContext& ctx) noexcept;

/// Decode three-byte opcodes (0F 3A XX): PALIGNR, PCLMULQDQ, AESKEYGENASSIST, SHA1RNDS4
[[nodiscard]] DecodeError decode_three_byte_3a(DecodeContext& ctx) noexcept;

/// Decode VEX.0F38 map (BMI, FMA, AVX2 unique, F16C VCVTPH2PS)
/// EVEX-prefixed instruction (62 P0 P1 P2 ...). Consumes the whole encoding
/// and yields Opcode::EVEX_UNSUPPORTED, which raises #UD: Vitreo advertises no
/// EVEX-encoded ISA. Takes the reader directly rather than a DecodeContext
/// because the caller is still in the prefix loop and has no opcode byte yet.
[[nodiscard]] DecodeError decode_evex(ByteReader& reader, DecodeMode mode,
                                      Prefixes& prefix, DecodedInstruction& out,
                                      u8 p0, usize start_pos) noexcept;

[[nodiscard]] DecodeError decode_vex_38(DecodeContext& ctx) noexcept;

/// Decode VEX.0F3A map (RORX, VEXTRACT/INSERT/PERM2x128, F16C VCVTPS2PH)
[[nodiscard]] DecodeError decode_vex_3a(DecodeContext& ctx) noexcept;

} // namespace vitreo::cpu::decode
