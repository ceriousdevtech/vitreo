// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
// Faulting 16- and 32-byte vector stores, interpreter vs JIT, shared by the
// 32-bit, 64-bit and 32-bit-linear suites.
//
// Expected values come from Windows, not from either engine:
// tools/oracle_probe/simd_store_fault_probe.expected (x64 and x86, every
// straddling offset, second page NOACCESS or READONLY) records a write
// access violation at the FIRST INACCESSIBLE BYTE and no byte of the store
// changed, for MOVUPS/MOVDQU/VMOVUPS xmm/VEXTRACTF128 and 32-byte
// VMOVUPS/VMOVDQU. Intel SDM Vol. 3A 6.15: a faulting store does not
// complete. Every case also diffs the JIT against the interpreter.

#include <gtest/gtest.h>
#include <functional>
#include <string>
#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "jit_dispatch_execute.hpp"

namespace vitreo::cpu::test_simd_store {

/// One store form, `[eax]`/`[rax]` destination, source register 1.
struct StoreForm {
    const char* name;
    std::vector<u8> bytes;
    u32 width;
    u32 align;      ///< architectural alignment requirement, 0 = none
    bool from_high; ///< stores ymm1[255:128] (VEXTRACT imm8[0] = 1)
};

inline const std::vector<StoreForm>& store_forms() {
    static const std::vector<StoreForm> forms = {
        {"movups", {0x0F, 0x11, 0x08}, 16, 0, false},
        {"movupd", {0x66, 0x0F, 0x11, 0x08}, 16, 0, false},
        {"movdqu", {0xF3, 0x0F, 0x7F, 0x08}, 16, 0, false},
        {"movaps", {0x0F, 0x29, 0x08}, 16, 16, false},
        {"movdqa", {0x66, 0x0F, 0x7F, 0x08}, 16, 16, false},
        {"movntps", {0x0F, 0x2B, 0x08}, 16, 16, false},
        {"movntdq", {0x66, 0x0F, 0xE7, 0x08}, 16, 16, false},
        {"vmovups xmm", {0xC5, 0xF8, 0x11, 0x08}, 16, 0, false},
        {"vmovdqu xmm", {0xC5, 0xFA, 0x7F, 0x08}, 16, 0, false},
        {"vextractf128 hi", {0xC4, 0xE3, 0x7D, 0x19, 0x08, 0x01}, 16, 0, true},
        {"vextracti128 lo", {0xC4, 0xE3, 0x7D, 0x39, 0x08, 0x00}, 16, 0, false},
        {"vmovups ymm", {0xC5, 0xFC, 0x11, 0x08}, 32, 0, false},
        {"vmovdqu ymm", {0xC5, 0xFE, 0x7F, 0x08}, 32, 0, false},
        {"vmovaps ymm", {0xC5, 0xFC, 0x29, 0x08}, 32, 32, false},
        {"vmovntdq ymm", {0xC5, 0xFD, 0xE7, 0x08}, 32, 32, false},
    };
    return forms;
}

/// Effective addresses relative to a boundary `b` whose next page is denied,
/// with the address Windows reports (0 = no fault).
struct EaCase {
    GuestAddr ea;
    GuestAddr fault;
};

inline std::vector<EaCase> ea_cases(const StoreForm& f, GuestAddr b) {
    std::vector<EaCase> v;
    const u32 w = f.width;
    v.push_back({b - w, 0});                    // last fitting slot
    v.push_back({b - 2 * w, 0});
    v.push_back({b, b});                        // wholly in the denied page
    if (f.align == 0) {
        v.push_back({b - w - 5, 0});            // unaligned, fits
        v.push_back({b + 4, b + 4});            // unaligned, wholly denied
        for (u32 off = 1; off < w; off += (w == 16 ? 7 : 5)) {
            v.push_back({b - off, b});          // straddles: first page ok
        }
        v.push_back({b - (w - 1), b});
        if (w == 32) v.push_back({b - 16, b});  // second half denied exactly
    } else if (f.align == 16 && w == 16) {
        v.push_back({b + 16, b + 16});
    }
    return v;
}

struct Env {
    GuestMemory& mem;
    GuestAddr code;
    bool is64;
    std::function<BasicBlock(const std::vector<u8>&)> build;
    std::function<void(CpuState&)> init;
    std::function<JitExecResult(const BasicBlock&, CpuState&)> run_jit;
};

inline u8 seed_byte(int reg, bool high, int b) {
    return static_cast<u8>((high ? 0x80 : 0x00) + 0x10 * (reg & 7) + b + 1);
}

/// Each byte's value, or -1 when the guest cannot read it.
inline std::vector<int> peek(GuestMemory& m, GuestAddr a, usize n) {
    std::vector<int> out(n, -1);
    for (usize i = 0; i < n; ++i) {
        u8 b = 0;
        if (m.read_bytes(a + i, &b, 1).is_ok()) out[i] = b;
    }
    return out;
}

inline void poke(GuestMemory& m, GuestAddr a, const std::vector<u8>& v) {
    for (usize i = 0; i < v.size(); ++i) (void)m.write_bytes(a + i, &v[i], 1);
}

inline void run_store_case(const Env& env, const StoreForm& f,
                           const EaCase& c) {
    SCOPED_TRACE(::testing::Message()
                 << (env.is64 ? "x64 " : "x86 ") << f.name << " ea=0x"
                 << std::hex << c.ea);
    // CMP sets flags that are live across the store; the fault must
    // report them unchanged.
    std::vector<u8> code = {0x39, 0xC1};  // cmp ecx, eax
    code.insert(code.end(), f.bytes.begin(), f.bytes.end());
    ASSERT_TRUE(env.mem.write_bytes(env.code, code.data(), code.size()));
    const BasicBlock block = env.build(code);
    ASSERT_EQ(block.instruction_count, 2u);

    constexpr usize kPad = 48;
    const GuestAddr lo = c.ea - kPad;
    const usize n = 2 * kPad + f.width;
    std::vector<u8> fill(n);
    for (usize i = 0; i < n; ++i) fill[i] = static_cast<u8>(0x5A ^ i);
    poke(env.mem, lo, fill);
    const std::vector<int> before = peek(env.mem, lo, n);

    auto seed = [&](CpuState& cpu) {
        env.init(cpu);
        for (int r = 0; r < 16; ++r) {
            for (int b = 0; b < 16; ++b) {
                cpu.sse.xmm[r].u8_[b] = seed_byte(r, false, b);
                cpu.avx.ymm_high[r].u8_[b] = seed_byte(r, true, b);
            }
        }
        cpu.gpr.set64(static_cast<u8>(RegIndex::RAX), c.ea);
        cpu.gpr.set64(static_cast<u8>(RegIndex::RCX), 5);
        cpu.flags.rflags = 0x202;
    };

    CpuState icpu;
    seed(icpu);
    ExecResult ires = ExecResult::Continue;
    u32 istatus = 0, ierror = 0;
    u64 iaddr = 0;
    {
        Interpreter interp(icpu, env.mem);
        ASSERT_EQ(interp.step(), ExecResult::Continue);
        ires = interp.step();
        if (ires == ExecResult::Exception) {
            istatus = cpu_exception_to_status(interp.context().exception);
            ierror = interp.context().exception_error;
            iaddr = interp.context().exception_addr;
        }
    }
    const std::vector<int> after_interp = peek(env.mem, lo, n);
    poke(env.mem, lo, fill);

    CpuState jcpu;
    seed(jcpu);
    ASSERT_EQ(env.run_jit(block, jcpu), JitExecResult::Continue);
    const std::vector<int> after_jit = peek(env.mem, lo, n);

    // ---- Windows (oracle) ----
    if (c.fault != 0) {
        ASSERT_EQ(ires, ExecResult::Exception);
        EXPECT_EQ(istatus, 0xC0000005u);
        EXPECT_EQ(ierror, 1u) << "a store is a write";
        EXPECT_EQ(iaddr, c.fault) << "first inaccessible byte";
        EXPECT_EQ(after_interp, before) << "interpreter wrote part of a faulting store";
    } else {
        ASSERT_EQ(ires, ExecResult::Continue);
        for (u32 b = 0; b < f.width; ++b) {
            const bool high = f.from_high || b >= 16;
            EXPECT_EQ(after_interp[kPad + b], seed_byte(1, high, b & 15)) << "byte " << b;
        }
    }

    // ---- JIT against the interpreter ----
    EXPECT_EQ(after_jit, after_interp) << "memory differs";
    EXPECT_EQ(jcpu.pending_raise.active, ires == ExecResult::Exception);
    if (ires == ExecResult::Exception && jcpu.pending_raise.active) {
        EXPECT_EQ(jcpu.pending_raise.code, istatus);
        EXPECT_EQ(jcpu.pending_raise.num_args, 2u);
        EXPECT_EQ(jcpu.pending_raise.args[0], ierror);
        EXPECT_EQ(jcpu.pending_raise.args[1], iaddr);
    }
    EXPECT_EQ(jcpu.rip, icpu.rip);
    for (u8 r = 0; r < (env.is64 ? 16 : 8); ++r) {
        EXPECT_EQ(jcpu.gpr.get64(r), icpu.gpr.get64(r)) << "gpr " << +r;
    }
    for (int r = 0; r < 16; ++r) {
        for (int b = 0; b < 16; ++b) {
            EXPECT_EQ(jcpu.sse.xmm[r].u8_[b], icpu.sse.xmm[r].u8_[b]);
            EXPECT_EQ(jcpu.avx.ymm_high[r].u8_[b], icpu.avx.ymm_high[r].u8_[b]);
        }
    }
    jcpu.materialize_lazy_flags();
    icpu.materialize_lazy_flags();
    constexpr u64 kStatus = 0x8D5;  // CF PF AF ZF SF OF
    EXPECT_EQ(jcpu.flags.rflags & kStatus, icpu.flags.rflags & kStatus);
    poke(env.mem, lo, fill);
}

/// Every form at every EA case against boundary @p b.
inline void run_all_forms(const Env& env, GuestAddr b) {
    for (const StoreForm& f : store_forms()) {
        for (const EaCase& c : ea_cases(f, b)) {
            run_store_case(env, f, c);
            if (::testing::Test::HasFatalFailure()) return;
        }
    }
}

}  // namespace vitreo::cpu::test_simd_store
