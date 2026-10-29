// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): memory operands
// on the inline access fast path.

#include "jit_parity_64_fixture.hpp"

// ============================================================================
// Long-mode memory operands routed through the inline access fast path.
//
// Every 8/16/32/64-bit memory operand in compile_64bit now goes through
// emit_guest_load*/emit_guest_store*, which service a hit inside a published
// region without calling a helper. The emitters use different scratch
// registers than the hand-rolled helper calls they replaced (X9/X3/X4/X8, plus
// X17 and X10 for stores), so the hazard is a value that was live across the
// access in one of those registers -- a corrupted operand or a store of the
// wrong value, not a crash. These run with the interpreter delegate disabled
// so the native path is the one under test, at addresses above 4 GB where only
// the long-mode region slots apply.
// ============================================================================

TEST_F(JitParity64Test, NativeByteMovMemoryRoundTripThroughInlinePath) {
    constexpr GuestAddr data_address = 0x140020000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address + 5, 0xC3));
    // MOV CL,[RAX+5] ; MOV [RAX+6],CL
    check_native_parity({0x8A, 0x48, 0x05, 0x88, 0x48, 0x06},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 0xAAAABBBBCCCCDDEEull);
        },
        /*check_flags=*/false);
    auto stored = m_memory->read_u8(data_address + 6);
    ASSERT_TRUE(stored.is_ok());
    EXPECT_EQ(stored.value(), 0xC3);
}

TEST_F(JitParity64Test, NativeCmp8MemoryOperandMatchesInterpreter) {
    constexpr GuestAddr data_address = 0x140021000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address + 1, 0x0D));
    // The memchr('\r') shape: CMP byte ptr [RAX+1], 0x0D
    check_native_parity({0x80, 0x78, 0x01, 0x0D},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
}

TEST_F(JitParity64Test, NativeCmp8RegAgainstMemoryMatchesInterpreter) {
    constexpr GuestAddr data_address = 0x140022000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address + 2, 0x41));
    // CMP CL, byte ptr [RAX+2] — the LHS in W0 must survive the load.
    check_native_parity({0x3A, 0x48, 0x02},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 0xFFFFFFFFFFFFFF41ull);
        });
}

TEST_F(JitParity64Test, NativeSub8MemoryDestinationReadModifyWrite) {
    constexpr GuestAddr data_address = 0x140023000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address + 3, 0x10));
    ASSERT_TRUE(m_memory->write_u8(data_address + 4, 0xEE));  // neighbour
    // SUB byte ptr [RAX+3], 0x20 — borrows, and must not touch [RAX+4].
    check_native_parity({0x80, 0x68, 0x03, 0x20},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
    auto neighbour = m_memory->read_u8(data_address + 4);
    ASSERT_TRUE(neighbour.is_ok());
    EXPECT_EQ(neighbour.value(), 0xEE);
}

TEST_F(JitParity64Test, NativeOr8MemoryDestinationReadModifyWrite) {
    constexpr GuestAddr data_address = 0x140024000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address, 0x0F));
    // OR byte ptr [RAX], CL
    check_native_parity({0x08, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 0x11112222333344F0ull);
        });
}

TEST_F(JitParity64Test, NativeTest8MemoryOperandMatchesInterpreter) {
    constexpr GuestAddr data_address = 0x140025000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address, 0x80));
    // TEST byte ptr [RAX], 0x80
    check_native_parity({0xF6, 0x00, 0x80},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
}

TEST_F(JitParity64Test, NativeIncByteMemoryPreservesCarry) {
    constexpr GuestAddr data_address = 0x140026000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address, 0xFF));
    // INC byte ptr [RAX] — wraps to 0, and CF must survive the RMW because
    // the captured carry is held in X21 across both memory accesses.
    check_native_parity({0xFE, 0x00},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.flags.set_cf(true);
        });
}

TEST_F(JitParity64Test, NativeShrByteMemoryByOneMatchesInterpreter) {
    constexpr GuestAddr data_address = 0x140027000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address, 0x81));
    // SHR byte ptr [RAX], 1 — CF takes the shifted-out bit.
    check_native_parity({0xD0, 0x28},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
}

TEST_F(JitParity64Test, NativeMovsxFromMemoryMatchesInterpreter) {
    constexpr GuestAddr data_address = 0x140028000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u8(data_address, 0x80));
    ASSERT_TRUE(m_memory->write_u16(data_address + 2, 0x8001));
    ASSERT_TRUE(m_memory->write_u32(data_address + 4, 0x80000001u));
    check_native_parity({
            0x48, 0x0F, 0xBE, 0x08,        // MOVSX RCX, byte ptr [RAX]
            0x0F, 0xBE, 0x50, 0x00,        // MOVSX EDX, byte ptr [RAX]
            0x0F, 0xBF, 0x58, 0x02,        // MOVSX EBX, word ptr [RAX+2]
            0x48, 0x63, 0x70, 0x04,        // MOVSXD RSI, dword ptr [RAX+4]
            0x0F, 0xB6, 0x78, 0x00,        // MOVZX EDI, byte ptr [RAX]
        },
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); },
        /*check_flags=*/false);
}

TEST_F(JitParity64Test, NativeCmp16AndTest16MemoryMatchInterpreter) {
    constexpr GuestAddr data_address = 0x140029000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u16(data_address, 0x8000));
    // CMP word ptr [RAX], 0x7FFF
    check_native_parity({0x66, 0x81, 0x38, 0xFF, 0x7F},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
    // TEST word ptr [RAX], 0x8000
    check_native_parity({0x66, 0xF7, 0x00, 0x00, 0x80},
        [](CpuState& cpu) { cpu.gpr.set64(RAX, data_address); });
}

TEST_F(JitParity64Test, NativeCmov32FromMemoryKeepsLoadedValue) {
    constexpr GuestAddr data_address = 0x14002A000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u32(data_address, 0xDEADBEEFu));
    // TEST EAX,EAX ; CMOVZ ECX,[RDX] — the loaded value lives in X21 across
    // the condition call, so a clobber there shows up as the wrong result.
    check_native_parity({0x85, 0xC0, 0x0F, 0x44, 0x0A},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0);  // ZF=1 -> the move is taken
            cpu.gpr.set64(RCX, 0x1111222233334444ull);
            cpu.gpr.set64(RDX, data_address);
        });
    // And the not-taken direction.
    check_native_parity({0x85, 0xC0, 0x0F, 0x44, 0x0A},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 1);  // ZF=0
            cpu.gpr.set64(RCX, 0x1111222233334444ull);
            cpu.gpr.set64(RDX, data_address);
        });
}

TEST_F(JitParity64Test, NativeAlu32MemoryDestinationStoresThroughInlinePath) {
    constexpr GuestAddr data_address = 0x14002B000ull;
    map_tracked_region(data_address, 0x1000);
    ASSERT_TRUE(m_memory->write_u32(data_address, 0x0000FFFFu));
    ASSERT_TRUE(m_memory->write_u32(data_address + 4, 0xA5A5A5A5u));
    // ADD dword ptr [RAX], ECX — must not disturb the following dword.
    check_native_parity({0x01, 0x08},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, data_address);
            cpu.gpr.set64(RCX, 0xFFFFFFFF00000001ull);
        });
    auto neighbour = m_memory->read_u32(data_address + 4);
    ASSERT_TRUE(neighbour.is_ok());
    EXPECT_EQ(neighbour.value(), 0xA5A5A5A5u);
}

TEST_F(JitParity64Test, NativeAlu32MemoryRmwCoversOrderedGranuleOffsets) {
    constexpr GuestAddr data_address = 0x14002C000ull;
    map_tracked_region(data_address, 0x1000);

    // Two ADDs make the first checked access publish the R/W slot and the
    // second exercise the guarded ordered hit. Host offsets 13..15 make the
    // dword cross a 16-byte granule and therefore must take the safe fallback.
    const std::initializer_list<u8> code = {
        0x83, 0x00, 0x01,  // ADD dword ptr [RAX],1
        0x83, 0x00, 0x01,  // ADD dword ptr [RAX],1
    };
    for (u32 offset = 0; offset < 16; ++offset) {
        SCOPED_TRACE(::testing::Message() << "host offset " << offset);
        const GuestAddr target = data_address + offset;
        const Byte* host = m_memory->host_ptr(target, sizeof(u32));
        ASSERT_NE(host, nullptr);
        ASSERT_EQ(reinterpret_cast<uintptr_t>(host) & 0xFu, offset);
        ASSERT_TRUE(m_memory->write_u32(target, 0x7FFFFFFEu).is_ok());

        // Keep this alignment matrix from tripping the compiler's deliberate
        // same-RIP recompile demotion after eight independently checked cases.
        m_jit = std::make_unique<JitCompiler>(*m_memory);
        check_native_parity(code, [target](CpuState& cpu) {
            cpu.gpr.set64(RAX, target);
        });

        const auto stored = m_memory->read_u32(target);
        ASSERT_TRUE(stored.is_ok());
        EXPECT_EQ(stored.value(), 0x80000000u);
    }
}

TEST_F(JitParity64Test, HybridTsoScalarLoadStoreCoversAllGranuleOffsets) {
    ScopedEnvironmentVariable no_tso("VITREO_JIT_NO_TSO", "0");
    ScopedEnvironmentVariable ordered("VITREO_JIT_TSO_ORDERED", "0");
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    constexpr GuestAddr data_address = 0x14002D000ull;
    constexpr u64 store_value = 0x8877665544332211ull;
    map_tracked_region(data_address, 0x1000);

    for (const int width : {1, 2, 4, 8}) {
        std::vector<u8> code;
        switch (width) {
            case 1:
                code = {0x88, 0x08,              // MOV byte ptr [RAX],CL
                        0x0F, 0xB6, 0x10};       // MOVZX EDX,byte ptr [RAX]
                break;
            case 2:
                code = {0x66, 0x89, 0x08,        // MOV word ptr [RAX],CX
                        0x0F, 0xB7, 0x10};       // MOVZX EDX,word ptr [RAX]
                break;
            case 4:
                code = {0x89, 0x08,              // MOV dword ptr [RAX],ECX
                        0x8B, 0x10};             // MOV EDX,dword ptr [RAX]
                break;
            default:
                code = {0x48, 0x89, 0x08,        // MOV qword ptr [RAX],RCX
                        0x48, 0x8B, 0x10};       // MOV RDX,qword ptr [RAX]
                break;
        }

        for (u32 offset = 0; offset < 16; ++offset) {
            SCOPED_TRACE(::testing::Message()
                         << "width " << width << ", host offset " << offset);
            const GuestAddr target = data_address + offset;
            const Byte* host = m_memory->host_ptr(target, width);
            ASSERT_NE(host, nullptr);
            ASSERT_EQ(reinterpret_cast<uintptr_t>(host) & 0xFu, offset);

            std::vector<u8> bytes(32, 0xA5);
            ASSERT_TRUE(m_memory->write_bytes(
                data_address, bytes.data(), bytes.size()));

            // The TSO mode is captured at construction. Recreate the compiler
            // to exercise the shipping hybrid mode and avoid same-RIP churn
            // demotion over this 64-case alignment matrix.
            m_jit = std::make_unique<JitCompiler>(*m_memory);
            check_parity(code.data(), code.size(), [target](CpuState& cpu) {
                cpu.gpr.set64(RAX, target);
                cpu.gpr.set64(RCX, store_value);
                cpu.gpr.set64(RDX, ~store_value);
            }, /*check_flags=*/false, /*check_xmm=*/false,
               /*require_jit=*/true);

            ASSERT_TRUE(m_memory->read_bytes(
                data_address, bytes.data(), bytes.size()));
            for (usize i = 0; i < bytes.size(); ++i) {
                const bool written = i >= offset && i < offset + width;
                const u8 expected = written
                    ? static_cast<u8>(store_value >> ((i - offset) * 8))
                    : 0xA5;
                EXPECT_EQ(bytes[i], expected) << "byte " << i;
            }
        }
    }
}
