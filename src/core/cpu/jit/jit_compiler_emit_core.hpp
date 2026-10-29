// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_emit_core.hpp
/// @brief Core emission helpers: word emit, NZCV hold, Jcc condition evaluation, 32-bit GPR access, helper BLRs, cold-arm deferral.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_emit_core.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    // ---- Emission helpers ----

    /// Emit a single ARM64 instruction to the code buffer
    void emit(u32 insn);

    [[nodiscard]] bool should_hold_nzcv() const noexcept;
    void emit_nzcv_spill();
    void emit_nzcv_fill();
    void emit_nzcv_spill_if_kind();
    void emit_nzcv_fill_if_kind();
    void emit_store_nzcv_kind(NzcvState state);
    [[nodiscard]] static u8 nzcv_kind_byte(NzcvState state) noexcept;
    [[nodiscard]] bool jcc_fuses_from_carried_nzcv(u8 x86_cc) const noexcept;
    /// True when a non-fused Jcc may probe runtime nzcv_kind for a carried
    /// Sub/And/Add NZCV. False for Sub8 writers (unsigned-only live NZCV).
    [[nodiscard]] bool can_try_carried_jcc(u8 x86_cc) const noexcept;
    /// Evaluate Jcc into W0. Returns the NZCV kind to advertise on linked
    /// exits (Carried = leave cpu.nzcv_kind, NZCV still live on the fast path).
    NzcvState emit_eval_jcc(u8 cond, int fused_arm);
    /// BLR cond_fns[cond]. Fallback for overlays, parity, and uncommon FlagOps.
    void emit_cond_helper(u8 cond);
    /// W0 = x86 condition from flags.rflags (FlagOp::None, no overlays).
    void emit_eval_from_rflags(u8 cond);
    /// CSET W0 from reconstructed NZCV for Sub/And/Add, including the
    /// unsigned TEST/ADD cases that have no single ARM condition.
    void emit_cset_for_lazy_kind(NzcvState kind, u8 cond);
    /// Evaluate cond into W0 from FlagOp / rflags without a C++ helper when
    /// the recorded op is Add/Sub/And/Or/Xor 32/64. Overlays and other ops
    /// still call emit_cond_helper.
    void emit_eval_lazy_condition(u8 cond);

    /// Emit: load 32-bit value from CpuState GPR into ARM64 Wt
    void emit_load_gpr32(u8 arm_reg, u8 x86_reg);

    /// Emit: store 32-bit value from ARM64 Wt into CpuState GPR
    void emit_store_gpr32(u8 arm_reg, u8 x86_reg);

    /// Merge `width` bits of `value_reg` into guest register `x86_reg` at bit
    /// `lsb`, leaving the rest of the register intact — x86's partial-register
    /// write rule. Carries the same invalidation as `emit_store_gpr32`.
    void emit_merge_gpr_narrow(u8 value_reg, u8 x86_reg, u8 lsb, u8 width);

    /// BLR X16. Flushes pins, then reloads pins and the linear base.
    void emit_helper_blr();

    /// `emit_helper_blr()` for a call that computes the instruction's
    /// ARCHITECTURAL RESULT in C++ (jit_sse_op, jit_fpu_op, jit_round_op,
    /// jit_sse41_op, jit_pcmpxstrx, jit_fxsave_fxrstor). Identical code; it
    /// additionally marks the instruction so coverage buckets it as `Helper`
    /// rather than `Native`. Use the plain form for memory-access thunks,
    /// fault raises and flag materialisation — those are part of a native
    /// lowering.
    void emit_semantic_helper_blr();

    /// Emit the x87 side effect every MMX instruction (except EMMS) has:
    /// tag word := all-Valid, TOP := 0 (Intel SDM Vol. 2). Architecturally
    /// visible through FNSTENV/FXSAVE, and required even for MMX instructions
    /// that only READ an MM register.
    void emit_mmx_enter();

    /// Mid-block forward Jcc: fused `B.cond` to a cold taken stub, or
    /// lazy-flag / rflags eval + `CBNZ W0`. Fall-through continues in the
    /// current translation. Stubs are flushed after the block body.
    void queue_mid_block_jcc_exit(int fused_arm, u8 x86_cc, u64 target);
    [[nodiscard]] bool is_forward_in_block_target(
        GuestAddr target, GuestAddr next_rip) const noexcept;
    void queue_forward_in_block_jcc(u8 arm_cond, GuestAddr target);
    void emit_flush_cold_jcc_exits();

    /// Defer `body`, the slow arm of the instruction being compiled, to the
    /// block's cold region. Each entry holds a hot-path placeholder for a
    /// branch to the arm, encoded once the arm's displacement is known; the
    /// arm then branches back to the current position, where the hot path
    /// continues. `body` must capture by value everything it needs beyond
    /// DeferredColdArm's restored state.
    ///
    /// The arm's helper call is not emitted at the site, so this records what
    /// it would have done to the instruction's compile state there: a helper
    /// BLR ran on one path (and, if `semantic_helper`, a semantic one), and
    /// the ESP frame is not live at the join. Called while the cold region
    /// itself is being emitted, it lays the arm down inline instead, behind a
    /// B, exactly as the caller would have without it.
    void defer_cold_arm(std::vector<ColdArmEntry> entries,
                        std::function<void()> body, bool semantic_helper);
    /// Emit every deferred arm; part of emit_flush_cold_jcc_exits(), ahead of
    /// the stubs whose branches an arm's helper call or EA may add.
    void emit_flush_deferred_cold_arms();
    /// The mid-block Jcc taken exits alone; emit_flush_cold_jcc_exits() ends
    /// with them. A block laid out for split placement emits them first, in
    /// its hot part.
    void emit_flush_mid_jcc_exits();

    /// BLR X16 for a *pure memory* helper (jit_mem_read*/jit_mem_write*).
    ///
    /// Six of the eight pins live in X23-X28, which AAPCS64 makes
    /// callee-saved, so a C helper is already required to preserve them; only
    /// RSP->X12 and RBP->X13 are in the caller-saved temporary class. The
    /// memory helpers read CpuState::gpr nowhere that binds (mem_watch
    /// publishing and the [jit-av] dump are diagnostics, and the dump is now
    /// deferred to after the epilogue), and none of them re-enter guest code.
    ///
    /// NOT a general replacement for emit_helper_blr(). The interpreter
    /// delegate (instruction_compilation.cpp) *is* the guest register file for
    /// the instruction it runs: it reads and writes CpuState::gpr directly, so
    /// its pins must be fully spilled and reloaded. Same for anything that can
    /// re-enter guest code (thunk gateways, run_guest_callback). When unsure,
    /// use emit_helper_blr().
    void emit_memory_helper_blr();

    /// Emit the out-of-lined memory slow paths and the one shared
    /// `pending_raise` exit stub, then patch every branch that targets them.
    /// Called from emit_flush_cold_jcc_exits() before the Jcc stubs, because
    /// a memory slow path emits its own raise check.
    void emit_flush_cold_mem_stubs();

