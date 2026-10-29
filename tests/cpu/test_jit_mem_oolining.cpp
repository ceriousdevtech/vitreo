// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Gates for the out-of-lined guest-memory slow path, the shared per-block
// raise-exit stub, and the narrowed pin spill around pure memory helpers.
//
// Three separable changes share these tests:
//
//   Stage 1 -- move the memory slow path (helper marshal + BLR + raise check)
//              out of the hot fall-through into the block tail.
//   Stage 2 -- emit ONE `pending_raise` exit stub per block instead of 14
//              words at every helper call site.
//   Stage 3 -- around a *pure memory* helper, spill only the caller-saved
//              pins (RSP->X12, RBP->X13). RAX/RCX/RDX/RBX/RSI/RDI live in
//              X23-X28, which AAPCS64 makes callee-saved, so the C helper is
//              required to preserve them.
//
// Honest labelling of what each test can catch:
//
//   * Stage 1 is pure code motion and changes no semantics, so NO test here
//     can fail on it for a correctness reason. `HotPathHasNoHelperCallBefore
//     TheInlineLoad` is a *structural* gate and is the only true negative
//     control Stage 1 admits.
//   * `SingleMemoryInstructionBlockCodeSize` is a true negative control for
//     Stages 2+3 (it fails against the pre-change tree) but NOT for Stage 1,
//     which leaves the total emitted word count unchanged by construction.
//   * The fault tests are regression tests. They cannot fail before the change
//     because the behaviour they pin already holds; they exist because Stage 3
//     is the change most able to break them.
//
// Every memory operand here is above 4 GB. A 64-bit effective address
// marshalled to a helper through a 32-bit MOV is silently correct below 4 GB,
// which is how that bug family kept surviving older tests.

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <functional>
#include <vector>
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/stencils.hpp"
#include "core/cpu/jit/block_cache.hpp"
#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u8 RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5,
             RSI = 6, RDI = 7;

constexpr GuestAddr kCodeBase = 0x140000000ull;
constexpr usize     kCodeSize = 0x4000;
constexpr GuestAddr kData     = 0x240006000ull;   // > 4 GB, per the EA rule
constexpr usize     kDataSize = 0x2000;
constexpr GuestAddr kStack    = 0x540000000ull;
constexpr usize     kStackSize = 0x4000;
// Deliberately unmapped, and deliberately NOT reachable by truncating
// kUnmapped to 32 bits (which would land in nothing either way).
constexpr GuestAddr kUnmapped = 0x7F0000000ull;

constexpr u32 kBlrX16 = 0xD63F0200u;
constexpr u32 kDmbIsh = 0xD5033BBFu;
// linear_fault.hpp: the no-op ARM word marking a TSO site the
// exact-site fault handler may rewrite.
constexpr u32 kTsoPatchMarker = 0xAA1F03FFu;

bool is_acquire_load32(u32 word, u8 address_reg) {
    const u32 opcode = word & ~0x1Fu;
    return opcode == arm64::ldapr_w(0, address_reg) ||
           opcode == arm64::ldar_w(0, address_reg);
}

class JitMemOoliningTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCodeBase, kCodeSize,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kData, kDataSize,
                                         MemoryProtection::ReadWrite));
        ASSERT_TRUE(m_memory->map_region(kStack, kStackSize,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    BasicBlock build_block(const std::vector<u8>& code, bool is_64) {
        BasicBlock block;
        block.start_addr = kCodeBase;
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

    CompiledBlock* compile64(const std::vector<u8>& code) {
        EXPECT_TRUE(m_memory->write_bytes(kCodeBase, code.data(),
                                          code.size()));
        BasicBlock block = build_block(code, true);
        EXPECT_GT(block.instruction_count, 0u);
        return m_jit->compile(block);
    }

    static std::vector<u32> words_of(const CompiledBlock* cb) {
        std::vector<u32> out(cb->code_size / 4);
        std::memcpy(out.data(), cb->code, cb->code_size);
        return out;
    }

    void init_cpu(CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, kStack + kStackSize - 0x100);
        cpu.rip = kCodeBase;
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

// `mov eax, [rbx+0x24]` -- the reference instruction the whole design is
// measured against. 3 guest bytes, RBX pinned in X26, EAX pinned in X23.
const std::vector<u8> kMovEaxRbx24 = {0x8B, 0x43, 0x24};
const std::vector<u8> kMovRbx24Eax = {0x89, 0x43, 0x24};

// Six 32-bit loads through RBX at distinct offsets: the shape where Stage 2's
// shared raise stub actually pays (baseline emits 14 raise words per access).
const std::vector<u8> kSixLoads = {
    0x8B, 0x43, 0x24,   // mov eax,[rbx+0x24]
    0x8B, 0x4B, 0x28,   // mov ecx,[rbx+0x28]
    0x8B, 0x53, 0x2C,   // mov edx,[rbx+0x2c]
    0x8B, 0x73, 0x30,   // mov esi,[rbx+0x30]
    0x8B, 0x7B, 0x34,   // mov edi,[rbx+0x34]
    0x8B, 0x6B, 0x38,   // mov ebp,[rbx+0x38]
};

// ---------------------------------------------------------------------------
// Measurement. Not an assertion -- prints the numbers the design is scored
// against so a size regression is visible in the log even when a gate passes.
// ---------------------------------------------------------------------------
TEST_F(JitMemOoliningTest, ReportEmittedWordsForReferenceInstruction) {
    CompiledBlock* one = compile64(kMovEaxRbx24);
    ASSERT_NE(one, nullptr);
    std::printf("[measure] mov eax,[rbx+0x24]  (1 access) : %u emitted ARM "
                "words\n", one->code_size / 4);
    CompiledBlock* six = compile64(kSixLoads);
    ASSERT_NE(six, nullptr);
    std::printf("[measure] six 32-bit loads    (6 access) : %u emitted ARM "
                "words\n", six->code_size / 4);
}

// ---------------------------------------------------------------------------
// T1 -- code-size gate. TRUE NEGATIVE CONTROL for Stages 2+3.
//
// Upper bounds only: an exact figure would break on every unrelated lowering
// change. Baselines before the change are 87 words (1 access) and 340 words
// (6 accesses); the bounds below sit under both with a little headroom.
//
// Note honestly: on the ONE-access block Stage 2 saves nothing (14 inline
// words become 2 inline + a 12-word shared stub). That block's saving is
// entirely Stage 3's. Stage 2 only shows up in the six-access bound.
// ---------------------------------------------------------------------------
TEST_F(JitMemOoliningTest, SingleMemoryInstructionBlockCodeSize) {
    CompiledBlock* cb = compile64(kMovEaxRbx24);
    ASSERT_NE(cb, nullptr);
    const u32 words = cb->code_size / 4;
    EXPECT_LE(words, 80u)
        << "emitted " << words << " ARM words for a 3-byte guest load; the "
           "memory slow path, the raise stub or the pin spill has grown back "
           "inline";
}

TEST_F(JitMemOoliningTest, SixAccessBlockCodeSize) {
    CompiledBlock* cb = compile64(kSixLoads);
    ASSERT_NE(cb, nullptr);
    const u32 words = cb->code_size / 4;
    EXPECT_LE(words, 220u)
        << "emitted " << words << " ARM words for six 3-byte guest loads; the "
           "per-access raise stub or the 8-register pin spill is back";
}

TEST_F(JitMemOoliningTest, SixAccessSharesOneMemoryHelper) {
    CompiledBlock* cb = compile64(kSixLoads);
    ASSERT_NE(cb, nullptr);
    const std::vector<u32> w = words_of(cb);
    u32 blr = 0;
    for (u32 word : w) {
        if (word == kBlrX16) ++blr;
    }
    EXPECT_LE(blr, 2u)
        << "six same-width loads emitted " << blr
        << " helper BLRs; linear64 cold stubs of one kind share a body";
}

// ---------------------------------------------------------------------------
// T2 -- structural gate. TRUE NEGATIVE CONTROL for Stage 1, and the ONLY one
// Stage 1 admits: it is pure code motion, so no correctness test can fail on
// it.
//
// Before Stage 1 the region fast path ends with `B .done` jumping over an
// inline slow path. After Stage 1 the fast path falls through to the merge
// point and the slow path lives in the block tail. The scalar TSO lowering is
// now itself a guarded ordered/plain diamond, so its final fallback DMB -- not
// the ordered LDAPR/LDAR inside that diamond -- must fall through to merge.
// ---------------------------------------------------------------------------
TEST_F(JitMemOoliningTest, RegionFastPathFallsThroughToMergePoint) {
    CompiledBlock* cb = compile64(kMovEaxRbx24);
    ASSERT_NE(cb, nullptr);
    const std::vector<u32> w = words_of(cb);

    // Exact-address 32-bit region load: X17 = X3 + X4, then the TSO-ordered
    // load through X17.
    //
    // The shape this scans for CHANGED when TSO fault patching became the
    // default (m_tso_fault_patch, constructor_destructor.cpp): the ordered
    // access is no longer a guarded ordered/plain diamond emitted inline. It
    // is now a patch marker plus a single ordered load, and the exact-site
    // fault handler rewrites that pair to [LDR,DMB] only for the rare access
    // that straddles a 16-byte granule. This test kept asserting the old
    // diamond and so failed on a legitimate codegen change -- it was reporting
    // "no guarded exact-address inline region load found" while the region
    // load was right there, in its current form.
    // Note the marker sits AFTER the load and BEFORE the store: the handler
    // rewrites an ordered load to [LDR, DMB] and an ordered store to
    // [DMB, STR], so the marker always occupies the slot the barrier needs.
    usize load_at = w.size();
    for (usize i = 0; i + 2 < w.size(); ++i) {
        if (w[i] == arm64::add_x(17, 3, 4) &&
            is_acquire_load32(w[i + 1], 17) &&
            w[i + 2] == kTsoPatchMarker) {
            load_at = i + 2;
            break;
        }
    }
    ASSERT_LT(load_at, w.size())
        << "no exact-address inline region load found";

    // Stage 1, unchanged in intent: the fast path FALLS THROUGH to the merge
    // point. Before Stage 1 it ended with `B .done` jumping over an inline
    // slow path; after it, the slow path lives in the block tail and nothing
    // branches over anything here.
    const u32 next = w[load_at + 1];
    EXPECT_NE(next & 0xFC000000u, 0x14000000u)
        << "word after the inline TSO load (index " << (load_at + 1)
        << ") is an unconditional B: the region fast path is branching over "
           "an inline slow path again instead of falling through";
}


TEST_F(JitMemOoliningTest, RegionStoreAvoidsTemporaryForOrdinaryRegisterPair) {
    CompiledBlock* cb = compile64(kMovRbx24Eax);
    ASSERT_NE(cb, nullptr);
    const std::vector<u32> w = words_of(cb);

    // RAX is pinned in X23. The old generic marshal always staged it through
    // X17 before copying it to the store-value register X10, adding one hot
    // host instruction to every x64 region store. This operand pairing is not
    // the X9/X10 swap case and must use the direct two-move path.
    const u32 old_stage = arm64::mov_x(17, 23);
    EXPECT_EQ(std::find(w.begin(), w.end(), old_stage), w.end())
        << "ordinary x64 store value was unnecessarily staged through X17";
}

// ---------------------------------------------------------------------------
// T3 -- ExceptionAddress at a mid-block fault above 4 GB.
//
// Three 64-bit instructions; the SECOND faults. The raise must be attributed
// to instruction 2's RIP, not to block start and not to instruction 1.
// args[1] carries the full 64-bit guest address, which also re-catches the
// 32-bit-MOV EA truncation shape.
//
// REGRESSION TEST: cannot fail before the change.
// ---------------------------------------------------------------------------
TEST_F(JitMemOoliningTest, MidBlockLoadFaultReportsRipAndAddress) {
    // mov eax,[rbx+0x24]      ; valid   (3 bytes)
    // mov ecx,[rdx]           ; faults  (2 bytes)
    // mov esi,[rbx+0x28]      ; never reached
    std::vector<u8> code = {0x8B, 0x43, 0x24,
                            0x8B, 0x0A,
                            0x8B, 0x73, 0x28};
    CompiledBlock* cb = compile64(code);
    ASSERT_NE(cb, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RBX, kData);
    cpu.gpr.set64(RDX, kUnmapped);
    (void)m_jit->execute(*cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "unmapped high load did not raise";
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 0u) << "should be a read, not a write";
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped)
        << "faulting address truncated or wrong";
    EXPECT_EQ(cpu.rip, kCodeBase + 3)
        << "ExceptionAddress must be instruction 2, not block start (0x"
        << std::hex << kCodeBase << ") or instruction 1";
}

TEST_F(JitMemOoliningTest, MidBlockStoreFaultReportsRipAndAddress) {
    // mov eax,[rbx+0x24]      ; valid
    // mov [rdx],ecx           ; faults  (2 bytes)
    // mov esi,[rbx+0x28]
    std::vector<u8> code = {0x8B, 0x43, 0x24,
                            0x89, 0x0A,
                            0x8B, 0x73, 0x28};
    CompiledBlock* cb = compile64(code);
    ASSERT_NE(cb, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.gpr.set64(RBX, kData);
    cpu.gpr.set64(RDX, kUnmapped);
    (void)m_jit->execute(*cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u) << "should be a write";
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped);
    EXPECT_EQ(cpu.rip, kCodeBase + 3);
}

TEST_F(JitMemOoliningTest, MidBlockFaultPreservesUpperCanonicalRipBits) {
    constexpr GuestAddr kUpperCodeBase = 0xFFFF800014000000ull;
    // mov eax,[rbx+0x24] ; valid
    // mov ecx,[rdx]      ; faults
    const std::vector<u8> code = {0x8B, 0x43, 0x24, 0x8B, 0x0A};
    BasicBlock block = build_block(code, true);
    block.start_addr = kUpperCodeBase;
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    CpuState cpu;
    init_cpu(cpu);
    cpu.rip = kUpperCodeBase;
    cpu.gpr.set64(RBX, kData);
    cpu.gpr.set64(RDX, kUnmapped);
    (void)m_jit->execute(*cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[1], kUnmapped);
    EXPECT_EQ(cpu.rip, kUpperCodeBase + 3)
        << "fault RIP lost upper canonical address bits";
}

// ---------------------------------------------------------------------------
// T4 -- the whole guest register file at a mid-block fault.
//
// This is the test that catches Stage 3 going wrong: if a pin that the C ABI
// was assumed to preserve is in fact clobbered, or if the narrowed spill
// misses RSP/RBP, one of these sixteen comparisons fails.
//
// Every seed value has live bits 63:32. A test using 32-bit values would pass
// against a spill that zero-extends, which is precisely the recorded pin-width
// landmine.
//
// REGRESSION TEST.
// ---------------------------------------------------------------------------
TEST_F(JitMemOoliningTest, MidBlockFaultPreservesAllSixteenGprs) {
    // Each of the three instructions before the fault leaves a pin holding
    // something its CpuState slot does not, which is the only state in which
    // a missing or wrong spill is observable at all.
    //
    // mov eax,[rbx+0x24]   ; mutates a callee-saved pin (RAX->X23)
    // add rdi, rsi         ; mutates a second, 64-bit    (48 01 F7)
    // add rbp, rsi         ; mutates a CALLER-saved pin  (48 01 F5)
    // mov ecx,[rdx]        ; faults
    // mov edx,[rbx+0x28]   ; never reached
    std::vector<u8> code = {0x8B, 0x43, 0x24,
                            0x48, 0x01, 0xF7,
                            0x48, 0x01, 0xF5,
                            0x8B, 0x0A,
                            0x8B, 0x53, 0x28};

    const auto seed = [](CpuState& cpu) {
        // Distinct, all with non-zero high halves. RSP/RBP stay in the mapped
        // stack so nothing else faults; their high halves are live regardless.
        cpu.gpr.set64(RAX, 0xAAAAAAAA00000001ull);
        cpu.gpr.set64(RCX, 0xAAAAAAAA00000002ull);
        cpu.gpr.set64(RSI, 0xAAAAAAAA00000006ull);
        cpu.gpr.set64(RDI, 0xAAAAAAAA00000007ull);
        cpu.gpr.set64(RBP, 0xAAAAAAAA00000005ull);
        for (u8 i = 8; i < 16; ++i) {
            cpu.gpr.set64(i, 0xBBBBBBBB00000000ull | i);
        }
        cpu.gpr.set64(RBX, kData);
        cpu.gpr.set64(RDX, kUnmapped);
    };

    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, true);
    ASSERT_EQ(block.instruction_count, 5u);

    // Interpreter reference.
    CpuState icpu;
    init_cpu(icpu);
    seed(icpu);
    ExecResult ires;
    {
        Interpreter interp(icpu, *m_memory);
        ires = interp.run(block.instruction_count);
    }
    ASSERT_TRUE(ires == ExecResult::Exception || icpu.pending_raise.active);

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    CpuState jcpu;
    init_cpu(jcpu);
    seed(jcpu);
    (void)m_jit->execute(*cb, jcpu);
    ASSERT_TRUE(jcpu.pending_raise.active);

    for (u8 i = 0; i < 16; ++i) {
        EXPECT_EQ(icpu.gpr.get64(i), jcpu.gpr.get64(i))
            << "GPR " << static_cast<unsigned>(i)
            << " diverged at fault time: interp=0x" << std::hex
            << icpu.gpr.get64(i) << " jit=0x" << jcpu.gpr.get64(i);
    }
    EXPECT_EQ(jcpu.rip, kCodeBase + 9) << "fault attributed to wrong RIP";
}

// ---------------------------------------------------------------------------
// Pin-spill correctness and width, 64-bit block.
//
// RSP->X12 and RBP->X13 are the two pins in AAPCS64's caller-saved temporary
// class, so they are the only two Stage 3 still spills around a memory helper.
//
// CRUCIAL: both must be MODIFIED IN-BLOCK before the faulting access. A test
// that only seeds them passes even against a missing spill, because the
// post-call `emit_pin_reload_one` reads CpuState -- which still holds the
// block-entry value -- and hands back something that happens to be right. The
// bug is only observable once the pin and CpuState disagree. (Verified: the
// seed-only version of this test did not catch dropping the RBP spill.)
//
// Every value keeps live bits 63:32; a 32-bit-only assertion would pass
// against a spill that zero-extends, which is the recorded pin-width landmine.
// ---------------------------------------------------------------------------
TEST_F(JitMemOoliningTest, NarrowedSpillPreservesModifiedRspRbp64) {
    // add rbp, rbx    ; 48 01 DD -- RBP now differs from its CpuState slot
    // sub rsp, 0x20   ; 48 83 EC 20 -- so does RSP
    // mov ecx,[rdx]   ; faults, taking the memory helper slow path
    std::vector<u8> code = {0x48, 0x01, 0xDD,
                            0x48, 0x83, 0xEC, 0x20,
                            0x8B, 0x0A};

    const u64 seed_rbp = 0xCCCCCCCC12345678ull;
    const u64 seed_rbx = 0x1111111100000011ull;
    const u64 seed_rsp = kStack + kStackSize - 0x100;

    const auto seed = [&](CpuState& cpu) {
        cpu.gpr.set64(RBP, seed_rbp);
        cpu.gpr.set64(RBX, seed_rbx);
        cpu.gpr.set64(RSP, seed_rsp);
        cpu.gpr.set64(RDX, kUnmapped);
    };

    ASSERT_TRUE(m_memory->write_bytes(kCodeBase, code.data(), code.size()));
    BasicBlock block = build_block(code, true);
    ASSERT_EQ(block.instruction_count, 3u);

    CpuState icpu;
    init_cpu(icpu);
    seed(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(block.instruction_count);
    }

    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    CpuState cpu;
    init_cpu(cpu);
    seed(cpu);
    (void)m_jit->execute(*cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active);
    // Independent of the interpreter, so a shared misunderstanding cannot
    // make both sides agree on a wrong answer.
    EXPECT_EQ(cpu.gpr.get64(RBP), seed_rbp + seed_rbx)
        << "RBP was not spilled around the memory helper, or lost bits 63:32";
    EXPECT_EQ(cpu.gpr.get64(RSP), seed_rsp - 0x20)
        << "RSP was not spilled around the memory helper";
    EXPECT_EQ(icpu.gpr.get64(RBP), cpu.gpr.get64(RBP));
    EXPECT_EQ(icpu.gpr.get64(RSP), cpu.gpr.get64(RSP));
}

// ---------------------------------------------------------------------------
// 32-bit twin -- pin-spill WIDTH under the shared raise-exit stub.
//
// Scope note, stated precisely because it is easy to get wrong:
//
//   Stage 1 (out-of-lining) and Stage 3 (narrowed spill) are 64-BIT ONLY.
//   emit_memory_helper_blr() is called only from emit_x64_region_load/store,
//   which a 32-bit block never reaches. So there is nothing about Stage 3 to
//   test in 32-bit mode.
//
//   Stage 2 (the shared raise-exit stub) applies to BOTH bitnesses -- the
//   32-bit legacy region paths have their own emit_exit_if_pending_raise
//   sites. That stub reaches CpuState through emit_store_pins, which carries
//   the per-bitness width rule:
//
//     64-bit block: the pin owns all 64 bits -> str_x.
//     32-bit block: the pin owns bits 31:0 ONLY; bits 63:32 live in CpuState
//                   and were never in the pin, so the spill must be a RAW
//                   32-bit store. emit_store_gpr32_mem would apply x86-64
//                   zero-extension and blank them.
//
// This test seeds bits 63:32 of a pin in CpuState, faults inside a 32-bit
// block so the shared stub runs, and requires those bits to survive.
// ---------------------------------------------------------------------------
class JitMemOolining32Test : public ::testing::Test {
protected:
    static constexpr GuestAddr kCode32 = 0x00400000ull;
    static constexpr GuestAddr kStack32 = 0x00500000ull;
    static constexpr GuestAddr kUnmapped32 = 0x7FFF0000ull;

    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory->map_region(kCode32, 0x4000,
                                         MemoryProtection::ReadWriteExecute));
        ASSERT_TRUE(m_memory->map_region(kStack32, 0x4000,
                                         MemoryProtection::ReadWrite));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        m_cache = std::make_unique<BlockCache>();
        m_jit->set_block_cache(m_cache.get());
    }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
    std::unique_ptr<BlockCache> m_cache;
};

TEST_F(JitMemOolining32Test, SharedRaiseStubKeepsPinHighHalvesIn32BitBlock) {
    // mov ecx,[edx]   ; faults -- 8B 0A, same encoding in 32-bit mode
    std::vector<u8> code = {0x8B, 0x0A};
    ASSERT_TRUE(m_memory->write_bytes(kCode32, code.data(), code.size()));

    BasicBlock block;
    block.start_addr = kCode32;
    block.is_64bit = false;
    {
        MemoryByteReader reader(code.data(), code.size());
        DecodedInstruction inst;
        ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode32, inst),
                  DecodeError::Success);
        block.instructions.push_back(inst);
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count = 1;
    }
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);

    CpuState cpu;
    cpu.reset();
    cpu.rip = kCode32;
    cpu.gpr.set64(RSP, kStack32 + 0x3F00);
    cpu.gpr.set64(RDX, kUnmapped32);
    // Bits 63:32 that a 32-bit block keeps in CpuState and never puts in the
    // pin. A zero-extending spill blanks exactly these.
    const u64 hi = 0x99999999ull << 32;
    cpu.gpr.set64(RBX, hi | 0x0000BEEFull);
    cpu.gpr.set64(RSI, hi | 0x0000CAFEull);
    cpu.gpr.set64(RBP, hi | 0x0000F00Dull);

    (void)m_jit->execute(*cb, cpu);

    ASSERT_TRUE(cpu.pending_raise.active) << "32-bit block did not raise";
    EXPECT_EQ(cpu.gpr.get64(RBX), hi | 0x0000BEEFull)
        << "RBX bits 63:32 blanked by the raise stub's pin spill";
    EXPECT_EQ(cpu.gpr.get64(RSI), hi | 0x0000CAFEull)
        << "RSI bits 63:32 blanked by the raise stub's pin spill";
    EXPECT_EQ(cpu.gpr.get64(RBP), hi | 0x0000F00Dull)
        << "RBP bits 63:32 blanked by the raise stub's pin spill";
}

}  // namespace
