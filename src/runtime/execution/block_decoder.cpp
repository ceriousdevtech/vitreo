// SPDX-License-Identifier: LGPL-2.1-only

/// @file block_decoder.cpp
/// @brief Decode a guest basic block for the JIT. See block_decoder.hpp.
///
/// GuestMappedByteReader streams a mapped window so unique-code compile
/// storms do not re-resolve guest memory for every instruction.

#include "runtime/execution/block_decoder.hpp"

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/decode/guest_mapped_reader.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vitreo::cli {

namespace {

// ---------------------------------------------------------------------------
// Process-wide decoded-block memo.
//
// `BlockCache` is per guest thread, so without this every thread that reaches
// an address decodes it again and holds its own 144-bytes-per-instruction
// copy. The same library code runs on dozens of threads across
// several processes, which showed up as ~838k live allocations and ~350 MB of
// malloc in one process, plus decode outweighing compile 105:16 in the CPU
// profile.
//
// The entry originally held only a WEAK reference, which made the memo
// almost useless in practice. `publish_compiled` releases the IR of a
// FULLY-NATIVE block — the common, desirable case — so its weak reference
// expired as soon as the first thread compiled it, and every one of the other
// 39 threads that later reached the same address missed and re-decoded from
// scratch. Measured on a live run: `decode_basic_block` was
// 30.5% of the execution loop's inclusive samples, and translation outweighed
// execution 5:1.
//
// The entry now holds a BOUNDED STRONG reference as well, charged against a
// retained-IR budget of the same shape as the compile memo's
// (`cpu::jit::memo_ir_admit`), capped by VITREO_DECODE_MEMO_IR_MB. That is
// what protects against the ~350 MB / ~838k live allocations the weak
// reference was guarding: over budget, a CLOCK (second-chance) scan evicts
// cold strong entries, and a publish that still cannot be admitted falls back
// to the old weak-only behaviour.
//
// The counter is deliberately SEPARATE from the compile memo's. That one is
// zeroed outright by `JitCompileMemo::clear_compile_memo`, on the (correct, for
// it) grounds that clearing the index drops every record it accounts for. A
// shared counter would be zeroed out from under this memo's live entries, and
// their later releases would underflow an unsigned counter into a value that
// permanently refuses admission.
//
// Dropping a strong reference is always safe for lifetime purposes: it only
// releases the MEMO's claim. A delegating block whose generated code baked
// `&instructions[i]` holds its own strong reference through its cache entry
// (see `publish_compiled` / `compiled_from_cache`), so eviction here can never
// leave live code pointing at freed IR, and never produces the documented
// null-array hazard — the memo only ever hands out a complete array or misses.
//
// Correctness rests on `decode_basic_block` being a pure function of
// (addr, is_64bit, block bytes): the memo re-hashes the guest bytes over the
// remembered length on every hit, so self-modified code misses and re-decodes.
constexpr usize kNoClockSlot = ~usize{0};

struct DecodeMemoEntry {
    u64 code_hash{0};
    u32 byte_length{0};
    u32 instruction_count{0};
    bool ends_with_branch{false};
    bool is_64bit{false};
    /// `decode_flag_set(is_64bit)` at the time this entry was decoded, not the
    /// guarded-island bit alone. Every environment flag that can change what a
    /// decode produces belongs here: this memo is addressed by guest ADDRESS,
    /// so an entry decoded under one configuration is otherwise served to
    /// another. That was latent while the flags were process-constant
    /// statics; making them refreshable so tests can reach both arms is what
    /// exposed it.
    u32 decode_flags{0};
    /// Bytes charged to the retained-IR budget. 0 when weak-only.
    usize ir_bytes{0};
    /// Index into g_clock, or kNoClockSlot when weak-only.
    usize clock_slot{kNoClockSlot};
    /// CLOCK reference bit. Set by readers under the shared lock, so it must
    /// be atomic: reader/reader concurrency is real, writer/reader is not.
    std::atomic<bool> used{false};
    /// Budgeted cross-thread retention. Null when admission was refused.
    std::shared_ptr<const cpu::jit::DecodedInsns::Storage> strong;
    /// Always set. The only source of a hit for a weak-only entry.
    std::weak_ptr<const cpu::jit::DecodedInsns::Storage> insns;
};

std::shared_mutex g_decode_memo_mutex;
std::unordered_map<GuestAddr, DecodeMemoEntry> g_decode_memo;
/// CLOCK eviction ring over the STRONG entries only. Weak-only entries hold
/// no budget, so they are reclaimed by the expiry sweep instead.
std::vector<GuestAddr> g_clock;
usize g_clock_hand = 0;

/// Entries whose weak reference has expired cost a bucket each. Sweep them
/// when the map grows, rather than tracking liveness per insertion.
constexpr usize kDecodeMemoSweepAt = 1u << 17;
/// Size that triggers the next sweep; guarded by the unique lock. After a
/// sweep it moves to twice what survived. A fixed trigger re-swept the whole
/// map on EVERY insert once more than 2^17 entries were live, because a sweep
/// that frees nothing leaves the size above it: 262,144 live blocks took
/// ~95 s to decode instead of ~0.3 s.
usize g_decode_memo_sweep_at = kDecodeMemoSweepAt;

std::atomic<u64> g_memo_hits{0};
std::atomic<u64> g_memo_miss_absent{0};
std::atomic<u64> g_memo_miss_key{0};
std::atomic<u64> g_memo_miss_expired{0};
std::atomic<u64> g_memo_miss_hash{0};
std::atomic<u64> g_memo_inserts{0};
std::atomic<u64> g_memo_strong_retained{0};
std::atomic<u64> g_memo_evictions{0};
std::atomic<u64> g_memo_admit_rejects{0};

bool decode_memo_enabled() {
    static const bool enabled =
        std::getenv("VITREO_JIT_NO_DECODE_MEMO") == nullptr;
    return enabled;
}

/// VITREO_JIT_NO_DECODE_MEMO_STRONG=1 bisects the strong-retention half of the
/// memo on its own, leaving the weak-only behaviour that shipped before.
bool decode_memo_strong_enabled() {
    static const bool enabled =
        std::getenv("VITREO_JIT_NO_DECODE_MEMO_STRONG") == nullptr;
    return enabled;
}

bool decode_memo_stats_enabled() {
    static const bool on = std::getenv("VITREO_DECODE_MEMO_STATS") != nullptr;
    return on;
}

// ---- Retained-IR budget for the strong half of the memo ----
std::atomic<usize> g_memo_ir_bytes{0};

usize memo_ir_cap() noexcept {
    static const usize cap = [] () -> usize {
        const char* e = std::getenv("VITREO_DECODE_MEMO_IR_MB");
        usize mb = 64;
        if (e && *e) {
            char* end = nullptr;
            const unsigned long long v = std::strtoull(e, &end, 10);
            if (end != e) mb = static_cast<usize>(v);
        }
        return mb << 20;
    }();
    return cap;
}

/// Reserve @p bytes against the budget. All calls happen under the memo's
/// unique lock, so a plain load/store pair would do; the atomic is for the
/// lock-free `memo_ir_bytes()` readout.
bool memo_ir_admit(usize bytes) noexcept {
    const usize cur = g_memo_ir_bytes.load(std::memory_order_relaxed);
    if (cur + bytes > memo_ir_cap()) return false;
    g_memo_ir_bytes.store(cur + bytes, std::memory_order_relaxed);
    return true;
}

void memo_ir_release(usize bytes) noexcept {
    const usize cur = g_memo_ir_bytes.load(std::memory_order_relaxed);
    g_memo_ir_bytes.store(cur >= bytes ? cur - bytes : 0,
                          std::memory_order_relaxed);
}

usize memo_ir_bytes() noexcept {
    return g_memo_ir_bytes.load(std::memory_order_relaxed);
}

void decode_memo_stats_dump() noexcept;

// ---------------------------------------------------------------------------
// Cached environment-derived decode flags.
//
// `getenv` takes a global lock and linear-scans `environ`; `__findenv_locked`
// measured 3.0% of running CPU in a live run purely because this was
// read on every decode. Index 0 is the 32-bit default, index 1 the 64-bit one,
// because the flag's default differs by mode. -1 means "not yet read".
std::atomic<i8> g_guarded_islands[2] = {std::atomic<i8>{-1},
                                        std::atomic<i8>{-1}};

bool guarded_terminal_islands_flag(bool is_64bit) noexcept {
    std::atomic<i8>& slot = g_guarded_islands[is_64bit ? 1 : 0];
    i8 v = slot.load(std::memory_order_relaxed);
    if (v < 0) {
        v = environment_flag_enabled("VITREO_JIT_GUARDED_TERMINAL_TRACES",
                                     is_64bit)
                ? i8{1}
                : i8{0};
        slot.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

/// 32-bit traces: default ON since 2026-09-06.
///
/// They were opt-in against a recorded hazard -- "cold forward-Jcc exits can
/// carry the wrong continuation through long 32-bit transform walks, turning
/// finite map-load work into an unbounded CPU loop". That hazard was the
/// x64 trace bug, and the 2026-08-27 fix for it covered both bitnesses:
/// compute_omit_lazy_flags no longer models a forward Jcc whose target is
/// in-block as REJOINING the translation, and `flag_liveness.cpp` gates the
/// only remaining admission of that model on `block.is_64bit`. 32-bit has
/// been taking the conservative escaping rule ever since. Nobody re-measured
/// until now.
///
/// Worth it, and the reason is the block boundary, not the transition -- a
/// linked hop already bypasses the prologue (`CompiledBlock::link_entry`) and
/// costs ~28.7 ns, but the lazy-flag PUBLICATION at a block boundary is paid
/// in full. Differenced 60M-30M on tools/jit_bench/out32: cmp_jcc
/// 0.50 s -> 0.11 s (4.5x); alu, call_ret, flag_chain and submem_jcc all
/// neutral, nothing regresses. On a 32-bit guest the mechanism is visible
/// directly: blocks 91,156 -> 69,244, instructions per block 5.50 -> 8.54,
/// and blocks that publish lazy flags at their boundary 45,945 -> 13,099.
///
/// tools/stress moves <1.5% on every x86 row, because its kernels loop on
/// BACKWARD branches and traces only follow forward ones. VITREO_JIT_TRACES=0
/// to bisect. Full write-up: jit-refactor/TRACES32.md.
// Index 0 is the 32-bit flag, index 1 the 64-bit one, matching
// g_guarded_islands. Cached in a refreshable slot rather than a function-local
// static for one reason: a `static const bool` reads the environment exactly
// once per process, so no test can ever exercise both arms, and the only test
// this gate had asserted its DEFAULT rather than its behaviour.
std::atomic<i8> g_trace_blocks[2] = {std::atomic<i8>{-1}, std::atomic<i8>{-1}};

bool trace_blocks_flag(bool is_64bit) noexcept {
    std::atomic<i8>& slot = g_trace_blocks[is_64bit ? 1 : 0];
    i8 v = slot.load(std::memory_order_relaxed);
    if (v < 0) {
        v = environment_flag_enabled(
                is_64bit ? "VITREO_JIT_TRACES64" : "VITREO_JIT_TRACES", true)
                ? i8{1}
                : i8{0};
        slot.store(v, std::memory_order_relaxed);
    }
    return v != 0;
}

bool trace_blocks_32() noexcept { return trace_blocks_flag(false); }

/// x64 traces: default ON since 2026-08-27. See the long note at the use site.
bool trace_blocks_64() noexcept { return trace_blocks_flag(true); }

/// Everything OTHER than the guest bytes that changes what a decode produces.
///
/// Folded into the decode memo's entries, so a run configured differently can
/// never be served another configuration's IR. Anything added to the decoder
/// that varies its output must be added here too — that is the whole contract,
/// and it is why these three live in one function instead of three call sites.
u32 decode_flag_set(bool is_64bit) noexcept {
    u32 flags = 0;
    if (guarded_terminal_islands_flag(is_64bit)) flags |= 1u << 0;
    if (trace_blocks_32()) flags |= 1u << 1;
    if (trace_blocks_64()) flags |= 1u << 2;
    return flags;
}

/// Remove clock slot @p idx. Requires the unique lock.
void clock_remove_at(usize idx) {
    if (idx >= g_clock.size()) return;
    const GuestAddr moved = g_clock.back();
    g_clock[idx] = moved;
    g_clock.pop_back();
    if (idx < g_clock.size()) {
        const auto it = g_decode_memo.find(moved);
        if (it != g_decode_memo.end()) it->second.clock_slot = idx;
    }
    if (g_clock_hand >= g_clock.size()) g_clock_hand = 0;
}

/// Drop one entry, returning any budget it held. Requires the unique lock.
void memo_drop_locked(std::unordered_map<GuestAddr, DecodeMemoEntry>::iterator
                          it) {
    if (it->second.ir_bytes != 0) {
        memo_ir_release(it->second.ir_bytes);
    }
    const usize slot = it->second.clock_slot;
    g_decode_memo.erase(it);
    if (slot != kNoClockSlot) clock_remove_at(slot);
}

/// One CLOCK sweep step: give a recently used entry a second chance, evict the
/// first entry that has not been touched since the hand last passed it.
/// Requires the unique lock. Returns false when nothing is evictable.
bool memo_evict_one_locked() {
    for (usize tries = 0, limit = g_clock.size() * 2 + 1;
         tries < limit && !g_clock.empty(); ++tries) {
        if (g_clock_hand >= g_clock.size()) g_clock_hand = 0;
        const GuestAddr addr = g_clock[g_clock_hand];
        const auto it = g_decode_memo.find(addr);
        if (it == g_decode_memo.end()) {
            // Stale slot; the entry went away without its slot.
            clock_remove_at(g_clock_hand);
            continue;
        }
        if (it->second.used.load(std::memory_order_relaxed)) {
            it->second.used.store(false, std::memory_order_relaxed);
            ++g_clock_hand;
            continue;
        }
        memo_drop_locked(it);
        g_memo_evictions.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void decode_memo_stats_dump() noexcept {
    usize entries = 0;
    usize strong_entries = 0;
    {
        std::shared_lock<std::shared_mutex> rlock(g_decode_memo_mutex);
        entries = g_decode_memo.size();
        strong_entries = g_clock.size();
    }
    const u64 hits = g_memo_hits.load(std::memory_order_relaxed);
    const u64 absent = g_memo_miss_absent.load(std::memory_order_relaxed);
    const u64 key = g_memo_miss_key.load(std::memory_order_relaxed);
    const u64 expired = g_memo_miss_expired.load(std::memory_order_relaxed);
    const u64 hash = g_memo_miss_hash.load(std::memory_order_relaxed);
    const u64 misses = absent + key + expired + hash;
    const u64 total = hits + misses;
    std::fprintf(stderr,
        "[decode-memo pid=%d] decodes=%llu hit=%llu (%.1f%%) miss: absent=%llu "
        "key=%llu expired=%llu hash=%llu | inserted=%llu strong=%llu "
        "evicted=%llu over_budget=%llu | entries=%zu strong_entries=%zu "
        "retained IR=%.1f MB\n",
        static_cast<int>(::getpid()),
        static_cast<unsigned long long>(total),
        static_cast<unsigned long long>(hits),
        total ? 100.0 * static_cast<double>(hits) /
                    static_cast<double>(total)
              : 0.0,
        static_cast<unsigned long long>(absent),
        static_cast<unsigned long long>(key),
        static_cast<unsigned long long>(expired),
        static_cast<unsigned long long>(hash),
        static_cast<unsigned long long>(
            g_memo_inserts.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_strong_retained.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_evictions.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            g_memo_admit_rejects.load(std::memory_order_relaxed)),
        entries, strong_entries,
        static_cast<double>(memo_ir_bytes()) / (1024.0 * 1024.0));
}

void decode_memo_stats_arm_atexit() noexcept {
    static std::once_flag once;
    std::call_once(once, [] { std::atexit(decode_memo_stats_dump); });
}

/// FNV-1a over @p len guest bytes at @p addr. Matches the block-byte hash the
/// compile memo uses. Returns false when the range is not fully readable.
bool hash_block_bytes(GuestMemory& memory, GuestAddr addr, u32 len,
                      u64& out) {
    constexpr u64 kFnvOffset = 14695981039346656037ull;
    constexpr u64 kFnvPrime = 1099511628211ull;
    auto fnv1a = [](u64 hash, const Byte* bytes, usize n) noexcept {
        for (usize i = 0; i < n; ++i) {
            hash ^= static_cast<u8>(bytes[i]);
            hash *= kFnvPrime;
        }
        return hash;
    };
    if (len == 0) {
        return false;
    }
    // Instruction bytes: fetch access, so an execute-only (PAGE_EXECUTE)
    // block hashes like any other.
    if (const Byte* mapped = memory.fetch_ptr(addr, len)) {
        out = fnv1a(kFnvOffset, mapped, len);
        return true;
    }
    // A block crossing two mappings: hash byte by byte.
    u64 hash = kFnvOffset;
    for (u32 i = 0; i < len; ++i) {
        auto byte = memory.fetch_u8(addr + i);
        if (byte.is_err()) {
            return false;
        }
        const Byte b = byte.value();
        hash = fnv1a(hash, &b, 1);
    }
    out = hash;
    return true;
}

} // namespace

cpu::jit::BasicBlock decode_basic_block(GuestMemory& memory, GuestAddr addr,
                                         bool is_64bit) {
    cpu::jit::BasicBlock block;
    block.start_addr = addr;
    block.is_64bit = is_64bit;

    const bool memo_on = decode_memo_enabled();
    // x64 trace extension: keep a terminal instruction in the cold
    // fall-through arm when the immediately preceding forward Jcc skips it
    // exactly. The JIT can then branch directly to the instruction after the
    // island while preserving the terminal instruction for the untaken path.
    // Cached: reading this with getenv on every decode cost 3.0% of running
    // CPU in a live run. Same-process A/B tests call
    // refresh_decode_env_flags() after mutating the environment.
    const bool guarded_terminal_islands =
        guarded_terminal_islands_flag(is_64bit);
    const u32 decode_flags = decode_flag_set(is_64bit);
    if (memo_on) {
        if (decode_memo_stats_enabled()) decode_memo_stats_arm_atexit();
        u64 cand_hash = 0;
        u32 cand_byte_length = 0;
        u32 cand_instruction_count = 0;
        bool cand_ends_with_branch = false;
        std::shared_ptr<const cpu::jit::DecodedInsns::Storage> insns;
        enum class Probe { Absent, KeyMismatch, Expired, Live } probe =
            Probe::Absent;
        {
            std::shared_lock<std::shared_mutex> rlock(g_decode_memo_mutex);
            const auto it = g_decode_memo.find(addr);
            if (it == g_decode_memo.end()) {
                probe = Probe::Absent;
            } else if (it->second.is_64bit != is_64bit ||
                       it->second.decode_flags != decode_flags) {
                probe = Probe::KeyMismatch;
            } else {
                // Take the array before hashing: an entry whose IR is gone is
                // a miss no matter what the bytes say.
                insns = it->second.strong ? it->second.strong
                                          : it->second.insns.lock();
                if (insns) {
                    cand_hash = it->second.code_hash;
                    cand_byte_length = it->second.byte_length;
                    cand_instruction_count = it->second.instruction_count;
                    cand_ends_with_branch = it->second.ends_with_branch;
                    // CLOCK reference bit. Cheap enough to set under the
                    // shared lock; a lost update only costs one extra pass.
                    it->second.used.store(true, std::memory_order_relaxed);
                    probe = Probe::Live;
                } else {
                    probe = Probe::Expired;
                }
            }
        }
        switch (probe) {
        case Probe::Absent:
            g_memo_miss_absent.fetch_add(1, std::memory_order_relaxed);
            break;
        case Probe::KeyMismatch:
            g_memo_miss_key.fetch_add(1, std::memory_order_relaxed);
            break;
        case Probe::Expired:
            g_memo_miss_expired.fetch_add(1, std::memory_order_relaxed);
            break;
        case Probe::Live: {
            u64 now_hash = 0;
            if (hash_block_bytes(memory, addr, cand_byte_length, now_hash) &&
                now_hash == cand_hash) {
                block.instructions = cpu::jit::DecodedInsns{std::move(insns)};
                block.byte_length = cand_byte_length;
                block.instruction_count = cand_instruction_count;
                block.ends_with_branch = cand_ends_with_branch;
                g_memo_hits.fetch_add(1, std::memory_order_relaxed);
                return block;
            }
            g_memo_miss_hash.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        }
    }

    // Long-mode traces amortize prologue/exit/link overhead across branch-
    // dense straight-line regions. Keep the probe bounded, but large enough
    // for common 16/32-way unrolled loops to reach their counted backedge in
    // one translation instead of being split into four linked blocks.
    constexpr u32 MAX_BLOCK_INSNS = 192;
    // How many forward Jccs one trace may absorb. Tunable so a miscompiled
    // trace can be bisected by depth: the first value that stops reproducing
    // names the Jcc that breaks it.
    static const u32 MAX_TRACE_JCCS = []() -> u32 {
        const char* e = std::getenv("VITREO_JIT_MAX_TRACE_JCCS");
        return e != nullptr ? static_cast<u32>(std::strtoul(e, nullptr, 0)) : 32u;
    }();
    const bool trace_blocks = trace_blocks_32();
    // DEFAULT OFF, and the reason is measured, not inherited. x64 traces are
    // a large, real win on the bench kernels -- cmp_jcc 14.1 -> 6.7 ns
    // (3.8x -> 1.8x the host runner), cmp_mov_jcc 6.1x -> 4.1x, CMP 11.00 -> 4.84
    // emitted ARM/x86, blocks 133 -> 100 -- because an x64 block otherwise
    // ENDS at its first conditional branch, so `cmp; jcc` pays a 12-word
    // prologue, a full lazy-flag publication and an epilogue per TWO guest
    // instructions.
    //
    // And they break a real application. A multi-process launcher dies
    // 16,368 instructions in with a null dereference at RIP=0x14009ea3b,
    // reproducibly, under both memory models; the same binary with this flag
    // off reaches 17 processes and paints windows. Bisected away from the 64-bit lazy-flag deferral
    // (VITREO_JIT_NO_DEFER64=1 does not change it), so the fault is in trace
    // formation itself -- the same "cold forward-Jcc exits can carry the
    // wrong continuation" hazard recorded above for 32-bit, now confirmed to
    // apply to x64.
    //
    // FIXED 2026-08-27 and now DEFAULT ON. The fault was not in trace
    // formation itself but in a disagreement it exposed: compute_omit_lazy_flags
    // modelled a forward Jcc whose target is in-block as REJOINING the
    // translation, so a later in-block full-flag writer was taken to dominate
    // both edges and the publisher's lazy record was omitted. control.cpp
    // compiles every such Jcc as queue_mid_block_jcc_exit — the block EXITS
    // there — so the taken edge left with AF/PF live only in the record that
    // had just been dropped, and the successor could evaluate a condition off
    // stale flags. Before traces a Jcc's target was never in-block, so the
    // path never ran; the same mismatch is the recorded 32-bit trace hazard.
    // Guarded by JitParity64Test.TracedForwardJccIntoBlockKeepsPublisherFlags,
    // which is verified to FAIL against the restored bug.
    //
    // Worth it: cmp_jcc 14.1 -> 6.8 ns (3.7x -> 1.7x the host runner), cmp_mov_jcc
    // 6.5x -> 4.9x, many_blocks 2.4x -> 2.1x, CMP 11.00 -> 4.84 emitted
    // ARM/x86; the same launcher reaches the same 17 processes and windows
    // as with traces off. VITREO_JIT_TRACES64=0 to bisect.
    const bool trace_blocks64 = trace_blocks_64();
    auto mode = is_64bit ? cpu::DecodeMode::Mode64 : cpu::DecodeMode::Mode32;
    u64 rip = addr;
    u32 trace_jccs = 0;
    bool expect_guarded_terminal = false;
    u64 guarded_terminal_target = 0;
    // DecodedInstruction is deliberately rich (four full operands), so
    // reserving MAX_BLOCK_INSNS here makes every cached block own a 10 KiB
    // allocation even though ordinary basic blocks are only a few
    // instructions long. Large guests retain hundreds of thousands of blocks
    // across their worker-thread caches. Decode into bounded temporary storage
    // and retain only the instructions the block actually contains.
    std::array<cpu::DecodedInstruction, MAX_BLOCK_INSNS> decoded{};
    cpu::GuestMappedByteReader reader(memory, addr);

    for (u32 i = 0; i < MAX_BLOCK_INSNS; ++i) {
        cpu::DecodedInstruction instr{};
        auto err = cpu::decode_instruction(reader, mode, instr);
        if (err != cpu::DecodeError::Success) {
            break;
        }

        decoded[block.instruction_count] = instr;
        block.instruction_count++;
        rip += instr.length;

        const bool guarded_ud2 = guarded_terminal_islands &&
            expect_guarded_terminal && instr.opcode == cpu::Opcode::UD2 &&
            rip == guarded_terminal_target;
        // The island must be the instruction immediately after the Jcc.
        expect_guarded_terminal = false;

        // End block on control-flow instructions
        bool is_branch = false;
        bool is_conditional_branch = false;
        switch (instr.opcode) {
        case cpu::Opcode::JMP_REL8:   case cpu::Opcode::JMP_REL32:
        case cpu::Opcode::JMP_RM32:   case cpu::Opcode::JMP_RM64:
        case cpu::Opcode::JMP_M16_16: case cpu::Opcode::JMP_M16_32:
        case cpu::Opcode::JMP_M16_64:
        case cpu::Opcode::CALL_REL32: case cpu::Opcode::CALL_RM32:
        case cpu::Opcode::CALL_RM64:
        case cpu::Opcode::CALL_M16_16: case cpu::Opcode::CALL_M16_32:
        case cpu::Opcode::CALL_M16_64:
        case cpu::Opcode::RET_NEAR:   case cpu::Opcode::RET_NEAR_IMM16:
        case cpu::Opcode::RET_FAR:    case cpu::Opcode::RET_FAR_IMM16:
        case cpu::Opcode::IRETQ:
        case cpu::Opcode::INT_3:      case cpu::Opcode::UD2:
            is_branch = true;
            break;
        case cpu::Opcode::JZ_REL8:    case cpu::Opcode::JZ_REL32:
        case cpu::Opcode::JNZ_REL8:   case cpu::Opcode::JNZ_REL32:
        case cpu::Opcode::JB_REL8:    case cpu::Opcode::JB_REL32:
        case cpu::Opcode::JNB_REL8:   case cpu::Opcode::JNB_REL32:
        case cpu::Opcode::JBE_REL8:   case cpu::Opcode::JBE_REL32:
        case cpu::Opcode::JNBE_REL8:  case cpu::Opcode::JNBE_REL32:
        case cpu::Opcode::JL_REL8:    case cpu::Opcode::JL_REL32:
        case cpu::Opcode::JNL_REL8:   case cpu::Opcode::JNL_REL32:
        case cpu::Opcode::JLE_REL8:   case cpu::Opcode::JLE_REL32:
        case cpu::Opcode::JNLE_REL8:  case cpu::Opcode::JNLE_REL32:
        case cpu::Opcode::JS_REL8:    case cpu::Opcode::JS_REL32:
        case cpu::Opcode::JNS_REL8:   case cpu::Opcode::JNS_REL32:
        case cpu::Opcode::JO_REL8:    case cpu::Opcode::JO_REL32:
        case cpu::Opcode::JNO_REL8:   case cpu::Opcode::JNO_REL32:
        case cpu::Opcode::JP_REL8:    case cpu::Opcode::JP_REL32:
        case cpu::Opcode::JNP_REL8:   case cpu::Opcode::JNP_REL32:
        case cpu::Opcode::LOOP_REL8:
        case cpu::Opcode::JECXZ_REL8: case cpu::Opcode::JRCXZ_REL8:
            is_conditional_branch = true;
            is_branch = true;
            break;
        case cpu::Opcode::INT_IMM8:
        case cpu::Opcode::SYSCALL:
        case cpu::Opcode::SYSENTER:
        case cpu::Opcode::SYSEXIT:
            is_branch = true;
            break;
        default:
            break;
        }

        if (is_branch) {
            block.ends_with_branch = true;
            if (guarded_ud2) {
                // Retain UD2 in the IR. The forward Jcc compiles to an
                // in-block branch over its cold delegate, while an untaken
                // edge still executes UD2 and raises #UD through the existing
                // interpreter path. Standalone and non-exactly-skipped UD2s
                // continue to terminate blocks below.
                continue;
            }
            // 64-bit traces are gated separately from the 32-bit ones: the
            // recorded hazard (cold forward-Jcc exits carrying the wrong
            // continuation through long 32-bit transform walks) was measured
            // on 32-bit map-load code, and x64 blocks pay far more for
            // stopping here — a 2-instruction `cmp; jcc` block spends a
            // 12-word prologue, a full lazy-flag publication and an epilogue
            // per two guest instructions. VITREO_JIT_TRACES64=1 to measure.
            const bool traces_here = is_64bit ? trace_blocks64 : trace_blocks;
            if (traces_here && is_conditional_branch &&
                trace_jccs < MAX_TRACE_JCCS && instr.operand_count != 0) {
                const u64 target = rip + static_cast<u64>(
                    static_cast<i64>(instr.operands[0].imm));
                if (target > rip) {
                    ++trace_jccs;
                    if (guarded_terminal_islands) {
                        expect_guarded_terminal = true;
                        guarded_terminal_target = target;
                    }
                    continue;
                }
            }
            break;
        }
    }

    block.instructions.assign(
        decoded.begin(), decoded.begin() + block.instruction_count);
    block.byte_length = static_cast<u32>(rip - addr);

    if (memo_on && block.byte_length != 0) {
        u64 code_hash = 0;
        auto shared = block.instructions.share();
        if (shared && hash_block_bytes(memory, addr, block.byte_length,
                                       code_hash)) {
            const usize ir_bytes =
                shared->size() * sizeof(cpu::DecodedInstruction);
            std::unique_lock<std::shared_mutex> wlock(g_decode_memo_mutex);
            const bool legacy_sweep = cpu::jit::reentry_legacy();
            if (g_decode_memo.size() >= (legacy_sweep ? kDecodeMemoSweepAt
                                                      : g_decode_memo_sweep_at)) {
                // Weak-only entries are the ones that can rot. A strong entry
                // holds budget, so it must go out through memo_drop_locked.
                std::erase_if(g_decode_memo, [](const auto& kv) {
                    return kv.second.ir_bytes == 0 && kv.second.insns.expired();
                });
                g_decode_memo_sweep_at =
                    std::max(kDecodeMemoSweepAt, g_decode_memo.size() * 2);
            }
            // Overwrite: a differing hash at the same address means the code
            // changed, and the newest decode is the live one. Drop through the
            // budget-aware path so the old array's bytes are returned.
            if (const auto old = g_decode_memo.find(addr);
                old != g_decode_memo.end()) {
                memo_drop_locked(old);
            }

            // Retain a strong reference within the process-wide budget, so a
            // fully-native block (whose cache entry releases its IR in
            // publish_compiled) stays memoized for the other guest threads.
            bool strong = decode_memo_strong_enabled() && ir_bytes != 0;
            if (strong) {
                while (!memo_ir_admit(ir_bytes)) {
                    if (!memo_evict_one_locked()) {
                        strong = false;
                        g_memo_admit_rejects.fetch_add(
                            1, std::memory_order_relaxed);
                        break;
                    }
                }
            }

            DecodeMemoEntry& entry = g_decode_memo[addr];
            entry.code_hash = code_hash;
            entry.byte_length = block.byte_length;
            entry.instruction_count = block.instruction_count;
            entry.ends_with_branch = block.ends_with_branch;
            entry.is_64bit = is_64bit;
            entry.decode_flags = decode_flags;
            entry.used.store(true, std::memory_order_relaxed);
            entry.insns = shared;
            if (strong) {
                entry.ir_bytes = ir_bytes;
                entry.strong = std::move(shared);
                entry.clock_slot = g_clock.size();
                g_clock.push_back(addr);
                g_memo_strong_retained.fetch_add(1, std::memory_order_relaxed);
            }
            const u64 n =
                g_memo_inserts.fetch_add(1, std::memory_order_relaxed) + 1;
            wlock.unlock();
            // Periodic, not only atexit: a guest process that is SIGKILLed
            // (or never exits) still leaves its last line in the log.
            if (decode_memo_stats_enabled() && (n % 20000u) == 0u) {
                decode_memo_stats_dump();
            }
        }
    }
    return block;
}

bool rematerialize_block_instructions(GuestMemory& memory,
                                      cpu::jit::BasicBlock& block) {
    if (block.instructions.size() == block.instruction_count &&
        block.instruction_count != 0) {
        return true;
    }
    if (block.instruction_count == 0) return false;

    // Re-entrancy: decode_basic_block only reads guest memory and the decode
    // memo. It never enters guest code, never calls the JIT, and never
    // touches a BlockCache, so this cannot recurse into compilation or into
    // this function. The lock it takes (g_decode_memo_mutex) is a leaf lock,
    // held only for the duration of one map operation and never held by any
    // caller of this function.
    cpu::jit::BasicBlock fresh =
        decode_basic_block(memory, block.start_addr, block.is_64bit);

    // Adopt only on an exact shape match. Anything else means the bytes behind
    // this cache entry changed, and its compiled code (and any block that
    // linked to it) describes code that no longer exists.
    const bool ok = fresh.instruction_count == block.instruction_count &&
                    fresh.byte_length == block.byte_length &&
                    fresh.ends_with_branch == block.ends_with_branch &&
                    fresh.instructions.size() == block.instruction_count;
    cpu::jit::note_decoded_redecode(ok);
    if (!ok) return false;

    block.instructions = std::move(fresh.instructions);
    block.instructions_released = false;
    return true;
}

void refresh_decode_env_flags() noexcept {
    g_guarded_islands[0].store(-1, std::memory_order_relaxed);
    g_guarded_islands[1].store(-1, std::memory_order_relaxed);
    g_trace_blocks[0].store(-1, std::memory_order_relaxed);
    g_trace_blocks[1].store(-1, std::memory_order_relaxed);
}

DecodeMemoStats decode_memo_stats() noexcept {
    DecodeMemoStats s;
    s.hits = g_memo_hits.load(std::memory_order_relaxed);
    s.miss_absent = g_memo_miss_absent.load(std::memory_order_relaxed);
    s.miss_key = g_memo_miss_key.load(std::memory_order_relaxed);
    s.miss_expired = g_memo_miss_expired.load(std::memory_order_relaxed);
    s.miss_hash = g_memo_miss_hash.load(std::memory_order_relaxed);
    s.inserts = g_memo_inserts.load(std::memory_order_relaxed);
    s.strong_retained = g_memo_strong_retained.load(std::memory_order_relaxed);
    s.evictions = g_memo_evictions.load(std::memory_order_relaxed);
    s.admit_rejects = g_memo_admit_rejects.load(std::memory_order_relaxed);
    std::shared_lock<std::shared_mutex> rlock(g_decode_memo_mutex);
    s.entries = g_decode_memo.size();
    s.strong_entries = g_clock.size();
    s.retained_bytes = memo_ir_bytes();
    return s;
}

void reset_decode_memo() noexcept {
    std::unique_lock<std::shared_mutex> wlock(g_decode_memo_mutex);
    for (auto& kv : g_decode_memo) {
        if (kv.second.ir_bytes != 0) {
            memo_ir_release(kv.second.ir_bytes);
        }
    }
    g_decode_memo.clear();
    g_clock.clear();
    g_clock_hand = 0;
    g_decode_memo_sweep_at = kDecodeMemoSweepAt;
    g_memo_hits.store(0, std::memory_order_relaxed);
    g_memo_miss_absent.store(0, std::memory_order_relaxed);
    g_memo_miss_key.store(0, std::memory_order_relaxed);
    g_memo_miss_expired.store(0, std::memory_order_relaxed);
    g_memo_miss_hash.store(0, std::memory_order_relaxed);
    g_memo_inserts.store(0, std::memory_order_relaxed);
    g_memo_strong_retained.store(0, std::memory_order_relaxed);
    g_memo_evictions.store(0, std::memory_order_relaxed);
    g_memo_admit_rejects.store(0, std::memory_order_relaxed);
}


}  // namespace vitreo::cli
