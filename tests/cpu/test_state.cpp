// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "core/cpu/state/cpu_state.hpp"
#include <gtest/gtest.h>

using namespace vitreo;
using namespace vitreo::cpu;

// ============================================================================
// GeneralRegs64 Tests
// ============================================================================

TEST(GeneralRegs64, InitializesToZero) {
    GeneralRegs64 regs;
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(regs.r[i], 0u);
    }
}

TEST(GeneralRegs64, Get64Set64) {
    GeneralRegs64 regs;
    regs.set64(0, 0x123456789ABCDEF0ULL);
    EXPECT_EQ(regs.get64(0), 0x123456789ABCDEF0ULL);
    EXPECT_EQ(regs.rax, 0x123456789ABCDEF0ULL);
}

TEST(GeneralRegs64, Get32Set32ClearsUpper) {
    GeneralRegs64 regs;
    regs.rax = 0xFFFFFFFFFFFFFFFFULL;
    regs.set32(0, 0x12345678);
    EXPECT_EQ(regs.rax, 0x12345678ULL);  // Upper 32 bits cleared
    EXPECT_EQ(regs.get32(0), 0x12345678U);
}

TEST(GeneralRegs64, Get16Set16PreservesUpper) {
    GeneralRegs64 regs;
    regs.rax = 0xDEADBEEFCAFEBABEULL;
    regs.set16(0, 0x1234);
    EXPECT_EQ(regs.rax, 0xDEADBEEFCAFE1234ULL);
    EXPECT_EQ(regs.get16(0), 0x1234);
}

TEST(GeneralRegs64, Get8lSet8lPreservesUpper) {
    GeneralRegs64 regs;
    regs.rax = 0xDEADBEEFCAFEBABEULL;
    regs.set8l(0, 0x42);
    EXPECT_EQ(regs.rax, 0xDEADBEEFCAFEBA42ULL);
    EXPECT_EQ(regs.get8l(0), 0x42);
}

TEST(GeneralRegs64, Get8hSet8h) {
    GeneralRegs64 regs;
    regs.rax = 0xDEADBEEFCAFEBABEULL;
    regs.set8h(0, 0x42);  // AH
    EXPECT_EQ(regs.rax, 0xDEADBEEFCAFE42BEULL);
    EXPECT_EQ(regs.get8h(0), 0x42);
}

TEST(GeneralRegs64, AllRegistersAccessible) {
    GeneralRegs64 regs;
    regs.rax = 1; regs.rcx = 2; regs.rdx = 3; regs.rbx = 4;
    regs.rsp = 5; regs.rbp = 6; regs.rsi = 7; regs.rdi = 8;
    regs.r8 = 9; regs.r9 = 10; regs.r10 = 11; regs.r11 = 12;
    regs.r12 = 13; regs.r13 = 14; regs.r14 = 15; regs.r15 = 16;
    
    for (u8 i = 0; i < 16; ++i) {
        EXPECT_EQ(regs.get64(i), static_cast<u64>(i + 1));
    }
}

// ============================================================================
// GeneralRegs32 Tests
// ============================================================================

TEST(GeneralRegs32, InitializesToZero) {
    GeneralRegs32 regs;
    for (int i = 0; i < 8; ++i) {
        EXPECT_EQ(regs.r[i], 0u);
    }
}

TEST(GeneralRegs32, Get32Set32) {
    GeneralRegs32 regs;
    regs.set32(0, 0x12345678);
    EXPECT_EQ(regs.get32(0), 0x12345678U);
    EXPECT_EQ(regs.eax, 0x12345678U);
}

// ============================================================================
// Flags Tests
// ============================================================================

TEST(Flags, DefaultStateBit1Set) {
    Flags flags;
    // Bit 1 is always set
    EXPECT_EQ(flags.rflags & 2, 2u);
}

TEST(Flags, SetClearCarryFlag) {
    Flags flags;
    EXPECT_FALSE(flags.cf());
    flags.set_cf(true);
    EXPECT_TRUE(flags.cf());
    flags.set_cf(false);
    EXPECT_FALSE(flags.cf());
}

TEST(Flags, SetClearZeroFlag) {
    Flags flags;
    EXPECT_FALSE(flags.zf());
    flags.set_zf(true);
    EXPECT_TRUE(flags.zf());
}

TEST(Flags, SetClearSignFlag) {
    Flags flags;
    EXPECT_FALSE(flags.sf());
    flags.set_sf(true);
    EXPECT_TRUE(flags.sf());
}

TEST(Flags, SetClearOverflowFlag) {
    Flags flags;
    EXPECT_FALSE(flags.of());
    flags.set_of(true);
    EXPECT_TRUE(flags.of());
}

TEST(Flags, DirectionFlag) {
    Flags flags;
    EXPECT_FALSE(flags.df());
    flags.set_df(true);
    EXPECT_TRUE(flags.df());
}

TEST(Flags, ComputeParityEven) {
    // 0x00 has 0 ones -> even -> true
    EXPECT_TRUE(Flags::compute_parity(0x00));
    // 0xFF has 8 ones -> even -> true
    EXPECT_TRUE(Flags::compute_parity(0xFF));
    // 0x0F has 4 ones -> even -> true
    EXPECT_TRUE(Flags::compute_parity(0x0F));
}

TEST(Flags, ComputeParityOdd) {
    // 0x01 has 1 one -> odd -> false
    EXPECT_FALSE(Flags::compute_parity(0x01));
    // 0x07 has 3 ones -> odd -> false
    EXPECT_FALSE(Flags::compute_parity(0x07));
}

TEST(Flags, UpdateZSP_Zero) {
    Flags flags;
    flags.update_zsp(static_cast<u32>(0));
    EXPECT_TRUE(flags.zf());
    EXPECT_FALSE(flags.sf());
    EXPECT_TRUE(flags.pf());  // 0 bits = even
}

TEST(Flags, UpdateZSP_Negative32) {
    Flags flags;
    flags.update_zsp(static_cast<u32>(0x80000000));
    EXPECT_FALSE(flags.zf());
    EXPECT_TRUE(flags.sf());  // Sign bit set
}

TEST(Flags, UpdateFlagsAdd_NoOverflow) {
    Flags flags;
    u32 a = 5, b = 3, result = 8;
    flags.update_flags_add(a, b, result);
    
    EXPECT_FALSE(flags.zf());
    EXPECT_FALSE(flags.sf());
    EXPECT_FALSE(flags.cf());
    EXPECT_FALSE(flags.of());
}

TEST(Flags, UpdateFlagsAdd_CarryAndOverflow) {
    Flags flags;
    u32 a = 0x80000000U;  // Largest negative in signed view
    u32 b = 0x80000000U;
    u32 result = a + b;   // 0 with carry
    flags.update_flags_add(a, b, result);
    
    EXPECT_TRUE(flags.zf());
    EXPECT_FALSE(flags.sf());
    EXPECT_TRUE(flags.cf());  // Unsigned overflow
    EXPECT_TRUE(flags.of());  // Signed overflow
}

TEST(Flags, UpdateFlagsSub_Borrow) {
    Flags flags;
    u32 a = 5, b = 10, result = a - b;
    flags.update_flags_sub(a, b, result);
    
    EXPECT_FALSE(flags.zf());
    EXPECT_TRUE(flags.sf());  // Negative result
    EXPECT_TRUE(flags.cf());  // Borrow
}

TEST(Flags, UpdateFlagsLogic) {
    Flags flags;
    flags.set_cf(true);
    flags.set_of(true);
    flags.update_flags_logic(static_cast<u32>(0x100));
    
    EXPECT_FALSE(flags.cf());  // Cleared
    EXPECT_FALSE(flags.of());  // Cleared
    EXPECT_FALSE(flags.zf());
    EXPECT_FALSE(flags.sf());
}

// ============================================================================
// SegmentRegs Tests
// ============================================================================

TEST(SegmentRegs, InitializesToZero) {
    SegmentRegs seg;
    EXPECT_EQ(seg.cs, 0);
    EXPECT_EQ(seg.ds, 0);
    EXPECT_EQ(seg.ss, 0);
}

TEST(SegmentRegs, SetupFlat32) {
    SegmentRegs seg;
    seg.setup_flat32();
    
    EXPECT_EQ(seg.cs, 0x08);
    EXPECT_EQ(seg.ds, 0x10);
    EXPECT_TRUE(seg.cs_desc.is_code);
    EXPECT_FALSE(seg.ds_desc.is_code);
    EXPECT_TRUE(seg.cs_desc.big);  // 32-bit
}

TEST(SegmentRegs, SetupLongMode) {
    SegmentRegs seg;
    seg.setup_long_mode();
    
    EXPECT_EQ(seg.cs, 0x08);
    EXPECT_TRUE(seg.cs_desc.long_mode);
    EXPECT_FALSE(seg.cs_desc.big);  // Must be 0 in long mode
}

TEST(SegmentDescriptor, EffectiveLimitGranularity) {
    SegmentDescriptor desc;
    desc.limit = 0xFFFFF;
    desc.granularity = true;
    
    EXPECT_EQ(desc.effective_limit(), 0xFFFFFFFFULL);
}

TEST(SegmentDescriptor, EffectiveLimitByte) {
    SegmentDescriptor desc;
    desc.limit = 0x1000;
    desc.granularity = false;
    
    EXPECT_EQ(desc.effective_limit(), 0x1000ULL);
}

// ============================================================================
// FpuState Tests
// ============================================================================

TEST(FpuState, DefaultControlWord) {
    FpuState fpu;
    EXPECT_EQ(fpu.control_word, 0x037F);  // Default: all exceptions masked
}

TEST(FpuState, DefaultTagsAllEmpty) {
    FpuState fpu;
    for (u8 i = 0; i < 8; ++i) {
        EXPECT_EQ(fpu.get_tag(i), FpuTag::Empty);
    }
}

TEST(FpuState, TopAccessor) {
    FpuState fpu;
    EXPECT_EQ(fpu.top(), 0);
    fpu.set_top(5);
    EXPECT_EQ(fpu.top(), 5);
}

TEST(FpuState, StIndexWraps) {
    FpuState fpu;
    fpu.set_top(6);
    EXPECT_EQ(fpu.st_index(0), 6);
    EXPECT_EQ(fpu.st_index(1), 7);
    EXPECT_EQ(fpu.st_index(2), 0);  // Wraps
    EXPECT_EQ(fpu.st_index(3), 1);
}

TEST(FpuState, PushPop) {
    FpuState fpu;
    Float80 value(3.14159);
    
    EXPECT_EQ(fpu.top(), 0);
    fpu.push(value);
    EXPECT_EQ(fpu.top(), 7);  // Decremented and wrapped
    EXPECT_NE(fpu.get_tag(7), FpuTag::Empty);
    
    Float80 popped = fpu.pop();
    EXPECT_EQ(fpu.top(), 0);
    EXPECT_NEAR(popped.to_double(), 3.14159, 0.00001);
}

TEST(FpuState, Reset) {
    FpuState fpu;
    fpu.push(Float80(1.0));
    fpu.push(Float80(2.0));
    
    fpu.reset();
    
    EXPECT_EQ(fpu.control_word, 0x037F);
    EXPECT_EQ(fpu.status_word, 0);
    EXPECT_EQ(fpu.tag_word, 0xFFFF);
    EXPECT_EQ(fpu.top(), 0);
}

// ============================================================================
// SseState Tests
// ============================================================================

TEST(SseState, DefaultMxcsr) {
    SseState sse;
    EXPECT_EQ(sse.mxcsr, 0x1F80u);  // All exceptions masked
}

TEST(SseState, XmmInitToZero) {
    SseState sse;
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(sse.xmm[i].u64_[0], 0u);
        EXPECT_EQ(sse.xmm[i].u64_[1], 0u);
    }
}

TEST(SseState, RoundingControl) {
    SseState sse;
    sse.set_rounding(SseRounding::Down);
    EXPECT_EQ(sse.rounding(), SseRounding::Down);
    
    sse.set_rounding(SseRounding::Truncate);
    EXPECT_EQ(sse.rounding(), SseRounding::Truncate);
}

TEST(SseState, Reset) {
    SseState sse;
    sse.xmm[0].u64_[0] = 0x12345678;
    sse.mxcsr = 0xFFFF;
    
    sse.reset();
    
    EXPECT_EQ(sse.xmm[0].u64_[0], 0u);
    EXPECT_EQ(sse.mxcsr, 0x1F80u);
}

TEST(XmmReg, SetBytes) {
    XmmReg reg;
    u8 data[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    reg.set_bytes(data);
    
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(reg.u8_[i], i + 1);
    }
}

TEST(YmmReg, LowHighAccess) {
    YmmReg reg;
    reg.low().u64_[0] = 0x1111111111111111ULL;
    reg.high().u64_[0] = 0x2222222222222222ULL;
    
    EXPECT_EQ(reg.u64_[0], 0x1111111111111111ULL);
    EXPECT_EQ(reg.u64_[2], 0x2222222222222222ULL);
}

TEST(YmmReg, ClearHigh) {
    YmmReg reg;
    reg.u64_[0] = reg.u64_[1] = reg.u64_[2] = reg.u64_[3] = 0xFFFFFFFFFFFFFFFFULL;
    
    reg.clear_high();
    
    EXPECT_EQ(reg.u64_[0], 0xFFFFFFFFFFFFFFFFULL);  // Low preserved
    EXPECT_EQ(reg.u64_[1], 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_EQ(reg.u64_[2], 0u);  // High cleared
    EXPECT_EQ(reg.u64_[3], 0u);
}

// ============================================================================
// CpuState Tests
// ============================================================================

TEST(CpuState, DefaultIsLongMode) {
    CpuState state;
    EXPECT_TRUE(state.is_long_mode());
    EXPECT_TRUE(state.is_64bit());
    EXPECT_FALSE(state.is_32bit());
}

TEST(CpuState, InitProtectedMode) {
    CpuState state;
    state.init_protected_mode();
    
    EXPECT_TRUE(state.is_protected_mode());
    EXPECT_TRUE(state.is_32bit());
    EXPECT_FALSE(state.is_64bit());
    EXPECT_EQ(state.seg.cs, 0x08);
}

TEST(CpuState, InitLongMode) {
    CpuState state;
    state.init_long_mode();
    
    EXPECT_TRUE(state.is_long_mode());
    EXPECT_TRUE(state.is_64bit());
    EXPECT_TRUE(state.seg.cs_desc.long_mode);
}

TEST(CpuState, StackPointer32bit) {
    CpuState state;
    state.init_protected_mode();
    state.gpr.rsp = 0xFFFFFFFF12345678ULL;
    
    // In 32-bit mode, sp() returns only lower 32 bits
    EXPECT_EQ(state.sp(), 0x12345678ULL);
    
    state.set_sp(0xDEADBEEF);
    EXPECT_EQ(state.gpr.rsp, 0xDEADBEEFULL);  // Upper bits cleared
}

TEST(CpuState, StackPointer64bit) {
    CpuState state;
    state.init_long_mode();
    state.gpr.rsp = 0xFFFFFFFF12345678ULL;
    
    EXPECT_EQ(state.sp(), 0xFFFFFFFF12345678ULL);
    
    state.set_sp(0x123456789ABCDEF0ULL);
    EXPECT_EQ(state.gpr.rsp, 0x123456789ABCDEF0ULL);
}

TEST(CpuState, AdvanceIP32bit) {
    CpuState state;
    state.init_protected_mode();
    state.rip = 0xFFFFFFF0;
    
    state.advance_ip(0x20);
    
    EXPECT_EQ(state.rip, 0x10ULL);  // Wrapped at 32 bits
}

TEST(CpuState, AdvanceIP64bit) {
    CpuState state;
    state.init_long_mode();
    state.rip = 0x7FFFFFFFFFFF0000ULL;
    
    state.advance_ip(0x20);
    
    EXPECT_EQ(state.rip, 0x7FFFFFFFFFFF0020ULL);
}

TEST(CpuState, Reset) {
    CpuState state;
    state.gpr.rax = 0x12345678;
    state.rip = 0x1000;
    state.flags.set_cf(true);
    
    state.reset();
    
    EXPECT_EQ(state.gpr.rax, 0u);
    EXPECT_EQ(state.rip, 0u);
    EXPECT_FALSE(state.flags.cf());
    EXPECT_TRUE(state.is_long_mode());
}
