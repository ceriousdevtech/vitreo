// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom matcher. See loop_idiom_matcher.hpp / loop_idiom_desc.hpp.
//
// Instruction semantics relied on (Intel SDM Vol. 2 unless noted):
//   MOV/MOVZX write no flags; a 32-bit destination zero-extends to 64 bits in
//     64-bit mode, an 8/16-bit destination preserves the rest (Vol. 1 3.4.1.1);
//     without REX, 8-bit register ids 4-7 are AH/CH/DH/BH.
//   INC/DEC leave CF unchanged; ADD/SUB/CMP write all six status flags; TEST
//     clears CF and OF; LEA writes no flags.
//   Jcc reads flags and writes none; LOOP decrements ECX/RCX (address size),
//     writes no flags and branches while the count is not zero.
//   PCMPEQB/W/D write all-ones / all-zero lanes; PMOVMSKB r32 collects the
//     byte sign bits into bits 15:0 and zero-extends; neither writes flags.
//   Legacy MOVDQA/MOVAPS/MOVNTDQ/MOVNTPS require 16-byte alignment (#GP).

#include "core/cpu/jit/loop_idiom_matcher.hpp"

#include <array>
#include <cstdint>

namespace vitreo::cpu::jit::loop_idiom {

const char* reject_reason_name(RejectReason r) noexcept {
    switch (r) {
    case RejectReason::None: return "None";
    case RejectReason::NotSelfLoop: return "NotSelfLoop";
    case RejectReason::Length: return "Length";
    case RejectReason::Prefix: return "Prefix";
    case RejectReason::Opcode: return "Opcode";
    case RejectReason::StackReg: return "StackReg";
    case RejectReason::RipRelative: return "RipRelative";
    case RejectReason::LoopCarried: return "LoopCarried";
    case RejectReason::InductionShape: return "InductionShape";
    case RejectReason::Stride: return "Stride";
    case RejectReason::Width: return "Width";
    case RejectReason::FlagRead: return "FlagRead";
    case RejectReason::CondUndefined: return "CondUndefined";
    case RejectReason::InBodyBranch: return "InBodyBranch";
    case RejectReason::NoExit: return "NoExit";
    case RejectReason::StreamCount: return "StreamCount";
    case RejectReason::KindShape: return "KindShape";
    case RejectReason::Field: return "Field";
    }
    return "?";
}

namespace {

constexpr usize kMaxInsns = 24;
constexpr u8 kRsp = 4;
constexpr u8 kRcx = 1;

// The build has no exceptions: a rejection is a sticky per-thread reason.
// Every fallible function returns a value-initialised result after
// recording it, and every caller checks `failed()` before using a result.
thread_local RejectReason t_err = RejectReason::None;

bool failed() noexcept { return t_err != RejectReason::None; }
void set_err(RejectReason r) noexcept {
    if (t_err == RejectReason::None) t_err = r;
}

#define LI_REJECT(r)                         \
    do {                                     \
        set_err(RejectReason::r);            \
        return {};                           \
    } while (0)
#define LI_CHECK()                           \
    do {                                     \
        if (failed()) return {};             \
    } while (0)

/// A register operand normalised to its 64-bit parent.
struct Part {
    u8 reg{0};
    u8 width{0};  ///< bytes; 16 for XMM
    bool high8{false};
    bool xmm{false};
};

enum class ArgKind : u8 { None, Reg, Mem, Imm };
struct Arg {
    ArgKind kind{ArgKind::None};
    Part part{};
    const MemoryOperand* mem{nullptr};
    u8 mem_w{0};
    i64 imm{0};
};

enum class OpKind : u8 {
    Load, Store, Update, Cmp, Test, PcmpEq, Pmovmskb, Nop, Jcc, Jmp, Loop
};

struct Op {
    OpKind kind{OpKind::Nop};
    Part dst{};          // Load / PcmpEq / Pmovmskb destination
    Arg src{};           // Store value; PcmpEq / Pmovmskb source
    const MemoryOperand* mem{nullptr};  // Load source / Store destination
    u8 mem_w{0};
    bool align16{false};
    bool zext{false};    // MOVZX
    // Update
    u8 upd_reg{0};
    i64 step{0};
    u8 upd_w{0};
    bool writes_flags{false};
    bool incdec{false};
    // Cmp / Test
    Arg a{}, b{};
    u8 cmp_w{0};
    // PcmpEq lane log2
    u8 lane_log2{0};
    // Branch: condition under which it is TAKEN, and its target
    Cond cc{Cond::Z};
    u64 target{0};
};

bool cond_of(Opcode op, Cond& out) {
    switch (op) {
    case Opcode::JZ_REL8: case Opcode::JZ_REL32: out = Cond::Z; return true;
    case Opcode::JNZ_REL8: case Opcode::JNZ_REL32: out = Cond::NZ; return true;
    case Opcode::JB_REL8: case Opcode::JB_REL32: out = Cond::B; return true;
    case Opcode::JNB_REL8: case Opcode::JNB_REL32: out = Cond::AE; return true;
    case Opcode::JBE_REL8: case Opcode::JBE_REL32: out = Cond::BE; return true;
    case Opcode::JNBE_REL8: case Opcode::JNBE_REL32: out = Cond::A; return true;
    case Opcode::JL_REL8: case Opcode::JL_REL32: out = Cond::L; return true;
    case Opcode::JNL_REL8: case Opcode::JNL_REL32: out = Cond::GE; return true;
    case Opcode::JLE_REL8: case Opcode::JLE_REL32: out = Cond::LE; return true;
    case Opcode::JNLE_REL8: case Opcode::JNLE_REL32: out = Cond::G; return true;
    default: return false;
    }
}

Cond negate(Cond c) {
    switch (c) {
    case Cond::Z: return Cond::NZ;
    case Cond::NZ: return Cond::Z;
    case Cond::B: return Cond::AE;
    case Cond::AE: return Cond::B;
    case Cond::BE: return Cond::A;
    case Cond::A: return Cond::BE;
    case Cond::L: return Cond::GE;
    case Cond::GE: return Cond::L;
    case Cond::LE: return Cond::G;
    case Cond::G: return Cond::LE;
    }
    return Cond::Z;
}

bool is_jcc_opcode(Opcode op) {
    Cond c;
    if (cond_of(op, c)) return true;
    switch (op) {
    case Opcode::JO_REL8: case Opcode::JO_REL32:
    case Opcode::JNO_REL8: case Opcode::JNO_REL32:
    case Opcode::JS_REL8: case Opcode::JS_REL32:
    case Opcode::JNS_REL8: case Opcode::JNS_REL32:
    case Opcode::JP_REL8: case Opcode::JP_REL32:
    case Opcode::JNP_REL8: case Opcode::JNP_REL32:
        return true;
    default: return false;
    }
}

/// Register operand -> Part. Throws Reject on a stack register.
Part reg_part(const Operand& o, const DecodedInstruction& in) {
    Part p;
    switch (o.type) {
    case OperandType::Reg8:
        if (!in.prefixes.has_rex() && o.reg >= 4 && o.reg <= 7) {
            p.reg = static_cast<u8>(o.reg - 4);
            p.high8 = true;
        } else {
            p.reg = o.reg;
        }
        p.width = 1;
        break;
    case OperandType::Reg16: p.reg = o.reg; p.width = 2; break;
    case OperandType::Reg32: p.reg = o.reg; p.width = 4; break;
    case OperandType::Reg64: p.reg = o.reg; p.width = 8; break;
    case OperandType::Reg128: p.reg = o.reg; p.width = 16; p.xmm = true; return p;
    default: LI_REJECT(Opcode);
    }
    if (p.reg >= 16) LI_REJECT(Opcode);
    if (p.reg == kRsp && !p.high8) LI_REJECT(StackReg);
    return p;
}

bool check_mem(const MemoryOperand& m) {
    if (m.is_rip_relative()) LI_REJECT(RipRelative);
    if (m.has_absolute || m.vsib) LI_REJECT(Opcode);
    if (m.segment != SegReg::Default) LI_REJECT(Prefix);
    if ((m.has_base() && m.base == kRsp) || (m.has_index() && m.index == kRsp)) {
        LI_REJECT(StackReg);
    }
    if ((m.has_base() && m.base >= 16) || (m.has_index() && m.index >= 16)) {
        LI_REJECT(Opcode);
    }
    return true;
}

Arg arg_of(const Operand& o, const DecodedInstruction& in) {
    Arg a;
    if (is_register_operand(o.type)) {
        a.kind = ArgKind::Reg;
        a.part = reg_part(o, in);
    } else if (is_memory_operand(o.type)) {
        a.kind = ArgKind::Mem;
        check_mem(o.mem);
        a.mem = &o.mem;
        a.mem_w = operand_size(o.type);
        if (a.mem_w == 0 || a.mem_w > 16) LI_REJECT(Width);
    } else if (is_immediate_operand(o.type)) {
        a.kind = ArgKind::Imm;
        a.imm = o.imm;
    } else {
        LI_REJECT(Opcode);
    }
    return a;
}

/// Decode one instruction into an Op (whitelist + operand shape).
Op classify(const DecodedInstruction& in, u64 rip_after, bool is64) {
    const Prefixes& px = in.prefixes;
    if (px.lock || px.address_size || px.segment != SegReg::Default) {
        LI_REJECT(Prefix);
    }
    if (px.vex || px.evex) LI_REJECT(Opcode);
    Op op;
    const Opcode oc = in.opcode;
    // These record a rejection and let classify() finish with in-bounds
    // defaults; the caller checks failed() before using the Op.
    auto need = [&](usize n) {
        if (in.operand_count != n) set_err(RejectReason::Opcode);
    };
    auto no_rep = [&] {
        if (px.rep || px.repne) set_err(RejectReason::Prefix);
    };
    auto branch_target = [&](const Operand& o) -> u64 {
        if (o.type != OperandType::Rel8 && o.type != OperandType::Rel32) {
            LI_REJECT(Opcode);
        }
        if (px.operand_size) LI_REJECT(Prefix);
        u64 t = rip_after + static_cast<u64>(o.imm);
        if (!is64) t &= 0xFFFFFFFFull;
        return t;
    };

    switch (oc) {
    // ---- loads ----
    case Opcode::MOV_R8_RM8: case Opcode::MOV_R16_RM16:
    case Opcode::MOV_R32_RM32: case Opcode::MOV_R64_RM64:
    case Opcode::MOVZX_R32_RM8: case Opcode::MOVZX_R32_RM16:
    case Opcode::MOVZX_R64_RM8: case Opcode::MOVZX_R64_RM16: {
        no_rep();
        need(2);
        if (!is_memory_operand(in.operands[1].type)) {
            LI_REJECT(Opcode);  // reg-reg moves are not accepted
        }
        op.kind = OpKind::Load;
        op.dst = reg_part(in.operands[0], in);
        const Arg m = arg_of(in.operands[1], in);
        op.mem = m.mem;
        op.mem_w = m.mem_w;
        op.zext = oc == Opcode::MOVZX_R32_RM8 || oc == Opcode::MOVZX_R32_RM16 ||
                  oc == Opcode::MOVZX_R64_RM8 || oc == Opcode::MOVZX_R64_RM16;
        if (!op.zext && op.dst.width != op.mem_w) LI_REJECT(Width);
        return op;
    }
    case Opcode::MOVDQU_XMM_XMMM128: case Opcode::MOVDQA_XMM_XMMM128:
    case Opcode::MOVUPS_XMM_XMMM128: case Opcode::MOVAPS_XMM_XMMM128: {
        need(2);
        if (!is_memory_operand(in.operands[1].type)) LI_REJECT(Opcode);
        op.kind = OpKind::Load;
        op.dst = reg_part(in.operands[0], in);
        if (!op.dst.xmm) LI_REJECT(Opcode);
        const Arg m = arg_of(in.operands[1], in);
        op.mem = m.mem;
        op.mem_w = 16;
        op.align16 = oc == Opcode::MOVDQA_XMM_XMMM128 || oc == Opcode::MOVAPS_XMM_XMMM128;
        return op;
    }
    // ---- stores ----
    case Opcode::MOV_RM8_R8: case Opcode::MOV_RM16_R16:
    case Opcode::MOV_RM32_R32: case Opcode::MOV_RM64_R64:
    case Opcode::MOV_RM8_IMM8: case Opcode::MOV_RM16_IMM16:
    case Opcode::MOV_RM32_IMM32: case Opcode::MOV_RM64_IMM32:
    case Opcode::MOVNTI_M32_R32: case Opcode::MOVNTI_M64_R64: {
        no_rep();
        need(2);
        if (!is_memory_operand(in.operands[0].type)) LI_REJECT(Opcode);
        op.kind = OpKind::Store;
        const Arg m = arg_of(in.operands[0], in);
        op.mem = m.mem;
        op.mem_w = m.mem_w;
        op.src = arg_of(in.operands[1], in);
        if (op.src.kind == ArgKind::Reg && op.src.part.width != op.mem_w) {
            LI_REJECT(Width);
        }
        if (op.src.kind == ArgKind::Mem) LI_REJECT(Opcode);
        return op;
    }
    case Opcode::MOVDQU_XMMM128_XMM: case Opcode::MOVDQA_XMMM128_XMM:
    case Opcode::MOVUPS_XMMM128_XMM: case Opcode::MOVAPS_XMMM128_XMM:
    case Opcode::MOVNTDQ_M128_XMM: case Opcode::MOVNTPS_M128_XMM: {
        need(2);
        if (!is_memory_operand(in.operands[0].type)) LI_REJECT(Opcode);
        op.kind = OpKind::Store;
        const Arg m = arg_of(in.operands[0], in);
        op.mem = m.mem;
        op.mem_w = 16;
        op.src = arg_of(in.operands[1], in);
        if (op.src.kind != ArgKind::Reg || !op.src.part.xmm) LI_REJECT(Opcode);
        op.align16 = oc != Opcode::MOVDQU_XMMM128_XMM && oc != Opcode::MOVUPS_XMMM128_XMM;
        return op;
    }
    // ---- induction updates ----
    case Opcode::INC_R32: case Opcode::INC_RM32: case Opcode::INC_RM64:
    case Opcode::DEC_R32: case Opcode::DEC_RM32: case Opcode::DEC_RM64:
    case Opcode::INC_R16: case Opcode::INC_RM16: case Opcode::INC_RM8:
    case Opcode::DEC_R16: case Opcode::DEC_RM16: case Opcode::DEC_RM8: {
        no_rep();
        need(1);
        if (!is_register_operand(in.operands[0].type)) LI_REJECT(Opcode);
        const Part p = reg_part(in.operands[0], in);
        if (p.width != 4 && p.width != 8) LI_REJECT(InductionShape);
        op.kind = OpKind::Update;
        op.upd_reg = p.reg;
        op.upd_w = p.width;
        const bool inc = oc == Opcode::INC_R32 || oc == Opcode::INC_RM32 ||
                         oc == Opcode::INC_RM64;
        op.step = inc ? 1 : -1;
        op.writes_flags = true;
        op.incdec = true;
        return op;
    }
    case Opcode::ADD_RM32_IMM8: case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_RM64_IMM8: case Opcode::ADD_RM64_IMM32:
    case Opcode::SUB_RM32_IMM8: case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_RM64_IMM8: case Opcode::SUB_RM64_IMM32: {
        no_rep();
        need(2);
        if (!is_register_operand(in.operands[0].type) ||
            !is_immediate_operand(in.operands[1].type)) {
            LI_REJECT(Opcode);
        }
        const Part p = reg_part(in.operands[0], in);
        if (p.width != 4 && p.width != 8) LI_REJECT(InductionShape);
        op.kind = OpKind::Update;
        op.upd_reg = p.reg;
        op.upd_w = p.width;
        const bool add = oc == Opcode::ADD_RM32_IMM8 || oc == Opcode::ADD_RM32_IMM32 ||
                         oc == Opcode::ADD_RM64_IMM8 || oc == Opcode::ADD_RM64_IMM32;
        op.step = add ? in.operands[1].imm : -in.operands[1].imm;
        op.writes_flags = true;
        return op;
    }
    case Opcode::LEA_R32_M: case Opcode::LEA_R64_M: {
        no_rep();
        need(2);
        const Part p = reg_part(in.operands[0], in);
        if (!is_memory_operand(in.operands[1].type)) LI_REJECT(Opcode);
        const MemoryOperand& m = in.operands[1].mem;
        check_mem(m);
        if (m.base != p.reg || m.has_index()) LI_REJECT(InductionShape);
        if (p.width != 4 && p.width != 8) LI_REJECT(InductionShape);
        op.kind = OpKind::Update;
        op.upd_reg = p.reg;
        op.upd_w = p.width;
        op.step = m.disp;
        op.writes_flags = false;
        return op;
    }
    // ---- compares ----
    case Opcode::CMP_AL_IMM8: case Opcode::CMP_AX_IMM16: case Opcode::CMP_EAX_IMM32:
    case Opcode::CMP_RAX_IMM32: case Opcode::CMP_R8_RM8: case Opcode::CMP_R16_RM16:
    case Opcode::CMP_R32_RM32: case Opcode::CMP_R64_RM64: case Opcode::CMP_RM8_IMM8:
    case Opcode::CMP_RM16_IMM16: case Opcode::CMP_RM16_IMM8: case Opcode::CMP_RM32_IMM32:
    case Opcode::CMP_RM32_IMM8: case Opcode::CMP_RM64_IMM32: case Opcode::CMP_RM64_IMM8:
    case Opcode::CMP_RM8_R8: case Opcode::CMP_RM16_R16: case Opcode::CMP_RM32_R32:
    case Opcode::CMP_RM64_R64: {
        no_rep();
        need(2);
        op.kind = OpKind::Cmp;
        op.a = arg_of(in.operands[0], in);
        op.b = arg_of(in.operands[1], in);
        op.cmp_w = op.a.kind == ArgKind::Reg ? op.a.part.width
                 : op.a.kind == ArgKind::Mem ? op.a.mem_w : 0;
        if (op.cmp_w == 0 || op.cmp_w > 8) LI_REJECT(Opcode);
        if (op.b.kind == ArgKind::Reg && op.b.part.width != op.cmp_w) {
            LI_REJECT(Width);
        }
        if (op.b.kind == ArgKind::Mem && op.b.mem_w != op.cmp_w) {
            LI_REJECT(Width);
        }
        op.writes_flags = true;
        return op;
    }
    case Opcode::TEST_RM8_R8: case Opcode::TEST_RM16_R16:
    case Opcode::TEST_RM32_R32: case Opcode::TEST_RM64_R64: {
        no_rep();
        need(2);
        if (!is_register_operand(in.operands[0].type) ||
            !is_register_operand(in.operands[1].type)) {
            LI_REJECT(Opcode);
        }
        op.kind = OpKind::Test;
        op.a = arg_of(in.operands[0], in);
        op.b = arg_of(in.operands[1], in);
        if (op.a.part.reg != op.b.part.reg || op.a.part.high8 != op.b.part.high8 ||
            op.a.part.width != op.b.part.width) {
            LI_REJECT(Opcode);
        }
        op.cmp_w = op.a.part.width;
        op.writes_flags = true;
        return op;
    }
    // ---- vector compare + mask ----
    case Opcode::PCMPEQB_XMM_XMMM128: case Opcode::PCMPEQW_XMM_XMMM128:
    case Opcode::PCMPEQD_XMM_XMMM128: {
        need(2);
        op.kind = OpKind::PcmpEq;
        op.dst = reg_part(in.operands[0], in);
        op.src = arg_of(in.operands[1], in);
        if (!op.dst.xmm || op.src.kind != ArgKind::Reg || !op.src.part.xmm) {
            LI_REJECT(Opcode);
        }
        op.lane_log2 = oc == Opcode::PCMPEQB_XMM_XMMM128 ? 0
                     : oc == Opcode::PCMPEQW_XMM_XMMM128 ? 1 : 2;
        return op;
    }
    case Opcode::PMOVMSKB_R32_XMM: {
        need(2);
        op.kind = OpKind::Pmovmskb;
        op.dst = reg_part(in.operands[0], in);
        op.src = arg_of(in.operands[1], in);
        if (op.dst.xmm || op.src.kind != ArgKind::Reg || !op.src.part.xmm) {
            LI_REJECT(Opcode);
        }
        // PMOVMSKB r32 zero-extends to the full register in 64-bit mode.
        op.dst.width = is64 ? 8 : 4;
        return op;
    }
    case Opcode::NOP:
        op.kind = OpKind::Nop;
        return op;
    case Opcode::JMP_REL8: case Opcode::JMP_REL32:
        need(1);
        op.kind = OpKind::Jmp;
        op.target = branch_target(in.operands[0]);
        return op;
    case Opcode::LOOP_REL8:
        need(1);
        op.kind = OpKind::Loop;
        op.target = branch_target(in.operands[0]);
        return op;
    default:
        break;
    }
    if (cond_of(oc, op.cc)) {
        need(1);
        op.kind = OpKind::Jcc;
        op.target = branch_target(in.operands[0]);
        return op;
    }
    if (is_jcc_opcode(oc)) LI_REJECT(CondUndefined);
    LI_REJECT(Opcode);
}

// ---------------------------------------------------------------------------
// Data flow
// ---------------------------------------------------------------------------

enum class ValKind : u8 { None, Elem, LaneEq, Mask };

/// What a written (temp) register holds in the current iteration.
struct TempVal {
    ValKind kind{ValKind::None};
    u8 stream{0};
    u8 load_w{0};
    bool full{false};     ///< write covers the whole register (zero-extended)
    u8 written_w{0};      ///< partial writes: bytes written
    bool high8{false};
    u8 lane_log2{0};      ///< LaneEq / Mask
    u8 inv_xmm{0};        ///< LaneEq / Mask
};

enum class RefKind : u8 { Imm, Inv, Ind, Elem, Mask };
struct Ref {
    RefKind kind{RefKind::Imm};
    i64 imm{0};
    Part part{};        ///< Inv: the invariant register part
    u8 ind{0};          ///< Ind: induction slot
    u8 phase{0};
    u8 stream{0};       ///< Elem / Mask
    u8 read_w{0};       ///< Elem: width the value is read at
    u8 lane_log2{0};    ///< Mask
    u8 inv_xmm{0};      ///< Mask
};

enum class PredKind : u8 { None, Ind, Value, StreamEq, AnyLane };
struct Pred {
    PredKind kind{PredKind::None};
    bool from_update{false};
    bool from_incdec{false};
    // Ind
    u8 ind{0};
    u8 phase{0};
    u8 cmp_w{0};
    bool ind_is_lhs{true};
    Ref other{};
    // Value / StreamEq / AnyLane
    u8 stream{0};
    Ref value{};
    u8 lane_log2{0};
    u8 inv_xmm{0};
};

struct StreamAcc {
    u8 base{kNoReg};
    u8 index{kNoReg};
    u8 scale_log2{0};
    i64 disp_eff{0};
    bool store{false};
    bool align16{false};
};

struct Matcher {
    const BasicBlock& block;
    const MatchConfig& cfg;
    bool is64;
    std::array<Op, kMaxInsns> ops{};
    usize n{0};

    // Induction table
    struct Ind {
        u8 reg;
        i64 step;
        u8 width;
    };
    std::array<Ind, 4> inds{};
    u8 n_ind{0};
    std::array<i8, 16> ind_of{};   // reg -> slot, -1 none
    std::array<bool, 16> gpr_written{};
    std::array<bool, 16> xmm_written{};

    // Per-iteration walk state
    std::array<u8, 16> phase{};    // per induction slot
    std::array<TempVal, 16> gpr_val{};
    std::array<TempVal, 16> xmm_val{};
    std::array<bool, 16> ind_addr_use{};

    std::array<StreamAcc, 3> streams{};
    u8 n_streams{0};
    u8 elem_w{0};
    int stride_sign{0};

    Matcher(const BasicBlock& b, const MatchConfig& c)
        : block(b), cfg(c), is64(b.is_64bit) {
        ind_of.fill(-1);
    }

    u8 native_w() const { return is64 ? 8 : 4; }

    bool decode_all() {
        if (block.instruction_count < 2 || block.instruction_count > kMaxInsns ||
            block.instructions.size() != block.instruction_count) {
            LI_REJECT(Length);
        }
        n = block.instruction_count;
        u64 off = 0;
        for (usize i = 0; i < n; ++i) {
            const DecodedInstruction& in = block.instructions[i];
            off += in.length;
            u64 rip_after = block.start_addr + off;
            if (!is64) rip_after &= 0xFFFFFFFFull;
            ops[i] = classify(in, rip_after, is64);
            LI_CHECK();
        }
        if (off != block.byte_length) LI_REJECT(NotSelfLoop);
        const Op& last = ops[n - 1];
        if (last.kind != OpKind::Jcc && last.kind != OpKind::Jmp &&
            last.kind != OpKind::Loop) {
            LI_REJECT(NotSelfLoop);
        }
        if (last.target != block.start_addr) LI_REJECT(NotSelfLoop);
        const u64 lo = block.start_addr;
        const u64 hi = block.start_addr + block.byte_length;
        for (usize i = 0; i + 1 < n; ++i) {
            const Op& o = ops[i];
            if (o.kind == OpKind::Jmp || o.kind == OpKind::Loop) {
                LI_REJECT(InBodyBranch);
            }
            if (o.kind == OpKind::Jcc && o.target >= lo && o.target < hi) {
                LI_REJECT(InBodyBranch);
            }
        }
        return true;
    }

    bool find_inductions() {
        auto add_ind = [&](u8 reg, i64 step, u8 width) -> bool {
            if (ind_of[reg] >= 0) LI_REJECT(InductionShape);
            if (step == 0 || step < -16 || step > 16) {
                LI_REJECT(InductionShape);
            }
            if (width != native_w() && !(is64 && width == 4)) {
                LI_REJECT(InductionShape);
            }
            if (n_ind >= 3) LI_REJECT(Field);
            ind_of[reg] = static_cast<i8>(n_ind);
            inds[n_ind++] = Ind{reg, step, width};
            return true;
        };
        for (usize i = 0; i < n; ++i) {
            const Op& o = ops[i];
            if (o.kind == OpKind::Update && !add_ind(o.upd_reg, o.step, o.upd_w)) return false;
            if (o.kind == OpKind::Loop && !add_ind(kRcx, -1, native_w())) return false;
        }
        // Everything else a loop writes is a temp; record it so a read before
        // the write in body order is recognised as loop-carried.
        for (usize i = 0; i < n; ++i) {
            const Op& o = ops[i];
            if (o.kind == OpKind::Load || o.kind == OpKind::Pmovmskb) {
                if (o.dst.xmm) {
                    xmm_written[o.dst.reg] = true;
                } else {
                    if (ind_of[o.dst.reg] >= 0) LI_REJECT(InductionShape);
                    gpr_written[o.dst.reg] = true;
                }
            } else if (o.kind == OpKind::PcmpEq) {
                xmm_written[o.dst.reg] = true;
            }
        }
        return true;
    }

    /// Value of a register read at this point of the iteration.
    Ref read_reg(const Part& p) {
        Ref r;
        if (p.xmm) {
            if (!xmm_written[p.reg]) {
                r.kind = RefKind::Inv;
                r.part = p;
                return r;
            }
            const TempVal& t = xmm_val[p.reg];
            if (t.kind == ValKind::None) LI_REJECT(LoopCarried);
            if (t.kind == ValKind::Elem) {
                r.kind = RefKind::Elem;
                r.stream = t.stream;
                r.read_w = 16;
                return r;
            }
            // LaneEq: only consumed by PMOVMSKB, handled by the caller.
            r.kind = RefKind::Mask;
            r.stream = t.stream;
            r.lane_log2 = t.lane_log2;
            r.inv_xmm = t.inv_xmm;
            r.read_w = 16;
            return r;
        }
        if (ind_of[p.reg] >= 0) {
            if (p.high8) LI_REJECT(InductionShape);
            r.kind = RefKind::Ind;
            r.ind = static_cast<u8>(ind_of[p.reg]);
            r.phase = phase[r.ind];
            r.part = p;
            return r;
        }
        if (!gpr_written[p.reg]) {
            r.kind = RefKind::Inv;
            r.part = p;
            return r;
        }
        const TempVal& t = gpr_val[p.reg];
        if (t.kind == ValKind::None) LI_REJECT(LoopCarried);
        if (p.high8 != t.high8) LI_REJECT(LoopCarried);
        if (!t.full && p.width > t.written_w) LI_REJECT(LoopCarried);
        if (t.kind == ValKind::Mask) {
            r.kind = RefKind::Mask;
            r.stream = t.stream;
            r.lane_log2 = t.lane_log2;
            r.inv_xmm = t.inv_xmm;
            r.read_w = p.width;
            return r;
        }
        r.kind = RefKind::Elem;
        r.stream = t.stream;
        r.read_w = p.width;
        return r;
    }

    /// Record an access and return its stream slot.
    u8 access(const MemoryOperand& m, u8 width, bool store, bool align16) {
        if (elem_w == 0) elem_w = width;
        if (width != elem_w) LI_REJECT(Width);
        i64 stride = 0;
        i64 disp_eff = m.disp;
        bool has_ind = false;
        auto term = [&](u8 reg, i64 coef) -> bool {
            if (ind_of[reg] >= 0) {
                const u8 s = static_cast<u8>(ind_of[reg]);
                const Ind& ind = inds[s];
                if (is64 && ind.width != 8) LI_REJECT(InductionShape);
                stride += coef * ind.step;
                disp_eff += coef * phase[s] * ind.step;
                has_ind = true;
                ind_addr_use[s] = true;
                return true;
            }
            if (gpr_written[reg]) LI_REJECT(LoopCarried);
            return true;
        };
        const u8 base = m.has_base() ? m.base : kNoReg;
        const u8 index = m.has_index() ? m.index : kNoReg;
        u8 scale_log2 = 0;
        switch (m.scale) {
        case 1: scale_log2 = 0; break;
        case 2: scale_log2 = 1; break;
        case 4: scale_log2 = 2; break;
        case 8: scale_log2 = 3; break;
        default: LI_REJECT(Opcode);
        }
        if (base != kNoReg && !term(base, 1)) return {};
        if (index != kNoReg && !term(index, i64{1} << scale_log2)) return {};
        if (!has_ind) LI_REJECT(Stride);
        if (stride != elem_w && stride != -static_cast<i64>(elem_w)) {
            LI_REJECT(Stride);
        }
        const int sign = stride > 0 ? 1 : -1;
        if (stride_sign == 0) stride_sign = sign;
        if (sign != stride_sign) LI_REJECT(Stride);
        if (disp_eff < INT32_MIN || disp_eff > INT32_MAX) LI_REJECT(Field);

        for (u8 s = 0; s < n_streams; ++s) {
            StreamAcc& a = streams[s];
            if (a.base == base && a.index == index && a.scale_log2 == scale_log2 &&
                a.disp_eff == disp_eff && a.store == store) {
                a.align16 = a.align16 || align16;
                return s;
            }
        }
        if (n_streams >= 3) LI_REJECT(StreamCount);
        StreamAcc& a = streams[n_streams];
        a.base = base;
        a.index = index;
        a.scale_log2 = scale_log2;
        a.disp_eff = disp_eff;
        a.store = store;
        a.align16 = align16;
        return n_streams++;
    }

    Ref resolve(const Arg& a, bool align16_mem = false) {
        Ref r;
        switch (a.kind) {
        case ArgKind::Imm:
            r.kind = RefKind::Imm;
            r.imm = a.imm;
            return r;
        case ArgKind::Reg:
            return read_reg(a.part);
        case ArgKind::Mem:
            r.kind = RefKind::Elem;
            r.stream = access(*a.mem, a.mem_w, false, align16_mem);
            r.read_w = a.mem_w;
            return r;
        case ArgKind::None:
            break;
        }
        LI_REJECT(Opcode);
    }

    // ---- results ----
    Desc desc{};
    InlineGate gate{};
    bool have_ind_exit{false};
    Pred ind_pred{};
    Cond ind_cont{Cond::NZ};
    u8 n_data{0};
    std::array<Pred, 2> data_pred{};
    std::array<Cond, 2> data_cont{};
    bool have_store{false};
    usize store_idx{0};
    Ref store_val{};
    u8 store_stream{0};
    bool loop_insn{false};

    bool add_exit(const Pred& p, Cond cont) {
        if (p.kind == PredKind::None) LI_REJECT(FlagRead);
        if (p.kind == PredKind::Ind) {
            if (p.from_update && cont != Cond::Z && cont != Cond::NZ) {
                LI_REJECT(CondUndefined);
            }
            if (have_ind_exit) LI_REJECT(Field);
            have_ind_exit = true;
            ind_pred = p;
            ind_cont = cont;
            return true;
        }
        if (cont != Cond::Z && cont != Cond::NZ) LI_REJECT(Field);
        if (n_data >= 2) LI_REJECT(Field);
        data_pred[n_data] = p;
        data_cont[n_data] = cont;
        ++n_data;
        return true;
    }

    Pred pred_from_cmp(const Op& o) {
        const Ref a = resolve(o.a);
        LI_CHECK();
        const Ref b = resolve(o.b);
        LI_CHECK();
        Pred p;
        auto is_const = [](const Ref& r) {
            return r.kind == RefKind::Imm ||
                   (r.kind == RefKind::Inv && !r.part.xmm);
        };
        auto ind_pred = [&](const Ref& ind, const Ref& other, bool lhs) -> bool {
            if (ind.part.width != o.cmp_w) LI_REJECT(Width);
            if (o.cmp_w != 4 && o.cmp_w != 8) LI_REJECT(Width);
            if (o.cmp_w > inds[ind.ind].width) LI_REJECT(Width);
            if (other.kind == RefKind::Inv && other.part.high8) {
                LI_REJECT(Field);
            }
            p.kind = PredKind::Ind;
            p.ind = ind.ind;
            p.phase = ind.phase;
            p.cmp_w = o.cmp_w;
            p.ind_is_lhs = lhs;
            p.other = other;
            return true;
        };
        if (a.kind == RefKind::Ind && is_const(b)) {
            if (!ind_pred(a, b, true)) return {};
            return p;
        }
        if (b.kind == RefKind::Ind && is_const(a)) {
            if (!ind_pred(b, a, false)) return {};
            return p;
        }
        auto value_pred = [&](const Ref& e, const Ref& v) {
            p.kind = PredKind::Value;
            p.stream = e.stream;
            p.cmp_w = o.cmp_w;
            p.value = v;
        };
        if (a.kind == RefKind::Elem && is_const(b)) { value_pred(a, b); return p; }
        if (b.kind == RefKind::Elem && is_const(a)) { value_pred(b, a); return p; }
        if (a.kind == RefKind::Elem && b.kind == RefKind::Elem && a.stream != b.stream) {
            if (a.read_w != elem_w || b.read_w != elem_w) LI_REJECT(Width);
            p.kind = PredKind::StreamEq;
            p.cmp_w = o.cmp_w;
            return p;
        }
        LI_REJECT(KindShape);
    }

    Pred pred_from_test(const Op& o) {
        const Ref a = resolve(o.a);
        LI_CHECK();
        Pred p;
        Ref zero;
        zero.kind = RefKind::Imm;
        zero.imm = 0;
        if (a.kind == RefKind::Ind) {
            if (o.cmp_w != 4 && o.cmp_w != 8) LI_REJECT(Width);
            if (o.cmp_w > inds[a.ind].width) LI_REJECT(Width);
            p.kind = PredKind::Ind;
            p.ind = a.ind;
            p.phase = a.phase;
            p.cmp_w = o.cmp_w;
            p.ind_is_lhs = true;
            p.other = zero;
            // TEST clears CF/OF: only ZF has the CMP-with-0 meaning.
            p.from_update = true;
            return p;
        }
        if (a.kind == RefKind::Elem) {
            p.kind = PredKind::Value;
            p.stream = a.stream;
            p.cmp_w = o.cmp_w;
            p.value = zero;
            return p;
        }
        if (a.kind == RefKind::Mask) {
            // The mask occupies bits 15:0; a narrower TEST would see only
            // some lanes.
            if (o.cmp_w < 2) LI_REJECT(Width);
            p.kind = PredKind::AnyLane;
            p.stream = a.stream;
            p.lane_log2 = a.lane_log2;
            p.inv_xmm = a.inv_xmm;
            return p;
        }
        LI_REJECT(KindShape);
    }

    bool walk() {
        Pred flags{};
        for (usize i = 0; i < n; ++i) {
            const Op& o = ops[i];
            const bool last = i + 1 == n;
            switch (o.kind) {
            case OpKind::Nop:
                break;
            case OpKind::Load: {
                const u8 s = access(*o.mem, o.mem_w, false, o.align16);
                LI_CHECK();
                TempVal t;
                t.kind = ValKind::Elem;
                t.stream = s;
                t.load_w = o.mem_w;
                if (o.dst.xmm) {
                    t.full = true;
                    xmm_val[o.dst.reg] = t;
                    break;
                }
                // MOVZX and 32/64-bit MOV write the whole register (a 32-bit
                // write zero-extends in 64-bit mode); 8/16-bit MOV merge.
                t.full = o.zext || o.dst.width >= 4;
                t.written_w = o.dst.width;
                t.high8 = o.dst.high8;
                gpr_val[o.dst.reg] = t;
                break;
            }
            case OpKind::Store: {
                if (have_store) LI_REJECT(StreamCount);
                Ref v = resolve(o.src);
                LI_CHECK();
                // The address is evaluated as part of this instruction.
                store_stream = access(*o.mem, o.mem_w, true, o.align16);
                LI_CHECK();
                have_store = true;
                store_idx = i;
                store_val = v;
                break;
            }
            case OpKind::Update: {
                const u8 s = static_cast<u8>(ind_of[o.upd_reg]);
                phase[s] = 1;
                if (o.writes_flags) {
                    Pred p;
                    p.kind = PredKind::Ind;
                    p.from_update = true;
                    p.from_incdec = o.incdec;
                    p.ind = s;
                    p.phase = 1;
                    p.cmp_w = o.upd_w;
                    p.ind_is_lhs = true;
                    p.other.kind = RefKind::Imm;
                    p.other.imm = 0;
                    flags = p;
                }
                break;
            }
            case OpKind::Cmp:
                flags = pred_from_cmp(o);
                LI_CHECK();
                break;
            case OpKind::Test:
                flags = pred_from_test(o);
                LI_CHECK();
                break;
            case OpKind::PcmpEq: {
                const Ref d = read_reg(o.dst);
                LI_CHECK();
                const Ref s = read_reg(o.src.part);
                LI_CHECK();
                if (d.kind != RefKind::Elem || s.kind != RefKind::Inv) {
                    LI_REJECT(KindShape);
                }
                TempVal t;
                t.kind = ValKind::LaneEq;
                t.stream = d.stream;
                t.lane_log2 = o.lane_log2;
                t.inv_xmm = s.part.reg;
                t.full = true;
                xmm_val[o.dst.reg] = t;
                break;
            }
            case OpKind::Pmovmskb: {
                const TempVal& x = xmm_written[o.src.part.reg]
                                       ? xmm_val[o.src.part.reg] : TempVal{};
                if (x.kind != ValKind::LaneEq) LI_REJECT(KindShape);
                TempVal t = x;
                t.kind = ValKind::Mask;
                t.full = true;
                t.written_w = o.dst.width;
                gpr_val[o.dst.reg] = t;
                break;
            }
            case OpKind::Jcc:
                if (!add_exit(flags, last ? o.cc : negate(o.cc))) return false;
                break;
            case OpKind::Jmp:
                break;
            case OpKind::Loop: {
                loop_insn = true;
                const u8 s = static_cast<u8>(ind_of[kRcx]);
                phase[s] = 1;
                Pred p;
                p.kind = PredKind::Ind;
                p.from_update = true;
                p.ind = s;
                p.phase = 1;
                p.cmp_w = native_w();
                p.ind_is_lhs = true;
                p.other.kind = RefKind::Imm;
                p.other.imm = 0;
                // LOOP writes no flags: `flags` is left as it was.
                if (!add_exit(p, Cond::NZ)) return false;
                break;
            }
            }
        }
        return true;
    }

    static u8 log2_of(u32 v) {
        u8 l = 0;
        while ((1u << l) < v) ++l;
        return l;
    }

    bool fill_value_of(const Ref& v, Value& out) {
        if (v.kind == RefKind::Imm) {
            if (v.imm < INT32_MIN || v.imm > INT32_MAX) LI_REJECT(Field);
            out.src = ValueSrc::Imm;
            out.imm = static_cast<i32>(v.imm);
            return true;
        }
        if (v.kind == RefKind::Inv) {
            out.src = v.part.xmm ? ValueSrc::Xmm : ValueSrc::Gpr;
            out.reg = v.part.reg;
            out.high8 = v.part.high8;
            return true;
        }
        LI_REJECT(KindShape);
    }

    bool data_value_of(const Ref& v, DataExit& out) {
        if (v.kind == RefKind::Imm) {
            out.val = ValueSrc::Imm;
            out.val_imm = static_cast<i32>(v.imm);
            return true;
        }
        if (v.kind == RefKind::Inv && !v.part.xmm) {
            out.val = ValueSrc::Gpr;
            out.val_reg = v.part.reg;
            out.val_high8 = v.part.high8;
            return true;
        }
        LI_REJECT(KindShape);
    }

    bool build() {
        // Stream order in the descriptor: loads first (body order), then the store.
        std::array<u8, 3> slot{};
        u8 n_loads = 0;
        for (u8 s = 0; s < n_streams; ++s) {
            if (!streams[s].store) slot[s] = n_loads++;
        }
        if (n_loads > 2) LI_REJECT(StreamCount);
        for (u8 s = 0; s < n_streams; ++s) {
            if (streams[s].store) slot[s] = n_loads;
        }
        if (n_streams == 0) LI_REJECT(KindShape);

        Desc& d = desc;
        d.is64 = is64;
        d.elem_log2 = log2_of(elem_w);
        d.vector = elem_w == 16;
        d.ordered = d.vector ? cfg.ordered_vector : cfg.ordered_gpr;
        d.dir_down = stride_sign < 0;
        d.n_loads = n_loads;
        d.has_store = have_store;
        for (u8 s = 0; s < n_streams; ++s) {
            Stream& o = d.streams[slot[s]];
            o.base = streams[s].base;
            o.index = streams[s].index;
            o.scale_log2 = streams[s].scale_log2;
            o.disp_eff = static_cast<i32>(streams[s].disp_eff);
            o.align16 = streams[s].align16;
        }
        d.n_ind = n_ind;
        for (u8 i = 0; i < n_ind; ++i) {
            d.ind[i].reg = inds[i].reg;
            d.ind[i].step = static_cast<i8>(inds[i].step);
            d.ind[i].wide = inds[i].width == 8;
        }
        d.loop_rip = block.start_addr;

        if (have_ind_exit) {
            const Pred& p = ind_pred;
            IndExit& e = d.ind_exit;
            d.has_ind_exit = true;
            e.ind = p.ind;
            e.cont = ind_cont;
            e.ind_is_lhs = p.ind_is_lhs;
            e.phase = p.phase;
            e.cmp64 = p.cmp_w == 8;
            if (p.other.kind == RefKind::Imm) {
                e.bound = ValueSrc::Imm;
                e.bound_imm = static_cast<i32>(p.other.imm);
            } else {
                e.bound = ValueSrc::Gpr;
                e.bound_reg = p.other.part.reg;
            }
        }

        // Data exits
        d.n_data = n_data;
        bool has_stream_eq = false;
        for (u8 k = 0; k < n_data; ++k) {
            const Pred& p = data_pred[k];
            DataExit& x = d.data[k];
            const Cond exit_cond = negate(data_cont[k]);
            switch (p.kind) {
            case PredKind::Value:
                x.op = DataOp::ValueEq;
                x.stream = slot[p.stream];
                x.exit_if_eq = exit_cond == Cond::Z;
                x.cmp_log2 = log2_of(p.cmp_w);
                if (!data_value_of(p.value, x)) return false;
                break;
            case PredKind::StreamEq:
                x.op = DataOp::StreamEq;
                x.stream = 0;
                x.exit_if_eq = exit_cond == Cond::Z;
                x.cmp_log2 = d.elem_log2;
                has_stream_eq = true;
                break;
            case PredKind::AnyLane:
                x.op = DataOp::AnyLaneEq;
                x.stream = slot[p.stream];
                // ZF=1 <=> mask == 0 <=> no lane equal.
                x.exit_if_eq = exit_cond == Cond::NZ;
                x.val = ValueSrc::Xmm;
                x.val_reg = p.inv_xmm;
                x.cmp_log2 = p.lane_log2;
                break;
            default:
                LI_REJECT(KindShape);
            }
        }

        if (!have_ind_exit && n_data == 0) LI_REJECT(NoExit);

        // Kind
        if (have_store) {
            if (store_val.kind == RefKind::Elem) {
                if (n_loads != 1 || store_val.read_w != elem_w) {
                    LI_REJECT(KindShape);
                }
                for (u8 k = 0; k < n_data; ++k) {
                    if (d.data[k].op != DataOp::ValueEq || d.data[k].stream != 0) {
                        LI_REJECT(KindShape);
                    }
                }
                d.kind = Kind::Copy;
            } else if (store_val.kind == RefKind::Imm || store_val.kind == RefKind::Inv) {
                if (n_loads != 0 || n_data != 0) LI_REJECT(KindShape);
                d.kind = Kind::Fill;
                if (!fill_value_of(store_val, d.fill)) return false;
            } else {
                LI_REJECT(KindShape);
            }
        } else if (n_loads == 1) {
            if (has_stream_eq) LI_REJECT(KindShape);
            d.kind = Kind::Scan;
        } else if (n_loads == 2) {
            if (!has_stream_eq) LI_REJECT(KindShape);
            d.kind = Kind::Compare;
        } else {
            LI_REJECT(KindShape);
        }

        // Inline trip-count gate. The induction exit bounds the trip count
        // from above whether or not a data exit can leave earlier (memchr,
        // bounded compare), and the helper declines below m + 2 remaining
        // iterations either way, so the gate is a pure pre-check.
        if (have_ind_exit) {
            const IndExit& e = d.ind_exit;
            const Induction& ind = d.ind[e.ind];
            InlineGate g;
            g.reg = ind.reg;
            g.wide = e.cmp64;
            if (ind_pred.from_update && e.cont == Cond::NZ && e.bound == ValueSrc::Imm &&
                e.bound_imm == 0 && e.phase == 1) {
                if (ind.step == -1) g.form = InlineGate::Form::CountDown;
                if (ind.step == 1) g.form = InlineGate::Form::NegIndexUp;
            } else if (!ind_pred.from_update && (e.ind_is_lhs || e.cont == Cond::NZ) &&
                       ind.step > 0 &&
                       (ind.step & (ind.step - 1)) == 0 &&
                       (e.cont == Cond::NZ || e.cont == Cond::B)) {
                g.form = InlineGate::Form::UpToBound;
                g.step_log2 = log2_of(static_cast<u32>(ind.step));
                g.bound_is_imm = e.bound == ValueSrc::Imm;
                g.bound_reg = e.bound_reg;
                g.bound_imm = e.bound_imm;
            }
            gate = g;
        }
        return true;
    }
};

}  // namespace

MatchResult match_loop_idiom(const BasicBlock& block, const MatchConfig& cfg) noexcept {
    MatchResult r;
    t_err = RejectReason::None;
    Matcher m(block, cfg);
    const bool ok = m.decode_all() && m.find_inductions() && m.walk() && m.build();
    if (!ok || failed()) {
        r.reason = failed() ? t_err : RejectReason::Field;
        t_err = RejectReason::None;
        return r;
    }
    if (!pack(m.desc)) {
        r.reason = RejectReason::Field;
        return r;
    }
    r.match = LoopIdiomMatch{m.desc, m.gate};
    r.reason = RejectReason::None;
    return r;
}

}  // namespace vitreo::cpu::jit::loop_idiom
