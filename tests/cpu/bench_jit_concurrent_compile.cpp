// SPDX-License-Identifier: LGPL-2.1-only
// Concurrent FIRST misses through production decode_basic_block + compile.
// No timing assertions. Workers persist across samples. Per-address barriers
// expose same-RIP races; the identical barrier-only control is reported rather
// than subtracted. Validate native execution outside the translation timer.
// Optional argv4=start-only synchronizes only phase entry/exit, preserving
// natural owner-ahead and warm-adoption progress through the cold block set.
#include <algorithm>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/translation_store_stats.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using Clock = std::chrono::steady_clock;

namespace {
constexpr GuestAddr kBase = 0x100000;
constexpr usize kStride = 512;
constexpr u32 kDefaultAdds = 128;

std::vector<u8> corpus(u32 adds) {
    std::vector<u8> bytes{0xB8, 0x34, 0x12, 0, 0}; // MOV EAX,1234h
    for (u32 i = 0; i < adds; ++i)
        bytes.insert(bytes.end(), {0x83, 0xC0, 0x01}); // ADD EAX,1
    bytes.insert(bytes.end(), {0xE9, 0, 0, 0, 0}); // JMP next instruction
    return bytes;
}

CpuState seed(bool wide, GuestAddr address) {
    CpuState cpu;
    if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
    cpu.rip = address;
    cpu.gpr.rsp = 0x90000000;
    cpu.flags.rflags = 0x202;
    return cpu;
}

u64 mix(u64 hash, u64 value) {
    return (hash ^ value) * 1099511628211ull;
}

struct Item {
    CompiledBlock compiled;
    void* cold_code{nullptr};
};
struct Worker {
    std::vector<Item> items;
    bool okay{true};
    u64 checksum{0};
};
enum class Phase { SyncOnly, Cold, Warm, Validate, Stop };

class Workers {
public:
    Workers(GuestMemory& memory, bool wide, bool same, bool lockstep, usize threads,
            usize blocks, usize byte_length, u32 instructions, const CpuState& oracle)
        : m_memory(memory), m_wide(wide), m_same(same), m_lockstep(lockstep), m_blocks(blocks),
          m_length(byte_length), m_instructions(instructions), m_oracle(oracle), m_start(threads + 1),
          m_finish(threads + 1), m_step(threads), results(threads) {
        for (auto& worker : results) worker.items.resize(blocks);
        for (usize t = 0; t < threads; ++t)
            m_threads.emplace_back([this, t] { work(t); });
        // An untimed rendezvous also waits for each thread's JitCompiler
        // construction. None of thread creation/initialization leaks into
        // the measured synchronization-only or translation phases.
        m_start.arrive_and_wait();
        m_finish.arrive_and_wait();
    }
    ~Workers() {
        m_phase = Phase::Stop;
        m_start.arrive_and_wait();
        for (auto& thread : m_threads) thread.join();
    }
    double run(Phase phase, usize sample) {
        m_phase = phase;
        m_sample = sample;
        const auto begin = Clock::now();
        m_start.arrive_and_wait();
        m_finish.arrive_and_wait();
        return std::chrono::duration<double, std::nano>(Clock::now() - begin).count();
    }
    bool okay() const {
        return std::all_of(results.begin(), results.end(),
                           [](const Worker& w) { return w.okay; });
    }
    GuestAddr address(usize thread, usize i) const {
        const usize streams = m_same ? 1 : results.size();
        return kBase + ((m_sample * streams + (m_same ? 0 : thread)) *
                        m_blocks + i) * kStride;
    }
private:
    void work(usize t) {
        // Construct and destroy on the owning worker. compile()/execute()
        // acquire their existing arena leases; no raw epoch manipulation.
        JitCompiler jit(m_memory);
        auto& output = results[t];
        for (;;) {
            m_start.arrive_and_wait();
            if (m_phase == Phase::Stop) return;
            if (m_phase == Phase::Validate) {
                for (auto& item : output.items) {
                    if (!item.compiled.code) { output.okay = false; continue; }
                    CpuState cpu = seed(m_wide, item.compiled.guest_addr);
                    auto result = jit.execute(item.compiled, cpu);
                    if (result == JitExecResult::StaleCode)
                        result = jit.execute(item.compiled, cpu);
                    cpu.materialize_lazy_flags();
                    output.okay &= result == JitExecResult::Continue &&
                        !cpu.pending_raise.active &&
                        cpu.rip == item.compiled.guest_addr + m_length &&
                        cpu.flags.rflags == m_oracle.flags.rflags;
                    if (result != JitExecResult::Continue || cpu.pending_raise.active ||
                        cpu.rip != item.compiled.guest_addr + m_length ||
                        cpu.flags.rflags != m_oracle.flags.rflags)
                        std::fprintf(stderr, "execute mismatch worker=%zu result=%u raise=%u rip=%llx expected=%llx flags=%llx expected_flags=%llx\n",
                            t, unsigned(result), unsigned(cpu.pending_raise.active),
                            (unsigned long long)cpu.rip,
                            (unsigned long long)(item.compiled.guest_addr + m_length),
                            (unsigned long long)cpu.flags.rflags,
                            (unsigned long long)m_oracle.flags.rflags);
                    for (u8 r = 0; r < 16; ++r) {
                        output.okay &= cpu.gpr.get64(r) == m_oracle.gpr.get64(r);
                        if (cpu.gpr.get64(r) != m_oracle.gpr.get64(r))
                            std::fprintf(stderr, "register mismatch worker=%zu reg=%u actual=%llx expected=%llx\n",
                                t, unsigned(r), (unsigned long long)cpu.gpr.get64(r),
                                (unsigned long long)m_oracle.gpr.get64(r));
                    }
                }
            } else {
                output.checksum = 14695981039346656037ull;
                for (usize i = 0; i < m_blocks; ++i) {
                    if (m_lockstep) m_step.arrive_and_wait();
                    if (m_phase == Phase::SyncOnly) continue;
                    auto block = cli::decode_basic_block(m_memory, address(t, i), m_wide);
                    output.okay &= block.instruction_count == m_instructions &&
                        block.byte_length == m_length && block.ends_with_branch;
                    if (block.instruction_count != m_instructions || block.byte_length != m_length || !block.ends_with_branch)
                        std::fprintf(stderr, "decode mismatch worker=%zu insns=%u expected=%u bytes=%u expected_bytes=%zu branch=%u\n",
                            t, block.instruction_count, m_instructions, block.byte_length, m_length, unsigned(block.ends_with_branch));
                    auto* compiled = jit.compile(block);
                    if (!compiled) { std::fprintf(stderr, "compile declined worker=%zu block=%zu\n", t, i); output.okay = false; continue; }
                    output.okay &= compiled->guest_size == m_length &&
                        compiled->native_insns == m_instructions && compiled->code_size != 0;
                    if (compiled->guest_size != m_length || compiled->native_insns != m_instructions || !compiled->code_size)
                        std::fprintf(stderr, "native metadata mismatch worker=%zu guest=%u native_insns=%u code_size=%u\n",
                            t, compiled->guest_size, compiled->native_insns, compiled->code_size);
                    auto& item = output.items[i];
                    item.compiled = *compiled;
                    if (m_phase == Phase::Cold) item.cold_code = compiled->code;
                    output.checksum = mix(output.checksum, block.instruction_count);
                    output.checksum = mix(output.checksum, block.byte_length);
                    output.checksum = mix(output.checksum, compiled->native_insns);
                    output.checksum = mix(output.checksum, compiled->code_size);
                }
            }
            m_finish.arrive_and_wait();
        }
    }

    GuestMemory& m_memory;
    bool m_wide, m_same, m_lockstep;
    usize m_blocks, m_length;
    u32 m_instructions;
    CpuState m_oracle;
    std::barrier<> m_start, m_finish, m_step;
    std::vector<std::thread> m_threads;
    Phase m_phase{Phase::SyncOnly};
    usize m_sample{0};
public:
    std::vector<Worker> results;
};

bool measure(bool wide, bool same, bool lockstep, usize threads,
             usize blocks, usize repeats, u32 adds) {
    const auto bytes = corpus(adds);
    const u32 instructions = adds + 2;
    const usize streams = same ? 1 : threads;
    GuestMemory memory;
    const usize mapped = (streams * blocks * repeats * kStride + 4095) & ~usize{4095};
    if (!memory.map_region(kBase, mapped, MemoryProtection::ReadWriteExecute)) return false;
    for (usize i = 0; i < streams * blocks * repeats; ++i)
        if (!memory.write_bytes(kBase + i * kStride, bytes.data(), bytes.size())) return false;
    CpuState oracle = seed(wide, kBase);
    Interpreter interpreter(oracle, memory);
    for (u32 i = 0; i < instructions; ++i) {
        const auto result = interpreter.step();
        const auto expected = i + 1 == instructions ? ExecResult::Branch : ExecResult::Continue;
        if (result != expected) {
            std::fprintf(stderr, "oracle step failed at %u result=%u expected=%u\n", i, unsigned(result), unsigned(expected));
            return false;
        }
    }
    oracle.materialize_lazy_flags();
    if (oracle.gpr.rax != 0x1234 + adds || oracle.rip != kBase + bytes.size()) {
        std::fprintf(stderr, "oracle result mismatch ax=%llx rip=%llx bytes=%zu\n", (unsigned long long)oracle.gpr.rax, (unsigned long long)oracle.rip, bytes.size());
        return false;
    }

    JitCompileMemo::clear_compile_memo();
    cli::reset_decode_memo();
    Workers workers(memory, wide, same, lockstep, threads, blocks, bytes.size(), instructions, oracle);
    for (usize sample = 0; sample < repeats; ++sample) {
        const double sync_ns = workers.run(Phase::SyncOnly, sample);
        std::unordered_map<GuestAddr, std::unordered_set<void*>> cold_pointers;
        u64 cold_checksum = 0;
        for (const auto phase : {Phase::Cold, Phase::Warm}) {
            const auto before = tstats::totals();
            const usize arena_before = JitCodeArena::arena_bytes_used();
            const u64 retire_before = JitCodeArena::arena_chunk_retirements();
            const double ns = workers.run(phase, sample);
            const auto after = tstats::totals();
            const usize arena_after = JitCodeArena::arena_bytes_used();
            const u64 retired = JitCodeArena::arena_chunk_retirements() - retire_before;
            const u64 fresh = after.compiles[static_cast<usize>(TranslationOutcome::Fresh)] -
                              before.compiles[static_cast<usize>(TranslationOutcome::Fresh)];
            const u64 hits = after.compiles[static_cast<usize>(TranslationOutcome::MemoHit)] -
                             before.compiles[static_cast<usize>(TranslationOutcome::MemoHit)];
            const usize unique_guests = blocks * streams;
            std::unordered_map<GuestAddr, std::unordered_set<void*>> pointers;
            u64 returned_native_bytes = 0, canonical_native_bytes = 0;
            u64 checksum = workers.results[0].checksum;
            if (phase == Phase::Cold) cold_checksum = checksum;
            else if (checksum != cold_checksum) {
                std::fprintf(stderr, "cold/warm metadata checksum mismatch\n");
                return false;
            }
            for (usize t = 0; t < threads; ++t) {
                if (workers.results[t].checksum != checksum) { std::fprintf(stderr, "worker checksum mismatch worker=%zu\n", t); return false; }
                for (const auto& item : workers.results[t].items) {
                    auto& codes = pointers[item.compiled.guest_addr];
                    if (codes.empty()) canonical_native_bytes += item.compiled.code_size;
                    if (codes.insert(item.compiled.code).second)
                        returned_native_bytes += item.compiled.code_size;
                    if (phase == Phase::Warm &&
                        !cold_pointers[item.compiled.guest_addr].contains(item.compiled.code))
                        return false;
                    if (phase == Phase::Warm && !same && item.compiled.code != item.cold_code)
                        return false;
                }
            }
            usize unique_native = 0;
            for (const auto& [address, codes] : pointers) unique_native += codes.size();
            if (pointers.size() != unique_guests || fresh + hits != threads * blocks ||
                retired != 0 || arena_after < arena_before) {
                std::fprintf(stderr, "accounting mismatch pointers=%zu expected=%zu fresh=%llu hits=%llu calls=%zu retired=%llu arena_before=%zu arena_after=%zu\n",
                    pointers.size(), unique_guests, (unsigned long long)fresh, (unsigned long long)hits,
                    threads * blocks, (unsigned long long)retired, arena_before, arena_after);
                return false;
            }
            if (phase == Phase::Warm && (fresh != 0 || unique_native != unique_guests)) return false;
            if (phase == Phase::Cold) cold_pointers = pointers;
            workers.run(Phase::Validate, sample);
            if (!workers.okay()) return false;
            std::printf("{\"bits\":%u,\"threads\":%zu,\"pattern\":\"%s\","
                        "\"sample\":%zu,\"phase\":\"%s\",\"blocks_per_thread\":%zu,"
                        "\"unique_guest_blocks\":%zu,\"backend_compiles\":%llu,"
                        "\"memo_hits\":%llu,\"unique_native_pointers\":%zu,"
                        "\"returned_duplicate_native_bytes\":%llu,\"arena_growth_bytes\":%zu,"
                        "\"wall_ns\":%.0f,\"sync_only_ns\":%.0f,\"checksum\":%llu,"
                        "\"semantic_rax\":%llu,\"semantic_flags\":%llu,"
                        "\"adds\":%u,\"instructions\":%u,\"schedule\":\"%s\"}\n",
                        wide ? 64 : 32, threads, same ? "same" : "disjoint", sample,
                        phase == Phase::Cold ? "cold" : "warm", blocks, unique_guests,
                        static_cast<unsigned long long>(fresh), static_cast<unsigned long long>(hits),
                        unique_native, static_cast<unsigned long long>(returned_native_bytes - canonical_native_bytes),
                        arena_after - arena_before, ns, sync_ns,
                        static_cast<unsigned long long>(checksum),
                        static_cast<unsigned long long>(oracle.gpr.rax),
                        static_cast<unsigned long long>(oracle.flags.rflags), adds, instructions,
                        lockstep ? "lockstep" : "start-only");
            std::fflush(stdout);
        }
    }
    return true;
}
} // namespace

int main(int argc, char** argv) {
    const usize blocks = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 128;
    const usize repeats = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 5;
    const usize adds = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : kDefaultAdds;
    const std::string_view schedule = argc > 4 ? argv[4] : "lockstep";
    if (!blocks || blocks > 4096 || !repeats || repeats > 20 || adds > kDefaultAdds ||
        (schedule != "lockstep" && schedule != "start-only")) return 2;
    // Measure has no disk-cache reuse; count every backend emission and memo
    // adoption with the existing thread-safe production instrumentation.
    set_translation_store_mode_for_test(true, TranslationStoreMode::Measure);
    JitCompileMemo::enable_compile_memo();
    for (const bool wide : {false, true})
        for (const usize threads : {1u, 2u, 4u, 8u})
            for (const bool same : {true, false})
                if (!measure(wide, same, schedule == "lockstep", threads,
                             blocks, repeats, static_cast<u32>(adds))) {
                    std::fprintf(stderr, "concurrent compile validation failed bits=%u threads=%zu pattern=%s\n",
                                 wide ? 64 : 32, threads, same ? "same" : "disjoint");
                    return 3;
                }
    JitCompileMemo::clear_compile_memo();
    cli::reset_decode_memo();
    JitCompileMemo::disable_compile_memo();
    return 0;
}
