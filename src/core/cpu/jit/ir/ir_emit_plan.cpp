// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_emit_plan.cpp
/// @brief `JitCompiler::ir_plan_snapshot_block`: where every value of a
///        snapshot-path block lives. Overview and shared helpers are in
///        ir_emit_internal.hpp.

#include "ir_emit_internal.hpp"

namespace vitreo::cpu::jit {

/// Decide, once, where every value in a snapshot-path block lives.
///
/// Three homes, cheapest first. A read of a guest register the block never
/// writes stays in its architectural pin: the pin register IS the operand, and
/// `emit_helper_blr` reloads the caller-saved pins, so it survives every
/// access and needs neither a pool register nor a slot. A constant is
/// recomputed in one or two MOVs. Everything else takes a pool register for
/// its whole live range, and only a value used in a later SEGMENT than the one
/// that produced it also needs invocation-local spill backing.
///
/// Segments are where a pool register stops being trustworthy: a RIP
/// publication, either side of a CF-merge helper, and after a Load or Store,
/// all of which may destroy X0-X4.
///
/// Missing the pin home is what made cross-instruction read reuse a 3.5x
/// code-size regression on pure long-mode scalar blocks -- the reused reads
/// stayed live for the whole block, overflowed the three-register pool, and
/// the all-or-nothing fallback then put every value on a slot.
bool JitCompiler::ir_plan_snapshot_block(const ir::IrBlock& block) const noexcept {
    const usize op_count = block.insts.size();
    IrSnapshotPlan& plan = m_ir_plan;
    plan.pin_home.assign(block.next_value, 0xFF);
    plan.remat_kind.assign(block.next_value, 0);
    plan.remat_reg.assign(block.next_value, 0);
    plan.remat_wide.assign(block.next_value, 0);
    plan.remat_bits.assign(block.next_value, 0);
    plan.assignment.assign(block.next_value, 0xFF);
    plan.needs_backing.assign(block.next_value, 0);
    plan.def_at.assign(block.next_value, 0);
    plan.zeroes.assign(block.next_value, 0);
    plan.seg_in.assign(op_count, 0);
    plan.seg_out.assign(op_count, 0);
    plan.spill_slot.clear();

    // A guest-register read may use its architectural pin (or CpuState slot)
    // as its home while that register is not written before the value's last
    // use. `next_write[i * 16 + r]` is the first op at or after `i` that
    // writes register r: a GprWrite naming it, or a Divide, whose helper
    // writes the accumulator pair and is treated as writing every register.
    // Intel SDM Vol. 1 3.4.1.1: a 32-bit write changes the 64-bit register
    // too, so one entry per architectural register is correct -- and a narrow
    // write names its register through `architectural_gpr`, because
    // `mov ah, 5` must invalidate RAX and not RSP.
    //
    // This used to be "the block never writes the register at all", which
    // copied EVERY read of a register the block later writes into a scratch
    // first -- `lea rax,[rcx+rdx*8]; mov rcx,[rax]` paid two MOVs before
    // the LEA could even start.
    constexpr u32 kNoWrite = 0xFFFFFFFFu;
    std::vector<u32> next_write((op_count + 1) * 16, kNoWrite);
    for (usize i = op_count; i-- > 0;) {
        for (u32 r = 0; r < 16; ++r)
            next_write[i * 16 + r] = next_write[(i + 1) * 16 + r];
        const auto& in = block.insts[i];
        if (in.op == ir::Op::GprWrite) {
            next_write[i * 16 + ir::architectural_gpr(in.operands[0].type,
                                                      in.operands[0].reg_index)] =
                static_cast<u32>(i);
        } else if (in.op == ir::Op::Divide) {
            for (u32 r = 0; r < 16; ++r) next_write[i * 16 + r] = static_cast<u32>(i);
        }
    }
    std::vector<u32> last_use(block.next_value, 0);
    std::vector<u32> def_index(block.next_value, 0xFFFFFFFFu);
    for (usize i = 0; i < op_count; ++i) {
        const auto& in = block.insts[i];
        if (in.defines_value() && in.result < def_index.size())
            def_index[in.result] = static_cast<u32>(i);
        for (u32 k = 0; k < in.operand_count; ++k) {
            if (in.operands[k].kind == ir::OperandKind::Value)
                last_use[in.operands[k].as_value()] = static_cast<u32>(i);
        }
    }
    for (usize def_index = 0; def_index < op_count; ++def_index) {
        const auto& in = block.insts[def_index];
        if (!in.defines_value() || in.type == ir::Type::Flags) continue;
        const bool value_wide = in.type == ir::Type::I64;
        if (in.op == ir::Op::Const) {
            plan.remat_kind[in.result] = 1;
            plan.remat_wide[in.result] = value_wide;
            plan.remat_bits[in.result] = in.operands[0].payload;
        } else if (in.op == ir::Op::GprRead &&
                   // A sub-register read is an EXTRACT, never an alias of the
                   // pin: handing the consumer the whole register is how
                   // `test al, al` ends up testing EAX. Only a full-width read
                   // has an architectural home its consumers can use directly.
                   (in.type == ir::Type::I32 || in.type == ir::Type::I64) &&
                   // The first write after the read must come at or after the
                   // value's last use: a GprWrite AT the last use is that use
                   // (it reads the value before overwriting the register).
                   next_write[(def_index + 1) * 16 + in.operands[0].reg_index] >=
                       last_use[in.result]) {
            const u8 index = in.operands[0].reg_index;
            const u8 pin = block.is_64bit ? pinned_gpr64(index) : pinned_gpr32(index);
            if (pin != 0xFF) {
                plan.pin_home[in.result] = pin;
            } else {
                plan.remat_kind[in.result] = 2;
                plan.remat_wide[in.result] = value_wide;
                plan.remat_reg[in.result] = index;
            }
        }
    }

    // After its full-width commit to a pinned register, a value IS that pin
    // until the register is written again -- and the register-read reuse
    // pass aliases later reads of the register to this very value, which is
    // what made these intervals long. A value whose last use comes before the
    // next write of its register therefore leaves scratch at the commit:
    // later uses read the pin, which survives helpers and accesses (callee-
    // saved, or spilled and reloaded around every call).
    plan.commit_home.assign(block.next_value, 0xFF);
    plan.commit_at.assign(block.next_value, 0xFFFFFFFFu);
    if (block.is_64bit) {
        for (usize w = 0; w < op_count; ++w) {
            const auto& commit = block.insts[w];
            if (commit.op != ir::Op::GprWrite) continue;
            const ir::Type t = commit.operands[0].type;
            if (t != ir::Type::I32 && t != ir::Type::I64) continue;
            if (commit.operands[1].kind != ir::OperandKind::Value) continue;
            const ir::ValueId v = commit.operands[1].as_value();
            if (plan.pin_home[v] != 0xFF || plan.remat_kind[v] != 0) continue;
            if (plan.commit_home[v] != 0xFF) continue;   // first commit only
            if (def_index[v] == 0xFFFFFFFFu) continue;
            if (block.insts[def_index[v]].type != t) continue;
            const u8 reg = commit.operands[0].reg_index;
            const u8 pin = pinned_gpr64(reg);
            if (pin == 0xFF) continue;
            if (last_use[v] <= w) continue;              // nothing after it
            if (next_write[(w + 1) * 16 + reg] < last_use[v]) continue;
            plan.commit_home[v] = pin;
            plan.commit_at[v] = static_cast<u32>(w);
        }
    }

    // A result whose last use is its full-width commit, later in the same
    // guest instruction, is computed straight into that register's pin:
    // `add rsp, 8` is one ADD, not an ADD into a scratch and a MOV. Sound when
    //  - nothing between the definition and the commit can fault (the early
    //    write would be observed by an exception before the instruction
    //    retires) or writes the register, and
    //  - every read of the register homed in its pin is dead by the
    //    definition (a flag record of the OLD value must not see the new one).
    // A load also needs its address elsewhere than the pin (its cold path
    // reuses the address).
    plan.commit_pin.assign(block.next_value, 0xFF);
    plan.commit_undo.assign(block.next_value, 0);
    // `push`: RSP - k committed before the store, as the stencil path does.
    // Sound because the descriptor makes every fault inside the instruction
    // add k back (JitFlagRecovery::rsp_undo, a 6-bit field), and the store is
    // the instruction's ONLY fallible op, so no fault can see the move from
    // anywhere the undo does not cover. Returns k, or 0.
    // Ops [0, rising_until) have non-decreasing guest RIPs: within that
    // prefix an instruction's index is bounded by its byte offset.
    usize rising_until = op_count;
    for (usize i = 1; i < op_count; ++i) {
        if (block.insts[i].guest_rip < block.insts[i - 1].guest_rip) {
            rising_until = i;
            break;
        }
    }
    const auto rsp_undo_for = [&](usize d) -> i8 {
        if (d >= rising_until) return 0;
        const auto& def = block.insts[d];
        if (def.op != ir::Op::Lea || def.type != ir::Type::I64 ||
            def.operand_count != 4 ||
            def.operands[0].kind != ir::OperandKind::Value ||
            def.operands[1].kind != ir::OperandKind::None) return 0;
        const i64 disp = static_cast<i64>(def.operands[3].payload);
        if (disp >= 0 || disp < -31) return 0;
        const u32 base = def_index[def.operands[0].as_value()];
        if (base == 0xFFFFFFFFu) return 0;
        const auto& read = block.insts[base];
        if (read.op != ir::Op::GprRead || read.type != ir::Type::I64 ||
            read.operands[0].reg_index != 4) return 0;   // RSP
        // The undo lives on the instruction's fault site, and only the first
        // kMaxInsnArmPos instructions have one. Every instruction is at least
        // a byte, so its index is at most its offset in the block.
        if (def.guest_rip < block.entry_rip ||
            def.guest_rip - block.entry_rip >= kMaxInsnArmPos) return 0;
        for (usize j = d; j-- > 0 && block.insts[j].guest_rip == def.guest_rip;) {
            if (block.insts[j].may_fault) return 0;
        }
        return static_cast<i8>(-disp);
    };
    // Every read of `pin`'s old contents is dead by d, so a value computed
    // into it at d is not seen by a reader that wants the register as it was.
    // `self` and `also` are the values that will live there.
    const auto pin_reads_dead = [&](usize d, u32 w, u8 pin, ir::ValueId self,
                                    ir::ValueId also) {
        for (ir::ValueId u = 0; u < block.next_value; ++u) {
            // A value defined after the commit reads the register's NEW
            // contents -- this one -- which is exactly what the pin holds.
            if (def_index[u] != 0xFFFFFFFFu && def_index[u] > w) continue;
            if (u == self || u == also) continue;
            if (plan.pin_home[u] == pin && last_use[u] > d) return false;
            if (plan.commit_pin[u] == pin && last_use[u] > d) return false;
            // Lives in this pin after its own commit and is still used.
            if (plan.commit_home[u] == pin && last_use[u] > d &&
                plan.commit_at[u] < w) return false;
        }
        return true;
    };
    if (block.is_64bit) {
        for (usize d = 0; d < op_count; ++d) {
            const auto& def = block.insts[d];
            if (!def.defines_value()) continue;
            if (def.type != ir::Type::I32 && def.type != ir::Type::I64) continue;
            switch (def.op) {
            case ir::Op::Add: case ir::Op::Sub: case ir::Op::And:
            case ir::Op::Or: case ir::Op::Xor: case ir::Op::Not:
            case ir::Op::Neg: case ir::Op::Shl: case ir::Op::Shr:
            case ir::Op::Sar: case ir::Op::Rol: case ir::Op::Ror:
            case ir::Op::Lea: case ir::Op::Mul: case ir::Op::Zext:
            case ir::Op::Sext: case ir::Op::Load:
            case ir::Op::AddCarry: case ir::Op::SubBorrow:
                break;
            default: continue;
            }
            if (plan.pin_home[def.result] != 0xFF || plan.remat_kind[def.result] != 0)
                continue;
            // The commit is the value's last use -- or its first commit, when
            // the value then stays in that pin for every later use
            // (commit_home): `add rsp, 8` whose new RSP the next `pop`
            // addresses through is one ADD into X12, as on the stencil path,
            // not an ADD into a scratch and a MOV.
            const bool homed = plan.commit_home[def.result] != 0xFF;
            const u32 w = homed ? plan.commit_at[def.result] : last_use[def.result];
            if (w <= d) continue;
            const auto& commit = block.insts[w];
            if (commit.op != ir::Op::GprWrite || commit.guest_rip != def.guest_rip ||
                commit.operands[1].as_value() != def.result ||
                commit.operands[0].type != def.type) continue;
            const u8 reg = commit.operands[0].reg_index;
            const u8 pin = pinned_gpr64(reg);
            if (pin == 0xFF) continue;
            if (homed && plan.commit_home[def.result] != pin) continue;
            bool safe = true;
            const i8 undo = reg == 4 ? rsp_undo_for(d) : i8{0};
            bool undo_used = false;
            for (usize j = d + 1; j < w && safe; ++j) {
                const auto& mid = block.insts[j];
                if (mid.may_fault) {
                    if (undo != 0 && !undo_used && mid.op == ir::Op::Store &&
                        mid.mem.width != ir::Type::V128) {
                        undo_used = true;
                    } else {
                        safe = false;
                    }
                }
                if (mid.op == ir::Op::GprWrite &&
                    ir::architectural_gpr(mid.operands[0].type,
                                          mid.operands[0].reg_index) == reg) safe = false;
                if (mid.op == ir::Op::Divide) safe = false;
            }
            if (!safe) continue;
            if (!pin_reads_dead(d, w, pin, def.result, def.result)) continue;
            if (def.op == ir::Op::Load &&
                plan.pin_home[def.operands[0].as_value()] == pin) continue;
            // Nothing after the commit in the same instruction may fault
            // either: the undo is per instruction.
            if (undo_used) {
                for (usize j = w + 1; j < op_count &&
                     block.insts[j].guest_rip == def.guest_rip && safe; ++j) {
                    if (block.insts[j].may_fault) safe = false;
                }
                if (!safe) continue;
                plan.commit_undo[def.result] = undo;
            }
            plan.commit_pin[def.result] = pin;
        }
        // A Zext of a narrow value that is already zero-extended (a Trunc's
        // UXTB, an extracting GprRead, a narrow load) emits only a move, so
        // computing that source straight into the Zext's pin leaves the Zext
        // nothing to do: `movzx ecx, cl` is one UXTB W24 and
        // `movzx ecx, byte [rdx]` one LDRB W24, not an op into a scratch and
        // a MOV on the dependency chain. Same rules as above, from the
        // source's definition: no fault after it (the load's own fault
        // commits nothing), no other write of the register, and no reader of
        // the pin's old contents still live.
        for (usize z = 0; z < op_count; ++z) {
            const auto& zext = block.insts[z];
            if (zext.op != ir::Op::Zext || !zext.defines_value()) continue;
            const u8 pin = plan.commit_pin[zext.result];
            if (pin == 0xFF || plan.commit_undo[zext.result] != 0) continue;
            const auto& operand = zext.operands[0];
            if (operand.kind != ir::OperandKind::Value ||
                (operand.type != ir::Type::I8 && operand.type != ir::Type::I16)) continue;
            const ir::ValueId s = operand.as_value();
            const u32 d = def_index[s];
            if (d == 0xFFFFFFFFu || d >= z) continue;
            const auto& src = block.insts[d];
            if (src.type != operand.type ||
                (src.op != ir::Op::Trunc && src.op != ir::Op::GprRead &&
                 src.op != ir::Op::Load)) continue;
            if (src.guest_rip != zext.guest_rip || last_use[s] != z) continue;
            if (plan.pin_home[s] != 0xFF || plan.remat_kind[s] != 0 ||
                plan.commit_home[s] != 0xFF || plan.commit_pin[s] != 0xFF) continue;
            const bool homed = plan.commit_home[zext.result] != 0xFF;
            const u32 w = homed ? plan.commit_at[zext.result] : last_use[zext.result];
            const u8 reg = block.insts[w].operands[0].reg_index;
            bool safe = true;
            for (usize j = d + 1; j < w && safe; ++j) {
                const auto& mid = block.insts[j];
                if (mid.may_fault || mid.op == ir::Op::Divide) safe = false;
                // A read emitted here would see the new value in the pin.
                if ((mid.op == ir::Op::GprWrite || mid.op == ir::Op::GprRead) &&
                    ir::architectural_gpr(mid.operands[0].type,
                                          mid.operands[0].reg_index) == reg) safe = false;
            }
            if (!safe || !pin_reads_dead(d, w, pin, s, zext.result)) continue;
            if (src.op == ir::Op::Load &&
                plan.pin_home[src.operands[0].as_value()] == pin) continue;
            plan.commit_pin[s] = pin;
        }
    }

    // The CF capture a carry-preserving record needs (INC/DEC) runs at the
    // START of its guest instruction, before any of that instruction's
    // values exist: it reads only the incoming record, which nothing in the
    // instruction changes before the definition, and it clobbers X0-X4 and
    // X21. Captured at the definition instead, the instruction's own result
    // crossed it and took a spill slot -- a store and two reloads for every
    // `inc rax`.
    plan.insn_has_merge.assign(op_count, 0);
    for (usize i = 0; i < op_count;) {
        usize end = i;
        bool merge = false;
        while (end < op_count && block.insts[end].guest_rip == block.insts[i].guest_rip) {
            merge |= ir_is_cf_merge(block.insts[end]);
            ++end;
        }
        for (usize j = i; j < end; ++j) plan.insn_has_merge[j] = merge;
        i = end;
    }
    std::vector<u8> pstate_flags(block.next_value, 0);
    for (const auto& d : block.insts) {
        if (d.op == ir::Op::DefFlags && d.flags.repr == ir::FlagRepr::InPstate)
            pstate_flags[d.result] = 1;
    }
    u32 seg = 0;
    for (usize i = 0; i < op_count; ++i) {
        const auto& in = block.insts[i];
        if (i == 0 || in.guest_rip != block.insts[i - 1].guest_rip) {
            for (usize scan = i; scan < op_count &&
                 block.insts[scan].guest_rip == in.guest_rip; ++scan) {
                if (block.insts[scan].may_fault) { ++seg; break; }
            }
            if (plan.insn_has_merge[i]) ++seg;  // the hoisted CF capture
        }
        plan.seg_in[i] = seg;
        // `EvalCc` may BLR the condition helper, so it ends a segment exactly
        // as an access does.
        // A CondExit evaluates its condition like EvalCc, which may BLR.
        // The merge itself writes only X3 (emit_lazy_flag_store_raw's
        // clobber contract), so with its capture hoisted it ends nothing.
        // A condition of a PSTATE-only definition is always taken off the
        // claim (keep_carry_in_pstate), never through the helper.
        const bool pstate_cc = in.op == ir::Op::EvalCc &&
            in.operands[0].kind == ir::OperandKind::Value &&
            pstate_flags[in.operands[0].as_value()];
        if (in.op == ir::Op::Load || in.op == ir::Op::Store ||
            (in.op == ir::Op::EvalCc && !pstate_cc) || in.op == ir::Op::CondExit ||
            in.op == ir::Op::FlagsImage ||
            in.op == ir::Op::FlagsInstall || in.op == ir::Op::Divide) ++seg;
        plan.seg_out[i] = seg;
        if (in.defines_value() && in.type != ir::Type::Flags)
            plan.def_at[in.result] = static_cast<u32>(i);
    }

    // A computed value used in a later segment than its definition -- across
    // a guest memory access, typically -- would take a spill slot: a store
    // after the definition and a frame-base load plus a reload at every later
    // use. `push rbx` paid four memory operations for its new RSP that way,
    // where the stencil path parks it in X21. X21 (`kIrSurvivor`) is
    // callee-saved, so helper calls preserve it, and the scalar cold memory
    // stubs never touch it. The snapshot lowerings that DO write it are the
    // CF-merge capture (`emit_lazy_cf_to_call_tmp`) and the condition
    // evaluators, so a survivor's interval may contain only ops whose
    // lowering provably leaves X21 alone (`survivor_safe_op`).
    //
    // One register, so one survivor at a time, chosen greedily in definition
    // order with strictly disjoint intervals: the next survivor's definition
    // writes X21 only after the previous one's last register use has read it.
    plan.survivor.assign(block.next_value, 0);
    if (block.is_64bit) {
        const auto survivor_safe_op = [](const ir::Inst& in) {
            switch (in.op) {
            case ir::Op::Const: case ir::Op::GprRead: case ir::Op::GprWrite:
            case ir::Op::Add: case ir::Op::Sub: case ir::Op::And:
            case ir::Op::Or: case ir::Op::Xor: case ir::Op::Not: case ir::Op::Neg:
            case ir::Op::Shl: case ir::Op::Shr: case ir::Op::Sar:
            case ir::Op::Rol: case ir::Op::Ror: case ir::Op::Mul: case ir::Op::SMulH:
            case ir::Op::Trunc: case ir::Op::Zext: case ir::Op::Sext: case ir::Op::Lea:
            case ir::Op::SetCf: case ir::Op::SetOf:
            case ir::Op::AddCarry: case ir::Op::SubBorrow:
                return true;
            case ir::Op::Load: case ir::Op::Store:
                // The 256-bit cold stub uses X21; scalar and 128-bit ones do
                // not, but only scalar accesses are admitted here.
                return in.mem.width != ir::Type::V128;
            case ir::Op::DefFlags:
                // A by-CL record and a CF merge are three-operand; the merge
                // captures CF into X21.
                return in.operand_count == 2 || in.flags.repr == ir::FlagRepr::Dead;
            default:
                return false;
            }
        };
        std::vector<u8> non_flag_use(block.next_value, 0);
        for (const auto& u : block.insts) {
            if (u.op == ir::Op::DefFlags) continue;
            for (u32 k = 0; k < u.operand_count; ++k) {
                if (u.operands[k].kind == ir::OperandKind::Value)
                    non_flag_use[u.operands[k].as_value()] = 1;
            }
        }
        u32 busy_until = 0;
        bool busy = false;
        for (usize d = 0; d < op_count; ++d) {
            const auto& def = block.insts[d];
            if (!def.defines_value()) continue;
            if (def.type != ir::Type::I8 && def.type != ir::Type::I16 &&
                def.type != ir::Type::I32 && def.type != ir::Type::I64) continue;
            const ir::ValueId v = def.result;
            if (plan.pin_home[v] != 0xFF || plan.remat_kind[v] != 0 ||
                plan.commit_pin[v] != 0xFF) continue;
            if (!survivor_safe_op(def) || def.op == ir::Op::GprWrite ||
                def.op == ir::Op::Store || def.op == ir::Op::DefFlags) continue;
            // A result only flag records read may be the `cmp reg, 0` form,
            // whose emitter aliases it to its first operand.
            if (!non_flag_use[v]) continue;
            // Uses after a commit_home commit read the pin.
            const u32 end = plan.commit_home[v] != 0xFF ? plan.commit_at[v]
                                                        : last_use[v];
            if (end <= d) continue;
            if (plan.seg_in[end] == plan.seg_out[d]) continue;  // no crossing
            if (busy && d <= busy_until) continue;
            // An instruction with a CF merge writes X21 at its start and
            // reads it at the merge.
            bool safe = !plan.insn_has_merge[d];
            for (usize j = d + 1; j <= end && safe; ++j)
                safe = survivor_safe_op(block.insts[j]) && !plan.insn_has_merge[j];
            if (!safe) continue;
            plan.survivor[v] = 1;
            busy = true;
            busy_until = end;
        }
    }

    // Pressure spills instead of declining the whole block: rematerializable
    // values are evicted first (they reload at use, kIrRematAt), then the
    // furthest-used computed values (backed by a slot, reloaded at use).
    plan.pooled = plan_pool_registers(block, 0, op_count, plan.pin_home,
                                      plan.survivor, &plan.assignment, 0,
                                      &plan.commit_at, /*allow_spill=*/true,
                                      &plan.remat_kind);

    bool any_backing = false;
    for (usize i = 0; i < op_count; ++i) {
        const auto& in = block.insts[i];
        for (u32 k = 0; k < in.operand_count; ++k) {
            const auto& operand = in.operands[k];
            if (operand.kind != ir::OperandKind::Value ||
                operand.type == ir::Type::Flags ||
                // A vector value lives in its own register pool and never
                // crosses a may-fault op, so it has nothing to back: the spill
                // frame is GPR-shaped and a `STR X` of a V register would be a
                // silent 8-of-16 bytes.
                operand.type == ir::Type::V128) continue;
            const ir::ValueId value = operand.as_value();
            if (plan.pin_home[value] != 0xFF || plan.remat_kind[value] != 0) continue;
            // Served by the pin it was committed to (commit_home).
            if (plan.commit_home[value] != 0xFF && i > plan.commit_at[value]) continue;
            // Computed into its pin, which nothing else writes before the
            // commit and which survives helpers (a load may have had to
            // use a different destination).
            if (plan.commit_pin[value] != 0xFF &&
                block.insts[plan.def_at[value]].op != ir::Op::Load) continue;
            if (plan.survivor[value]) continue;   // X21 holds it throughout
            if (!plan.pooled || plan.assignment[value] == 0xFF ||
                plan.seg_in[i] != plan.seg_out[plan.def_at[value]]) {
                plan.needs_backing[value] = 1;
                any_backing = true;
            }
        }
    }
    // A block whose values all stay in pins or pool registers never addresses
    // the frame, so it must not pay to plan one.
    if (!any_backing) return true;
    plan.spill_slot.assign(block.next_value, 0xFF);
    return ir::plan_spill_slots(block, &plan.spill_slot);
}

}  // namespace vitreo::cpu::jit
