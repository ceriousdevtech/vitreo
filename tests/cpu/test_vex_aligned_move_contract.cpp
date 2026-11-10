// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// VEX.128 VMOVAPS / VMOVAPD / VMOVDQA memory forms, interpreter and JIT.
//
// Intel SDM Vol. 2 MOVAPS, MOVAPD, MOVDQA (opcode tables, Description and
// "Other Exceptions", Exceptions Type1.SSE/SSE2 in Table 2-18):
//   VEX.128.0F.WIG    28/29 /r  VMOVAPS xmm1, xmm2/m128 | xmm2/m128, xmm1
//   VEX.128.66.0F.WIG 28/29 /r  VMOVAPD
//   VEX.128.66.0F.WIG 6F/7F /r  VMOVDQA
// - "When the source or destination operand is a memory operand, the operand
//   must be aligned on a 16-byte boundary or a general-protection exception
//   (#GP) will be generated" -- the VEX.128 forms included.
// - "VEX.vvvv ... reserved and must be 1111b otherwise instructions will #UD";
//   the same note is on VMOVUPS/VMOVUPD (0F 10/11) and VMOVDQU (F3 0F 6F/7F).
// - 0F 28/29 are defined only with VEX.pp = 00 (VMOVAPS) and 01 (VMOVAPD);
//   VEX.F3/F2.0F 28/29 is no instruction at all, so it is #UD.
//
// Windows reports the #GP as STATUS_ACCESS_VIOLATION with [0] = 0 and
// [1] = all-ones (no CR2), the #UD as STATUS_ILLEGAL_INSTRUCTION. A faulting
// instruction commits nothing (Intel SDM Vol. 3A 6.5). Expected values come
// from that contract, not from either engine.

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "jit_crypto_test_harness.hpp"

using namespace vitreo;
using namespace vitreo::cpu;
namespace w = ws3test;

namespace {

constexpr u32 kAccessViolation = 0xC0000005u;
constexpr u32 kIllegalInstruction = 0xC000001Du;

struct MoveForm {
    const char* name;
    std::vector<u8> code;
    bool store;
    bool needs64;  // REX-extended register: long mode only
    int reg;       // the xmm register operand
};

/// Every VEX.128 aligned-move memory form, [rdx] operand, low and high
/// register, two- and three-byte VEX.
const std::vector<MoveForm>& move_forms() {
    static const std::vector<MoveForm> f{
        {"vmovaps xmm0,[rdx] (C5)", {0xC5, 0xF8, 0x28, 0x02}, false, false, 0},
        {"vmovaps xmm0,[rdx] (C4)", {0xC4, 0xE1, 0x78, 0x28, 0x02}, false, false, 0},
        {"vmovaps xmm9,[rdx]", {0xC4, 0x61, 0x78, 0x28, 0x0A}, false, true, 9},
        {"vmovaps xmm9,[rdx] (C5)", {0xC5, 0x78, 0x28, 0x0A}, false, true, 9},
        {"vmovapd xmm0,[rdx]", {0xC5, 0xF9, 0x28, 0x02}, false, false, 0},
        {"vmovapd xmm9,[rdx]", {0xC4, 0x61, 0x79, 0x28, 0x0A}, false, true, 9},
        {"vmovdqa xmm0,[rdx]", {0xC5, 0xF9, 0x6F, 0x02}, false, false, 0},
        {"vmovdqa xmm9,[rdx]", {0xC4, 0x61, 0x79, 0x6F, 0x0A}, false, true, 9},
        // A low register other than xmm0: VEX.vvvv = 1111b names xmm0, so
        // any engine that treated vvvv as a merge source (dst != vvvv) took a
        // different path for these than for xmm0 -- in both bitnesses.
        {"vmovaps xmm3,[rdx]", {0xC5, 0xF8, 0x28, 0x1A}, false, false, 3},
        {"vmovapd xmm3,[rdx]", {0xC5, 0xF9, 0x28, 0x1A}, false, false, 3},
        {"vmovdqa xmm3,[rdx]", {0xC4, 0xE1, 0x79, 0x6F, 0x1A}, false, false, 3},
        {"vmovaps [rdx],xmm3", {0xC5, 0xF8, 0x29, 0x1A}, true, false, 3},
        {"vmovaps [rdx],xmm0", {0xC5, 0xF8, 0x29, 0x02}, true, false, 0},
        {"vmovaps [rdx],xmm9", {0xC4, 0x61, 0x78, 0x29, 0x0A}, true, true, 9},
        {"vmovapd [rdx],xmm0", {0xC5, 0xF9, 0x29, 0x02}, true, false, 0},
        {"vmovapd [rdx],xmm9", {0xC4, 0x61, 0x79, 0x29, 0x0A}, true, true, 9},
        {"vmovdqa [rdx],xmm0", {0xC5, 0xF9, 0x7F, 0x02}, true, false, 0},
        {"vmovdqa [rdx],xmm9", {0xC4, 0x61, 0x79, 0x7F, 0x0A}, true, true, 9},
    };
    return f;
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

void set_ea(CpuState& cpu, bool bits64, u64 ea) {
    if (bits64) cpu.gpr.set64(w::kRdx, ea);
    else cpu.gpr.set32(w::kRdx, static_cast<u32>(ea));
}

void fill_data(w::CryptoRig& rig) {
    std::vector<u8> bytes(64);
    for (usize i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<u8>(0xA0 + i);
    ASSERT_TRUE(rig.mem().write_bytes(w::kData, bytes.data(), bytes.size()));
}

std::vector<u8> read_data(w::CryptoRig& rig) {
    std::vector<u8> bytes(64);
    EXPECT_TRUE(rig.mem().read_bytes(w::kData, bytes.data(), bytes.size()));
    return bytes;
}

void expect_untouched(const CpuState& got, const CpuState& init) {
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE(::testing::Message() << "xmm" << i);
        EXPECT_EQ(got.sse.xmm[i].u64_[0], init.sse.xmm[i].u64_[0]);
        EXPECT_EQ(got.sse.xmm[i].u64_[1], init.sse.xmm[i].u64_[1]);
        EXPECT_EQ(got.avx.ymm_high[i].u64_[0], init.avx.ymm_high[i].u64_[0]);
        EXPECT_EQ(got.avx.ymm_high[i].u64_[1], init.avx.ymm_high[i].u64_[1]);
    }
    EXPECT_EQ(got.flags.rflags, w::kFlags);
}

const std::vector<u8> kTrailingMovEax{0xB8, 0x78, 0x56, 0x34, 0x12};

}  // namespace

TEST(VexAlignedMoveContract, AlignedOperandMatchesInBothEngines) {
    for_each_cfg([](w::CryptoRig& rig) {
        for (const MoveForm& f : move_forms()) {
            if (f.needs64 && !rig.bits64()) continue;
            SCOPED_TRACE(f.name);
            fill_data(rig);
            CpuState init = rig.base();
            set_ea(init, rig.bits64(), w::kData + 16);
            CpuState interpreted = init;
            ASSERT_EQ(rig.interp(f.code, interpreted), ExecResult::Continue);
            const std::vector<u8> interp_mem = read_data(rig);
            fill_data(rig);
            const w::JitRun r = rig.jit(f.code, init);
            ASSERT_TRUE(r.compiled);
            EXPECT_EQ(r.result, w::JitExecResult::Continue);
            EXPECT_FALSE(r.cpu.pending_raise.active);
            w::expect_vec_state_equal(interpreted, r.cpu);
            EXPECT_EQ(read_data(rig), interp_mem);
            // The architected result, independent of either engine: the
            // destination now holds the 16 source bytes.
            const int reg = f.reg;
            u8 xmm_bytes[16];
            std::memcpy(xmm_bytes, &r.cpu.sse.xmm[reg], 16);
            if (f.store) {
                for (int i = 0; i < 16; ++i) {
                    EXPECT_EQ(interp_mem[16 + i], xmm_bytes[i]) << i;
                }
            } else {
                for (int i = 0; i < 16; ++i) {
                    EXPECT_EQ(xmm_bytes[i], static_cast<u8>(0xA0 + 16 + i)) << i;
                }
                // VEX.128: DEST[MAXVL-1:128] := 0.
                EXPECT_EQ(r.cpu.avx.ymm_high[reg].u64_[0], 0u);
                EXPECT_EQ(r.cpu.avx.ymm_high[reg].u64_[1], 0u);
            }
        }
    });
}

TEST(VexAlignedMoveContract, MisalignedOperandIsGpWithNoFaultAddress) {
    for_each_cfg([](w::CryptoRig& rig) {
        const u64 all_ones = rig.bits64() ? std::numeric_limits<u64>::max()
                                          : u64{0xFFFFFFFFu};
        for (const MoveForm& f : move_forms()) {
            if (f.needs64 && !rig.bits64()) continue;
            for (u64 misalign : {u64{1}, u64{8}}) {
                SCOPED_TRACE(::testing::Message() << f.name << " +" << misalign);
                fill_data(rig);
                const std::vector<u8> before = read_data(rig);
                CpuState init = rig.base();
                set_ea(init, rig.bits64(), w::kData + misalign);

                CpuState interpreted = init;
                ASSERT_TRUE(rig.mem().write_bytes(w::kCode, f.code.data(),
                                                  f.code.size()));
                Interpreter interpreter(interpreted, rig.mem());
                ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
                {
                    SCOPED_TRACE("interpreter");
                    const ExecContext& ctx = interpreter.context();
                    EXPECT_EQ(cpu_exception_to_status(ctx.exception),
                              kAccessViolation);
                    EXPECT_EQ(ctx.exception_error, 0u);
                    EXPECT_EQ(ctx.exception_addr, all_ones);
                    expect_untouched(interpreted, init);
                    EXPECT_EQ(read_data(rig), before);
                }

                for (bool trailing : {false, true}) {
                    SCOPED_TRACE(trailing ? "jit, mov eax follows" : "jit");
                    std::vector<u8> block = f.code;
                    if (trailing) {
                        block.insert(block.end(), kTrailingMovEax.begin(),
                                     kTrailingMovEax.end());
                    }
                    const w::JitRun r = rig.jit(block, init);
                    ASSERT_TRUE(r.compiled);
                    EXPECT_TRUE(r.cpu.pending_raise.active)
                        << "jit result " << static_cast<int>(r.result);
                    if (!r.cpu.pending_raise.active) continue;
                    EXPECT_EQ(r.cpu.pending_raise.code, kAccessViolation);
                    ASSERT_EQ(r.cpu.pending_raise.num_args, 2u);
                    EXPECT_EQ(r.cpu.pending_raise.args[0], 0u);
                    EXPECT_EQ(r.cpu.pending_raise.args[1], all_ones)
                        << std::hex << "got 0x" << r.cpu.pending_raise.args[1];
                    EXPECT_EQ(r.cpu.rip, w::kCode);
                    EXPECT_EQ(r.cpu.gpr.get64(static_cast<u8>(RegIndex::RAX)),
                              init.gpr.get64(static_cast<u8>(RegIndex::RAX)));
                    expect_untouched(r.cpu, init);
                    EXPECT_EQ(read_data(rig), before);
                }
            }
        }
    });
}

namespace {

/// Encodings the SDM leaves undefined: VEX.vvvv != 1111b on the unary moves,
/// and VEX.pp = F3/F2 on 0F 28/29. `ok` is the same instruction with the
/// field corrected, which must still decode.
struct UdForm {
    const char* name;
    std::vector<u8> bad;
    std::vector<u8> ok;
    Opcode ok_opcode;
};

const std::vector<UdForm>& ud_forms() {
    static const std::vector<UdForm> f{
        // vvvv = 1101b (byte 0x68 / 0x69 / 0x6A ...), the matrix's [nds] forms.
        {"vmovaps nds", {0xC4, 0xE1, 0x68, 0x28, 0x03}, {0xC4, 0xE1, 0x78, 0x28, 0x03},
         Opcode::MOVAPS_XMM_XMMM128},
        {"vmovaps nds (C5)", {0xC5, 0xE8, 0x28, 0x03}, {0xC5, 0xF8, 0x28, 0x03},
         Opcode::MOVAPS_XMM_XMMM128},
        {"vmovaps store nds", {0xC5, 0xE8, 0x29, 0x03}, {0xC5, 0xF8, 0x29, 0x03},
         Opcode::MOVAPS_XMMM128_XMM},
        {"vmovapd nds", {0xC4, 0xE1, 0x69, 0x28, 0x03}, {0xC4, 0xE1, 0x79, 0x28, 0x03},
         Opcode::MOVAPD_XMM_XMMM128},
        {"vmovapd store nds", {0xC5, 0xE9, 0x29, 0x03}, {0xC5, 0xF9, 0x29, 0x03},
         Opcode::MOVAPD_XMMM128_XMM},
        {"vmovdqa nds", {0xC4, 0xE1, 0x69, 0x6F, 0x03}, {0xC4, 0xE1, 0x79, 0x6F, 0x03},
         Opcode::MOVDQA_XMM_XMMM128},
        {"vmovdqa store nds", {0xC5, 0xE9, 0x7F, 0x03}, {0xC5, 0xF9, 0x7F, 0x03},
         Opcode::MOVDQA_XMMM128_XMM},
        {"vmovdqa reg nds", {0xC5, 0xE9, 0x6F, 0xC1}, {0xC5, 0xF9, 0x6F, 0xC1},
         Opcode::MOVDQA_XMM_XMMM128},
        {"vmovaps reg nds ymm", {0xC5, 0xEC, 0x28, 0xC1}, {0xC5, 0xFC, 0x28, 0xC1},
         Opcode::MOVAPS_XMM_XMMM128},
        // The unaligned twins carry the same reserved-vvvv note.
        {"vmovups nds", {0xC5, 0xE8, 0x10, 0x03}, {0xC5, 0xF8, 0x10, 0x03},
         Opcode::MOVUPS_XMM_XMMM128},
        {"vmovups store nds", {0xC5, 0xE8, 0x11, 0x03}, {0xC5, 0xF8, 0x11, 0x03},
         Opcode::MOVUPS_XMMM128_XMM},
        {"vmovupd nds", {0xC5, 0xE9, 0x10, 0x03}, {0xC5, 0xF9, 0x10, 0x03},
         Opcode::MOVUPD_XMM_XMMM128},
        {"vmovupd store nds", {0xC5, 0xE9, 0x11, 0x03}, {0xC5, 0xF9, 0x11, 0x03},
         Opcode::MOVUPD_XMMM128_XMM},
        {"vmovdqu nds", {0xC5, 0xEA, 0x6F, 0x03}, {0xC5, 0xFA, 0x6F, 0x03},
         Opcode::MOVDQU_XMM_XMMM128},
        {"vmovdqu store nds", {0xC5, 0xEA, 0x7F, 0x03}, {0xC5, 0xFA, 0x7F, 0x03},
         Opcode::MOVDQU_XMMM128_XMM},
        // VEX.pp = F3 / F2 on 0F 28/29: the matrix's [rep] forms.
        {"vex.f3 0f 28", {0xC4, 0xE1, 0x7A, 0x28, 0x03}, {0xC4, 0xE1, 0x78, 0x28, 0x03},
         Opcode::MOVAPS_XMM_XMMM128},
        {"vex.f3 0f 28 nds", {0xC4, 0xE1, 0x6A, 0x28, 0x03}, {0xC4, 0xE1, 0x78, 0x28, 0x03},
         Opcode::MOVAPS_XMM_XMMM128},
        {"vex.f2 0f 28", {0xC5, 0xFB, 0x28, 0x03}, {0xC5, 0xF8, 0x28, 0x03},
         Opcode::MOVAPS_XMM_XMMM128},
        {"vex.f3 0f 29", {0xC5, 0xFA, 0x29, 0x03}, {0xC5, 0xF8, 0x29, 0x03},
         Opcode::MOVAPS_XMMM128_XMM},
        {"vex.f2 0f 29", {0xC5, 0xFB, 0x29, 0x03}, {0xC5, 0xF8, 0x29, 0x03},
         Opcode::MOVAPS_XMMM128_XMM},
        {"vex.f3 0f 28 reg", {0xC5, 0xFA, 0x28, 0xC1}, {0xC5, 0xF8, 0x28, 0xC1},
         Opcode::MOVAPS_XMM_XMMM128},
    };
    return f;
}

DecodeError decode_bytes(const std::vector<u8>& code, DecodeMode mode,
                         DecodedInstruction& out) {
    MemoryByteReader reader(code.data(), code.size());
    return decode_instruction(reader, mode, out);
}

}  // namespace

TEST(VexAlignedMoveContract, UndefinedEncodingsAreUd) {
    for (DecodeMode mode : {DecodeMode::Mode32, DecodeMode::Mode64}) {
        for (const UdForm& f : ud_forms()) {
            SCOPED_TRACE(::testing::Message()
                         << f.name << (mode == DecodeMode::Mode64 ? " /64" : " /32"));
            DecodedInstruction in;
            EXPECT_EQ(decode_bytes(f.bad, mode, in), DecodeError::InvalidOpcode);
            ASSERT_EQ(decode_bytes(f.ok, mode, in), DecodeError::Success);
            EXPECT_EQ(in.opcode, f.ok_opcode);
        }
    }
}

TEST(VexAlignedMoveContract, UndefinedEncodingsRaiseIllegalInstruction) {
    // The interpreter is what runs an encoding the decoder refuses (the JIT
    // block builder stops before it): STATUS_ILLEGAL_INSTRUCTION, at the
    // instruction, with nothing committed -- aligned or not.
    for_each_cfg([](w::CryptoRig& rig) {
        for (const UdForm& f : ud_forms()) {
            for (u64 off : {u64{0}, u64{1}}) {
                SCOPED_TRACE(::testing::Message() << f.name << " +" << off);
                fill_data(rig);
                const std::vector<u8> before = read_data(rig);
                CpuState init = rig.base();
                // The [rbx] operand (rm=011): point it at kData.
                if (rig.bits64()) init.gpr.set64(static_cast<u8>(RegIndex::RBX), w::kData + off);
                else init.gpr.set32(static_cast<u8>(RegIndex::RBX),
                                    static_cast<u32>(w::kData + off));
                CpuState cpu = init;
                ASSERT_TRUE(rig.mem().write_bytes(w::kCode, f.bad.data(),
                                                  f.bad.size()));
                Interpreter interpreter(cpu, rig.mem());
                ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
                EXPECT_EQ(cpu_exception_to_status(interpreter.context().exception),
                          kIllegalInstruction);
                EXPECT_EQ(cpu.rip, w::kCode);
                expect_untouched(cpu, init);
                EXPECT_EQ(read_data(rig), before);
            }
        }
    });
}
