// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_coverage.cpp
/// @brief JIT native-lowering coverage telemetry. See jit_coverage.hpp.

#include "jit_coverage.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace vitreo::cpu::jit::coverage {

namespace {

// Opcode is a u16 enum with ~999 values today. Sized with headroom; anything
// beyond lands in the overflow bucket rather than corrupting adjacent counters.
constexpr usize kOpcodeSlots = 2048;

// Index 0 = 32-bit guest blocks, index 1 = 64-bit. Kept as two separate tables
// rather than one pooled table because the 32-bit and 64-bit lowerings are
// written in different files by different work streams.
constexpr usize kBitnessSlots = 2;

// Capacity of the form-keyed table. The full ISA matrix is ~1300 opcodes x a
// few dozen reachable forms x 2 bitnesses; 64Ki slots keeps the load factor
// low enough that linear probing stays short.
constexpr usize kFormSlots = 1u << 16;

constexpr u64 make_form_key(Opcode op, FormKey form, bool is_64bit) noexcept {
    return ((static_cast<u64>(op) << 17) |
            (static_cast<u64>(form.bits) << 1) |
            (is_64bit ? 1ull : 0ull)) + 1ull;
}

constexpr usize form_slot_hash(u64 key) noexcept {
    // splitmix64 finalizer: the low bits of the key are bitness and form
    // flags, which cluster hard under a plain mask.
    u64 z = key;
    z ^= z >> 30; z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27; z *= 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<usize>(z) & (kFormSlots - 1);
}

struct Tables {
    // Static: instructions emitted at compile time.
    std::array<std::array<std::atomic<u64>, kOpcodeSlots>, kBitnessSlots>
        compiled_native{};
    std::array<std::array<std::atomic<u64>, kOpcodeSlots>, kBitnessSlots>
        compiled_delegate{};

    // Dynamic: instructions actually executed.
    std::array<std::array<std::atomic<u64>, kOpcodeSlots>, kBitnessSlots>
        exec_delegate{};
    std::array<std::atomic<u64>, kBitnessSlots> exec_native{};

    std::array<std::atomic<u64>, kBitnessSlots> overflow{};

    // Form-keyed counts. Open-addressed, lock-free, fixed capacity: the hot
    // path is a compile-time event (not per executed instruction), but it
    // still runs on every JIT compile, so it must not take a lock or allocate.
    //
    // The key is (opcode << 17) | (form << 1) | bitness, stored +1 so that 0
    // means empty. A full table drops records into `form_overflow` rather than
    // evicting: a silently lossy scoreboard is worse than a loud one.
    struct FormSlot {
        std::atomic<u64> key{0};
        std::array<std::atomic<u64>, 3> n{};
    };
    std::array<FormSlot, kFormSlots> forms{};
    std::atomic<u64> form_overflow{0};
};

Tables& tables() noexcept {
    // Function-local static: no global-constructor ordering hazard against the
    // JIT compiler's own statics, and zero cost when the feature is off because
    // every caller checks enabled() first.
    static Tables t;
    return t;
}

const char* bitness_label(usize i) noexcept { return i == 0 ? "x86" : "x64"; }

// One ranked table of delegated opcodes for a single bitness.
void dump_bitness(usize b, u64 grand_total_exec) {
    auto& t = tables();

    const u64 native_exec = t.exec_native[b].load(std::memory_order_relaxed);
    u64 delegate_exec = 0;
    u64 compiled_native_total = 0;
    u64 compiled_delegate_total = 0;
    usize distinct_native = 0;
    usize distinct_delegate = 0;

    std::vector<std::pair<usize, u64>> ranked;
    ranked.reserve(128);

    for (usize i = 0; i < kOpcodeSlots; ++i) {
        const u64 cn = t.compiled_native[b][i].load(std::memory_order_relaxed);
        const u64 cd = t.compiled_delegate[b][i].load(std::memory_order_relaxed);
        const u64 xd = t.exec_delegate[b][i].load(std::memory_order_relaxed);

        compiled_native_total += cn;
        compiled_delegate_total += cd;
        delegate_exec += xd;
        if (cn != 0) ++distinct_native;
        if (cd != 0) ++distinct_delegate;
        if (xd != 0 || cd != 0) ranked.emplace_back(i, xd);
    }

    const u64 total_exec = native_exec + delegate_exec;
    if (total_exec == 0 && compiled_native_total == 0
        && compiled_delegate_total == 0) {
        return;  // This bitness never ran.
    }

    const auto pct = [](u64 n, u64 d) {
        return d == 0 ? 0.0
                      : 100.0 * static_cast<double>(n) / static_cast<double>(d);
    };

    std::fprintf(stderr,
        "[jit-coverage] === %s ===\n"
        "[jit-coverage]   emitted:  %llu native + %llu delegated instructions"
        " (%.1f%% native)\n"
        "[jit-coverage]   opcodes:  %zu natively lowered, %zu delegated\n"
        "[jit-coverage]   executed: %llu native + %llu delegated"
        " (%.2f%% delegated)\n",
        bitness_label(b),
        static_cast<unsigned long long>(compiled_native_total),
        static_cast<unsigned long long>(compiled_delegate_total),
        pct(compiled_native_total,
            compiled_native_total + compiled_delegate_total),
        distinct_native, distinct_delegate,
        static_cast<unsigned long long>(native_exec),
        static_cast<unsigned long long>(delegate_exec),
        pct(delegate_exec, total_exec));

    if (grand_total_exec != 0) {
        std::fprintf(stderr,
            "[jit-coverage]   share of all executed instructions: %.1f%%\n",
            pct(total_exec, grand_total_exec));
    }

    if (ranked.empty()) return;

    // Rank by dynamic weight — a rarely-emitted opcode inside a hot loop
    // matters far more than a widely-emitted one that never runs. Ties break on
    // static count so cold-but-common opcodes still surface in a run where the
    // dynamic counters saw nothing.
    std::sort(ranked.begin(), ranked.end(),
              [&](const auto& x, const auto& y) {
                  if (x.second != y.second) return x.second > y.second;
                  return t.compiled_delegate[b][x.first].load(
                             std::memory_order_relaxed)
                       > t.compiled_delegate[b][y.first].load(
                             std::memory_order_relaxed);
              });

    std::fprintf(stderr,
        "[jit-coverage]   top delegated opcodes (%s), by executions:\n",
        bitness_label(b));
    const usize shown = std::min<usize>(20, ranked.size());
    for (usize i = 0; i < shown; ++i) {
        const usize op = ranked[i].first;
        const u64 xd = ranked[i].second;
        const u64 cd =
            t.compiled_delegate[b][op].load(std::memory_order_relaxed);
        // opcode_name() has no entry for every Opcode value. Always print the
        // numeric id too: a bare "?" is not something a work stream can act on,
        // but "op=417" can be looked up in decode/opcodes.hpp.
        const char* name = opcode_name(static_cast<Opcode>(op));
        std::fprintf(stderr,
            "[jit-coverage]     op=%-4zu %-24s exec=%-12llu (%.2f%% of all %s)"
            "  emitted=%llu\n",
            op, name ? name : "(unnamed)",
            static_cast<unsigned long long>(xd),
            pct(xd, total_exec), bitness_label(b),
            static_cast<unsigned long long>(cd));
    }

    const u64 ovf = t.overflow[b].load(std::memory_order_relaxed);
    if (ovf != 0) {
        std::fprintf(stderr,
            "[jit-coverage]   WARNING: %llu records exceeded the opcode table"
            " (raise kOpcodeSlots)\n",
            static_cast<unsigned long long>(ovf));
    }
}

}  // namespace

namespace {

/// Dumps from a static destructor so the report survives every exit path. The
/// CLI re-execs itself for elevation and for guest child processes, so a dump
/// placed only on the normal return path never runs.
///
/// The constructor touches tables() so the counter storage is constructed
/// *before* this reporter and therefore destroyed *after* it. Without that
/// ordering the destructor would read freed memory during static teardown.
struct Reporter {
    Reporter() { (void)tables(); }
    ~Reporter() { dump(); }
};

}  // namespace

namespace detail {

// Definition for the header's inline gate.
std::atomic<int> g_coverage_enabled{-1};

// Function-local statics use a TLS guard on Apple platforms; that showed up as
// _tlv_get_addr inside every JitCompiler::execute. Caching in a process-wide
// atomic made the check one relaxed load, but the *call* remained — an
// out-of-line cross-TU call per dispatched block. enabled() now lives in the
// header and inlines to that one load; only this cold one-time path is left
// here, and it must not be inlined back into it.
[[gnu::noinline]] bool coverage_init_slow() noexcept {
    const bool on = (std::getenv("VITREO_JIT_COVERAGE") != nullptr);
    if (on) {
        static const Reporter reporter{};
        (void)reporter;
    }
    g_coverage_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

}  // namespace detail

namespace {

/// Find (or claim) the slot for one key. Returns nullptr when the table is
/// full, which is counted, never silently dropped.
Tables::FormSlot* find_form_slot(Tables& t, u64 key, bool create) noexcept {
    usize i = form_slot_hash(key);
    for (usize probe = 0; probe < 64; ++probe, i = (i + 1) & (kFormSlots - 1)) {
        u64 seen = t.forms[i].key.load(std::memory_order_acquire);
        if (seen == key) return &t.forms[i];
        if (seen == 0) {
            if (!create) return nullptr;
            if (t.forms[i].key.compare_exchange_strong(
                    seen, key, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return &t.forms[i];
            }
            // Lost the race; re-check this slot before moving on.
            if (t.forms[i].key.load(std::memory_order_acquire) == key) {
                return &t.forms[i];
            }
        }
    }
    return nullptr;
}

}  // namespace

void record_compiled(Opcode op, FormKey form, bool is_64bit,
                     LowerKind kind) noexcept {
    if (!enabled()) return;
    auto& t = tables();
    const usize b = is_64bit ? 1u : 0u;
    const auto idx = static_cast<usize>(op);
    if (idx >= kOpcodeSlots) {
        t.overflow[b].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // Opcode-level aggregate, kept for the ranked stderr report. A helper
    // lowering is NOT native here: that conflation is exactly what let
    // `jit_sse_op` arms read as covered.
    auto& table = (kind == LowerKind::Native) ? t.compiled_native[b]
                                              : t.compiled_delegate[b];
    table[idx].fetch_add(1, std::memory_order_relaxed);

    if (auto* slot = find_form_slot(t, make_form_key(op, form, is_64bit),
                                    /*create=*/true)) {
        slot->n[static_cast<usize>(kind)].fetch_add(
            1, std::memory_order_relaxed);
    } else {
        t.form_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

u64 compiled_count(Opcode op, FormKey form, bool is_64bit,
                   LowerKind kind) noexcept {
    auto& t = tables();
    auto* slot = find_form_slot(t, make_form_key(op, form, is_64bit),
                                /*create=*/false);
    if (slot == nullptr) return 0;
    return slot->n[static_cast<usize>(kind)].load(std::memory_order_relaxed);
}

bool set_enabled(bool on) noexcept {
    const bool was = enabled();
    (void)tables();  // Construct the storage before anyone records into it.
    detail::g_coverage_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    return was;
}

void reset() noexcept {
    auto& t = tables();
    for (usize b = 0; b < kBitnessSlots; ++b) {
        t.exec_native[b].store(0, std::memory_order_relaxed);
        t.overflow[b].store(0, std::memory_order_relaxed);
        for (usize i = 0; i < kOpcodeSlots; ++i) {
            t.compiled_native[b][i].store(0, std::memory_order_relaxed);
            t.compiled_delegate[b][i].store(0, std::memory_order_relaxed);
            t.exec_delegate[b][i].store(0, std::memory_order_relaxed);
        }
    }
    for (usize i = 0; i < kFormSlots; ++i) {
        if (t.forms[i].key.load(std::memory_order_relaxed) == 0) continue;
        for (auto& c : t.forms[i].n) c.store(0, std::memory_order_relaxed);
        t.forms[i].key.store(0, std::memory_order_release);
    }
    t.form_overflow.store(0, std::memory_order_relaxed);
}

void record_delegate_exec(Opcode op, bool is_64bit) noexcept {
    if (!enabled()) return;
    auto& t = tables();
    const usize b = is_64bit ? 1u : 0u;
    const auto idx = static_cast<usize>(op);
    if (idx >= kOpcodeSlots) {
        t.overflow[b].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 n = t.exec_delegate[b][idx].fetch_add(
                      1, std::memory_order_relaxed) + 1;
    // Periodic, not only at static destruction: every GUI guest is SIGKILLed,
    // so a destructor-only report says nothing about exactly the processes
    // whose delegate profile is worth reading. Same reason the decode memo and
    // the LOCK-path counters dump periodically. The modulus is per-opcode, so a
    // dominant delegate reports early and a long tail never reports alone.
    if ((n % (u64{1} << 22)) == 0) dump();
}

void record_block_exec(u32 native_insns, bool is_64bit) noexcept {
    if (!enabled() || native_insns == 0) return;
    tables().exec_native[is_64bit ? 1u : 0u].fetch_add(
        native_insns, std::memory_order_relaxed);
}

void dump() noexcept {
    if (!enabled()) return;

    auto& t = tables();
    u64 grand_total = 0;
    for (usize b = 0; b < kBitnessSlots; ++b) {
        grand_total += t.exec_native[b].load(std::memory_order_relaxed);
        for (usize i = 0; i < kOpcodeSlots; ++i) {
            grand_total += t.exec_delegate[b][i].load(std::memory_order_relaxed);
        }
    }

    std::fprintf(stderr, "[jit-coverage] pid=%d JIT native-lowering coverage\n",
                 static_cast<int>(::getpid()));
    for (usize b = 0; b < kBitnessSlots; ++b) {
        dump_bitness(b, grand_total);
    }
}

}  // namespace vitreo::cpu::jit::coverage

namespace vitreo::cpu::jit {

void format_form_key(FormKey key, char* out, usize cap) noexcept {
    // Order is fixed and matches the bit order in isa_form.hpp so two reports
    // of the same form are textually identical.
    static constexpr struct { u16 bit; const char* tag; } kTags[] = {
        {FormKey::kMem,      "mem"},
        {FormKey::kMemDst,   "memdst"},
        {FormKey::kImm,      "imm"},
        {FormKey::kImmWide,  "immwide"},
        {FormKey::kRexW,     "rexw"},
        {FormKey::kHighReg,  "highreg"},
        {FormKey::kOpSize16, "o16"},
        {FormKey::kAddrSize, "a32"},
        {FormKey::kSegFsGs,  "fsgs"},
        {FormKey::kSegOther, "seg"},
        {FormKey::kVex,      "vex"},
        {FormKey::kVexL,     "vex256"},
        {FormKey::kVexNds,   "nds"},
        {FormKey::kLock,     "lock"},
        {FormKey::kRep,      "rep"},
        {FormKey::kRipOrAbs, "riprel"},
    };
    if (cap == 0) return;
    usize n = 0;
    for (const auto& t : kTags) {
        if ((key.bits & t.bit) == 0) continue;
        const usize len = std::strlen(t.tag);
        if (n + len + 2 >= cap) break;
        if (n != 0) out[n++] = ',';
        std::memcpy(out + n, t.tag, len);
        n += len;
    }
    if (n == 0) {
        const char* none = "regreg";
        const usize len = std::strlen(none);
        if (len + 1 <= cap) { std::memcpy(out, none, len); n = len; }
    }
    out[n] = '\0';
}

}  // namespace vitreo::cpu::jit
