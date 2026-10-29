// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// VEX-encoded SSE/SSE2/SSSE3/SSE4 ops: NDS merge, 256-bit high lane, YMM zeroing
// (Intel SDM Vol. 2 AVX, Vol. 1 256-bit SIMD)

#include "exec_ops.hpp"
#include "core/cpu/decode/vex256_lane_imm.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

namespace {

void swap_ymm_lanes(CpuState& cpu, u8 reg) noexcept {
    XmmReg tmp = cpu.sse.xmm[reg];
    cpu.sse.xmm[reg] = cpu.avx.ymm_high[reg];
    cpu.avx.ymm_high[reg] = tmp;
}

// The scalar/128-only set moved to decode/opcodes.cpp as
// `vex_l_is_reserved`, so the decoder can reject a 256-bit encoding of one of
// these rather than letting it decode and fault at execution. This forwarder
// keeps the bridge's own guard in place for any path that reaches it with an
// already-decoded instruction.
bool is_avx_scalar(Opcode op) noexcept { return vex_l_is_reserved(op); }

bool is_flags_only_sse(Opcode op) noexcept { return sse_writes_only_flags(op); }

// Intel SDM Vol. 2 VCVTPD2PS / VCVTPD2DQ / VCVTTPD2DQ: the VEX.256 form reads
// four doubles (ymm2/m256) and writes four 32-bit results to the XMM
// destination -- DEST[63:0] from SRC[127:0], DEST[127:64] from SRC[255:128] --
// and zeroes DEST[255:128]. It is not a per-lane op: the upper lane's results
// land in the LOW 128 bits.
bool vex_narrow_256_to_128(Opcode op) noexcept {
    switch (op) {
        case Opcode::CVTPD2PS_XMM_XMMM128:
        case Opcode::CVTPD2DQ_XMM_XMMM128:
        case Opcode::CVTTPD2DQ_XMM_XMMM128:
            return true;
        default:
            return false;
    }
}

// Intel SDM Vol. 2 PMOVMSKB / MOVMSKPS / MOVMSKPD: VEX.256 writes the high
// lane's mask in the next bits of r32, not as a second 128-bit extract that
// replaces the low lane.
u8 vex_ymm_mask_shift(Opcode op) noexcept {
    switch (op) {
        case Opcode::PMOVMSKB_R32_XMM: return 16;
        case Opcode::MOVMSKPS_R32_XMM: return 4;
        case Opcode::MOVMSKPD_R32_XMM: return 2;
        default: return 0;
    }
}


// Intel SDM Vol. 2 VPMOVSX/VPMOVZX: the 256-bit form widens ONE source that is
// still at most 128 bits (VPMOVZXBW ymm1, xmm2/m128 ->
// "Packed_Zero_Extend_BYTE_to_WORD(DEST[127:0], SRC[63:0])" then
// "(DEST[255:128], SRC[127:64])"). The high destination lane therefore comes
// from the UPPER HALF OF THAT SAME 128-bit source -- never from the source
// register's YMM[255:128], and, for a memory source, from +half bytes, not
// +16. Returns the byte size the 128-bit form reads, i.e. that half.
u8 vex_widen_half_bytes(Opcode op) noexcept {
    switch (op) {
        case Opcode::PMOVSXBW_XMM_XMMM64:
        case Opcode::PMOVZXBW_XMM_XMMM64:
        case Opcode::PMOVSXWD_XMM_XMMM64:
        case Opcode::PMOVZXWD_XMM_XMMM64:
        case Opcode::PMOVSXDQ_XMM_XMMM64:
        case Opcode::PMOVZXDQ_XMM_XMMM64:
            return 8;
        case Opcode::PMOVSXBD_XMM_XMMM32:
        case Opcode::PMOVZXBD_XMM_XMMM32:
        case Opcode::PMOVSXWQ_XMM_XMMM32:
        case Opcode::PMOVZXWQ_XMM_XMMM32:
            return 4;
        case Opcode::PMOVSXBQ_XMM_XMMM16:
        case Opcode::PMOVZXBQ_XMM_XMMM16:
            return 2;
        // VCVTPS2PD / VCVTDQ2PD ymm1, xmm2/m128: two 32-bit elements widen to
        // each 64-bit lane, so the upper lane reads SRC[127:64] (+8 bytes).
        case Opcode::CVTPS2PD_XMM_XMMM64:
        case Opcode::CVTDQ2PD_XMM_XMMM64:
            return 8;
        default:
            return 0;
    }
}

}  // namespace

// Intel SDM Vol. 2, VPSLLW/VPSRLW/VPSRAW/VPSLLD/VPSRLD/VPSRAD/VPSLLQ/VPSRLQ/
// VPSLLDQ/VPSRLDQ with an imm8 (VEX.NDD.128.66.0F 71-73 /x ib): ModRM.reg is
// the opcode extension, so the *source* is the r/m register and **VEX.vvvv
// encodes the destination**. The legacy form shifts its one register in place,
// which would clobber the source and never write the destination.
bool is_vex_ndd_shift_imm(Opcode op) noexcept {
    switch (op) {
        case Opcode::PSLLW_XMM_IMM8:
        case Opcode::PSRLW_XMM_IMM8:
        case Opcode::PSRAW_XMM_IMM8:
        case Opcode::PSLLD_XMM_IMM8:
        case Opcode::PSRLD_XMM_IMM8:
        case Opcode::PSRAD_XMM_IMM8:
        case Opcode::PSLLQ_XMM_IMM8:
        case Opcode::PSRLQ_XMM_IMM8:
        case Opcode::PSLLDQ_XMM_IMM8:
        case Opcode::PSRLDQ_XMM_IMM8:
            return true;
        default:
            return false;
    }
}

// Intel SDM Vol. 2 VPSLLW/D/Q, VPSRLW/D/Q, VPSRAW/D with an xmm/m128 count:
// "COUNT := SRC2[63:0]" for EVERY lane of the 256-bit form. The count is a
// single 128-bit operand, so the high-lane pass must read the same low
// qword -- not the count register's YMM[255:128], and not +16 bytes of
// memory. Shared with the JIT's lane-split driver (jit/ops/sse.cpp).
bool vex_rm_is_shared_shift_count(Opcode op) noexcept {
    switch (op) {
        case Opcode::PSLLW_XMM_XMMM128:
        case Opcode::PSLLD_XMM_XMMM128:
        case Opcode::PSLLQ_XMM_XMMM128:
        case Opcode::PSRLW_XMM_XMMM128:
        case Opcode::PSRLD_XMM_XMMM128:
        case Opcode::PSRLQ_XMM_XMMM128:
        case Opcode::PSRAW_XMM_XMMM128:
        case Opcode::PSRAD_XMM_XMMM128:
            return true;
        default:
            return false;
    }
}

bool vex_sse_merges_vvvv(const DecodedInstruction& instr) noexcept {
    if (instr.operand_count < 1) return false;
    if (instr.operands[0].type != OperandType::Reg128) return false;
    if (is_flags_only_sse(instr.opcode)) return false;
    switch (instr.opcode) {
        case Opcode::MOVMSKPS_R32_XMM:
        case Opcode::MOVMSKPD_R32_XMM:
        case Opcode::EXTRACTPS_RM32_XMM_IMM8:
        case Opcode::PEXTRB_RM8_XMM_IMM8:
        case Opcode::PEXTRD_RM32_XMM_IMM8:
            return false;
        case Opcode::MOVSS_XMM_XMMM32:
        case Opcode::MOVSD_XMM_XMMM64:
            // Register form merges from vvvv; memory form is handled after SSE.
            return instr.operand_count > 1
                && is_register_operand(instr.operands[1].type);
        default:
            return true;
    }
}

bool opcode_is_vex_sse_family(Opcode op) noexcept {
    return op >= Opcode::ADDSS_XMM_XMMM32 && op < Opcode::DAA;
}

ExecResult exec_avx_sse_bridge(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    switch (instr.opcode) {
    case Opcode::MINPS_XMM_XMMM128: case Opcode::MAXPS_XMM_XMMM128:
    case Opcode::MINPD_XMM_XMMM128: case Opcode::MAXPD_XMM_XMMM128:
    case Opcode::MINSS_XMM_XMMM32: case Opcode::MAXSS_XMM_XMMM32:
    case Opcode::MINSD_XMM_XMMM64: case Opcode::MAXSD_XMM_XMMM64:
        return exec_sse_minmax(ctx,instr);
    default: break;
    }
    if (instr.prefixes.vex_l && is_avx_scalar(instr.opcode)) {
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }

    const u8 vvvv = instr.prefixes.vex_vvvv & 15;

    // Intel SDM Vol. 2B PTEST, 4-496/497: VPTEST reduces the ENTIRE
    // 128/256-bit operand to one ZF/CF pair and never modifies a vector.
    // The generic two-pass bridge overwrote the low-lane flags with the
    // high-lane flags and published them before a possible high-half fault.
    if (instr.opcode == Opcode::PTEST_XMM_XMMM128) {
        if (vvvv != 0) {
            ctx.exception = CpuException::InvalidOpcode;
            return ExecResult::Exception;
        }
        const auto& first = instr.operands[0];
        const auto& second = instr.operands[1];
        const unsigned lanes = instr.prefixes.vex_l ? 2u : 1u;
        XmmReg source[2]{};
        if (second.type == OperandType::Reg128) {
            source[0] = ctx.cpu.sse.xmm[second.reg & 15];
            if (lanes == 2) source[1] = ctx.cpu.avx.ymm_high[second.reg & 15];
        } else {
            auto address = calc_effective_address(ctx, second.mem);
            if (!address || !ctx.mem.read_bytes(address.value(),
                    reinterpret_cast<Byte*>(source), lanes * sizeof(XmmReg))) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        u64 product = 0;
        u64 complement_product = 0;
        for (unsigned lane = 0; lane < lanes; ++lane) {
            const auto& value = lane == 0 ? ctx.cpu.sse.xmm[first.reg & 15]
                                         : ctx.cpu.avx.ymm_high[first.reg & 15];
            for (unsigned word = 0; word < 2; ++word) {
                product |= source[lane].u64_[word] & value.u64_[word];
                complement_product |= source[lane].u64_[word] & ~value.u64_[word];
            }
        }
        ctx.cpu.materialize_lazy_flags();
        ctx.cpu.flags.set_zf(product == 0);
        ctx.cpu.flags.set_cf(complement_product == 0);
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_sf(false);
        ctx.cpu.flags.set_af(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }
    u8 dst = 255;
    if (instr.operand_count > 0 && instr.operands[0].type == OperandType::Reg128) {
        dst = instr.operands[0].reg & 15;
    }
    u8 src = 255;
    if (instr.operand_count > 1 && instr.operands[1].type == OperandType::Reg128) {
        src = instr.operands[1].reg & 15;
    }

    // A faulting VEX.256 memory read must not publish either destination
    // lane, including the source merge performed before legacy execution:
    // the two passes below read the m256 as two m128 halves, and a fault in
    // the high half would otherwise leave the low half's result committed.
    // Hardware never writes DEST on a faulting read. The JIT lane-split
    // driver probes the whole operand before its first pass for the same
    // reason (jit/ops/sse.cpp compile_vex256_lane_split).
    struct Vex256DestinationRestore {
        CpuState& cpu;
        u8 reg;
        bool active;
        XmmReg low;
        XmmReg high;
        ~Vex256DestinationRestore() {
            if (active) {
                cpu.sse.xmm[reg] = low;
                cpu.avx.ymm_high[reg] = high;
            }
        }
    } restore{ctx.cpu, dst,
              instr.prefixes.vex_l && dst != 255
                  && instr.operand_count > 1
                  && is_memory_operand(instr.operands[1].type),
              dst != 255 ? ctx.cpu.sse.xmm[dst] : XmmReg{},
              dst != 255 ? ctx.cpu.avx.ymm_high[dst] : XmmReg{}};

    const XmmReg saved_vvvv = ctx.cpu.sse.xmm[vvvv];

    // Clearing `prefixes.vex` below is what makes each legacy handler run in
    // legacy mode, and it also hides the one thing those handlers still have
    // to know: a VEX form carries NO 16-byte alignment requirement on its
    // memory operand (Intel SDM Vol. 2 -- unaligned unless the opcode says
    // otherwise), while the legacy spelling of the very same opcode does.
    //
    // Scoped over the WHOLE bridge rather than one call: it drives up to four
    // legacy executions, including the +16 high lane of a 256-bit form, and
    // the high lane is exactly the one a narrower wrap would miss. Saved and
    // restored because a delegate can nest.
    struct VexOriginScope {
        ExecContext& ctx;
        bool saved;
        explicit VexOriginScope(ExecContext& c) noexcept
            : ctx(c), saved(c.vex_origin) { c.vex_origin = true; }
        ~VexOriginScope() { ctx.vex_origin = saved; }
    } vex_origin_scope{ctx};

    DecodedInstruction sse = instr;

    // NDD shift-by-immediate: retarget the in-place legacy shift at vvvv and
    // seed it with the r/m register, so the source register is left unchanged.
    const bool ndd = is_vex_ndd_shift_imm(instr.opcode) && dst != 255;
    if (ndd) {
        const u8 src_reg = dst;
        dst = vvvv;
        sse.operands[0].reg = vvvv;
        ctx.cpu.sse.xmm[vvvv] = ctx.cpu.sse.xmm[src_reg];
        if (instr.prefixes.vex_l) {
            ctx.cpu.avx.ymm_high[vvvv] = ctx.cpu.avx.ymm_high[src_reg];
        }
        // The retargeted legacy shift only touches operands[0]; the source
        // register is read once here and never written.
        src = 255;
    }

    // VEX.NDS with the destination ALSO naming the r/m source (`vpand ymm2,
    // ymm1, ymm2`): the merge copy below would overwrite the second source
    // with SRC1 before the legacy two-operand form ever reads it, silently
    // computing `SRC1 op SRC1`. Stage the second source in a register that is
    // neither the destination nor vvvv, retarget the operand at it, and put it
    // back afterwards. 16 registers and two exclusions always leave one free.
    u8 scratch = 255;
    XmmReg scratch_saved_lo{};
    XmmReg scratch_saved_hi{};
    const bool merges = !ndd && vex_sse_merges_vvvv(instr) && dst != 255;
    if (merges && src != 255 && src == dst && dst != vvvv) {
        for (u8 r = 0; r < 16; ++r) {
            if (r != dst && r != vvvv) { scratch = r; break; }
        }
        scratch_saved_lo = ctx.cpu.sse.xmm[scratch];
        scratch_saved_hi = ctx.cpu.avx.ymm_high[scratch];
        ctx.cpu.sse.xmm[scratch] = ctx.cpu.sse.xmm[src];
        ctx.cpu.avx.ymm_high[scratch] = ctx.cpu.avx.ymm_high[src];
        sse.operands[1].reg = scratch;
        src = scratch;
    }
    // Restores the staged register on every exit path, including the early
    // returns inside the 256-bit lane handling below.
    struct ScratchRestore {
        CpuState& cpu;
        u8 reg;
        const XmmReg& lo;
        const XmmReg& hi;
        ~ScratchRestore() {
            if (reg == 255) return;
            cpu.sse.xmm[reg] = lo;
            cpu.avx.ymm_high[reg] = hi;
        }
    } scratch_restore{ctx.cpu, scratch, scratch_saved_lo, scratch_saved_hi};

    if (merges) {
        ctx.cpu.sse.xmm[dst] = ctx.cpu.sse.xmm[vvvv];
        if (instr.prefixes.vex_l) {
            ctx.cpu.avx.ymm_high[dst] = ctx.cpu.avx.ymm_high[vvvv];
        }
    }

    // Captured before the legacy pass, which writes the destination and so may
    // clobber the source register when they are the same.
    const u8 widen_half =
        instr.prefixes.vex_l ? vex_widen_half_bytes(instr.opcode) : u8{0};
    const XmmReg widen_src =
        (widen_half != 0 && src != 255) ? ctx.cpu.sse.xmm[src] : XmmReg{};

    // Intel SDM Vol. 2 VMOVAPS/VMOVAPD/VMOVDQA (load and store) and
    // VMOVNTPS/VMOVNTPD/VMOVNTDQ: the 256-bit form takes a 32-byte-aligned
    // m256 and raises #GP(0) otherwise (exception type 1). The two 16-byte
    // legacy passes below each check only 16, so an m256 that is 16- but not
    // 32-aligned used to pass here while the JIT's wide path faults.
    if (instr.prefixes.vex_l) {
        bool aligned_move = false;
        switch (instr.opcode) {
            case Opcode::MOVAPS_XMM_XMMM128: case Opcode::MOVAPS_XMMM128_XMM:
            case Opcode::MOVAPD_XMM_XMMM128: case Opcode::MOVAPD_XMMM128_XMM:
            case Opcode::MOVDQA_XMM_XMMM128: case Opcode::MOVDQA_XMMM128_XMM:
            case Opcode::MOVNTPS_M128_XMM:   case Opcode::MOVNTPD_M128_XMM:
            case Opcode::MOVNTDQ_M128_XMM:
                aligned_move = true;
                break;
            default:
                break;
        }
        for (u8 i = 0; aligned_move && i < instr.operand_count; ++i) {
            if (!is_memory_operand(instr.operands[i].type)) continue;
            auto addr = calc_effective_address(ctx, instr.operands[i].mem);
            if (!addr || (addr.value() & 31u) != 0) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
    }

    // A 256-bit memory destination is written by two 16-byte legacy passes
    // below. A store denied in its upper half must not leave the lower half
    // written: Windows reports the first inaccessible byte and changes none
    // of the 32 (tools/oracle_probe/simd_store_fault_probe.expected).
    if (instr.prefixes.vex_l && instr.operand_count > 0 &&
        is_memory_operand(instr.operands[0].type)) {
        auto addr = calc_effective_address(ctx, instr.operands[0].mem);
        if (!addr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        if (!ctx.mem.is_range_writable(addr.value(), 32, ctx.cpu.is_64bit())) {
            record_write_access_violation(ctx, addr.value(), 32);
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
    }

    // A shift count that is also the destination is overwritten by the low
    // pass below; the high pass needs the ORIGINAL count, captured here.
    const bool shared_count = vex_rm_is_shared_shift_count(instr.opcode);
    const XmmReg shared_count_saved =
        (shared_count && src != 255) ? ctx.cpu.sse.xmm[src] : XmmReg{};

    sse.prefixes.vex = false;
    sse.prefixes.vex_l = false;
    ExecResult r = execute_instruction(ctx, sse);
    if (r != ExecResult::Continue) return r;

    // Intel SDM VMOVSS/VMOVSD from memory: merge [127:32]/[127:64] from vvvv.
    if (instr.opcode == Opcode::MOVSS_XMM_XMMM32 && dst != 255
        && instr.operand_count > 1 && is_memory_operand(instr.operands[1].type)) {
        ctx.cpu.sse.xmm[dst].u32_[1] = saved_vvvv.u32_[1];
        ctx.cpu.sse.xmm[dst].u32_[2] = saved_vvvv.u32_[2];
        ctx.cpu.sse.xmm[dst].u32_[3] = saved_vvvv.u32_[3];
    }
    if (instr.opcode == Opcode::MOVSD_XMM_XMMM64 && dst != 255
        && instr.operand_count > 1 && is_memory_operand(instr.operands[1].type)) {
        ctx.cpu.sse.xmm[dst].u64_[1] = saved_vvvv.u64_[1];
    }

    if (widen_half != 0) {
        DecodedInstruction hi = sse;
        u8 stage = 255;
        XmmReg stage_saved{};
        if (src != 255) {
            // Stage SRC shifted down by `widen_half` bytes in a register that
            // is neither the destination nor the source, so the legacy
            // 128-bit form reads the source's upper half.
            for (u8 rg = 0; rg < 16; ++rg) {
                if (rg != dst && rg != src) { stage = rg; break; }
            }
            stage_saved = ctx.cpu.sse.xmm[stage];
            XmmReg shifted{};
            for (u8 i = 0; i + widen_half < 16; ++i) {
                shifted.u8_[i] = widen_src.u8_[i + widen_half];
            }
            ctx.cpu.sse.xmm[stage] = shifted;
            hi.operands[1].reg = stage;
        } else if (instr.operand_count > 1
                   && is_memory_operand(hi.operands[1].type)) {
            hi.operands[1].mem.disp += widen_half;
        }
        if (dst != 255) swap_ymm_lanes(ctx.cpu, dst);
        r = execute_instruction(ctx, hi);
        if (dst != 255) swap_ymm_lanes(ctx.cpu, dst);
        if (stage != 255) ctx.cpu.sse.xmm[stage] = stage_saved;
        if (r != ExecResult::Continue) return r;
        restore.active = false;
        return ExecResult::Continue;
    }

    if (instr.prefixes.vex_l && vex_narrow_256_to_128(instr.opcode) && dst != 255) {
        const u64 low_result = ctx.cpu.sse.xmm[dst].u64_[0];
        DecodedInstruction hi = sse;
        u8 stage = 255;
        XmmReg stage_saved{};
        if (src != 255) {
            // The low pass may already have overwritten a source that is also
            // the destination; its upper lane is untouched, so stage that in
            // a register that is neither.
            for (u8 rg = 0; rg < 16; ++rg) {
                if (rg != dst && rg != src) { stage = rg; break; }
            }
            stage_saved = ctx.cpu.sse.xmm[stage];
            ctx.cpu.sse.xmm[stage] = ctx.cpu.avx.ymm_high[src];
            hi.operands[1].reg = stage;
        } else if (instr.operand_count > 1 && is_memory_operand(hi.operands[1].type)) {
            hi.operands[1].mem.disp += 16;
        }
        r = execute_instruction(ctx, hi);
        const u64 high_result = ctx.cpu.sse.xmm[dst].u64_[0];
        if (stage != 255) ctx.cpu.sse.xmm[stage] = stage_saved;
        if (r != ExecResult::Continue) return r;
        ctx.cpu.sse.xmm[dst].u64_[0] = low_result;
        ctx.cpu.sse.xmm[dst].u64_[1] = high_result;
        ctx.cpu.avx.ymm_high[dst].clear();
        restore.active = false;
        return ExecResult::Continue;
    }

    if (instr.prefixes.vex_l) {
        const u8 mask_shift = vex_ymm_mask_shift(instr.opcode);
        if (mask_shift != 0 && src != 255 && instr.operand_count > 0
            && (instr.operands[0].type == OperandType::Reg32
                || instr.operands[0].type == OperandType::Reg64)) {
            const OperandType dest_ty = instr.operands[0].type;
            const u8 dest_reg = instr.operands[0].reg;
            const u64 low_mask = read_register(ctx, dest_ty, dest_reg);
            swap_ymm_lanes(ctx.cpu, src);
            r = execute_instruction(ctx, sse);
            const u64 high_mask = read_register(ctx, dest_ty, dest_reg);
            swap_ymm_lanes(ctx.cpu, src);
            if (r != ExecResult::Continue) return r;
            write_register(ctx, dest_ty, dest_reg,
                           low_mask | (high_mask << mask_shift));
            return ExecResult::Continue;
        }
        DecodedInstruction hi = sse;
        for (u8 i = 0; i < hi.operand_count; ++i) {
            if (hi.operands[i].type == OperandType::Imm8) {
                hi.operands[i].imm = cpu::vex256_high_lane_imm(
                    hi.opcode, static_cast<u8>(hi.operands[i].imm));
            }
        }
        // A shift count is one 128-bit operand for both lanes: neither the
        // count register's upper lane nor the next 16 bytes of memory are
        // the high pass's count. Swapping the count register's lanes here
        // zeroed YMM[255:128] of `vpslld ymm0, ymm1, xmm2` whenever
        // ymm2[255:128] held anything >= the lane width.
        if (instr.operand_count > 0 && is_memory_operand(hi.operands[0].type)) {
            hi.operands[0].mem.disp += 16;
        }
        if (!shared_count && instr.operand_count > 1 &&
            is_memory_operand(hi.operands[1].type)) {
            hi.operands[1].mem.disp += 16;
        }
        // The count register may also be the destination (`vpslld ymm0,
        // ymm1, xmm0`): the destination swap below then moves the count's
        // lanes too, so stage the low count in a free register for the pass.
        u8 count_stage = 255;
        XmmReg count_stage_saved{};
        if (shared_count && src != 255 && src == dst) {
            for (u8 rg = 0; rg < 16; ++rg) {
                if (rg != dst && rg != vvvv) { count_stage = rg; break; }
            }
            count_stage_saved = ctx.cpu.sse.xmm[count_stage];
            ctx.cpu.sse.xmm[count_stage] = shared_count_saved;
            hi.operands[1].reg = count_stage;
        }
        if (dst != 255) swap_ymm_lanes(ctx.cpu, dst);
        if (!shared_count && src != 255 && src != dst) swap_ymm_lanes(ctx.cpu, src);
        r = execute_instruction(ctx, hi);
        if (!shared_count && src != 255 && src != dst) swap_ymm_lanes(ctx.cpu, src);
        if (dst != 255) swap_ymm_lanes(ctx.cpu, dst);
        if (count_stage != 255) ctx.cpu.sse.xmm[count_stage] = count_stage_saved;
        if (r != ExecResult::Continue) return r;
    } else if (dst != 255 && !is_flags_only_sse(instr.opcode)) {
        ctx.cpu.avx.ymm_high[dst].clear();
    }

    restore.active = false;
    return ExecResult::Continue;
}

}  // namespace vitreo::cpu::ops
