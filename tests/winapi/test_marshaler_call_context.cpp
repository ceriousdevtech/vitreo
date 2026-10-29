/// @file test_marshaler_call_context.cpp
/// @brief Unit tests for Marshaler fastcall call-context (INFRA-003)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class MarshalerCallContextTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Map a 64 KiB region at 0x1000 for general memory ops
        auto res = memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        // Map a 4 KiB region at STACK_BASE for stack reads
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        // Point RSP somewhere inside the stack region
        cpu.gpr.rsp = STACK_BASE + 0x800;
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    cpu::CpuState cpu;
};

// =============================================================================
// Register-based arguments (indices 0–3)
// =============================================================================

TEST_F(MarshalerCallContextTest, Arg0ReturnsRcx) {
    cpu.gpr.rcx = 0xAAAA'BBBB'CCCC'DDDDu;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(0), 0xAAAA'BBBB'CCCC'DDDDu);
}

TEST_F(MarshalerCallContextTest, Arg1ReturnsRdx) {
    cpu.gpr.rdx = 0x1111'2222'3333'4444u;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(1), 0x1111'2222'3333'4444u);
}

TEST_F(MarshalerCallContextTest, Arg2ReturnsR8) {
    cpu.gpr.r8 = 0xDEAD'BEEF'CAFE'BABEu;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(2), 0xDEAD'BEEF'CAFE'BABEu);
}

TEST_F(MarshalerCallContextTest, Arg3ReturnsR9) {
    cpu.gpr.r9 = 0xFACE'FEED'0000'0001u;
    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(3), 0xFACE'FEED'0000'0001u);
}

// =============================================================================
// Stack-based arguments (indices >= 4)
// =============================================================================

TEST_F(MarshalerCallContextTest, Arg4ReadsAfterReturnAddressAndShadowSpace) {
    // arg(4) -> RSP + 0x28
    GuestAddr addr = cpu.gpr.rsp + 0x28;
    u64 expected = 0x5555'6666'7777'8888u;
    ASSERT_TRUE(memory.write_u64(addr, expected).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(4), expected);
}

TEST_F(MarshalerCallContextTest, Arg5ReadsFromNextStackSlot) {
    // arg(5) -> RSP + 0x30
    GuestAddr addr = cpu.gpr.rsp + 0x30;
    u64 expected = 0x9999'AAAA'BBBB'CCCCu;
    ASSERT_TRUE(memory.write_u64(addr, expected).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(5), expected);
}

// =============================================================================
// set_return
// =============================================================================

TEST_F(MarshalerCallContextTest, SetReturnWritesToRax) {
    Marshaler m(memory);
    m.attach_cpu(cpu);

    m.set_return(42u);
    EXPECT_EQ(cpu.gpr.rax, 42u);

    m.set_return(0xFFFF'FFFF'FFFF'FFFFu);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFF'FFFF'FFFF'FFFFu);
}

TEST_F(MarshalerCallContextTest, SetReturnI64PutsWholeValueInRaxOnX64) {
    Marshaler m(memory);
    m.attach_cpu(cpu);  // 64-bit CpuState by default

    cpu.gpr.rdx = 0xAAAA'AAAA'AAAA'AAAAu;  // must be left untouched on x64
    m.set_return_i64(0x1234'5678'9ABC'DEF0u);
    EXPECT_EQ(cpu.gpr.rax, 0x1234'5678'9ABC'DEF0u);
    EXPECT_EQ(cpu.gpr.rdx, 0xAAAA'AAAA'AAAA'AAAAu);
}

// =============================================================================
// arg_ptr
// =============================================================================

TEST_F(MarshalerCallContextTest, ArgPtrReturnsGuestAddr) {
    cpu.gpr.rcx = 0x0000'1234'0000'5678u;
    Marshaler m(memory);
    m.attach_cpu(cpu);

    GuestAddr ptr = m.arg_ptr(0);
    EXPECT_EQ(ptr, static_cast<GuestAddr>(0x0000'1234'0000'5678u));
}

// =============================================================================
// has_cpu
// =============================================================================

TEST_F(MarshalerCallContextTest, HasCpuReturnsFalseBeforeAttach) {
    Marshaler m(memory);
    EXPECT_FALSE(m.has_cpu());
}

TEST_F(MarshalerCallContextTest, HasCpuReturnsTrueAfterAttach) {
    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_TRUE(m.has_cpu());
}

// =============================================================================
// Marshaler without CPU – memory ops still work, arg() returns 0
// =============================================================================

TEST_F(MarshalerCallContextTest, MemoryOpsWorkWithoutCpu) {
    Marshaler m(memory);

    ASSERT_TRUE(m.write_u32(0x1000, 0xDEADu).is_ok());
    auto r = m.read_u32(0x1000);
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value(), 0xDEADu);
}

TEST_F(MarshalerCallContextTest, ArgReturnsZeroWithoutCpu) {
    Marshaler m(memory);
    EXPECT_EQ(m.arg(0), 0u);
    EXPECT_EQ(m.arg(3), 0u);
    EXPECT_EQ(m.arg(5), 0u);
}

TEST_F(MarshalerCallContextTest, SetReturnNoOpWithoutCpu) {
    Marshaler m(memory);
    // Should not crash – just a no-op
    m.set_return(42u);
}

// =============================================================================
// PE32 (32-bit) stdcall tests — INFRA-005
// =============================================================================

class MarshalerStdcallTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());
        auto stack_res = memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        cpu.init_protected_mode();           // 32-bit mode
        cpu.set_sp(STACK_BASE + 0x800);      // ESP
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    GuestMemory memory;
    cpu::CpuState cpu;
};

TEST_F(MarshalerStdcallTest, Arg0ReadsFromStackAboveReturnAddr) {
    // stdcall: ESP → return addr, ESP+4 → arg0
    GuestAddr esp = cpu.sp();
    ASSERT_TRUE(memory.write_u32(esp + 4, 0xDEADu).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(0), 0xDEADu);
}

TEST_F(MarshalerStdcallTest, Arg1ReadsFromStackPlus8) {
    GuestAddr esp = cpu.sp();
    ASSERT_TRUE(memory.write_u32(esp + 8, 0xBEEFu).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(1), 0xBEEFu);
}

TEST_F(MarshalerStdcallTest, Arg4ReadsFromStackPlus20) {
    // arg(4) → ESP + 4 + 4*4 = ESP + 20
    GuestAddr esp = cpu.sp();
    ASSERT_TRUE(memory.write_u32(esp + 20, 0xCAFEu).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    EXPECT_EQ(m.arg(4), 0xCAFEu);
}

TEST_F(MarshalerStdcallTest, ArgReturnsOnly32Bits) {
    GuestAddr esp = cpu.sp();
    // Write full 32-bit value
    ASSERT_TRUE(memory.write_u32(esp + 4, 0xFFFFFFFFu).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);
    // Should be 0xFFFFFFFF, not sign-extended
    EXPECT_EQ(m.arg(0), 0xFFFFFFFFu);
}

TEST_F(MarshalerStdcallTest, SetReturnMasksTo32Bits) {
    Marshaler m(memory);
    m.attach_cpu(cpu);

    m.set_return(0x1'FFFF'FFFFuLL);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFF'FFFFu);
}

TEST_F(MarshalerStdcallTest, SetReturn32BitValueUnchanged) {
    Marshaler m(memory);
    m.attach_cpu(cpu);

    m.set_return(42u);
    EXPECT_EQ(cpu.gpr.rax, 42u);
}

// A 64-bit return on x86 travels in the EDX:EAX pair. set_return_i64 must
// populate both halves; the regression it guards is GetTickCount64 leaving EDX
// stale, which fed a garbage high dword into curl's 64-bit divide and made
// every Battle.net connect appear to time out.
TEST_F(MarshalerStdcallTest, SetReturnI64SplitsAcrossEdxEax) {
    Marshaler m(memory);
    m.attach_cpu(cpu);

    cpu.gpr.rdx = 0xDEAD'BEEFu;  // pre-existing stale high dword
    m.set_return_i64(0x0000'552D'6ABC'1234uLL);
    EXPECT_EQ(cpu.gpr.rax & 0xFFFF'FFFFu, 0x6ABC'1234u);  // EAX = low dword
    EXPECT_EQ(cpu.gpr.rdx & 0xFFFF'FFFFu, 0x0000'552Du);  // EDX = high dword
}

// A value that fits in 32 bits must still clear EDX, not leave it stale.
TEST_F(MarshalerStdcallTest, SetReturnI64ClearsEdxForSmallValue) {
    Marshaler m(memory);
    m.attach_cpu(cpu);

    cpu.gpr.rdx = 0xDEAD'BEEFu;
    m.set_return_i64(348822006u);  // a real GetTickCount64 result, < 2^32
    EXPECT_EQ(cpu.gpr.rax & 0xFFFF'FFFFu, 348822006u);
    EXPECT_EQ(cpu.gpr.rdx & 0xFFFF'FFFFu, 0u);
}

// =============================================================================
// Guest return address — the call-site key used by allocation traces
// =============================================================================

// stdcall: on callee entry ESP points at the return address, the same fact
// arg() relies on to place arg0 at ESP+4. A trace keyed on this value is
// diffable against a reference run of the same guest binary.
TEST_F(MarshalerStdcallTest, GuestReturnAddressReadsDwordAtEsp) {
    GuestAddr esp = cpu.sp();
    ASSERT_TRUE(memory.write_u32(esp, 0x004de657u).is_ok());
    ASSERT_TRUE(memory.write_u32(esp + 4, 0x01000000u).is_ok());  // arg0

    Marshaler m(memory);
    m.attach_cpu(cpu);

    EXPECT_EQ(m.guest_return_address(), 0x004de657u);
    // Reading the return address must not disturb argument placement.
    EXPECT_EQ(m.arg(0), 0x01000000u);
}

TEST_F(MarshalerCallContextTest, GuestReturnAddressReadsQwordAtRsp) {
    GuestAddr rsp = cpu.gpr.rsp;
    ASSERT_TRUE(memory.write_u64(rsp, 0x0000'7FFE'1234'5678u).is_ok());

    Marshaler m(memory);
    m.attach_cpu(cpu);

    EXPECT_EQ(m.guest_return_address(), 0x0000'7FFE'1234'5678u);
}

// No CPU attached is a valid state for a Marshaler; it must report 0 rather
// than dereferencing a null CpuState.
TEST_F(MarshalerCallContextTest, GuestReturnAddressWithoutCpuIsZero) {
    Marshaler m(memory);
    EXPECT_EQ(m.guest_return_address(), 0u);
}

// =============================================================================
// Stack window (Marshaler::StackWindow; WP-R1 C6). The window's bytes are made
// to differ from guest memory on purpose, so each read shows which one
// answered.
// =============================================================================

TEST_F(MarshalerCallContextTest, X64StackWindowAnswersTheSlotsItCovers) {
    cpu.init_long_mode();
    cpu.gpr.rsp = STACK_BASE + 0x800;
    const GuestAddr sp = cpu.gpr.rsp;
    ASSERT_TRUE(memory.write_u64(sp, 0x1111).is_ok());
    ASSERT_TRUE(memory.write_u64(sp + 0x28, 0x5555).is_ok());
    ASSERT_TRUE(memory.write_u64(sp + 0x38, 0x7777).is_ok());
    std::array<u8, 0x38> window{};
    const u64 ret = 0x0000000140001234ull, arg4 = 0xAAAA, arg5 = 0xBBBB;
    std::memcpy(window.data(), &ret, 8);
    std::memcpy(window.data() + 0x28, &arg4, 8);
    std::memcpy(window.data() + 0x30, &arg5, 8);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    m.set_stack_window({sp, static_cast<u32>(window.size()), window.data()});
    EXPECT_EQ(m.guest_return_address(), ret);
    EXPECT_EQ(m.arg(4), arg4);
    EXPECT_EQ(m.arg(5), arg5);
    // Past the window: guest memory, as without one.
    EXPECT_EQ(m.arg(6), 0x7777u);
    // A deeper frame (a nested crossing on this Marshaler) is not the
    // window's: it reads its own slots.
    cpu.gpr.rsp = sp - 0x100;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0x4444).is_ok());
    EXPECT_EQ(m.arg(4), 0x4444u);
    cpu.gpr.rsp = sp;
    m.set_stack_window({});
    EXPECT_EQ(m.arg(4), 0x5555u);
    EXPECT_EQ(m.guest_return_address(), 0x1111u);
}

TEST_F(MarshalerCallContextTest, X86StackWindowAnswersTheSlotsItCovers) {
    cpu.init_protected_mode();
    cpu.set_sp(STACK_BASE + 0x800);
    const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
    ASSERT_TRUE(memory.write_u32(sp + 4, 0x51).is_ok());
    ASSERT_TRUE(memory.write_u32(sp + 16, 0x54).is_ok());
    std::array<u8, 16> window{};
    const u32 ret = 0x00401000u, a0 = 0xA0, a1 = 0xA1, a2 = 0xA2;
    std::memcpy(window.data(), &ret, 4);
    std::memcpy(window.data() + 4, &a0, 4);
    std::memcpy(window.data() + 8, &a1, 4);
    std::memcpy(window.data() + 12, &a2, 4);
    Marshaler m(memory);
    m.attach_cpu(cpu);
    m.set_stack_window({sp, static_cast<u32>(window.size()), window.data()});
    EXPECT_EQ(m.guest_return_address(), ret);
    EXPECT_EQ(m.arg(0), a0);
    EXPECT_EQ(m.arg(1), a1);
    EXPECT_EQ(m.arg(2), a2);
    EXPECT_EQ(m.arg32(2), a2);
    EXPECT_EQ(m.arg(3), 0x54u) << "past the window";
}
