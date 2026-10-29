// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// JIT vs interpreter parity for SSE floating-point ADD/SUB/MUL/DIV.
//
// The interpreter is the semantic reference (sse_float_semantics.hpp, from
// Intel SDM Vol. 1 §4.8.3.4 and §11.5.2.6). The JIT lowers these ops natively
// and falls back to jit_sse_op only when the NEON RESULT is a NaN -- the whole
// set of places where ARM's arithmetic disagrees with x86:
//
//   * both operands NaN: x86 takes the DESTINATION's NaN, ARM prefers the
//     signalling one, so a QNaN dst against an SNaN src diverges;
//   * an invalid operation on non-NaN operands (Inf-Inf, 0*Inf, Inf/Inf, 0/0):
//     x86 produces the Indefinite QNaN whose SIGN IS SET, ARM's default NaN
//     has it clear;
//   * SNaN quieting, which both do, but only after the ordering rule above.
//
// Everything else -- ±Inf with a finite partner, ±0 signs, denormals, overflow
// to Inf, and the exactness/divide-by-zero MXCSR sticky bits -- must come out
// bit-identical from the native path. These tests compare the full 128 bits of
// every XMM register AND MXCSR, because a lowering that gets the value right
// and the sticky bits wrong is still wrong (a masked exception is only ever
// observed through MXCSR).
//
// The sweep is a full cross product of special values, not a handful of cases:
// which arm of the gate an instruction takes depends on both operands, so a
// spot check cannot see a gate that is merely almost right.

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <limits>
#include "core/cpu/seh/seh32.hpp"

#include "core/cpu/decode/decoder.hpp"
#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "jit_dispatch_execute.hpp"
#include "core/cpu/jit/stencils.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;

namespace {

constexpr u64 kCodeAddr = 0x1000;
constexpr u32 kDataAddr = 0x2000;
constexpr u32 kMxcsrDefault = 0x1F80;

// ---------------------------------------------------------------------------
// Special-value corpus
// ---------------------------------------------------------------------------

// f32: +0, -0, denormal (both signs), 1.0, -2.0, pi, max finite (both signs,
// so ADD/MUL overflow to ±Inf), ±Inf, QNaN, SNaN, and a negative SNaN so the
// destination-first rule can be told apart from "prefer the signalling one".
constexpr u32 kF32[] = {
    0x00000000u,  // +0
    0x80000000u,  // -0
    0x00000001u,  // smallest positive denormal
    0x807FFFFFu,  // largest negative denormal
    0x3F800000u,  // 1.0
    0xC0000000u,  // -2.0
    0x40490FDBu,  // pi
    0x7F7FFFFFu,  // FLT_MAX
    0xFF7FFFFFu,  // -FLT_MAX
    0x7F800000u,  // +Inf
    0xFF800000u,  // -Inf
    0x7FC00001u,  // +QNaN, payload 1
    0xFFC00002u,  // -QNaN, payload 2
    0x7F800001u,  // +SNaN
    0xFFBFFFFFu,  // -SNaN
};
constexpr usize kF32Count = sizeof(kF32) / sizeof(kF32[0]);

constexpr u64 kF64[] = {
    0x0000000000000000ull,  // +0
    0x8000000000000000ull,  // -0
    0x0000000000000001ull,  // smallest positive denormal
    0x800FFFFFFFFFFFFFull,  // largest negative denormal
    0x3FF0000000000000ull,  // 1.0
    0xC000000000000000ull,  // -2.0
    0x400921FB54442D18ull,  // pi
    0x7FEFFFFFFFFFFFFFull,  // DBL_MAX
    0xFFEFFFFFFFFFFFFFull,  // -DBL_MAX
    0x7FF0000000000000ull,  // +Inf
    0xFFF0000000000000ull,  // -Inf
    0x7FF8000000000001ull,  // +QNaN
    0xFFF8000000000002ull,  // -QNaN
    0x7FF0000000000001ull,  // +SNaN
    0xFFF7FFFFFFFFFFFFull,  // -SNaN
};
constexpr usize kF64Count = sizeof(kF64) / sizeof(kF64[0]);

struct Form {
    const char* name;
    std::vector<u8> code;  // xmm0 <op> xmm1, or xmm0 <op> [kDataAddr]
    bool is_pd;            // lane width: false = f32 lanes, true = f64 lanes
    bool scalar;           // only lane 0 participates
    bool memory;           // second operand comes from kDataAddr
};

std::vector<u8> with_prefix(u8 prefix, u8 op, bool memory) {
    std::vector<u8> out;
    if (prefix != 0) out.push_back(prefix);
    out.push_back(0x0F);
    out.push_back(op);
    if (memory) {
        // mod=00 reg=xmm0 rm=101 -> disp32 absolute (32-bit mode).
        out.push_back(0x05);
        out.push_back(static_cast<u8>(kDataAddr & 0xFF));
        out.push_back(static_cast<u8>((kDataAddr >> 8) & 0xFF));
        out.push_back(static_cast<u8>((kDataAddr >> 16) & 0xFF));
        out.push_back(static_cast<u8>((kDataAddr >> 24) & 0xFF));
    } else {
        out.push_back(0xC1);  // mod=11 reg=xmm0 rm=xmm1
    }
    return out;
}

std::vector<Form> arith_forms(bool memory) {
    struct Spec { const char* name; u8 prefix; u8 op; bool pd; bool scalar; };
    static const Spec specs[] = {
        {"addps", 0x00, 0x58, false, false},
        {"subps", 0x00, 0x5C, false, false},
        {"mulps", 0x00, 0x59, false, false},
        {"divps", 0x00, 0x5E, false, false},
        {"addpd", 0x66, 0x58, true,  false},
        {"subpd", 0x66, 0x5C, true,  false},
        {"mulpd", 0x66, 0x59, true,  false},
        {"divpd", 0x66, 0x5E, true,  false},
        {"addss", 0xF3, 0x58, false, true},
        {"subss", 0xF3, 0x5C, false, true},
        {"mulss", 0xF3, 0x59, false, true},
        {"divss", 0xF3, 0x5E, false, true},
        {"addsd", 0xF2, 0x58, true,  true},
        {"subsd", 0xF2, 0x5C, true,  true},
        {"mulsd", 0xF2, 0x59, true,  true},
        {"divsd", 0xF2, 0x5E, true,  true},
    };
    std::vector<Form> out;
    for (const Spec& s : specs) {
        out.push_back(Form{s.name, with_prefix(s.prefix, s.op, memory), s.pd,
                           s.scalar, memory});
    }
    return out;
}

std::string hex128(const XmmReg& v) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(16) << v.u64_[1] << ':' << std::setw(16) << v.u64_[0];
    return ss.str();
}

class JitSseFpParityTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        ASSERT_TRUE(m_memory
                        ->map_region(0x1000, 0x10000,
                                     MemoryProtection::ReadWriteExecute)
                        .is_ok());
    }

    BasicBlock build_block(const std::vector<u8>& code, DecodeMode mode=DecodeMode::Mode32) {
        BasicBlock block;
        block.start_addr = kCodeAddr;
        MemoryByteReader reader(code.data(), code.size());
        while (reader.remaining() > 0) {
            DecodedInstruction inst;
            if (decode_instruction(reader, mode, inst) !=
                DecodeError::Success) {
                break;
            }
            block.instructions.push_back(inst);
        }
        block.byte_length = static_cast<u32>(reader.position());
        block.instruction_count =
            static_cast<u32>(block.instructions.size());
        block.is_64bit=mode==DecodeMode::Mode64;
        return block;
    }

    void init_cpu(CpuState& cpu, const XmmReg& x0, const XmmReg& x1,
                  u32 mxcsr) {
        cpu.init_protected_mode();
        cpu.gpr.set32(static_cast<u8>(RegIndex::RSP), 0x10000);
        cpu.rip = kCodeAddr;
        cpu.sse.mxcsr = mxcsr;
        cpu.sse.xmm[0] = x0;
        cpu.sse.xmm[1] = x1;
    }

    /// Run one instruction through interpreter and JIT from identical state
    /// and require bit-identical XMM0/XMM1 and MXCSR.
    void compare(const Form& form, const XmmReg& x0, const XmmReg& x1,
                 u32 mxcsr, const char* tag) {
        ASSERT_TRUE(
            m_memory->write_bytes(kCodeAddr, form.code.data(),
                                  form.code.size()));
        if (form.memory) {
            ASSERT_TRUE(m_memory->write_bytes(
                kDataAddr, reinterpret_cast<const u8*>(&x1), 16));
        }

        BasicBlock block = build_block(form.code);
        ASSERT_EQ(block.instruction_count, 1u)
            << form.name << ": decode produced " << block.instruction_count
            << " instructions";

        CpuState icpu;
        init_cpu(icpu, x0, x1, mxcsr);
        {
            Interpreter interp(icpu, *m_memory);
            ASSERT_EQ(interp.run(1), ExecResult::Continue)
                << form.name << " interpreter refused to execute";
        }

        CpuState jcpu;
        init_cpu(jcpu, x0, x1, mxcsr);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr) << form.name << ": JIT declined the block";
        ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, jcpu), JitExecResult::Continue)
            << form.name << " JIT refused to execute";

        // Full 128 bits: NaN payload and sign are exactly what diverges, and a
        // float comparison would call two different NaNs equal (or two equal
        // zeros of different sign so as well).
        EXPECT_EQ(icpu.sse.xmm[0].u64_[0], jcpu.sse.xmm[0].u64_[0])
            << form.name << ' ' << tag << " xmm0 low: interp="
            << hex128(icpu.sse.xmm[0]) << " jit=" << hex128(jcpu.sse.xmm[0]);
        EXPECT_EQ(icpu.sse.xmm[0].u64_[1], jcpu.sse.xmm[0].u64_[1])
            << form.name << ' ' << tag << " xmm0 high: interp="
            << hex128(icpu.sse.xmm[0]) << " jit=" << hex128(jcpu.sse.xmm[0]);
        EXPECT_EQ(icpu.sse.xmm[1].u64_[0], jcpu.sse.xmm[1].u64_[0])
            << form.name << ' ' << tag << " xmm1 clobbered";
        EXPECT_EQ(icpu.sse.xmm[1].u64_[1], jcpu.sse.xmm[1].u64_[1])
            << form.name << ' ' << tag << " xmm1 clobbered";
        EXPECT_EQ(icpu.sse.mxcsr, jcpu.sse.mxcsr)
            << form.name << ' ' << tag << " MXCSR sticky bits: interp=0x"
            << std::hex << icpu.sse.mxcsr << " jit=0x" << jcpu.sse.mxcsr;
    }

    /// Fill an XmmReg's lanes starting at corpus index `start`, so a packed
    /// instruction sees four (or two) DIFFERENT special values at once.
    static XmmReg lanes(bool is_pd, usize start) {
        XmmReg v{};
        if (is_pd) {
            for (usize lane = 0; lane < 2; ++lane) {
                v.u64_[lane] = kF64[(start + lane) % kF64Count];
            }
        } else {
            for (usize lane = 0; lane < 4; ++lane) {
                v.u32_[lane] = kF32[(start + lane) % kF32Count];
            }
        }
        return v;
    }

    /// Scalar forms: lane 0 is the value under test, the upper lanes carry
    /// unrelated bits so a lowering that widens the op (or clobbers the upper
    /// lanes on the way to the helper) is caught.
    static XmmReg scalar_lane(bool is_pd, usize index) {
        XmmReg v{};
        v.u64_[0] = is_pd ? kF64[index] : 0ull;
        if (!is_pd) v.u32_[0] = kF32[index];
        v.u32_[1] = is_pd ? 0u : 0xDEADBEEFu;
        v.u64_[1] = 0x0123456789ABCDEFull;
        if (is_pd) v.u64_[0] = kF64[index];
        return v;
    }

    std::unique_ptr<GuestMemory> m_memory;
};

// ---------------------------------------------------------------------------

TEST_F(JitSseFpParityTest, RegisterFormsMatchInterpreterAcrossSpecialValues) {
    for (const Form& form : arith_forms(/*memory=*/false)) {
        const usize count = form.is_pd ? kF64Count : kF32Count;
        for (usize i = 0; i < count; ++i) {
            for (usize j = 0; j < count; ++j) {
                const XmmReg x0 = form.scalar ? scalar_lane(form.is_pd, i)
                                              : lanes(form.is_pd, i);
                const XmmReg x1 = form.scalar ? scalar_lane(form.is_pd, j)
                                              : lanes(form.is_pd, j);
                std::string tag = "dst[" + std::to_string(i) + "] src[" +
                                  std::to_string(j) + "]";
                compare(form, x0, x1, kMxcsrDefault, tag.c_str());
                if (::testing::Test::HasFailure()) return;
            }
        }
    }
}

TEST_F(JitSseFpParityTest, MemoryFormsMatchInterpreterAcrossSpecialValues) {
    for (const Form& form : arith_forms(/*memory=*/true)) {
        const usize count = form.is_pd ? kF64Count : kF32Count;
        for (usize i = 0; i < count; ++i) {
            for (usize j = 0; j < count; ++j) {
                const XmmReg x0 = form.scalar ? scalar_lane(form.is_pd, i)
                                              : lanes(form.is_pd, i);
                const XmmReg x1 = form.scalar ? scalar_lane(form.is_pd, j)
                                              : lanes(form.is_pd, j);
                std::string tag = "dst[" + std::to_string(i) + "] mem[" +
                                  std::to_string(j) + "]";
                compare(form, x0, x1, kMxcsrDefault, tag.c_str());
                if (::testing::Test::HasFailure()) return;
            }
        }
    }
}

// Named cases, so a failure says WHICH contract broke rather than an index.
TEST_F(JitSseFpParityTest, NamedNanAndInfinityContracts) {
    const auto ps = [](u32 a, u32 b, u32 c, u32 d) {
        XmmReg v{};
        v.u32_[0] = a; v.u32_[1] = b; v.u32_[2] = c; v.u32_[3] = d;
        return v;
    };
    struct Case {
        const char* name;
        const char* op;   // form name to look up
        XmmReg dst;
        XmmReg src;
    };
    const u32 qnan = 0x7FC00001u, snan = 0x7F800001u;
    const u32 pinf = 0x7F800000u, ninf = 0xFF800000u;
    const u32 pzero = 0x00000000u, nzero = 0x80000000u;
    const u32 one = 0x3F800000u, fmax = 0x7F7FFFFFu, den = 0x00000001u;
    const Case cases[] = {
        // Destination QNaN vs source SNaN: x86 keeps the destination, ARM
        // would take the signalling operand. This is the case a "quiet the
        // SNaN first" implementation gets wrong and nothing else notices.
        {"qnan dst beats snan src", "addps",
         ps(qnan, qnan, qnan, qnan), ps(snan, snan, snan, snan)},
        {"snan dst beats qnan src", "mulps",
         ps(snan, snan, snan, snan), ps(qnan, qnan, qnan, qnan)},
        // Invalid on non-NaN operands -> Indefinite QNaN, sign SET.
        {"inf minus inf", "subps",
         ps(pinf, pinf, pinf, pinf), ps(pinf, pinf, pinf, pinf)},
        {"inf plus neg inf", "addps",
         ps(pinf, ninf, pinf, ninf), ps(ninf, pinf, ninf, pinf)},
        {"zero times inf", "mulps",
         ps(pzero, nzero, pzero, nzero), ps(pinf, pinf, ninf, ninf)},
        {"inf over inf", "divps",
         ps(pinf, ninf, pinf, ninf), ps(pinf, pinf, ninf, ninf)},
        {"zero over zero", "divps",
         ps(pzero, nzero, pzero, nzero), ps(pzero, pzero, nzero, nzero)},
        // Divide by zero: finite/±0 is ±Inf plus MXCSR.ZE.
        {"finite over zero", "divps",
         ps(one, one, one, one), ps(pzero, nzero, pzero, nzero)},
        // Inf with a finite partner is exact on both ISAs -- the native path
        // must take it, and must still get the sign right.
        {"inf plus finite", "addps",
         ps(pinf, ninf, pinf, ninf), ps(one, one, one, one)},
        {"inf times finite", "mulps",
         ps(pinf, ninf, pinf, ninf), ps(one, one, one, one)},
        // Overflow of finite operands to ±Inf, and the signed-zero rules.
        {"overflow to inf", "addps",
         ps(fmax, fmax, fmax, fmax), ps(fmax, fmax, fmax, fmax)},
        {"signed zero sum", "addps",
         ps(pzero, pzero, nzero, nzero), ps(pzero, nzero, pzero, nzero)},
        {"signed zero difference", "subps",
         ps(pzero, pzero, nzero, nzero), ps(pzero, nzero, pzero, nzero)},
        // Denormals: MXCSR default has FTZ and DAZ clear, so they survive.
        {"denormal sum", "addps",
         ps(den, den, den, den), ps(den, 0x80000001u, den, 0x80000001u)},
        {"denormal product underflows", "mulps",
         ps(den, den, den, den), ps(den, one, den, one)},
        {"inexact quotient", "divps",
         ps(one, one, one, one), ps(0x40400000u, 0x40400000u,
                                    0x40400000u, 0x40400000u)},  // 1/3
    };
    const auto forms = arith_forms(/*memory=*/false);
    for (const Case& c : cases) {
        for (const Form& form : forms) {
            if (std::strcmp(form.name, c.op) != 0) continue;
            compare(form, c.dst, c.src, kMxcsrDefault, c.name);
        }
    }
}

// The JIT installs MXCSR.RC on the host FPCR once per block entry rather than
// in front of each FP instruction. A non-default rounding mode must still
// reach the native lowering, and a loop must not drift out of it.
TEST_F(JitSseFpParityTest, NonDefaultRoundingModesMatchInterpreter) {
    const u32 rc_modes[] = {
        0x1F80u,  // nearest-even
        0x3F80u,  // down
        0x5F80u,  // up
        0x7F80u,  // toward zero
    };
    const auto forms = arith_forms(/*memory=*/false);
    for (u32 mxcsr : rc_modes) {
        for (const Form& form : forms) {
            const usize count = form.is_pd ? kF64Count : kF32Count;
            for (usize i = 0; i < count; ++i) {
                const usize j = (i + 5) % count;
                const XmmReg x0 = form.scalar ? scalar_lane(form.is_pd, i)
                                              : lanes(form.is_pd, i);
                const XmmReg x1 = form.scalar ? scalar_lane(form.is_pd, j)
                                              : lanes(form.is_pd, j);
                std::string tag = "mxcsr=0x" + std::to_string(mxcsr) +
                                  " dst[" + std::to_string(i) + "]";
                compare(form, x0, x1, mxcsr, tag.c_str());
                if (::testing::Test::HasFailure()) return;
            }
        }
    }
}

// Pre-set sticky bits must be preserved, never cleared, by a native lowering.
TEST_F(JitSseFpParityTest, PreExistingStickyBitsSurviveNativePath) {
    const u32 all_sticky = kMxcsrDefault | 0x3Fu;
    const auto forms = arith_forms(/*memory=*/false);
    for (const Form& form : forms) {
        const XmmReg x0 = form.scalar ? scalar_lane(form.is_pd, 4)
                                      : lanes(form.is_pd, 4);
        const XmmReg x1 = form.scalar ? scalar_lane(form.is_pd, 6)
                                      : lanes(form.is_pd, 6);
        compare(form, x0, x1, all_sticky, "sticky preset");
    }
}

TEST_F(JitSseFpParityTest, NativePackedMinmaxMatrix) {
    // Exact outputs and sticky flags measured on Intel silicon, both Windows
    // ABIs. The source probe's corpus is the existing special corpus minus
    // pi and +/-max-finite; indices below only map its input identifiers.
    constexpr usize corpus[]={0,1,2,3,4,5,9,10,11,12,13,14};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/oracle_data/sse_minmax_probe.expected";
    std::ifstream input(path);
    ASSERT_TRUE(input.good()) << path;
    std::string line;
    usize checked=0;
    while(std::getline(input,line)) {
        if(line.starts_with("constants ") || line.starts_with("fault ")) continue;
        std::istringstream row(line);
        std::string form,operation,control_token,a_token,b_token,status_token,output_token;
        ASSERT_TRUE(bool(row>>form>>operation>>control_token>>a_token>>b_token>>status_token>>output_token));
        SCOPED_TRACE(line);
        const bool pd=operation.ends_with("pd"),minimum=operation.starts_with("min");
        const bool vex=form!="legacy",wide=form=="vex256";
        const u32 control=static_cast<u32>(std::stoul(control_token.substr(8),nullptr,16));
        const usize a=std::stoul(a_token.substr(2)),b=std::stoul(b_token.substr(2));
        ASSERT_LT(a,std::size(corpus)); ASSERT_LT(b,std::size(corpus));
        const u32 status=static_cast<u32>(std::stoul(status_token.substr(6),nullptr,16));
        const std::string output=output_token.substr(4);
        std::vector<u8> code;
        // Encodings checked against sse_minmax_probe's compiler disassembly.
        if(vex) code={0xc5,static_cast<u8>((wide?0xfc:0xf8)|(pd?1:0))};
        else {if(pd) code.push_back(0x66);code.push_back(0x0f);}
        code.push_back(minimum?0x5d:0x5f);code.push_back(0xc1);
        const usize operand_rm=code.size()-1;
        // STMXCSR [rbx] publishes the lazy host sticky accumulator as on Windows.
        code.insert(code.end(),{0x0f,0xae,0x1b});
        // LDMXCSR [rbx] resets pending host flags too: every oracle case starts
        // with _mm_setcsr, independently of the previous test's host FPU state.
        code.insert(code.begin(),{0x0f,0xae,0x13});
        // Oracle a=+0 rows also exercise an adjacent VEX self-XOR proof.
        // VEX.128 proves upper zero even when the following operation is 256.
        const bool zero_producer=a==0;
        if(zero_producer) code.insert(code.begin()+3,{0xc5,0xf8,0x57,0xc0});
        const usize count=zero_producer?4:3;
        for(const bool memory:{false,true}) for(const bool mode64:{false,true}) {
            code[operand_rm+3+(zero_producer?4:0)]=memory?0x02:0xc1; // xmm0,[rdx] / xmm0,xmm1
            ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
            auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
            ASSERT_EQ(block.instruction_count,count);
            CpuState seed;
            if(mode64) seed.init_long_mode(); else seed.init_protected_mode();
            seed.rip=kCodeAddr;seed.gpr.set64(static_cast<u8>(RegIndex::RBX),kDataAddr);
            const u64 source_address=vex?0x3001:0x3000;
            seed.gpr.set64(static_cast<u8>(RegIndex::RDX),source_address);
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP),0x10000);
            seed.sse.mxcsr=control;
            for(usize half=0;half<2;++half) {
                auto& first=half?seed.avx.ymm_high[0]:seed.sse.xmm[0];
                auto& second=half?seed.avx.ymm_high[1]:seed.sse.xmm[1];
                if(pd) for(usize lane=0;lane<2;++lane) {
                    first.u64_[lane]=kF64[corpus[a]];second.u64_[lane]=kF64[corpus[b]];
                } else for(usize lane=0;lane<4;++lane) {
                    first.u32_[lane]=kF32[corpus[a]];second.u32_[lane]=kF32[corpus[b]];
                }
            }
            ASSERT_TRUE(m_memory->write_bytes(source_address,
                reinterpret_cast<const Byte*>(&seed.sse.xmm[1]),sizeof(XmmReg)));
            ASSERT_TRUE(m_memory->write_bytes(source_address+sizeof(XmmReg),
                reinterpret_cast<const Byte*>(&seed.avx.ymm_high[1]),sizeof(XmmReg)));
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto interpreter=seed;
            Interpreter reference(interpreter,*m_memory);
            ASSERT_EQ(reference.run(count),ExecResult::Continue);
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto actual=seed;
            JitCompiler jit(*m_memory);
            auto* compiled=jit.compile(block);
            ASSERT_NE(compiled,nullptr);
            ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled,actual),JitExecResult::Continue);
            ASSERT_FALSE(actual.pending_raise.active);
            EXPECT_EQ(interpreter.sse.mxcsr,status);
            EXPECT_EQ(actual.sse.mxcsr,status);
            for(usize word=0;word<(wide?4u:2u);++word) {
                const u64 expected=std::stoull(output.substr(word*16,16),nullptr,16);
                const auto& i=word<2?interpreter.sse.xmm[0]:interpreter.avx.ymm_high[0];
                const auto& j=word<2?actual.sse.xmm[0]:actual.avx.ymm_high[0];
                EXPECT_EQ(i.u64_[word%2],expected);EXPECT_EQ(j.u64_[word%2],expected);
            }
            for(usize word=0;word<2;++word) {
                const u64 upper=wide?std::stoull(output.substr((word+2)*16,16),nullptr,16)
                                    :vex?0:seed.avx.ymm_high[0].u64_[word];
                EXPECT_EQ(interpreter.avx.ymm_high[0].u64_[word],upper);
                EXPECT_EQ(actual.avx.ymm_high[0].u64_[word],upper);
                EXPECT_EQ(actual.sse.xmm[1].u64_[word],seed.sse.xmm[1].u64_[word]);
                EXPECT_EQ(actual.avx.ymm_high[1].u64_[word],seed.avx.ymm_high[1].u64_[word]);
            }
            ASSERT_FALSE(HasFailure());
            ++checked;
        }
    }
    EXPECT_EQ(checked,6912u*4);
}

TEST_F(JitSseFpParityTest, NativePackedMinmaxExceptions) {
    constexpr usize corpus[]={0,1,2,3,4,5,9,10,11,12,13,14};
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/oracle_data/sse_minmax_probe.expected";
    std::ifstream input(path);ASSERT_TRUE(input.good());
    std::string line;usize checked=0;
    while(std::getline(input,line)) {
        if(!line.starts_with("fault ")) continue;
        std::istringstream row(line);
        std::string tag,form,precision,control_text,a_text,b_text,mixed_text,
                    code_text,after_text,stored_text;
        ASSERT_TRUE(bool(row>>tag>>form>>precision>>control_text>>a_text>>b_text>>mixed_text>>code_text>>after_text>>stored_text));
        SCOPED_TRACE(line);
        const bool pd=precision=="pd",vex=form!="legacy",wide=form=="vex256";
        const u32 control=static_cast<u32>(std::stoul(control_text.substr(8),nullptr,16));
        const usize a=std::stoul(a_text.substr(2)),b=std::stoul(b_text.substr(2));
        const bool mixed=std::stoul(mixed_text.substr(6))!=0;
        const u32 exception=static_cast<u32>(std::stoul(code_text.substr(5),nullptr,16));
        const u32 after=static_cast<u32>(std::stoul(after_text.substr(6),nullptr,16));
        const bool stored=std::stoul(stored_text.substr(7))!=0;
        std::vector<u8> code;
        if(vex) code={0xc5,static_cast<u8>((wide?0xfc:0xf8)|(pd?1:0))};
        else {if(pd) code.push_back(0x66);code.push_back(0x0f);}
        code.insert(code.end(),{0x5d,0xc1,0x0f,0xae,0x1b});
        code.insert(code.begin(),{0x0f,0xae,0x13});
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
        for(const bool mode64:{false,true}) {
            auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
            CpuState seed;
            if(mode64) seed.init_long_mode();else seed.init_protected_mode();
            seed.rip=kCodeAddr;seed.gpr.set64(static_cast<u8>(RegIndex::RBX),kDataAddr);
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP),0x10000);
            seed.sse.mxcsr=control;
            for(usize word=0;word<4;++word) {
                const usize ai=mixed&&word==1?2:a,bi=mixed&&word==1?4:b;
                auto& first=word<2?seed.sse.xmm[0]:seed.avx.ymm_high[0];
                auto& second=word<2?seed.sse.xmm[1]:seed.avx.ymm_high[1];
                first.u64_[word%2]=pd?kF64[corpus[ai]]:u64(kF32[corpus[ai]])|(u64(kF32[corpus[ai]])<<32);
                second.u64_[word%2]=pd?kF64[corpus[bi]]:u64(kF32[corpus[bi]])|(u64(kF32[corpus[bi]])<<32);
            }
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto interpreter=seed;Interpreter reference(interpreter,*m_memory);
            EXPECT_EQ(reference.run(3),exception?ExecResult::Exception:ExecResult::Continue);
            EXPECT_EQ(interpreter.sse.mxcsr,after);
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto actual=seed;JitCompiler jit(*m_memory);
            auto* compiled=jit.compile(block);ASSERT_NE(compiled,nullptr);
            test::execute_like_dispatcher(jit, *compiled,actual);
            EXPECT_EQ(actual.sse.mxcsr,after);
            EXPECT_EQ(actual.pending_raise.active,exception!=0);
            if(exception) {
                EXPECT_EQ(actual.pending_raise.code,exception);
                EXPECT_EQ(actual.rip,kCodeAddr+3);
                EXPECT_EQ(interpreter.rip,kCodeAddr+3);
                for(usize word=0;word<2;++word) {
                    EXPECT_EQ(interpreter.sse.xmm[0].u64_[word],seed.sse.xmm[0].u64_[word]);
                    EXPECT_EQ(actual.sse.xmm[0].u64_[word],seed.sse.xmm[0].u64_[word]);
                    EXPECT_EQ(interpreter.avx.ymm_high[0].u64_[word],seed.avx.ymm_high[0].u64_[word]);
                    EXPECT_EQ(actual.avx.ymm_high[0].u64_[word],seed.avx.ymm_high[0].u64_[word]);
                }
            } else {
                const u64 lower=actual.sse.xmm[0].u64_[0]|actual.sse.xmm[0].u64_[1];
                const u64 upper=wide?(actual.avx.ymm_high[0].u64_[0]|actual.avx.ymm_high[0].u64_[1]):0;
                EXPECT_EQ((lower|upper)!=0,stored);
            }
            ASSERT_FALSE(HasFailure());++checked;
        }
    }
    EXPECT_EQ(checked,120u*2);
}


// MINSS/MAXSS/MINSD/MAXSD against tools/oracle_probe/sse_minmax_scalar_probe:
// lane 0 selected, the rest of the destination untouched, the sticky flags
// (IE for a QNaN too, DE for a denormal unless DAZ), in all four FTZ/DAZ
// modes, register and memory source, both bitnesses, interpreter and JIT.
// They used to compute lane 0 right but set no flag at all.
namespace {
// The probe's own grid, in its order (the .expected rows index into it).
constexpr u32 kScalarProbeF32[] = {
    0, 0x80000000, 1, 0x807fffff, 0x3f800000, 0xc0000000,
    0x7f800000, 0xff800000, 0x7fc00001, 0xffc00002, 0x7f800001, 0xffbfffff};
constexpr u64 kScalarProbeF64[] = {
    0, 0x8000000000000000ULL, 1, 0x800fffffffffffffULL,
    0x3ff0000000000000ULL, 0xc000000000000000ULL,
    0x7ff0000000000000ULL, 0xfff0000000000000ULL,
    0x7ff8000000000001ULL, 0xfff8000000000002ULL,
    0x7ff0000000000001ULL, 0xfff7ffffffffffffULL};
// As the probe's fill(): byte i = pattern + i, lane 0 = the grid value.
XmmReg scalar_probe_operand(bool sd, usize index, u8 pattern) {
    XmmReg v{};
    for (usize i = 0; i < 16; ++i) v.u8_[i] = static_cast<u8>(pattern + i);
    if (sd) v.u64_[0] = kScalarProbeF64[index];
    else v.u32_[0] = kScalarProbeF32[index];
    return v;
}
// The probe prints the 16 bytes most significant first.
XmmReg scalar_probe_bytes(const std::string& hex) {
    XmmReg v{};
    for (usize i = 0; i < 16; ++i) {
        v.u8_[15 - i] = static_cast<u8>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
    }
    return v;
}
std::vector<u8> scalar_minmax_code(bool sd, bool minimum, bool memory) {
    // LDMXCSR [rbx]; MIN/MAX xmm0, xmm1 | [rdx]; STMXCSR [rbx]. STMXCSR
    // publishes the lazily harvested host flags, as on Windows.
    return {0x0f, 0xae, 0x13, static_cast<u8>(sd ? 0xf2 : 0xf3), 0x0f,
            static_cast<u8>(minimum ? 0x5d : 0x5f),
            static_cast<u8>(memory ? 0x02 : 0xc1), 0x0f, 0xae, 0x1b};
}
}  // namespace

TEST_F(JitSseFpParityTest, NativeScalarMinmaxMatchesOracle) {
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/oracle_data/sse_minmax_scalar_probe.expected";
    std::ifstream input(path);
    ASSERT_TRUE(input.good()) << path;
    constexpr u64 kSource=0x3000;
    std::string line;
    usize checked=0;
    while(std::getline(input,line)) {
        if(line.starts_with("fault ")) continue;
        std::istringstream row(line);
        std::string op,control_token,a_token,b_token,mxcsr_token,out_token;
        ASSERT_TRUE(bool(row>>op>>control_token>>a_token>>b_token>>mxcsr_token>>out_token));
        SCOPED_TRACE(line);
        ASSERT_EQ(line.find("FAULTED"),std::string::npos);
        const bool sd=op.ends_with("sd"),minimum=op.starts_with("min");
        const u32 control=static_cast<u32>(std::stoul(control_token.substr(8),nullptr,16));
        const usize a=std::stoul(a_token.substr(2)),b=std::stoul(b_token.substr(2));
        const u32 status=static_cast<u32>(std::stoul(mxcsr_token.substr(6),nullptr,16));
        const XmmReg expected=scalar_probe_bytes(out_token.substr(4));
        for(const bool memory:{false,true}) for(const bool mode64:{false,true}) {
            const auto code=scalar_minmax_code(sd,minimum,memory);
            ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
            auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
            ASSERT_EQ(block.instruction_count,3u);
            CpuState seed;
            if(mode64) seed.init_long_mode(); else seed.init_protected_mode();
            seed.rip=kCodeAddr;
            seed.gpr.set64(static_cast<u8>(RegIndex::RBX),kDataAddr);
            seed.gpr.set64(static_cast<u8>(RegIndex::RDX),kSource);
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP),0x10000);
            seed.sse.mxcsr=control;
            seed.sse.xmm[0]=scalar_probe_operand(sd,a,0xA0);
            seed.sse.xmm[1]=scalar_probe_operand(sd,b,0xC0);
            for(usize w=0;w<2;++w) {
                seed.avx.ymm_high[0].u64_[w]=0x1111111111111111ull*(w+1);
                seed.avx.ymm_high[1].u64_[w]=0x3333333333333333ull*(w+1);
            }
            ASSERT_TRUE(m_memory->write_bytes(kSource,
                reinterpret_cast<const Byte*>(&seed.sse.xmm[1]),sizeof(XmmReg)));
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto interpreter=seed;
            Interpreter reference(interpreter,*m_memory);
            ASSERT_EQ(reference.run(3),ExecResult::Continue);
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto actual=seed;
            JitCompiler jit(*m_memory);
            auto* compiled=jit.compile(block);
            ASSERT_NE(compiled,nullptr);
            ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled,actual),JitExecResult::Continue);
            ASSERT_FALSE(actual.pending_raise.active);
            EXPECT_EQ(interpreter.sse.mxcsr,status);
            EXPECT_EQ(actual.sse.mxcsr,status);
            for(usize w=0;w<2;++w) {
                EXPECT_EQ(interpreter.sse.xmm[0].u64_[w],expected.u64_[w]);
                EXPECT_EQ(actual.sse.xmm[0].u64_[w],expected.u64_[w]);
                // Legacy encoding: no YMM write; the source is read only.
                EXPECT_EQ(actual.avx.ymm_high[0].u64_[w],seed.avx.ymm_high[0].u64_[w]);
                EXPECT_EQ(actual.sse.xmm[1].u64_[w],seed.sse.xmm[1].u64_[w]);
            }
            ASSERT_FALSE(HasFailure());
            ++checked;
        }
    }
    EXPECT_EQ(checked,4u*4u*144u*4u);
}

TEST_F(JitSseFpParityTest, NativeScalarMinmaxExceptions) {
    const auto path=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/oracle_data/sse_minmax_scalar_probe.expected";
    std::ifstream input(path);
    ASSERT_TRUE(input.good()) << path;
    std::string line;
    usize checked=0,raised=0;
    while(std::getline(input,line)) {
        if(!line.starts_with("fault ")) continue;
        std::istringstream row(line);
        std::string tag,op,control_t,a_t,b_t,code_t,nparams_t,info0_t,info1_t,record_t,out_t;
        ASSERT_TRUE(bool(row>>tag>>op>>control_t>>a_t>>b_t>>code_t>>nparams_t>>info0_t>>info1_t>>record_t>>out_t));
        SCOPED_TRACE(line);
        const bool sd=op.ends_with("sd"),minimum=op.starts_with("min");
        const u32 control=static_cast<u32>(std::stoul(control_t.substr(8),nullptr,16));
        const usize a=std::stoul(a_t.substr(2)),b=std::stoul(b_t.substr(2));
        const u32 exception=static_cast<u32>(std::stoul(code_t.substr(5),nullptr,16));
        const u64 info1=std::stoull(info1_t.substr(6),nullptr,16);
        const u32 record=static_cast<u32>(std::stoul(record_t.substr(13),nullptr,16));
        const XmmReg expected=scalar_probe_bytes(out_t.substr(4));
        for(const bool mode64:{false,true}) {
            const auto code=scalar_minmax_code(sd,minimum,false);
            ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
            auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
            CpuState seed;
            if(mode64) seed.init_long_mode(); else seed.init_protected_mode();
            seed.rip=kCodeAddr;
            seed.gpr.set64(static_cast<u8>(RegIndex::RBX),kDataAddr);
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP),0x10000);
            seed.sse.mxcsr=control;
            seed.sse.xmm[0]=scalar_probe_operand(sd,a,0xA0);
            seed.sse.xmm[1]=scalar_probe_operand(sd,b,0xC0);
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto interpreter=seed;
            Interpreter reference(interpreter,*m_memory);
            EXPECT_EQ(reference.run(3),exception?ExecResult::Exception:ExecResult::Continue);
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,control));
            auto actual=seed;
            JitCompiler jit(*m_memory);
            auto* compiled=jit.compile(block);
            ASSERT_NE(compiled,nullptr);
            test::execute_like_dispatcher(jit, *compiled,actual);
            EXPECT_EQ(actual.pending_raise.active,exception!=0);
            for(usize w=0;w<2;++w) {
                // Faulting: the destination is not written (the probe's
                // `out` is the original operand).
                EXPECT_EQ(interpreter.sse.xmm[0].u64_[w],expected.u64_[w]);
                EXPECT_EQ(actual.sse.xmm[0].u64_[w],expected.u64_[w]);
            }
            if(exception) {
                // Windows: STATUS_FLOAT_INVALID_OPERATION, {0, MXCSR} with
                // the new sticky bit already set.
                EXPECT_EQ(actual.pending_raise.code,exception);
                EXPECT_EQ(actual.pending_raise.num_args,2u);
                EXPECT_EQ(actual.pending_raise.args[0],0u);
                EXPECT_EQ(actual.pending_raise.args[1],info1);
                EXPECT_EQ(actual.sse.mxcsr,record);
                EXPECT_EQ(interpreter.sse.mxcsr,record);
                EXPECT_EQ(actual.rip,kCodeAddr+3);
                EXPECT_EQ(interpreter.rip,kCodeAddr+3);
                ++raised;
            }
            ASSERT_FALSE(HasFailure());
            ++checked;
        }
    }
    EXPECT_EQ(checked,4u*3u*5u*2u*2u);
    EXPECT_GT(raised,40u);
}

// The two tests above pass whichever arm runs -- the slow arm IS the
// interpreter -- so this pins the shape that makes the native arm the one
// that runs: the selection is an FCMGT, the arm is out of line, and every
// exit to it is a CBZ/CBNZ (imm19; a TBZ cannot reach a far cold arm). No
// MXCSR test on IM (7), DM (8) or FTZ (15) is a TBZ/TBNZ any more, and FTZ
// is not a reason to leave at all: Unreal runs FTZ-only, and every MIN/MAX
// took the interpreter there.
TEST_F(JitSseFpParityTest, MinmaxIsNativeInEveryFlushModeAndItsArmIsReachable) {
    namespace a64 = vitreo::cpu::jit::arm64;
    const std::vector<std::vector<u8>> forms = {
        {0xf3,0x0f,0x5d,0xc1}, {0xf3,0x0f,0x5f,0xc1},   // minss/maxss
        {0xf2,0x0f,0x5d,0xc1}, {0xf2,0x0f,0x5f,0xc1},   // minsd/maxsd
        {0x0f,0x5d,0xc1}, {0x66,0x0f,0x5f,0xc1}};       // minps/maxpd
    for(const auto& code:forms) for(const bool mode64:{false,true}) {
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
        auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
        JitCompiler jit(*m_memory);
        auto* compiled=jit.compile(block);
        ASSERT_NE(compiled,nullptr);
        std::vector<u32> words(static_cast<const u32*>(compiled->code),
            static_cast<const u32*>(compiled->code)+compiled->code_size/4);
        if(compiled->cold_code()!=nullptr) {
            const auto* cold=reinterpret_cast<const u32*>(compiled->cold_code());
            words.insert(words.end(),cold,cold+compiled->cold_size/4);
        }
        const auto count=[&](u32 mask,u32 want) {
            return std::count_if(words.begin(),words.end(),
                [&](u32 w){return (w&mask)==(want&mask);});
        };
        const bool pd=code[0]==0xf2||code[0]==0x66;
        const u32 fcmgt=a64::fcmgt_v(2,0,1,pd?a64::FS_D:a64::FS_S);
        EXPECT_EQ(count(0xFFE0FC00u,fcmgt),1) << "selection is not native";
        for(const u8 bit:{u8{7},u8{8},u8{15}}) {
            // TBZ/TBNZ W2 (MXCSR), any displacement.
            EXPECT_EQ(count(0x7EF8001Fu,a64::tbz(2,bit,0)),0)
                << "MXCSR bit " << int(bit) << " still tested with TB(N)Z";
        }
    }
}

// SHA1RNDS4 (every imm8 function), SHA1NEXTE, SHA1MSG1, SHA1MSG2 are native
// ARM SHA1C/P/M/SU1 sequences with lane reversals (emit_sha1_native); the
// interpreter implements the Intel SDM text directly. Random states, both
// bitnesses, register and (64-bit) aligned memory source.
TEST_F(JitSseFpParityTest, Sha1NativeMatchesInterpreter) {
    struct Form { std::vector<u8> reg; std::vector<u8> mem; };
    std::vector<Form> forms;
    for (u8 imm=0; imm<4; ++imm)
        forms.push_back({{0x0f,0x3a,0xcc,0xc1,imm},{0x0f,0x3a,0xcc,0x02,imm}});
    forms.push_back({{0x0f,0x38,0xc8,0xc1},{0x0f,0x38,0xc8,0x02}});  // sha1nexte
    forms.push_back({{0x0f,0x38,0xc9,0xc1},{0x0f,0x38,0xc9,0x02}});  // sha1msg1
    forms.push_back({{0x0f,0x38,0xca,0xc1},{0x0f,0x38,0xca,0x02}});  // sha1msg2
    constexpr u64 kSource=0x3000;  // 16-byte aligned
    u64 rng=0x243F6A8885A308D3ull;
    const auto next=[&](){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;};
    usize checked=0;
    for (const Form& form:forms) for (const bool memory:{false,true})
    for (const bool mode64:{false,true}) {
        if (memory && !mode64) continue;
        const auto& code=memory?form.mem:form.reg;
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
        auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
        ASSERT_EQ(block.instruction_count,1u);
        JitCompiler jit(*m_memory);
        auto* compiled=jit.compile(block);
        ASSERT_NE(compiled,nullptr);
        for (int round=0; round<300; ++round) {
            CpuState seed;
            if(mode64) seed.init_long_mode(); else seed.init_protected_mode();
            seed.rip=kCodeAddr;
            seed.gpr.set64(static_cast<u8>(RegIndex::RDX),kSource);
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP),0x10000);
            for (u8 r=0;r<8;++r) {
                seed.sse.xmm[r].u64_[0]=next();seed.sse.xmm[r].u64_[1]=next();
            }
            const u64 m0=next(),m1=next();
            ASSERT_TRUE(m_memory->write_u64(kSource,m0));
            ASSERT_TRUE(m_memory->write_u64(kSource+8,m1));
            auto interpreter=seed;
            Interpreter reference(interpreter,*m_memory);
            ASSERT_EQ(reference.run(1),ExecResult::Continue);
            auto actual=seed;
            ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled,actual),JitExecResult::Continue);
            ASSERT_FALSE(actual.pending_raise.active);
            for (u8 r=0;r<8;++r) {
                ASSERT_EQ(actual.sse.xmm[r].u64_[0],interpreter.sse.xmm[r].u64_[0])
                    << "form " << int(code[2]) << " imm " << int(code.back())
                    << " xmm" << int(r) << " mem=" << memory;
                ASSERT_EQ(actual.sse.xmm[r].u64_[1],interpreter.sse.xmm[r].u64_[1])
                    << "form " << int(code[2]) << " xmm" << int(r);
            }
            ++checked;
        }
    }
    EXPECT_EQ(checked,7u*3u*300u);
}

// The same four instructions against silicon: tools/oracle_probe/
// sha1_ni_probe ran each on an Alder Lake core (Rosetta has no SHA-NI, so
// the oracle sweep cannot). Interpreter and JIT, both bitnesses.
TEST_F(JitSseFpParityTest, Sha1MatchesSilicon) {
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/oracle_data/sha1_ni_probe.expected");
    ASSERT_TRUE(input.good());
    const auto quad=[](const std::string& hex) {
        XmmReg v{};
        for (int l=0;l<4;++l)
            v.u32_[3-l]=static_cast<u32>(std::stoul(hex.substr(l*8,8),nullptr,16));
        return v;
    };
    std::string line;
    usize checked=0;
    while (std::getline(input,line)) {
        std::istringstream row(line);
        std::string op,a_t,b_t,out_t;
        ASSERT_TRUE(bool(row>>op>>a_t>>b_t>>out_t));
        SCOPED_TRACE(line);
        std::vector<u8> code;
        if (op.starts_with("sha1rnds4_"))
            code={0x0f,0x3a,0xcc,0xc1,static_cast<u8>(op.back()-'0')};
        else if (op=="sha1nexte") code={0x0f,0x38,0xc8,0xc1};
        else if (op=="sha1msg1") code={0x0f,0x38,0xc9,0xc1};
        else { ASSERT_EQ(op,"sha1msg2"); code={0x0f,0x38,0xca,0xc1}; }
        const XmmReg a=quad(a_t.substr(2)),b=quad(b_t.substr(2)),
                     expected=quad(out_t.substr(4));
        for (const bool mode64:{false,true}) {
            ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
            auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
            CpuState seed;
            if(mode64) seed.init_long_mode(); else seed.init_protected_mode();
            seed.rip=kCodeAddr;
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP),0x10000);
            seed.sse.xmm[0]=a; seed.sse.xmm[1]=b;
            auto interpreter=seed;
            Interpreter reference(interpreter,*m_memory);
            ASSERT_EQ(reference.run(1),ExecResult::Continue);
            JitCompiler jit(*m_memory);
            auto* compiled=jit.compile(block);
            ASSERT_NE(compiled,nullptr);
            auto actual=seed;
            ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled,actual),JitExecResult::Continue);
            for (usize w=0;w<2;++w) {
                EXPECT_EQ(interpreter.sse.xmm[0].u64_[w],expected.u64_[w]);
                EXPECT_EQ(actual.sse.xmm[0].u64_[w],expected.u64_[w]);
            }
            ASSERT_FALSE(HasFailure());
            ++checked;
        }
    }
    EXPECT_EQ(checked,448u*2u);
}

TEST_F(JitSseFpParityTest, NativePackedMinmaxMemoryFaults) {
    // The SDK assembler's twelve functions perform the MIN/MAX memory read
    // itself. Oracle rows include #GP, page boundaries, and #XM priority.
    constexpr u64 base=0x80000, page=0x1000;
    ASSERT_TRUE(m_memory->map_region(base,page*2,MemoryProtection::ReadWrite));
    ASSERT_TRUE(m_memory->protect_region(base+page,page,MemoryProtection::None));
    const std::array<u64,5> offsets={16,17,page-16,page-7,page};
    std::ifstream input(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()
        / "tests/oracle_data/sse_minmax_memory_probe.expected");
    ASSERT_TRUE(input.good());
    std::string line;usize checked=0;
    while(std::getline(input,line)) {
        std::istringstream row(line);
        std::string tag,form,op;
        ASSERT_TRUE(bool(row>>tag>>form>>op));
        std::unordered_map<std::string,std::string> fields;
        for(std::string field;row>>field;) {
            const auto equals=field.find('=');
            ASSERT_NE(equals,std::string::npos);
            fields.emplace(field.substr(0,equals),field.substr(equals+1));
        }
        const auto hex=[&](const char* key){return std::stoull(fields.at(key),nullptr,16);};
        const bool vex=form!="legacy",wide=form=="vex256",pd=op.ends_with("pd");
        const usize number=std::stoul(fields.at("case"));ASSERT_LT(number,offsets.size());
        const u32 status=static_cast<u32>(hex("code")),mxcsr=static_cast<u32>(hex("mxcsr"));
        std::vector<u8> code={0x0f,0xae,0x13}; // LDMXCSR [rbx]
        if(vex) code.insert(code.end(),{0xc5,static_cast<u8>((wide?0xfc:0xf8)|(pd?1:0))});
        else {if(pd) code.push_back(0x66);code.push_back(0x0f);}
        code.insert(code.end(),{static_cast<u8>(op.starts_with("min")?0x5d:0x5f),0x02});
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
        for(bool mode64:{false,true}) {
            SCOPED_TRACE(line);
            SCOPED_TRACE(mode64);
            CpuState seed;
            if(mode64)seed.init_long_mode();else seed.init_protected_mode();
            seed.rip=kCodeAddr;seed.gpr.rbx=kDataAddr;seed.gpr.rdx=base+offsets[number];
            seed.gpr.rsp=0x10000;seed.sse.mxcsr=kMxcsrDefault&~MXCSR_IM;
            for(usize word=0;word<4;++word) {
                auto& reg=word<2?seed.sse.xmm[0]:seed.avx.ymm_high[0];
                reg.u64_[word%2]=pd?u64{0x7ff8000000000001}:u64{0x7fc000017fc00001};
            }
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,seed.sse.mxcsr));
            auto interpreter=seed;Interpreter reference(interpreter,*m_memory);
            ASSERT_EQ(reference.run(2),ExecResult::Exception);
            ASSERT_TRUE(m_memory->write_u32(kDataAddr,seed.sse.mxcsr));
            auto actual=seed;JitCompiler jit(*m_memory);
            auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
            auto* compiled=jit.compile(block);ASSERT_NE(compiled,nullptr);
            test::execute_like_dispatcher(jit, *compiled,actual);
            ASSERT_TRUE(actual.pending_raise.active);
            EXPECT_EQ(actual.pending_raise.code,status);
            EXPECT_EQ(cpu_exception_to_status(reference.context().exception),status);
            EXPECT_EQ(interpreter.sse.mxcsr,mxcsr);EXPECT_EQ(actual.sse.mxcsr,mxcsr);
            const u64 fault=fields.at("address")=="unavailable"
                ?(mode64?std::numeric_limits<u64>::max():std::numeric_limits<u32>::max())
                :fields.at("address")=="guard"?base+page:hex("other");
            EXPECT_EQ(reference.context().exception_addr,fault);
            EXPECT_EQ(actual.pending_raise.num_args,2u);
            EXPECT_EQ(actual.pending_raise.args[0],hex("access"));
            EXPECT_EQ(actual.pending_raise.args[1],fault);
            EXPECT_EQ(actual.rip,kCodeAddr+3);EXPECT_EQ(interpreter.rip,kCodeAddr+3);
            for(usize word=0;word<2;++word) {
                EXPECT_EQ(interpreter.sse.xmm[0].u64_[word],seed.sse.xmm[0].u64_[word]);
                EXPECT_EQ(actual.sse.xmm[0].u64_[word],seed.sse.xmm[0].u64_[word]);
                EXPECT_EQ(interpreter.avx.ymm_high[0].u64_[word],seed.avx.ymm_high[0].u64_[word]);
                EXPECT_EQ(actual.avx.ymm_high[0].u64_[word],seed.avx.ymm_high[0].u64_[word]);
            }
            ASSERT_FALSE(HasFailure());++checked;
        }
    }
    EXPECT_EQ(checked,120u);
}


TEST_F(JitSseFpParityTest, NativePackedMinmaxZeroProofBoundaries) {
    // SDM XORPS upper-half rules plus native MIN/MAX a=denormal,b=1 rows:
    // DAZ clear preserves that input and sets DE. An adjacent proof must not
    // cover legacy upper bits, survive an intervening write, or omit memory.
    for(bool mode64:{false,true}) for(bool legacy:{false,true})
    for(bool overwrite:{false,true}) {
        SCOPED_TRACE(mode64);
        SCOPED_TRACE(legacy);
        SCOPED_TRACE(overwrite);
        std::vector<u8> code={0x0f,0xae,0x13};
        if(legacy)code.insert(code.end(),{0x0f,0x57,0xc0});
        else code.insert(code.end(),{0xc5,0xf8,0x57,0xc0});
        if(overwrite)code.insert(code.end(),{0x0f,0x10,0xc2}); // MOVUPS xmm0,xmm2
        code.insert(code.end(),{0xc5,0xfc,0x5d,0xc1,0x0f,0xae,0x1b});
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,code.data(),code.size()));
        CpuState seed;
        if(mode64)seed.init_long_mode();else seed.init_protected_mode();
        seed.rip=kCodeAddr;seed.gpr.rbx=kDataAddr;seed.gpr.rsp=0x10000;
        seed.sse.mxcsr=kMxcsrDefault;
        for(usize lane=0;lane<4;++lane) {
            seed.sse.xmm[0].u32_[lane]=kF32[11];
            seed.avx.ymm_high[0].u32_[lane]=kF32[2];
            seed.sse.xmm[1].u32_[lane]=seed.avx.ymm_high[1].u32_[lane]=kF32[4];
            seed.sse.xmm[2].u32_[lane]=kF32[2];
        }
        ASSERT_TRUE(m_memory->write_u32(kDataAddr,kMxcsrDefault));
        auto interpreter=seed;Interpreter reference(interpreter,*m_memory);
        ASSERT_EQ(reference.run(overwrite?5:4),ExecResult::Continue);
        ASSERT_TRUE(m_memory->write_u32(kDataAddr,kMxcsrDefault));
        auto actual=seed;JitCompiler jit(*m_memory);
        auto block=build_block(code,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
        auto* compiled=jit.compile(block);ASSERT_NE(compiled,nullptr);
        ASSERT_EQ(test::execute_like_dispatcher(jit, *compiled,actual),JitExecResult::Continue);
        ASSERT_FALSE(actual.pending_raise.active);
        const u32 status=kMxcsrDefault|((legacy||overwrite)?MXCSR_DE:0);
        EXPECT_EQ(interpreter.sse.mxcsr,status);EXPECT_EQ(actual.sse.mxcsr,status);
        for(usize lane=0;lane<4;++lane) {
            const u32 low=overwrite?kF32[2]:kF32[0],high=legacy?kF32[2]:kF32[0];
            EXPECT_EQ(interpreter.sse.xmm[0].u32_[lane],low);
            EXPECT_EQ(actual.sse.xmm[0].u32_[lane],low);
            EXPECT_EQ(interpreter.avx.ymm_high[0].u32_[lane],high);
            EXPECT_EQ(actual.avx.ymm_high[0].u32_[lane],high);
        }
        ASSERT_FALSE(HasFailure());
    }
    ASSERT_TRUE(m_memory->map_region(0x80000,0x1000,MemoryProtection::None));
    const std::vector<u8> memory={0xc5,0xf8,0x57,0xc0,0xc5,0xf8,0x5f,0x02};
    ASSERT_TRUE(m_memory->write_bytes(kCodeAddr,memory.data(),memory.size()));
    for(bool mode64:{false,true}) {
        CpuState actual;
        if(mode64)actual.init_long_mode();else actual.init_protected_mode();
        actual.rip=kCodeAddr;actual.gpr.rdx=0x80000;actual.gpr.rsp=0x10000;
        JitCompiler jit(*m_memory);
        auto block=build_block(memory,mode64?DecodeMode::Mode64:DecodeMode::Mode32);
        auto* compiled=jit.compile(block);ASSERT_NE(compiled,nullptr);
        test::execute_like_dispatcher(jit, *compiled,actual);
        ASSERT_TRUE(actual.pending_raise.active);
        EXPECT_EQ(actual.pending_raise.code,STATUS_ACCESS_VIOLATION);
        EXPECT_EQ(actual.pending_raise.args[1],0x80000u);
        EXPECT_EQ(actual.rip,kCodeAddr+4);
    }
}

// Intel SDM Vol. 2 CVTPD2PS / VCVTPD2PS: two (VEX.256: four) f64 -> f32 in the
// low half of xmm1, rounded per MXCSR.RC. Legacy keeps YMM[255:128]; VEX
// zeroes every bit above the result. PE is set only when a FINITE source
// narrows inexactly (sse_cvt_narrow_pe), and Invalid only for an SNaN, so an
// infinity or a QNaN source raises nothing at all. Each engine writes PE into
// MXCSR itself and leaves IE/OE/UE (and the host's own inexact) in the host's
// sticky accumulator until the guest can observe MXCSR, so every case runs
// twice: LDMXCSR; CVT compares the engines' own MXCSR, and LDMXCSR; CVT;
// STMXCSR [rbx] compares what the guest reads. Every window of the special
// values plus the narrowing boundaries, register and memory, 32- and 64-bit,
// under each rounding mode and FTZ / DAZ.
TEST_F(JitSseFpParityTest, Cvtpd2psMatchesInterpreterAcrossSpecialValues) {
    const u32 modes[] = {
        kMxcsrDefault,
        kMxcsrDefault | 0x2000u,  // RC = down
        kMxcsrDefault | 0x4000u,  // RC = up
        kMxcsrDefault | 0x6000u,  // RC = toward zero
        kMxcsrDefault | 0x8000u,  // FTZ
        kMxcsrDefault | 0x0040u,  // DAZ
        kMxcsrDefault | 0x8040u,  // FTZ + DAZ
    };
    std::vector<u64> corpus(std::begin(kF64), std::end(kF64));
    corpus.insert(corpus.end(), {
        0x3810000000000000ull,  // 2^-126 = FLT_MIN, exact
        0x36A0000000000000ull,  // 2^-149, the least f32 denormal: exact unless flushed
        0x3690000000000000ull,  // 2^-150: ties to zero
        0x37A16C262777579Cull,  // 1e-40: an inexact f32 denormal
        0x47EFFFFFEFFFFFFFull,  // just under the overflow midpoint: FLT_MAX
        0x47EFFFFFF0000000ull,  // the midpoint above FLT_MAX: RN overflows
        0x3FD5555555555555ull,  // 1/3: an ordinary inexact narrowing
    });
    const auto quiet_or_infinite = [](u64 bits) {
        const bool all_ones = ((bits >> 52) & 0x7FFu) == 0x7FFu;
        const u64 mantissa = bits & 0x000FFFFFFFFFFFFFull;
        return all_ones && (mantissa == 0 || (mantissa >> 51) != 0);
    };
    enum class Enc { Legacy, Vex128, Vex256 };
    usize checked = 0;
    for (const bool mode64 : {false, true})
    for (const Enc enc : {Enc::Legacy, Enc::Vex128, Enc::Vex256})
    for (const bool memory : {false, true})
    for (const bool publish : {false, true}) {
        const bool vex = enc != Enc::Legacy, wide = enc == Enc::Vex256;
        std::vector<u8> code{0x0F, 0xAE, 0x13};  // LDMXCSR [rbx]
        if (vex) {
            code.insert(code.end(), {0xC5, static_cast<u8>(wide ? 0xFD : 0xF9)});
        } else {
            code.insert(code.end(), {0x66, 0x0F});
        }
        // xmm0, xmm1 / xmm0, [rdx]; then STMXCSR [rbx].
        code.insert(code.end(), {0x5A, static_cast<u8>(memory ? 0x02 : 0xC1)});
        if (publish) code.insert(code.end(), {0x0F, 0xAE, 0x1B});
        const u32 count = publish ? 3u : 2u;
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr, code.data(), code.size()));
        const BasicBlock block =
            build_block(code, mode64 ? DecodeMode::Mode64 : DecodeMode::Mode32);
        ASSERT_EQ(block.instruction_count, count);
        // Legacy m128 must be 16-byte aligned; VEX has no requirement.
        const u64 source = vex ? 0x3001 : 0x3000;
        for (const u32 mxcsr : modes)
        for (usize i = 0; i < corpus.size(); ++i) {
            const auto value = [&](usize lane) {
                return corpus[(i + lane) % corpus.size()];
            };
            std::ostringstream tag;
            tag << (mode64 ? "64 " : "32 ")
                << (enc == Enc::Legacy ? "legacy" : wide ? "vex256" : "vex128")
                << (memory ? " mem" : " reg")
                << (publish ? " published" : " own") << " mxcsr=0x"
                << std::hex << mxcsr << std::dec << " src[" << i << "]";
            SCOPED_TRACE(tag.str());
            CpuState seed;
            if (mode64) seed.init_long_mode(); else seed.init_protected_mode();
            seed.rip = kCodeAddr;
            seed.gpr.set64(static_cast<u8>(RegIndex::RBX), kDataAddr);
            seed.gpr.set64(static_cast<u8>(RegIndex::RDX), source);
            seed.gpr.set64(static_cast<u8>(RegIndex::RSP), 0x10000);
            seed.sse.mxcsr = mxcsr;
            seed.sse.xmm[0].u64_[0] = 0xAAAAAAAAAAAAAAAAull;
            seed.sse.xmm[0].u64_[1] = 0x5555555555555555ull;
            seed.avx.ymm_high[0].u64_[0] = 0x1111111111111111ull;
            seed.avx.ymm_high[0].u64_[1] = 0x2222222222222222ull;
            for (usize lane = 0; lane < 2; ++lane) {
                seed.sse.xmm[1].u64_[lane] = value(lane);
                seed.avx.ymm_high[1].u64_[lane] = value(lane + 2);
            }
            ASSERT_TRUE(m_memory->write_bytes(source,
                reinterpret_cast<const Byte*>(&seed.sse.xmm[1]), 16));
            ASSERT_TRUE(m_memory->write_bytes(source + 16,
                reinterpret_cast<const Byte*>(&seed.avx.ymm_high[1]), 16));

            ASSERT_TRUE(m_memory->write_u32(kDataAddr, mxcsr));
            CpuState icpu = seed;
            {
                Interpreter interp(icpu, *m_memory);
                ASSERT_EQ(interp.run(count), ExecResult::Continue);
            }
            const auto ipub = m_memory->read_u32(kDataAddr);
            ASSERT_TRUE(ipub.is_ok());

            ASSERT_TRUE(m_memory->write_u32(kDataAddr, mxcsr));
            CpuState jcpu = seed;
            JitCompiler jit(*m_memory);
            CompiledBlock* cb = jit.compile(block);
            ASSERT_NE(cb, nullptr) << "JIT declined the block";
            ASSERT_EQ(test::execute_like_dispatcher(jit, *cb, jcpu), JitExecResult::Continue);
            ASSERT_FALSE(jcpu.pending_raise.active);
            const auto jpub = m_memory->read_u32(kDataAddr);
            ASSERT_TRUE(jpub.is_ok());

            EXPECT_EQ(icpu.sse.mxcsr, jcpu.sse.mxcsr)
                << "MXCSR: interp=0x" << std::hex << icpu.sse.mxcsr
                << " jit=0x" << jcpu.sse.mxcsr;
            EXPECT_EQ(ipub.value(), jpub.value())
                << "published MXCSR: interp=0x" << std::hex << ipub.value()
                << " jit=0x" << jpub.value();
            for (usize q = 0; q < 2; ++q) {
                EXPECT_EQ(icpu.sse.xmm[0].u64_[q], jcpu.sse.xmm[0].u64_[q])
                    << "xmm0 interp=" << hex128(icpu.sse.xmm[0])
                    << " jit=" << hex128(jcpu.sse.xmm[0]);
                EXPECT_EQ(icpu.avx.ymm_high[0].u64_[q],
                          jcpu.avx.ymm_high[0].u64_[q]);
                EXPECT_EQ(jcpu.sse.xmm[1].u64_[q], seed.sse.xmm[1].u64_[q]);
                EXPECT_EQ(jcpu.avx.ymm_high[1].u64_[q],
                          seed.avx.ymm_high[1].u64_[q]);
                // Above the result: legacy keeps YMM high, VEX zeroes it.
                const u64 above = vex ? 0 : seed.avx.ymm_high[0].u64_[q];
                EXPECT_EQ(jcpu.avx.ymm_high[0].u64_[q], above);
            }
            if (!wide) EXPECT_EQ(jcpu.sse.xmm[0].u64_[1], 0u);
            // SDM, not the interpreter: an Inf or QNaN converts exactly and
            // raises nothing, so such a source leaves the flags as loaded.
            bool silent = true;
            for (usize lane = 0; lane < (wide ? 4u : 2u); ++lane) {
                silent = silent && quiet_or_infinite(value(lane));
            }
            if (silent && publish) {
                EXPECT_EQ(ipub.value(), mxcsr) << "interpreter raised a flag";
                EXPECT_EQ(jpub.value(), mxcsr) << "JIT raised a flag";
            }
            if (HasFailure()) return;
            ++checked;
        }
    }
    EXPECT_EQ(checked,
              2u * 3u * 2u * 2u * std::size(modes) * (kF64Count + 7u));
}

// The PE update is on the hot path. A real double -> float narrowing is
// usually inexact, and a cold arm taken on that costs a branch out, an
// XMM-cache reload and a branch back, and marks the instruction as a helper
// call for the bookkeeping after it. The arm's entry is a CBZ/CBNZ on W3, the
// lane mask's register; the inline form has none.
TEST_F(JitSseFpParityTest, Cvtpd2psSetsPrecisionWithoutAColdArm) {
    for (const bool memory : {false, true}) {
        const std::vector<u8> code = with_prefix(0x66, 0x5A, memory);
        ASSERT_TRUE(m_memory->write_bytes(kCodeAddr, code.data(), code.size()));
        BasicBlock block = build_block(code);
        ASSERT_EQ(block.instruction_count, 1u);
        JitCompiler jit(*m_memory);
        CompiledBlock* cb = jit.compile(block);
        ASSERT_NE(cb, nullptr);
        const auto* words = static_cast<const u32*>(cb->code);
        usize mask_branches = 0;
        for (u32 i = 0; i < cb->code_size / 4u; ++i) {
            // CBZ/CBNZ, either width (bit 31 ignored), Rt = 3.
            if ((words[i] & 0x7E00001Fu) == 0x34000003u) ++mask_branches;
        }
        EXPECT_EQ(mask_branches, 0u) << (memory ? "m128" : "register");
    }
}

}  // namespace
