#pragma once
/// @file block_decoder.hpp
/// @brief Decode a guest basic block for the JIT.
///
/// Reads instruction bytes straight out of guest memory and decodes forward
/// until the block ends — at a branch, a call, a return, or an instruction the
/// decoder does not handle. The JIT compiles what comes back; the interpreter
/// runs anything the decode stopped short of.
///
/// Guest memory is the source, so a read can fail at any point: a block may
/// run into an unmapped page mid-instruction, and the decode has to stop
/// cleanly rather than fault.

#include <vitreo/common.hpp>

#include "core/cpu/jit/block_cache.hpp"

namespace vitreo {
class GuestMemory;
} // namespace vitreo

namespace vitreo::cli {

/// Decode the basic block starting at @p addr.
///
/// @param is_64bit Selects the decoder's operating mode. The same bytes decode
///        differently in 32- and 64-bit code, so this must match the guest
///        thread's actual mode rather than the process's nominal bitness.
/// @return The decoded block. May be empty when the first instruction is
///         unreadable or undecodable, which the caller must treat as "run this
///         under the interpreter" rather than as an error.
[[nodiscard]] cpu::jit::BasicBlock decode_basic_block(GuestMemory& memory,
                                                      GuestAddr addr,
                                                      bool is_64bit);

/// Re-decode a cached block whose decoded array was released by the retention
/// policy (`cpu::jit::release_block_decoded`).
///
/// This is the read side of the release: `BasicBlock` keeps the cheap scalars
/// (`instruction_count`, `byte_length`, `ends_with_branch`) but gives the
/// 144-bytes-per-instruction array back, and any reader that needs the array
/// again calls this first.
///
/// Rebuilding is legitimate because `decode_basic_block` is a pure function of
/// (address, mode, guest bytes) — the same property the decode memo already
/// relies on. What is NOT guaranteed is that the guest bytes are unchanged, so
/// the rebuilt block is adopted only when it reproduces the cached block's
/// shape exactly. A mismatch means the cache entry is stale (self-modified
/// code, a remapped page) and the caller must NOT execute it.
///
/// Does nothing and returns true when @p block already holds its array.
///
/// @return true when @p block holds a usable array on return; false when the
///         entry is stale and must be invalidated.
[[nodiscard]] bool rematerialize_block_instructions(
    GuestMemory& memory, cpu::jit::BasicBlock& block);

/// Drop the cached environment-derived decode flags so the next decode reads
/// the environment again.
///
/// `decode_basic_block` used to call `environment_flag_enabled` on every
/// decode purely so that same-process A/B tests could not inherit the first
/// test's environment. `getenv` takes a global lock and linear-scans `environ`,
/// which measured at 3.0% of running CPU in a live run, so the value is
/// now cached. Any code that mutates the decoder's environment variables
/// in-process must call this afterwards; nothing in a guest run does.
void refresh_decode_env_flags() noexcept;

/// Process-wide decoded-block memo counters. Diagnostic only.
/// VITREO_DECODE_MEMO_STATS=1 also prints them periodically and at exit.
struct DecodeMemoStats {
    u64 hits{0};             ///< Reused a memoized decode.
    u64 miss_absent{0};      ///< No entry for the address.
    u64 miss_key{0};         ///< Entry existed but for a different mode/flags.
    u64 miss_expired{0};     ///< Entry existed, its IR had been freed.
    u64 miss_hash{0};        ///< Entry existed, guest bytes had changed.
    u64 inserts{0};          ///< Decodes published into the memo.
    u64 strong_retained{0};  ///< Publishes that took a budgeted strong ref.
    u64 evictions{0};        ///< Strong entries dropped to stay in budget.
    u64 admit_rejects{0};    ///< Publishes that fell back to a weak ref.
    usize entries{0};        ///< Live map size.
    usize strong_entries{0}; ///< Live strong-referenced entries.
    usize retained_bytes{0}; ///< Bytes charged to the retained-IR budget.
};
[[nodiscard]] DecodeMemoStats decode_memo_stats() noexcept;

/// Drop every memoized decode and return its retained-IR budget. Tests only.
void reset_decode_memo() noexcept;

} // namespace vitreo::cli
