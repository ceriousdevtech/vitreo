// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_compile_memo.hpp
/// @brief The process-wide store of already-compiled blocks.
///
/// Many guest threads execute the same code -- CRT init, schannel, winhttp,
/// kernel32 helpers -- so once any thread has compiled a self-contained block
/// at a guest address, every other thread can reuse the same arena pointer
/// instead of re-decoding and re-emitting it.
///
/// Extracted from JitCompiler for the same reason as JitCodeArena: this is
/// **process-wide state, and a compiler is not.** All of it is static, it
/// outlives every JitCompiler, and it was previously indistinguishable from
/// the per-block emission state in the same class.
///
/// The cut is between the STORE and the compiler's ADOPTION of what it finds.
/// Looking a block up, publishing one, purging records for an executable write
/// or a retired arena chunk, and the counters over all of that are here.
/// `try_adopt_shared_block()` and `link_adopted_block()` stay on JitCompiler:
/// they fill in a BasicBlock and patch link tails, which is compiling, not
/// remembering.

#include <vitreo/common.hpp>
#include <core/cpu/decode/decoder.hpp>
#include <core/cpu/jit/block_cache.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/jit/jit_memo_lock.hpp>

#include <array>
#include <atomic>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace vitreo::cpu::jit {

/// Self-modifying-code demotion accounting (defined in
/// constructor_destructor.cpp). See the comment there for why the existing
/// 32-message log cap left the size of the demoted set invisible.
void smc_note_demote() noexcept;

/// The process-wide compiled-block store. All state is static.
class JitCompileMemo {
public:
    // ---- Process-wide compiled-block memo (Stage A2) ----
    // Many worker threads execute the same guest code (CRT init, schannel,
    // winhttp, kernel32 helpers, …).  Without sharing, every thread re-decodes
    // and re-codegens those blocks, wasting CPU and arena bytes.
    // Once a fully self-contained block at guest VA `start_addr` has been
    // compiled by ANY thread, every other thread can reuse the same arena
    // pointer.
    //
    // A block with pre-decoded delegates embeds `&instructions[i]` — the one
    // genuinely thread-owned thing in generated code, since everything else
    // reaches per-thread state through the pinned CpuState (X19) and
    // JitContext (X20). Such blocks used to be excluded outright, which on a
    // multi-process browser startup is a large share of all compiles. They
    // are shareable if the memo record itself owns the decoded array, because
    // the array is immutable (DecodedInsns is copy-on-write) and the key
    // already pins the exact guest bytes it was decoded from — so the
    // reusing thread's own decode would be byte-identical to it.
    // `CompiledRecord::insns` holds
    // that ownership and `publish_compiled` adopts the array into the
    // reusing thread's cache entry.
    //
    // Arena code is shared RX. Direct-link tails patch MOV immediates in
    // that shared copy (generation + arena target); they never embed a
    // heap pointer, so a memo reuse of the unpatched stream stays safe.
    // The store is split into kMemoShards shards (see Shard), each behind
    // its own reader/writer lock: the steady-state path (everything cached)
    // proceeds in parallel, and a first compile write-locks one shard only.
    //
    // OPT-IN: gtest tests reuse identical guest VAs across test cases for
    // different code. The key includes current code identity so those blocks
    // cannot alias when memoization is enabled. Defaults to OFF; the main
    // runtime turns it on once after process init.
    struct CompiledKey {
        GuestAddr start_addr;
        u64 code_hash;
        u64 pin_layout;
        u32 guest_size;
        bool is_64bit;
        bool native_ud2;
        bool forward_join_state;
        bool native_callret;
        // Every field after the guest-identity ones describes the OPTIMIZATION
        // POLICY the cached code was emitted under. The memo is process-wide,
        // so a policy missing from this tuple lets one configuration serve its
        // code to another and an A/B measures whichever arm compiled first.
        u32 ir_policy;
        // X15 is the check map or the JIT view: code from one cannot run
        // under the other.
        bool linear64_jit_view;
        // L3: API-call tails carry the in-chain thunk stub
        // (JitCompiler::inchain_thunks_codegen) or the plain dispatcher
        // exit. The two codegen modes must never serve each other's code.
        bool inchain_thunks;
        // Cold-path residency variant (jit_compiler_cold_residency.hpp): the
        // recompile laid out from a cold-entry profile, without counters.
        // It never replaces the record it was profiled from.
        bool cold_residency;
        // Eager slow paths in a lazy-site process (cold_eager_requested):
        // a lazy translation of the block is exactly what must not be reused.
        bool cold_eager;

        bool operator==(const CompiledKey&) const = default;
    };
    struct CompiledKeyHash {
        usize operator()(const CompiledKey& key) const noexcept {
            usize hash = std::hash<GuestAddr>{}(key.start_addr);
            hash ^= std::hash<u64>{}(key.code_hash) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<u64>{}(key.pin_layout) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<u32>{}(key.guest_size) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.is_64bit) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.native_ud2) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.forward_join_state) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.native_callret) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<u32>{}(key.ir_policy) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.linear64_jit_view) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.inchain_thunks) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.cold_residency) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            hash ^= std::hash<bool>{}(key.cold_eager) + 0x9e3779b9u +
                    (hash << 6) + (hash >> 2);
            return hash;
        }
    };
    /// Bound local compilations without the memo, and actual shared-code
    /// rewrites with it. Ordinary memoized compilations need no counter entry.
    static constexpr u32 kChurnRecompileLimit = 8;

    /// A GuestMemory is going away: forget every record that retains pages in
    /// it. Registered as GuestMemory's teardown hook, so the records can never
    /// release a page into a destroyed address space. Production has exactly
    /// one GuestMemory for the process lifetime; tests build many.
    static void forget_guest_memory(GuestMemory* memory) noexcept;

    struct DirectLinkMeta {
        u32 patch_offset{0};
        u32 gen_offset{0};
        u64 target{0};
    };
    // Field order is by size so the record packs into 96 bytes: it is held
    // once per translation for the process lifetime (a browser renderer
    // carries ~250k), so every padding byte here is paid 250k times. The record's
    // pin layout is `CompiledKey::pin_layout` and is not duplicated.
    struct CompiledRecord {
        void* code{nullptr};
        // Patchable direct-edge slots in `code` (byte offsets, one per
        // constant forward edge). Stored so a memo reuse can still patch the
        // shared arena copy — never a heap pointer.
        std::vector<DirectLinkMeta> links;
        // Strong reference to the decoded array whose element addresses this
        // block's interpreter-delegate stencils baked in. Non-null only for
        // delegate-bearing blocks. This is what makes such a block shareable:
        // the arena bytes are thread-neutral, but they dereference this exact
        // allocation, so the memo must own it for as long as the record can
        // hand the code to another thread. Fully native blocks keep this null
        // and cost the memo nothing.
        std::shared_ptr<const DecodedInsns::Storage> insns;
        // The guest address space whose code pages this record retains
        // (see `retain_jit_code_page`). Null once that GuestMemory has been
        // torn down, at which point there is nothing left to release.
        GuestMemory* memory{nullptr};
        u64   arena_generation{0};
        u32   code_size{0};
        // Carried so a memo-reused block still reports its native instruction
        // count to the coverage telemetry. Without it, cross-thread reuse would
        // contribute zero to the denominator and inflate the delegate share.
        u32   native_insns{0};
        u32   link_code_offset{sizeof(u32)};
        u32   call_code_offset{std::numeric_limits<u32>::max()};
        // Decoded shape of the block this code was emitted from. A thread
        // that adopts the record from the shared table never decodes the
        // block itself, so the per-thread BasicBlock it publishes takes
        // these from here; the key already pins (address, bytes, size,
        // mode), of which the shape is a pure function.
        u32   instruction_count{0};
        // Bytes charged to the retained-IR budget for `insns` (0 when none).
        // A decoded array is instruction_count * sizeof(DecodedInstruction),
        // far below 4 GiB, so 32 bits hold it.
        u32   ir_bytes{0};
        u16   terminal_opcode{0};   ///< DIAGNOSTIC: opcode that ends the block
        bool  requires_protection_lease{false};
        bool  ends_with_branch{false};
        u16   cold_offset{0};       ///< CompiledBlock::cold_offset
        u16   cold_size{0};         ///< CompiledBlock::cold_size
    };
    static_assert(sizeof(CompiledRecord) == 96,
                  "CompiledRecord is held per translation for the process "
                  "lifetime; a new field must not reintroduce padding");

    struct CompiledEntry {
        CompiledKey key;
        CompiledRecord record;
    };

    /// Every translation published at one guest start address.
    ///
    /// Almost every address has exactly ONE (the memo key pins the bytes and
    /// the emission policy, and both are constant for the life of a loaded
    /// image), so that one lives inline in the map node. A second variant at
    /// the same address — a recompile after an executable write — spills to
    /// a heap vector.
    ///
    /// Before this the memo was three tables per record: a key-indexed map
    /// of records, an address-indexed map of key vectors, and the key vector
    /// itself. The second and third existed only to answer "any translation
    /// at this RIP?" for the cold-path probe and to reach the record from a
    /// page purge; keyed by address, the primary map answers both. Measured
    /// in the footprint harness (tests/cpu/test_jit_memory_footprint.cpp):
    /// 3 allocations / 318 bytes per record -> 1 / ~190.
    class CompiledList {
    public:
        CompiledList() = default;
        CompiledList(CompiledList&&) noexcept = default;
        CompiledList& operator=(CompiledList&&) noexcept = default;
        CompiledList(const CompiledList&) = delete;
        CompiledList& operator=(const CompiledList&) = delete;

        [[nodiscard]] bool empty() const noexcept { return m_count == 0; }
        [[nodiscard]] usize size() const noexcept { return m_count; }

        /// The entry whose key equals @p key, or null.
        [[nodiscard]] CompiledEntry* find(const CompiledKey& key) noexcept {
            if (m_count == 0) return nullptr;
            if (m_first.key == key) return &m_first;
            if (m_more) {
                for (CompiledEntry& e : *m_more) {
                    if (e.key == key) return &e;
                }
            }
            return nullptr;
        }

        /// Append. The caller has checked that @p key is absent. Returns the
        /// stored entry; the reference is valid until the next push/erase.
        CompiledEntry& push(CompiledKey key, CompiledRecord&& record) {
            if (m_count == 0) {
                m_first.key = key;
                m_first.record = std::move(record);
                m_count = 1;
                return m_first;
            }
            if (!m_more) m_more = std::make_unique<std::vector<CompiledEntry>>();
            m_more->push_back(CompiledEntry{key, std::move(record)});
            ++m_count;
            return m_more->back();
        }

        /// Visit every entry; @p fn returns true to erase that entry. The
        /// record of an erased entry is destroyed before the next visit.
        template <typename F>
        void erase_if(F&& fn) {
            if (m_count == 0) return;
            if (m_more) {
                std::erase_if(*m_more, [&](CompiledEntry& e) { return fn(e); });
                if (m_more->empty()) m_more.reset();
            }
            if (fn(m_first)) {
                if (!m_more) {
                    m_first = CompiledEntry{};
                    m_count = 0;
                    return;
                }
                m_first = std::move(m_more->back());
                m_more->pop_back();
                if (m_more->empty()) m_more.reset();
            }
            m_count = 1 + (m_more ? static_cast<u32>(m_more->size()) : 0);
        }

        template <typename F>
        void for_each(F&& fn) {
            if (m_count == 0) return;
            fn(m_first);
            if (m_more) {
                for (CompiledEntry& e : *m_more) fn(e);
            }
        }
        template <typename F>
        void for_each(F&& fn) const {
            if (m_count == 0) return;
            fn(m_first);
            if (m_more) {
                for (const CompiledEntry& e : *m_more) fn(e);
            }
        }

    private:
        CompiledEntry m_first{};
        std::unique_ptr<std::vector<CompiledEntry>> m_more;
        u32 m_count{0};
    };

    // Count changes between compiled byte identities, process-wide. A writer
    // may restore the same executable version after invalidating it; repeated
    // invalidation alone must not permanently disable translation.
    // Counts saturate at kChurnRecompileLimit, which demotes the address.
    struct SmcRewriteCount {
        u64 last_generation{0};
        u64 last_code_hash{0};
        bool have_code_hash{false};
        u32 count{0};
    };

    /// One slice of the store. Every record, its page-index entries and the
    /// rewrite history at its address live in the shard of the record's
    /// START PAGE (shard_index), so every per-address operation -- compile
    /// probe, cold adoption, SMC acceptance, publication -- takes exactly one
    /// shard lock, and blocks on one page share it. A single process-wide
    /// lock here was a handoff convoy in a 70-thread browser helper (11 threads
    /// parked on it at 11% CPU).
    ///
    /// Operations over the whole store visit shards in index order. Those
    /// that must be atomic over the store (clear_compile_memo,
    /// forget_guest_memory) hold every shard lock at once; no code path
    /// takes a second shard lock while holding one otherwise.
    struct alignas(128) Shard {
        JitMemoLock lock;
        /// Guest start address -> its translations. Answers the compile-time
        /// probe (exact key), the dispatcher's cold-path probe ("is any
        /// translation of the block at this RIP published?", asked before
        /// the block is decoded, when the full key is not yet known) and the
        /// page purge below, all from one node.
        std::unordered_map<GuestAddr, CompiledList> compiled;
        /// Guest page -> start addresses of every record IN THIS SHARD
        /// touching it, so an executable write purges exactly the
        /// overlapping records instead of walking the whole table.
        std::unordered_map<GuestAddr, std::vector<GuestAddr>> pages;
        std::unordered_map<GuestAddr, SmcRewriteCount> smc_rewrites;
        /// Records published in this shard. Written under the exclusive
        /// lock; read without it by compile_memo_size().
        std::atomic<usize> records{0};
        /// smc_rewrites.size(). Written under the exclusive lock; read
        /// without it so the common "address never rewritten" answer takes
        /// no lock at all.
        std::atomic<usize> smc_tracked{0};
    };
    static constexpr usize kMemoShards = 64;
    static_assert((kMemoShards & (kMemoShards - 1)) == 0);
    static std::array<Shard, kMemoShards>                   s_shards;
    /// Largest (last page - first page) of any record indexed since the last
    /// clear, in pages. A record touching page P starts on a page in
    /// [P - span, P], which bounds the shards a page purge must visit.
    static std::atomic<u64>                                 s_max_span_pages;
    static std::atomic<bool>                                s_compiled_memo_enabled;

    [[nodiscard]] static usize shard_index(GuestAddr start_addr) noexcept {
        // Fibonacci hash of the page number: neighbouring pages land in
        // different shards.
        const u64 page = static_cast<u64>(start_addr) >> 12;
        return static_cast<usize>((page * 0x9E3779B97F4A7C15ull) >> 58) &
               (kMemoShards - 1);
    }
    static_assert(kMemoShards == 64, "shard_index takes the top 6 bits");
    [[nodiscard]] static Shard& shard_for(GuestAddr start_addr) noexcept {
        return s_shards[shard_index(start_addr)];
    }

    /// Release everything a record derived outside its list entry: its guest
    /// page retains, its page-index membership (only when it was the LAST
    /// translation at its address, since the page index is keyed by start
    /// address) and its retained-IR budget. The caller then drops the entry
    /// from its CompiledList. Caller holds @p shard's lock exclusively.
    static void release_record_locked(Shard& shard, const CompiledKey& key,
                                      CompiledRecord& record,
                                      bool last_at_addr) noexcept;
    /// Erase every record in @p shard for which @p retire returns true,
    /// releasing each through release_record_locked. Caller holds @p shard's
    /// lock exclusively.
    template <typename F>
    static void erase_records_locked(Shard& shard, F&& retire) noexcept;
    /// Add a freshly inserted record to the page index, and retain its guest
    /// code pages unless the caller already did (@p pages_retained). Caller
    /// holds @p shard's lock exclusively.
    static void index_compiled_record_locked(Shard& shard,
                                             const CompiledKey& key,
                                             CompiledRecord& record,
                                             bool pages_retained) noexcept;
    /// Retain / release every guest page [key.start_addr, +guest_size)
    /// touches in @p memory's code-page map. No memo lock is needed.
    static void retain_record_pages(const CompiledKey& key,
                                    GuestMemory* memory) noexcept;
    static void release_record_pages(const CompiledKey& key,
                                     GuestMemory* memory) noexcept;
    /// Purge pass of purge_shared_records_overlapping over one shard.
    static usize purge_shard_overlapping(Shard& shard, GuestAddr addr,
                                         GuestAddr end, GuestAddr first_page,
                                         GuestAddr last_page,
                                         u64 generation) noexcept;

public:
    /// Enable the process-wide compile memo.  Call once from the main
    /// runtime after JIT init.  Tests should NOT enable this.
    static void enable_compile_memo() noexcept {
        s_compiled_memo_enabled.store(true, std::memory_order_release);
    }

    static void disable_compile_memo() noexcept {
        s_compiled_memo_enabled.store(false, std::memory_order_release);
    }

    /// Drop every memo record whose code lives in one of these arena chunks.
    /// Registered with JitCodeArena as its retirement observer; see the hook's
    /// comment for why the arena calls out rather than reaching in.
    static void purge_memo_for_retired_chunks(const std::vector<usize>& victims);
    static void clear_compile_memo() noexcept;

    /// Number of records currently published in the shared table.
    [[nodiscard]] static usize compile_memo_size() noexcept;

    /// Shared-table counters. `fresh_emits` is every block that actually ran
    /// the emitter into the arena; `compile_hits` are compile() calls the
    /// memo answered instead; `cold_adoptions` are dispatcher cold-path misses
    /// the table answered BEFORE the block was decoded. The sum of the three
    /// is roughly "blocks that became executable somewhere", and the ratio
    /// of fresh_emits to that sum is how much duplicated emission remains.
    struct CompileMemoStats {
        u64 compile_probes{0};
        u64 compile_hits{0};
        u64 fresh_emits{0};
        u64 cold_probes{0};
        u64 cold_adoptions{0};
        u64 cold_hash_mismatches{0};
        u64 cold_demoted{0};
        u64 smc_purges{0};
        usize records{0};
    };
    [[nodiscard]] static CompileMemoStats compile_memo_stats() noexcept;
    /// DIAGNOSTIC: one line per record -- start_addr code_hash guest_size
    /// is_64bit code_size native_insns -- for cross-process overlap studies.
    /// Written at perf-window stop when /tmp/vitreo-jit-memo-dump exists.
    static void dump_records(const char* path) noexcept;
    static void reset_compile_memo_stats() noexcept;

    /// Executable write at [addr, addr+size): erase every shared record whose
    /// guest bytes overlap it and count rewrites toward process-wide demotion. Called
    /// from the writing thread's observer and from every thread's
    /// synchronize_executable_writes(); idempotent, cheap when no record
    /// touches the written pages.
    /// @return number of records erased
    static usize purge_shared_records_overlapping(GuestAddr addr,
                                                  usize size,
                                                  u64 generation = 0) noexcept;

    /// True when repeated executable writes demoted the block at `addr` to the
    /// interpreter process-wide.
    [[nodiscard]] static bool shared_smc_demoted(GuestAddr addr) noexcept;
    /// Every compile asks this. An address whose shard tracks no rewrite
    /// history is accepted without locking. Other outcomes that leave the
    /// history unchanged (untracked address, already demoted, same byte
    /// identity) are decided under the shard's shared lock; only a
    /// byte-identity change takes it exclusively, and re-reads the history
    /// there.
    [[nodiscard]] static bool accept_smc_compilation(
        GuestAddr addr, u64 code_hash, bool& tracked) noexcept;

    /// Publish the translation @p build fills in, unless an equal key is
    /// already published, the arena chunk holding @p code no longer carries
    /// @p generation, or the retained-IR budget refuses @p ir_bytes. The
    /// record is built and the map node allocated before the shard's
    /// exclusive lock is taken; under it every refusal condition is
    /// re-checked, so an insert racing purge_memo_for_retired_chunks (which
    /// write-locks every shard after the generation moved) either precedes
    /// the purge of its shard (which erases it) or sees the retired
    /// generation, and adopters (who read under the shard's shared lock)
    /// only ever see a complete record.
    /// @return true when the record was inserted
    template <typename Build>
    static bool publish_record(const CompiledKey& key, const void* code,
                               u64 generation, usize ir_bytes, Build&& build) {
        return publish_record_impl(
            key, code, generation, ir_bytes,
            [](void* ctx, CompiledRecord& record) {
                (*static_cast<std::remove_reference_t<Build>*>(ctx))(record);
            },
            static_cast<void*>(&build));
    }

    /// A/B arm: VITREO_JIT_MEMO_LEGACY_LOCK=1 (read once) makes
    /// accept_smc_compilation and publish_record take the shard lock
    /// exclusively for the whole lookup/build/insert, with no lock-free or
    /// shared-lock probe first.
    [[nodiscard]] static bool legacy_memo_locking() noexcept;
    /// Tests: 1 legacy, 0 reader-first, -1 re-read the environment.
    static void set_legacy_memo_locking_for_test(int mode) noexcept;

private:
    using BuildRecordFn = void (*)(void* ctx, CompiledRecord& record);
    static bool publish_record_impl(const CompiledKey& key, const void* code,
                                    u64 generation, usize ir_bytes,
                                    BuildRecordFn build, void* ctx);
};

}  // namespace vitreo::cpu::jit
