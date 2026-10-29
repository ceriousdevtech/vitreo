// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Shared JitExecTest fixture and code-scan helpers for the test_jit_*.cpp suite

#pragma once

#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "jit_dispatch_execute.hpp"
#include "core/cpu/jit/jit_stats.hpp"
#include "core/math/host_fenv.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/jit/jit_emit_internal.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {
// The word ranges a compiled block occupies: [code, code + code_size) and,
// for a block placed split (VITREO_JIT_SPLIT_COLD), its cold part. Pointers
// are the placed addresses, so a branch read from either range resolves
// against them directly.
struct WordRange {
    const u32* begin;
    const u32* end;
};
inline std::vector<WordRange> block_word_ranges(const CompiledBlock& cb) {
    std::vector<WordRange> ranges;
    const auto* hot = static_cast<const u32*>(cb.code);
    ranges.push_back({hot, hot + cb.code_size / sizeof(u32)});
    if (const u8* cold_bytes = cb.cold_code()) {
        const auto* cold = reinterpret_cast<const u32*>(cold_bytes);
        ranges.push_back({cold, cold + cb.cold_size / sizeof(u32)});
    }
    return ranges;
}
} // namespace

// Register indices
constexpr u8 EAX = static_cast<u8>(RegIndex::RAX);
constexpr u8 ECX = static_cast<u8>(RegIndex::RCX);
constexpr u8 EDX = static_cast<u8>(RegIndex::RDX);
constexpr u8 EBX = static_cast<u8>(RegIndex::RBX);
constexpr u8 ESP = static_cast<u8>(RegIndex::RSP);
constexpr u8 EBP = static_cast<u8>(RegIndex::RBP);
constexpr u8 ESI = static_cast<u8>(RegIndex::RSI);
constexpr u8 EDI = static_cast<u8>(RegIndex::RDI);

class JitExecTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000,
                                                MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_protected_mode();
        m_cpu->gpr.set32(ESP, 0x10000);
        m_cpu->rip = 0x1000;

        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }

    // Decode machine code into a BasicBlock
    BasicBlock build_block(const u8* code, usize len) {
        auto wr = m_memory->write_bytes(0x1000, code, len);
        EXPECT_TRUE(wr);

        BasicBlock block;
        block.start_addr = 0x1000;

        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError err = decode_instruction(reader, DecodeMode::Mode32, inst);
            if (err != DecodeError::Success) break;
            block.instructions.push_back(inst);
        }

        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    // Compile + execute, return result
    JitExecResult jit_run(const u8* code, usize len) {
        BasicBlock block = build_block(code, len);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        if (!cb) return JitExecResult::Unsupported;
        return test::execute_like_dispatcher(*m_jit, *cb, *m_cpu);
    }

    BasicBlock build_block64_at(GuestAddr address, const u8* code, usize len) {
        auto wr = m_memory->write_bytes(address, code, len);
        EXPECT_TRUE(wr);

        BasicBlock block;
        block.start_addr = address;
        block.is_64bit = true;

        MemoryByteReader reader(code, len);
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            DecodeError err = decode_instruction(reader, DecodeMode::Mode64, inst);
            if (err != DecodeError::Success) break;
            block.instructions.push_back(inst);
        }

        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        return block;
    }

    BasicBlock build_block64(const u8* code, usize len) {
        return build_block64_at(0x1000, code, len);
    }

    JitExecResult jit_run64(const u8* code, usize len) {
        BasicBlock block = build_block64(code, len);
        CompiledBlock* cb = m_jit->compile(block);
        EXPECT_NE(cb, nullptr);
        if (!cb) return JitExecResult::Unsupported;
        return test::execute_like_dispatcher(*m_jit, *cb, *m_cpu);
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
    std::unique_ptr<JitCompiler> m_jit;
};

// Pins the per-block (legacy) exit emission for a test that inspects the
// words of a block's own probe, dispatcher tail or prologue; the shared
// stubs carry the same words once per process.
struct SharedExitStubsOff {
    SharedExitStubsOff() { vitreo::cpu::jit::JitCompiler::set_shared_exit_stubs_for_test(0); }
    ~SharedExitStubsOff() { vitreo::cpu::jit::JitCompiler::set_shared_exit_stubs_for_test(-1); }
};

namespace {

inline usize count_word(const CompiledBlock& cb, u32 word) {
    usize hits = 0;
    for (const WordRange& range : block_word_ranges(cb)) {
        hits += static_cast<usize>(std::count(range.begin, range.end, word));
    }
    return hits;
}

inline bool code_contains_word(const CompiledBlock& cb, u32 word) {
    return count_word(cb, word) != 0;
}

// Block linking is a global opt-in and defaults to off, so every test below
// has to turn it on (and back off) or it would assert against an unlinked
// epilogue and pass for the wrong reason.
struct JitLinkGuard {
    JitLinkGuard() { JitCompiler::enable_jit_link(); }
    ~JitLinkGuard() { JitCompiler::disable_jit_link(); }
};

}  // namespace
