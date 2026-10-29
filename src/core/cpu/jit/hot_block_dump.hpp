// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/types.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::cpu::jit::hot_dump {

namespace detail {
// 0 unknown, 1 off, 2 on. Read once per dispatched block, so the disabled
// answer must inline to a single relaxed load rather than a cross-TU call.
extern std::atomic<int> g_state;
[[nodiscard]] bool hot_dump_init_slow() noexcept;

// Stable snapshot shape used by the writer and focused recorder tests. A
// code_variant identifies one distinct byte sequence observed at a RIP; the
// same bytes reuse their existing variant if that RIP is compiled again.
struct CapturedBlock {
    u64 rip = 0;
    u32 code_variant = 0;
    std::vector<u8> bytes;
    u32 native_insns = 0;
    bool delegated = false;
    u64 exec_count = 0;
    u64 insn_count = 0;
};

[[nodiscard]] std::vector<CapturedBlock> snapshot_for_testing();
void reset_for_testing() noexcept;
void record_exec_for_testing(u64 rip, u32 code_variant, u32 block_insns,
                             u32 sample_weight) noexcept;
}  // namespace detail

// VITREO_JIT_DUMP_HOT_TID optionally selects one host thread. Execution
// counts are 1:32 sampled dispatcher entries; linked block hops are excluded
// and that provenance is written into every dump row.
[[nodiscard]] inline bool enabled() noexcept {
    const int seen = detail::g_state.load(std::memory_order_relaxed);
    if (seen != 0) return seen == 2;
    return detail::hot_dump_init_slow();
}
inline constexpr u32 kInvalidCodeVariant = ~u32{0};
[[nodiscard]] u32 record_compile(u64 rip, const u8* bytes, u32 len,
                                 u32 native_insns, bool delegated) noexcept;
void record_exec(u64 rip, u32 code_variant, u32 block_insns) noexcept;
// Expand every "%p" token to the host process id. This lets a single
// inherited environment capture every process in a Windows process tree
// without the children overwriting one shared dump file.
[[nodiscard]] std::string expand_process_path(std::string_view path,
                                              u64 process_id);
// One-shot and never throws. Safe if g_mu is held or unusable (atexit +
// crash_handler). Subsequent calls are a no-op.
void dump() noexcept;

}  // namespace vitreo::cpu::jit::hot_dump
