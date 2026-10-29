// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_pe_block_differential.cpp
/// @brief Opt-in sweep: every basic block of a real x64 PE image, compiled by
///        Tier 1 and run against the interpreter from seeded states.
///
/// Skipped unless VITREO_TEST_PE_IMAGE names a memory-mapped image dump
/// (headers and sections at their RVAs) and VITREO_TEST_PE_LEADERS a file
/// whose first line is the image base and whose remaining lines are block
/// leader RVAs, all hex. The image is mapped at its base; general registers
/// point into a scratch heap filled with pointers back into itself, so loads
/// chain through memory instead of faulting at once.

#if defined(__aarch64__)
#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/memory/guest_memory.hpp"
#include "ir_tier1_first_compile.hpp"
#include "jit_dispatch_execute.hpp"
#include "runtime/execution/block_decoder.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr GuestAddr kHeap = 0x200000000ull;
constexpr usize kHeapSize = usize{1} << 20;

struct Range {
    GuestAddr addr;
    usize size;
};

std::vector<Range> writable_sections(const std::vector<u8>& image,
                                     GuestAddr base) {
    std::vector<Range> out;
    u32 lfanew = 0;
    std::memcpy(&lfanew, image.data() + 0x3C, 4);
    u16 count = 0, opt_size = 0;
    std::memcpy(&count, image.data() + lfanew + 6, 2);
    std::memcpy(&opt_size, image.data() + lfanew + 20, 2);
    const usize table = lfanew + 24 + opt_size;
    for (u16 i = 0; i < count; ++i) {
        const u8* s = image.data() + table + usize{i} * 40;
        u32 vsize = 0, rva = 0, chars = 0;
        std::memcpy(&vsize, s + 8, 4);
        std::memcpy(&rva, s + 12, 4);
        std::memcpy(&chars, s + 36, 4);
        if (chars & 0x80000000u) out.push_back({base + rva, vsize});
    }
    return out;
}

void seed_state(CpuState& cpu, std::mt19937_64& rng, GuestAddr rip) {
    cpu.init_long_mode();
    cpu.rip = rip;
    for (u8 r = 0; r < 16; ++r) {
        const u64 v = rng();
        switch (v % 4) {
        case 0:
        case 1: cpu.gpr.set64(r, kHeap + 0x1000 + ((v >> 8) % (kHeapSize - 0x2000)) / 8 * 8); break;
        case 2: cpu.gpr.set64(r, (v >> 8) & 0xFF); break;
        default: cpu.gpr.set64(r, v >> (v & 63)); break;
        }
    }
    cpu.gpr.set64(4, kHeap + kHeapSize / 2);
    cpu.flags.rflags = 0x202 | (rng() & 0x8D5);
    for (u8 x = 0; x < 16; ++x)
        for (u8 i = 0; i < 2; ++i) cpu.sse.xmm[x].u64_[i] = rng();
}

TEST(IrPeBlockDifferential, EveryBlockMatchesTheInterpreter) {
    const char* image_path = std::getenv("VITREO_TEST_PE_IMAGE");
    const char* leaders_path = std::getenv("VITREO_TEST_PE_LEADERS");
    if (!image_path || !leaders_path) GTEST_SKIP() << "opt-in sweep";
    const bool tier0 = std::getenv("VITREO_TEST_PE_TIER0") != nullptr;
    vitreo::cpu::jit::test::IrTier1FirstCompile tier1;
    if (tier0) JitCompiler::set_ir_tier_threshold_for_test(1 << 30);

    std::ifstream image_file(image_path, std::ios::binary);
    std::vector<u8> image((std::istreambuf_iterator<char>(image_file)),
                          std::istreambuf_iterator<char>());
    ASSERT_GT(image.size(), 0x1000u);
    std::ifstream leaders_file(leaders_path);
    std::string line;
    ASSERT_TRUE(std::getline(leaders_file, line));
    const GuestAddr base = std::stoull(line, nullptr, 16);
    std::vector<u32> leaders;
    while (std::getline(leaders_file, line))
        if (!line.empty()) leaders.push_back(static_cast<u32>(std::stoul(line, nullptr, 16)));
    usize first = 0, limit = leaders.size();
    if (const char* v = std::getenv("VITREO_TEST_PE_FIRST")) first = std::stoul(v);
    if (const char* v = std::getenv("VITREO_TEST_PE_COUNT"))
        limit = std::min(leaders.size(), first + std::stoul(v));
    unsigned seeds = 4;
    if (const char* v = std::getenv("VITREO_TEST_PE_SEEDS")) seeds = std::stoul(v);

    GuestMemory memory;
    constexpr usize reservation = usize{16} << 30;
    LinearReservationRegistration registration;
    if (std::getenv("VITREO_TEST_PE_LINEAR")) {
        ASSERT_TRUE(install_linear_fault_handler());
        ASSERT_TRUE(memory.enable_linear_reservation(reservation));
        ASSERT_TRUE(memory.enable_linear_hardware_protection());
        ASSERT_TRUE(registration.acquire(memory.linear_base(), reservation));
    }
    const usize image_span = (image.size() + 0xFFF) & ~usize{0xFFF};
    ASSERT_TRUE(memory.map_region(base, image_span, MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(memory.write_bytes(base, image.data(), image.size()));
    ASSERT_TRUE(memory.map_region(kHeap, kHeapSize, MemoryProtection::ReadWrite));
    const std::vector<Range> writable = writable_sections(image, base);

    std::vector<u8> heap(kHeapSize);
    auto jit = std::make_unique<JitCompiler>(memory);
    usize compiled = 0, ir_blocks = 0, runs = 0, diverged = 0, skipped_loops = 0;
    std::vector<u8> scratch;
    auto restore = [&] {
        ASSERT_TRUE(memory.write_bytes(kHeap, heap.data(), heap.size()));
        for (const Range& r : writable)
            ASSERT_TRUE(memory.write_bytes(r.addr, image.data() + (r.addr - base), r.size));
    };

    for (usize li = first; li < limit; ++li) {
        const GuestAddr rip = base + leaders[li];
        BasicBlock block = vitreo::cli::decode_basic_block(memory, rip, true);
        if (block.instruction_count == 0) continue;
        if (li % 4096 == 0) jit = std::make_unique<JitCompiler>(memory);
        CompiledBlock* native = jit->compile(block);
        if (native == nullptr) continue;
        ++compiled;
        if (!tier0 && !jit->ir_emit_active()) continue;
        ++ir_blocks;
        for (unsigned s = 0; s < seeds; ++s) {
            std::mt19937_64 rng(li * 977 + s);
            for (usize q = 0; q < kHeapSize; q += 8) {
                const u64 v = rng();
                u64 word = (v & 3) == 0 ? (v >> 17)
                         : (v & 3) == 1 ? (v >> 40)
                         : kHeap + 0x1000 + ((v >> 8) % (kHeapSize - 0x2000)) / 8 * 8;
                std::memcpy(heap.data() + q, &word, 8);
            }
            CpuState oracle;
            seed_state(oracle, rng, rip);
            CpuState state = oracle;
            const CpuState initial = oracle;

            restore();
            const JitExecResult jit_result = test::execute_like_dispatcher(*jit, *native, state);
            std::vector<u8> jit_heap(kHeapSize);
            ASSERT_TRUE(memory.read_bytes(kHeap, jit_heap.data(), kHeapSize));
            const bool jit_fault = state.pending_raise.active;
            if (state.rip == rip && !jit_fault) { ++skipped_loops; continue; }

            restore();
            unsigned steps = 0;
            std::string trace;
            bool interp_fault = false;
            std::optional<CpuState> first_match;
            {
                Interpreter interpreter(oracle, memory);
                const unsigned cap = 1u << 20;
                while (steps < cap) {
                    const u64 before = oracle.rip;
                    const ExecResult r = interpreter.step();
                    ++steps;
                    if (steps < 64) {
                        char t[64];
                        std::snprintf(t, sizeof t, " %llx:%d", (unsigned long long)before, int(r));
                        trace += t;
                    }
                    if (r == ExecResult::Exception) { interp_fault = true; break; }
                    if (r != ExecResult::Continue && r != ExecResult::Branch) break;
                    if (oracle.rip == state.rip && !jit_fault) {
                        bool same = true;
                        for (u8 r = 0; r < 16; ++r)
                            same = same && oracle.gpr.get64(r) == state.gpr.get64(r);
                        if (same) break;
                        if (!first_match) first_match = oracle;
                    }
                }
            }
            ++runs;
            if (!interp_fault && oracle.rip != state.rip && first_match) oracle = *first_match;
            std::vector<u8> interp_heap(kHeapSize);
            ASSERT_TRUE(memory.read_bytes(kHeap, interp_heap.data(), kHeapSize));
            state.materialize_lazy_flags();
            oracle.materialize_lazy_flags();
            std::string diff;
            char buf[160];
            if (jit_fault != interp_fault) {
                std::snprintf(buf, sizeof buf, " fault jit=%d interp=%d", jit_fault, interp_fault);
                diff += buf;
            }
            if (state.rip != oracle.rip) {
                std::snprintf(buf, sizeof buf, " rip jit=%llx interp=%llx",
                              (unsigned long long)state.rip, (unsigned long long)oracle.rip);
                diff += buf;
            }
            for (u8 r = 0; r < 16; ++r)
                if (state.gpr.get64(r) != oracle.gpr.get64(r)) {
                    std::snprintf(buf, sizeof buf, " r%u jit=%llx interp=%llx", r,
                                  (unsigned long long)state.gpr.get64(r),
                                  (unsigned long long)oracle.gpr.get64(r));
                    diff += buf;
                }
            const u64 fmask = std::getenv("VITREO_TEST_PE_ALLFLAGS") ? 0x8D5 : 0xC5;
            if ((state.flags.rflags & fmask) != (oracle.flags.rflags & fmask)) {
                std::snprintf(buf, sizeof buf, " flags jit=%llx interp=%llx",
                              (unsigned long long)(state.flags.rflags & fmask),
                              (unsigned long long)(oracle.flags.rflags & fmask));
                diff += buf;
            }
            for (u8 x = 0; x < 16; ++x)
                if (std::memcmp(&state.sse.xmm[x], &oracle.sse.xmm[x], 16) != 0) {
                    std::snprintf(buf, sizeof buf, " xmm%u", x);
                    diff += buf;
                }
            if (jit_heap != interp_heap) {
                for (usize q = 0; q < kHeapSize; ++q)
                    if (jit_heap[q] != interp_heap[q]) {
                        std::snprintf(buf, sizeof buf, " heap@%llx",
                                      (unsigned long long)(kHeap + q));
                        diff += buf;
                        break;
                    }
            }
            if (!diff.empty()) {
                ++diverged;
                std::printf("DIVERGE leader=%zu rva=%x len=%u seed=%u insns=%u steps=%u:%s\n",
                            li, leaders[li], block.byte_length, s, block.instruction_count, steps, diff.c_str());
                if (std::getenv("VITREO_TEST_PE_VERBOSE")) {
                    for (u8 r = 0; r < 16; ++r)
                        std::printf("  init r%u=%llx final jit=%llx interp=%llx\n", r,
                                    (unsigned long long)initial.gpr.get64(r),
                                    (unsigned long long)state.gpr.get64(r),
                                    (unsigned long long)oracle.gpr.get64(r));
                    std::printf("  init flags=%llx jit_result=%d pending=%d trace:%s\n",
                                (unsigned long long)initial.flags.rflags, int(jit_result),
                                int(state.pending_raise.active), trace.c_str());
                }
                break;
            }
        }
    }
    std::printf("compiled=%zu ir=%zu runs=%zu diverged=%zu loops=%zu\n", compiled,
                ir_blocks, runs, diverged, skipped_loops);
    EXPECT_EQ(diverged, 0u);
}

}  // namespace
#endif
