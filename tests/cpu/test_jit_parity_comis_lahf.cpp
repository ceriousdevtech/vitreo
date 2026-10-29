// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// JIT vs Interpreter parity tests (32-bit mode): COMISS/UCOMISS/COMISD/UCOMISD and LAHF/SAHF
//
// Fixture and shared helpers: jit_parity_fixture.hpp.

#include "jit_parity_fixture.hpp"

// ============================================================================
// Native COMISS / UCOMISS / COMISD / UCOMISD flag encoding (SDM Vol. 2)
// ============================================================================

static void seed_dirty_status_flags(CpuState& cpu) {
    cpu.flags.set_of(true);
    cpu.flags.set_sf(true);
    cpu.flags.set_af(true);
    cpu.flags.set_zf(true);
    cpu.flags.set_pf(true);
    cpu.flags.set_cf(true);
}

TEST_F(JitParityTest, ComissGreaterNative) {
    // 0F 2F C1 — COMISS xmm0, xmm1
    check_parity({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 5.0f;
        cpu.sse.xmm[1].f32_[0] = 2.0f;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComissLessNative) {
    check_parity({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 1.0f;
        cpu.sse.xmm[1].f32_[0] = 5.0f;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComissEqualNative) {
    check_parity({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 3.0f;
        cpu.sse.xmm[1].f32_[0] = 3.0f;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComissEqualSignedZerosNative) {
    check_parity({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 0.0f;
        cpu.sse.xmm[1].f32_[0] = -0.0f;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComissQNaNUnorderedNative) {
    check_parity({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = std::numeric_limits<float>::quiet_NaN();
        cpu.sse.xmm[1].f32_[0] = 1.0f;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, UcomissQNaNUnorderedNative) {
    // 0F 2E C1 — UCOMISS xmm0, xmm1 (same flags as COMISS; QNaN #IA differs)
    check_parity({0x0F, 0x2E, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 1.0f;
        cpu.sse.xmm[1].f32_[0] = std::numeric_limits<float>::quiet_NaN();
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComissMemNative) {
    // 0F 2F 05 disp32 — COMISS xmm0, [0x2000]
    const float src = -4.0f;
    ASSERT_TRUE(m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&src),
                                      sizeof(src)).is_ok());
    check_parity({0x0F, 0x2F, 0x05, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            seed_dirty_status_flags(cpu);
            cpu.sse.xmm[0].f32_[0] = 1.0f;
        },
        /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComisdGreaterNative) {
    // 66 0F 2F C1 — COMISD xmm0, xmm1
    check_parity({0x66, 0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f64_[0] = 9.0;
        cpu.sse.xmm[1].f64_[0] = 3.0;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, UcomisdQNaNUnorderedNative) {
    // 66 0F 2E C1 — UCOMISD xmm0, xmm1
    check_parity({0x66, 0x0F, 0x2E, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f64_[0] = std::numeric_limits<double>::quiet_NaN();
        cpu.sse.xmm[1].f64_[0] = 0.0;
    }, /*check_flags=*/true, /*check_xmm=*/true);
}

TEST_F(JitParityTest, ComisdMemNative) {
    // 66 0F 2F 05 disp32 — COMISD xmm0, [0x2000]
    const double src = 8.0;
    ASSERT_TRUE(m_memory->write_bytes(0x2000, reinterpret_cast<const u8*>(&src),
                                      sizeof(src)).is_ok());
    check_parity({0x66, 0x0F, 0x2F, 0x05, 0x00, 0x20, 0x00, 0x00},
        [](CpuState& cpu) {
            seed_dirty_status_flags(cpu);
            cpu.sse.xmm[0].f64_[0] = 2.0;
        },
        /*check_flags=*/true, /*check_xmm=*/true);
}

// ---------------------------------------------------------------------------
// COMISS/UCOMISS as a FULL flag writer: it sets ZF/PF/CF from the compare and
// clears OF/SF/AF (Intel SDM Vol. 2), so any pending lazy FlagOp, cf_overlay
// or of_overlay is dead and must not survive into the result. The JIT lowers
// this branch-free off ARM NZCV with no materialize helper; these run with
// VITREO_JIT_NO_DELEGATE so an interpreter delegate cannot pass for a
// lowering, and they check AF, which the default check_parity skips.
// ---------------------------------------------------------------------------

class JitComisFlagTest : public JitParityTest {
protected:
    void native_af(std::initializer_list<u8> code,
                   std::function<void(CpuState&)> setup) {
        ScopedEnvVar no_delegate("VITREO_JIT_NO_DELEGATE", "1");
        std::vector<u8> v(code);
        check_parity(v.data(), v.size(), setup, /*check_flags=*/true,
                     /*check_xmm=*/true, /*require_jit=*/true,
                     /*check_af=*/true);
    }
};

TEST_F(JitComisFlagTest, ComissClearsOfSfAfGreater) {
    native_af({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 5.0f;
        cpu.sse.xmm[1].f32_[0] = 2.0f;
    });
}

TEST_F(JitComisFlagTest, ComissClearsOfSfAfLess) {
    native_af({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = -7.5f;
        cpu.sse.xmm[1].f32_[0] = 2.0f;
    });
}

TEST_F(JitComisFlagTest, ComissClearsOfSfAfEqual) {
    native_af({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 3.25f;
        cpu.sse.xmm[1].f32_[0] = 3.25f;
    });
}

TEST_F(JitComisFlagTest, ComissSignedZerosCompareEqual) {
    native_af({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = -0.0f;
        cpu.sse.xmm[1].f32_[0] = 0.0f;
    });
}

TEST_F(JitComisFlagTest, ComissUnorderedSetsZfPfCf) {
    native_af({0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = std::numeric_limits<float>::quiet_NaN();
        cpu.sse.xmm[1].f32_[0] = 1.0f;
    });
}

TEST_F(JitComisFlagTest, UcomissUnorderedSetsZfPfCf) {
    native_af({0x0F, 0x2E, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f32_[0] = 1.0f;
        cpu.sse.xmm[1].f32_[0] =
            std::numeric_limits<float>::signaling_NaN();
    });
}

TEST_F(JitComisFlagTest, UcomisdUnorderedSetsZfPfCf) {
    native_af({0x66, 0x0F, 0x2E, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f64_[0] = std::numeric_limits<double>::quiet_NaN();
        cpu.sse.xmm[1].f64_[0] = -2.0;
    });
}

TEST_F(JitComisFlagTest, ComisdLessNative) {
    native_af({0x66, 0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.sse.xmm[0].f64_[0] = -1.0;
        cpu.sse.xmm[1].f64_[0] = 1.0;
    });
}

// A pending lazy ADD owns all six flags. COMISS overwrites all six, so the
// JIT discards the record instead of materializing it; if the discard were
// wrong (or the ADD leaked through) OF/SF/AF would not read back as 0.
TEST_F(JitComisFlagTest, ComissDiscardsPendingLazyAdd) {
    // 01 C8 — ADD EAX, ECX ; 0F 2F C1 — COMISS xmm0, xmm1
    native_af({0x01, 0xC8, 0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x7FFFFFFFu);   // signed overflow: OF=1, SF=1, AF=1
        cpu.gpr.set32(ECX, 1u);
        cpu.sse.xmm[0].f32_[0] = 9.0f;
        cpu.sse.xmm[1].f32_[0] = 4.0f;     // greater: ZF=PF=CF=0
    });
}

TEST_F(JitComisFlagTest, ComissDiscardsPendingLazyAddUnordered) {
    native_af({0x01, 0xC8, 0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x7FFFFFFFu);
        cpu.gpr.set32(ECX, 1u);
        cpu.sse.xmm[0].f32_[0] = std::numeric_limits<float>::quiet_NaN();
        cpu.sse.xmm[1].f32_[0] = 4.0f;     // unordered: ZF=PF=CF=1
    });
}

// STC installs a cf_overlay over a live FlagOp; COMISS must clear it, or a
// "greater" compare would read back CF=1.
TEST_F(JitComisFlagTest, ComissClearsCfOverlay) {
    // 01 C8 ADD EAX,ECX ; F9 STC ; 0F 2F C1 COMISS
    native_af({0x01, 0xC8, 0xF9, 0x0F, 0x2F, 0xC1}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x10u);
        cpu.gpr.set32(ECX, 0x20u);
        cpu.sse.xmm[0].f32_[0] = 9.0f;
        cpu.sse.xmm[1].f32_[0] = 4.0f;
    });
}

// ROR reg,1 installs an of_overlay over a live FlagOp (partial-flag writer).
// COMISS clears OF, so the overlay must not survive either.
TEST_F(JitComisFlagTest, ComissClearsOfOverlay) {
    // 01 C8 ADD EAX,ECX ; D1 C8 ROR EAX,1 ; 0F 2F C1 COMISS
    native_af({0x01, 0xC8, 0xD1, 0xC8, 0x0F, 0x2F, 0xC1},
              [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x80000001u);
        cpu.gpr.set32(ECX, 0x00000002u);
        cpu.sse.xmm[0].f32_[0] = 1.0f;
        cpu.sse.xmm[1].f32_[0] = 8.0f;   // less: CF=1, ZF=PF=0
    });
}

// ---------------------------------------------------------------------------
// LAHF / SAHF vs the lazy-flag record.
//
// LAHF only READS flags, so it must see a materialized RFLAGS; SAHF is a
// PARTIAL writer (SF/ZF/AF/PF/CF, OF untouched — Intel SDM Vol. 2), so a
// pending FlagOp still owns OF and cannot be discarded. Both take an inline
// fast path when nothing is pending; these cover both sides of that branch.
// ---------------------------------------------------------------------------

TEST_F(JitComisFlagTest, LahfWithNoPendingLazyOp) {
    // 9F LAHF with nothing pending — the inline fast path.
    native_af({0x9F}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.gpr.set32(EAX, 0x11223344u);
    });
}

TEST_F(JitComisFlagTest, LahfAfterPendingLazyAdd) {
    // 01 D8 ADD EAX,EBX ; 9F LAHF — LAHF must observe the ADD's flags.
    native_af({0x01, 0xD8, 0x9F}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x7FFFFFFFu);
        cpu.gpr.set32(EBX, 1u);
    });
}

TEST_F(JitComisFlagTest, LahfAfterCfOverlay) {
    // 01 D8 ADD EAX,EBX ; F9 STC ; 9F LAHF — the CF overlay must reach AH.
    native_af({0x01, 0xD8, 0xF9, 0x9F}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x10u);
        cpu.gpr.set32(EBX, 0x20u);
    });
}

TEST_F(JitComisFlagTest, LahfAfterOfOverlay) {
    // 01 D8 ADD ; D1 C8 ROR EAX,1 ; 9F LAHF (OF is not in AH, but the overlay
    // must not perturb the five bits that are).
    native_af({0x01, 0xD8, 0xD1, 0xC8, 0x9F}, [](CpuState& cpu) {
        cpu.gpr.set32(EAX, 0x80000001u);
        cpu.gpr.set32(EBX, 0x00000002u);
    });
}

TEST_F(JitComisFlagTest, SahfWithNoPendingLazyOp) {
    // B4 D5 MOV AH,0xD5 ; 9E SAHF
    native_af({0xB4, 0xD5, 0x9E}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.gpr.set32(EAX, 0x11223344u);
    });
}

TEST_F(JitComisFlagTest, SahfWithNoPendingLazyOpClearBits) {
    native_af({0xB4, 0x00, 0x9E}, [](CpuState& cpu) {
        seed_dirty_status_flags(cpu);
        cpu.gpr.set32(EAX, 0x11223344u);
    });
}

// SAHF leaves OF alone, so the pending ADD's OF has to be frozen into RFLAGS
// before AH is applied. Discarding the record (COMISS's trick) would be wrong
// here: OF would read back as 0 instead of the ADD's 1.
TEST_F(JitComisFlagTest, SahfPreservesPendingLazyOf) {
    // 01 D9 ADD ECX,EBX ; B4 00 MOV AH,0 ; 9E SAHF
    native_af({0x01, 0xD9, 0xB4, 0x00, 0x9E}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x7FFFFFFFu);   // OF=1 from the ADD
        cpu.gpr.set32(EBX, 1u);
        cpu.gpr.set32(EAX, 0u);
    });
}

TEST_F(JitComisFlagTest, SahfAfterOfOverlayPreservesOf) {
    // 01 D9 ADD ECX,EBX ; D1 C9 ROR ECX,1 ; B4 D5 MOV AH,0xD5 ; 9E SAHF
    native_af({0x01, 0xD9, 0xD1, 0xC9, 0xB4, 0xD5, 0x9E},
              [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x80000001u);
        cpu.gpr.set32(EBX, 0x00000002u);
        cpu.gpr.set32(EAX, 0u);
    });
}

TEST_F(JitComisFlagTest, LahfSahfRoundTripAfterPendingAdd) {
    // 01 D9 ADD ECX,EBX ; 9F LAHF ; 9E SAHF ; 9F LAHF
    native_af({0x01, 0xD9, 0x9F, 0x9E, 0x9F}, [](CpuState& cpu) {
        cpu.gpr.set32(ECX, 0x7FFFFFFFu);
        cpu.gpr.set32(EBX, 1u);
        cpu.gpr.set32(EAX, 0u);
    });
}
