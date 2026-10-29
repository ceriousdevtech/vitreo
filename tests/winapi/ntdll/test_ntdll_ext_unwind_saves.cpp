// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_unwind_saves.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): RtlVirtualUnwind register-save and chain-info handling

#include "ntdll_ext_test_fixture.hpp"

TEST_F(NtdllExtTest, RtlVirtualUnwindRestoresSaveXmm128) {
    // Learn x64 EH: UWOP_SAVE_XMM128 saves "all 128 bits of a nonvolatile XMM
    // register" at a 16-scaled offset from the frame base. Unwinding restores
    // it into CONTEXT.Xmm<n> and does not change RSP.
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
    constexpr GuestAddr kUnwind = kImage + 0x300;
    constexpr u32 kBeginRva = 0x400;
    ASSERT_TRUE(memory.write_u32(kFunc + 0, kBeginRva).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 4, kBeginRva + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 8, 0x300).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 0, 0x01).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 1, 16).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 2, 3).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 3, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 4, 16).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 5, (6u << 4) | 8u).is_ok()); // SAVE_XMM128 xmm6
    ASSERT_TRUE(memory.write_u16(kUnwind + 6, 2).is_ok());             // offset/16
    ASSERT_TRUE(memory.write_u8(kUnwind + 8, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 9, (1u << 4) | 2u).is_ok()); // ALLOC 16

    constexpr GuestAddr kCallerRip = 0x14000ABCDull;
    constexpr GuestAddr kFrameRsp = kStack + 0x100;
    constexpr u64 kXmm6Lo = 0x0123456789ABCDEFull;
    constexpr u64 kXmm6Hi = 0xFEDCBA9876543210ull;
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 16, kCallerRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 32, kXmm6Lo).is_ok());
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 40, kXmm6Hi).is_ok());
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
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kCallerRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kFrameRsp + 24);
    // CONTEXT.Xmm6 = FltSave.XmmRegisters[6] at 0x1A0 + 6 * 16 (winnt.h).
    EXPECT_EQ(memory.read_u64(kCtx + 0x200).value(), kXmm6Lo);
    EXPECT_EQ(memory.read_u64(kCtx + 0x208).value(), kXmm6Hi);
}

TEST_F(NtdllExtTest, RtlVirtualUnwindPushMachframeRestoresRipAndRsp) {
    // Learn x64 EH, UWOP_PUSH_MACHFRAME (op info 0): RIP at [RSP], old RSP
    // at [RSP+24]. The machine frame supplies both, so no return address is
    // popped after it.
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
    constexpr GuestAddr kUnwind = kImage + 0x300;
    constexpr u32 kBeginRva = 0x400;
    ASSERT_TRUE(memory.write_u32(kFunc + 0, kBeginRva).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 4, kBeginRva + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 8, 0x300).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 0, 0x01).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 1, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 2, 2).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 3, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 5, (3u << 4) | 2u).is_ok()); // ALLOC 32
    ASSERT_TRUE(memory.write_u8(kUnwind + 6, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 7, (0u << 4) | 10u).is_ok()); // MACHFRAME

    constexpr GuestAddr kFrameRsp = kStack + 0x100;
    constexpr GuestAddr kInterruptedRip = 0x140005555ull;
    constexpr GuestAddr kInterruptedRsp = kStack + 0x700;
    // Machine frame sits right above the 32-byte allocation.
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 32 + 0, kInterruptedRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 32 + 24, kInterruptedRsp).is_ok());
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
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kInterruptedRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kInterruptedRsp);
}

TEST_F(NtdllExtTest, RtlVirtualUnwindSaveNonvolIsRelativeToFrameBase) {
    // Learn x64 EH: with a frame register, an offset-taking code is relative
    // to FP - 16 * FrameOffset, not to RSP. Here RSP has moved 0x40 below
    // the fixed allocation (alloca) after the prolog.
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

    // Prolog: push rbp (1); sub rsp,0x20 (5); lea rbp,[rsp] (9);
    //         mov [rbp+0x10],rsi (13)
    constexpr GuestAddr kFunc = kImage + 0x200;
    constexpr GuestAddr kUnwind = kImage + 0x300;
    constexpr u32 kBeginRva = 0x400;
    ASSERT_TRUE(memory.write_u32(kFunc + 0, kBeginRva).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 4, kBeginRva + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kFunc + 8, 0x300).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 0, 0x01).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 1, 13).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 2, 5).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 3, 5).is_ok()); // RBP, offset 0
    ASSERT_TRUE(memory.write_u8(kUnwind + 4, 13).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 5, (6u << 4) | 4u).is_ok()); // SAVE_NONVOL rsi
    ASSERT_TRUE(memory.write_u16(kUnwind + 6, 0x10 / 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 8, 9).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 9, 3u).is_ok());            // SET_FPREG
    ASSERT_TRUE(memory.write_u8(kUnwind + 10, 5).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 11, (3u << 4) | 2u).is_ok()); // ALLOC 32
    ASSERT_TRUE(memory.write_u8(kUnwind + 12, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kUnwind + 13, (5u << 4) | 0u).is_ok()); // PUSH rbp

    constexpr GuestAddr kFrameBase = kStack + 0x200;  // RBP == fixed alloc base
    constexpr GuestAddr kCurrentRsp = kFrameBase - 0x40;
    constexpr u64 kSavedRsi = 0x5151515151515151ull;
    constexpr u64 kCallerRbp = 0x6262626262626262ull;
    constexpr GuestAddr kCallerRip = 0x140007777ull;
    ASSERT_TRUE(memory.write_u64(kFrameBase + 0x10, kSavedRsi).is_ok());
    ASSERT_TRUE(memory.write_u64(kCurrentRsp + 0x10, 0xBADBADBADull).is_ok());
    ASSERT_TRUE(memory.write_u64(kFrameBase + 0x20, kCallerRbp).is_ok());
    ASSERT_TRUE(memory.write_u64(kFrameBase + 0x28, kCallerRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0x98, kCurrentRsp).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0xA0, kFrameBase).is_ok());

    constexpr GuestAddr kHandlerData = kCtx + 0x600;
    constexpr GuestAddr kEstablisher = kCtx + 0x608;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = kImage;
    cpu.gpr.r8 = kImage + kBeginRva + 0x40;
    cpu.gpr.r9 = kFunc;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kCtx).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kHandlerData).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, kEstablisher).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    unwind->handler(marshaler);
    EXPECT_EQ(memory.read_u64(kCtx + 0xA8).value(), kSavedRsi);
    EXPECT_EQ(memory.read_u64(kCtx + 0xA0).value(), kCallerRbp);
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kCallerRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kFrameBase + 0x30);
    EXPECT_EQ(memory.read_u64(kEstablisher).value(), kFrameBase);
}

TEST_F(NtdllExtTest, RtlVirtualUnwindFollowsChainInfoToPrimaryHandler) {
    // Learn x64 EH: UNW_FLAG_CHAININFO ends with a RUNTIME_FUNCTION for
    // the primary UNWIND_INFO. The language handler is on the primary;
    // popping RIP on the chained node skips the catcher.
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

    constexpr GuestAddr kChainFunc = kImage + 0x200;
    constexpr GuestAddr kPrimaryUnwind = kImage + 0x300;
    constexpr GuestAddr kChainUnwind = kImage + 0x340;
    constexpr u32 kPrimaryBegin = 0x400;
    constexpr u32 kChainBegin = 0x450;
    constexpr u32 kHandlerRva = 0x600;
    constexpr u32 kFuncInfoRva = 0x700;

    ASSERT_TRUE(memory.write_u32(kChainFunc + 0, kChainBegin).is_ok());
    ASSERT_TRUE(memory.write_u32(kChainFunc + 4, kChainBegin + 0x30).is_ok());
    ASSERT_TRUE(memory.write_u32(kChainFunc + 8, 0x340).is_ok());

    // Primary: PUSH RBP + EHANDLER. Version 1, flags=1 → 0x09.
    ASSERT_TRUE(memory.write_u8(kPrimaryUnwind + 0, 0x09).is_ok());
    ASSERT_TRUE(memory.write_u8(kPrimaryUnwind + 1, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kPrimaryUnwind + 2, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kPrimaryUnwind + 3, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(kPrimaryUnwind + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u8(kPrimaryUnwind + 5, (5u << 4) | 0u).is_ok());
    ASSERT_TRUE(memory.write_u16(kPrimaryUnwind + 6, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kPrimaryUnwind + 8, kHandlerRva).is_ok());
    ASSERT_TRUE(memory.write_u32(kPrimaryUnwind + 12, kFuncInfoRva).is_ok());

    // Chained fragment: ALLOC_SMALL 16 + CHAININFO. Version 1, flags=4 → 0x21.
    ASSERT_TRUE(memory.write_u8(kChainUnwind + 0, 0x21).is_ok());
    ASSERT_TRUE(memory.write_u8(kChainUnwind + 1, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kChainUnwind + 2, 1).is_ok());
    ASSERT_TRUE(memory.write_u8(kChainUnwind + 3, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(kChainUnwind + 4, 8).is_ok());
    ASSERT_TRUE(memory.write_u8(kChainUnwind + 5, (1u << 4) | 2u).is_ok());
    ASSERT_TRUE(memory.write_u16(kChainUnwind + 6, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kChainUnwind + 8, kPrimaryBegin).is_ok());
    ASSERT_TRUE(memory.write_u32(kChainUnwind + 12, kPrimaryBegin + 0x80).is_ok());
    ASSERT_TRUE(memory.write_u32(kChainUnwind + 16, 0x300).is_ok());

    constexpr GuestAddr kCallerRip = 0x14000CAFEull;
    constexpr GuestAddr kSavedRbp = 0x0000000000FEEDBACEull;
    constexpr GuestAddr kFrameRsp = kStack + 0x100;
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 16, kSavedRbp).is_ok());
    ASSERT_TRUE(memory.write_u64(kFrameRsp + 24, kCallerRip).is_ok());
    ASSERT_TRUE(memory.write_u64(kCtx + 0x98, kFrameRsp).is_ok());

    constexpr GuestAddr kHandlerData = kCtx + 0x600;
    constexpr GuestAddr kEstablisher = kCtx + 0x608;
    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    // HandlerType: UNW_FLAG_EHANDLER, the kind the primary declares (Learn,
    // RtlVirtualUnwind: only a handler of a requested kind is returned).
    cpu.gpr.rcx = 1;
    cpu.gpr.rdx = kImage;
    cpu.gpr.r8 = kImage + kChainBegin + 0x10;
    cpu.gpr.r9 = kChainFunc;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kCtx).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kHandlerData).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, kEstablisher).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    unwind->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, kImage + kHandlerRva);
    EXPECT_EQ(memory.read_u64(kCtx + 0xF8).value(), kCallerRip);
    EXPECT_EQ(memory.read_u64(kCtx + 0x98).value(), kFrameRsp + 32);
    EXPECT_EQ(memory.read_u64(kCtx + 0xA0).value(), kSavedRbp);
    EXPECT_EQ(memory.read_u64(kHandlerData).value(), kPrimaryUnwind + 12);
}
