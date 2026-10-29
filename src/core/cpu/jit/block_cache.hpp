#pragma once

/// @file block_cache.hpp
/// @brief JIT basic block cache (CPU-035)

#include <vitreo/common.hpp>

#include <core/cpu/decode/decoder.hpp>

#include <cstddef>
#include <list>
#include <functional>
#include <memory>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cpu::jit {

/// A block's decoded instructions, in its own heap vector.
///
/// Element addresses are stable for the array's lifetime: it is immutable once
/// published, and the generated delegate stencils bake `&instructions[i]`.
class DecodedArray {
public:
    [[nodiscard]] usize size() const noexcept { return m_insns.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_insns.empty(); }
    [[nodiscard]] const cpu::DecodedInstruction* data() const noexcept {
        return m_insns.data();
    }
    [[nodiscard]] const cpu::DecodedInstruction& operator[](
        usize i) const noexcept {
        return m_insns[i];
    }

    /// Allocated element slots.
    [[nodiscard]] usize capacity() const noexcept { return m_insns.capacity(); }

    void push_back(const cpu::DecodedInstruction& insn) {
        m_insns.push_back(insn);
    }

    void resize(usize n) { m_insns.resize(n); }

    template <typename It>
    void assign(It first, It last) {
        m_insns.assign(first, last);
    }

private:
    std::vector<cpu::DecodedInstruction> m_insns;
};

/// Refcounted, copy-on-write handle to a block's decoded instruction array.
///
/// `DecodedInstruction` is 144 bytes, so a block's IR dominates the cache's
/// heap. Two things make sharing worthwhile:
///
///  - `BlockCache` is per guest thread, so every thread that executes a given
///    address used to own its own copy of the same decode.
///  - The JIT's interpreter-delegate path bakes `&instructions[i]` into
///    generated code, so a delegating block's array must stay alive and at a
///    stable address for as long as its compiled code can run. Refcounting
///    expresses that lifetime directly.
///
/// Reads go straight through. The two mutators (`push_back` / `assign`) detach
/// first, so a handle that is still shared is never modified in place.
class DecodedInsns {
public:
    using Storage = DecodedArray;

    DecodedInsns() = default;
    explicit DecodedInsns(std::shared_ptr<const Storage> insns) noexcept
        : m_insns(std::move(insns)) {}

    [[nodiscard]] usize size() const noexcept {
        return m_insns ? m_insns->size() : 0;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const cpu::DecodedInstruction& operator[](
        usize i) const noexcept {
        return (*m_insns)[i];
    }
    [[nodiscard]] const cpu::DecodedInstruction* begin() const noexcept {
        return m_insns ? m_insns->data() : nullptr;
    }
    [[nodiscard]] const cpu::DecodedInstruction* end() const noexcept {
        return begin() + size();
    }
    [[nodiscard]] const cpu::DecodedInstruction* data() const noexcept {
        return begin();
    }

    /// Shared ownership, for publishing into a cross-thread decode memo.
    [[nodiscard]] std::shared_ptr<const Storage> share() const noexcept {
        return m_insns;
    }

    /// Drop this block's reference. Other holders — including generated code
    /// that baked a pointer into the array — keep theirs.
    void reset() noexcept { m_insns.reset(); }

    /// Allocated element slots, 0 when no array is held. Lets callers assert
    /// that a short block did not retain the decoder's scratch capacity and
    /// that a released block really gave its storage back.
    [[nodiscard]] usize capacity() const noexcept {
        return m_insns ? m_insns->capacity() : 0;
    }

    void push_back(const cpu::DecodedInstruction& insn) {
        detach()->push_back(insn);
    }

    void resize(usize n) { detach()->resize(n); }

    template <typename It>
    void assign(It first, It last) {
        detach()->assign(first, last);
    }

private:
    /// Copy-on-write: obtain a uniquely owned array to mutate.
    Storage* detach() {
        if (!m_insns) {
            auto fresh = std::make_shared<Storage>();
            Storage* raw = fresh.get();
            m_insns = std::move(fresh);
            return raw;
        }
        // Always copy. A shared array may already be referenced by another
        // thread's block or by baked-in generated code, so what comes back
        // must be privately owned to be safe to mutate.
        auto fresh = std::make_shared<Storage>(*m_insns);
        Storage* raw = fresh.get();
        m_insns = std::move(fresh);
        return raw;
    }

    std::shared_ptr<const Storage> m_insns;
};

/// A basic block of decoded instructions
class BlockCache;

struct BasicBlock {
    // Layout: 8-byte fields first, then 4-byte, then the flags, so the
    // struct carries only tail padding. One BasicBlock lives in every thread's
    // cache for every block that thread runs (a browser renderer holds ~250k
    // across 23 threads), as a std::list node that malloc rounds up to the
    // next 16 bytes, so 106 payload bytes cost 128. (They cost 144 while the
    // dynamic-pin preference field existed, and 160 with the old 136-byte
    // layout and its 21 bytes of padding.) JIT-emitted linked tails read the
    // compiled_* fields at offsets taken with offsetof at emit time
    // (stage_b1_inline_block_link.cpp), so field ORDER is free to change;
    // only `start_addr` is pinned at offset 0 by a static_assert there.
    GuestAddr start_addr{0};
    u32 byte_length{0};
    u32 instruction_count{0};
    DecodedInsns instructions;
    u64 execution_count{0};

    // JIT compiled code (set by JitCompiler after compilation)
    void* compiled_code{nullptr};
    // JIT-to-JIT entry: after STR LR, GPR pin load, and linear-base load.
    // Linked hops keep pins in X23–X28 / X12 / X13 and share one LR frame.
    void* compiled_link_code{nullptr};
    /// The BlockCache this entry lives in, or null while the block is still a
    /// caller-owned temporary. Set by insert(). Exists so publish_compiled()
    /// — the ONE choke point every compile path goes through — can refresh
    /// this block's L1 slot, which caches compiled_link_code by value. Hooking
    /// the individual publish call sites instead missed several: the whole
    /// emitted fast path went silently dead in 22 tests.
    BlockCache* cache_owner{nullptr};
    // Immutable ABI expected by compiled_link_code. Generated link tails read
    // this cache-owned field before bypassing the target prologue.
    u64 compiled_pin_layout{0};
    // Process-wide executable-arena generation that owns compiled_code.
    // A cache entry from an older generation must never be entered after the
    // arena has been reclaimed and its bytes reused for another block.
    u64 compiled_arena_generation{0};
    // Native guest-CALL entry. Unlike compiled_link_code, this landing pad
    // preserves the host continuation before entering the same linked ABI.
    // Null until the native path lands.
    void* compiled_call_code{nullptr};

    u32 compiled_code_size{0};
    // Instructions in compiled_code that got a native lowering (excluding
    // interpreter-delegate instructions). Diagnostic only — the denominator for
    // JIT coverage telemetry. Mirrored here because the run loop enters
    // compiled code through a CompiledBlock it builds from this cache entry,
    // not through the one JitCompiler::compile returned. See jit_coverage.hpp.
    u32 compiled_native_insns{0};
    // Exact hot-dump byte variant paired with compiled_code. Diagnostic only.
    u32 compiled_hot_dump_variant{~u32{0}};

    bool ends_with_branch{false};
    bool is_64bit{false};  ///< True if block was decoded in 64-bit (long) mode
    // Cache-local second-chance bit. Fast hits avoid LRU list mutation but
    // must still distinguish executed blocks from cold insertion traffic.
    bool cache_referenced{false};
    bool compiled_requires_protection_lease{false};
    // Set when JIT compilation was attempted and failed (don't retry)
    bool compile_failed{false};
    /// The decoded array was dropped by the retention policy while
    /// `instruction_count` stayed set, so `instructions` is empty for a block
    /// that really does have instructions.
    ///
    /// `instruction_count > 0 && instructions.empty()` already implies this;
    /// the flag exists so diagnostics and tests can tell a released block from
    /// a block that never decoded anything, and so a re-decode that fails can
    /// be distinguished from a first decode that failed.
    bool instructions_released{false};
};
static_assert(sizeof(BasicBlock) == 112,
              "BasicBlock is one std::list node per cached block per thread; "
              "keep its layout tight (see the layout comment)");

// ---------------------------------------------------------------------------
// Decoded-IR retention policy
//
// `DecodedInstruction` is 144 bytes and a `BlockCache` is per guest thread, so
// the decoded array dominates the emulator's own host heap. Measured on a
// logged-in multi-process launcher: 136,178 live blocks / 957,731 compiled x86
// instructions, and `vmmap` attributed ~400 MB of DIRTY MALLOC_SMALL per
// process to Vitreo's heap rather than to guest memory.
//
// Retention is only strictly REQUIRED for blocks whose generated code baked
// raw `&instructions[i]` pointers for the interpreter-delegate path (see
// instruction_compilation.cpp, `m_has_predecoded_delegate`). Everything else
// can give the array back and re-decode on demand: `decode_basic_block` is a
// pure function of (address, mode, guest bytes), and the process-wide
// decode memo usually answers a re-decode without re-running the decoder.
// ---------------------------------------------------------------------------

/// VITREO_JIT_RELEASE_DECODED gate. Default ON; set to "0" to retain every
/// block's decoded array (the pre-policy behaviour) for A/B measurement.
/// Read once into a `static const`, following the codebase convention.
[[nodiscard]] bool release_decoded_enabled() noexcept;

/// Counters for the retention policy. Diagnostic only.
struct DecodedReleaseStats {
    /// Cache entries whose decoded array handle was dropped.
    u64 releases{0};
    /// Bytes those arrays spanned. This is an upper bound on bytes actually
    /// returned to the allocator: a released array is still alive if the
    /// decode memo or another thread's cache entry holds
    /// the same `Storage`.
    u64 released_bytes{0};
    /// Blocks retained because their generated code baked pointers into the
    /// array. The policy's fail-closed arm.
    u64 retained_delegate{0};
    /// Re-decodes triggered because a released block needed its array again.
    u64 redecodes{0};
    /// Re-decodes that did NOT reproduce the cached block shape (guest bytes
    /// changed under the entry). The caller must fall back, not proceed.
    u64 redecode_failures{0};
};

[[nodiscard]] DecodedReleaseStats decoded_release_stats() noexcept;
void reset_decoded_release_stats() noexcept;
void note_decoded_redecode(bool ok) noexcept;

/// Drop @p block's decoded array when the policy permits it.
///
/// @param has_baked_pointers  True when generated code embedded addresses of
///        elements of this exact array. The caller must FAIL CLOSED: pass true
///        whenever it cannot prove otherwise.
/// @return true when the array was released.
///
/// Safe to call on a block with no array, on a block whose array was already
/// released, and on a block that was never compiled (it simply declines).
bool release_block_decoded(BasicBlock& block, bool has_baked_pointers) noexcept;

/// VITREO_JIT_REENTRY_LEGACY=1 restores the pre-2026-10-19 evict/re-entry
/// path for A/B measurement: every cache node goes back to malloc, each insert
/// rescans its pages' start lists for a duplicate, and an adopted block's
/// memo record and link handoff are freshly allocated. Default off.
[[nodiscard]] bool reentry_legacy() noexcept;
/// Test hook: 1 forces legacy, 0 forces the default, -1 re-reads the env.
void set_reentry_legacy_for_test(int policy) noexcept;

/// Keeps a few freed single-object nodes per size for reuse.
///
/// A cache over its capacity evicts one block for every block it inserts,
/// so each re-entry used to free an LRU node, a hash node and (when a page
/// emptied) a page node, then malloc the same three sizes back. Recycling
/// turns that into free-list pops. The bins are bounded, so a mass
/// invalidation or clear() hands everything past kMaxPerBin back to malloc.
class NodeRecycler {
public:
    NodeRecycler() = default;
    NodeRecycler(const NodeRecycler&) = delete;
    NodeRecycler& operator=(const NodeRecycler&) = delete;
    ~NodeRecycler();

    [[nodiscard]] void* take(usize bytes);
    void give(void* p, usize bytes) noexcept;

private:
    static constexpr usize kBins = 6;
    static constexpr u32 kMaxPerBin = 64;
    struct Bin {
        usize bytes{0};
        void* head{nullptr};
        u32 count{0};
    };
    Bin m_bins[kBins]{};
};

/// Allocator handing single-object allocations to a NodeRecycler. Arrays
/// (hash bucket tables) go straight to the global allocator.
template <typename T>
struct RecyclingAllocator {
    using value_type = T;
    NodeRecycler* pool{nullptr};

    explicit RecyclingAllocator(NodeRecycler* p) noexcept : pool(p) {}
    template <typename U>
    RecyclingAllocator(const RecyclingAllocator<U>& other) noexcept  // NOLINT
        : pool(other.pool) {}

    [[nodiscard]] T* allocate(usize n) {
        if (n == 1) return static_cast<T*>(pool->take(sizeof(T)));
        return std::allocator<T>().allocate(n);
    }
    void deallocate(T* p, usize n) noexcept {
        if (n == 1) {
            pool->give(p, sizeof(T));
            return;
        }
        std::allocator<T>().deallocate(p, n);
    }
    template <typename U>
    bool operator==(const RecyclingAllocator<U>& o) const noexcept {
        return pool == o.pool;
    }
    template <typename U>
    bool operator!=(const RecyclingAllocator<U>& o) const noexcept {
        return pool != o.pool;
    }
};

/// LRU block cache mapping guest addresses to pre-decoded blocks
class BlockCache {
public:
    explicit BlockCache(usize max_blocks = 4096);

    // No copy or move: each instance is registered for the performance window.
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;
    BlockCache(BlockCache&&) = delete;
    BlockCache& operator=(BlockCache&&) = delete;
    ~BlockCache();

    /// Look up a cached block by guest address. Returns nullptr on miss.
    /// Promotes found block to MRU position.
    BasicBlock* lookup(GuestAddr addr);

    /// Chain re-entry lookup: same hit/miss semantics as lookup(), updates L1,
    /// but skips LRU splice on most hash hits. Hot dispatcher misses alias in
    /// L1 and used to pay list surgery on every hash hit; promote every 64th
    /// hash hit so eviction still tracks heat. Prefer lookup() for cold
    /// dispatcher entry so first touch still goes MRU.
    BasicBlock* lookup_fast(GuestAddr addr);

    /// Diagnostics: return a cached block with start < addr < start+length
    /// (strict mid-body; exact start returns nullptr). Uses the page→blocks
    /// index. Does not execute, promote, or touch L1.
    [[nodiscard]] BasicBlock* find_covering(GuestAddr addr);

    /// Insert a new block. Evicts LRU if cache is full.
    /// Returns a pointer to the cached entry (never null).
    BasicBlock* insert(BasicBlock block);

    /// Invalidate a single block at exact address
    /// @return true if block was found and removed
    bool invalidate(GuestAddr addr);
    /// Starts of every cached block whose guest bytes contain @p rip (blocks
    /// may overlap). Uses the page index; a block is found from the page of
    /// its start, so the page of @p rip and the one before it are searched.
    void collect_containing(GuestAddr rip, std::vector<GuestAddr>& out) const;

    /// Invalidate all blocks overlapping [start, start+length)
    /// @param removed_out optional list of removed block start addresses
    /// @param compiled_removed_out optional subset that held native code
    /// @return Number of blocks removed
    usize invalidate_range(GuestAddr start, usize length,
                           std::vector<GuestAddr>* removed_out = nullptr,
                           std::vector<GuestAddr>* compiled_removed_out = nullptr);

    /// Invalidate an overlapping range only when one of its pages contains cached code.
    /// @param removed_out optional list of removed block start addresses
    /// @return Number of blocks removed
    usize invalidate_if_cached_page(GuestAddr start, usize length,
                                    std::vector<GuestAddr>* removed_out = nullptr,
                                    std::vector<GuestAddr>* compiled_removed_out = nullptr);

    /// Forget compiled code emitted before `generation`, keeping the decodes.
    /// Used when the JIT retires an arena segment: those blocks' native code
    /// has been unmapped, but their guest bytes are unchanged, so re-decoding
    /// them would be pure loss.
    /// @return number of entries whose compiled code was dropped
    usize drop_compiled_older_than(u64 generation);
    /// Same, for every compiled entry `retired` returns true for. Entries
    /// keep their decode; ones that already released it are evicted.
    usize drop_compiled_if(const std::function<bool(const BasicBlock&)>& retired);

    /// Clear all cached blocks
    void clear();

    /// Current number of cached blocks
    [[nodiscard]] usize size() const noexcept;

    /// Maximum capacity
    [[nodiscard]] usize capacity() const noexcept;

    /// Total lookup hits (L1 + hash)
    [[nodiscard]] u64 total_hits() const noexcept;

    /// Total lookup misses
    [[nodiscard]] u64 total_misses() const noexcept;

    [[nodiscard]] u64 l1_hits() const noexcept;
    [[nodiscard]] u64 hash_hits() const noexcept;
    [[nodiscard]] u64 evictions() const noexcept;
    [[nodiscard]] u64 invalidations() const noexcept;

    /// One direct-mapped L1 slot.
    ///
    /// This used to be a bare `BasicBlock*`, which cost the JIT-emitted link
    /// tail a THREE-DEEP dependent load chain on the hottest path there is:
    /// ctx -> l1 base, base -> slot, slot -> {start_addr, compiled_link_code}.
    /// `sc_blocksize` measured the resulting per-transition cost at ~49 cycles
    /// on x64, 4.1x the 32-bit path, and that transition cost is essentially
    /// the whole of the `branchy` gap against native Windows.
    ///
    /// Inlining the tag and the entry pointer into the slot lets the tail load
    /// both with ONE `LDP`, so the chain is base -> LDP. `tag`/`link_code`
    /// occupy the first 16 bytes for exactly that reason and must stay
    /// adjacent and 16-byte aligned. `pin_layout` is a copy of the block's
    /// (now fixed) compiled pin layout; no emitted code reads it since the
    /// dynamic pin policy was removed, but it keeps `block` at +24, which the
    /// emitted RSB push hard-codes (stage_b1_inline_block_link.cpp) and the
    /// static_assert below pins. `block` is what the fallback paths need.
    ///
    /// `tag == 0` means empty: guest address 0 is never executable, so an
    /// empty slot simply fails the tag compare and needs no separate null
    /// test. These fields are COPIES, so every mutation of the underlying
    /// block's published code must refresh or clear the slot — see
    /// refresh_l1() and the callers listed there.
    struct alignas(32) L1Entry {
        GuestAddr   tag{0};              ///< +0  block->start_addr, 0 = empty
        void*       link_code{nullptr};  ///< +8  block->compiled_link_code
        u64         pin_layout{0};       ///< +16 block->compiled_pin_layout
        BasicBlock* block{nullptr};      ///< +24 owner
    };
    static_assert(sizeof(L1Entry) == 32, "emitted L1 tail assumes 32-byte slots");
    static_assert(offsetof(L1Entry, tag) == 0 &&
                      offsetof(L1Entry, link_code) == 8,
                  "emitted L1 tail LDPs tag/link_code as one 16-byte pair");
    static_assert(offsetof(L1Entry, block) == 24,
                  "emitted RSB push reads L1Entry::block at +24 (and +56 for "
                  "the next slot); do not drop or reorder pin_layout");

    /// Direct access to the L1 direct-mapped array base. Used by the JIT to
    /// emit inline block-link tail sequences that re-enter compiled code
    /// without returning to the dispatcher (Stage B1).
    /// Returns a pointer to a contiguous array of `kL1Capacity()` L1Entry
    /// slots, indexed by `addr & (kL1Capacity()-1)`.
    [[nodiscard]] L1Entry* l1_base() noexcept { return m_l1.get(); }

    /// Re-copy `block`'s published code pointers into its L1 slot, or clear
    /// the slot when the block no longer holds live code. Must be called
    /// after ANY mutation of compiled_link_code / compiled_pin_layout: the
    /// slot caches them, and a slot still holding a retired entry pointer
    /// would send an emitted tail branching into an unmapped arena segment.
    void refresh_l1(const BasicBlock* block) noexcept;
    [[nodiscard]] static constexpr usize l1_capacity() noexcept { return kL1Size; }

    /// Direct-map index used by C++ lookup and the JIT-emitted L1 tail.
    /// x86/x64 block starts are byte-addressed and need not be four-byte
    /// aligned, so no low bits may be shifted away: a CALL continuation and
    /// its callee are distinct starts a few bytes apart. The emitted inline
    /// tails compute the SAME formula via emit_l1_index(); a formula the JIT
    /// does not match could enter the wrong block.
    ///
    /// The bare low bits are not enough on their own. MEASURED 2026-09-08 on
    /// tools/stress codefoot16384 (947 KiB of .text, 16,428 functions): MSVC
    /// aligns function entries to 16 bytes, so `addr & 0xFFFF` can only ever
    /// reach 4,096 of the 65,536 slots — 15/16 of the table is unreachable —
    /// and 75.2% of that binary's functions landed on a slot they had to
    /// share, up to 10 deep. XOR-folding bits 16+ down over the index both
    /// keeps every low bit (so nearby unaligned starts stay distinct) and
    /// spends the alignment zeros on the address' 64 KiB page number, which
    /// took the same binary to 0 colliding entries.
    ///
    /// This is a MIX, not a shift, which is why l1_index_shift() is gone: any
    /// pure shift trades one of these two properties for the other.
    /// The L1 is two-way set-associative: kL1Size entries as kL1Size/2 sets
    /// of two adjacent ways. MEASURED 2026-09-10 on a launcher login (perf
    /// window exit histogram): in every large browser process 52-82 % of all
    /// dispatcher exits were "indirect tail: L1 addr mismatch" and 6-15 %
    /// "const tail: L1 addr mismatch" -- the slot held a DIFFERENT block, a
    /// conflict miss, with 880 K live blocks against 65,536 direct-mapped
    /// slots. A second way halves that at the same memory: the emitted
    /// probe checks way 0 exactly as before and only a way-0 miss pays a
    /// second LDP/EOR/CBNZ before falling back. The index below names the
    /// SET; l1_index() returns way 0's entry index.
    [[nodiscard]] static constexpr u32 l1_index_bits() noexcept {
        return kL1SetBits;
    }
    /// Bits folded down over the index. Must equal the LSR amount the JIT
    /// emits, and must be >= l1_index_bits() so the fold cannot cancel the
    /// low bits it is meant to disambiguate.
    [[nodiscard]] static constexpr u32 l1_index_fold() noexcept {
        return kL1IndexBits;
    }
    /// Entry index of way 0 of the set that holds `addr`; way 1 is +1.
    [[nodiscard]] static constexpr usize l1_index(GuestAddr addr) noexcept {
        return ((addr ^ (addr >> l1_index_fold())) & ((kL1Size / kL1Ways) - 1))
               * kL1Ways;
    }
    /// The set index the emitted probe computes (l1_index() / kL1Ways).
    [[nodiscard]] static constexpr usize l1_set_index(GuestAddr addr) noexcept {
        return l1_index(addr) / kL1Ways;
    }

    /// The next address at or past `base + min_distance` that shares `base`'s
    /// L1 slot, or 0 if none within 256 KiB of there.
    ///
    /// Tests and diagnostics need to construct a deliberate L1 collision.
    /// They used to do it as `base + l1_capacity()`, which silently encoded
    /// the assumption that the index is a bare mask — under a mix there is no
    /// constant stride that aliases from every base, and such a test keeps
    /// compiling while testing nothing. Asking the formula lives here so it
    /// cannot drift from l1_index() above.
    [[nodiscard]] static constexpr GuestAddr l1_alias_after(
        GuestAddr base, GuestAddr min_distance) noexcept {
        const usize want = l1_index(base);
        const GuestAddr from = base + min_distance;
        for (GuestAddr a = from; a < from + 0x2000000; ++a) {
            if (l1_index(a) == want) return a;
        }
        return 0;
    }

    /// Conservative bounds of every page that currently holds compiled code.
    /// A guest range that does not overlap [code_span_low(), code_span_high())
    /// provably contains no compiled code. The span only ever grows
    /// (remove_code_pages does not shrink it). When no code is cached, low >
    /// high and every range is code-free. Prefer range_is_code_free() or the
    /// page map for a store check — the span over-approximates once code is
    /// compiled at both a low image and a high DLL.
    [[nodiscard]] GuestAddr code_span_low() const noexcept { return m_code_span_low; }
    [[nodiscard]] GuestAddr code_span_high() const noexcept { return m_code_span_high; }

    /// 1 byte per 4 KiB guest page in the low 4 GiB. Non-zero iff this cache
    /// currently holds compiled code on that page. JIT stores load this
    /// pointer and skip the SMC helper for data pages inside the span.
    [[nodiscard]] u8* code_page_map() noexcept { return m_code_page_map.get(); }
    [[nodiscard]] const u8* code_page_map() const noexcept {
        return m_code_page_map.get();
    }
    static constexpr usize kGuestPageCount = usize{1} << 20;

    /// Bumped whenever the code span grows. The JIT compares this against the
    /// generation its published write-regions were validated under, so a block
    /// compiled into a page inside a published region retracts that region.
    [[nodiscard]] u64 code_generation() const noexcept { return m_code_gen; }

    /// True when [start, start+length) holds no compiled code. Uses the
    /// conservative span as a reject, then the page map so a heap sitting
    /// between two images is not treated as code.
    [[nodiscard]] bool range_is_code_free(GuestAddr start, usize length) const noexcept;

    /// Associate this per-thread cache with its guest address space. This
    /// contributes the cache's live code pages to GuestMemory's process-wide
    /// SMC index, including blocks inserted before the association.
    void bind_guest_memory(GuestMemory* memory);

    /// Walk compiled entries. Used by the performance window dump.
    void for_each_compiled(void (*fn)(u64 guest_addr, u64 exec_count,
                                      u32 native_bytes, u32 guest_insns,
                                      void* ctx),
                           void* ctx) const;

private:
    // Declared before every container that allocates from it, so it is
    // destroyed after them.
    NodeRecycler m_recycler;
    template <typename K, typename V>
    using RecycledMap =
        std::unordered_map<K, V, std::hash<K>, std::equal_to<K>,
                           RecyclingAllocator<std::pair<const K, V>>>;

    // Evicted blocks keep only heat and their invalidation span. Keeping these
    // pages in the existing code-page index also catches writes after eviction.
    struct HotnessEntry {
        GuestAddr start_addr;
        u32 byte_length;
        bool is_64bit;
        u64 execution_count;
    };
    using HotnessList = std::list<HotnessEntry, RecyclingAllocator<HotnessEntry>>;
    HotnessList m_hotness{RecyclingAllocator<HotnessEntry>{&m_recycler}};
    RecycledMap<GuestAddr, HotnessList::iterator> m_hotness_map{
        0, std::hash<GuestAddr>{}, std::equal_to<GuestAddr>{},
        RecyclingAllocator<std::pair<const GuestAddr, HotnessList::iterator>>{
            &m_recycler}};
    void forget_hotness(GuestAddr addr);
    void remember_hotness(const BasicBlock& block);

    usize m_max_blocks;
    u64 m_hits{0};
    u64 m_misses{0};
    u64 m_l1_hits{0};
    u64 m_hash_hits{0};
    u64 m_evictions{0};
    u64 m_invalidations{0};

    // Direct-mapped L1 for the inline block-link tail. Index is
    // `addr & (kL1Size-1)` — same bits the JIT ubfx extracts. Shifting by two
    // treated x86/x64 block starts as four-byte aligned, making distinct
    // nearby starts such as a CALL continuation and callee fight for one
    // slot. The window an index covers is `kL1Size` bytes of guest address
    // space: 65536 slots alias every 64 KiB. A linked
    // tail that aliases falls back to the dispatcher. MEASURED 2026-08-23:
    // widening the former four-byte-shifted table to 2^20 entries (a 4 MiB
    // window) left a browser helper's
    // average linked-chain length unchanged (9.0 -> 7.8, i.e. inside the
    // run-to-run spread) and cost 8 MiB of address space per guest thread.
    // Byte indexing instead keeps the table footprint unchanged and uses the
    // address entropy that x86/x64 block starts actually provide.
    // Chains break because the target block is not compiled *in this
    // thread's cache* yet, not because its L1 slot is taken. Do not retry
    // the widening without new evidence.
    //
    // The table is mapped, not new[]-ed: one BlockCache exists per guest
    // thread, so a value-initialized array would memset the whole table on every
    // thread creation. Anonymous pages are already zero and are only
    // faulted in for the slots a thread actually indexes, which keeps a
    // cold thread's cost at ~0 and a hot thread's at what it uses.
    // MEASURED 2026-09-10 on a launcher login: the "L1 addr mismatch" exits
    // that dominate every large browser process did not move with two-way
    // associativity, with a block cache that never evicted, or with an
    // eight-times larger table (2^19 entries), so they are not L1 capacity
    // misses; see the exit histogram's thunk-target split. 2^16 stays.
    static constexpr u32 kL1IndexBits = 16;
    static constexpr usize kL1Size = usize{1} << kL1IndexBits;
public:
    static constexpr u32 kL1Ways = 2;
    static constexpr u32 kL1SetBits = kL1IndexBits - 1;
private:
    struct L1Unmap {
        void operator()(L1Entry* p) const noexcept;
    };
    std::unique_ptr<L1Entry[], L1Unmap> m_l1;

    /// Write `block`'s current published state into the set at `idx` (way 0's
    /// entry index): the way already holding it, else an empty way, else
    /// way 1 is evicted and way 0 shifts into it so way 0 is the most
    /// recently installed.
    void store_l1(usize idx, BasicBlock* block) noexcept;
    /// Empty one entry completely (tag, payload and owner).
    void clear_l1(usize idx) noexcept;
    /// The entry index in the set at `idx` whose owner is `block`, or
    /// SIZE_MAX when neither way holds it.
    [[nodiscard]] usize l1_way_of(usize idx, const BasicBlock* block) const noexcept;
    /// The entry index in the set at `idx` whose owner starts at `addr`, or
    /// SIZE_MAX.
    [[nodiscard]] usize l1_way_for(usize idx, GuestAddr addr) const noexcept;
    /// Clear whichever way of the set at `idx` holds `block`, if any.
    void clear_l1_block(usize idx, const BasicBlock* block) noexcept;
    /// Re-copy `block`'s published state into slot `idx` if the copies have
    /// drifted. Called from the C++ L1 hit path, which is what makes a slot
    /// installed before compilation pick the code up afterwards.
    void sync_l1_payload(usize idx, BasicBlock* block) noexcept;

    // LRU list: front = MRU, back = LRU
    using LruList = std::list<BasicBlock, RecyclingAllocator<BasicBlock>>;
    LruList m_lru{RecyclingAllocator<BasicBlock>{&m_recycler}};

    // Hash map for O(1) lookup: addr -> iterator into LRU list
    RecycledMap<GuestAddr, LruList::iterator> m_map{
        0, std::hash<GuestAddr>{}, std::equal_to<GuestAddr>{},
        RecyclingAllocator<std::pair<const GuestAddr, LruList::iterator>>{
            &m_recycler}};
    /// Per guest page holding cached code: a refcount of the resident blocks
    /// and hotness spans touching it (the cheap invalidate_if_cached_page
    /// negative/positive filter), and the reverse index of their start
    /// addresses, which lets invalidate_range touch only candidate blocks
    /// instead of walking the whole LRU (installer file-copy SMC was
    /// O(cache) per write and got progressively slower as the cache filled).
    ///
    /// One map, not a refcount map plus a page->unordered_set map: the two
    /// were keyed identically and the set cost a 32-byte node per block per
    /// thread plus its own bucket array; a short vector of starts (a page
    /// holds tens of blocks, so the linear erase is cheap) costs ~12.
    struct CodePage {
        usize refs{0};
        std::vector<GuestAddr> starts;
    };
    RecycledMap<GuestAddr, CodePage> m_code_pages{
        0, std::hash<GuestAddr>{}, std::equal_to<GuestAddr>{},
        RecyclingAllocator<std::pair<const GuestAddr, CodePage>>{&m_recycler}};

    /// Single-entry negative memo for invalidate_if_cached_page: the last page
    /// proven to contain no compiled code. Guest writes are overwhelmingly
    /// sequential within one data page, so this turns a per-write hash lookup
    /// into a pointer-sized compare. Cleared whenever m_code_pages gains an
    /// entry, so it cannot report stale "no code here".
    GuestAddr m_no_code_page{0};
    bool m_no_code_page_valid{false};

    /// Monotonic span covering every page ever added to m_code_pages, plus a
    /// generation bumped on each growth. Cheap reject for range_is_code_free();
    /// the page map is the precise test used by JIT stores.
    GuestAddr m_code_span_low{~GuestAddr{0}};
    GuestAddr m_code_span_high{0};
    u64 m_code_gen{0};

    // 1 MiB, one byte per guest page, and one per guest thread. A `new u8[]`
    // here is value-initialized, so every thread creation memset a megabyte
    // and handed the process a megabyte of dirty private memory whether or
    // not that thread ever compiled anything. A launcher process tree runs 100+ guest
    // threads per process, so that alone was ~100 MB of resident waste, plus
    // the large-malloc regions the allocator kept after those threads exited.
    // Mapping it anonymously gives the same guaranteed-zero bytes but only
    // faults in the 4 KiB map pages covering images this thread actually
    // compiled from.
    struct PageMapUnmap {
        void operator()(u8* p) const noexcept;
    };
    std::unique_ptr<u8[], PageMapUnmap> m_code_page_map;

    /// Coarse summary over m_code_page_map: one counter per 2 MiB chunk
    /// (kChunkPages guest pages), holding how many pages in that chunk carry
    /// compiled code. A zero counter proves the whole chunk is code-free, so
    /// range_is_code_free() skips 512 pages at a time instead of stepping one
    /// 4 KiB page at a time.
    ///
    /// This matters because the write-region publish helpers test *whole
    /// regions*, not the accessed address: a multi-megabyte heap region cost
    /// thousands of page loads per publish, and a 32-bit guest that alternates
    /// stores between two buffers (a decompressor's source and destination)
    /// re-publishes — and so re-walked the region — on essentially every store.
    ///
    /// Indices are the same u32-truncated page indices m_code_page_map uses, so
    /// guest addresses at or above 4 GiB alias here exactly as they already do
    /// in the page map. The counter is therefore an exact summary of the page
    /// map, never an independent approximation.
    static constexpr u32 kChunkShift = 9;  // 512 pages == 2 MiB per chunk
    static constexpr usize kChunkCount = kGuestPageCount >> kChunkShift;
    std::unique_ptr<u32[]> m_code_chunk_pages;
    GuestMemory* m_guest_memory{nullptr};

    void evict_lru();
    void add_code_pages(const BasicBlock& block);
    void remove_code_pages(const BasicBlock& block);

    /// Install `block` into the direct-mapped L1 slot for its start_addr.
    /// When the slot already holds a block that strictly covers this start
    /// (overlapping Jcc-first suffix of TEST;Jcc, same l1_index), keep the
    /// cover so JIT linked continues to the earlier entry still L1-hit.
    /// Incoming covers that strictly contain the resident replace it.
    void install_l1(BasicBlock* block) noexcept;

    /// Record a successful hit for chain re-entry shortcuts. Direct-mapped
    /// sticky uses a mix that diverges from L1's low bits so L1 aliases still
    /// hit here. Cleared when the pointed-to block is removed.
    void remember_hit(BasicBlock* block) noexcept;
    void clear_hints_for(const BasicBlock* block) noexcept;

    /// Last successful lookup/insert. Chain self-loops and tight re-entry to
    /// the same start address resolve with one pointer compare before L1.
    BasicBlock* m_last_hit{nullptr};

    /// Small direct-mapped sticky table checked after L1 miss in lookup_fast.
    /// Refills L1 on hit; no LRU splice. Size and index are independent of L1.
    /// Large render workloads keep more than 16 L1-aliasing dispatch targets
    /// active at once; 16 slots made fallback map traversal a hot runtime leaf.
    /// 256 pointers are still only 2 KiB per guest thread.
    static constexpr usize kStickySize = 256;
    BasicBlock* m_sticky[kStickySize]{};

    [[nodiscard]] static constexpr usize sticky_index(GuestAddr addr) noexcept {
        // Sticky exists to catch what L1 drops, so it must index on bits the
        // L1 index does NOT determine -- otherwise an L1 alias is a sticky
        // alias too and the fallback table catches nothing.
        //
        // It used to mix `addr ^ (addr >> 16) ^ (addr >> 32)` against an L1
        // index of `addr & 0xFFFF`, which were genuinely different functions.
        // Once l1_index() began folding bit 16 down (see above), that same
        // expression became the low 8 bits OF the L1 index, and the two
        // degenerated into one function: every L1 collision collided here as
        // well. Caught by LookupFastStickyRetainsAliasingWorkingSet, which
        // went from 0 hash hits to 32.
        //
        // Indexing on the fold's SOURCE bits restores the complement: two
        // addresses share an L1 slot only by differing above bit 15, which is
        // exactly what this reads.
        // With a two-way L1 the set index covers kL1SetBits low bits (mixed
        // with the fold), so sticky must key from bit kL1SetBits upward: two
        // set aliases that differ only in bit 15 must land in different
        // sticky slots, or the fallback thrashes exactly where L1 does.
        return static_cast<usize>(((addr >> kL1SetBits) ^ (addr >> 32)) &
                                  (kStickySize - 1));
    }
};

} // namespace vitreo::cpu::jit
