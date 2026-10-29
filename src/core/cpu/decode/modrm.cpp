// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "modrm.hpp"

namespace vitreo::cpu {

i8 decode_modrm(
    ByteReader& reader,
    DecodeMode mode,
    const Prefixes& prefix,
    bool is_64bit_operand,
    u8 modrm,
    Operand& rm_operand,
    u8& reg_field,
    bool vsib
) noexcept {
    u8 mod = (modrm >> 6) & 0x03;
    reg_field = (modrm >> 3) & 0x07;
    u8 rm = modrm & 0x07;
    
    // Apply REX.R to reg field
    if (prefix.rex_r()) {
        reg_field |= 0x08;
    }
    
    // REX.B extends rm field
    if (prefix.rex_b()) {
        rm |= 0x08;
    }
    
    if (mod == 3) {
        // Register-direct mode
        if (is_64bit_operand) {
            rm_operand = Operand::make_reg(OperandType::Reg64, rm);
        } else if (prefix.operand_size && mode != DecodeMode::Mode16) {
            rm_operand = Operand::make_reg(OperandType::Reg16, rm);
        } else if (mode == DecodeMode::Mode16 && !prefix.operand_size) {
            rm_operand = Operand::make_reg(OperandType::Reg16, rm);
        } else {
            rm_operand = Operand::make_reg(OperandType::Reg32, rm);
        }
        return 0;
    }
    
    // Memory addressing mode
    MemoryOperand mem;
    mem.segment = prefix.segment;
    
    bool need_sib = false;
    i8 disp_size = 0;
    
    if (mode == DecodeMode::Mode64 || (mode == DecodeMode::Mode32 && !prefix.address_size) ||
        (mode == DecodeMode::Mode16 && prefix.address_size)) {
        // 32/64-bit addressing
        if ((rm & 0x07) == 4 && mod != 3) {
            // SIB byte follows
            need_sib = true;
        } else if ((rm & 0x07) == 5 && mod == 0) {
            // Special case: RIP-relative (64-bit) or disp32 only (32-bit)
            if (mode == DecodeMode::Mode64) {
                mem.base = 16;  // RIP-relative marker
            } else {
                mem.base = 255;  // No base
            }
            disp_size = 4;
        } else {
            mem.base = rm;
        }
        
        if (mod == 1) disp_size = 1;
        else if (mod == 2) disp_size = 4;
        
    } else {
        // 16-bit addressing (legacy, not fully implemented)
        return -1;
    }
    
    // Read SIB byte if needed
    if (need_sib) {
        u8 sib;
        if (!reader.read_u8(sib)) return -1;
        
        u8 ss = (sib >> 6) & 0x03;
        u8 idx = (sib >> 3) & 0x07;
        u8 base_sib = sib & 0x07;
        
        // REX.X extends index
        if (prefix.rex_x()) {
            idx |= 0x08;
        }
        // REX.B extends base
        if (prefix.rex_b()) {
            base_sib |= 0x08;
        }
        
        mem.scale = static_cast<u8>(1 << ss);
        
        if (idx == 4 && !vsib) {
            // No index register (RSP can't be GPR index). VSIB allows XMM4.
            mem.index = 255;
            mem.scale = 1;
        } else {
            mem.index = idx;
        }
        mem.vsib = vsib;
        
        if ((base_sib & 0x07) == 5) {
            if (mod == 0) {
                // No base, disp32
                mem.base = 255;
                disp_size = 4;
            } else {
                mem.base = base_sib;
            }
        } else {
            mem.base = base_sib;
        }
    }
    
    // Read displacement
    if (disp_size == 1) {
        i8 d8;
        if (!reader.read_i8(d8)) return -1;
        mem.disp = d8;
    } else if (disp_size == 4) {
        i32 d32;
        if (!reader.read_i32(d32)) return -1;
        mem.disp = d32;
    }
    
    // Determine memory operand type based on operand size
    OperandType mem_type;
    if (is_64bit_operand) {
        mem_type = OperandType::Mem64;
    } else if (prefix.operand_size && mode != DecodeMode::Mode16) {
        mem_type = OperandType::Mem16;
    } else if (mode == DecodeMode::Mode16 && !prefix.operand_size) {
        mem_type = OperandType::Mem16;
    } else {
        mem_type = OperandType::Mem32;
    }
    
    rm_operand = Operand::make_mem(mem_type, mem);
    return disp_size + (need_sib ? 1 : 0);
}

u8 default_operand_size(DecodeMode mode, const Prefixes& prefix) noexcept {
    if (prefix.rex_w()) return 8;
    if (mode == DecodeMode::Mode64) {
        return prefix.operand_size ? 2 : 4;
    } else if (mode == DecodeMode::Mode32) {
        return prefix.operand_size ? 2 : 4;
    } else {
        return prefix.operand_size ? 4 : 2;
    }
}

} // namespace vitreo::cpu
