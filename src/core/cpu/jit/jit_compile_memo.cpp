// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compile_memo.cpp
/// @brief Process-wide compiled translation store and invalidation ownership.

#include "jit_compile_memo.hpp"
#include "jit_code_arena.hpp"
#include "jit_block.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

namespace vitreo::cpu::jit {

// Process-wide compiled-block memo (Stage A2).
std::array<JitCompileMemo::Shard, JitCompileMemo::kMemoShards> JitCompileMemo::s_shards;
std::atomic<u64>                                               JitCompileMemo::s_max_span_pages{0};
std::atomic<bool>                                              JitCompileMemo::s_compiled_memo_enabled{false};

namespace {
std::atomic<u64> g_shared_smc_purges{0};

using Shard = JitCompileMemo::Shard;

/// Every shard lock held exclusively, taken in index order (the only order
/// any path takes more than one shard lock in).
struct AllShardsExclusive {
    AllShardsExclusive() noexcept {
        for (Shard& shard : JitCompileMemo::s_shards) shard.lock.lock();
    }
    ~AllShardsExclusive() {
        for (usize i = JitCompileMemo::kMemoShards; i-- > 0;) {
            JitCompileMemo::s_shards[i].lock.unlock();
        }
    }
    AllShardsExclusive(const AllShardsExclusive&) = delete;
    AllShardsExclusive& operator=(const AllShardsExclusive&) = delete;
};

void note_span_pages(u64 span) noexcept {
    u64 seen = JitCompileMemo::s_max_span_pages.load(std::memory_order_relaxed);
    while (span > seen &&
           !JitCompileMemo::s_max_span_pages.compare_exchange_weak(
               seen, span, std::memory_order_seq_cst,
               std::memory_order_relaxed)) {
    }
}
}  // namespace

u64 shared_table_smc_purges() noexcept {
    return g_shared_smc_purges.load(std::memory_order_relaxed);
}
void reset_shared_table_smc_purges() noexcept {
    g_shared_smc_purges.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Shared compiled-block table maintenance.
//
// JitCompileMemo::s_shards hold the process-wide table of every published
// translation, keyed by guest start address and sharded by start page; the
// per-thread BlockCache is an L1 over it. Everything here runs under the
// owning shard's lock held exclusively, and every removal goes through
// release_record_locked so the shard's page index, the page retains and the
// retained-IR budget can never disagree with its primary map.
// ---------------------------------------------------------------------------

void JitCompileMemo::retain_record_pages(const CompiledKey& key,
                                         GuestMemory* memory) noexcept {
    if (memory == nullptr || key.guest_size == 0) return;
    const GuestAddr last_page =
        (key.start_addr + key.guest_size - 1) & ~GuestAddr{0xFFF};
    for (GuestAddr page = key.start_addr & ~GuestAddr{0xFFF};;
         page += 0x1000) {
        memory->retain_jit_code_page(page);
        if (page == last_page) break;
    }
}

void JitCompileMemo::release_record_pages(const CompiledKey& key,
                                          GuestMemory* memory) noexcept {
    if (memory == nullptr || key.guest_size == 0) return;
    const GuestAddr last_page =
        (key.start_addr + key.guest_size - 1) & ~GuestAddr{0xFFF};
    for (GuestAddr page = key.start_addr & ~GuestAddr{0xFFF};;
         page += 0x1000) {
        memory->release_jit_code_page(page);
        if (page == last_page) break;
    }
}

void JitCompileMemo::index_compiled_record_locked(Shard& shard,
                                                  const JitCompileMemo::CompiledKey& key,
                                                  JitCompileMemo::CompiledRecord& record,
                                                  bool pages_retained) noexcept {
    static std::once_flag teardown_hook_once;
    std::call_once(teardown_hook_once, [] {
        GuestMemory::set_teardown_hook(&JitCompileMemo::forget_guest_memory);
    });
    shard.records.store(shard.records.load(std::memory_order_relaxed) + 1,
                        std::memory_order_relaxed);
    if (key.guest_size == 0) return;
    const GuestAddr last = key.start_addr + key.guest_size - 1;
    const GuestAddr first_page = key.start_addr & ~GuestAddr{0xFFF};
    const GuestAddr last_page = last & ~GuestAddr{0xFFF};
    note_span_pages((last_page - first_page) >> 12);
    for (GuestAddr page = first_page;; page += 0x1000) {
        auto& starts = shard.pages[page];
        if (std::find(starts.begin(), starts.end(), key.start_addr) ==
            starts.end()) {
            starts.push_back(key.start_addr);
        }
        // The record, not any thread's cache entry, owns the page's "holds
        // compiled code" mark: a thread that compiled the block may evict it
        // or exit while the translation stays adoptable by everyone else.
        if (record.memory && !pages_retained) {
            record.memory->retain_jit_code_page(page);
        }
        if (page == last_page) break;
    }
}

void JitCompileMemo::release_record_locked(Shard& shard, const CompiledKey& key,
                                           CompiledRecord& record,
                                           bool last_at_addr) noexcept {
    if (key.guest_size != 0) {
        const GuestAddr last = key.start_addr + key.guest_size - 1;
        for (GuestAddr page = key.start_addr & ~GuestAddr{0xFFF};;
             page += 0x1000) {
            if (last_at_addr) {
                if (auto pit = shard.pages.find(page); pit != shard.pages.end()) {
                    auto& starts = pit->second;
                    starts.erase(std::remove(starts.begin(), starts.end(),
                                             key.start_addr),
                                 starts.end());
                    if (starts.empty()) shard.pages.erase(pit);
                }
            }
            if (record.memory) record.memory->release_jit_code_page(page);
            if (page == (last & ~GuestAddr{0xFFF})) break;
        }
    }
    if (record.ir_bytes != 0) memo_ir_release(record.ir_bytes);
    if (const usize n = shard.records.load(std::memory_order_relaxed); n != 0) {
        shard.records.store(n - 1, std::memory_order_relaxed);
    }
}

template <typename F>
void JitCompileMemo::erase_records_locked(Shard& shard, F&& retire) noexcept {
    for (auto it = shard.compiled.begin(); it != shard.compiled.end();) {
        CompiledList& list = it->second;
        // Decide first, then release: whether a record is the last at its
        // address depends on how many of its siblings survive.
        usize surviving = list.size();
        list.for_each([&](CompiledEntry& e) {
            if (retire(e)) --surviving;
        });
        if (surviving == list.size()) {
            ++it;
            continue;
        }
        list.erase_if([&](CompiledEntry& e) {
            if (!retire(e)) return false;
            release_record_locked(shard, e.key, e.record, surviving == 0);
            return true;
        });
        if (list.empty()) {
            it = shard.compiled.erase(it);
        } else {
            ++it;
        }
    }
}

void JitCompileMemo::purge_memo_for_retired_chunks(
    const std::vector<usize>& victims) {
    // Records are purged by chunk, not by generation, so a record inserted
    // concurrently under the old generation (a compile that was mid-flight) is
    // caught too; finalize_code() additionally refuses to insert once the
    // chunk generation no longer matches. The chunks' generations moved
    // before this call, so per shard an insert either takes the shard lock
    // before this pass does (and is erased here) or after (and sees the
    // retirement): every shard must be write-locked, but not all at once.
    for (Shard& shard : s_shards) {
        std::unique_lock<JitMemoLock> memo_lock(shard.lock);
        erase_records_locked(shard, [&](const CompiledEntry& e) {
            const void* code = e.record.code;
            if (code == nullptr) return false;
            const usize index = JitCodeArena::arena_chunk_index(code);
            // Releases the record's page retains and IR budget too, so a
            // retired translation cannot be adopted by anyone.
            if (std::find(victims.begin(), victims.end(), index) ==
                victims.end()) {
                return false;
            }
            // Retirement-thrash accounting, for translations no thread still
            // caches (a per-thread cache evicts long before the memo does).
            // Idempotent: the predicate runs twice per record.
            JitCodeArena::note_retired_translation(e.key.start_addr,
                                                   e.key.code_hash);
            return true;
        });
    }
}

void JitCompileMemo::clear_compile_memo() noexcept {
    AllShardsExclusive all;
    for (Shard& shard : s_shards) {
        erase_records_locked(shard, [](const CompiledEntry&) { return true; });
        shard.compiled.clear();
        shard.pages.clear();
        shard.smc_rewrites.clear();
        shard.records.store(0, std::memory_order_relaxed);
        shard.smc_tracked.store(0, std::memory_order_release);
    }
    s_max_span_pages.store(0, std::memory_order_seq_cst);
    // Every record is gone, so the retained-IR budget is fully released even
    // if some record's accounting drifted.
    memo_ir_reset();
}

usize JitCompileMemo::compile_memo_size() noexcept {
    usize total = 0;
    for (const Shard& shard : s_shards) {
        total += shard.records.load(std::memory_order_relaxed);
    }
    return total;
}

void JitCompileMemo::forget_guest_memory(GuestMemory* memory) noexcept {
    if (memory == nullptr) return;
    AllShardsExclusive all;
    // The normal case is process exit, where the dying address space owns
    // every record. Drop the tables wholesale then. Erasing record by record
    // costs O(blocks on the page) per record in the page index, which is
    // quadratic: a launcher killed with SIGTERM spent minutes here
    // (sampled inside std::remove in the per-record erase). The result is
    // the same as the loop below. The page table only ever holds entries
    // for indexed records, the pages are unmapped with the address space so
    // there is nothing to release into, and the IR budget is returned in one
    // step.
    usize owned_ir_bytes = 0;
    bool owns_every_record = true;
    for (const Shard& shard : s_shards) {
        for (const auto& [addr, list] : shard.compiled) {
            (void)addr;
            list.for_each([&](const CompiledEntry& e) {
                if (e.record.memory != memory) owns_every_record = false;
                owned_ir_bytes += e.record.ir_bytes;
            });
            if (!owns_every_record) break;
        }
        if (!owns_every_record) break;
    }
    if (owns_every_record) {
        if (owned_ir_bytes != 0) memo_ir_release(owned_ir_bytes);
        for (Shard& shard : s_shards) {
            shard.compiled.clear();
            shard.pages.clear();
            shard.records.store(0, std::memory_order_relaxed);
        }
        return;
    }
    for (Shard& shard : s_shards) {
        erase_records_locked(shard, [&](CompiledEntry& e) {
            if (e.record.memory != memory) return false;
            // The pages are being unmapped with the address space; there is
            // nothing to release into.
            e.record.memory = nullptr;
            return true;
        });
    }
}

bool JitCompileMemo::shared_smc_demoted(GuestAddr addr) noexcept {
    if (!JitCompileMemo::s_compiled_memo_enabled.load(std::memory_order_acquire)) return false;
    Shard& shard = shard_for(addr);
    if (shard.smc_tracked.load(std::memory_order_acquire) == 0) return false;
    std::shared_lock<JitMemoLock> lock(shard.lock);
    const auto it = shard.smc_rewrites.find(addr);
    return it != shard.smc_rewrites.end() && it->second.count >= JitCompileMemo::kChurnRecompileLimit;
}

namespace {
// -1 until first read from VITREO_JIT_MEMO_LEGACY_LOCK.
std::atomic<int> g_legacy_memo_lock{-1};
}  // namespace

bool JitCompileMemo::legacy_memo_locking() noexcept {
    int mode = g_legacy_memo_lock.load(std::memory_order_relaxed);
    if (mode < 0) {
        mode = environment_flag_enabled("VITREO_JIT_MEMO_LEGACY_LOCK") ? 1 : 0;
        g_legacy_memo_lock.store(mode, std::memory_order_relaxed);
    }
    return mode != 0;
}

void JitCompileMemo::set_legacy_memo_locking_for_test(int mode) noexcept {
    g_legacy_memo_lock.store(mode < 0 ? -1 : (mode != 0 ? 1 : 0),
                             std::memory_order_relaxed);
}

bool JitCompileMemo::accept_smc_compilation(
    GuestAddr addr, u64 code_hash, bool& tracked) noexcept {
    Shard& shard = shard_for(addr);
    if (!legacy_memo_locking()) {
        // A history is only ever created by a purge, under the exclusive
        // lock; with none in this shard the address is untracked, which is
        // what the locked lookup would have answered at this instant.
        if (shard.smc_tracked.load(std::memory_order_acquire) == 0) {
            tracked = false;
            return true;
        }
        std::shared_lock<JitMemoLock> rlock(shard.lock);
        const auto it = shard.smc_rewrites.find(addr);
        tracked = it != shard.smc_rewrites.end();
        if (!tracked) return true;
        const SmcRewriteCount& history = it->second;
        if (history.count >= kChurnRecompileLimit) return false;
        if (history.have_code_hash && history.last_code_hash == code_hash) {
            return true;
        }
    }
    std::unique_lock<JitMemoLock> lock(shard.lock);
    const auto it = shard.smc_rewrites.find(addr);
    tracked = it != shard.smc_rewrites.end();
    if (!tracked) return true;
    auto& history = it->second;
    if (history.count >= kChurnRecompileLimit) return false;
    if (history.have_code_hash && history.last_code_hash != code_hash) {
        if (++history.count == kChurnRecompileLimit) smc_note_demote();
    }
    history.last_code_hash = code_hash;
    history.have_code_hash = true;
    return history.count < kChurnRecompileLimit;
}

bool JitCompileMemo::publish_record_impl(const CompiledKey& key,
                                         const void* code, u64 generation,
                                         usize ir_bytes, BuildRecordFn build,
                                         void* ctx) {
    Shard& shard = shard_for(key.start_addr);
    if (legacy_memo_locking()) {
        std::unique_lock<JitMemoLock> wlock(shard.lock);
        CompiledList& list = shard.compiled[key.start_addr];
        if (list.find(key) == nullptr &&
            JitCodeArena::arena_code_live(code, generation) &&
            (ir_bytes == 0 || memo_ir_admit(ir_bytes))) {
            CompiledRecord record{};
            build(ctx, record);
            CompiledEntry& stored = list.push(key, std::move(record));
            index_compiled_record_locked(shard, key, stored.record,
                                         /*pages_retained=*/false);
            return true;
        }
        // The probe above created the node; do not leave an empty list
        // behind for the cold-path probe to find.
        if (list.empty()) shard.compiled.erase(key.start_addr);
        return false;
    }

    // Refusals are monotonic (a published key stays until purged, a retired
    // generation never comes back), so a shared probe that refuses is final.
    bool have_list = false;
    {
        std::shared_lock<JitMemoLock> rlock(shard.lock);
        if (!JitCodeArena::arena_code_live(code, generation)) return false;
        if (const auto lit = shard.compiled.find(key.start_addr);
            lit != shard.compiled.end()) {
            if (lit->second.find(key) != nullptr) return false;
            have_list = true;
        }
    }

    // Declared before the lock so a refused record and an unused node are
    // freed after it is released.
    CompiledRecord record{};
    build(ctx, record);
    decltype(Shard::compiled)::node_type node;
    if (!have_list) {
        decltype(Shard::compiled) scratch;
        scratch.try_emplace(key.start_addr);
        node = scratch.extract(scratch.begin());
    }

    // Mark the record's guest pages as holding code BEFORE taking the write
    // lock, so the code-page shards (and any JIT-view mprotect a first retain
    // performs) are never entered while this memo shard is write-held.
    //
    // Ordering: retain_jit_code_page() has fully published each page's mark
    // (span, map byte, check bit, JIT-view protection) when it returns, and
    // the record only becomes reachable by the insert below, under the write
    // lock that every adopter acquires. So, as before, no thread can reach
    // this translation while a store into its bytes could bypass the SMC
    // helper. Marking early only over-reports for the window up to the insert
    // (a store merely takes the checked helper); a refused insert drops
    // exactly the references taken here, so every page's count still equals
    // the number of live records (and cache entries) that retained it.
    GuestMemory* const memory = record.memory;
    retain_record_pages(key, memory);
    bool inserted = false;
    {
        std::unique_lock<JitMemoLock> wlock(shard.lock);
        // purge_memo_for_retired_chunks write-locks this shard after the
        // chunk's generation moved: checked here, the insert either precedes
        // that purge (which erases it) or observes the retirement.
        auto lit = shard.compiled.find(key.start_addr);
        if (JitCodeArena::arena_code_live(code, generation) &&
            (lit == shard.compiled.end() || lit->second.find(key) == nullptr) &&
            (ir_bytes == 0 || memo_ir_admit(ir_bytes))) {
            if (lit == shard.compiled.end()) {
                lit = node.empty()
                          ? shard.compiled.try_emplace(key.start_addr).first
                          : shard.compiled.insert(std::move(node)).position;
            }
            CompiledEntry& stored = lit->second.push(key, std::move(record));
            index_compiled_record_locked(shard, key, stored.record,
                                         /*pages_retained=*/true);
            inserted = true;
        }
    }
    // The compiling thread's GuestMemory outlives its compile, so the pages
    // can be released after the lock.
    if (!inserted) release_record_pages(key, memory);
    return inserted;
}

usize JitCompileMemo::purge_shard_overlapping(Shard& shard, GuestAddr addr,
                                              GuestAddr end,
                                              GuestAddr first_page,
                                              GuestAddr last_page,
                                              u64 generation) noexcept {
    // Shared probe first: executable writes to data on RWX pages are the
    // common case and must not serialise every thread on the write lock.
    std::vector<JitCompileMemo::CompiledKey> victims;
    {
        std::shared_lock<JitMemoLock> rlock(shard.lock);
        if (shard.pages.empty()) return 0;
        auto collect = [&](const std::vector<GuestAddr>& starts) {
            for (const GuestAddr start : starts) {
                auto lit = shard.compiled.find(start);
                if (lit == shard.compiled.end()) continue;
                lit->second.for_each([&](const CompiledEntry& e) {
                    const GuestAddr key_end = e.key.start_addr + e.key.guest_size;
                    if (e.key.start_addr < end && key_end > addr) {
                        victims.push_back(e.key);
                    }
                });
            }
        };
        const u64 range_pages = ((last_page - first_page) >> 12) + 1;
        if (range_pages > shard.pages.size()) {
            // A large write over a sparse shard: walk the shard's pages.
            for (const auto& [page, starts] : shard.pages) {
                if (page >= first_page && page <= last_page) collect(starts);
            }
        } else {
            for (GuestAddr page = first_page;; page += 0x1000) {
                if (auto pit = shard.pages.find(page); pit != shard.pages.end()) {
                    collect(pit->second);
                }
                if (page == last_page) break;
            }
        }
    }
    if (victims.empty()) return 0;
    usize erased = 0;
    std::unique_lock<JitMemoLock> wlock(shard.lock);
    for (const JitCompileMemo::CompiledKey& key : victims) {
        auto lit = shard.compiled.find(key.start_addr);
        if (lit == shard.compiled.end()) continue;
        CompiledList& list = lit->second;
        if (list.find(key) == nullptr) continue;
        auto [hit, inserted] = shard.smc_rewrites.try_emplace(key.start_addr);
        if (inserted) {
            shard.smc_tracked.store(shard.smc_tracked.load(std::memory_order_relaxed) + 1,
                                    std::memory_order_release);
        }
        auto& history = hit->second;
        if (!history.have_code_hash) {
            history.last_code_hash = key.code_hash;
            history.have_code_hash = true;
        }
        history.last_generation = std::max(history.last_generation, generation);
        const bool last_at_addr = list.size() == 1;
        list.erase_if([&](CompiledEntry& e) {
            if (!(e.key == key)) return false;
            release_record_locked(shard, e.key, e.record, last_at_addr);
            return true;
        });
        if (list.empty()) shard.compiled.erase(lit);
        ++erased;
    }
    return erased;
}

usize JitCompileMemo::purge_shared_records_overlapping(GuestAddr addr,
                                                     usize size,
                                                     u64 generation) noexcept {
    if (size == 0) return 0;
    const GuestAddr end = addr + size;
    const GuestAddr first_page = addr & ~GuestAddr{0xFFF};
    const GuestAddr last_page = (end - 1) & ~GuestAddr{0xFFF};
    // A record touching the written pages starts at most s_max_span_pages
    // before the first of them, and records live in their start page's
    // shard: visit exactly the shards of those start pages.
    const u64 span = s_max_span_pages.load(std::memory_order_seq_cst);
    const GuestAddr reach = static_cast<GuestAddr>(span) << 12;
    const GuestAddr lowest_start = first_page > reach ? first_page - reach : 0;
    u64 shard_mask = 0;
    if (((last_page - lowest_start) >> 12) + 1 >= kMemoShards) {
        shard_mask = ~u64{0};
    } else {
        for (GuestAddr page = lowest_start;; page += 0x1000) {
            shard_mask |= u64{1} << shard_index(page);
            if (page == last_page) break;
        }
    }
    usize erased = 0;
    for (usize i = 0; i < kMemoShards; ++i) {
        if ((shard_mask & (u64{1} << i)) == 0) continue;
        erased += purge_shard_overlapping(s_shards[i], addr, end, first_page,
                                          last_page, generation);
    }
    if (erased != 0) {
        g_shared_smc_purges.fetch_add(erased, std::memory_order_relaxed);
    }
    return erased;
}

void JitCompileMemo::dump_records(const char* path) noexcept {
    FILE* f = std::fopen(path, "w");
    if (f == nullptr) return;
    for (Shard& shard : s_shards) {
        std::shared_lock<JitMemoLock> rlock(shard.lock);
        for (const auto& [addr, list] : shard.compiled) {
            (void)addr;
            list.for_each([&](const CompiledEntry& e) {
                std::fprintf(f, "%llx %llx %u %d %u %u\n",
                             static_cast<unsigned long long>(e.key.start_addr),
                             static_cast<unsigned long long>(e.key.code_hash),
                             e.key.guest_size, e.key.is_64bit ? 1 : 0,
                             e.record.code_size, e.record.native_insns);
            });
        }
    }
    std::fclose(f);
}

} // namespace vitreo::cpu::jit
