// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_symbol_map.hpp
/// @brief Make emitted code addressable by an external profiler.
///
/// JIT code lives in an anonymous MAP_JIT reservation, so every sampling
/// profiler on this platform reports it as an unresolved address: `sample`
/// prints `???`, and Instruments has no dSYM to consult. That is the reason
/// the block-transition cost has been narrowed by elimination rather than
/// measured — every candidate mechanism reachable from the source has been
/// tested and rejected, and what is left needs cycle attribution inside the
/// emitted code itself.
///
/// This writes the missing half: one line per compiled block naming the host
/// address range and what is inside it. A profiler's raw addresses can then be
/// resolved with `tools/jit_bench/resolve_jit_symbols.py`.
///
/// Format, one block per line, all values hex without `0x` except the counts:
///
///     <host_start> <size> <guest_rip> <link_off> <call_off> <native_insns>
///
/// `link_off` matters as much as the range: a linked hop enters there rather
/// than at the start, so a sample's offset relative to it says whether the
/// time is in the dispatcher prologue, the block body, or the tail.
///
/// Off unless `VITREO_JIT_SYMBOL_MAP` names a writable path. It appends, and
/// blocks are appended as they are compiled, because arena chunk retirement
/// recycles addresses: a map written at exit would be missing every block
/// whose chunk had been reused, and would silently mis-resolve the rest.
/// A later line for an overlapping range therefore supersedes an earlier one,
/// and the resolver honours that ordering.
///
/// "%p" in the path expands to the host pid. `VITREO_JIT_SYMBOL_MAP_CODE`
/// adds each block's words (and a split block's cold part, on a `cold` line);
/// `VITREO_JIT_SYMBOL_MAP_RIPS=<file>` of hex guest RIPs, one per line,
/// restricts the map to blocks starting at those RIPs -- the hot set of an
/// earlier profile, whose code can then be read without dumping every block.

#include <vitreo/common.hpp>
#include <cstdio>

namespace vitreo::cpu::jit {

struct CompiledBlock;

namespace symbol_map {

/// Whether a map path was given. Read once; a single relaxed load on the
/// compile path when disabled.
[[nodiscard]] bool enabled() noexcept;

/// Append one block. Cheap and off by default; called from the two places a
/// CompiledBlock's host range becomes final.
void record(const CompiledBlock& block) noexcept;

/// Record fresh finalized code before it becomes visible in the shared memo.
void record_provenance(const CompiledBlock& block) noexcept;

/// Flush the opt-in bounded VITREO_JIT_PROVENANCE history and append a query
/// watermark. Invoked on the hang-dump servicing thread, never in a signal.
void flush_provenance(std::FILE* diagnostic) noexcept;

/// Whether record() wrote the code of the block at `guest_addr`, so its
/// instruction sites may follow (code dump on, block passes the RIP filter).
[[nodiscard]] bool wants_sites(u64 guest_addr) noexcept;

/// One `site <host_begin> <host_end> <opcode>` line for the block just
/// recorded: the precise-RIP interval of one guest instruction (byte offsets
/// from the block's code, cold ones by their placed offset), so a census can
/// charge every word to the x86 instruction that emitted it.
void record_site(u32 host_begin, u32 host_end, const char* opcode) noexcept;

}  // namespace symbol_map
}  // namespace vitreo::cpu::jit
