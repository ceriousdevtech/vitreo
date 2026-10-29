// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_unwind_frames.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): RtlVirtualUnwind frame-shape replay

#include "ntdll_ext_test_fixture.hpp"

TEST_F(NtdllExtTest, RtlVirtualUnwindReplaysAllocSmallFrame) {
    // Microsoft Learn winnt.h: ntdll RtlVirtualUnwind reconstructs the
    // caller's CONTEXT from UNWIND_INFO. A no-op export leaves RIP/RSP
    // unchanged, so x64 C++ personalities cannot find a catch.
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto lookup = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlLookupFunctionEntry";
        });
    const auto unwind = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlVirtualUnwind";
        });
    ASSERT_NE(lookup, exports.entries.end());
    ASSERT_NE(unwind, exports.entries.end());

    GuestMemory memory;
    constexpr GuestAddr kImage = 0x10000;
    constexpr GuestAddr kCtx = 0x20000;
    constexpr GuestAddr kStack = 0x30000;
    ASSERT_TRUE(memory.map_region(
        kImage, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kCtx, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    constexpr GuestAddr kFunc = kImage + 0x200;
    constexpr GuestAddr kUnwind = kImage + 0x300;
    constexpr u32 kBeginRva = 0x400;
    ASSERT_TRUE(memory.write_u32(kFunc + 0, kBeginRva).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 4, kBeginRva + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 8, 0x300).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 0, 0x01).is_ok()); // version 1
    ASSERT_TRUE(memory.write_u8(kUnwind + 1, 8).is_ok());    // prolog
    ASSERT_TRUE(memory.write_u8(kUnwind + 2, 1).is_ok());    // one code
    ASSERT_TRUE(memory.write_u8(kUnwind + 3, 0).is_ok());
    // UWOP_ALLOC_SMALL info=1 → 16-byte alloc; offset 8 (after prolog)
    ASSERT_TRUE(memory.write_u8(kUnwind + 4, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 5, (1u << 4) | 2u).is_ok());

    constexpr GuestAddr kCallerRip = 0x140001234ull;
    constexpr GuestAddr kFrameRsp = kStack + 0x100;
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 16, kCallerRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0x98, kFrameRsp).is_ok());

    constexpr GuestAddr kHandlerData = kCtx + 0x600;
    constexpr GuestAddr kEstablisher = kCtx + 0x608;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = kImage;
    cpu.gpr.r8 = kImage + kBeginRva + 0x20;
    cpu.gpr.r9 = kFunc;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kCtx).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kHandlerData).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, kEstablisher).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    unwind->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kCallerRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kFrameRsp + 24);
    EXPECT_EQ(memory.read_u64(kEstablisher).value(), kFrameRsp);
}

TEST_F(NtdllExtTest, RtlVirtualUnwindSimulatesFramePointerEpilog) {
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto unwind = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlVirtualUnwind";
        });
    ASSERT_NE(unwind, exports.entries.end());

    GuestMemory memory;
    constexpr GuestAddr kImage = 0x10000;
    constexpr GuestAddr kCtx = 0x20000;
    constexpr GuestAddr kStack = 0x30000;
    ASSERT_TRUE(memory.map_region(
        kImage, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kCtx, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    constexpr GuestAddr kFunc = kImage + 0x200;
    constexpr u32 kBeginRva = 0x400;
    constexpr GuestAddr kUnwind = kImage + 0x300;
    constexpr GuestAddr kEpilog = kImage + kBeginRva + 0x70;
    ASSERT_TRUE(memory.write_u32(kFunc + 0, kBeginRva).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 4, kBeginRva + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 8, 0x300).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 0, 0x01).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 1, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 2, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 3, 5).is_ok()); // frame register RBP
    ASSERT_TRUE(memory.write_u8(kUnwind + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 5, (3u << 4) | 2u).is_ok());
    // lea rsp,[rbp+0x20]; pop rbx; ret
    const u8 epilog[] = {0x48, 0x8D, 0x65, 0x20, 0x5B, 0xC3};
    ASSERT_TRUE(memory.write_bytes(kEpilog, epilog, sizeof(epilog)).is_ok());

    constexpr GuestAddr kFrameRbp = kStack + 0x100;
    constexpr GuestAddr kTrimmedRsp = kFrameRbp + 0x20;
    constexpr u64 kSavedRbx = 0x1122334455667788ull;
    constexpr u64 kCallerRip = 0x140004321ull;
    ASSERT_TRUE(memory.write_u64(kTrimmedRsp + 0, kSavedRbx).is_ok());
    ASSERT_TRUE(memory.write_u64(kTrimmedRsp + 8, kCallerRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0x98, kStack + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0xA0, kFrameRbp).is_ok());

    constexpr GuestAddr kHandlerData = kCtx + 0x600;
    constexpr GuestAddr kEstablisher = kCtx + 0x608;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = kImage;
    cpu.gpr.r8 = kEpilog;
    cpu.gpr.r9 = kFunc;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kCtx).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kHandlerData).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, kEstablisher).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    unwind->handler(marshaler);

    EXPECT_EQ(memory.read_u64(kCtx + 0x90).value(), kSavedRbx);
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kCallerRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kTrimmedRsp + 16);
}

TEST_F(NtdllExtTest, RtlVirtualUnwindRestoresRbpForSetFpreg) {
    // Learn x64 EH: UWOP_PUSH_NONVOL restores the register; the next frame's
    // UWOP_SET_FPREG uses that RBP. A VSP-only walk keeps the exception-time
    // RBP and then computes a non-canonical caller RIP.
    const auto exports = make_ntdll_ext_exports(mgr);
    const auto unwind = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "RtlVirtualUnwind";
        });
    ASSERT_NE(unwind, exports.entries.end());

    GuestMemory memory;
    constexpr GuestAddr kImage = 0x10000;
    constexpr GuestAddr kCtx = 0x20000;
    constexpr GuestAddr kStack = 0x30000;
    ASSERT_TRUE(memory.map_region(
        kImage, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kCtx, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    constexpr GuestAddr kInnerFunc = kImage + 0x200;
    constexpr GuestAddr kOuterFunc = kImage + 0x20C;
    constexpr GuestAddr kInnerUnwind = kImage + 0x300;
    constexpr GuestAddr kOuterUnwind = kImage + 0x320;
    constexpr u32 kInnerBegin = 0x400;
    constexpr u32 kOuterBegin = 0x500;
    ASSERT_TRUE(memory.write_u32(kInnerFunc + 0, kInnerBegin).is_ok());
    ASSERT_TRUE(memory.write_u32(kInnerFunc + 4, kInnerBegin + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kInnerFunc + 8, 0x300).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 0, 0x01).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 1, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 2, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 3, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 4, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 5, (3u << 4) | 2u).is_ok()); // ALLOC 32
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 6, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kInnerUnwind + 7, (5u << 4) | 0u).is_ok()); // PUSH RBP

    ASSERT_TRUE(memory.write_u32(kOuterFunc + 0, kOuterBegin).is_ok());
    ASSERT_TRUE(memory.write_u32(kOuterFunc + 4, kOuterBegin + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kOuterFunc + 8, 0x320).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 0, 0x01).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 1, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 2, 3).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 3, 5).is_ok()); // frame = RBP, off 0
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 4, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 5, (1u << 4) | 2u).is_ok()); // ALLOC 16
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 6, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 7, 3).is_ok());               // SET_FPREG
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 8, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kOuterUnwind + 9, (7u << 4) | 0u).is_ok()); // PUSH RDI

    constexpr GuestAddr kOuterRbp = kStack + 0x200;
    constexpr GuestAddr kInnerRsp = kOuterRbp - 64;
    constexpr GuestAddr kOuterRip = kImage + kOuterBegin + 0x20;
    constexpr GuestAddr kOuterCaller = 0x140009999ull;
    ASSERT_TRUE(memory.write_u64(kInnerRsp + 32, kOuterRbp).is_ok());
    ASSERT_TRUE(memory.write_u64(kInnerRsp + 40, kOuterRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kOuterRbp, 0x1122334455667788ull).is_ok());
    ASSERT_TRUE(memory.write_u64(kOuterRbp + 8, kOuterCaller).is_ok());

    ASSERT_TRUE(memory.write_u64(kCtx + 0x98, kInnerRsp).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0xA0, 0xDEADBEEFull).is_ok()); // stale RBP

    constexpr GuestAddr kHandlerData = kCtx + 0x600;
    constexpr GuestAddr kEstablisher = kCtx + 0x608;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    auto invoke = [&](GuestAddr func, GuestAddr control_pc) {
        cpu.gpr.rcx = 0;
        cpu.gpr.rdx = kImage;
        cpu.gpr.r8 = control_pc;
        cpu.gpr.r9 = func;
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kCtx).is_ok());
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kHandlerData).is_ok());
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, kEstablisher).is_ok());
        ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        unwind->handler(marshaler);
    };

    invoke(kInnerFunc, kImage + kInnerBegin + 0x20);
    EXPECT_EQ(memory.read_u64(kCtx + 0xA0).value(), kOuterRbp);
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kOuterRip);

    invoke(kOuterFunc, kOuterRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kOuterCaller);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kOuterRbp + 16);
}
