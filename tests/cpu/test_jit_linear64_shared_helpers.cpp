// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// The process-wide linear64 dispatch tails and the shared cold helper. Split
// from test_jit_linear.cpp; shared fixtures and matchers live in
// jit_linear_fixture.hpp.

#include "jit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test;

namespace {

// Intel SDM Vol. 3A 6.6 through the process-wide scalar helper that every
// site's cold stub calls (VITREO_JIT_COLD_STUB_LEGACY unset): a fault is
// precise -- RIP names the faulting instruction, registers and RFLAGS are
// what the instruction before it left, and a faulting store commits nothing.
// `add rcx,rdx ; <access [rbx]> ; inc rsi` runs for every scalar width and
// the sign/zero-extending loads against a Mixed host page (guest pages 0 and
// 1 RW, 2 NOACCESS, 3 read-only) followed by unmapped memory: inside an
// allowed page, unaligned across a 16-byte granule and a guest page, from
// read-only, from the denied page, straddling into it, and across the host
// page into unmapped memory. Every outcome, including the AV's
// ExceptionInformation, is the interpreter's. Each case runs twice: the
// second run executes any ordered access the first rewrote in place.
// x86 guests do not reach these stubs (their linear model has its own store
// stubs and no hardware protection by default; run.cpp).
// The process-wide dispatcher tails and entry prologue of a standard-pin x64
// block (JitContext::dispatch_tail) leave exactly the architectural state the
// per-block copies did: the interpreter's GPRs, RIP, flags and exception for
// a Continue exit with live and with carried flags, a precise raise, and an
// indirect RET exit, under both emissions.
TEST_F(JitLinear64Test, SharedDispatchTailsMatchInterpreterInBothModes) {
    struct Restore {
        ~Restore() { JitCompiler::set_shared_exit_stubs_for_test(-1); }
    } restore;
    const GuestAddr data = kData64;
    ASSERT_TRUE(m_memory->protect_region(data + 0x2000, 0x1000,
                                         MemoryProtection::None));
    struct Form {
        const char* name;
        std::vector<u8> code;
        GuestAddr rbx;
    };
    const Form forms[] = {
        // add rcx,rdx ; mov [rbx],rax ; cmp rsi,rdi ; jmp +0x20
        {"store then cmp, jmp exit",
         {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x39, 0xFE, 0xEB, 0x20},
         data + 0x10},
        {"store faults",
         {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x39, 0xFE, 0xEB, 0x20},
         data + 0x2010},
        // sub rsi,rdi ; mov rax,[rbx] ; jne +0x30 ; inc rcx ; jmp +0x40
        {"load, jcc legs",
         {0x48, 0x29, 0xFE, 0x48, 0x8B, 0x03, 0x75, 0x30, 0x48, 0xFF, 0xC1,
          0xEB, 0x40},
         data + 0x18},
        // mov rsp,rbx ; add rcx,rdx ; ret
        {"ret", {0x48, 0x89, 0xDC, 0x48, 0x01, 0xD1, 0xC3}, data + 0x20},
    };
    ASSERT_TRUE(m_memory->write_u64(data + 0x20, 0x0000000140001234ull));
    const u32 shared_entry = arm64::ldr_x_uoff(
        16, REG_JIT_CTX,
        (static_cast<u32>(offsetof(JitContext, dispatch_tail)) >> 3) +
            4 /* kEntryCheckMap */);
    GuestAddr code_at = kCode64 + 0x1000;
    for (int mode = 0; mode < 2; ++mode) {
        JitCompiler::set_shared_exit_stubs_for_test(mode);
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        for (const Form& f : forms) {
            SCOPED_TRACE(::testing::Message() << f.name << " shared=" << mode);
            code_at += 0x80;
            ASSERT_TRUE(m_memory->write_bytes(code_at, f.code.data(), f.code.size()));
            BasicBlock block = build64(f.code);
            block.start_addr = code_at;
            CompiledBlock* compiled = m_jit->compile(block);
            ASSERT_NE(compiled, nullptr);
            const auto* words = reinterpret_cast<const u32*>(compiled->code);
            EXPECT_EQ(words[1] == shared_entry, mode == 1)
                << "dispatcher entry is not the shared prologue";

            CpuState seed = cpu64(f.rbx);
            seed.rip = code_at;
            for (u8 g = 0; g < 16; ++g) {
                if (g == ESP || g == EBX) continue;
                seed.gpr.set64(g, 0x8F7E6D5C4B3A2918ull + 0x0102030405060708ull * g);
            }
            seed.flags.rflags = 0x202u;
            CpuState jcpu = seed;
            EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                      JitExecResult::Continue);
            CpuState icpu = seed;
            Interpreter interp(icpu, *m_memory);
            // Step only while RIP is inside the block: a taken Jcc leg leaves.
            bool ifault = false;
            for (u32 i = 0; i < block.instruction_count && !ifault &&
                            icpu.rip >= code_at &&
                            icpu.rip < code_at + f.code.size();
                 ++i) {
                ifault = interp.run(1) == ExecResult::Exception;
            }
            ASSERT_EQ(jcpu.pending_raise.active, ifault);
            if (ifault) EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
            icpu.materialize_lazy_flags();
            jcpu.materialize_lazy_flags();
            EXPECT_EQ(jcpu.rip, icpu.rip);
            EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
            for (u8 g = 0; g < 16; ++g) {
                EXPECT_EQ(jcpu.gpr.get64(g), icpu.gpr.get64(g)) << "r" << int(g);
            }
        }
    }
}

TEST_F(JitLinear64Test, SharedColdHelperFaultsArePreciseAgainstInterpreter) {
    struct Form {
        std::vector<u8> code;
        int width;
        bool store;
    };
    const Form forms[] = {
        {{0x8A, 0x03}, 1, false},              // mov al,[rbx]
        {{0x66, 0x8B, 0x03}, 2, false},        // mov ax,[rbx]
        {{0x8B, 0x03}, 4, false},              // mov eax,[rbx]
        {{0x48, 0x8B, 0x03}, 8, false},        // mov rax,[rbx]
        {{0x48, 0x0F, 0xBE, 0x03}, 1, false},  // movsx rax,byte [rbx]
        {{0x0F, 0xB7, 0x03}, 2, false},        // movzx eax,word [rbx]
        {{0x48, 0x63, 0x03}, 4, false},        // movsxd rax,dword [rbx]
        {{0x88, 0x03}, 1, true},               // mov [rbx],al
        {{0x66, 0x89, 0x03}, 2, true},         // mov [rbx],ax
        {{0x89, 0x03}, 4, true},               // mov [rbx],eax
        {{0x48, 0x89, 0x03}, 8, true},         // mov [rbx],rax
    };
    const GuestAddr data = kData64;
    std::vector<u8> pattern(0x4000);
    for (usize i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u8>(0x80u ^ (i * 37u) ^ (i >> 7));
    }
    ASSERT_TRUE(m_memory->write_bytes(data, pattern.data(), pattern.size()));
    ASSERT_TRUE(m_memory->protect_region(data + 0x2000, 0x1000,
                                         MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(data + 0x3000, 0x1000,
                                         MemoryProtection::Read));
    ASSERT_NE(m_memory->linear_check_page_map()[data >> kLinearHostPageShift] &
                  kLinearCheckMixedRead,
              0u);
    const auto reset = [&] {
        ASSERT_TRUE(m_memory->write_bytes(data, pattern.data(), 0x2000));
    };
    const auto snapshot = [&] {
        std::vector<u8> out(0x2000);
        EXPECT_TRUE(m_memory->read_bytes(data, out.data(), out.size()));
        return out;
    };
    GuestAddr code_at = kCode64 + 0x2000;
    const auto compile_at = [&](const std::vector<u8>& code) {
        code_at += 0x40;
        EXPECT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
        BasicBlock block = build64(code);
        block.start_addr = code_at;
        return block;
    };

    for (const Form& f : forms) {
        std::vector<u8> code = {0x48, 0x01, 0xD1};  // add rcx,rdx
        code.insert(code.end(), f.code.begin(), f.code.end());
        code.insert(code.end(), {0x48, 0xFF, 0xC6});  // inc rsi
        const BasicBlock block = compile_at(code);
        ASSERT_EQ(block.instruction_count, 3u);
        CompiledBlock* compiled = m_jit->compile(block);
        ASSERT_NE(compiled, nullptr);

        const int w = f.width;
        const GuestAddr half = static_cast<GuestAddr>(w > 1 ? w / 2 : 0);
        const GuestAddr cases[] = {
            data + 0x0010,         // allowed
            data + 0x1000 - half,  // unaligned across granule and guest page
            data + 0x3008,         // read-only page
            data + 0x2010,         // denied page
            data + 0x2000 - half,  // straddles into the denied page
            data + 0x4000 - half,  // read-only page into unmapped memory
        };
        for (const GuestAddr at : cases) {
            if (w == 1 && (at == data + 0x2000 || at == data + 0x4000)) continue;
            for (int pass = 0; pass < 2; ++pass) {
                SCOPED_TRACE(::testing::Message()
                             << "width=" << w << " store=" << f.store
                             << " form=" << (&f - forms) << " at=0x"
                             << std::hex << at << " pass=" << pass);
                CpuState seed = cpu64(at);
                seed.rip = code_at;
                for (u8 g = 0; g < 16; ++g) {
                    if (g == ESP || g == EBX) continue;
                    seed.gpr.set64(g, 0x8F7E6D5C4B3A2918ull +
                                          0x0102030405060708ull * g);
                }
                seed.flags.rflags = 0x202u;

                reset();
                CpuState jcpu = seed;
                EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, jcpu),
                          JitExecResult::Continue);
                const std::vector<u8> jmem = snapshot();

                reset();
                CpuState icpu = seed;
                Interpreter interp(icpu, *m_memory);
                const bool ifault =
                    interp.run(block.instruction_count) == ExecResult::Exception;
                const std::vector<u8> imem = snapshot();

                ASSERT_EQ(jcpu.pending_raise.active, ifault);
                if (ifault) {
                    EXPECT_EQ(jcpu.pending_raise.code, 0xC0000005u);
                    ASSERT_EQ(jcpu.pending_raise.num_args, 2u);
                    EXPECT_EQ(jcpu.pending_raise.args[0],
                              interp.context().exception_error);
                    EXPECT_EQ(jcpu.pending_raise.args[1],
                              interp.context().exception_addr);
                }
                icpu.materialize_lazy_flags();
                jcpu.materialize_lazy_flags();
                EXPECT_EQ(jcpu.rip, icpu.rip);
                EXPECT_EQ(jcpu.flags.rflags & 0x8D5u, icpu.flags.rflags & 0x8D5u);
                for (u8 g = 0; g < 16; ++g) {
                    EXPECT_EQ(jcpu.gpr.get64(g), icpu.gpr.get64(g)) << "r" << int(g);
                }
                EXPECT_TRUE(jmem == imem) << "guest memory differs";
            }
        }
    }

    // White box: a hardware fault INSIDE the shared helper's direct access.
    // Make the host page read-only (guest pages 0, 1, 3 read-only, 2
    // NOACCESS) and grant guest page 3 for writing in the sub-page map alone:
    // the helper then stores to the read-only host page itself, and the fault
    // handler must attribute the fault to the calling site through the
    // helper's return address, as a precise write AV.
    if (const char* knob = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        knob != nullptr && knob[0] == '0') {
        return;
    }
    ASSERT_TRUE(m_memory->protect_region(data, 0x2000, MemoryProtection::Read));
    u8* sub = const_cast<u8*>(m_memory->linear_subpage_map());
    const usize page = data >> kLinearHostPageShift;
    const u8 saved = sub[page];
    sub[page] = static_cast<u8>(saved | 0x80u);  // write bit of guest page 3
    // add rcx,rdx ; mov [rbx],rax ; add rsi,rdi. The last ADD overwrites
    // every flag, so the first ADD's flag record may be elided and rebuilt
    // only from the block's recovery descriptor -- which the handler finds
    // only by attributing the helper's fault to this block.
    const BasicBlock block = compile_at(
        {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x01, 0xFE});
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    CpuState cpu = cpu64(data + 0x3010);
    cpu.rip = code_at;
    cpu.gpr.rcx = 0xFFFFFFFFu;
    cpu.gpr.rdx = 1;
    cpu.gpr.set64(EAX, 0x1111111122222222ull);
    cpu.flags.rflags = 0x202u;
    EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
              JitExecResult::Continue);
    sub[page] = saved;
    ASSERT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
    EXPECT_EQ(cpu.pending_raise.args[0], 1u);
    EXPECT_EQ(cpu.pending_raise.args[1], data + 0x3010);
    EXPECT_EQ(cpu.rip, code_at + 3u);
    cpu.materialize_lazy_flags();
    // SDM ADD: 0xFFFFFFFF + 1 = 0x100000000 in 64 bits; AF (carry out of
    // bit 3) and PF (low byte 0x00) set, CF/ZF/SF/OF clear.
    EXPECT_EQ(cpu.gpr.rcx, 0x100000000ull);
    EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x014u);
    std::array<u8, 4> stored{};
    ASSERT_TRUE(m_memory->read_bytes(data + 0x3010, stored.data(), stored.size()));
    EXPECT_TRUE(std::equal(stored.begin(), stored.end(), pattern.begin() + 0x3010));
}

// The white-box case above, run twice in a fresh block. On the first run the
// helper's ordered store first takes the ordered-fault retry, whose
// trampoline returns to the access with X30 = the retry PC, inside the helper
// page; the shared helper keeps its caller's return address in X3 across the
// access for exactly that reason. The real fault that follows must still be
// attributed to the calling block through X3, or the elided ADD record is
// lost (flags 0). The second run, already patched, enters with X30 intact.
TEST_F(JitLinear64Test, SharedHelperFaultAfterAnOrderedRetryIsAttributedToTheCaller) {
    if (const char* knob = std::getenv("VITREO_JIT_SUBPAGE_STUB");
        knob != nullptr && knob[0] == '0') {
        GTEST_SKIP();
    }
    const GuestAddr data = kData64;
    std::vector<u8> pattern(0x4000);
    for (usize i = 0; i < pattern.size(); ++i) {
        pattern[i] = static_cast<u8>(0x80u ^ (i * 37u) ^ (i >> 7));
    }
    ASSERT_TRUE(m_memory->write_bytes(data, pattern.data(), pattern.size()));
    ASSERT_TRUE(m_memory->protect_region(data + 0x2000, 0x1000, MemoryProtection::None));
    ASSERT_TRUE(m_memory->protect_region(data, 0x2000, MemoryProtection::Read));
    ASSERT_TRUE(m_memory->protect_region(data + 0x3000, 0x1000, MemoryProtection::Read));
    u8* sub = const_cast<u8*>(m_memory->linear_subpage_map());
    const usize page = data >> kLinearHostPageShift;
    const u8 saved = sub[page];
    sub[page] = static_cast<u8>(saved | 0x80u);  // write bit of guest page 3
    const GuestAddr code_at = kCode64 + 0x3800;
    const std::vector<u8> code = {0x48, 0x01, 0xD1, 0x48, 0x89, 0x03, 0x48, 0x01, 0xFE};
    ASSERT_TRUE(m_memory->write_bytes(code_at, code.data(), code.size()));
    BasicBlock block = build64(code);
    block.start_addr = code_at;
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    for (int pass = 0; pass < 2; ++pass) {
        SCOPED_TRACE(pass == 0 ? "first run (ordered retry)" : "second run");
        CpuState cpu = cpu64(data + 0x3010);
        cpu.rip = code_at;
        cpu.gpr.rcx = 0xFFFFFFFFu;
        cpu.gpr.rdx = 1;
        cpu.gpr.set64(EAX, 0x1111111122222222ull);
        cpu.flags.rflags = 0x202u;
        EXPECT_EQ(test::execute_like_dispatcher(*m_jit, *compiled, cpu),
                  JitExecResult::Continue);
        ASSERT_TRUE(cpu.pending_raise.active);
        EXPECT_EQ(cpu.pending_raise.code, 0xC0000005u);
        EXPECT_EQ(cpu.pending_raise.args[0], 1u);
        EXPECT_EQ(cpu.pending_raise.args[1], data + 0x3010);
        EXPECT_EQ(cpu.rip, code_at + 3u);
        cpu.materialize_lazy_flags();
        EXPECT_EQ(cpu.gpr.rcx, 0x100000000ull);
        EXPECT_EQ(cpu.flags.rflags & 0x8D5u, 0x014u) << "the elided ADD record";
    }
    sub[page] = saved;
}

}  // namespace
