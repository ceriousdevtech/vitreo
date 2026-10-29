#pragma once

// Private to the jit_memory_emit*.cpp translation units (not installed,
// not a SOURCES entry). The guest memory lowering was one file whose
// helper-page state was file-local; when it was split by cohesion, the
// names below gained callers in more than one part. The stateless key and
// slot helpers are defined here. The mutable state is only declared: the
// helper page, its cursor, the publication table, the generation counter
// and the mutex must each stay ONE object process-wide, so they are defined
// once, in jit_memory_emit_process_stubs.cpp, instead of being copied into
// an anonymous namespace per file (which would fork the helper page).

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#include "vitreo/types.hpp"
#include "jit_helpers.hpp"

namespace vitreo::cpu::jit {

// Process-wide linear64 try_region+helper bodies. n=1 blocks cannot share
// inside themselves; these live in an immortal MAP_JIT page so every site
// is a trampoline. TSO/pin/bitness variants are emitted lazily.
struct ScalarMemHelperKey {
    u32 helper_off = 0;
    int width = 0;
    u8 is_store = 0;
    u8 pins = 0;
    u8 is_64 = 0;
    u8 tso = 0;
    u8 linear_reload = 0;
    // JitConfig::linear64_jit_view: the helper's post-call reload puts the
    // JIT view's base in X15 instead of the check map, so the two modes must
    // never share a body (tests run both in one process).
    u8 jit_view = 0;
};

constexpr usize kScalarMemHelperPage = 65536;

[[nodiscard]] constexpr u32 linear64_cold_mem_slot(bool is_store,
                                                   int width) noexcept {
    const u32 width_log = width == 1 ? 0u : width == 2 ? 1u : width == 4 ? 2u : 3u;
    return (is_store ? 4u : 0u) + width_log;
}

[[nodiscard]] constexpr u32 linear64_hot_wide_slot(bool is_store,
                                                   int width) noexcept {
    return (width == 32 ? 2u : 0u) + (is_store ? 1u : 0u);
}

[[nodiscard]] constexpr u32 helper_off_for_width(bool is_store, int width) noexcept {
    if (is_store) {
        return width == 1 ? JIT_CTX_OFF_WRITE8
             : width == 2 ? JIT_CTX_OFF_WRITE16
             : width == 4 ? JIT_CTX_OFF_WRITE32
                          : JIT_CTX_OFF_WRITE64;
    }
    return width == 1 ? JIT_CTX_OFF_READ8
         : width == 2 ? JIT_CTX_OFF_READ16
         : width == 4 ? JIT_CTX_OFF_READ32
                      : JIT_CTX_OFF_READ64;
}

// Defined in jit_memory_emit_process_stubs.cpp.
extern const bool g_env_no_single_page_load;
void record_mem_helper(const void* native, const std::vector<u32>& words,
                       const char* kind, int width);
[[nodiscard]] std::mutex& g_scalar_mem_helper_mu();
extern std::atomic<u64> g_mem_helper_generation;
extern void* g_scalar_mem_helper_page;
extern usize g_scalar_mem_helper_used;
extern std::vector<std::pair<ScalarMemHelperKey, void*>> g_scalar_mem_helpers;
[[nodiscard]] bool map_helper_page();
[[nodiscard]] void* lookup_scalar_mem_helper(const ScalarMemHelperKey& key);
void fill_live_jit_contexts_locked();

}  // namespace vitreo::cpu::jit
