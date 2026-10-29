// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_cold_stubs.hpp
/// @brief Cold-region stub records: mid-block Jcc exits, memory and linear-store slow paths, raise/UD2/alignment stubs.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_cold_stubs.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    // Mid-block forward Jcc taken exits. The hot fall-through stays
    // sequential (B.cond / CBNZ over the stub). Stubs are emitted after
    // the block body so they do not sit between compare and the next insn.
    struct ColdJccExit {
        usize branch_pos = 0;
        usize helper_branch_pos = static_cast<usize>(-1);
        u64 target = 0;
        u8 arm_cond = 0;
        bool fused = false;
        NzcvState carry = NzcvState::None;
        // A publisher immediately before this Jcc whose inline lazy-flag store
        // was omitted. The fall-through path has a later writer that kills it,
        // but the taken edge leaves the block with those flags live, so the
        // store is emitted here in the cold stub instead of in the hot body.
        // Only ever set for the fused path, which emits nothing between the
        // publisher and the branch — so these ARM registers still hold the
        // publisher's operands when the stub is entered.
        bool stub_store = false;
        CpuState::FlagOp stub_op = CpuState::FlagOp::None;
        u8 stub_src = 0;
        u8 stub_dst = 0;
        // 64-bit publishers record full X operands; replaying them with the
        // 32-bit W-width moves would truncate flag_src/flag_dst and
        // materialize the wrong AF/PF/OF.
        bool stub_wide = false;
    };

    // Fused x64 forward Jcc whose exact target is another instruction in the
    // same trace. The branch is emitted before its ARM target exists and
    // patched when compilation reaches that guest instruction boundary.
    struct ForwardJccFixup {
        usize branch_pos{0};
        GuestAddr target{0};
        u8 arm_cond{0};
        NzcvState nzcv{NzcvState::None};
        CarrySource carry{CarrySource::Unknown};
    };
    std::vector<ForwardJccFixup> m_forward_jcc_fixups;
    // Native UD2 has no architectural fall-through. Compilation advances
    // past its cold branch, but the next forward-Jcc target may have only the
    // branch source as a real predecessor and can inherit that edge's state.
    bool m_native_ud2_linear_unreachable{false};

    // Publisher whose inline lazy-flag store was skipped, carried exactly one
    // instruction so only an immediately-following Jcc can claim it.
    struct PendingStubStore {
        bool valid = false;
        CpuState::FlagOp op = CpuState::FlagOp::None;
        u8 src = 0;
        u8 dst = 0;
            bool wide = false;
    };
    PendingStubStore m_pending_stub_store;
    PendingStubStore m_incoming_stub_store;
    /// Publish this instruction's skipped flag store (m_pending_stub_store)
    /// inline, now, if it has one. Every scalar guest-store emitter calls it
    /// first: a memory-destination flag writer stores its result AFTER the
    /// deferral, and the store's slow path calls a helper that clobbers the
    /// operand registers the next instruction's Jcc would replay.
    void publish_pending_stub_store();
    // Non-replayable N/Z provenance for a flag-setting immediate ALU result
    // whose exact next instruction is a register self-TEST. Unlike
    // PendingStubStore this carries no lazy source and cannot reach a cold exit.
    u8 m_pending_alu32_nz_result{0xFF};
    u8 m_incoming_alu32_nz_result{0xFF};

    /// Emit the lazy-flag store unconditionally, ignoring m_omit_lazy_current.
    void emit_lazy_flag_store_raw(CpuState::FlagOp op, u8 src_arm_reg,
                                  u8 dst_arm_reg, bool wide);
    void emit_lazy_flag_store_raw(CpuState::FlagOp op, u8 src_arm_reg,
                                  u8 dst_arm_reg);

    /// Emit a scalar float->int conversion that honours MXCSR.RC.
    ///
    /// `fcvt_op_base` is the FCVTNS encoding for the desired widths already
    /// carrying its Rn/Rd fields; the rmode bits (20:19) are substituted per
    /// mode. x86 RC and ARM rmode agree on nearest (00) and zero (11) but
    /// SWAP the two directed modes: x86 01 is toward -inf where ARM 01 is
    /// toward +inf.
    void emit_fcvt_scalar_by_mxcsr(u32 fcvt_op_base);

    /// EFLAGS epilogue shared by the BMI1/BMI2 flag writers: SF and ZF from
    /// `result`, CF from the 0/1 in `cf_reg`, OF cleared. AF and PF are
    /// documented undefined for all of them and are left alone. This is a full
    /// write of the four defined flags, so it retires any pending lazy FlagOp
    /// and both overlays rather than layering on top of them.
    void emit_bmi_result_flags(u8 result_reg, u8 cf_reg, bool w64);
    /// Load host NZCV from CpuState's materialised RFLAGS in the shape a SUBS
    /// leaves (NzcvState::Sub): N = SF, Z = ZF, C = NOT CF, V = OF. For the
    /// helper arm of a lowering whose native arm ends in a SUBS, so the
    /// instruction can claim NZCV on both paths. Clobbers X2-X4.
    void emit_nzcv_sub_from_rflags();
    std::vector<ColdJccExit> m_cold_jcc_exits;

    // Out-of-lined guest-memory slow path (64-bit region load/store).
    //
    // The hot region probe now FALLS THROUGH to the merge point; the helper
    // marshal, the BLR and the raise check live in the block tail. Unlike a
    // ColdJccExit -- which is terminal -- this stub RETURNS, so it carries a
    // liveness obligation across the round trip:
    //
    //   live-in : X9 (full 64-bit EA), X10 (store value only), X19, X20, pins
    //   live-out: dst_arm (load only)
    //   NZCV    : already destroyed by the probe's SUBS before either branch
    //             is taken, so no caller can have host flags live here; the
    //             emit_nzcv_spill/fill bracket around stores keeps working
    //             because it uses [SP,#8] and SP does not move.
    //   X21     : REG_CALL_TMP. fpu.cpp parks an 80-bit mantissa in it across
    //             a guest load whose slow path runs through here, so the cold
    //             block must never touch X21. Shared scalar bodies are entered
    //             by BL (return in X30, saved in JitContext::cold_mem_lr
    //             across the helper BLR). X21 stays untouched.
    //
    // The stub is a POD rather than a closure because both emitters park the
    // whole 64-bit effective address in X9 before the probe, which is what
    // makes the slow path re-emittable verbatim at the tail.
    struct ColdMemStub {
        usize below_branch_pos = 0;  // B.cond CC placeholder (addr < base)
        usize above_branch_pos = 0;  // B.cond CS placeholder (addr+w-1 >= end)
        usize return_pos = 0;        // hot-path merge label, word index
        u32   helper_off = 0;        // JIT_CTX_OFF_READ32 / _WRITE64 / ...
        int   width = 0;
        u8    dst_arm = 0;           // load destination; unused for a store
        u8    store_arm = 10;        // preserved scalar store value
        // Guest EA. 9 means the hot path already left it in X9. Any other
        // register is copied to X9 at the trampoline so the helper ABI and
        // the hot path can share a pin without a second MOV.
        u8    addr_arm = 9;
        bool  is_store = false;
        // 128-bit vector access: the slow path splits into two 8-byte helper
        // calls staged through SseState::jit_vec_scratch, and dst_arm names a
        // V register rather than an X register.
        bool  is_vec128 = false;
        // 256-bit vector access (VEX.256 pure moves): the slow path runs the
        // 128-bit stub body twice, for dst_arm at the address and dst_arm2 at
        // address+16.
        bool  is_vec256 = false;
        u8    dst_arm2 = 0;
        // CpuState byte offsets of the two 16-byte halves the 256-bit value
        // lives in (XMM slot, then the YMM high slot). The 256-bit stub stages
        // every 8-byte helper piece through these, never through V registers:
        // a helper call clobbers V0-V7, so a half held in a V register across
        // the other half's calls is destroyed. (That was the first version of
        // this stub, and it corrupted a browser renderer within two seconds.)
        u32   vec_slot_lo = 0;
        u32   vec_slot_hi = 0;
        // linear64 has one upper-range branch rather than the region cache's
        // lower/upper pair.
        bool  single_upper_branch = false;
        // A block-entry proof established the immutable reservation bound, so
        // no per-access upper branch exists. above_branch_pos still names the
        // access for precise-RIP attribution from its protection branch.
        bool  range_proven = false;
        // Site used the process-wide hot helper; above_branch_pos is CBNZ W16
        // (helper fail status) rather than CBNZ X8 (inline range check).
        bool  hot_helper_fail = false;
        // Guest flags are live across this access and the FAST path preserves
        // NZCV, so the MRS/STR + LDR/MSR pair moves in here instead of being
        // paid inline on every execution. Only the helper call this stub makes
        // destroys the flags. Set by the linear64 emitters, whose whole guard
        // (ORR/LSR/CBNZ + LDRB/CBNZ) is NZCV-clean by construction; the region
        // path bounds-checks with SUBS and must keep spilling inline.
        bool  spill_nzcv = false;
        // An enclosing flag-neutral instruction already saved guest NZCV in
        // [SP,#8]. If this checked access raises, restore that slot before
        // joining the shared exception exit.
        bool  restore_nzcv_on_raise = false;
        // Compile-time lazy NZCV kind to publish after restoring the spill.
        // Zero means the runtime kind byte already owns a carried predecessor.
        u8    nzcv_kind_on_raise = 0;
        // Multi-byte linear64 last-byte calculation can wrap u64. Carry must
        // take the checked helper rather than aliasing the low reservation.
        usize overflow_branch_pos = static_cast<usize>(-1);
        // Direct linear64 accesses still require a readable/writable region
        // slot. These branches reject an absent, stale, or too-small slot.
        usize permission_below_branch_pos = static_cast<usize>(-1);
        usize permission_above_branch_pos = static_cast<usize>(-1);
        // A concurrent VirtualProtect after execute-entry invalidation must
        // fail closed until the dispatcher drains its shootdown.
        usize protection_generation_branch_pos = static_cast<usize>(-1);
        // Hardware-protected linear64: the one remaining per-access check.
        // Non-zero mixed-map byte => this 16 KiB host page cannot state the
        // guest's 4 KiB protection exactly, so the access takes the helper.
        usize mixed_page_branch_pos = static_cast<usize>(-1);
        // A scalar access that crosses a 16 KiB host page takes the checked
        // helper, allowing the hot path to inspect only its single page.
        usize host_page_cross_branch_pos = static_cast<usize>(-1);
        // A live write-watch allocation keeps linear64 stores on the checked
        // helper so record_store() observes the full 64-bit guest address.
        usize watch_branch_pos = static_cast<usize>(-1);
        // Stores intersecting the full x64 compiled-code envelope require the
        // checked helper's exact sparse-page SMC lookup.
        usize smc_branch_pos = static_cast<usize>(-1);
        // The fast path is the marker-free ordered site of linear_fault.hpp:
        // no host-page crossing test, no patch marker. Its alignment fault is
        // the crossing test, and this stub carries the crossing variant the
        // fault handler redirects it to (kTsoCrossingVariantWords, laid out
        // immediately before the check-byte branch's target).
        bool  tso_crossing_variant = false;
        // The fast path is the range-by-fault guard (linear_fault.hpp): no
        // reservation test inline, and this stub carries the range variant
        // the fault handler redirects the site to.
        bool  range_variant = false;
        // A JIT-view lean site (JitConfig::linear64_jit_view). The hot path is
        //     LSR X16, Xea, #bits ; CBNZ X16, V      (or CBNZ XZR, V: constant EA)
        //     ADD X17, X15, Xea   ; LDAPR/LDAR/STLR Rt, [X17]
        // (vectors: a crossing+window guard into X16, then LDR/STR Q
        // [X15, Xea], or ADD X17 + LDP/STP Q) and this stub carries V, the
        // site's checked variant: the site's pre-view fast path (window test,
        // check byte, host-view X14 access) relocated here, ending in B
        // return_pos. lean_window_pos is the guard CBNZ, bound to V when V is
        // laid out; it is also how the fault handler finds V when the
        // enforcing view refuses the access, and the word it rewrites to B V
        // (linear_fault.cpp, lean_site).
        bool  lean_variant = false;
        bool  lean_window_is_locator = false;
        usize lean_window_pos = static_cast<usize>(-1);
        // XMM register cache entries live at the access (m_xmm_cache), minus
        // the stub's own vector destination: a stub that calls a helper
        // reloads them from CpuState before it rejoins the fast path.
        u16   xmm_cache_live = 0;
    };
    std::vector<ColdMemStub> m_cold_mem_stubs;

    // 32-bit linear store slow path. Only a store that lands on a compiled
    // code page reaches the helper; every other probe branch already targets
    // the inline store, so this stub needs just the page branches and a merge
    // label. Emitting the marshal/BLR/raise-check in the block tail instead of
    // between the probe and the inline store removes ~21 words from the hot
    // stream of every 32-bit guest store, plus the `B .inline` that used to
    // jump over it.
    /// A flag record the Tier 1 recovery pass elided, resolved to ARM
    /// registers, to be republished inside a cold store stub.
    ///
    /// Only a HARDWARE fault rebuilds an elided record from the recovery map.
    /// A store that misses its check byte takes a cold stub and calls the
    /// write helper, and a write the helper refuses raises in SOFTWARE — a
    /// path the map never reaches. Republishing here closes that, and costs
    /// nothing on the hot path because the stub is cold.
    ///
    /// This is the same idea as `m_pending_stub_store` on the stencil path,
    /// which replays a skipped record on a Jcc's taken edge.
    struct StubFlagReplay {
        bool active = false;
        /// `CpuState::FlagOp` byte of the elided record.
        u8   flag_op = 0;
        /// The result was discarded (CMP/TEST), so `dst` holds the FIRST x86
        /// operand and the record's `flag_dst` is recomputed as `dst OP src`.
        bool recompute = false;
        /// ARM register holding the operand, or 0xFF to use the immediate.
        u8   src_reg = 0xFF;
        u8   dst_reg = 0xFF;
        u32  src_imm = 0;
        u32  dst_imm = 0;
    };

    struct ColdLinearStoreStub {
        usize page_branch_pos = 0;
        usize last_page_branch_pos = static_cast<usize>(-1);
        usize return_pos = 0;   // merge label: after the inline store
        u32   helper_off = 0;
        int   width = 0;
        /// The check-byte fast path is NZCV-clean, so the guest flags are
        /// saved and restored HERE rather than around every inline store.
        bool  spill_nzcv = false;
        /// Emitted by emit_linear_store_checked(), so the stub knows the byte
        /// in W5 is the per-host-page check byte and can serve the
        /// write-watch-only case without a helper call.
        bool  checked = false;
        /// A Tier 1 flag record this store's guest instruction elided, to be
        /// republished before the write helper can raise. Inactive normally.
        StubFlagReplay replay{};
        /// Whether X15 (`REG_ESP_FRAME`) held `linear_base + ESP` at the point
        /// this store was emitted. The stub's helper call clobbers X15, so if
        /// it did, the stub re-establishes it before rejoining the hot path.
        bool esp_frame_live = false;
    };
    std::vector<ColdLinearStoreStub> m_cold_linear_store_stubs;

    // Narrow first consumer of block-entry reservation proofs. A block made
    // solely of native MOV r{32,64},[rsp+disp] loads can validate its complete
    // address interval and mixed-protection state once at entry, then hold a
    // GuestMemory lease while omitting both repeated guards. Failure branches
    // share one zero-instruction deopt stub.
    bool m_stack_entry_load_proof{false};
    std::vector<usize> m_stack_proof_branch_pos;
    std::vector<usize> m_stack_proof_mixed_branch_pos;

    // Shared per-block `pending_raise` exit stub. Every helper call site emits
    // LDRB W16,[X19,#active] + CBNZ and records the CBNZ's index here;
    // emit_flush_raise_exit_stub() emits the ~13-word body ONCE and patches
    // all of them to it. Sound because the body depends on nothing from the
    // call site: it reads only X19, X22 and the pins, and never touches RIP.
    // The originating exit or checked-memory cold stub already published it.
    std::vector<usize> m_raise_branch_pos;
    // Pending-raise edges whose authoritative guest NZCV is in [SP,#8],
    // bucketed by the kind byte the restore prefix must publish. Bucket zero
    // preserves an already-published runtime carried kind.
    std::array<std::vector<usize>, 5> m_raise_restore_nzcv_branch_pos;
    // Unconditional branches from native exception stubs to that same body.
    std::vector<usize> m_direct_raise_branch_pos;

    // Native x64 UD2 sites publish their own precise RIP, then branch to one
    // per-block cold body which materializes lazy flags and raises #UD.  This
    // removes the interpreter delegate (and its retained decoded IR) without
    // duplicating the cold exception machinery at every guarded trap island.
    struct ColdUd2Exit {
        usize branch_pos;
        GuestAddr rip;
    };
    std::vector<ColdUd2Exit> m_cold_ud2_exits;

    // Out-of-line #GP alignment checks for the aligned vector memory forms
    // (MOVDQA/MOVAPS/MOVAPD and their VEX.256 forms). The hot stream keeps
    // two words, UBFX of the low bits and a CBNZ to the stub; the stub calls
    // jit_require_align16/32, which raises, so the hot path never pays the
    // helper call it used to make on EVERY aligned access.
    struct ColdAlignCheck {
        usize branch_pos;    // CBNZ placeholder in the hot stream
        usize return_pos;    // word after the CBNZ (resume when not raising)
        u8    ea_reg;        // X register holding the EA at the branch
        bool  align32;       // 32-byte (VEX.256) rather than 16-byte
        u16   xmm_cache_live = 0;  // reloaded if the helper ever returns
    };
    std::vector<ColdAlignCheck> m_cold_align_checks;
    /// Hot-stream half: UBFX + CBNZ placeholder; the stub is flushed with the
    /// other cold code.
    void emit_align_check(u8 ea_reg, bool align32);

    /// Emit the 16-byte alignment check for `op` IF the architecture requires
    /// one, and nothing otherwise. Safe to call after any `emit_compute_ea`.
    ///
    /// Self-guarding on purpose: the rule is a property of the OPERAND, not of
    /// the opcode, so encoding it once here beats repeating three conditions at
    /// seventy-six lowerings and getting one of them wrong -- which is exactly
    /// how a bogus #GP on VPXOR ships and terminates a launcher at startup.
    void emit_m128_align_check_if_required(u8 ea_reg,
                                           const DecodedInstruction& instr,
                                           const Operand& op);
    void emit_flush_cold_align_checks();

