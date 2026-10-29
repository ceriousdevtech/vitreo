// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_block_publish.cpp
/// @brief Publishing and adopting compiled blocks
///
/// Split out of jit_compiler.cpp by pure code motion; see that file for
/// the JIT register convention.

#include "jit_crash_breadcrumbs.hpp"

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
#include "jit_split_layout.hpp"
#include "ir/jit_ir.hpp"
#include "stencils.hpp"
#include "jit_coverage.hpp"
#include "jit_stats.hpp"
#include "core/cpu/decode/guest_mapped_reader.hpp"
#include "flag_liveness.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "hot_block_dump.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/math/host_fenv.hpp"
#include "core/base/perf_window.hpp"
#include "core/cpu/decode/opcodes.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <malloc/malloc.h>
#include "jit_compile_internal.hpp"

namespace vitreo::cpu::jit {

using namespace jit_detail;

void JitCompiler::publish_compiled_block(BasicBlock& block,
                                         CompiledBlock& compiled) {
    // A newly compiled block may satisfy the previous source's single pending
    // fixed edge. Patch before arming this block's own candidate.
    try_patch_direct_link(compiled);
    publish_compiled(block, compiled);
    link_published_block(compiled);

    // compile() returns the last record in this thread-owned vector. Dispatch
    // copies its executable metadata into BasicBlock immediately, so retaining
    // every fully native return record makes a compile storm grow this vector
    // without bound. A record with decoded_owner is different: generated
    // delegate stencils embed pointers into that owned snapshot.
    std::lock_guard<std::mutex> lock(m_compile_mutex);
    if (!compiled.decoded_owner && !m_compiled_blocks.empty() &&
        &m_compiled_blocks.back() == &compiled &&
        !m_compiled_blocks.back().decoded_owner) {
        m_compiled_blocks.pop_back();
    }
}

void JitCompiler::link_published_block(const CompiledBlock& compiled) {
    remember_direct_link(compiled);

    // Compilation order often resolves a target before its source. Patch
    // those cases immediately from the thread-owned cache rather than waiting
    // for another compile event — once per DISTINCT already-compiled target
    // among this block's recorded edges.
    const auto source_link = m_direct_links.find(compiled.guest_addr);
    if (source_link == m_direct_links.end()) return;
    // The edge list has now been consumed: remember_direct_link() recorded
    // every slot as a patch site, and the loop below patches what already
    // resolves. Nothing reads the entry again -- a recompile or re-adoption
    // of this block re-populates it from the memo record -- so it is a
    // HANDOFF from compile()/try_adopt_shared_block() to this point, not a
    // table. Kept as a table it was ~75 B per block per thread for the life
    // of the thread (node, edge vector and bucket), measured in
    // tests/cpu/test_jit_memory_footprint.cpp.
    std::vector<JitCompileMemo::DirectLinkMeta> legacy_links;
    decltype(m_direct_links)::node_type node;
    const bool legacy_reentry = reentry_legacy();
    if (legacy_reentry) {
        legacy_links = std::move(source_link->second);
        m_direct_links.erase(source_link);
    } else {
        node = m_direct_links.extract(source_link);
    }
    const std::vector<JitCompileMemo::DirectLinkMeta>& links =
        legacy_reentry ? legacy_links : node.mapped();
    // Recycle the node on every exit, emptied but with its capacity.
    struct KeepSpare {
        decltype(m_direct_links)::node_type& node;
        decltype(m_direct_links)::node_type& spare;
        ~KeepSpare() {
            if (node.empty()) return;
            node.mapped().clear();
            spare = std::move(node);
        }
    } keep_spare{node, m_direct_links_spare};
    if (m_block_cache) {
        u64 last_done = 0;
        for (const JitCompileMemo::DirectLinkMeta& meta : links) {
            if (meta.target == last_done) continue;
            last_done = meta.target;
            if (BasicBlock* target = m_block_cache->lookup(meta.target);
                target && target->compiled_code &&
                target->compiled_link_code) {
                // The shared inverse of publish_compiled(): a native CALL
                // site needs the target's call entry as well as its linked
                // entry, and a lease-bearing target must stay unpatched.
                // Only this source's own sites need resolving.
                try_patch_direct_link(compiled_from_cache(*target),
                                      compiled.code);
            }
        }
    }
}

void JitCompiler::stage_direct_links(
    GuestAddr addr, std::vector<JitCompileMemo::DirectLinkMeta>& links) {
    if (const auto it = m_direct_links.find(addr); it != m_direct_links.end()) {
        it->second.swap(links);
        links.clear();
        return;
    }
    if (m_direct_links_spare.empty()) {
        m_direct_links[addr].swap(links);
        return;
    }
    decltype(m_direct_links)::node_type node = std::move(m_direct_links_spare);
    node.key() = addr;
    node.mapped().swap(links);
    m_direct_links.insert(std::move(node));
}

void JitCompiler::link_adopted_block(BasicBlock& block) {
    if (block.compiled_code == nullptr) return;
    const CompiledBlock compiled = compiled_from_cache(block);
    // Same order as publish_compiled_block: satisfy pending edges INTO this
    // code first, then arm this block's own recorded edges.
    try_patch_direct_link(compiled);
    link_published_block(compiled);
}

namespace {

/// FNV-1a over the guest bytes at [addr, addr+len). Returns false when the
/// range is not readable. `mapped_out` receives the host pointer when the
/// range was hashed in place (for hot-dump bookkeeping), else nullptr.
bool hash_guest_bytes(GuestMemory& memory, GuestAddr addr, u32 len,
                      u64& hash_out, const u8*& mapped_out) noexcept {
    constexpr u64 kFnvOffset = 14695981039346656037ull;
    constexpr u64 kFnvPrime = 1099511628211ull;
    const auto fnv1a = [](u64 hash, const u8* bytes, usize n) noexcept {
        for (usize i = 0; i < n; ++i) {
            hash ^= bytes[i];
            hash *= kFnvPrime;
        }
        return hash;
    };
    mapped_out = nullptr;
    if (len == 0) return false;
    if (const Byte* mapped = memory.host_ptr(addr, len)) {
        mapped_out = mapped;
        hash_out = fnv1a(kFnvOffset, mapped, len);
        return true;
    }
    u8 chunk[64];
    u64 hash = kFnvOffset;
    usize remaining = len;
    GuestAddr cursor = addr;
    while (remaining != 0) {
        const usize n = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (memory.read_bytes(cursor, chunk, n).is_err()) return false;
        hash = fnv1a(hash, chunk, n);
        cursor += static_cast<GuestAddr>(n);
        remaining -= n;
    }
    hash_out = hash;
    return true;
}

}  // namespace

bool JitCompiler::shared_adoption_enabled() noexcept {
    auto& slot = shared_adoption_slot();
    int v = slot.load(std::memory_order_relaxed);
    if (v < 0) {
        v = environment_flag_enabled("VITREO_JIT_SHARED_ADOPT", true) ? 1 : 0;
        slot.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

bool JitCompiler::try_adopt_shared_block(GuestAddr addr, bool is_64bit,
                                         BasicBlock& out) {
    if (!JitCompileMemo::s_compiled_memo_enabled.load(std::memory_order_acquire)) return false;
    if (!shared_adoption_enabled()) return false;
    g_cold_probes.fetch_add(1, std::memory_order_relaxed);

    // Match compile()'s emission policy. The pin layout is a function of
    // mode and linear-mapping state only; select_pin_layout never reads the
    // instructions.
    BasicBlock probe;
    probe.start_addr = addr;
    probe.is_64bit = is_64bit;
    const u64 fixed_layout = select_pin_layout(probe).id;
    const u32 policy = ir_policy();

    JitCompileMemo::CompiledKey key{};
    const bool legacy_reentry = reentry_legacy();
    JitCompileMemo::CompiledRecord legacy_rec{};
    JitCompileMemo::CompiledRecord& rec =
        legacy_reentry ? legacy_rec : m_adopt_record;
    // The reused record must not keep a decoded array alive past this call.
    struct DropInsns {
        JitCompileMemo::CompiledRecord& r;
        ~DropInsns() { r.insns.reset(); }
    } drop_insns{rec};
    bool found = false;
    {
        JitCompileMemo::Shard& shard = JitCompileMemo::shard_for(addr);
        std::shared_lock<JitMemoLock> rlock(shard.lock);
        if (shard.smc_tracked.load(std::memory_order_acquire) != 0) {
            if (const auto rewrite = shard.smc_rewrites.find(addr);
                rewrite != shard.smc_rewrites.end() &&
                rewrite->second.count >= JitCompileMemo::kChurnRecompileLimit) {
                g_cold_demoted.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        auto lit = shard.compiled.find(addr);
        if (lit == shard.compiled.end()) return false;
        // Prefer a tier-1 (or untiered) record over a tier-0 one, then a
        // residency variant over the record it was profiled from. A tier-0
        // record's counter is shared, so adopting it still tiers up.
        // An uncounted tier-0 record (kIrKeyUncounted) outranks a counted
        // one: it is what a declined identity converges on.
        const auto rank = [](const JitCompileMemo::CompiledKey& k) {
            const int tier = (k.ir_policy & kIrKeyTier0) == 0 ? 4
                : (k.ir_policy & kIrKeyUncounted) != 0 ? 2 : 0;
            return tier + (k.cold_residency ? 1 : 0);
        };
        lit->second.for_each([&](const JitCompileMemo::CompiledEntry& e) {
            const JitCompileMemo::CompiledKey& candidate = e.key;
            if (candidate.is_64bit != is_64bit ||
                candidate.pin_layout != fixed_layout ||
                candidate.native_ud2 != m_cfg.native_ud2_enabled ||
                candidate.forward_join_state != m_cfg.forward_join_state_enabled ||
                candidate.native_callret != m_cfg.native_callret_enabled ||
                (candidate.ir_policy &
                 ~(kIrKeyTiered | kIrKeyTier0 | kIrKeyUncounted)) != policy ||
                candidate.linear64_jit_view != m_cfg.linear64_jit_view) {
                return;
            }
            // A lazy translation of a block whose lazy stub could not be
            // materialized would only side-exit again; adopt it eagerly.
            if (!candidate.cold_eager && cold_lazy_enabled() &&
                cold_eager_requested(addr, candidate.guest_size)) {
                return;
            }
            if (found && rank(candidate) <= rank(key)) return;
            // A counted tier-0 record of an IR-declined identity is exactly
            // what its threads are swapping away from (service_tier_request).
            if ((candidate.ir_policy & kIrKeyTier0) != 0 &&
                (candidate.ir_policy & kIrKeyUncounted) == 0 &&
                tier_declined(addr, candidate.code_hash)) {
                return;
            }
            if (!JitCodeArena::arena_code_live(e.record.code, e.record.arena_generation)) {
                return;
            }
            key = candidate;
            rec = e.record;
            found = true;
        });
    }
    if (!found) return false;

    // Fail closed on the guest bytes as they are NOW. The record's key pinned
    // the bytes it was emitted from; an executable write the table has not
    // been told about (or a page remapped underneath) must never be served
    // stale code.
    u64 hash = 0;
    const u8* mapped = nullptr;
    if (!hash_guest_bytes(m_memory, addr, key.guest_size, hash, mapped) ||
        hash != key.code_hash) {
        g_cold_hash_mismatches.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (rec.insns && rec.insns->size() != rec.instruction_count) return false;
    u32 hot_dump_variant = hot_dump::kInvalidCodeVariant;
    if (mapped != nullptr) {
        hot_dump_variant = hot_dump::record_compile(
            addr, mapped, key.guest_size, rec.instruction_count, false);
    }

    out = BasicBlock{};
    out.start_addr = addr;
    out.byte_length = key.guest_size;
    out.instruction_count = rec.instruction_count;
    out.is_64bit = is_64bit;
    out.ends_with_branch = rec.ends_with_branch;
    if (rec.insns) {
        // Delegate stencils in the shared code dereference this exact array;
        // the per-thread entry co-owns it, as memo reuse already does.
        out.instructions = DecodedInsns{rec.insns};
    } else if (rec.instruction_count != 0) {
        // Fully native: nothing in the arena refers to a decode, and this
        // thread never made one. The entry is in the same state a compiled
        // block reaches after release_block_decoded(); the interpreter
        // fallback re-decodes on demand.
        out.instructions_released = true;
    }

    CompiledBlock compiled{rec.code, rec.code_size, addr, key.guest_size,
                           rec.arena_generation, {}};
    compiled.native_insns = rec.native_insns;
    compiled.terminal_opcode = rec.terminal_opcode;
    compiled.hot_dump_variant = hot_dump_variant;
    compiled.link_code_offset = rec.link_code_offset;
    compiled.call_code_offset = rec.call_code_offset;
    compiled.requires_protection_lease = rec.requires_protection_lease;
    compiled.pin_layout = key.pin_layout;
    compiled.delegate_insns = rec.insns;
    publish_compiled(out, compiled);
    if (!rec.links.empty()) {
        if (legacy_reentry) {
            m_direct_links[addr] = std::move(rec.links);
        } else {
            stage_direct_links(addr, rec.links);
        }
    }
    g_cold_adoptions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace vitreo::cpu::jit
