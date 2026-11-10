// SPDX-License-Identifier: LGPL-2.1-only
// Empty-memo decode throughput, separately from warm memo lookup. No timing
// assertions: compare interleaved Release builds on the same machine.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/memory/guest_memory.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cli;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
    const usize requested = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 8192;
    if (requested == 0 || requested > 65536) return 2;
    constexpr GuestAddr base = 0x100000;
    constexpr usize stride = 256;
    GuestMemory memory;
    if (!memory.map_region(base, requested * stride,
                           MemoryProtection::ReadWriteExecute)) return 1;
    for (const bool wide : {false, true}) {
        for (const u32 count : {2u, 8u, 16u, 17u, 64u, 192u}) {
            // Keep each row within the default memo retention budget so the
            // warm measurement really measures hits, including long traces.
            const usize blocks = std::min(requested,
                (32u << 20) / (count * sizeof(cpu::DecodedInstruction)));
            std::vector<u8> bytes(count - 1, 0x90);
            bytes.push_back(0xC3);
            for (usize i = 0; i < blocks; ++i) {
                if (!memory.write_bytes(base + i * stride,
                                        bytes.data(), bytes.size())) return 1;
            }
            std::vector<double> cold, warm;
            u64 checksum = 0;
            for (int repeat = 0; repeat < 7; ++repeat) {
                reset_decode_memo();
                for (int phase = 0; phase < 2; ++phase) {
                    const auto begin = Clock::now();
                    for (usize i = 0; i < blocks; ++i) {
                        const auto block = decode_basic_block(
                            memory, base + i * stride, wide);
                        if (block.instruction_count != count ||
                            block.byte_length != count ||
                            !block.ends_with_branch ||
                            block.instructions[count - 1].opcode !=
                                cpu::Opcode::RET_NEAR) return 1;
                        checksum += block.instruction_count;
                    }
                    const double ns = std::chrono::duration<double, std::nano>(
                        Clock::now() - begin).count() / blocks;
                    (phase == 0 ? cold : warm).push_back(ns);
                }
                if (decode_memo_stats().hits != blocks) return 1;
            }
            std::sort(cold.begin(), cold.end());
            std::sort(warm.begin(), warm.end());
            std::printf("{\"bits\":%u,\"instructions\":%u,\"blocks\":%zu,"
                        "\"cold_ns\":%.3f,\"warm_ns\":%.3f,\"checksum\":%llu}\n",
                        wide ? 64 : 32, count, blocks, cold[3], warm[3],
                        static_cast<unsigned long long>(checksum));
        }
    }
    reset_decode_memo();
}
