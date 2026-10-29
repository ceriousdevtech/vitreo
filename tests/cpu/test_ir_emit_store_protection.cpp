// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file test_ir_emit_store_protection.cpp
/// @brief Tier 1 IR emission parity: read-only stores and helper raises; contract in test_ir_emit.cpp.

#include "ir_emit_linear_fixture.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::test::ir_emit;

// Does a guest store to a READ-ONLY page reach the helper-raise path?
//
// This matters to the flag-recovery elision. The linear store's cold stub
// calls the write helper and then `emit_exit_if_pending_raise`, so a write the
// helper refuses leaves the block through the shared raise stub — which
// persists live NZCV but knows nothing about recovery plans and cannot rebuild
// an elided record. The SIGSEGV handler, which does rebuild, is not involved.
//
// The test is written as a PARITY check of one store, stepping the interpreter
// exactly as many times as there are instructions. An earlier version looped a
// fixed four times over a three-instruction block, so the interpreter's fourth
// step ran whatever followed in memory and "faulted" on garbage — which made
// the test report a divergence that was its own.
//
// This FAILED when written: the interpreter refused the store and the JIT
// accepted it, because the 32-bit linear model mapped every committed host
// page RW and the store fast path's check byte carries no protection bit. It
// passes now that the 32-bit model enables hardware protection, as the x64
// model already did.
TEST_F(IrEmitFaultRecovery, StoreToAReadOnlyPageMatchesTheInterpreter) {
    constexpr GuestAddr kRo = kLinData + 0x2000;
    ASSERT_TRUE(m_memory->map_region(kRo, 0x1000, MemoryProtection::Read));

    const std::vector<u8> code = {0x89, 0x1A};   // mov [edx], ebx
    ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));

    CpuState interp_cpu;
    seed(interp_cpu, kRo);
    ExecResult interp_result;
    {
        Interpreter interp(interp_cpu, *m_memory);
        interp_result = interp.step();          // exactly the one store
    }
    const bool interpreter_refused = interp_result != ExecResult::Continue;

    CpuState jit_cpu;
    seed(jit_cpu, kRo);
    BasicBlock block = decode_at(code);
    ASSERT_TRUE(m_jit->ir_build_block(block).ok);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_jit->ir_emit_active());
    m_jit->execute(*compiled, jit_cpu);
    const bool jit_refused = jit_cpu.pending_raise.active;

    std::printf("[measure] store to a Read-only page: interpreter %s, "
                "Tier 1 %s\n",
                interpreter_refused ? "REFUSED" : "accepted",
                jit_refused ? "REFUSED" : "accepted");

    EXPECT_EQ(jit_refused, interpreter_refused)
        << "the JIT and the interpreter must agree on whether a store to a "
           "read-only guest page is allowed";
}

// Is the ORACLE itself consistent about writes to a read-only guest page?
//
// `GuestMemory::write_u32` refuses one, but several interpreter store paths
// take a `host_ptr_mut` fast path instead, and `host_ptr_mut` passes nullptr
// for its `writable` out-parameter — it resolves the address and never
// consults the protection. So the read-modify-write forms may disagree with
// the plain store. Measured rather than assumed, because the interpreter is
// the semantic oracle and an inconsistent oracle cannot be used to judge the
// JIT.
TEST(GuestProtection, GuestStoresToAReadOnlyPageAreRefusedNotCrashes) {
    struct Case { const char* what; std::vector<u8> code; };
    const Case cases[] = {
        {"mov [edx],eax",   {0x89, 0x02}},
        {"add [edx],eax",   {0x01, 0x02}},
        {"xor [edx],eax",   {0x31, 0x02}},
        {"inc dword [edx]", {0xFF, 0x02}},
        {"xchg [edx],eax",  {0x87, 0x02}},
        {"lock add [edx],eax",  {0xF0, 0x01, 0x02}},
        {"lock xchg [edx],eax", {0xF0, 0x87, 0x02}},
        {"lock cmpxchg [edx],eax", {0xF0, 0x0F, 0xB1, 0x02}},
        {"lock xor [edx],eax",  {0xF0, 0x31, 0x02}},
        {"lock inc dword [edx]",{0xF0, 0xFF, 0x02}},
    };
    for (const Case& c : cases) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x1000, 0x1000,
                                      MemoryProtection::ReadWrite));
        ASSERT_TRUE(memory.map_region(0x20000, 0x1000,
                                      MemoryProtection::Read));
        ASSERT_TRUE(memory.write_bytes(kCode, c.code.data(), c.code.size()));

        CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.set32(0, 0xAABB'CCDDu);
        cpu.gpr.set32(2, 0x20000);
        cpu.gpr.set32(4, 0x1800);
        cpu.rip = kCode;

        Interpreter interp(cpu, memory);
        const ExecResult r = interp.step();
        const auto after = memory.read_u32(0x20000);
        ASSERT_TRUE(after.is_ok());
        std::printf("[measure] %-24s -> %-9s memory 0x%08x%s\n",
                    c.what,
                    r == ExecResult::Continue ? "Continue" : "Exception",
                    after.value(),
                    after.value() != 0 ? "   <-- WRITE LANDED" : "");
        // Two claims, and the second is the one that used to crash the host:
        // the store must be refused, and it must be refused rather than
        // reaching a host SIGBUS through a raw pointer.
        EXPECT_NE(r, ExecResult::Continue)
            << c.what << ": a store to a read-only guest page must not "
                         "succeed";
        EXPECT_EQ(after.value(), 0u)
            << c.what << ": the write must not have landed";
    }
}

// The mirror question for LOADS is left OPEN, deliberately and on the record.
//
// The 32-bit linear load fast path emits a bare `LDR` with no bounds test, no
// check byte and no permission test, so on paper a load from a guest page the
// guest may not read would succeed. It could not be demonstrated: neither
// `map_region` nor `protect_region` accepts `MemoryProtection::None` in this
// configuration, so a committed-but-inaccessible guest page could not be
// constructed to test against. A page that is simply not committed stays
// PROT_NONE in the reservation and faults correctly, which is the case that
// does work.
//
// So the honest state is "unmeasured", not "safe" and not "broken", and a
// test asserting either would be inventing its own premise. Recorded in
// BASELINE.md. The x64 model does test the mixed-protection bit on loads
// (`emit_linear64_check_page_test`), so if this is ever closed for 32-bit
// there is a shape to copy — and it costs a check on the hottest instruction
// in the JIT, which is a measurement, not a bug fix.

// Can an elided flag record escape through the HELPER-raise path?
//
// This is the residual risk left open when the producer landed, and enabling
// hardware protection for 32-bit may have made it reachable rather than
// theoretical. Host pages are 16 KiB and guest pages 4 KiB, so a read-only
// guest page sharing a host page with a writable one cannot be expressed in
// hardware: reconciliation relaxes the host page to RW and sets
// `kLinearCheckMixedProt`. The store then finds a nonzero check byte, takes
// the cold stub, and the write helper refuses it in software — raising through
// `emit_exit_if_pending_raise`, NOT through the SIGSEGV handler that is the
// only thing able to rebuild an elided record.
//
// If this fails, the elision is unsound for stores and the fix is to replay
// the skipped record in the cold stub (what the stencil path does with
// `m_pending_stub_store`).
TEST_F(IrEmitFaultRecovery, ElidedRecordSurvivesAHelperRefusedStore) {
    // Two 4 KiB regions inside ONE 16 KiB host page: the first writable, the
    // second read-only. That is what makes the host page mixed.
    constexpr GuestAddr kMixedBase = 0x50000;      // 16 KiB aligned
    ASSERT_TRUE(m_memory->map_region(kMixedBase, 0x1000,
                                     MemoryProtection::ReadWrite));
    constexpr GuestAddr kRo = kMixedBase + 0x1000;
    ASSERT_TRUE(m_memory->map_region(kRo, 0x1000, MemoryProtection::Read));

    // add eax,ecx ; mov [edx],ebx ; xor ecx,ecx  — the ADD's record is elided
    // across the store, and the store is refused.
    const std::vector<u8> code = {0x01, 0xC8, 0x89, 0x1A, 0x31, 0xC9};
    ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));

    CpuState interp_cpu;
    seed(interp_cpu, kRo);
    {
        Interpreter interp(interp_cpu, *m_memory);
        for (int i = 0; i < 3; ++i) {
            if (interp.step() != ExecResult::Continue) break;
        }
    }
    const u64 oracle_flags = observable_rflags(interp_cpu);

    CpuState jit_cpu;
    seed(jit_cpu, kRo);
    BasicBlock block = decode_at(code);
    ASSERT_TRUE(m_jit->ir_build_block(block).ok);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_jit->ir_emit_active());
    m_jit->execute(*compiled, jit_cpu);

    std::printf("[measure] helper-refused store, elided record: "
                "raised=%d  JIT EFLAGS=0x%llx  oracle=0x%llx\n",
                jit_cpu.pending_raise.active ? 1 : 0,
                static_cast<unsigned long long>(observable_rflags(jit_cpu)),
                static_cast<unsigned long long>(oracle_flags));

    EXPECT_TRUE(jit_cpu.pending_raise.active)
        << "the store must be refused";
    EXPECT_EQ(observable_rflags(jit_cpu), oracle_flags)
        << "EFLAGS across a HELPER-refused store with an elided record";
}

// A store-covered plan whose store faults in HARDWARE.
//
// The companion to the helper-refused case: the same shape, but the store
// address is simply unmapped, so it is a real SIGSEGV and the record is
// rebuilt by the fault handler from the recovery map rather than by the cold
// stub. Both routes have to produce the same architectural flags, and only
// having one of them tested is how the store hole survived a whole increment.
TEST_F(IrEmitFaultRecovery, ElidedRecordIsRebuiltAfterAFaultingStore) {
    expect_matches_interpreter({0x01, 0xC8,        // add eax, ecx
                                0x89, 0x1A,        // mov [edx], ebx  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided ADD record, faulting store");
}

TEST_F(IrEmitFaultRecovery, ElidedCompareRecordIsRecomputedAfterAFaultingStore) {
    // RecomputeDst through the store path: the cold stub has to redo the
    // subtraction itself, not just copy two registers.
    expect_matches_interpreter({0x39, 0xC8,        // cmp eax, ecx
                                0x89, 0x1A,        // mov [edx], ebx  <- faults
                                0x31, 0xC9},       // xor ecx, ecx
                               "elided CMP record, faulting store");
}

// The same, with the store's guest page WRITE-WATCHED rather than refused.
// The watched path takes the cold stub, does the store inline and returns
// WITHOUT calling the helper — so it must not raise, must not disturb the
// flags, and the block must finish normally. It is the case where the replay
// must NOT fire.
TEST_F(IrEmitFaultRecovery, ElidedRecordIsUnharmedByAColdStubThatDoesNotRaise) {
    constexpr GuestAddr kOk = kLinData + 0x100;
    const std::vector<u8> code = {0x01, 0xC8,        // add eax, ecx
                                  0x89, 0x1A,        // mov [edx], ebx
                                  0x31, 0xC9};       // xor ecx, ecx
    ASSERT_TRUE(m_memory->write_bytes(kLinCode, code.data(), code.size()));

    CpuState interp_cpu;
    seed(interp_cpu, kOk);
    {
        Interpreter interp(interp_cpu, *m_memory);
        for (int i = 0; i < 3; ++i) {
            if (interp.step() != ExecResult::Continue) break;
        }
    }

    CpuState jit_cpu;
    seed(jit_cpu, kOk);
    BasicBlock block = decode_at(code);
    ASSERT_TRUE(m_jit->ir_build_block(block).ok);
    CompiledBlock* compiled = m_jit->compile(block);
    ASSERT_NE(compiled, nullptr);
    ASSERT_TRUE(m_jit->ir_emit_active());
    m_jit->execute(*compiled, jit_cpu);

    EXPECT_FALSE(jit_cpu.pending_raise.active)
        << "a store to a writable page must not raise";
    EXPECT_EQ(observable_rflags(jit_cpu), observable_rflags(interp_cpu))
        << "flags after a block that completed normally";
    for (u8 r = 0; r < 8; ++r) {
        EXPECT_EQ(jit_cpu.gpr.get32(r), interp_cpu.gpr.get32(r))
            << "GPR " << static_cast<int>(r);
    }
    const auto stored = m_memory->read_u32(kOk);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), interp_cpu.gpr.get32(3)) << "the store landed";
}
