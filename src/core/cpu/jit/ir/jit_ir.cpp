// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_ir.cpp
/// @brief Stage 1 block IR: construction, verification and printing.
///
/// See `jit_ir.hpp` for the data model. This file contains no ARM64 codegen
/// and no dependency on
/// `JitCompiler`, so it builds on both backends and its tests need neither.

#include "jit_ir.hpp"

#include <cstdarg>
#include <cstdio>

namespace vitreo::cpu::jit::ir {

// =============================================================================
// Names
// =============================================================================

const char* type_name(Type t) noexcept {
    switch (t) {
        case Type::Void:  return "void";
        case Type::I8:    return "i8";
        case Type::I16:   return "i16";
        case Type::I32:   return "i32";
        case Type::I64:   return "i64";
        case Type::Flags: return "flags";
        case Type::V128:  return "v128";
    }
    return "?";
}

const char* op_name(Op op) noexcept {
    switch (op) {
        case Op::GprRead:  return "gpr.read";
        case Op::GprWrite: return "gpr.write";
        case Op::Const:    return "const";
        case Op::FlagsIn:  return "flags.in";
        case Op::Add:      return "add";
        case Op::Sub:      return "sub";
        case Op::And:      return "and";
        case Op::Or:       return "or";
        case Op::Xor:      return "xor";
        case Op::Not:      return "not";
        case Op::Neg:      return "neg";
        case Op::Shl:      return "shl";
        case Op::Shr:      return "shr";
        case Op::Sar:      return "sar";
        case Op::Mul:      return "mul";
        case Op::SMulH:    return "smulh";
        case Op::SegSelector:  return "seg.selector";
        case Op::Divide:       return "divide";
        case Op::FlagsImage:   return "flags.image";
        case Op::FlagsInstall: return "flags.install";
        case Op::SetCf:    return "set.cf";
        case Op::SetOf:    return "set.of";
        case Op::EvalCc:   return "eval.cc";
        case Op::Select:   return "select";
        case Op::Trunc:    return "trunc";
        case Op::Rol:      return "rol";
        case Op::Ror:      return "ror";
        case Op::Lea:      return "lea";
        case Op::Zext:     return "zext";
        case Op::Sext:     return "sext";
        case Op::DefFlags: return "defflags";
        case Op::Load:     return "load";
        case Op::Store:    return "store";
        case Op::Jcc:      return "jcc";
        case Op::CondExit: return "cond_exit";
        case Op::AddCarry: return "add.carry";
        case Op::SubBorrow: return "sub.borrow";
        case Op::Jmp:      return "jmp";
        case Op::JmpIndirect: return "jmp.indirect";
        case Op::SegBase:      return "seg.base";
        case Op::XmmRead:      return "xmm.read";
        case Op::XmmWrite:     return "xmm.write";
        case Op::VAdd:         return "v.add";
        case Op::VSub:         return "v.sub";
        case Op::VCmpEq:       return "v.cmpeq";
        case Op::VCmpGt:       return "v.cmpgt";
        case Op::VMinU:        return "v.minu";
        case Op::VMaxU:        return "v.maxu";
        case Op::VMinS:        return "v.mins";
        case Op::VMaxS:        return "v.maxs";
        case Op::VMulLo:       return "v.mullo";
        case Op::VAvgU:        return "v.avgu";
        case Op::VQAddS:       return "v.qadds";
        case Op::VQAddU:       return "v.qaddu";
        case Op::VQSubS:       return "v.qsubs";
        case Op::VQSubU:       return "v.qsubu";
        case Op::VAbs:         return "v.abs";
        case Op::VZipLo:       return "v.ziplo";
        case Op::VZipHi:       return "v.ziphi";
        case Op::VPackS:       return "v.packs";
        case Op::VPackU:       return "v.packu";
        case Op::VShlImm:      return "v.shl";
        case Op::VShrImm:      return "v.shr";
        case Op::VSarImm:      return "v.sar";
        case Op::VXor:         return "v.xor";
        case Op::VAnd:         return "vand";
        case Op::VOr:         return "vor";
        case Op::VInsertLow32: return "v.insert.low32";
        case Op::VZext32:      return "v.zext32";
        case Op::VLow32:       return "v.low32";
    }
    return "?";
}

const char* mem_class_name(MemClass c) noexcept {
    switch (c) {
        case MemClass::Guest: return "guest";
    }
    return "?";
}

const char* flag_repr_name(FlagRepr r) noexcept {
    switch (r) {
        case FlagRepr::Dead:             return "dead";
        case FlagRepr::InPstate:         return "pstate";
        case FlagRepr::InCpuStateRecord: return "record";
        case FlagRepr::InRflags:         return "rflags";
    }
    return "?";
}

const char* nzcv_kind_name(NzcvKind k) noexcept {
    switch (k) {
        case NzcvKind::None:    return "none";
        case NzcvKind::Sub:     return "sub";
        case NzcvKind::And:     return "and";
        case NzcvKind::Add:     return "add";
        case NzcvKind::IncDec:  return "incdec";
        case NzcvKind::Sub8:    return "sub8";
        case NzcvKind::Carried: return "carried";
        case NzcvKind::NzOnly:  return "nzonly";
    }
    return "?";
}

const char* fault_state_name(FaultState s) noexcept {
    switch (s) {
        case FaultState::None:        return "none";
        case FaultState::RipOnly:     return "rip";
        case FaultState::RipAndFlags: return "rip+flags";
    }
    return "?";
}

namespace {

/// Names for the lazy records Stage 1 can produce. `FlagState::FlagOp` has
/// roughly sixty enumerators covering every width and the ADC/SBB/ROL forms;
/// spelling all of them here would be a maintenance liability for a debug
/// printer, so anything outside the register-ALU set prints as `#N`. The
/// numeric form is still stable and still diffable, which is all the printer
/// contract promises.
const char* flag_op_name(FlagState::FlagOp op) noexcept {
    using F = FlagState::FlagOp;
    switch (op) {
        case F::None:  return "none";
        case F::Add8:  return "add8";
        case F::Add16: return "add16";
        case F::Add32: return "add32";
        case F::Add64: return "add64";
        case F::Sub8:  return "sub8";
        case F::Sub16: return "sub16";
        case F::Sub32: return "sub32";
        case F::Sub64: return "sub64";
        case F::And8:  return "and8";
        case F::And16: return "and16";
        case F::And32: return "and32";
        case F::And64: return "and64";
        case F::Or8:   return "or8";
        case F::Or16:  return "or16";
        case F::Or32:  return "or32";
        case F::Or64:  return "or64";
        case F::Xor8:  return "xor8";
        case F::Xor16: return "xor16";
        case F::Xor32: return "xor32";
        case F::Xor64: return "xor64";
        case F::Neg32: return "neg32";
        case F::Shl32: return "shl32";
        case F::Shr32: return "shr32";
        case F::Sar32: return "sar32";
        case F::Inc32: return "inc32";
        case F::Dec32: return "dec32";
        default:       return nullptr;
    }
}

const char* const kGpr64[16] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
};
const char* const kGpr32[8] = {
    "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi",
};
const char* const kGpr16[8] = {
    "ax", "cx", "dx", "bx", "sp", "bp", "si", "di",
};
/// Legacy (no-REX) 8-bit encoding order: indices 4-7 are the high bytes.
const char* const kGpr8[8] = {
    "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh",
};
/// What Reg8 indices 4-7 mean WITH a REX prefix.
const char* const kGpr8Rex[4] = {
    "spl", "bpl", "sil", "dil",
};

void append_format(std::string& out, const char* fmt, ...)
    __attribute__((format(printf, 2, 3)));

void append_format(std::string& out, const char* fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) out.append(buf, static_cast<usize>(n < static_cast<int>(sizeof(buf))
                                                      ? n
                                                      : sizeof(buf) - 1));
}

}  // namespace

const char* gpr_name(GuestReg reg, bool is_64bit) noexcept {
    // The high-byte slots sit above the real register indices.
    if (is_high_byte(reg.width, reg.index))
        return kGpr8[reg.index - kHighByteBase + 4];
    if (reg.index > kMaxGuestGprIndex) return "r?";
    if (reg.width == Type::I64) return kGpr64[reg.index];
    if (reg.index >= 8) return kGpr64[reg.index];  // r8d/r8w/r8b: Stage 2+
    switch (reg.width) {
        case Type::I32: return kGpr32[reg.index];
        case Type::I16: return kGpr16[reg.index];
        // With the high bytes moved out, a Reg8 index of 4-7 is the REX
        // spelling: SPL/BPL/SIL/DIL.
        case Type::I8:  return reg.index < 4 ? kGpr8[reg.index]
                                             : kGpr8Rex[reg.index - 4];
        default: break;
    }
    (void)is_64bit;
    return "r?";
}

// =============================================================================
// IrBlock
// =============================================================================

Type IrBlock::type_of(ValueId v) const noexcept {
    for (const Inst& inst : insts) {
        if (inst.result == v) return inst.type;
    }
    return Type::Void;
}

// =============================================================================
// Builder
// =============================================================================

ValueId IrBuilder::append(Inst inst) {
    inst.guest_rip = m_rip;
    if (inst.type != Type::Void) {
        inst.result = m_block.next_value++;
    } else {
        inst.result = kInvalidValue;
    }
    m_block.insts.push_back(inst);
    return inst.result;
}

ValueId IrBuilder::gpr_read(GuestReg reg) {
    Inst inst;
    inst.op = Op::GprRead;
    inst.type = reg.width;
    inst.operand_count = 1;
    inst.operands[0] = Operand::gpr(reg);
    return append(inst);
}

ValueId IrBuilder::seg_base(u8 segment, Type width) {
    Inst inst;
    inst.op = Op::SegBase;
    inst.type = width;
    inst.operand_count = 1;
    inst.operands[0] = Operand::imm(segment, Type::I32);
    return append(inst);
}

ValueId IrBuilder::xmm_read(u8 index) {
    Inst inst;
    inst.op = Op::XmmRead;
    inst.type = Type::V128;
    inst.operand_count = 1;
    inst.operands[0] = Operand::gpr(GuestReg{index, Type::V128});
    return append(inst);
}

void IrBuilder::xmm_write(u8 index, ValueId value) {
    Inst inst;
    inst.op = Op::XmmWrite;
    inst.type = Type::Void;
    inst.operand_count = 2;
    inst.operands[0] = Operand::gpr(GuestReg{index, Type::V128});
    inst.operands[1] = Operand::value(value, Type::V128);
    append(inst);
}

ValueId IrBuilder::v_insert_low32(ValueId dst, ValueId low32) {
    Inst inst;
    inst.op = Op::VInsertLow32;
    inst.type = Type::V128;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(dst, Type::V128);
    inst.operands[1] = Operand::value(low32, Type::I32);
    return append(inst);
}

ValueId IrBuilder::v_zext32(ValueId low32) {
    Inst inst;
    inst.op = Op::VZext32;
    inst.type = Type::V128;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(low32, Type::I32);
    return append(inst);
}

ValueId IrBuilder::v_low32(ValueId src) {
    Inst inst;
    inst.op = Op::VLow32;
    inst.type = Type::I32;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(src, Type::V128);
    return append(inst);
}

void IrBuilder::gpr_write(GuestReg reg, ValueId value) {
    Inst inst;
    inst.op = Op::GprWrite;
    inst.type = Type::Void;
    inst.operand_count = 2;
    inst.operands[0] = Operand::gpr(reg);
    // The operand type is the guest register's access width, and `verify()`
    // requires the definition to have exactly that type. A 32-bit guest
    // writing AL must therefore build the read-modify-write explicitly; there
    // is no width coercion here to hide it.
    inst.operands[1] = Operand::value(value, reg.width);
    append(inst);
}

ValueId IrBuilder::constant(Type type, u64 bits) {
    Inst inst;
    inst.op = Op::Const;
    inst.type = type;
    inst.operand_count = 1;
    inst.operands[0] = Operand::imm(bits, type);
    return append(inst);
}

ValueId IrBuilder::binary(Op op, Type type, ValueId lhs, ValueId rhs) {
    Inst inst;
    inst.op = op;
    inst.type = type;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(lhs, type);
    inst.operands[1] = Operand::value(rhs, type);
    return append(inst);
}

ValueId IrBuilder::unary(Op op, Type type, ValueId src) {
    Inst inst;
    inst.op = op;
    inst.type = type;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(src, type);
    return append(inst);
}

ValueId IrBuilder::trunc(Type to, ValueId src) {
    Inst inst;
    inst.op = Op::Trunc;
    inst.type = to;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(src, m_block.type_of(src));
    return append(inst);
}

ValueId IrBuilder::vec_shift_imm(Op op, u8 elem, ValueId a, u8 count) {
    Inst inst;
    inst.op = op;
    inst.type = Type::V128;
    inst.vec_elem = elem;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(a, Type::V128);
    inst.operands[1] = Operand::imm(count, Type::I8);
    return append(inst);
}

ValueId IrBuilder::seg_selector(u8 seg) {
    Inst inst;
    inst.op = Op::SegSelector;
    inst.type = Type::I64;
    inst.operand_count = 1;
    inst.operands[0] = Operand::imm(seg, Type::I8);
    return append(inst);
}

void IrBuilder::divide(ValueId divisor, u8 kind, FaultState state) {
    Inst inst;
    inst.op = Op::Divide;
    inst.type = Type::Void;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(divisor, m_block.type_of(divisor));
    inst.operands[1] = Operand::imm(kind, Type::I8);
    inst.may_fault = true;
    inst.fault_state = state;
    (void)append(inst);
}

ValueId IrBuilder::vec_binary(Op op, u8 elem, ValueId a, ValueId b) {
    Inst inst;
    inst.op = op;
    inst.type = Type::V128;
    inst.vec_elem = elem;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(a, Type::V128);
    inst.operands[1] = Operand::value(b, Type::V128);
    return append(inst);
}

ValueId IrBuilder::vec_unary(Op op, u8 elem, ValueId a) {
    Inst inst;
    inst.op = op;
    inst.type = Type::V128;
    inst.vec_elem = elem;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(a, Type::V128);
    return append(inst);
}

ValueId IrBuilder::flags_image(ValueId flags) {
    Inst inst;
    inst.op = Op::FlagsImage;
    inst.type = Type::I64;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(flags, Type::Flags);
    return append(inst);
}

void IrBuilder::flags_install(ValueId value) {
    Inst inst;
    inst.op = Op::FlagsInstall;
    inst.type = Type::Void;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(value, Type::I64);
    (void)append(inst);
}

ValueId IrBuilder::smulh(ValueId a, ValueId b) {
    Inst inst;
    inst.op = Op::SMulH;
    inst.type = Type::I64;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(a, Type::I64);
    inst.operands[1] = Operand::value(b, Type::I64);
    return append(inst);
}

ValueId IrBuilder::select(Type type, ValueId cond, ValueId a, ValueId b) {
    Inst inst;
    inst.op = Op::Select;
    inst.type = type;
    inst.operand_count = 3;
    // The condition carries its OWN width, not a fixed i8. Typing it i8 made
    // every wider condition a silent truncation -- the verifier would have
    // accepted `select` on a 64-bit "is this count zero" and the lowering
    // would have compared only its low 32 bits.
    inst.operands[0] = Operand::value(cond, m_block.type_of(cond));
    inst.operands[1] = Operand::value(a, type);
    inst.operands[2] = Operand::value(b, type);
    return append(inst);
}

ValueId IrBuilder::eval_cc(ValueId flags, u8 cond) {
    Inst inst;
    inst.op = Op::EvalCc;
    inst.type = Type::I8;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(flags, Type::Flags);
    inst.operands[1] = Operand::imm(cond, Type::I8);
    return append(inst);
}

ValueId IrBuilder::carry_binary(Op op, Type type, ValueId a, ValueId b,
                                ValueId cf) {
    Inst inst;
    inst.op = op;
    inst.type = type;
    inst.operand_count = 3;
    inst.operands[0] = Operand::value(a, type);
    inst.operands[1] = Operand::value(b, type);
    inst.operands[2] = Operand::value(cf, Type::I8);
    return append(inst);
}

void IrBuilder::set_flag_overlay(Op op, ValueId bit, Type type) {
    Inst inst;
    inst.op = op;
    inst.type = Type::Void;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(bit, type);
    (void)append(inst);
}

ValueId IrBuilder::shift_imm(Op op, Type type, ValueId src, u64 count) {
    Inst inst;
    inst.op = op;
    inst.type = type;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(src, type);
    inst.operands[1] = Operand::imm(count, Type::I8);
    return append(inst);
}

ValueId IrBuilder::def_flags_shift_reg(const FlagDef& def, ValueId original,
                                       ValueId result, ValueId count, Type type) {
    Inst inst;
    inst.op = Op::DefFlags;
    inst.type = Type::Flags;
    inst.flags = def;
    inst.operand_count = 3;
    inst.operands[0] = Operand::value(original, type);
    inst.operands[1] = Operand::value(result, type);
    inst.operands[2] = Operand::value(count, Type::I8);
    return append(inst);
}

ValueId IrBuilder::shift_reg(Op op, Type type, ValueId src, ValueId count) {
    Inst inst;
    inst.op = op;
    inst.type = type;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(src, type);
    inst.operands[1] = Operand::value(count, Type::I8);
    return append(inst);
}

ValueId IrBuilder::lea(Type type, ValueId base, ValueId index, u64 scale,
                       i64 disp) {
    Inst inst;
    inst.op = Op::Lea;
    inst.type = type;
    inst.operand_count = 4;
    inst.operands[0] = Operand::value(base, type);
    inst.operands[1] = index == kInvalidValue ? Operand{}
                                              : Operand::value(index, type);
    inst.operands[2] = Operand::imm(scale, Type::I8);
    inst.operands[3] = Operand::imm(static_cast<u64>(disp), type);
    return append(inst);
}

ValueId IrBuilder::zext(Type to, ValueId src) {
    Inst inst;
    inst.op = Op::Zext;
    inst.type = to;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(src, m_block.type_of(src));
    return append(inst);
}

ValueId IrBuilder::sext(Type to, ValueId src) {
    Inst inst;
    inst.op = Op::Sext;
    inst.type = to;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(src, m_block.type_of(src));
    return append(inst);
}

ValueId IrBuilder::flags_in() {
    Inst inst;
    inst.op = Op::FlagsIn;
    inst.type = Type::Flags;
    return append(inst);
}

ValueId IrBuilder::def_flags(const FlagDef& def, ValueId a, ValueId b,
                             ValueId merge_from) {
    Inst inst;
    inst.op = Op::DefFlags;
    inst.type = Type::Flags;
    inst.flags = def;
    const Type at = m_block.type_of(a);
    inst.operands[0] = Operand::value(a, at);
    inst.operand_count = 1;
    if (b != kInvalidValue) {
        inst.operands[1] = Operand::value(b, m_block.type_of(b));
        inst.operand_count = 2;
    }
    if (merge_from != kInvalidValue) {
        // Slot 2 is the CF merge edge, and it is only ever a Flags value.
        inst.operands[2] = Operand::value(merge_from, Type::Flags);
        inst.operand_count = 3;
    }
    return append(inst);
}

void IrBuilder::jcc(u8 cond, ValueId flags, u64 target, u64 fallthrough) {
    Inst inst;
    inst.op = Op::Jcc;
    inst.type = Type::Void;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(flags, m_block.type_of(flags));
    inst.branch = BranchInfo{cond, target, fallthrough};
    (void)append(inst);
}

void IrBuilder::cond_exit(u8 cond, ValueId flags, u64 target, u64 fallthrough) {
    Inst inst;
    inst.op = Op::CondExit;
    inst.type = Type::Void;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(flags, m_block.type_of(flags));
    inst.branch = BranchInfo{cond, target, fallthrough};
    (void)append(inst);
}

void IrBuilder::jmp(u64 target) {
    Inst inst;
    inst.op = Op::Jmp;
    inst.type = Type::Void;
    inst.branch = BranchInfo{0, target, kInvalidRip};
    (void)append(inst);
}

void IrBuilder::jmp_indirect(ValueId target, bool is_return) {
    Inst inst;
    inst.op = Op::JmpIndirect;
    inst.type = Type::Void;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(target, m_block.type_of(target));
    inst.branch.is_return = is_return;
    (void)append(inst);
}

void IrBuilder::set_call_return_site(u64 return_site) noexcept {
    if (m_block.insts.empty()) return;
    m_block.insts.back().branch.return_site = return_site;
}

ValueId IrBuilder::load_aligned128(ValueId addr, FaultState state) {
    const ValueId v = load(Type::V128, addr, state);
    m_block.insts.back().mem.align16 = true;
    return v;
}

void IrBuilder::store_aligned128(ValueId addr, ValueId value, FaultState state) {
    store(Type::V128, addr, value, state);
    m_block.insts.back().mem.align16 = true;
}

ValueId IrBuilder::load(Type width, ValueId addr, FaultState state) {
    Inst inst;
    inst.op = Op::Load;
    inst.type = width;
    inst.operand_count = 1;
    inst.operands[0] = Operand::value(addr, m_block.type_of(addr));
    inst.mem = MemAccess{MemClass::Guest, width};
    inst.may_fault = true;
    inst.fault_state = state;
    return append(inst);
}

void IrBuilder::store(Type width, ValueId addr, ValueId value,
                      FaultState state) {
    Inst inst;
    inst.op = Op::Store;
    inst.type = Type::Void;
    inst.operand_count = 2;
    inst.operands[0] = Operand::value(addr, m_block.type_of(addr));
    inst.operands[1] = Operand::value(value, m_block.type_of(value));
    inst.mem = MemAccess{MemClass::Guest, width};
    inst.may_fault = true;
    inst.fault_state = state;
    (void)append(inst);
}

void IrBuilder::mark_may_fault(FaultState state) noexcept {
    if (m_block.insts.empty()) return;
    Inst& inst = m_block.insts.back();
    inst.may_fault = state != FaultState::None;
    inst.fault_state = state;
}

// =============================================================================
// Verifier
// =============================================================================

namespace {

class Failer {
public:
    explicit Failer(std::string* why) noexcept : m_why(why) {}

    /// Always returns false, so every check reads `return fail(...)`.
    bool operator()(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        if (m_why) {
            char buf[256];
            va_list args;
            va_start(args, fmt);
            const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            m_why->assign(buf, static_cast<usize>(n > 0 ? n : 0));
        }
        return false;
    }

private:
    std::string* m_why;
};

/// True when `bits` is representable in `t` either zero- or sign-extended.
/// A literal whose high bits are neither all-zero nor a sign extension is a
/// silent truncation waiting to happen, which is precisely the class of bug
/// the explicit width discipline exists to catch.
bool imm_fits(u64 bits, Type t) noexcept {
    const u32 w = type_bits(t);
    if (w == 0) return false;
    if (w >= 64) return true;
    const u64 zero_ext = bits >> w;
    if (zero_ext == 0) return true;
    const u64 sign_ext = static_cast<u64>(static_cast<i64>(bits << (64 - w)) >>
                                          (64 - w));
    return sign_ext == bits;
}

/// Per-op shape and type rules. Operand *definitions* have already been
/// checked to exist and to carry the type the use claims, so this only has to
/// state each op's own arity and width rule.
bool verify_op_shape(const IrBlock& block, const Inst& in, u32 i, Failer& fail) {
    const auto operand_is = [&](u32 k, OperandKind kind) {
        return in.operand_count > k && in.operands[k].kind == kind;
    };

    switch (in.op) {
        case Op::GprRead: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::GuestReg))
                return fail("op %u: gpr.read takes exactly one register operand", i);
            if (!is_integer_type(in.type))
                return fail("op %u: gpr.read result must be an integer type", i);
            if (in.operands[0].type != in.type)
                return fail("op %u: gpr.read width %s does not match result type %s",
                            i, type_name(in.operands[0].type), type_name(in.type));
            break;
        }
        case Op::GprWrite: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::GuestReg) ||
                !operand_is(1, OperandKind::Value))
                return fail("op %u: gpr.write takes a register and a value", i);
            if (in.type != Type::Void)
                return fail("op %u: gpr.write defines no value", i);
            if (in.operands[1].type != in.operands[0].type)
                return fail("op %u: gpr.write of %s into a %s register; widen or "
                            "narrow explicitly", i,
                            type_name(in.operands[1].type),
                            type_name(in.operands[0].type));
            break;
        }
        case Op::Const: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Imm))
                return fail("op %u: const takes exactly one immediate", i);
            if (!is_integer_type(in.type))
                return fail("op %u: const result must be an integer type", i);
            if (!imm_fits(in.operands[0].payload, in.type))
                return fail("op %u: const 0x%llx does not fit %s", i,
                            static_cast<unsigned long long>(in.operands[0].payload),
                            type_name(in.type));
            break;
        }
        case Op::SegBase: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Imm))
                return fail("op %u: seg.base takes one immediate", i);
            if (in.type != Type::I32 && !(block.is_64bit && in.type == Type::I64))
                return fail("op %u: seg.base width must match supported address mode", i);
            break;
        }
        case Op::XmmRead: {
            if (in.operand_count != 1 ||
                !operand_is(0, OperandKind::GuestReg)) {
                return fail("op %u: xmm.read takes one register operand", i);
            }
            if (in.type != Type::V128)
                return fail("op %u: xmm.read result must be v128", i);
            if (in.operands[0].reg_index >= 8 && !block.is_64bit) {
                return fail("op %u: xmm.read of XMM%u in a 32-bit block", i,
                            in.operands[0].reg_index);
            }
            break;
        }
        case Op::XmmWrite: {
            if (in.operand_count != 2 ||
                !operand_is(0, OperandKind::GuestReg) ||
                !operand_is(1, OperandKind::Value)) {
                return fail("op %u: xmm.write takes a register and a value", i);
            }
            if (in.type != Type::Void)
                return fail("op %u: xmm.write defines no value", i);
            if (in.operands[1].type != Type::V128)
                return fail("op %u: xmm.write value must be v128", i);
            if (in.operands[0].reg_index >= 8 && !block.is_64bit) {
                return fail("op %u: xmm.write of XMM%u in a 32-bit block", i,
                            in.operands[0].reg_index);
            }
            break;
        }
        case Op::VAbs:
            if (in.vec_elem > 3)
                return fail("op %u: %s element size %u is not a VSize", i,
                            op_name(in.op), in.vec_elem);
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: %s takes one value operand", i,
                            op_name(in.op));
            if (in.type != Type::V128)
                return fail("op %u: %s produces a V128", i, op_name(in.op));
            break;
        // SMIN/SMAX/UMIN/UMAX, MUL and URHADD have NO 64-bit element form:
        // ARM64 leaves size=0b11 unallocated for all six, so emitting one at
        // element size 3 produces an illegal instruction rather than a wrong
        // answer. Nothing generates these today -- x86 has no PMINSQ/PMULLQ
        // outside AVX512 -- but the constraint belongs here and not in the
        // frontend's head, because the backend reads `vec_elem` and encodes it
        // without looking. The same class of latent defect as emitting a W-form
        // shift for a 64-bit count.
        case Op::VMinU: case Op::VMaxU: case Op::VMinS: case Op::VMaxS:
        case Op::VMulLo: case Op::VAvgU:
            if (in.vec_elem > 2)
                return fail("op %u: %s has no 64-bit element form on ARM64 "
                            "(element size %u)", i, op_name(in.op),
                            in.vec_elem);
            [[fallthrough]];
        case Op::VAdd: case Op::VSub: case Op::VCmpEq: case Op::VCmpGt:
        case Op::VQAddS: case Op::VQAddU: case Op::VQSubS: case Op::VQSubU:
        case Op::VZipLo: case Op::VZipHi:
        case Op::VPackS: case Op::VPackU:
            if (in.vec_elem > 3)
                return fail("op %u: %s element size %u is not a VSize", i,
                            op_name(in.op), in.vec_elem);
            [[fallthrough]];
        case Op::VAnd:
        case Op::VOr:
        case Op::VXor: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Value)) {
                return fail("op %u: %s takes two value operands", i,
                            op_name(in.op));
            }
            if (in.type != Type::V128 ||
                in.operands[0].type != Type::V128 ||
                in.operands[1].type != Type::V128) {
                return fail("op %u: %s is v128 only", i, op_name(in.op));
            }
            break;
        }
        case Op::VShlImm: case Op::VShrImm: case Op::VSarImm: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Imm))
                return fail("op %u: %s takes a vector and a count", i,
                            op_name(in.op));
            if (in.type != Type::V128 || in.operands[0].type != Type::V128)
                return fail("op %u: %s is v128 only", i, op_name(in.op));
            if (in.vec_elem > 3)
                return fail("op %u: %s element size %u is not a VSize", i,
                            op_name(in.op), in.vec_elem);
            // A count at or past the element width is NOT a shift -- x86
            // produces all-zero (or all-sign) and the frontend resolves that
            // before building one, so the IR never carries it.
            const u32 bits = 8u << in.vec_elem;
            if (in.operands[1].payload == 0 || in.operands[1].payload >= bits)
                return fail("op %u: %s count %llu is not in 1..%u", i,
                            op_name(in.op),
                            static_cast<unsigned long long>(
                                in.operands[1].payload), bits - 1);
            break;
        }
        case Op::VInsertLow32: {
            // Operand 0 supplies bits 127:32, operand 1 bits 31:0. Getting
            // these the wrong way round is the MOVSS merge bug, and it is
            // invisible whenever the destination's upper lanes are zero.
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Value)) {
                return fail("op %u: v.insert.low32 takes two value operands",
                            i);
            }
            if (in.type != Type::V128 || in.operands[0].type != Type::V128)
                return fail("op %u: v.insert.low32 merges into a v128", i);
            if (in.operands[1].type != Type::I32)
                return fail("op %u: v.insert.low32 inserts an i32", i);
            break;
        }
        case Op::VZext32: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: v.zext32 takes one value operand", i);
            if (in.type != Type::V128 || in.operands[0].type != Type::I32)
                return fail("op %u: v.zext32 is i32 -> v128", i);
            break;
        }
        case Op::VLow32: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: v.low32 takes one value operand", i);
            if (in.type != Type::I32 || in.operands[0].type != Type::V128)
                return fail("op %u: v.low32 is v128 -> i32", i);
            break;
        }
        case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Xor: {
            if (in.operand_count != 2)
                return fail("op %u: %s takes two operands", i, op_name(in.op));
            if (!is_integer_type(in.type))
                return fail("op %u: %s result must be an integer type", i,
                            op_name(in.op));
            for (u32 k = 0; k < 2; ++k) {
                // Immediates are not accepted here on purpose: `Const` makes a
                // literal an ordinary SSA value, so every later pass (CSE,
                // constant folding, the Stage 2 EA commoner) sees one operand
                // shape instead of two.
                if (!operand_is(k, OperandKind::Value))
                    return fail("op %u: %s operand %u must be a value", i,
                                op_name(in.op), k);
                if (in.operands[k].type != in.type)
                    return fail("op %u: %s operand %u is %s but result is %s; "
                                "there is no implicit width coercion", i,
                                op_name(in.op), k,
                                type_name(in.operands[k].type),
                                type_name(in.type));
            }
            break;
        }
        case Op::Not: case Op::Neg: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: %s takes one value operand", i, op_name(in.op));
            if (!is_integer_type(in.type) || in.operands[0].type != in.type)
                return fail("op %u: %s operand type %s must equal result type %s",
                            i, op_name(in.op), type_name(in.operands[0].type),
                            type_name(in.type));
            break;
        }
        case Op::Shl: case Op::Shr: case Op::Sar:
        case Op::Rol: case Op::Ror: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value))
                return fail("op %u: %s takes a value and a count", i,
                            op_name(in.op));
            if (!is_integer_type(in.type) || in.operands[0].type != in.type)
                return fail("op %u: %s shifted value is %s but result is %s", i,
                            op_name(in.op), type_name(in.operands[0].type),
                            type_name(in.type));
            const Operand& count = in.operands[1];
            if (count.kind == OperandKind::Imm) {
                // A count >= width is not a shift, it is a different
                // instruction (x86 masks the count by 31/63 first). Making the
                // IR reject it forces the masking to be explicit in the
                // lowering that knows the guest rule.
                if (count.payload >= type_bits(in.type))
                    return fail("op %u: %s count %llu is not below the %u-bit "
                                "width; mask the count explicitly", i,
                                op_name(in.op),
                                static_cast<unsigned long long>(count.payload),
                                type_bits(in.type));
            } else if (count.kind == OperandKind::Value) {
                if (count.type != Type::I8)
                    return fail("op %u: %s runtime count must be i8, not %s", i,
                                op_name(in.op), type_name(count.type));
            } else {
                return fail("op %u: %s count must be an immediate or a value", i,
                            op_name(in.op));
            }
            break;
        }
        case Op::Lea: {
            if (in.operand_count != 4)
                return fail("op %u: lea takes base, index, scale, disp", i);
            if (in.type != Type::I32 && in.type != Type::I64)
                return fail("op %u: lea result must be i32 or i64", i);
            if (!operand_is(0, OperandKind::Value) ||
                in.operands[0].type != in.type)
                return fail("op %u: lea base must be a value of the result type",
                            i);
            const Operand& index = in.operands[1];
            if (index.kind == OperandKind::Value) {
                if (index.type != in.type)
                    return fail("op %u: lea index is %s but result is %s", i,
                                type_name(index.type), type_name(in.type));
            } else if (index.kind != OperandKind::None) {
                return fail("op %u: lea index must be a value or absent", i);
            }
            if (!operand_is(2, OperandKind::Imm))
                return fail("op %u: lea scale must be an immediate", i);
            const u64 scale = in.operands[2].payload;
            if (scale != 1 && scale != 2 && scale != 4 && scale != 8)
                return fail("op %u: lea scale %llu is not 1, 2, 4 or 8", i,
                            static_cast<unsigned long long>(scale));
            if (!operand_is(3, OperandKind::Imm) ||
                !imm_fits(in.operands[3].payload, in.type))
                return fail("op %u: lea displacement must be an immediate that "
                            "fits %s", i, type_name(in.type));
            break;
        }
        case Op::Mul: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Value))
                return fail("op %u: mul takes two value operands", i);
            if (!is_integer_type(in.type) ||
                in.operands[0].type != in.type || in.operands[1].type != in.type)
                return fail("op %u: mul operands are %s and %s but the result "
                            "is %s", i, type_name(in.operands[0].type),
                            type_name(in.operands[1].type), type_name(in.type));
            break;
        }
        case Op::SMulH: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Value))
                return fail("op %u: smulh takes two value operands", i);
            // I64 only by construction: at every narrower width the full
            // product fits an I64 `Mul`, so a narrow `SMulH` would be a second
            // way to say the same thing, and the lowering has no encoding for
            // it either.
            if (in.type != Type::I64 || in.operands[0].type != Type::I64 ||
                in.operands[1].type != Type::I64)
                return fail("op %u: smulh is i64 only", i);
            break;
        }
        case Op::SegSelector: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Imm))
                return fail("op %u: seg.selector takes one immediate", i);
            if (in.type != Type::I64)
                return fail("op %u: seg.selector result must be i64", i);
            break;
        }
        case Op::Divide: {
            if (in.operand_count != 2 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Imm))
                return fail("op %u: divide takes a divisor and a kind", i);
            if (in.type != Type::Void)
                return fail("op %u: divide defines no value; it writes the "
                            "implicit accumulator pair", i);
            if (in.operands[1].payload > 7)
                return fail("op %u: divide kind %llu is not a DivideKind", i,
                            static_cast<unsigned long long>(
                                in.operands[1].payload));
            constexpr Type kWidths[] = {Type::I8, Type::I16, Type::I32,
                                        Type::I64};
            const unsigned pair = static_cast<unsigned>(in.operands[1].payload >> 1);
            if (in.operands[0].type != kWidths[pair])
                return fail("op %u: divide divisor is %s but the form is %u-bit",
                            i, type_name(in.operands[0].type), 8u << pair);
            if (!in.may_fault)
                return fail("op %u: divide must declare that it can fault "
                            "(#DE on a zero divisor or an unrepresentable "
                            "quotient)", i);
            break;
        }
        case Op::FlagsImage: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value) ||
                in.operands[0].type != Type::Flags)
                return fail("op %u: flags.image takes the live flags value", i);
            if (in.type != Type::I64)
                return fail("op %u: flags.image result must be i64", i);
            break;
        }
        case Op::FlagsInstall: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value) ||
                in.operands[0].type != Type::I64)
                return fail("op %u: flags.install takes one i64 value", i);
            if (in.type != Type::Void)
                return fail("op %u: flags.install defines no value", i);
            break;
        }
        case Op::Select: {
            if (in.operand_count != 3 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Value) ||
                !operand_is(2, OperandKind::Value))
                return fail("op %u: select takes a condition and two values", i);
            if (!is_integer_type(in.operands[0].type))
                return fail("op %u: select condition is %s; it must be an "
                            "integer tested against zero", i,
                            type_name(in.operands[0].type));
            if (in.operands[1].type != in.type || in.operands[2].type != in.type)
                return fail("op %u: select arms are %s and %s but the result "
                            "is %s", i, type_name(in.operands[1].type),
                            type_name(in.operands[2].type), type_name(in.type));
            break;
        }
        case Op::AddCarry: case Op::SubBorrow: {
            if (in.operand_count != 3 || !operand_is(0, OperandKind::Value) ||
                !operand_is(1, OperandKind::Value) ||
                !operand_is(2, OperandKind::Value))
                return fail("op %u: %s takes two values and a carry", i,
                            op_name(in.op));
            if (in.type != Type::I8 && in.type != Type::I16 && in.type != Type::I32)
                return fail("op %u: %s is I8-I32 only, has %s", i,
                            op_name(in.op), type_name(in.type));
            if (in.operands[0].type != in.type || in.operands[1].type != in.type)
                return fail("op %u: %s operands are %s and %s but the result "
                            "is %s", i, op_name(in.op),
                            type_name(in.operands[0].type),
                            type_name(in.operands[1].type), type_name(in.type));
            if (in.operands[2].type != Type::I8)
                return fail("op %u: %s carry must be an I8 0/1", i,
                            op_name(in.op));
            break;
        }
        case Op::Trunc: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: trunc takes one value operand", i);
            if (!is_integer_type(in.type) || !is_integer_type(in.operands[0].type))
                return fail("op %u: trunc operates on integer types only", i);
            if (type_bits(in.operands[0].type) <= type_bits(in.type))
                return fail("op %u: trunc from %s to %s does not narrow", i,
                            type_name(in.operands[0].type), type_name(in.type));
            break;
        }
        case Op::Zext: case Op::Sext: {
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: %s takes one value operand", i, op_name(in.op));
            if (!is_integer_type(in.type) || !is_integer_type(in.operands[0].type))
                return fail("op %u: %s operates on integer types only", i,
                            op_name(in.op));
            if (type_bits(in.operands[0].type) >= type_bits(in.type))
                return fail("op %u: %s from %s to %s does not widen", i,
                            op_name(in.op), type_name(in.operands[0].type),
                            type_name(in.type));
            break;
        }
        case Op::FlagsIn: {
            if (in.type != Type::Flags)
                return fail("op %u: flags.in result type must be flags", i);
            if (in.operand_count != 0)
                return fail("op %u: flags.in takes no operands", i);
            // One per block. Two would be two names for the same architectural
            // state, which is exactly the ambiguity SSA exists to remove.
            for (u32 j = 0; j < i; ++j) {
                if (block.insts[j].op == Op::FlagsIn)
                    return fail("op %u: a block has at most one flags.in "
                                "(already defined at op %u)", i, j);
            }
            break;
        }

        case Op::DefFlags: {
            {
                // The narrow records pack their count the same way their
                // 32-bit siblings do, so the rule is the family's, not one
                // width's.
                const bool is_shift =
                    in.flags.record == FlagState::FlagOp::Shl8 ||
                    in.flags.record == FlagState::FlagOp::Shl16 ||
                    in.flags.record == FlagState::FlagOp::Shl32 ||
                    in.flags.record == FlagState::FlagOp::Shr8 ||
                    in.flags.record == FlagState::FlagOp::Shr16 ||
                    in.flags.record == FlagState::FlagOp::Shr32 ||
                    in.flags.record == FlagState::FlagOp::Sar8 ||
                    in.flags.record == FlagState::FlagOp::Sar16 ||
                    in.flags.record == FlagState::FlagOp::Sar32;
                // A by-CL shift carries its count as a VALUE instead, because
                // `shift_count` is a compile-time constant and the guest's is
                // not. The record format already allows it -- the publisher
                // packs a count REGISTER -- so this is the same record with a
                // third operand rather than a different one.
                const bool runtime_count = is_shift && in.operand_count == 3 &&
                    in.operands[2].kind == OperandKind::Value &&
                    in.operands[2].type == Type::I8;
                if (runtime_count) {
                    if (in.flags.shift_count != 0)
                        return fail("op %u: a runtime-count shift must not also "
                                    "carry a compile-time count", i);
                    break;
                }
                if (is_shift && (in.flags.shift_count == 0 ||
                                 in.flags.shift_count > 31)) {
                    return fail("op %u: shift record needs a count of 1-31, "
                                "has %u; a zero-count shift modifies no flags "
                                "and must define none", i,
                                in.flags.shift_count);
                }
                if (!is_shift && in.flags.shift_count != 0) {
                    return fail("op %u: shift_count is meaningless for %s", i,
                                flag_op_name(in.flags.record));
                }
            }
            if (in.type != Type::Flags)
                return fail("op %u: defflags result type must be flags", i);
            const bool merges = preserves_cf(in.flags.record);
            const u32 value_sources =
                in.operand_count >= 3 ? in.operand_count - 1 : in.operand_count;
            if (value_sources == 0 || value_sources > 2)
                return fail("op %u: defflags takes one or two value sources", i);
            for (u32 k = 0; k < value_sources; ++k) {
                if (!operand_is(k, OperandKind::Value) ||
                    !is_integer_type(in.operands[k].type))
                    return fail("op %u: defflags source %u must be an integer "
                                "value", i, k);
            }
            if (value_sources == 2 &&
                in.operands[0].type != in.operands[1].type)
                return fail("op %u: defflags sources disagree on width (%s vs %s)",
                            i, type_name(in.operands[0].type),
                            type_name(in.operands[1].type));

            // The CF merge edge, enforced in BOTH directions. A CF-preserving
            // record with no incoming flag value would silently invent a
            // carry; a merge edge on a record that DEFINES CF would claim an
            // inheritance that does not exist.
            const bool has_merge = in.operand_count == 3;
            if (merges && !has_merge)
                return fail("op %u: %s preserves CF but names no flag value to "
                            "inherit it from", i,
                            flag_op_name(in.flags.record)
                                ? flag_op_name(in.flags.record) : "#?");
            if (!merges && has_merge)
                return fail("op %u: %s defines CF, so it must not carry a "
                            "merge edge", i,
                            flag_op_name(in.flags.record)
                                ? flag_op_name(in.flags.record) : "#?");
            if (has_merge &&
                (!operand_is(2, OperandKind::Value) ||
                 in.operands[2].type != Type::Flags))
                return fail("op %u: defflags merge source must be a flags "
                            "value", i);

            // Lattice consistency. Each element names exactly one home, so a
            // node carrying an NZCV kind it cannot honour, or a record it did
            // not publish, is malformed rather than merely optimistic.
            const FlagDef& f = in.flags;
            switch (f.repr) {
                case FlagRepr::Dead:
                    if (f.nzcv != NzcvKind::None || f.record != FlagState::FlagOp::None)
                        return fail("op %u: dead flag def still claims a home "
                                    "(nzcv=%s record=%s)", i,
                                    nzcv_kind_name(f.nzcv),
                                    flag_op_name(f.record) ? flag_op_name(f.record)
                                                           : "#?");
                    break;
                case FlagRepr::InPstate:
                    if (f.nzcv == NzcvKind::None)
                        return fail("op %u: flag def lives in PSTATE but names no "
                                    "NZCV kind", i);
                    if (f.record != FlagState::FlagOp::None)
                        return fail("op %u: flag def claims PSTATE and a lazy "
                                    "record at once; use record + nzcv", i);
                    break;
                case FlagRepr::InCpuStateRecord:
                    if (f.record == FlagState::FlagOp::None)
                        return fail("op %u: flag def claims a lazy record of "
                                    "FlagOp::None", i);
                    break;
                case FlagRepr::InRflags:
                    if (f.nzcv != NzcvKind::None ||
                        f.record != FlagState::FlagOp::None)
                        return fail("op %u: materialized-RFLAGS flag def must "
                                    "not also name PSTATE or a record", i);
                    break;
            }
            if (f.keep_overlay && f.repr != FlagRepr::Dead)
                return fail("op %u: keep_overlay is only meaningful for an "
                            "omitted (dead) record", i);
            break;
        }

        case Op::JmpIndirect: {
            if (in.type != Type::Void)
                return fail("op %u: %s defines no value", i, op_name(in.op));
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value))
                return fail("op %u: jmp.indirect takes one target value", i);
            // Same width discipline as a memory address, and for the same
            // reason: a truncated indirect target is a jump into nowhere that
            // is invisible below 4 GB.
            const Type want = block.is_64bit ? Type::I64 : Type::I32;
            if (in.operands[0].type != want)
                return fail("op %u: jmp.indirect target is %s but a %u-bit "
                            "block's addresses are %s", i,
                            type_name(in.operands[0].type),
                            block.is_64bit ? 64u : 32u, type_name(want));
            const u32 total = static_cast<u32>(block.insts.size());
            if (i + 1 != total)
                return fail("op %u: %s is a terminator but %u op(s) follow it",
                            i, op_name(in.op), total - i - 1);
            break;
        }

        case Op::Jcc:
        case Op::Jmp: {
            if (in.type != Type::Void)
                return fail("op %u: %s defines no value", i, op_name(in.op));
            if (in.branch.target == kInvalidRip)
                return fail("op %u: %s has no target", i, op_name(in.op));
            if (in.op == Op::Jcc) {
                if (in.operand_count != 1)
                    return fail("op %u: jcc takes exactly one flags operand", i);
                if (!operand_is(0, OperandKind::Value) ||
                    in.operands[0].type != Type::Flags)
                    return fail("op %u: jcc condition must be a flags value", i);
                if (in.branch.cond > 0xF)
                    return fail("op %u: jcc condition code %u is not a valid "
                                "x86 condition", i, in.branch.cond);
                if (in.branch.fallthrough == kInvalidRip)
                    return fail("op %u: jcc has no fall-through target", i);
            } else if (in.operand_count != 0) {
                return fail("op %u: jmp takes no operands", i);
            }
            // A terminator must END the block. This is what keeps
            // `eliminate_dead_flags`' linear killer scan sound — see
            // `is_terminator`.
            const u32 op_count = static_cast<u32>(block.insts.size());
            if (i + 1 != op_count)
                return fail("op %u: %s is a terminator but %u op(s) follow it",
                            i, op_name(in.op), op_count - i - 1);
            break;
        }

        case Op::CondExit: {
            if (in.type != Type::Void)
                return fail("op %u: cond_exit defines no value", i);
            if (in.operand_count != 1 || !operand_is(0, OperandKind::Value) ||
                in.operands[0].type != Type::Flags)
                return fail("op %u: cond_exit takes exactly one flags operand", i);
            if (in.branch.cond > 0xF)
                return fail("op %u: cond_exit condition code %u is not a valid "
                            "x86 condition", i, in.branch.cond);
            if (in.branch.target == kInvalidRip ||
                in.branch.fallthrough == kInvalidRip)
                return fail("op %u: cond_exit needs both edges", i);
            // The block continues past it; a branch that ends the block is a
            // terminator `jcc`.
            if (i + 1 == static_cast<u32>(block.insts.size()))
                return fail("op %u: cond_exit is the last op; use jcc", i);
            // The exit edge leaves with every flag live, so the operand must
            // be the LIVE flags value -- the latest flags definition before
            // it. That is what makes `eliminate_dead_flags` keep (publish)
            // the definition the successor reads.
            ValueId live = kInvalidValue;
            for (u32 j = i; j-- > 0;) {
                const Inst& prior = block.insts[j];
                if (prior.defines_value() && prior.type == Type::Flags) {
                    live = prior.result;
                    break;
                }
            }
            if (live == kInvalidValue || in.operands[0].as_value() != live)
                return fail("op %u: cond_exit must test the block's live flags", i);
            break;
        }

        case Op::Load:
        case Op::Store: {
            const bool is_store = in.op == Op::Store;
            const u32 want = is_store ? 2u : 1u;
            if (in.operand_count != want)
                return fail("op %u: %s takes %u operand(s)", i,
                            op_name(in.op), want);
            if (!operand_is(0, OperandKind::Value))
                return fail("op %u: %s address must be a value", i,
                            op_name(in.op));

            // THE ADDRESS WIDTH RULE, and the reason it is stated as an
            // invariant rather than left to each lowering.
            //
            // A guest address must be exactly the guest's pointer width: I32
            // in a 32-bit block, I64 in a 64-bit one. Vitreo has a recorded
            // FAMILY of bugs — four instances, all the same shape — where a
            // 64-bit effective address was marshalled through a 32-bit move
            // and silently truncated. Every one of them was invisible below
            // 4 GB, which is where oracle tests run, so they survived the
            // test suite and surfaced as wild pointers in real guests.
            //
            // Here that mistake cannot be built: the operand carries its type,
            // the type must match the block's bitness, and a mismatch is a
            // rejected block rather than a wrong address.
            const Type want_addr = block.is_64bit ? Type::I64 : Type::I32;
            if (in.operands[0].type != want_addr)
                return fail("op %u: %s address is %s but a %u-bit block's "
                            "addresses are %s (truncation would be silent "
                            "below 4 GB)", i, op_name(in.op),
                            type_name(in.operands[0].type),
                            block.is_64bit ? 64u : 32u, type_name(want_addr));

            // `V128` joins the integer widths: it is a 16-byte access whose
            // value happens to live in a vector register. `align16` belongs to
            // it alone -- the 16-byte ALIGNED copies raise #GP off a multiple
            // of 16 and the unaligned spellings of the same copy do not.
            if (!is_integer_type(in.mem.width) && in.mem.width != Type::V128)
                return fail("op %u: %s width must be an integer or v128 type",
                            i, op_name(in.op));
            if (in.mem.align16 && in.mem.width != Type::V128)
                return fail("op %u: %s asks for 16-byte alignment at width %s;"
                            " only a v128 access has that rule", i,
                            op_name(in.op), type_name(in.mem.width));
            if (is_store) {
                if (!operand_is(1, OperandKind::Value))
                    return fail("op %u: store value must be a value", i);
                if (in.operands[1].type != in.mem.width)
                    return fail("op %u: store value is %s but the access width "
                                "is %s", i, type_name(in.operands[1].type),
                                type_name(in.mem.width));
                if (in.type != Type::Void)
                    return fail("op %u: store defines no value", i);
            } else {
                if (in.type != in.mem.width)
                    return fail("op %u: load result is %s but the access width "
                                "is %s", i, type_name(in.type),
                                type_name(in.mem.width));
            }

            // A memory access that claims it cannot fault is the shape the
            // whole recovery model exists to prevent. There is no such thing
            // as a guest access with no fault path.
            if (!in.may_fault || in.fault_state == FaultState::None)
                return fail("op %u: %s must declare may_fault", i,
                            op_name(in.op));
            break;
        }
    }

    // Guest register index must exist in this guest's register file. A 32-bit
    // block naming r8 would silently index the wrong pin.
    for (u32 k = 0; k < in.operand_count; ++k) {
        if (in.operands[k].kind != OperandKind::GuestReg) continue;
        const u8 idx = in.operands[k].reg_index;
        const Type width = in.operands[k].type;
        if (idx > kMaxGuestRegOperandIndex)
            return fail("op %u: guest register index %u out of range", i, idx);
        // Above the real registers only the four high-byte slots exist, and
        // only at I8. Anything else there is a frontend that forgot to
        // normalize, which would otherwise read as a plausible register.
        if (idx > kMaxGuestGprIndex && !is_high_byte(width, idx))
            return fail("op %u: index %u is the high-byte space and needs i8",
                        i, idx);
        if (!block.is_64bit && architectural_gpr(width, idx) >= 8)
            return fail("op %u: guest register index %u does not exist in a "
                        "32-bit block", i, idx);
        if (!block.is_64bit && in.operands[k].type == Type::I64)
            return fail("op %u: 64-bit register access in a 32-bit block", i);
    }
    return true;
}

/// The flag/fault invariant, and the reason this IR exists.
///
/// A flag definition is live from its defining op until the next `DefFlags`
/// (its killer), or to the end of the block if there is none. Across that
/// interval:
///
///   * `Dead` requires a killer AND no `may_fault` op in between. This is the
///     exact condition `compute_omit_lazy_flags` fails to check
///     (`flag_liveness.cpp:604-609`, versus the guard that
///     `pending_flags_dead_across` does apply at `:355-362`): `classify()` is
///     opcode-name-based and calls `MOV [mem], eax` Neutral, so
///     `CMP eax,imm ; MOV [mem],eax ; ADD eax,1` omits the CMP's record and a
///     fault on the MOV exposes pre-CMP RFLAGS to the exception record,
///     `GetThreadContext` or `PUSHF`. Here it is one forward scan and it is
///     mechanical.
///   * `InPstate` is never sufficient across a faulting op: the signal handler
///     does not persist host NZCV, so the flags simply are not there to
///     recover.
///   * `InCpuStateRecord` / `InRflags` are architecturally visible, but the
///     faulting op must say so — `fault_state` must be `RipAndFlags`, not
///     `RipOnly`. That is what makes "an optimization that cannot describe its
///     exception state falls back, it does not guess" a checkable claim rather
///     than a slogan.
bool verify_flag_fault_invariant(const IrBlock& block, Failer& fail) {
    const u32 n = static_cast<u32>(block.insts.size());
    for (u32 i = 0; i < n; ++i) {
        const Inst& def = block.insts[i];
        if (def.op != Op::DefFlags) continue;

        u32 killer = n;
        for (u32 j = i + 1; j < n; ++j) {
            if (block.insts[j].op == Op::DefFlags) { killer = j; break; }
        }

        if (def.flags.repr == FlagRepr::Dead && killer == n) {
            return fail("op %u: flag def %%%u is marked dead but nothing "
                        "redefines flags before the block ends; it is live-out",
                        i, def.result);
        }
        if (def.flags.exit_deferred &&
            (killer == n || def.flags.repr != FlagRepr::InCpuStateRecord)) {
            return fail("op %u: flag def %%%u is exit-deferred but is not a "
                        "record killed inside the block", i, def.result);
        }

        for (u32 j = i + 1; j < killer; ++j) {
            const Inst& mid = block.insts[j];
            if (!mid.may_fault) continue;
            // A recovery plan lifts this rule for exactly the instructions
            // it covers. The check is deliberately per-fault-site and matched
            // to THIS definition: accepting a Dead definition because some
            // plan exists elsewhere in the block would make the rule vacuous
            // the moment a block had two.
            bool covered = false;
            for (const FlagRecoveryPlan& plan : block.flag_recovery) {
                if (plan.covers(def.guest_rip, mid.guest_rip)) {
                    covered = true;
                    break;
                }
            }
            if (covered && def.flags.repr == FlagRepr::Dead) continue;
            // An exit-deferred record is absent on the fall-through, exactly
            // like a Dead one, so every fault before its killer needs a plan.
            if (def.flags.exit_deferred) {
                if (covered) continue;
                return fail("op %u: flag def %%%u publishes only on its exit "
                            "edge but op %u may fault before its killer "
                            "without a recovery plan", i, def.result, j);
            }

            switch (def.flags.repr) {
                case FlagRepr::Dead:
                    return fail("op %u: flag def %%%u is marked dead but op %u "
                                "may fault before its killer; a fault would "
                                "expose stale RFLAGS",
                                i, def.result, j);
                case FlagRepr::InPstate:
                    return fail("op %u: flag def %%%u lives only in PSTATE "
                                "across faulting op %u; the fault handler does "
                                "not persist NZCV", i, def.result, j);
                case FlagRepr::InCpuStateRecord:
                case FlagRepr::InRflags:
                    if (mid.fault_state != FaultState::RipAndFlags) {
                        return fail("op %u may fault with flag def %%%u live but "
                                    "declares fault_state=%s; it must declare "
                                    "rip+flags", j, def.result,
                                    fault_state_name(mid.fault_state));
                    }
                    break;
            }
        }
    }
    return true;
}

}  // namespace

bool verify(const IrBlock& block, std::string* why) {
    Failer fail(why);

    // SSA bookkeeping. Two small vectors sized by the block's value count;
    // this runs once per attempted block, not per emitted instruction.
    std::vector<u8> defined(block.next_value, 0);
    std::vector<Type> def_type(block.next_value, Type::Void);

    u64 prev_rip = kInvalidRip;
    bool have_prev_rip = false;

    const u32 n = static_cast<u32>(block.insts.size());
    for (u32 i = 0; i < n; ++i) {
        const Inst& in = block.insts[i];

        // ---- guest_rip is mandatory -------------------------------------
        if (in.guest_rip == kInvalidRip) {
            return fail("op %u (%s) has no guest_rip; it could not be attributed "
                        "to a guest instruction at a fault", i, op_name(in.op));
        }
        if (have_prev_rip && in.guest_rip != prev_rip && in.guest_rip < prev_rip) {
            // Ops must form contiguous ascending runs by RIP. Reordering
            // inside one guest instruction's interval is free; crossing a
            // boundary is what the fault side table cannot describe, so the
            // strong rule is enforced and relaxed later only with fixups.
            return fail("op %u: guest_rip 0x%llx precedes the previous op's "
                        "0x%llx; ops must stay in ascending RIP intervals", i,
                        static_cast<unsigned long long>(in.guest_rip),
                        static_cast<unsigned long long>(prev_rip));
        }
        prev_rip = in.guest_rip;
        have_prev_rip = true;

        // ---- fault declaration agrees with itself ------------------------
        if (in.may_fault && in.fault_state == FaultState::None)
            return fail("op %u may fault but declares no fault state", i);
        if (!in.may_fault && in.fault_state != FaultState::None)
            return fail("op %u cannot fault but declares fault_state=%s", i,
                        fault_state_name(in.fault_state));

        if (in.operand_count > kMaxOperands)
            return fail("op %u has %u operands, more than the %u slots it "
                        "carries", i, in.operand_count, kMaxOperands);

        // ---- uses are defined, and typed as the use claims ---------------
        for (u32 k = 0; k < in.operand_count; ++k) {
            const Operand& o = in.operands[k];
            if (o.kind != OperandKind::Value) continue;
            const ValueId v = o.as_value();
            if (v >= block.next_value)
                return fail("op %u operand %u uses %%%u, which was never "
                            "numbered", i, k, v);
            if (!defined[v])
                return fail("op %u operand %u uses %%%u before it is defined",
                            i, k, v);
            if (def_type[v] != o.type)
                return fail("op %u operand %u uses %%%u as %s but it is defined "
                            "as %s", i, k, v, type_name(o.type),
                            type_name(def_type[v]));
        }

        if (!verify_op_shape(block, in, i, fail)) return false;

        // ---- the definition itself ---------------------------------------
        if (in.type == Type::Void) {
            if (in.result != kInvalidValue)
                return fail("op %u defines %%%u but has void type", i, in.result);
        } else {
            if (in.result == kInvalidValue)
                return fail("op %u has type %s but defines no value", i,
                            type_name(in.type));
            if (in.result >= block.next_value)
                return fail("op %u defines %%%u, outside the block's value "
                            "numbering", i, in.result);
            if (defined[in.result])
                return fail("op %u redefines %%%u; values are single-assignment",
                            i, in.result);
            defined[in.result] = 1;
            def_type[in.result] = in.type;
        }
    }

    return verify_flag_fault_invariant(block, fail);
}

// =============================================================================
// Printer
// =============================================================================

namespace {

void print_operand(std::string& out, const IrBlock& block, const Inst& in,
                   u32 k) {
    const Operand& o = in.operands[k];
    switch (o.kind) {
        case OperandKind::None:
            out += "_";
            break;
        case OperandKind::Value:
            append_format(out, "%%%u", o.as_value());
            break;
        case OperandKind::GuestReg:
            out += gpr_name(o.as_gpr(), block.is_64bit);
            break;
        case OperandKind::Imm: {
            // Shift counts, LEA scales and LEA displacements read as numbers,
            // not as bit patterns; everything else prints as hex because the
            // interesting constants in x86 code are masks and addresses.
            const bool decimal =
                (in.op == Op::Shl || in.op == Op::Shr || in.op == Op::Sar) ||
                (in.op == Op::Lea && k == 2);
            const bool signed_dec = (in.op == Op::Lea && k == 3);
            if (signed_dec) {
                i64 v = static_cast<i64>(o.payload);
                const u32 w = type_bits(o.type);
                if (w > 0 && w < 64) {
                    v = static_cast<i64>(o.payload << (64 - w)) >>
                        static_cast<i64>(64 - w);
                }
                append_format(out, "%lld", static_cast<long long>(v));
            } else if (decimal) {
                append_format(out, "%llu",
                              static_cast<unsigned long long>(o.payload));
            } else {
                append_format(out, "0x%llx",
                              static_cast<unsigned long long>(o.payload));
            }
            break;
        }
    }
}

}  // namespace

std::string print_inst(const IrBlock& block, InstId id) {
    std::string out;
    if (id >= block.insts.size()) return out;
    const Inst& in = block.insts[id];

    if (in.defines_value()) {
        append_format(out, "%%%u:%s = ", in.result, type_name(in.type));
    }
    out += op_name(in.op);
    for (u32 k = 0; k < in.operand_count; ++k) {
        out += k == 0 ? " " : ", ";
        print_operand(out, block, in, k);
    }

    append_format(out, "  ; rip=0x%llx",
                  static_cast<unsigned long long>(in.guest_rip));
    if (in.may_fault) {
        append_format(out, " may_fault=%s", fault_state_name(in.fault_state));
    }
    if (in.op == Op::Jcc) {
        append_format(out, " cc=0x%x target=0x%llx fallthrough=0x%llx",
                      in.branch.cond,
                      static_cast<unsigned long long>(in.branch.target),
                      static_cast<unsigned long long>(in.branch.fallthrough));
    }
    if (in.op == Op::Jmp) {
        append_format(out, " target=0x%llx",
                      static_cast<unsigned long long>(in.branch.target));
    }
    if (is_terminator(in.op)) {
        if (in.branch.return_site != kInvalidRip) {
            append_format(out, " call.ret=0x%llx",
                          static_cast<unsigned long long>(
                              in.branch.return_site));
        }
        if (in.branch.is_return) out += " ret";
    }
    if (in.op == Op::Load || in.op == Op::Store) {
        append_format(out, " %s:%s", mem_class_name(in.mem.klass),
                      type_name(in.mem.width));
    }
    if (in.op == Op::DefFlags && in.operand_count == 3) {
        out += " merge";
    }
    if (in.op == Op::DefFlags) {
        const FlagDef& f = in.flags;
        append_format(out, " repr=%s", flag_repr_name(f.repr));
        if (f.nzcv != NzcvKind::None) {
            append_format(out, " nzcv=%s", nzcv_kind_name(f.nzcv));
        }
        if (f.record != FlagState::FlagOp::None) {
            if (const char* name = flag_op_name(f.record)) {
                append_format(out, " record=%s", name);
            } else {
                append_format(out, " record=#%u", static_cast<u32>(f.record));
            }
        }
        if (f.overlay_live) out += " overlay";
        if (f.keep_overlay) out += " keep_overlay";
    }
    return out;
}

std::string print(const IrBlock& block) {
    std::string out;
    // Blocks are tens of ops of ~48 characters; one reserve keeps the printer
    // from growing the string a dozen times in a debug dump.
    out.reserve(block.insts.size() * 64 + 64);
    append_format(out, "block entry=0x%llx bits=%u\n",
                  static_cast<unsigned long long>(block.entry_rip),
                  block.is_64bit ? 64u : 32u);
    for (usize i = 0; i < block.insts.size(); ++i) {
        out += "  ";
        out += print_inst(block, static_cast<InstId>(i));
        out += "\n";
    }
    return out;
}

}  // namespace vitreo::cpu::jit::ir
