// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_lowering.hpp
/// @brief Instruction lowering: compile_instruction, flag-record omission and retry state, per-category opcode compilers.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_lowering.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    /// Compile one decoded instruction. Returns false if unsupported.
    /// Thin wrapper over compile_instruction_impl that maintains the
    /// native-vs-delegate coverage counters (see jit_coverage.hpp).
    bool compile_instruction(const DecodedInstruction& instr,
                             u64 instr_rip, u64 next_rip,
                             bool is_last);

    /// Actual lowering selection. Sets m_last_insn_delegated when the
    /// instruction fell through to the interpreter delegate.
    bool compile_instruction_impl(const DecodedInstruction& instr,
                                  u64 instr_rip, u64 next_rip,
                                  bool is_last);

    /// Set by compile_instruction_impl's delegate path, read by the wrapper.
    /// Block-local bookkeeping only; carries no correctness meaning.
    bool m_last_insn_delegated{false};

    /// When true, the next emit_set_lazy_flags(_64) is a no-op because
    /// within-block liveness proved this full-flag writer's result is dead
    /// (overwritten by a later full-flag writer before any consumer).
    bool m_omit_lazy_current{false};
    /// Set with m_omit_lazy_current when the omission was granted by the
    /// adjacent-ADC/SBB rule (flag_liveness marks it 2). The record itself is
    /// dead, but the CF/OF overlay clear that is normally part of publishing
    /// it is NOT: the consumer reads CF from PSTATE and the overlay would win
    /// over it, so a stale CLC/STC must still be retired here.
    bool m_omit_lazy_keep_overlay{false};
    /// Set on a forward Jcc whose taken edge the liveness internal-join proof
    /// discharged. Only then may the Jcc be compiled as a real intra-block
    /// branch, which has no cold stub to replay the predecessor's deferred
    /// lazy-flag store on.
    bool m_internal_join_jcc_current{false};
    std::vector<u8> m_omit_lazy;
    std::vector<StencilFlagRecovery> m_stencil_flag_recovery;
    /// Per instruction: the owning publisher + 1 where an omitted publisher's
    /// record is absent from CpuState (compute_omit_lazy_flags). Emission
    /// there may read flags only from host NZCV; a record read, or a block
    /// truncated there so the interpreter would read it, sets
    /// m_elided_record_read, and the attempt is retried without crossing
    /// SETcc / CMOVcc (m_force_no_omit), which is the analysis that never
    /// relied on a reader fusing.
    std::vector<u32> m_elided_span;
    bool m_elided_live_current{false};
    bool m_elided_record_read{false};
    bool m_force_no_omit{false};
    bool m_retry_no_omit{false};
    /// The no-crossing attempt still read an elided record: the last attempt
    /// elides none (m_force_no_elide), so no read can be stale.
    bool m_force_no_elide{false};
    bool m_retry_no_elide{false};
    /// One compile() attempt; compile() wraps it with the no-omission retry.
    CompiledBlock* compile_attempt(const BasicBlock& block);
    /// Where the first guarded read of an attempt happened, for its report:
    /// the instruction index, the publisher owning the elided span, and
    /// whether it was a record read (0) or a block truncated there (1).
    u32 m_elided_read_insn{0};
    u32 m_elided_read_owner{0};
    u8 m_elided_read_kind{0};
    u32 m_current_insn_index{0};
    /// The previous instruction's deferred record (m_incoming_stub_store) was
    /// just published inline on the hot path, so from here on that
    /// publisher's record IS in CpuState: its span no longer guards reads.
    void note_elided_record_republished() noexcept {
        if (m_current_insn_index == 0) return;
        note_elided_record_published(m_current_insn_index - 1);
    }
    /// Publisher @p publisher's skipped record was published inline after
    /// all (by the Jcc after it, or by its own store, see
    /// publish_pending_stub_store): from here on it IS in CpuState, so its
    /// span no longer guards reads.
    void note_elided_record_published(u32 publisher) noexcept {
        const u32 cur = m_current_insn_index;
        if (cur >= m_elided_span.size()) return;
        const u32 owner = publisher + 1;  // spans store the publisher + 1
        for (u32 k = owner; k < m_elided_span.size(); ++k) {
            if (m_elided_span[k] == owner) m_elided_span[k] = 0;
        }
        m_elided_live_current = m_elided_span[cur] != 0;
    }
    /// A lazy-record read is about to be emitted (kind 0), or the block is
    /// being truncated so the interpreter would read it (kind 1).
    void note_lazy_record_read(u8 kind = 0) noexcept {
        if (!m_elided_live_current || m_elided_record_read) return;
        m_elided_record_read = true;
        m_elided_read_insn = m_current_insn_index;
        m_elided_read_owner = m_current_insn_index < m_elided_span.size()
                                  ? m_elided_span[m_current_insn_index] : 0;
        m_elided_read_kind = kind;
    }

    /// Opcode of the next instruction in the block being compiled
    /// (Opcode::INVALID for the last one). Advisory lookahead only — a
    /// lowering may use it to decide whether publishing NZCV is worth the
    /// spill tax it puts on the following instruction; never a correctness
    /// input.
    Opcode m_next_opcode{Opcode::INVALID};
    // Exact decoded lookahead used only for the adjacent ADD→self-TEST fold.
    // 0xFF unless the next instruction is TEST r32,r32 on one register.
    u8 m_next_self_test_gpr{0xFF};

    /// Count of natively-lowered instructions emitted into the block currently
    /// being compiled. Stashed into CompiledBlock so block execution can
    /// attribute native instruction counts at runtime.
    u32 m_block_native_insns{0};
    u16 m_block_terminal_opcode{0};
    /// Per-instruction emitted-ARM footprint for the block being compiled.
    /// Populated only when VITREO_JIT_STATS / VITREO_JIT_DISASM is set; empty
    /// and untouched otherwise. Observation only -- see jit_stats.hpp.
    std::vector<stats::InsnSite> m_stats_insns;

    // ---- Per-category opcode compilation (defined in ops/*.cpp) ----
    bool compile_mov(const DecodedInstruction& instr,
                     u64 instr_rip, u64 next_rip, bool is_last);
    void emit_carry_flag_control(Opcode opcode);

    bool compile_arithmetic(const DecodedInstruction& instr,
                            u64 instr_rip, u64 next_rip, bool is_last);
    /// The ALU lowering families split out of compile_arithmetic (ADR-0006
    /// step 3), compile_<family> in ops/<family>.cpp. Each returns nullopt
    /// when the opcode is not in its family, so compile_arithmetic offers the
    /// instruction to the next family; otherwise true, meaning the lowering
    /// reached its `break`, after which compile_arithmetic runs the
    /// end-of-block tail. The opcode sets are disjoint.
    [[nodiscard]] std::optional<bool> compile_alu32(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_alu16(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_alu8(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_adc_sbb(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_unary(
        const DecodedInstruction& instr, u64 next_rip);
    bool compile_shifts(const DecodedInstruction& instr,
                        u64 instr_rip, u64 next_rip, bool is_last);
    /// The shift/rotate families split out of compile_shifts,
    /// compile_shifts_<family> in ops/shifts_<family>.cpp; the case arms are
    /// the old single switch's, verbatim. nullopt when the opcode is not in
    /// the family, so compile_shifts tries the next; true when its arm
    /// reached break, after which compile_shifts emits the shared is_last
    /// tail exactly as after the old switch.
    [[nodiscard]] std::optional<bool> compile_shifts_rm8(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_shifts_rm16(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_shifts_rotate_narrow(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_shifts_rotate_rm32(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_shifts_rcl_rcr_rm32(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_shifts_rcl_rcr_narrow(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_shifts_shld_shrd(
        const DecodedInstruction& instr, u64 next_rip);
    bool compile_mul_div(const DecodedInstruction& instr,
                         u64 instr_rip, u64 next_rip, bool is_last);
    bool compile_control(const DecodedInstruction& instr,
                         u64 instr_rip, u64 next_rip, bool is_last);
    bool compile_stack(const DecodedInstruction& instr,
                       u64 instr_rip, u64 next_rip, bool is_last);
    bool compile_memory_ops(const DecodedInstruction& instr,
                            u64 instr_rip, u64 next_rip, bool is_last);
    /// compile_memory_ops' opcode families, ops/memory_ops_<family>.cpp, split
    /// out of ops/memory_ops.cpp as pure code motion. nullopt = not in this
    /// family, so compile_memory_ops tries the next; false = the family
    /// declines; true = its arm reached break, after which compile_memory_ops
    /// runs the shared block tail exactly as after the old single switch.
    /// Label sets are disjoint.
    [[nodiscard]] std::optional<bool> compile_memory_ops_xchg(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_memory_ops_cond(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_memory_ops_bt(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_memory_ops_bitscan(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_memory_ops_bmi(
        const DecodedInstruction& instr, u64 next_rip);
    bool compile_string_ops(const DecodedInstruction& instr,
                            u64 instr_rip, u64 next_rip, bool is_last);
    /// REP MOVS/STOS: native element loop when ECX <= 64, else existing helper.
    /// Overlap is handled by walking in DF order. SMC uses emit_guest_store.
    void emit_rep_movs_stos_or_helper(u32 elem_sz, bool is_stos, u32 helper_off);
    /// Native arithmetic body. compile_fpu wraps this with the precision
    /// control guard; nothing else should call it directly.
    bool compile_fpu_native(const DecodedInstruction& instr,
                            u64 instr_rip, u64 next_rip, bool is_last);
    /// The x87 lowering families split out of compile_fpu_native (ADR-0006),
    /// compile_fpu_<family> in ops/fpu_<family>.cpp. Each returns nullopt
    /// when the opcode is not in its family, so compile_fpu_native offers the
    /// instruction to the next family and finally to the jit_fpu_op helper
    /// call; otherwise the lowering's own result. The opcode sets are
    /// disjoint.
    [[nodiscard]] std::optional<bool> compile_fpu_control(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_stack(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_load(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_store(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_arith_reg(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_arith_mem(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_arith_int(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_math(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_fpu_compare(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);

    bool compile_fpu(const DecodedInstruction& instr,
                     u64 instr_rip, u64 next_rip, bool is_last);
    bool compile_sse(const DecodedInstruction& instr,
                     u64 instr_rip, u64 next_rip, bool is_last);
    /// One 64-bit guest store of XMM `xmm`'s low (half 0) or high (half 1)
    /// quadword to `dst` (MOVSD/MOVLPS/MOVLPD/MOVHPS/MOVHPD m64, xmm).
    bool emit_xmm_qword_store(u8 xmm, unsigned half,
                              const DecodedInstruction& instr,
                              const Operand& dst, u64 next_rip);
    bool compile_sse_minmax(const DecodedInstruction& instr,
                           u64 instr_rip, u64 next_rip, bool is_last);
    bool compile_locked(const DecodedInstruction& instr,
                        u64 instr_rip, u64 next_rip, bool is_last);
    /// compile_locked's opcode families, ops/locked_<family>.cpp, split out of
    /// ops/memory_ops.cpp as pure code motion, with the same nullopt / false /
    /// true contract as compile_memory_ops' families above. compile_locked
    /// keeps the LOCK_DIAG preamble, the unsupported-opcode diagnostic and the
    /// pending-raise/is_last tail. Label sets are disjoint.
    [[nodiscard]] std::optional<bool> compile_locked_rmw32(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_locked_rmw64(
        const DecodedInstruction& instr, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_locked_narrow(
        const DecodedInstruction& instr, u64 next_rip);

    /// Native LOCK read-modify-write (ops/native_lock.cpp). The fast path is
    /// one FEAT_LSE atomic on the guest-base window, bracketed by the reader
    /// side of the LockPrefixGuard protocol; every case it cannot take branches
    /// to the form's existing helper sequence, which the caller emits right
    /// after it. Flags always end up in CpuState memory on both paths.
    enum class NativeLockKind : u8 {
        Add, Sub, Or, And, Xor, Xadd, Inc, Dec, Xchg, Cmpxchg,
    };
    struct NativeLockSite {
        bool active{false};
        /// Branches to the helper path: position, register, and X-width.
        struct SlowBranch { usize pos; u8 reg; bool wide; };
        std::vector<SlowBranch> to_slow;
        usize to_standdown{0};
        usize to_done{0};
    };
    /// Emits the fast path and, on success, leaves the emitter at the start
    /// of the helper path (the caller's existing sequence). Returns false and
    /// emits nothing when the native form does not apply.
    bool emit_native_lock_rmw(NativeLockSite& site, NativeLockKind kind,
                              int width, const MemoryOperand& mem,
                              u64 next_rip, const Operand* src);
    /// After the caller's helper sequence: point the fast path's exit here.
    void join_native_lock(NativeLockSite& site);

    /// 64-bit native codegen — covers REX.W variants of MOV/ALU/LEA/PUSH/POP
    /// /MOVZX/MOVSX/MOVSXD/INC/DEC/NEG/NOT. Returns true if the opcode was
    /// natively handled, false if the caller should fall back to the
    /// universal interpreter-delegate path.
    bool compile_64bit(const DecodedInstruction& instr,
                       u64 instr_rip, u64 next_rip, bool is_last);

    /// Native ARM64 codegen for the hot MMX opcodes (MOVD/MOVQ/PMULUDQ/
    /// PSRLQ/PSLLQ). Mode-independent — MMX semantics don't change with
    /// REX — but memory operands use 64-bit-safe addressing, so this is
    /// wired into the 64-bit path. Returns false for any opcode it does
    /// not handle, so the caller falls back to the interpreter delegate.
    bool compile_mmx(const DecodedInstruction& instr,
                     u64 instr_rip, u64 next_rip, bool is_last);

    /// Emit direct ARMv8 SHA-256 message-schedule code for the register-source
    /// forms of x86 SHA256MSG1/SHA256MSG2, replacing the `jit_sse_op` helper
    /// dispatch. Returns false when the form must stay on the helper (memory
    /// source, unreachable XMM save-area offset, or an unhandled opcode); the
    /// caller then emits its existing helper sequence unchanged.
    bool emit_sha256_msg_direct(Opcode opcode, u8 dst_xmm, u8 src_xmm);

    /// Emit direct ARMv8 FEAT_AES code for x86 AESENC/AESENCLAST/AESDEC/
    /// AESDECLAST/AESIMC, replacing the `jit_sse_op` helper dispatch, in both
    /// the register-source and memory-source forms. Returns false when the
    /// form must stay on the helper (host without FEAT_AES, an unhandled
    /// opcode, an unreachable XMM save-area offset, or a memory source in a
    /// memory configuration with no validated inline 128-bit vector load);
    /// the caller then emits its existing helper sequence unchanged.
    /// Emits nothing on a false return.
    bool emit_aes_direct(const DecodedInstruction& instr, u64 next_rip);

    /// ROUNDSS/ROUNDSD (Intel SDM Vol. 2) lowered inline: scalar FRINT*,
    /// MXCSR.PE/IE, and the DEST[127:32|64] merge from `src1_xmm` (the
    /// destination itself for the legacy form, VEX.vvvv for the VEX form,
    /// which also zeroes YMM[255:128] when `zero_upper`). Emits nothing and
    /// returns false when a CpuState offset is out of reach.
    bool emit_round_scalar(const DecodedInstruction& instr, u64 next_rip,
                           u8 src1_xmm, bool zero_upper);

    /// MXCSR |= flag, through REG_SCRATCH2/REG_SCRATCH3. NZCV untouched.
    void emit_mxcsr_or_flag(u32 flag);

    /// Branches past `emit_mxcsr_or_flag(MXCSR_IE)` unless lane 0 of V1
    /// holds a signalling NaN of the given width (exponent all ones, quiet
    /// bit clear, payload non-zero). Clobbers REG_SCRATCH0/2/3, no NZCV.
    void emit_round_ie_if_snan_lane0(bool dbl);

    /// Emit an SMC-guarded linear store of `width` bytes (1/2/4/8): if the
    /// guest page has compiled code, route through the write helper (which
    /// drives notify_executable_write); otherwise store inline at
    /// linear_base+addr. Only valid when the linear model is active.
    /// Linear store with SMC/span probe. When `esp_frame_disp >= 0`, the
    /// inline path uses `STR [esp_frame, #disp]` (probe still uses guest EA).
    void emit_linear_store(u8 src_arm_reg, u8 addr_arm_reg, int width,
                           i32 esp_frame_disp = -1);
    /// Check-byte lowering of the above, used whenever the reservation carries
    /// a per-host-page check map (`m_cfg.linear32_check_map`). One LDRB of that
    /// byte answers "does this page hold translated code or belong to a live
    /// MEM_WRITE_WATCH allocation"; anything set takes the cold helper, which
    /// performs the write, the SMC notify and the write-watch record.
    void emit_linear_store_checked(u8 src_arm_reg, u8 addr_arm_reg, int width,
                                   i32 esp_frame_disp = -1);
    /// Store W10 at linear_base + zext(W9), `width` bytes, honouring the TSO
    /// mode. Shared by the fast path and the cold stub's warm half.
    void emit_linear_store_body(int width);
    /// True when a 32-bit guest store lowers to the NZCV-clean check-byte
    /// sequence, so the guest flags can be saved in its cold stub instead of
    /// around every inline store.
    [[nodiscard]] bool linear32_store_is_nzcv_clean() const noexcept;

    /// True when a 64-bit guest STORE lowers through `emit_linear64_store`,
    /// whose whole sequence -- MOV/ADD/ORR/LSR, the CBNZ range guard, the
    /// LDRB/CBNZ check byte, and STR/STLR -- contains no instruction that
    /// writes NZCV. The inline MRS/STR + LDR/MSR pair is then pure waste on
    /// every execution, and `ColdMemStub::spill_nzcv` pays it only on the
    /// rarely-taken helper path, exactly as the 32-bit check-byte store does.
    ///
    /// Deliberately NOT true for the region path: `emit_x64_region_store`
    /// bounds-checks with SUBS and genuinely destroys the guest flags.
    [[nodiscard]] bool linear64_store_is_nzcv_clean() const noexcept;

    /// Default store probe is exact dual-index (LDRB map[i] OR map[end]).
    /// VITREO_JIT_STORE_PROBE_HALFWORD=1 restores LDRH(map[i]|map[i+1]) for
    /// browser-guest A/B; neighbour-only false positives then take the cold helper.
    [[nodiscard]] static bool linear32_store_probe_halfword() noexcept;
    /// Mark guest page(s) dirty when a write-watch region is live.
    /// `addr_wreg` is the 32-bit guest address; `last_byte` is width-1.
    void emit_mark_write_watch(u8 addr_wreg, u32 last_byte);

    /// Load MMX register MMn's 64-bit mantissa into an ARM64 X register.
    void emit_load_mm(u8 dst_arm_reg, u8 mm_reg);

    /// Store an ARM64 X register into MMX register MMn, replicating
    /// FpuState::set_mm exactly: write the mantissa AND set the x87
    /// exponent/sign halfword to 0xFFFF (the "MMX in use" tag).
    /// Clobbers REG_SCRATCH3 and REG_EA_TMP for address/tag scratch.
    void emit_store_mm(u8 src_arm_reg, u8 mm_reg);

