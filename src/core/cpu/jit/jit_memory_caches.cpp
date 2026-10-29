// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_memory_caches.cpp
/// @brief Per-thread memory translation-cache revalidation
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

namespace vitreo::cpu::jit {

using namespace jit_detail;

// Bring this thread's memory translation caches up to date with the guest
// memory layout: a generation change (map/unmap/commit/decommit) empties the
// read table, protect shootdowns retract only the overlapping entries, and
// the write table is retired when layout, code pages or write watches moved.
// execute() runs it on every dispatcher entry and the scalar memory helpers
// on every call (jit_revalidate_memory_caches).
void JitCompiler::revalidate_memory_caches() {
    // Invalidate both region caches if the memory layout changed
    // (VirtualAlloc/VirtualFree/commit bump the generation).
    u64 cur_gen = m_memory.generation();
    const u64 protect_snapshot = m_memory.protect_sequence();
    if (cur_gen != m_region_gen) {
        m_region_gen = cur_gen;
        m_protect_seq = protect_snapshot;
        m_jit_ctx.region_base = 0;
        m_jit_ctx.region_end = 0;
        m_jit_ctx.region_host_base = nullptr;
        m_jit_ctx.region2_base = 0;
        m_jit_ctx.region2_end = 0;
        m_jit_ctx.region2_host_base = nullptr;
        m_jit_ctx.x64_read16_region_base = 0;
        m_jit_ctx.x64_read16_region_end = 0;
        m_jit_ctx.x64_read16_region_host_base = nullptr;
        m_jit_ctx.x64_read16_region2_base = 0;
        m_jit_ctx.x64_read16_region2_end = 0;
        m_jit_ctx.x64_read16_region2_host_base = nullptr;
        // A map/unmap/commit can invalidate any cached host pointer, and the
        // indexed table is read by emitted code with no further checks.
        m_x64_rslots.fill(X64MemSlot{});
    } else if (protect_snapshot != m_protect_seq) {
        drain_protect_shootdowns(protect_snapshot);
    }
    m_jit_ctx.protect_sequence_seen = m_protect_seq;

    // Retract both inline-store regions whenever the guest memory layout or
    // the set of code-bearing pages changed. Either can break the invariant
    // the emitted stores depend on (writable, and free of compiled code), and
    // neither is observable from inside JIT-emitted asm — so it is checked
    // here, once per block entry, rather than per store.
    {
        const u64 code_gen = m_memory.jit_code_generation();
        const int watch_gen = write_watch::table_generation();
        if (cur_gen != m_jit_ctx.wregion_mem_gen ||
            code_gen != m_jit_ctx.wregion_code_gen ||
            watch_gen != m_jit_ctx.wregion_watch_gen ||
            write_watch::any_active()) {
            m_jit_ctx.wregion_mem_gen = cur_gen;
            m_jit_ctx.wregion_code_gen = code_gen;
            m_jit_ctx.wregion_watch_gen = watch_gen;
            m_jit_ctx.wregion_base = 0;
            m_jit_ctx.wregion_end = 0;
            m_jit_ctx.wregion_host_base = nullptr;
            m_jit_ctx.wregion2_base = 0;
            m_jit_ctx.wregion2_end = 0;
            m_jit_ctx.wregion2_host_base = nullptr;
            // Emitted stores through this table perform no permission or SMC
            // check, so it must not outlive either generation it was
            // validated under.
            m_x64_wslots.fill(X64MemSlot{});
        }
    }
}

// Protect shootdowns published after m_protect_seq retract only the cached
// translations they overlap and lose; an overflowed log retracts everything.
void JitCompiler::drain_protect_shootdowns(u64 protect_snapshot) {
    {
        GuestMemory::ProtectShootdown events[GuestMemory::kProtectShootdownLog];
        bool overflow = false;
        const usize n = m_memory.copy_protect_shootdowns_after(
            m_protect_seq, events, GuestMemory::kProtectShootdownLog,
            &overflow);
        // Advance only to the snapshot known before the copy. The copy may
        // observe a later publication internally; replaying that event is
        // harmless, while skipping an event published after our snapshot is
        // a stale-permission bug.
        m_protect_seq = protect_snapshot;
        auto overlaps = [](u64 base, u64 end, GuestAddr addr,
                           usize size) -> bool {
            if (end <= base || size == 0) {
                return false;
            }
            const u64 r1 = static_cast<u64>(addr) + size;
            if (r1 < static_cast<u64>(addr)) {
                return true;
            }
            return base < r1 && static_cast<u64>(addr) < end;
        };
        auto retract = [](auto& base, auto& end, u8*& host) {
            base = 0;
            end = 0;
            host = nullptr;
        };
        auto retract_if = [&](auto& base, auto& end, u8*& host,
                              GuestAddr addr, usize size) {
            if (overlaps(base, end, addr, size)) {
                retract(base, end, host);
            }
        };
        auto retract_slots = [&](std::array<X64MemSlot, X64_SLOT_COUNT>& slots,
                                 GuestAddr addr, usize size) {
            for (auto& slot : slots) {
                if (slot.host && overlaps(slot.base, slot.end, addr, size)) {
                    slot = {};
                }
            }
        };
        if (overflow) {
            retract(m_jit_ctx.region_base, m_jit_ctx.region_end,
                    m_jit_ctx.region_host_base);
            retract(m_jit_ctx.region2_base, m_jit_ctx.region2_end,
                    m_jit_ctx.region2_host_base);
            retract(m_jit_ctx.wregion_base, m_jit_ctx.wregion_end,
                    m_jit_ctx.wregion_host_base);
            retract(m_jit_ctx.wregion2_base, m_jit_ctx.wregion2_end,
                    m_jit_ctx.wregion2_host_base);
            retract(m_jit_ctx.x64_read16_region_base,
                    m_jit_ctx.x64_read16_region_end,
                    m_jit_ctx.x64_read16_region_host_base);
            retract(m_jit_ctx.x64_read16_region2_base,
                    m_jit_ctx.x64_read16_region2_end,
                    m_jit_ctx.x64_read16_region2_host_base);
            retract(m_jit_ctx.stack_region_base, m_jit_ctx.stack_region_end,
                    m_jit_ctx.stack_region_host_base);
            m_x64_rslots.fill(X64MemSlot{});
            m_x64_wslots.fill(X64MemSlot{});
        } else {
            for (usize i = 0; i < n; ++i) {
                const GuestAddr addr = events[i].addr;
                const usize size = events[i].size;
                const MemoryProtection prot = events[i].protection;
                // RW↔RX keeps Read. Retracting load caches on every W^X
                // protect forces get_region_info on the next block and
                // convoys on m_regions_mutex.
                if (!has_flag(prot, MemoryProtection::Read)) {
                    retract_if(m_jit_ctx.region_base, m_jit_ctx.region_end,
                               m_jit_ctx.region_host_base, addr, size);
                    retract_if(m_jit_ctx.region2_base, m_jit_ctx.region2_end,
                               m_jit_ctx.region2_host_base, addr, size);
                    retract_if(m_jit_ctx.x64_read16_region_base,
                               m_jit_ctx.x64_read16_region_end,
                               m_jit_ctx.x64_read16_region_host_base, addr,
                               size);
                    retract_if(m_jit_ctx.x64_read16_region2_base,
                               m_jit_ctx.x64_read16_region2_end,
                               m_jit_ctx.x64_read16_region2_host_base, addr,
                               size);
                    retract_if(m_jit_ctx.stack_region_base,
                               m_jit_ctx.stack_region_end,
                               m_jit_ctx.stack_region_host_base, addr, size);
                    retract_slots(m_x64_rslots, addr, size);
                    // x64_wslots also back the combined non-LOCK RMW path and
                    // therefore require Read as well as Write.
                    retract_slots(m_x64_wslots, addr, size);
                }
                if (!has_flag(prot, MemoryProtection::Write)) {
                    retract_if(m_jit_ctx.wregion_base, m_jit_ctx.wregion_end,
                               m_jit_ctx.wregion_host_base, addr, size);
                    retract_if(m_jit_ctx.wregion2_base, m_jit_ctx.wregion2_end,
                               m_jit_ctx.wregion2_host_base, addr, size);
                    retract_slots(m_x64_wslots, addr, size);
                }
            }
        }
    }
}

} // namespace vitreo::cpu::jit
