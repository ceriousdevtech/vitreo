// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// SIMD/crypto memory-READ fault contract, interpreter and JIT.
//
// Windows reports a faulting data read as STATUS_ACCESS_VIOLATION with
// ExceptionInformation[0] = 0 (read) and [1] = the first inaccessible byte.
// A misaligned legacy (non-VEX) m128 is #GP, which Windows also reports as
// STATUS_ACCESS_VIOLATION but with [1] = 0xFFFFFFFFFFFFFFFF (0xFFFFFFFF for
// a 32-bit process): a #GP supplies no CR2 (jit_require_align16; the
// minmax memory oracle). In every case the instruction commits nothing:
// DEST, YMM[255:128] and the GPRs are unchanged (Intel SDM Vol. 3A 6.5:
// faults are reported before the instruction's state changes).
//
// The expected values below come from that contract, not from either engine.

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "jit_crypto_test_harness.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
namespace w = ws3test;

namespace {

constexpr u32 kAccessViolation = 0xC0000005u;
constexpr u8 kRax = static_cast<u8>(RegIndex::RAX);

struct Form {
    const char* name;
    std::vector<u8> code32;  // [edx] / [rdx] operand, no REX
    std::vector<u8> code64;  // empty: same as code32
    bool legacy_aligned;     // a legacy m128 form: misalignment is #GP
};

const std::vector<Form>& forms() {
    static const std::vector<Form> f{
        {"aesenc xmm0,[rdx]", {0x66, 0x0F, 0x38, 0xDC, 0x02}, {}, true},
        {"aesdeclast xmm0,[rdx]", {0x66, 0x0F, 0x38, 0xDF, 0x02}, {}, true},
        {"aesimc xmm0,[rdx]", {0x66, 0x0F, 0x38, 0xDB, 0x02}, {}, true},
        {"aeskeygenassist xmm0,[rdx],1",
         {0x66, 0x0F, 0x3A, 0xDF, 0x02, 0x01}, {}, true},
        {"pclmulqdq xmm0,[rdx],0x11",
         {0x66, 0x0F, 0x3A, 0x44, 0x02, 0x11}, {}, true},
        {"sha256rnds2 xmm1,[rdx]", {0x0F, 0x38, 0xCB, 0x0A}, {}, true},
        {"sha256msg1 xmm0,[rdx]", {0x0F, 0x38, 0xCC, 0x02}, {}, true},
        {"sha1msg2 xmm0,[rdx]", {0x0F, 0x38, 0xCA, 0x02}, {}, true},
        {"sha1rnds4 xmm0,[rdx],1", {0x0F, 0x3A, 0xCC, 0x02, 0x01}, {}, true},
        {"vaesenc xmm0,xmm1,[rdx]", {0xC4, 0xE2, 0x71, 0xDC, 0x02}, {}, false},
        {"vaesdeclast xmm0,xmm1,[rdx]",
         {0xC4, 0xE2, 0x71, 0xDF, 0x02}, {}, false},
        {"vpclmulqdq xmm0,xmm1,[rdx],0x10",
         {0xC4, 0xE3, 0x71, 0x44, 0x02, 0x10}, {}, false},
        {"vaeskeygenassist xmm0,[rdx],1",
         {0xC4, 0xE3, 0x79, 0xDF, 0x02, 0x01}, {}, false},
        // Non-crypto VEX.128 merging forms through the AVX/SSE bridge, both
        // shapes: on the lane-split allowlist (VPMULLD, VPMAXSD, VPADDD) and
        // off it (VPMADDUBSW, VPSHUFB, VPUNPCKHBW). The bridge must not commit
        // DEST := SRC1 or the YMM[255:128] zero before the m128 read.
        {"vpmaddubsw xmm0,xmm1,[rdx]", {0xC4, 0xE2, 0x71, 0x04, 0x02}, {}, false},
        {"vpmulld xmm0,xmm1,[rdx]", {0xC4, 0xE2, 0x71, 0x40, 0x02}, {}, false},
        {"vpmaxsd xmm0,xmm1,[rdx]", {0xC4, 0xE2, 0x71, 0x3D, 0x02}, {}, false},
        {"vpaddd xmm0,xmm1,[rdx]", {0xC5, 0xF1, 0xFE, 0x02}, {}, false},
        {"vpshufb xmm0,xmm1,[rdx]", {0xC4, 0xE2, 0x71, 0x00, 0x02}, {}, false},
        {"vpunpckhbw xmm0,xmm1,[rdx]", {0xC5, 0xF1, 0x68, 0x02}, {}, false},
        {"pmaxsd xmm0,[rdx]", {0x66, 0x0F, 0x38, 0x3D, 0x02}, {}, true},
        // SSSE3 m128 forms; PSHUFB is a jit_sse_op helper form (op 255).
        {"pshufb xmm0,[rdx]", {0x66, 0x0F, 0x38, 0x00, 0x02}, {}, true},
        {"pmaddubsw xmm0,[rdx]", {0x66, 0x0F, 0x38, 0x04, 0x02}, {}, true},
        // SSE/SSE2 legacy m128 forms, read in the interpreter by sse_int
        // (read_ps), sse_data (load_xmm_aligned), sse_arith, sse_cmp,
        // sse_conv and sse_shuffle.
        {"paddd xmm0,[rdx]", {0x66, 0x0F, 0xFE, 0x02}, {}, true},
        {"pxor xmm0,[rdx]", {0x66, 0x0F, 0xEF, 0x02}, {}, true},
        {"movdqa xmm0,[rdx]", {0x66, 0x0F, 0x6F, 0x02}, {}, true},
        {"movaps xmm0,[rdx]", {0x0F, 0x28, 0x02}, {}, true},
        {"xorps xmm0,[rdx]", {0x0F, 0x57, 0x02}, {}, true},
        {"addps xmm0,[rdx]", {0x0F, 0x58, 0x02}, {}, true},
        {"cmpps xmm0,[rdx],1", {0x0F, 0xC2, 0x02, 0x01}, {}, true},
        {"cvtdq2ps xmm0,[rdx]", {0x0F, 0x5B, 0x02}, {}, true},
        {"pshufd xmm0,[rdx],0x1b", {0x66, 0x0F, 0x70, 0x02, 0x1B}, {}, true},
        {"unpcklps xmm0,[rdx]", {0x0F, 0x14, 0x02}, {}, true},
        // Unaligned and narrower SSE/FMA reads: no alignment rule.
        {"movdqu xmm0,[rdx]", {0xF3, 0x0F, 0x6F, 0x02}, {}, false},
        {"movsd xmm0,qword [rdx]", {0xF2, 0x0F, 0x10, 0x02}, {}, false},
        {"addss xmm0,dword [rdx]", {0xF3, 0x0F, 0x58, 0x02}, {}, false},
        {"cvtps2pd xmm0,qword [rdx]", {0x0F, 0x5A, 0x02}, {}, false},
        {"vfmadd231ps xmm0,xmm1,[rdx]", {0xC4, 0xE2, 0x71, 0xB8, 0x02}, {}, false},
        // x87 and MMX memory reads (interpreter fpu.cpp / mmx.cpp, the JIT's
        // jit_fpu_op helper): nothing pushed, TOP and tags unchanged.
        {"fld qword [rdx]", {0xDD, 0x02}, {}, false},
        {"fld dword [rdx]", {0xD9, 0x02}, {}, false},
        {"fld tbyte [rdx]", {0xDB, 0x2A}, {}, false},
        {"fild qword [rdx]", {0xDF, 0x2A}, {}, false},
        {"fild dword [rdx]", {0xDB, 0x02}, {}, false},
        {"fadd dword [rdx]", {0xD8, 0x02}, {}, false},
        {"fdiv qword [rdx]", {0xDC, 0x32}, {}, false},
        {"paddd mm0,qword [rdx]", {0x0F, 0xFE, 0x02}, {}, false},
        // CRC32 (SSE4.2): GPR destination, no alignment rule.
        {"crc32 eax,byte [rdx]", {0xF2, 0x0F, 0x38, 0xF0, 0x02}, {}, false},
        {"crc32 eax,word [rdx]",
         {0x66, 0xF2, 0x0F, 0x38, 0xF1, 0x02}, {}, false},
        {"crc32 eax,dword [rdx]", {0xF2, 0x0F, 0x38, 0xF1, 0x02}, {}, false},
        {"crc32 rax,qword [rdx]", {}, {0xF2, 0x48, 0x0F, 0x38, 0xF1, 0x02},
         false},
    };
    return f;
}

const std::vector<u8>& code_for(const Form& f, bool bits64) {
    return bits64 && !f.code64.empty() ? f.code64 : f.code32;
}

void set_ea(CpuState& cpu, bool bits64, u64 ea) {
    if (bits64) cpu.gpr.set64(w::kRdx, ea);
    else cpu.gpr.set32(w::kRdx, static_cast<u32>(ea));
}

void expect_state_untouched(const CpuState& got, const CpuState& init) {
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE(::testing::Message() << "xmm" << i);
        EXPECT_EQ(got.sse.xmm[i].u64_[0], init.sse.xmm[i].u64_[0]);
        EXPECT_EQ(got.sse.xmm[i].u64_[1], init.sse.xmm[i].u64_[1]);
        EXPECT_EQ(got.avx.ymm_high[i].u64_[0], init.avx.ymm_high[i].u64_[0]);
        EXPECT_EQ(got.avx.ymm_high[i].u64_[1], init.avx.ymm_high[i].u64_[1]);
    }
    EXPECT_EQ(got.gpr.get64(kRax), init.gpr.get64(kRax));
    EXPECT_EQ(got.flags.rflags, w::kFlags);
    // x87/MMX forms: no push, no TOP or tag change, no register written.
    EXPECT_EQ(got.fpu.status_word, init.fpu.status_word);
    EXPECT_EQ(got.fpu.tag_word, init.fpu.tag_word);
    for (int i = 0; i < 8; ++i) {
        SCOPED_TRACE(::testing::Message() << "st" << i);
        EXPECT_EQ(got.fpu.st[i].mantissa, init.fpu.st[i].mantissa);
        EXPECT_EQ(got.fpu.st[i].exponent_sign, init.fpu.st[i].exponent_sign);
    }
}

/// `mov eax, 0x12345678`: appended after the faulting instruction so the JIT
/// block has a successor that must NOT run once the fault is raised.
const std::vector<u8> kTrailingMovEax{0xB8, 0x78, 0x56, 0x34, 0x12};

/// Run `code` through both engines from `init` and require the read fault
/// [0] = 0, [1] = `fault_va` (truncated to 32 bits in a 32-bit block, as
/// the 32-bit dispatcher writes it), with no state committed by either.
void expect_read_fault(w::CryptoRig& rig, const std::vector<u8>& code,
                       const CpuState& init, u64 fault_va) {
    const bool b64 = rig.bits64();
    const u64 expected = b64 ? fault_va : (fault_va & 0xFFFFFFFFull);

    CpuState interpreted = init;
    EXPECT_TRUE(rig.mem().write_bytes(w::kCode, code.data(), code.size()));
    Interpreter interpreter(interpreted, rig.mem());
    ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
    const ExecContext& ctx = interpreter.context();
    {
        SCOPED_TRACE("interpreter");
        EXPECT_EQ(cpu_exception_to_status(ctx.exception), kAccessViolation);
        EXPECT_EQ(ctx.exception_error, 0u);
        const u64 addr = b64 ? ctx.exception_addr
                             : (ctx.exception_addr & 0xFFFFFFFFull);
        EXPECT_EQ(addr, expected) << std::hex << "got 0x" << addr;
        expect_state_untouched(interpreted, init);
    }

    // Alone, and as the first instruction of a two-instruction block: the
    // raise must leave the block at the faulting instruction, so the
    // trailing MOV EAX never runs and RIP names the faulting instruction.
    for (bool trailing : {false, true}) {
        SCOPED_TRACE(trailing ? "jit, mov eax follows" : "jit");
        std::vector<u8> block = code;
        if (trailing) {
            block.insert(block.end(), kTrailingMovEax.begin(),
                         kTrailingMovEax.end());
        }
        const w::JitRun r = rig.jit(block, init);
        ASSERT_TRUE(r.compiled);
        ASSERT_TRUE(r.cpu.pending_raise.active)
            << "jit result " << static_cast<int>(r.result);
        EXPECT_EQ(r.cpu.pending_raise.code, kAccessViolation);
        ASSERT_EQ(r.cpu.pending_raise.num_args, 2u);
        EXPECT_EQ(r.cpu.pending_raise.args[0], 0u);
        EXPECT_EQ(r.cpu.pending_raise.args[1], expected)
            << std::hex << "got 0x" << r.cpu.pending_raise.args[1];
        EXPECT_EQ(r.cpu.rip, w::kCode) << std::hex << "got 0x" << r.cpu.rip;
        expect_state_untouched(r.cpu, init);
    }
}

template <typename Fn>
void for_each_cfg(Fn&& fn) {
    for (w::MemCfg cfg : w::kAllCfgs) {
        w::CryptoRig rig;
        if (!rig.init(cfg)) continue;  // linear reservation unavailable
        SCOPED_TRACE(w::cfg_name(cfg));
        fn(rig);
    }
}

}  // namespace

TEST(SimdReadFaultContract, UnmappedSourceIsReadAccessViolationAtEa) {
    for_each_cfg([](w::CryptoRig& rig) {
        for (const Form& f : forms()) {
            if (code_for(f, rig.bits64()).empty()) continue;
            SCOPED_TRACE(f.name);
            CpuState init = rig.base();
            set_ea(init, rig.bits64(), w::kUnmapped);
            expect_read_fault(rig, code_for(f, rig.bits64()), init,
                              w::kUnmapped);
        }
    });
}

TEST(SimdReadFaultContract, PageStraddleReportsFirstInaccessibleByte) {
    // CryptoRig maps [kCode, kCode + 0x10000). An unaligned operand (VEX,
    // CRC32) starting 1 byte before the end reads that byte and faults at the
    // end of the mapping; an aligned legacy m128 cannot straddle, so it
    // starts at the end itself.
    constexpr u64 kEnd = w::kCode + 0x10000;
    for_each_cfg([&](w::CryptoRig& rig) {
        for (const Form& f : forms()) {
            const std::vector<u8>& code = code_for(f, rig.bits64());
            if (code.empty()) continue;
            if (std::string(f.name).find("byte") != std::string::npos) continue;
            SCOPED_TRACE(f.name);
            CpuState init = rig.base();
            set_ea(init, rig.bits64(), f.legacy_aligned ? kEnd : kEnd - 1);
            expect_read_fault(rig, code, init, kEnd);
        }
    });
}

TEST(SimdReadFaultContract, VexStraddleEightBytesLeavesDestinationAndUpper) {
    constexpr u64 kEnd = w::kCode + 0x10000;
    for_each_cfg([&](w::CryptoRig& rig) {
        for (const Form& f : forms()) {
            if (f.legacy_aligned || f.code32.empty() || (f.code32[0] != 0xC4 && f.code32[0] != 0xC5)) continue;
            SCOPED_TRACE(f.name);
            CpuState init = rig.base();
            set_ea(init, rig.bits64(), kEnd - 8);
            expect_read_fault(rig, f.code32, init, kEnd);
        }
    });
}

TEST(SimdReadFaultContract, MisalignedLegacyM128IsGpWithNoFaultAddress) {
    for_each_cfg([](w::CryptoRig& rig) {
        for (const Form& f : forms()) {
            if (!f.legacy_aligned) continue;
            SCOPED_TRACE(f.name);
            CpuState init = rig.base();
            set_ea(init, rig.bits64(), w::kData + 8);
            expect_read_fault(rig, f.code32, init,
                              std::numeric_limits<u64>::max());
        }
    });
}

TEST(SimdReadFaultContract, MisalignedGpDoesNotInheritStaleWriteDirection) {
    // The interpreter context outlives one instruction; a #GP must not report
    // the direction of an earlier write fault.
    w::CryptoRig rig;
    ASSERT_TRUE(rig.init(w::MemCfg::Region64));
    CpuState cpu = rig.base();
    // movdqu [0x20000000], xmm0 (write AV), then aesenc xmm0,[rdx+8].
    const std::vector<u8> code{0xF3, 0x0F, 0x7F, 0x04, 0x25, 0x00, 0x00, 0x00,
                               0x20, 0x66, 0x0F, 0x38, 0xDC, 0x42, 0x08};
    ASSERT_TRUE(rig.mem().write_bytes(w::kCode, code.data(), code.size()));
    Interpreter interpreter(cpu, rig.mem());
    ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
    ASSERT_EQ(interpreter.context().exception_error, 1u);
    cpu.rip = w::kCode + 9;
    ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
    EXPECT_EQ(interpreter.context().exception_error, 0u);
    EXPECT_EQ(interpreter.context().exception_addr,
              std::numeric_limits<u64>::max());
}

TEST(SimdReadFaultContract, MisalignedVexM128Executes) {
    // VEX forms carry no alignment requirement: same result in both engines.
    for_each_cfg([](w::CryptoRig& rig) {
        for (const Form& f : forms()) {
            if (f.legacy_aligned || f.code32.empty() || (f.code32[0] != 0xC4 && f.code32[0] != 0xC5)) continue;
            SCOPED_TRACE(f.name);
            CpuState init = rig.base();
            set_ea(init, rig.bits64(), w::kData + 8);
            w::expect_parity(rig, f.code32, init);
        }
    });
}

TEST(SimdReadFaultContract, VexBridgeMergingFormFaultLeavesDestination) {
    // DEST == SRC1 (no merge copy) still zeroes YMM[255:128]; a faulting read
    // must leave that upper half too. vpaddd/vpmaddubsw/vpshufb xmm1,xmm1,[rdx].
    const std::vector<std::vector<u8>> codes{
        {0xC5, 0xF1, 0xFE, 0x0A},
        {0xC4, 0xE2, 0x71, 0x04, 0x0A},
        {0xC4, 0xE2, 0x71, 0x00, 0x0A},
    };
    for_each_cfg([&](w::CryptoRig& rig) {
        for (const auto& code : codes) {
            for (u64 ea : {u64{w::kUnmapped}, u64{w::kCode + 0x10000 - 8}}) {
                SCOPED_TRACE(::testing::Message()
                             << std::hex << "op=" << int(code[code.size() - 2])
                             << " ea=" << ea);
                CpuState init = rig.base();
                set_ea(init, rig.bits64(), ea);
                expect_read_fault(rig, code, init,
                                  ea == w::kUnmapped ? ea : w::kCode + 0x10000);
            }
        }
    });
}

TEST(SimdReadFaultContract, InterpreterM256HalvesFaultAtFirstInaccessibleByte) {
    // AVX2 m256 sources the interpreter reads as two m128 halves (VPERM2I128,
    // the variable shifts) or as dword/qword pieces (VPERMQ, VPERMD). Each
    // half-read used to be unchecked (the instruction ran on zeros) or
    // reported a later piece; now the read faults at the first inaccessible
    // byte and nothing is written. Interpreter only: the JIT delegates these.
    const std::vector<std::pair<const char*, std::vector<u8>>> codes{
        {"vperm2i128 ymm0,ymm1,[rdx],0x20", {0xC4, 0xE3, 0x75, 0x46, 0x02, 0x20}},
        {"vpsrlvd ymm0,ymm1,[rdx]", {0xC4, 0xE2, 0x75, 0x45, 0x02}},
        {"vpsllvq ymm0,ymm1,[rdx]", {0xC4, 0xE2, 0xF5, 0x47, 0x02}},
        {"vpermq ymm0,[rdx],0x1b", {0xC4, 0xE3, 0xFD, 0x00, 0x02, 0x1B}},
        {"vpermd ymm0,ymm1,[rdx]", {0xC4, 0xE2, 0x75, 0x36, 0x02}},
    };
    constexpr u64 kEnd = w::kCode + 0x10000;
    for_each_cfg([&](w::CryptoRig& rig) {
        for (const auto& [name, code] : codes) {
            // Unmapped; the high half straddling the end; the low half
            // straddling it.
            for (u64 ea : {u64{w::kUnmapped}, kEnd - 24, kEnd - 1}) {
                SCOPED_TRACE(::testing::Message()
                             << name << std::hex << " ea=" << ea);
                const u64 fault_va = ea == w::kUnmapped ? ea : kEnd;
                CpuState init = rig.base();
                set_ea(init, rig.bits64(), ea);
                CpuState cpu = init;
                ASSERT_TRUE(rig.mem().write_bytes(w::kCode, code.data(),
                                                  code.size()));
                Interpreter interpreter(cpu, rig.mem());
                ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
                const ExecContext& ctx = interpreter.context();
                EXPECT_EQ(cpu_exception_to_status(ctx.exception),
                          kAccessViolation);
                EXPECT_EQ(ctx.exception_error, 0u);
                const u64 addr = rig.bits64()
                    ? ctx.exception_addr : (ctx.exception_addr & 0xFFFFFFFFull);
                EXPECT_EQ(addr, fault_va) << std::hex << "got 0x" << addr;
                expect_state_untouched(cpu, init);
            }
        }
    });
}
