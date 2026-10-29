// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - 32-bit frame-based SEH dispatch tests (SEH-001)

#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/memory/guest_memory.hpp"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>
#include <string>

using namespace vitreo;
using namespace vitreo::cpu;

class Seh32Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Map a scratch region for CONTEXT/EXCEPTION_RECORD
        ASSERT_TRUE(m_memory.map_region(0x10000, 0x4000, MemoryProtection::ReadWrite).is_ok());
        // Map a region for SEH chain
        ASSERT_TRUE(m_memory.map_region(0x20000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        // Set up as 32-bit CPU
        m_cpu.init_protected_mode();
        // Set FS base (TEB simulation)
        m_cpu.seg.fs_desc.base = 0x20000;
    }

    GuestMemory m_memory;
    CpuState m_cpu;
};

// ---------------------------------------------------------------------------
// Test 1: cpu_exception_to_status maps GeneralProtection → ACCESS_VIOLATION
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, CpuExceptionToStatus_GP) {
    u32 status = cpu_exception_to_status(CpuException::GeneralProtection);
    EXPECT_EQ(status, STATUS_ACCESS_VIOLATION);
}

// ---------------------------------------------------------------------------
// Test 2: cpu_exception_to_status maps DivideError → INTEGER_DIVIDE_BY_ZERO
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, CpuExceptionToStatus_DivideError) {
    u32 status = cpu_exception_to_status(CpuException::DivideError);
    EXPECT_EQ(status, STATUS_INTEGER_DIVIDE_BY_ZERO);
}

TEST_F(Seh32Test, JitHelperIllegalInstructionReturnsToSharedExceptionPath) {
    m_cpu.rip = 0x401000;
    m_cpu.pending_raise.active = true;
    m_cpu.pending_raise.from_helper = true;
    m_cpu.pending_raise.code = STATUS_ILLEGAL_INSTRUCTION;

    ExecContext context(m_cpu, m_memory);
    ExecResult result = ExecResult::Continue;

    ASSERT_TRUE(consume_pending_helper_raise(m_cpu, context, result));
    EXPECT_EQ(result, ExecResult::Exception);
    EXPECT_EQ(context.exception, CpuException::InvalidOpcode);
    EXPECT_EQ(context.exception_addr, 0x401000u);
    EXPECT_FALSE(m_cpu.pending_raise.active);
}

TEST_F(Seh32Test, JitHelperAccessViolationPreservesFaultAddress) {
    m_cpu.rip = 0x402000;
    m_cpu.pending_raise.active = true;
    m_cpu.pending_raise.from_helper = true;
    m_cpu.pending_raise.code = STATUS_ACCESS_VIOLATION;
    m_cpu.pending_raise.num_args = 2;
    m_cpu.pending_raise.args[1] = 0xDEADBEEFu;

    ExecContext context(m_cpu, m_memory);
    ExecResult result = ExecResult::Continue;

    ASSERT_TRUE(consume_pending_helper_raise(m_cpu, context, result));
    EXPECT_EQ(result, ExecResult::Exception);
    EXPECT_EQ(context.exception, CpuException::GeneralProtection);
    EXPECT_EQ(context.exception_addr, 0xDEADBEEFu);
    EXPECT_FALSE(m_cpu.pending_raise.active);
}

TEST_F(Seh32Test, JitHelperBreakpointPreservesTrapAddress) {
    m_cpu.rip = 0x403001;
    m_cpu.pending_raise.active = true;
    m_cpu.pending_raise.from_helper = true;
    m_cpu.pending_raise.code = STATUS_BREAKPOINT;

    ExecContext context(m_cpu, m_memory);
    ExecResult result = ExecResult::Continue;

    ASSERT_TRUE(consume_pending_helper_raise(m_cpu, context, result));
    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(context.exception, CpuException::Breakpoint);
    EXPECT_EQ(context.interrupt_num, 3);
    EXPECT_EQ(context.exception_addr, 0x403000u);
    EXPECT_FALSE(m_cpu.pending_raise.active);
}

TEST_F(Seh32Test, JitHelperFastFail32ReturnsInterrupt29) {
    m_cpu.init_protected_mode();
    m_cpu.rip = 0x1b357c7d;
    m_cpu.gpr.rcx = 2; // FAST_FAIL_STACK_COOKIE_CHECK_FAILURE
    m_cpu.pending_raise.active = true;
    m_cpu.pending_raise.from_helper = true;
    m_cpu.pending_raise.code = STATUS_STACK_BUFFER_OVERRUN;
    m_cpu.pending_raise.flags = 1;
    m_cpu.pending_raise.num_args = 1;
    m_cpu.pending_raise.args[0] = 2;

    ExecContext context(m_cpu, m_memory);
    ExecResult result = ExecResult::Continue;

    ASSERT_TRUE(consume_pending_helper_raise(m_cpu, context, result));
    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(context.interrupt_num, 0x29);
    EXPECT_EQ(context.exception_error, 2u);
    EXPECT_EQ(context.exception_addr, 0x1b357c7du);
    EXPECT_FALSE(m_cpu.pending_raise.active);
}

TEST_F(Seh32Test, JitHelperFastFailReturnsInterrupt29) {
    m_cpu.init_long_mode();
    m_cpu.rip = 0x000000014004DE71ull;
    m_cpu.gpr.rcx = 2; // FAST_FAIL_STACK_COOKIE_CHECK_FAILURE
    m_cpu.pending_raise.active = true;
    m_cpu.pending_raise.from_helper = true;
    m_cpu.pending_raise.code = STATUS_STACK_BUFFER_OVERRUN;
    m_cpu.pending_raise.flags = 1;
    m_cpu.pending_raise.num_args = 1;
    m_cpu.pending_raise.args[0] = 2;

    ExecContext context(m_cpu, m_memory);
    ExecResult result = ExecResult::Continue;

    ASSERT_TRUE(consume_pending_helper_raise(m_cpu, context, result));
    EXPECT_EQ(result, ExecResult::Interrupt);
    EXPECT_EQ(context.interrupt_num, 0x29);
    EXPECT_EQ(context.exception_error, 2u);
    EXPECT_EQ(context.exception_addr, 0x000000014004DE71ull);
    EXPECT_FALSE(m_cpu.pending_raise.active);
}

TEST_F(Seh32Test, X64JitHelperAccessViolationReturnsToSharedExceptionPath) {
    m_cpu.init_long_mode();
    m_cpu.rip = 0x0000000140123456ull;
    m_cpu.pending_raise.active = true;
    m_cpu.pending_raise.from_helper = true;
    m_cpu.pending_raise.code = STATUS_ACCESS_VIOLATION;
    m_cpu.pending_raise.num_args = 2;
    m_cpu.pending_raise.args[0] = 0;
    m_cpu.pending_raise.args[1] = 0;

    ExecContext context(m_cpu, m_memory);
    ExecResult result = ExecResult::Continue;

    ASSERT_TRUE(consume_pending_helper_raise(m_cpu, context, result));
    EXPECT_EQ(result, ExecResult::Exception);
    EXPECT_EQ(context.exception, CpuException::GeneralProtection);
    EXPECT_EQ(context.exception_error, 0u);
    EXPECT_EQ(context.exception_addr, 0u);
    EXPECT_FALSE(m_cpu.pending_raise.active);
}

// ---------------------------------------------------------------------------
// Test 3: Write an EXCEPTION_RECORD and read fields back
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, WriteExceptionRecord) {
    constexpr u32 addr = 0x10000;
    constexpr u32 code = STATUS_ACCESS_VIOLATION;
    constexpr u32 flags = 0;
    constexpr u32 exc_addr = 0x401000;
    u32 params[2] = { 0, 0x402000 }; // read fault at 0x402000

    auto res = write_exception_record_32(m_memory, addr, code, flags, exc_addr, 2, params);
    ASSERT_TRUE(res.is_ok());

    // Verify exception code
    auto code_read = m_memory.read_u32(addr + EREC32_CODE);
    ASSERT_TRUE(code_read.is_ok());
    EXPECT_EQ(code_read.value(), code);

    // Verify exception address
    auto addr_read = m_memory.read_u32(addr + EREC32_ADDRESS);
    ASSERT_TRUE(addr_read.is_ok());
    EXPECT_EQ(addr_read.value(), exc_addr);

    // Verify number of parameters
    auto nparams = m_memory.read_u32(addr + EREC32_NUM_PARAMS);
    ASSERT_TRUE(nparams.is_ok());
    EXPECT_EQ(nparams.value(), 2u);

    // Verify parameter values
    auto p0 = m_memory.read_u32(addr + EREC32_PARAMS);
    ASSERT_TRUE(p0.is_ok());
    EXPECT_EQ(p0.value(), 0u);

    auto p1 = m_memory.read_u32(addr + EREC32_PARAMS + 4);
    ASSERT_TRUE(p1.is_ok());
    EXPECT_EQ(p1.value(), 0x402000u);
}

// ---------------------------------------------------------------------------
// Test 4: Write CONTEXT from CPU state and verify fields
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, WriteContext) {
    m_cpu.gpr.rax = 0xDEADBEEF;
    m_cpu.gpr.rcx = 0x12345678;
    m_cpu.rip     = 0x00401000;
    m_cpu.gpr.rsp = 0x0012FF00;
    m_cpu.flags.rflags = 0x00000246; // ZF+PF+IF set, bit 1 always set

    constexpr u32 ctx_addr = 0x10000;
    auto res = write_context_32(m_memory, ctx_addr, m_cpu);
    ASSERT_TRUE(res.is_ok());

    // ContextFlags
    auto cf = m_memory.read_u32(ctx_addr + CTX32_FLAGS);
    ASSERT_TRUE(cf.is_ok());
    EXPECT_EQ(cf.value(), CONTEXT_FULL_32);

    // EAX
    auto eax = m_memory.read_u32(ctx_addr + CTX32_EAX);
    ASSERT_TRUE(eax.is_ok());
    EXPECT_EQ(eax.value(), 0xDEADBEEFu);

    // ECX
    auto ecx = m_memory.read_u32(ctx_addr + CTX32_ECX);
    ASSERT_TRUE(ecx.is_ok());
    EXPECT_EQ(ecx.value(), 0x12345678u);

    // EIP
    auto eip = m_memory.read_u32(ctx_addr + CTX32_EIP);
    ASSERT_TRUE(eip.is_ok());
    EXPECT_EQ(eip.value(), 0x00401000u);

    // ESP
    auto esp = m_memory.read_u32(ctx_addr + CTX32_ESP);
    ASSERT_TRUE(esp.is_ok());
    EXPECT_EQ(esp.value(), 0x0012FF00u);

    // EFLAGS
    auto efl = m_memory.read_u32(ctx_addr + CTX32_EFLAGS);
    ASSERT_TRUE(efl.is_ok());
    EXPECT_EQ(efl.value(), 0x00000246u);
}

// ---------------------------------------------------------------------------
// Test 5: Read CONTEXT from memory into CPU state
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, ReadContext) {
    constexpr u32 ctx_addr = 0x10000;

    // Write known values at CONTEXT offsets
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_FLAGS,  CONTEXT_FULL_32).is_ok());
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_EAX,    0xAABBCCDD).is_ok());
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_ECX,    0x11223344).is_ok());
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_EIP,    0x00402000).is_ok());
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_ESP,    0x0012FE00).is_ok());
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_EBP,    0x0012FE80).is_ok());
    ASSERT_TRUE(m_memory.write_u32(ctx_addr + CTX32_EFLAGS, 0x00000202).is_ok());

    CpuState cpu;
    cpu.init_protected_mode();
    auto res = read_context_32(m_memory, ctx_addr, cpu);
    ASSERT_TRUE(res.is_ok());

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xAABBCCDDu);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rcx), 0x11223344u);
    EXPECT_EQ(static_cast<u32>(cpu.rip),     0x00402000u);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rsp), 0x0012FE00u);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rbp), 0x0012FE80u);
    EXPECT_EQ(cpu.flags.eflags(),            0x00000202u);
}

// ---------------------------------------------------------------------------
// Test 6: SEH chain head is 0xFFFFFFFF when no handlers installed
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, ReadSehChainHead_NoHandlers) {
    // FS:[0] = 0xFFFFFFFF  (end-of-chain sentinel)
    u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);
    ASSERT_TRUE(m_memory.write_u32(fs_base, 0xFFFFFFFF).is_ok());

    auto result = read_seh_chain_head(m_memory, m_cpu);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// Test 7: SEH chain head returns the first handler's frame address
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, ReadSehChainHead_HasHandler) {
    u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);
    ASSERT_TRUE(m_memory.write_u32(fs_base, 0x20100).is_ok());

    auto result = read_seh_chain_head(m_memory, m_cpu);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 0x20100u);
}

TEST_F(Seh32Test, WriteSehChainHeadUnwindsToSelectedFrame) {
    constexpr u32 discarded_inner_frame = 0x20100;
    constexpr u32 selected_outer_frame = 0x20200;
    u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);
    ASSERT_TRUE(
        m_memory.write_u32(fs_base, discarded_inner_frame).is_ok());

    ASSERT_TRUE(write_seh_chain_head(
                    m_memory, m_cpu, selected_outer_frame)
                    .is_ok());

    auto result = read_seh_chain_head(m_memory, m_cpu);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), selected_outer_frame);
}

// ---------------------------------------------------------------------------
// Test 8: Read a single EXCEPTION_REGISTRATION record
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, ReadSehHandler) {
    constexpr u32 frame = 0x20100;
    // EXCEPTION_REGISTRATION: { Next (dword), Handler (dword) }
    ASSERT_TRUE(m_memory.write_u32(frame,     0xFFFFFFFF).is_ok()); // Next  = end
    ASSERT_TRUE(m_memory.write_u32(frame + 4, 0x00401000).is_ok()); // Handler

    auto result = read_seh_handler(m_memory, frame);
    ASSERT_TRUE(result.is_ok());

    const auto& info = result.value();
    EXPECT_EQ(info.frame_addr,   frame);
    EXPECT_EQ(info.handler_addr, 0x00401000u);
    EXPECT_EQ(info.next_frame,   0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// Registration records must live on the faulting thread's own stack
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, RegistrationOutsideThreadStackIsNotDispatchable) {
    const u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);
    // NT_TIB32: StackBase (high) at +0x04, StackLimit (low) at +0x08.
    ASSERT_TRUE(m_memory.write_u32(fs_base + 0x04, 0x20800).is_ok());
    ASSERT_TRUE(m_memory.write_u32(fs_base + 0x08, 0x20400).is_ok());

    EXPECT_TRUE(seh32_registration_dispatchable(m_memory, m_cpu, 0x20400));
    EXPECT_TRUE(seh32_registration_dispatchable(m_memory, m_cpu, 0x207f8));

    // Below the stack, past its top, straddling the top, and misaligned are all
    // signs of a corrupt chain rather than a frame Windows would dispatch.
    EXPECT_FALSE(seh32_registration_dispatchable(m_memory, m_cpu, 0x203fc));
    EXPECT_FALSE(seh32_registration_dispatchable(m_memory, m_cpu, 0x20800));
    EXPECT_FALSE(seh32_registration_dispatchable(m_memory, m_cpu, 0x207fc));
    EXPECT_FALSE(seh32_registration_dispatchable(m_memory, m_cpu, 0x20402));
}

TEST_F(Seh32Test, RegistrationCheckPermissiveWithoutStackBounds) {
    const u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);
    ASSERT_TRUE(m_memory.write_u32(fs_base + 0x04, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(fs_base + 0x08, 0).is_ok());

    // A TEB that never described its stack cannot condemn a frame.
    EXPECT_TRUE(seh32_registration_dispatchable(m_memory, m_cpu, 0x20100));
}

// ---------------------------------------------------------------------------
// Test 9: Walk a two-element SEH chain
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, ReadSehHandler_Chain) {
    constexpr u32 frame1 = 0x20100;
    constexpr u32 frame2 = 0x20108;

    // First registration: Next = frame2
    ASSERT_TRUE(m_memory.write_u32(frame1,     frame2).is_ok());
    ASSERT_TRUE(m_memory.write_u32(frame1 + 4, 0x00401000).is_ok());

    // Second registration: Next = end-of-chain
    ASSERT_TRUE(m_memory.write_u32(frame2,     0xFFFFFFFF).is_ok());
    ASSERT_TRUE(m_memory.write_u32(frame2 + 4, 0x00402000).is_ok());

    // Read first handler
    auto r1 = read_seh_handler(m_memory, frame1);
    ASSERT_TRUE(r1.is_ok());
    EXPECT_EQ(r1.value().handler_addr, 0x00401000u);
    EXPECT_EQ(r1.value().next_frame,   frame2);

    // Follow the chain to the second handler
    auto r2 = read_seh_handler(m_memory, r1.value().next_frame);
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(r2.value().handler_addr, 0x00402000u);
    EXPECT_EQ(r2.value().next_frame,   0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// Test 10: Round-trip — write context, mutate CPU, read context back
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, ContextRoundTrip) {
    constexpr u32 ctx_addr = 0x10000;

    // Set CPU to known state
    m_cpu.gpr.rax = 0x11111111;
    m_cpu.gpr.rcx = 0x22222222;
    m_cpu.gpr.rdx = 0x33333333;
    m_cpu.gpr.rbx = 0x44444444;
    m_cpu.gpr.rsp = 0x0012FF00;
    m_cpu.gpr.rbp = 0x0012FF80;
    m_cpu.gpr.rsi = 0x55555555;
    m_cpu.gpr.rdi = 0x66666666;
    m_cpu.rip     = 0x00401234;
    m_cpu.flags   = Flags(0x00000246);

    // Write to guest memory
    auto wres = write_context_32(m_memory, ctx_addr, m_cpu);
    ASSERT_TRUE(wres.is_ok());

    // Clobber CPU state
    m_cpu.gpr.rax = 0;
    m_cpu.gpr.rcx = 0;
    m_cpu.gpr.rdx = 0;
    m_cpu.gpr.rbx = 0;
    m_cpu.gpr.rsp = 0;
    m_cpu.gpr.rbp = 0;
    m_cpu.gpr.rsi = 0;
    m_cpu.gpr.rdi = 0;
    m_cpu.rip     = 0;
    m_cpu.flags   = Flags(0);

    // Read back
    auto rres = read_context_32(m_memory, ctx_addr, m_cpu);
    ASSERT_TRUE(rres.is_ok());

    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rax), 0x11111111u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rcx), 0x22222222u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rdx), 0x33333333u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rbx), 0x44444444u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rsp), 0x0012FF00u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rbp), 0x0012FF80u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rsi), 0x55555555u);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rdi), 0x66666666u);
    EXPECT_EQ(static_cast<u32>(m_cpu.rip),     0x00401234u);
    EXPECT_EQ(m_cpu.flags.eflags(),            0x00000246u);
}

// ---------------------------------------------------------------------------
// Nested FS:[0] registration (RtlpExecuteHandlerForException contract)
// ---------------------------------------------------------------------------
TEST_F(Seh32Test, NestedExecutionFrame_NextIsEstablisher) {
    constexpr u32 establisher = 0x20100;
    constexpr u32 nested_addr = 0x20200;
    u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);

    ASSERT_TRUE(m_memory.write_u32(fs_base, establisher).is_ok());
    ASSERT_TRUE(m_memory.write_u32(establisher, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(m_memory.write_u32(establisher + 4, 0x00401000u).is_ok());
    ASSERT_TRUE(m_memory.write_u32(establisher + 8, 0xABCDu).is_ok()); // stash slot

    ASSERT_TRUE(ensure_seh32_exception_protector(m_memory).is_ok());
    EXPECT_NE(seh32_exception_protector_addr(), 0u);

    Seh32NestedExecution nested{};
    ASSERT_TRUE(install_seh32_nested_execution(
                    m_memory, m_cpu, nested_addr, establisher, nested)
                    .is_ok());
    EXPECT_TRUE(nested.installed);
    EXPECT_EQ(nested.previous_head, establisher);

    // During handler execution FS:[0] names the nested record, and
    // Nested.Next is the establisher — the recovery idiom used by Petite etc.
    auto head = read_seh_chain_head(m_memory, m_cpu);
    ASSERT_TRUE(head.is_ok());
    EXPECT_EQ(head.value(), nested_addr);

    auto next = m_memory.read_u32(nested_addr);
    ASSERT_TRUE(next.is_ok());
    EXPECT_EQ(next.value(), establisher);

    auto prot = m_memory.read_u32(nested_addr + 4);
    ASSERT_TRUE(prot.is_ok());
    EXPECT_EQ(prot.value(), seh32_exception_protector_addr());

    auto saved = m_memory.read_u32(nested_addr + 8);
    ASSERT_TRUE(saved.is_ok());
    EXPECT_EQ(saved.value(), establisher);

    // Normal unlink restores previous head.
    ASSERT_TRUE(unlink_seh32_nested_execution(m_memory, m_cpu, nested).is_ok());
    head = read_seh_chain_head(m_memory, m_cpu);
    ASSERT_TRUE(head.is_ok());
    EXPECT_EQ(head.value(), establisher);
}

TEST_F(Seh32Test, NestedExecutionFrame_HandlerRewireUnlink) {
    constexpr u32 establisher = 0x20100;
    constexpr u32 outer = 0x20110;
    constexpr u32 nested_addr = 0x20200;
    u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);

    ASSERT_TRUE(m_memory.write_u32(establisher, outer).is_ok());
    ASSERT_TRUE(m_memory.write_u32(establisher + 4, 0x00401000u).is_ok());
    ASSERT_TRUE(m_memory.write_u32(outer, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(m_memory.write_u32(outer + 4, 0x00402000u).is_ok());
    ASSERT_TRUE(m_memory.write_u32(fs_base, establisher).is_ok());

    ASSERT_TRUE(ensure_seh32_exception_protector(m_memory).is_ok());
    Seh32NestedExecution nested{};
    ASSERT_TRUE(install_seh32_nested_execution(
                    m_memory, m_cpu, nested_addr, establisher, nested)
                    .is_ok());

    // Packer unlinks itself: FS:[0] = establisher->Next (outer).
    ASSERT_TRUE(write_seh_chain_head(m_memory, m_cpu, outer).is_ok());

    // RtlpExecuteHandler2 then pops Next from the current head.
    ASSERT_TRUE(unlink_seh32_nested_execution(m_memory, m_cpu, nested).is_ok());
    auto head = read_seh_chain_head(m_memory, m_cpu);
    ASSERT_TRUE(head.is_ok());
    EXPECT_EQ(head.value(), 0xFFFFFFFFu);
}

TEST_F(Seh32Test, NestedExecutionFrame_ReusedScratchStaysAcyclic) {
    constexpr u32 establisher = 0x20100;
    constexpr u32 scratch_record = 0x20200;
    const u32 fs_base = static_cast<u32>(m_cpu.seg.fs_desc.base);

    ASSERT_TRUE(m_memory.write_u32(fs_base, establisher).is_ok());
    ASSERT_TRUE(m_memory.write_u32(establisher, 0xffffffffu).is_ok());
    ASSERT_TRUE(m_memory.write_u32(establisher + 4, 0x00401000u).is_ok());
    ASSERT_TRUE(ensure_seh32_exception_protector(m_memory).is_ok());

    Seh32NestedExecution outer{};
    ASSERT_TRUE(install_seh32_nested_execution(
                    m_memory, m_cpu, scratch_record, establisher, outer)
                    .is_ok());
    ASSERT_EQ(outer.nested_addr, scratch_record);

    // A handler can trigger another dispatch while the outer synthetic record
    // is still FS:[0]. Reusing the dispatcher's scratch address must allocate
    // below it instead of overwriting it with Next == self.
    Seh32NestedExecution inner{};
    ASSERT_TRUE(install_seh32_nested_execution(
                    m_memory, m_cpu, scratch_record, establisher, inner)
                    .is_ok());
    EXPECT_EQ(inner.previous_head, outer.nested_addr);
    EXPECT_EQ(inner.nested_addr,
              outer.nested_addr - SEH32_NESTED_RECORD_SIZE);
    EXPECT_EQ(m_memory.read_u32(inner.nested_addr).value_or(0),
              outer.nested_addr);
    EXPECT_NE(m_memory.read_u32(inner.nested_addr).value_or(0),
              inner.nested_addr);

    ASSERT_TRUE(unlink_seh32_nested_execution(m_memory, m_cpu, inner).is_ok());
    EXPECT_EQ(read_seh_chain_head(m_memory, m_cpu).value_or(0),
              outer.nested_addr);
    ASSERT_TRUE(unlink_seh32_nested_execution(m_memory, m_cpu, outer).is_ok());
    EXPECT_EQ(read_seh_chain_head(m_memory, m_cpu).value_or(0), establisher);
}

// ---------------------------------------------------------------------------
// The protector stub itself, executed. The record-layout tests above never ran
// it, which is how its unwind branch shipped jumping one byte short of `ret`
// (into the mov imm32 before it) -- dead until RtlUnwind began calling the
// handlers it passes, then an access violation on the first C++ throw.
// ---------------------------------------------------------------------------
namespace {

/// Call the protector as a handler with the given ExceptionFlags; returns the
/// disposition, or ~0u if it does not return to the pushed return address.
u32 run_protector(GuestMemory& memory, CpuState& cpu, u32 exception_flags,
                  u32 nested_record, u32 dispatcher_ctx) {
    constexpr u32 kRecord = 0x10000;
    constexpr u32 kReturn = 0x10F00;
    constexpr u32 kStackTop = 0x13F00;
    (void)memory.write_u32(kRecord + EREC32_FLAGS, exception_flags);
    u32 sp = kStackTop;
    const u32 args[4] = {kRecord, nested_record, 0x10800, dispatcher_ctx};
    for (int i = 3; i >= 0; --i) {
        sp -= 4;
        (void)memory.write_u32(sp, args[i]);
    }
    sp -= 4;
    (void)memory.write_u32(sp, kReturn);
    cpu.set_sp(sp);
    cpu.rip = seh32_exception_protector_addr();
    Interpreter interp(cpu, memory);
    for (int steps = 0; steps < 32 && cpu.rip != kReturn; ++steps) {
        const ExecResult r = interp.step();
        if (r != ExecResult::Continue && r != ExecResult::Branch) return ~0u;
    }
    if (cpu.rip != kReturn || cpu.sp() != kStackTop) return ~0u;
    return static_cast<u32>(cpu.gpr.rax);
}

} // namespace

TEST_F(Seh32Test, ProtectorStub_UnwindReturnsContinueSearch) {
    ASSERT_TRUE(ensure_seh32_exception_protector(m_memory).is_ok());
    // EXCEPTION_UNWINDING, and EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND.
    EXPECT_EQ(run_protector(m_memory, m_cpu, 0x2, 0x20200, 0x20300),
              ExceptionContinueSearch);
    EXPECT_EQ(run_protector(m_memory, m_cpu, 0x6, 0x20200, 0x20300),
              ExceptionContinueSearch);
}

TEST_F(Seh32Test, ProtectorStub_ExceptionReportsNestedFrame) {
    ASSERT_TRUE(ensure_seh32_exception_protector(m_memory).is_ok());
    constexpr u32 kNested = 0x20200;
    constexpr u32 kDispatcherContext = 0x20300;
    constexpr u32 kEstablisher = 0x20100;
    ASSERT_TRUE(m_memory.write_u32(kNested + 8, kEstablisher).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kDispatcherContext, 0).is_ok());
    EXPECT_EQ(run_protector(m_memory, m_cpu, 0x0, kNested, kDispatcherContext),
              ExceptionNestedException);
    EXPECT_EQ(m_memory.read_u32(kDispatcherContext).value_or(0), kEstablisher);
}

// Expected dispositions and output writes were captured from native Windows.
TEST_F(Seh32Test, NestedHandlerMatchesNativeWindowsFlagMatrix) {
    ASSERT_TRUE(ensure_seh32_exception_protector(m_memory).is_ok());
    std::ifstream expected(std::string(VITREO_TEST_SOURCE_DIR) +
        "/../oracle_data/seh32_nested_protector_probe.expected");
    ASSERT_TRUE(expected.is_open());
    std::string line;
    unsigned cases = 0;
    while (std::getline(expected, line)) {
        unsigned flags, disposition, output;
        if (std::sscanf(line.c_str(), "flags=%x disposition=%u output=%x",
                        &flags, &disposition, &output) != 3) continue;
        SCOPED_TRACE(flags);
        ASSERT_TRUE(m_memory.write_u32(0x20208, 0x12345678).is_ok());
        ASSERT_TRUE(m_memory.write_u32(0x20300, 0xa5a5a5a5).is_ok());
        EXPECT_EQ(run_protector(m_memory, m_cpu, flags, 0x20200, 0x20300),
                  disposition);
        EXPECT_EQ(m_memory.read_u32(0x20300).value_or(0), output);
        ++cases;
    }
    EXPECT_EQ(cases, 256u);
}
