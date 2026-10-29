// AVX / AVX2 / FMA3 / F16C lowering.
//
// Split out of ops/sse.cpp (ADR-0006, step 3). compile_sse was 6,863 lines in
// a SINGLE function -- unreviewable, and impossible to break up until three
// `[&]` lambdas its case arms closed over were promoted to member functions
// first. Those promotions were verified to capture nothing from the enclosing
// scope, so they were mechanical.
//
// The cut is the seam compile_sse's own prologue already describes: a legacy
// SSE opcode reused under a VEX prefix is NOT the legacy instruction and must
// go to the interpreter's AVX/SSE bridge, whereas the AVX-unique opcodes sit
// outside the legacy opcode range and keep their native lowerings. Those are
// what live here.
//
// Verified byte-identical on JitEmissionGolden and clean on vitreo-fuzz-jit
// (156,800/156,800 x86).

#include "core/math/host_fenv.hpp"
#include "../jit_helpers.hpp"
#include "../../interpreter/ops/exec_ops.hpp"
#include <cstdlib>
#include <limits>
#include <optional>

#include <optional>

namespace vitreo::cpu::jit {

// AVX/AVX2/FMA3/F16C opcodes that are OUTSIDE the legacy SSE opcode range.
//
// Split out of compile_sse (ADR-0006 step 3), which was 6,863 lines in ONE
// function. The seam is the one compile_sse's own prologue already names: a
// legacy SSE opcode reused under VEX must go to the interpreter bridge, while
// "the AVX-unique opcodes (VFMADD*/VPERM*/VPBLEND*/VINSERT*/VPBROADCAST*) are
// outside that range and keep their lowerings". A semantic boundary, not a
// line-count one.
//
// nullopt means "not one of these", so compile_sse falls through to its own
// switch. `break` inside an arm means exactly what it meant before the split:
// run the end-of-block tail and report success.
std::optional<bool> JitCompiler::compile_avx_unique(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip,
    bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // ---- F16C: VCVTPH2PS / VCVTPS2PH -------------------------------------
    //
    // ARM64 FCVTL/FCVTN are exactly the half<->single conversions, and
    // half-to-single is mandatory in the base architecture (only FP16
    // *arithmetic* is optional), so this needs no feature gate.
    //
    // VCVTPH2PS has no rounding control -- every half is exactly representable
    // as a single -- so it lowers unconditionally.
    case Opcode::VCVTPH2PS_XMM_XMMM64:
    case Opcode::VCVTPH2PS_YMM_XMMM128: {
        const bool ymm = instr.opcode == Opcode::VCVTPH2PS_YMM_XMMM128;
        const u8 dst = op0.reg & 15;
        const u32 dst_off = xmm_u64_off(dst, 0);
        if (!fits_ldr_q_offset(dst_off)) return false;
        if (!fits_ldr_q_offset(ymm_high_u64_off(dst, 0))) return false;

        // Decide every reachability question before emitting anything --
        // bailing out after emit() would leave orphaned instructions.
        if (is_register_operand(op1.type)) {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_q_offset(src_off)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off >> 4));
        } else {
            // Memory source: m64 (xmm form, four halves) or m128 (ymm form,
            // eight halves). Intel SDM Vol. 2 VCVTPH2PS: VEX memory operands
            // carry no alignment requirement. The whole operand is read into
            // GPRs before V0 is formed and before anything is stored, so a
            // fault on the second qword leaves the destination untouched
            // (hardware never writes DEST on a faulting read), and no vector
            // register is live across a guest load (its slow path BLRs a
            // helper, which clobbers V0-V7).
            //
            // The EA is parked in X21 (callee-saved) and EA+8 is formed from
            // it with ADD Xd -- never MOV Wd -- so a 64-bit EA is not
            // truncated (the recorded jit64_ea_truncation family).
            //
            // The guest loads' region probe (SUBS) and helper BLR clobber
            // NZCV, so a live predecessor flag result is spilled around them,
            // exactly as the FMA memory form below does.
            const bool hold_nzcv = should_hold_nzcv();
            if (hold_nzcv) emit_nzcv_spill();
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            if (ymm) {
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::ins_v_gpr(0, 0, REG_CALL_TMP, arm64::VS_D));
                emit(arm64::ins_v_gpr(0, 1, REG_SCRATCH0, arm64::VS_D));
            } else {
                emit(arm64::ins_v_gpr(0, 0, REG_SCRATCH0, arm64::VS_D));
            }
            if (hold_nzcv) emit_nzcv_fill();
        }

        emit(arm64::fcvtl_4s_4h(1, 0));                 // low 4 halves -> 4 singles
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, dst_off >> 4));
        if (ymm) {
            emit(arm64::fcvtl2_4s_8h(2, 0));            // upper 4 halves
            emit(arm64::str_q_uoff(2, REG_CPU_STATE,
                                   ymm_high_u64_off(dst, 0) >> 4));
        } else {
            // 128-bit VEX form zeroes the destination's upper half.
            emit(arm64::eor_v16b(3, 3, 3));
            emit(arm64::str_q_uoff(3, REG_CPU_STATE,
                                   ymm_high_u64_off(dst, 0) >> 4));
        }
        break;
    }

    // ---- FMA3: VFMADD / VFMSUB / VFNMADD / VFNMSUB -----------------------
    //
    // x86 FMA is *fused* — one rounding for the whole multiply-add — and so are
    // ARM64's FMLA/FMLS and the scalar FMADD family. That makes the mapping
    // exact. A lowering that decomposed into a separate multiply and add would
    // be wrong in the last bit and no test of ordinary values would notice.
    //
    // The three orders differ only in which operand plays which role
    // (Intel SDM Vol. 2 FMA):
    //   132: dest := dest*rm   + vvvv     -> a=dest, b=rm,   c=vvvv
    //   213: dest := vvvv*dest + rm       -> a=vvvv, b=dest, c=rm
    //   231: dest := vvvv*rm   + dest     -> a=vvvv, b=rm,   c=dest
    //
    // and the four sign variants reduce to "accumulate into +c or -c, with
    // FMLA or FMLS":
    //   VFMADD   a*b + c   ->  acc =  c;  FMLA
    //   VFMSUB   a*b - c   ->  acc = -c;  FMLA
    //   VFNMADD -a*b + c   ->  acc =  c;  FMLS
    //   VFNMSUB -a*b - c   ->  acc = -c;  FMLS
    // Negating the accumulator first is exact, so it costs no accuracy.
    case Opcode::VFMADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD132SS_XMM_XMM_XMMM32:
    case Opcode::VFMADD132SD_XMM_XMM_XMMM64:
    case Opcode::VFMADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD213SS_XMM_XMM_XMMM32:
    case Opcode::VFMADD213SD_XMM_XMM_XMMM64:
    case Opcode::VFMADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFMADD231PD_XMM_XMM_XMMM128:
    case Opcode::VFMADD231SS_XMM_XMM_XMMM32:
    case Opcode::VFMADD231SD_XMM_XMM_XMMM64:
    case Opcode::VFMSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB132SS_XMM_XMM_XMMM32:
    case Opcode::VFMSUB132SD_XMM_XMM_XMMM64:
    case Opcode::VFMSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB213SS_XMM_XMM_XMMM32:
    case Opcode::VFMSUB213SD_XMM_XMM_XMMM64:
    case Opcode::VFMSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUB231SS_XMM_XMM_XMMM32:
    case Opcode::VFMSUB231SD_XMM_XMM_XMMM64:
    case Opcode::VFNMADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD132SS_XMM_XMM_XMMM32:
    case Opcode::VFNMADD132SD_XMM_XMM_XMMM64:
    case Opcode::VFNMADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD213SS_XMM_XMM_XMMM32:
    case Opcode::VFNMADD213SD_XMM_XMM_XMMM64:
    case Opcode::VFNMADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFNMADD231PD_XMM_XMM_XMMM128:
    case Opcode::VFNMADD231SS_XMM_XMM_XMMM32:
    case Opcode::VFNMADD231SD_XMM_XMM_XMMM64:
    case Opcode::VFNMSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB132SS_XMM_XMM_XMMM32:
    case Opcode::VFNMSUB132SD_XMM_XMM_XMMM64:
    case Opcode::VFNMSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB213SS_XMM_XMM_XMMM32:
    case Opcode::VFNMSUB213SD_XMM_XMM_XMMM64:
    case Opcode::VFNMSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFNMSUB231SS_XMM_XMM_XMMM32:
    case Opcode::VFNMSUB231SD_XMM_XMM_XMMM64: {
        const Opcode oc = instr.opcode;
        auto is_one_of = [&](std::initializer_list<Opcode> l) {
            for (Opcode o : l) if (oc == o) return true;
            return false;
        };
        const bool dbl = is_one_of({
            Opcode::VFMADD132PD_XMM_XMM_XMMM128, Opcode::VFMADD213PD_XMM_XMM_XMMM128,
            Opcode::VFMADD231PD_XMM_XMM_XMMM128, Opcode::VFMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFMSUB213PD_XMM_XMM_XMMM128, Opcode::VFMSUB231PD_XMM_XMM_XMMM128,
            Opcode::VFNMADD132PD_XMM_XMM_XMMM128, Opcode::VFNMADD213PD_XMM_XMM_XMMM128,
            Opcode::VFNMADD231PD_XMM_XMM_XMMM128, Opcode::VFNMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB213PD_XMM_XMM_XMMM128, Opcode::VFNMSUB231PD_XMM_XMM_XMMM128,
            Opcode::VFMADD132SD_XMM_XMM_XMMM64, Opcode::VFMADD213SD_XMM_XMM_XMMM64,
            Opcode::VFMADD231SD_XMM_XMM_XMMM64, Opcode::VFMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFMSUB213SD_XMM_XMM_XMMM64, Opcode::VFMSUB231SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD132SD_XMM_XMM_XMMM64, Opcode::VFNMADD213SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD231SD_XMM_XMM_XMMM64, Opcode::VFNMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB213SD_XMM_XMM_XMMM64, Opcode::VFNMSUB231SD_XMM_XMM_XMMM64});
        const bool scalar = is_one_of({
            Opcode::VFMADD132SS_XMM_XMM_XMMM32, Opcode::VFMADD213SS_XMM_XMM_XMMM32,
            Opcode::VFMADD231SS_XMM_XMM_XMMM32, Opcode::VFMSUB132SS_XMM_XMM_XMMM32,
            Opcode::VFMSUB213SS_XMM_XMM_XMMM32, Opcode::VFMSUB231SS_XMM_XMM_XMMM32,
            Opcode::VFNMADD132SS_XMM_XMM_XMMM32, Opcode::VFNMADD213SS_XMM_XMM_XMMM32,
            Opcode::VFNMADD231SS_XMM_XMM_XMMM32, Opcode::VFNMSUB132SS_XMM_XMM_XMMM32,
            Opcode::VFNMSUB213SS_XMM_XMM_XMMM32, Opcode::VFNMSUB231SS_XMM_XMM_XMMM32,
            Opcode::VFMADD132SD_XMM_XMM_XMMM64, Opcode::VFMADD213SD_XMM_XMM_XMMM64,
            Opcode::VFMADD231SD_XMM_XMM_XMMM64, Opcode::VFMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFMSUB213SD_XMM_XMM_XMMM64, Opcode::VFMSUB231SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD132SD_XMM_XMM_XMMM64, Opcode::VFNMADD213SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD231SD_XMM_XMM_XMMM64, Opcode::VFNMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB213SD_XMM_XMM_XMMM64, Opcode::VFNMSUB231SD_XMM_XMM_XMMM64});
        const bool negate_acc = is_one_of({
            Opcode::VFMSUB132PS_XMM_XMM_XMMM128, Opcode::VFMSUB213PS_XMM_XMM_XMMM128,
            Opcode::VFMSUB231PS_XMM_XMM_XMMM128, Opcode::VFMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFMSUB213PD_XMM_XMM_XMMM128, Opcode::VFMSUB231PD_XMM_XMM_XMMM128,
            Opcode::VFMSUB132SS_XMM_XMM_XMMM32, Opcode::VFMSUB213SS_XMM_XMM_XMMM32,
            Opcode::VFMSUB231SS_XMM_XMM_XMMM32, Opcode::VFMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFMSUB213SD_XMM_XMM_XMMM64, Opcode::VFMSUB231SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB132PS_XMM_XMM_XMMM128, Opcode::VFNMSUB213PS_XMM_XMM_XMMM128,
            Opcode::VFNMSUB231PS_XMM_XMM_XMMM128, Opcode::VFNMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB213PD_XMM_XMM_XMMM128, Opcode::VFNMSUB231PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB132SS_XMM_XMM_XMMM32, Opcode::VFNMSUB213SS_XMM_XMM_XMMM32,
            Opcode::VFNMSUB231SS_XMM_XMM_XMMM32, Opcode::VFNMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB213SD_XMM_XMM_XMMM64, Opcode::VFNMSUB231SD_XMM_XMM_XMMM64});
        const bool use_fmls = is_one_of({
            Opcode::VFNMADD132PS_XMM_XMM_XMMM128, Opcode::VFNMADD213PS_XMM_XMM_XMMM128,
            Opcode::VFNMADD231PS_XMM_XMM_XMMM128, Opcode::VFNMADD132PD_XMM_XMM_XMMM128,
            Opcode::VFNMADD213PD_XMM_XMM_XMMM128, Opcode::VFNMADD231PD_XMM_XMM_XMMM128,
            Opcode::VFNMADD132SS_XMM_XMM_XMMM32, Opcode::VFNMADD213SS_XMM_XMM_XMMM32,
            Opcode::VFNMADD231SS_XMM_XMM_XMMM32, Opcode::VFNMADD132SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD213SD_XMM_XMM_XMMM64, Opcode::VFNMADD231SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB132PS_XMM_XMM_XMMM128, Opcode::VFNMSUB213PS_XMM_XMM_XMMM128,
            Opcode::VFNMSUB231PS_XMM_XMM_XMMM128, Opcode::VFNMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB213PD_XMM_XMM_XMMM128, Opcode::VFNMSUB231PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB132SS_XMM_XMM_XMMM32, Opcode::VFNMSUB213SS_XMM_XMM_XMMM32,
            Opcode::VFNMSUB231SS_XMM_XMM_XMMM32, Opcode::VFNMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB213SD_XMM_XMM_XMMM64, Opcode::VFNMSUB231SD_XMM_XMM_XMMM64});

        // VEX.256 reaches this arm only through compile_vex256_lane_split,
        // which clears vex_l and re-emits the body once per 128-bit lane with
        // vec_off() naming the half. A raw VEX.L instruction is routed there at
        // the top of compile_sse, so seeing vex_l still set here means the
        // allowlist declined it -- scalar SS/SD, where VEX.L=1 is reserved.
        if (instr.prefixes.vex_l) return false;
        // Belt and braces: the scalar merge writes DEST[127:32] of xmm[dst] and
        // has no high-lane meaning, so it must never run as a lane pass.
        if (m_vec_lane_split && scalar) return false;

        const bool mem_src = !is_register_operand(op1.type);
        const u8 dst = op0.reg & 15;
        const u8 vvvv = instr.prefixes.vex_vvvv & 15;
        const u8 rm = mem_src ? 0 : (op1.reg & 15);

        // Explicit tables, not character offsets into opcode_name(): the
        // mnemonics are not a fixed width ("VFMADD" is 6 characters, "VFNMADD"
        // is 7), so indexing for the order digits silently read the wrong
        // characters for every VFNMADD/VFNMSUB form.
        const bool o132 = is_one_of({
            Opcode::VFMADD132PS_XMM_XMM_XMMM128, Opcode::VFMADD132PD_XMM_XMM_XMMM128,
            Opcode::VFMADD132SS_XMM_XMM_XMMM32, Opcode::VFMADD132SD_XMM_XMM_XMMM64,
            Opcode::VFMSUB132PS_XMM_XMM_XMMM128, Opcode::VFMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFMSUB132SS_XMM_XMM_XMMM32, Opcode::VFMSUB132SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD132PS_XMM_XMM_XMMM128, Opcode::VFNMADD132PD_XMM_XMM_XMMM128,
            Opcode::VFNMADD132SS_XMM_XMM_XMMM32, Opcode::VFNMADD132SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB132PS_XMM_XMM_XMMM128, Opcode::VFNMSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB132SS_XMM_XMM_XMMM32, Opcode::VFNMSUB132SD_XMM_XMM_XMMM64});
        const bool o213 = is_one_of({
            Opcode::VFMADD213PS_XMM_XMM_XMMM128, Opcode::VFMADD213PD_XMM_XMM_XMMM128,
            Opcode::VFMADD213SS_XMM_XMM_XMMM32, Opcode::VFMADD213SD_XMM_XMM_XMMM64,
            Opcode::VFMSUB213PS_XMM_XMM_XMMM128, Opcode::VFMSUB213PD_XMM_XMM_XMMM128,
            Opcode::VFMSUB213SS_XMM_XMM_XMMM32, Opcode::VFMSUB213SD_XMM_XMM_XMMM64,
            Opcode::VFNMADD213PS_XMM_XMM_XMMM128, Opcode::VFNMADD213PD_XMM_XMM_XMMM128,
            Opcode::VFNMADD213SS_XMM_XMM_XMMM32, Opcode::VFNMADD213SD_XMM_XMM_XMMM64,
            Opcode::VFNMSUB213PS_XMM_XMM_XMMM128, Opcode::VFNMSUB213PD_XMM_XMM_XMMM128,
            Opcode::VFNMSUB213SS_XMM_XMM_XMMM32, Opcode::VFNMSUB213SD_XMM_XMM_XMMM64});

        u8 a_reg, b_reg, c_reg;
        // Whichever of a/b/c is the r/m operand comes from memory instead of a
        // register; the other two are always registers, so at most one of these
        // is ever true.
        bool a_mem = false, b_mem = false, c_mem = false;
        if (o132)      { a_reg = dst;  b_reg = rm;   c_reg = vvvv;
                         b_mem = mem_src; }
        else if (o213) { a_reg = vvvv; b_reg = dst;  c_reg = rm;
                         c_mem = mem_src; }
        else           { a_reg = vvvv; b_reg = rm;   c_reg = dst;
                         b_mem = mem_src; }

        const u32 a_off = vec_off(a_reg);
        const u32 b_off = vec_off(b_reg);
        const u32 c_off = vec_off(c_reg);
        const u32 d_off = vec_off(dst);
        const u32 dhi_off = ymm_high_u64_off(dst, 0);
        // 16-byte staging slot for the packed memory form (see below). Every
        // reachability test happens BEFORE the first emit(): bailing out after
        // emitting would leave orphaned words in the buffer.
        const u32 scr_off = jit_vec_scratch_off();
        if ((!a_mem && !fits_ldr_q_offset(a_off)) ||
            (!b_mem && !fits_ldr_q_offset(b_off)) ||
            (!c_mem && !fits_ldr_q_offset(c_off)) ||
            !fits_ldr_q_offset(d_off) ||
            !fits_ldr_q_offset(dhi_off)) {
            return false;
        }
        if (mem_src && !scalar &&
            (!fits_ldr_q_offset(scr_off) || !fits_ldr_x_offset(scr_off) ||
             !fits_ldr_x_offset(scr_off + 8))) {
            return false;
        }

        const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;

        // ---- memory r/m source ------------------------------------------
        //
        // Ordering is forced, not stylistic: emit_helper_blr does not preserve
        // ANY vector register (V0-V7 and V16-V31 are caller-saved under
        // AAPCS64, and V8-V15 preserve only their low 64 bits, which cannot
        // hold a 128-bit lane). So every guest access must complete before the
        // first V register is populated. The packed form therefore stages both
        // qwords memory-to-memory through CpuState::sse.jit_vec_scratch rather
        // than holding the first half in a register across the second load.
        //
        // The EA is parked in X21 (REG_CALL_TMP), which is callee-saved and
        // survives the read helper's BLR, and is never narrowed: emit_compute_ea
        // forwards to emit_compute_ea64 in long mode and every marshal here is
        // MOV Xd,Xn / ADD Xd,Xn,#imm. (The recorded jit64_ea_truncation_bug
        // family is introduced by a MOV Wd,Wn on this path.)
        //
        // FMA3 memory operands carry no alignment requirement (Intel SDM Vol. 2,
        // "FMA Instruction Operand Encoding" — VEX memory operands are unaligned
        // unless the opcode says otherwise), so no jit_require_align16 here.
        const bool hold_nzcv = mem_src && should_hold_nzcv();
        if (hold_nzcv) emit_nzcv_spill();
        if (mem_src) {
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            if (scalar) {
                if (dbl) emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                else     emit_guest_load32(REG_SCRATCH0, REG_CALL_TMP);
                // Only lane 0 is read by the scalar arithmetic; the zeroing is
                // for determinism, not semantics.
                emit(arm64::eor_v16b(6, 6, 6));
                emit(arm64::ins_v_gpr(6, 0, REG_SCRATCH0,
                                      dbl ? arm64::VS_D : arm64::VS_S));
            } else {
                emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       scr_off >> 3));
                // X4 is clobbered by the region probe inside the load above, so
                // EA+8 is recomputed from the parked X21, never from a leftover.
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (scr_off + 8) >> 3));
                emit(arm64::ldr_q_uoff(6, REG_CPU_STATE, scr_off >> 4));
            }
        }
        // No guest memory access from here on, so V6 is safe to hold.
        auto load_operand = [&](u8 v, bool from_mem, u32 off) {
            if (from_mem) emit(arm64::mov_v16b(v, 6));
            else emit(arm64::ldr_q_uoff(v, REG_CPU_STATE, off >> 4));
        };
        load_operand(0, a_mem, a_off);
        load_operand(1, b_mem, b_off);
        load_operand(2, c_mem, c_off);

        if (scalar) {
            // Scalar forms leave DEST[127:32] (SS) / [127:64] (SD) untouched,
            // so compute into a temp and merge lane 0 back into the old dest.
            if (!use_fmls && !negate_acc) {
                emit(arm64::fmadd_s_scalar(4, 0, 1, 2, fs));   //  c + a*b
            } else if (!use_fmls && negate_acc) {
                emit(arm64::fnmsub_s_scalar(4, 0, 1, 2, fs));  // -c + a*b
            } else if (use_fmls && !negate_acc) {
                emit(arm64::fmsub_s_scalar(4, 0, 1, 2, fs));   //  c - a*b
            } else {
                emit(arm64::fnmadd_s_scalar(4, 0, 1, 2, fs));  // -c - a*b
            }
            emit_x86_fma_nan_fixup(4, 0, 1, 2, dbl, 3, 5, 6);
            emit(arm64::ldr_q_uoff(3, REG_CPU_STATE, d_off >> 4));
            // INS Vd.<t>[0], Vn.<t>[0] — replace only the low lane.
            emit(arm64::ins_v_elem(3, 0, 4, 0,
                                   dbl ? arm64::VS_D : arm64::VS_S));
            emit(arm64::str_q_uoff(3, REG_CPU_STATE, d_off >> 4));
        } else {
            // Original c, for the NaN fixup -- which FPCR.AH makes a no-op.
            if (!math::sse_alternate_nan_handling()) emit(arm64::mov_v16b(5, 2));
            if (negate_acc) emit(arm64::fneg_v(2, 2, fs));
            if (use_fmls) emit(arm64::fmls_v(2, 0, 1, fs));
            else emit(arm64::fmla_v(2, 0, 1, fs));
            emit_x86_fma_nan_fixup(2, 0, 1, 5, dbl, 3, 4, 7);
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, d_off >> 4));
        }

        // 128-bit VEX zeroes the destination's upper half. Under the lane
        // split BOTH lanes are written outright, and this store would zero the
        // half the second pass just computed -- the single easiest way to get
        // the 256-bit extension wrong.
        if (!m_vec_lane_split) {
            emit(arm64::eor_v16b(5, 5, 5));
            emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
        }
        if (hold_nzcv) emit_nzcv_fill();
        break;
    }

    // ---- FMA3 alternating: VFMADDSUB / VFMSUBADD -------------------------
    //
    // Intel SDM Vol. 2 VFMADDSUB: the EVEN lanes subtract and the ODD lanes
    // add; VFMSUBADD is the mirror. There is no ARM64 instruction with an
    // alternating sign, so compute both results and overwrite the even lanes
    // of one with the other. Two INS for PS (lanes 0 and 2), one for PD
    // (lane 0) -- cheaper and far easier to read than synthesising an
    // alternating sign mask, which cannot be built with a single MOVI for the
    // PD case anyway.
    case Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB231PS_XMM_XMM_XMMM128:
    case Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128:
    case Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128: {
        const Opcode oc = instr.opcode;
        auto is_one_of = [&](std::initializer_list<Opcode> l) {
            for (Opcode o : l) if (oc == o) return true;
            return false;
        };
        const bool dbl = is_one_of({
            Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128, Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128,
            Opcode::VFMADDSUB231PD_XMM_XMM_XMMM128, Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128,
            Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128, Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128});
        const bool o132 = is_one_of({
            Opcode::VFMADDSUB132PS_XMM_XMM_XMMM128, Opcode::VFMADDSUB132PD_XMM_XMM_XMMM128,
            Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128, Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128});
        const bool o213 = is_one_of({
            Opcode::VFMADDSUB213PS_XMM_XMM_XMMM128, Opcode::VFMADDSUB213PD_XMM_XMM_XMMM128,
            Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128, Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128});
        // VFMSUBADD: even lanes ADD, odd lanes SUB (the opposite of the
        // VFMADDSUB default).
        const bool even_is_add = is_one_of({
            Opcode::VFMSUBADD132PS_XMM_XMM_XMMM128, Opcode::VFMSUBADD132PD_XMM_XMM_XMMM128,
            Opcode::VFMSUBADD213PS_XMM_XMM_XMMM128, Opcode::VFMSUBADD213PD_XMM_XMM_XMMM128,
            Opcode::VFMSUBADD231PS_XMM_XMM_XMMM128, Opcode::VFMSUBADD231PD_XMM_XMM_XMMM128});

        // VEX.256 reaches this arm only through compile_vex256_lane_split,
        // which clears vex_l and re-emits it once per 128-bit lane with
        // vec_off() naming the half (Intel SDM Vol. 2 VFMADDSUB: the 256-bit
        // form alternates sub/add over all eight/four elements, i.e. the
        // 128-bit operation applied to each lane, since both lanes start on
        // an even element). A raw VEX.L instruction here means the driver was
        // bypassed.
        if (instr.prefixes.vex_l) return false;

        const bool mem_src = !is_register_operand(op1.type);
        if (mem_src && !is_memory_operand(op1.type)) return false;
        const u8 dst = op0.reg & 15;
        const u8 vvvv = instr.prefixes.vex_vvvv & 15;
        const u8 rm = mem_src ? 0 : (op1.reg & 15);

        // Operand roles are the FMA3 ones (see the VFMADD arm above); the
        // r/m operand is b for 132/231 and c for 213.
        u8 a_reg, b_reg, c_reg;
        bool b_mem = false, c_mem = false;
        if (o132)      { a_reg = dst;  b_reg = rm;   c_reg = vvvv;
                         b_mem = mem_src; }
        else if (o213) { a_reg = vvvv; b_reg = dst;  c_reg = rm;
                         c_mem = mem_src; }
        else           { a_reg = vvvv; b_reg = rm;   c_reg = dst;
                         b_mem = mem_src; }

        const u32 a_off = vec_off(a_reg);
        const u32 b_off = vec_off(b_reg);
        const u32 c_off = vec_off(c_reg);
        const u32 d_off = vec_off(dst);
        const u32 dhi_off = ymm_high_u64_off(dst, 0);
        // Staging slot for the memory operand. Every reachability test runs
        // before the first emit().
        const u32 scr_off = jit_vec_scratch_off();
        if (!fits_ldr_q_offset(a_off) ||
            (!b_mem && !fits_ldr_q_offset(b_off)) ||
            (!c_mem && !fits_ldr_q_offset(c_off)) ||
            !fits_ldr_q_offset(d_off) || !fits_ldr_q_offset(dhi_off)) {
            return false;
        }
        if (mem_src &&
            (!fits_ldr_q_offset(scr_off) || !fits_ldr_x_offset(scr_off) ||
             !fits_ldr_x_offset(scr_off + 8))) {
            return false;
        }

        // Memory r/m: both qwords are staged memory-to-CpuState before any V
        // register is populated, because the guest-load slow path BLRs a
        // helper and no V register survives a call with 128 bits intact
        // (AAPCS64). Same shape, and same X21-parked 64-bit EA, as the
        // VFMADD memory form above. FMA3 memory operands are unaligned (Intel
        // SDM Vol. 2, "FMA Instruction Operand Encoding"). The lane-split
        // driver has already added 16 to the displacement on the high lane.
        const bool hold_nzcv = mem_src && should_hold_nzcv();
        if (hold_nzcv) emit_nzcv_spill();
        if (mem_src) {
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, scr_off >> 3));
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   (scr_off + 8) >> 3));
        }
        const u32 b_src = b_mem ? scr_off : b_off;
        const u32 c_src = c_mem ? scr_off : c_off;

        const arm64::FSize fs = dbl ? arm64::FS_D : arm64::FS_S;
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, a_off >> 4));   // a
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, b_src >> 4));   // b
        emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, c_src >> 4));   // c
        if (!math::sse_alternate_nan_handling()) {
            emit(arm64::mov_v16b(5, 2));                // original c (NaN fixup)
        }

        emit(arm64::fneg_v(3, 2, fs));                  // -c
        emit(arm64::fmla_v(2, 0, 1, fs));               // V2 = a*b + c
        emit(arm64::fmla_v(3, 0, 1, fs));               // V3 = a*b - c

        // Start from whichever result the ODD lanes need, then patch the even
        // lanes in from the other.
        const u8 base = even_is_add ? 3 : 2;   // odd lanes come from here
        const u8 patch = even_is_add ? 2 : 3;  // even lanes come from here
        emit(arm64::mov_v16b(4, base));
        emit(arm64::ins_v_elem(4, 0, patch, 0, dbl ? arm64::VS_D : arm64::VS_S));
        if (!dbl) {
            emit(arm64::ins_v_elem(4, 2, patch, 2, arm64::VS_S));
        }
        // x86 NaN selection uses the ORIGINAL c (V5), not the negated
        // accumulator -- consistent with the VFMSUB lowering above.
        emit_x86_fma_nan_fixup(4, 0, 1, 5, dbl, 2, 3, 6);

        emit(arm64::str_q_uoff(4, REG_CPU_STATE, d_off >> 4));
        // 128-bit VEX zeroes YMM[255:128]; under the lane split both halves
        // are written outright and this store would destroy the high result.
        if (!m_vec_lane_split) {
            emit(arm64::eor_v16b(5, 5, 5));
            emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
        }
        if (hold_nzcv) emit_nzcv_fill();
        break;
    }

    // ---- VZEROUPPER / VZEROALL -------------------------------------------
    //
    // Intel SDM Vol. 2: VZEROUPPER zeroes YMM[255:128] for every register;
    // VZEROALL zeroes the whole of YMM0-15. Emitted at the end of essentially
    // every AVX routine, so worth not delegating.
    case Opcode::VZEROUPPER:
    case Opcode::VZEROALL: {
        const bool all = instr.opcode == Opcode::VZEROALL;
        for (u8 r = 0; r < 16; ++r) {
            if (!fits_ldr_q_offset(ymm_high_u64_off(r, 0))) return false;
            if (all && !fits_ldr_q_offset(xmm_u64_off(r, 0))) return false;
        }
        emit(arm64::eor_v16b(0, 0, 0));
        for (u8 r = 0; r < 16; ++r) {
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, ymm_high_u64_off(r, 0) >> 4));
            if (all) {
                emit(arm64::str_q_uoff(0, REG_CPU_STATE, xmm_u64_off(r, 0) >> 4));
            }
        }
        break;
    }

    // ---- Broadcasts (scalar register or memory source) --------------------
    //
    // Splat one element across the destination. ARM64 DUP is the whole
    // operation. The decoder gives the memory-source broadcasts their own
    // opcodes (VBROADCASTSS_XMM_M32 and friends, decode/opcodes/vex.cpp), so
    // they are NOT filtered out by the is_register_operand guard below —
    // they are lowered by the separate block that follows. The guard here
    // still matters for the VPBROADCAST*_*_XMMM* forms, whose single opcode
    // covers both a register and a memory source. Intel SDM Vol. 2,
    // VPBROADCAST: the low byte/word/dword/qword of the register source, or
    // the scalar memory operand, is replicated into every destination lane.
    case Opcode::VPBROADCASTB_XMM_XMMM8:
    case Opcode::VPBROADCASTB_YMM_XMMM8:
    case Opcode::VPBROADCASTW_XMM_XMMM16:
    case Opcode::VPBROADCASTW_YMM_XMMM16:
    case Opcode::VPBROADCASTD_XMM_XMMM32:
    case Opcode::VPBROADCASTD_YMM_XMMM32:
    case Opcode::VPBROADCASTQ_XMM_XMMM64:
    case Opcode::VPBROADCASTQ_YMM_XMMM64:
    case Opcode::VBROADCASTSS_XMM_XMM:
    case Opcode::VBROADCASTSS_YMM_XMM:
    case Opcode::VBROADCASTSD_YMM_XMM: {
        const Opcode oc = instr.opcode;
        const bool integer_broadcast =
            oc == Opcode::VPBROADCASTB_XMM_XMMM8 ||
            oc == Opcode::VPBROADCASTB_YMM_XMMM8 ||
            oc == Opcode::VPBROADCASTW_XMM_XMMM16 ||
            oc == Opcode::VPBROADCASTW_YMM_XMMM16 ||
            oc == Opcode::VPBROADCASTD_XMM_XMMM32 ||
            oc == Opcode::VPBROADCASTD_YMM_XMMM32 ||
            oc == Opcode::VPBROADCASTQ_XMM_XMMM64 ||
            oc == Opcode::VPBROADCASTQ_YMM_XMMM64;
        const bool memory_source = is_memory_operand(op1.type);
        if (!is_register_operand(op1.type) &&
            !(integer_broadcast && memory_source)) {
            return false;
        }
        arm64::VSize sz = arm64::VS_S;
        if (oc == Opcode::VPBROADCASTB_XMM_XMMM8 ||
            oc == Opcode::VPBROADCASTB_YMM_XMMM8) sz = arm64::VS_B;
        else if (oc == Opcode::VPBROADCASTW_XMM_XMMM16 ||
                 oc == Opcode::VPBROADCASTW_YMM_XMMM16) sz = arm64::VS_H;
        else if (oc == Opcode::VPBROADCASTQ_XMM_XMMM64 ||
                 oc == Opcode::VPBROADCASTQ_YMM_XMMM64 ||
                 oc == Opcode::VBROADCASTSD_YMM_XMM) sz = arm64::VS_D;

        const bool ymm = oc == Opcode::VPBROADCASTB_YMM_XMMM8 ||
                         oc == Opcode::VPBROADCASTW_YMM_XMMM16 ||
                         oc == Opcode::VPBROADCASTD_YMM_XMMM32 ||
                         oc == Opcode::VPBROADCASTQ_YMM_XMMM64 ||
                         oc == Opcode::VBROADCASTSS_YMM_XMM ||
                         oc == Opcode::VBROADCASTSD_YMM_XMM;

        const u8 dst = op0.reg & 15;
        const u32 d_off = xmm_u64_off(dst, 0);
        const u32 dhi_off = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(d_off) || !fits_ldr_q_offset(dhi_off)) {
            return false;
        }

        if (memory_source) {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            if (sz == arm64::VS_B) {
                emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            } else if (sz == arm64::VS_H) {
                emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            } else if (sz == arm64::VS_D) {
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            } else {
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
            }
            // Guest-load slow paths may BLR and clobber V0-V7, so form the
            // vector only after the checked load returns.
            emit(arm64::dup_v_gpr(1, REG_SCRATCH0, sz));
        } else {
            const u32 src_off = xmm_u64_off(op1.reg & 15, 0);
            if (!fits_ldr_q_offset(src_off)) return false;
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off >> 4));
            emit(arm64::dup_v_elem0(1, 0, sz));
        }
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, d_off >> 4));
        if (ymm) {
            emit(arm64::str_q_uoff(1, REG_CPU_STATE, dhi_off >> 4));
        } else {
            emit(arm64::eor_v16b(2, 2, 2));
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, dhi_off >> 4));
        }
        break;
    }

    // ---- Broadcasts (memory source) ---------------------------------------
    //
    // VBROADCASTSS/SD/F128/I128 with an m32/m64/m128 source. Splatting a
    // scalar constant out of memory is the shape of essentially every
    // vectorised loop prologue, and compile_sse is reached from 64-bit
    // blocks (instruction_compilation.cpp), so delegating these is a real
    // x64 cost.
    //
    // NOT lowered with LD1R: LD1R needs a *host* pointer in Xn, and the JIT
    // never holds one. Guest reads go through emit_guest_load32/64, which
    // probe the region slot cache, bounds-check both ends of the access and
    // fall back to a helper BLR (with emit_exit_if_pending_raise) on a miss.
    // An LD1R on a raw guest EA would be an unchecked wild host read. The
    // shape used here — guest-load into a GPR, then DUP Vd.<T>, Rn — is the
    // same one VMASKMOVPS/VPMASKMOVD uses above.
    //
    // VEX high-lane semantics: VBROADCASTSS_XMM_M32 is the only VEX.128
    // encoding here and it must zero YMM[255:128] (Intel SDM Vol. 2; see the
    // interpreter in interpreter/ops/avx_ops.cpp, which calls .clear()). It
    // gets an explicit zero store. The other four are VEX.256-only and write
    // all 32 destination bytes, so no stale upper half can survive.
    case Opcode::VBROADCASTSS_XMM_M32:
    case Opcode::VBROADCASTSS_YMM_M32:
    case Opcode::VBROADCASTSD_YMM_M64:
    case Opcode::VBROADCASTF128_YMM_M128:
    case Opcode::VBROADCASTI128_YMM_M128: {
        if (!is_memory_operand(op1.type)) return false;
        const Opcode oc = instr.opcode;

        const bool wide = oc == Opcode::VBROADCASTF128_YMM_M128 ||
                          oc == Opcode::VBROADCASTI128_YMM_M128;
        const bool qword = oc == Opcode::VBROADCASTSD_YMM_M64;
        // Every form except the VEX.128 VBROADCASTSS writes both halves.
        const bool ymm = oc != Opcode::VBROADCASTSS_XMM_M32;

        // The 128-bit forms are two 8-byte guest loads at EA and EA+8. In a
        // 32-bit block that addition would carry out of the 32-bit address
        // space instead of wrapping, which is not what the interpreter's
        // single 16-byte read does. Long mode only; 32-bit keeps the
        // delegate. (The m32/m64 forms are a single load and are fine in
        // both bitnesses.)
        if (wide && !m_block_is_64bit) return false;

        const u8 dst = op0.reg & 15;
        const u32 d_off = xmm_u64_off(dst, 0);
        const u32 dhi_off = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(d_off) || !fits_ldr_q_offset(dhi_off)) {
            return false;
        }

        if (wide) {
            if (!fits_ldr_x_offset(d_off) || !fits_ldr_x_offset(d_off + 8)) {
                return false;
            }
            // The EA must survive TWO guest loads, and a guest load can BLR
            // a helper that clobbers every caller-saved register. Park it in
            // X21 (REG_CALL_TMP, callee-saved) for the whole instruction —
            // same idiom as the VMASKMOV block above. mov_x, never mov_w:
            // a 64-bit EA through a 32-bit MOV silently drops bits 63:32 and
            // only misbehaves above 4 GB.
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));

            // Store each half to CpuState immediately: nothing may stay live
            // in a caller-saved GPR or in V0-V7 across the next guest load.
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, d_off >> 3));
            // Recompute EA+8 from the parked copy rather than incrementing a
            // register the previous load may have clobbered.
            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, 8));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                   (d_off + 8) >> 3));

            // Replicate the freshly written low lane into the high lane.
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, d_off >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, dhi_off >> 4));
            break;
        }

        // Single-element forms. emit_compute_ea forwards to
        // emit_compute_ea64 in long mode, so the address is a full canonical
        // u64 and is never truncated. REG_EA_TMP (X5) is the EA emitter's
        // internal scratch whenever the operand has an index, a >4095
        // displacement or a segment base — nothing live is held there.
        emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
        if (qword) {
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }

        // DUP after the load, never before: the load's slow path BLRs a
        // helper, which clobbers V0-V7.
        emit(arm64::dup_v_gpr(0, REG_SCRATCH0,
                              qword ? arm64::VS_D : arm64::VS_S));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_off >> 4));
        if (ymm) {
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, dhi_off >> 4));
        } else {
            // VEX.128: YMM[255:128] := 0.
            emit(arm64::eor_v16b(1, 1, 1));
            emit(arm64::str_q_uoff(1, REG_CPU_STATE, dhi_off >> 4));
        }
        break;
    }

    // ---- VEXTRACTF128 / VEXTRACTI128 / VINSERTF128 / VINSERTI128 ---------
    //
    // Pure 128-bit lane moves between an xmm and one half of a ymm; imm8[0]
    // picks the half and imm8[7:1] is ignored. The F and I forms move
    // identical bits.
    case Opcode::VEXTRACTF128_XMMM128_YMM_IMM8:
    case Opcode::VEXTRACTI128_XMMM128_YMM_IMM8: {
        // Decoder order for these is (dest = r/m, src = ModRM.reg).
        if (instr.operand_count < 3) return false;
        const u8 src = op1.reg & 15;
        const bool hi = (static_cast<u8>(instr.operands[2].imm) & 1) != 0;
        const u32 src_off = hi ? ymm_high_u64_off(src, 0) : xmm_u64_off(src, 0);
        if (!fits_ldr_q_offset(src_off)) return false;

        if (!is_register_operand(op0.type)) {
            if (!is_memory_operand(op0.type)) return false;
            // Intel SDM Vol. 2 VEXTRACTF128/VEXTRACTI128 (memory destination
            // form): DEST[127:0] := the selected lane; a 16-byte store with
            // no alignment requirement. A store denied anywhere in its 16
            // bytes performs none of itself (Vol. 3A 6.15), exactly like the
            // interpreter's single write_bytes. Both paths below are one
            // validated 16-byte write; two 8-byte stores are not.
            const bool hold_nzcv = should_hold_nzcv();
            if (hold_nzcv) emit_nzcv_spill();
            if (m_block_is_64bit && m_cfg.linear64 && m_cfg.linear64_vec128 &&
                m_linear_base_pinned) {
                emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
                emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off >> 4));
                emit_linear64_store128(0, REG_SCRATCH2);
            } else {
                emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
                emit_vec_store_span(REG_SCRATCH2, src_off, 0, 16, instr_rip);
            }
            if (hold_nzcv) emit_nzcv_fill();
            break;
        }

        const u8 dst = op0.reg & 15;
        const u32 d_off = xmm_u64_off(dst, 0);
        const u32 dhi_off = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(d_off) || !fits_ldr_q_offset(dhi_off)) {
            return false;
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off >> 4));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_off >> 4));
        // Register destination form: DEST[MAXVL-1:128] := 0.
        emit(arm64::eor_v16b(1, 1, 1));
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, dhi_off >> 4));
        break;
    }

    case Opcode::VINSERTF128_YMM_YMM_XMMM128_IMM8:
    case Opcode::VINSERTI128_YMM_YMM_XMMM128_IMM8: {
        if (instr.operand_count < 3) return false;
        const bool mem_src = !is_register_operand(op1.type);
        if (mem_src && !is_memory_operand(op1.type)) return false;
        const u8 dst = op0.reg & 15;
        const u8 src1 = instr.prefixes.vex_vvvv & 15;   // full ymm source
        const u8 src2 = mem_src ? 0 : (op1.reg & 15);   // xmm to insert
        const bool hi = (static_cast<u8>(instr.operands[2].imm) & 1) != 0;

        const u32 s1_lo = xmm_u64_off(src1, 0);
        const u32 s1_hi = ymm_high_u64_off(src1, 0);
        const u32 s2_off = xmm_u64_off(src2, 0);
        const u32 d_lo = xmm_u64_off(dst, 0);
        const u32 d_hi = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(s1_lo) || !fits_ldr_q_offset(s1_hi) ||
            !fits_ldr_q_offset(s2_off) || !fits_ldr_q_offset(d_lo) ||
            !fits_ldr_q_offset(d_hi)) {
            return false;
        }
        // Memory SRC2 is an m128 with no alignment requirement (Intel SDM
        // Vol. 2 VINSERTF128: VEX memory operands are unaligned). Both qwords
        // are read into GPRs BEFORE any vector register is formed or any
        // destination byte is stored: the guest-load slow path BLRs a helper
        // that clobbers V0-V7, and a fault on the second qword must leave
        // DEST untouched. EA lives in X21 (callee-saved); EA+8 is an ADD X,
        // never a MOV W, so a 64-bit EA is not truncated.
        const bool hold_nzcv = mem_src && should_hold_nzcv();
        if (hold_nzcv) emit_nzcv_spill();
        if (mem_src) {
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            emit_guest_load64(REG_SCRATCH0, REG_CALL_TMP);
            emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP, 8));
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH4);
        }
        // Load both halves of SRC1 before storing, so dst == src1 is safe.
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s1_lo >> 4));
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, s1_hi >> 4));
        if (mem_src) {
            emit(arm64::ins_v_gpr(2, 0, REG_CALL_TMP, arm64::VS_D));
            emit(arm64::ins_v_gpr(2, 1, REG_SCRATCH0, arm64::VS_D));
        } else {
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, s2_off >> 4));
        }
        if (hi) emit(arm64::mov_v16b(1, 2));
        else emit(arm64::mov_v16b(0, 2));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_lo >> 4));
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, d_hi >> 4));
        if (hold_nzcv) emit_nzcv_fill();
        break;
    }

    // ---- VPBLENDD ---------------------------------------------------------
    //
    // Per-dword select between SRC1 (VEX.vvvv) and SRC2 (r/m) by imm8 bit.
    // The selector is a compile-time constant, so this is a copy of SRC1 plus
    // one INS per selected lane rather than a mask-and-BSL.
    case Opcode::VPBLENDD_XMM_XMM_XMMM128_IMM8:
    case Opcode::VPBLENDD_YMM_YMM_YMMM256_IMM8: {
        if (!is_register_operand(op1.type)) return false;
        if (instr.operand_count < 3) return false;
        const bool ymm =
            instr.opcode == Opcode::VPBLENDD_YMM_YMM_YMMM256_IMM8;
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        const u8 dst = op0.reg & 15;
        const u8 s1 = instr.prefixes.vex_vvvv & 15;
        const u8 s2 = op1.reg & 15;

        const u32 s1_lo = xmm_u64_off(s1, 0), s1_hi = ymm_high_u64_off(s1, 0);
        const u32 s2_lo = xmm_u64_off(s2, 0), s2_hi = ymm_high_u64_off(s2, 0);
        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(s1_lo) || !fits_ldr_q_offset(s1_hi) ||
            !fits_ldr_q_offset(s2_lo) || !fits_ldr_q_offset(s2_hi) ||
            !fits_ldr_q_offset(d_lo) || !fits_ldr_q_offset(d_hi)) {
            return false;
        }

        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s1_lo >> 4));
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, s2_lo >> 4));
        for (u8 lane = 0; lane < 4; ++lane) {
            if (imm & (1u << lane)) {
                emit(arm64::ins_v_elem(0, lane, 1, lane, arm64::VS_S));
            }
        }
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_lo >> 4));

        if (ymm) {
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, s1_hi >> 4));
            emit(arm64::ldr_q_uoff(3, REG_CPU_STATE, s2_hi >> 4));
            for (u8 lane = 0; lane < 4; ++lane) {
                if (imm & (1u << (lane + 4))) {
                    emit(arm64::ins_v_elem(2, lane, 3, lane, arm64::VS_S));
                }
            }
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, d_hi >> 4));
        } else {
            emit(arm64::eor_v16b(4, 4, 4));
            emit(arm64::str_q_uoff(4, REG_CPU_STATE, d_hi >> 4));
        }
        break;
    }

    // ---- VPERM2F128 / VPERM2I128 -----------------------------------------
    //
    // Each output 128-bit lane picks one of four inputs (SRC1.lo, SRC1.hi,
    // SRC2.lo, SRC2.hi) via a 2-bit field, and imm8 bits 3 and 7 zero the
    // corresponding lane outright (Intel SDM Vol. 2 VPERM2F128).
    case Opcode::VPERM2F128_YMM_YMM_YMMM256_IMM8:
    case Opcode::VPERM2I128_YMM_YMM_YMMM256_IMM8: {
        if (!is_register_operand(op1.type)) return false;
        if (instr.operand_count < 3) return false;
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        const u8 dst = op0.reg & 15;
        const u8 s1 = instr.prefixes.vex_vvvv & 15;
        const u8 s2 = op1.reg & 15;

        const u32 cand[4] = {
            xmm_u64_off(s1, 0), ymm_high_u64_off(s1, 0),
            xmm_u64_off(s2, 0), ymm_high_u64_off(s2, 0),
        };
        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        for (u32 off : cand) if (!fits_ldr_q_offset(off)) return false;
        if (!fits_ldr_q_offset(d_lo) || !fits_ldr_q_offset(d_hi)) return false;

        // Both source halves are read before either destination half is
        // written, so dst aliasing src1 or src2 is safe.
        emit(arm64::eor_v16b(4, 4, 4));
        if (imm & 0x08) emit(arm64::mov_v16b(0, 4));
        else emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, cand[imm & 3] >> 4));
        if (imm & 0x80) emit(arm64::mov_v16b(1, 4));
        else emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, cand[(imm >> 4) & 3] >> 4));
        emit(arm64::str_q_uoff(0, REG_CPU_STATE, d_lo >> 4));
        emit(arm64::str_q_uoff(1, REG_CPU_STATE, d_hi >> 4));
        break;
    }

    // ---- VPERMQ / VPERMPD (immediate) ------------------------------------
    //
    // Four output qwords, each selected from the four qwords of a 256-bit
    // source by a 2-bit field of imm8. Cross-lane, but the selector is
    // constant, so it is four INS from two loaded halves.
    case Opcode::VPERMQ_YMM_YMMM256_IMM8:
    case Opcode::VPERMPD_YMM_YMMM256_IMM8: {
        if (instr.operand_count < 3) return false;
        const bool mem_src = !is_register_operand(op1.type);
        if (mem_src && !is_memory_operand(op1.type)) return false;
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        const u8 dst = op0.reg & 15;
        const u8 src = mem_src ? 0 : (op1.reg & 15);

        // A memory source is an m256 staged through the 32-byte
        // CpuState::sse.jit_vec_scratch, so these two offsets name either the
        // register's halves or the two staged halves.
        const u32 scr = jit_vec_scratch_off();
        const u32 s_lo = mem_src ? scr : xmm_u64_off(src, 0);
        const u32 s_hi = mem_src ? scr + 16 : ymm_high_u64_off(src, 0);
        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(s_lo) || !fits_ldr_q_offset(s_hi) ||
            !fits_ldr_q_offset(d_lo) || !fits_ldr_q_offset(d_hi)) {
            return false;
        }
        if (mem_src) {
            for (u32 piece = 0; piece < 4; ++piece) {
                if (!fits_ldr_x_offset(scr + piece * 8)) return false;
            }
        }
        // Intel SDM Vol. 2 VPERMQ/VPERMPD: the m256 source has no alignment
        // requirement. All four qwords are read -- memory to CpuState scratch,
        // never held in a V register across a guest load, whose slow path
        // BLRs a helper -- before either destination half is written, so a
        // fault on any piece leaves DEST untouched. EA parked in X21.
        const bool hold_nzcv = mem_src && should_hold_nzcv();
        if (hold_nzcv) emit_nzcv_spill();
        if (mem_src) {
            emit_compute_ea(REG_CALL_TMP, op1.mem, next_rip);
            for (u32 piece = 0; piece < 4; ++piece) {
                u8 addr = REG_CALL_TMP;
                if (piece != 0) {
                    emit(arm64::add_x_imm(REG_SCRATCH4, REG_CALL_TMP,
                                          piece * 8));
                    addr = REG_SCRATCH4;
                }
                emit_guest_load64(REG_SCRATCH0, addr);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (scr + piece * 8) >> 3));
            }
        }
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s_lo >> 4));  // src qwords 0,1
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, s_hi >> 4));  // src qwords 2,3
        for (u8 out = 0; out < 4; ++out) {
            const u8 sel = (imm >> (2 * out)) & 3;
            const u8 from = (sel < 2) ? 0 : 1;    // which loaded half
            const u8 lane = sel & 1;              // which qword within it
            const u8 into = (out < 2) ? 2 : 3;    // result low / high half
            emit(arm64::ins_v_elem(into, out & 1, from, lane, arm64::VS_D));
        }
        emit(arm64::str_q_uoff(2, REG_CPU_STATE, d_lo >> 4));
        emit(arm64::str_q_uoff(3, REG_CPU_STATE, d_hi >> 4));
        if (hold_nzcv) emit_nzcv_fill();
        break;
    }

    // ---- VPBLENDVB --------------------------------------------------------
    //
    // Per-byte select between SRC1 (VEX.vvvv) and SRC2 (r/m) by the sign bit of
    // the mask register, which is named in imm8[7:4]. CMLT #0 turns each mask
    // byte's sign bit into an all-ones/all-zeros byte, which is exactly what
    // BSL wants.
    // VBLENDVPS / VBLENDVPD share the encoding and the lowering; only the
    // element width fed to CMLT #0 differs (Intel SDM Vol. 2: PS selects per
    // 32-bit element, PD per 64-bit element).
    case Opcode::VPBLENDVB_XMM_XMM_XMMM128_XMM:
    case Opcode::VPBLENDVB_YMM_YMM_YMMM256_YMM:
    case Opcode::VBLENDVPS_XMM_XMM_XMMM128_XMM:
    case Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM:
    case Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM:
    case Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM: {
        if (!is_register_operand(op1.type)) return false;
        if (instr.operand_count < 3) return false;
        const bool ymm =
            instr.opcode == Opcode::VPBLENDVB_YMM_YMM_YMMM256_YMM ||
            instr.opcode == Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM ||
            instr.opcode == Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM;
        const arm64::VSize esize =
            (instr.opcode == Opcode::VBLENDVPS_XMM_XMM_XMMM128_XMM ||
             instr.opcode == Opcode::VBLENDVPS_YMM_YMM_YMMM256_YMM)
                ? arm64::VS_S
                : ((instr.opcode == Opcode::VBLENDVPD_XMM_XMM_XMMM128_XMM ||
                    instr.opcode == Opcode::VBLENDVPD_YMM_YMM_YMMM256_YMM)
                       ? arm64::VS_D
                       : arm64::VS_B);
        const u8 dst = op0.reg & 15;
        const u8 s1 = instr.prefixes.vex_vvvv & 15;
        const u8 s2 = op1.reg & 15;
        u8 msk = (static_cast<u8>(instr.operands[2].imm) >> 4) & 15;
        if (!m_block_is_64bit) msk &= 7;

        struct Half { u32 s1, s2, msk, d; };
        const Half lo{xmm_u64_off(s1, 0), xmm_u64_off(s2, 0),
                      xmm_u64_off(msk, 0), xmm_u64_off(dst, 0)};
        const Half hi{ymm_high_u64_off(s1, 0), ymm_high_u64_off(s2, 0),
                      ymm_high_u64_off(msk, 0), ymm_high_u64_off(dst, 0)};
        for (const Half& h : {lo, hi}) {
            if (!fits_ldr_q_offset(h.s1) || !fits_ldr_q_offset(h.s2) ||
                !fits_ldr_q_offset(h.msk) || !fits_ldr_q_offset(h.d)) {
                return false;
            }
        }

        auto blend = [&](const Half& h) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, h.s1 >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, h.s2 >> 4));
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, h.msk >> 4));
            emit(arm64::cmlt_v_zero(2, 2, esize));
            // BSL Vd, Vn, Vm: Vd = (Vd & Vn) | (~Vd & Vm), Vd being the mask.
            emit(arm64::bsl_v16b(2, 1, 0));
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, h.d >> 4));
        };
        blend(lo);
        if (ymm) {
            blend(hi);
        } else {
            emit(arm64::eor_v16b(3, 3, 3));
            emit(arm64::str_q_uoff(3, REG_CPU_STATE, hi.d >> 4));
        }
        break;
    }

    // ---- AVX2 variable shifts: VPSLLV / VPSRLV / VPSRAV -------------------
    //
    // Per-lane shift by a per-lane count. ARM64 USHL/SSHL do this, but the
    // shift amount is read as a SIGNED byte of each element, while x86 counts
    // are full-width unsigned: a count of 0x100 means "shift by 256" (result
    // 0) on x86 and "shift by 0" on ARM. So the count is clamped first.
    //
    // Clamping to the element width gives 0 for the logical shifts, which is
    // what x86 wants for counts >= width. VPSRAV clamps to width-1 instead,
    // because shifting a signed value right by width-1 already yields all sign
    // bits -- the answer x86 specifies for an out-of-range arithmetic count.
    case Opcode::VPSLLVD_XMM_XMM_XMMM128:
    case Opcode::VPSLLVD_YMM_YMM_YMMM256:
    case Opcode::VPSLLVQ_XMM_XMM_XMMM128:
    case Opcode::VPSLLVQ_YMM_YMM_YMMM256:
    case Opcode::VPSRLVD_XMM_XMM_XMMM128:
    case Opcode::VPSRLVD_YMM_YMM_YMMM256:
    case Opcode::VPSRLVQ_XMM_XMM_XMMM128:
    case Opcode::VPSRLVQ_YMM_YMM_YMMM256:
    case Opcode::VPSRAVD_XMM_XMM_XMMM128:
    case Opcode::VPSRAVD_YMM_YMM_YMMM256: {
        if (!is_register_operand(op1.type)) return false;
        const Opcode oc = instr.opcode;
        auto one_of = [&](std::initializer_list<Opcode> l) {
            for (Opcode o : l) if (oc == o) return true;
            return false;
        };
        const bool ymm = one_of({
            Opcode::VPSLLVD_YMM_YMM_YMMM256, Opcode::VPSLLVQ_YMM_YMM_YMMM256,
            Opcode::VPSRLVD_YMM_YMM_YMMM256, Opcode::VPSRLVQ_YMM_YMM_YMMM256,
            Opcode::VPSRAVD_YMM_YMM_YMMM256});
        const bool qword = one_of({
            Opcode::VPSLLVQ_XMM_XMM_XMMM128, Opcode::VPSLLVQ_YMM_YMM_YMMM256,
            Opcode::VPSRLVQ_XMM_XMM_XMMM128, Opcode::VPSRLVQ_YMM_YMM_YMMM256});
        const bool right = one_of({
            Opcode::VPSRLVD_XMM_XMM_XMMM128, Opcode::VPSRLVD_YMM_YMM_YMMM256,
            Opcode::VPSRLVQ_XMM_XMM_XMMM128, Opcode::VPSRLVQ_YMM_YMM_YMMM256,
            Opcode::VPSRAVD_XMM_XMM_XMMM128, Opcode::VPSRAVD_YMM_YMM_YMMM256});
        const bool arith = one_of({
            Opcode::VPSRAVD_XMM_XMM_XMMM128, Opcode::VPSRAVD_YMM_YMM_YMMM256});

        const arm64::VSize vs = qword ? arm64::VS_D : arm64::VS_S;
        const u32 width = qword ? 64u : 32u;
        const u32 clamp = arith ? (width - 1) : width;

        const u8 dst = op0.reg & 15;
        const u8 s1 = instr.prefixes.vex_vvvv & 15;
        const u8 cnt = op1.reg & 15;

        struct Half { u32 s1, cnt, d; };
        const Half lo{xmm_u64_off(s1, 0), xmm_u64_off(cnt, 0), xmm_u64_off(dst, 0)};
        const Half hi{ymm_high_u64_off(s1, 0), ymm_high_u64_off(cnt, 0),
                      ymm_high_u64_off(dst, 0)};
        for (const Half& h : {lo, hi}) {
            if (!fits_ldr_q_offset(h.s1) || !fits_ldr_q_offset(h.cnt) ||
                !fits_ldr_q_offset(h.d)) {
                return false;
            }
        }

        // Clamp constant, splatted from a GPR (MOVI cannot make an arbitrary
        // per-32/64-bit-lane immediate).
        emit_mov_imm32(REG_SCRATCH0, clamp);
        emit(arm64::dup_v_gpr(3, REG_SCRATCH0, vs));

        auto shift_half = [&](const Half& h) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, h.s1 >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, h.cnt >> 4));
            // Clamp: cnt = (cnt > clamp) ? clamp : cnt.
            //
            // Not UMIN: ARM64's UMIN/UMAX have no 64-bit-element form
            // (8B/16B/4H/8H/2S/4S only), so the VPSLLVQ/VPSRLVQ paths emitted
            // an illegal encoding and the process died with SIGILL. CMHI does
            // have a 2D form, so compare-and-select covers every width.
            emit(arm64::cmhi_v(4, 1, 3, vs));
            emit(arm64::bsl_v16b(4, 3, 1));
            emit(arm64::mov_v16b(1, 4));
            if (right) emit(arm64::neg_v(1, 1, vs));
            emit(arith ? arm64::sshl_v(0, 0, 1, vs)
                       : arm64::ushl_v(0, 0, 1, vs));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, h.d >> 4));
        };
        shift_half(lo);
        if (ymm) {
            shift_half(hi);
        } else {
            emit(arm64::eor_v16b(2, 2, 2));
            emit(arm64::str_q_uoff(2, REG_CPU_STATE, hi.d >> 4));
        }
        break;
    }

    // ---- VPERMILPS / VPERMILPD (immediate form) --------------------------
    //
    // Permute floats WITHIN each 128-bit lane — the control never crosses the
    // lane boundary, which is what separates these from VPERMPS/VPERMPD. The
    // immediate form has a compile-time selector, so it is one INS per output
    // element from a loaded copy of the source.
    //
    // VPERMILPS applies the same imm8 (2 bits per element) to both lanes.
    // VPERMILPD uses one bit per element: imm8 bits 0,1 for the low lane and
    // bits 2,3 for the high lane.
    case Opcode::VPERMILPS_IMM:
    case Opcode::VPERMILPD_IMM: {
        if (!is_register_operand(op1.type)) return false;
        if (instr.operand_count < 3) return false;
        const bool pd = instr.opcode == Opcode::VPERMILPD_IMM;
        const bool ymm = instr.prefixes.vex_l;
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        const u8 dst = op0.reg & 15;
        const u8 src = op1.reg & 15;

        const u32 s_lo = xmm_u64_off(src, 0), s_hi = ymm_high_u64_off(src, 0);
        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        if (!fits_ldr_q_offset(s_lo) || !fits_ldr_q_offset(s_hi) ||
            !fits_ldr_q_offset(d_lo) || !fits_ldr_q_offset(d_hi)) {
            return false;
        }

        // Load both source halves before writing either destination half, so
        // dst == src is safe.
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s_lo >> 4));
        if (ymm) emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, s_hi >> 4));

        // lane 0 -> V2 from V0, lane 1 -> V3 from V1.
        for (u8 lane = 0; lane < (ymm ? 2u : 1u); ++lane) {
            const u8 from = lane == 0 ? 0 : 1;
            const u8 into = lane == 0 ? 2 : 3;
            if (pd) {
                for (u8 i = 0; i < 2; ++i) {
                    const u8 sel = (imm >> (2 * lane + i)) & 1;
                    emit(arm64::ins_v_elem(into, i, from, sel, arm64::VS_D));
                }
            } else {
                for (u8 i = 0; i < 4; ++i) {
                    const u8 sel = (imm >> (2 * i)) & 3;
                    emit(arm64::ins_v_elem(into, i, from, sel, arm64::VS_S));
                }
            }
        }

        emit(arm64::str_q_uoff(2, REG_CPU_STATE, d_lo >> 4));
        if (ymm) {
            emit(arm64::str_q_uoff(3, REG_CPU_STATE, d_hi >> 4));
        } else {
            emit(arm64::eor_v16b(4, 4, 4));
            emit(arm64::str_q_uoff(4, REG_CPU_STATE, d_hi >> 4));
        }
        break;
    }

    // ---- VPERMD / VPERMPS (cross-lane, variable) -------------------------
    //
    // Eight dwords selected from a 256-bit source by eight runtime indices,
    // crossing the 128-bit lane boundary. ARM64 has no dword permute, but TBL
    // over a two-register (32-byte) table is exactly a 256-bit byte permute,
    // so the dword indices are converted to byte indices:
    //
    //   byte_idx = (index & 7) * 0x04040404 + 0x03020100
    //
    // The multiply both scales the dword index to a byte offset AND replicates
    // it across the four bytes of its dword, because (index & 7) * 4 <= 28
    // never carries between bytes. Adding 0x03020100 then walks the four bytes
    // within the selected dword.
    //
    // Indices are VEX.vvvv, source is r/m (Intel SDM Vol. 2 VPERMD).
    case Opcode::VPERMD_YMM_YMM_YMMM256:
    case Opcode::VPERMPS_YMM_YMM_YMMM256: {
        const bool memory_source = is_memory_operand(op1.type);
        if (!is_register_operand(op1.type) && !memory_source) return false;
        const u8 dst = op0.reg & 15;
        const u8 idx = instr.prefixes.vex_vvvv & 15;
        const u8 src = op1.reg & 15;

        const u32 s_lo = memory_source ? jit_vec_scratch_off()
                                      : xmm_u64_off(src, 0);
        const u32 s_hi = memory_source ? jit_vec_scratch_off() + 16
                                      : ymm_high_u64_off(src, 0);
        const u32 i_lo = xmm_u64_off(idx, 0), i_hi = ymm_high_u64_off(idx, 0);
        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        for (u32 o : {s_lo, s_hi, i_lo, i_hi, d_lo, d_hi}) {
            if (!fits_ldr_q_offset(o)) return false;
        }

        if (memory_source) {
            // A guest load may take a cold helper and clobber every
            // caller-saved GPR plus V0-V7. Park the full long-mode EA in X21
            // and stage all 32 source bytes through CpuState before creating
            // the TBL register pair. This also keeps dst==index aliasing safe.
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));
            for (u32 off = 0; off < 32; off += 8) {
                if (off == 0) {
                    emit(arm64::mov_x(REG_SCRATCH2, REG_CALL_TMP));
                } else {
                    emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, off));
                }
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                                       (s_lo + off) >> 3));
            }
        }

        // V0:V1 must be consecutive to form the two-register table.
        emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, s_lo >> 4));
        emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, s_hi >> 4));

        emit_mov_imm32(REG_SCRATCH0, 7u);
        emit(arm64::dup_v_gpr(4, REG_SCRATCH0, arm64::VS_S));       // & 7
        emit_mov_imm32(REG_SCRATCH0, 0x04040404u);
        emit(arm64::dup_v_gpr(5, REG_SCRATCH0, arm64::VS_S));       // scale+splat
        emit_mov_imm32(REG_SCRATCH0, 0x03020100u);
        emit(arm64::dup_v_gpr(6, REG_SCRATCH0, arm64::VS_S));       // byte walk

        auto permute_half = [&](u32 idx_off, u8 tmp) {
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, idx_off >> 4));
            emit(arm64::and_v16b(2, 2, 4));
            emit(arm64::mul_v(2, 2, 5, arm64::VS_S));
            emit(arm64::add_v(2, 2, 6, arm64::VS_S));
            emit(arm64::tbl_v16b_2(tmp, 0, 2));
        };
        // Both halves are permuted before either is stored, so dst aliasing
        // src or the index register is safe.
        permute_half(i_lo, 7);
        permute_half(i_hi, 3);
        emit(arm64::str_q_uoff(7, REG_CPU_STATE, d_lo >> 4));
        emit(arm64::str_q_uoff(3, REG_CPU_STATE, d_hi >> 4));
        break;
    }

    // ---- VPERMILPS / VPERMILPD (variable control) ------------------------
    //
    // Same idea as VPERMD but the control never leaves its 128-bit lane, so
    // the PS form is a one-register TBL per lane. The PD form has only two
    // possible sources per lane, so a select off the control's bit 1 is
    // cheaper than building a byte-index vector -- and ARM64's MUL has no 2D
    // form anyway, which rules out the VPERMD trick at 64-bit width.
    case Opcode::VPERMILPS_VAR:
    case Opcode::VPERMILPD_VAR: {
        if (!is_register_operand(op1.type)) return false;
        const bool pd = instr.opcode == Opcode::VPERMILPD_VAR;
        const bool ymm = instr.prefixes.vex_l;
        const u8 dst = op0.reg & 15;
        const u8 src = instr.prefixes.vex_vvvv & 15;   // permuted value
        const u8 ctl = op1.reg & 15;                   // control vector

        const u32 s_lo = xmm_u64_off(src, 0), s_hi = ymm_high_u64_off(src, 0);
        const u32 c_lo = xmm_u64_off(ctl, 0), c_hi = ymm_high_u64_off(ctl, 0);
        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        for (u32 o : {s_lo, s_hi, c_lo, c_hi, d_lo, d_hi}) {
            if (!fits_ldr_q_offset(o)) return false;
        }

        if (!pd) {
            emit_mov_imm32(REG_SCRATCH0, 3u);
            emit(arm64::dup_v_gpr(4, REG_SCRATCH0, arm64::VS_S));
            emit_mov_imm32(REG_SCRATCH0, 0x04040404u);
            emit(arm64::dup_v_gpr(5, REG_SCRATCH0, arm64::VS_S));
            emit_mov_imm32(REG_SCRATCH0, 0x03020100u);
            emit(arm64::dup_v_gpr(6, REG_SCRATCH0, arm64::VS_S));
        }

        auto do_lane = [&](u32 src_off, u32 ctl_off, u8 out) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, src_off >> 4));
            emit(arm64::ldr_q_uoff(2, REG_CPU_STATE, ctl_off >> 4));
            if (pd) {
                // Selector is bit 1 of each control qword: shift it into the
                // sign position and let CMLT build the all-ones mask.
                emit(arm64::shl_v_imm(2, 2, 62, arm64::VS_D));
                emit(arm64::cmlt_v_zero(2, 2, arm64::VS_D));
                emit(arm64::dup_v_elem0(3, 0, arm64::VS_D));      // src.D[0] to both
                emit(arm64::ins_v_elem(7, 0, 0, 1, arm64::VS_D));
                emit(arm64::ins_v_elem(7, 1, 0, 1, arm64::VS_D)); // src.D[1] both
                emit(arm64::bsl_v16b(2, 7, 3));
                emit(arm64::mov_v16b(out, 2));
            } else {
                emit(arm64::and_v16b(2, 2, 4));
                emit(arm64::mul_v(2, 2, 5, arm64::VS_S));
                emit(arm64::add_v(2, 2, 6, arm64::VS_S));
                emit(arm64::tbl_v16b_1(out, 0, 2));
            }
        };

        do_lane(s_lo, c_lo, 8);
        if (ymm) do_lane(s_hi, c_hi, 9);
        emit(arm64::str_q_uoff(8, REG_CPU_STATE, d_lo >> 4));
        if (ymm) {
            emit(arm64::str_q_uoff(9, REG_CPU_STATE, d_hi >> 4));
        } else {
            emit(arm64::eor_v16b(10, 10, 10));
            emit(arm64::str_q_uoff(10, REG_CPU_STATE, d_hi >> 4));
        }
        break;
    }

    // ---- VTESTPS / VTESTPD ------------------------------------------------
    //
    // Like PTEST but only the element SIGN bits participate:
    //   ZF := (SRC AND DEST) has no sign bit set
    //   CF := (SRC AND NOT DEST) has no sign bit set
    // OF/SF/AF/PF are cleared. That is a full write of the defined flags, so
    // it retires any pending lazy FlagOp and both overlays rather than
    // layering on them -- a partial writer that leaves a lazy op live diverges
    // only in mixed blocks, which is the worst failure shape to debug.
    //
    // CMLT #0 turns each element's sign bit into an all-ones element, and the
    // two 64-bit halves are OR-reduced into a GPR so the answer is just
    // "is this register zero".
    case Opcode::VTESTPS:
    case Opcode::VTESTPD: {
        if (!is_register_operand(op1.type)) return false;
        const bool pd = instr.opcode == Opcode::VTESTPD;
        const bool ymm = instr.prefixes.vex_l;
        const arm64::VSize vs = pd ? arm64::VS_D : arm64::VS_S;
        const u8 dst = op0.reg & 15;
        const u8 src = op1.reg & 15;

        const u32 d_lo = xmm_u64_off(dst, 0), d_hi = ymm_high_u64_off(dst, 0);
        const u32 s_lo = xmm_u64_off(src, 0), s_hi = ymm_high_u64_off(src, 0);
        for (u32 o : {d_lo, d_hi, s_lo, s_hi}) {
            if (!fits_ldr_q_offset(o)) return false;
        }

        // V4 accumulates "AND has a sign bit", V5 "AND NOT has a sign bit".
        emit(arm64::eor_v16b(4, 4, 4));
        emit(arm64::eor_v16b(5, 5, 5));
        auto accumulate = [&](u32 dof, u32 sof) {
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, dof >> 4));
            emit(arm64::ldr_q_uoff(1, REG_CPU_STATE, sof >> 4));
            emit(arm64::and_v16b(2, 1, 0));        // SRC AND DEST
            emit(arm64::bic_v16b(3, 1, 0));        // SRC AND NOT DEST
            emit(arm64::cmlt_v_zero(2, 2, vs));
            emit(arm64::cmlt_v_zero(3, 3, vs));
            emit(arm64::orr_v16b(4, 4, 2));
            emit(arm64::orr_v16b(5, 5, 3));
        };
        accumulate(d_lo, s_lo);
        if (ymm) accumulate(d_hi, s_hi);

        // Fold each 128-bit accumulator into one GPR.
        emit(arm64::umov_x_d(REG_SCRATCH0, 4, 0));
        emit(arm64::umov_x_d(REG_SCRATCH1, 4, 1));
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::umov_x_d(REG_SCRATCH2, 5, 0));
        emit(arm64::umov_x_d(REG_SCRATCH4, 5, 1));
        emit(arm64::orr_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));

        // ZF = (and == 0), CF = (andnot == 0); everything else cleared.
        emit(arm64::cmp_x_imm(REG_SCRATCH0, 0));
        emit(arm64::cset_w(REG_SCRATCH0, arm64::EQ));   // ZF
        emit(arm64::cmp_x_imm(REG_SCRATCH2, 0));
        emit(arm64::cset_w(REG_SCRATCH2, arm64::EQ));   // CF

        const u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit_mov_imm32(REG_SCRATCH1, 0x8D5u);           // OF|SF|ZF|AF|PF|CF
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH1));
        emit(arm64::orr_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH0, 6));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));

        emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(CpuState::FlagOp::None));
        emit(arm64::strb_uoff(REG_SCRATCH1, REG_CPU_STATE, flag_op_offset()));
        emit_clear_cf_overlay();
        emit(arm64::strb_uoff(31, REG_CPU_STATE, of_overlay_offset()));
        break;
    }

    // ---- VMASKMOVPS/PD and VPMASKMOVD/Q ----------------------------------
    //
    // Masked memory access, and the property that matters most is NOT the data
    // movement: Intel SDM Vol. 2 VMASKMOV specifies that elements whose mask
    // bit is clear are **not accessed at all** and raise no fault. Code relies
    // on this to walk the tail of an array where the next element would cross
    // into an unmapped page. A lowering that loaded the full 128/256 bits and
    // then blended would be correct on every value test and would fault
    // exactly where a guest expects safety.
    //
    // So this branches per element and only touches memory for enabled lanes.
    // Everything is staged through GPRs and CpuState rather than a vector
    // accumulator, because a guest access can call a helper and V0-V7 are
    // caller-saved under AAPCS -- an accumulator held in one would be
    // destroyed by the first slow-path element.
    case Opcode::VMASKMOVPS_LOAD:
    case Opcode::VMASKMOVPS_STORE:
    case Opcode::VMASKMOVPD_LOAD:
    case Opcode::VMASKMOVPD_STORE:
    case Opcode::VPMASKMOVD_LOAD:
    case Opcode::VPMASKMOVD_STORE:
    case Opcode::VPMASKMOVQ_LOAD:
    case Opcode::VPMASKMOVQ_STORE: {
        const Opcode oc = instr.opcode;
        const bool store = oc == Opcode::VMASKMOVPS_STORE ||
                           oc == Opcode::VMASKMOVPD_STORE ||
                           oc == Opcode::VPMASKMOVD_STORE ||
                           oc == Opcode::VPMASKMOVQ_STORE;
        const bool qword = oc == Opcode::VMASKMOVPD_LOAD ||
                           oc == Opcode::VMASKMOVPD_STORE ||
                           oc == Opcode::VPMASKMOVQ_LOAD ||
                           oc == Opcode::VPMASKMOVQ_STORE;
        const bool ymm = instr.prefixes.vex_l;
        const u32 esize = qword ? 8u : 4u;
        const u32 n = (ymm ? 32u : 16u) / esize;

        const Operand& mem_op = store ? op0 : op1;
        const Operand& reg_op = store ? op1 : op0;
        if (!is_memory_operand(mem_op.type)) return false;
        // emit_guest_load64/store64 are the long-mode access primitives; a
        // 32-bit guest using the qword forms keeps the delegate rather than
        // having them misapplied.
        if (qword && !m_block_is_64bit) return false;

        const u8 vec = reg_op.reg & 15;
        const u8 msk = instr.prefixes.vex_vvvv & 15;

        // Byte offset of element i inside CpuState for a given xmm register.
        auto elem_off = [&](u8 reg, u32 i) -> u32 {
            const u32 per_half = 16u / esize;
            return (i < per_half)
                ? xmm_u64_off(reg, 0) + i * esize
                : ymm_high_u64_off(reg, 0) + (i - per_half) * esize;
        };
        for (u32 i = 0; i < n; ++i) {
            const u32 vo = elem_off(vec, i), mo = elem_off(msk, i);
            if ((vo % esize) || (mo % esize)) return false;
            if ((vo / esize) > 4095 || (mo / esize) > 4095) return false;
        }

        // Base address into REG_CALL_TMP (X21, callee-saved) so it survives
        // the helper calls a guest access can make.
        if (m_block_is_64bit) emit_compute_ea64(REG_SCRATCH2, mem_op.mem, next_rip);
        else emit_compute_ea(REG_SCRATCH2, mem_op.mem, next_rip);
        emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH2));

        // The MASK may name the same register as the destination
        // (`vmaskmovps xmm0, xmm0, [ebx]` is a legal encoding). A load clears
        // the destination first, which would destroy the mask before a single
        // element is tested — every lane then read a zero mask and the whole
        // result came back zero. Stage the mask in the JIT's private 32-byte
        // slot first; it is read per element from there instead.
        //
        // The slot survives the loop: only the 128-bit cold memory stubs use
        // it, and this loop issues scalar accesses.
        const u32 scr = jit_vec_scratch_off();
        const bool mask_aliases_dst = (!store && msk == vec);
        if (mask_aliases_dst) {
            if ((scr & 15u) != 0 || (scr >> 4) >= 4096u ||
                ((scr + 16) >> 4) >= 4096u) {
                return false;
            }
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE, xmm_u64_off(msk, 0) >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, scr >> 4));
            emit(arm64::ldr_q_uoff(0, REG_CPU_STATE,
                                   ymm_high_u64_off(msk, 0) >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, (scr + 16) >> 4));
        }
        // Where element i of the mask lives: the architectural register, or
        // the staging slot when it would have been overwritten.
        auto mask_off = [&](u32 i) -> u32 {
            if (!mask_aliases_dst) return elem_off(msk, i);
            const u32 per_half = 16u / esize;
            return (i < per_half) ? scr + i * esize
                                  : scr + 16u + (i - per_half) * esize;
        };
        for (u32 i = 0; i < n; ++i) {
            const u32 mo2 = mask_off(i);
            if ((mo2 % esize) || (mo2 / esize) > 4095) return false;
        }

        // A load zeroes every masked-out element, so clear the destination
        // (including the upper half, which a 128-bit VEX form always zeroes)
        // before filling the enabled lanes.
        if (!store) {
            emit(arm64::eor_v16b(0, 0, 0));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE, xmm_u64_off(vec, 0) >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE,
                                   ymm_high_u64_off(vec, 0) >> 4));
        }

        for (u32 i = 0; i < n; ++i) {
            const u32 mo = mask_off(i);
            const u32 vo = elem_off(vec, i);

            // Test the mask element's sign bit and skip the access entirely
            // when it is clear. TBZ is emitted before the body, so
            // REG_SCRATCH0 still holds the mask when the branch executes even
            // though the body reuses it for data.
            if (qword) {
                emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, mo >> 3));
            } else {
                emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, mo >> 2));
            }
            const usize skip_pos = m_code_buf.size();
            emit(0);  // TBZ REG_SCRATCH0, #sign, skip

            emit(arm64::add_x_imm(REG_SCRATCH2, REG_CALL_TMP, i * esize));
            if (store) {
                if (qword) {
                    emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, vo >> 3));
                    emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
                } else {
                    emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, vo >> 2));
                    if (m_block_is_64bit) {
                        emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH2);
                    } else {
                        emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
                    }
                }
            } else {
                if (qword) {
                    emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                    emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, vo >> 3));
                } else {
                    if (m_block_is_64bit) {
                        emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
                    } else {
                        emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
                    }
                    emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, vo >> 2));
                }
            }

            const usize after = m_code_buf.size();
            m_code_buf[skip_pos] = arm64::tbz(
                REG_SCRATCH0, static_cast<u8>(esize * 8 - 1),
                static_cast<i32>(after - skip_pos) * 4);
        }
        break;
    }

    // ---- AVX2 gathers -----------------------------------------------------
    //
    // DEST[i] := mem[base + sign_extend(index[i]) * scale + disp] for elements
    // whose mask sign bit is set. Three contracts make this more than a loop
    // (Intel SDM Vol. 2 VGATHERDPS):
    //
    //  * Masked-out elements are NOT accessed and do not fault, exactly as for
    //    VMASKMOV — so this branches per element rather than gathering wide.
    //  * The mask register is fully zeroed on completion, and on a fault the
    //    elements already done must be visible. Clearing each mask element
    //    right after its own element gives that partial state naturally.
    //  * #UD if any pair of destination, index and mask names the same
    //    register. Those are static, so an overlap simply declines the
    //    lowering and lets the interpreter raise it.
    //
    // The index vector is an XMM register, not a GPR, so emit_compute_ea
    // cannot be used: base+disp is computed by hand into REG_CALL_TMP (X21,
    // callee-saved, survives the helper a guest access may call) and the
    // scaled index is added per element. Data is staged through GPRs and
    // CpuState for the same caller-saved-vector reason as VPMASKMOV.
    case Opcode::VGATHERDPS_XMM_VM32X_XMM:
    case Opcode::VGATHERDPS_YMM_VM32Y_YMM:
    case Opcode::VPGATHERDD_XMM_VM32X_XMM:
    case Opcode::VPGATHERDD_YMM_VM32Y_YMM:
    case Opcode::VGATHERQPS:
    case Opcode::VPGATHERQD:
    case Opcode::VGATHERDPD:
    case Opcode::VPGATHERDQ:
    case Opcode::VGATHERQPD:
    case Opcode::VPGATHERQQ: {
        const Opcode oc = instr.opcode;
        auto one_of = [&](std::initializer_list<Opcode> l) {
            for (Opcode o : l) if (oc == o) return true;
            return false;
        };
        const bool idx_q = one_of({Opcode::VGATHERQPS, Opcode::VPGATHERQD,
                                   Opcode::VGATHERQPD, Opcode::VPGATHERQQ});
        const bool elem_q = one_of({Opcode::VGATHERDPD, Opcode::VPGATHERDQ,
                                    Opcode::VGATHERQPD, Opcode::VPGATHERQQ});
        const bool ymm = instr.prefixes.vex_l;
        const u32 isize = idx_q ? 8u : 4u;
        const u32 esize = elem_q ? 8u : 4u;
        // Only the dword-index/dword-element shape scales to 8 elements.
        const u32 n = (!idx_q && !elem_q) ? (ymm ? 8u : 4u) : (ymm ? 4u : 2u);

        if (!is_memory_operand(op1.type)) return false;
        const MemoryOperand& vm = op1.mem;
        if (vm.segment != SegReg::Default) return false;   // FS/GS: delegate
        if (!vm.has_index()) return false;

        const u8 dst = op0.reg & 15;
        const u8 idx = vm.index & 15;
        const u8 msk = instr.prefixes.vex_vvvv & 15;
        // Overlap is #UD; decline so the interpreter raises it.
        if (dst == idx || dst == msk || idx == msk) return false;

        auto off_of = [&](u8 reg, u32 i, u32 size) -> u32 {
            const u32 per_half = 16u / size;
            return (i < per_half) ? xmm_u64_off(reg, 0) + i * size
                                  : ymm_high_u64_off(reg, 0) + (i - per_half) * size;
        };
        for (u32 i = 0; i < n; ++i) {
            if (off_of(dst, i, esize) / esize > 4095) return false;
            if (off_of(msk, i, esize) / esize > 4095) return false;
            if (off_of(idx, i, isize) / isize > 4095) return false;
        }
        if (!fits_ldr_q_offset(ymm_high_u64_off(dst, 0)) ||
            !fits_ldr_q_offset(ymm_high_u64_off(msk, 0))) {
            return false;
        }

        // base + disp into REG_CALL_TMP.
        if (vm.has_base()) {
            if (m_block_is_64bit) emit_load_gpr64(REG_CALL_TMP, vm.base);
            else emit_load_gpr32(REG_CALL_TMP, vm.base);
        } else {
            emit(arm64::movz_w(REG_CALL_TMP, 0));
        }
        if (vm.disp != 0) {
            emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(vm.disp));
            emit(arm64::add_x(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH0));
        }

        u8 scale_shift = 0;
        switch (vm.scale) {
        case 1: scale_shift = 0; break;
        case 2: scale_shift = 1; break;
        case 4: scale_shift = 2; break;
        case 8: scale_shift = 3; break;
        default: return false;
        }

        for (u32 i = 0; i < n; ++i) {
            const u32 mo = off_of(msk, i, esize);
            const u32 io = off_of(idx, i, isize);
            const u32 dof = off_of(dst, i, esize);

            if (elem_q) emit(arm64::ldr_x_uoff(REG_SCRATCH0, REG_CPU_STATE, mo >> 3));
            else emit(arm64::ldr_w_uoff(REG_SCRATCH0, REG_CPU_STATE, mo >> 2));
            const usize skip_pos = m_code_buf.size();
            emit(0);  // TBZ mask-sign, skip

            // Index elements are SIGN-extended before scaling.
            if (idx_q) {
                emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE, io >> 3));
            } else {
                emit(arm64::ldrsw_uoff(REG_SCRATCH1, REG_CPU_STATE, io >> 2));
            }
            emit(arm64::add_x_lsl(REG_SCRATCH2, REG_CALL_TMP, REG_SCRATCH1,
                                  scale_shift));
            if (!m_block_is_64bit) emit(arm64::mov_w(REG_SCRATCH2, REG_SCRATCH2));

            if (elem_q) {
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, dof >> 3));
            } else if (m_block_is_64bit) {
                emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, dof >> 2));
            } else {
                emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
                emit(arm64::str_w_uoff(REG_SCRATCH0, REG_CPU_STATE, dof >> 2));
            }

            const usize after = m_code_buf.size();
            m_code_buf[skip_pos] = arm64::tbz(
                REG_SCRATCH0, static_cast<u8>(esize * 8 - 1),
                static_cast<i32>(after - skip_pos) * 4);

            // Retire this mask element whether or not it was gathered, so a
            // fault on a later element leaves exactly the completed prefix.
            if (elem_q) emit(arm64::str_x_uoff(31, REG_CPU_STATE, mo >> 3));
            else emit(arm64::str_w_uoff(31, REG_CPU_STATE, mo >> 2));
        }

        // Zero everything above the gathered elements in both destination and
        // mask: a 128-bit result clears bits 255:128, and the narrow forms
        // (2 dword elements) also clear the rest of the low 128.
        const u32 produced = n * esize;
        if (produced <= 16) {
            emit(arm64::eor_v16b(0, 0, 0));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE,
                                   ymm_high_u64_off(dst, 0) >> 4));
            emit(arm64::str_q_uoff(0, REG_CPU_STATE,
                                   ymm_high_u64_off(msk, 0) >> 4));
            for (u32 b = produced; b < 16; b += 4) {
                emit(arm64::str_w_uoff(31, REG_CPU_STATE,
                                       (xmm_u64_off(dst, 0) + b) >> 2));
                emit(arm64::str_w_uoff(31, REG_CPU_STATE,
                                       (xmm_u64_off(msk, 0) + b) >> 2));
            }
        }
        break;
    }

    // VCVTPS2PH is deliberately NOT lowered here. Beyond rounding control
    // (imm8 picks the mode, or defers to MXCSR), narrowing can raise the
    // MXCSR sticky exception bits -- precision, underflow, denormal -- and the
    // interpreter already accumulates them correctly. ARM64 reports the same
    // conditions in FPSR rather than MXCSR, so a native lowering has to clear
    // FPSR, convert, read it back and merge; until it does, the delegate is
    // both correct and cheaper than getting this subtly wrong.
    // OracleAvx64.Vcvtps2phInexactSetsPe covers exactly this.

    default: return std::nullopt;
    }

    finish_vector_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit
