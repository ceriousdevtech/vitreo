// 64-bit shifts and rotates.
//
// Split out of ops/ops64.cpp (ADR-0006, step 3), where compile_64bit was
// 5,916 lines in a single function. The 32-bit forms of this same family have
// had their own file (ops/shifts.cpp) all along, so this restores a boundary
// the codebase had already drawn once and then lost on the x64 side -- the
// recurring shape where machinery exists for one bitness and is open-coded
// for the other.
//
// Verified byte-identical on JitEmissionGolden, and clean on
// vitreo-fuzz-flag-blocks (800/800 x64 vs the host x86 runner). vitreo-fuzz-jit --arch=x64
// reports only the 12 pre-existing RCL_EAX_CL divergences, which are filed
// separately and reproduce without this change.

#include "../jit_helpers.hpp"
#include "../../interpreter/ops/exec_ops.hpp"
#include <vitreo/diagnostics.hpp>
#include "../jit_emit_internal.hpp"

#include <cstdlib>
#include <optional>

#include <optional>

namespace vitreo::cpu::jit {

namespace {
// Bisection switches for this family. They moved here with their only
// remaining callers when the shift/rotate lowering was split out of
// ops64.cpp; nothing left behind reads them.
const bool no_shift32_mem = std::getenv("VITREO_JIT_64BIT_NO_SHIFT32_MEM") != nullptr;
const bool no_shr16 = std::getenv("VITREO_JIT_64BIT_NO_SHR16") != nullptr;
}  // namespace

// 64-bit shifts and rotates: SHL/SHR/SAR, SHLD/SHRD, ROL/ROR, and the 16-
// and 32-bit forms that appear inside a 64-bit block.
//
// Split out of compile_64bit (ADR-0006 step 3), which was 5,916 lines in ONE
// function. This is the same family that already has its own file for 32-bit
// blocks (ops/shifts.cpp), so the cut restores a boundary the codebase had
// already drawn once and then lost on the x64 side -- the recurring shape
// where machinery exists for one bitness and is open-coded for the other.
//
// nullopt means "not a shift or rotate", so compile_64bit continues into its
// own switch. `break` means what it meant before the split: run the
// end-of-block tail and report success.
std::optional<bool> JitCompiler::compile_shifts64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ---- SHL ----------------------------------------------------------------

    case Opcode::SHL_RM64_1: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        emit_set_lazy_flags64(CpuState::FlagOp::Shl64_1, REG_SCRATCH1, REG_SCRATCH0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        break;
    }

    case Opcode::SHL_RM64_IMM8: {
        u8 imm = static_cast<u8>(op1.imm) & 63u;
        if (imm == 0) break;
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH0, imm));
        if (imm == 1) {
            emit_set_lazy_flags64(CpuState::FlagOp::Shl64_1, REG_SCRATCH1,
                                 REG_SCRATCH0);
        } else {
            emit_set_lazy_flags64(CpuState::FlagOp::Shl64, REG_SCRATCH1,
                                 REG_SCRATCH0);
            emit_shift64_cf_imm(REG_SCRATCH1, imm, /*left=*/true);
        }
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        break;
    }

    case Opcode::SHL_RM64_CL: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        // A checked load may call an AAPCS64 helper that clobbers W1.
        // Capture the architectural count only after that call returns.
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        emit_mov_imm32(REG_SCRATCH3, 63u);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH3));
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
        emit(arm64::lslv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        usize cbz_pos = m_code_buf.size(); emit(0u);
        emit_set_lazy_flags64(CpuState::FlagOp::Shl64, REG_CALL_TMP, REG_SCRATCH0);
        if (!m_omit_lazy_current) {
            // Intel SDM SHL: count one defines OF; larger counts do not.
            emit(arm64::cmp_w_imm(REG_SCRATCH1, 1));
            const usize not_one = m_code_buf.size(); emit(0u);
            emit_mov_imm32(REG_SCRATCH3,
                          static_cast<u32>(CpuState::FlagOp::Shl64_1));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
            m_code_buf[not_one] = arm64::b_cond(arm64::NE,
                static_cast<i32>((m_code_buf.size() - not_one) * 4));
        }
        emit_shift64_cf(REG_CALL_TMP, REG_SCRATCH1, /*left=*/true);
        usize after_flags = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(static_cast<u8>(REG_SCRATCH1),
            static_cast<i32>((after_flags - cbz_pos) * 4));

        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- SHR ----------------------------------------------------------------

    case Opcode::SHR_RM64_1: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        emit_set_lazy_flags64(CpuState::FlagOp::Shr64_1, REG_SCRATCH1, REG_SCRATCH0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        break;
    }

    case Opcode::SHR_RM64_IMM8: {
        u8 imm = static_cast<u8>(op1.imm) & 63u;
        if (imm == 0) break;
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0, imm));
        if (imm == 1) {
            emit_set_lazy_flags64(CpuState::FlagOp::Shr64_1, REG_SCRATCH1,
                                 REG_SCRATCH0);
        } else {
            emit_set_lazy_flags64(CpuState::FlagOp::Shr64, REG_SCRATCH1,
                                 REG_SCRATCH0);
            emit_shift64_cf_imm(REG_SCRATCH1, imm, /*left=*/false);
        }
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        break;
    }

    case Opcode::SHR_RM64_CL: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        emit_mov_imm32(REG_SCRATCH3, 63u);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH3));
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
        emit(arm64::lsrv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        usize cbz_pos = m_code_buf.size(); emit(0u);
        emit_set_lazy_flags64(CpuState::FlagOp::Shr64, REG_CALL_TMP, REG_SCRATCH0);
        if (!m_omit_lazy_current) {
            // Intel SDM SHR: count one copies the original sign bit to OF.
            emit(arm64::cmp_w_imm(REG_SCRATCH1, 1));
            const usize not_one = m_code_buf.size(); emit(0u);
            emit_mov_imm32(REG_SCRATCH3,
                          static_cast<u32>(CpuState::FlagOp::Shr64_1));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
            m_code_buf[not_one] = arm64::b_cond(arm64::NE,
                static_cast<i32>((m_code_buf.size() - not_one) * 4));
        }
        emit_shift64_cf(REG_CALL_TMP, REG_SCRATCH1, /*left=*/false);
        usize after_flags = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(static_cast<u8>(REG_SCRATCH1),
            static_cast<i32>((after_flags - cbz_pos) * 4));

        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- SAR ----------------------------------------------------------------

    case Opcode::SAR_RM64_1: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::asr_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        emit_set_lazy_flags64(CpuState::FlagOp::Sar64_1, REG_SCRATCH1, REG_SCRATCH0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        break;
    }

    case Opcode::SAR_RM64_IMM8: {
        u8 imm = static_cast<u8>(op1.imm) & 63u;
        if (imm == 0) break;
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH1, REG_SCRATCH0));
        emit(arm64::asr_x_imm(REG_SCRATCH0, REG_SCRATCH0, imm));
        if (imm == 1) {
            emit_set_lazy_flags64(CpuState::FlagOp::Sar64_1, REG_SCRATCH1,
                                 REG_SCRATCH0);
        } else {
            emit_set_lazy_flags64(CpuState::FlagOp::Sar64, REG_SCRATCH1,
                                 REG_SCRATCH0);
            emit_shift64_cf_imm(REG_SCRATCH1, imm, /*left=*/false);
        }
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            maybe_publish_result_nz64(REG_SCRATCH0);
        }
        break;
    }

    case Opcode::SAR_RM64_CL: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        emit_mov_imm32(REG_SCRATCH3, 63u);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH3));
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
        emit(arm64::asrv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        usize cbz_pos = m_code_buf.size(); emit(0u);
        emit_set_lazy_flags64(CpuState::FlagOp::Sar64, REG_CALL_TMP, REG_SCRATCH0);
        if (!m_omit_lazy_current) {
            // Intel SDM SAR: "the OF flag is cleared for all 1-bit shifts".
            // Sar64 records no count, so it cannot define OF; a count of one
            // publishes Sar64_1, as the SHL/SHR CL forms above do.
            emit(arm64::cmp_w_imm(REG_SCRATCH1, 1));
            const usize not_one = m_code_buf.size(); emit(0u);
            emit_mov_imm32(REG_SCRATCH3,
                          static_cast<u32>(CpuState::FlagOp::Sar64_1));
            emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE, flag_op_offset()));
            m_code_buf[not_one] = arm64::b_cond(arm64::NE,
                static_cast<i32>((m_code_buf.size() - not_one) * 4));
        }
        emit_shift64_cf(REG_CALL_TMP, REG_SCRATCH1, /*left=*/false);
        usize after_flags = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(static_cast<u8>(REG_SCRATCH1),
            static_cast<i32>((after_flags - cbz_pos) * 4));

        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- SHLD / SHRD r/m64 -------------------------------------------------
    // Intel SDM Vol. 2: COUNT := COUNT MOD 64. count==0 ⇒ no op / flags
    // unchanged. Else result combines DEST with SRC; CF = last bit shifted
    // out of DEST; ZSP from result; OF defined for count==1 (sign change),
    // and for CL forms the interpreter also updates OF on any nonzero count.
    case Opcode::SHLD_RM64_R64_IMM8:
    case Opcode::SHRD_RM64_R64_IMM8:
    case Opcode::SHLD_RM64_R64_CL:
    case Opcode::SHRD_RM64_R64_CL: {
        const bool is_shld =
            (instr.opcode == Opcode::SHLD_RM64_R64_IMM8 ||
             instr.opcode == Opcode::SHLD_RM64_R64_CL);
        const bool is_cl =
            (instr.opcode == Opcode::SHLD_RM64_R64_CL ||
             instr.opcode == Opcode::SHRD_RM64_R64_CL);
        const bool is_mem_op = is_mem(op0);

        u8 imm_count = 0;
        if (!is_cl) {
            imm_count = static_cast<u8>(instr.operands[2].imm) & 63;
            if (imm_count == 0) break;
        }

        u32 cbz_idx = 0;
        if (is_cl) {
            emit_load_gpr32(REG_SCRATCH1,
                             static_cast<u8>(RegIndex::RCX));
            emit_mov_imm32(REG_SCRATCH3, 63u);
            emit(arm64::and_w(8, REG_SCRATCH1, REG_SCRATCH3));
            cbz_idx = static_cast<u32>(m_code_buf.size());
            emit(0); // CBZ W8, skip
        }

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_mem_op) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH4, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH4, op0.reg);
        }
        emit_load_gpr64(REG_SCRATCH1, op1.reg);

        if (is_cl) {
            emit_load_gpr32(REG_SCRATCH3,
                             static_cast<u8>(RegIndex::RCX));
            emit_mov_imm32(REG_EA_TMP, 63u);
            emit(arm64::and_w(8, REG_SCRATCH3, REG_EA_TMP));

            emit(arm64::movz_w(REG_SCRATCH2, 64));
            emit(arm64::sub_w(REG_SCRATCH2, REG_SCRATCH2, 8));

            if (is_shld) {
                emit(arm64::lslv_x(REG_SCRATCH0, REG_SCRATCH4, 8));
                emit(arm64::lsrv_x(REG_EA_TMP, REG_SCRATCH1,
                                    REG_SCRATCH2));
                emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_EA_TMP));
                emit(arm64::lsrv_x(REG_SCRATCH2, REG_SCRATCH4,
                                    REG_SCRATCH2));
            } else {
                emit(arm64::lsrv_x(REG_SCRATCH0, REG_SCRATCH4, 8));
                emit(arm64::lslv_x(REG_EA_TMP, REG_SCRATCH1,
                                    REG_SCRATCH2));
                emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_EA_TMP));
                emit(arm64::movz_w(REG_SCRATCH2, 1));
                emit(arm64::sub_w(REG_SCRATCH2, 8, REG_SCRATCH2));
                emit(arm64::lsrv_x(REG_SCRATCH2, REG_SCRATCH4,
                                    REG_SCRATCH2));
            }
            emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1)); // AND #1
        } else {
            if (is_shld) {
                emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH4,
                                       imm_count));
                emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH1,
                                       static_cast<u8>(64 - imm_count)));
                emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH2));
                emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(64 - imm_count)));
            } else {
                emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH4,
                                       imm_count));
                emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH1,
                                       static_cast<u8>(64 - imm_count)));
                emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0,
                                   REG_SCRATCH2));
                emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH4,
                                       static_cast<u8>(imm_count - 1)));
            }
            emit(arm64::and_w_imm(REG_SCRATCH2, REG_SCRATCH2, 0x1)); // AND #1
        }

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
            emit_mov_imm32(REG_SCRATCH1, 0xC5u);
            emit(0x8A210063u); // BIC X3, X3, X1
            emit(0xAA020063u); // ORR X3, X3, X2 (CF)

            emit(arm64::cmp_x_imm(REG_SCRATCH0, 0));
            emit(arm64::cset_w(REG_SCRATCH1, arm64::EQ));
            emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 6));
            emit(0xAA010063u);

            emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
            emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 7));
            emit(0xAA010063u);

            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_CALL_TMP, REG_SCRATCH1, 4));
            emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1, REG_CALL_TMP));
            emit_mov_imm32(REG_CALL_TMP, 0xFu);
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_CALL_TMP));
            emit_mov_imm32(REG_CALL_TMP, 0x9669u);
            emit(arm64::lsrv_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH1));
            emit_mov_imm32(REG_SCRATCH1, 1);
            emit(arm64::and_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH1));
            emit(arm64::lsl_w_imm(REG_CALL_TMP, REG_CALL_TMP, 2));
            emit(0xAA150063u); // ORR X3, X3, X21

            if (is_cl || imm_count == 1) {
                emit(arm64::lsr_x_imm(REG_SCRATCH1, REG_SCRATCH0, 63));
                emit(arm64::lsr_x_imm(REG_SCRATCH2, REG_SCRATCH4, 63));
                emit(arm64::eor_w(REG_SCRATCH1, REG_SCRATCH1,
                                   REG_SCRATCH2));
                emit_mov_imm32(REG_SCRATCH2, 0x800u);
                emit(0x8A220063u); // BIC X3, X3, X2
                emit(arm64::lsl_w_imm(REG_SCRATCH1, REG_SCRATCH1, 11));
                emit(0xAA010063u);
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                    rf_off / 8));
        }

        if (is_mem_op) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }

        if (is_cl) {
            u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }
        break;
    }

    // ========================================================================
    // ROL / ROR RM64 — 64-bit rotates.
    //
    // ARM64 only has ROR (rotate right). For ROL by N we emit ROR by (64-N).
    // For the CL form we negate the (masked) count and use RORV.
    //
    // Intel SDM Vol. 2 RCL/RCR/ROL/ROR: only CF is defined for every nonzero
    // count; OF is defined only when count==1; ZF/SF/PF/AF are unchanged.
    // FlagOp::Ror64 would replace a pending SUB/XOR and lose its ZF (e.g.
    // SUB;ROR;JZ). Use cf_overlay/of_overlay and leave flag_op intact, matching
    // the 32-bit ROR stencil and Interpreter::exec_ror write_cf_only.
    //
    // count == 0 leaves dest and flags unchanged. IMM8 0 skips emit. CL form
    // skips the overlay write via CBZ on the masked count.
    // ========================================================================

    // ---- ROL ----------------------------------------------------------------

    case Opcode::ROL_RM64_1: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        // ROL by 1 == ROR by 63.
        emit(arm64::ror_x_imm(REG_SCRATCH0, REG_SCRATCH0, 63));
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/false,
                                   /*define_of=*/true);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::ROL_RM64_IMM8: {
        u8 imm = static_cast<u8>(op1.imm) & 63u;
        if (imm == 0) break;
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        // ROL by n == ROR by (64 - n) for n in [1..63].
        emit(arm64::ror_x_imm(REG_SCRATCH0, REG_SCRATCH0,
                              static_cast<u8>(64 - imm)));
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/false,
                                   /*define_of=*/imm == 1);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::ROL_RM64_CL: {
        // Load the OPERAND first, then derive the count.
        //
        // The count used to be computed into W1/W3/W4 ahead of the operand
        // load, and emit_guest_load64 reaches a helper call on the non-linear
        // region path, which clobbers all three. The ROR amount in W4 came
        // back as whatever the helper left, so `rol qword [rbx], cl` rotated by
        // the wrong amount — observed as no rotation at all — and the flag
        // code below reads the count from W1 as well. Found by the ISA matrix
        // sweep; same defect family as ADC/SBB r8, IMUL r16 and CMPSS.
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        // count = CL & 63; rotate amount for ROR = (-count) & 63.
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        emit_mov_imm32(REG_SCRATCH3, 63u);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH3));
        // REG_SCRATCH4 = (-count) & 63  (ROR amount equivalent to ROL by count)
        emit(arm64::neg_w(REG_SCRATCH4, REG_SCRATCH1));
        emit(arm64::and_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH3));
        emit(arm64::rorv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH4));

        usize cbz_pos = m_code_buf.size(); emit(0u);
        emit(arm64::cmp_w_imm(REG_SCRATCH1, 1));
        usize bne_pos = m_code_buf.size(); emit(0u);
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/false,
                                   /*define_of=*/true);
        usize after_of = m_code_buf.size(); emit(0u);
        usize no_of = m_code_buf.size();
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/false,
                                   /*define_of=*/false);
        usize after_flags = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(static_cast<u8>(REG_SCRATCH1),
            static_cast<i32>((after_flags - cbz_pos) * 4));
        m_code_buf[bne_pos] = arm64::b_cond(arm64::NE,
            static_cast<i32>((no_of - bne_pos) * 4));
        m_code_buf[after_of] = arm64::b_imm(
            static_cast<i32>((after_flags - after_of) * 4));

        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- ROR ----------------------------------------------------------------

    case Opcode::ROR_RM64_1: {
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::ror_x_imm(REG_SCRATCH0, REG_SCRATCH0, 1));
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/true,
                                   /*define_of=*/true);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::ROR_RM64_IMM8: {
        u8 imm = static_cast<u8>(op1.imm) & 63u;
        if (imm == 0) break;
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::ror_x_imm(REG_SCRATCH0, REG_SCRATCH0, imm));
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/true,
                                   /*define_of=*/imm == 1);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::ROR_RM64_CL: {
        // Operand first, then the count — see the twin comment on ROL_RM64_CL.
        bool m = is_mem(op0);
        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        emit_mov_imm32(REG_SCRATCH3, 63u);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH3));
        emit(arm64::rorv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        usize cbz_pos = m_code_buf.size(); emit(0u);
        emit(arm64::cmp_w_imm(REG_SCRATCH1, 1));
        usize bne_pos = m_code_buf.size(); emit(0u);
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/true,
                                   /*define_of=*/true);
        usize after_of = m_code_buf.size(); emit(0u);
        usize no_of = m_code_buf.size();
        emit_rotate64_partial_flags(REG_SCRATCH0, /*cf_from_msb=*/true,
                                   /*define_of=*/false);
        usize after_flags = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(static_cast<u8>(REG_SCRATCH1),
            static_cast<i32>((after_flags - cbz_pos) * 4));
        m_code_buf[bne_pos] = arm64::b_cond(arm64::NE,
            static_cast<i32>((no_of - bne_pos) * 4));
        m_code_buf[after_of] = arm64::b_imm(
            static_cast<i32>((after_flags - after_of) * 4));

        if (m) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ========================================================================
    // 32-bit-operand shifts inside a 64-bit block.
    //
    // In long mode the 32-bit native stencils are gated off, and these RM32
    // shift forms have no REX.W variant, so without these cases they fall
    // through to the interpreter delegate (the dominant self-extract
    // fallback: SHR_RM32_IMM8 alone was ~24% of delegated instructions).
    //
    // Memory destinations use the same full-width-EA narrow access wrappers
    // as the other PE32+ operations. Register writes zero-extend to 64 bits
    // via emit_store_gpr32 (x86-64 semantics). Flag handling mirrors the
    // interpreter through emit_set_shift_lazy_flags.
    // ========================================================================
    case Opcode::SHL_RM32_IMM8:
    case Opcode::SHL_RM32_1:
    case Opcode::SHR_RM32_IMM8:
    case Opcode::SHR_RM32_1:
    case Opcode::SAR_RM32_IMM8:
    case Opcode::SAR_RM32_1: {
        const bool operand_is_mem = is_mem(op0);
        if (operand_is_mem &&
            no_shift32_mem)
            return false;
        const bool by_one = (instr.opcode == Opcode::SHL_RM32_1 ||
                             instr.opcode == Opcode::SHR_RM32_1 ||
                             instr.opcode == Opcode::SAR_RM32_1);
        u8 shift = by_one ? 1u : static_cast<u8>(op1.imm & 31);
        if (shift == 0) {
            // Flags unchanged; 32-bit dest write still zero-extends the GPR.
            if (!operand_is_mem) {
                emit_load_gpr32(REG_SCRATCH0, op0.reg);
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
            }
            break;
        }
        // A register operand is shifted where it lives: in place on its pin.
        // The original value and the count are staged only for a record that
        // is published (emit_set_shift_lazy_flags emits nothing, and defers
        // nothing, when within-block liveness omitted it).
        const u8 pin = operand_is_mem ? u8{0xFF} : pinned_gpr32(op0.reg);
        const u8 r = pin != 0xFF ? pin : u8{REG_SCRATCH0};
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH4);
        } else if (pin == 0xFF) {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        if (!m_omit_lazy_current) {
            emit(arm64::mov_w(REG_SCRATCH4, r));      // original
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(shift));
        }
        CpuState::FlagOp fo;
        switch (instr.opcode) {
        case Opcode::SHL_RM32_IMM8:
        case Opcode::SHL_RM32_1:
            emit(arm64::lsl_w_imm(r, r, shift));
            fo = CpuState::FlagOp::Shl32; break;
        case Opcode::SHR_RM32_IMM8:
        case Opcode::SHR_RM32_1:
            emit(arm64::lsr_w_imm(r, r, shift));
            fo = CpuState::FlagOp::Shr32; break;
        default:
            emit(arm64::asr_w_imm(r, r, shift));
            fo = CpuState::FlagOp::Sar32; break;
        }
        emit_set_shift_lazy_flags(fo, REG_SCRATCH4, REG_SCRATCH1, r);
        if (pin != 0xFF) {
            if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                m_esp_frame_live = false;
            }
            break;
        }
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::SHL_RM32_CL:
    case Opcode::SHR_RM32_CL:
    case Opcode::SAR_RM32_CL: {
        const bool operand_is_mem = is_mem(op0);
        if (operand_is_mem &&
            no_shift32_mem)
            return false;
        // A checked memory load may call a helper and clobber volatile ARM64
        // registers, so load the destination before capturing CL.
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        emit_mov_imm32(REG_SCRATCH3, 31u);
        emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1, REG_SCRATCH3));  // count &= 31
        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));      // original
        CpuState::FlagOp fo;
        switch (instr.opcode) {
        case Opcode::SHL_RM32_CL:
            emit(arm64::lslv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            fo = CpuState::FlagOp::Shl32; break;
        case Opcode::SHR_RM32_CL:
            emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            fo = CpuState::FlagOp::Shr32; break;
        default:
            emit(arm64::asrv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
            fo = CpuState::FlagOp::Sar32; break;
        }
        // Intel SDM Vol. 2 SHL/SHR/SAR: when (CL & 31) == 0 the FLAGS are
        // unaffected. That is the only part of the instruction that is
        // skipped — the destination is still written.
        //
        // This used to skip the destination write as well, on the reading that
        // a zero count makes the whole instruction a no-op. It does not, and
        // in 64-bit mode the difference is architecturally visible: a 32-bit
        // register destination zero-extends into bits 63:32 (Intel SDM Vol. 1
        // 3.4.1.1), and there is no exception for a zero shift count.
        // With RAX = 0xAAAAAAAADEADBEEF and CL = 0, `shr eax, cl` leaves
        // RAX = 0xDEADBEEF, not 0xAAAAAAAADEADBEEF.
        //
        // The shift result register already holds the original value when the
        // count is zero (a variable shift by 0 is the identity), so the store
        // is correct on both paths and only the flag update needs the branch.
        usize cbz_pos = m_code_buf.size(); emit(0u);
        emit_set_shift_lazy_flags(fo, REG_SCRATCH4, REG_SCRATCH1, REG_SCRATCH0);
        usize after_flags = m_code_buf.size();
        m_code_buf[cbz_pos] = arm64::cbz_w(static_cast<u8>(REG_SCRATCH1),
            static_cast<i32>((after_flags - cbz_pos) * 4));
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ---- SHR r/m16, imm8 / 1 / CL -----------------------------------------
    // This is the remaining sampled self-extract shift fallback. Unlike
    // the old narrow JIT path, Shr16's lazy state records original+count so CF
    // and the count==1 OF rule materialize exactly.
    case Opcode::SHR_RM16_IMM8:
    case Opcode::SHR_RM16_1:
    case Opcode::SHR_RM16_CL: {
        if (no_shr16)
            return false;
        const bool operand_is_mem = is_mem(op0);
        const bool by_cl = instr.opcode == Opcode::SHR_RM16_CL;
        const bool by_one = instr.opcode == Opcode::SHR_RM16_1;

        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_load16_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_load_gpr16(REG_SCRATCH0, op0.reg);
        }

        if (by_cl) {
            emit_load_gpr32(REG_SCRATCH1,
                            static_cast<u8>(RegIndex::RCX));
            emit_mov_imm32(REG_SCRATCH3, 31u);
            emit(arm64::and_w(REG_SCRATCH1, REG_SCRATCH1,
                              REG_SCRATCH3));
        } else {
            const u8 count = by_one
                ? 1u : static_cast<u8>(op1.imm & 31);
            if (count == 0) break;
            emit_mov_imm32(REG_SCRATCH1, count);
        }

        emit(arm64::mov_w(REG_SCRATCH4, REG_SCRATCH0));  // original
        if (by_cl) {
            emit(arm64::lsrv_w(REG_SCRATCH0, REG_SCRATCH0,
                               REG_SCRATCH1));
        } else {
            const u8 count = by_one
                ? 1u : static_cast<u8>(op1.imm & 31);
            emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH0, count));
        }

        usize zero_count_pos = 0;
        if (by_cl) {
            zero_count_pos = m_code_buf.size();
            emit(0u);
        }
        emit_set_shift_lazy_flags(CpuState::FlagOp::Shr16,
                                  REG_SCRATCH4, REG_SCRATCH1,
                                  REG_SCRATCH0);
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_guest_store16_64(REG_SCRATCH0, REG_SCRATCH4);
        } else {
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        }
        if (by_cl) {
            const usize done_pos = m_code_buf.size();
            m_code_buf[zero_count_pos] = arm64::cbz_w(
                static_cast<u8>(REG_SCRATCH1),
                static_cast<i32>((done_pos - zero_count_pos) * 4));
        }
        break;
    }

    // ---- ROR r/m32, imm8 / 1 ------------------------------------------------
    // The fallback histogram fingered ROR r/m32,imm8 as ~19% of delegated x64
    // instructions in a self-extract path, so route it natively.
    //
    // ROR is a *partial-flag* op: it writes only CF (and OF for count==1),
    // leaving SF/ZF/PF/AF untouched. A lazy FlagOp cannot express that — setting
    // one overwrites any pending lazy op and loses the un-computed flags in a
    // mixed block (e.g. SUB;ROR;JZ, where the JZ must see the SUB's ZF). So
    // materialize pending flags first, then patch CF/OF directly in rflags
    // (flag_op stays None). This mirrors the proven 32-bit compile_shifts ROR
    // path exactly, including memory destinations (64-bit EA + 32-bit access).
    case Opcode::ROR_RM32_IMM8:
    case Opcode::ROR_RM32_1: {
        u8 shift = (instr.opcode == Opcode::ROR_RM32_1)
                       ? 1u : static_cast<u8>(op1.imm & 31);
        if (shift == 0) {
            // Intel SDM Vol. 2 ROL/ROR: a rotate by 0 leaves the FLAGS
            // untouched — but not the destination. A 32-bit register write
            // still zero-extends into bits 63:32 (Intel SDM Vol. 1 3.4.1.1),
            // with no exception for a zero count. Verified on real x86-64:
            // `ror eax, 0` on RAX = 0xAAAAAAAADEADBEEF yields 0xDEADBEEF.
            if (!is_mem(op0)) {
                emit_load_gpr32(REG_SCRATCH0, op0.reg);
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
            }
            break;
        }

        const bool operand_is_mem = is_mem(op0);
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, shift));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            // CF = bit 31 of result.
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC  X3, X3, X4       (clear CF)
            emit(0x531F7C04u);   // UBFX W4, W0, #31, #1  (bit31 -> W4)
            emit(0xAA040063u);   // ORR  X3, X3, X4       (set CF)
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                // OF = bit31 XOR bit30 of result (defined only for count==1).
                emit(arm64::movz_w(4, 0x800));
                emit(0x8A240063u);   // BIC  X3, X3, X4   (clear OF, bit 11)
                emit(0x531F7C02u);   // UBFX W2, W0, #31, #1
                emit(0x531E7804u);   // UBFX W4, W0, #30, #1
                emit(0x4A020084u);   // EOR  W4, W4, W2
                emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
                emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                      of_overlay_offset()));
                emit(0x53155084u);   // LSL  W4, W4, #11
                emit(0xAA040063u);   // ORR  X3, X3, X4
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (operand_is_mem) {
            // Load helpers clobber the address register — recompute EA.
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);  // zero-extends to 64 bits
        }
        break;
    }

    // ---- ROL r/m32, imm8 / 1 ------------------------------------------------
    // Same partial-flag contract as ROR r/m32,imm above (Intel SDM Vol. 2
    // RCL/RCR/ROL/ROR): CF (and OF for count==1) only; SF/ZF/PF/AF untouched.
    // ARM64 has no ROL — emit ROR by (32-n). CF = LSB of result; OF for
    // count==1 is CF XOR MSB. Memory uses 64-bit EA + 32-bit access.
    case Opcode::ROL_RM32_IMM8:
    case Opcode::ROL_RM32_1: {
        u8 shift = (instr.opcode == Opcode::ROL_RM32_1)
                       ? 1u : static_cast<u8>(op1.imm & 31);
        if (shift == 0) {
            // Intel SDM Vol. 2 ROL/ROR: a rotate by 0 leaves the FLAGS
            // untouched — but not the destination. A 32-bit register write
            // still zero-extends into bits 63:32 (Intel SDM Vol. 1 3.4.1.1),
            // with no exception for a zero count. Verified on real x86-64:
            // `ror eax, 0` on RAX = 0xAAAAAAAADEADBEEF yields 0xDEADBEEF.
            if (!is_mem(op0)) {
                emit_load_gpr32(REG_SCRATCH0, op0.reg);
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
            }
            break;
        }

        const bool operand_is_mem = is_mem(op0);
        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }
        // ROL by n == ROR by (32 - n) for n in [1..31].
        const u8 ror_amount = static_cast<u8>((32 - shift) & 31);
        emit(arm64::ror_w_imm(REG_SCRATCH0, REG_SCRATCH0, ror_amount));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            // CF = bit 0 of result.
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC  X3, X3, X4       (clear CF)
            emit(arm64::and_w_imm(REG_SCRATCH4, REG_SCRATCH0, 0x1)); // AND W4, W0, #1
            emit(0xAA040063u);   // ORR  X3, X3, X4       (set CF)
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            if (shift == 1) {
                // OF = CF XOR MSB of result (defined only for count==1).
                emit(arm64::movz_w(4, 0x800));
                emit(0x8A240063u);   // BIC  X3, X3, X4   (clear OF, bit 11)
                emit(0x531F7C02u);   // UBFX W2, W0, #31, #1
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
                                     // AND W4, W0, #1
                emit(0x4A020084u);   // EOR  W4, W4, W2
                emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
                emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                      of_overlay_offset()));
                emit(0x53155084u);   // LSL  W4, W4, #11
                emit(0xAA040063u);   // ORR  X3, X3, X4
            }
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);  // zero-extends to 64 bits
        }
        break;
    }

    // ---- ROR / ROL r/m32, CL ------------------------------------------------
    // Same partial-flag contract as ROR r/m32,imm above (Intel SDM Vol. 2
    // RCL/RCR/ROL/ROR). Count = CL & 0x1F; zero is a complete no-op (dst and
    // flags untouched, including a register's upper 32 bits). Memory uses the
    // 64-bit EA + 32-bit access wrappers; load before capturing CL because a
    // checked miss may call a host helper that clobbers volatiles.
    case Opcode::ROR_RM32_CL:
    case Opcode::ROL_RM32_CL: {
        const bool is_rol = (instr.opcode == Opcode::ROL_RM32_CL);
        const bool operand_is_mem = is_mem(op0);

        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        }

        emit_load_gpr32(REG_SCRATCH1, static_cast<u8>(RegIndex::RCX));
        // AND W8, W1, #0x1F — keep masked count in W8 across the rotate.
        emit(arm64::and_w_imm(8, REG_SCRATCH1, 0x1F));
        const u32 cbz_idx = static_cast<u32>(m_code_buf.size());
        emit(0);  // CBZ W8, end

        // ARM RORV already uses the low 5 bits of Wm; for ROL, negate count.
        if (is_rol) {
            emit(0x4B0103E1u); // SUB W1, WZR, W1 (NEG W1, W1)
        }
        emit(arm64::rorv_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));

        {
            u32 rf_off = cpu_rflags_offset();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
            emit(arm64::movz_w(4, 1));
            emit(0x8A240063u);   // BIC X3, X3, X4
            if (is_rol) {
                // CF = bit 0 of result.
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                // CF = bit 31 of result.
                emit(0x531F7C04u); // UBFX W4, W0, #31, #1
            }
            emit(0xAA040063u);   // ORR X3, X3, X4
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  cf_overlay_offset()));

            // OF defined only when masked count == 1.
            emit(arm64::cmp_w_imm(8, 1));
            const usize of_skip = m_code_buf.size();
            emit(0u);  // B.NE skip OF
            emit(arm64::movz_w(4, 0x800));
            emit(0x8A240063u);   // BIC X3, X3, X4
            emit(0x531F7C02u);   // UBFX W2, W0, #31, #1
            if (is_rol) {
                emit(arm64::and_w_imm(4, REG_SCRATCH0, 0x1));
            } else {
                emit(0x531E7804u); // UBFX W4, W0, #30, #1
            }
            emit(0x4A020084u);   // EOR W4, W4, W2
            emit(arm64::add_w_imm(REG_SCRATCH1, REG_SCRATCH4, 1));
            emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                  of_overlay_offset()));
            emit(0x53155084u);   // LSL W4, W4, #11
            emit(0xAA040063u);   // ORR X3, X3, X4
            const usize of_done = m_code_buf.size();
            m_code_buf[of_skip] = arm64::b_cond(
                arm64::NE, static_cast<i32>(of_done - of_skip) * 4);
            emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        }

        // A masked count of zero skips the rotate and the flag writes, but NOT
        // the destination write: a 32-bit register destination still
        // zero-extends into bits 63:32 (Intel SDM Vol. 1 3.4.1.1), verified on
        // real x86-64. So the CBZ lands here, ahead of the store, rather than
        // past it. REG_SCRATCH0 still holds the loaded original on that path.
        {
            const u32 skip_target = static_cast<u32>(m_code_buf.size());
            m_code_buf[cbz_idx] = arm64::cbz_w(
                8, static_cast<i32>(skip_target - cbz_idx) * 4);
        }

        if (operand_is_mem) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
