// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// MMX instruction execution (CPU-045).
//
// MMX registers MM0-MM7 are aliased to the low 64 bits of the x87 FPU registers.
// The FpuState::get_mm/set_mm methods handle this aliasing.

#include "exec_ops.hpp"
#include "helpers.hpp"

namespace vitreo::cpu::ops {

/// Helper: Read 64-bit MMX register
static u64 read_mm(ExecContext& ctx, u8 reg) noexcept {
    ctx.cpu.fpu.mmx_enter();
    return ctx.cpu.fpu.get_mm(reg & 7);
}

/// Helper: Write 64-bit MMX register
static void write_mm(ExecContext& ctx, u8 reg, u64 val) noexcept {
    ctx.cpu.fpu.set_mm(reg & 7, val);
    ctx.cpu.fpu.mmx_enter();
}

/// Helper: Read 64-bit value from MMX register or memory operand
static std::optional<u64> read_mmm64(ExecContext& ctx, const Operand& op) noexcept {
    if (op.type == OperandType::RegMM) {
        return read_mm(ctx, op.reg);
    }
    // Memory operand
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return std::nullopt;
    auto result = read_memory(ctx, addr.value(), 8);
    if (!result) return std::nullopt;
    return result.value();
}

/// Helper: Write 64-bit value to memory
static bool write_mem64(ExecContext& ctx, const Operand& op, u64 val) noexcept {
    auto addr = calc_effective_address(ctx, op.mem);
    if (!addr) return false;
    auto r = write_memory(ctx, addr.value(), val, 8);
    return r.is_ok();
}

ExecResult exec_mmx(ExecContext& ctx, const DecodedInstruction& instr) noexcept {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ========================================================================
    // MOVQ mm, mm/m64 — Move Quadword
    // ========================================================================
    case Opcode::MOVQ_MM_MMM64: {
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        write_mm(ctx, op0.reg, *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVQ mm/m64, mm — Move Quadword (store)
    // ========================================================================
    case Opcode::MOVQ_MMM64_MM: {
        u64 val = read_mm(ctx, op1.reg);
        if (op0.type == OperandType::RegMM) {
            write_mm(ctx, op0.reg, val);
        } else {
            if (!write_mem64(ctx, op0, val)) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVD mm, r/m32 — Move Doubleword (zero-extend to 64 bits)
    // ========================================================================
    case Opcode::MOVD_MM_RM32: {
        u64 val;
        if (op1.type == OperandType::Reg32) {
            val = read_register(ctx, OperandType::Reg32, op1.reg);
        } else {
            auto addr = calc_effective_address(ctx, op1.mem);
            if (!addr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto result = read_memory(ctx, addr.value(), 4);
            if (!result) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            val = result.value();
        }
        // Zero-extend 32-bit value to 64 bits
        write_mm(ctx, op0.reg, val);
        return ExecResult::Continue;
    }

    // ========================================================================
    // MOVD r/m32, mm — Move Doubleword (low 32 bits)
    // ========================================================================
    case Opcode::MOVD_RM32_MM: {
        u64 val = read_mm(ctx, op1.reg);
        u32 lo32 = static_cast<u32>(val);
        if (op0.type == OperandType::Reg32) {
            write_register(ctx, OperandType::Reg32, op0.reg, lo32);
        } else {
            auto addr = calc_effective_address(ctx, op0.mem);
            if (!addr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto r = write_memory(ctx, addr.value(), lo32, 4);
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PADDW mm, mm/m64 — Add Packed Word (wraps per 16-bit lane)
    // ========================================================================
    case Opcode::PADDW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            const u32 shift = static_cast<u32>(lane) * 16u;
            const u16 sum = static_cast<u16>((dst >> shift) & 0xFFFFu) +
                            static_cast<u16>((*src >> shift) & 0xFFFFu);
            result |= static_cast<u64>(sum) << shift;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSUBW mm, mm/m64 — Subtract Packed Word (wraps per 16-bit lane)
    // Intel SDM Vol. 2: PSUBB/PSUBW/PSUBD, NP 0F F9 /r.
    // ========================================================================
    case Opcode::PSUBW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            const u32 shift = static_cast<u32>(lane) * 16u;
            const u16 diff = static_cast<u16>((dst >> shift) & 0xFFFFu) -
                             static_cast<u16>((*src >> shift) & 0xFFFFu);
            result |= static_cast<u64>(diff) << shift;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMINSW / PMAXSW mm, mm/m64 — packed signed word min/max.
    // Intel SDM Vol. 2: NP 0F EA /r and NP 0F EE /r.
    // ========================================================================
    case Opcode::PMINSW_MM_MMM64:
    case Opcode::PMAXSW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const bool take_max = instr.opcode == Opcode::PMAXSW_MM_MMM64;
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            const u32 shift = static_cast<u32>(lane) * 16u;
            const i16 d = static_cast<i16>((dst >> shift) & 0xFFFFu);
            const i16 s = static_cast<i16>((*src >> shift) & 0xFFFFu);
            const i16 v = take_max ? (d > s ? d : s) : (d < s ? d : s);
            result |= static_cast<u64>(static_cast<u16>(v)) << shift;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PACKSSDW mm, mm/m64 — pack 4 signed dwords into 4 signed words.
    // Intel SDM Vol. 2: NP 0F 6B /r. Dest low two words from dest dwords,
    // high two words from source dwords; saturate to int16.
    // ========================================================================
    case Opcode::PACKSSDW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto sat_i16 = [](u64 packed, int lane) -> u64 {
            const i32 dword = static_cast<i32>(
                (packed >> (static_cast<u32>(lane) * 32u)) & 0xFFFFFFFFu);
            i16 word = static_cast<i16>(dword);
            if (dword > 32767) word = 32767;
            else if (dword < -32768) word = -32768;
            return static_cast<u16>(word);
        };
        u64 result = 0;
        result |= sat_i16(dst, 0);
        result |= sat_i16(dst, 1) << 16;
        result |= sat_i16(*src, 0) << 32;
        result |= sat_i16(*src, 1) << 48;
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSHUFW mm1, mm2/m64, imm8 — shuffle packed words into an MM register.
    // Intel SDM Vol. 2: NP 0F 70 /r ib.
    // ========================================================================
    case Opcode::PSHUFW_MM_MMM64_IMM8: {
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            const u32 sel = (imm >> (lane * 2)) & 3u;
            const u16 word = static_cast<u16>((*src >> (sel * 16u)) & 0xFFFFu);
            result |= static_cast<u64>(word) << (static_cast<u32>(lane) * 16u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PACKUSWB mm, mm/m64 — Pack 8 signed words into 8 unsigned saturated
    // bytes: low 4 bytes from the destination, high 4 bytes from the source.
    // ========================================================================
    case Opcode::PACKUSWB_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto saturate = [](u64 packed, int lane) -> u64 {
            const i16 word = static_cast<i16>(
                (packed >> (static_cast<u32>(lane) * 16u)) & 0xFFFFu);
            if (word < 0) return 0;
            if (word > 255) return 255;
            return static_cast<u64>(word);
        };
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            result |= saturate(dst, lane) << (static_cast<u32>(lane) * 8u);
            result |= saturate(*src, lane)
                      << ((static_cast<u32>(lane) + 4u) * 8u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSRLD / PSRAD / PSLLD mm, imm8 or mm/m64 — per-dword shifts.
    // A count above 31 zeroes each lane (PSRAD fills with the sign bit).
    // Register-count forms use the full 64-bit source (Intel SDM Vol. 2).
    // ========================================================================
    case Opcode::PSRLD_MM_IMM8:
    case Opcode::PSRAD_MM_IMM8:
    case Opcode::PSLLD_MM_IMM8:
    case Opcode::PSRLD_MM_MMM64:
    case Opcode::PSRAD_MM_MMM64:
    case Opcode::PSLLD_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        u64 count = 0;
        const bool from_reg =
            instr.opcode == Opcode::PSRLD_MM_MMM64 ||
            instr.opcode == Opcode::PSRAD_MM_MMM64 ||
            instr.opcode == Opcode::PSLLD_MM_MMM64;
        if (from_reg) {
            auto src = read_mmm64(ctx, op1);
            if (!src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            count = *src;
        } else {
            count = static_cast<u8>(op1.imm);
        }
        const bool arith =
            instr.opcode == Opcode::PSRAD_MM_IMM8 ||
            instr.opcode == Opcode::PSRAD_MM_MMM64;
        const bool left =
            instr.opcode == Opcode::PSLLD_MM_IMM8 ||
            instr.opcode == Opcode::PSLLD_MM_MMM64;
        u64 result = 0;
        for (int lane = 0; lane < 2; ++lane) {
            const u32 shift = static_cast<u32>(lane) * 32u;
            const u32 value = static_cast<u32>((dst >> shift) & 0xFFFFFFFFu);
            u32 lane_result = 0;
            if (arith) {
                const i32 signed_value = static_cast<i32>(value);
                lane_result = count > 31
                    ? static_cast<u32>(signed_value >> 31)
                    : static_cast<u32>(signed_value >> static_cast<u32>(count));
            } else if (count <= 31) {
                lane_result = left
                    ? value << static_cast<u32>(count)
                    : value >> static_cast<u32>(count);
            }
            result |= static_cast<u64>(lane_result) << shift;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PADDD mm, mm/m64 — Add Packed Doubleword (wraps per 32-bit lane)
    // ========================================================================
    case Opcode::PADDD_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const u32 lo = static_cast<u32>(dst) + static_cast<u32>(*src);
        const u32 hi = static_cast<u32>(dst >> 32) +
                       static_cast<u32>(*src >> 32);
        write_mm(ctx, op0.reg,
                 static_cast<u64>(lo) | (static_cast<u64>(hi) << 32));
        return ExecResult::Continue;
    }

    // ========================================================================
    // PADDQ mm, mm/m64 — Add Packed Quadword
    // ========================================================================
    case Opcode::PADDQ_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        write_mm(ctx, op0.reg, dst + *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PMULUDQ mm, mm/m64 — Unsigned Dword Multiply to Qword
    // ========================================================================
    case Opcode::PMULUDQ_MM_MMM64: {
        u32 dst_lo = static_cast<u32>(read_mm(ctx, op0.reg));
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u32 src_lo = static_cast<u32>(*src);
        u64 result = static_cast<u64>(dst_lo) * static_cast<u64>(src_lo);
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PAND mm, mm/m64 — Logical AND
    // ========================================================================
    case Opcode::PAND_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        write_mm(ctx, op0.reg, dst & *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PANDN mm, mm/m64 — Logical AND NOT:  dst = (~dst) & src
    // A video blit uses PANDN to build a color-key mask.
    // ========================================================================
    case Opcode::PANDN_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        write_mm(ctx, op0.reg, (~dst) & *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPEQB/W/D mm, mm/m64 — Packed compare-equal (per byte/word/dword).
    // Each element becomes all-ones on equality, all-zeros otherwise. Used to
    // build a transparency mask in a video color-key blit.
    // ========================================================================
    case Opcode::PCMPEQB_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 8; ++i) {
            u64 d = (dst >> (i * 8)) & 0xFF;
            u64 s = (*src >> (i * 8)) & 0xFF;
            result |= (d == s ? u64{0xFF} : u64{0}) << (i * 8);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PCMPEQW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            u64 d = (dst >> (i * 16)) & 0xFFFF;
            u64 s = (*src >> (i * 16)) & 0xFFFF;
            result |= (d == s ? u64{0xFFFF} : u64{0}) << (i * 16);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PCMPEQD_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 2; ++i) {
            u64 d = (dst >> (i * 32)) & 0xFFFFFFFF;
            u64 s = (*src >> (i * 32)) & 0xFFFFFFFF;
            result |= (d == s ? u64{0xFFFFFFFF} : u64{0}) << (i * 32);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PCMPGTB/W/D mm, mm/m64 — Packed signed compare-greater-than.
    // Intel SDM Vol. 2: each element becomes all-ones if dest > src, else
    // all-zeros. NP 0F 64/65/66; the 66-prefixed forms are SSE2 XMM.
    // ========================================================================
    case Opcode::PCMPGTB_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 8; ++i) {
            const i8 d = static_cast<i8>((dst >> (i * 8)) & 0xFF);
            const i8 s = static_cast<i8>((*src >> (i * 8)) & 0xFF);
            result |= (d > s ? u64{0xFF} : u64{0}) << (i * 8);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PCMPGTW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            const i16 d = static_cast<i16>((dst >> (i * 16)) & 0xFFFF);
            const i16 s = static_cast<i16>((*src >> (i * 16)) & 0xFFFF);
            result |= (d > s ? u64{0xFFFF} : u64{0}) << (i * 16);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PCMPGTD_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 2; ++i) {
            const i32 d = static_cast<i32>((dst >> (i * 32)) & 0xFFFFFFFFu);
            const i32 s = static_cast<i32>((*src >> (i * 32)) & 0xFFFFFFFFu);
            result |= (d > s ? u64{0xFFFFFFFF} : u64{0}) << (i * 32);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // POR mm, mm/m64 — Logical OR
    // ========================================================================
    case Opcode::POR_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        write_mm(ctx, op0.reg, dst | *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PXOR mm, mm/m64 — Logical XOR
    // ========================================================================
    case Opcode::PXOR_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        write_mm(ctx, op0.reg, dst ^ *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSLLQ mm, imm8 — Shift Left Logical
    // ========================================================================
    case Opcode::PSLLQ_MM_IMM8: {
        u64 dst = read_mm(ctx, op0.reg);
        u8 shift = static_cast<u8>(op1.imm);
        if (shift > 63) {
            write_mm(ctx, op0.reg, 0);
        } else {
            write_mm(ctx, op0.reg, dst << shift);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSRLQ mm, imm8 — Shift Right Logical
    // ========================================================================
    case Opcode::PSRLQ_MM_IMM8: {
        u64 dst = read_mm(ctx, op0.reg);
        u8 shift = static_cast<u8>(op1.imm);
        if (shift > 63) {
            write_mm(ctx, op0.reg, 0);
        } else {
            write_mm(ctx, op0.reg, dst >> shift);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSLLQ mm, mm/m64 — Shift Left Logical by Register
    // ========================================================================
    case Opcode::PSLLQ_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 cnt = *src;
        if (cnt > 63) {
            write_mm(ctx, op0.reg, 0);
        } else {
            write_mm(ctx, op0.reg, dst << cnt);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PSRLQ mm, mm/m64 — Shift Right Logical by Register
    // ========================================================================
    case Opcode::PSRLQ_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 cnt = *src;
        if (cnt > 63) {
            write_mm(ctx, op0.reg, 0);
        } else {
            write_mm(ctx, op0.reg, dst >> cnt);
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKLBW mm, mm/m64 — Unpack Low Bytes
    // Interleaves the low 4 bytes of dst and src:
    //   result = d0 s0 d1 s1 d2 s2 d3 s3  (byte 0 = low)
    // ========================================================================
    case Opcode::PUNPCKLBW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            u64 d = (dst >> (i * 8)) & 0xFF;
            u64 s = (*src >> (i * 8)) & 0xFF;
            result |= d << (i * 16);
            result |= s << (i * 16 + 8);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKHBW mm, mm/m64 — Unpack High Bytes
    //   result = d4 s4 d5 s5 d6 s6 d7 s7
    // ========================================================================
    case Opcode::PUNPCKHBW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            u64 d = (dst >> ((i + 4) * 8)) & 0xFF;
            u64 s = (*src >> ((i + 4) * 8)) & 0xFF;
            result |= d << (i * 16);
            result |= s << (i * 16 + 8);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKLWD mm, mm/m64 — Unpack Low Words
    //   result = d0 s0 d1 s1  (16-bit words; word 0 = low)
    // ========================================================================
    case Opcode::PUNPCKLWD_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 2; ++i) {
            u64 d = (dst >> (i * 16)) & 0xFFFF;
            u64 s = (*src >> (i * 16)) & 0xFFFF;
            result |= d << (i * 32);
            result |= s << (i * 32 + 16);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKHWD mm, mm/m64 — Unpack High Words
    //   result = d2 s2 d3 s3
    // ========================================================================
    case Opcode::PUNPCKHWD_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 2; ++i) {
            u64 d = (dst >> ((i + 2) * 16)) & 0xFFFF;
            u64 s = (*src >> ((i + 2) * 16)) & 0xFFFF;
            result |= d << (i * 32);
            result |= s << (i * 32 + 16);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKLDQ mm, mm/m64 — Unpack Low Doublewords
    // Interleaves low 32-bit values: dst = [dst[0:31], src[0:31]]
    // ========================================================================
    case Opcode::PUNPCKLDQ_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        // Result: low dword from dst | low dword from src shifted to high
        u32 dst_lo = static_cast<u32>(dst);
        u32 src_lo = static_cast<u32>(*src);
        u64 result = static_cast<u64>(dst_lo) | (static_cast<u64>(src_lo) << 32);
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKHDQ mm, mm/m64 — Unpack High Doublewords
    // Interleaves high 32-bit values: dst = [dst[32:63], src[32:63]]
    // ========================================================================
    case Opcode::PUNPCKHDQ_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u32 dst_hi = static_cast<u32>(dst >> 32);
        u32 src_hi = static_cast<u32>(*src >> 32);
        u64 result = static_cast<u64>(dst_hi) | (static_cast<u64>(src_hi) << 32);
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    // ========================================================================
    // PUNPCKHQDQ mm, mm/m64 — Unpack High Quadwords
    // This is actually not a standard MMX instruction but may be expected
    // ========================================================================
    case Opcode::PUNPCKHQDQ_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        // For 64-bit MMX, PUNPCKHQDQ would take high qword from both
        // Since MMX is only 64-bit, this behaves like: dst = src
        write_mm(ctx, op0.reg, *src);
        return ExecResult::Continue;
    }

    // ========================================================================
    // Remaining NP 0F MMX / SSE-on-MMX forms. Intel SDM Vol. 2: destination
    // is MM0-7 (aliased with the x87 stack), wrap or saturate per lane, no
    // EFLAGS update.
    // ========================================================================
    case Opcode::PADDB_MM_MMM64:
    case Opcode::PSUBB_MM_MMM64:
    case Opcode::PSUBD_MM_MMM64:
    case Opcode::PSUBQ_MM_MMM64:
    case Opcode::PADDUSB_MM_MMM64:
    case Opcode::PADDUSW_MM_MMM64:
    case Opcode::PSUBUSB_MM_MMM64:
    case Opcode::PSUBUSW_MM_MMM64:
    case Opcode::PADDSB_MM_MMM64:
    case Opcode::PADDSW_MM_MMM64:
    case Opcode::PSUBSB_MM_MMM64:
    case Opcode::PSUBSW_MM_MMM64:
    case Opcode::PMINUB_MM_MMM64:
    case Opcode::PMAXUB_MM_MMM64:
    case Opcode::PAVGB_MM_MMM64:
    case Opcode::PAVGW_MM_MMM64:
    case Opcode::PMULLW_MM_MMM64:
    case Opcode::PMULHW_MM_MMM64:
    case Opcode::PMULHUW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        const auto sat_i8 = [](i16 v) -> u8 {
            if (v > 127) return 127;
            if (v < -128) return static_cast<u8>(0x80);
            return static_cast<u8>(v);
        };
        const auto sat_i16 = [](i32 v) -> u16 {
            if (v > 32767) return 32767;
            if (v < -32768) return static_cast<u16>(0x8000);
            return static_cast<u16>(v);
        };
        switch (instr.opcode) {
        case Opcode::PADDB_MM_MMM64:
        case Opcode::PSUBB_MM_MMM64:
        case Opcode::PADDSB_MM_MMM64:
        case Opcode::PSUBSB_MM_MMM64:
        case Opcode::PADDUSB_MM_MMM64:
        case Opcode::PSUBUSB_MM_MMM64:
        case Opcode::PMINUB_MM_MMM64:
        case Opcode::PMAXUB_MM_MMM64:
        case Opcode::PAVGB_MM_MMM64:
            for (int lane = 0; lane < 8; ++lane) {
                const u32 sh = static_cast<u32>(lane) * 8u;
                const u8 a = static_cast<u8>((dst >> sh) & 0xFFu);
                const u8 b = static_cast<u8>((*src >> sh) & 0xFFu);
                u8 v = 0;
                switch (instr.opcode) {
                case Opcode::PADDB_MM_MMM64:
                    v = static_cast<u8>(a + b); break;
                case Opcode::PSUBB_MM_MMM64:
                    v = static_cast<u8>(a - b); break;
                case Opcode::PADDSB_MM_MMM64:
                    v = sat_i8(static_cast<i16>(static_cast<i8>(a)) +
                               static_cast<i16>(static_cast<i8>(b)));
                    break;
                case Opcode::PSUBSB_MM_MMM64:
                    v = sat_i8(static_cast<i16>(static_cast<i8>(a)) -
                               static_cast<i16>(static_cast<i8>(b)));
                    break;
                case Opcode::PADDUSB_MM_MMM64: {
                    const u16 sum = static_cast<u16>(a) + static_cast<u16>(b);
                    v = sum > 255 ? 255 : static_cast<u8>(sum);
                    break;
                }
                case Opcode::PSUBUSB_MM_MMM64:
                    v = (a < b) ? 0 : static_cast<u8>(a - b);
                    break;
                case Opcode::PMINUB_MM_MMM64:
                    v = (a < b) ? a : b; break;
                case Opcode::PMAXUB_MM_MMM64:
                    v = (a > b) ? a : b; break;
                case Opcode::PAVGB_MM_MMM64:
                    v = static_cast<u8>(
                        (static_cast<u16>(a) + static_cast<u16>(b) + 1u) >> 1);
                    break;
                default: break;
                }
                result |= static_cast<u64>(v) << sh;
            }
            break;
        case Opcode::PADDUSW_MM_MMM64:
        case Opcode::PSUBUSW_MM_MMM64:
        case Opcode::PADDSW_MM_MMM64:
        case Opcode::PSUBSW_MM_MMM64:
        case Opcode::PAVGW_MM_MMM64:
        case Opcode::PMULLW_MM_MMM64:
        case Opcode::PMULHW_MM_MMM64:
        case Opcode::PMULHUW_MM_MMM64:
            for (int lane = 0; lane < 4; ++lane) {
                const u32 sh = static_cast<u32>(lane) * 16u;
                const u16 a = static_cast<u16>((dst >> sh) & 0xFFFFu);
                const u16 b = static_cast<u16>((*src >> sh) & 0xFFFFu);
                u16 v = 0;
                switch (instr.opcode) {
                case Opcode::PADDUSW_MM_MMM64: {
                    const u32 sum = static_cast<u32>(a) + static_cast<u32>(b);
                    v = sum > 65535u ? 65535u : static_cast<u16>(sum);
                    break;
                }
                case Opcode::PSUBUSW_MM_MMM64:
                    v = (a < b) ? 0 : static_cast<u16>(a - b);
                    break;
                case Opcode::PADDSW_MM_MMM64:
                    v = sat_i16(static_cast<i32>(static_cast<i16>(a)) +
                                static_cast<i32>(static_cast<i16>(b)));
                    break;
                case Opcode::PSUBSW_MM_MMM64:
                    v = sat_i16(static_cast<i32>(static_cast<i16>(a)) -
                                static_cast<i32>(static_cast<i16>(b)));
                    break;
                case Opcode::PAVGW_MM_MMM64:
                    v = static_cast<u16>(
                        (static_cast<u32>(a) + static_cast<u32>(b) + 1u) >> 1);
                    break;
                case Opcode::PMULLW_MM_MMM64:
                    v = static_cast<u16>(
                        static_cast<i32>(static_cast<i16>(a)) *
                        static_cast<i32>(static_cast<i16>(b)));
                    break;
                case Opcode::PMULHW_MM_MMM64:
                    v = static_cast<u16>(
                        (static_cast<i32>(static_cast<i16>(a)) *
                         static_cast<i32>(static_cast<i16>(b))) >> 16);
                    break;
                case Opcode::PMULHUW_MM_MMM64:
                    v = static_cast<u16>(
                        (static_cast<u32>(a) * static_cast<u32>(b)) >> 16);
                    break;
                default: break;
                }
                result |= static_cast<u64>(v) << sh;
            }
            break;
        case Opcode::PSUBD_MM_MMM64: {
            const u32 lo = static_cast<u32>(dst) - static_cast<u32>(*src);
            const u32 hi = static_cast<u32>(dst >> 32) -
                           static_cast<u32>(*src >> 32);
            result = static_cast<u64>(lo) | (static_cast<u64>(hi) << 32);
            break;
        }
        case Opcode::PSUBQ_MM_MMM64:
            result = dst - *src;
            break;
        default:
            break;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PACKSSWB_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto sat = [](u64 packed, int lane) -> u64 {
            const i16 word = static_cast<i16>(
                (packed >> (static_cast<u32>(lane) * 16u)) & 0xFFFFu);
            if (word > 127) return 127;
            if (word < -128) return 0x80;
            return static_cast<u8>(word);
        };
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            result |= sat(dst, lane) << (static_cast<u32>(lane) * 8u);
            result |= sat(*src, lane) << ((static_cast<u32>(lane) + 4u) * 8u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PMADDWD_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int pair = 0; pair < 2; ++pair) {
            const u32 base = static_cast<u32>(pair) * 32u;
            const i16 d0 = static_cast<i16>((dst >> base) & 0xFFFFu);
            const i16 d1 = static_cast<i16>((dst >> (base + 16u)) & 0xFFFFu);
            const i16 s0 = static_cast<i16>((*src >> base) & 0xFFFFu);
            const i16 s1 = static_cast<i16>((*src >> (base + 16u)) & 0xFFFFu);
            const i32 sum = static_cast<i32>(d0) * static_cast<i32>(s0) +
                            static_cast<i32>(d1) * static_cast<i32>(s1);
            result |= static_cast<u64>(static_cast<u32>(sum)) << base;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PSADBW_MM_MMM64: {
        u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u32 sum = 0;
        for (int lane = 0; lane < 8; ++lane) {
            const u32 sh = static_cast<u32>(lane) * 8u;
            const int a = static_cast<int>((dst >> sh) & 0xFFu);
            const int b = static_cast<int>((*src >> sh) & 0xFFu);
            sum += static_cast<u32>(a > b ? a - b : b - a);
        }
        write_mm(ctx, op0.reg, sum);
        return ExecResult::Continue;
    }

    case Opcode::PSRLW_MM_IMM8:
    case Opcode::PSRAW_MM_IMM8:
    case Opcode::PSLLW_MM_IMM8:
    case Opcode::PSRLW_MM_MMM64:
    case Opcode::PSRAW_MM_MMM64:
    case Opcode::PSLLW_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        u64 count = 0;
        const bool from_reg =
            instr.opcode == Opcode::PSRLW_MM_MMM64 ||
            instr.opcode == Opcode::PSRAW_MM_MMM64 ||
            instr.opcode == Opcode::PSLLW_MM_MMM64;
        if (from_reg) {
            auto src = read_mmm64(ctx, op1);
            if (!src) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            count = *src;
        } else {
            count = static_cast<u8>(op1.imm);
        }
        const bool arith =
            instr.opcode == Opcode::PSRAW_MM_IMM8 ||
            instr.opcode == Opcode::PSRAW_MM_MMM64;
        const bool left =
            instr.opcode == Opcode::PSLLW_MM_IMM8 ||
            instr.opcode == Opcode::PSLLW_MM_MMM64;
        u64 result = 0;
        for (int lane = 0; lane < 4; ++lane) {
            const u32 sh = static_cast<u32>(lane) * 16u;
            const u16 value = static_cast<u16>((dst >> sh) & 0xFFFFu);
            u16 lane_result = 0;
            if (arith) {
                const i16 signed_value = static_cast<i16>(value);
                lane_result = count > 15
                    ? static_cast<u16>(signed_value >> 15)
                    : static_cast<u16>(signed_value >> static_cast<u32>(count));
            } else if (count <= 15) {
                lane_result = left
                    ? static_cast<u16>(value << static_cast<u32>(count))
                    : static_cast<u16>(value >> static_cast<u32>(count));
            }
            result |= static_cast<u64>(lane_result) << sh;
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PMOVMSKB_R32_MM: {
        const u64 src = read_mm(ctx, op1.reg);
        u32 mask = 0;
        for (int lane = 0; lane < 8; ++lane) {
            if ((src >> (static_cast<u32>(lane) * 8u + 7u)) & 1ull)
                mask |= 1u << lane;
        }
        write_register(ctx, OperandType::Reg32, op0.reg, mask);
        return ExecResult::Continue;
    }

    // PINSRW mm, r32/m16, imm8 — SEL := imm8[1:0] (Intel SDM Vol. 2).
    case Opcode::PINSRW_MM_R32M16_IMM8: {
        u64 dst = read_mm(ctx, op0.reg);
        u16 word = 0;
        if (op1.type == OperandType::Reg32) {
            word = static_cast<u16>(read_register(ctx, op1.type, op1.reg));
        } else {
            auto addr = calc_effective_address(ctx, op1.mem);
            if (!addr) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            auto r = read_memory(ctx, addr.value(), 2);
            if (!r) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
            word = static_cast<u16>(r.value());
        }
        const u8 sel = static_cast<u8>(instr.operands[2].imm) & 3u;
        const u32 sh = static_cast<u32>(sel) * 16u;
        dst &= ~(0xFFFFull << sh);
        dst |= static_cast<u64>(word) << sh;
        write_mm(ctx, op0.reg, dst);
        return ExecResult::Continue;
    }

    // PEXTRW r32, mm, imm8 — SEL := imm8[1:0]; zero-extend into r32.
    case Opcode::PEXTRW_R32_MM_IMM8: {
        const u64 src = read_mm(ctx, op1.reg);
        const u8 sel = static_cast<u8>(instr.operands[2].imm) & 3u;
        const u32 val = static_cast<u16>(src >> (static_cast<u32>(sel) * 16u));
        write_register(ctx, OperandType::Reg32, op0.reg, val);
        return ExecResult::Continue;
    }

    // MOVNTQ m64, mm — store; non-temporal hint ignored.
    case Opcode::MOVNTQ_M64_MM: {
        const u64 src = read_mm(ctx, op1.reg);
        if (!write_mem64(ctx, op0, src)) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        return ExecResult::Continue;
    }

    // MOVQ2DQ xmm, mm — DEST[63:0] := SRC; DEST[127:64] := 0 (Intel SDM Vol. 2).
    // Also enters MMX state even though the MM source is not rewritten.
    case Opcode::MOVQ2DQ_XMM_MM: {
        // read_mm() enters MMX state (tags all-Valid, TOP=0). Do NOT write the
        // source back: set_mm() forces exponent_sign to 0xFFFF, which would
        // rewrite an MM register this instruction only reads. That corrupted
        // any register still holding a real x87 value.
        const u64 src = read_mm(ctx, op1.reg);
        auto& dst = ctx.cpu.sse.xmm[op0.reg & 15];
        dst.clear();
        dst.u64_[0] = src;
        return ExecResult::Continue;
    }

    // MOVDQ2Q mm, xmm — DEST := SRC[63:0]
    case Opcode::MOVDQ2Q_MM_XMM: {
        write_mm(ctx, op0.reg, ctx.cpu.sse.xmm[op1.reg & 15].u64_[0]);
        return ExecResult::Continue;
    }

    // MASKMOVQ mm1, mm2 — store selected bytes of mm1 to DS:EDI/RDI using
    // the MSB of each mm2 byte as the write enable. EDI is not incremented.
    // The instruction still enters MMX state even if the mask is all zeros.
    case Opcode::MASKMOVQ_MM_MM: {
        const u64 data = read_mm(ctx, op0.reg);
        const u64 mask = read_mm(ctx, op1.reg);
        write_mm(ctx, op0.reg, data);
        auto addr = maskmov_ds_edi_address(ctx, instr);
        if (!addr) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        for (int lane = 0; lane < 8; ++lane) {
            if (((mask >> (static_cast<u32>(lane) * 8u + 7u)) & 1ull) == 0)
                continue;
            const u8 byte = static_cast<u8>(data >> (static_cast<u32>(lane) * 8u));
            if (!write_memory(ctx, addr.value() + static_cast<u64>(lane),
                              byte, 1)) {
                ctx.exception = CpuException::GeneralProtection;
                return ExecResult::Exception;
            }
        }
        return ExecResult::Continue;
    }

    // ========================================================================
    // SSSE3 on MMX (NP 0F 38 / 0F 3A) — 64-bit forms of the XMM ops.
    // Intel SDM Vol. 2: 8 bytes / 4 words / 2 dwords. Interpreter is the
    // semantic reference; lane counts are half the XMM implementations.
    // ========================================================================

    case Opcode::PSHUFB_MM_MMM64: {
        // Control byte bit 7 → 0; else DEST[i] := ORIG[src[i] & 7].
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 8; ++i) {
            const u8 ctrl = static_cast<u8>(*src >> (static_cast<u32>(i) * 8u));
            u8 val = 0;
            if ((ctrl & 0x80u) == 0) {
                val = static_cast<u8>(dst >> ((ctrl & 7u) * 8u));
            }
            result |= static_cast<u64>(val) << (static_cast<u32>(i) * 8u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PALIGNR_MM_MMM64_IMM8: {
        // Concat [src 8 | dst 8], shift right imm8 bytes, take low 8.
        // imm8 >= 16 → 0. Same concat order as XMM PALIGNR.
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const u8 shift = static_cast<u8>(instr.operands[2].imm);
        u64 result = 0;
        if (shift < 16) {
            u8 concat[16];
            for (int i = 0; i < 8; ++i) {
                concat[i] = static_cast<u8>(*src >> (static_cast<u32>(i) * 8u));
                concat[i + 8] = static_cast<u8>(dst >> (static_cast<u32>(i) * 8u));
            }
            for (int i = 0; i < 8; ++i) {
                const int idx = i + static_cast<int>(shift);
                if (idx < 16) {
                    result |= static_cast<u64>(concat[idx]) << (static_cast<u32>(i) * 8u);
                }
            }
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PHADDW_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto w = [](u64 v, int lane) -> i16 {
            return static_cast<i16>(static_cast<u16>(v >> (static_cast<u32>(lane) * 16u)));
        };
        const auto wrap = [](i16 a, i16 b) -> u16 {
            return static_cast<u16>(static_cast<i16>(static_cast<i32>(a) + static_cast<i32>(b)));
        };
        u64 result = 0;
        result |= static_cast<u64>(wrap(w(dst, 0), w(dst, 1)));
        result |= static_cast<u64>(wrap(w(dst, 2), w(dst, 3))) << 16;
        result |= static_cast<u64>(wrap(w(*src, 0), w(*src, 1))) << 32;
        result |= static_cast<u64>(wrap(w(*src, 2), w(*src, 3))) << 48;
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PHADDD_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const u32 d0 = static_cast<u32>(dst);
        const u32 d1 = static_cast<u32>(dst >> 32);
        const u32 s0 = static_cast<u32>(*src);
        const u32 s1 = static_cast<u32>(*src >> 32);
        const u64 result = static_cast<u64>(d0 + d1) | (static_cast<u64>(s0 + s1) << 32);
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PHADDSW_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto w = [](u64 v, int lane) -> i16 {
            return static_cast<i16>(static_cast<u16>(v >> (static_cast<u32>(lane) * 16u)));
        };
        const auto sat = [](i16 a, i16 b) -> u16 {
            const i32 sum = static_cast<i32>(a) + static_cast<i32>(b);
            if (sum > 32767) return 32767;
            if (sum < -32768) return static_cast<u16>(static_cast<i16>(-32768));
            return static_cast<u16>(static_cast<i16>(sum));
        };
        u64 result = 0;
        result |= static_cast<u64>(sat(w(dst, 0), w(dst, 1)));
        result |= static_cast<u64>(sat(w(dst, 2), w(dst, 3))) << 16;
        result |= static_cast<u64>(sat(w(*src, 0), w(*src, 1))) << 32;
        result |= static_cast<u64>(sat(w(*src, 2), w(*src, 3))) << 48;
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PHSUBW_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto w = [](u64 v, int lane) -> i16 {
            return static_cast<i16>(static_cast<u16>(v >> (static_cast<u32>(lane) * 16u)));
        };
        const auto wrap = [](i16 a, i16 b) -> u16 {
            return static_cast<u16>(static_cast<i16>(static_cast<i32>(a) - static_cast<i32>(b)));
        };
        u64 result = 0;
        result |= static_cast<u64>(wrap(w(dst, 0), w(dst, 1)));
        result |= static_cast<u64>(wrap(w(dst, 2), w(dst, 3))) << 16;
        result |= static_cast<u64>(wrap(w(*src, 0), w(*src, 1))) << 32;
        result |= static_cast<u64>(wrap(w(*src, 2), w(*src, 3))) << 48;
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PHSUBD_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const u32 d0 = static_cast<u32>(dst);
        const u32 d1 = static_cast<u32>(dst >> 32);
        const u32 s0 = static_cast<u32>(*src);
        const u32 s1 = static_cast<u32>(*src >> 32);
        const u64 result = static_cast<u64>(d0 - d1) | (static_cast<u64>(s0 - s1) << 32);
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PHSUBSW_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        const auto w = [](u64 v, int lane) -> i16 {
            return static_cast<i16>(static_cast<u16>(v >> (static_cast<u32>(lane) * 16u)));
        };
        const auto sat = [](i16 a, i16 b) -> u16 {
            const i32 diff = static_cast<i32>(a) - static_cast<i32>(b);
            if (diff > 32767) return 32767;
            if (diff < -32768) return static_cast<u16>(static_cast<i16>(-32768));
            return static_cast<u16>(static_cast<i16>(diff));
        };
        u64 result = 0;
        result |= static_cast<u64>(sat(w(dst, 0), w(dst, 1)));
        result |= static_cast<u64>(sat(w(dst, 2), w(dst, 3))) << 16;
        result |= static_cast<u64>(sat(w(*src, 0), w(*src, 1))) << 32;
        result |= static_cast<u64>(sat(w(*src, 2), w(*src, 3))) << 48;
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PMADDUBSW_MM_MMM64: {
        // Unsigned dest bytes × signed src bytes; adjacent pairs add; sat i16.
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            const u8 da = static_cast<u8>(dst >> (static_cast<u32>(2 * i) * 8u));
            const u8 db = static_cast<u8>(dst >> (static_cast<u32>(2 * i + 1) * 8u));
            const i8 sa = static_cast<i8>(static_cast<u8>(*src >> (static_cast<u32>(2 * i) * 8u)));
            const i8 sb = static_cast<i8>(static_cast<u8>(*src >> (static_cast<u32>(2 * i + 1) * 8u)));
            i32 sum = static_cast<i32>(da) * static_cast<i32>(sa) +
                      static_cast<i32>(db) * static_cast<i32>(sb);
            if (sum > 32767) sum = 32767;
            else if (sum < -32768) sum = -32768;
            result |= static_cast<u64>(static_cast<u16>(static_cast<i16>(sum)))
                      << (static_cast<u32>(i) * 16u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PMULHRSW_MM_MMM64: {
        // ((i32)a * (i32)b + 0x4000) >> 15, low 16 bits. Intel SDM Vol. 2.
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            const i16 a = static_cast<i16>(static_cast<u16>(dst >> (static_cast<u32>(i) * 16u)));
            const i16 b = static_cast<i16>(static_cast<u16>(*src >> (static_cast<u32>(i) * 16u)));
            const i32 prod = static_cast<i32>(a) * static_cast<i32>(b);
            const u16 r = static_cast<u16>(static_cast<i16>((prod + 0x4000) >> 15));
            result |= static_cast<u64>(r) << (static_cast<u32>(i) * 16u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PSIGNB_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 8; ++i) {
            const i8 d = static_cast<i8>(static_cast<u8>(dst >> (static_cast<u32>(i) * 8u)));
            const i8 s = static_cast<i8>(static_cast<u8>(*src >> (static_cast<u32>(i) * 8u)));
            i8 out = d;
            if (s < 0) out = static_cast<i8>(-static_cast<i32>(d));
            else if (s == 0) out = 0;
            result |= static_cast<u64>(static_cast<u8>(out)) << (static_cast<u32>(i) * 8u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PSIGNW_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            const i16 d = static_cast<i16>(static_cast<u16>(dst >> (static_cast<u32>(i) * 16u)));
            const i16 s = static_cast<i16>(static_cast<u16>(*src >> (static_cast<u32>(i) * 16u)));
            i16 out = d;
            if (s < 0) out = static_cast<i16>(-static_cast<i32>(d));
            else if (s == 0) out = 0;
            result |= static_cast<u64>(static_cast<u16>(out)) << (static_cast<u32>(i) * 16u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PSIGND_MM_MMM64: {
        const u64 dst = read_mm(ctx, op0.reg);
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 2; ++i) {
            const i32 d = static_cast<i32>(static_cast<u32>(dst >> (static_cast<u32>(i) * 32u)));
            const i32 s = static_cast<i32>(static_cast<u32>(*src >> (static_cast<u32>(i) * 32u)));
            i32 out = d;
            if (s < 0) out = static_cast<i32>(-static_cast<i64>(d));
            else if (s == 0) out = 0;
            result |= static_cast<u64>(static_cast<u32>(out)) << (static_cast<u32>(i) * 32u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PABSB_MM_MMM64: {
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 8; ++i) {
            const i8 v = static_cast<i8>(static_cast<u8>(*src >> (static_cast<u32>(i) * 8u)));
            const i8 a = (v < 0) ? static_cast<i8>(-static_cast<i32>(v)) : v;
            result |= static_cast<u64>(static_cast<u8>(a)) << (static_cast<u32>(i) * 8u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PABSW_MM_MMM64: {
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 4; ++i) {
            const i16 v = static_cast<i16>(static_cast<u16>(*src >> (static_cast<u32>(i) * 16u)));
            const i16 a = (v < 0) ? static_cast<i16>(-static_cast<i32>(v)) : v;
            result |= static_cast<u64>(static_cast<u16>(a)) << (static_cast<u32>(i) * 16u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    case Opcode::PABSD_MM_MMM64: {
        auto src = read_mmm64(ctx, op1);
        if (!src) {
            ctx.exception = CpuException::GeneralProtection;
            return ExecResult::Exception;
        }
        u64 result = 0;
        for (int i = 0; i < 2; ++i) {
            const i32 v = static_cast<i32>(static_cast<u32>(*src >> (static_cast<u32>(i) * 32u)));
            const i32 a = (v < 0) ? static_cast<i32>(-static_cast<i64>(v)) : v;
            result |= static_cast<u64>(static_cast<u32>(a)) << (static_cast<u32>(i) * 32u);
        }
        write_mm(ctx, op0.reg, result);
        return ExecResult::Continue;
    }

    default:
        ctx.exception = CpuException::InvalidOpcode;
        return ExecResult::Exception;
    }
}

} // namespace vitreo::cpu::ops
