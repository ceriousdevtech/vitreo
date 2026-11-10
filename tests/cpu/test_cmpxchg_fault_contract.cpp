// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CMPXCHG / CMPXCHG8B / CMPXCHG16B memory-fault contract, interpreter and JIT.
//
// Measured on Windows 11 x64 and x86 (tests/oracle_data/
// cmpxchg_fault_probe.expected): every CMPXCHG m8/m16/m32/m64, CMPXCHG8B and
// CMPXCHG16B, with and without LOCK, reports a faulting destination as
// STATUS_ACCESS_VIOLATION with ExceptionInformation[0] = 1 (WRITE) -- for an
// uncommitted or PAGE_NOACCESS page as well as for a PAGE_READONLY one, and on
// a read-only page whether the compare matches or not. [1] is the first
// inaccessible byte: the denied page's first byte when the operand starts in
// a writable page. The accumulator, the source registers, ZF (and the other
// flags) and memory are all left as they were.
//
// The expected values below come from that recording, not from either engine.

#include <gtest/gtest.h>

#include <cstring>
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
constexpr u8 kRcx = static_cast<u8>(RegIndex::RCX);
constexpr u8 kRdx = static_cast<u8>(RegIndex::RDX);
constexpr u8 kRbx = static_cast<u8>(RegIndex::RBX);
constexpr u8 kRsi = static_cast<u8>(RegIndex::RSI);

// The rig maps [0x1000, 0x11000) read/write. The page at kRoPage is made
// PAGE_READONLY; 0x11000 and up is unmapped.
constexpr u64 kRoPage = 0x8000;
constexpr u64 kEnd = w::kCode + 0x10000;

struct Form {
    const char* name;
    std::vector<u8> code;  // [rsi] operand; source register ECX/CL (or ECX:EBX)
    u8 width;              // destination bytes
    bool only64;
    bool pair;             // CMPXCHG8B/16B: EDX:EAX vs m, ECX:EBX stored
};

const std::vector<Form>& forms() {
    static const std::vector<Form> f{
        {"cmpxchg [rsi],cl", {0x0F, 0xB0, 0x0E}, 1, false, false},
        {"lock cmpxchg [rsi],cl", {0xF0, 0x0F, 0xB0, 0x0E}, 1, false, false},
        {"cmpxchg [rsi],cx", {0x66, 0x0F, 0xB1, 0x0E}, 2, false, false},
        {"lock cmpxchg [rsi],cx", {0xF0, 0x66, 0x0F, 0xB1, 0x0E}, 2, false, false},
        {"cmpxchg [rsi],ecx", {0x0F, 0xB1, 0x0E}, 4, false, false},
        {"lock cmpxchg [rsi],ecx", {0xF0, 0x0F, 0xB1, 0x0E}, 4, false, false},
        {"cmpxchg [rsi],rcx", {0x48, 0x0F, 0xB1, 0x0E}, 8, true, false},
        {"lock cmpxchg [rsi],rcx", {0xF0, 0x48, 0x0F, 0xB1, 0x0E}, 8, true, false},
        {"cmpxchg8b [rsi]", {0x0F, 0xC7, 0x0E}, 8, false, true},
        {"lock cmpxchg8b [rsi]", {0xF0, 0x0F, 0xC7, 0x0E}, 8, false, true},
        {"cmpxchg16b [rsi]", {0x48, 0x0F, 0xC7, 0x0E}, 16, true, true},
        {"lock cmpxchg16b [rsi]", {0xF0, 0x48, 0x0F, 0xC7, 0x0E}, 16, true, true},
    };
    return f;
}

/// `mov eax, 0x12345678`: a successor the JIT block must not run.
const std::vector<u8> kTrailingMovEax{0xB8, 0x78, 0x56, 0x34, 0x12};

bool init_rig(w::CryptoRig& rig, w::MemCfg cfg) {
    if (!rig.init(cfg)) return false;
    // Seed a pattern everywhere the operands land, then deny writes.
    std::vector<u8> pattern(0x2000);
    for (usize i = 0; i < pattern.size(); ++i)
        pattern[i] = static_cast<u8>(0x31 + i * 7);
    EXPECT_TRUE(rig.mem().write_bytes(kRoPage - 0x1000, pattern.data(),
                                      pattern.size()));
    EXPECT_TRUE(rig.mem().write_bytes(kEnd - 0x1000, pattern.data(), 0x1000));
    EXPECT_TRUE(rig.mem().protect_region(kRoPage, 0x1000, MemoryProtection::Read));
    return true;
}

u64 read_le(w::CryptoRig& rig, u64 at, u8 n) {
    u64 v = 0;
    for (u8 i = 0; i < n && i < 8; ++i) {
        auto b = rig.mem().read_u8(at + i);
        if (!b) return 0;
        v |= static_cast<u64>(b.value()) << (8 * i);
    }
    return v;
}

/// Registers for one form at @p ea: the accumulator equals the destination
/// (@p match) or differs from it in every byte.
CpuState setup(w::CryptoRig& rig, const Form& f, u64 ea, bool match) {
    CpuState cpu = rig.base();
    const bool b64 = rig.bits64();
    const u8 half = f.pair ? static_cast<u8>(f.width / 2) : f.width;
    u64 lo = read_le(rig, ea, half);
    u64 hi = f.pair ? read_le(rig, ea + half, half) : 0;
    if (!match) { lo = ~lo; hi = ~hi; }
    auto set = [&](u8 r, u64 v) {
        if (b64) cpu.gpr.set64(r, v); else cpu.gpr.set32(r, static_cast<u32>(v));
    };
    // Only the low `half` bytes are compared; the rest stay distinctive.
    const u64 mask = half >= 8 ? ~u64{0} : ((u64{1} << (8 * half)) - 1);
    set(kRax, (0x1122334455667788ull & ~mask) | (lo & mask));
    set(kRdx, f.pair ? ((0x0BADF00D0BADF00Dull & ~mask) | (hi & mask))
                     : 0x0BADF00D0BADF00Dull);
    set(kRcx, 0x5A5A5A5A5A5A5A5Aull);
    set(kRbx, 0x6B6B6B6B6B6B6B6Bull);
    set(kRsi, ea);
    return cpu;
}

void expect_untouched(const CpuState& got, const CpuState& init) {
    for (u8 r : {kRax, kRcx, kRdx, kRbx, kRsi}) {
        SCOPED_TRACE(::testing::Message() << "gpr" << int(r));
        EXPECT_EQ(got.gpr.get64(r), init.gpr.get64(r));
    }
    CpuState g = got;
    g.materialize_lazy_flags();
    EXPECT_EQ(g.flags.rflags, w::kFlags) << std::hex << g.flags.rflags;
}

/// The JIT as production runs it: native or helper lowering, and the
/// per-instruction interpreter delegate for a form with no lowering
/// (unlocked CMPXCHG m8).
w::JitRun run_jit(w::CryptoRig& rig, const std::vector<u8>& code,
                  const CpuState& init) {
    w::JitRun r = rig.jit(code, init);
    if (!r.compiled) r = rig.jit(code, init, /*allow_delegate=*/true);
    return r;
}

std::vector<u8> snapshot(w::CryptoRig& rig, u64 ea) {
    std::vector<u8> out(16, 0);
    for (int i = 0; i < 16; ++i) {
        auto b = rig.mem().read_u8(ea + i);
        out[i] = b ? b.value() : 0;
    }
    return out;
}

void expect_write_fault(w::CryptoRig& rig, const Form& f, u64 ea, bool match,
                        u64 fault_va) {
    const bool b64 = rig.bits64();
    const u64 expected = b64 ? fault_va : (fault_va & 0xFFFFFFFFull);
    const CpuState init = setup(rig, f, ea, match);
    const std::vector<u8> before = snapshot(rig, ea);

    CpuState interpreted = init;
    EXPECT_TRUE(rig.mem().write_bytes(w::kCode, f.code.data(), f.code.size()));
    Interpreter interpreter(interpreted, rig.mem());
    {
        SCOPED_TRACE("interpreter");
        ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
        const ExecContext& ctx = interpreter.context();
        EXPECT_EQ(cpu_exception_to_status(ctx.exception), kAccessViolation);
        EXPECT_EQ(ctx.exception_error, 1u);
        const u64 addr = b64 ? ctx.exception_addr
                             : (ctx.exception_addr & 0xFFFFFFFFull);
        EXPECT_EQ(addr, expected) << std::hex << "got 0x" << addr;
        expect_untouched(interpreted, init);
        EXPECT_EQ(snapshot(rig, ea), before);
    }

    for (bool trailing : {false, true}) {
        SCOPED_TRACE(trailing ? "jit, mov eax follows" : "jit");
        std::vector<u8> block = f.code;
        if (trailing)
            block.insert(block.end(), kTrailingMovEax.begin(),
                         kTrailingMovEax.end());
        const w::JitRun r = run_jit(rig, block, init);
        ASSERT_TRUE(r.compiled);
        ASSERT_TRUE(r.cpu.pending_raise.active)
            << "jit result " << static_cast<int>(r.result);
        EXPECT_EQ(r.cpu.pending_raise.code, kAccessViolation);
        ASSERT_EQ(r.cpu.pending_raise.num_args, 2u);
        EXPECT_EQ(r.cpu.pending_raise.args[0], 1u);
        EXPECT_EQ(r.cpu.pending_raise.args[1], expected)
            << std::hex << "got 0x" << r.cpu.pending_raise.args[1];
        EXPECT_EQ(r.cpu.rip, w::kCode) << std::hex << "got 0x" << r.cpu.rip;
        expect_untouched(r.cpu, init);
        EXPECT_EQ(snapshot(rig, ea), before);
    }
}

template <typename Fn>
void for_each_form(Fn&& fn) {
    for (w::MemCfg cfg : w::kAllCfgs) {
        w::CryptoRig rig;
        if (!init_rig(rig, cfg)) continue;
        SCOPED_TRACE(w::cfg_name(cfg));
        for (const Form& f : forms()) {
            if (f.only64 && !rig.bits64()) continue;
            SCOPED_TRACE(f.name);
            fn(rig, f);
        }
    }
}

}  // namespace

TEST(CmpxchgFaultContract, UnmappedDestinationIsWriteAccessViolation) {
    for_each_form([](w::CryptoRig& rig, const Form& f) {
        expect_write_fault(rig, f, w::kUnmapped, /*match=*/true, w::kUnmapped);
    });
}

TEST(CmpxchgFaultContract, ReadOnlyDestinationFaultsWhetherOrNotItMatches) {
    for_each_form([](w::CryptoRig& rig, const Form& f) {
        for (bool match : {true, false}) {
            SCOPED_TRACE(match ? "match" : "no match");
            expect_write_fault(rig, f, kRoPage + 0x40, match, kRoPage + 0x40);
        }
    });
}

TEST(CmpxchgFaultContract, StraddleReportsFirstInaccessibleByte) {
    // CMPXCHG16B needs 16-byte alignment, so it cannot straddle.
    for_each_form([](w::CryptoRig& rig, const Form& f) {
        if (f.width == 1 || f.width == 16) return;
        for (bool match : {true, false}) {
            SCOPED_TRACE(match ? "into read-only, match"
                               : "into read-only, no match");
            expect_write_fault(rig, f, kRoPage - 1, match, kRoPage);
        }
        SCOPED_TRACE("into unmapped");
        expect_write_fault(rig, f, kEnd - 1, /*match=*/true, kEnd);
    });
}

TEST(CmpxchgFaultContract, WritableDestinationParity) {
    // Positive control: both engines agree on registers, flags and memory.
    for_each_form([](w::CryptoRig& rig, const Form& f) {
        for (bool match : {true, false}) {
            SCOPED_TRACE(match ? "match" : "no match");
            const u64 ea = w::kData + 0x40;
            const std::vector<u8> seed = snapshot(rig, ea);
            const CpuState init = setup(rig, f, ea, match);
            CpuState interpreted = init;
            ASSERT_EQ(rig.interp(f.code, interpreted), ExecResult::Continue);
            const std::vector<u8> after_interp = snapshot(rig, ea);
            ASSERT_TRUE(rig.mem().write_bytes(ea, seed.data(), seed.size()));
            const w::JitRun r = run_jit(rig, f.code, init);
            ASSERT_TRUE(r.compiled);
            EXPECT_FALSE(r.cpu.pending_raise.active);
            EXPECT_EQ(snapshot(rig, ea), after_interp);
            EXPECT_EQ(after_interp != seed, match);
            for (u8 reg : {kRax, kRcx, kRdx, kRbx, kRsi})
                EXPECT_EQ(r.cpu.gpr.get64(reg), interpreted.gpr.get64(reg))
                    << "gpr" << int(reg);
            CpuState a = interpreted, b = r.cpu;
            a.materialize_lazy_flags();
            b.materialize_lazy_flags();
            EXPECT_EQ(a.flags.rflags, b.flags.rflags);
            EXPECT_EQ(((a.flags.rflags >> 6) & 1) != 0, match);
            ASSERT_TRUE(rig.mem().write_bytes(ea, seed.data(), seed.size()));
        }
    });
}

TEST(CmpxchgFaultContract, MisalignedCmpxchg16bIsGpWithNoFaultAddress) {
    // #GP(0): Windows reports [0] = 0, [1] = 0xFFFFFFFFFFFFFFFF, plain and
    // LOCK alike (cmpxchg16b_align_probe.expected).
    for_each_form([](w::CryptoRig& rig, const Form& f) {
        if (f.width != 16) return;
        for (u64 ea : {u64{w::kData + 8}, u64{w::kData + 1}}) {
            SCOPED_TRACE(::testing::Message() << std::hex << "ea=" << ea);
            const CpuState init = setup(rig, f, ea, /*match=*/true);
            const std::vector<u8> before = snapshot(rig, ea);
            CpuState interpreted = init;
            ASSERT_TRUE(rig.mem().write_bytes(w::kCode, f.code.data(),
                                              f.code.size()));
            Interpreter interpreter(interpreted, rig.mem());
            ASSERT_EQ(interpreter.run(1), ExecResult::Exception);
            EXPECT_EQ(cpu_exception_to_status(interpreter.context().exception),
                      kAccessViolation);
            EXPECT_EQ(interpreter.context().exception_error, 0u);
            EXPECT_EQ(interpreter.context().exception_addr, ~u64{0});
            expect_untouched(interpreted, init);
            const w::JitRun r = run_jit(rig, f.code, init);
            ASSERT_TRUE(r.compiled);
            ASSERT_TRUE(r.cpu.pending_raise.active);
            EXPECT_EQ(r.cpu.pending_raise.args[0], 0u);
            EXPECT_EQ(r.cpu.pending_raise.args[1], ~u64{0});
            expect_untouched(r.cpu, init);
            EXPECT_EQ(snapshot(rig, ea), before);
        }
    });
}
