// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file translation_store_stats.hpp
/// @brief Translation-cost measurement for the persistent translation store
///        (VITREO_TRANSLATION_STORE=measure|warm).
///
/// Answers "how much translator time does this process spend, when, and how
/// much of it did the store replace": wall time inside JitCompiler::compile
/// (decode of a dispatcher block miss is counted separately), split by how
/// each compile was served, sampled every 250 ms into
/// $VITREO_LOG_DIR/tstore_<pid>.csv with wall-clock stamps and load-phase
/// marks (first present ...) so an analyzer can cut any window of the run.
/// tools/perf/rv/tstore_report.py reads it.
///
/// Arch-neutral (no emitter): the runtime's decode path reports into it on
/// both backends. Everything is a no-op while the mode is Off.
#pragma once

#include <vitreo/types.hpp>

namespace vitreo::cpu::jit {

enum class TranslationStoreMode : u8 {
    Off = 0,
    /// Count translation cost; no store.
    Measure,
    /// Count, adopt stored translations of image code, store fresh ones.
    Warm,
};

/// VITREO_TRANSLATION_STORE: unset/empty/"warm" -> Warm, "off"/"0" -> Off,
/// "measure".
/// Any other value is reported once and treated as Off. Read on first use,
/// never at static initialization.
[[nodiscard]] TranslationStoreMode translation_store_mode() noexcept;
/// TEST-ONLY: force a mode; nullptr-like reset with force=false.
void set_translation_store_mode_for_test(bool force, TranslationStoreMode mode) noexcept;

/// How one compile() was served.
enum class TranslationOutcome : u8 {
    None = 0,      ///< declined / deferred: no code
    MemoHit,       ///< the in-process memo had it
    StoreHit,      ///< adopted from the persistent store
    StoreVerified, ///< store hit, compiled fresh for shadow verification
    Fresh,         ///< emitted
};

namespace tstats {

[[nodiscard]] inline bool measuring() noexcept {
    return translation_store_mode() != TranslationStoreMode::Off;
}
/// Monotonic ticks (mach_absolute_time) and their conversion.
[[nodiscard]] u64 now_ticks() noexcept;
[[nodiscard]] u64 ticks_to_ns(u64 ticks) noexcept;

void note_decode(u64 ns) noexcept;
void note_compile(TranslationOutcome outcome, u64 ns) noexcept;

/// Store events (counted only; the store owns their meaning).
enum class Event : u8 {
    Lookup,
    Hit,
    MissNoRecord,
    MissIndexNotReady,
    RejectMac,
    RejectBytes,
    RejectDecode,
    RejectShape,
    RejectReloc,
    VerifyMatch,
    VerifyMismatch,
    Produced,
    ProducedBytes,   ///< add `n` bytes
    SkipNotImage,    ///< not clean image code (or modified pages)
    SkipSeen,        ///< address already translated in this process
    SkipDelegate,
    SkipPinless,
    SkipUnrelocatable,
    SkipUnrecordedHostAddr,
    SkipFull,
    SkipDiagnostics,
    ProducedRelocs,  ///< add the host-address relocations of a stored block
    RejectStub,      ///< a process stub the record needs cannot be published here
    SkipDuplicate,   ///< a fresh block the store already holds: not written again
    Count,
};
void note(Event event, u64 n = 1) noexcept;
[[nodiscard]] u64 count(Event event) noexcept;

struct Totals {
    u64 compiles[5]{};      ///< by TranslationOutcome
    u64 compile_ns[5]{};
    u64 decodes{0};
    u64 decode_ns{0};
};
[[nodiscard]] Totals totals() noexcept;

/// One line `[tstore] ...` with every counter, to stderr.
void report(const char* why) noexcept;
/// Flush the CSV now (tests, exit).
void flush() noexcept;
/// TEST-ONLY: zero every counter.
void reset_for_test() noexcept;

/// The store's end-of-process work (write buffered records), registered when
/// it activates.
using ShutdownHook = void (*)();
void set_shutdown_hook(ShutdownHook hook) noexcept;
/// End of process: run the shutdown hook, then write the final CSV line and
/// the exit report. Idempotent. The runtime ends with _exit, so atexit never
/// runs; every runtime exit path calls this instead.
void shutdown() noexcept;

}  // namespace tstats
}  // namespace vitreo::cpu::jit
