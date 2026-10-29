// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Linear64 value and flag semantics against the interpreter or oracle: pinned
// destinations, elided flag records, SETcc/CMOV and FMA NaN priority. Split
// from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Register MOV and LEA write a pinned destination directly instead of
// staging through X0 (ops64.cpp). The hazards are the 32-bit write's
// zero-extension -- MOV EAX,EAX clears RAX[63:32] (Intel SDM Vol. 1 3.4.1.1)
// -- and an LEA whose destination is also its index when the base is not
// pinned. Each form runs as its own block against the interpreter, from
// registers whose upper halves are all set, so every pinned/unpinned
// combination of the block's layout is exercised.
TEST_F(JitLinear64Test, RegisterMovAndLeaIntoPinsMatchInterpreter) {
    const std::vector<std::vector<u8>> forms = {
        {0x89, 0xC0},                    // mov eax,eax
        {0x89, 0xD8},                    // mov eax,ebx
        {0x8B, 0xC3},                    // mov eax,ebx (8B form)
        {0x44, 0x89, 0xC0},              // mov eax,r8d
        {0x41, 0x89, 0xC0},              // mov r8d,eax
        {0x45, 0x89, 0xCA},              // mov r10d,r9d
        {0x45, 0x89, 0xC0},              // mov r8d,r8d
        {0x48, 0x89, 0xD8},              // mov rax,rbx
        {0x48, 0x8B, 0xC3},              // mov rax,rbx (8B form)
        {0x4C, 0x89, 0xC8},              // mov rax,r9
        {0x49, 0x89, 0xC1},              // mov r9,rax
        {0x4D, 0x89, 0xCA},              // mov r10,r9
        {0x48, 0x89, 0xC0},              // mov rax,rax
        {0x48, 0x8D, 0x44, 0x83, 0x08},  // lea rax,[rbx+rax*4+8]
        {0x49, 0x8D, 0x04, 0x41},        // lea rax,[r9+rax*2]
        {0x4B, 0x8D, 0x04, 0x01},        // lea rax,[r9+r8]
        {0x4E, 0x8D, 0x04, 0x40},        // lea r8,[rax+r8*2]
        {0x4F, 0x8D, 0x44, 0x08, 0xF0},  // lea r8,[r8+r9-16]
        {0x48, 0x8D, 0x04, 0x18},        // lea rax,[rax+rbx]
        {0x48, 0x8D, 0x00},              // lea rax,[rax]
        {0x48, 0x8D, 0x05, 0x10, 0x00, 0x00, 0x00},  // lea rax,[rip+16]
    };
    GuestAddr code_at = kCode64;
    for (const auto& code : forms) {
        SCOPED_TRACE(::testing::Message() << "form bytes " << code.size()
                                          << " opcode 0x" << std::hex
                                          << int(code[code.size() > 2 &&
                                                      (code[0] & 0xF0) == 0x40
                                                  ? 1 : 0]));
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState seed = cpu64(0);
        seed.rip = code_at;
        for (u8 r = 0; r < 16; ++r) {
            if (r == ESP) continue;
            seed.gpr.set64(r, 0xF0E0D0C0B0A09080ull + 0x0101010101010101ull * r);
        }
        CpuState jcpu = seed;
        CpuState icpu = seed;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(1);
        for (u8 r = 0; r < 16; ++r) {
            EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r)) << "r" << int(r);
        }
    }
}

// ALU/CMP/TEST with a memory operand read the operand first and then work on
// a pinned register in place (ops64.cpp), relying on the access's slow path
// preserving pins. Intel SDM Vol. 2 ADD/SUB/AND/OR/XOR/CMP/TEST: the result
// and all six status flags follow from the two operands, and a faulting read
// (Vol. 3A 6.6) leaves every register, RFLAGS and RIP as before the
// instruction. Each form runs alone and before a flag-consuming Jcc, through
// the inline path, through the helper (a Mixed host page), and against an
// unmapped address; pinned and unpinned destinations, the base register as
// destination, and both operand orders are covered.
TEST_F(JitLinear64Test, MemoryOperandAluIntoPinsMatchesInterpreterOnEveryPath) {
    const std::vector<std::vector<u8>> forms = {
        {0x03, 0x43, 0x04},        // add eax,[rbx+4]
        {0x2B, 0x0B},              // sub ecx,[rbx]
        {0x23, 0x13},              // and edx,[rbx]
        {0x0B, 0x33},              // or esi,[rbx]
        {0x33, 0x3B},              // xor edi,[rbx]
        {0x03, 0x1B},              // add ebx,[rbx]  (destination is the base)
        {0x44, 0x2B, 0x0B},        // sub r9d,[rbx]  (unpinned destination)
        {0x48, 0x03, 0x03},        // add rax,[rbx]
        {0x48, 0x2B, 0x4B, 0x08},  // sub rcx,[rbx+8]
        {0x48, 0x33, 0x1B},        // xor rbx,[rbx]
        {0x4C, 0x23, 0x13},        // and r10,[rbx]
        {0x3B, 0x03},              // cmp eax,[rbx]
        {0x39, 0x33},              // cmp [rbx],esi
        {0x44, 0x3B, 0x13},        // cmp r10d,[rbx]
        {0x83, 0x3B, 0x05},        // cmp dword [rbx],5
        {0x48, 0x3B, 0x13},        // cmp rdx,[rbx]
        {0x48, 0x39, 0x0B},        // cmp [rbx],rcx
        {0x85, 0x03},              // test [rbx],eax
        {0x85, 0x3B},              // test [rbx],edi
        {0x44, 0x85, 0x03},        // test [rbx],r8d
        {0x48, 0x85, 0x0B},        // test [rbx],rcx
    };
    // jb +0 / jle +0: a consumer of CF and of ZF|SF^OF after the form.
    const std::vector<std::vector<u8>> tails = {{}, {0x72, 0x00}, {0x7E, 0x00}};

    // kData64's host page is Mixed (its second guest page is denied), so a
    // read of its first guest page takes the checked helper; kData64 + 0x2000
    // is plain RW on its own host page only when host pages are 4 KiB, so the
    // inline path uses a separate, wholly accessible region.
    constexpr GuestAddr kInline = kData64 + 0x10000;
    ASSERT_TRUE(m_memory->map_region(kInline, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                         MemoryProtection::None));
    const u8* map = m_memory->linear_check_page_map();
    ASSERT_NE(map[kData64 >> kLinearHostPageShift] & kLinearCheckMixedRead, 0u);
    ASSERT_EQ(map[kInline >> kLinearHostPageShift] & kLinearCheckMixedRead, 0u);
    struct Path { const char* name; GuestAddr rbx; bool faults; };
    const Path paths[] = {
        {"inline", kInline + 0x100, false},
        {"helper", kData64 + 0x100, false},
        {"fault", kUnmapped64 + 0x100, true},
    };
    // Memory words whose sums, differences and ANDs set CF, OF, SF, ZF.
    const u64 memory_values[] = {0x7FFFFFFF80000001ull, 0xFFFFFFFFFFFFFFFFull,
                                 0x0000000000000005ull};

    GuestAddr code_at = kCode64;
    for (const auto& form : forms) {
        for (const auto& tail : tails) {
            std::vector<u8> code = form;
            code.insert(code.end(), tail.begin(), tail.end());
            code_at += 0x40;
            ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
            BasicBlock block = build64(code);
            block.start_addr = code_at;
            ASSERT_EQ(block.instruction_count, tail.empty() ? 1u : 2u);
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            for (const Path& path : paths) {
                for (const u64 value : memory_values) {
                    SCOPED_TRACE(::testing::Message()
                                 << "code@0x" << std::hex << code_at
                                 << " bytes=" << code.size() << " path="
                                 << path.name << " mem=0x" << value);
                    if (!path.faults) {
                        ASSERT_TRUE(m_memory->write_u64(path.rbx, value));
                        ASSERT_TRUE(m_memory->write_u64(path.rbx + 8, ~value));
                    }
                    CpuState seed = cpu64(path.rbx);
                    seed.rip = code_at;
                    for (u8 r = 0; r < 16; ++r) {
                        if (r == ESP || r == EBX) continue;
                        seed.gpr.set64(r, 0x80000000FFFFFFF0ull +
                                              0x0001000100000003ull * r);
                    }
                    seed.flags.rflags = 0x8D7u;
                    CpuState jcpu = seed;
                    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                              JitExecResult::Continue);
                    if (path.faults) {
                        ASSERT_TRUE(jcpu.pending_raise.active);
                        EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
                        // The access address is RBX plus the form's disp.
                        EXPECT_GE(jcpu.pending_raise.args[1], path.rbx);
                        EXPECT_LE(jcpu.pending_raise.args[1], path.rbx + 8);
                        EXPECT_EQ(jcpu.rip, code_at);
                        jcpu.materialize_lazy_flags();
                        EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, 0x8D5u);
                        for (u8 r = 0; r < 16; ++r) {
                            EXPECT_EQ(jcpu.gpr.get64(r), seed.gpr.get64(r))
                                << "r" << int(r);
                        }
                        continue;
                    }
                    EXPECT_FALSE(jcpu.pending_raise.active);
                    CpuState icpu = seed;
                    Interpreter interp(icpu, *m_memory);
                    (void)interp.run(block.instruction_count);
                    icpu.materialize_lazy_flags();
                    jcpu.materialize_lazy_flags();
                    EXPECT_EQ(jcpu.rip, icpu.rip);
                    EXPECT_EQ(jcpu.flags.rflags & 0x8D5u,
                              icpu.flags.rflags & 0x8D5u);
                    for (u8 r = 0; r < 16; ++r) {
                        EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r))
                            << "r" << int(r);
                    }
                }
            }
        }
    }
}

// Intel SDM Vol. 3A 6.6: a fault is precise -- RIP names the faulting
// instruction and RFLAGS is what the instructions before it left. The JIT
// elides a flag record that a later full writer overwrites even when a memory
// access between them may fault, and rebuilds the record from the block's
// recovery descriptor: in the signal handler for a hardware fault, and in
// execute() for a software raise. Each block runs with its access faulting in
// hardware (unmapped), faulting in software (a denied guest page of a Mixed
// host page, refused by the checked helper) and succeeding. The last four
// forms must KEEP the record -- an operand register is written before the
// fault, ADD's lone register holds the result, or the operand is unpinned --
// and are here so a planner that wrongly accepts them fails.
TEST_F(JitLinear64Test, ElidedFlagRecordIsRebuiltAtAPreciseFault) {
    struct Form { const char* name; std::vector<u8> bytes; u32 fault_index; };
    const Form forms[] = {
        {"cmp rax,rcx; mov rdx,[rbx]; add rsi,1",
         {0x48, 0x39, 0xC8, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01}, 1},
        {"sub rax,rcx; mov rdx,[rbx]; add rsi,1",
         {0x48, 0x29, 0xC8, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01}, 1},
        {"add eax,ecx; mov edx,[rbx]; sub esi,1",
         {0x01, 0xC8, 0x8B, 0x13, 0x83, 0xEE, 0x01}, 1},
        {"test rax,rcx; mov rdx,[rbx]; or esi,1",
         {0x48, 0x85, 0xC8, 0x48, 0x8B, 0x13, 0x83, 0xCE, 0x01}, 1},
        {"cmp rax,-3; mov rdx,[rbx]; add rsi,1",
         {0x48, 0x83, 0xF8, 0xFD, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01}, 1},
        {"add eax,0x7fffffff; mov rdx,[rbx]; add rsi,1",
         {0x05, 0xFF, 0xFF, 0xFF, 0x7F, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01}, 1},
        {"cmp rax,rcx; add rdx,[rbx]  (the killer faults)",
         {0x48, 0x39, 0xC8, 0x48, 0x03, 0x13}, 1},
        {"xor rax,rcx; mov rdx,[rbx]; mov rdi,[rbx+8]; add rsi,1",
         {0x48, 0x31, 0xC8, 0x48, 0x8B, 0x13, 0x48, 0x8B, 0x7B, 0x08,
          0x48, 0x83, 0xC6, 0x01}, 1},
        {"cmp rax,rcx; mov rcx,rdi; mov rdx,[rbx]; add rsi,1",
         {0x48, 0x39, 0xC8, 0x48, 0x89, 0xF9, 0x48, 0x8B, 0x13,
          0x48, 0x83, 0xC6, 0x01}, 2},
        {"cmp rax,rcx; xchg rdi,rcx; mov rdx,[rbx]; add rsi,1",
         {0x48, 0x39, 0xC8, 0x48, 0x87, 0xCF, 0x48, 0x8B, 0x13,
          0x48, 0x83, 0xC6, 0x01}, 2},
        {"add rax,rax; mov rdx,[rbx]; add rsi,1",
         {0x48, 0x01, 0xC0, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01}, 1},
        {"sub r8,rax; mov rdx,[rbx]; add rsi,1",
         {0x49, 0x29, 0xC0, 0x48, 0x8B, 0x13, 0x48, 0x83, 0xC6, 0x01}, 1},
    };
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                         MemoryProtection::None));
    const u8* map = m_memory->linear_check_page_map();
    ASSERT_NE(map[kData64 >> kLinearHostPageShift] & kLinearCheckMixedRead, 0u);
    struct Path { const char* name; GuestAddr rbx; bool faults; };
    const Path paths[] = {
        {"hardware fault", kUnmapped64 + 0x100, true},
        {"software fault", kData64 + 0x1100, true},
        {"no fault", kData64 + 0x100, false},
    };
    // RAX/RCX pairs whose ADD/SUB/CMP set CF, OF, ZF and SF in turn.
    const std::pair<u64, u64> operands[] = {
        {0x8000000000000001ull, 0x7FFFFFFFFFFFFFFFull},
        {0x0000000000000005ull, 0x0000000000000005ull},
        {0x00000000FFFFFFFFull, 0x0000000080000001ull},
    };
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100, 0x0123456789ABCDEFull));
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x108, 0xFEDCBA9876543210ull));

    GuestAddr code_at = kCode64;
    for (const Form& form : forms) {
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, form.bytes.data(),
                                          form.bytes.size()));
        BasicBlock block = build64(form.bytes);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        // The raise exit's ADR X17, . (JitContext::raise_block_pc) must not
        // cost the block its hot/cold split: the software-fault paths below
        // then also prove the recovery finds the block from a COLD address.
        EXPECT_NE(compiled->cold_size, 0u) << form.name << ": not split";
        GuestAddr fault_rip = code_at;
        for (u32 i = 0; i < form.fault_index; ++i) {
            fault_rip += block.instructions[i].length;
        }
        for (const Path& path : paths) {
            for (const auto& [rax, rcx] : operands) {
                SCOPED_TRACE(::testing::Message()
                             << form.name << " / " << path.name << " rax=0x"
                             << std::hex << rax << " rcx=0x" << rcx);
                CpuState seed = cpu64(path.rbx);
                seed.rip = code_at;
                seed.gpr.rax = rax;
                seed.gpr.set64(1, rcx);
                seed.gpr.set64(2, 0x2222222222222222ull);
                seed.gpr.set64(6, 0x6666666666666666ull);
                seed.gpr.set64(7, 0x7777777777777777ull);
                seed.gpr.set64(8, 0x0000000100000000ull);
                seed.flags.rflags = 0x2u;
                CpuState jcpu = seed;
                EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                          JitExecResult::Continue);
                CpuState icpu = seed;
                Interpreter interp(icpu, *m_memory);
                (void)interp.run(path.faults ? form.fault_index
                                             : block.instruction_count);
                icpu.materialize_lazy_flags();
                jcpu.materialize_lazy_flags();
                if (path.faults) {
                    ASSERT_TRUE(jcpu.pending_raise.active);
                    EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
                    EXPECT_EQ(jcpu.rip, fault_rip);
                } else {
                    EXPECT_FALSE(jcpu.pending_raise.active);
                    EXPECT_EQ(jcpu.rip, icpu.rip);
                }
                EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
                for (u8 r = 0; r < 16; ++r) {
                    EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r)) << "r" << int(r);
                }
            }
        }
    }
}

// SETcc / CMOVcc between a publisher and its killer read host NZCV and the
// publisher's record is elided (flag_liveness nzcv_reader_cc). Intel SDM Vol. 2
// SETcc / CMOVcc define the results from EFLAGS of the CMP/TEST/ADD before
// them; each block runs against the interpreter on operand pairs that make
// every condition both true and false. The forms include unpinned R8/R9
// destinations, AH (no REX), the 32-bit CMOV zero-extension, and readers the
// analysis must not cross (parity, CF after TEST, an OR publisher).
TEST_F(JitLinear64Test, SetccAndCmovBetweenPublisherAndKillerMatchInterpreter) {
    const std::vector<std::vector<u8>> forms = {
        {0x39, 0xC8, 0x0F, 0x94, 0xC0, 0x0F, 0x9F, 0xC2, 0x83, 0xC6, 0x01},
        {0x48, 0x39, 0xC8, 0x41, 0x0F, 0x92, 0xC0, 0x41, 0x0F, 0x97, 0xC1,
         0x48, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x4C, 0xC1, 0x0F, 0x4F, 0xD1, 0x83, 0xC6, 0x01},
        {0x48, 0x39, 0xC8, 0x4C, 0x0F, 0x42, 0xC1, 0x4C, 0x0F, 0x47, 0xC8,
         0x48, 0x83, 0xC6, 0x01},
        {0x85, 0xC0, 0x0F, 0x98, 0xC0, 0x0F, 0x94, 0xC1, 0x83, 0xC6, 0x01},
        {0x01, 0xC8, 0x0F, 0x90, 0xC0, 0x0F, 0x92, 0xC2, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x94, 0xC4, 0x0F, 0x9C, 0xC7, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x4E, 0xC1, 0x0F, 0x9E, 0xC2, 0x48, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x9A, 0xC0, 0x83, 0xC6, 0x01},
        {0x85, 0xC8, 0x0F, 0x92, 0xC0, 0x0F, 0x46, 0xD1, 0x83, 0xC6, 0x01},
        {0x09, 0xC8, 0x0F, 0x94, 0xC0, 0x83, 0xC6, 0x01},
        {0x39, 0xC8, 0x0F, 0x94, 0xC0, 0x48, 0x8B, 0x13, 0x83, 0xC6, 0x01},
        {0x48, 0x83, 0xF8, 0x05, 0x0F, 0x9C, 0xC0, 0x48, 0x0F, 0x4D, 0xD1,
         0x48, 0x29, 0xCE},
        // Loads between the compare and its readers: mov rdx,[rbx];
        // movzx edi,byte [rbx+1]; then sete/cmovl/setb.
        {0x39, 0xC8, 0x48, 0x8B, 0x13, 0x0F, 0x94, 0xC0, 0x83, 0xC6, 0x01},
        {0x48, 0x39, 0xC8, 0x0F, 0xB6, 0x7B, 0x01, 0x0F, 0x4C, 0xC1,
         0x48, 0x8B, 0x13, 0x0F, 0x92, 0xC2, 0x83, 0xC6, 0x01},
    };
    // The loads above read [rbx]; run every block once with RBX in a plain
    // page (inline load) and once in a Mixed host page, whose read takes the
    // checked helper: its cold stub must hand the readers intact NZCV.
    ASSERT_TRUE(m_memory->protect_region(kData64 + 0x1000, 0x1000,
                                         MemoryProtection::None));
    constexpr GuestAddr kPlain = kData64 + 0x10000;
    ASSERT_TRUE(m_memory->map_region(kPlain, 0x4000, MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->write_u64(kPlain + 0x100, 0x1122334455667788ull));
    const GuestAddr rbx_values[] = {kPlain + 0x100, kData64 + 0x100};
    const std::pair<u64, u64> operands[] = {
        {5, 5}, {3, 7}, {7, 3},
        {0x80000000ull, 0x7FFFFFFFull},
        {0xFFFFFFFF80000000ull, 1},
        {0x7FFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull},
        {0, 0},
    };
    ASSERT_TRUE(m_memory->write_u64(kData64 + 0x100, 0x1122334455667788ull));
    GuestAddr code_at = kCode64;
    for (const auto& code : forms) {
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        for (const GuestAddr rbx : rbx_values)
        for (const auto& [rax, rcx] : operands) {
            SCOPED_TRACE(::testing::Message()
                         << "block 0x" << std::hex << code_at << " rax=0x" << rax
                         << " rcx=0x" << rcx << " rbx=0x" << rbx);
            CpuState seed = cpu64(rbx);
            seed.rip = code_at;
            seed.gpr.rax = rax;
            seed.gpr.set64(1, rcx);
            seed.gpr.set64(2, 0xDDDDDDDDDDDDDDDDull);
            seed.gpr.set64(6, 0x6666666666666666ull);
            seed.gpr.set64(7, 0x7777777777777777ull);
            seed.gpr.set64(8, 0x8888888888888888ull);
            seed.gpr.set64(9, 0x9999999999999999ull);
            seed.flags.rflags = 0x8D7u;
            CpuState jcpu = seed;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
            EXPECT_FALSE(jcpu.pending_raise.active);
            CpuState icpu = seed;
            Interpreter interp(icpu, *m_memory);
            (void)interp.run(block.instruction_count);
            icpu.materialize_lazy_flags();
            jcpu.materialize_lazy_flags();
            EXPECT_EQ(jcpu.rip, icpu.rip);
            EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
            for (u8 r = 0; r < 16; ++r) {
                EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r)) << "r" << int(r);
            }
        }
    }
}

// A Jcc at an internal join reads flags from two publishers: a lexicographic
// pair compare (from a real x64 image) where JNE skips the second CMP and
// lands on the JB. Intel SDM Vol. 2 Jcc: JB tests the CF of whichever CMP ran
// last on the path taken. The second CMP's record must therefore exist when
// JB reads it, however the block is compiled; the interpreter is the oracle.
TEST_F(JitLinear64Test, JccAtAnInternalJoinReadsTheLastPublisherOnEachPath) {
    const std::vector<u8> code = {
        0x48, 0x39, 0xC6,        // cmp rsi, rax
        0x74, 0x2D,              // je  +0x2d (out)
        0x8B, 0x06,              // mov eax, [rsi]
        0x41, 0x39, 0x01,        // cmp [r9], eax
        0x75, 0x07,              // jne -> jb below
        0x41, 0x8B, 0x41, 0x04,  // mov eax, [r9+4]
        0x3B, 0x46, 0x04,        // cmp eax, [rsi+4]
        0x72, 0x1D,              // jb  +0x1d (out)
        0x48, 0x83, 0xC6, 0x08,  // add rsi, 8
        0x48, 0x89, 0xF0,        // mov rax, rsi
    };
    const GuestAddr code_at = kCode64 + 0x40;
    const GuestAddr a = kData64 + 0x200;
    const GuestAddr b = kData64 + 0x300;
    ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
    BasicBlock block = build64(code);
    block.start_addr = code_at;
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    struct Pair { u32 a0, a1, b0, b1; };
    unsigned taken = 0, fell = 0;
    for (const Pair p : {Pair{5, 9, 5, 3}, Pair{5, 3, 5, 9}, Pair{5, 9, 5, 9},
                         Pair{5, 0, 4, 0}, Pair{4, 0, 5, 0},
                         Pair{0x80000000u, 1, 0x80000000u, 0xFFFFFFFFu},
                         Pair{7, 0xFFFFFFFFu, 7, 0}}) {
        SCOPED_TRACE(::testing::Message() << std::hex << "a=" << p.a0 << ","
                     << p.a1 << " b=" << p.b0 << "," << p.b1);
        ASSERT_TRUE(m_memory->write_u32(a, p.a0).is_ok());
        ASSERT_TRUE(m_memory->write_u32(a + 4, p.a1).is_ok());
        ASSERT_TRUE(m_memory->write_u32(b, p.b0).is_ok());
        ASSERT_TRUE(m_memory->write_u32(b + 4, p.b1).is_ok());
        CpuState seed = cpu64(0);
        seed.rip = code_at;
        seed.gpr.rax = 0x1234;
        seed.gpr.rsi = a;
        seed.gpr.r9 = b;
        seed.flags.rflags = 0x8D7u;
        CpuState jcpu = seed;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                  JitExecResult::Continue);
        CpuState icpu = seed;
        Interpreter interp(icpu, *m_memory);
        for (u32 i = 0; i < block.instruction_count; ++i) {
            if (icpu.rip < code_at || icpu.rip >= code_at + code.size()) break;
            (void)interp.step();
        }
        (icpu.rip == code_at + 0x13 + 2 + 0x1D ? taken : fell)++;
        icpu.materialize_lazy_flags();
        jcpu.materialize_lazy_flags();
        EXPECT_EQ(jcpu.rip, icpu.rip);
        EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
        for (u8 r = 0; r < 16; ++r)
            EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r)) << "r" << int(r);
    }
    EXPECT_GT(taken, 1u);
    EXPECT_GT(fell, 1u);
}

// FMA NaN propagation, against Windows (tools/oracle_probe/
// fma_nan_priority_probe.expected, Alder Lake): when several sources are
// NaNs, x86 returns the quieted NaN of the FIRST MULTIPLICAND, then the
// second, then the addend -- 132: op1 > op3 > op2, 213: op2 > op1 > op3,
// 231: op2 > op3 > op1 -- signalling or not; a propagated NaN keeps its sign
// through every negated form; an invalid result is the 0xFFC00000 /
// 0xFFF8000000000000 indefinite. Both engines must match the table, so this
// also pins the interpreter, which the parity tests treat as the reference.
TEST_F(JitLinear64Test, FmaNanPropagationMatchesOracle) {
    struct Form {
        const char* name;
        std::vector<u8> code;  // op1 = xmm0 (dst), op2 = xmm1 (vvvv), op3 = xmm2 (rm)
        bool dbl;
        int winner[8];         // by NaN-source mask (bit i = op(i+1)); 0-based op
    };
    const Form forms[] = {
        {"132ps", {0xC4, 0xE2, 0x71, 0x98, 0xC2}, false, {-1, 0, 1, 0, 2, 0, 2, 0}},
        {"213ps", {0xC4, 0xE2, 0x71, 0xA8, 0xC2}, false, {-1, 0, 1, 1, 2, 0, 1, 1}},
        {"231ps", {0xC4, 0xE2, 0x71, 0xB8, 0xC2}, false, {-1, 0, 1, 1, 2, 2, 1, 1}},
        {"132pd", {0xC4, 0xE2, 0xF1, 0x98, 0xC2}, true, {-1, 0, 1, 0, 2, 0, 2, 0}},
        {"213pd", {0xC4, 0xE2, 0xF1, 0xA8, 0xC2}, true, {-1, 0, 1, 1, 2, 0, 1, 1}},
        {"231pd", {0xC4, 0xE2, 0xF1, 0xB8, 0xC2}, true, {-1, 0, 1, 1, 2, 2, 1, 1}},
    };
    const u32 q32[3] = {0x7FC000A1u, 0x7FC000B2u, 0x7FC000C3u};
    const u32 s32[3] = {0x7F8000A1u, 0x7F8000B2u, 0x7F8000C3u};
    const u64 q64[3] = {0x7FF80000000000A1ull, 0x7FF80000000000B2ull,
                        0x7FF80000000000C3ull};
    const u64 s64[3] = {0x7FF00000000000A1ull, 0x7FF00000000000B2ull,
                        0x7FF00000000000C3ull};
    GuestAddr code_at = kCode64;
    const auto run_both = [&](const std::vector<u8>& code, const u64 lane[3],
                              bool dbl, u64& jit_out, u64& interp_out) {
        code_at += 0x40;
        ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);
        CpuState seed = cpu64(0);
        seed.rip = code_at;
        for (int r = 0; r < 3; ++r) {
            for (int i = 0; i < (dbl ? 2 : 4); ++i) {
                if (dbl) seed.sse.xmm[r].u64_[i] = lane[r];
                else seed.sse.xmm[r].u32_[i] = static_cast<u32>(lane[r]);
            }
        }
        CpuState jcpu = seed;
        CpuState icpu = seed;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu), JitExecResult::Continue);
        Interpreter interp(icpu, *m_memory);
        (void)interp.run(1);
        jit_out = dbl ? jcpu.sse.xmm[0].u64_[0] : jcpu.sse.xmm[0].u32_[0];
        interp_out = dbl ? icpu.sse.xmm[0].u64_[0] : icpu.sse.xmm[0].u32_[0];
    };
    for (const Form& f : forms) {
        for (int sig = 0; sig < 2; ++sig) {
            for (int mask = 1; mask < 8; ++mask) {
                SCOPED_TRACE(::testing::Message() << f.name << (sig ? " snan" : " qnan")
                                                  << " mask=" << mask);
                u64 lane[3];
                for (int i = 0; i < 3; ++i) {
                    const bool nan = ((mask >> i) & 1) != 0;
                    lane[i] = f.dbl ? (nan ? (sig ? s64[i] : q64[i])
                                           : 0x3FF8000000000000ull)   // 1.5
                                    : (nan ? (sig ? s32[i] : q32[i])
                                           : 0x3FC00000u);            // 1.5f
                }
                const int w = f.winner[mask];
                const u64 want = f.dbl ? q64[w] : q32[w];
                u64 jit = 0, interp = 0;
                run_both(f.code, lane, f.dbl, jit, interp);
                EXPECT_EQ(jit, want) << std::hex << "jit " << jit;
                EXPECT_EQ(interp, want) << std::hex << "interp " << interp;
            }
        }
    }
    // inf * 0 + 1.5: the indefinite, sign set.
    {
        const u64 ps[3] = {0x3FC00000u, 0x7F800000u, 0};
        const u64 pd[3] = {0x3FF8000000000000ull, 0x7FF0000000000000ull, 0};
        u64 jit = 0, interp = 0;
        run_both(forms[2].code, ps, false, jit, interp);
        EXPECT_EQ(jit, 0xFFC00000u);
        EXPECT_EQ(interp, 0xFFC00000u);
        run_both(forms[5].code, pd, true, jit, interp);
        EXPECT_EQ(jit, 0xFFF8000000000000ull);
        EXPECT_EQ(interp, 0xFFF8000000000000ull);
    }
    // VFNMSUB231PS = -(op2*op3) - op1: a propagated NaN keeps its own sign.
    const std::vector<u8> fnmsub = {0xC4, 0xE2, 0x71, 0xBE, 0xC2};
    for (int src = 0; src < 3; ++src) {
        for (int sign = 0; sign < 2; ++sign) {
            SCOPED_TRACE(::testing::Message() << "vfnmsub231ps op" << src + 1
                                              << " sign=" << sign);
            u64 lane[3] = {0x3FC00000u, 0x3FC00000u, 0x3FC00000u};
            lane[src] = q32[src] | (sign ? 0x80000000u : 0u);
            u64 jit = 0, interp = 0;
            run_both(fnmsub, lane, false, jit, interp);
            EXPECT_EQ(jit, lane[src]) << std::hex << jit;
            EXPECT_EQ(interp, lane[src]) << std::hex << interp;
        }
    }
}

}  // namespace
