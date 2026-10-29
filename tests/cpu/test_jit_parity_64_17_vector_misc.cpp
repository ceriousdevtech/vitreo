// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests — 64-bit long mode (CPU-048): IAT thunks,
// VMOVDQU, blends, SUBPD and UCOMISD.

#include "jit_parity_64_fixture.hpp"

// chrome_elf IAT thunks live at 0x18xxxxxxxx and load from 0x18xxxxxxxx.
// The Rosetta catalog plants FF 25 at kCodeVa64=0x05000000, which cannot
// catch a >4GB RIP-relative EA or load truncation. Intel SDM Vol. 2 JMP
// r/m64: EA := RIP(next) + disp32; RIP := [EA].
TEST_F(JitParity64Test, JmpRipRelIatThunkHighImage) {
    constexpr u64 kThunk = 0x1801e9800ull;
    constexpr u64 kIat = 0x180219318ull;
    constexpr u64 kTarget = 0x180050000ull;
    const u32 disp = static_cast<u32>(kIat - (kThunk + 6));
    ASSERT_EQ(disp, 0x2fb12u);

    ASSERT_TRUE(m_memory->map_region(0x1801e9000ull, 0x2000,
                                     MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(m_memory->map_region(0x180219000ull, 0x1000,
                                     MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->map_region(0x180050000ull, 0x1000,
                                     MemoryProtection::ReadWriteExecute));

    const u8 thunk[6] = {
        0xFF, 0x25,
        static_cast<u8>(disp), static_cast<u8>(disp >> 8),
        static_cast<u8>(disp >> 16), static_cast<u8>(disp >> 24),
    };
    ASSERT_TRUE(m_memory->write_bytes(kThunk, thunk, sizeof(thunk)));
    ASSERT_TRUE(m_memory->write_u64(kIat, kTarget).is_ok());
    const u8 ret = 0xC3;
    ASSERT_TRUE(m_memory->write_bytes(kTarget, &ret, 1));

    MemoryByteReader reader(thunk, sizeof(thunk));
    DecodedInstruction inst{};
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
    ASSERT_EQ(inst.opcode, Opcode::JMP_RM64);
    ASSERT_EQ(inst.length, 6u);

    BasicBlock block;
    block.start_addr = kThunk;
    block.is_64bit = true;
    block.instructions.push_back(inst);
    block.byte_length = 6;
    block.instruction_count = 1;

    CpuState icpu;
    icpu.init_long_mode();
    icpu.rip = kThunk;
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    EXPECT_EQ(icpu.rip, kTarget);

    CpuState jcpu;
    jcpu.init_long_mode();
    jcpu.rip = kThunk;
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr) << "JMP [rip+disp] at a PE32+ IAT must be native";
    test::execute_like_dispatcher(*m_jit, *cb, jcpu);
    EXPECT_EQ(jcpu.rip, kTarget)
        << "JIT IAT thunk target interp=0x" << std::hex << icpu.rip
        << " jit=0x" << jcpu.rip;
}

// Partition-alloc bucket index from Spotify.exe 0x14001bcae..0x14001bd1d:
// size in RDX → CX, then `cmp cx, 0x6f; jae ud2`. JIT-on takes the UD2
// with RCX=0x6f; interpreter does not. Intel SDM BSR/ROR/SBB/CMOVB r64.
TEST_F(JitParity64Test, PartitionBucketIndexBsrRorCmovb) {
    static const u8 kCode[] = {
        0x48, 0x8B, 0x53, 0x10, 0x48, 0x8D, 0x82, 0xFF, 0xFF, 0x0F, 0x00,
        0x48, 0xC1, 0xE8, 0x04, 0x48, 0x85, 0xD2, 0x48, 0x0F, 0x44, 0xC2,
        0x41, 0xB8, 0x7F, 0x00, 0x00, 0x00, 0x4C, 0x0F, 0xBD, 0xC2,
        0x41, 0x83, 0xF0, 0x3F, 0xB9, 0x3C, 0x00, 0x00, 0x00, 0x44, 0x29, 0xC1,
        0x49, 0x89, 0xD1, 0x49, 0xD3, 0xC9, 0x45, 0x89, 0xCA, 0x41, 0x83, 0xE2, 0x07,
        0x41, 0xC1, 0xE0, 0x03, 0x4D, 0x29, 0xC2, 0x49, 0x83, 0xF9, 0x10,
        0x49, 0x83, 0xDA, 0xFF, 0x49, 0x81, 0xC2, 0xC7, 0x01, 0x00, 0x00,
        0x49, 0x83, 0xFA, 0x6F, 0xB9, 0x6F, 0x00, 0x00, 0x00, 0x49, 0x0F, 0x42, 0xCA,
        0x48, 0x81, 0xFA, 0x01, 0x01, 0x00, 0x00, 0x48, 0x0F, 0x42, 0xC8,
        0x66, 0x89, 0x4F, 0x34, 0x66, 0x83, 0xF9, 0x6F,
    };
    ASSERT_TRUE(m_memory->write_u64(0x3010, 0x2af000094000ull).is_ok());
    check_parity(kCode, sizeof(kCode), [](CpuState& cpu) {
        cpu.gpr.set64(RBX, 0x3000);
        cpu.gpr.set64(RDI, 0x4000);
    }, /*check_flags=*/true, /*check_xmm=*/false, /*require_jit=*/true);
}

// VEX.256 VMOVDQU must move all 32 bytes (Intel SDM Vol. 2, VMOVDQU).
// Native 128-bit MOVDQU lowering used to ignore VEX.L and leave dest[16:31]
// untouched — CRT AVX memcpy then left a stack pointer in a size field.
TEST_F(JitParity64Test, VmovdquYmmCopiesAll32Bytes) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    static const u8 kCode[] = {
        0xC5, 0xFE, 0x6F, 0x02,  // vmovdqu ymm0, [rdx]
        0xC5, 0xFE, 0x7F, 0x00,  // vmovdqu [rax], ymm0
    };
    u8 src[32];
    u8 fill[40];
    for (int i = 0; i < 32; ++i) src[i] = static_cast<u8>(0xA0 + i);
    std::memset(fill, 0x5A, sizeof(fill));
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3000, src, sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(0x3100, fill, sizeof(fill)).is_ok());

    BasicBlock block = build_block(kCode, sizeof(kCode));
    ASSERT_EQ(block.instruction_count, 2u);
    ASSERT_TRUE(block.instructions[0].prefixes.vex);
    ASSERT_TRUE(block.instructions[0].prefixes.vex_l);
    ASSERT_TRUE(block.instructions[1].prefixes.vex);
    ASSERT_TRUE(block.instructions[1].prefixes.vex_l);

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = 0x1000;
        cpu.gpr.set64(RDX, 0x3000);
        cpu.gpr.set64(RAX, 0x3100);
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(block.instruction_count);
    }
    u8 after_interp[40] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x3100, after_interp, sizeof(after_interp)).is_ok());

    ASSERT_TRUE(m_memory->write_bytes(0x3100, fill, sizeof(fill)).is_ok());
    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    test::execute_like_dispatcher(*m_jit, *cb, jcpu);

    u8 after_jit[40] = {};
    ASSERT_TRUE(m_memory->read_bytes(0x3100, after_jit, sizeof(after_jit)).is_ok());
    EXPECT_EQ(std::memcmp(after_interp, after_jit, sizeof(after_jit)), 0);
    EXPECT_EQ(std::memcmp(after_jit, src, sizeof(src)), 0);
    EXPECT_EQ(after_jit[32], 0x5A);
    for (int b = 0; b < 16; ++b) {
        EXPECT_EQ(icpu.sse.xmm[0].u8_[b], jcpu.sse.xmm[0].u8_[b]);
        EXPECT_EQ(icpu.avx.ymm_high[0].u8_[b], jcpu.avx.ymm_high[0].u8_[b]);
        EXPECT_EQ(jcpu.avx.ymm_high[0].u8_[b], src[16 + b]);
    }
}

// VEX.256 VBLENDVPS selects per 32-bit element by the mask element's sign bit
// (Intel SDM Vol. 2 VBLENDVPS). The lowering shares its shape with VPBLENDVB,
// so the element width fed to CMLT #0 is the thing under test: a byte-wide
// compare would leak the mask's low bytes into the selection.
TEST_F(JitParity64Test, VblendvpsYmmMatchesInterpreterPerDword) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    static const u8 kCode[] = {
        0xC4, 0xE3, 0x7D, 0x4A, 0xC1, 0x20,  // vblendvps ymm0, ymm0, ymm1, ymm2
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    BasicBlock block = build_block(kCode, sizeof(kCode));
    ASSERT_EQ(block.instruction_count, 1u);

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = 0x1000;
        for (int i = 0; i < 4; ++i) {
            cpu.sse.xmm[0].u32_[i] = 0x11111111u;
            cpu.avx.ymm_high[0].u32_[i] = 0x22222222u;
            cpu.sse.xmm[1].u32_[i] = 0xAAAAAAAAu;
            cpu.avx.ymm_high[1].u32_[i] = 0xBBBBBBBBu;
            cpu.sse.xmm[2].u32_[i] = (i % 2) ? 0x80000001u : 0x7FFFFFFFu;
            cpu.avx.ymm_high[2].u32_[i] = (i % 2) ? 0x7FFFFFFFu : 0x80000001u;
        }
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(block.instruction_count);
    }
    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    test::execute_like_dispatcher(*m_jit, *cb, jcpu);

    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(jcpu.sse.xmm[0].u32_[i], icpu.sse.xmm[0].u32_[i]);
        EXPECT_EQ(jcpu.avx.ymm_high[0].u32_[i], icpu.avx.ymm_high[0].u32_[i]);
        EXPECT_EQ(jcpu.sse.xmm[0].u32_[i], (i % 2) ? 0xAAAAAAAAu : 0x11111111u);
        EXPECT_EQ(jcpu.avx.ymm_high[0].u32_[i],
                  (i % 2) ? 0x22222222u : 0xBBBBBBBBu);
    }
}

// VEX.128 VBLENDVPD selects per 64-bit element and clears YMM[255:128].
TEST_F(JitParity64Test, VblendvpdXmmMatchesInterpreterPerQword) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    static const u8 kCode[] = {
        0xC4, 0xE3, 0x71, 0x4B, 0xC2, 0x50,  // vblendvpd xmm0, xmm1, xmm2, xmm5
    };
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    BasicBlock block = build_block(kCode, sizeof(kCode));
    ASSERT_EQ(block.instruction_count, 1u);

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = 0x1000;
        cpu.sse.xmm[1].u64_[0] = 0x1111111111111111ull;
        cpu.sse.xmm[1].u64_[1] = 0x2222222222222222ull;
        cpu.sse.xmm[2].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
        cpu.sse.xmm[2].u64_[1] = 0xBBBBBBBBBBBBBBBBull;
        cpu.sse.xmm[5].u64_[0] = 0x8000000000000000ull;
        cpu.sse.xmm[5].u64_[1] = 0x7FFFFFFFFFFFFFFFull;
        cpu.avx.ymm_high[0].u64_[0] = 0xDEADBEEFDEADBEEFull;
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(block.instruction_count);
    }
    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    test::execute_like_dispatcher(*m_jit, *cb, jcpu);

    EXPECT_EQ(jcpu.sse.xmm[0].u64_[0], icpu.sse.xmm[0].u64_[0]);
    EXPECT_EQ(jcpu.sse.xmm[0].u64_[1], icpu.sse.xmm[0].u64_[1]);
    EXPECT_EQ(jcpu.sse.xmm[0].u64_[0], 0xAAAAAAAAAAAAAAAAull);
    EXPECT_EQ(jcpu.sse.xmm[0].u64_[1], 0x2222222222222222ull);
    EXPECT_EQ(jcpu.avx.ymm_high[0].u64_[0], icpu.avx.ymm_high[0].u64_[0]);
    EXPECT_EQ(jcpu.avx.ymm_high[0].u64_[0], 0ull);
}

// Intel SDM Vol. 2 PBLENDW: imm8 bit i chooses source word i and otherwise
// retains the corresponding legacy destination word. The instruction does not
// modify flags. Cover the identity/full-source fast paths, alternating masks,
// and an irregular dense mask through both engines.
TEST_F(JitParity64Test, PblendwImmediateMasksMatchInterpreterAndPreserveFlags) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    for (const u8 mask : {u8{0x00}, u8{0xFF}, u8{0x55}, u8{0xAA}, u8{0xE7}}) {
        SCOPED_TRACE(::testing::Message() << "mask=0x" << std::hex
                                         << static_cast<unsigned>(mask));
        const u8 code[] = {0x66, 0x0F, 0x3A, 0x0E, 0xC1, mask};
        check_parity(code, sizeof(code), [](CpuState& cpu) {
            cpu.flags.rflags = 0x8D7u;
            for (u8 i = 0; i < 8; ++i) {
                cpu.sse.xmm[0].u16_[i] = static_cast<u16>(0x1000u + i);
                cpu.sse.xmm[1].u16_[i] = static_cast<u16>(0xA000u + i);
            }
        }, true, true, true);
    }
}

TEST_F(JitParity64Test, PblendwAlternatingMasksUseOneInstructionConstants) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    auto compile_words = [&](u8 mask) -> std::vector<u32> {
        const u8 code[] = {0x66, 0x0F, 0x3A, 0x0E, 0xC1, mask};
        if (!m_memory->write_bytes(0x1000, code, sizeof(code))) {
            ADD_FAILURE() << "failed to write PBLENDW test code";
            return {};
        }
        BasicBlock block = build_block(code, sizeof(code));
        CompiledBlock* compiled = m_jit->compile(block);
        EXPECT_NE(compiled, nullptr);
        std::vector<u32> words;
        if (compiled != nullptr) {
            const auto* bytes = static_cast<const u8*>(compiled->code);
            for (u32 offset = 0; offset + sizeof(u32) <= compiled->code_size;
                 offset += sizeof(u32)) {
                u32 word = 0;
                std::memcpy(&word, bytes + offset, sizeof(word));
                words.push_back(word);
            }
        }
        return words;
    };

    const auto alternating = compile_words(0x55);
    EXPECT_EQ(std::count(alternating.begin(), alternating.end(),
                         arm64::movi_v4s_ff_msl8(2)), 1);
    EXPECT_EQ(std::count(alternating.begin(), alternating.end(),
                         arm64::movi_v16b(2, 0xFF)), 0);
    EXPECT_EQ(std::count(alternating.begin(), alternating.end(),
                         arm64::bsl_v16b(2, 1, 0)), 1);

    const auto complement = compile_words(0xAA);
    EXPECT_EQ(std::count(complement.begin(), complement.end(),
                         arm64::mvni_v4s_ff_msl8(2)), 1);
    EXPECT_EQ(std::count(complement.begin(), complement.end(),
                         arm64::movi_v16b(2, 0xFF)), 0);
    EXPECT_EQ(std::count(complement.begin(), complement.end(),
                         arm64::bsl_v16b(2, 1, 0)), 1);

    const auto irregular = compile_words(0xE7);
    EXPECT_EQ(std::count(irregular.begin(), irregular.end(),
                         arm64::movi_v16b(2, 0xFF)), 1);
    EXPECT_EQ(std::count(irregular.begin(), irregular.end(),
                         arm64::bsl_v16b(2, 1, 0)), 1);

}

// VEX.256 VPMOVMSKB must concatenate the high-lane mask into bits [31:16]
// (Intel SDM Vol. 2). Overwriting with the high 16 bits made AVX2 wcslen
// treat trailing zeros in ymm[255:128] as a NUL at the start of the block.
TEST_F(JitParity64Test, VpmovmskbYmmConcatenatesHighLaneMask) {
    // Deliberately NOT VITREO_JIT_NO_DELEGATE: VEX.256 VPMOVMSKB is not
    // element-wise (it produces ONE 32-bit mask, not two 16-bit ones), so it
    // has no lane-split lowering and is correctly served by the interpreter
    // bridge. This test asserts the SEMANTICS, not the lowering. It carried
    // the no-delegate scope until 2026-08-22 and passed only because the
    // delegate gate cached its getenv on first use, so the variable never
    // took effect in a full-suite run.
    static const u8 kCode[] = {0xC5, 0xFD, 0xD7, 0xC1};  // vpmovmskb eax, ymm1
    BasicBlock block = build_block(kCode, sizeof(kCode));
    ASSERT_EQ(block.instruction_count, 1u);
    ASSERT_TRUE(block.instructions[0].prefixes.vex_l);

    auto setup = [](CpuState& cpu) {
        cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
        for (int i = 0; i < 16; ++i) {
            cpu.sse.xmm[1].u8_[i] = 0x01;
            cpu.avx.ymm_high[1].u8_[i] = 0x01;
        }
        cpu.avx.ymm_high[1].u8_[0] = 0x80;
    };

    check_parity(kCode, sizeof(kCode), setup, /*check_flags=*/false,
                 /*check_xmm=*/true, /*require_jit=*/true);

    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    EXPECT_EQ(icpu.gpr.get64(RAX), 0x00010000ull);
}

// SUBPD xmm, m128 must read both qwords at EA and EA+8 (Intel SDM Vol. 2).
// Native FINITE_FP used to add 8 to X4 after emit_x64_region_load clobbered
// it with (addr-base), so the high qword came from a small offset.
TEST_F(JitParity64Test, SubpdRipRelMemLoadsBothQwords) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    // 66 0F 5C 05 F8 1F 00 00 — SUBPD xmm0, [rip+0x1FF8] → 0x3000
    static const u8 kCode[] = {
        0x66, 0x0F, 0x5C, 0x05, 0xF8, 0x1F, 0x00, 0x00,
    };
    double src[2] = {3.0, 7.0};
    ASSERT_TRUE(m_memory->write_bytes(
        0x3000, reinterpret_cast<const u8*>(src), sizeof(src)).is_ok());

    auto setup = [](CpuState& cpu) {
        cpu.sse.xmm[0].f64_[0] = 10.0;
        cpu.sse.xmm[0].f64_[1] = 20.0;
    };

    check_parity(kCode, sizeof(kCode), setup, /*check_flags=*/false,
                 /*check_xmm=*/true, /*require_jit=*/true);

    CpuState icpu;
    init_cpu(icpu);
    setup(icpu);
    ASSERT_TRUE(m_memory->write_bytes(0x1000, kCode, sizeof(kCode)).is_ok());
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }
    EXPECT_DOUBLE_EQ(icpu.sse.xmm[0].f64_[0], 7.0);
    EXPECT_DOUBLE_EQ(icpu.sse.xmm[0].f64_[1], 13.0);
}

// Same contract above 4 GB. Native FINITE_FP used to overwrite the parked
// EA with the first loaded qword, so an Inf/NaN helper reread that bit
// pattern as the guest address (Spotify/CEF SUBPD jit-av at ~0x666628).
TEST_F(JitParity64Test, SubpdRipRelHighAddressLoadsBothQwords) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    constexpr GuestAddr kHighCode = 0x1400FAF42ull;
    constexpr GuestAddr kHighData = 0x140261190ull;
    // 66 0F 5C 15 46 62 16 00 — SUBPD xmm2, [rip+0x166246] → 0x140261190
    static const u8 kCode[] = {
        0x66, 0x0F, 0x5C, 0x15, 0x46, 0x62, 0x16, 0x00,
    };
    ASSERT_TRUE(m_memory->map_region(kHighCode & ~0xFFFull, 0x2000,
                                     MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(m_memory->map_region(kHighData & ~0xFFFull, 0x1000,
                                     MemoryProtection::ReadWrite));

    double src[2] = {3.0, 7.0};
    ASSERT_TRUE(m_memory->write_bytes(
        kHighData, reinterpret_cast<const u8*>(src), sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(kHighCode, kCode, sizeof(kCode)).is_ok());

    BasicBlock block;
    block.start_addr = kHighCode;
    block.is_64bit = true;
    MemoryByteReader reader(kCode, sizeof(kCode));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
    block.instructions.push_back(inst);
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = 1;

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = kHighCode;
        cpu.sse.xmm[2].f64_[0] = 10.0;
        cpu.sse.xmm[2].f64_[1] = 20.0;
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }

    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, jcpu);

    EXPECT_FALSE(jcpu.pending_raise.active);
    EXPECT_DOUBLE_EQ(jcpu.sse.xmm[2].f64_[0], icpu.sse.xmm[2].f64_[0]);
    EXPECT_DOUBLE_EQ(jcpu.sse.xmm[2].f64_[1], icpu.sse.xmm[2].f64_[1]);
    EXPECT_DOUBLE_EQ(jcpu.sse.xmm[2].f64_[0], 7.0);
    EXPECT_DOUBLE_EQ(jcpu.sse.xmm[2].f64_[1], 13.0);
}

TEST_F(JitParity64Test, SubpdRipRelHighAddressInfUsesHelper) {
    ScopedEnvironmentVariable no_delegate("VITREO_JIT_NO_DELEGATE", "1");
    constexpr GuestAddr kHighCode = 0x7FFA06F9E545ull;
    constexpr GuestAddr kHighData = 0x7FFA109B4620ull;
    // 66 0F 5C 05 D3 60 A1 09 — SUBPD xmm0, [rip+0x09A160D3]
    static const u8 kCode[] = {
        0x66, 0x0F, 0x5C, 0x05, 0xD3, 0x60, 0xA1, 0x09,
    };
    ASSERT_TRUE(m_memory->map_region(kHighCode & ~0xFFFull, 0x2000,
                                     MemoryProtection::ReadWriteExecute));
    ASSERT_TRUE(m_memory->map_region(kHighData & ~0xFFFull, 0x1000,
                                     MemoryProtection::ReadWrite));

    const double inf = std::numeric_limits<double>::infinity();
    double src[2] = {inf, 4.0};
    ASSERT_TRUE(m_memory->write_bytes(
        kHighData, reinterpret_cast<const u8*>(src), sizeof(src)).is_ok());
    ASSERT_TRUE(m_memory->write_bytes(kHighCode, kCode, sizeof(kCode)).is_ok());

    BasicBlock block;
    block.start_addr = kHighCode;
    block.is_64bit = true;
    MemoryByteReader reader(kCode, sizeof(kCode));
    DecodedInstruction inst;
    ASSERT_EQ(decode_instruction(reader, DecodeMode::Mode64, inst),
              DecodeError::Success);
    block.instructions.push_back(inst);
    block.byte_length = static_cast<u32>(reader.position());
    block.instruction_count = 1;

    auto setup = [](CpuState& cpu) {
        cpu.init_long_mode();
        cpu.gpr.set64(RSP, 0x10000);
        cpu.rip = kHighCode;
        cpu.sse.xmm[0].f64_[0] = 1.0;
        cpu.sse.xmm[0].f64_[1] = 8.0;
    };

    CpuState icpu;
    setup(icpu);
    {
        Interpreter interp(icpu, *m_memory);
        interp.run(1);
    }

    CpuState jcpu;
    setup(jcpu);
    CompiledBlock* cb = m_jit->compile(block);
    ASSERT_NE(cb, nullptr);
    (void)test::execute_like_dispatcher(*m_jit, *cb, jcpu);

    EXPECT_FALSE(jcpu.pending_raise.active)
        << "Inf/NaN helper reused the loaded qword as the guest address";
    EXPECT_DOUBLE_EQ(jcpu.sse.xmm[0].f64_[0], icpu.sse.xmm[0].f64_[0]);
    EXPECT_DOUBLE_EQ(jcpu.sse.xmm[0].f64_[1], icpu.sse.xmm[0].f64_[1]);
}

TEST_F(JitParity64Test, NativeUcomisdEqualStack) {
    // Intel SDM Vol. 2 UCOMISD: +0 == +0 → ZF=1, PF=0, CF=0.
    // 66 0F 2E 44 24 78 — UCOMISD xmm0, qword [rsp+0x78]
    const double src = 0.0;
    ASSERT_TRUE(m_memory->write_bytes(0x10000 + 0x78,
                                      reinterpret_cast<const u8*>(&src),
                                      sizeof(src)).is_ok());
    check_native_parity({0x66, 0x0F, 0x2E, 0x44, 0x24, 0x78},
        [](CpuState& cpu) {
            cpu.flags.set_zf(false);
            cpu.flags.set_cf(true);
            cpu.sse.xmm[0].f64_[0] = 0.0;
        },
        /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, NativeUcomisdXmm5EqualStack) {
    // PartitionBucket::Init: ucomisd xmm5, [rsp+0x78] / ja → 16 pages.
    // 66 0F 2E 6C 24 78 — UCOMISD xmm5, qword [rsp+0x78]
    const double src = 0.0;
    ASSERT_TRUE(m_memory->write_bytes(0x10000 + 0x78,
                                      reinterpret_cast<const u8*>(&src),
                                      sizeof(src)).is_ok());
    check_native_parity({0x66, 0x0F, 0x2E, 0x6C, 0x24, 0x78},
        [](CpuState& cpu) {
            cpu.flags.set_zf(false);
            cpu.flags.set_cf(true);
            cpu.sse.xmm[5].f64_[0] = 0.0;
        },
        /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParity64Test, NativeUcomisdEqualDoesNotTakeJa) {
    // 66 0F 2E C1 / 0F 97 C0 — UCOMISD xmm0, xmm1; SETA AL
    check_native_parity({0x66, 0x0F, 0x2E, 0xC1, 0x0F, 0x97, 0xC0},
        [](CpuState& cpu) {
            cpu.gpr.set64(RAX, 0xFFFFFFFFFFFFFFFFull);
            cpu.sse.xmm[0].f64_[0] = 0.0;
            cpu.sse.xmm[1].f64_[0] = 0.0;
        },
        /*check_flags=*/true, /*check_xmm=*/true);
}
