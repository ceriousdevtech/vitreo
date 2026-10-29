// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler_direct_links.hpp
/// @brief Block finalization, direct-link patch-site records, execute-time TLS slots and memory-cache revalidation.
///
/// NOT a standalone header: a slice of class JitCompiler's body, textually
/// included by jit_compiler.hpp inside the class definition (access on
/// entry: private). Include <core/cpu/jit/jit_compiler.hpp> instead.
#ifndef VITREO_JIT_COMPILER_CLASS_BODY
#error "jit_compiler_direct_links.hpp is part of JitCompiler's body; include <core/cpu/jit/jit_compiler.hpp>"
#endif

    /// Finalize code buffer into executable memory
    CompiledBlock* finalize_code(const BasicBlock& block,
                                 const JitCompileMemo::CompiledKey* compiled_key);

    /// Direct-edge metadata recorded while emitting linked tails — one entry
    /// PER constant forward edge, not one per block. Offsets are into the ARM
    /// stream, not host heap, so memo reuse stays safe. The original design
    /// allowed exactly one slot per block (the mid-block Jcc taken edge);
    /// every JMP rel / CALL rel / terminal-Jcc leg paid the 11-word L1 probe
    /// per hop instead of a patched B — measured as most of the gap to
    /// the host x86 runner on the control-flow kernels.
    std::vector<JitCompileMemo::DirectLinkMeta> m_emit_links;

    /// Emit one generation-guarded patchable direct-branch slot for
    /// @p target and record it in m_emit_links. Falls through (both on
    /// generation mismatch and while unpatched) to whatever is emitted next.
    /// No-op unless @p edge is enabled in link_edge_mask() and the edge is
    /// guest-FORWARD. Flag-neutral: the sequence is LDR/MOVZ/EOR/CBNZ
    /// plus the patched B, so a Carried edge's live PSTATE crosses it intact.
    void emit_direct_link_slot(u64 target, LinkEdge edge);

    /// Handoff of a block's recorded edges from compile() /
    /// try_adopt_shared_block() to link_published_block(), which consumes
    /// and erases the entry. Transient: holds at most the blocks between
    /// those two points, never one entry per live block.
    std::unordered_map<GuestAddr, std::vector<JitCompileMemo::DirectLinkMeta>> m_direct_links;
    /// The last handoff node link_published_block consumed, kept (with its
    /// vector's capacity) for the next adoption to refill: a thread over its
    /// cache capacity re-adopts a block per eviction, and each one used to
    /// malloc and free a node and an edge vector.
    decltype(m_direct_links)::node_type m_direct_links_spare;
    /// Copy target for the record try_adopt_shared_block reads under the memo
    /// lock. Reused so the copy keeps its edge vector's capacity.
    JitCompileMemo::CompiledRecord m_adopt_record;
    /// Hand @p links to m_direct_links[@p addr] (replacing any entry),
    /// leaving @p links holding spare capacity.
    void stage_direct_links(GuestAddr addr,
                            std::vector<JitCompileMemo::DirectLinkMeta>& links);

    /// One recorded direct-edge slot: a source block's patchable B waiting
    /// for (or already patched to) its target. 32 bytes, held per edge per
    /// thread for as long as the source code is live.
    struct DirectPatchSite {
        void* code{nullptr};
        u64 expected_pin_layout{0};
        // Chunk generation of `code`: the site is dead once its chunk is
        // retired, and must never be written again after that.
        u64 arena_generation{0};
        u32 patch_offset{0};
        u32 gen_offset{0};
    };
    static_assert(sizeof(DirectPatchSite) == 32,
                  "one DirectPatchSite is held per direct edge per thread");

    /// Every site this thread recorded against one target address, plus
    /// what a PATCHED edge into that target keeps alive. The retains are a
    /// function of the TARGET only, so they live here once rather than in
    /// each site: before this every site carried two vectors of its own
    /// (an 80-byte payload in a 112-byte multimap node per edge, the
    /// largest per-thread item in a browser renderer's heap).
    ///
    /// The first site is inline because nearly every target has exactly
    /// one source edge; further sites spill to a heap vector behind one
    /// pointer, so the node stays at 64 payload bytes (malloc's 96-byte
    /// class with the key and hash) instead of 80 (the 112-byte class).
    struct DirectPatchTarget {
        DirectPatchSite first{};
        std::unique_ptr<std::vector<DirectPatchSite>> more;
        // Delegate stencils embed pointers into decoded IR. Cache eviction
        // must not free an array still reachable through a patched edge.
        std::shared_ptr<const DecodedInsns::Storage> retained_insns;
        // A patched source can outlive the target's per-thread BlockCache
        // entry. Every guest page from the target's first page up to this
        // exclusive bound stays registered as JIT code until the last site
        // for the target is retired, so a later guest rewrite still advances
        // the executable/link generation instead of leaving the embedded
        // host target apparently current. Zero means nothing is retained.
        GuestAddr retained_end{0};

        [[nodiscard]] bool empty() const noexcept { return first.code == nullptr; }
        [[nodiscard]] usize size() const noexcept {
            return empty() ? 0 : 1 + (more ? more->size() : 0);
        }
        template <typename F>
        void for_each(F&& fn) {
            if (empty()) return;
            fn(first);
            if (more) {
                for (DirectPatchSite& site : *more) fn(site);
            }
        }
        void push(const DirectPatchSite& site) {
            if (empty()) {
                first = site;
                return;
            }
            if (!more) more = std::make_unique<std::vector<DirectPatchSite>>();
            more->push_back(site);
        }
        /// Drop every site for which @p dead returns true.
        template <typename F>
        void erase_if(F&& dead) {
            if (empty()) return;
            if (more) {
                std::erase_if(*more, [&](const DirectPatchSite& s) { return dead(s); });
                if (more->empty()) more.reset();
            }
            if (dead(first)) {
                if (!more) {
                    first = DirectPatchSite{};
                } else {
                    first = more->back();
                    more->pop_back();
                    if (more->empty()) more.reset();
                }
            }
        }
    };
    static_assert(sizeof(DirectPatchTarget) == 64,
                  "one DirectPatchTarget is held per link target per thread");
    std::unordered_map<GuestAddr, DirectPatchTarget> m_direct_patch_targets;

    /// Release the page retains and IR reference @p target holds. Does not
    /// touch its sites.
    void release_patch_target_retains(GuestAddr target,
                                      DirectPatchTarget& entry) noexcept;

    void release_direct_link_targets() noexcept;

    /// This thread's arena lease state (JitCodeArena::ArenaReadLease::
    /// thread_state()), resolved on the first execute(). See there.
    void* m_arena_tls{nullptr};
    /// The other thread_local slots execute() touches on every dispatched
    /// block, resolved alongside m_arena_tls under the same ownership rule.
    /// On Darwin every thread_local access is a `_tlv_get_addr` call; these
    /// four were ~1% of a game thread's time in the dispatcher.
    JitCompiler** m_tls_gui_jit{nullptr};
    u64* m_tls_last_block_pc{nullptr};
    bool* m_tls_jit_writable{nullptr};
    volatile JitCrashBreadcrumbs* m_tls_crash{nullptr};

    /// See jit_compiler.cpp: execute()'s per-entry cache revalidation, also
    /// run by the scalar memory helpers through jit_revalidate_memory_caches.
    void revalidate_memory_caches();
    /// Out of line so the no-change path above, run on every scalar helper
    /// call, does not reserve the one-ring scratch buffer on its stack.
    [[gnu::noinline, gnu::cold]] void
    drain_protect_shootdowns(u64 protect_snapshot);
    friend void jit_revalidate_memory_caches(JitContext* ctx) noexcept;

