// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_linking.hpp
/// @brief Block linking policy: direct-link edge classes, shared-table adoption, link enable and generation, return predictors.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: public). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_linking.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

public:



    /// Drop all cached compile records (and the process-wide SMC demotion
    /// set). Useful for arena reset paths and test isolation.
    /// Constant-successor edge classes eligible for a patchable direct-link
    /// slot. Enabling every class at once regressed the UI from ~1000
    /// presented frames to ZERO (reverted in 5be713ad) while process, window
    /// and draw counts all stayed healthy, and the cause was never isolated.
    /// The classes are therefore re-landed and measured ONE AT A TIME, with
    /// tools/jit_bench/analyze_app_ab_logs.py's presents column as the gate.
    ///
    /// `VITREO_JIT_LINK_EDGES` is a decimal bitmask of these bits. The default
    /// is every class except CallRel (2026-10-08), so 31 turns CALL rel links
    /// on; native CALL/RET (VITREO_JIT_NATIVE_CALLRET) needs them.
    ///
    /// The August regression reproduced as a launcher UI presenting
    /// ~30 s late with CallRel on, graded by how many call sites were linked:
    /// publishing a caller re-patched EVERY recorded site into each target,
    /// quadratic in a callee's caller count. try_patch_direct_link now resolves
    /// only the new source's sites and skips current ones; with that, CallRel
    /// starts the UI on time and runs the x64 call kernel 11-13% faster.
    ///
    /// Menu-frame losses first blamed on CallRel (-9.7%, -6.2%) compared two
    /// different binaries; one binary A/B'd by this mask alone
    /// (runs gated on swap growth, both run orders pooled) shows nothing like
    /// it. Two generic link defects turned up on the way and are fixed: a
    /// mid-block Jcc exit lost its slot to any terminal slot (see
    /// emit_flush_mid_jcc_exits), and a site beyond B's +-128 MiB reach was
    /// never patched -- most CALL rel sites in a 1.5 GB arena, which then ran
    /// their slot's guard AND the probe -- until JitCodeArena::alloc_far_branch.
    /// CallRel on vs off with the first fix only: -1.1% (95% CI -2.1..-0.0%).
    /// With both, on an idle machine (16 runs, both orders): -0.70% (CI
    /// -1.6..+0.2%), its two halves -1.35% and -0.05%. Not shown >= off, so
    /// the default stays off; the residual's mechanism is not identified.
    enum class LinkEdge : u32 {
        None     = 0,
        MidJcc   = 1u << 0,  ///< mid-block Jcc taken edge (cold-exit flush)
        TermJcc  = 1u << 1,  ///< terminal Jcc, taken leg
        TermFall = 1u << 2,  ///< terminal Jcc / block-end fall-through leg
        JmpRel   = 1u << 3,  ///< JMP rel8 / rel32
        CallRel  = 1u << 4,  ///< CALL rel32
    };

    /// Bitmask of the LinkEdge classes allowed to carry a slot. Read from the
    /// environment once; a class that is not set emits no slot and keeps the
    /// unchanged L1 probe tail, so the mask can only remove patched edges.
    static constexpr u32 kDefaultLinkEdges =
        static_cast<u32>(LinkEdge::MidJcc) |
        static_cast<u32>(LinkEdge::TermJcc) |
        static_cast<u32>(LinkEdge::TermFall) |
        static_cast<u32>(LinkEdge::JmpRel);

    [[nodiscard]] static u32 link_edge_mask() noexcept;

    /// Whether a BACKWARD constant edge may carry a direct-link patch slot.
    ///
    /// Orthogonal to the LinkEdge class mask: that mask says which SHAPES may
    /// be patched, this says whether the loop-closing direction may be. Kept
    /// separate because it is the one that carries the counted hop budget, so
    /// a regression should name the direction and not the shape.
    /// `VITREO_JIT_LINK_BACKWARD=0` disables it for bisection.
    [[nodiscard]] static bool backward_link_enabled() noexcept;
    [[nodiscard]] static bool link_edge_enabled(LinkEdge edge) noexcept {
        return (link_edge_mask() & static_cast<u32>(edge)) != 0;
    }

    /// Whether an L1 probe's way-1 hit runs its own copy of the way-0 hit
    /// suffix (hop budget, layout load, BR/RET) instead of branching back to
    /// it. Each copy is on its own path, so a probe then carries two of each.
    /// `VITREO_JIT_L1_WAY1_FALLTHROUGH=0` restores the branch.
    [[nodiscard]] static bool l1_way1_fallthrough_enabled() noexcept;

    /// Give this compiler's arena chunk back. `seal` marks the remainder
    /// unusable so the next block is forced into a different chunk, which is
    /// what the arena tests need in order to build a known layout.
    ///
    /// A forwarder, not a boundary leak: the cursor is this compiler's own
    /// state and the arena is where the chunk table lives.
    void release_arena_chunk(bool seal = false) noexcept {
        JitCodeArena::release_chunk(m_arena, seal);
    }




    /// Dispatcher cold path: the per-thread cache missed at `addr`. Probe the
    /// shared table by (address, mode, this compiler's emit policy) and, if a
    /// live record's byte hash still matches the guest bytes at `addr`, fill
    /// `out` with a per-thread BasicBlock that already carries the shared
    /// translation — no decode, no emit. The caller inserts `out` into its
    /// cache and then calls link_adopted_block() once it has decided the
    /// block may really execute natively.
    /// @return true when `out` was filled.
    bool try_adopt_shared_block(GuestAddr addr, bool is_64bit,
                                BasicBlock& out);

    /// Cold-path adoption gate. VITREO_JIT_SHARED_ADOPT=0 turns it off for
    /// bisection (the table is still consulted by compile(), exactly the
    /// pre-Phase-2 behaviour). Tests flip it to measure the two arms in one
    /// process: -1 = re-read the environment, 0 = off, 1 = on.
    static void set_shared_adoption_for_test(int policy) noexcept {
        shared_adoption_slot().store(policy, std::memory_order_relaxed);
    }
    [[nodiscard]] static bool shared_adoption_enabled() noexcept;

private:
    static std::atomic<int>& shared_adoption_slot() noexcept {
        static std::atomic<int> slot{-1};
        return slot;
    }

public:

    /// Second half of publish_compiled_block() for a block that arrived via
    /// try_adopt_shared_block(): wire direct-link patch sites to and from the
    /// adopted code. Separate because the dispatcher may still veto native
    /// execution of an inserted block (thunk region, PC trace) and a vetoed
    /// block must not become a link target.
    void link_adopted_block(BasicBlock& block);



    /// Stage B1: enable JIT-emitted inline block-link tails.
    /// Default OFF.  Tests must NOT enable this (they intentionally execute
    /// blocks at the same guest VA across cases for different code, which
    /// could chain into stale cached entries).  The runtime turns it on
    /// once after JIT init.
    static void enable_jit_link() noexcept {
        s_jit_link_enabled.store(true, std::memory_order_release);
    }
    static void disable_jit_link() noexcept {
        s_jit_link_enabled.store(false, std::memory_order_release);
    }
    static bool jit_link_enabled() noexcept {
        return s_jit_link_enabled.load(std::memory_order_acquire);
    }
    /// Process-wide direct-link generation, for tests that assert a
    /// retirement did NOT fall back to bumping it.
    [[nodiscard]] static u32 link_generation_for_test() noexcept {
        return s_link_generation.load(std::memory_order_acquire);
    }

private:
    static std::atomic<bool> s_jit_link_enabled;
    static std::atomic<u32> s_link_generation;

    /// Patch recorded direct-edge sites into `block`. With `only_source`,
    /// only that source block's own sites: a newly published source needs
    /// its edges resolved, not every other caller of the same target redone.
    void try_patch_direct_link(const CompiledBlock& block,
                               const void* only_source = nullptr);
    void remember_direct_link(const CompiledBlock& block);
    /// `reason` only labels the DIAGNOSTIC bump histogram
    /// (/tmp/vitreo-jit-link-stats): 0 thread exit, 1/4 lost write history,
    /// 2/5 write to retained-only code pages, 3/6 write invalidating cached
    /// blocks (1-3 in the writer's observer, 4-6 at a foreign sync).
    static void bump_link_generation(u32 reason = 7) noexcept;

    /// Guest bytes [addr, addr+size) were rewritten: fail closed exactly the
    /// patched direct-link sites whose TARGET translated any of those bytes
    /// (and the mid-block B.cond retargets into those targets), and advance
    /// the return-predictor epoch. Every other patched edge in the process
    /// keeps working.
    ///
    /// This is what an executable write used to do with bump_link_generation,
    /// which failed EVERY patched edge in the process closed: they then ran
    /// the L1 probe tail on every hop until their source or target happened
    /// to be republished. A JS engine writes code continuously, so in a CEF
    /// renderer that meant hardly any edge of the (never rewritten) browser
    /// engine code stayed linked, and the 16-bit generation horizon below
    /// was soon crossed, after which nothing was patched at all.
    ///
    /// `writer` is the writing thread's observer, which also records the
    /// unlink for patchers that have not consumed the write yet and advances
    /// the predictor epoch. A replay from another thread's sync (a foreign
    /// writer, or a site this thread patched before it saw the write) only
    /// fails the sites closed.
    static void unlink_direct_links_overlapping(GuestAddr addr, usize size,
                                                bool writer = true) noexcept;
    /// This compiler is going away and releases the target retains its
    /// sites rely on: fail exactly those sites (and their B.cond retargets)
    /// closed, where it used to bump the link generation for everyone.
    void unlink_own_direct_sites() noexcept;
    /// Sequence number of the last recorded unlink.
    [[nodiscard]] static u64 unlink_sequence() noexcept;
    /// Advanced by every bump_link_generation and every unlink. execute()
    /// drops this thread's return predictors (RSB + sticky rings) when it
    /// moves, exactly as it used to on every link-generation bump.
    static std::atomic<u32> s_predictor_epoch;
    /// s_unlink_seq as read when this thread last began consuming
    /// executable-write events: an unlink with a later sequence may concern
    /// a target this thread's cache still holds (see try_patch_direct_link).
    u64 m_unlink_seq_at_sync{0};
    /// This thread patched a direct-link site since its last
    /// synchronize_executable_writes, so that sync re-runs the unlink for
    /// every event it consumes (a write whose observer ran before the patch
    /// registered may only now be published).
    bool m_patched_since_sync{false};

    /// Drop every cached return prediction (software RSB + the 2-deep sticky
    /// rings). MUST be called anywhere `m_jit_ctx.block_cache_l1` is nulled:
    /// these structures are probed BEFORE the L1 base load in
    /// emit_linked_tail_reg, so nulling L1 alone does not stop a linked chain
    /// from BR-ing to an invalidated block's host pointer.
    void invalidate_return_predictors() noexcept {
        m_jit_ctx.rsb_top = 0;
        m_jit_ctx.ret_sticky_rip[0] = 0;
        m_jit_ctx.ret_sticky_rip[1] = 0;
        m_jit_ctx.ret_sticky_host[0] = nullptr;
        m_jit_ctx.ret_sticky_host[1] = nullptr;
        m_jit_ctx.ret_sticky_i = 0;
        m_jit_ctx.cont_sticky_rip[0] = 0;
        m_jit_ctx.cont_sticky_rip[1] = 0;
        m_jit_ctx.cont_sticky_host[0] = nullptr;
        m_jit_ctx.cont_sticky_host[1] = nullptr;
        m_jit_ctx.cont_sticky_i = 0;
    }

    /// Allocate executable memory from this compiler's chunk, taking a new
    /// chunk when it is full. `generation` receives the chunk generation the
    /// block must be published under.

