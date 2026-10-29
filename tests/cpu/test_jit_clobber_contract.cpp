// Emitter clobber contracts, checked against the emitted words (ADR-0006).
//
// The failure this exists for: `emit_lazy_flag_store_raw` was widened to write
// REG_SCRATCH4 so it could pair flag_src/flag_dst into one STP. `shifts.cpp`
// and `mul_div.cpp` compute a value into the scratches and publish it
// immediately, so that destroyed the CF/OF they had parked there. It surfaced
// as CF *and* OF diverging from the interpreter on every
// Sh{l,r}Rm{8,16}By1_CarryOut and SarRm{8,16}Imm8 parity test -- a long way
// from the emitter, and only after a full build.
//
// A declared clobber set turns that into an immediate, local failure, and
// makes widening the set a visible act rather than a silent one.
#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "core/cpu/jit/jit_compiler.hpp"
#include "core/memory/guest_memory.hpp"

namespace {

using vitreo::u32;
using vitreo::u8;
using vitreo::cpu::jit::JitCompiler;

/// Which GPRs a single ARM64 word writes.
///
/// Deliberately NOT a general disassembler. It covers the encodings this JIT
/// emits and reports anything else as UNKNOWN, which fails the test. Guessing
/// would be worse than useless here: a contract check that silently passes on
/// an encoding it does not understand is not a check.
struct Writes {
    std::set<u8> regs;
    bool unknown = false;
};

Writes gpr_writes(u32 w) {
    Writes r;
    const u32 op0 = (w >> 25) & 0xFu;  // bits 28:25, the top-level group
    const u8 rd = static_cast<u8>(w & 0x1Fu);

    // Data Processing -- Immediate (100x). Every form writes Rd.
    if ((op0 & 0xEu) == 0x8u) {
        if (rd != 31) r.regs.insert(rd);
        return r;
    }

    // Data Processing -- Register (x101).
    if ((op0 & 0x7u) == 0x5u) {
        // CCMP/CCMN write NZCV only; their bits 4:0 are an immediate, not Rd.
        if ((w & 0x1FE00800u) == 0x1A400000u) return r;
        if (rd != 31) r.regs.insert(rd);
        return r;
    }

    // Branches, Exception, System (101x).
    if ((op0 & 0xEu) == 0xAu) {
        if ((w & 0xFC000000u) == 0x94000000u) {  // BL writes the link register
            r.regs.insert(30);
            return r;
        }
        if ((w & 0xFFF00000u) == 0xD5300000u) {  // MRS writes Rt
            if (rd != 31) r.regs.insert(rd);
            return r;
        }
        // B, B.cond, CBZ/CBNZ, TBZ/TBNZ, MSR, RET: no GPR write.
        return r;
    }

    // Loads and Stores (x1x0).
    if ((op0 & 0x5u) == 0x4u) {
        const bool vector = ((w >> 26) & 1u) != 0;  // V bit: FP/SIMD transfer
        // Load/store pair: bits 29:27 == 101.
        if (((w >> 27) & 0x7u) == 0x5u) {
            if (((w >> 22) & 1u) == 0u) return r;  // STP writes no register
            if (vector) return r;
            const u8 rt2 = static_cast<u8>((w >> 10) & 0x1Fu);
            if (rd != 31) r.regs.insert(rd);
            if (rt2 != 31) r.regs.insert(rt2);
            return r;
        }
        // Load/store register, unsigned offset or register offset: bits 29:27
        // == 111, and bits 23:22 are opc -- 00 is a store, anything else
        // loads into Rt.
        if (((w >> 27) & 0x7u) == 0x7u) {
            const u32 opc = (w >> 22) & 0x3u;
            if (opc == 0u) return r;   // STR / STRB / STRH
            if (vector) return r;      // loads a V register, not a GPR
            if (rd != 31) r.regs.insert(rd);
            return r;
        }
        r.unknown = true;
        return r;
    }

    // Data Processing -- Scalar FP and SIMD (x111). FMOV to a GPR writes Rd;
    // everything else writes a V register.
    if ((op0 & 0x7u) == 0x7u) {
        if ((w & 0x7FE06000u) == 0x1E260000u ||    // FMOV Wd, Sn
            (w & 0x7FE06000u) == 0x9E660000u) {    // FMOV Xd, Dn
            if (rd != 31) r.regs.insert(rd);
        }
        return r;
    }

    r.unknown = true;
    return r;
}

u32 mask_of(const std::vector<u32>& words, bool* unknown_out) {
    u32 mask = 0;
    *unknown_out = false;
    for (u32 w : words) {
        const Writes wr = gpr_writes(w);
        if (wr.unknown) *unknown_out = true;
        for (u8 g : wr.regs) mask |= (1u << g);
    }
    return mask;
}

std::string name_regs(u32 mask) {
    std::string s;
    for (u8 i = 0; i < 32; ++i) {
        if (mask & (1u << i)) s += (s.empty() ? "X" : ", X") + std::to_string(i);
    }
    return s.empty() ? "(none)" : s;
}

class JitClobberContract : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<vitreo::GuestMemory>();
        m_jit = std::make_unique<JitCompiler>(*m_memory);
    }
    std::unique_ptr<vitreo::GuestMemory> m_memory;
    std::unique_ptr<JitCompiler> m_jit;
};

// The decoder must be able to classify everything these emitters produce. An
// UNKNOWN word means the check silently stopped covering something.
TEST_F(JitClobberContract, DecoderClassifiesEveryEmittedWord) {
    for (auto which : {JitCompiler::TestEmitter::LazyFlagStoreNarrow,
                       JitCompiler::TestEmitter::LazyFlagStoreWide,
                       JitCompiler::TestEmitter::SetLazyFlagsPreserveCf,
                       JitCompiler::TestEmitter::ClearCfOverlay}) {
        bool unknown = false;
        const std::vector<u32> words = m_jit->emit_for_test(which);
        EXPECT_FALSE(words.empty());
        (void)mask_of(words, &unknown);
        EXPECT_FALSE(unknown)
            << "an emitted encoding is not classified by gpr_writes(); the "
               "contract check is no longer covering this emitter";
    }
}

// The contract itself.
TEST_F(JitClobberContract, LazyFlagPublicationWritesOnlyItsDeclaredScratch) {
    for (auto which : {JitCompiler::TestEmitter::LazyFlagStoreNarrow,
                       JitCompiler::TestEmitter::LazyFlagStoreWide,
                       JitCompiler::TestEmitter::SetLazyFlagsPreserveCf,
                       JitCompiler::TestEmitter::ClearCfOverlay}) {
        bool unknown = false;
        const u32 mask = mask_of(m_jit->emit_for_test(which), &unknown);
        const u32 extra = mask & ~JitCompiler::kLazyFlagStoreClobbers;
        EXPECT_EQ(extra, 0u)
            << "the lazy-flag publication writes " << name_regs(mask)
            << " but declares " << name_regs(JitCompiler::kLazyFlagStoreClobbers)
            << ".\nCallers rely on the declared set: shifts.cpp and "
               "mul_div.cpp compute into a scratch and publish immediately, so "
               "a wider clobber destroys their CF/OF.\nIf the widening is "
               "intended, change kLazyFlagStoreClobbers AND re-audit every "
               "publication site -- that edit is the point.";
    }
}

// A negative control: the check must actually reject a register outside the
// declared set, or it proves nothing.
TEST_F(JitClobberContract, ContractRejectsAnUndeclaredWrite) {
    const std::vector<u32> forged = {
        0x2A0603E4u,  // ORR W4, WZR, W6  -- a write to X4, as the real bug was
    };
    bool unknown = false;
    const u32 mask = mask_of(forged, &unknown);
    EXPECT_FALSE(unknown);
    EXPECT_EQ(mask, 1u << 4) << "decoder failed to see the write to X4";
    EXPECT_NE(mask & ~JitCompiler::kLazyFlagStoreClobbers, 0u)
        << "the contract check would not have caught the real REG_SCRATCH4 bug";
}

}  // namespace
