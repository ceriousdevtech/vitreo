// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_shared_stubs.hpp
/// @brief Shared and process-wide stubs: lazy cold memory stubs, stub registry, continue/API-call exits, link probes, dispatch tails.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_shared_stubs.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    // ---- Lazy cold memory stubs (jit_cold_lazy.cpp) ----
    //
    // An eligible linear64 trampoline site (shared helper ABI, every piece a
    // pure function of the site) gets no slow path at compile time. Its hot
    // branches target a one-word slot `BL lazy_entry` followed by a
    // kColdLazyRecordWords record; the block's lazy entry LDRs
    // JitContext::cold_lazy_entry and BRs to the host trampoline with X30 at
    // the record. The first take emits the eager stub -- the same words,
    // variants included -- into its chunk's far-branch island, publishes it
    // as an interval of the owning instruction, and CASes the slot to
    // `B stub` (the lean variant for a JIT-view lean site). Enabled only by
    // VITREO_JIT_COLD_LAZY=1.
    static constexpr u32 kColdLazyRecordWords = ColdLazyRecord::kWords;
public:
    /// Whether this process emits lazy cold memory stubs.
    [[nodiscard]] static bool cold_lazy_enabled() noexcept;
    /// TEST-ONLY: force lazy emission on (1) or off (0); -1 = environment.
    static void set_cold_lazy_for_test(int value) noexcept;
private:
    struct ColdLazySite {
        usize slot_pos;     ///< the `BL lazy_entry` word
        usize return_pos;   ///< hot merge label
        usize mixed_pos;    ///< hot check-byte branch, or npos
        usize guard_pos;    ///< a word of the owning instruction's interval
        u8 raise_kind;      ///< restore-entry kind, or kColdLazyPlainRaise
        u32 packed;         ///< record word 2
        u32 config;         ///< record word 3
    };
    static constexpr u8 kColdLazyPlainRaise = 0xFF;
    std::vector<ColdLazySite> m_cold_lazy_sites;
    /// Raise-exit entries a lazy site's materialized stub branches to: the
    /// plain shared entry, and each restore-NZCV entry by kind.
    bool m_raise_lazy_plain{false};
    std::array<bool, 5> m_raise_lazy_restore{};
    usize m_raise_entry_plain_pos{static_cast<usize>(-1)};
    std::array<usize, 5> m_raise_entry_restore_pos{};
    /// Set while a lazy stub is emitted outside any compile
    /// (cold_lazy_emit): owning_insn() answers for the one site, and the
    /// hot-code edits of the flush are skipped.
    bool m_cold_lazy_late{false};
    /// This compile emits every slow path eagerly although lazy sites are
    /// enabled: an instruction in the block once failed to materialize its
    /// lazy stub (its chunk's island was full). Part of the memo key.
    bool m_cold_eager_variant{false};
    /// Instructions whose lazy stub failed to materialize on this thread;
    /// the blocks holding them are withdrawn at the next dispatch so they
    /// recompile as m_cold_eager_variant instead of side-exiting each take.
    std::vector<GuestAddr> m_cold_eager_pending;
    /// Process-wide: @p rip's lazy stub could not be materialized.
    static void note_cold_eager_rip(GuestAddr rip);
    /// Process-wide: a block [start, start + length) holds such a rip.
    [[nodiscard]] static bool cold_eager_requested(GuestAddr start, u32 length);
    void service_cold_eager_pending();
    /// Emit the eager slow path of the lazy site whose record is @p record
    /// as if placed at @p at; false if it cannot be (a reach, a missing
    /// helper). @p entry is the slot's resume offset (words) in @p out.
    [[nodiscard]] bool cold_lazy_emit(const u32* record, uintptr_t at,
                                      GuestAddr rip, std::vector<u32>& out,
                                      usize& entry);
    /// Write each lazy site's record after placement. @p split_bytes and
    /// @p cold_delta_bytes as for split_placed_offset.
    void cold_lazy_write_records(u8* base, u32 split_bytes,
                                 u32 cold_delta_bytes);
    /// Interpret the instruction a failed lazy take left at (see
    /// JitContext::cold_side_exit). Called by execute() after the exit.
    void finish_cold_side_exit(CpuState& cpu);
public:
    /// The trampoline's C++ half (jit_cold_lazy.cpp). @p record is X30 at
    /// entry; @p frame the trampoline's register save area. Returns the
    /// resume address.
    uintptr_t cold_lazy_take(const u32* record, u64* frame);
private:
    /// m_jit_ctx joins the process-wide registry for the compiler's lifetime:
    /// every process-wide stub compiled code loads through JitContext
    /// (capture_cf_stub, raise_exit, continue_exit, the linear64 helper
    /// slots) is copied into it at registration and by each publisher, so
    /// code this thread did not compile -- adopted, or reached through a
    /// link another thread patched -- never loads a null stub here.
    void register_live_jit_context();
    void unregister_live_jit_context() noexcept;
    /// execute() entry: re-check this context against the registry's
    /// generation (one acquire load while nothing new was published).
    void sync_process_wide_stubs();
    void* materialize_linear64_hot_helper(bool is_store, int width);
    void* materialize_process_raise_exit();
    /// The process-wide carry-capture stub (JitContext::capture_cf_stub),
    /// built on first use and published to this compiler's context; null if
    /// the helper page is full.
    void* materialize_capture_cf_stub();
    void emit_flush_cold_linear_store_stubs();
    void emit_flush_cold_ud2_stub();
    void emit_flush_raise_exit_stub();
    void emit_flush_stack_proof_exit_stub();
    /// The byte displacement of a deferred branch patch from word `from` to
    /// word `to`, checked against the branch's imm`imm_bits` field (26 for
    /// B/BL, 19 for B.cond/CBZ/CBNZ, 14 for TBZ/TBNZ). A displacement past the
    /// field marks the block undeliverable (finalize_code then declines it to
    /// the interpreter, with one log line) and returns 0, so the placeholder
    /// still encodes; the block is never published.
    i32 cold_branch_disp(usize from, usize to, unsigned imm_bits,
                         const char* what);

    /// Emit `B .continue_exit` (one word) in place of a private copy of the
    /// persist-NZCV + epilogue tail, and record it for patching.
    /// `spilled` selects the [SP,#8] persist variant over the live-PSTATE one.
    void emit_shared_continue_exit(bool spilled);

    /// L3: the continue exit of an API-call tail, with RIP and the loop
    /// counter already published and the predecessor's NZCV live in PSTATE.
    /// When inchain_thunks_codegen() is on it branches to the block's shared
    /// in-chain thunk exit: a target the registry's slot map marks in-chain
    /// is offered to the runtime from translated code; anything else (and
    /// every declined call) leaves exactly as emit_shared_continue_exit(false)
    /// does. Off, it IS emit_shared_continue_exit(false), byte for byte.
    void emit_shared_api_call_exit();
    /// The two parts of that exit (see the .cpp): the hot slot test, which
    /// reports where its B to the cold part and its .plain exit are, and the
    /// cold call body, which returns to @p plain on a hop-cap exit.
    void emit_api_call_exit_hot(usize& to_cold, usize& plain);
    /// The slot test alone, ending at .plain (nothing emitted there).
    void emit_api_test_inline(usize& to_cold, usize& plain);
    void emit_api_call_exit_cold(usize plain);
    /// Patch the B at @p from to reach @p to (imm26). Past that reach the
    /// block is declined to the interpreter (cold_branch_disp).
    void patch_api_call_b(usize from, usize to);

    /// Body of one shared linked-tail stub: NZCV-kind store, L1 probe on the
    /// target in REG_SCRATCH0, optional backward-edge hop budget, BR on hit,
    /// shared continue-exit on miss.
    void emit_cont_tail_stub_body(NzcvState carry, bool backward);

    /// Process-wide linked-tail probes (JitContext::link_probe). A site sets
    /// X0 = guest target and BLRs one: the probe RETs the target's linked
    /// entry in X16 on a hit, and on a miss publishes RIP (X0) and the loop
    /// counter and RETs X16 = 0. Flag-neutral.
    enum LinkProbe : u8 {
        kProbeL1 = 0,         ///< L1 probe, no hop budget (forward edge)
        kProbeL1Counted = 1,  ///< L1 probe with the counted hop budget
        kProbeRet = 2,        ///< sticky ring, software RSB, then counted L1
        kLinkProbeKinds = 3,
    };
    /// On only under VITREO_JIT_LINK_PROBE_SHARED=1 (or the test override),
    /// never with exit statistics, which keep the per-site probe copies and
    /// their per-cause landing pads.
    [[nodiscard]] static bool shared_link_probe_enabled() noexcept;
    /// On only under VITREO_JIT_RSB_PUSH_SHARED=1 (or the test override).
    [[nodiscard]] static bool shared_rsb_push_enabled() noexcept;
    void emit_link_probe_body(u8 kind);
    void emit_rsb_push_body();
    /// Built on first use and published to every live context; null if the
    /// helper page is full (the caller then emits the inline copy).
    void* materialize_link_probe(u8 kind);
    void* materialize_rsb_push();
    /// LDR X16, [X20, #link_probe[kind]]; BLR X16; CBZ X16, .miss; BR X16;
    /// .miss: with X0 already holding the target. False, emitting nothing,
    /// when the stub is unavailable.
    bool emit_shared_link_probe(u8 kind);

    /// Process-wide dispatcher tails (JitContext::dispatch_tail) for an x64
    /// linear64 block whose pins are exactly kStdPins64: everything after a
    /// block's last block-specific word -- NZCV persist, the eight pin
    /// stores and the Continue epilogue, or the pin stores before the
    /// process raise exit. Entered by BR from the block's shared exit body.
    enum DispatchTail : u8 {
        kTailContinueLive = 0,     ///< emit_persist_live_nzcv + epilogue
        kTailContinueSpilled = 1,  ///< emit_persist_spilled_nzcv + epilogue
        kTailRaise = 2,            ///< pin stores + JitContext::raise_exit
        /// The in-chain API exit's slot test (any pin layout). Entered with
        /// X30 = .plain; returns to X30 when the target is not an in-chain
        /// export and to X30 + 4 when it is.
        kTailApiTest = 3,
        /// Dispatcher-entry prologue body after the block's own LR push:
        /// the eight pin loads, X14 and X15 (check map / JIT view). BLR'd.
        kEntryCheckMap = 4,
        kEntryJitView = 5,
        /// Cold-entry counter (jit_compiler_cold_residency.hpp). BLR'd with
        /// X30 at the site's data word; returns past it.
        kColdCounter = 6,
        /// IR tier counter (jit_compiler_cold_residency.hpp). BLR'd with
        /// X30 at the site's data word; returns past it.
        kTierCounter = 7,
        kDispatchTails = 8,
    };
    /// Off under VITREO_JIT_PROLOGUE_LEGACY=1.
    [[nodiscard]] static bool shared_prologue_enabled() noexcept;
    /// x64 linear64 block whose pins are exactly kStdPins64.
    [[nodiscard]] bool std_pin_block() const noexcept;
    void emit_api_test_body();
    static constexpr u8 kStdPins64[8] = {23, 24, 25, 26, 12, 13, 27, 28};
    /// Off under VITREO_JIT_DISPATCH_TAIL_LEGACY=1.
    [[nodiscard]] static bool shared_dispatch_tail_enabled() noexcept;
    [[nodiscard]] bool dispatch_tail_applies() const noexcept;
    void* materialize_dispatch_tail(u8 kind);
    /// LDR X16, [X20, #dispatch_tail[kind]]; BR X16. False, emitting
    /// nothing, when the block does not qualify or the stub is unavailable.
    bool emit_dispatch_tail(u8 kind);

    /// Emit every distinct linked-tail stub that has sites and no body yet,
    /// and patch its sites. Called twice: right after the instruction loop, so
    /// the stubs every block exit runs through sit next to the hot body
    /// rather than past the cold memory stubs, and again after the cold Jcc
    /// exits, whose tails may name a new stub or reuse an emitted one. Runs
    /// before emit_flush_continue_exit_stubs(), whose bodies these stubs
    /// branch to.
    void emit_flush_cont_tail_stubs();

    /// Emit the (at most two) shared continue-exit bodies and patch every
    /// `emit_shared_continue_exit` branch to them. Must run after every other
    /// flush, because those flushes emit tails of their own.
    void emit_flush_continue_exit_stubs();

