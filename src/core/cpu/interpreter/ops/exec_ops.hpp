// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Interpreter execution handler interface

#pragma once

#include "../interpreter.hpp"

namespace vitreo::cpu::ops {

/// Execute MOV instructions
[[nodiscard]] ExecResult exec_mov(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute ADD instructions
[[nodiscard]] ExecResult exec_add(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SUB instructions
[[nodiscard]] ExecResult exec_sub(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute XOR instructions
[[nodiscard]] ExecResult exec_xor(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMP instructions
[[nodiscard]] ExecResult exec_cmp(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute TEST instructions
[[nodiscard]] ExecResult exec_test(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute PUSH instructions
[[nodiscard]] ExecResult exec_push(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute POP instructions
[[nodiscard]] ExecResult exec_pop(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute JMP instructions
[[nodiscard]] ExecResult exec_jmp(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute Jcc instructions
[[nodiscard]] ExecResult exec_jcc(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute LOOP/JCXZ-family instructions
[[nodiscard]] ExecResult exec_loop_jcxz(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CALL instructions
[[nodiscard]] ExecResult exec_call(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute RET instructions
[[nodiscard]] ExecResult exec_ret(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute far RET / RET imm16 (RETF)
[[nodiscard]] ExecResult exec_ret_far(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute far JMP (EA ptr / FF /5 m16:16/32/64), same-privilege user-mode
[[nodiscard]] ExecResult exec_jmp_far(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute far CALL (9A ptr / FF /3 m16:16/32/64), same-privilege user-mode
[[nodiscard]] ExecResult exec_call_far(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute IRET / IRETD / IRETQ (user-mode same-privilege)
[[nodiscard]] ExecResult exec_iret(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute indirect CALL (CALL r/m) instructions
[[nodiscard]] ExecResult exec_call_indirect(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute indirect JMP (JMP r/m) instructions
[[nodiscard]] ExecResult exec_jmp_indirect(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute LEAVE instruction
[[nodiscard]] ExecResult exec_leave(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute ENTER instruction
[[nodiscard]] ExecResult exec_enter(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute LEA instructions
[[nodiscard]] ExecResult exec_lea(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute MOVZX/MOVSX instructions
[[nodiscard]] ExecResult exec_movzx_movsx(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute INT instructions
[[nodiscard]] ExecResult exec_int(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute XCHG instructions
[[nodiscard]] ExecResult exec_xchg(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute misc instructions (HLT, UD2, CPUID, RDTSC, NOP)
[[nodiscard]] ExecResult exec_misc(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Monotonic virtual TSC in nanoseconds (steady_clock). Shared by
/// interpreter RDTSC/RDTSCP and the JIT helper.
[[nodiscard]] u64 virtual_tsc_ticks() noexcept;

/// Execute CMPXCHG instructions
[[nodiscard]] ExecResult exec_cmpxchg(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMPXCHG8B instruction
[[nodiscard]] ExecResult exec_cmpxchg8b(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute XADD instructions
[[nodiscard]] ExecResult exec_xadd(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute XCHG r/m, r instructions
[[nodiscard]] ExecResult exec_xchg_rm(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute flag manipulation instructions (CLC, STC, CMC, CLD, STD, LAHF, SAHF)
[[nodiscard]] ExecResult exec_flag_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute AND instructions
[[nodiscard]] ExecResult exec_and(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute OR instructions
[[nodiscard]] ExecResult exec_or(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute NOT instructions
[[nodiscard]] ExecResult exec_not(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SHL instructions
[[nodiscard]] ExecResult exec_shl(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SHR instructions
[[nodiscard]] ExecResult exec_shr(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SAR instructions
[[nodiscard]] ExecResult exec_sar(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SHLD (double-precision shift left)
[[nodiscard]] ExecResult exec_shld(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SHRD (double-precision shift right)
[[nodiscard]] ExecResult exec_shrd(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute ROL instructions
[[nodiscard]] ExecResult exec_rol(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute ROR instructions
[[nodiscard]] ExecResult exec_ror(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute RCL instructions
[[nodiscard]] ExecResult exec_rcl(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute RCR instructions
[[nodiscard]] ExecResult exec_rcr(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute MOVS string instructions
[[nodiscard]] ExecResult exec_movs(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute STOS string instructions
[[nodiscard]] ExecResult exec_stos(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute LODS string instructions
[[nodiscard]] ExecResult exec_lods(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMPS string instructions
[[nodiscard]] ExecResult exec_cmps(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SCAS string instructions
[[nodiscard]] ExecResult exec_scas(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CLD/STD/CLC/STC/CMC flag instructions
[[nodiscard]] ExecResult exec_cld_std(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute INC instructions
[[nodiscard]] ExecResult exec_inc(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute DEC instructions
[[nodiscard]] ExecResult exec_dec(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute NEG instructions
[[nodiscard]] ExecResult exec_neg(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute ADC instructions
[[nodiscard]] ExecResult exec_adc(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SBB instructions
[[nodiscard]] ExecResult exec_sbb(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute MUL instructions
[[nodiscard]] ExecResult exec_mul(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute IMUL instructions
[[nodiscard]] ExecResult exec_imul(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute DIV instructions
[[nodiscard]] ExecResult exec_div(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute IDIV instructions
[[nodiscard]] ExecResult exec_idiv(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CDQ/CQO instructions
[[nodiscard]] ExecResult exec_cdq_cqo(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute indirect CALL instructions
[[nodiscard]] ExecResult exec_call_indirect(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute indirect JMP instructions
[[nodiscard]] ExecResult exec_jmp_indirect(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute LEAVE instructions
[[nodiscard]] ExecResult exec_leave(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute ENTER instructions
[[nodiscard]] ExecResult exec_enter(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMPXCHG instructions
[[nodiscard]] ExecResult exec_cmpxchg(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMPXCHG8B instruction
[[nodiscard]] ExecResult exec_cmpxchg8b(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMPXCHG16B instruction (REX.W 0F C7 /1)
[[nodiscard]] ExecResult exec_cmpxchg16b(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute XADD instructions
[[nodiscard]] ExecResult exec_xadd(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute XCHG r/m instructions
[[nodiscard]] ExecResult exec_xchg_rm(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute flag manipulation instructions
[[nodiscard]] ExecResult exec_flag_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute CMOVcc instructions
[[nodiscard]] ExecResult exec_cmov(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SETcc instructions
[[nodiscard]] ExecResult exec_setcc(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute BSF instructions
[[nodiscard]] ExecResult exec_bsf(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute BSR instructions
[[nodiscard]] ExecResult exec_bsr(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

[[nodiscard]] ExecResult exec_popcnt(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute BMI1/BMI2/ABM instructions (ANDN, BEXTR, BLSI, ..., TZCNT, LZCNT)
[[nodiscard]] ExecResult exec_bmi_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute BT/BTS/BTR/BTC instructions
[[nodiscard]] ExecResult exec_bt(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute BSWAP instructions
[[nodiscard]] ExecResult exec_bswap(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute x87 FPU instructions
[[nodiscard]] ExecResult exec_fpu(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE arithmetic instructions
[[nodiscard]] ExecResult exec_sse_arith(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
// Packed MIN/MAX owns the entire VEX operand before publishing either lane.
[[nodiscard]] ExecResult exec_sse_minmax(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE data transfer instructions
[[nodiscard]] ExecResult exec_sse_data(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE conversion instructions
[[nodiscard]] ExecResult exec_sse_conv(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE comparison instructions (CPU-036)
[[nodiscard]] ExecResult exec_sse_cmp(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE2 integer arithmetic instructions (CPU-037)
[[nodiscard]] ExecResult exec_sse_int(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE shuffle & unpack instructions (CPU-038)
[[nodiscard]] ExecResult exec_sse_shuffle(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSSE3/SSE4.1/AES-NI/SHA/PCLMULQDQ instructions (three-byte escapes)
[[nodiscard]] ExecResult exec_crypto_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute SSE4.1/SSE4.2 instructions (CPU-041)
[[nodiscard]] ExecResult exec_sse4_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

/// Execute MMX instructions (CPU-045)
[[nodiscard]] ExecResult exec_mmx(ExecContext& ctx, const DecodedInstruction& instr) noexcept;

[[nodiscard]] ExecResult exec_legacy_usermode(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
[[nodiscard]] ExecResult exec_avx_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
[[nodiscard]] ExecResult exec_fma_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
[[nodiscard]] ExecResult exec_xsave_ops(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
[[nodiscard]] ExecResult exec_f16c_adx(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
[[nodiscard]] ExecResult exec_avx_sse_bridge(ExecContext& ctx, const DecodedInstruction& instr) noexcept;
[[nodiscard]] bool opcode_is_vex_sse_family(Opcode op) noexcept;

/// True when the VEX form of this legacy SSE opcode merges VEX.vvvv into the
/// destination before the operation (Intel SDM Vol. 2, VEX.NDS).
[[nodiscard]] bool vex_sse_merges_vvvv(const DecodedInstruction& instr) noexcept;

/// True for the VEX.NDD packed shift-by-immediate forms, whose destination is
/// VEX.vvvv and whose source is the ModRM r/m register.
[[nodiscard]] bool is_vex_ndd_shift_imm(Opcode op) noexcept;

/// True for the packed shift-by-xmm/m128-count forms, whose VEX.256 form
/// shifts both lanes by the ONE count in the low qword of r/m (Intel SDM
/// Vol. 2 VPSLLD: "COUNT := SRC2[63:0]"). Both engines consult this when
/// running the high lane.
[[nodiscard]] bool vex_rm_is_shared_shift_count(Opcode op) noexcept;

} // namespace vitreo::cpu::ops
