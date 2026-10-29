// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Within-block flag liveness across partial flag writers (flag_liveness.cpp):
// the analysis decisions, and JIT-vs-interpreter parity for every lowering
// whose record or carry capture they let the stencil path skip.
//
// Contract (Intel SDM Vol. 2): MUL/IMUL write CF/OF and leave SF/ZF/AF/PF
// undefined; BT/BTS/BTR/BTC write CF; ROL/ROR write CF (and OF for a count of
// one) and nothing for a masked count of zero; a CL-count shift writes nothing
// for a masked count of zero. None of them READS a flag. INC/DEC write
// OF/SF/ZF/AF/PF and preserve CF. A register-only CMP/ADD/SUB/AND/OR/XOR/TEST
// then replaces all six. Every block below ends in such a writer, so all six
// compared flags are architecturally defined.

#include "jit_parity_64_fixture.hpp"
#include "core/cpu/jit/flag_liveness.hpp"

namespace {

BasicBlock decode_mode(const std::vector<u8>& code, bool is_64bit) {
    BasicBlock block;
    block.start_addr = 0x1000;
    block.is_64bit = is_64bit;
    MemoryByteReader reader(code.data(), code.size());
    while (reader.remaining() > 0) {
        DecodedInstruction inst;
        if (decode_instruction(reader,
                               is_64bit ? DecodeMode::Mode64 : DecodeMode::Mode32,
                               inst) != DecodeError::Success) {
            break;
        }
        block.instructions.push_back(inst);
    }
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = static_cast<u32>(block.instructions.size());
    return block;
}

std::vector<u8> omit_of(const std::vector<u8>& code, bool is_64bit,
                        u32 expect_insns) {
    BasicBlock block = decode_mode(code, is_64bit);
    EXPECT_EQ(block.instruction_count, expect_insns);
    std::vector<u8> omit;
    compute_omit_lazy_flags(block, omit);
    for (u8& o : omit) o &= 3u;  // drop the internal-join marker bit
    return omit;
}

std::vector<u8> cat(std::initializer_list<std::vector<u8>> parts) {
    std::vector<u8> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

const std::vector<u8> kAddEaxEbx = {0x01, 0xD8};
const std::vector<u8> kCmpEsiEdi = {0x39, 0xFE};

}  // namespace

// The crossing rule used to match opcode NAMES ("IMUL ", "ROL ", "BT "), and
// opcode_name() spells every opcode without a hand-written name as its enum
// identifier, so it matched almost nothing. Every form here must be crossed:
// the ADD's record dies at the CMP in both bitnesses.
TEST(FlagLivenessPartialWriterTest, EveryRegisterPartialWriterIsCrossed) {
    struct Form { const char* name; std::vector<u8> bytes; bool only64; };
    const Form forms[] = {
        {"imul ecx,edx", {0x0F, 0xAF, 0xCA}, false},
        {"imul ecx,edx,7", {0x6B, 0xCA, 0x07}, false},
        {"imul ecx,edx,imm32", {0x69, 0xCA, 0x67, 0x45, 0x23, 0x01}, false},
        {"imul rcx,rdx", {0x48, 0x0F, 0xAF, 0xCA}, true},
        {"imul edx", {0xF7, 0xEA}, false},
        {"mul ecx", {0xF7, 0xE1}, false},
        {"bt ecx,edx", {0x0F, 0xA3, 0xD1}, false},
        {"bt ecx,5", {0x0F, 0xBA, 0xE1, 0x05}, false},
        {"bts ecx,edx", {0x0F, 0xAB, 0xD1}, false},
        {"btr ecx,edx", {0x0F, 0xB3, 0xD1}, false},
        {"btc ecx,edx", {0x0F, 0xBB, 0xD1}, false},
        {"rol ecx,3", {0xC1, 0xC1, 0x03}, false},
        {"ror ecx,1", {0xD1, 0xC9}, false},
        {"rol ecx,cl", {0xD3, 0xC1}, false},
        {"ror rcx,cl", {0x48, 0xD3, 0xC9}, true},
        {"shl ecx,cl", {0xD3, 0xE1}, false},
    };
    for (bool wide : {false, true}) {
        for (const Form& f : forms) {
            if (f.only64 && !wide) continue;
            SCOPED_TRACE(testing::Message() << f.name << (wide ? " x64" : " x86"));
            const auto omit = omit_of(cat({kAddEaxEbx, f.bytes, kCmpEsiEdi}), wide, 3);
            ASSERT_EQ(omit.size(), 3u);
            EXPECT_EQ(omit[0], 1) << "the ADD's record is dead at the CMP";
        }
    }
    // Not crossable: RCL reads CF, DIV can raise #DE, a memory form can fault.
    const Form barriers[] = {
        {"rcl ecx,1", {0xD1, 0xD1}, false},
        {"div ecx", {0xF7, 0xF1}, false},
        {"imul ecx,[rbx]", {0x0F, 0xAF, 0x0B}, false},
        {"bt [rbx],edx", {0x0F, 0xA3, 0x13}, false},
    };
    for (bool wide : {false, true}) {
        for (const Form& f : barriers) {
            SCOPED_TRACE(testing::Message() << f.name << (wide ? " x64" : " x86"));
            const auto omit = omit_of(cat({kAddEaxEbx, f.bytes, kCmpEsiEdi}), wide, 3);
            ASSERT_EQ(omit.size(), 3u);
            EXPECT_EQ(omit[0], 0) << "the ADD's record may be observed";
        }
    }
}

// shl r9d,8 ; inc rbp ; mov ecx,r9d ; add r8d,r8d -- a range decoder's
// normalization. In long mode the INC's record dies at the ADD across the
// register move, and with it the SHL's (the INC no longer captures its CF).
TEST(FlagLivenessPartialWriterTest, LongModeIncDecRecordDiesAcrossRegisterMoves) {
    const std::vector<u8> norm = {0x41, 0xC1, 0xE1, 0x08,  // shl r9d,8
                                  0x48, 0xFF, 0xC5,        // inc rbp
                                  0x44, 0x89, 0xC9,        // mov ecx,r9d
                                  0x45, 0x01, 0xC0};       // add r8d,r8d
    auto omit = omit_of(norm, true, 4);
    ASSERT_EQ(omit.size(), 4u);
    EXPECT_EQ(omit[0], 1) << "SHL's record: the INC no longer reads its CF";
    EXPECT_EQ(omit[1], 1) << "INC's record dies at the ADD";

    // 32-bit blocks keep the adjacent-only rule (their lowering still
    // captures the carry): shl ecx,8 ; inc ebp ; mov eax,ecx ; add edx,edx.
    omit = omit_of({0xC1, 0xE1, 0x08, 0xFF, 0xC5, 0x89, 0xC8, 0x01, 0xD2}, false, 4);
    ASSERT_EQ(omit.size(), 4u);
    EXPECT_EQ(omit[0], 0);
    EXPECT_EQ(omit[1], 0);

    // dec r10 ; mov rcx,r9 ; imul ecx,edx ; cmp eax,ecx: moves and a partial
    // writer between the DEC and its killer.
    omit = omit_of({0x49, 0xFF, 0xCA, 0x4C, 0x89, 0xC9, 0x0F, 0xAF, 0xCA,
                    0x39, 0xC8}, true, 4);
    ASSERT_EQ(omit.size(), 4u);
    EXPECT_EQ(omit[0], 1);

    // Every way the INC's record can still be observed keeps it.
    struct Tail { const char* name; std::vector<u8> bytes; u32 insns; };
    const Tail kept[] = {
        {"load", {0x8B, 0x03, 0x01, 0xD2}, 3},        // mov eax,[rbx] ; add
        {"jz", {0x74, 0x00, 0x01, 0xD2}, 3},          // a branch observes it
        {"adc", {0x89, 0xD0, 0x11, 0xD2}, 3},         // mov ; adc reads CF
        {"nop", {0x90, 0x01, 0xD2}, 3},               // not a register move
        {"live out", {0x89, 0xD0}, 2},
    };
    for (const Tail& t : kept) {
        SCOPED_TRACE(t.name);
        omit = omit_of(cat({{0xFF, 0xC1}, t.bytes}), true, t.insns);  // inc ecx
        ASSERT_FALSE(omit.empty());
        EXPECT_EQ(omit[0], 0);
    }
    // INC r/m8 lowers through a path that always captures: adjacent only.
    omit = omit_of({0xFE, 0xC1, 0x89, 0xD0, 0x01, 0xD2}, true, 3);  // inc cl
    ASSERT_FALSE(omit.empty());
    EXPECT_EQ(omit[0], 0);
}

// imul ecx,edx ; cmp eax,ecx -- the range decoder's bit step. The IMUL's own
// record is dead at the CMP in long mode.
TEST(FlagLivenessPartialWriterTest, LongModeImulOwnRecordDiesAtAFullWriter) {
    struct Case { const char* name; std::vector<u8> bytes; u32 insns; u8 expect; };
    const Case cases[] = {
        {"imul; cmp", {0x0F, 0xAF, 0xCA, 0x39, 0xC8}, 2, 1},
        {"imul r64; cmp", {0x48, 0x0F, 0xAF, 0xCA, 0x39, 0xC8}, 2, 1},
        {"imul imm8; mov; cmp", {0x6B, 0xCA, 0x07, 0x89, 0xD8, 0x39, 0xC8}, 3, 1},
        // A faulting SOURCE precedes the IMUL's own flag write.
        {"imul [rbx]; cmp", {0x0F, 0xAF, 0x0B, 0x39, 0xC8}, 2, 1},
        {"imul; jc; cmp", {0x0F, 0xAF, 0xCA, 0x72, 0x00, 0x39, 0xC8}, 3, 0},
        {"imul; adc", {0x0F, 0xAF, 0xCA, 0x11, 0xD2}, 2, 0},
        {"imul; load; cmp", {0x0F, 0xAF, 0xCA, 0x8B, 0x03, 0x39, 0xC8}, 3, 0},
        {"imul live out", {0x0F, 0xAF, 0xCA, 0x89, 0xD0}, 2, 0},
        {"one-operand imul", {0xF7, 0xEA, 0x39, 0xC8}, 2, 0},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.name);
        auto omit = omit_of(c.bytes, true, c.insns);
        ASSERT_FALSE(omit.empty());
        EXPECT_EQ(omit[0], c.expect);
    }
    // 32-bit blocks: the rule is long-mode only.
    auto omit = omit_of({0x0F, 0xAF, 0xCA, 0x39, 0xC8}, false, 2);
    ASSERT_FALSE(omit.empty());
    EXPECT_EQ(omit[0], 0);
}

// ---------------------------------------------------------------------------
// Interpreter parity for the lowerings those decisions change.
// ---------------------------------------------------------------------------

namespace {

struct Inputs { u64 a, b, c, d; };

const Inputs kInputs[] = {
    {0, 0, 0, 0},
    {1, 2, 3, 4},
    {0xFFFFFFFFull, 1, 0x7FFFFFFFull, 0x80000000ull},
    {0x123456789ABCDEF0ull, 0x0FEDCBA987654321ull, 0x0000000100000000ull, 0x1F},
    {0x8000000000000000ull, 0x7FFFFFFFFFFFFFFFull, 0x20, 0xFFFFFFFFFFFFFFFFull},
    {0x00FFFF00ull, 0x00000800ull, 0x00FFFFFFull, 0x00000400ull},
};

}  // namespace

TEST_F(JitParity64Test, PartialWriterCrossingMatchesInterpreter) {
    struct Block { const char* name; std::vector<u8> bytes; };
    const Block blocks[] = {
        {"shr ecx,11; imul ecx,edx; cmp eax,ecx",
         {0xC1, 0xE9, 0x0B, 0x0F, 0xAF, 0xCA, 0x39, 0xC8}},
        {"add rax,rbx; imul rcx,rdx; cmp rsi,rdi",
         {0x48, 0x01, 0xD8, 0x48, 0x0F, 0xAF, 0xCA, 0x48, 0x39, 0xFE}},
        {"add eax,ebx; imul ecx,edx,imm32; cmp esi,edi",
         {0x01, 0xD8, 0x69, 0xCA, 0x67, 0x45, 0x23, 0x01, 0x39, 0xFE}},
        {"add rax,rbx; imul rcx,rdx,-3; mov eax,ebx; cmp esi,edi",
         {0x48, 0x01, 0xD8, 0x48, 0x6B, 0xCA, 0xFD, 0x89, 0xD8, 0x39, 0xFE}},
        {"sub eax,ebx; mul ecx; cmp esi,edi",
         {0x29, 0xD8, 0xF7, 0xE1, 0x39, 0xFE}},
        {"add eax,ebx; imul edx; cmp esi,edi",
         {0x01, 0xD8, 0xF7, 0xEA, 0x39, 0xFE}},
        {"add eax,ebx; bt ecx,edx; cmp esi,edi",
         {0x01, 0xD8, 0x0F, 0xA3, 0xD1, 0x39, 0xFE}},
        {"add eax,ebx; btc ecx,5; cmp esi,edi",
         {0x01, 0xD8, 0x0F, 0xBA, 0xF9, 0x05, 0x39, 0xFE}},
        {"add eax,ebx; rol ecx,cl; cmp esi,edi",
         {0x01, 0xD8, 0xD3, 0xC1, 0x39, 0xFE}},
        {"add eax,ebx; ror rcx,1; cmp esi,edi",
         {0x01, 0xD8, 0x48, 0xD1, 0xC9, 0x39, 0xFE}},
        {"add eax,ebx; shl edx,cl; cmp esi,edi",
         {0x01, 0xD8, 0xD3, 0xE2, 0x39, 0xFE}},
        // A flag READER between the partial writer and the killer reads the
        // partial writer's flags, never the crossed publisher's NZCV.
        {"add eax,ebx; imul ecx,edx; setc al; cmp esi,edi",
         {0x01, 0xD8, 0x0F, 0xAF, 0xCA, 0x0F, 0x92, 0xC0, 0x39, 0xFE}},
        {"add eax,ebx; imul ecx,edx; cmovo eax,edx; cmp esi,edi",
         {0x01, 0xD8, 0x0F, 0xAF, 0xCA, 0x0F, 0x40, 0xC2, 0x39, 0xFE}},
        {"add eax,ebx; bt ecx,edx; setc al; cmp esi,edi",
         {0x01, 0xD8, 0x0F, 0xA3, 0xD1, 0x0F, 0x92, 0xC0, 0x39, 0xFE}},
        {"add eax,ebx; rol ecx,cl; setz al; setc ah; cmp esi,edi",
         {0x01, 0xD8, 0xD3, 0xC1, 0x0F, 0x94, 0xC0, 0x0F, 0x92, 0xC4, 0x39, 0xFE}},
        {"sub eax,ebx; ror ecx,1; seto al; cmp esi,edi",
         {0x29, 0xD8, 0xD1, 0xC9, 0x0F, 0x90, 0xC0, 0x39, 0xFE}},
    };
    for (const Block& blk : blocks) {
        for (const Inputs& in : kInputs) {
            for (u64 cl : {u64{0}, u64{1}, u64{5}, u64{32}, u64{63}}) {
                SCOPED_TRACE(testing::Message()
                             << blk.name << " a=" << std::hex << in.a << " b=" << in.b
                             << " c=" << in.c << " d=" << in.d << " cl=" << cl);
                // A fresh compiler per case: one compiler recompiling the
                // same address trips the churn limit and declines.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity(blk.bytes.data(), blk.bytes.size(), [&](CpuState& cpu) {
                    cpu.gpr.set64(RAX, in.a);
                    cpu.gpr.set64(RBX, in.b);
                    cpu.gpr.set64(RCX, (in.c & ~u64{0xFF}) | cl);
                    cpu.gpr.set64(RDX, in.d);
                    cpu.gpr.set64(RSI, in.a ^ in.d);
                    cpu.gpr.set64(RDI, in.b + in.c);
                    cpu.flags.rflags = 0x2 | (cl & 1);
                }, true, false, true, true);
                if (HasFatalFailure()) return;
            }
        }
    }
}

TEST_F(JitParity64Test, DeadIncDecAcrossRegisterMovesMatchesInterpreter) {
    struct Block { const char* name; std::vector<u8> bytes; };
    const Block blocks[] = {
        {"shl r9d,8; inc rbp; mov ecx,r9d; add r8d,r8d",
         {0x41, 0xC1, 0xE1, 0x08, 0x48, 0xFF, 0xC5, 0x44, 0x89, 0xC9,
          0x45, 0x01, 0xC0}},
        {"sub eax,ebx; dec r10; mov rcx,r9; imul ecx,edx; cmp eax,ecx",
         {0x29, 0xD8, 0x49, 0xFF, 0xCA, 0x4C, 0x89, 0xC9, 0x0F, 0xAF, 0xCA,
          0x39, 0xC8}},
        {"cmp eax,ebx; inc ecx; movzx edx,cl; dec esi; mov edi,0x55; or eax,edx",
         {0x39, 0xD8, 0xFF, 0xC1, 0x0F, 0xB6, 0xD1, 0xFF, 0xCE, 0xBF, 0x55,
          0x00, 0x00, 0x00, 0x09, 0xD0}},
        // Kept records: the carry the INC preserves is what ADC reads, and
        // SETZ reads the INC's own ZF across the register move.
        {"add eax,ebx; inc rcx; mov edx,esi; adc edi,edx",
         {0x01, 0xD8, 0x48, 0xFF, 0xC1, 0x89, 0xF2, 0x11, 0xD7}},
        {"add eax,ebx; inc ecx; mov edx,esi; setz dl; or eax,edx",
         {0x01, 0xD8, 0xFF, 0xC1, 0x89, 0xF2, 0x0F, 0x94, 0xC2, 0x09, 0xD0}},
        {"imul ecx,edx; mov eax,ebx; seto al; cmp esi,edi",
         {0x0F, 0xAF, 0xCA, 0x89, 0xD8, 0x0F, 0x90, 0xC0, 0x39, 0xFE}},
    };
    for (const Block& blk : blocks) {
        for (const Inputs& in : kInputs) {
            for (u64 carry : {u64{0}, u64{1}}) {
                SCOPED_TRACE(testing::Message()
                             << blk.name << " a=" << std::hex << in.a << " b=" << in.b
                             << " c=" << in.c << " d=" << in.d << " cf=" << carry);
                // A fresh compiler per case: one compiler recompiling the
                // same address trips the churn limit and declines.
                m_jit = std::make_unique<JitCompiler>(*m_memory);
                check_parity(blk.bytes.data(), blk.bytes.size(), [&](CpuState& cpu) {
                    cpu.gpr.set64(RAX, in.a);
                    cpu.gpr.set64(RBX, in.b);
                    cpu.gpr.set64(RCX, in.c);
                    cpu.gpr.set64(RDX, in.d);
                    cpu.gpr.set64(RBP, in.d - 1);
                    cpu.gpr.set64(RSI, in.a ^ in.c);
                    cpu.gpr.set64(R8, in.b);
                    cpu.gpr.set64(R9, in.c);
                    cpu.gpr.set64(R10, in.a);
                    cpu.flags.rflags = 0x2 | carry;
                }, true, false, true, true);
                if (HasFatalFailure()) return;
            }
        }
    }
}

// A fault is precise (Intel SDM Vol. 3A 6.6): RIP names the faulting
// instruction and RFLAGS are those before it. The IMUL's own record may be
// elided past a faulting SOURCE (its fault precedes the IMUL's flag write),
// but the writer before it must still be published; an INC/DEC before a
// faulting load keeps its record.
TEST_F(JitParity64Test, FaultsAroundElidedPartialWriterRecordsArePrecise) {
    // `defined`: the flags the faulting instruction's predecessors define.
    // After an IMUL only CF and OF are (SF/ZF/AF/PF undefined).
    struct Form { const char* name; std::vector<u8> bytes; bool all_defined; };
    const Form forms[] = {
        {"add ebx,ebx; imul ecx,[rax]; cmp edx,edx",
         {0x01, 0xDB, 0x0F, 0xAF, 0x08, 0x39, 0xD2}, true},
        {"add rbx,rbx; imul rcx,[rax],7; cmp edx,edx",
         {0x48, 0x01, 0xDB, 0x48, 0x6B, 0x08, 0x07, 0x39, 0xD2}, true},
        {"shl ebx,4; inc ecx; mov edx,[rax]; cmp edx,edx",
         {0xC1, 0xE3, 0x04, 0xFF, 0xC1, 0x8B, 0x10, 0x39, 0xD2}, true},
        {"add ebx,ebx; imul ecx,edx; mov edx,[rax]; cmp edx,edx",
         {0x01, 0xDB, 0x0F, 0xAF, 0xCA, 0x8B, 0x10, 0x39, 0xD2}, false},
    };
    for (const Form& form : forms) {
        for (u64 carry : {u64{0}, u64{1}}) {
            SCOPED_TRACE(testing::Message() << form.name << " cf=" << carry);
            ASSERT_TRUE(m_memory->write_bytes(0x1000, form.bytes.data(),
                                              form.bytes.size()));
            BasicBlock block = build_block(form.bytes.data(), form.bytes.size());
            const auto setup = [&](CpuState& cpu) {
                cpu.gpr.set64(RAX, 0x40000000ull);  // unmapped
                cpu.gpr.set64(RBX, 0x80000001ull);
                cpu.gpr.set64(RCX, 0xFFFFFFFFull);
                cpu.gpr.set64(RDX, 0x12345ull);
                cpu.flags.rflags = 0x2 | carry;
            };
            CpuState icpu; init_cpu(icpu); setup(icpu);
            { Interpreter interp(icpu, *m_memory); (void)interp.run(block.instruction_count); }
            icpu.materialize_lazy_flags();
            CpuState jcpu; init_cpu(jcpu); setup(jcpu);
            JitCompiler jit(*m_memory);
            CompiledBlock* cb = jit.compile(block);
            ASSERT_NE(cb, nullptr);
            (void)test::execute_like_dispatcher(jit, *cb, jcpu);
            EXPECT_TRUE(jcpu.pending_raise.active) << "the access did not fault";
            jcpu.materialize_lazy_flags();
            EXPECT_EQ(icpu.rip, jcpu.rip);
            for (int i = 0; i < 16; i++) {
                EXPECT_EQ(icpu.gpr.get64(i), jcpu.gpr.get64(i)) << gpr_name64(i);
            }
            EXPECT_EQ(icpu.flags.cf(), jcpu.flags.cf());
            EXPECT_EQ(icpu.flags.of(), jcpu.flags.of());
            if (!form.all_defined) continue;
            EXPECT_EQ(icpu.flags.zf(), jcpu.flags.zf());
            EXPECT_EQ(icpu.flags.sf(), jcpu.flags.sf());
            EXPECT_EQ(icpu.flags.pf(), jcpu.flags.pf());
            EXPECT_EQ(icpu.flags.af(), jcpu.flags.af());
        }
    }
}

// ---------------------------------------------------------------------------
// Randomized differential: short long-mode blocks built from the instruction
// shapes the rules above reason about -- full writers, the crossable partial
// writers (IMUL/MUL/BT*/ROL/ROR/CL shifts), INC/DEC, register moves, valid
// loads, flag READERS (SETcc/CMOVcc/ADC/SBB/Jcc) and forward Jcc skips --
// run through the interpreter and the JIT from the same state.
//
// A reader only tests flags that are architecturally DEFINED at that point
// on every path that reaches it (the generator tracks the defined set along
// the fall-through and intersects it at each Jcc target), and each block ends
// in a register CMP, so every compared flag is defined. Undefined flags
// (IMUL's SF/ZF/AF/PF, BT's OF/SF/AF/PF, a multi-bit rotate's OF, a logic
// op's AF) are never observed.
// ---------------------------------------------------------------------------

namespace {

struct FuzzRng64 {
    u64 s;
    u64 next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return s >> 11; }
    u32 below(u32 n) { return static_cast<u32>(next() % n); }
};

constexpr u8 kFC = 1, kFP = 4, kFA = 0x10, kFZ = 0x40, kFS = 0x80;
constexpr u32 kFO = 0x800;
// Defined-flag set as a mask over RFLAGS bit positions.
constexpr u32 kAllArith = kFC | kFP | kFA | kFZ | kFS | kFO;

u32 cc_reads(u8 cc) {
    switch (cc >> 1) {
    case 0: return kFO;
    case 1: return kFC;
    case 2: return kFZ;
    case 3: return kFC | kFZ;
    case 4: return kFS;
    case 5: return kFP;
    case 6: return kFS | kFO;
    default: return kFZ | kFS | kFO;
    }
}

struct GenInsn {
    std::vector<u8> bytes;
    int skip = 0;   // forward Jcc: instructions skipped (patched to bytes)
};

// Registers the generator may name: RAX RCX RDX RSI RDI RBP R8-R11 (RBX
// is the data pointer, RSP the stack).
constexpr u8 kRegs[] = {0, 1, 2, 6, 7, 5, 8, 9, 10, 11};

u8 rex(bool w, u8 reg, u8 rm) {
    return static_cast<u8>(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0));
}
u8 modrm_rr(u8 reg, u8 rm) { return static_cast<u8>(0xC0 | ((reg & 7) << 3) | (rm & 7)); }

std::vector<u8> rr(bool w, std::initializer_list<u8> op, u8 reg, u8 rm) {
    std::vector<u8> b{rex(w, reg, rm)};
    b.insert(b.end(), op);
    b.push_back(modrm_rr(reg, rm));
    return b;
}

std::vector<GenInsn> generate_block(FuzzRng64& rng, u32& defined_out) {
    std::vector<GenInsn> out;
    u32 defined = kAllArith;  // the initial RFLAGS are concrete
    // Pending forward-Jcc joins: (target index, defined set on the taken edge).
    std::vector<std::pair<usize, u32>> joins;
    std::vector<std::pair<usize, bool>> join_overlay;
    // The interpreter (the oracle here) writes a rotate's or BT's CF/OF
    // through overlays, and its MUL/IMUL and shifts write RFLAGS directly
    // without retiring them, so a stale overlay wins there. Until a full
    // writer clears it, those direct writers are not generated (the JIT side
    // of that shape is pinned by ImulCarryAfterARotateIsTheProducts).
    bool overlay = false;
    const u32 n = 4 + rng.below(9);
    for (u32 k = 0; k < n; ++k) {
        for (const auto& j : joins) {
            if (j.first == out.size()) defined &= j.second;
        }
        for (const auto& j : join_overlay) {
            if (j.first == out.size()) overlay |= j.second;
        }
        const bool w = rng.below(2) != 0;
        const u8 a = kRegs[rng.below(sizeof(kRegs))];
        const u8 b = kRegs[rng.below(sizeof(kRegs))];
        GenInsn in;
        switch (rng.below(16)) {
        case 0: case 1: {  // full writer: add/sub/and/or/xor/cmp/test
            static const u8 ops[] = {0x01, 0x29, 0x21, 0x09, 0x31, 0x39, 0x85};
            const u8 op = ops[rng.below(7)];
            in.bytes = rr(w, {op}, b, a);
            const bool logic = op == 0x21 || op == 0x09 || op == 0x31 || op == 0x85;
            defined = logic ? (kAllArith & ~u32{kFA}) : kAllArith;
            overlay = false;
            break;
        }
        case 2: {  // imul r, r/m (two- or three-operand)
            if (overlay) { --k; continue; }
            if (rng.below(2)) {
                in.bytes = rr(w, {0x0F, 0xAF}, a, b);
            } else {
                in.bytes = rr(w, {0x6B}, a, b);
                in.bytes.push_back(static_cast<u8>(rng.below(256)));
            }
            defined = kFC | kFO;
            break;
        }
        case 3: {  // one-operand MUL/IMUL: RDX:RAX
            if (overlay) { --k; continue; }
            in.bytes = rr(w, {0xF7}, rng.below(2) ? 4 : 5, b);
            defined = kFC | kFO;
            break;
        }
        case 4: {  // BT/BTS/BTR/BTC r/m, r  or  BT* r/m, imm8
            if (rng.below(2)) {
                static const u8 ops[] = {0xA3, 0xAB, 0xB3, 0xBB};
                in.bytes = rr(w, {0x0F, ops[rng.below(4)]}, b, a);
            } else {
                in.bytes = rr(w, {0x0F, 0xBA}, static_cast<u8>(4 + rng.below(4)), a);
                in.bytes.push_back(static_cast<u8>(rng.below(64)));
            }
            defined = kFC;  // OF/SF/AF/PF undefined; ZF not relied on
            overlay = true;
            break;
        }
        case 5: {  // ROL/ROR by 1, imm8, CL
            overlay = true;
            const u8 ext = static_cast<u8>(rng.below(2));
            switch (rng.below(3)) {
            case 0: in.bytes = rr(w, {0xD1}, ext, a); defined |= kFC | kFO; break;
            case 1:
                in.bytes = rr(w, {0xC1}, ext, a);
                in.bytes.push_back(static_cast<u8>(2 + rng.below(30)));
                defined = (defined | kFC) & ~kFO;
                break;
            default:
                in.bytes = rr(w, {0xD3}, ext, a);
                defined &= ~kFO;  // count 0 keeps CF; any other count sets it
                break;
            }
            break;
        }
        case 6: {  // SHL/SHR by CL
            if (overlay) { --k; continue; }
            in.bytes = rr(w, {0xD3}, rng.below(2) ? 4 : 5, a);
            defined &= ~(kFO | kFA);
            break;
        }
        case 7: case 8: {  // INC/DEC
            in.bytes = rr(w, {0xFF}, static_cast<u8>(rng.below(2)), a);
            defined |= kFO | kFS | kFZ | kFA | kFP;
            overlay = false;
            break;
        }
        case 9: case 10: {  // register moves
            switch (rng.below(3)) {
            case 0: in.bytes = rr(w, {0x89}, b, a); break;
            case 1: in.bytes = rr(false, {0x0F, 0xB6}, a, b); break;  // movzx r32, r8
            default: {
                in.bytes = {static_cast<u8>(0x40 | ((a & 8) ? 1 : 0)),
                            static_cast<u8>(0xB8 + (a & 7))};
                const u32 imm = static_cast<u32>(rng.next());
                for (int i = 0; i < 4; ++i) in.bytes.push_back(static_cast<u8>(imm >> (8 * i)));
            }
            }
            break;
        }
        case 11: {  // load: mov r, [rbx + disp8]
            in.bytes = {rex(w, a, 3), 0x8B, static_cast<u8>(0x40 | ((a & 7) << 3) | 3),
                        static_cast<u8>(rng.below(16) * 8)};
            break;
        }
        case 12: case 13: {  // readers of DEFINED flags
            u8 cc = 0;
            bool found = false;
            for (int tries = 0; tries < 16 && !found; ++tries) {
                cc = static_cast<u8>(rng.below(16));
                found = (cc_reads(cc) & ~defined) == 0;
            }
            if (!found) { --k; continue; }
            switch (rng.below(4)) {
            case 0:  // SETcc r/m8
                in.bytes = rr(false, {0x0F, static_cast<u8>(0x90 + cc)}, 0, a);
                break;
            case 1:  // CMOVcc
                in.bytes = rr(w, {0x0F, static_cast<u8>(0x40 + cc)}, a, b);
                break;
            default:  // ADC/SBB need CF
                if ((defined & kFC) == 0) { --k; continue; }
                in.bytes = rr(w, {rng.below(2) ? u8{0x11} : u8{0x19}}, b, a);
                defined = kAllArith;
                overlay = false;
                break;
            }
            break;
        }
        default: {  // forward Jcc over 1-3 instructions
            if (k + 2 >= n) { --k; continue; }
            u8 cc = 0;
            bool found = false;
            for (int tries = 0; tries < 16 && !found; ++tries) {
                cc = static_cast<u8>(rng.below(16));
                found = (cc_reads(cc) & ~defined) == 0;
            }
            if (!found) { --k; continue; }
            in.bytes = {static_cast<u8>(0x70 + cc), 0};
            in.skip = 1 + static_cast<int>(rng.below(3));
            joins.emplace_back(out.size() + 1 + static_cast<usize>(in.skip), defined);
            join_overlay.emplace_back(out.size() + 1 + static_cast<usize>(in.skip), overlay);
            break;
        }
        }
        out.push_back(std::move(in));
    }
    for (const auto& j : joins) {
        if (j.first >= out.size()) defined &= j.second;
    }
    // A register CMP defines every compared flag.
    out.push_back(GenInsn{rr(true, {0x39}, 1, 0), 0});
    defined_out = kAllArith;
    // Patch the Jcc displacements now that every length is known.
    for (usize i = 0; i < out.size(); ++i) {
        if (out[i].skip == 0) continue;
        usize disp = 0;
        // Never past the final CMP: the block's last writer is always run.
        for (usize s = 1; s <= static_cast<usize>(out[i].skip) && i + s + 1 < out.size(); ++s) {
            disp += out[i + s].bytes.size();
        }
        out[i].bytes[1] = static_cast<u8>(disp);
    }
    return out;
}

}  // namespace

TEST_F(JitParity64Test, RandomPartialWriterBlocksMatchInterpreter) {
    constexpr GuestAddr kData = 0x9000;  // inside the fixture's mapped region
    FuzzRng64 rng{0x5EED1A2Bull};
    u32 compared = 0;
    for (u32 trial = 0; trial < 3000; ++trial) {
        u32 defined = 0;
        const std::vector<GenInsn> gen = generate_block(rng, defined);
        std::vector<u8> code;
        for (const GenInsn& g : gen) code.insert(code.end(), g.bytes.begin(), g.bytes.end());
        ASSERT_LT(code.size(), 0x800u);
        ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
        std::array<u8, 128> data{};
        for (u8& d : data) d = static_cast<u8>(rng.next());
        ASSERT_TRUE(m_memory->write_bytes(kData, data.data(), data.size()));
        std::array<u64, 16> gpr{};
        for (u64& g : gpr) {
            g = rng.next() ^ (rng.next() << 21);
            if (rng.below(4) == 0) g &= 0xFF;  // small counts and products
        }
        gpr[RSP] = 0x10000;
        gpr[RBX] = kData;
        const u64 rflags = 0x2 | (rng.next() & kAllArith);
        const GuestAddr end = 0x1000 + code.size();
        std::string text;
        for (u8 byte : code) {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%02X ", byte);
            text += hex;
        }
        for (u8 r = 0; r < 16; ++r) {
            char reg[40];
            std::snprintf(reg, sizeof(reg), "%s%s=%llx", r ? " " : " | ",
                          gpr_name64(r), static_cast<unsigned long long>(gpr[r]));
            text += reg;
        }
        text += " rflags=" + std::to_string(rflags);
        SCOPED_TRACE(testing::Message() << "trial " << trial << " code " << text);

        CpuState icpu; init_cpu(icpu);
        for (u8 r = 0; r < 16; ++r) icpu.gpr.set64(r, gpr[r]);
        icpu.flags.rflags = rflags;
        {
            Interpreter interp(icpu, *m_memory);
            for (u32 s = 0; s < 64 && icpu.rip != end; ++s) {
                const ExecResult r = interp.step();
                ASSERT_TRUE(r == ExecResult::Continue || r == ExecResult::Branch);
            }
        }
        ASSERT_EQ(icpu.rip, end);
        icpu.materialize_lazy_flags();

        CpuState jcpu; init_cpu(jcpu);
        for (u8 r = 0; r < 16; ++r) jcpu.gpr.set64(r, gpr[r]);
        jcpu.flags.rflags = rflags;
        ASSERT_TRUE(m_memory->write_bytes(kData, data.data(), data.size()));
        JitCompiler jit(*m_memory);
        for (int hop = 0; hop < 16 && jcpu.rip != end; ++hop) {
            const usize off = static_cast<usize>(jcpu.rip - 0x1000);
            ASSERT_LT(off, code.size());
            BasicBlock part = build_block(code.data() + off, code.size() - off);
            part.start_addr = jcpu.rip;
            CompiledBlock* cb = jit.compile(part);
            ASSERT_NE(cb, nullptr);
            ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, jcpu),
                      JitExecResult::Continue);
            ASSERT_FALSE(jcpu.pending_raise.active);
        }
        ASSERT_EQ(jcpu.rip, end);
        jcpu.materialize_lazy_flags();
        for (u8 r = 0; r < 16; ++r) {
            ASSERT_EQ(icpu.gpr.get64(r), jcpu.gpr.get64(r)) << gpr_name64(r);
        }
        ASSERT_EQ(icpu.flags.rflags & defined, jcpu.flags.rflags & defined)
            << "interp rflags=0x" << std::hex << icpu.flags.rflags
            << " jit rflags=0x" << jcpu.flags.rflags;
        ++compared;
    }
    EXPECT_EQ(compared, 3000u);
}
// Intel SDM Vol. 2A IMUL: CF = OF = 1 when the signed product does not fit
// the destination, whatever carry a preceding rotate left. Expected values
// are the SDM's, computed here, and checked against the JIT only: the
// interpreter currently gets this shape wrong (its MUL/IMUL write RFLAGS.CF
// directly and the rotate's CF overlay wins at the next materialization), so
// it cannot be the oracle for it.
TEST_F(JitParity64Test, ImulCarryAfterARotateIsTheProducts) {
    // ror edi,5 ; imul rdi,rax,-71 ; setc cl ; seto dl
    const std::vector<u8> code = {0x40, 0xC1, 0xCF, 0x05, 0x48, 0x6B, 0xF8, 0xB9,
                                  0x0F, 0x92, 0xC1, 0x0F, 0x90, 0xC2};
    for (u64 rax : {0xdeb696e22aa551a5ull, u64{3}, 0x0100000000000000ull,
                    0x8000000000000000ull}) {
        const __int128 product = static_cast<__int128>(static_cast<i64>(rax)) * -71;
        const u64 overflow = product != static_cast<i64>(product) ? 1 : 0;
        SCOPED_TRACE(testing::Message() << std::hex << "rax=" << rax);
        for (u64 cf_in : {u64{0}, u64{1}}) {
            ASSERT_TRUE(m_memory->write_bytes(0x1000, code.data(), code.size()));
            BasicBlock block = build_block(code.data(), code.size());
            const auto setup = [&](CpuState& cpu) {
                cpu.gpr.set64(RAX, rax);
                cpu.gpr.set64(RDI, 0xecad8c6b6c5e9384ull ^ cf_in);
                cpu.flags.rflags = 0x2 | cf_in;
            };
            CpuState jcpu; init_cpu(jcpu); setup(jcpu);
            JitCompiler jit(*m_memory);
            CompiledBlock* cb = jit.compile(block);
            ASSERT_NE(cb, nullptr);
            (void)test::execute_like_dispatcher(jit, *cb, jcpu);
            EXPECT_EQ(jcpu.gpr.get64(RCX) & 0xFF, overflow) << "JIT CF";
            EXPECT_EQ(jcpu.gpr.get64(RDX) & 0xFF, overflow) << "JIT OF";
        }
    }
}
