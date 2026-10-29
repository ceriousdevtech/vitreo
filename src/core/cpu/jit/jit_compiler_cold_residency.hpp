// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_cold_residency.hpp
/// @brief Cold-path residency: cold-entry counters, the one-shot residency
///        recompile they request, and retargeting a mid-block Jcc's B.cond
///        straight to its linked successor (jit_cold_residency.cpp,
///        jit_direct_link.cpp).
///
/// A split block (JitConfig::split_cold) moves everything after
/// m_hot_end_pos out of line. Which pieces are cold is a compile-time guess;
/// a deferred cold arm or memory slow stub the guest takes on most
/// executions is hot code paying a far branch and a cold cache line. Each
/// counted piece's entry calls the process kColdCounter dispatcher tail,
/// which bumps the piece's counter in a run allocated per translation. Every
/// kColdEntryThreshold entries of one piece the tail stores the run into
/// JitContext::cold_residency_request; the dispatcher (service) records a
/// profile for that block identity, withdraws only this thread's cache entry
/// and its incoming edges, and the next dispatch compiles the residency
/// variant (CompiledKey::cold_residency) with the hot pieces before
/// m_hot_end_pos and no counters. A profile exists at most once per
/// (start, code bytes): one recompile per block per code epoch. Nothing
/// global is invalidated and the old translation stays valid for every
/// thread still running it.
///
/// A stack-entry-proof block that deopts (JitExecResult::Unsupported)
/// kStackProofDeoptThreshold times on one thread gets a profile that drops
/// the proof, through the same withdrawal.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_cold_residency.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

public:
    /// Off under VITREO_JIT_COLD_RESIDENCY=0 (counters, recompile and
    /// retarget). VITREO_JIT_JCC_RETARGET=0 turns off the retarget alone.
    [[nodiscard]] static bool cold_residency_enabled() noexcept;
    [[nodiscard]] static bool jcc_retarget_enabled() noexcept;
    /// Tests: 0/1 force, -1 returns to the environment's choice.
    static void set_cold_residency_for_test(int on) noexcept;
    static void set_jcc_retarget_for_test(int on) noexcept;
    /// Tests: forget every process-wide residency profile and this
    /// compiler's per-thread withdrawal and deopt records.
    void reset_cold_residency_for_test();

    /// Power of two: the counter tail tests `count & (threshold - 1)`.
    static constexpr u32 kColdEntryThreshold = 1024;
    /// A piece entered this often by the time its block's request is
    /// serviced is placed hot in the residency variant.
    static constexpr u32 kColdHotPieceEntries = kColdEntryThreshold / 8;
    static constexpr u32 kStackProofDeoptThreshold = 64;
    /// Counted pieces per block: deferred arms first, then memory stubs.
    static constexpr u32 kMaxColdPieces = 64;

    /// Dispatcher entry, before the block lookup: act on a pending counter
    /// or deopt request. One load and compare when there is none.
    void service_cold_residency() {
        if (m_jit_ctx.cold_residency_request != 0 || m_jit_ctx.tier_request_count != 0 ||
            m_proof_request_rip != 0) {
            service_cold_residency_slow();
        }
        if (!m_cold_eager_pending.empty()) service_cold_eager_pending();
    }

    struct ColdResidencyStats {
        u64 counter_runs{0};        ///< translations with bound counters
        u64 counter_sites{0};       ///< counted piece entries emitted
        u64 requests{0};            ///< threshold crossings serviced
        u64 profiles{0};            ///< residency profiles created
        u64 withdrawals{0};         ///< cache entries withdrawn
        u64 variant_compiles{0};    ///< fresh residency-variant emissions
        u64 variant_split_moves{0}; ///< variants whose hot end moved
        u64 proof_drops{0};         ///< profiles that drop the stack proof
        u64 retarget_candidates{0}; ///< B.cond words eligible to retarget
        u64 retargets{0};           ///< B.cond words patched to a successor
        u64 retarget_undos{0};      ///< B.cond words restored
    };
    [[nodiscard]] static ColdResidencyStats cold_residency_stats() noexcept;
    /// The first word of every bound counter site, `LDR X30, [X20, #tail]`.
    [[nodiscard]] static u32 cold_counter_site_word() noexcept;
    /// Tests: the current count of the piece a bound site's data word
    /// (the third word of the site) names; 0 for an unbound word.
    [[nodiscard]] static u32 cold_counter_value_for_test(u32 data_word) noexcept;
    /// Tests: what an SMC invalidation does to every direct edge.
    static void bump_link_generation_for_test() noexcept { bump_link_generation(); }
    /// Tests: what a guest write to [addr, addr+size) does to direct edges,
    /// as the writing thread's observer runs it (before the write event is
    /// published to any other thread).
    static void unlink_direct_links_for_test(GuestAddr addr, usize size) noexcept {
        unlink_direct_links_overlapping(addr, size);
    }
    static void undo_jcc_retargets_for_test(const void* target_code) noexcept {
        undo_jcc_retargets(target_code);
    }

    // ---- IR tier-up (jit_ir_tier.cpp) -------------------------------------
    // A block the Tier-1 gate admits (jit_ir_gate.cpp) is first compiled
    // with stencils only (tier 0) plus one tier counter site right after
    // m_body_start_pos, so every entry counts: dispatcher, linked hop, its
    // own back-edge link and every in-block loop-back. The site decrements
    // the translation's slot counter inline (kTierSiteWords words, flag-
    // neutral, one not-taken TBZ); the entry that takes it negative calls the
    // process kTierCounter tail, which appends the slot to this thread's
    // JitContext::tier_requests ring and parks the counter (a full ring
    // re-arms it for kTierRetryEntries instead). The next dispatcher entry
    // (service_cold_residency) services the whole ring: per slot it marks
    // the identity (start, code hash) hot process-wide, turns the block's
    // cold-entry counts into a residency profile, and withdraws this
    // thread's translation exactly as a residency request does, then zeroes
    // the counter so another thread still running it asks on its next
    // entry. The next dispatch compiles tier 1: the IR, the residency
    // variant when a profile exists, and no counters. A cold-counter request
    // on a tier-0 block marks it hot too, so a hot block compiles at most
    // twice.
    //
    // If the IR declines a tier-1 compile that is not a residency variant,
    // the compile is abandoned before emitting anything, the identity is
    // recorded as declined, and the retry is tier 0 again -- UNCOUNTED, keyed
    // apart (kIrKeyUncounted), so it neither adopts the counted translation
    // nor is adopted in its place by a counted compile. A thread still running
    // the counted translation asks on its next entry (the counter was zeroed);
    // its request for a declined identity withdraws that translation and
    // leaves the counter at zero, so the next dispatch swaps it for the
    // uncounted one. A declined identity compiles straight to an uncounted
    // tier 0. Past the counter-slot cap, tier 0 is uncounted too (its site
    // branched over), never IR-on-first-compile.

    /// VITREO_JIT_IR_TIER_THRESHOLD (default kDefaultIrTierThreshold);
    /// 0 or 1 compiles the IR on first compile (no tier 0).
    [[nodiscard]] static u32 ir_tier_threshold() noexcept;
    /// A tier-up recompile costs about what ~1000 tier-0 entries lose to
    /// tier 1 (tools/jit_bench cold set): 64-256 slow blocks that run a few
    /// hundred times by up to 1.5x, 4096 leaves hot code at tier 0 too long.
    /// Independent of kColdEntryThreshold: the tier-up residency profile
    /// scales its hot ratio to the entries counted.
    static constexpr u32 kDefaultIrTierThreshold = 512;
    static constexpr u32 kTierSiteWords = 11;
    /// Entries a site waits before asking again when its thread's request
    /// ring was full. One: a longer window re-arms every overflowing block
    /// in step, so each window services only one ring's worth (measured on
    /// tools/jit_bench cold_blocks.S); a dispatcher entry drains the ring
    /// within kMaxLinkedHops counted hops anyway.
    static constexpr u32 kTierRetryEntries = 1;
    static constexpr u32 kMaxIrTierThreshold = (1u << 24) - 1;
    /// Bits above the packed IR policy in memo / store keys: tiering
    /// applies, and the emission is tier 0. A key with neither is a compile
    /// without tiering (IR on first compile, counters as before).
    static constexpr u32 kIrKeyTiered = 1u << 31;
    static constexpr u32 kIrKeyTier0 = 1u << 30;
    /// A tier-0 emission whose counter site is branched over (an IR-declined
    /// identity, or past the slot cap). Keyed apart from the counted tier-0
    /// record so a declined identity's retry compiles (or adopts) code that
    /// no longer touches the shared counter slot: every entry of a counted
    /// translation is a plain load/store of that slot, and threads running
    /// the same hot block made it ping-pong between cores.
    static constexpr u32 kIrKeyUncounted = 1u << 29;
    /// Tests: force a threshold; -1 returns to the environment's choice.
    static void set_ir_tier_threshold_for_test(int threshold) noexcept;
    /// Tests: the last compile's per-word EmitAttribution categories (empty
    /// unless VITREO_JIT_EMIT_ATTRIBUTION was set). Index i tags word i of
    /// the emission buffer, before split placement.
    [[nodiscard]] const std::vector<u8>& emit_attribution_tags_for_test() const noexcept {
        return m_attr_tags;
    }
    /// Tests: forget every identity marked hot or IR-declined.
    static void reset_ir_tier_for_test();
    /// Tests: cap bound counter slots at `limit` (0: the built-in cap).
    static void set_tier_slot_limit_for_test(usize limit) noexcept;
    [[nodiscard]] static usize tier_slots_used_for_test() noexcept;
    /// The request-tail call word of every tier counter site (its ninth),
    /// `LDR X30, [X20, #tail]`; BLR X30 and the slot index follow it.
    [[nodiscard]] static u32 tier_counter_site_word() noexcept;
    /// JitContext::tier_counters for every context; null until the tier
    /// counter region is mapped.
    [[nodiscard]] static u32* tier_counter_base() noexcept;
    /// Tests: the entry count of the slot a bound site's data word names.
    [[nodiscard]] static u32 tier_counter_value_for_test(u32 data_word) noexcept;
    /// The last compile was tier 0 (stencils, counted or not).
    [[nodiscard]] bool last_compile_was_tier0() const noexcept { return m_tier_stencil; }

    struct IrTierStats {
        u64 tier0_compiles{0};  ///< stencil compiles carrying a tier counter
        u64 uncounted{0};       ///< tier-0 compiles without one (cap, declined)
        u64 tier1_compiles{0};  ///< fresh compiles of a hot identity
        u64 requests{0};        ///< tier requests serviced
        u64 promotions{0};      ///< identities marked hot
        u64 withdrawals{0};     ///< cache entries withdrawn for tier-up
        u64 store_tier1{0};     ///< tier-1 records adopted on first compile
        u64 declines{0};        ///< identities whose tier-1 IR declined
        u64 parked{0};          ///< counters of declined identities parked
    };
    [[nodiscard]] static IrTierStats ir_tier_stats() noexcept;

private:
    void service_cold_residency_slow();
    /// Withdraw this thread's compiled code for `start` and fail closed its
    /// incoming direct edges, once per thread per block identity.
    void withdraw_for_residency(GuestAddr start);
    /// The withdrawal itself; false when already withdrawn or not cached.
    bool withdraw_block(GuestAddr start, u64 identity);
    /// withdraw_block without its once-per-identity filter.
    bool withdraw_block_now(GuestAddr start);
    /// Choose tier 0, tier 1 or no tiering for the block about to be
    /// emitted (compile_attempt, after the code hash, before the memo key).
    void select_ir_tier(const BasicBlock& block, u64 code_hash, bool have_hash);
    /// The ir_policy word the memo and translation-store keys carry: the
    /// policy plus the tier this compile emits.
    [[nodiscard]] u32 ir_key_policy() const noexcept;
    /// This compile becomes tier 1 (a stored tier-1 record was found).
    void promote_compile_to_tier1(GuestAddr start, u64 code_hash);
    /// After the IR attempt of a tier-1 compile: if the IR declined (and the
    /// compile is not a residency variant), record the identity declined.
    /// True when the caller should abandon this compile and retry it as
    /// tier 0 (`retry_possible`: the memo may hold the tier-0 translation).
    bool decline_tier1_compile(GuestAddr start, u64 code_hash, bool retry_possible);
    /// Tier counter site: kTierSiteWords words, X30 clobbered, flags and
    /// every other register preserved. Emitted only by a tier-0 compile.
    void emit_tier_entry_counter();
    void emit_tier_counter_tail_body();
    /// Bind (or neutralize) every tier counter site in m_code_buf.
    void bind_tier_counter(GuestAddr start);
    /// A ring entry (tier slot index): promote and withdraw, or keep a
    /// declined identity parked. False when it names no slot.
    bool service_tier_request(u64 request);
    /// Pieces of cold-counter run `run` entered at least 1/8 as often as
    /// their block's `entries`.
    [[nodiscard]] static u64 cold_run_hot_mask(u32 run, u32 entries) noexcept;
    static void note_residency_profile(GuestAddr start, u64 code_hash, u64 hot_mask);
    void note_tier1_compile() noexcept;
    static void mark_tier_hot(GuestAddr start, u64 code_hash);
    [[nodiscard]] static bool tier_hot(GuestAddr start, u64 code_hash);
    static void mark_tier_declined(GuestAddr start, u64 code_hash);
    [[nodiscard]] static bool tier_declined(GuestAddr start, u64 code_hash);
    /// Tiering applies to this compile (gate open, threshold > 1, hash).
    bool m_tier_active{false};
    /// This compile is tier 0.
    bool m_tier_stencil{false};
    /// This tier-0 compile binds a counter slot.
    bool m_tier_counted{false};
    /// compile(): the attempt was abandoned by decline_tier1_compile.
    bool m_retry_tier0{false};
    /// Run unit of the cold counters bind_cold_counters bound (0: none).
    u32 m_bound_cold_run{0};
    void note_stack_proof_deopt(GuestAddr rip);
    /// Choose counters or the residency variant for the block about to be
    /// emitted (compile_attempt, after the code hash).
    void select_cold_residency(GuestAddr start, u64 code_hash, bool have_hash);
    /// A counted piece's entry: three words, X30 clobbered, flags and every
    /// other register preserved. `ordinal` is the piece's index.
    void emit_cold_entry_counter(u32 ordinal);
    void emit_cold_counter_tail_body();
    /// Bind (or, with residency off, neutralize) every counter site in
    /// m_code_buf to a fresh counter run. finalize_code, before placement.
    void bind_cold_counters(GuestAddr start);
    [[nodiscard]] bool residency_piece_hot(u32 ordinal) const noexcept {
        return ordinal < 64 && ((m_residency_hot_mask >> ordinal) & 1u) != 0;
    }
    /// Residency variant: the deferred arms and memory stubs the profile
    /// found hot, flushed before the cold region (emit_flush_cold_jcc_exits).
    void emit_flush_residency_hot_pieces();

    /// Record every hot-part B.cond / CBZ / CBNZ of a just-placed block that
    /// branches to a forward direct-link slot's guard with nothing before it.
    void note_jcc_retarget_candidates(u8* mem, u32 hot_bytes,
                                      u64 chunk_generation);
    /// After a slot at `gen_site` was published for `gen`: patch its
    /// recorded B.cond words to `entry` of `target`.
    static void retarget_jcc_into(u32* gen_site, const void* target_code,
                                  void* entry, u32 gen) noexcept;
    /// Restore retargeted B.cond words: those into `target_code`, or every
    /// one when null.
    static void undo_jcc_retargets(const void* target_code) noexcept;
    static void jcc_retarget_stats(ColdResidencyStats& out) noexcept;

    /// For an emitter whose exits all leave with no live NZCV claim (the
    /// IR tier): before the branch of a mid-block exit to `target` that
    /// will carry a forward slot, clear CpuState::nzcv_kind once in the hot
    /// body. The kind only enables a fast path -- the lazy record stays
    /// authoritative -- so clearing it early never changes guest state, and
    /// every later exit can then leave Carried: no kind store before its
    /// slot guard, so its branch is a retarget candidate.
    void emit_mid_exit_kind_clear(u64 target);
    /// The carry an exit to `target` may claim instead of `carry`.
    [[nodiscard]] NzcvState exit_carry_after_kind_clear(u64 target,
                                                         NzcvState carry) const noexcept;
    bool m_exit_kind_cleared{false};

    bool m_residency_counters{false};
    bool m_residency_variant{false};
    bool m_residency_drop_proof{false};
    u64 m_residency_hot_mask{0};
    /// Ordinal base of the block's memory stubs (its deferred-arm count).
    u32 m_cold_mem_ordinal_base{0};
    GuestAddr m_proof_request_rip{0};
    std::unordered_map<GuestAddr, u32> m_proof_deopts;
    std::unordered_set<GuestAddr> m_residency_withdrawn;
