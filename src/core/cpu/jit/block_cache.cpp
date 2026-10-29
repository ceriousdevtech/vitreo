/// @file block_cache.cpp
/// @brief JIT basic block cache implementation (CPU-035)

#include <sys/mman.h>
#include <cstdio>
#include <cstdlib>
#include "core/cpu/jit/block_cache.hpp"
#include "core/memory/guest_memory.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace vitreo::cpu::jit {

namespace {
constexpr GuestAddr kPageMask = ~static_cast<GuestAddr>(0xFFFu);

// ---- Decoded-IR retention policy counters (see block_cache.hpp) ----
std::atomic<u64> g_ir_releases{0};
std::atomic<u64> g_ir_released_bytes{0};
std::atomic<u64> g_ir_retained_delegate{0};
std::atomic<u64> g_ir_redecodes{0};
std::atomic<u64> g_ir_redecode_failures{0};

std::mutex g_cache_mu;
std::vector<BlockCache*> g_caches;
bool g_visitors_installed = false;

void visit_caches(void (*fn)(u64, u64, u64, u64, u64, u64, void*), void* ctx) {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    for (BlockCache* cache : g_caches) {
        if (cache == nullptr) continue;
        fn(cache->l1_hits(), cache->hash_hits(), cache->total_misses(),
           cache->evictions(), cache->invalidations(), cache->size(), ctx);
    }
}

void visit_compiled(void (*fn)(u64, u64, u32, u32, void*), void* ctx) {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    for (BlockCache* cache : g_caches) {
        if (cache == nullptr) continue;
        cache->for_each_compiled(fn, ctx);
    }
}

void register_cache(BlockCache* cache) {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    if (!g_visitors_installed) {
        diagnostics::set_perf_cache_visitors(&visit_caches, &visit_compiled);
        g_visitors_installed = true;
    }
    g_caches.push_back(cache);
}

void unregister_cache(BlockCache* cache) {
    std::lock_guard<std::mutex> lock(g_cache_mu);
    g_caches.erase(std::remove(g_caches.begin(), g_caches.end(), cache),
                   g_caches.end());
}
}  // namespace

bool release_decoded_enabled() noexcept {
    // DEFAULT ON. The drop condition below is proved from the one flag that
    // decides whether the arena holds `&instructions[i]`, and every reader
    // that can meet a released block either re-decodes or single-steps.
    // VITREO_JIT_RELEASE_DECODED=0 restores unconditional retention for A/B.
    static const bool enabled = [] {
        const char* e = std::getenv("VITREO_JIT_RELEASE_DECODED");
        return !(e != nullptr && e[0] == '0' && e[1] == '\0');
    }();
    return enabled;
}

DecodedReleaseStats decoded_release_stats() noexcept {
    DecodedReleaseStats s;
    s.releases = g_ir_releases.load(std::memory_order_relaxed);
    s.released_bytes = g_ir_released_bytes.load(std::memory_order_relaxed);
    s.retained_delegate =
        g_ir_retained_delegate.load(std::memory_order_relaxed);
    s.redecodes = g_ir_redecodes.load(std::memory_order_relaxed);
    s.redecode_failures =
        g_ir_redecode_failures.load(std::memory_order_relaxed);
    return s;
}

void reset_decoded_release_stats() noexcept {
    g_ir_releases.store(0, std::memory_order_relaxed);
    g_ir_released_bytes.store(0, std::memory_order_relaxed);
    g_ir_retained_delegate.store(0, std::memory_order_relaxed);
    g_ir_redecodes.store(0, std::memory_order_relaxed);
    g_ir_redecode_failures.store(0, std::memory_order_relaxed);
}

namespace {
void ir_release_stats_dump() noexcept {
    const DecodedReleaseStats s = decoded_release_stats();
    std::fprintf(stderr,
                 "[ir-release] releases=%llu bytes=%llu (%.1f MB) "
                 "retained_delegate=%llu redecodes=%llu redecode_fail=%llu\n",
                 static_cast<unsigned long long>(s.releases),
                 static_cast<unsigned long long>(s.released_bytes),
                 static_cast<double>(s.released_bytes) / (1024.0 * 1024.0),
                 static_cast<unsigned long long>(s.retained_delegate),
                 static_cast<unsigned long long>(s.redecodes),
                 static_cast<unsigned long long>(s.redecode_failures));
    std::fflush(stderr);
}

/// VITREO_IR_RELEASE_STATS=1 prints the retention counters at exit and every
/// 20k releases. Kept here rather than in the perf window so the policy is
/// measurable without touching a file another change owns.
bool ir_release_stats_enabled() noexcept {
    static const bool on = std::getenv("VITREO_IR_RELEASE_STATS") != nullptr;
    return on;
}

void ir_release_stats_arm_atexit() noexcept {
    static const bool armed = [] {
        std::atexit(ir_release_stats_dump);
        return true;
    }();
    (void)armed;
}
}  // namespace

void note_decoded_redecode(bool ok) noexcept {
    if (ok) {
        g_ir_redecodes.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_ir_redecode_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

bool release_block_decoded(BasicBlock& block, bool has_baked_pointers) noexcept {
    if (has_baked_pointers) {
        // FAIL CLOSED. Generated code dereferences elements of this exact
        // array, and the cache entry is one of its owners: try_patch_direct_link
        // also republishes it out of the cache entry when a later block links
        // to this one (jit_compiler.cpp, `retained_target_insns`), so the
        // handle is load-bearing even though the compiled record holds a
        // reference of its own.
        g_ir_retained_delegate.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!release_decoded_enabled()) return false;
    const usize count = block.instructions.size();
    if (count == 0) return false;
    // A block whose array does not match its own scalar count is not a shape
    // the re-decode hook can rebuild; leave it alone.
    if (count != block.instruction_count) return false;
    block.instructions.reset();
    block.instructions_released = true;
    const u64 n = g_ir_releases.fetch_add(1, std::memory_order_relaxed) + 1;
    g_ir_released_bytes.fetch_add(
        static_cast<u64>(count) * sizeof(cpu::DecodedInstruction),
        std::memory_order_relaxed);
    if (ir_release_stats_enabled()) {
        ir_release_stats_arm_atexit();
        if ((n % 20000u) == 0u) ir_release_stats_dump();
    }
    return true;
}

namespace {
std::atomic<int> g_reentry_legacy{-1};
}  // namespace

bool reentry_legacy() noexcept {
    int v = g_reentry_legacy.load(std::memory_order_relaxed);
    if (v < 0) {
        const char* e = std::getenv("VITREO_JIT_REENTRY_LEGACY");
        v = (e != nullptr && e[0] == '1' && e[1] == '\0') ? 1 : 0;
        g_reentry_legacy.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

void set_reentry_legacy_for_test(int policy) noexcept {
    g_reentry_legacy.store(policy < 0 ? -1 : (policy != 0 ? 1 : 0),
                           std::memory_order_relaxed);
}

NodeRecycler::~NodeRecycler() {
    for (Bin& bin : m_bins) {
        while (bin.head != nullptr) {
            void* next = *static_cast<void**>(bin.head);
            ::operator delete(bin.head);
            bin.head = next;
        }
    }
}

void* NodeRecycler::take(usize bytes) {
    for (Bin& bin : m_bins) {
        if (bin.bytes != bytes) continue;
        if (bin.head == nullptr) break;
        void* p = bin.head;
        bin.head = *static_cast<void**>(p);
        --bin.count;
        return p;
    }
    return ::operator new(bytes);
}

void NodeRecycler::give(void* p, usize bytes) noexcept {
    if (p == nullptr) return;
    if (bytes >= sizeof(void*) && !reentry_legacy()) {
        for (Bin& bin : m_bins) {
            if (bin.bytes == 0) bin.bytes = bytes;
            if (bin.bytes != bytes) continue;
            if (bin.count >= kMaxPerBin) break;
            *static_cast<void**>(p) = bin.head;
            bin.head = p;
            ++bin.count;
            return;
        }
    }
    ::operator delete(p);
}

void BlockCache::L1Unmap::operator()(L1Entry* p) const noexcept {
    if (p != nullptr) {
        ::munmap(p, kL1Size * sizeof(L1Entry));
    }
}

void BlockCache::PageMapUnmap::operator()(u8* p) const noexcept {
    if (p != nullptr) {
        ::munmap(p, kGuestPageCount);
    }
}

namespace {
void* map_zero_pages(usize bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    return p;
}

BlockCache::L1Entry* map_l1_table(usize bytes) {
    return static_cast<BlockCache::L1Entry*>(map_zero_pages(bytes));
}

/// Zero a whole anonymous mapping and return its physical pages to the system.
/// A `memset` would leave every page resident and dirty; re-mapping the same
/// range in place drops them and restores demand-zero behavior.
void reset_zero_pages(void* base, usize bytes) noexcept {
    if (base == nullptr) return;
    void* p = ::mmap(base, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        // Keep the contract (all-zero) even if the kernel refused the remap.
        std::memset(base, 0, bytes);
    }
}
}  // namespace

BlockCache::BlockCache(usize max_blocks)
    : m_max_blocks(max_blocks)
    , m_l1(map_l1_table(kL1Size * sizeof(L1Entry)))
    , m_code_page_map(static_cast<u8*>(map_zero_pages(kGuestPageCount)))
    , m_code_chunk_pages(std::make_unique<u32[]>(kChunkCount)) {
    // Every lookup path indexes the L1 unconditionally, so a cache without
    // one is not a valid cache. The build is -fno-exceptions, and the
    // make_unique this replaced would have terminated on a failed
    // allocation too; fail loudly rather than null-deref later.
    if (m_l1 == nullptr) {
        std::fprintf(stderr,
                     "[jit] fatal: could not map the %zu-byte block-cache L1\n",
                     kL1Size * sizeof(L1Entry));
        std::abort();
    }
    register_cache(this);
}

BlockCache::~BlockCache() {
    bind_guest_memory(nullptr);
    unregister_cache(this);
}

void BlockCache::bind_guest_memory(GuestMemory* memory) {
    if (m_guest_memory == memory) return;
    if (m_guest_memory) {
        for (const auto& [page, entry] : m_code_pages) {
            (void)entry;
            m_guest_memory->release_jit_code_page(page);
        }
    }
    m_guest_memory = memory;
    if (m_guest_memory) {
        for (const auto& [page, entry] : m_code_pages) {
            (void)entry;
            m_guest_memory->retain_jit_code_page(page);
        }
    }
}

void BlockCache::for_each_compiled(void (*fn)(u64, u64, u32, u32, void*),
                                   void* ctx) const {
    if (fn == nullptr) return;
    for (const auto& block : m_lru) {
        if (block.compiled_code == nullptr) continue;
        fn(block.start_addr, block.execution_count, block.compiled_code_size,
           block.instruction_count, ctx);
    }
}

bool BlockCache::range_is_code_free(GuestAddr start, usize length) const noexcept {
    if (length == 0 || m_code_span_low > m_code_span_high) {
        return true;
    }
    const GuestAddr end = start + length;
    if (end <= m_code_span_low || start >= m_code_span_high) {
        return true;
    }
    if (!m_code_page_map) {
        return false;
    }
    const GuestAddr first = start & kPageMask;
    const GuestAddr last = (end - 1) & kPageMask;
    // Byte span covered by one chunk counter, and the mask that rounds a guest
    // address down to its chunk start.
    constexpr GuestAddr kChunkBytes = GuestAddr{1} << (kChunkShift + 12);
    constexpr GuestAddr kChunkByteMask = kChunkBytes - 1;
    for (GuestAddr page = first;; page += 0x1000) {
        if (page >= (GuestAddr{1} << 32)) {
            for (const auto& [high, entry] : m_code_pages) {
                if (entry.refs != 0 && high >= page && high <= last) return false;
            }
            break;
        }
        const u32 idx = static_cast<u32>(page) >> 12;
        if (m_code_chunk_pages && m_code_chunk_pages[idx >> kChunkShift] == 0) {
            // No compiled code anywhere in this chunk. Jump to its last page so
            // the loop increment lands on the next chunk; if the range ends
            // inside this chunk there is nothing left that could hold code.
            const GuestAddr chunk_last =
                (page & ~kChunkByteMask) + kChunkBytes - 0x1000;
            if (chunk_last >= last) break;
            page = chunk_last;
            continue;
        }
        if (idx < kGuestPageCount && m_code_page_map[idx] != 0) {
            return false;
        }
        if (page == last) break;
    }
    return true;
}

usize BlockCache::l1_way_of(usize idx, const BasicBlock* block) const noexcept {
    for (u32 w = 0; w < kL1Ways; ++w) {
        if (m_l1[idx + w].block == block) return idx + w;
    }
    return static_cast<usize>(-1);
}

usize BlockCache::l1_way_for(usize idx, GuestAddr addr) const noexcept {
    for (u32 w = 0; w < kL1Ways; ++w) {
        const BasicBlock* b = m_l1[idx + w].block;
        if (b != nullptr && b->start_addr == addr) return idx + w;
    }
    return static_cast<usize>(-1);
}

void BlockCache::clear_l1_block(usize idx, const BasicBlock* block) noexcept {
    const usize way = l1_way_of(idx, block);
    if (way != static_cast<usize>(-1)) clear_l1(way);
}

void BlockCache::store_l1(usize idx, BasicBlock* block) noexcept {
    if (block == nullptr) {
        m_l1[idx] = L1Entry{};
        return;
    }
    // Way selection: a block already in the set is refreshed in place;
    // a NEW block always enters way 0 and the set shifts down, so way 0 is
    // the most recently installed and way 1 the older one (which is what an
    // overflow evicts). Filling an empty way 1 first would invert that and
    // evict the newer block on the next overflow.
    usize slot = static_cast<usize>(-1);
    for (u32 w = 0; w < kL1Ways && slot == static_cast<usize>(-1); ++w) {
        if (m_l1[idx + w].block == block) slot = idx + w;
    }
    if (slot == static_cast<usize>(-1)) {
        for (u32 w = kL1Ways - 1; w > 0; --w) m_l1[idx + w] = m_l1[idx + w - 1];
        slot = idx;
    }
    L1Entry& e = m_l1[slot];
    // The tag is always the block's address, even when it has no compiled
    // code yet. Gating the tag on compiled_link_code instead made the slot
    // depend on whether install happened before or after publish, and a
    // block installed while still uncompiled then stayed invisible to the
    // emitted probe. A null entry pointer needs no such help: the probe
    // already tests it (CBZ) and falls back.
    //
    // The invariant that actually matters is the other direction — a slot
    // must never hold a RETIRED entry pointer, because the emitted tail
    // would branch straight into an unmapped arena segment. That is what
    // refresh_l1() at the arena-retire site guarantees.
    e.link_code = block->compiled_link_code;
    e.pin_layout = block->compiled_pin_layout;
    e.block = block;
    e.tag = block->start_addr;
}

void BlockCache::clear_l1(usize idx) noexcept {
    m_l1[idx] = L1Entry{};
}

void BlockCache::sync_l1_payload(usize idx, BasicBlock* block) noexcept {
    // A slot is installed on the first lookup, which happens BEFORE the block
    // is compiled, so its cached entry pointer starts null. The C++ L1 hit
    // path returns without re-installing, so without this the slot would keep
    // its null pointer for the block's whole life and the emitted probe would
    // miss forever — the entire fast path silently dead. Refresh whenever the
    // copies have drifted; the compare keeps a hot dispatcher lookup from
    // dirtying the line on every hit.
    const usize way = l1_way_of(idx, block);
    if (way == static_cast<usize>(-1)) {
        store_l1(idx, block);
        return;
    }
    L1Entry& e = m_l1[way];
    if (e.link_code != block->compiled_link_code ||
        e.pin_layout != block->compiled_pin_layout || e.tag != block->start_addr) {
        e.link_code = block->compiled_link_code;
        e.pin_layout = block->compiled_pin_layout;
        e.tag = block->start_addr;
    }
}

void BlockCache::refresh_l1(const BasicBlock* block) noexcept {
    if (block == nullptr) return;
    const usize idx = l1_index(block->start_addr);
    if (l1_way_of(idx, block) != static_cast<usize>(-1)) {
        store_l1(idx, const_cast<BasicBlock*>(block));
    }
}

void BlockCache::install_l1(BasicBlock* block) noexcept {
    if (block == nullptr) {
        return;
    }
    const usize idx = l1_index(block->start_addr);
    if (block->byte_length != 0) {
        const GuestAddr block_end =
            block->start_addr + static_cast<GuestAddr>(block->byte_length);
        for (u32 w = 0; w < kL1Ways; ++w) {
            BasicBlock* cur = m_l1[idx + w].block;
            if (cur == nullptr || cur == block || cur->byte_length == 0) continue;
            const GuestAddr cur_end =
                cur->start_addr + static_cast<GuestAddr>(cur->byte_length);
            // Resident cover owns this set: incoming is a mid-block / suffix
            // start that shares the set (TEST;Jcc vs Jcc-first).
            if (cur->start_addr < block->start_addr &&
                block->start_addr < cur_end) {
                return;
            }
            // Incoming is the longer cover — displace the suffix resident.
            if (block->start_addr < cur->start_addr &&
                cur->start_addr < block_end) {
                clear_l1(idx + w);
                store_l1(idx, block);
                return;
            }
        }
    }
    store_l1(idx, block);
}

void BlockCache::remember_hit(BasicBlock* block) noexcept {
    if (block == nullptr) return;
    block->cache_referenced = true;
    m_last_hit = block;
    m_sticky[sticky_index(block->start_addr)] = block;
}

void BlockCache::clear_hints_for(const BasicBlock* block) noexcept {
    if (block == nullptr) return;
    if (m_last_hit == block) m_last_hit = nullptr;
    const usize sidx = sticky_index(block->start_addr);
    if (m_sticky[sidx] == block) m_sticky[sidx] = nullptr;
}

BasicBlock* BlockCache::lookup(GuestAddr addr) {
    // L1 fast path: two-way set, no LRU promotion
    usize idx = l1_index(addr);
    if (const usize way = l1_way_for(idx, addr); way != static_cast<usize>(-1)) {
        BasicBlock* fast = m_l1[way].block;
        ++m_hits;
        ++m_l1_hits;
        sync_l1_payload(idx, fast);
        remember_hit(fast);
        return fast;
    }

    // Full hash-map lookup
    auto it = m_map.find(addr);
    if (it == m_map.end()) {
        ++m_misses;
        return nullptr;
    }

    // Promote to MRU and update L1
    m_lru.splice(m_lru.begin(), m_lru, it->second);
    BasicBlock* block = &(*it->second);
    install_l1(block);
    ++m_hits;
    ++m_hash_hits;
    remember_hit(block);
    return block;
}

BasicBlock* BlockCache::lookup_fast(GuestAddr addr) {
    // Last-hit: chain self-loops and consecutive re-entry to the same start
    // pay one pointer compare before touching L1 or the hash map.
    if (m_last_hit && m_last_hit->start_addr == addr) {
        ++m_hits;
        m_last_hit->cache_referenced = true;
        install_l1(m_last_hit);
        return m_last_hit;
    }

    // Same L1 path as lookup(): hot chain re-entries that already sit in
    // the direct map never touched the LRU list.
    usize idx = l1_index(addr);
    if (const usize way = l1_way_for(idx, addr); way != static_cast<usize>(-1)) {
        BasicBlock* fast = m_l1[way].block;
        ++m_hits;
        ++m_l1_hits;
        sync_l1_payload(idx, fast);
        remember_hit(fast);
        return fast;
    }

    // Sticky: survivors of L1 alias thrash. Different index mix than L1, so
    // a pair that keeps displacing each other in L1 still hits here. Refill
    // L1; skip LRU splice (same rationale as sparse promote on hash hits).
    {
        BasicBlock* sticky = m_sticky[sticky_index(addr)];
        if (sticky && sticky->start_addr == addr) {
            ++m_hits;
            sticky->cache_referenced = true;
            install_l1(sticky);
            m_last_hit = sticky;
            return sticky;
        }
    }

    auto it = m_map.find(addr);
    if (it == m_map.end()) {
        ++m_misses;
        return nullptr;
    }

    BasicBlock* block = &(*it->second);
    install_l1(block);
    ++m_hits;
    ++m_hash_hits;
    // PERF: full lookup() splices on every hash hit. One load saw
    // ~245M dispatcher entries with heavy C++ re-entry after L1 alias; the
    // splice dominated. Refresh MRU every 64th hash hit so eviction policy
    // still tracks heat without list surgery on each miss-path hop.
    if ((m_hash_hits & 63u) == 0u) {
        m_lru.splice(m_lru.begin(), m_lru, it->second);
    }
    remember_hit(block);
    return block;
}

BasicBlock* BlockCache::find_covering(GuestAddr addr) {
    const GuestAddr page = addr & kPageMask;
    auto pit = m_code_pages.find(page);
    if (pit == m_code_pages.end()) {
        return nullptr;
    }
    for (GuestAddr start : pit->second.starts) {
        if (start == addr) {
            continue;
        }
        auto mit = m_map.find(start);
        if (mit == m_map.end()) {
            continue;
        }
        BasicBlock* block = &(*mit->second);
        const GuestAddr end =
            start + static_cast<GuestAddr>(block->byte_length);
        if (start < addr && addr < end) {
            return block;
        }
    }
    return nullptr;
}

BasicBlock* BlockCache::insert(BasicBlock block) {
    const GuestAddr addr = block.start_addr;

    // Restore only the same decode mode and span. History is invalidated by
    // guest writes through the same page index as resident translations.
    if (auto saved = m_hotness_map.find(addr); saved != m_hotness_map.end()) {
        const auto& heat = *saved->second;
        if (heat.is_64bit == block.is_64bit &&
            heat.byte_length == block.byte_length) {
            block.execution_count = std::max(block.execution_count,
                                             heat.execution_count);
        }
        forget_hotness(addr);
    }

    if (!reentry_legacy()) {
        // One hash probe for both outcomes. Evicting after the slot exists is
        // safe: the victim is a different, already-listed block, and an
        // unordered_map erase never invalidates another element's iterator.
        auto [slot, fresh] = m_map.try_emplace(addr);
        if (!fresh) {
            remove_code_pages(*slot->second);
            *slot->second = std::move(block);
            add_code_pages(*slot->second);
            m_lru.splice(m_lru.begin(), m_lru, slot->second);
            BasicBlock* cached = &(*slot->second);
            cached->cache_owner = this;
            install_l1(cached);
            remember_hit(cached);
            cached->cache_referenced = false;
            return cached;
        }
        if (m_lru.size() >= m_max_blocks) {
            evict_lru();
        }
        m_lru.push_front(std::move(block));
        slot->second = m_lru.begin();
        add_code_pages(m_lru.front());
        BasicBlock* cached = &m_lru.front();
        cached->cache_owner = this;
        install_l1(cached);
        remember_hit(cached);
        cached->cache_referenced = false;
        return cached;
    }

    // If already present, update in place and promote
    auto it = m_map.find(addr);
    if (it != m_map.end()) {
        remove_code_pages(*it->second);
        *it->second = std::move(block);
        add_code_pages(*it->second);
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        BasicBlock* cached = &(*it->second);
        cached->cache_owner = this;
        install_l1(cached);
        remember_hit(cached);
        cached->cache_referenced = false;
        return cached;
    }

    // Evict LRU if at capacity
    if (m_lru.size() >= m_max_blocks) {
        evict_lru();
    }

    // Insert at front (MRU)
    m_lru.push_front(std::move(block));
    m_map[addr] = m_lru.begin();
    add_code_pages(m_lru.front());
    BasicBlock* cached = &m_lru.front();
    cached->cache_owner = this;
    install_l1(cached);
    remember_hit(cached);
    cached->cache_referenced = false;
    return cached;
}

void l1_note_published(BasicBlock& block) noexcept {
    if (block.cache_owner != nullptr) {
        block.cache_owner->refresh_l1(&block);
    }
}

bool BlockCache::invalidate(GuestAddr addr) {
    forget_hotness(addr);
    auto it = m_map.find(addr);
    if (it == m_map.end()) {
        return false;
    }

    auto& block = *it->second;
    clear_l1_block(l1_index(addr), &block);
    clear_hints_for(&block);
    remove_code_pages(block);
    m_lru.erase(it->second);
    m_map.erase(it);
    ++m_invalidations;
    return true;
}

void BlockCache::collect_containing(GuestAddr rip,
                                    std::vector<GuestAddr>& out) const {
    const GuestAddr page = rip & kPageMask;
    for (int k = 0; k < 2; ++k) {
        if (k == 1 && page < 0x1000) break;
        const GuestAddr p = page - static_cast<GuestAddr>(k) * 0x1000;
        auto pit = m_code_pages.find(p);
        if (pit == m_code_pages.end()) continue;
        for (GuestAddr start : pit->second.starts) {
            auto mit = m_map.find(start);
            if (mit == m_map.end()) continue;
            const GuestAddr end =
                start + static_cast<GuestAddr>(mit->second->byte_length);
            if (start <= rip && rip < end &&
                std::find(out.begin(), out.end(), start) == out.end()) {
                out.push_back(start);
            }
        }
    }
}

usize BlockCache::invalidate_range(GuestAddr start, usize length,
                                   std::vector<GuestAddr>* removed_out,
                                   std::vector<GuestAddr>* compiled_removed_out) {
    if (length == 0) {
        return 0;
    }
    const GuestAddr range_end = start + static_cast<GuestAddr>(length);
    const GuestAddr first_page = start & kPageMask;
    const GuestAddr last_page = (range_end - 1) & kPageMask;

    // Gather candidate block starts from the page index (deduped). Avoids
    // walking the entire LRU on every SMC write — critical once the cache is
    // full during a long installer file copy.
    std::unordered_set<GuestAddr> candidates;
    for (GuestAddr page = first_page;; page += 0x1000) {
        auto pit = m_code_pages.find(page);
        if (pit != m_code_pages.end()) {
            candidates.insert(pit->second.starts.begin(),
                              pit->second.starts.end());
        }
        if (page == last_page) {
            break;
        }
    }

    usize removed = 0;
    for (GuestAddr block_start : candidates) {
        auto mit = m_map.find(block_start);
        if (mit == m_map.end()) {
            auto saved = m_hotness_map.find(block_start);
            if (saved != m_hotness_map.end()) {
                const auto& heat = *saved->second;
                if (block_start < range_end &&
                    block_start + heat.byte_length > start) {
                    forget_hotness(block_start);
                }
            }
            continue;
        }
        auto list_it = mit->second;
        const GuestAddr block_end =
            block_start + static_cast<GuestAddr>(list_it->byte_length);
        if (!(block_start < range_end && block_end > start)) {
            continue;
        }

        clear_l1_block(l1_index(block_start), &(*list_it));
        clear_hints_for(&(*list_it));
        if (compiled_removed_out && list_it->compiled_code != nullptr) {
            compiled_removed_out->push_back(block_start);
        }
        remove_code_pages(*list_it);
        m_lru.erase(list_it);
        m_map.erase(mit);
        if (removed_out) {
            removed_out->push_back(block_start);
        }
        ++removed;
    }

    m_invalidations += removed;
    return removed;
}

usize BlockCache::invalidate_if_cached_page(GuestAddr start, usize length,
                                            std::vector<GuestAddr>* removed_out,
                                            std::vector<GuestAddr>* compiled_removed_out) {
    if (length == 0) return 0;
    const GuestAddr end = start + static_cast<GuestAddr>(length - 1);
    const GuestAddr first_page = start & kPageMask;
    const GuestAddr last_page = end & kPageMask;

    // This runs on every guest memory write, and the overwhelming majority of
    // writes are to data pages holding no code at all. A decompression or
    // memcpy inner loop walks sequentially within one page, so the same page is
    // tested millions of times in a row and the hash lookup below is repeated
    // for an answer that cannot have changed.
    //
    // Remember the last single page proven to hold no code. add_code_pages
    // clears it, so the memo can never outlive the fact it caches.
    if (first_page == last_page && m_no_code_page_valid &&
        first_page == m_no_code_page) {
        return 0;
    }

    for (GuestAddr page = first_page;; page += 0x1000) {
        if (m_code_pages.contains(page)) {
            return invalidate_range(start, length, removed_out, compiled_removed_out);
        }
        if (page == last_page) break;
    }

    if (first_page == last_page) {
        m_no_code_page = first_page;
        m_no_code_page_valid = true;
    }
    return 0;
}

usize BlockCache::drop_compiled_older_than(u64 generation) {
    return drop_compiled_if([generation](const BasicBlock& block) {
        return block.compiled_arena_generation < generation;
    });
}

usize BlockCache::drop_compiled_if(
    const std::function<bool(const BasicBlock&)>& retired) {
    usize dropped = 0;
    // Entries left with neither compiled code nor a usable decode cannot be
    // executed by anything: both JitCompiler::compile and the interpreter
    // block path walk [0, instruction_count) through `instructions`.
    // `publish_compiled` releases the decoded array for a fully-native block
    // (its generated code no longer refers to it) while leaving
    // instruction_count set, so dropping that block's code here would make it
    // recompilable with a null array — a host null dereference on the next
    // execution. Such an entry has to go, so the next execution re-decodes.
    std::vector<GuestAddr> undecodable;
    for (BasicBlock& block : m_lru) {
        if (block.compiled_code == nullptr || !retired(block)) {
            continue;
        }
        // The decode stays: the guest bytes are unchanged, only the arena
        // segment holding this block's ARM64 went away. Clearing every
        // published entry pointer stops a JIT-emitted L1 tail from branching
        // into an unmapped segment.
        block.compiled_code = nullptr;
        block.compiled_link_code = nullptr;
        block.compiled_call_code = nullptr;
        block.compiled_pin_layout = 0;
        // The L1 slot caches this block's entry pointer, so nulling the
        // field is no longer enough on its own: a slot still holding the
        // retired pointer would send an emitted tail branching straight into
        // an unmapped arena segment. This is the one direction of the cache
        // that is a crash rather than a missed fast path.
        refresh_l1(&block);
        block.compiled_code_size = 0;
        block.compiled_arena_generation = 0;
        block.compiled_native_insns = 0;
        block.compiled_hot_dump_variant = ~u32{0};
        block.compiled_requires_protection_lease = false;
        block.compile_failed = false;
        ++dropped;
        if (block.instructions.size() != block.instruction_count) {
            undecodable.push_back(block.start_addr);
        }
    }
    for (const GuestAddr addr : undecodable) {
        (void)invalidate(addr);
    }
    return dropped;
}

void BlockCache::clear() {
    if (m_guest_memory) {
        for (const auto& [page, entry] : m_code_pages) {
            (void)entry;
            m_guest_memory->release_jit_code_page(page);
        }
    }
    // Re-map rather than fill: a 16 MB table would otherwise be made
    // resident and dirty by the clear itself.
    reset_zero_pages(m_l1.get(), kL1Size * sizeof(L1Entry));
    m_last_hit = nullptr;
    std::fill_n(m_sticky, kStickySize, nullptr);
    m_lru.clear();
    m_map.clear();
    m_hotness.clear();
    m_hotness_map.clear();
    m_code_pages.clear();
    m_no_code_page_valid = false;
    reset_zero_pages(m_code_page_map.get(), kGuestPageCount);
    if (m_code_chunk_pages) {
        std::memset(m_code_chunk_pages.get(), 0, kChunkCount * sizeof(u32));
    }
    // The span may only shrink here, which cannot invalidate a published
    // write-region — but bump anyway so consumers re-validate rather than
    // trusting a span that no longer describes the same cache contents.
    m_code_span_low = ~GuestAddr{0};
    m_code_span_high = 0;
    ++m_code_gen;
}

usize BlockCache::size() const noexcept {
    return m_lru.size();
}

usize BlockCache::capacity() const noexcept {
    return m_max_blocks;
}

u64 BlockCache::total_hits() const noexcept {
    return m_hits;
}

u64 BlockCache::total_misses() const noexcept {
    return m_misses;
}

u64 BlockCache::l1_hits() const noexcept {
    return m_l1_hits;
}

u64 BlockCache::hash_hits() const noexcept {
    return m_hash_hits;
}

u64 BlockCache::evictions() const noexcept {
    return m_evictions;
}

u64 BlockCache::invalidations() const noexcept {
    return m_invalidations;
}

void BlockCache::evict_lru() {
    if (m_lru.empty()) {
        return;
    }

    // Fast paths do not splice the LRU list. Give their referenced entries
    // one second chance instead of repeatedly evicting hot translations.
    // Each iteration clears a bit; even an entirely hot cache takes at most
    // size() iterations before selecting a victim. Capacity stays bounded.
    while (m_lru.back().cache_referenced) {
        auto recent = std::prev(m_lru.end());
        recent->cache_referenced = false;
        m_lru.splice(m_lru.begin(), m_lru, recent);
    }
    auto& back = m_lru.back();
    clear_l1_block(l1_index(back.start_addr), &back);
    clear_hints_for(&back);
    if (back.compiled_code == nullptr && !back.compile_failed &&
        back.execution_count != 0 && back.byte_length != 0) {
        remember_hotness(back); // transfers the existing code-page references
    } else {
        remove_code_pages(back);
    }
    m_map.erase(back.start_addr);
    m_lru.pop_back();
    ++m_evictions;
}

void BlockCache::forget_hotness(GuestAddr addr) {
    auto saved = m_hotness_map.find(addr);
    if (saved == m_hotness_map.end()) return;
    BasicBlock span;
    span.start_addr = saved->second->start_addr;
    span.byte_length = saved->second->byte_length;
    remove_code_pages(span);
    m_hotness.erase(saved->second);
    m_hotness_map.erase(saved);
}

void BlockCache::remember_hotness(const BasicBlock& block) {
    // Four times the decoded cache covers revisit distances larger than its
    // LRU while bounding per-thread metadata. Older history becomes cold.
    const usize capacity = m_max_blocks > (~usize{0} / 4)
        ? m_max_blocks : std::max(usize{1}, m_max_blocks * 4);
    while (m_hotness.size() >= capacity) {
        forget_hotness(m_hotness.back().start_addr);
    }
    m_hotness.push_front({block.start_addr, block.byte_length,
                          block.is_64bit, block.execution_count});
    m_hotness_map.emplace(block.start_addr, m_hotness.begin());
}

void BlockCache::add_code_pages(const BasicBlock& block) {
    if (block.byte_length == 0) return;
    // A page that previously held no code may now hold some, so the negative
    // memo in invalidate_if_cached_page must not survive this.
    m_no_code_page_valid = false;
    const bool legacy = reentry_legacy();
    const GuestAddr last = block.start_addr + block.byte_length - 1;
    for (GuestAddr page = block.start_addr & kPageMask;; page += 0x1000) {
        CodePage& entry = m_code_pages[page];
        if (entry.refs++ == 0 && m_guest_memory) {
            m_guest_memory->retain_jit_code_page(page);
        }
        // insert() is the only caller, and it has already dropped any
        // resident block or hotness span with this start, so the start is
        // never on the page yet. Scanning for it cost a walk of every start
        // on a dense page (~200 for 20-byte blocks) per re-entry.
        if (!legacy || std::find(entry.starts.begin(), entry.starts.end(),
                                 block.start_addr) == entry.starts.end()) {
            entry.starts.push_back(block.start_addr);
        }
        if (page < (GuestAddr{1} << 32)) {
            const u32 idx = static_cast<u32>(page) >> 12;
            if (m_code_page_map[idx] == 0) {
                m_code_page_map[idx] = 1;
                ++m_code_chunk_pages[idx >> kChunkShift];
            }
        }
        if (page == (last & kPageMask)) break;
    }
    // Grow the conservative code span. Any JIT write-region published before
    // this point may now overlap compiled code, so bump the generation to
    // retract it (see JitCompiler::execute).
    const GuestAddr lo = block.start_addr & kPageMask;
    const GuestAddr hi = (last & kPageMask) + 0x1000;
    if (lo < m_code_span_low || hi > m_code_span_high) {
        if (lo < m_code_span_low) m_code_span_low = lo;
        if (hi > m_code_span_high) m_code_span_high = hi;
        ++m_code_gen;
    }
}

void BlockCache::remove_code_pages(const BasicBlock& block) {
    // The old path remains selectable for same-binary measurements. Every
    // start is unique: insert removes an existing block/history before adding
    // its new span, and add_code_pages rejects duplicate page membership.
    static const bool find_erase = [] {
        const char* value = std::getenv("VITREO_JIT_PAGE_ERASE_FIND");
        return value == nullptr || std::strcmp(value, "1") == 0;
    }();
    if (block.byte_length == 0) return;
    const bool legacy = reentry_legacy();
    const GuestAddr last = block.start_addr + block.byte_length - 1;
    for (GuestAddr page = block.start_addr & kPageMask;; page += 0x1000) {
        auto it = m_code_pages.find(page);
        if (it != m_code_pages.end()) {
            auto& starts = it->second.starts;
            if (!legacy) {
                // Nothing reads the order, so fill the hole from the back
                // instead of shifting a dense page's whole tail down.
                const auto hit =
                    std::find(starts.begin(), starts.end(), block.start_addr);
                if (hit != starts.end()) {
                    *hit = starts.back();
                    starts.pop_back();
                }
            } else if (starts.capacity() >= 128 && find_erase) {
                // Small vectors favor the existing inline compaction loop.
                // Use retained capacity so a dense page keeps one strategy as
                // its membership shrinks during eviction; sparse pages stay
                // on the same path in both measurement arms.
                const auto start = std::find(starts.begin(), starts.end(), block.start_addr);
                if (start != starts.end()) starts.erase(start);
            } else {
                starts.erase(std::remove(starts.begin(), starts.end(),
                                         block.start_addr), starts.end());
            }
            if (--it->second.refs == 0) {
                if (m_guest_memory) {
                    m_guest_memory->release_jit_code_page(page);
                }
                m_code_pages.erase(it);
                if (page < (GuestAddr{1} << 32)) {
                    const u32 idx = static_cast<u32>(page) >> 12;
                    if (m_code_page_map[idx] != 0) {
                        m_code_page_map[idx] = 0;
                        --m_code_chunk_pages[idx >> kChunkShift];
                    }
                }
            }
        }
        if (page == (last & kPageMask)) break;
    }
}

} // namespace vitreo::cpu::jit
