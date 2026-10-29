// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_compile_internal.hpp
/// @brief State shared by the jit_compiler.cpp translation units
///
/// jit_compiler.cpp was one 7,488-line file, so everything below was a
/// file-local symbol. Splitting it by pure code motion left these with
/// users in more than one of its parts; every other former file-local
/// stays file-local in the part that now holds all of its users. Private:
/// include it only from those jit_*.cpp parts, never from a header.

#include <atomic>
#include <pthread.h>

#include "vitreo/types.hpp"
#include "jit_compiler.hpp"

namespace vitreo::cpu::jit {

// W^X state (defined in jit_execute.cpp). constinit: no dynamic
// initialization, so other parts access it directly, not through the
// thread_local wrapper call. It must stay one variable per thread.
extern constinit thread_local __attribute__((tls_model("initial-exec")))
    bool t_jit_writable;

// @p slot is this thread's t_jit_writable (execute() caches its address).
inline void jit_set_writable_at(bool* slot, bool writable) {
    if (*slot == writable) return;
    pthread_jit_write_protect_np(writable ? 0 : 1);
    *slot = writable;
}
inline void jit_set_writable(bool writable) {
    jit_set_writable_at(&t_jit_writable, writable);
}

// Cached diagnostic env knobs (defined in jit_finalize.cpp).
extern const bool g_env_no_pin;
extern const bool g_env_trace_opcodes;
extern const bool g_env_enable_idioms;

// Compile-memo counters (defined in jit_compile_stats.cpp).
extern std::atomic<u64> g_memo_delegate_reuses;
extern std::atomic<u64> g_memo_native_publishes;
extern std::atomic<u64> g_memo_native_reuses;
extern std::atomic<u64> g_memo_compile_probes;
extern std::atomic<u64> g_memo_compile_hits;
extern std::atomic<u64> g_fresh_emits;
extern std::atomic<u64> g_cold_probes;
extern std::atomic<u64> g_cold_adoptions;
extern std::atomic<u64> g_cold_hash_mismatches;
extern std::atomic<u64> g_cold_demoted;
bool memo_stats_enabled() noexcept;
void memo_stats_arm_atexit() noexcept;

// Stack-proof outcome counters (defined in jit_compile_stats.cpp).
enum class StackProofOutcome : u8 {
    Eligible,
    NoStackLoad,
    OneStackLoad,
    RspWrite,
    OtherMemory,
    BadStackLoad,
    OtherOpcode,
    Count,
};
bool stack_proof_stats_enabled() noexcept;
void record_stack_proof_outcome(StackProofOutcome outcome,
                                Opcode rejected_opcode,
                                u32 stack_load_count) noexcept;

// Emit attribution switch (defined in jit_compile_stats.cpp).
extern std::atomic<bool> g_emit_attribution_on;

// Block-entry diagnostics (defined in jit_block_watch.cpp).
u64* rip_counter_slot(GuestAddr addr);
void report_watched_block(CpuState* cpu, GuestMemory* memory, u64 rip);

} // namespace vitreo::cpu::jit
