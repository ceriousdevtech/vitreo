// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Emitted-size gates for block TERMINATORS (Jcc / JMP / CALL / RET).
//
// Whole-guest telemetry (`VITREO_JIT_STATS`) put conditional branches at the
// top of the emitted-ARM budget by a wide margin -- ~110-165 ARM instructions
// per Jcc against ~10-19 for an ordinary data op. That cost is not in the
// condition evaluation; it is in the two block tails a terminal Jcc emits,
// each of which carried its own private copy of the "publish NZCV, store
// pins, pop the chain frame, return" epilogue.
//
// These tests measure the emitted word count for the canonical terminator
// shapes and hold upper bounds on them. They are upper bounds, not exact
// figures, so an unrelated lowering change does not break them; they are
// tight enough that reintroducing a per-tail epilogue copy fails them.
//
// `Report*` cases assert nothing and exist so the numbers stay visible in the
// test log.

#include <gtest/gtest.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/state/cpu_state.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr GuestAddr kCodeBase  = 0x140000000ull;
constexpr usize     kCodeSize  = 0x4000;
constexpr GuestAddr kCode32    = 0x00400000ull;
constexpr GuestAddr kStack     = 0x540000000ull;
constexpr usize     kStackSize = 0x4000;
constexpr GuestAddr kTeb       = 0x550000000ull;
constexpr usize     kTebSize   = 0x4000;

class JitTerminatorSizeTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCodeBase, kCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kCode32, kCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kStack, kStackSize,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kTeb, kTebSize,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    BasicBlock build_block(GuestAddr base, const std::vector<u8>& code,
                           bool is_64) {
        BasicBlock block;
        block.start_addr = base;
        block.is_64bit = is_64;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader,
                                   is_64 ? DecodeMode::Mode64
                                         : DecodeMode::Mode32,
                                   inst) != DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = static_cast<u32>(block.instructions.size());
        return block;
    }

    // Returns emitted ARM instruction count, or 0 if compilation refused.
    u32 words(GuestAddr base, const std::vector<u8>& code, bool is_64) {
        EXPECT_TRUE(m_memory->write_bytes(base, code.data(), code.size()));
        BasicBlock block = build_block(base, code, is_64);
        EXPECT_GT(block.instruction_count, 0u);
        CompiledBlock* cb = m_jit->compile(block);
        if (cb == nullptr) return 0;
        // A tier-0 compile carries the fixed-size tier counter site, which
        // test_jit_ir_tier.cpp bounds; these budgets are for the block's code.
        const u32 tier_site =
            m_jit->last_compile_was_tier0() ? JitCompiler::kTierSiteWords : 0;
        return cb->code_size / 4 - tier_site;
    }

    u32 words64(const std::vector<u8>& c) { return words(kCodeBase, c, true); }
    u32 words32(const std::vector<u8>& c) { return words(kCode32, c, false); }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// cmp eax,ecx ; jz rel32(+0x40)  -- the FUSED shape (NZCV live from CMP).
const std::vector<u8> kCmpJzRel32 = {
    0x39, 0xC8,
    0x0F, 0x84, 0x40, 0x00, 0x00, 0x00,
};

// cmp eax,ecx ; jz rel8(+0x10)
const std::vector<u8> kCmpJzRel8 = {
    0x39, 0xC8,
    0x74, 0x10,
};

// cmp eax,ecx alone -- the terminator's cost is the difference against this.
const std::vector<u8> kCmpOnly = { 0x39, 0xC8 };

// cmp eax,ecx ; jmp rel32
const std::vector<u8> kCmpJmpRel32 = {
    0x39, 0xC8,
    0xE9, 0x40, 0x00, 0x00, 0x00,
};

// call rel32(+0x40) -- the stack write must retire before RSP/RIP commit, but
// a successful linked hop need not publish the target through CpuState.
const std::vector<u8> kCallRel32 = {
    0xE8, 0x40, 0x00, 0x00, 0x00,
};

// add eax,ecx — the x64 32-bit ALU shape that used to copy both pins through
// scratches, then publish flags with FMOV/INS/STR Q.
const std::vector<u8> kAddEaxEcx = {0x01, 0xC8};

}  // namespace

// Pins the per-block (legacy) exit emission for a test that inspects the
// words of a block's own probe, dispatcher tail or prologue; the shared
// stubs carry the same words once per process.
struct SharedExitStubsOff {
    SharedExitStubsOff() { vitreo::cpu::jit::JitCompiler::set_shared_exit_stubs_for_test(0); }
    ~SharedExitStubsOff() { vitreo::cpu::jit::JitCompiler::set_shared_exit_stubs_for_test(-1); }
};

// ---------------------------------------------------------------------------
// Measurement -- no assertions, keeps the figures in the log.
// ---------------------------------------------------------------------------
TEST_F(JitTerminatorSizeTest, ReportTerminatorEmittedWords) {
    const u32 base64 = words64(kCmpOnly);
    std::printf("[measure] x64  add eax,ecx                 : %u ARM\n",
                words64(kAddEaxEcx));
    std::printf("[measure] x64  cmp eax,ecx (no terminator) : %u ARM\n",
                base64);
    std::printf("[measure] x64  cmp ; jz rel32              : %u ARM\n",
                words64(kCmpJzRel32));
    std::printf("[measure] x64  cmp ; jz rel8               : %u ARM\n",
                words64(kCmpJzRel8));
    std::printf("[measure] x64  cmp ; jmp rel32             : %u ARM\n",
                words64(kCmpJmpRel32));
    std::printf("[measure] x64  call rel32                  : %u ARM\n",
                words64(kCallRel32));

    const u32 base32 = words32(kCmpOnly);
    std::printf("[measure] x86  cmp eax,ecx (no terminator) : %u ARM\n",
                base32);
    std::printf("[measure] x86  cmp ; jz rel32              : %u ARM\n",
                words32(kCmpJzRel32));
    std::printf("[measure] x86  cmp ; jz rel8               : %u ARM\n",
                words32(kCmpJzRel8));
    std::printf("[measure] x86  cmp ; jmp rel32             : %u ARM\n",
                words32(kCmpJmpRel32));
}

// Same shapes with inline block linking ON -- the production configuration.
// Compile only; these blocks are never executed, so no stale chain can form.
TEST_F(JitTerminatorSizeTest, ReportTerminatorEmittedWordsLinked) {
    JitCompiler::enable_jit_link();
    std::printf("[measure/linked] x64  cmp only        : %u ARM\n",
                words64(kCmpOnly));
    std::printf("[measure/linked] x64  cmp ; jz rel32  : %u ARM\n",
                words64(kCmpJzRel32));
    std::printf("[measure/linked] x64  cmp ; jmp rel32 : %u ARM\n",
                words64(kCmpJmpRel32));
    std::printf("[measure/linked] x86  cmp only        : %u ARM\n",
                words32(kCmpOnly));
    std::printf("[measure/linked] x86  cmp ; jz rel32  : %u ARM\n",
                words32(kCmpJzRel32));
    std::printf("[measure/linked] x86  cmp ; jmp rel32 : %u ARM\n",
                words32(kCmpJmpRel32));
    JitCompiler::disable_jit_link();
}

// Reach gate. Before the shared exit stub, every block tail carried its own
// `LDR X30,[SP],#16 ; MOV W0,#0 ; RET`, so a linked terminal Jcc block held
// two RETs and two L1 probes. Counting the RETs proves the shared stub is on
// the emitted path -- a size number alone could come from anywhere.
namespace {

// A split-placed block's words in buffer order: the hot part, then the cold
// part, which the emitter laid down after it.
std::vector<u32> hot_then_cold_words(const CompiledBlock& cb) {
    std::vector<u32> words(cb.code_size / sizeof(u32));
    std::memcpy(words.data(), cb.code, cb.code_size);
    if (const u8* cold = cb.cold_code()) {
        const usize n = cb.cold_size / sizeof(u32);
        const usize at = words.size();
        words.resize(at + n);
        std::memcpy(words.data() + at, cold, cb.cold_size);
    }
    return words;
}
}  // namespace

TEST_F(JitTerminatorSizeTest, LinkedJccBlockHasOneSharedReturn) {
    // This gate isolates the shared L1 probe of the fixed pin layout.
    JitCompiler::enable_jit_link();
    EXPECT_TRUE(m_memory->write_bytes(kCodeBase, kCmpJzRel32.data(),
                                      kCmpJzRel32.size()));
    BasicBlock block = build_block(kCodeBase, kCmpJzRel32, true);
    CompiledBlock* cb = m_jit->compile(block);
    JitCompiler::disable_jit_link();
    ASSERT_NE(cb, nullptr);

    constexpr u32 kRet   = 0xD65F03C0u;  // RET X30
    constexpr u32 kBrX16 = 0xD61F0200u;  // BR X16 -- the linked hop
    u32 rets = 0, hops = 0;
    // Both parts: the shared return is a miss path, in the cold part.
    for (const u32 w : hot_then_cold_words(*cb)) {
        if (w == kRet) ++rets;
        if (w == kBrX16) ++hops;
    }
    EXPECT_EQ(rets, 1u) << "each Jcc leg is still emitting a private epilogue";
    // One shared probe. Its way-1 hit runs its own copy of the hit suffix,
    // BR X16 included, when l1_way1_fallthrough_enabled().
    const u32 hops_per_probe =
        JitCompiler::l1_way1_fallthrough_enabled() ? 2u : 1u;
    EXPECT_EQ(hops, hops_per_probe)
        << "each Jcc leg is still emitting a private L1 probe";
}

TEST_F(JitTerminatorSizeTest, LinkedDirectCallPublishesRipOnlyOnFallback) {
    SharedExitStubsOff legacy_exits;
    struct LinkGuard {
        LinkGuard() { JitCompiler::enable_jit_link(); }
        ~LinkGuard() { JitCompiler::disable_jit_link(); }
    } link_guard;

    EXPECT_TRUE(m_memory->write_bytes(kCodeBase, kCallRel32.data(),
                                      kCallRel32.size()));
    BasicBlock block = build_block(kCodeBase, kCallRel32, true);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    // The fallback that publishes the RIP is a miss path in the cold part.
    const std::vector<u32> emitted = hot_then_cold_words(*cb);
    constexpr u32 kBrX16 = 0xD61F0200u;
    const u32 publish_rip = arm64::str_x_uoff(
        REG_SCRATCH0, REG_CPU_STATE, rip_offset() >> 3);
    constexpr u64 target = kCodeBase + 5 + 0x40;
    const std::vector<u32> deferred_site = {
        arm64::movz_x(REG_SCRATCH0, static_cast<u16>(target), 0),
        arm64::movk_x(REG_SCRATCH0, static_cast<u16>(target >> 16), 16),
        arm64::movk_x(REG_SCRATCH0, static_cast<u16>(target >> 32), 32),
    };
    const auto site = std::search(emitted.begin(), emitted.end(),
                                  deferred_site.begin(), deferred_site.end());
    ASSERT_NE(site, emitted.end())
        << "linked CALL did not defer its adjacent RIP publication";
    EXPECT_NE(std::find(site + deferred_site.size(), emitted.end(), kBrX16),
              emitted.end());
    EXPECT_NE(std::find(site + deferred_site.size(), emitted.end(), publish_rip),
              emitted.end())
        << "linked CALL fallback no longer publishes the target RIP";
}

// ---------------------------------------------------------------------------
// Gates. A terminal Jcc emits TWO block tails; a shared out-of-line exit stub
// makes the second tail nearly free. Baselines before the shared-exit change
// were 118 (x64 rel32) and 109 (x86 rel32) total block words.
// ---------------------------------------------------------------------------
TEST_F(JitTerminatorSizeTest, TerminalJccBlockStaysSmall) {
    const u32 n64 = words64(kCmpJzRel32);
    ASSERT_GT(n64, 0u);
    EXPECT_LE(n64, 100u) << "x64 cmp;jz rel32 block grew to " << n64;

    const u32 n32 = words32(kCmpJzRel32);
    ASSERT_GT(n32, 0u);
    EXPECT_LE(n32, 95u) << "x86 cmp;jz rel32 block grew to " << n32;
}

// The two legs of a Jcc must not each cost a full tail: the whole Jcc block
// must stay well under twice an unconditional JMP block, which has one tail.
TEST_F(JitTerminatorSizeTest, JccCostsLessThanTwoUnconditionalTails) {
    const u32 jcc = words64(kCmpJzRel32);
    const u32 jmp = words64(kCmpJmpRel32);
    ASSERT_GT(jcc, 0u);
    ASSERT_GT(jmp, 0u);
    EXPECT_LT(jcc, 2u * jmp)
        << "jcc=" << jcc << " jmp=" << jmp
        << " -- the second Jcc leg is paying a full private tail";
}

// Intel SDM Vol. 1 §3.4.1.1: a 32-bit write zero-extends the destination
// 64-bit GPR. The lowering must operate on the X pin with a W-form ALU so
// bits 63:32 clear, and it must not pay a load/store pair around that op.
// Intel SDM Vol. 2 CALL: push next RIP, then transfer. The return-address
// store grows the stack by touching the guard page; do not bake a
// StackLimit helper that raises before that store.
TEST_F(JitTerminatorSizeTest, X64CallRel32DoesNotSpillCalleeSavedPins) {
    const u32 n = words64(kCallRel32);
    ASSERT_GT(n, 0u);
    EXPECT_LE(n, 86u) << "x64 call rel32 block grew to " << n
                      << " — CALL emit picked up another helper or pin spill";
}

// NT_TIB.StackLimit is the lowest *committed* page, not the reserved floor.
// A CALL whose return-address slot is below StackLimit but still mapped must
// store and retire, the same way a guard-page touch grows the stack on
// Windows. Raising STATUS_STACK_OVERFLOW first turns ordinary deep calls
// into exceptions.
TEST_F(JitTerminatorSizeTest, X64CallRel32StoresBelowTebStackLimit) {
    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, kCallRel32.data(),
                                      kCallRel32.size()));
    BasicBlock block = build_block(kCodeBase, kCallRel32, true);
    ASSERT_EQ(block.instruction_count, 1u);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);

    const u64 stack_limit = kStack + 0x1000;
    const u64 stack_base = kStack + kStackSize;
    ASSERT_TRUE(m_memory->write_u64(kTeb + 0x08, stack_base));
    ASSERT_TRUE(m_memory->write_u64(kTeb + 0x10, stack_limit));

    CpuState cpu;
    cpu.init_long_mode();
    cpu.rip = kCodeBase;
    cpu.set_sp(stack_limit);
    cpu.seg.gs_desc.base = kTeb;

    EXPECT_EQ(m_jit->execute(*compiled, cpu), JitExecResult::Continue);
    EXPECT_FALSE(cpu.pending_raise.active)
        << "CALL raised 0x" << std::hex
        << cpu.pending_raise.code
        << " instead of storing through the mapped slot below StackLimit";
    EXPECT_EQ(cpu.sp(), stack_limit - 8);
    EXPECT_EQ(m_memory->read_u64(stack_limit - 8).value(),
              kCodeBase + kCallRel32.size());
}

TEST_F(JitTerminatorSizeTest, X64Reg32AluOperatesInPlaceAndZeroExtends) {
    const u32 n = words64(kAddEaxEcx);
    ASSERT_GT(n, 0u);
    EXPECT_LE(n, 28u) << "x64 add eax,ecx block grew to " << n
                      << " — pin staging or 32-bit flag publication bloated";

    EXPECT_TRUE(m_memory->write_bytes(kCodeBase, kAddEaxEcx.data(),
                                      kAddEaxEcx.size()));
    BasicBlock block = build_block(kCodeBase, kAddEaxEcx, true);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rax = 0x1111111100000005ull;
    cpu.gpr.rcx = 0x2222222200000003ull;
    cpu.gpr.rsp = kStack + kStackSize - 0x100;
    cpu.rip = kCodeBase;
    EXPECT_EQ(m_jit->execute(*cb, cpu), JitExecResult::Continue);
    EXPECT_EQ(cpu.gpr.rax, 0x0000000000000008ull);
    EXPECT_EQ(cpu.gpr.rcx, 0x2222222200000003ull);
}
