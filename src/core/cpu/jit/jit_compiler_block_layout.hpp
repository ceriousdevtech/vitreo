// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_block_layout.hpp
/// @brief Per-block layout: shared exit stubs, deferred cold arms, per-instruction position tables, Tier 1 IR scratch.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_block_layout.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    // Shared per-block "continue" exit stubs for linked-tail fallbacks.
    //
    // Every block tail (Jcc taken leg, Jcc fall-through leg, JMP, CALL, RET,
    // every cold Jcc stub) ended with a private 16-word copy of
    //     persist NZCV ; store 8 pins ; LDR X30,[SP],#16 ; MOV W0,#0 ; RET
    // A fused terminal Jcc alone carried two of them, ~32 of its 77 emitted
    // words. The body depends on nothing from the call site -- RIP and the
    // iteration counter are already in CpuState when control reaches it, and
    // NZCV comes either from live PSTATE or from the block's fixed [SP,#8]
    // spill slot -- so one copy per (persist mode) serves the whole block and
    // each site pays a single B.
    //
    // Index 0 = live-PSTATE persist, index 1 = [SP,#8]-spilled persist.
    static constexpr u32 kContinueExitVariants = 2;
    std::array<std::vector<usize>, kContinueExitVariants> m_continue_exit_pos;
    /// Where each variant's body was emitted; npos until a flush emits it.
    /// A split-placed block flushes once before its cold region, so the
    /// bodies its hot exits run sit in the hot part, and once after; sites
    /// the cold region adds branch back to a body already emitted.
    std::array<usize, kContinueExitVariants> m_continue_exit_body{};

    // L3: the block's shared in-chain API-call exit
    // (emit_shared_api_call_exit). Sites branch to its hot part (the slot
    // test), which branches to its cold part (the call) only for an
    // in-chain export. npos until a flush emits the hot part.
    std::vector<usize> m_api_exit_sites;
    usize m_api_exit_hot{static_cast<usize>(-1)};
    usize m_api_exit_to_cold{0};   // the hot part's B .call, patched later
    usize m_api_exit_plain{0};     // the hot part's .plain (continue exit)
    bool m_api_exit_cold_done{false};
    /// Emit the hot part (if any site needs it) and patch the sites. Runs
    /// before every continue-exit flush, in the hot part of a split block.
    void emit_flush_api_call_exit_stubs();
    /// Emit the cold part after the hot one, in the cold region of a split
    /// block (after m_hot_end_pos), and patch the hot part's B into it.
    void emit_flush_api_call_exit_cold();

    // Shared per-block linked-tail (L1 probe) stub. The probe body is fixed
    // apart from two things: the guest target, which emit_update_rip has
    // already left in REG_SCRATCH0 at every call site, and the (carry,
    // backward-edge) pair, which selects the NZCV-kind store and whether the
    // hop budget is charged. One stub per distinct pair therefore serves
    // every tail in the block, and a tail costs a single B instead of the
    // ~15-word probe plus the ~16-word epilogue behind it.
    // Set by emit_update_rip: the value it left in REG_SCRATCH0 and the
    // buffer index immediately after it. emit_linked_tail_continue reuses
    // that register only when BOTH match exactly -- same value, and nothing
    // emitted since -- so a caller that does not update RIP first, or that
    // emits anything in between, provably cannot inherit a stale X0.
    u64   m_rip_reg_value{0};
    usize m_rip_reg_pos{static_cast<usize>(-1)};
    // Where that materialization began: a tail with a direct-link slot moves
    // it behind the slot, onto the fallthrough that is its only reader.
    usize m_rip_reg_start{static_cast<usize>(-1)};

    struct ContTailStub {
        NzcvState carry{NzcvState::None};
        bool backward{false};
        std::vector<usize> sites;
        /// Where the stub body was emitted; npos until the first flush that
        /// sees a site for it. Later sites branch back to it.
        usize emitted_pos{static_cast<usize>(-1)};
    };
    std::vector<ContTailStub> m_cont_tail_stubs;

    /// A lowering's rarely taken slow arm, emitted in the block's cold region
    /// rather than inline behind a `B` over it (defer_cold_arm()). Inline, the
    /// arm's words sit in the hot path's cache lines and the fast path pays a
    /// taken branch to skip them.
    struct ColdArmEntry {
        usize pos;                       ///< hot-path placeholder to the arm
        std::function<u32(i32)> encode;  ///< its branch, for a byte displacement
    };
    struct DeferredColdArm {
        std::vector<ColdArmEntry> entries;
        usize resume_pos{0};             ///< where the arm branches back to
        std::function<void()> body;      ///< emits the arm, minus its last B
        // Emitter state at the deferral site that arm bodies read, restored
        // while `body` runs so the arm is exactly the words it would have
        // been inline.
        bool pins_live{false};
        bool linear_base_pinned{false};
        bool current_original_vex{false};
        u32 nzcv_hold_depth{0};
        u32 owner{~0u};  ///< index into m_insn_* of the deferring instruction
        u16 xmm_cache_live{0};  ///< reloaded before the arm rejoins
    };
    std::vector<DeferredColdArm> m_deferred_cold_arms;
    /// Where each deferred arm landed, so a stub its body queued (an align
    /// check) still finds the instruction that owns it.
    /// A lean site's checked variant (ColdMemStub::lean_variant), owned by its
    /// guest instruction: published as a second precise-RIP interval for that
    /// instruction, since a fault inside it is that instruction's fault.
    struct LeanVariantRange {
        usize begin;
        usize end;
        u32 owner;
    };
    std::vector<LeanVariantRange> m_lean_variant_ranges;

    struct ColdArmRange {
        usize begin;
        usize end;
        u32 owner;
    };
    std::vector<ColdArmRange> m_cold_arm_ranges;
    bool m_flushing_cold_arms{false};

    /// Index into m_insn_* of the instruction that emitted the word at `pos`:
    /// the one whose interval holds it, or whose deferred cold arm does.
    [[nodiscard]] bool owning_insn(usize pos, u32& insn) const noexcept;

    // Loop-back tracking for Jcc compilation
    u32 m_body_start_pos{0};         // Code buffer index after prologue
    u32 m_link_entry_pos{1};         // Index of JIT-to-JIT entry (after pins)
    // Index where the block's cold region starts: everything emitted after the
    // hot body and its per-block linked-tail stubs (memory slow paths, align /
    // raise / UD2 stubs, mid-block Jcc taken exits; the shared continue exits
    // too, unless the block is laid out for split placement, which keeps the
    // ones its hot exits use in the hot part). 0 until compile() reaches that
    // point.
    u32 m_hot_end_pos{0};
    u32 m_call_entry_pos{std::numeric_limits<u32>::max()};
                                        // Native guest-CALL landing pad
    GuestAddr m_block_start_addr{0}; // Guest address of current block
    GuestAddr m_diagnostic_instruction_rip{0}; // Inline recorder's exact site
    // Guest RIP → ARM buffer index for each insn compiled in this block.
    // A backward Jcc whose target is one of these (not only block start)
    // can B to that offset instead of exiting to the dispatcher.
    static constexpr u32 kMaxInsnArmPos = 192;
    std::array<u64, kMaxInsnArmPos> m_insn_rips{};
    /// Per guest instruction: how to rebuild a flag record the Tier 1 pass
    /// elided, if a fault lands in that instruction. Inactive
    /// (`flag_op == 0`) for every instruction unless the flag-recovery pass
    /// produced a plan covering it.
    std::array<JitFlagRecovery, kMaxInsnArmPos> m_insn_flag_recovery{};
    /// Per recorded instruction: bytes the fault paths add back to the guest
    /// RSP (JitFlagRecovery::rsp_undo), merged into the published sites.
    std::array<i8, kMaxInsnArmPos> m_insn_rsp_undo{};
    /// Recorded-site index of the instruction being lowered, or ~0u when it
    /// has none (past kMaxInsnArmPos). An undo needs a site to live on.
    u32 m_current_site{~0u};

    /// Set by `ir_emit_block` while lowering a guest instruction whose flag
    /// record was elided across a fault. Any cold store stub emitted for that
    /// instruction copies it and republishes the record before the helper can
    /// raise. Cleared at every instruction boundary.
    StubFlagReplay m_ir_stub_replay{};
    std::array<u32, kMaxInsnArmPos> m_insn_arm_pos{};
    std::array<u32, kMaxInsnArmPos> m_insn_arm_end{};
    u32 m_insn_pos_count{0};
    std::array<GuestAddr, kMaxInsnArmPos> m_block_insn_rips{};
    u32 m_block_insn_rip_count{0};

    /// Tier 1 (ADR-0001) scratch, reused across compiles.
    ///
    /// `IrBlock::clear()` keeps capacity, so the steady state is zero
    /// allocations per block -- which is the reason this is a member and not a
    /// local. `m_ir_rips` is the per-instruction guest address table the IR
    /// frontend requires: `DecodedInstruction` carries no address, so it is
    /// re-derived by accumulating `length` from `block.start_addr`, the same
    /// walk `flag_liveness.cpp:389-393` does.
    ///
    /// This deliberately does NOT reuse `m_block_insn_rips`: that table is
    /// capped at `kMaxInsnArmPos` and stops filling when the cap is hit, so a
    /// long block would hand the frontend a SHORT address array while still
    /// reporting the full instruction count -- an out-of-bounds read whose
    /// symptom would be a wrong `guest_rip` on a fault.
    ir::IrBlock m_ir_block{};
    std::vector<u64> m_ir_rips;
    ir::BuildResult m_ir_result{};

    /// One IR block's snapshot-path allocation plan.
    ///
    /// Seam A runs after the pin ABI is selected and before the first ARM word
    /// is emitted, so the plan `ir_emit_feasible` must build to answer "can
    /// this be lowered" is exactly the plan `ir_emit_block` then needs.
    /// Building it twice cost roughly 3 us per long-mode compile and let the
    /// two copies disagree about which values live where. The containers are
    /// members so a steady-state compile reuses their capacity instead of
    /// constructing ten of them per block.
    struct IrSnapshotPlan {
        /// Which `ir_emit_block` branch feasibility chose, so emission does
        /// not re-derive `requires_snapshot_backing` over the whole block.
        bool snapshot_path{false};
        /// True when every pool-resident value fits the scratch registers.
        bool pooled{false};
        std::vector<u8> pin_home;      ///< ARM pin register holding it, or 0xFF
        std::vector<u8> remat_kind;    ///< 0 none, 1 Const, 2 GprRead
        std::vector<u8> remat_reg;
        std::vector<u8> remat_wide;
        std::vector<u64> remat_bits;
        std::vector<u8> assignment;    ///< pool register, or 0xFF
        std::vector<u8> spill_slot;    ///< slot index, or 0xFF; empty if unused
        std::vector<u8> needs_backing;
        std::vector<u32> seg_in;       ///< per op: segment its operands live in
        std::vector<u32> seg_out;      ///< per op: segment its result lives in
        std::vector<u32> def_at;
        std::vector<u8> zeroes;        ///< `plan_pool_registers` survivor input
        /// The guest pin a value is computed straight into, because its only
        /// remaining use is the commit to that register (ir_plan_snapshot_block
        /// states the conditions), or 0xFF.
        std::vector<u8> commit_pin;
        /// For a `commit_pin` value that is RSP moved down by this many bytes
        /// and committed BEFORE its instruction's one access: the
        /// instruction's `rsp_undo`, which both fault paths add back. 0 when
        /// the commit waits for the access instead.
        std::vector<i8> commit_undo;
        /// The pin a value lives in AFTER its full-width commit, and the op
        /// index of that commit (commit_home / commit_at). Valid only when
        /// the register is not written again before the value's last use, so
        /// every later use reads the pin instead of a pool register or slot.
        std::vector<u8> commit_home;
        std::vector<u32> commit_at;
        /// Parked in `kIrSurvivor` (X21) from its definition to its last
        /// register use, instead of a spill slot (ir_plan_snapshot_block).
        std::vector<u8> survivor;
        /// Per op: its guest instruction holds a live CF merge, whose capture
        /// runs at the instruction's start and lives in X21 until the merge.
        std::vector<u8> insn_has_merge;
        std::vector<u8> loc;           ///< emission: value -> current register
        std::vector<u32> mat_seg;      ///< emission: segment it was last materialized in
    };
    mutable IrSnapshotPlan m_ir_plan;

    /// Build `m_ir_plan` for `block`. False means no allocation exists, which
    /// is a decline, not a fallback.
    [[nodiscard]] bool ir_plan_snapshot_block(const ir::IrBlock& block) const noexcept;

    /// Gate-checked Tier 1 attempt. Called once per block at Seam A, before
    /// any ARM word is emitted. Sets `m_ir_emit_active` when the block will be
    /// lowered by `ir_emit_block` instead of the stencil loop.
    void ir_attempt_block(const BasicBlock& block);
    /// The policy half of the gate: VITREO_JIT_IR (or its default) admits
    /// this block to Tier 1. Says nothing about whether the IR accepts it.
    [[nodiscard]] bool ir_gate_open(const BasicBlock& block) const;

    /// Count how many live-out flag definitions a one-instruction look into
    /// the successor would prove dead. Measurement only, gated on
    /// `VITREO_JIT_IR_STATS`; emits nothing and changes no code.
    void ir_measure_cross_block_flags() noexcept;

    /// Lower `m_ir_block` into `m_code_buf`, replacing the stencil compiler's
    /// per-instruction loop for this block only (ADR-0002). Cannot fail.
    void ir_emit_block(const BasicBlock& block);

    /// The long-mode / snapshot-backed half of `ir_emit_block`, taken when
    /// `m_ir_plan.snapshot_path` is set (ir_emit_snapshot.cpp). Cannot fail.
    void ir_emit_snapshot_block(const BasicBlock& block);

    /// True when Seam A accepted this block AND `ir_emit_feasible` agreed, so
    /// the stencil instruction loop is skipped. Reset per block.
    bool m_ir_emit_active{false};
    /// Set by `ir_emit_feasible` when it declines, so the coverage report can
    /// distinguish a missing lowering from register pressure.
    mutable ir::DeclineReason m_ir_emit_decline{
        ir::DeclineReason::BackendUnsupported};

    // True if the current block is being compiled in 64-bit (long) mode.
    // When set, compile_instruction routes EVERY opcode through the
    // interpreter-delegate stencil (no native x32-only stencils), and
    // emits 64-bit-safe RIP arg materialization + post-delegate
    // divergence checks.  Set by compile() from BasicBlock::is_64bit.
    bool m_block_is_64bit{false};

