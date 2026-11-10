// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// compile_vex256_lane_split with a memory source, followed by another
// instruction in the same block.
//
// The driver emits a checked whole-operand probe of the m256 (and lane 0's
// own checked m128 read) before it knows whether both lanes will lower. A
// checked load registers its slow path (a ColdMemStub naming branch
// positions in the code buffer), so discarding the emitted words on a
// decline would leave those entries pointing into whatever the NEXT
// instruction emits there, and the block-tail stub flush would patch that
// code. Every allowlisted VEX.256 form with a memory source is run, with a
// trailing `mov eax, imm32`, through the interpreter and through the JIT
// (delegate allowed, as production runs a declined form) in all four memory
// setups; the two must agree on every vector register, RAX and RIP.

#include <gtest/gtest.h>

#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "jit_crypto_test_harness.hpp"

namespace vitreo::cpu::jit {
[[nodiscard]] bool vex256_lane_split_allowed(Opcode op) noexcept;
}

using namespace vitreo;
using namespace vitreo::cpu;
namespace w = ws3test;

namespace {

constexpr u8 kRax = static_cast<u8>(RegIndex::RAX);

/// Every 3-byte-VEX, L=1, [rdx] encoding whose opcode is on the lane-split
/// allowlist, with an imm8 appended when the form takes one.
std::vector<std::vector<u8>> allowlisted_m256_forms(bool bits64) {
    std::vector<std::vector<u8>> out;
    for (u8 map = 1; map <= 3; ++map) {
        for (u8 pp = 0; pp < 4; ++pp) {
            for (unsigned op = 0; op < 256; ++op) {
                // vvvv = xmm2 (inverted 1101b), L = 1, W = 0; ModRM [rdx],
                // reg = xmm1.
                std::vector<u8> b{0xC4, static_cast<u8>(0xE0 | map),
                                  static_cast<u8>((13u << 3) | 4u | pp),
                                  static_cast<u8>(op), 0x0A, 0x1B};
                MemoryByteReader reader(b.data(), b.size());
                DecodedInstruction in;
                if (decode_instruction(reader,
                                       bits64 ? DecodeMode::Mode64
                                              : DecodeMode::Mode32,
                                       in) != DecodeError::Success) {
                    continue;
                }
                if (!in.prefixes.vex || !in.prefixes.vex_l) continue;
                if (!jit::vex256_lane_split_allowed(in.opcode)) continue;
                bool mem = false;
                for (u8 i = 0; i < in.operand_count; ++i)
                    mem |= is_memory_operand(in.operands[i].type);
                if (!mem) continue;
                b.resize(in.length);
                out.push_back(b);
            }
        }
    }
    return out;
}

}  // namespace

TEST(Vex256LaneSplitDecline, MemorySourceThenNextInstructionMatchesInterpreter) {
    const std::vector<u8> trailing{0xB8, 0x78, 0x56, 0x34, 0x12};  // mov eax
    for (w::MemCfg cfg : w::kAllCfgs) {
        w::CryptoRig rig;
        if (!rig.init(cfg)) continue;
        SCOPED_TRACE(w::cfg_name(cfg));
        const auto forms = allowlisted_m256_forms(rig.bits64());
        ASSERT_GT(forms.size(), 50u);
        // A finite, non-zero m256 so FP forms raise nothing.
        for (u32 i = 0; i < 8; ++i) {
            ASSERT_TRUE(rig.mem().write_u32(w::kData + 4 * i,
                                            0x3F800000u + i * 0x10000u));
        }
        for (const auto& form : forms) {
            std::vector<u8> code = form;
            code.insert(code.end(), trailing.begin(), trailing.end());
            ::testing::Message m;
            for (u8 x : form) m << std::hex << int(x) << ' ';
            SCOPED_TRACE(m);
            const CpuState init = rig.base();
            CpuState interpreted = init;
            ASSERT_EQ(rig.interp(code, interpreted, 2), ExecResult::Continue);
            const w::JitRun r = rig.jit(code, init, /*allow_delegate=*/true);
            ASSERT_TRUE(r.compiled);
            EXPECT_FALSE(r.cpu.pending_raise.active);
            for (int i = 0; i < 16; ++i) {
                SCOPED_TRACE(::testing::Message() << "xmm" << i);
                EXPECT_EQ(r.cpu.sse.xmm[i].u64_[0], interpreted.sse.xmm[i].u64_[0]);
                EXPECT_EQ(r.cpu.sse.xmm[i].u64_[1], interpreted.sse.xmm[i].u64_[1]);
                EXPECT_EQ(r.cpu.avx.ymm_high[i].u64_[0],
                          interpreted.avx.ymm_high[i].u64_[0]);
                EXPECT_EQ(r.cpu.avx.ymm_high[i].u64_[1],
                          interpreted.avx.ymm_high[i].u64_[1]);
            }
            EXPECT_EQ(static_cast<u32>(r.cpu.gpr.get64(kRax)), 0x12345678u);
            EXPECT_EQ(r.cpu.rip, interpreted.rip);
        }
    }
}
