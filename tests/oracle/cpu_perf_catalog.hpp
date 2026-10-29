// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file cpu_perf_catalog.hpp
/// @brief Looped 32-bit JIT throughput catalog (Windows / the host x86 runner / Vitreo).
///
/// Two-instruction fall-through bodies are call/restore-bound. Each entry
/// here is an inner x86 loop of 256 iterations, same shape as walk_loop256.
/// Do not emit 0x49 (DEC ECX): the host x86 runner is 64-bit CS and treats
/// it as REX. Use SUB ECX, 1 (83 E9 01).
///
/// Generic ISA, not title-specific. Classes match hot-dump labels: alu,
/// mem, sse, x87, jcc.
///
/// `bench_on_jit` enables a linear GuestMemory so SSE mem128 uses the same
/// UXTW path as a 32-bit production guest. Scattered test memory makes
/// every MOVAPS a `jit_sse_op` (~10× the host runner) and is not a JIT regress.

#include "oracle/cpu_perf_oracle.hpp"

#include <cstdint>
#include <vector>

namespace vitreo::oracle::perf {

struct CatalogEntry {
    const char* name;
    const char* cls;
    std::vector<uint8_t> body;
};

/// `mov ecx, 256; <kernel>; sub ecx, 1; jnz kernel`.
[[nodiscard]] std::vector<uint8_t> loop256(const std::vector<uint8_t>& kernel);

/// Same with ESI as the counter. Use when the kernel already owns ECX
/// (captured walk blocks).
[[nodiscard]] std::vector<uint8_t> loop256_esi(const std::vector<uint8_t>& kernel);

/// @p prefix once, then loop256(@p kernel).
[[nodiscard]] std::vector<uint8_t> prefix_loop256(
    const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& kernel);

[[nodiscard]] std::vector<CatalogEntry> cpu_perf_catalog();

/// Same with EBX as the counter. Use when the kernel owns ECX and ESI
/// (REP string kernels set their own ESI/EDI/ECX).
[[nodiscard]] std::vector<uint8_t> loop256_ebx(const std::vector<uint8_t>& kernel);

/// `mov ebp,esp; mov esp,<scratch>; loop256(kernel); mov esp,ebp`.
/// Lets PUSH/POP/CALL kernels run without disturbing the host stack the
/// Windows and the host x86 runner trampolines return on.
[[nodiscard]] std::vector<uint8_t> stack_loop256(const std::vector<uint8_t>& kernel);

/// Extended per-class catalog: the instruction families the base catalog
/// does not cover (LOCK atomics, division, string ops, scalar SSE, x87
/// with finite operands, stack/branch shapes) plus two harness-overhead
/// baselines. Each entry carries the input State it needs.
struct ClassEntry {
    const char* name;
    const char* cls;
    std::vector<uint8_t> body;
    State in;
};

/// scratch_state() with every dword of the 512-byte window set to 1.0f.
[[nodiscard]] State fp_scratch_state();
/// scratch_state() with every dword a 16-aligned pointer inside the window.
[[nodiscard]] State chase_scratch_state();

[[nodiscard]] std::vector<ClassEntry> cpu_perf_class_catalog();

[[nodiscard]] Report bench_class_catalog(uint32_t warmup, uint32_t iters,
                                         bool with_windows);


/// Bench every catalog row. Windows column is filled when @p with_windows
/// and `windows_ready()`.
[[nodiscard]] Report bench_catalog(uint32_t warmup, uint32_t iters,
                                   bool with_windows);

}  // namespace vitreo::oracle::perf
