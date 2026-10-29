// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SSE idiom detection for JIT fusion (CPU-041)

#include "idiom_detector.hpp"

namespace vitreo::cpu::jit {

namespace {

bool has_no_prefixes(const DecodedInstruction& instr) noexcept {
    const auto& p = instr.prefixes;
    return !p.lock && !p.rep && !p.repne && !p.operand_size &&
           !p.address_size && p.segment == SegReg::Default &&
           !p.has_rex();
}

bool is_sse_load128(Opcode op) noexcept {
    switch (op) {
    case Opcode::MOVDQA_XMM_XMMM128:
    case Opcode::MOVDQU_XMM_XMMM128:
    case Opcode::MOVAPS_XMM_XMMM128:
    case Opcode::MOVUPS_XMM_XMMM128:
        return true;
    default:
        return false;
    }
}

bool is_sse_store128(Opcode op) noexcept {
    switch (op) {
    case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVDQU_XMMM128_XMM:
    case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVUPS_XMMM128_XMM:
    case Opcode::MOVNTPS_M128_XMM:
    case Opcode::MOVNTPD_M128_XMM:
        return true;
    default:
        return false;
    }
}

/// Try to match an SSE memcpy pattern starting at index `start`.
/// Pattern: N loads from [base1+disp] into distinct XMMs,
///          followed by N stores from those XMMs to [base2+disp],
///          with sequential 16-byte displacements.
/// Returns match on success, or nullopt.
bool try_match_memcpy(const BasicBlock& block, u32 start,
                      IdiomMatch& out) noexcept {
    const u32 count = block.instruction_count;
    if (start >= count) return false;

    // Count consecutive SSE loads from memory
    u32 load_count = 0;
    u8 src_base = 0xFF;
    i32 first_src_disp = 0;

    for (u32 i = start; i < count; ++i) {
        const auto& instr = block.instructions[i];
        if (!is_sse_load128(instr.opcode)) break;
        if (instr.operand_count < 2) break;
        if (!is_memory_operand(instr.operands[1].type)) break;

        const auto& mem = instr.operands[1].mem;
        if (mem.index != 255) break;

        if (load_count == 0) {
            src_base = mem.base;
            first_src_disp = mem.disp;
        } else {
            if (mem.base != src_base) break;
            i32 expected_disp = first_src_disp +
                static_cast<i32>(load_count) * 16;
            if (mem.disp != expected_disp) break;
        }
        ++load_count;
    }

    if (load_count < 2) return false;

    // Now expect load_count stores to sequential addresses
    u32 store_start = start + load_count;
    if (store_start + load_count > count) return false;

    u8 dst_base = 0xFF;
    i32 first_dst_disp = 0;

    for (u32 i = 0; i < load_count; ++i) {
        const auto& instr = block.instructions[store_start + i];
        if (!is_sse_store128(instr.opcode)) return false;
        if (instr.operand_count < 2) return false;
        if (!is_memory_operand(instr.operands[0].type)) return false;

        const auto& mem = instr.operands[0].mem;
        if (mem.index != 255) return false;

        if (i == 0) {
            dst_base = mem.base;
            first_dst_disp = mem.disp;
        } else {
            if (mem.base != dst_base) return false;
            i32 expected_disp = first_dst_disp + static_cast<i32>(i) * 16;
            if (mem.disp != expected_disp) return false;
        }

        // Verify the store XMM matches the corresponding load XMM
        u8 store_xmm = instr.operands[1].reg;
        u8 load_xmm = block.instructions[start + i].operands[0].reg;
        if (store_xmm != load_xmm) return false;
    }

    if (dst_base == src_base) return false;

    out.type = IdiomType::SseMemcpy;
    out.start_idx = start;
    out.end_idx = store_start + load_count;
    out.src_base_reg = src_base;
    out.dst_base_reg = dst_base;
    out.byte_count = load_count * 16;
    out.xmm_count = static_cast<u8>(load_count > 8 ? 8 : load_count);
    for (u32 i = 0; i < out.xmm_count; ++i) {
        out.xmm_regs[i] = block.instructions[start + i].operands[0].reg;
    }
    return true;
}

} // anonymous namespace

std::vector<IdiomMatch> detect_idioms(const BasicBlock& block) noexcept {
    std::vector<IdiomMatch> matches;

    u32 i = 0;
    while (i < block.instruction_count) {
        IdiomMatch match;
        if (try_match_memcpy(block, i, match)) {
            matches.push_back(match);
            i = match.end_idx;
        } else {
            ++i;
        }
    }

    return matches;
}

std::optional<ZeroFillLoopMatch>
detect_zero_fill_loop(const BasicBlock& block) noexcept {
    if (block.is_64bit || block.instruction_count != 4 ||
        block.instructions.size() != 4) {
        return std::nullopt;
    }

    const auto& store = block.instructions[0];
    const auto& advance = block.instructions[1];
    const auto& decrement = block.instructions[2];
    const auto& branch = block.instructions[3];
    if (!has_no_prefixes(store) || !has_no_prefixes(advance) ||
        !has_no_prefixes(decrement) || !has_no_prefixes(branch)) {
        return std::nullopt;
    }

    if (store.opcode != Opcode::MOV_RM8_IMM8 ||
        store.operand_count != 2 ||
        store.operands[0].type != OperandType::Mem8 ||
        store.operands[1].type != OperandType::Imm8 ||
        store.operands[1].imm != 0) {
        return std::nullopt;
    }
    const auto& dst_mem = store.operands[0].mem;
    if (!dst_mem.has_base() || dst_mem.has_index() || dst_mem.disp != 0 ||
        dst_mem.segment != SegReg::Default) {
        return std::nullopt;
    }
    const u8 dst_reg = dst_mem.base;

    if (advance.opcode != Opcode::LEA_R32_M ||
        advance.operand_count != 2 ||
        advance.operands[0].type != OperandType::Reg32 ||
        advance.operands[0].reg != dst_reg ||
        !is_memory_operand(advance.operands[1].type)) {
        return std::nullopt;
    }
    const auto& lea_mem = advance.operands[1].mem;
    if (lea_mem.base != dst_reg || lea_mem.has_index() || lea_mem.disp != 1 ||
        lea_mem.segment != SegReg::Default) {
        return std::nullopt;
    }

    if (decrement.opcode != Opcode::SUB_RM32_IMM8 ||
        decrement.operand_count != 2 ||
        decrement.operands[0].type != OperandType::Reg32 ||
        decrement.operands[1].type != OperandType::Imm8 ||
        decrement.operands[1].imm != 1) {
        return std::nullopt;
    }
    const u8 count_reg = decrement.operands[0].reg;
    if (count_reg == dst_reg) return std::nullopt;

    if (branch.opcode != Opcode::JNZ_REL8 &&
        branch.opcode != Opcode::JNZ_REL32) {
        return std::nullopt;
    }
    if (branch.operand_count != 1 ||
        (branch.operands[0].type != OperandType::Rel8 &&
         branch.operands[0].type != OperandType::Rel32)) {
        return std::nullopt;
    }

    const u32 branch_offset =
        store.length + advance.length + decrement.length;
    const u32 fallthrough_offset = branch_offset + branch.length;
    const i64 target_offset =
        static_cast<i64>(fallthrough_offset) + branch.operands[0].imm;
    if (target_offset != 0 ||
        block.byte_length != fallthrough_offset) {
        return std::nullopt;
    }

    return ZeroFillLoopMatch{
        dst_reg, count_reg, branch_offset, fallthrough_offset
    };
}

} // namespace vitreo::cpu::jit
