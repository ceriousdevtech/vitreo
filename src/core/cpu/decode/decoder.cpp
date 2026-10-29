// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "decoder.hpp"
#include "modrm.hpp"
#include "opcodes/decode_ops.hpp"

namespace vitreo::cpu {

DecodeError decode_instruction(
    ByteReader& reader, 
    DecodeMode mode,
    DecodedInstruction& out
) noexcept {
    out = DecodedInstruction{};
    usize start_pos = reader.position();
    
    // Phase 1: Read prefixes
    Prefixes& prefix = out.prefixes;
    u8 byte;
    
    while (true) {
        if (!reader.read_u8(byte)) {
            return DecodeError::UnexpectedEnd;
        }
        
        // Check for legacy prefixes
        switch (byte) {
            case 0xF0: prefix.lock = true; continue;
            case 0xF2: prefix.repne = true; continue;
            case 0xF3: prefix.rep = true; continue;
            case 0x66: prefix.operand_size = true; continue;
            case 0x67: prefix.address_size = true; continue;
            case 0x26: prefix.segment = SegReg::ES; continue;
            case 0x2E: prefix.segment = SegReg::CS; continue;
            case 0x36: prefix.segment = SegReg::SS; continue;
            case 0x3E: prefix.segment = SegReg::DS; continue;
            case 0x64: prefix.segment = SegReg::FS; continue;
            case 0x65: prefix.segment = SegReg::GS; continue;
            default: break;
        }
        
        // Check for REX prefix (64-bit mode only, 0x40-0x4F)
        if (mode == DecodeMode::Mode64 && (byte & 0xF0) == 0x40) {
            prefix.rex = byte;
            continue;
        }
        
        // Not a prefix, this is the opcode (or VEX)
        break;
    }

    bool is_64bit_operand = prefix.rex_w();
    u8 op_size = default_operand_size(mode, prefix);
    
    // VEX: C4 (3-byte) / C5 (2-byte). In 32-bit mode these encodings are
    // LES/LDS when ModRM.mod != 11b (Intel SDM Vol. 2, VEX).
    bool is_vex = false;
    u8 vex_map = 0;  // 1 = 0F, 2 = 0F38, 3 = 0F3A

    // EVEX (Intel SDM Vol. 2, 2.6): 62 P0 P1 P2 <opcode> <modrm> ...
    //
    // The escape byte is shared with BOUND, and disambiguated the same way
    // C4/C5 are disambiguated from LES/LDS: BOUND requires a memory operand,
    // so the mod==11 encodings are free for the prefix. In 64-bit mode BOUND
    // does not exist and 62 is always EVEX.
    //
    // Vitreo advertises no EVEX-encoded ISA -- AVX-512, AVX10, APX and AMX
    // are all hidden -- so every one of these raises #UD, which is what a
    // processor without those features does. Decoding it anyway buys two
    // things a bare "invalid opcode" does not:
    //
    //   * the #UD is diagnosable. "An EVEX instruction we do not implement"
    //     and "corrupt bytes" are the same event to a decode failure, and
    //     they call for opposite responses.
    //   * the LENGTH is right. Without this, only the escape byte is
    //     consumed and the instruction reports as one byte long.
    if (byte == 0x62) {
        u8 p0;
        if (!reader.read_u8(p0)) return DecodeError::UnexpectedEnd;
        const bool evex_candidate =
            (mode == DecodeMode::Mode64) || ((p0 & 0xC0) == 0xC0);
        if (evex_candidate) {
            return decode::decode_evex(reader, mode, prefix, out, p0,
                                       start_pos);
        }
        // BOUND r, m — 16/32-bit only, memory operand required.
        Operand rm;
        u8 reg_field;
        Prefixes tmp = prefix;
        if (decode_modrm(reader, mode, tmp, false, p0, rm, reg_field) < 0) {
            return DecodeError::InvalidModRM;
        }
        if (is_register_operand(rm.type)) {
            return DecodeError::InvalidOpcode;
        }
        const bool word = (op_size == 2);
        rm.type = word ? OperandType::Mem32 : OperandType::Mem64;
        out.opcode = word ? Opcode::BOUND_R16_M32 : Opcode::BOUND_R32_M64;
        out.add_operand(Operand::make_reg(
            word ? OperandType::Reg16 : OperandType::Reg32, reg_field));
        out.add_operand(rm);
        out.length = static_cast<u8>(reader.position() - start_pos);
        return DecodeError::Success;
    }

    if (byte == 0xC4 || byte == 0xC5) {
        u8 b1;
        if (!reader.read_u8(b1)) {
            return DecodeError::UnexpectedEnd;
        }
        const bool vex_candidate =
            (mode == DecodeMode::Mode64) || ((b1 & 0xC0) == 0xC0);
        if (!vex_candidate) {
            // LES (C4) / LDS (C5) — 32-bit far-pointer load
            Operand rm;
            u8 reg_field;
            Prefixes tmp = prefix;
            if (decode_modrm(reader, mode, tmp, false, b1, rm, reg_field) < 0) {
                return DecodeError::InvalidModRM;
            }
            if (is_register_operand(rm.type)) {
                return DecodeError::InvalidOpcode;
            }
            const bool word = (op_size == 2);
            rm.type = word ? OperandType::Mem32 : OperandType::MemUnknown;
            out.opcode = (byte == 0xC4)
                ? (word ? Opcode::LES_R16_M32 : Opcode::LES_R32_M48)
                : (word ? Opcode::LDS_R16_M32 : Opcode::LDS_R32_M48);
            out.add_operand(Operand::make_reg(
                word ? OperandType::Reg16 : OperandType::Reg32, reg_field));
            out.add_operand(rm);
            out.length = static_cast<u8>(reader.position() - start_pos);
            return DecodeError::Success;
        }
        is_vex = true;
        prefix.vex = true;
        if (byte == 0xC5) {
            // C5 [R̅ vvvv L pp]
            prefix.rex = static_cast<u8>(0x40 | (((b1 & 0x80) == 0) ? 0x04 : 0));
            prefix.vex_vvvv = static_cast<u8>((~b1 >> 3) & 0x0F);
            prefix.vex_l = (b1 & 0x04) != 0;
            const u8 pp = b1 & 0x03;
            if (pp == 1) prefix.operand_size = true;
            else if (pp == 2) prefix.rep = true;
            else if (pp == 3) prefix.repne = true;
            vex_map = 1;
        } else {
            // C4 [R̅ X̅ B̅ mmmmm] [W vvvv L pp]
            u8 b2;
            if (!reader.read_u8(b2)) {
                return DecodeError::UnexpectedEnd;
            }
            u8 rex = 0x40;
            if ((b1 & 0x80) == 0) rex |= 0x04;  // R
            if ((b1 & 0x40) == 0) rex |= 0x02;  // X
            if ((b1 & 0x20) == 0) rex |= 0x01;  // B
            if (b2 & 0x80) rex |= 0x08;         // W
            prefix.rex = rex;
            prefix.vex_vvvv = static_cast<u8>((~b2 >> 3) & 0x0F);
            prefix.vex_l = (b2 & 0x04) != 0;
            const u8 pp = b2 & 0x03;
            if (pp == 1) prefix.operand_size = true;
            else if (pp == 2) prefix.rep = true;
            else if (pp == 3) prefix.repne = true;
            vex_map = b1 & 0x1F;
            if (vex_map < 1 || vex_map > 3) {
                return DecodeError::InvalidOpcode;
            }
            // VEX.W outside long mode.
            //
            // In map 1 (the 0F map) VEX.W is never an element-size selector:
            // it is either ignored or, on the instructions that take a GPR
            // operand (VMOVD/VMOVQ, VCVTSI2SS/SD, VCVTSS2SI/VCVTSD2SI), an
            // operand-size promotion exactly like REX.W — and like REX.W it
            // does not exist outside 64-bit mode (Intel SDM Vol. 2A §2.3.5.1).
            // Honouring it made `C4 E1 F9 6E C1` decode as `MOVQ xmm, r64` in
            // a 32-bit block, an encoding no 32-bit guest can execute.
            //
            // Maps 2 and 3 are deliberately untouched: there W selects ELEMENT
            // size on the gathers (0F38 90-93) and the whole FMA family
            // (0F38 96-BF), which are valid in 32-bit mode, so clearing it
            // would silently turn every double-precision FMA into a
            // single-precision one. The GPR instructions in those maps (the
            // BMI1/BMI2 families, and RORX in map 3) gate on mode at their own
            // decode sites instead.
            if (mode != DecodeMode::Mode64 && vex_map == 1) {
                prefix.rex = static_cast<u8>(prefix.rex & ~0x08u);
            }
        }
        if (!reader.read_u8(byte)) {
            return DecodeError::UnexpectedEnd;
        }
        is_64bit_operand = prefix.rex_w();
        op_size = default_operand_size(mode, prefix);
    }

    // Phase 2: Decode opcode
    
    // Handle escape bytes
    bool is_two_byte = false;
    bool is_three_byte_38 = false;
    bool is_three_byte_3a = false;
    if (is_vex) {
        if (vex_map == 2) is_three_byte_38 = true;
        else if (vex_map == 3) is_three_byte_3a = true;
        else is_two_byte = true;
    } else if (byte == 0x0F) {
        is_two_byte = true;
        if (!reader.read_u8(byte)) {
            return DecodeError::UnexpectedEnd;
        }
        // Check for three-byte escape: 0F 38 XX or 0F 3A XX
        if (byte == 0x38) {
            is_two_byte = false;
            is_three_byte_38 = true;
            if (!reader.read_u8(byte)) {
                return DecodeError::UnexpectedEnd;
            }
        } else if (byte == 0x3A) {
            is_two_byte = false;
            is_three_byte_3a = true;
            if (!reader.read_u8(byte)) {
                return DecodeError::UnexpectedEnd;
            }
        }
    }
    
    // Create decode context
    decode::DecodeContext ctx(reader, mode, prefix, out, byte);
    // (LOCK legality is checked after the opcode is known; see below.)
    DecodeError result = DecodeError::InvalidOpcode;

    // A VEX-prefixed instruction in any map is decoded by first running the
    // LEGACY decoder for that map, and nothing in that path checks the opcode
    // has a VEX encoding at all. Every real VEX instruction reached this way
    // names an XMM/YMM or 128/256-bit memory operand, so the vector operand is
    // the legality test — see the fuller comment on the two-byte branch below.
    //
    // In maps 2 and 3 the legacy-only strays are the GPR instructions that
    // share those maps with SSSE3/SSE4: MOVBE and CRC32. `C4 E2 7D F1 03`
    // decoded as `MOVBE m16, r16`. They are rejected here rather than falling
    // through to decode_vex_38/3a, which own the genuine VEX GPR families
    // (BMI1/BMI2, RORX) and are reached only when the legacy decoder declines
    // — no opcode is claimed by both.
    const auto vex_shape_is_legal = [&]() {
        for (u8 i = 0; i < out.operand_count; ++i) {
            const OperandType t = out.operands[i].type;
            if (t == OperandType::Reg128 || t == OperandType::Mem128 ||
                t == OperandType::Mem256) {
                return true;
            }
        }
        return false;
    };

    // Shared legality for a VEX-prefixed legacy opcode in maps 2 and 3.
    // The vector-operand shape test is the same as map 1's. The two opcode
    // predicates were only consulted on the map-1 path before, so
    // `VINSERTPS`/`VPEXTRB`/`VAESENC`/`VPCLMULQDQ`/`VPCMPISTRI` with VEX.L=1
    // decoded here and faulted only when executed (the ISA matrix counted
    // each as a missing 256-bit lowering), and a VEX prefix on the SHA
    // opcodes -- which have no VEX form at all -- decoded and RAN. Both
    // measured on silicon: tools/oracle_probe/vex_scalar_round_probe.c.
    const auto vex_legacy_map23_is_legal = [&]() {
        if (!vex_shape_is_legal()) return false;
        if (opcode_has_no_vex_form(out.opcode)) return false;
        if (prefix.vex_l && vex_l_is_reserved(out.opcode)) return false;
        return true;
    };

    if (is_three_byte_38) {
        result = decode::decode_three_byte_38(ctx);
        if (result == DecodeError::Success && prefix.vex &&
            !vex_legacy_map23_is_legal()) {
            return DecodeError::InvalidOpcode;
        }
        if (result == DecodeError::InvalidOpcode && prefix.vex) {
            result = decode::decode_vex_38(ctx);
        }
    } else if (is_three_byte_3a) {
        result = decode::decode_three_byte_3a(ctx);
        if (result == DecodeError::Success && prefix.vex &&
            !vex_legacy_map23_is_legal()) {
            return DecodeError::InvalidOpcode;
        }
        if (result == DecodeError::InvalidOpcode && prefix.vex) {
            result = decode::decode_vex_3a(ctx);
        }
    } else if (is_two_byte) {
        result = decode::decode_two_byte(ctx);
        if (result == DecodeError::Success && prefix.vex) {
            // VEX map 1 is the SSE/AVX map, and a VEX-prefixed instruction is
            // decoded here by running the LEGACY two-byte decoder. Nothing in
            // that path checks that the opcode has a VEX encoding at all, so
            // `C4 E1 F8 B6 C1` came back as `MOVZX r64, r/m8` — an encoding
            // that is #UD on hardware (Intel SDM Vol. 2A §2.3). MOVSX and
            // LFS/LGS/LSS decoded the same way. A guest executing garbage then
            // gets a silent wrong execution instead of a fault.
            //
            // Every instruction that genuinely has a VEX map-1 encoding names
            // an XMM/YMM register or a 128/256-bit memory operand — including
            // the ones that also take a GPR (VMOVD/VMOVQ, VCVTSI2SS/SD,
            // VCVTSS2SI, VPMOVMSKB, VPEXTRW), which all keep their vector
            // operand. So the vector operand IS the legality test, and it needs
            // no second hand-written opcode list to fall out of date.
            //
            // The exceptions have no vector operand by construction:
            // VZEROUPPER/VZEROALL take none; VLDMXCSR/VSTMXCSR take a 32-bit
            // memory operand; and the scalar convert-to-GPR forms
            // (VCVTSS2SI/VCVTSD2SI and their truncating twins) are
            // `r32/r64, xmm/m32` — so their MEMORY form names a GPR and a
            // 32/64-bit memory operand and nothing vector at all.
            //
            // This list is bounded and self-guarding, unlike the coverage
            // allowlists this program is removing: if a form is missing from
            // it the decoder REJECTS a legal encoding, and
            // VexAliasRouting.EveryAdvertisedVexSseFormReachesTheAvxBridge —
            // which is driven from CPUID — fails immediately. It is what
            // caught the convert family here. MMX is deliberately NOT
            // accepted: there is no VEX encoding of an MMX instruction.
            bool has_vector_operand = false;
            bool has_mmx_operand = false;
            for (u8 i = 0; i < out.operand_count; ++i) {
                const OperandType t = out.operands[i].type;
                if (t == OperandType::Reg128 || t == OperandType::Mem128 ||
                    t == OperandType::Mem256) {
                    has_vector_operand = true;
                } else if (t == OperandType::RegMM) {
                    has_mmx_operand = true;
                }
            }
            // No MMX instruction has a VEX encoding — the VEX prefix was
            // introduced alongside the decision to leave MMX behind, and the
            // SDM lists no VEX form for any of them. Vitreo decoded
            // `C4 E1 78 2D C1` as `CVTPS2PI mm, xmm/m64`, and the whole
            // MMX half of SSSE3 (PSHUFB mm, PHADDW mm, …) the same way.
            //
            // This is checked separately from the vector-operand test above
            // because the MMX conversions name an XMM register TOO, so they
            // satisfy that test while still having no VEX encoding.
            if (has_mmx_operand) {
                return DecodeError::InvalidOpcode;
            }
            const bool operand_free_vex =
                out.opcode == Opcode::VZEROUPPER ||
                out.opcode == Opcode::VZEROALL ||
                out.opcode == Opcode::LDMXCSR_M32 ||
                out.opcode == Opcode::STMXCSR_M32 ||
                out.opcode == Opcode::CVTSS2SI_R32_XMMM32 ||
                out.opcode == Opcode::CVTSS2SI_R64_XMMM32 ||
                out.opcode == Opcode::CVTSD2SI_R32_XMMM64 ||
                out.opcode == Opcode::CVTSD2SI_R64_XMMM64 ||
                out.opcode == Opcode::CVTTSS2SI_R32_XMMM32 ||
                out.opcode == Opcode::CVTTSS2SI_R64_XMMM32 ||
                out.opcode == Opcode::CVTTSD2SI_R32_XMMM64 ||
                out.opcode == Opcode::CVTTSD2SI_R64_XMMM64;
            if (!has_vector_operand && !operand_free_vex) {
                return DecodeError::InvalidOpcode;
            }
            // VEX.L=1 on a scalar or otherwise 128-bit-only instruction is
            // reserved and #UDs on hardware (Intel SDM Vol. 2A §2.3.6.1).
            // Vitreo decoded it, the AVX bridge raised #UD at EXECUTION, and
            // the ISA matrix therefore counted every one as a missing 256-bit
            // lowering — 212 mnemonics' worth of phantom work. The fault
            // belongs at decode.
            if (prefix.vex_l && vex_l_is_reserved(out.opcode)) {
                return DecodeError::InvalidOpcode;
            }
        }
    } else {
        // Dispatch to per-category handlers based on opcode byte
        // MOV opcodes: 88-8B, B0-BF, A0-A3, C6, C7
        // Plus 8C (MOV r/m16, Sreg) and 8E (MOV Sreg, r/m16)
        if ((byte >= 0x88 && byte <= 0x8B) || byte == 0x8C || byte == 0x8E ||
            (byte >= 0xB0 && byte <= 0xBF) ||
            (byte >= 0xA0 && byte <= 0xA3) || byte == 0xC6 || byte == 0xC7) {
            result = decode::decode_mov(ctx);
        }
        // ADD opcodes: 00-05
        else if (byte >= 0x00 && byte <= 0x05) {
            result = decode::decode_add(ctx);
        }
        // OR opcodes: 08-0D
        else if (byte >= 0x08 && byte <= 0x0D) {
            result = decode::decode_or(ctx);
        }
        // AND opcodes: 20-25
        else if (byte >= 0x20 && byte <= 0x25) {
            result = decode::decode_and(ctx);
        }
        // SUB opcodes: 28-2D
        else if (byte >= 0x28 && byte <= 0x2D) {
            result = decode::decode_sub(ctx);
        }
        // XOR opcodes: 30-35
        else if (byte >= 0x30 && byte <= 0x35) {
            result = decode::decode_xor(ctx);
        }
        // CMP opcodes: 38-3D
        else if (byte >= 0x38 && byte <= 0x3D) {
            result = decode::decode_cmp(ctx);
        }
        // Group 1 opcodes: 80-83 (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m, imm)
        else if (byte >= 0x80 && byte <= 0x83) {
            result = decode::decode_group1(ctx);
        }
        // TEST opcodes: 84, 85, A8, A9
        else if (byte == 0x84 || byte == 0x85 || byte == 0xA8 || byte == 0xA9) {
            result = decode::decode_test(ctx);
        }
        // INC r32 (40-47) / DEC r32 (48-4F) — 32-bit mode only
        // (In 64-bit mode these bytes are REX prefixes, consumed above)
        else if (byte >= 0x40 && byte <= 0x4F) {
            u8 reg = byte & 0x07;
            if (byte <= 0x47) {
                // INC r32
                if (op_size == 2) {
                    out.opcode = Opcode::INC_RM16;
                    out.add_operand(Operand::make_reg(OperandType::Reg16, reg));
                } else {
                    out.opcode = Opcode::INC_RM32;
                    out.add_operand(Operand::make_reg(OperandType::Reg32, reg));
                }
            } else {
                // DEC r32
                if (op_size == 2) {
                    out.opcode = Opcode::DEC_RM16;
                    out.add_operand(Operand::make_reg(OperandType::Reg16, reg));
                } else {
                    out.opcode = Opcode::DEC_RM32;
                    out.add_operand(Operand::make_reg(OperandType::Reg32, reg));
                }
            }
            result = DecodeError::Success;
        }
        // PUSH opcodes: segment registers, 50-57, 68, 6A
        else if (byte == 0x06 || byte == 0x0E || byte == 0x16 || byte == 0x1E ||
                 (byte >= 0x50 && byte <= 0x57) || byte == 0x68 || byte == 0x6A) {
            result = decode::decode_push(ctx);
        }
        // PUSHA/PUSHAD (60), POPA/POPAD (61) — Intel SDM Vol. 2 PUSHA/POPA:
        // "64-bit Mode: Invalid". Decoding them in long mode let the
        // interpreter execute a 32-bit PUSHAD/POPAD against a 64-bit stack,
        // silently writing eight 4-byte slots and truncating seven registers
        // on the way back, where the guest should have taken #UD
        // (STATUS_ILLEGAL_INSTRUCTION). x64 code that probes for #UD relies on
        // the fault.
        else if (byte == 0x60 || byte == 0x61) {
            if (mode == DecodeMode::Mode64) {
                result = DecodeError::InvalidOpcode;
            } else {
                out.opcode = (byte == 0x60) ? Opcode::PUSHA : Opcode::POPA;
                result = DecodeError::Success;
            }
        }
        // POP opcodes: segment registers and 58-5F
        else if (byte == 0x07 || byte == 0x17 || byte == 0x1F ||
                 (byte >= 0x58 && byte <= 0x5F)) {
            result = decode::decode_pop(ctx);
        }
        // JMP opcodes: E9, EB
        else if (byte == 0xE9 || byte == 0xEB) {
            result = decode::decode_jmp(ctx);
        }
        // Jcc rel8 opcodes: 70-7F
        else if (byte >= 0x70 && byte <= 0x7F) {
            result = decode::decode_jcc_rel8(ctx);
        }
        // LOOP/JCXZ family: E0-E3
        else if (byte >= 0xE0 && byte <= 0xE3) {
            result = decode::decode_loop_jcxz(ctx);
        }
        // CALL/RET opcodes: C2, C3, E8
        else if (byte == 0xC2 || byte == 0xC3 || byte == 0xE8) {
            result = decode::decode_call_ret(ctx);
        }
        // ENTER: C8
        else if (byte == 0xC8) {
            result = decode::decode_enter(ctx);
        }
        // LEAVE: C9
        else if (byte == 0xC9) {
            result = decode::decode_leave(ctx);
        }
        // RETF (CB) / RETF imm16 (CA iw) — Intel SDM Vol. 2 RET
        else if (byte == 0xCB) {
            out.opcode = Opcode::RET_FAR;
            result = DecodeError::Success;
        }
        else if (byte == 0xCA) {
            u16 imm;
            if (!reader.read_u16(imm)) {
                return DecodeError::UnexpectedEnd;
            }
            out.opcode = Opcode::RET_FAR_IMM16;
            out.add_operand(Operand::make_imm(OperandType::Imm16, imm));
            result = DecodeError::Success;
        }
        // IRET/IRETD/IRETQ (CF) — Intel SDM Vol. 2. Operand size selects the
        // stack slot; 64-bit mode is IRETQ (user-mode same-privilege).
        else if (byte == 0xCF) {
            if (mode == DecodeMode::Mode64) {
                out.opcode = Opcode::IRETQ;
            } else if (prefix.operand_size) {
                out.opcode = Opcode::IRET;
            } else {
                out.opcode = Opcode::IRETD;
            }
            result = DecodeError::Success;
        }
        // Far JMP (EA cd/cp) / far CALL (9A cd/cp) — Intel SDM Vol. 2 JMP/CALL.
        // ptr16:16 (66H) or ptr16:32. #UD in 64-bit mode.
        else if (byte == 0xEA || byte == 0x9A) {
            if (mode == DecodeMode::Mode64) {
                result = DecodeError::InvalidOpcode;
            } else if (op_size == 2) {
                u16 offset, selector;
                if (!reader.read_u16(offset) || !reader.read_u16(selector)) {
                    return DecodeError::UnexpectedEnd;
                }
                out.opcode = (byte == 0xEA) ? Opcode::JMP_PTR16_16
                                            : Opcode::CALL_PTR16_16;
                out.add_operand(Operand::make_imm(OperandType::Imm16, offset));
                out.add_operand(Operand::make_imm(OperandType::Imm16, selector));
                result = DecodeError::Success;
            } else {
                u32 offset;
                u16 selector;
                if (!reader.read_u32(offset) || !reader.read_u16(selector)) {
                    return DecodeError::UnexpectedEnd;
                }
                out.opcode = (byte == 0xEA) ? Opcode::JMP_PTR16_32
                                            : Opcode::CALL_PTR16_32;
                out.add_operand(Operand::make_imm(OperandType::Imm32, offset));
                out.add_operand(Operand::make_imm(OperandType::Imm16, selector));
                result = DecodeError::Success;
            }
        }
        // LEA opcode: 8D
        else if (byte == 0x8D) {
            result = decode::decode_lea(ctx);
        }
        // INT opcodes: CC, CD, CE (INTO)
        else if (byte == 0xCC || byte == 0xCD || byte == 0xCE) {
            result = decode::decode_int(ctx);
        }
        // NOP/XCHG: 90-97
        else if (byte >= 0x90 && byte <= 0x97) {
            result = decode::decode_nop_xchg(ctx);
        }
        // PUSHFD: 9C
        else if (byte == 0x9C) {
            out.opcode = Opcode::PUSHF;
            result = DecodeError::Success;
        }
        // POPFD: 9D
        else if (byte == 0x9D) {
            out.opcode = Opcode::POPF;
            result = DecodeError::Success;
        }
        // HLT: F4
        else if (byte == 0xF4) {
            out.opcode = Opcode::HLT;
            result = DecodeError::Success;
        }
        // INT1 / ICEBP: F1. Undocumented by Intel as a mnemonic but
        // architecturally present on every implementation, and the encoding
        // debuggers and anti-debug code actually use. It raises #DB directly,
        // unlike INT 01 (CD 01) which goes through the IDT gate.
        else if (byte == 0xF1) {
            out.opcode = Opcode::INT_1;
            result = DecodeError::Success;
        }
        // SALC / SETALC: D6. Undocumented, valid in 16/32-bit modes only,
        // #UD in 64-bit mode. AL = CF ? 0xFF : 0x00, no flags affected.
        else if (byte == 0xD6) {
            if (mode == DecodeMode::Mode64) {
                result = DecodeError::InvalidOpcode;
            } else {
                out.opcode = Opcode::SALC;
                result = DecodeError::Success;
            }
        }
        // Group 3: F6, F7 (TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m)
        else if (byte == 0xF6 || byte == 0xF7) {
            result = decode::decode_group3(ctx);
        }
        // POP r/m: 8F /0
        else if (byte == 0x8F) {
            result = decode::decode_pop_rm(ctx);
        }
        // Group 5: FF (INC/DEC/CALL/JMP/PUSH r/m)
        else if (byte == 0xFF) {
            result = decode::decode_group5(ctx);
        }
        // Shift/Rotate Group 2: C0, C1, D0, D1, D2, D3
        else if (byte == 0xC0 || byte == 0xC1 || byte == 0xD0 ||
                 byte == 0xD1 || byte == 0xD2 || byte == 0xD3) {
            result = decode::decode_shifts(ctx);
        }
        // XCHG r/m, r: 86, 87
        else if (byte == 0x86 || byte == 0x87) {
            result = decode::decode_xchg_rm(ctx);
        }
        // MOVSXD: 63
        else if (byte == 0x63) {
            result = decode::decode_movsxd(ctx);
        }
        // String operations: A4-A7 (MOVS/CMPS), AA-AF (STOS/LODS/SCAS)
        else if ((byte >= 0xA4 && byte <= 0xA7) ||
                 (byte >= 0xAA && byte <= 0xAF)) {
            result = decode::decode_string_ops(ctx);
        }
        // XLAT: AL := byte at DS:[(E/R)BX + unsigned AL].
        else if (byte == 0xD7) {
            if (prefix.lock) {
                result = DecodeError::InvalidPrefix;
            } else {
                out.opcode = Opcode::XLAT;
                result = DecodeError::Success;
            }
        }
        // Flag manipulation: CLC(F8), STC(F9), CMC(F5), CLD(FC), STD(FD)
        else if (byte == 0xF8) {
            out.opcode = Opcode::CLC;
            result = DecodeError::Success;
        }
        else if (byte == 0xF9) {
            out.opcode = Opcode::STC;
            result = DecodeError::Success;
        }
        else if (byte == 0xF5) {
            out.opcode = Opcode::CMC;
            result = DecodeError::Success;
        }
        else if (byte == 0xFC) {
            out.opcode = Opcode::CLD;
            result = DecodeError::Success;
        }
        else if (byte == 0xFD) {
            out.opcode = Opcode::STD;
            result = DecodeError::Success;
        }
        // LAHF(9F), SAHF(9E)
        else if (byte == 0x9F) {
            out.opcode = Opcode::LAHF;
            result = DecodeError::Success;
        }
        else if (byte == 0x9E) {
            out.opcode = Opcode::SAHF;
            result = DecodeError::Success;
        }
        // ADC opcodes: 10-15
        else if (byte >= 0x10 && byte <= 0x15) {
            result = decode::decode_adc(ctx);
        }
        // SBB opcodes: 18-1D
        else if (byte >= 0x18 && byte <= 0x1D) {
            result = decode::decode_sbb(ctx);
        }
        // IMUL three-operand: 69, 6B
        else if (byte == 0x69 || byte == 0x6B) {
            result = decode::decode_imul_three_op(ctx);
        }
        // Sign extension: 98 (CBW/CWDE/CDQE), 99 (CWD/CDQ/CQO)
        else if (byte == 0x98 || byte == 0x99) {
            result = decode::decode_sign_extend(ctx);
        }
        // Group 4: FE (INC/DEC r/m8)
        else if (byte == 0xFE) {
            result = decode::decode_group4(ctx);
        }
        // x87 FPU escape bytes: D8-DF
        else if (byte >= 0xD8 && byte <= 0xDF) {
            result = decode::decode_fpu(ctx);
        }
        // FWAIT: 9B
        else if (byte == 0x9B) {
            out.opcode = Opcode::FWAIT;
            result = DecodeError::Success;
        }
        // Privileged I/O: INS/OUTS (6C-6F)
        else if (byte == 0x6C) { out.opcode = Opcode::INSB;    result = DecodeError::Success; }
        else if (byte == 0x6D) { out.opcode = Opcode::INSW_D;  result = DecodeError::Success; }
        else if (byte == 0x6E) { out.opcode = Opcode::OUTSB;   result = DecodeError::Success; }
        else if (byte == 0x6F) { out.opcode = Opcode::OUTSW_D; result = DecodeError::Success; }
        // Privileged I/O: IN/OUT with imm8 (E4-E7) - consume imm8 for correct length
        else if (byte == 0xE4) { u8 dummy; if (!reader.read_u8(dummy)) return DecodeError::UnexpectedEnd; out.opcode = Opcode::IN_AL_IMM8;   result = DecodeError::Success; }
        else if (byte == 0xE5) { u8 dummy; if (!reader.read_u8(dummy)) return DecodeError::UnexpectedEnd; out.opcode = Opcode::IN_EAX_IMM8;  result = DecodeError::Success; }
        else if (byte == 0xE6) { u8 dummy; if (!reader.read_u8(dummy)) return DecodeError::UnexpectedEnd; out.opcode = Opcode::OUT_IMM8_AL;   result = DecodeError::Success; }
        else if (byte == 0xE7) { u8 dummy; if (!reader.read_u8(dummy)) return DecodeError::UnexpectedEnd; out.opcode = Opcode::OUT_IMM8_EAX;  result = DecodeError::Success; }
        // Privileged I/O: IN/OUT with DX (EC-EF)
        else if (byte == 0xEC) { out.opcode = Opcode::IN_AL_DX;  result = DecodeError::Success; }
        else if (byte == 0xED) { out.opcode = Opcode::IN_EAX_DX; result = DecodeError::Success; }
        else if (byte == 0xEE) { out.opcode = Opcode::OUT_DX_AL; result = DecodeError::Success; }
        else if (byte == 0xEF) { out.opcode = Opcode::OUT_DX_EAX; result = DecodeError::Success; }
        // CLI / STI (FA, FB)
        else if (byte == 0xFA) { out.opcode = Opcode::CLI; result = DecodeError::Success; }
        else if (byte == 0xFB) { out.opcode = Opcode::STI; result = DecodeError::Success; }
        // Decimal adjusts / ASCII adjusts — #UD in 64-bit (Intel SDM Vol. 2)
        else if (byte == 0x27 || byte == 0x2F || byte == 0x37 || byte == 0x3F) {
            if (mode == DecodeMode::Mode64) {
                result = DecodeError::InvalidOpcode;
            } else {
                if (byte == 0x27) out.opcode = Opcode::DAA;
                else if (byte == 0x2F) out.opcode = Opcode::DAS;
                else if (byte == 0x37) out.opcode = Opcode::AAA;
                else out.opcode = Opcode::AAS;
                result = DecodeError::Success;
            }
        }
        else if (byte == 0xD4 || byte == 0xD5) {
            if (mode == DecodeMode::Mode64) {
                result = DecodeError::InvalidOpcode;
            } else {
                u8 imm;
                if (!reader.read_u8(imm)) return DecodeError::UnexpectedEnd;
                out.opcode = (byte == 0xD4) ? Opcode::AAM : Opcode::AAD;
                out.add_operand(Operand::make_imm(OperandType::Imm8, imm));
                result = DecodeError::Success;
            }
        }
    }
    
    if (result != DecodeError::Success) {
        return result;
    }

    // VEX.LIG (Intel SDM Vol. 2A §3.1.1.2): on the scalar forms VEX.L is
    // ignored, and silicon executes an L=1 encoding exactly as L=0
    // (tools/oracle_probe/vex_scalar_round_probe.c). Normalise it here so
    // that every consumer -- the AVX bridge, the JIT's scalar arms, the ISA
    // matrix's form key -- sees the one form that exists. Before this the
    // JIT declined every such encoding as "a 256-bit form it cannot split"
    // and the bridge faulted it.
    if (prefix.vex && prefix.vex_l && vex_l_is_ignored(out.opcode)) {
        prefix.vex_l = false;
    }

    // LOCK legality (Intel SDM Vol. 2, LOCK). The prefix is defined only on a
    // specific set of read-modify-write instructions, and only when the
    // DESTINATION is a memory operand; anywhere else it is #UD. Vitreo
    // accepted it on everything, so `LOCK CMP`, `LOCK MOV` and even
    // `LOCK FST m32` decoded and executed where hardware faults.
    if (prefix.lock) {
        const bool dest_is_memory =
            out.operand_count > 0 && is_memory_operand(out.operands[0].type);
        if (!dest_is_memory || !lock_prefix_is_legal(out.opcode)) {
            return DecodeError::InvalidOpcode;
        }
    }

    // Calculate instruction length
    out.length = static_cast<u8>(reader.position() - start_pos);
    
    return DecodeError::Success;
}

} // namespace vitreo::cpu
