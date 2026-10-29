// The process-wide immortal helper page of the Tier-0 memory lowering
// and every stub published on it: the helper table and its mutex and
// generation, the live-JitContext registry, the hot linear64 helpers,
// the capture-CF / raise-exit / link-probe / RSB-push stubs and the
// shared dispatch tail. The state jit_memory_emit_internal.hpp declares
// is defined here, once.
//
// Split out of jit_memory_emit.cpp as pure code motion: every line below
// the includes is verbatim from that file, whose header has the history.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
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
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/base/perf_window.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#include "jit_memory_emit_internal.hpp"

namespace vitreo::cpu::jit {

// Moved here with its only caller when the memory lowering was split out of
// jit_compiler.cpp; nothing left behind reads it.
const bool g_env_no_single_page_load =
    std::getenv("VITREO_JIT_NO_SINGLE_PAGE_LOAD") != nullptr;

namespace {
const bool g_env_mem_helper_map =
    std::getenv("VITREO_JIT_MAP_HELPERS") != nullptr;
}  // namespace

// Immutable process-lifetime bodies have no unique guest RIP. Record their
// own identity and emitted words instead of assigning a sampled helper PC
// to whichever guest instruction happened to call it. All callers hold the
// helper publication mutex; output is bounded by the 64 KiB helper page.
void record_mem_helper(const void* native, const std::vector<u32>& words,
                       const char* kind, int width) {
    if (!g_env_mem_helper_map) return;
    static std::FILE* const file = [] {
        const char* dir = std::getenv("VITREO_LOG_DIR");
        if (dir == nullptr || dir[0] == '\0') return stderr;
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/jithelpers_%d.txt", dir,
                      static_cast<int>(::getpid()));
        std::FILE* opened = std::fopen(path, "a");
        return opened == nullptr ? stderr : opened;
    }();
    std::fprintf(file, "[jit-helper-map] native=%p size=%zu kind=%s width=%d words=",
                 native, words.size() * sizeof(u32), kind, width);
    for (const u32 word : words) std::fprintf(file, "%08x", word);
    std::fputc('\n', file);
    std::fflush(file);
}

// Leaked: JitCompiler destructors (unregister_live_jit_context) and
// execute() (sync_process_wide_stubs) lock it, possibly during exit(), after
// static destruction would have destroyed a namespace-scope mutex.
[[nodiscard]] std::mutex& g_scalar_mem_helper_mu() {
    static auto* mu = new std::mutex;
    return *mu;
}
// Helpers are immutable and process-lifetime. Publish a new generation only
// after code and all associated pointers are initialized under the mutex.
std::atomic<u64> g_mem_helper_generation{1};
void* g_scalar_mem_helper_page = nullptr;
usize g_scalar_mem_helper_used = 0;

std::vector<std::pair<ScalarMemHelperKey, void*>> g_scalar_mem_helpers;

namespace {
void* g_process_raise_exit = nullptr;
void* g_capture_cf_stub = nullptr;
void* g_process_continue_exit = nullptr;
void* g_link_probe[4]{};
void* g_rsb_push = nullptr;
void* g_dispatch_tail[8]{};
void* g_linear64_hot_mem_wide[4]{};
// The same helpers emitted under JitConfig::linear64_jit_view, whose X15 is
// the JIT view: their check-map probe reloads the map from JitContext.
void* g_linear64_hot_mem_wide_view[4]{};

// Apple cores mispredict BLR/BR targets gigabytes from the branch: a hot
// trampoline into a helper page mapped wherever mmap chose cost call_ret
// ~170 cycles per CALL/RET pair. Use the arena's stub page below chunk 0.
// VITREO_JIT_HELPER_PAGE_FAR=1 restores the separate mapping.
[[nodiscard]] bool helper_page_near_arena() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_HELPER_PAGE_FAR");
        return v == nullptr || v[0] == '\0' || v[0] == '0';
    }();
    return on;
}
}  // namespace

[[nodiscard]] bool map_helper_page() {
    static_assert(kScalarMemHelperPage == JitCodeArena::kSharedStubPageSize);
    void* page = nullptr;
    if (helper_page_near_arena()) {
        JitCodeArena::ensure_initialized();
        page = JitCodeArena::shared_stub_page();
    }
    if (page == nullptr) {
        page = mmap(nullptr, kScalarMemHelperPage,
                    PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
        if (page == MAP_FAILED) return false;
    }
    g_scalar_mem_helper_page = page;
    g_scalar_mem_helper_used = 0;
    (void)note_jit_code_range(
        reinterpret_cast<uintptr_t>(page),
        reinterpret_cast<uintptr_t>(page) + kScalarMemHelperPage);
    note_shared_mem_helper_range(
        reinterpret_cast<uintptr_t>(page),
        reinterpret_cast<uintptr_t>(page) + kScalarMemHelperPage);
    return true;
}

namespace {
[[nodiscard]] void* copy_words_to_helper_page(const std::vector<u32>& words) {
    const usize bytes = words.size() * sizeof(u32);
    if (g_scalar_mem_helper_page == nullptr && !map_helper_page()) {
        return nullptr;
    }
    const usize aligned = (g_scalar_mem_helper_used + 15u) & ~usize{15};
    if (aligned + bytes > kScalarMemHelperPage) return nullptr;
    void* dest = static_cast<u8*>(g_scalar_mem_helper_page) + aligned;
    pthread_jit_write_protect_np(0);
    std::memcpy(dest, words.data(), bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(dest, bytes);
    g_scalar_mem_helper_used = aligned + bytes;
    return dest;
}
}  // namespace

[[nodiscard]] void* lookup_scalar_mem_helper(const ScalarMemHelperKey& key) {
    for (const auto& entry : g_scalar_mem_helpers) {
        if (entry.first.helper_off == key.helper_off &&
            entry.first.width == key.width &&
            entry.first.is_store == key.is_store &&
            entry.first.pins == key.pins &&
            entry.first.is_64 == key.is_64 &&
            entry.first.tso == key.tso &&
            entry.first.linear_reload == key.linear_reload &&
            entry.first.jit_view == key.jit_view) {
            return entry.second;
        }
    }
    return nullptr;
}

namespace {
// Every live JitContext, with the configuration bits that select its helper
// variants. Compiled code is shared between threads -- a thread adopts blocks
// another compiled (try_adopt_shared_block) and enters code through direct
// links another thread patched into shared code -- and it reaches the
// process-wide stubs above through the RUNNING thread's JitContext (LDR X16,
// [X20, #slot]; BLR/BR X16). So a stub must be in EVERY context before code
// that loads it can run anywhere: its publisher fills every registered
// context, under the mutex, before that code is finalized, and a context
// registered later is filled at registration. A context that missed one ran
// adopted code into BLR X16 = 0 (faulting 32-bit thread: PC=0, X16=0, LR
// in JIT code, on its ThreadEntry thread). Leaked, so a compiler destroyed
// during process teardown never touches a destroyed vector.
struct LiveJitContext {
    JitContext* ctx;
    bool linear64;
    bool jit_view;
    u8 tso;
};

[[nodiscard]] std::vector<LiveJitContext>& live_jit_contexts() {
    static auto* contexts = new std::vector<LiveJitContext>();
    return *contexts;
}

// Copy every published process-wide stub `live` lacks. Set-once: a slot that
// already holds a pointer is never rewritten. Caller holds
// g_scalar_mem_helper_mu.
void fill_jit_context_locked(const LiveJitContext& live) {
    // Release stores: `ctx` may belong to another thread, whose compiled code
    // loads these slots while this runs.
    JitContext& ctx = *live.ctx;
    if (ctx.capture_cf_stub == nullptr) {
        __atomic_store_n(&ctx.capture_cf_stub, g_capture_cf_stub,
                         __ATOMIC_RELEASE);
    }
    if (ctx.raise_exit == nullptr) {
        __atomic_store_n(&ctx.raise_exit, g_process_raise_exit,
                         __ATOMIC_RELEASE);
    }
    if (ctx.continue_exit == nullptr) {
        __atomic_store_n(&ctx.continue_exit, g_process_continue_exit,
                         __ATOMIC_RELEASE);
    }
    for (u32 kind = 0; kind < 4; ++kind) {
        if (ctx.link_probe[kind] == nullptr) {
            __atomic_store_n(&ctx.link_probe[kind], g_link_probe[kind],
                             __ATOMIC_RELEASE);
        }
    }
    if (ctx.rsb_push == nullptr) {
        __atomic_store_n(&ctx.rsb_push, g_rsb_push, __ATOMIC_RELEASE);
    }
    // Before the tier tail: a bound tier site loads this base on every entry,
    // and binding requires the tail.
    if (ctx.tier_counters == nullptr) {
        __atomic_store_n(&ctx.tier_counters, JitCompiler::tier_counter_base(),
                         __ATOMIC_RELEASE);
    }
    for (u32 kind = 0; kind < 8; ++kind) {
        if (ctx.dispatch_tail[kind] == nullptr) {
            __atomic_store_n(&ctx.dispatch_tail[kind], g_dispatch_tail[kind],
                             __ATOMIC_RELEASE);
        }
    }
    if (!live.linear64) return;
    static constexpr int kWidths[] = {1, 2, 4, 8};
    for (int store = 0; store < 2; ++store) {
        for (int width : kWidths) {
            const u32 slot = linear64_cold_mem_slot(store != 0, width);
            if (ctx.linear64_cold_mem[slot] != nullptr) continue;
            const ScalarMemHelperKey key{
                helper_off_for_width(store != 0, width),
                width,
                static_cast<u8>(store),
                1,
                1,
                live.tso,
                1,
                static_cast<u8>(live.jit_view ? 1 : 0),
            };
            __atomic_store_n(&ctx.linear64_cold_mem[slot],
                             lookup_scalar_mem_helper(key), __ATOMIC_RELEASE);
        }
    }
    void* const* const hot_wide =
        live.jit_view ? g_linear64_hot_mem_wide_view : g_linear64_hot_mem_wide;
    for (u32 slot = 0; slot < 4; ++slot) {
        if (ctx.linear64_hot_mem_wide[slot] == nullptr) {
            __atomic_store_n(&ctx.linear64_hot_mem_wide[slot], hot_wide[slot],
                             __ATOMIC_RELEASE);
        }
    }
}
}  // namespace

// After a publisher added a stub: every live context gets it. Caller holds
// g_scalar_mem_helper_mu.
void fill_live_jit_contexts_locked() {
    for (const LiveJitContext& live : live_jit_contexts()) {
        fill_jit_context_locked(live);
    }
}

void JitCompiler::register_live_jit_context() {
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    const LiveJitContext live{&m_jit_ctx, m_cfg.linear64,
                              m_cfg.linear64_jit_view,
                              static_cast<u8>(m_cfg.tso_mode)};
    live_jit_contexts().push_back(live);
    fill_jit_context_locked(live);
    m_mem_helper_generation =
        g_mem_helper_generation.load(std::memory_order_relaxed);
}

void JitCompiler::unregister_live_jit_context() noexcept {
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    auto& contexts = live_jit_contexts();
    contexts.erase(std::remove_if(contexts.begin(), contexts.end(),
                                  [this](const LiveJitContext& live) {
                                      return live.ctx == &m_jit_ctx;
                                  }),
                   contexts.end());
}

void JitCompiler::sync_process_wide_stubs() {
    // Publishers already filled this context (fill_live_jit_contexts_locked);
    // this re-checks it at every dispatcher entry, for the cost of one
    // acquire load while nothing new was published.
    const u64 generation = g_mem_helper_generation.load(std::memory_order_acquire);
    if (m_mem_helper_generation == generation) return;
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    m_mem_helper_generation = g_mem_helper_generation.load(std::memory_order_relaxed);
    fill_jit_context_locked(LiveJitContext{&m_jit_ctx, m_cfg.linear64,
                                           m_cfg.linear64_jit_view,
                                           static_cast<u8>(m_cfg.tso_mode)});
}

void* JitCompiler::materialize_linear64_hot_helper(bool is_store, int width) {
    // Only hardware-protected wide accesses call a shared hot guard: today
    // the 256-bit forms (width 32). Width 16 is still accepted because the
    // wide slot table has 128-bit entries; scalar accesses are guarded inline.
    assert(width == 16 || width == 32);
    const u32 slot = linear64_hot_wide_slot(is_store, width);
    ts_note_stub(1u << (stored_stub::kHotWideShift + linear64_hot_wide_slot(is_store, width)));
    void** const global = m_cfg.linear64_jit_view
                              ? g_linear64_hot_mem_wide_view
                              : g_linear64_hot_mem_wide;
    void** const published = m_jit_ctx.linear64_hot_mem_wide;
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    if (global[slot] != nullptr) {
        published[slot] = global[slot];
        return global[slot];
    }

    SharedEmissionScope shared_scope(*this);
    std::vector<u32> saved;
    saved.swap(m_code_buf);
    const bool saved_esp = m_esp_frame_live;
    const bool saved_blr = m_flags.helper_blr_this_insn;

    const u8 ea = 9;
    const u8 last_arm = (width > 1) ? u8{17} : ea;
    if (width > 1) {
        emit(arm64::add_x_imm(last_arm, ea, static_cast<u32>(width - 1)));
        emit(arm64::orr_x(8, ea, last_arm));
        emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    } else {
        emit(arm64::lsr_x_imm(8, ea, m_cfg.linear64_address_bits));
    }
    const usize outside_pos = m_code_buf.size();
    emit(0);
    usize host_page_cross_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    if (width > 1 && !g_env_no_single_page_load) {
        emit(arm64::eor_x(8, ea, last_arm));
        emit(arm64::ubfx_x(8, 8, static_cast<u8>(kLinearHostPageShift), 1));
        host_page_cross_pos = m_code_buf.size();
        emit(0);
        const u8 index_width = static_cast<u8>(
            m_cfg.linear64_address_bits - kLinearHostPageShift);
        emit(arm64::ubfx_x(4, last_arm,
                           static_cast<u8>(kLinearHostPageShift),
                           index_width));
        emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(3), 4));
        mixed_page_pos = m_code_buf.size();
        emit(0);
    } else {
        emit_linear64_check_page_test(ea, last_arm, width, mixed_page_pos);
    }
    // The host access stays at the site: a faulting LDAPR/STLR here would
    // sit under an extra BLR frame, and the Mach trampoline pops only the
    // block prologue.
    emit(arm64::movz_w(16, 0));
    emit(arm64::ret());

    // A MIXED host page: decide the access from the per-guest-page bits
    // (GuestMemory::linear_subpage_map) and report success, so the caller's
    // own direct access proceeds, instead of sending it to the cold stub's
    // helper calls -- for a 128-bit access, two of them. Same decision as
    // the scalar cold stub's sub-page entry (emit_subpage_entry): a store
    // whose page also holds code or a write watch, a null map, or a denied
    // guest page fail as before. X3 holds the check byte; X9 the EA, X17 the
    // last byte (the access does not cross a host page: that branch is
    // above). X4/X8/X16 are scratch here, and everything is flag-neutral.
    usize subpage = static_cast<usize>(-1);
    // (pos, kind | reg): kind 0 = CBNZ W, 0x100 = TBZ #0, 0x400 = CBZ X.
    std::vector<std::pair<usize, u32>> sub_fails;
    static const bool subpage_enabled = [] {
        const char* v = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        return v == nullptr || v[0] != '0';
    }();
    // Under VITREO_JIT_NO_SINGLE_PAGE_LOAD a wide access may straddle a host
    // page and X3 is the OR of both endpoints' bytes: no single sub-page byte
    // decides it, so it keeps the helper.
    const bool single_host_page = width == 1 || !g_env_no_single_page_load;
    if (subpage_enabled && single_host_page &&
        mixed_page_pos != static_cast<usize>(-1)) {
        subpage = m_code_buf.size();
        const auto fail_cbnz_w = [&](u8 reg) {
            sub_fails.push_back({m_code_buf.size(), reg});
            emit(0);
        };
        const auto fail_tbz0 = [&](u8 reg) {
            sub_fails.push_back({m_code_buf.size(), 0x100u | reg});
            emit(0);
        };
        if (is_store) {
            emit(arm64::ubfx_w(8, 3, 1, 2));  // HoldsCode | Watched
            fail_cbnz_w(8);
        }
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                               JIT_CTX_OFF_LINEAR_SUBPAGE_MAP >> 3));
        sub_fails.push_back({m_code_buf.size(), 0x400u | 16u});  // CBZ X16
        emit(0);
        const u8 index_width = static_cast<u8>(
            m_cfg.linear64_address_bits - kLinearHostPageShift);
        emit(arm64::ubfx_x(8, ea, static_cast<u8>(kLinearHostPageShift),
                           index_width));
        emit(arm64::ldrb_reg(8, 16, 8));
        if (is_store) emit(arm64::lsr_w_imm(8, 8, 4));  // the write bits
        emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearGuestPageShift), 2));
        emit(arm64::lsrv_w(3, 8, 3));
        fail_tbz0(3);
        if (width > 1) {
            emit(arm64::ubfx_x(4, last_arm,
                               static_cast<u8>(kLinearGuestPageShift), 2));
            emit(arm64::lsrv_w(4, 8, 4));
            fail_tbz0(4);
        }
        emit(arm64::movz_w(16, 0));
        emit(arm64::ret());
    }

    const usize fail = m_code_buf.size();
    for (const auto& [pos, kind] : sub_fails) {
        const u8 reg = static_cast<u8>(kind & 0xFFu);
        if (kind & 0x100u) {
            m_code_buf[pos] = arm64::tbz(
                reg, 0, cold_branch_disp(pos, fail, 14, "hot mem sub-page denied"));
        } else if (kind & 0x400u) {
            m_code_buf[pos] = arm64::cbz_x(
                reg, cold_branch_disp(pos, fail, 19, "hot mem sub-page map"));
        } else {
            m_code_buf[pos] = arm64::cbnz_w(
                reg, cold_branch_disp(pos, fail, 19, "hot mem sub-page reason"));
        }
    }
    m_code_buf[outside_pos] = arm64::cbnz_x(
        8, cold_branch_disp(outside_pos, fail, 19, "hot mem outside"));
    if (host_page_cross_pos != static_cast<usize>(-1)) {
        m_code_buf[host_page_cross_pos] = arm64::cbnz_x(
            8, cold_branch_disp(host_page_cross_pos, fail, 19,
                                "hot mem host-page cross"));
    }
    if (mixed_page_pos != static_cast<usize>(-1)) {
        const usize mixed_dest =
            subpage != static_cast<usize>(-1) ? subpage : fail;
        const i64 words =
            static_cast<i64>(mixed_dest) - static_cast<i64>(mixed_page_pos);
        const bool bit_test = !is_store && words >= -(i64{1} << 13) &&
                              words < (i64{1} << 13);
        m_code_buf[mixed_page_pos] =
            bit_test ? arm64::tbnz(
                           3, kLinearCheckMixedReadBit,
                           cold_branch_disp(mixed_page_pos, mixed_dest, 14,
                                            "hot mem mixed page"))
                     : arm64::cbnz_x(
                           3, cold_branch_disp(mixed_page_pos, mixed_dest, 19,
                                               "hot mem mixed page"));
    }

    emit(arm64::movz_w(16, 1));
    emit(arm64::ret());

    m_esp_frame_live = saved_esp;
    m_flags.helper_blr_this_insn = saved_blr;
    std::vector<u32> words = std::move(m_code_buf);
    m_code_buf = std::move(saved);

    void* dest = copy_words_to_helper_page(words);
    if (dest == nullptr) return nullptr;
    record_mem_helper(dest, words,
                      is_store ? "linear64-hot-store" : "linear64-hot-load", width);
    global[slot] = dest;
    published[slot] = dest;
    g_mem_helper_generation.fetch_add(1, std::memory_order_release);
    fill_live_jit_contexts_locked();
    return dest;
}

void* JitCompiler::materialize_capture_cf_stub() {
    ts_note_stub(stored_stub::kCaptureCf);
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    if (g_capture_cf_stub != nullptr) {
        m_jit_ctx.capture_cf_stub = g_capture_cf_stub;
        return g_capture_cf_stub;
    }
    // CpuState::lazy_cf(), in generated code, for one shared copy instead of
    // an inline walk at every carry consumer compiled with the record's kind
    // unknown (INC/DEC at a block start was ~90 words each). Same order of
    // questions: the CLC/STC/CMC overlay, then the record. The common kinds
    // are answered here -- the 64-bit ones too, which the inline walk sent to
    // the C++ helper -- and every other kind goes to jit_capture_cf, the
    // reference. Out: W21. Clobbers X0-X4, X16, X17, X30 and NZCV, as the
    // inline walk did; X12/X13 (pins) and X14/X15 survive the helper call.
    using F = CpuState::FlagOp;
    SharedEmissionScope shared_scope(*this);
    std::vector<u32> saved;
    saved.swap(m_code_buf);
    const u32 src_off = flag_src_offset();
    const u32 dst_off = flag_dst_offset();
    std::vector<std::pair<usize, u8>> to_arm;  // (b.cond pos, arm index)
    const auto branch_to = [&](u8 cond, u8 arm) {
        to_arm.push_back({m_code_buf.size(), arm});
        emit(cond);  // condition code; patched below
    };
    enum : u8 { kSub64, kAdd64, kZero, kIncDec, kSub32, kAdd32, kNone, kArms };
    emit(arm64::ldrb_uoff(3, REG_CPU_STATE, cf_overlay_offset()));
    const usize no_overlay = m_code_buf.size();
    emit(0);  // CBZ W3, record
    emit(arm64::sub_w_imm(REG_CALL_TMP, 3, 1));  // overlay 1 = CF 0, 2 = CF 1
    emit(arm64::ret());
    m_code_buf[no_overlay] = arm64::cbz_w(
        3, static_cast<i32>(m_code_buf.size() - no_overlay) * 4);
    emit(arm64::ldrb_uoff(3, REG_CPU_STATE, flag_op_offset()));
    emit(arm64::cmp_w_imm(3, static_cast<u32>(F::Sub64)));
    branch_to(arm64::EQ, kSub64);
    emit(arm64::cmp_w_imm(3, static_cast<u32>(F::Add64)));
    branch_to(arm64::EQ, kAdd64);
    static_assert(static_cast<u32>(F::Xor64) - static_cast<u32>(F::And8) == 11,
                  "And8..Xor64 must stay one contiguous range");
    emit(arm64::sub_w_imm(4, 3, static_cast<u32>(F::And8)));
    emit(arm64::cmp_w_imm(4, 11));
    branch_to(arm64::LS, kZero);
    static_assert(static_cast<u32>(F::Dec64) - static_cast<u32>(F::Inc8) == 7,
                  "Inc8..Dec64 must stay one contiguous range");
    emit(arm64::sub_w_imm(4, 3, static_cast<u32>(F::Inc8)));
    emit(arm64::cmp_w_imm(4, 7));
    branch_to(arm64::LS, kIncDec);
    emit(arm64::cmp_w_imm(3, static_cast<u32>(F::Sub32)));
    branch_to(arm64::EQ, kSub32);
    emit(arm64::cmp_w_imm(3, static_cast<u32>(F::Add32)));
    branch_to(arm64::EQ, kAdd32);
    static_assert(static_cast<u32>(F::None) == 0);
    emit(arm64::cmp_w_imm(3, 0));
    branch_to(arm64::EQ, kNone);
    // Every other kind: the C++ reference, with the caller-saved pins and
    // X14/X15 kept on the host stack (16-byte aligned throughout).
    emit(arm64::sub_x_imm(31, 31, 48));
    emit(arm64::stp_x_off(29, 30, 31, 0));
    emit(arm64::stp_x_off(12, 13, 31, 2));
    emit(arm64::stp_x_off(14, 15, 31, 4));
    emit(arm64::mov_x(0, REG_CPU_STATE));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_CAPTURE_CF >> 3));
    emit(0xD63F0200u);  // BLR X16
    emit(arm64::mov_w(REG_CALL_TMP, 0));
    emit(arm64::ldp_x_off(14, 15, 31, 4));
    emit(arm64::ldp_x_off(12, 13, 31, 2));
    emit(arm64::ldp_x_off(29, 30, 31, 0));
    emit(arm64::add_x_imm(31, 31, 48));
    emit(arm64::ret());
    usize arm_pos[kArms]{};
    // SUB: flag_dst = a - b, flag_src = b; the borrow is a < b, a = r + b.
    arm_pos[kSub64] = m_code_buf.size();
    emit(arm64::ldr_x_uoff(1, REG_CPU_STATE, src_off >> 3));
    emit(arm64::ldr_x_uoff(2, REG_CPU_STATE, dst_off >> 3));
    emit(arm64::add_x(3, 2, 1));
    emit(arm64::subs_x(31, 3, 1));
    emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
    emit(arm64::ret());
    // ADD: flag_dst = a + b, flag_src = b; the carry is r < b.
    arm_pos[kAdd64] = m_code_buf.size();
    emit(arm64::ldr_x_uoff(1, REG_CPU_STATE, src_off >> 3));
    emit(arm64::ldr_x_uoff(2, REG_CPU_STATE, dst_off >> 3));
    emit(arm64::subs_x(31, 2, 1));
    emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
    emit(arm64::ret());
    // AND/OR/XOR clear CF.
    arm_pos[kZero] = m_code_buf.size();
    emit(arm64::movz_w(REG_CALL_TMP, 0));
    emit(arm64::ret());
    // INC/DEC records carry the CF they kept in flag_src bit 32.
    arm_pos[kIncDec] = m_code_buf.size();
    emit(arm64::ldr_x_uoff(1, REG_CPU_STATE, src_off >> 3));
    emit(arm64::ubfx_x(REG_CALL_TMP, 1, 32, 1));
    emit(arm64::ret());
    arm_pos[kSub32] = m_code_buf.size();
    emit(arm64::ldr_w_uoff(1, REG_CPU_STATE, src_off >> 2));
    emit(arm64::ldr_w_uoff(2, REG_CPU_STATE, dst_off >> 2));
    emit(arm64::add_w(3, 2, 1));
    emit(arm64::subs_w(31, 3, 1));
    emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
    emit(arm64::ret());
    arm_pos[kAdd32] = m_code_buf.size();
    emit(arm64::ldr_w_uoff(1, REG_CPU_STATE, src_off >> 2));
    emit(arm64::ldr_w_uoff(2, REG_CPU_STATE, dst_off >> 2));
    emit(arm64::subs_w(31, 2, 1));
    emit(arm64::cset_w(REG_CALL_TMP, arm64::CC));
    emit(arm64::ret());
    // No pending record: RFLAGS is authoritative (CF is bit 0).
    arm_pos[kNone] = m_code_buf.size();
    emit(arm64::ldr_x_uoff(1, REG_CPU_STATE, cpu_rflags_offset() >> 3));
    emit(arm64::ubfx_x(REG_CALL_TMP, 1, 0, 1));
    emit(arm64::ret());
    for (const auto& [pos, arm] : to_arm) {
        const u8 cond = static_cast<u8>(m_code_buf[pos]);
        m_code_buf[pos] = arm64::b_cond(
            cond, static_cast<i32>(arm_pos[arm] - pos) * 4);
    }
    std::vector<u32> words = std::move(m_code_buf);
    m_code_buf = std::move(saved);

    void* dest = copy_words_to_helper_page(words);
    if (dest == nullptr) return nullptr;
    record_mem_helper(dest, words, "capture-cf", 0);
    g_capture_cf_stub = dest;
    m_jit_ctx.capture_cf_stub = dest;
    g_mem_helper_generation.fetch_add(1, std::memory_order_release);
    fill_live_jit_contexts_locked();
    return dest;
}

void* JitCompiler::materialize_process_raise_exit() {
    ts_note_stub(stored_stub::kRaiseExit);
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    if (g_process_raise_exit != nullptr) {
        m_jit_ctx.raise_exit = g_process_raise_exit;
        m_jit_ctx.continue_exit = g_process_continue_exit;
        return g_process_raise_exit;
    }

    SharedEmissionScope shared_scope(*this);
    std::vector<u32> saved;
    saved.swap(m_code_buf);
    const bool saved_esp = m_esp_frame_live;
    // Pins are written at the site so this body is safe for any host
    // register assignment. The Mach trampoline is not
    // involved: this is a software pending-raise exit.
    emit_persist_live_nzcv();
    emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                           jit_iters_offset() >> 2));
    emit_exit_reason(JitExitReason::EpilogueRaise);
    // Process-wide, so it cannot depend on one compiler's configuration:
    // always discard native guest-CALL frames. execute() publishes the base
    // on every entry, so with no frames this restores SP to itself.
    emit_discard_native_call_frames();
    emit(0xF84107FEu);  // LDR X30, [SP], #16
    emit(arm64::movz_w(0, static_cast<u16>(JitExecResult::Continue)));
    emit(arm64::ret());
    m_esp_frame_live = saved_esp;
    std::vector<u32> words = std::move(m_code_buf);
    m_code_buf = std::move(saved);

    const usize bytes = words.size() * sizeof(u32);
    if (g_scalar_mem_helper_page == nullptr && !map_helper_page()) {
        return nullptr;
    }
    const usize aligned = (g_scalar_mem_helper_used + 15u) & ~usize{15};
    if (aligned + bytes > kScalarMemHelperPage) return nullptr;
    void* dest = static_cast<u8*>(g_scalar_mem_helper_page) + aligned;
    pthread_jit_write_protect_np(0);
    std::memcpy(dest, words.data(), bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(dest, bytes);
    g_scalar_mem_helper_used = aligned + bytes;
    record_mem_helper(dest, words, "process-raise-and-continue-exit", 0);
    g_process_raise_exit = dest;
    m_jit_ctx.raise_exit = dest;
    // Last five words are the native-frame discard (LDR X17 / MOV SP) and
    // LDR X30 / MOVZ W0,Continue / RET — the ordinary fallthrough epilogue
    // after the site has already stored pins.
    g_process_continue_exit = static_cast<u8*>(dest) + (words.size() - 5) * 4;
    m_jit_ctx.continue_exit = g_process_continue_exit;
    g_mem_helper_generation.fetch_add(1, std::memory_order_release);
    fill_live_jit_contexts_locked();
    return dest;
}
void* JitCompiler::materialize_link_probe(u8 kind) {
    assert(kind < kLinkProbeKinds);
    ts_note_stub(1u << (stored_stub::kLinkProbeShift + kind));
    if (void* published = m_jit_ctx.link_probe[kind]) return published;
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    if (g_link_probe[kind] != nullptr) {
        m_jit_ctx.link_probe[kind] = g_link_probe[kind];
        return g_link_probe[kind];
    }
    SharedEmissionScope shared_scope(*this);
    std::vector<u32> saved;
    saved.swap(m_code_buf);
    emit_link_probe_body(kind);
    std::vector<u32> words = std::move(m_code_buf);
    m_code_buf = std::move(saved);
    void* dest = copy_words_to_helper_page(words);
    if (dest == nullptr) return nullptr;
    static constexpr const char* kNames[] = {
        "link-probe-l1", "link-probe-l1-counted", "link-probe-ret"};
    record_mem_helper(dest, words, kNames[kind], 0);
    g_link_probe[kind] = dest;
    m_jit_ctx.link_probe[kind] = dest;
    g_mem_helper_generation.fetch_add(1, std::memory_order_release);
    fill_live_jit_contexts_locked();
    return dest;
}

void* JitCompiler::materialize_rsb_push() {
    ts_note_stub(stored_stub::kRsbPush);
    if (void* published = m_jit_ctx.rsb_push) return published;
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    if (g_rsb_push != nullptr) {
        m_jit_ctx.rsb_push = g_rsb_push;
        return g_rsb_push;
    }
    SharedEmissionScope shared_scope(*this);
    std::vector<u32> saved;
    saved.swap(m_code_buf);
    emit_rsb_push_body();
    std::vector<u32> words = std::move(m_code_buf);
    m_code_buf = std::move(saved);
    void* dest = copy_words_to_helper_page(words);
    if (dest == nullptr) return nullptr;
    record_mem_helper(dest, words, "rsb-push", 0);
    g_rsb_push = dest;
    m_jit_ctx.rsb_push = dest;
    g_mem_helper_generation.fetch_add(1, std::memory_order_release);
    fill_live_jit_contexts_locked();
    return dest;
}

namespace {
std::atomic<int> g_shared_exit_stubs_override{-1};

// VITREO_JIT_<name>_LEGACY: unset, empty or 0 keeps the shared stub.
[[nodiscard]] bool shared_stub_env(const char* name) noexcept {
    const char* v = std::getenv(name);
    return v == nullptr || v[0] == '\0' || v[0] == '0';
}
}  // namespace

int shared_exit_stubs_override() noexcept {
    return g_shared_exit_stubs_override.load(std::memory_order_relaxed);
}

void JitCompiler::set_shared_exit_stubs_for_test(int value) noexcept {
    g_shared_exit_stubs_override.store(value, std::memory_order_relaxed);
}

bool JitCompiler::shared_dispatch_tail_enabled() noexcept {
    static const bool on = shared_stub_env("VITREO_JIT_DISPATCH_TAIL_LEGACY");
    const int forced = shared_exit_stubs_override();
    return forced >= 0 ? forced != 0 : on;
}

bool JitCompiler::shared_prologue_enabled() noexcept {
    static const bool on = shared_stub_env("VITREO_JIT_PROLOGUE_LEGACY");
    const int forced = shared_exit_stubs_override();
    return forced >= 0 ? forced != 0 : on;
}

bool JitCompiler::std_pin_block() const noexcept {
    if (!m_block_is_64bit || !m_cfg.linear64 || !m_pins_live) return false;
    for (u8 x = 0; x < 16; ++x) {
        if (m_pin_arm[x] != (x < 8 ? kStdPins64[x] : u8{0xFF})) return false;
    }
    return true;
}

bool JitCompiler::dispatch_tail_applies() const noexcept {
    return shared_dispatch_tail_enabled() && std_pin_block();
}

void* JitCompiler::materialize_dispatch_tail(u8 kind) {
    assert(kind < kDispatchTails);
    ts_note_stub(1u << (stored_stub::kDispatchTailShift + kind));
    if (void* published = m_jit_ctx.dispatch_tail[kind]) return published;
    // The raise tail ends in the process raise exit; publish that first
    // (it takes the same mutex).
    if (kind == kTailRaise && materialize_process_raise_exit() == nullptr) {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    if (g_dispatch_tail[kind] != nullptr) {
        m_jit_ctx.dispatch_tail[kind] = g_dispatch_tail[kind];
        return g_dispatch_tail[kind];
    }
    SharedEmissionScope shared_scope(*this);
    std::vector<u32> saved;
    saved.swap(m_code_buf);
    if (kind == kTailApiTest) {
        emit_api_test_body();
    } else if (kind == kColdCounter) {
        emit_cold_counter_tail_body();
    } else if (kind == kTierCounter) {
        emit_tier_counter_tail_body();
    } else if (kind == kEntryCheckMap || kind == kEntryJitView) {
        for (u8 i = 0; i < 8; i += 2) {
            emit(arm64::ldp_x_off(kStdPins64[i], kStdPins64[i + 1],
                                  REG_CPU_STATE, i));
        }
        emit(arm64::ldr_x_uoff(REG_LINEAR_BASE, REG_JIT_CTX,
                               JIT_CTX_OFF_LINEAR_BASE >> 3));
        emit(arm64::ldr_x_uoff(REG_LINEAR64_CHECK_MAP, REG_JIT_CTX,
                               (kind == kEntryJitView
                                    ? JIT_CTX_OFF_JIT_VIEW_BASE
                                    : JIT_CTX_OFF_LINEAR_CHECK_MAP) >> 3));
        emit(arm64::ret());
    } else {
    if (kind == kTailContinueLive) emit_persist_live_nzcv();
    if (kind == kTailContinueSpilled) emit_persist_spilled_nzcv();
    for (u8 i = 0; i < 8; i += 2) {
        emit(arm64::stp_x_off(kStdPins64[i], kStdPins64[i + 1],
                              REG_CPU_STATE, i));
    }
    if (kind == kTailRaise) {
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_RAISE_EXIT >> 3));
        emit(0xD61F0200u);  // BR X16
    } else {
        // The process continue exit: no native guest-CALL frames left, then
        // the dispatcher LR frame.
        emit_discard_native_call_frames();
        emit(0xF84107FEu);  // LDR X30, [SP], #16
        emit(arm64::movz_w(0, static_cast<u16>(JitExecResult::Continue)));
        emit(arm64::ret());
    }
    }
    std::vector<u32> words = std::move(m_code_buf);
    m_code_buf = std::move(saved);
    void* dest = copy_words_to_helper_page(words);
    if (dest == nullptr) return nullptr;
    static constexpr const char* kNames[] = {
        "dispatch-tail-continue-live", "dispatch-tail-continue-spilled",
        "dispatch-tail-raise", "dispatch-tail-api-test",
        "entry-check-map", "entry-jit-view", "cold-entry-counter",
        "ir-tier-counter"};
    static_assert(std::size(kNames) == kDispatchTails);
    record_mem_helper(dest, words, kNames[kind], 0);
    g_dispatch_tail[kind] = dest;
    m_jit_ctx.dispatch_tail[kind] = dest;
    g_mem_helper_generation.fetch_add(1, std::memory_order_release);
    fill_live_jit_contexts_locked();
    return dest;
}

bool JitCompiler::emit_dispatch_tail(u8 kind) {
    if (!dispatch_tail_applies() || materialize_dispatch_tail(kind) == nullptr) {
        return false;
    }
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, (JIT_CTX_OFF_DISPATCH_TAIL >> 3) + kind));
    emit(0xD61F0200u);  // BR X16
    return true;
}

}  // namespace vitreo::cpu::jit
