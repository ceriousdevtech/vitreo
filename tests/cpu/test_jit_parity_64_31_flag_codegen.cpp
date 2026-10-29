// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): flag producers,
// consumers and constant codegen.

#include "jit_parity_64_fixture.hpp"

// A 32-bit write to a register held in CpuState (R8-R15) zero-extends into
// the full 64-bit slot (Intel SDM Vol. 1 3.4.1.1). It is one STP of the value
// and WZR, not a store of the value plus a separate store of zero: every
// 32-bit write to R8-R15 pays it, and compiled x64 code writes them
// constantly.
TEST_F(JitParity64Test, Unpinned32BitWriteIsOneZeroExtendingPair) {
    const std::array<u8, 3> mov_r8d_eax{0x41, 0x89, 0xC0};  // mov r8d, eax
    BasicBlock block = build_block(mov_r8d_eax.data(), mov_r8d_eax.size());
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, mov_r8d_eax.data(),
                                      mov_r8d_eax.size()));
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    const u32 slot = static_cast<u32>(offsetof(CpuState, gpr) + 8u * R8);
    const u32 pair = arm64::stp_w_off(0, 31, 19, 0) & ~0x003FFC1Fu;  // opcode
    usize pairs = 0;
    usize high_zero_stores = 0;
    const auto* words = static_cast<const u32*>(cb->code);
    for (u32 i = 0; i < cb->code_size / 4u; ++i) {
        const u32 w = words[i];
        if ((w & ~0x003FFC1Fu) == pair && ((w >> 10) & 31u) == 31u &&
            ((w >> 5) & 31u) == 19u && ((w >> 15) & 0x7Fu) == slot / 4u) {
            ++pairs;
        }
        if (w == arm64::str_w_uoff(31, 19, (slot + 4u) / 4u)) {
            ++high_zero_stores;
        }
    }
    EXPECT_EQ(pairs, 1u) << "the write is not one STP Wt, WZR into R8's slot";
    EXPECT_EQ(high_zero_stores, 0u) << "a separate store zeroes R8's high half";

    // And it still zero-extends: R8's stale high half must not survive.
    check_parity(mov_r8d_eax.data(), mov_r8d_eax.size(), [](CpuState& cpu) {
        cpu.gpr.set64(RAX, 0x1122334455667788ull);
        cpu.gpr.set64(R8, 0xFFFFFFFFFFFFFFFFull);
    });
}

// A 32-bit constant is one MOVZ or MOVN whenever one 16-bit half is 0x0000
// or 0xFFFF (Arm ARM C6.2: MOVN writes the complement of the shifted
// immediate), so small negative immediates -- `cmp r32, -100`,
// `add r32, -8` -- do not take a MOVZ+MOVK pair.
TEST_F(JitParity64Test, ThirtyTwoBitConstantsUseOneMoveWideWhenAHalfIsTrivial) {
    // mov r8d, imm32 (41 B8 id): R8 is not pinned, so the constant is
    // materialised into a scratch register and stored. Counted against the
    // same block with imm 0, so the block's own constants cancel out.
    const auto move_wides = [&](u32 value) -> usize {
        const std::array<u8, 6> code{
            0x41, 0xB8, static_cast<u8>(value), static_cast<u8>(value >> 8),
            static_cast<u8>(value >> 16), static_cast<u8>(value >> 24)};
        EXPECT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        BasicBlock block = build_block(code.data(), code.size());
        EXPECT_EQ(block.instruction_count, 1u);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        EXPECT_NE(cb, nullptr);
        if (cb == nullptr) return 0;
        usize n = 0;
        const auto* words = static_cast<const u32*>(cb->code);
        for (u32 i = 0; i < cb->code_size / 4u; ++i) {
            const u32 top = words[i] & 0xFF800000u;  // sf=0 MOVN/MOVZ/MOVK
            if (top == 0x12800000u || top == 0x52800000u || top == 0x72800000u) {
                ++n;
            }
        }
        return n;
    };
    // 1 is exactly one MOVZ on every tier; 0 is not (Tier 1 binds it to WZR).
    const usize base = move_wides(1);
    struct Case { u32 value; usize words; };
    for (const Case c : {Case{0xFFFFFF9Cu, 1}, Case{0x00010000u, 1},
                         Case{0x7FFFFFFFu, 1}, Case{0xFFFF0000u, 1},
                         Case{0x0000BEEFu, 1}, Case{0x12345678u, 2}}) {
        SCOPED_TRACE(::testing::Message() << std::hex << c.value);
        EXPECT_EQ(move_wides(c.value), base - 1 + c.words);
        const std::array<u8, 6> code{
            0x41, 0xB8, static_cast<u8>(c.value), static_cast<u8>(c.value >> 8),
            static_cast<u8>(c.value >> 16), static_cast<u8>(c.value >> 24)};
        check_parity(code.data(), code.size(), [](CpuState& cpu) {
            cpu.gpr.set64(R8, 0xAAAAAAAAAAAAAAAAull);
        });
    }
}

// MOVSD/MOVLPS/MOVLPD/MOVHPS/MOVHPD m64, xmm store one quadword (Intel SDM
// Vol. 2). They were two 32-bit guest stores, each with its own guard and
// STLR, so a store straddling into an unmapped page wrote its low half before
// faulting. One 64-bit store is both the cheaper lowering and the x86 one.
namespace {
struct QwordStoreForm { const char* name; std::array<u8, 4> bytes; usize len; };
const QwordStoreForm kQwordStores[] = {
    {"movsd [rax], xmm1", {0xF2, 0x0F, 0x11, 0x08}, 4},
    {"movlps [rax], xmm1", {0x0F, 0x13, 0x08, 0x00}, 3},
    {"movlpd [rax], xmm1", {0x66, 0x0F, 0x13, 0x08}, 4},
    {"movhps [rax], xmm1", {0x0F, 0x17, 0x08, 0x00}, 3},
    {"movhpd [rax], xmm1", {0x66, 0x0F, 0x17, 0x08}, 4},
};
}  // namespace

TEST_F(JitParity64Test, SseQwordStoresAreOneSixtyFourBitStore) {
    for (const QwordStoreForm& form : kQwordStores) {
        SCOPED_TRACE(form.name);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, form.bytes.data(), form.len));
        BasicBlock block = build_block(form.bytes.data(), form.len);
        ASSERT_EQ(block.instruction_count, 1u);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        usize stlr_x = 0, stlr_w = 0;
        const auto* words = static_cast<const u32*>(cb->code);
        for (u32 i = 0; i < cb->code_size / 4u; ++i) {
            if ((words[i] & 0xFFFFFC00u) == 0xC89FFC00u) ++stlr_x;
            if ((words[i] & 0xFFFFFC00u) == 0x889FFC00u) ++stlr_w;
        }
        EXPECT_EQ(stlr_x, 1u);
        EXPECT_EQ(stlr_w, 0u);
    }
}

TEST_F(JitParity64Test, SseQwordStoresMatchInterpreterIncludingPageStraddle) {
    m_compare_memory = true;
    for (const QwordStoreForm& form : kQwordStores) {
        // In bounds, and straddling the end of the mapping (0x1000..0x11000):
        // the second page is unmapped, so the store must fault with neither
        // half written.
        for (const GuestAddr at : {GuestAddr{0x2008}, GuestAddr{0x10FFC}}) {
            SCOPED_TRACE(::testing::Message() << form.name << " @0x"
                                              << std::hex << at);
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            ASSERT_TRUE(m_memory->write_u32(0x10FFC, 0x5A5A5A5Au));
            ASSERT_TRUE(m_memory->write_u64(0x2008, 0xA5A5A5A5A5A5A5A5ull));
            ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
            check_parity(
                form.bytes.data(), form.len,
                [at](CpuState& cpu) {
                    cpu.gpr.set64(RAX, at);
                    cpu.sse.xmm[1].u64_[0] = 0x0123456789ABCDEFull;
                    cpu.sse.xmm[1].u64_[1] = 0xFEDCBA9876543210ull;
                }, true, true, /*require_jit=*/true);
        }
    }
}

// A full-flag writer whose memory operand faults has not written any flag yet
// (Intel SDM Vol. 3A 6.6: a fault reports the state BEFORE the instruction),
// so the exception context must carry the PREVIOUS writer's flags. The
// in-block omission pass may only retire the earlier lazy record across a
// killer that cannot fault first.
TEST_F(JitParity64Test, FaultingFullWriterKeepsThePriorWritersFlags) {
    struct Form { const char* name; std::array<u8, 6> bytes; usize len; };
    static constexpr Form kForms[] = {
        // add eax, 1 ; cmp dword [rbx], 0
        {"cmp m32, imm8", {0x83, 0xC0, 0x01, 0x83, 0x3B, 0x00}, 6},
        // add eax, 1 ; comiss xmm0, [rbx]
        {"comiss xmm0, m32", {0x83, 0xC0, 0x01, 0x0F, 0x2F, 0x03}, 6},
        // add eax, 1 ; ucomisd xmm0, [rbx]
        {"ucomisd xmm0, m64", {0x83, 0xC0, 0x01, 0x66, 0x0F, 0x2E}, 6},
    };
    for (const Form& form : kForms) {
        SCOPED_TRACE(form.name);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        std::array<u8, 8> code{};
        std::memcpy(code.data(), form.bytes.data(), form.len);
        usize len = form.len;
        if (form.bytes[3] == 0x66) code[len++] = 0x03;  // ModRM [rbx]
        ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        check_parity(code.data(), len, [](CpuState& cpu) {
            // ADD sets ZF, CF and PF; RBX is past the end of the mapping.
            cpu.gpr.set64(RAX, 0xFFFFFFFFull);
            cpu.gpr.set64(RBX, 0x11000);
            cpu.flags.rflags = 0x2;
        }, true, false, /*require_jit=*/true, /*check_af=*/true);
        EXPECT_TRUE(m_last_interp_cpu.flags.zf()) << "the ADD must have run";
    }
}

namespace {

// Whether the block's code reads CpuState's lazy-flag kind byte: an LDRB
// Wt, [X19, #flag_op_offset] (unsigned offset). A Jcc that fuses from host
// NZCV never needs it; the kind-dispatch walk always does.
bool reads_lazy_kind_byte(const CompiledBlock* cb) {
    const auto* words = static_cast<const u32*>(cb->code);
    const u32 want = 0x39400000u | (flag_op_offset() << 10) |
                     (static_cast<u32>(REG_CPU_STATE) << 5);
    for (usize i = 0; i < cb->code_size / sizeof(u32); ++i) {
        if ((words[i] & 0xFFFFFFE0u) == want) return true;
    }
    return false;
}

}  // namespace

// TEST r/m8 and r/m16 leave host NZCV the way TEST r/m32 does: the byte or
// word result in bits 31:24 / 31:16 gives that width's SF and ZF, and TST
// clears C and V, TEST's CF and OF (Intel SDM Vol. 2 TEST). Every Jcc after
// one must branch exactly as the interpreter does.
TEST_F(JitParity64Test, NarrowTestFeedsEveryJccLikeTheInterpreter) {
    const std::vector<std::vector<u8>> forms = {
        {0x84, 0xD8},              // TEST AL, BL
        {0xA8, 0x81},              // TEST AL, 0x81
        {0x84, 0xFC},              // TEST AH, BH
        {0x66, 0x85, 0xD8},        // TEST AX, BX
        {0x66, 0xA9, 0x01, 0x80},  // TEST AX, 0x8001
    };
    const u64 values[][2] = {
        {0x0000, 0x0000}, {0x8080, 0x8080}, {0x7F7F, 0x8080},
        {0xFFFF, 0x0101}, {0x8000, 0x8000}, {0x7FFF, 0x8001},
        {0x8001, 0xFFFF}, {0x0100, 0x00FF},
    };
    for (const auto& form : forms) {
        for (u8 cc = 0; cc < 16; ++cc) {
            for (const auto& v : values) {
                std::vector<u8> code = form;
                code.push_back(static_cast<u8>(0x70 | cc));  // Jcc rel8 +2
                code.push_back(0x02);
                code.push_back(0x90);
                code.push_back(0x90);
                SCOPED_TRACE(::testing::Message()
                             << "form " << std::hex << +form[0] << " cc "
                             << +cc << " a " << v[0] << " b " << v[1]);
                // A fresh compiler per case: rewriting 0x1000 under one would
                // trip its self-modifying-code recompile limit.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity(code.data(), code.size(),
                    [&](CpuState& cpu) {
                        cpu.gpr.set64(RAX, 0x1234567800000000ull | v[0]);
                        cpu.gpr.set64(RBX, 0x00ABCDEF00000000ull | v[1]);
                    },
                    /*check_flags=*/true, /*check_xmm=*/false,
                    /*require_jit=*/true);
            }
        }
    }
}

// ...and so the Jcc branches on those flags directly instead of walking the
// lazy record's kind byte. Register and memory forms both.
TEST_F(JitParity64Test, NarrowTestThenJccBranchesOnHostFlags) {
    constexpr GuestAddr data_address = 0x14002A000ull;
    map_tracked_region(data_address, 0x1000);
    const std::vector<std::vector<u8>> blocks = {
        {0x84, 0xC9, 0x75, 0x02},              // TEST CL, CL; JNZ
        {0xF6, 0x00, 0x01, 0x74, 0x02},        // TEST byte [RAX], 1; JZ
        {0x66, 0x85, 0xD2, 0x78, 0x02},        // TEST DX, DX; JS
    };
    for (const auto& code : blocks) {
        SCOPED_TRACE(::testing::Message() << "first byte " << std::hex
                                          << +code[0]);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        BasicBlock block = build_block(code.data(), code.size());
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr);
        ASSERT_NE(cb->code, nullptr);
        EXPECT_FALSE(reads_lazy_kind_byte(cb));
    }
}

// LOCK CMPXCHG leaves host NZCV as CMP accumulator, destination -- the
// native CASAL path and the helper path alike -- so the CAS loop's Jcc
// branches on them. Match and mismatch; one test per operand size, since a
// shorter encoding rewritten over a longer one at 0x1000 inside one fixture
// does not replay cleanly.
#define VITREO_LOCK_CMPXCHG_JCC_CASES(...)                                     \
    constexpr GuestAddr data_address = 0x14002B000ull;                         \
    map_tracked_region(data_address, 0x1000);                                  \
    const std::vector<u8> form = {__VA_ARGS__};                                \
    for (u8 cc : {u8{0x4}, u8{0x5}, u8{0x2}, u8{0x7}, u8{0xC}, u8{0x8}}) {     \
        for (const bool match : {true, false}) {                               \
            std::vector<u8> code = form;                                       \
            code.push_back(static_cast<u8>(0x70 | cc));                        \
            code.push_back(0x02);                                              \
            code.push_back(0x90);                                              \
            code.push_back(0x90);                                              \
            ASSERT_TRUE(m_memory->write_u64(data_address,                      \
                                            0x8000000000000005ull));           \
            SCOPED_TRACE(::testing::Message() << "cc " << +cc                  \
                                              << " match " << match);          \
            m_jit = std::make_unique<JitCompiler>(*m_memory);                  \
            check_parity(code.data(), code.size(),                             \
                [&](CpuState& cpu) {                                           \
                    cpu.gpr.set64(RBX, data_address);                          \
                    cpu.gpr.set64(RAX, match ? 0x8000000000000005ull           \
                                             : 0x7000000000000009ull);         \
                    cpu.gpr.set64(RCX, 0x1111111122222222ull);                 \
                },                                                             \
                /*check_flags=*/true, /*check_xmm=*/false,                     \
                /*require_jit=*/true);                                         \
        }                                                                      \
    }                                                                          \
    std::vector<u8> code = form;                                               \
    code.push_back(0x75);                                                      \
    code.push_back(0x02);                                                      \
    ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));     \
    m_jit = std::make_unique<JitCompiler>(*m_memory);                          \
    BasicBlock block = build_block(code.data(), code.size());                  \
    CompiledBlock* cb = m_jit->compile(block);                                 \
    ASSERT_NE(cb, nullptr);                                                    \
    ASSERT_NE(cb->code, nullptr);                                              \
    EXPECT_FALSE(reads_lazy_kind_byte(cb))

TEST_F(JitParity64Test, LockCmpxchg64FeedsJccFromHostFlags) {
    VITREO_LOCK_CMPXCHG_JCC_CASES(0xF0, 0x48, 0x0F, 0xB1, 0x0B);  // [RBX], RCX
}

TEST_F(JitParity64Test, LockCmpxchg32FeedsJccFromHostFlags) {
    VITREO_LOCK_CMPXCHG_JCC_CASES(0xF0, 0x0F, 0xB1, 0x0B);  // [RBX], ECX
}

TEST_F(JitParity64Test, LockCmpxchg8FeedsJccFromHostFlags) {
    VITREO_LOCK_CMPXCHG_JCC_CASES(0xF0, 0x0F, 0xB0, 0x0B);  // [RBX], CL
}

#undef VITREO_LOCK_CMPXCHG_JCC_CASES

// INC/DEC keep CF, so at a block start -- where the lazy record's kind is not
// known when the block is compiled -- they read it out of whatever record a
// predecessor left. Every kind CpuState::lazy_cf() distinguishes, and the
// CLC/STC overlay on top, must give the interpreter's CF.
TEST_F(JitParity64Test, IncDecCarryFromAnUnknownRecordMatchesTheInterpreter) {
    using F = CpuState::FlagOp;
    struct Rec { F op; u64 src; u64 dst; u8 overlay; };
    const Rec records[] = {
        {F::Add32, 5, 3, 0},            {F::Add32, 1, 2, 0},
        {F::Sub32, 5, 0xFFFFFFFE, 0},   {F::Sub32, 1, 4, 0},
        {F::Add64, 5, 3, 0},            {F::Add64, 1, 2, 0},
        {F::Sub64, 5, ~u64{2}, 0},      {F::Sub64, 1, 4, 0},
        {F::And64, 7, 0, 0},            {F::Or32, 7, 7, 0},
        {F::Xor8, 1, 1, 0},             {F::And16, 3, 3, 0},
        {F::Inc32, u64{1} << 32, 5, 0}, {F::Dec64, 0, 5, 0},
        {F::Inc8, u64{1} << 32, 0, 0},  {F::Dec16, u64{1} << 32, 1, 0},
        {F::Adc32, (u64{1} << 32) | 0xFFFFFFFFu, 0xFFFFFFFFu, 0},
        {F::Sbb32, (u64{1} << 32) | 1, 0xFFFFFFFFu, 0},
        {F::Mul32, 0, 7, 0},            {F::Shl32, 1, 0x80000000u, 0},
        {F::Neg64, 5, ~u64{4}, 0},      {F::Sub8, 0x10, 0xF0, 0},
        {F::Add64, 1, 2, 1},            {F::Sub64, 1, 4, 2},
        {F::None, 0, 0, 0},
    };
    const std::vector<std::vector<u8>> forms = {
        {0xFF, 0xC0},        // INC EAX
        {0x48, 0xFF, 0xC9},  // DEC RCX
        {0xFE, 0xC2},        // INC DL
    };
    for (const auto& form : forms) {
        for (const Rec& r : records) {
            for (const bool rflags_cf : {false, true}) {
                SCOPED_TRACE(::testing::Message()
                             << "form " << std::hex << +form[0] << " op "
                             << static_cast<int>(r.op) << " src " << r.src
                             << " dst " << r.dst << " overlay " << +r.overlay
                             << " rflags.cf " << rflags_cf);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity(form.data(), form.size(),
                    [&](CpuState& cpu) {
                        cpu.flags.rflags = 0x2 | (rflags_cf ? 1u : 0u);
                        cpu.set_flag_op(r.op);
                        cpu.flag_src = r.src;
                        cpu.flag_dst = r.dst;
                        cpu.cf_overlay = r.overlay;
                        cpu.gpr.set64(RAX, 0x7FFFFFFF);
                        cpu.gpr.set64(RCX, 0x8000000000000000ull);
                        cpu.gpr.set64(RDX, 0xFF);
                    },
                    /*check_flags=*/true, /*check_xmm=*/false,
                    /*require_jit=*/true);
            }
        }
    }
}

// ...and so it reads that record through the one shared carry-capture stub,
// not an inline walk over the kind byte (which cost ~90 words per INC).
TEST_F(JitParity64Test, IncAtABlockStartCapturesCarryOutOfLine) {
    const std::vector<std::vector<u8>> forms = {
        {0xFF, 0xC0},        // INC EAX
        {0x48, 0xFF, 0xC9},  // DEC RCX
    };
    for (const auto& code : forms) {
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        BasicBlock block = build_block(code.data(), code.size());
        CompiledBlock* cb = m_jit->compile(block);
        ASSERT_NE(cb, nullptr);
        ASSERT_NE(cb->code, nullptr);
        EXPECT_FALSE(reads_lazy_kind_byte(cb)) << std::hex << +code[0];
    }
}

// A CMP / forward-JNE / CMP / SETB lexicographic compare (two-key ordered-map
// probe) and a CMP/SETL/CMOVL/CMOVE three-way compare, decoded as the run loop
// decodes them (forward Jccs absorbed into the trace). Whatever leg leaves the
// compiled block, the interpreter finishes the trace from CpuState, so a flag
// record lost on a taken mid-block exit shows up as a wrong SETB/CMOV result.
// Intel SDM Vol. 2A CMP: flags as SUB; Vol. 2B SETcc/CMOVcc: B = CF, L = SF!=OF,
// E = ZF.
TEST_F(JitParity64Test, TraceWithAbsorbedJccFeedsSetccAndCmovFromTheRightCmp) {
    struct Case {
        const char* name;
        std::vector<u8> bytes;
        std::function<void(CpuState&, GuestMemory&, u64, u64)> setup;
    };
    constexpr GuestAddr kData = 0x8000;
    auto w64 = [](GuestMemory& m, GuestAddr a, u64 v) {
        ASSERT_TRUE(m.write_bytes(a, reinterpret_cast<const u8*>(&v), 8));
    };
    const std::vector<Case> cases = {
        {"map probe (size, ptr) < (rdx, r8)",
         {0x4c, 0x8b, 0x07, 0x48, 0x8b, 0x57, 0x08, 0x48, 0x89, 0xd1, 0x48, 0x29,
          0xd9, 0x4d, 0x8d, 0x0c, 0x18, 0x4c, 0x89, 0x08, 0x48, 0x89, 0x48, 0x08,
          0x89, 0x68, 0x10, 0x48, 0x89, 0x44, 0x24, 0x28, 0x85, 0xed, 0x0f, 0x85,
          0xe2, 0x0a, 0x00, 0x00, 0x4c, 0x8d, 0x76, 0x58, 0x48, 0x8b, 0x46, 0x58,
          0x48, 0x85, 0xc0, 0x0f, 0x84, 0xa3, 0x0a, 0x00, 0x00, 0x49, 0x89, 0xc1,
          0x4c, 0x89, 0xf1, 0x4d, 0x8b, 0x51, 0x20, 0x49, 0x39, 0x52, 0x08, 0x75,
          0x03, 0x4d, 0x39, 0x02, 0x41, 0x0f, 0x92, 0xc2, 0x45, 0x0f, 0xb6, 0xd2,
          0x45, 0x84, 0xd2, 0x49, 0x0f, 0x44, 0xc9, 0x4f, 0x8b, 0x0c, 0xd1, 0x4d,
          0x85, 0xc9, 0x75, 0xdb},
         [&](CpuState& cpu, GuestMemory& m, u64 a, u64 b) {
             // rdi -> {ptr, size} query; rsi -> map with root at +0x58;
             // root node: children 0, key -> {a_ptr, a_size}.
             const u64 q_ptr = 0x5000, q_size = 0x40;
             w64(m, kData + 0x000, q_ptr);
             w64(m, kData + 0x008, q_size);
             cpu.gpr.set64(RDI, kData);
             cpu.gpr.set64(RSI, kData + 0x100);
             cpu.gpr.set64(RAX, kData + 0x400);
             cpu.gpr.set64(RBX, 0x10);
             cpu.gpr.set64(RBP, 0);
             w64(m, kData + 0x158, kData + 0x200);
             w64(m, kData + 0x200, 0);
             w64(m, kData + 0x208, 0);
             w64(m, kData + 0x220, kData + 0x300);
             w64(m, kData + 0x300, q_ptr + a - 1);   // node key ptr
             w64(m, kData + 0x308, q_size + b - 1);  // node key size
         }},
        {"three-way compare cmp/setl/cmovl/cmove",
         {0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0xce,
          0x0f, 0xb7, 0x49, 0x08, 0xf6, 0xc1, 0x01, 0x0f, 0x85, 0xb3, 0x00, 0x00,
          0x00, 0x66, 0x85, 0xc9, 0x0f, 0x88, 0xb4, 0x00, 0x00, 0x00, 0x89, 0xcf,
          0xc1, 0xef, 0x05, 0x39, 0xd7, 0x89, 0xd3, 0x0f, 0x4c, 0xdf, 0x31, 0xc0,
          0x85, 0xd2, 0x0f, 0x48, 0xd8, 0x29, 0xdf, 0x44, 0x39, 0xc7, 0x41, 0x0f,
          0x4d, 0xf8, 0x45, 0x85, 0xc0, 0x0f, 0x48, 0xf8, 0x4d, 0x85, 0xc9, 0x0f,
          0x84, 0x91, 0x00, 0x00, 0x00, 0x8b, 0x44, 0x24, 0x78, 0x8b, 0x54, 0x24,
          0x70, 0xf6, 0xc1, 0x02, 0x74, 0x76, 0x48, 0x83, 0xc6, 0x0a, 0x48, 0x63,
          0xca, 0x4d, 0x8d, 0x34, 0x49, 0x85, 0xc0, 0x78, 0x7c, 0x39, 0xc7, 0x0f,
          0x9c, 0xc1, 0xf6, 0xd9, 0x80, 0xc9, 0x01, 0x31, 0xd2, 0x39, 0xc7, 0x0f,
          0x4c, 0xc7, 0x0f, 0xb6, 0xc9, 0x0f, 0x44, 0xca, 0x85, 0xc0, 0x7e, 0x3c},
         [&](CpuState& cpu, GuestMemory& m, u64 a, u64 b) {
             // String header: length << 5 with bit 1 set (inline storage).
             const u16 hdr = static_cast<u16>((4 + a) << 5 | 2);
             ASSERT_TRUE(m.write_bytes(kData + 8,
                                       reinterpret_cast<const u8*>(&hdr), 2));
             cpu.gpr.set64(RCX, kData);
             cpu.gpr.set64(RDX, 0);
             cpu.gpr.set64(R8, 0x7FFFFFFF);
             cpu.gpr.set64(R9, kData + 0x600);
             const u32 other_len = static_cast<u32>(3 + b);
             const u32 zero = 0;
             const GuestAddr rsp = 0xF000;
             // [rsp+0x78]/[rsp+0x70] after 4 pushes and sub 0x28.
             ASSERT_TRUE(m.write_bytes(rsp - 0x48 + 0x78,
                 reinterpret_cast<const u8*>(&other_len), 4));
             ASSERT_TRUE(m.write_bytes(rsp - 0x48 + 0x70,
                 reinterpret_cast<const u8*>(&zero), 4));
             cpu.gpr.set64(RSP, rsp);
         }},
    };
    for (const Case& c : cases) {
        for (u64 a = 0; a < 3; ++a) {
            for (u64 b = 0; b < 3; ++b) {
                SCOPED_TRACE(::testing::Message() << c.name << " a=" << a
                                                  << " b=" << b);
                std::vector<u8> code = c.bytes;
                const GuestAddr end = 0x1000 + code.size();
                code.push_back(0xEB);  // jmp $: ends the trace at `end`
                code.push_back(0xFE);
                std::vector<u8> zeros(0x4000, 0);
                ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
                auto run_interp_to_end = [&](CpuState& cpu) {
                    Interpreter interp(cpu, *m_memory);
                    for (int n = 0; n < 1000 && cpu.rip != end; ++n) {
                        (void)interp.run(1);
                    }
                };
                auto seed = [&](CpuState& cpu) {
                    ASSERT_TRUE(m_memory->write_bytes(kData, zeros.data(),
                                                      zeros.size()));
                    init_cpu(cpu);
                    cpu.flags.rflags = 0x2;
                    c.setup(cpu, *m_memory, a, b);
                };
                CpuState icpu;
                seed(icpu);
                run_interp_to_end(icpu);
                ASSERT_EQ(icpu.rip, end);
                icpu.materialize_lazy_flags();

                CpuState jcpu;
                seed(jcpu);
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                BasicBlock block = cli::decode_basic_block(*m_memory, 0x1000, true);
                ASSERT_GT(block.instruction_count, 0u);
                ASSERT_LE(block.start_addr + block.byte_length, end + 2);
                CompiledBlock* cb = m_jit->compile(block);
                ASSERT_NE(cb, nullptr);
                (void)test::execute_like_dispatcher(*m_jit, *cb, jcpu);
                ASSERT_FALSE(jcpu.pending_raise.active);
                run_interp_to_end(jcpu);
                ASSERT_EQ(jcpu.rip, end);
                jcpu.materialize_lazy_flags();
                for (int r = 0; r < 16; ++r) {
                    EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r))
                        << gpr_name64(r);
                }
                EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
            }
        }
    }
}
