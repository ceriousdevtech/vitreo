// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_emit_tails.hpp
/// @brief Address formation, RIP and epilogue exits, linked tails, the clobber-contract test hook, sub-dispatch families and block tails.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_emit_tails.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    /// Emit: compute effective address for a memory operand into Xt.
    /// When include_segment_base is false, FS/GS segment base is not applied
    /// (required for LEA semantics).
    void emit_compute_ea(u8 dst_arm_reg, const MemoryOperand& mem,
                         u64 rip_val, bool include_segment_base = true);

    /// 32-bit EA into `scratch`, or the live pin when the address *is* that
    /// pin (`[ebx]` / `[edx]`). Callers must not write the returned pin.
    u8 emit_addr32(u8 scratch, const MemoryOperand& mem, u64 rip_val,
                   bool include_segment_base = true);

    /// `[esp+disp]` 32-bit load via a pinned host frame. `disp` must be a
    /// non-negative 4-byte-aligned unsigned LDR offset. False = use EA.
    bool try_esp_disp_load32(u8 dst_arm_reg, const MemoryOperand& mem);

    /// `[esp+disp]` 32-bit store: full SMC/span probe on the guest address,
    /// then `STR` through the live ESP host frame when the store is inline.
    bool try_esp_disp_store32(u8 src_arm_reg, const MemoryOperand& mem);

    /// 64-bit variant — full 64-bit EA. Used by 64-bit guest codegen where
    /// RIP-relative or large displacements may not fit in 32 bits.
    void emit_compute_ea64(u8 dst_arm_reg, const MemoryOperand& mem,
                           u64 rip_val, bool include_segment_base = true);
    /// Like emit_compute_ea64, but a pinned `[reg]` or `[reg+disp]` with no
    /// index or segment override stays off the X4 scratch: zero disp returns
    /// the pin, and an ARM64-encodable disp forms the EA in X9.
    u8 emit_ea64(u8 dst_arm_reg, const MemoryOperand& mem, u64 rip_val,
                 bool include_segment_base = true);

    /// Fold a BT/BTS/BTR/BTC bit-string byte offset into a computed effective
    /// address, at the block's address width.
    void emit_add_bit_base_offset(u8 ea_reg, u8 byte_off_reg);

    /// Publish a BT/BTS/BTR/BTC tested bit (0/1 in `bit_reg`) as CF.
    void emit_bt_write_cf(u8 bit_reg);

    /// Emit: update RIP to a new guest address
    void emit_update_rip(u64 new_rip);

    /// Publish the current instruction's RIP (m_diagnostic_instruction_rip)
    /// through `scratch` ahead of an inline BLR to a checked guest-memory
    /// helper. The helper raises with CpuState.rip as the faulting RIP and
    /// does not set it (jit_raise_access_violation); a cold memory stub
    /// publishes its own, an inline helper call does not.
    void emit_publish_rip_for_raising_helper(u8 scratch);

    /// Return to the dispatcher if a guest-memory helper raised an exception.
    /// @p flag_in_w16: the callee already left pending_raise.active in W16.
    void emit_exit_if_pending_raise(bool restore_spilled_nzcv = false,
                                    u8 spilled_nzcv_kind = 0xFF,
                                    bool flag_in_w16 = false);

    /// Emit: epilogue — return to dispatcher. Linked-tail misses persist
    /// spilled NZCV and pass `clear_kind = false` so the next execute()
    /// can restore NZCV for a Jcc-first block.
    void emit_epilogue(JitExecResult result, bool clear_kind = true);

    /// DIAGNOSTIC: emit `MOVZ W3, #reason; STR W3, [X20, #exit_reason]` on a
    /// path that is about to return to the C++ dispatcher. Emits NOTHING
    /// unless jit_exit_stats_enabled(). Both instructions are flag-neutral,
    /// so a tail that carries live PSTATE into emit_shared_continue_exit()
    /// stays correct. W3 is dead on every call site (see the definition in
    /// stage_b1_inline_block_link.cpp).
    void emit_exit_reason(JitExitReason reason);

    /// DIAGNOSTIC: emit `LDR W3,[X20,#off]; ADD W3,W3,#1; STR W3,[X20,#off]`
    /// for one of the JitContext linked-tail hit counters. Emits NOTHING
    /// unless jit_exit_stats_enabled(). Flag-neutral (plain ADD, not ADDS).
    /// `field_off` must be `offsetof(JitContext, <counter>)`.
    void emit_link_hit_counter(u32 field_off);

    /// Emit the BlockCache L1 index of the guest address in `src` into `dst`.
    ///
    /// The ONE place the emitted form of BlockCache::l1_index() lives. Every
    /// L1 probe tail must go through this: the C++ lookup and the emitted
    /// probe indexing a slot differently would send a hop into the wrong
    /// block, and there are four probe sites to keep in step. `src` is left
    /// untouched, and `dst` may equal `src`.
    void emit_l1_index(u8 dst, u8 src);
    /// Discard every native guest-CALL continuation frame by restoring SP to
    /// JitContext::native_sp_base, leaving the dispatcher LR frame on top.
    /// Emitted on every dispatcher exit of a native-CALL/RET block.
    void emit_discard_native_call_frames();
    /// The x64 call landing pad (kNativeCallFrameSize frame ABI in
    /// jit_context.hpp), emitted directly before the linked entry; sets
    /// m_call_entry_pos.
    void emit_native_call_landing_pad();
    /// RET with the popped guest RIP in `target_reg`: return natively when the
    /// top host frame is this chain's continuation for exactly that RIP,
    /// falling through otherwise. Emits nothing unless native CALL/RET is on.
    void emit_native_ret_check(u8 target_reg);
    void emit_persist_spilled_nzcv();
    void emit_persist_live_nzcv();

    /// Stage B1: emit an inline-L1-lookup tail that re-enters compiled
    /// code at `target` without returning to the dispatcher.  On L1
    /// miss / start_addr mismatch / null compiled_code, falls through to
    /// the standard `emit_epilogue(Continue)` path.  Caller must have
    /// already called `emit_update_rip(target)` (so the fallback path
    /// communicates the correct next guest IP).
    /// Returns true if the linked tail was emitted (in which case the
    /// caller MUST NOT also call emit_epilogue), false otherwise.
    /// `carry` is stored in cpu.nzcv_kind (unless Carried) and NZCV is
    /// spilled around the L1 SUBS/CMP so a Jcc-first target can B.cond.
    /// `edge` names the constant-successor class of this tail. Anything but
    /// LinkEdge::None additionally emits a direct-link patch slot when that
    /// class is enabled in link_edge_mask(); the L1 tail below is unchanged
    /// either way, so a disabled class costs nothing and changes nothing.
    bool emit_linked_tail_continue(u64 target,
                                  NzcvState carry = NzcvState::None,
                                  LinkEdge edge = LinkEdge::None);

    /// **The lazy-flag publication's clobber contract.**
    ///
    /// `emit_lazy_flag_store_raw` and its two wrappers write ONLY X3
    /// (REG_SCRATCH3) besides the CpuState slots they publish. Callers rely on
    /// that: `shifts.cpp` and `mul_div.cpp` compute a value into a scratch and
    /// publish immediately, so a wider clobber destroys it.
    ///
    /// This is not documentation. Widening the set was tried -- REG_SCRATCH4,
    /// to pair flag_src/flag_dst into one STP -- and it destroyed the CF/OF
    /// `shifts.cpp` had parked there, diverging from the interpreter on every
    /// Sh{l,r}Rm{8,16}By1_CarryOut and SarRm{8,16}Imm8 parity test. The set is
    /// asserted against the emitted words by
    /// `JitClobberContract.LazyFlagPublicationWritesOnlyItsDeclaredScratch`,
    /// so widening it now fails at the emitter rather than in a distant
    /// parity test -- and changing this constant is the visible act that says
    /// "every publication site must be re-audited".
public:
    static constexpr u32 kLazyFlagStoreClobbers = 1u << 3;  // X3 only

    /// Test hook: run one emitter into a fresh buffer and hand back its words,
    /// so a test can check which registers it writes against the contract
    /// declared beside it. See ADR-0006.
    enum class TestEmitter : u8 {
        LazyFlagStoreNarrow,
        LazyFlagStoreWide,
        SetLazyFlagsPreserveCf,
        ClearCfOverlay,
    };
    [[nodiscard]] std::vector<u32> emit_for_test(TestEmitter which);

private:
    /// 64-bit shifts and rotates, plus the 16/32-bit forms that appear inside
    /// a 64-bit block. nullopt when the opcode is neither, so `compile_64bit`
    /// continues into its own switch. Split out of compile_64bit; ADR-0006.
    [[nodiscard]] std::optional<bool> compile_shifts64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);

    /// The rest of compile_64bit's families, one TU each under ops/. nullopt
    /// means "not one of mine", so compile_64bit tries the next; split out
    /// of compile_64bit, see ADR-0006.
    [[nodiscard]] std::optional<bool> compile_mov64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_bit_ops64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_narrow_alu64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_arithmetic64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_adc_sbb64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_compare64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_unary64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_stack64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_control64(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip,
        bool is_last);
    [[nodiscard]] std::optional<bool> compile_flags64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_mul_div64(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip,
        bool is_last);
    [[nodiscard]] std::optional<bool> compile_misc64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_string_ops64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_sse64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);
    [[nodiscard]] std::optional<bool> compile_sse_move64(
        const DecodedInstruction& instr, u64 next_rip, bool is_last);

    /// End-of-block tail for a non-control-flow op in a 64-bit block.
    void finish_op64_block(u64 next_rip, bool is_last);

    /// AVX/AVX2/FMA3/F16C opcodes outside the legacy SSE opcode range.
    /// nullopt when the opcode is not one of them, so `compile_sse` continues
    /// into its own switch. Split out of compile_sse; see ADR-0006.
    [[nodiscard]] std::optional<bool> compile_avx_unique(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip,
        bool is_last);

    /// The legacy SSE opcode space of compile_sse, one family per
    /// ops/sse_*.cpp (ADR-0006 step 3; the arms are the old single
    /// switch's, verbatim). nullopt when the opcode is not in the family,
    /// so compile_sse tries the next one; false when the family declines
    /// it; true when its arm reached `break`. Unlike compile_avx_unique,
    /// true does NOT mean the block tail was emitted: compile_sse then
    /// runs the shared tail itself (MMX-state entry for an MM operand,
    /// then finish_vector_block), exactly as after the old switch.
    [[nodiscard]] std::optional<bool> compile_sse_data(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_conv_packed(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_int(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_fp_packed(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_neon_stencils(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_fp_reciprocal(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_fp_scalar(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_crypto(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_shuffle(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_shift(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_insert_extract(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_round(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_cmp(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_mmx_forms(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_state(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse4_helper_ops(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);
    [[nodiscard]] std::optional<bool> compile_sse_conv_scalar(
        const DecodedInstruction& instr, u64 instr_rip, u64 next_rip);

    /// The end-of-block tail a vector lowering reaches by `break`.
    void finish_vector_block(u64 next_rip, bool is_last);

    /// SSE register-to-register copy with a helper fallback. Promoted out of
    /// compile_sse, where it was a lambda its own case arms closed over.
    /// V<dst> = the CMPPS/CMPPD predicate `pred` (imm8[4:0]; imm8[4] swaps
    /// quiet/signalling, which changes MXCSR.IE, not the mask) of
    /// V<a> = SRC1 against V<b> = SRC2, per element of size `dbl`, raising
    /// invalid operation in the host FPSR exactly as Intel SDM Vol. 2 CMPPS
    /// Table 3-1 has it. `scalar` (CMPSS/CMPSD): element 0 only, in the
    /// compare and in the exception; the rest of V<dst> is unspecified.
    /// Clobbers V2, V3.
    void emit_fp_cmp_predicate(u8 dst, u8 pred, bool dbl, u8 a = 0,
                               u8 b = 1, bool scalar = false);
    /// SHA1RNDS4/SHA1NEXTE/SHA1MSG1/SHA1MSG2 on FEAT_SHA1: XMM @p dst :=
    /// OP(XMM @p dst, V1). The caller has put the second operand in V1.
    void emit_sha1_native(Opcode op, u8 dst, u8 imm8);
    /// Give V<r>, the fused result of a*b+c (any sign variant), x86 NaN
    /// semantics: an invalid result with no NaN input is the negative QNaN
    /// indefinite, and a NaN input propagates quieted with its sign intact,
    /// priority a, then b, then c (tools/oracle_probe/fma_nan_priority_probe).
    /// ARM FMLA/FMADD prefer the addend and SNaNs, and FNEG flips a NaN's
    /// sign. a/b/c/r must be distinct from the three temps. Clobbers X0.
    void emit_x86_fma_nan_fixup(u8 r, u8 a, u8 b, u8 c, bool dbl,
                                u8 t0, u8 t1, u8 t2);
    void emit_sse_reg_copy(u8 dst_xmm, u8 src_xmm, u16 fallback_op_id);
    /// 128-bit guest load into an XMM slot, fast path plus helper fallback.
    void emit_sse_load_mem128_fast(u8 dst_xmm, u8 guest_addr_reg, u32 mem_op_id);
    /// 128-bit guest store from an XMM slot, fast path plus helper fallback.
    /// @p instr_rip is published before the helper so a fault names the store.
    void emit_sse_store_mem128_fast(u8 src_xmm, u8 guest_addr_reg,
                                    u64 instr_rip);
    /// A 16- or 32-byte vector store of the CpuState bytes at @p lo_off
    /// (and @p hi_off for bytes 16-31) to the guest EA in @p addr_reg, as ONE
    /// validated write: a store denied anywhere performs none of itself and
    /// raises the access violation Windows reports (Intel SDM Vol. 3A 6.15;
    /// tools/oracle_probe/simd_store_fault_probe.expected). A 32-bit block
    /// with the linear model stores inline when the operand stays in one
    /// guest page that holds no code; everything else calls
    /// jit_mem_write_span. Clobbers X0-X17 and the vector scratch; the
    /// caller holds NZCV when it is live.
    void emit_vec_store_span(u8 addr_reg, u32 lo_off, u32 hi_off, u32 width,
                             u64 instr_rip);
    /// The jit_mem_write_span call of emit_vec_store_span, with no fast path.
    void emit_vec_store_span_helper(u8 addr_reg, u32 lo_off, u32 hi_off,
                                    u32 width, u64 instr_rip);

    /// Link into `target`, or fall back to the dispatcher tail.
    ///
    /// Every constant-successor terminator in the JIT ends this way, and the
    /// fallback -- spill the loop counter, then a Continue epilogue -- was
    /// open-coded at TWENTY call sites across `control.cpp`, `ops64.cpp`,
    /// `arithmetic.cpp`, `mov.cpp` and `jit_compiler.cpp`, identically. That
    /// is the duplication class that let the two-store lazy-flag publication
    /// land on the 32-bit path and silently miss x64: a fix applied to one
    /// copy of an idiom is not applied to the others, and nothing says so.
    ///
    /// Behaviour is byte-identical to the open-coded form; `ji_off` at every
    /// site was `jit_iters_offset()` (verified: all 66 definitions).
    void emit_tail_link_or_exit(u64 target,
                                NzcvState carry = NzcvState::None,
                                LinkEdge edge = LinkEdge::None);

    /// Opt-in x64 direct-CALL entry. After the architectural guest stack push
    /// has retired, validate depth, exact target, pin layout, and call entry,
    /// then BLR through the target's native landing pad. Every miss uses the
    /// established linked/dispatcher CALL tail. The post-BLR continuation is
    /// present for the matching native RET slice; until then it is unreachable.
    bool emit_linked_call_continue(u64 target, u64 next_rip);

    /// True when `target` is this block's start or an already-compiled insn.
    /// `arm_pos` is the code-buffer index to B to (quantum-capped loop).
    [[nodiscard]] bool find_in_block_loop_pos(u64 target, u32& arm_pos) const;

    /// Same L1 tail-link as emit_linked_tail_continue, but the guest RIP is
    /// already in `target_reg` (RET / JMP r/m / CALL r/m). Caller must have
    /// stored that RIP into CpuState. Returns true if the tail was emitted.
    /// When `try_rsb_first` is true, attempt a software RSB pop before L1
    /// (RET paths); shared hop-cap / NZCV fill / BR tail.
    /// `api_call_tail` marks an indirect CALL/JMP: its L1 miss takes the
    /// in-chain API thunk exit (emit_shared_api_call_exit) instead of the
    /// plain continue exit when inchain_thunks_codegen() is on.
    bool emit_linked_tail_reg(u8 target_reg,
                             NzcvState carry = NzcvState::None,
                             bool try_rsb_first = false,
                             bool native_ret = false,
                             bool api_call_tail = false);

    /// RET linked tail: RSB pop then L1 (see emit_linked_tail_reg).
    bool emit_linked_tail_ret(u8 target_reg,
                             NzcvState carry = NzcvState::None);

    /// Emit ARM that pushes `{return_rip, host_link}` onto the software RSB.
    /// `host_link` comes from an inline L1 lookup of `return_rip` (nullptr on
    /// miss / not compiled). Clobbers X16/X17/W3/W4; must not run while
    /// REG_CALL_TMP holds a live callee.
    void emit_rsb_push(u64 return_rip);

    /// Try to inline a CALL rel32 to a recognized MS CRT 64-bit shift
    /// helper (`__allshl` or `__aullshr`). Returns true if the call was
    /// replaced by inline ARM64 (caller must not emit any further
    /// CALL/epilogue logic for this instruction).
    bool try_emit_crt_shift_inline(u64 target, u64 next_rip, bool is_last);

