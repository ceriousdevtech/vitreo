// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_compiler.cpp
/// @brief Copy-and-patch JIT compiler implementation (CPU-040)
///
/// Compiles decoded x86 BasicBlocks into ARM64 machine code using
/// direct instruction encoding. Uses lazy flag evaluation to avoid
/// computing x86 flags after every ALU instruction.
///
/// Register convention for JIT code:
///   X19 = CpuState*        (callee-saved, pinned)
///   X20 = JitContext*       (callee-saved, pinned — helper fn ptrs + memory)
///   W0-W15 = scratch registers
///   X16-X17 = intra-procedure scratch

#include <cstdint>
///   X30 = LR (saved/restored in prologue/epilogue for BLR calls)

#include "jit_compiler.hpp"
#include "stencils.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include "jit_inval_stats.hpp"

/// @file constructor_destructor.cpp
/// @brief Constructor / Destructor
///
/// Closed cut from jit_compiler.cpp — verified with plan_extraction.py.

namespace vitreo::cpu::jit {

// Self-modifying-code demotion accounting.
//
// Repeatedly invalidated blocks eventually use the interpreter to bound
// compilation cost; occasional guest code patches remain eligible for JIT.
//
// `g_smc_demoted` is how many
// distinct blocks are permanently interpreted; `g_smc_interpret_hits` is how
// often that decision actually costs an execution. One relaxed increment each.
std::atomic<u64> g_smc_demoted{0};
std::atomic<u64> g_smc_interpret_hits{0};

void smc_note_demote() noexcept {
    const u64 n = g_smc_demoted.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0 || (n % 512) == 0) {  // powers of two, then every 512
        std::fprintf(stderr,
            "[jit-smc] blocks permanently interpreted: %llu "
            "(executions routed so far: %llu)\n",
            static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(
                g_smc_interpret_hits.load(std::memory_order_relaxed)));
    }
}

void smc_note_interpret_hit() noexcept {
    const u64 n = g_smc_interpret_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n % 250000) == 0) {
        std::fprintf(stderr,
            "[jit-smc] %llu executions routed to the interpreter across %llu "
            "demoted blocks\n",
            static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(
                g_smc_demoted.load(std::memory_order_relaxed)));
    }
}


// ============================================================================
// Constructor / Destructor
// ============================================================================

JitCompiler::JitCompiler(GuestMemory& memory) noexcept
    : m_memory(memory)
{
    m_code_buf.reserve(512);

    m_jit_ctx.x64_rslots = m_x64_rslots.data();
    m_jit_ctx.x64_wslots = m_x64_wslots.data();
    // L3: the host trampoline in-chain API-call exits BLR to
    // (jit_inchain_thunk.cpp), and the compiler its C++ half reaches.
    m_jit_ctx.inchain_trampoline =
        reinterpret_cast<void*>(&vitreo_jit_inchain_trampoline);
    m_jit_ctx.inchain_owner = this;
    // Lazy cold memory stubs (jit_cold_lazy.cpp): its C++ half also reaches
    // this compiler through inchain_owner.
    m_jit_ctx.cold_lazy_entry = cold_lazy_trampoline_address();
    m_jit_ctx.protect_sequence_live = m_memory.protect_sequence_slot();
    m_jit_ctx.code_generation_live = m_memory.jit_code_generation_slot();
    m_jit_ctx.watch_generation_live = &write_watch::g_table_gen;

    // Check if inline region cache should be disabled (debugging aid).
    const char* env = std::getenv("VITREO_NO_JITCACHE");
    m_cfg.no_jitcache = (env && env[0] == '1');

    // Linear (guest-base) memory path is the 32-bit production default. It only
    // takes effect for GuestMemory with a reservation; VITREO_JIT_LINEAR=0
    // keeps the region-cache path available for compatibility bisection.
    const bool recoverable_linear =
        m_memory.is_identity() ||
        linear_reservation_registered(m_memory.linear_base(),
                                      m_memory.linear_size());
    m_cfg.linear_jit = environment_flag_enabled("VITREO_JIT_LINEAR", true) &&
                   recoverable_linear;
    // Same fold the x64 store got (see the measurement below): the code-span
    // compare, the 2 MiB chunk table, the 4 KiB code-page map and the inline
    // write-watch mark are all one LDRB of the per-host-page check byte. The
    // emitted index is a bare UBFX with no bound test, so the map must cover
    // every address a 32-bit guest can form; a shorter reservation keeps the
    // legacy probe. VITREO_JIT_LINEAR32_CHECKMAP=0 restores it for bisection.
    //
    // The emitted index is a bare UBFX with no bound test, so its width is
    // taken from the reservation: every byte it can address is inside the map.
    // That is exactly the assumption the inline store beside it already makes
    // (it stores at linear_base + zext(addr) with no bound test either), so a
    // guest address the reservation does not cover is out of contract for this
    // memory model whichever check is emitted.
    {
        const usize linear_size = m_memory.linear_size();
        const bool pow2 = linear_size != 0 &&
                          (linear_size & (linear_size - 1)) == 0 &&
                          linear_size > (usize{1} << kLinearHostPageShift) &&
                          linear_size <= (usize{1} << 32);
        usize bits = 0;
        for (usize v = linear_size; v > 1; v >>= 1) ++bits;
        m_cfg.linear32_check_map =
            pow2 && m_memory.linear_check_page_map() != nullptr &&
            m_memory.linear_check_page_count() >=
                (linear_size >> kLinearHostPageShift) &&
            environment_flag_enabled("VITREO_JIT_LINEAR32_CHECKMAP", true);
        m_cfg.linear32_check_index_bits =
            m_cfg.linear32_check_map
                ? static_cast<u8>(bits - kLinearHostPageShift)
                : 0;
    }
    // x64 guest-base is the production default now that guest protection is
    // enforced by hardware inside the reservation. VITREO_JIT_LINEAR64=0 keeps
    // the region-cache path available for compatibility bisection; an absent or
    // unregistered reservation can never reach the direct emitter regardless.
    // It was once default off, for a recorded reason that was later measured
    // and found wrong, so do not reason from it. The record said the ~70% launcher
    // splash-to-window regression (73s/71s on versus 41s/43s off) was
    // "suspected mprotect traffic" from hardware-enforced protection. It was
    // not measured, and the real cost was in the emitted store:
    //
    //   MOV r/m32,r32   region 22.43 ARM/x86   guest-base 30.00 ARM/x86
    //   MOV r32,r/m32   region 20.86 ARM/x86   guest-base 18.86 ARM/x86
    //
    // The +7.6 per store was the per-store code-span compare -- two LDR+LDAR
    // +SUBS+B.cond pairs re-deriving, through ACQUIRE loads, a fact that only
    // changes when a new code page is first translated. A store-light
    // microbenchmark cannot see it; an application stores constantly. That
    // compare is now one bit of the per-host-page check byte
    // (kLinearCheckHoldsCode). A second per-store test of global state went the
    // same way: the write-watch published-map slot became kLinearCheckWatched,
    // which freed the pinned x15 to hold the check-map base and removed that
    // reload too. The store is now 13 emitted instructions with ONE dependent
    // load, no acquire load and no NZCV clobber:
    //
    //   MOV r/m32,r32   region 22.43   guest-base 30.00 -> 22.43 -> 19.91
    //   MOV r32,r/m32   region 20.86   guest-base 18.86 -> 17.86
    //
    // Both directions are now cheaper than the region path. Measured on the
    // bench kernels, normalised to the untouched `alu` control (these predate
    // the write-watch fold, so they understate it):
    //
    //   storemem     region 0.780   guest-base 0.913 before -> 0.766 after
    //   submem_jcc   region 1.743   guest-base 2.303 before -> 1.691 after
    //   load_chain   region 2.440   guest-base 1.151 before -> 1.104 after
    //
    // Post-fix application A/B reached a launcher's first window/device/draw with
    // hardware enforcement and stayed alive through the bounded run. The
    // software-only mode instead deterministically exposed an uncaught guest
    // exception, so it remains available only through explicit opt-in.
    // The check-page map is not optional: a direct store learns that its host
    // page holds translated code from that byte and from nothing else, so
    // without the map there is no safe direct store to emit.
    // Default mode is hardware-only: if the host cannot enforce guest page
    // protection, stay on the established region JIT instead of silently
    // entering the software-linear bisection path. Explicit =1 retains that
    // path for diagnostics and performance experiments; explicit =0 disables
    // linear64 entirely.
    m_cfg.linear64 = linear64_fast_path_requested(
                     m_memory.linear_hardware_protection()) &&
                 m_memory.is_linear() && !m_memory.is_identity() &&
                 recoverable_linear &&
                 m_memory.linear_check_page_map() != nullptr;
    m_cfg.native_ud2_enabled = environment_flag_enabled(
        "VITREO_JIT_NATIVE_UD2", true);
    m_cfg.forward_join_state_enabled = environment_flag_enabled(
        "VITREO_JIT_FORWARD_JOIN_STATE", true);
    m_cfg.native_callret_enabled = environment_flag_enabled(
        "VITREO_JIT_NATIVE_CALLRET", false);
    // On by default: +1.8..4.3% title-menu FPS over 8 interleaved
    // runs (2026-10-06). VITREO_JIT_SPLIT_COLD=0 places every block
    // contiguously, as before.
    m_cfg.split_cold = environment_flag_enabled("VITREO_JIT_SPLIT_COLD", true);
    // On by default: see the member's comment. VITREO_JIT_LINEAR64_PERMGUARD=0
    // is a measurement/bisection lever only -- it makes VirtualProtect
    // invisible to direct linear64 accesses and is not a shippable mode.
    m_cfg.linear64_perm_guard =
        environment_flag_enabled("VITREO_JIT_LINEAR64_PERMGUARD", true);
    m_cfg.linear64_vec128 =
        std::getenv("VITREO_JIT_NO_LINEAR64_VEC128") == nullptr;
    // Hardware enforcement replaces the eleven-instruction permission probe
    // with a one-byte mixed-page test. It is a property of the GuestMemory,
    // not a JIT option: the reservation either carries the guest's protections
    // or it does not.
    m_cfg.linear64_hw_protect = m_memory.linear_hardware_protection();
    m_cfg.xmm_cache = environment_flag_enabled("VITREO_JIT_XMM_CACHE", true);
    m_cfg.probe_poison_vregs =
        environment_flag_enabled("VITREO_JIT_PROBE_POISON_VREGS", false);
    m_cfg.linear64_range_by_fault =
        m_cfg.linear64_hw_protect &&
        m_memory.linear_check_map_ends_at_va_limit() &&
        environment_flag_enabled("VITREO_JIT_RANGE_FAULT", true);
    m_jit_ctx.linear_check_map = m_memory.linear_check_page_map();
    m_jit_ctx.linear_subpage_map = m_memory.linear_subpage_map();
    if (m_cfg.linear64) {
        // The shift/mask emitters bound an EA by the power-of-two core. The
        // extension above it (GuestMemory::linear_size()) is reached only
        // through the map-bounded paths: the range-by-fault guard (whose
        // unmasked index faults past the map, which covers linear_size()) and
        // the shared sub-page stub (bounded by linear64_window_bytes).
        usize size = m_memory.linear_power_of_two_size();
        if (size == 0 || (size & (size - 1)) != 0) {
            m_cfg.linear64 = false;
        } else {
            while (size > 1) {
                ++m_cfg.linear64_address_bits;
                size >>= 1;
            }
        }
    }
    m_jit_ctx.linear64_window_bytes =
        m_cfg.linear64 && m_cfg.linear64_hw_protect ? m_memory.linear_size() : 0;
    // The store ring records inline stores without changing the memory
    // model. Value watches still need writes to pass through GuestMemory.
    m_store_ring_stores = (std::getenv("VITREO_STORE_RING") != nullptr);
    if (std::getenv("VITREO_WATCH_VALUE") != nullptr) {
        m_cfg.linear_jit = false;
        m_cfg.linear64 = false;
        m_cfg.no_jitcache = true;
    }
    diagnostics::perf_note_memory_model(
        m_memory.is_linear(), m_cfg.linear_jit, m_cfg.linear64, m_cfg.no_jitcache);
    // x86-TSO ordering policy (Ordered by default).
    //
    // Ordered uses LDAPR/STLR. With FEAT_LSE2 those forms handle unaligned
    // addresses, but raise an Alignment fault when an access straddles a
    // 16-byte granule — which x86 permits. The fault handler backpatches the
    // two-word site to [plain, DMB] / [DMB, plain] outside signal context.
    // Without that handler we fail closed to Fence.
    //
    // Fence (VITREO_JIT_STRICT_TSO=1 or VITREO_JIT_TSO_ORDERED=0): DMB ISH.
    // Off (VITREO_JIT_NO_TSO=1): plain LDR/STR for cost measurement.
    if (environment_flag_enabled("VITREO_JIT_NO_TSO", false)) {
        m_cfg.tso_mode = TsoMode::Off;
        m_cfg.strict_tso = false;
        m_cfg.tso_fault_patch = false;
    } else if (environment_flag_enabled("VITREO_JIT_STRICT_TSO", false) ||
               !environment_flag_enabled("VITREO_JIT_TSO_ORDERED", true)) {
        m_cfg.tso_mode = TsoMode::Fence;
        m_cfg.strict_tso = true;
        m_cfg.tso_fault_patch = install_linear_fault_handler();
    } else if (install_linear_fault_handler()) {
        m_cfg.tso_mode = TsoMode::Ordered;
        m_cfg.strict_tso = false;
        m_cfg.tso_fault_patch = true;
    } else {
        m_cfg.tso_mode = TsoMode::Fence;
        m_cfg.strict_tso = true;
        m_cfg.tso_fault_patch = false;
    }
    // Fence mode used to bracket every 32-bit guest store with TWO DMB ISHs.
    // Only the leading one is x86-TSO: the trailing one adds the store->load
    // edge, which is precisely the reordering x86 permits (Intel SDM Vol. 3A
    // 9.2.2 -- "loads may be reordered with older stores to different
    // locations"). store->store is already ordered by the NEXT store's
    // leading barrier, and older-load->store by that load's trailing barrier.
    // VITREO_JIT_TSO_STORE_TAIL=1 restores the second fence for bisection.
    m_cfg.tso_store_tail_fence =
        environment_flag_enabled("VITREO_JIT_TSO_STORE_TAIL", false);
    m_cfg.tso_crossing_fault =
        m_cfg.tso_fault_patch &&
        environment_flag_enabled("VITREO_JIT_TSO_CROSSING_FAULT", true);
    m_cfg.tso_use_ldapr = host_supports_lrcpc();
    m_cfg.linear64_jit_view =
        environment_flag_enabled("VITREO_JIT_VIEW", false) && m_cfg.linear64 &&
        m_cfg.linear64_hw_protect && m_cfg.tso_crossing_fault &&
        m_memory.linear_jit_view_base() != nullptr &&
        linear_jit_view_registered(m_memory.linear_jit_view_base(),
                                   m_memory.linear_size());
    if (m_cfg.linear64_jit_view) {
        m_jit_ctx.jit_view_base = m_memory.linear_jit_view_base();
        m_cfg.linear64_range_by_fault = false;
    }
    // Intel SDM Vol. 3A section 9.2 requires ordinary WB stores, including
    // temporal SSE stores, to become visible in program order. ARM64 LDR/STR Q
    // provide no acquire/release form, so vector accesses need the DMB fences
    // emitted by emit_vector_tso_fence(). Leaving these off lets a scalar
    // publication become visible before the vector-initialized object it
    // publishes. VITREO_JIT_TSO_VECTOR=0 remains a performance/bisection
    // override, but the correctness default must preserve x86 TSO.
    m_cfg.tso_vector = environment_flag_enabled("VITREO_JIT_TSO_VECTOR", true);
    // Print AFTER every field above is resolved — a dump taken before TSO
    // install reported Ordered with fault_patch=0 (struct defaults) and was
    // actively misleading.
    if (std::getenv("VITREO_JIT_CONFIG") != nullptr) {
        static std::once_flag once;
        std::call_once(once, [this] {
            std::fprintf(stderr, "[jit-config]\n%s\n", m_cfg.describe().c_str());
        });
    }
    m_jit_ctx.read16_diag_enabled =
        environment_flag_enabled("VITREO_JIT_READ16_DIAG", false);

    // Parse block skip list for debugging: VITREO_JIT_SKIP=0x404e86,0x404f6f,...
    if (const char* skip_env = std::getenv("VITREO_JIT_SKIP")) {
        std::string s(skip_env);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) comma = s.size();
            std::string tok = s.substr(pos, comma - pos);
            if (!tok.empty()) {
                m_cfg.jit_skip.push_back(static_cast<GuestAddr>(std::strtoul(tok.c_str(), nullptr, 16)));
            }
            pos = comma + 1;
        }
    }

    // Range-based skip for binary search debugging
    if (const char* above = std::getenv("VITREO_JIT_SKIP_ABOVE")) {
        m_cfg.jit_skip_above = static_cast<GuestAddr>(std::strtoul(above, nullptr, 16));
    }
    if (const char* below = std::getenv("VITREO_JIT_SKIP_BELOW")) {
        m_cfg.jit_skip_below = static_cast<GuestAddr>(std::strtoul(below, nullptr, 16));
    }
    // Closed-open guest ranges (comma-separated):
    // VITREO_JIT_SKIP_RANGE=0x0042b000-0x0042d000,0x00482800-0x00484200
    if (const char* range = std::getenv("VITREO_JIT_SKIP_RANGE")) {
        const char* p = range;
        while (*p != '\0' && m_cfg.jit_skip_range_count < JitConfig::kMaxJitSkipRanges) {
            while (*p == ' ' || *p == ',') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            char* end = nullptr;
            const unsigned long lo = std::strtoul(p, &end, 16);
            if (end == nullptr || (*end != '-' && *end != ',')) {
                break;
            }
            if (*end != '-') {
                p = end;
                continue;
            }
            const unsigned long hi = std::strtoul(end + 1, &end, 16);
            if (hi > lo) {
                const u8 i = m_cfg.jit_skip_range_count++;
                m_cfg.jit_skip_range_from[i] = static_cast<GuestAddr>(lo);
                m_cfg.jit_skip_range_to[i] = static_cast<GuestAddr>(hi);
                std::fprintf(stderr,
                    "[jit] SKIP_RANGE interpret [0x%08lx, 0x%08lx)\n",
                    lo, hi);
            }
            p = (end != nullptr) ? end : "";
        }
    }

    // Initialize persistent JIT context — function pointers are constant.
    m_jit_ctx.memory = &m_memory;
    m_jit_ctx.write_watch_pages =
        reinterpret_cast<u8*>(write_watch::published_map_slot());
    m_jit_ctx.read32 = jit_mem_read32;
    m_jit_ctx.write32 = jit_mem_write32;
    m_jit_ctx.eval_cond = jit_eval_condition;
    m_jit_ctx.read8 = jit_mem_read8;
    m_jit_ctx.write8 = jit_mem_write8;
    m_jit_ctx.read16 = jit_mem_read16;
    m_jit_ctx.write16 = jit_mem_write16;
    m_jit_ctx.read64 = jit_mem_read64;
    m_jit_ctx.write64 = jit_mem_write64;
    m_jit_ctx.write_span = jit_mem_write_span;
    m_jit_ctx.require_stack_limit = jit_require_stack_limit;
    m_jit_ctx.rep_movsd = jit_rep_movsd;
    m_jit_ctx.rep_stosd = jit_rep_stosd;
    m_jit_ctx.materialize_flags = jit_materialize_flags;
    m_jit_ctx.capture_cf = jit_capture_cf;
    m_jit_ctx.zero_fill_loop = jit_zero_fill_loop;
    m_jit_ctx.cmpxchg8b = jit_cmpxchg8b;
    m_jit_ctx.fpu_op = jit_fpu_op;
    m_jit_ctx.sse_op = jit_sse_op;
    m_jit_ctx.cmpxchg32 = jit_cmpxchg32;
    m_jit_ctx.shld32 = jit_shld32;
    m_jit_ctx.rep_movsb = jit_rep_movsb;
    m_jit_ctx.atomic_xchg32 = jit_atomic_xchg32;
    m_jit_ctx.lock_inc32 = jit_lock_inc32;
    m_jit_ctx.lock_dec32 = jit_lock_dec32;
    m_jit_ctx.lock_xadd32 = jit_lock_xadd32;
    m_jit_ctx.lock_cmpxchg32 = jit_lock_cmpxchg32;
    m_jit_ctx.lock_cmpxchg8b = jit_lock_cmpxchg8b;
    m_jit_ctx.lock_or32 = jit_lock_or32;
    m_jit_ctx.lock_and32 = jit_lock_and32;
    m_jit_ctx.lock_add32 = jit_lock_add32;
    m_jit_ctx.lock_sub32 = jit_lock_sub32;
    m_jit_ctx.lock_cmpxchg8 = jit_lock_cmpxchg8;
    m_jit_ctx.lock_or16 = jit_lock_or16;
    m_jit_ctx.lock_and16 = jit_lock_and16;
    m_jit_ctx.fpu_peek_m32 = jit_fpu_peek_m32;
    m_jit_ctx.fpu_peek_m64 = jit_fpu_peek_m64;
    m_jit_ctx.fpu_peek_int = jit_fpu_peek_int;
    m_jit_ctx.fpu_pop = jit_fpu_pop;
    m_jit_ctx.fused_memcpy = jit_fused_memcpy;
    m_jit_ctx.shrd32 = jit_shrd32;
    m_jit_ctx.interpret_step = jit_interpret_step;
    m_jit_ctx.interpret_decoded = jit_interpret_decoded;
    m_jit_ctx.rep_stosb   = jit_rep_stosb;
    m_jit_ctx.rep_stosw   = jit_rep_stosw;
    m_jit_ctx.rep_movsw   = jit_rep_movsw;
    m_jit_ctx.repe_cmpsb  = jit_repe_cmpsb;
    m_jit_ctx.repne_scasb = jit_repne_scasb;
    m_jit_ctx.repe_cmpsw  = jit_repe_cmpsw;
    m_jit_ctx.repe_cmpsd  = jit_repe_cmpsd;
    m_jit_ctx.repne_cmpsw = jit_repne_cmpsw;
    m_jit_ctx.repne_cmpsd = jit_repne_cmpsd;
    m_jit_ctx.repe_scasw  = jit_repe_scasw;
    m_jit_ctx.repe_scasd  = jit_repe_scasd;
    m_jit_ctx.repne_scasw = jit_repne_scasw;
    m_jit_ctx.repne_scasd = jit_repne_scasd;
    m_jit_ctx.repe_scasb  = jit_repe_scasb;
    m_jit_ctx.repne_cmpsb = jit_repne_cmpsb;
    m_jit_ctx.pdep64 = jit_pdep64;
    m_jit_ctx.pext64 = jit_pext64;
    m_jit_ctx.rep_stosq   = jit_rep_stosq;
    m_jit_ctx.rep_movsq   = jit_rep_movsq;
    m_jit_ctx.repe_cmpsq  = jit_repe_cmpsq;
    m_jit_ctx.repne_cmpsq = jit_repne_cmpsq;
    m_jit_ctx.repe_scasq  = jit_repe_scasq;
    m_jit_ctx.repne_scasq = jit_repne_scasq;
    m_jit_ctx.rep_lodsb   = jit_rep_lodsb;
    m_jit_ctx.rep_lodsw   = jit_rep_lodsw;
    m_jit_ctx.rep_lodsd   = jit_rep_lodsd;

    // Specialized condition-evaluation helpers — indexed by x86 cc value.
    m_jit_ctx.cond_fns[0x0] = jit_cond_o;
    m_jit_ctx.cond_fns[0x1] = jit_cond_no;
    m_jit_ctx.cond_fns[0x2] = jit_cond_b;
    m_jit_ctx.cond_fns[0x3] = jit_cond_nb;
    m_jit_ctx.cond_fns[0x4] = jit_cond_z;
    m_jit_ctx.cond_fns[0x5] = jit_cond_nz;
    m_jit_ctx.cond_fns[0x6] = jit_cond_be;
    m_jit_ctx.cond_fns[0x7] = jit_cond_nbe;
    m_jit_ctx.cond_fns[0x8] = jit_cond_s;
    m_jit_ctx.cond_fns[0x9] = jit_cond_ns;
    m_jit_ctx.cond_fns[0xA] = jit_cond_p;
    m_jit_ctx.cond_fns[0xB] = jit_cond_np;
    m_jit_ctx.cond_fns[0xC] = jit_cond_l;
    m_jit_ctx.cond_fns[0xD] = jit_cond_nl;
    m_jit_ctx.cond_fns[0xE] = jit_cond_le;
    m_jit_ctx.cond_fns[0xF] = jit_cond_nle;

    // Initialize the process-wide shared MAP_JIT arena (once). The once_flag
    // and the initialiser are the arena's business; a compiler only asks.
    JitCodeArena::ensure_initialized();
    // The arena does not know what a compile memo is. Tell it who to call so a
    // retired translation cannot be adopted after its chunk is reused.
    // Many guest threads construct compilers concurrently. Publish the one
    // process-wide observer once, before any of those compilers can retire
    // code, rather than racing identical writes to the arena's callback.
    static std::once_flag retirement_observer_once;
    std::call_once(retirement_observer_once, [] {
        JitCodeArena::set_retirement_observer(
            &JitCompileMemo::purge_memo_for_retired_chunks);
    });
    m_arena_retire_seq =
        JitCodeArena::s_arena_retire_seq.load(std::memory_order_acquire);
    // Last: m_cfg is final, and from here other threads' publishers write
    // the process-wide stubs into m_jit_ctx.
    register_live_jit_context();
}

JitCompiler::~JitCompiler() {
    unregister_live_jit_context();
    if (m_jit_ctx.read16_diag_enabled && m_jit_ctx.read16_diag_calls != 0) {
        std::array<JitContext::Read16DiagRegion, 64> regions =
            m_jit_ctx.read16_diag_regions;
        std::array<JitContext::Read16DiagRip, 64> rips =
            m_jit_ctx.read16_diag_rips;
        std::sort(regions.begin(), regions.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.calls > rhs.calls;
                  });
        std::sort(rips.begin(), rips.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.calls > rhs.calls;
                  });
        std::fprintf(
            stderr,
            "[jit-read16-diag] calls=%llu low=%llu high=%llu "
            "x86=%llu x64=%llu unknown_mode=%llu "
            "distinct_regions=%llu region_collisions=%llu "
            "rip_collisions=%llu\n",
            static_cast<unsigned long long>(m_jit_ctx.read16_diag_calls),
            static_cast<unsigned long long>(m_jit_ctx.read16_diag_low_calls),
            static_cast<unsigned long long>(m_jit_ctx.read16_diag_high_calls),
            static_cast<unsigned long long>(m_jit_ctx.read16_diag_x86_calls),
            static_cast<unsigned long long>(m_jit_ctx.read16_diag_x64_calls),
            static_cast<unsigned long long>(
                m_jit_ctx.read16_diag_unknown_mode_calls),
            static_cast<unsigned long long>(
                m_jit_ctx.read16_diag_distinct_regions),
            static_cast<unsigned long long>(
                m_jit_ctx.read16_diag_region_collisions),
            static_cast<unsigned long long>(
                m_jit_ctx.read16_diag_rip_collisions));
        for (usize i = 0; i < 8 && regions[i].calls != 0; ++i) {
            std::fprintf(
                stderr,
                "[jit-read16-diag] region base=0x%llx end=0x%llx "
                "calls=%llu\n",
                static_cast<unsigned long long>(regions[i].base),
                static_cast<unsigned long long>(regions[i].end),
                static_cast<unsigned long long>(regions[i].calls));
        }
        for (usize i = 0; i < 8 && rips[i].calls != 0; ++i) {
            std::fprintf(
                stderr,
                "[jit-read16-diag] rip=0x%llx calls=%llu\n",
                static_cast<unsigned long long>(rips[i].rip),
                static_cast<unsigned long long>(rips[i].calls));
        }
    }
    // Arena code and direct patches are process-shared. Invalidate the
    // embedded targets before dropping this compiler's final page/IR leases;
    // another compiler may still reuse the patched source bytes.
    if (!m_direct_patch_targets.empty()) unlink_own_direct_sites();
    release_direct_link_targets();
    m_direct_patch_targets.clear();
    m_memory.clear_thread_executable_write_observer();
    // The chunk keeps its code (other threads may adopt it through the memo);
    // the next compiler resumes allocating after it.
    JitCodeArena::release_chunk(m_arena);
}

void JitCompiler::note_invalidated_blocks(const std::vector<GuestAddr>& removed) {
    if (!JitCompileMemo::s_compiled_memo_enabled.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(m_compile_mutex);
    for (GuestAddr addr : removed) {
        // Retain a local backstop even when the shared record was evicted or
        // denied admission. Ordinary, never-rewritten blocks need no entry.
        if (m_churn_interpret.count(addr) == 0) {
            auto& count = m_recompile_counts[addr];
            if (count < JitCompileMemo::kChurnRecompileLimit) ++count;
        }
    }
}

void JitCompiler::set_block_cache(BlockCache* cache) {
    m_block_cache = cache;
    if (cache) {
        cache->bind_guest_memory(&m_memory);
    }
    // As in synchronize_executable_writes: the unlink sequence first, then
    // the write generation this cache starts from.
    m_unlink_seq_at_sync = unlink_sequence();
    m_patched_since_sync = false;
    m_executable_generation = m_memory.executable_generation();
    m_jit_ctx.block_cache = cache;
    m_jit_ctx.block_cache_l1 = cache
        ? reinterpret_cast<void**>(cache->l1_base())
        : nullptr;
    if (cache) {
        // Invalidate immediately; compile() bounds repeated recompilation.
        m_memory.set_thread_executable_write_observer(
            [this](GuestAddr addr, usize size, u64 generation) -> bool {
                if (!m_block_cache) {
                    m_executable_generation = generation;
                    return false;
                }
                // (0, 0) is GuestMemory's lost-history overflow sentinel, not
                // Win32 FlushInstructionCache(NULL).
                if (addr == 0 && size == 0) {
                    std::lock_guard<std::mutex> lock(m_compile_mutex);
                    m_block_cache->clear();
                    m_jit_ctx.block_cache_l1 = nullptr;
                    invalidate_return_predictors();
                    bump_link_generation(1);
                    m_executable_generation = generation;
                    return true;
                }
                // This notification covers only this write. Its generation
                // may already include foreign-thread patches that this cache
                // has not consumed. Only synchronize_executable_writes may
                // advance the ranged-log cursor after processing those events.
                std::vector<GuestAddr> removed;
                const usize invalidated =
                    m_block_cache->invalidate_if_cached_page(addr, size, nullptr, &removed);
                if (inval_stats::g_enabled) {
                    inval_stats::note_event(size, removed.size());
                    for (GuestAddr a : removed) inval_stats::note_removed(a);
                }
                if (invalidated == 0) {
                    // A direct incoming edge independently retains its
                    // target page after the target BlockCache entry is
                    // evicted, and so does a shared-table record no thread
                    // currently caches. Such a write has no local block to
                    // remove, but it still invalidates the embedded host
                    // branch and the shared record.
                    if (m_memory.jit_code_pages_overlap(addr, size)) {
                        (void)JitCompileMemo::purge_shared_records_overlapping(addr, size, generation);
                        m_jit_ctx.block_cache_l1 = nullptr;
                        invalidate_return_predictors();
                        unlink_direct_links_overlapping(addr, size);
                        return true;
                    }
                    return false;
                }
                // One owner for invalidation: the writing thread removes the
                // shared record so no thread can adopt it afterwards. Other
                // threads' L1/sticky entries converge at their own sync.
                (void)JitCompileMemo::purge_shared_records_overlapping(addr, size, generation);
                m_jit_ctx.block_cache_l1 = nullptr;
                invalidate_return_predictors();
                unlink_direct_links_overlapping(addr, size);
                note_invalidated_blocks(removed);
                return true;
            });
    } else {
        m_memory.clear_thread_executable_write_observer();
    }
}

bool JitCompiler::synchronize_executable_writes() {
    // Read before the events: an unlink recorded after this point may belong
    // to a write this sync does not see yet (try_patch_direct_link checks).
    const u64 unlink_seq = unlink_sequence();
    const u64 generation = m_memory.executable_generation();
    if (generation == m_executable_generation) {
        return false;
    }
    m_unlink_seq_at_sync = unlink_seq;
    // Sites this thread patched since its last sync may lead into a
    // translation one of these writes made stale, if the write's observer
    // unlinked before the site was filed: replay the unlink for every event.
    const bool replay_all = m_patched_since_sync;
    m_patched_since_sync = false;
    if (!m_block_cache) {
        m_executable_generation = generation;
        return true;
    }

    // Foreign-thread FlushInstructionCache / executable stores bump the
    // process-wide generation. Apply the retained ranged log instead of
    // wiping the entire block cache — GPU-process self-modifying code made the
    // old clear()+bzero path dominate the main thread.
    const auto events =
        m_memory.collect_executable_writes_after(m_executable_generation);
    for (const auto& event : events) {
        if (event.addr == 0 && event.size == 0) {
            std::lock_guard<std::mutex> lock(m_compile_mutex);
            m_block_cache->clear();
            m_jit_ctx.block_cache_l1 = nullptr;
            invalidate_return_predictors();
            bump_link_generation(4);
            continue;
        }
        std::vector<GuestAddr> removed;
        const usize invalidated = m_block_cache->invalidate_if_cached_page(
            event.addr, event.size, nullptr, &removed);
        if (inval_stats::g_enabled) {
            for (GuestAddr a : removed) inval_stats::note_removed(a);
        }
        // The writer's observer already purged the shared table when the
        // writer had a JIT; a foreign writer (host-side FlushInstructionCache)
        // has none, so the purge must also run here. Replaying an already
        // purged write on every thread serialised them all on the memo lock.
        if (!event.writer_purged) {
            (void)JitCompileMemo::purge_shared_records_overlapping(
                event.addr, event.size, event.generation);
        }
        if (invalidated == 0) {
            // The writer's observer only reports a write that touched JIT
            // code, and already unlinked the edges into it. Re-scanning (a
            // shared lock for high pages) on every thread serialised them.
            if (event.writer_purged) {
                m_jit_ctx.block_cache_l1 = nullptr;
                invalidate_return_predictors();
                if (replay_all) {
                    unlink_direct_links_overlapping(event.addr, event.size,
                                                    /*writer=*/false);
                }
                continue;
            }
            if (m_memory.jit_code_pages_overlap(event.addr, event.size)) {
                m_jit_ctx.block_cache_l1 = nullptr;
                invalidate_return_predictors();
                // A foreign writer had no observer: nothing unlinked yet.
                unlink_direct_links_overlapping(event.addr, event.size,
                                                /*writer=*/false);
            }
            continue;
        }
        m_jit_ctx.block_cache_l1 = nullptr;
        invalidate_return_predictors();
        // This thread held a translation of the written page: any site it
        // patched into one after the writer's unlink is failed closed here.
        unlink_direct_links_overlapping(event.addr, event.size,
                                        /*writer=*/false);
        note_invalidated_blocks(removed);
    }

    m_executable_generation = generation;
    return true;
}

bool JitCompiler::synchronize_arena_generation() {
    const u64 retire_seq = JitCodeArena::s_arena_retire_seq.load(std::memory_order_acquire);
    // Taking or arming a chunk invalidates nothing: every previously
    // published pointer still addresses live code. Only a retirement round
    // invalidates, and only the translations JitCodeArena::arena_code_live() now refuses.
    if (retire_seq == m_arena_retire_seq) {
        return false;
    }
    m_arena_retire_seq = retire_seq;
#if defined(__aarch64__)
    // A retirement rewrote patch sites in live chunks (incoming direct links
    // failed closed) and invalidated their I-cache lines before bumping the
    // sequence this thread just observed. The context synchronisation makes
    // sure nothing this core prefetched through an old patch survives into
    // the native code it is about to enter.
    __asm__ volatile("isb" ::: "memory");
#endif
    if (m_block_cache) {
        // Drop the compiled pointers, keep the decodes: the guest bytes did
        // not change, so re-decoding every cached block would be pure loss.
        //
        // Each dropped translation is also reported to the arena's
        // retirement-thrash accounting, keyed by its guest address and the
        // hash of the guest bytes it was compiled from (the same FNV-1a
        // compile_attempt() keys the memo with). The bytes are unchanged
        // while the block is cached: an executable write removes the entry.
        m_block_cache->drop_compiled_if([this](const BasicBlock& block) {
            if (JitCodeArena::arena_code_live(block.compiled_code,
                                              block.compiled_arena_generation)) {
                return false;
            }
            if (block.byte_length != 0) {
                if (const Byte* bytes =
                        m_memory.fetch_ptr(block.start_addr, block.byte_length)) {
                    u64 hash = 14695981039346656037ull;
                    for (u32 i = 0; i < block.byte_length; ++i) {
                        hash ^= bytes[i];
                        hash *= 1099511628211ull;
                    }
                    JitCodeArena::note_retired_translation(block.start_addr,
                                                           hash);
                }
            }
            return true;
        });
        m_jit_ctx.block_cache_l1 = nullptr;
        invalidate_return_predictors();
    }
    {
        std::lock_guard<std::mutex> lock(m_compile_mutex);
        // Erase only retired records. A surviving record may own the decoded
        // array whose element addresses live generated code baked in, so
        // clearing wholesale would free memory that mapped code still reads.
        std::erase_if(m_compiled_blocks, [](const CompiledBlock& block) {
            return !JitCodeArena::arena_code_live(block.code, block.arena_generation);
        });
        // A patch site whose own chunk died must never be written again; its
        // target retains go with it. Sites in live chunks whose TARGET died
        // were already failed closed by the retirement and are re-patched
        // when the target is recompiled, so they stay.
        // The retains are per target and serve every site into it, so they
        // go only when the target's LAST site goes.
        for (auto it = m_direct_patch_targets.begin();
             it != m_direct_patch_targets.end();) {
            DirectPatchTarget& entry = it->second;
            entry.erase_if([](const DirectPatchSite& site) {
                return !JitCodeArena::arena_code_live(site.code, site.arena_generation);
            });
            if (!entry.empty()) {
                ++it;
                continue;
            }
            release_patch_target_retains(it->first, entry);
            it = m_direct_patch_targets.erase(it);
        }
    }
    return true;
}

// Inline block-link tail enable flag (Stage B1).  Default OFF.
std::atomic<bool>                                              JitCompiler::s_jit_link_enabled{false};
std::atomic<u32>                                               JitCompiler::s_link_generation{1};
std::atomic<u32>                                               JitCompiler::s_predictor_epoch{1};

} // namespace vitreo::cpu::jit
