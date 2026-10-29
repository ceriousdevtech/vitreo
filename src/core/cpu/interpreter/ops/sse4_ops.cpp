// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE4.1/SSE4.2 instruction execution (CPU-041)

#include "exec_ops.hpp"
#include "helpers.hpp"
#include <cstring>
#include <cmath>

#if defined(__aarch64__)
#include <arm_acle.h>
#endif

namespace vitreo::cpu::ops {

// ============================================================================
// SSE register / operand helpers (same pattern as crypto_ops)
// ============================================================================

static XmmReg& xmm_ref(ExecContext& ctx, u8 index) noexcept {
    return ctx.cpu.sse.xmm[index & 15];
}

static Result<XmmReg, Error> read_pack(ExecContext& ctx,
                                        const Operand& op,
                                        bool aligned_form = true) noexcept {
    if (op.type == OperandType::Reg128) {
        return Result<XmmReg, Error>::ok(ctx.cpu.sse.xmm[op.reg & 15]);
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<XmmReg, Error>::err(addr.error());
    // Legacy SSE4 forms with a FULL 128-bit memory operand require 16-byte
    // alignment, exactly as the SSE2 packed forms do -- the rule belongs to
    // the operand, not the opcode. Deliberately keyed on Mem128 so the
    // narrow-source PMOVZX*/PMOVSX* forms below (m64/m32/m16) are untouched:
    // they have no such requirement and must not start faulting.
    if (aligned_form && op.type == OperandType::Mem128 &&
        (addr.value() & 0xF) != 0) {
        ctx.exception = CpuException::GeneralProtection;
        return Result<XmmReg, Error>::err(Error::InvalidArgument);
    }
    // Read exactly the operand's memory width, not a blanket 16 bytes. The
    // narrow-source SSE4 ops (PMOVZX*/PMOVSX* with m64/m32/m16 operands) must
    // not touch the extra high bytes: doing so faults when the operand sits at
    // the tail of a mapped section (observed as an AV on a RIP-relative
    // pmovzxdq GHASH constant load in a browser library). Unread high bytes stay zero.
    usize n = operand_size(op.type);
    if (n == 0 || n > 16) n = 16;
    XmmReg tmp{};
    auto r = ctx.mem.read_bytes(addr.value(),
                                reinterpret_cast<Byte*>(&tmp), n);
    if (!r) return Result<XmmReg, Error>::err(r.error());
    return Result<XmmReg, Error>::ok(tmp);
}

static Result<u32, Error> read_rm32(ExecContext& ctx,
                                     const Operand& op) noexcept {
    if (is_register_operand(op.type)) {
        return Result<u32, Error>::ok(ctx.cpu.gpr.get32(op.reg));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u32, Error>::err(addr.error());
    auto r = ctx.mem.read_u32(addr.value());
    if (!r) return Result<u32, Error>::err(r.error());
    return Result<u32, Error>::ok(r.value());
}

static Result<u64, Error> read_rm64(ExecContext& ctx,
                                    const Operand& op) noexcept {
    if (is_register_operand(op.type)) {
        return Result<u64, Error>::ok(ctx.cpu.gpr.get64(op.reg));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u64, Error>::err(addr.error());
    auto r = ctx.mem.read_u64(addr.value());
    if (!r) return Result<u64, Error>::err(r.error());
    return Result<u64, Error>::ok(r.value());
}

static Result<u8, Error> read_rm8(ExecContext& ctx, const Operand& op,
                                  bool has_rex) noexcept {
    if (is_register_operand(op.type)) {
        if (!has_rex && op.reg >= 4 && op.reg <= 7) {
            return Result<u8, Error>::ok(ctx.cpu.gpr.get8h(op.reg - 4));
        }
        return Result<u8, Error>::ok(
            ctx.cpu.gpr.get8l(op.reg));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u8, Error>::err(addr.error());
    auto r = ctx.mem.read_u8(addr.value());
    if (!r) return Result<u8, Error>::err(r.error());
    return Result<u8, Error>::ok(r.value());
}

static Result<u16, Error> read_rm16(ExecContext& ctx,
                                     const Operand& op) noexcept {
    if (is_register_operand(op.type)) {
        return Result<u16, Error>::ok(
            static_cast<u16>(ctx.cpu.gpr.get32(op.reg)));
    }
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return Result<u16, Error>::err(addr.error());
    auto r = ctx.mem.read_u16(addr.value());
    if (!r) return Result<u16, Error>::err(r.error());
    return Result<u16, Error>::ok(r.value());
}

#define GP_FAULT { ctx.exception = CpuException::GeneralProtection; \
                   return ExecResult::Exception; }

// ============================================================================
// CRC32 lookup table (iSCSI polynomial 0x1EDC6F41, reflected)
// ============================================================================
namespace {
#if !defined(__aarch64__)
constexpr u32 crc32c_table[256] = {
    0x00000000,0xF26B8303,0xE13B70F7,0x1350F3F4,
    0xC79A971F,0x35F1141C,0x26A1E7E8,0xD4CA64EB,
    0x8AD958CF,0x78B2DBCC,0x6BE22838,0x9989AB3B,
    0x4D43CFD0,0xBF284CD3,0xAC78BF27,0x5E133C24,
    0x105EC76F,0xE235446C,0xF165B798,0x030E349B,
    0xD7C45070,0x25AFD373,0x36FF2087,0xC494A384,
    0x9A879FA0,0x68EC1CA3,0x7BBCEF57,0x89D76C54,
    0x5D1D08BF,0xAF768BBC,0xBC267848,0x4E4DFB4B,
    0x20BD8EDE,0xD2D60DDD,0xC186FE29,0x33ED7D2A,
    0xE72719C1,0x154C9AC2,0x061C6936,0xF477EA35,
    0xAA64D611,0x580F5512,0x4B5FA6E6,0xB93425E5,
    0x6DFE410E,0x9F95C20D,0x8CC531F9,0x7EAEB2FA,
    0x30E349B1,0xC288CAB2,0xD1D83946,0x23B3BA45,
    0xF779DEAE,0x05125DAD,0x1642AE59,0xE4292D5A,
    0xBA3A117E,0x4851927D,0x5B016189,0xA96AE28A,
    0x7DA08661,0x8FCB0562,0x9C9BF696,0x6EF07595,
    0x417B1DBC,0xB3109EBF,0xA0406D4B,0x522BEE48,
    0x86E18AA3,0x748A09A0,0x67DAFA54,0x95B17957,
    0xCBA24573,0x39C9C670,0x2A993584,0xD8F2B687,
    0x0C38D26C,0xFE53516F,0xED03A29B,0x1F682198,
    0x5125DAD3,0xA34E59D0,0xB01EAA24,0x42752927,
    0x96BF4DCC,0x64D4CECF,0x77843D3B,0x85EFBE38,
    0xDBFC821C,0x2997011F,0x3AC7F2EB,0xC8AC71E8,
    0x1C661503,0xEE0D9600,0xFD5D65F4,0x0F36E6F7,
    0x61C69362,0x93AD1061,0x80FDE395,0x72966096,
    0xA65C047D,0x5437877E,0x4767748A,0xB50CF789,
    0xEB1FCBAD,0x197448AE,0x0A24BB5A,0xF84F3859,
    0x2C855CB2,0xDEEEDFB1,0xCDBE2C45,0x3FD5AF46,
    0x7198540D,0x83F3D70E,0x90A324FA,0x62C8A7F9,
    0xB602C312,0x44694011,0x5739B3E5,0xA55230E6,
    0xFB410CC2,0x092A8FC1,0x1A7A7C35,0xE811FF36,
    0x3CDB9BDD,0xCEB018DE,0xDDE0EB2A,0x2F8B6829,
    0x82F63B78,0x709DB87B,0x63CD4B8F,0x91A6C88C,
    0x456CAC67,0xB7072F64,0xA457DC90,0x563C5F93,
    0x082F63B7,0xFA44E0B4,0xE9141340,0x1B7F9043,
    0xCFB5F4A8,0x3DDE77AB,0x2E8E845F,0xDCE5075C,
    0x92A8FC17,0x60C37F14,0x73938CE0,0x81F80FE3,
    0x55326B08,0xA759E80B,0xB4091BFF,0x466298FC,
    0x1871A4D8,0xEA1A27DB,0xF94AD42F,0x0B21572C,
    0xDFEB33C7,0x2D80B0C4,0x3ED04330,0xCCBBC033,
    0xA24BB5A6,0x502036A5,0x4370C551,0xB11B4652,
    0x65D122B9,0x97BAA1BA,0x84EA524E,0x7681D14D,
    0x2892ED69,0xDAF96E6A,0xC9A99D9E,0x3BC21E9D,
    0xEF087A76,0x1D63F975,0x0E330A81,0xFC588982,
    0xB21572C9,0x407EF1CA,0x532E023E,0xA145813D,
    0x758FE5D6,0x87E466D5,0x94B49521,0x66DF1622,
    0x38CC2A06,0xCAA7A905,0xD9F75AF1,0x2B9CD9F2,
    0xFF56BD19,0x0D3D3E1A,0x1E6DCDEE,0xEC064EED,
    0xC38D26C4,0x31E6A5C7,0x22B65633,0xD0DDD530,
    0x0417B1DB,0xF67C32D8,0xE52CC12C,0x1747422F,
    0x49547E0B,0xBB3FFD08,0xA86F0EFC,0x5A048DFF,
    0x8ECEE914,0x7CA56A17,0x6FF599E3,0x9D9E1AE0,
    0xD3D3E1AB,0x21B862A8,0x32E8915C,0xC083125F,
    0x144976B4,0xE622F5B7,0xF5720643,0x07198540,
    0x590AB964,0xAB613A67,0xB831C993,0x4A5A4A90,
    0x9E902E7B,0x6CFBAD78,0x7FAB5E8C,0x8DC0DD8F,
    0xE330A81A,0x115B2B19,0x020BD8ED,0xF0605BEE,
    0x24AA3F05,0xD6C1BC06,0xC5914FF2,0x37FACCF1,
    0x69E9F0D5,0x9B8273D6,0x88D28022,0x7AB90321,
    0xAE7367CA,0x5C18E4C9,0x4F48173D,0xBD23943E,
    0xF36E6F75,0x0105EC76,0x12551F82,0xE03E9C81,
    0x34F4F86A,0xC69F7B69,0xD5CF889D,0x27A40B9E,
    0x79B737BA,0x8BDCB4B9,0x988C474D,0x6AE7C44E,
    0xBE2DA0A5,0x4C4623A6,0x5F16D052,0xAD7D5351,
};

u32 crc32c_u8(u32 crc, u8 data) noexcept {
    return crc32c_table[(crc ^ data) & 0xFF] ^ (crc >> 8);
}

u32 crc32c_u32(u32 crc, u32 data) noexcept {
    crc = crc32c_u8(crc, static_cast<u8>(data));
    crc = crc32c_u8(crc, static_cast<u8>(data >> 8));
    crc = crc32c_u8(crc, static_cast<u8>(data >> 16));
    crc = crc32c_u8(crc, static_cast<u8>(data >> 24));
    return crc;
}

u32 crc32c_u64(u32 crc, u64 data) noexcept {
    crc = crc32c_u32(crc, static_cast<u32>(data));
    return crc32c_u32(crc, static_cast<u32>(data >> 32));
}
#else
// SSE4.2 CRC32 uses the same reflected Castagnoli polynomial as ARMv8's
// CRC32C instructions. Apple Silicon guarantees the CRC extension, so the
// interpreter can delegate the primitive while retaining the exact x86
// accumulator, byte order, and flag behavior.
__attribute__((target("crc")))
u32 crc32c_u8(u32 crc, u8 data) noexcept {
    return __crc32cb(crc, data);
}

__attribute__((target("crc")))
u32 crc32c_u16(u32 crc, u16 data) noexcept {
    return __crc32ch(crc, data);
}

__attribute__((target("crc")))
u32 crc32c_u32(u32 crc, u32 data) noexcept {
    return __crc32cw(crc, data);
}

__attribute__((target("crc")))
u32 crc32c_u64(u32 crc, u64 data) noexcept {
    return __crc32cd(crc, data);
}
#endif

// Shared aggregation + polarity for PCMP{I,E}STR{I,M}. Returns IntRes2.
// len1/len2 must already be clamped to [0, elem_count].
u16 pcmpxstrx_aggregate(const XmmReg& src1, const XmmReg& src2,
                        int len1, int len2, u8 imm) noexcept {
    bool is_word   = (imm & 0x01) != 0;
    bool is_signed = (imm & 0x02) != 0;
    int elem_count = is_word ? 8 : 16;

    auto get1 = [&](int i) -> i32 {
        if (is_word) return is_signed ? (i32)(i16)src1.u16_[i]
                                      : (i32)(u16)src1.u16_[i];
        return is_signed ? (i32)(i8)src1.u8_[i] : (i32)(u8)src1.u8_[i];
    };
    auto get2 = [&](int i) -> i32 {
        if (is_word) return is_signed ? (i32)(i16)src2.u16_[i]
                                      : (i32)(u16)src2.u16_[i];
        return is_signed ? (i32)(i8)src2.u8_[i] : (i32)(u8)src2.u8_[i];
    };
    auto eq_elem = [&](int i, int j) -> bool {
        if (is_word) return src1.u16_[i] == src2.u16_[j];
        return src1.u8_[i] == src2.u8_[j];
    };

    u16 int_res2 = 0;
    u8 agg = (imm >> 2) & 3;
    if (agg == 0) {
        // Equal any
        for (int j = 0; j < elem_count; ++j) {
            if (j >= len2) continue;
            for (int i = 0; i < elem_count; ++i) {
                if (i >= len1) continue;
                if (eq_elem(i, j)) { int_res2 |= (1 << j); break; }
            }
        }
    } else if (agg == 1) {
        // Ranges
        for (int j = 0; j < elem_count; ++j) {
            if (j >= len2) continue;
            i32 v = get2(j);
            for (int i = 0; i + 1 < len1; i += 2) {
                i32 lo = get1(i);
                i32 hi = get1(i + 1);
                if (v >= lo && v <= hi) { int_res2 |= (1 << j); break; }
            }
        }
    } else if (agg == 2) {
        // Equal each (SDM Table 4-7)
        for (int i = 0; i < elem_count; ++i) {
            bool v1 = (i < len1);
            bool v2 = (i < len2);
            if (!v1 && !v2) {
                int_res2 |= (1 << i);
            } else if (v1 && v2) {
                if (eq_elem(i, i)) int_res2 |= (1 << i);
            }
        }
    } else {
        // Equal ordered (substring search).
        // Per hardware behavior (verified on Intel):
        // For each start j, AND over i in [0, elem_count) of cell[j][i]:
        //   k = j + i
        //   if k >= elem_count    -> 1 (off-grid, accept)
        //   else if i >= len1     -> 1 (pattern exhausted, accept)
        //   else if k >= len2     -> 0 (text past-null in-grid, pattern still
        //                              wants a byte: reject)
        //   else                  -> compare
        // The crucial distinction: "invalid text" behaves differently when it's
        // past-null-within-vector vs past-vector-end.
        for (int j = 0; j < elem_count; ++j) {
            bool match = true;
            for (int i = 0; i < elem_count; ++i) {
                int k = j + i;
                if (k >= elem_count) break;      // off-grid: accept rest
                if (i >= len1) break;            // pattern exhausted: accept rest
                if (k >= len2) { match = false; break; }  // past-null
                if (!eq_elem(i, k)) { match = false; break; }
            }
            if (match) int_res2 |= (1 << j);
        }
    }

    u16 elem_mask = static_cast<u16>((1u << elem_count) - 1u);
    u8 pol = (imm >> 4) & 3;
    if (pol == 1) {
        int_res2 = static_cast<u16>(~int_res2) & elem_mask;
    } else if (pol == 3) {
        u16 valid_mask =
            (len2 >= elem_count) ? elem_mask
                                 : static_cast<u16>((1u << len2) - 1u);
        int_res2 ^= valid_mask;
        int_res2 &= elem_mask;
    } else {
        int_res2 &= elem_mask;
    }
    return int_res2;
}

} // anonymous namespace

// ============================================================================
// SSE4.1/SSE4.2 dispatch
// ============================================================================

ExecResult exec_sse4_ops(ExecContext& ctx,
                          const DecodedInstruction& instr) noexcept {
    // VEX encodings carry no alignment requirement; `ctx.vex_origin` catches
    // the ones the AVX bridge re-dispatches here with prefixes.vex cleared.
    bool aligned_form = !instr.prefixes.vex && !ctx.vex_origin;
    switch (instr.opcode) {
    // Unaligned BY DESIGN -- see the note beside the matching exclusion in
    // JitCompiler::emit_m128_align_check_if_required. String scans use
    // PCMPISTRI on arbitrary pointers, so these forms are unaligned.
    case Opcode::PCMPISTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPISTRM_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRM_XMM_XMMM128_IMM8:
        aligned_form = false;
        break;
    default:
        break;
    }
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ========================================================================
    // PTEST — set ZF if (src AND dst)==0, CF if (NOT dst AND src)==0
    // ========================================================================
    case Opcode::PTEST_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();

        u64 and0 = dst.u64_[0] & src.u64_[0];
        u64 and1 = dst.u64_[1] & src.u64_[1];
        u64 andn0 = (~dst.u64_[0]) & src.u64_[0];
        u64 andn1 = (~dst.u64_[1]) & src.u64_[1];

        ctx.cpu.flags.set_zf((and0 == 0 && and1 == 0));
        ctx.cpu.flags.set_cf((andn0 == 0 && andn1 == 0));
        ctx.cpu.flags.set_of(false);
        ctx.cpu.flags.set_sf(false);
        ctx.cpu.flags.set_af(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPEQQ — packed compare equal qword
    // ========================================================================
    case Opcode::PCMPEQQ_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 2; ++i)
            dst.u64_[i] = (dst.u64_[i] == src.u64_[i]) ? ~0ULL : 0ULL;
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPGTQ — packed compare greater than qword (signed)
    // ========================================================================
    case Opcode::PCMPGTQ_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 2; ++i) {
            i64 a = static_cast<i64>(dst.u64_[i]);
            i64 b = static_cast<i64>(src.u64_[i]);
            dst.u64_[i] = (a > b) ? ~0ULL : 0ULL;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PACKUSDW — pack dwords to unsigned words with saturation
    // ========================================================================
    case Opcode::PACKUSDW_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        XmmReg result{};
        for (int i = 0; i < 4; ++i) {
            i32 v = static_cast<i32>(dst.u32_[i]);
            result.u16_[i] = (v < 0) ? 0 :
                              (v > 0xFFFF) ? 0xFFFF :
                              static_cast<u16>(v);
        }
        for (int i = 0; i < 4; ++i) {
            i32 v = static_cast<i32>(src.u32_[i]);
            result.u16_[i + 4] = (v < 0) ? 0 :
                                  (v > 0xFFFF) ? 0xFFFF :
                                  static_cast<u16>(v);
        }
        dst = result;
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVNTDQA — non-temporal aligned load (behaves as normal load)
    // ========================================================================
    case Opcode::MOVNTDQA_XMM_M128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        xmm_ref(ctx, op0.reg) = src_r.value();
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMOVSX* — packed sign-extend
    // ========================================================================
    case Opcode::PMOVSXBW_XMM_XMMM64: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 8; ++i)
            dst.i16_[i] = static_cast<i16>(static_cast<i8>(src.u8_[i]));
        return ExecResult::Continue;
    }
    case Opcode::PMOVSXBD_XMM_XMMM32: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 4; ++i)
            dst.i32_[i] = static_cast<i32>(static_cast<i8>(src.u8_[i]));
        return ExecResult::Continue;
    }
    case Opcode::PMOVSXBQ_XMM_XMMM16: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 2; ++i)
            dst.i64_[i] = static_cast<i64>(static_cast<i8>(src.u8_[i]));
        return ExecResult::Continue;
    }
    case Opcode::PMOVSXWD_XMM_XMMM64: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 4; ++i)
            dst.i32_[i] = static_cast<i32>(static_cast<i16>(src.u16_[i]));
        return ExecResult::Continue;
    }
    case Opcode::PMOVSXWQ_XMM_XMMM32: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 2; ++i)
            dst.i64_[i] = static_cast<i64>(static_cast<i16>(src.u16_[i]));
        return ExecResult::Continue;
    }
    case Opcode::PMOVSXDQ_XMM_XMMM64: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 2; ++i)
            dst.i64_[i] = static_cast<i64>(static_cast<i32>(src.u32_[i]));
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMOVZX* — packed zero-extend
    // ========================================================================
    case Opcode::PMOVZXBW_XMM_XMMM64: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 8; ++i)
            dst.u16_[i] = static_cast<u16>(src.u8_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PMOVZXBD_XMM_XMMM32: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = static_cast<u32>(src.u8_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PMOVZXBQ_XMM_XMMM16: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        dst.u64_[0] = static_cast<u64>(src.u8_[0]);
        dst.u64_[1] = static_cast<u64>(src.u8_[1]);
        return ExecResult::Continue;
    }
    case Opcode::PMOVZXWD_XMM_XMMM64: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = static_cast<u32>(src.u16_[i]);
        return ExecResult::Continue;
    }
    case Opcode::PMOVZXWQ_XMM_XMMM32: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        dst.u64_[0] = static_cast<u64>(src.u16_[0]);
        dst.u64_[1] = static_cast<u64>(src.u16_[1]);
        return ExecResult::Continue;
    }
    case Opcode::PMOVZXDQ_XMM_XMMM64: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        dst.u64_[0] = static_cast<u64>(src.u32_[0]);
        dst.u64_[1] = static_cast<u64>(src.u32_[1]);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMIN/PMAX signed/unsigned byte/word/dword variants
    // ========================================================================
    case Opcode::PMINSB_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 16; ++i) {
            i8 a = static_cast<i8>(dst.u8_[i]);
            i8 b = static_cast<i8>(src.u8_[i]);
            dst.u8_[i] = static_cast<u8>(a < b ? a : b);
        }
        return ExecResult::Continue;
    }
    case Opcode::PMAXSB_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 16; ++i) {
            i8 a = static_cast<i8>(dst.u8_[i]);
            i8 b = static_cast<i8>(src.u8_[i]);
            dst.u8_[i] = static_cast<u8>(a > b ? a : b);
        }
        return ExecResult::Continue;
    }
    case Opcode::PMINSD_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 4; ++i) {
            i32 a = static_cast<i32>(dst.u32_[i]);
            i32 b = static_cast<i32>(src.u32_[i]);
            dst.u32_[i] = static_cast<u32>(a < b ? a : b);
        }
        return ExecResult::Continue;
    }
    case Opcode::PMAXSD_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 4; ++i) {
            i32 a = static_cast<i32>(dst.u32_[i]);
            i32 b = static_cast<i32>(src.u32_[i]);
            dst.u32_[i] = static_cast<u32>(a > b ? a : b);
        }
        return ExecResult::Continue;
    }
    case Opcode::PMINUW_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 8; ++i)
            dst.u16_[i] = dst.u16_[i] < src.u16_[i] ? dst.u16_[i] : src.u16_[i];
        return ExecResult::Continue;
    }
    case Opcode::PMAXUW_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 8; ++i)
            dst.u16_[i] = dst.u16_[i] > src.u16_[i] ? dst.u16_[i] : src.u16_[i];
        return ExecResult::Continue;
    }
    case Opcode::PMINUD_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = dst.u32_[i] < src.u32_[i] ? dst.u32_[i] : src.u32_[i];
        return ExecResult::Continue;
    }
    case Opcode::PMAXUD_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        for (int i = 0; i < 4; ++i)
            dst.u32_[i] = dst.u32_[i] > src.u32_[i] ? dst.u32_[i] : src.u32_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // PHMINPOSUW — horizontal minimum unsigned word + position
    // ========================================================================
    case Opcode::PHMINPOSUW_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        XmmReg src = src_r.value();
        auto& dst = xmm_ref(ctx, op0.reg);
        u16 min_val = src.u16_[0];
        u16 min_idx = 0;
        for (int i = 1; i < 8; ++i) {
            if (src.u16_[i] < min_val) {
                min_val = src.u16_[i];
                min_idx = static_cast<u16>(i);
            }
        }
        dst.clear();
        dst.u16_[0] = min_val;
        dst.u16_[1] = min_idx;
        return ExecResult::Continue;
    }

    // ========================================================================
    // Variable blend — PBLENDVB/BLENDVPS/BLENDVPD (implicit XMM0 mask)
    // ========================================================================
    case Opcode::PBLENDVB_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        const auto& mask = ctx.cpu.sse.xmm[0];
        for (int i = 0; i < 16; ++i)
            if (mask.u8_[i] & 0x80) dst.u8_[i] = src.u8_[i];
        return ExecResult::Continue;
    }
    case Opcode::BLENDVPS_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        const auto& mask = ctx.cpu.sse.xmm[0];
        for (int i = 0; i < 4; ++i)
            if (mask.u32_[i] & 0x80000000) dst.u32_[i] = src.u32_[i];
        return ExecResult::Continue;
    }
    case Opcode::BLENDVPD_XMM_XMMM128: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        const auto& mask = ctx.cpu.sse.xmm[0];
        for (int i = 0; i < 2; ++i)
            if (mask.u64_[i] & 0x8000000000000000ULL)
                dst.u64_[i] = src.u64_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // Immediate blend — BLENDPS/BLENDPD/PBLENDW
    // ========================================================================
    case Opcode::BLENDPS_XMM_XMMM128_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        for (int i = 0; i < 4; ++i)
            if (imm & (1 << i)) dst.u32_[i] = src.u32_[i];
        return ExecResult::Continue;
    }
    case Opcode::BLENDPD_XMM_XMMM128_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        for (int i = 0; i < 2; ++i)
            if (imm & (1 << i)) dst.u64_[i] = src.u64_[i];
        return ExecResult::Continue;
    }
    case Opcode::PBLENDW_XMM_XMMM128_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        for (int i = 0; i < 8; ++i)
            if (imm & (1 << i)) dst.u16_[i] = src.u16_[i];
        return ExecResult::Continue;
    }

    // ========================================================================
    // ROUND* — rounding (imm8 selects mode)
    // ========================================================================
    case Opcode::ROUNDPS_XMM_XMMM128_IMM8:
    case Opcode::ROUNDPD_XMM_XMMM128_IMM8:
    case Opcode::ROUNDSS_XMM_XMMM32_IMM8:
    case Opcode::ROUNDSD_XMM_XMMM64_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        // Intel SDM Vol. 2 ROUNDPS: imm[2]=1 → MXCSR.RC, else imm[1:0].
        const u8 rc = (imm & 4u)
                          ? static_cast<u8>((ctx.cpu.sse.mxcsr >> 13) & 3u)
                          : static_cast<u8>(imm & 3u);
        // Mode 00 is round-to-nearest-EVEN whatever MXCSR.RC says (only
        // imm8[2]=1 consults RC, and then rc already holds it). nearbyint
        // follows the host's current rounding mode, which is MXCSR.RC once
        // SSE rounding is installed, so it rounded mode 00 the wrong way
        // whenever RC was not nearest. roundeven is the mode itself.
        auto round_f = [rc](float v) {
            switch (rc) {
            case 0: return __builtin_roundevenf(v);
            case 1: return std::floor(v);
            case 2: return std::ceil(v);
            default: return std::trunc(v);
            }
        };
        auto round_d = [rc](double v) {
            switch (rc) {
            case 0: return __builtin_roundeven(v);
            case 1: return std::floor(v);
            case 2: return std::ceil(v);
            default: return std::trunc(v);
            }
        };
        bool inexact = false;
        bool invalid = false;
        auto note = [&](bool finite, bool changed) {
            if (finite && changed) inexact = true;
        };
        // An SNaN source raises #I (sticky MXCSR.IE when masked) and is
        // returned quietened; a QNaN raises nothing. Measured on an
        // i9-12900KF for every imm8 and both widths, legacy and VEX
        // (tools/oracle_probe/vex_scalar_round_probe.expected: in=7f800001
        // -> out=7fc00001 flags=1; in=7fc00000 -> flags=0). imm8[3]
        // suppresses only the precision exception, not this one.
        auto snan32 = [](u32 bits) {
            return (bits & 0x7FC00000u) == 0x7F800000u && (bits & 0x003FFFFFu) != 0;
        };
        auto snan64 = [](u64 bits) {
            return (bits & 0x7FF8000000000000ull) == 0x7FF0000000000000ull &&
                   (bits & 0x0007FFFFFFFFFFFFull) != 0;
        };
        if (instr.opcode == Opcode::ROUNDPS_XMM_XMMM128_IMM8) {
            for (int i = 0; i < 4; ++i) {
                float v;
                std::memcpy(&v, &src.u32_[i], 4);
                if (snan32(src.u32_[i])) invalid = true;
                const float r = round_f(v);
                note(std::isfinite(v), r != v);
                std::memcpy(&dst.u32_[i], &r, 4);
            }
        } else if (instr.opcode == Opcode::ROUNDPD_XMM_XMMM128_IMM8) {
            for (int i = 0; i < 2; ++i) {
                double v;
                std::memcpy(&v, &src.u64_[i], 8);
                if (snan64(src.u64_[i])) invalid = true;
                const double r = round_d(v);
                note(std::isfinite(v), r != v);
                std::memcpy(&dst.u64_[i], &r, 8);
            }
        } else if (instr.opcode == Opcode::ROUNDSS_XMM_XMMM32_IMM8) {
            float v;
            std::memcpy(&v, &src.u32_[0], 4);
            if (snan32(src.u32_[0])) invalid = true;
            const float r = round_f(v);
            note(std::isfinite(v), r != v);
            std::memcpy(&dst.u32_[0], &r, 4);
        } else {
            double v;
            std::memcpy(&v, &src.u64_[0], 8);
            if (snan64(src.u64_[0])) invalid = true;
            const double r = round_d(v);
            note(std::isfinite(v), r != v);
            std::memcpy(&dst.u64_[0], &r, 8);
        }
        // Precision is signaled only if imm[3]=0 (SDM ROUNDPS). Masked PE
        // still sets the sticky MXCSR flag; unmasked #XM is not raised here.
        if ((imm & 8u) == 0 && inexact) {
            ctx.cpu.sse.mxcsr |= MXCSR_PE;
        }
        if (invalid) {
            ctx.cpu.sse.mxcsr |= MXCSR_IE;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PINSRB — insert byte from r/m into XMM at position
    // ========================================================================
    case Opcode::PINSRB_XMM_RM8_IMM8: {
        auto val_r = read_rm8(ctx, op1, instr.prefixes.has_rex());
        if (!val_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        u8 pos = static_cast<u8>(instr.operands[2].imm) & 0xF;
        dst.u8_[pos] = val_r.value();
        return ExecResult::Continue;
    }

    // ========================================================================
    // PEXTRB — extract byte from XMM to r/m
    // ========================================================================
    case Opcode::PEXTRB_RM8_XMM_IMM8: {
        auto& src = xmm_ref(ctx, op1.reg);
        u8 pos = static_cast<u8>(instr.operands[2].imm) & 0xF;
        u8 val = src.u8_[pos];
        if (is_register_operand(op0.type)) {
            ctx.cpu.gpr.set32(op0.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, op0.mem);
            if (!addr) GP_FAULT;
            auto wr = ctx.mem.write_u8(addr.value(), val);
            if (!wr) GP_FAULT;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // EXTRACTPS — extract float from XMM to r/m32
    // ========================================================================
    case Opcode::EXTRACTPS_RM32_XMM_IMM8: {
        auto& src = xmm_ref(ctx, op1.reg);
        u8 pos = static_cast<u8>(instr.operands[2].imm) & 3;
        u32 val = src.u32_[pos];
        if (is_register_operand(op0.type)) {
            ctx.cpu.gpr.set32(op0.reg, val);
        } else {
            auto addr = calc_effective_address(ctx, op0.mem);
            if (!addr) GP_FAULT;
            auto wr = ctx.mem.write_u32(addr.value(), val);
            if (!wr) GP_FAULT;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // INSERTPS — insert float from XMM/m32 into XMM with zero mask
    // ========================================================================
    case Opcode::INSERTPS_XMM_XMMM32_IMM8: {
        auto& dst = xmm_ref(ctx, op0.reg);
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 count_s = (imm >> 6) & 3;
        u8 count_d = (imm >> 4) & 3;
        u8 zmask = imm & 0xF;
        u32 src_val;
        if (op1.type == OperandType::Reg128) {
            src_val = xmm_ref(ctx, op1.reg).u32_[count_s];
        } else {
            auto addr = calc_effective_address(ctx, op1.mem);
            if (!addr) GP_FAULT;
            auto r = ctx.mem.read_u32(addr.value());
            if (!r) GP_FAULT;
            src_val = r.value();
        }
        dst.u32_[count_d] = src_val;
        for (int i = 0; i < 4; ++i)
            if (zmask & (1 << i)) dst.u32_[i] = 0;
        return ExecResult::Continue;
    }

    // ========================================================================
    // DPPS — dot product of packed single-precision
    // ========================================================================
    case Opcode::DPPS_XMM_XMMM128_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        // Per Intel SDM: per-lane gated product then tree reduction
        //   tmp2 = tmp1[0] + tmp1[1]; tmp3 = tmp1[2] + tmp1[3]; sum = tmp2+tmp3.
        float t[4];
        for (int i = 0; i < 4; ++i) {
            if (imm & (1 << (i + 4))) {
                float a, b;
                std::memcpy(&a, &dst.u32_[i], 4);
                std::memcpy(&b, &src.u32_[i], 4);
                t[i] = a * b;
            } else {
                t[i] = 0.0f;
            }
        }
        float sum = (t[0] + t[1]) + (t[2] + t[3]);
        for (int i = 0; i < 4; ++i) {
            if (imm & (1 << i))
                std::memcpy(&dst.u32_[i], &sum, 4);
            else
                dst.u32_[i] = 0;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // DPPD — dot product of packed double-precision
    // ========================================================================
    case Opcode::DPPD_XMM_XMMM128_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        double t[2];
        for (int i = 0; i < 2; ++i) {
            if (imm & (1 << (i + 4))) {
                double a, b;
                std::memcpy(&a, &dst.u64_[i], 8);
                std::memcpy(&b, &src.u64_[i], 8);
                t[i] = a * b;
            } else {
                t[i] = 0.0;
            }
        }
        double sum = t[0] + t[1];
        for (int i = 0; i < 2; ++i) {
            if (imm & (1 << i))
                std::memcpy(&dst.u64_[i], &sum, 8);
            else
                dst.u64_[i] = 0;
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // MPSADBW — multiple packed sums of absolute difference
    // ========================================================================
    case Opcode::MPSADBW_XMM_XMMM128_IMM8: {
        auto src_r = read_pack(ctx, op1, aligned_form);
        if (!src_r) GP_FAULT;
        auto& dst = xmm_ref(ctx, op0.reg);
        XmmReg src = src_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        u8 src_off = (imm & 3) * 4;
        u8 dst_off = ((imm >> 2) & 1) * 4;
        XmmReg result{};
        for (int i = 0; i < 8; ++i) {
            u16 sad = 0;
            for (int j = 0; j < 4; ++j) {
                int d = static_cast<int>(dst.u8_[dst_off + i + j]) -
                        static_cast<int>(src.u8_[src_off + j]);
                sad += static_cast<u16>(d < 0 ? -d : d);
            }
            result.u16_[i] = sad;
        }
        dst = result;
        return ExecResult::Continue;
    }

    // ========================================================================
    // CRC32 — SSE4.2 CRC32C
    // ========================================================================
    case Opcode::CRC32_R32_RM8: {
        auto val_r = read_rm8(ctx, op1, instr.prefixes.has_rex());
        if (!val_r) GP_FAULT;
        u32 crc = ctx.cpu.gpr.get32(op0.reg);
        crc = crc32c_u8(crc, val_r.value());
        ctx.cpu.gpr.set32(op0.reg, crc);
        return ExecResult::Continue;
    }
    case Opcode::CRC32_R64_RM8: {
        auto val_r = read_rm8(ctx, op1, instr.prefixes.has_rex());
        if (!val_r) GP_FAULT;
        const u32 crc = crc32c_u8(
            ctx.cpu.gpr.get32(op0.reg), val_r.value());
        // Intel SDM CRC32: 8-bit source, 64-bit dest — DEST[63-32] := 0.
        ctx.cpu.gpr.set64(op0.reg, crc);
        return ExecResult::Continue;
    }
    case Opcode::CRC32_R32_RM32: {
        auto val_r = read_rm32(ctx, op1);
        if (!val_r) GP_FAULT;
        u32 crc = ctx.cpu.gpr.get32(op0.reg);
        crc = crc32c_u32(crc, val_r.value());
        ctx.cpu.gpr.set32(op0.reg, crc);
        return ExecResult::Continue;
    }
    case Opcode::CRC32_R32_RM16: {
        auto val_r = read_rm16(ctx, op1);
        if (!val_r) GP_FAULT;
        u32 crc = ctx.cpu.gpr.get32(op0.reg);
        u16 data = val_r.value();
#if defined(__aarch64__)
        crc = crc32c_u16(crc, data);
#else
        crc = crc32c_u8(crc, static_cast<u8>(data));
        crc = crc32c_u8(crc, static_cast<u8>(data >> 8));
#endif
        ctx.cpu.gpr.set32(op0.reg, crc);
        return ExecResult::Continue;
    }
    case Opcode::CRC32_R64_RM64: {
        auto val_r = read_rm64(ctx, op1);
        if (!val_r) GP_FAULT;
        const u32 crc = crc32c_u64(
            ctx.cpu.gpr.get32(op0.reg), val_r.value());
        // Intel SDM CRC32: the 32-bit result zero-extends in r64.
        ctx.cpu.gpr.set64(op0.reg, crc);
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVBE — byte-swap load/store (memory required; #UD if both ops are regs)
    // ========================================================================
    case Opcode::MOVBE_R32_M32: {
        if (!is_memory_operand(op1.type)) GP_FAULT;
        auto addr = calc_effective_address(ctx, op1.mem);
        if (!addr) GP_FAULT;
        auto v = ctx.mem.read_u32(addr.value());
        if (!v) GP_FAULT;
        u32 val = v.value();
        val = __builtin_bswap32(val);
        ctx.cpu.gpr.set32(op0.reg, val);
        return ExecResult::Continue;
    }
    case Opcode::MOVBE_R16_M16: {
        if (!is_memory_operand(op1.type)) GP_FAULT;
        auto addr = calc_effective_address(ctx, op1.mem);
        if (!addr) GP_FAULT;
        auto v = ctx.mem.read_u16(addr.value());
        if (!v) GP_FAULT;
        u16 val = v.value();
        val = __builtin_bswap16(val);
        // A 16-bit destination writes ONLY bits 15:0 — bits 63:16 are
        // preserved, with no zero-extension (Intel SDM Vol. 1 3.4.1.1 applies
        // the extension to 32-bit operands only). Doing this as a read-modify
        // through set32 kept bits 31:16 but still cleared 63:32, so a 64-bit
        // guest saw the top half of the register vanish. Confirmed against
        // real x86: RAX 0xDEADBEEFDEADBEEF keeps 0xDEADBEEFDEAD in the high
        // 48 bits.
        ctx.cpu.gpr.set16(op0.reg, val);
        return ExecResult::Continue;
    }
    case Opcode::MOVBE_M32_R32: {
        if (!is_memory_operand(op0.type)) GP_FAULT;
        auto addr = calc_effective_address(ctx, op0.mem);
        if (!addr) GP_FAULT;
        u32 val = ctx.cpu.gpr.get32(op1.reg);
        val = __builtin_bswap32(val);
        auto r = ctx.mem.write_u32(addr.value(), val);
        if (!r) GP_FAULT;
        return ExecResult::Continue;
    }
    case Opcode::MOVBE_M16_R16: {
        if (!is_memory_operand(op0.type)) GP_FAULT;
        auto addr = calc_effective_address(ctx, op0.mem);
        if (!addr) GP_FAULT;
        u16 val = static_cast<u16>(ctx.cpu.gpr.get32(op1.reg));
        val = __builtin_bswap16(val);
        auto r = ctx.mem.write_u16(addr.value(), val);
        if (!r) GP_FAULT;
        return ExecResult::Continue;
    }
    case Opcode::MOVBE_R64_M64: {
        if (!is_memory_operand(op1.type)) GP_FAULT;
        auto addr = calc_effective_address(ctx, op1.mem);
        if (!addr) GP_FAULT;
        auto v = ctx.mem.read_u64(addr.value());
        if (!v) GP_FAULT;
        ctx.cpu.gpr.set64(op0.reg, __builtin_bswap64(v.value()));
        return ExecResult::Continue;
    }
    case Opcode::MOVBE_M64_R64: {
        if (!is_memory_operand(op0.type)) GP_FAULT;
        auto addr = calc_effective_address(ctx, op0.mem);
        if (!addr) GP_FAULT;
        auto r = ctx.mem.write_u64(addr.value(), __builtin_bswap64(ctx.cpu.gpr.get64(op1.reg)));
        if (!r) GP_FAULT;
        return ExecResult::Continue;
    }

    // ========================================================================
    // SSE4.2 PCMP*STR* — packed compare implicit/explicit length strings
    // Simplified implementation: sets ECX/XMM0 + flags
    // ========================================================================
    case Opcode::PCMPISTRI_XMM_XMMM128_IMM8: {
        auto src2_r = read_pack(ctx, op1, aligned_form);
        if (!src2_r) GP_FAULT;
        auto& src1 = xmm_ref(ctx, op0.reg);
        XmmReg src2 = src2_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        // imm[0]: 0=byte, 1=word
        // imm[1]: 0=unsigned, 1=signed (affects only Ranges aggregation)
        bool is_word   = (imm & 0x01) != 0;
        bool is_signed = (imm & 0x02) != 0;
        int elem_count = is_word ? 8 : 16;

        // Helper lambdas: read element as i32 signed or unsigned
        auto get1 = [&](int i) -> i32 {
            if (is_word) return is_signed ? (i32)(i16)src1.u16_[i]
                                          : (i32)(u16)src1.u16_[i];
            return is_signed ? (i32)(i8)src1.u8_[i] : (i32)(u8)src1.u8_[i];
        };
        auto get2 = [&](int i) -> i32 {
            if (is_word) return is_signed ? (i32)(i16)src2.u16_[i]
                                          : (i32)(u16)src2.u16_[i];
            return is_signed ? (i32)(i8)src2.u8_[i] : (i32)(u8)src2.u8_[i];
        };
        auto eq_elem = [&](int i, int j) -> bool {
            if (is_word) return src1.u16_[i] == src2.u16_[j];
            return src1.u8_[i] == src2.u8_[j];
        };

        // Compute implicit string lengths (null-terminator scan, unsigned)
        int len1 = elem_count, len2 = elem_count;
        if (is_word) {
            for (int i = 0; i < 8; ++i) {
                if (src1.u16_[i] == 0) { len1 = i; break; }
            }
            for (int i = 0; i < 8; ++i) {
                if (src2.u16_[i] == 0) { len2 = i; break; }
            }
        } else {
            for (int i = 0; i < 16; ++i) {
                if (src1.u8_[i] == 0) { len1 = i; break; }
            }
            for (int i = 0; i < 16; ++i) {
                if (src2.u8_[i] == 0) { len2 = i; break; }
            }
        }

        // Aggregation
        u16 int_res2 = 0;
        u8 agg = (imm >> 2) & 3;
        if (agg == 0) {
            // Equal any: IntRes1[j] = OR over i of (xmm1[i] == xmm2[j])
            // for valid i,j; invalid lanes contribute 0.
            for (int j = 0; j < elem_count; ++j) {
                if (j >= len2) continue;
                for (int i = 0; i < elem_count; ++i) {
                    if (i >= len1) continue;
                    if (eq_elem(i, j)) { int_res2 |= (1 << j); break; }
                }
            }
        } else if (agg == 1) {
            // Ranges: pairs (xmm1[2k], xmm1[2k+1]) form [low,high].
            // IntRes1[j] = OR over k of (xmm1[2k] <= xmm2[j] <= xmm1[2k+1]).
            // Comparisons are signed/unsigned per imm[1].
            for (int j = 0; j < elem_count; ++j) {
                if (j >= len2) continue;
                i32 v = get2(j);
                for (int i = 0; i + 1 < len1; i += 2) {
                    i32 lo = get1(i);
                    i32 hi = get1(i + 1);
                    if (v >= lo && v <= hi) { int_res2 |= (1 << j); break; }
                }
            }
        } else if (agg == 2) {
            // Equal each: IntRes1[i] = (xmm1[i] == xmm2[i]) for valid lanes.
            // Per Intel SDM Table 4-7:
            //   valid:valid     -> compare
            //   valid:invalid   -> 0
            //   invalid:valid   -> 0
            //   invalid:invalid -> 1
            for (int i = 0; i < elem_count; ++i) {
                bool v1 = (i < len1);
                bool v2 = (i < len2);
                if (!v1 && !v2) {
                    int_res2 |= (1 << i);
                } else if (v1 && v2) {
                    if (eq_elem(i, i)) int_res2 |= (1 << i);
                }
            }
        } else {
            // Equal ordered (substring search). Verified against hardware:
            //   k = j + i
            //   k >= elem_count       -> 1 (off-grid, accept)
            //   i >= len1             -> 1 (pattern exhausted, accept)
            //   k >= len2             -> 0 (text past-null while pattern still
            //                               valid: reject)
            //   otherwise             -> compare
            for (int j = 0; j < elem_count; ++j) {
                bool match = true;
                for (int i = 0; i < elem_count; ++i) {
                    int k = j + i;
                    if (k >= elem_count) break;
                    if (i >= len1) break;
                    if (k >= len2) { match = false; break; }
                    if (!eq_elem(i, k)) { match = false; break; }
                }
                if (match) int_res2 |= (1 << j);
            }
        }

        // Polarity (imm[5:4]):
        //   00 -> IntRes2 = IntRes1
        //   01 -> IntRes2 = ~IntRes1 (full elem_count-bit complement)
        //   10 -> IntRes2 = IntRes1   (same as 00)
        //   11 -> IntRes2 = IntRes1 XOR validMask(xmm2)
        u16 elem_mask = static_cast<u16>((1u << elem_count) - 1u);
        u8 pol = (imm >> 4) & 3;
        if (pol == 1) {
            int_res2 = static_cast<u16>(~int_res2) & elem_mask;
        } else if (pol == 3) {
            u16 valid_mask =
                (len2 >= elem_count) ? elem_mask
                                     : static_cast<u16>((1u << len2) - 1u);
            int_res2 ^= valid_mask;
            int_res2 &= elem_mask;
        } else {
            int_res2 &= elem_mask;
        }

        // Output: ECX = bit index of LSB/MSB
        int idx = elem_count;
        if ((imm & 0x40) == 0) {
            for (int i = 0; i < elem_count; ++i)
                if (int_res2 & (1 << i)) { idx = i; break; }
        } else {
            for (int i = elem_count - 1; i >= 0; --i)
                if (int_res2 & (1 << i)) { idx = i; break; }
        }
        ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RCX), idx);

        // Flags
        ctx.cpu.flags.set_cf((int_res2 != 0));
        ctx.cpu.flags.set_zf((len2 < elem_count));
        ctx.cpu.flags.set_sf((len1 < elem_count));
        ctx.cpu.flags.set_of((int_res2 & 1) != 0);
        ctx.cpu.flags.set_af(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }

    case Opcode::PCMPISTRM_XMM_XMMM128_IMM8: {
        auto src2_r = read_pack(ctx, op1, aligned_form);
        if (!src2_r) GP_FAULT;
        auto& src1 = xmm_ref(ctx, op0.reg);
        XmmReg src2 = src2_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        bool is_word = (imm & 1) != 0;
        int elem_count = is_word ? 8 : 16;

        // Implicit lengths: scan for null terminator
        int len1 = elem_count, len2 = elem_count;
        if (is_word) {
            for (int i = 0; i < 8; ++i)
                if (src1.u16_[i] == 0) { len1 = i; break; }
            for (int i = 0; i < 8; ++i)
                if (src2.u16_[i] == 0) { len2 = i; break; }
        } else {
            for (int i = 0; i < 16; ++i)
                if (src1.u8_[i] == 0) { len1 = i; break; }
            for (int i = 0; i < 16; ++i)
                if (src2.u8_[i] == 0) { len2 = i; break; }
        }

        u16 int_res2 = pcmpxstrx_aggregate(src1, src2, len1, len2, imm);

        // Output: XMM0 = mask (bit/expanded)
        auto& xmm0 = ctx.cpu.sse.xmm[0];
        xmm0.clear();
        if (imm & 0x40) {
            if (is_word) {
                for (int i = 0; i < 8; ++i)
                    xmm0.u16_[i] = (int_res2 & (1 << i)) ? 0xFFFF : 0;
            } else {
                for (int i = 0; i < 16; ++i)
                    xmm0.u8_[i] = (int_res2 & (1 << i)) ? 0xFF : 0;
            }
        } else {
            xmm0.u16_[0] = int_res2;
        }

        ctx.cpu.flags.set_cf((int_res2 != 0));
        ctx.cpu.flags.set_zf((len2 < elem_count));
        ctx.cpu.flags.set_sf((len1 < elem_count));
        ctx.cpu.flags.set_of((int_res2 & 1) != 0);
        ctx.cpu.flags.set_af(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }

    case Opcode::PCMPESTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRM_XMM_XMMM128_IMM8: {
        auto src2_r = read_pack(ctx, op1, aligned_form);
        if (!src2_r) GP_FAULT;
        auto& src1 = xmm_ref(ctx, op0.reg);
        XmmReg src2 = src2_r.value();
        u8 imm = static_cast<u8>(instr.operands[2].imm);
        bool is_word = (imm & 1) != 0;
        int elem_count = is_word ? 8 : 16;

        // Explicit lengths from EAX/EDX: SDM says "absolute value of signed
        // value, saturated to max element count".
        i32 raw_a = static_cast<i32>(ctx.cpu.gpr.get32(
            static_cast<u8>(RegIndex::RAX)));
        i32 raw_d = static_cast<i32>(ctx.cpu.gpr.get32(
            static_cast<u8>(RegIndex::RDX)));
        i64 abs_a = (raw_a < 0) ? -(i64)raw_a : (i64)raw_a;
        i64 abs_d = (raw_d < 0) ? -(i64)raw_d : (i64)raw_d;
        int len1 = (abs_a > elem_count) ? elem_count : static_cast<int>(abs_a);
        int len2 = (abs_d > elem_count) ? elem_count : static_cast<int>(abs_d);

        u16 int_res2 = pcmpxstrx_aggregate(src1, src2, len1, len2, imm);

        bool is_index = (instr.opcode == Opcode::PCMPESTRI_XMM_XMMM128_IMM8);
        if (is_index) {
            int idx = elem_count;
            if ((imm & 0x40) == 0) {
                for (int i = 0; i < elem_count; ++i)
                    if (int_res2 & (1 << i)) { idx = i; break; }
            } else {
                for (int i = elem_count - 1; i >= 0; --i)
                    if (int_res2 & (1 << i)) { idx = i; break; }
            }
            ctx.cpu.gpr.set32(static_cast<u8>(RegIndex::RCX), idx);
        } else {
            auto& xmm0 = ctx.cpu.sse.xmm[0];
            xmm0.clear();
            if (imm & 0x40) {
                if (is_word) {
                    for (int i = 0; i < 8; ++i)
                        xmm0.u16_[i] = (int_res2 & (1 << i)) ? 0xFFFF : 0;
                } else {
                    for (int i = 0; i < 16; ++i)
                        xmm0.u8_[i] = (int_res2 & (1 << i)) ? 0xFF : 0;
                }
            } else {
                xmm0.u16_[0] = int_res2;
            }
        }

        ctx.cpu.flags.set_cf((int_res2 != 0));
        ctx.cpu.flags.set_zf((len2 < elem_count));
        ctx.cpu.flags.set_sf((len1 < elem_count));
        ctx.cpu.flags.set_of((int_res2 & 1) != 0);
        ctx.cpu.flags.set_af(false);
        ctx.cpu.flags.set_pf(false);
        ctx.cpu.set_flag_op(CpuState::FlagOp::None);
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

#undef GP_FAULT

} // namespace vitreo::cpu::ops
