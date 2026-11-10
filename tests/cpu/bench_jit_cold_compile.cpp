// SPDX-License-Identifier: LGPL-2.1-only
// Production decode + native compilation of unique blocks in one process.
// C++ allocation CALLS (not live heap delta); excludes direct malloc/mmap.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/flag_liveness.hpp"
#include "runtime/execution/block_decoder.hpp"

namespace {
thread_local bool counting = false;
thread_local size_t allocations = 0;
}
void* operator new(size_t size) {
    if (counting) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    std::abort();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using Clock = std::chrono::steady_clock;

// Isolate analysis from emitter/code-cache costs. Both arms reuse production
// output vectors; only the four temporary working vectors differ. Alternate
// arm order within each process to reduce drift and retain the first-call cost.
int analysis_benchmark(GuestMemory& memory, GuestAddr base, usize iterations) {
    for (const bool wide : {false, true}) {
        for (const u32 count : {2u, 8u, 16u, 64u, 192u}) {
            std::vector<u8> bytes;
            for (u32 i = 1; i < count; ++i) bytes.insert(bytes.end(), {0x83, 0xC0, 0x01});
            bytes.push_back(0xC3);
            if (!memory.write_bytes(base, bytes.data(), bytes.size())) return 1;
            cli::reset_decode_memo();
            auto block = cli::decode_basic_block(memory, base, wide);
            if (block.instruction_count != count) return 1;
            std::vector<double> times[2], allocs[2];
            u64 checksums[2]{};
            for (int arm = 0; arm < 2; ++arm) {
                times[arm].reserve(7);
                allocs[arm].reserve(7);
            }
            for (int repeat = 0; repeat < 7; ++repeat) {
                for (int order = 0; order < 2; ++order) {
                    const int arm = order ^ (repeat & 1);
                    FlagLivenessScratch scratch;
                    std::vector<u8> omit;
                    std::vector<u32> spans;
                    std::vector<StencilFlagRecovery> recovery;
                    allocations = 0;
                    counting = true;
                    const auto begin = Clock::now();
                    for (usize i = 0; i < iterations; ++i) {
                        compute_omit_lazy_flags(block, omit, &recovery, &spans, true,
                                                arm ? &scratch : nullptr);
                        if (omit.size() != count || spans.size() != count) return 1;
                        // Touch the complete output, so the analysis is observable.
                        for (usize j = 0; j < omit.size(); ++j)
                            checksums[arm] += omit[j] + spans[j];
                        checksums[arm] += recovery.size();
                    }
                    const auto end = Clock::now();
                    counting = false;
                    times[arm].push_back(std::chrono::duration<double, std::nano>(end-begin).count()/iterations);
                    allocs[arm].push_back(static_cast<double>(allocations)/iterations);
                }
            }
            if (checksums[0] != checksums[1]) return 1;
            for (int arm = 0; arm < 2; ++arm) {
                std::sort(times[arm].begin(), times[arm].end());
                std::sort(allocs[arm].begin(), allocs[arm].end());
                std::printf("{\"bits\":%u,\"instructions\":%u,\"iterations\":%zu,"
                            "\"reuse\":%d,\"flag_analysis_ns\":%.3f,"
                            "\"cpp_allocations_per_block\":%.3f,\"checksum\":%llu}\n",
                            wide?64:32,count,iterations,arm,times[arm][3],allocs[arm][3],
                            static_cast<unsigned long long>(checksums[arm]));
            }
        }
    }
    cli::reset_decode_memo();
    return 0;
}

int main(int argc, char** argv) {
    const usize requested = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1024;
    if (requested == 0 || requested > 8192) return 2;
    constexpr GuestAddr base = 0x100000;
    constexpr usize stride = 1024;
    GuestMemory memory;
    if (!memory.map_region(base, requested * stride,
                           MemoryProtection::ReadWriteExecute)) return 1;
    if (argc > 2 && std::strcmp(argv[2], "--analysis") == 0)
        return analysis_benchmark(memory, base, requested);
    for (const bool wide : {false, true}) {
        for (const u32 count : {2u, 8u, 16u, 64u, 192u}) {
            const usize blocks = count >= 64 ? std::min(requested, usize{128}) : requested;
            // Every ADD writes flags; all but the final live-out definition
            // can be elided. RET ends the production decode at exactly count.
            std::vector<u8> bytes;
            for (u32 i = 1; i < count; ++i) bytes.insert(bytes.end(), {0x83, 0xC0, 0x01});
            bytes.push_back(0xC3);
            for (usize i = 0; i < blocks; ++i) {
                if (!memory.write_bytes(base + i * stride, bytes.data(), bytes.size())) return 1;
            }
            std::vector<double> times, first, allocs;
            // Keep measurement bookkeeping outside the allocation counter.
            times.reserve(7);
            first.reserve(7);
            allocs.reserve(7);
            u64 checksum = 0;
            for (int repeat = 0; repeat < 7; ++repeat) {
                cli::reset_decode_memo();
                JitCompileMemo::clear_compile_memo();
                JitCompiler compiler(memory);
                allocations = 0;
                counting = true;
                const auto begin = Clock::now();
                for (usize i = 0; i < blocks; ++i) {
                    auto block = cli::decode_basic_block(memory, base + i * stride, wide);
                    auto* compiled = compiler.compile(block);
                    if (!compiled || compiled->code_size == 0 ||
                        block.instruction_count != count || block.byte_length != bytes.size() ||
                        compiled->native_insns != count) return 1;
                    checksum += block.instruction_count + block.byte_length +
                                compiled->native_insns + compiled->code_size;
                    if (i == 0) first.push_back(std::chrono::duration<double, std::nano>(Clock::now()-begin).count());
                }
                const auto end = Clock::now();
                counting = false;
                times.push_back(std::chrono::duration<double, std::nano>(end-begin).count()/blocks);
                allocs.push_back(static_cast<double>(allocations)/blocks);
            }
            std::sort(times.begin(), times.end());
            std::sort(first.begin(), first.end());
            std::sort(allocs.begin(), allocs.end());
            std::printf("{\"bits\":%u,\"instructions\":%u,\"blocks\":%zu,"
                        "\"cold_decode_compile_ns\":%.3f,\"first_block_ns\":%.3f,"
                        "\"cpp_allocations_per_block\":%.3f,\"checksum\":%llu}\n",
                        wide?64:32,count,blocks,times[3],first[3],allocs[3],
                        static_cast<unsigned long long>(checksum));
        }
    }
    cli::reset_decode_memo();
    JitCompileMemo::clear_compile_memo();
}
