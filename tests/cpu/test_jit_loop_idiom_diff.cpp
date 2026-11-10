// SPDX-License-Identifier: LGPL-2.1-only
// WS2: interpreter / JIT(off) / JIT(on) differential for every recognised
// loop shape, including faults, page crossings, overlap, DF, TSO mode and
// Tier 1. JIT(on) vs JIT(off) must be identical (registers, flags incl. AF,
// XMM, memory, exception record); both must match the interpreter.

#include "test_jit_loop_idiom_fixture.hpp"

#include "core/cpu/jit/loop_idiom_gate.hpp"
#include "core/cpu/jit/loop_idiom_thresholds.hpp"

#include <cstdlib>
#include <optional>
#include <string>

using namespace vitreo;
using namespace vitreo::cpu;
using namespace vitreo::cpu::jit;
using namespace vitreo::cpu::jit::loop_idiom;
using namespace vitreo::cpu::jit::loop_idiom::test;
using namespace vitreo::cpu::jit::loop_idiom::test::catalogue;

namespace {

constexpr u8 EAX = 0, ECX = 1, EDX = 2, ESI = 6, EDI = 7;
constexpr u64 SRC = 0x20100;
constexpr u64 DST = 0x40000;

bool ws2_env_off() {
    const char* v = std::getenv("VITREO_WS2");
    return v != nullptr && v[0] == '0' && v[1] == '\0';
}

class ScopedEnv {
public:
    ScopedEnv(const char* n, const char* v) : m_name(n) {
        if (const char* p = std::getenv(n)) m_prev = p;
        setenv(n, v, 1);
    }
    ~ScopedEnv() {
        if (m_prev) setenv(m_name.c_str(), m_prev->c_str(), 1);
        else unsetenv(m_name.c_str());
    }
private:
    std::string m_name;
    std::optional<std::string> m_prev;
};

/// default_data plus explicit bytes.
MemSetup with_bytes(std::vector<std::pair<u64, std::vector<u8>>> patches) {
    return [patches](GuestMemory& mem) {
        default_data(mem);
        for (const auto& [addr, bytes] : patches) {
            ASSERT_TRUE(mem.write_bytes(addr, bytes.data(), bytes.size()));
        }
    };
}

void set_xmm(CpuState& c, u8 r, u8 byte) {
    for (u8 i = 0; i < 16; ++i) c.sse.xmm[r].u8_[i] = static_cast<u8>(byte + i);
}

struct Pattern {
    const char* name;
    std::span<const u8> code;
    bool is64;
    u32 elem;
    std::function<void(CpuState&, u64 n)> setup;
};

/// Fill and copy loops parameterised by iteration count n.
std::vector<Pattern> fill_copy_patterns() {
    return {
        {"F1", kF1, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); c.gpr.set32(EAX, 0x3C); }},
        {"F2", kF2, false, 4, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(EAX, 0); c.gpr.set32(ECX, static_cast<u32>(n));
             c.gpr.set32(EDX, 0x11223344); }},
        {"F3", kF3, false, 2, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, static_cast<u32>(DST + 2 * n)); c.gpr.set32(ECX, static_cast<u32>(0 - n)); }},
        {"F4", kF4, true, 1, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, n); c.gpr.set64(ECX, 0x5A); }},
        {"F5", kF5, true, 8, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, n);
             c.gpr.set64(ESI, 0x0102030405060708ull); }},
        {"F6", kF6, true, 16, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n); set_xmm(c, 0, 0x90); }},
        {"F7", kF7, true, 16, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n); set_xmm(c, 0, 0xA0); }},
        {"F8", kF8, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); c.gpr.set32(EAX, 0x7712); }},
        {"F9", kF9, true, 4, [](CpuState& c, u64 n) {
             c.gpr.set64(EDI, DST); c.gpr.set64(EDX, 0xABCD000000000000ull | n); }},
        {"L1", kL1, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); c.gpr.set32(EAX, 0x61); }},
        {"G1", kG1, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(0x40 - n)); c.gpr.set32(EAX, 0x62); }},
        {"C1", kC1, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C2", kC2, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C3", kC3, false, 4, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(EAX, 0); c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C4", kC4, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, static_cast<u32>(SRC + n)); c.gpr.set32(EDI, static_cast<u32>(DST + n));
             c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C6", kC6, false, 1, [](CpuState& c, u64 n) {
             c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(n)); }},
        {"C7", kC7, true, 1, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC + n); c.gpr.set64(EDI, DST + n); c.gpr.set64(ECX, 0 - n); }},
        {"C8", kC8, true, 8, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, n); }},
        {"C9", kC9, true, 16, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC + 4); c.gpr.set64(EDI, DST + 8); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n); }},
        {"C10", kC10, true, 16, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n); }},
        {"C11", kC11, true, 4, [](CpuState& c, u64 n) {
             c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(EDX, n); }},
    };
}

CpuSetup bind(const Pattern& p, u64 n) {
    return [p, n](CpuState& c) { p.setup(c, n); };
}

void skip_if_env_off() {
    if (ws2_env_off()) GTEST_SKIP() << "VITREO_WS2=0: bulk-path assertions do not apply";
}

}  // namespace

TEST(LoopIdiomDiff, FillAndCopyAtEveryWindowBoundary) {
    // Around the REP chunk (4095) and the 64 KiB per-call budget, and past
    // it (several helper calls, re-dispatch in between).
    const u64 counts[] = {1, 2, 3, 4, 15, 16, 17, 4094, 4095, 4096, 4097, 12291,
                          65535, 65536, 65537, 65538, 0x1E000};
    for (const Pattern& p : fill_copy_patterns()) {
        for (u64 n : counts) {
            if (n * p.elem > 0x1F000) continue;  // must fit the source/destination windows
            if (std::string(p.name) == "G1" && n > 0x40 + 0x7FFF) continue;
            SCOPED_TRACE(std::string(p.name) + " n=" + std::to_string(n));
            const Triple t = diff3(p.code, p.is64, default_data, bind(p, n));
            if (!ws2_env_off() && n >= 4) {
                EXPECT_TRUE(t.on.bulked_first_entry) << "bulk path did not run";
            }
        }
    }
}

TEST(LoopIdiomDiff, FaultAtFirstMiddleLastElement) {
    // Data RW 0x20000..0x30000; 0x30000 unmapped.
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        std::vector<u8> f(0x10000, 0x41);
        ASSERT_TRUE(m.write_bytes(0x20000, f.data(), f.size()));
    };
    for (u64 k : {1ull, 2000ull, 4096ull, 4097ull}) {
        SCOPED_TRACE("store fault at element " + std::to_string(k));
        diff3(kF1, false, mem, [k](CpuState& c) {
            c.gpr.set32(EDI, static_cast<u32>(0x30000 - (k - 1)));
            c.gpr.set32(ECX, 6000);
            c.gpr.set32(EAX, 0x33);
        });
        SCOPED_TRACE("load fault at element " + std::to_string(k));
        const Triple t = diff3(kC1, false, mem, [k](CpuState& c) {
            c.gpr.set32(ESI, static_cast<u32>(0x30000 - (k - 1)));
            c.gpr.set32(EDI, 0x20000);
            c.gpr.set32(ECX, 6000);
        });
        EXPECT_TRUE(t.on.faulted);
        EXPECT_EQ(t.on.fault_addr, 0x30000u);
        EXPECT_EQ(t.on.info0, 0u) << "a load is a READ fault";
    }
    SCOPED_TRACE("64-bit dword store fault");
    const Triple t = diff3(kC11, true, mem, [](CpuState& c) {
        c.gpr.set64(ESI, 0x20000);
        c.gpr.set64(EDI, 0x30000 - 4 * 1500);
        c.gpr.set64(EDX, 3000);
    });
    EXPECT_TRUE(t.on.faulted);
    EXPECT_EQ(t.on.info0, 1u) << "a store is a WRITE fault";
    EXPECT_EQ(t.on.info1, 0x30000u);
}

TEST(LoopIdiomDiff, PageCrossIntoReadOnly) {
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m.map_region(0x30000, 0x1000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m.protect_region(0x30000, 0x1000, MemoryProtection::Read));
    };
    const Triple t = diff3(kF1, false, mem, [](CpuState& c) {
        c.gpr.set32(EDI, 0x30000 - 300);
        c.gpr.set32(ECX, 1000);
        c.gpr.set32(EAX, 0x44);
    });
    EXPECT_TRUE(t.on.faulted);
    EXPECT_EQ(t.on.fault_addr, 0x30000u);
    if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
}

TEST(LoopIdiomDiff, PageCrossAdjacentRegions) {
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m.map_region(0x30000, 0x10000, MemoryProtection::ReadWrite));
    };
    const Triple t = diff3(kC1, false, mem, [](CpuState& c) {
        c.gpr.set32(ESI, 0x2FF00);
        c.gpr.set32(EDI, 0x30F00);
        c.gpr.set32(ECX, 3000);
    });
    EXPECT_FALSE(t.on.faulted);
    if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
}

TEST(LoopIdiomDiff, CopyOverlap) {
    {
        SCOPED_TRACE("forward overlap dst = src + 1 replicates a byte: declines");
        const Triple t = diff3(kC1, false, default_data, [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, SRC + 1); c.gpr.set32(ECX, 500);
        });
        EXPECT_FALSE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("forward overlap dst = src + 4, dword elements: declines");
        const Triple t = diff3(kC3, false, default_data, [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, SRC + 4); c.gpr.set32(EAX, 0); c.gpr.set32(ECX, 500);
        });
        EXPECT_FALSE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("forward copy with dst < src (memmove-down) bulks");
        const Triple t = diff3(kC1, false, default_data, [](CpuState& c) {
            c.gpr.set32(ESI, SRC + 3); c.gpr.set32(EDI, SRC); c.gpr.set32(ECX, 500);
        });
        if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("backward copy with dst = src + 1 bulks");
        const Triple t = diff3(kC4, false, default_data, [](CpuState& c) {
            c.gpr.set32(ESI, SRC + 600); c.gpr.set32(EDI, SRC + 601); c.gpr.set32(ECX, 500);
        });
        if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("in place");
        diff3(kC1, false, default_data, [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, SRC); c.gpr.set32(ECX, 500);
        });
    }
}

TEST(LoopIdiomDiff, DirectionFlagIgnored) {
    for (bool df : {false, true}) {
        SCOPED_TRACE(df ? "DF=1" : "DF=0");
        diff3(kC1, false, default_data, [df](CpuState& c) {
            c.flags.set_df(df);
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, 700);
        });
        diff3(kF1, false, default_data, [df](CpuState& c) {
            c.flags.set_df(df);
            c.gpr.set32(EDI, DST); c.gpr.set32(ECX, 700); c.gpr.set32(EAX, 1);
        });
    }
}

TEST(LoopIdiomDiff, StoreIntoExecutableOrCompiledCodeDeclines) {
    {
        SCOPED_TRACE("RWX destination");
        const MemSetup mem = [](GuestMemory& m) {
            ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWriteExecute));
        };
        const Triple t = diff3(kF1, false, mem, [](CpuState& c) {
            c.gpr.set32(EDI, 0x20100); c.gpr.set32(ECX, 600); c.gpr.set32(EAX, 0x90);
        });
        EXPECT_FALSE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("RW page that holds a compiled block");
        const MemSetup mem = [](GuestMemory& m) {
            default_data(m);
            m.retain_jit_code_page(DST);
        };
        const Triple t = diff3(kF1, false, mem, [](CpuState& c) {
            c.gpr.set32(EDI, DST + 0x100); c.gpr.set32(ECX, 600); c.gpr.set32(EAX, 0x90);
        });
        EXPECT_FALSE(t.on.bulked_first_entry);
    }
}

TEST(LoopIdiomDiff, AlignedVectorMisalignedRaisesInTheBody) {
    const Triple t = diff3(kC10, true, default_data, [](CpuState& c) {
        c.gpr.set64(ESI, SRC + 8); c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * 100);
    });
    EXPECT_TRUE(t.off.faulted);
    EXPECT_FALSE(t.on.bulked_first_entry);
    const Triple nt = diff3(kF7, true, default_data, [](CpuState& c) {
        c.gpr.set64(EDI, DST + 4); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * 100);
    });
    EXPECT_TRUE(nt.off.faulted) << "MOVNTDQ to a misaligned address is #GP";
}

TEST(LoopIdiomDiff, TsoOffMode) {
    ScopedEnv env("VITREO_JIT_NO_TSO", "1");
    for (const Pattern& p : fill_copy_patterns()) {
        for (u64 n : {5ull, 4097ull}) {
            if (n * p.elem > 0x1F000) continue;
            SCOPED_TRACE(std::string(p.name) + " n=" + std::to_string(n));
            const Triple t = diff3(p.code, p.is64, default_data, bind(p, n));
            if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
        }
    }
}

TEST(LoopIdiomDiff, TierOneAlsoTakesLoopIdiom) {
    // The hook sits ahead of ir_emit_block, so a block whose body Tier 1
    // lowers still gets the bulk prologue. Every 64-bit catalogue fill/copy
    // loop is compiled with the IR on first compile; at least one fill and
    // one copy must actually be lowered by Tier 1, bulk on first entry, and
    // match the interpreter and JIT(off) (diff3).
    // Unset VITREO_JIT_IR is Tier 1 for every 64-bit block (ir_gate_open);
    // only an explicit policy that excludes x64 turns it off.
    const char* ir_env = std::getenv("VITREO_JIT_IR");
    if (ir_env != nullptr && ir_env[0] != '\0' && !JitCompiler::ir_enabled_for(true)) {
        GTEST_SKIP() << "x64 IR disabled by VITREO_JIT_IR";
    }
    u32 fills = 0, copies = 0;
    for (const Pattern& p : fill_copy_patterns()) {
        if (!p.is64) continue;
        SCOPED_TRACE(p.name);
        const Triple t = diff3(p.code, p.is64, default_data, bind(p, 300),
                               Force::OnNoThreshold, /*tier1=*/true);
        if (!t.on.tier1_active) continue;
        if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
        if (p.name[0] == 'F') ++fills;
        if (p.name[0] == 'C') ++copies;
    }
    EXPECT_GE(fills, 1u) << "no 64-bit catalogue fill loop was lowered by Tier 1";
    EXPECT_GE(copies, 1u) << "no 64-bit catalogue copy loop was lowered by Tier 1";
}

TEST(LoopIdiomDiff, TailContinuesInBlockBudgetReDispatches) {
    // kRanTail: the exit follows the bulk, so the block runs the tail in
    // the same entry (no second dispatcher entry, no second helper call).
    // kRanRedispatch: past the 64 KiB budget the block returns to the
    // dispatcher and the next entry bulks again.
    skip_if_env_off();
    const Pattern p = fill_copy_patterns()[0];  // F1, byte fill
    ASSERT_STREQ(p.name, "F1");
    const u64 before = bulk_runs();
    const LoopOutcome small = run_jit(p.code, p.is64, default_data, bind(p, 4096), Force::On);
    EXPECT_TRUE(small.bulked_first_entry);
    EXPECT_EQ(small.entries, 1u);
    EXPECT_EQ(bulk_runs() - before, 1u);
    const u64 before2 = bulk_runs();
    const LoopOutcome big = run_jit(p.code, p.is64, default_data, bind(p, 0x1E000), Force::On);
    EXPECT_TRUE(big.bulked_first_entry);
    // 0x1E000 = 122880 iterations: one budget-capped bulk, then one that
    // reaches the exit, then the tail in that same entry.
    EXPECT_EQ(bulk_runs() - before2, 2u);
    EXPECT_EQ(big.entries, 2u);
}

MemSetup equal_buffers(u64 a, u64 b, u64 len, std::optional<u64> diff_at,
                       std::optional<u64> zero_at = std::nullopt, u32 w = 1) {
    return [=](GuestMemory& m) {
        default_data(m);
        std::vector<u8> buf(len);
        for (u64 i = 0; i < len; ++i) buf[i] = static_cast<u8>(0x31 + i % 61);
        ASSERT_TRUE(m.write_bytes(a, buf.data(), buf.size()));
        if (diff_at) buf[*diff_at] ^= 0x10;
        if (zero_at) {
            for (u32 j = 0; j < w; ++j) buf[*zero_at + j] = 0;
            ASSERT_TRUE(m.write_bytes(a + *zero_at, buf.data() + *zero_at, w));
        }
        ASSERT_TRUE(m.write_bytes(b, buf.data(), buf.size()));
    };
}

TEST(LoopIdiomDiff, ShippingPolicyAndCrossover) {
    // Force::On is the shipping configuration: the inline gate declines
    // below the measured crossover, at it the bulk runs, and shapes the
    // table does not lower (ungated strlen, 8-byte fill) never bulk.
    skip_if_env_off();
    const Pattern f1 = fill_copy_patterns()[0];
    ASSERT_STREQ(f1.name, "F1");
    const u32 x = min_iters(Kind::Fill, 0);
    ASSERT_GE(x, 3u);
    const Triple below = diff3(f1.code, f1.is64, default_data, bind(f1, x - 1), Force::On);
    EXPECT_FALSE(below.on.bulked_first_entry) << "below the crossover";
    const Triple at = diff3(f1.code, f1.is64, default_data, bind(f1, x), Force::On);
    EXPECT_TRUE(at.on.bulked_first_entry) << "at the crossover";

    EXPECT_FALSE(lowered(Kind::Scan, 0, /*has_inline_gate=*/false));
    const Triple strlen_on = diff3(kS4, true, with_bytes({{SRC + 5000, {0}}}), [](CpuState& c) {
        c.gpr.set64(ECX, SRC); c.gpr.set64(EAX, ~u64{0});
    }, Force::On);
    EXPECT_FALSE(strlen_on.on.bulked_first_entry) << "ungated strlen is not lowered";

    // 8-byte fill and copy are lowered above their measured crossover.
    for (const Pattern& p : fill_copy_patterns()) {
        const std::string name = p.name;
        if (name != "F5" && name != "C8") continue;
        const Kind k = name == "F5" ? Kind::Fill : Kind::Copy;
        ASSERT_TRUE(lowering(k, 3).lowered) << name;
        const u32 m = min_iters(k, 3);
        ASSERT_GE(m, 3u);
        SCOPED_TRACE(name);
        EXPECT_FALSE(diff3(p.code, p.is64, default_data, bind(p, m - 1), Force::On).on.bulked_first_entry)
            << "below the crossover";
        EXPECT_TRUE(diff3(p.code, p.is64, default_data, bind(p, m), Force::On).on.bulked_first_entry)
            << "at the crossover";
    }
    // 8-byte compare: gated on the count; mismatch past the bulk window.
    ASSERT_TRUE(lowering(Kind::Compare, 3).lowered);
    const u32 m4 = min_iters(Kind::Compare, 3);
    const u64 len = 8 * (u64{m4} + 64);
    for (u32 n : {m4 - 1, m4}) {
        SCOPED_TRACE("M4 n=" + std::to_string(n));
        const Triple t = diff3(kM4, true, equal_buffers(DST, SRC, len, std::nullopt), [n](CpuState& c) {
            c.gpr.set64(EDI, DST); c.gpr.set64(ESI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, n);
        }, Force::On);
        EXPECT_EQ(t.on.bulked_first_entry, n >= m4);
    }
}

TEST(LoopIdiomDiff, EightByteElementsFaultOverlapDf) {
    // Data RW 0x20000..0x30000 and 0x40000..0x50000; both ends unmapped.
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m.map_region(0x40000, 0x10000, MemoryProtection::ReadWrite));
        std::vector<u8> f(0x10000, 0x41);
        ASSERT_TRUE(m.write_bytes(0x20000, f.data(), f.size()));
        ASSERT_TRUE(m.write_bytes(0x40000, f.data(), f.size()));
    };
    constexpr u64 kCount = 6000;
    for (u64 k : {1ull, 2ull, 3000ull, 4096ull, 4097ull, kCount}) {
        const u64 back = 8 * (k - 1);
        {
            SCOPED_TRACE("F5 store fault at element " + std::to_string(k));
            const Triple t = diff3(kF5, true, mem, [back](CpuState& c) {
                c.gpr.set64(EDI, 0x30000 - back); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, kCount);
                c.gpr.set64(ESI, 0x0102030405060708ull);
            });
            EXPECT_TRUE(t.on.faulted);
            EXPECT_EQ(t.on.info0, 1u) << "a store is a WRITE fault";
            EXPECT_EQ(t.on.info1, 0x30000u);
        }
        {
            SCOPED_TRACE("C8 load fault at element " + std::to_string(k));
            const Triple t = diff3(kC8, true, mem, [back](CpuState& c) {
                c.gpr.set64(ESI, 0x30000 - back); c.gpr.set64(EDI, 0x40000);
                c.gpr.set64(ECX, 0); c.gpr.set64(EDX, kCount);
            });
            EXPECT_TRUE(t.on.faulted);
            EXPECT_EQ(t.on.info0, 0u) << "a load is a READ fault";
            EXPECT_EQ(t.on.info1, 0x30000u);
        }
        {
            SCOPED_TRACE("C8 store fault at element " + std::to_string(k));
            const Triple t = diff3(kC8, true, mem, [back](CpuState& c) {
                c.gpr.set64(ESI, 0x20000); c.gpr.set64(EDI, 0x50000 - back);
                c.gpr.set64(ECX, 0); c.gpr.set64(EDX, kCount);
            });
            EXPECT_TRUE(t.on.faulted);
            EXPECT_EQ(t.on.info0, 1u);
            EXPECT_EQ(t.on.info1, 0x50000u);
        }
        {
            SCOPED_TRACE("M4 first-stream load fault at element " + std::to_string(k));
            const Triple t = diff3(kM4, true, mem, [back](CpuState& c) {
                c.gpr.set64(EDI, 0x30000 - back); c.gpr.set64(ESI, 0x40000);
                c.gpr.set64(ECX, 0); c.gpr.set64(EDX, kCount);
            });
            EXPECT_TRUE(t.on.faulted);
            EXPECT_EQ(t.on.info0, 0u);
            EXPECT_EQ(t.on.info1, 0x30000u);
        }
        {
            SCOPED_TRACE("M4 second-stream load fault at element " + std::to_string(k));
            const Triple t = diff3(kM4, true, mem, [back](CpuState& c) {
                c.gpr.set64(EDI, 0x20000); c.gpr.set64(ESI, 0x50000 - back);
                c.gpr.set64(ECX, 0); c.gpr.set64(EDX, kCount);
            });
            EXPECT_TRUE(t.on.faulted);
            EXPECT_EQ(t.on.info0, 0u);
            EXPECT_EQ(t.on.info1, 0x50000u);
        }
    }
    {
        SCOPED_TRACE("C8 forward overlap dst = src + 8 replicates a qword: declines");
        const Triple t = diff3(kC8, true, default_data, [](CpuState& c) {
            c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, SRC + 8); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 500);
        });
        EXPECT_FALSE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("C8 forward overlap dst = src + 4 (partial qword): declines");
        const Triple t = diff3(kC8, true, default_data, [](CpuState& c) {
            c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, SRC + 4); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 500);
        });
        EXPECT_FALSE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("C8 forward copy with dst < src (memmove-down) bulks");
        const Triple t = diff3(kC8, true, default_data, [](CpuState& c) {
            c.gpr.set64(ESI, SRC + 8); c.gpr.set64(EDI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 500);
        });
        if (!ws2_env_off()) EXPECT_TRUE(t.on.bulked_first_entry);
    }
    {
        SCOPED_TRACE("C8 in place");
        diff3(kC8, true, default_data, [](CpuState& c) {
            c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 500);
        });
    }
    {
        SCOPED_TRACE("M4 over overlapping buffers (src = dst + 8)");
        diff3(kM4, true, default_data, [](CpuState& c) {
            c.gpr.set64(EDI, SRC); c.gpr.set64(ESI, SRC + 8); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 500);
        });
    }
    for (bool df : {false, true}) {
        SCOPED_TRACE(df ? "DF=1" : "DF=0");
        diff3(kF5, true, default_data, [df](CpuState& c) {
            c.flags.set_df(df);
            c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 700); c.gpr.set64(ESI, 0x1122334455667788ull);
        });
        diff3(kC8, true, default_data, [df](CpuState& c) {
            c.flags.set_df(df);
            c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 700);
        });
        diff3(kM4, true, equal_buffers(DST, SRC, 8 * 800, 8 * 650 + 3), [df](CpuState& c) {
            c.flags.set_df(df);
            c.gpr.set64(EDI, DST); c.gpr.set64(ESI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 700);
        });
    }
    // Near misses run exactly as the per-instruction translation.
    SCOPED_TRACE("qword near misses");
    diff3(kNmQStride, true, default_data, [](CpuState& c) {
        c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 700); c.gpr.set64(ESI, 0x55);
    });
    diff3(kNmQWidth, true, default_data, [](CpuState& c) {
        c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 700);
    });
    diff3(kNmQCmpStride, true, default_data, [](CpuState& c) {
        c.gpr.set64(EDI, DST); c.gpr.set64(ESI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 700);
    });
}

TEST(LoopIdiomDiff, KillSwitchCompilesNoHook) {
    const Pattern p = fill_copy_patterns()[0];
    const LoopOutcome off = run_jit(p.code, p.is64, default_data, bind(p, 4096), Force::Off);
    EXPECT_FALSE(off.bulked_first_entry);
    skip_if_env_off();
    const LoopOutcome on = run_jit(p.code, p.is64, default_data, bind(p, 4096), Force::On);
    EXPECT_TRUE(on.bulked_first_entry) << "default thresholds still take a 4096-byte fill";
}

// ---------------------------------------------------------------------------
// Scan and compare: the data exit at every interesting element.

TEST(LoopIdiomDiff, ScanTerminatorPositions) {
    const u64 hits[] = {0, 1, 2, 3, 15, 16, 4093, 4094, 4095, 4096, 9000};
    for (u64 h : hits) {
        SCOPED_TRACE("hit=" + std::to_string(h));
        auto zero_at = [](u64 a, u32 w) { return with_bytes({{a, std::vector<u8>(w, 0)}}); };
        diff3(kS1, false, zero_at(SRC + h, 1), [](CpuState& c) { c.gpr.set32(EAX, SRC); });
        diff3(kS2, false, zero_at(SRC + h, 1), [](CpuState& c) { c.gpr.set32(EAX, SRC); });
        // memchr with a count larger than the hit, and smaller.
        for (u64 n : {h + 5, h > 2 ? h - 1 : 1}) {
            diff3(kS3, false, with_bytes({{SRC + h, {0x2A}}}), [n](CpuState& c) {
                c.gpr.set32(EDI, SRC); c.gpr.set32(ECX, static_cast<u32>(n)); c.gpr.set32(EAX, 0x2A);
            });
            diff3(kS7, true, with_bytes({{SRC + h, {0x2B}}}), [n](CpuState& c) {
                c.gpr.set64(EDI, SRC); c.gpr.set64(EDX, n); c.gpr.set64(ESI, 0x2B);
            });
        }
        diff3(kS4, true, zero_at(SRC + h, 1), [](CpuState& c) {
            c.gpr.set64(ECX, SRC); c.gpr.set64(EAX, ~u64{0});
        });
        if (2 * h < 0x1F000) {
            diff3(kS5, true, zero_at(SRC + 2 * h, 2), [](CpuState& c) {
                c.gpr.set64(EAX, SRC); c.gpr.set64(ECX, ~u64{0});
            });
        }
        // strcpy: the terminator is copied by the body.
        diff3(kC5, false, zero_at(SRC + h, 1), [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST);
        });
        if (2 * h < 0x1F000) {
            diff3(kC12, true, zero_at(SRC + 2 * h, 2), [](CpuState& c) {
                c.gpr.set64(ESI, SRC); c.gpr.set64(EDI, DST); c.gpr.set64(ECX, 0);
            });
        }
    }
}

TEST(LoopIdiomDiff, ScanValueFromHighByteReg) {
    diff3(kS3h, false, with_bytes({{SRC + 777, {0x5D}}}), [](CpuState& c) {
        c.gpr.set32(EDI, SRC); c.gpr.set32(ECX, 2000); c.gpr.set32(EAX, 0x5D00);
    });
}

TEST(LoopIdiomDiff, ScanFaultBeforeTerminator) {
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        std::vector<u8> f(0x10000, 0x41);
        ASSERT_TRUE(m.write_bytes(0x20000, f.data(), f.size()));
    };
    const Triple t = diff3(kS4, true, mem, [](CpuState& c) {
        c.gpr.set64(ECX, 0x2F000); c.gpr.set64(EAX, ~u64{0});
    });
    EXPECT_TRUE(t.on.faulted);
    EXPECT_EQ(t.on.fault_addr, 0x30000u);
}

TEST(LoopIdiomDiff, CompareMismatchPositions) {
    const u64 hits[] = {0, 1, 2, 3, 15, 16, 4093, 4094, 4095, 4096, 9000};
    for (u64 h : hits) {
        SCOPED_TRACE("mismatch at " + std::to_string(h));
        const u64 len = 12000;
        diff3(kM1, false, equal_buffers(SRC, DST, len, h), [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, 0); c.gpr.set32(EDX, 11000);
        });
        diff3(kM2, false, equal_buffers(SRC, DST, len, h), [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST);
        });
        diff3(kM2, false, equal_buffers(SRC, DST, len, std::nullopt, h), [](CpuState& c) {
            c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST);
        });
        diff3(kM3, true, equal_buffers(DST, SRC, len, h), [](CpuState& c) {
            c.gpr.set64(EDI, DST); c.gpr.set64(ESI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 11000);
        });
        if (8 * h < len) {
            diff3(kM4, true, equal_buffers(DST, SRC, len, 8 * h), [](CpuState& c) {
                c.gpr.set64(EDI, DST); c.gpr.set64(ESI, SRC); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 1400);
            });
        }
        if (2 * h + 1 < len) {
            diff3(kM5, true, equal_buffers(DST, SRC, len, std::nullopt, 2 * h, 2), [](CpuState& c) {
                c.gpr.set64(EDI, DST); c.gpr.set64(ESI, SRC); c.gpr.set64(ECX, 0);
            });
        }
    }
    SCOPED_TRACE("no mismatch within the count");
    diff3(kM1, false, equal_buffers(SRC, DST, 9000, std::nullopt), [](CpuState& c) {
        c.gpr.set32(ESI, SRC); c.gpr.set32(EDI, DST); c.gpr.set32(ECX, 0); c.gpr.set32(EDX, 8500);
    });
}

TEST(LoopIdiomDiff, CompareStopsAtFirstDifferenceAcrossRegions) {
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m.map_region(0x30000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(m.map_region(0x40000, 0x20000, MemoryProtection::ReadWrite));
        std::vector<u8> a(0x2000, 0x66);
        ASSERT_TRUE(m.write_bytes(0x2F000, a.data(), a.size()));
        a[0x1800] = 0x67;
        ASSERT_TRUE(m.write_bytes(0x40000, a.data(), a.size()));
    };
    diff3(kM3, true, mem, [](CpuState& c) {
        c.gpr.set64(EDI, 0x2F000); c.gpr.set64(ESI, 0x40000); c.gpr.set64(ECX, 0); c.gpr.set64(EDX, 0x1F00);
    });
}

// ---------------------------------------------------------------------------
// Wide (SSE) variants.

TEST(LoopIdiomDiff, SseScanEveryLane) {
    for (std::span<const u8> code : {std::span<const u8>(kS6), std::span<const u8>(kS6w),
                                     std::span<const u8>(kS6d)}) {
        for (u64 elem : {0ull, 1ull, 2ull, 255ull, 256ull, 4095ull}) {
            for (u32 lane = 0; lane < 16; lane += 1) {
                if (elem > 3 && lane % 5 != 0) continue;
                SCOPED_TRACE("elem=" + std::to_string(elem) + " byte=" + std::to_string(lane));
                diff3(code, true, with_bytes({{SRC + 16 * elem + lane, {0, 0, 0, 0}}}),
                      [](CpuState& c) {
                          c.gpr.set64(EAX, SRC);
                          for (u8 i = 0; i < 16; ++i) c.sse.xmm[0].u8_[i] = 0;
                          set_xmm(c, 1, 0x10);
                      });
            }
        }
    }
}

TEST(LoopIdiomDiff, SseScanLoadMaySpanPastTerminator) {
    const MemSetup mem = [](GuestMemory& m) {
        ASSERT_TRUE(m.map_region(0x20000, 0x10000, MemoryProtection::ReadWrite));
        std::vector<u8> f(0x10000, 0x41);
        ASSERT_TRUE(m.write_bytes(0x20000, f.data(), f.size()));
    };
    // 16-byte elements from 0x2F008: the last one straddles into 0x30000.
    // Only JIT(on) vs JIT(off) is compared: the existing JIT already leaves
    // XMM1 different from the interpreter on this straddling MOVDQU fault
    // (pre-existing; owned by the interpreter / JIT ops, not WS2).
    const CpuSetup cpu = [](CpuState& c) {
        c.gpr.set64(EAX, 0x2F008);
        for (u8 i = 0; i < 16; ++i) c.sse.xmm[0].u8_[i] = 0;
    };
    const LoopOutcome off = run_jit(kS6, true, mem, cpu, Force::Off);
    const LoopOutcome on = run_jit(kS6, true, mem, cpu, Force::OnNoThreshold);
    expect_same(off, on, "JIT(off) vs JIT(on)", true);
    EXPECT_TRUE(on.faulted);
    EXPECT_EQ(on.fault_addr, 0x30000u);
    if (!ws2_env_off()) EXPECT_TRUE(on.bulked_first_entry);
}

TEST(LoopIdiomDiff, FillFromXmmValueAtEntry) {
    // The helper reads XMM0 from CpuState at block entry.
    for (u64 n : {3ull, 200ull, 4097ull}) {
        diff3(kF6, true, default_data, [n](CpuState& c) {
            c.gpr.set64(EDI, DST); c.gpr.set64(EAX, 0); c.gpr.set64(EDX, 16 * n);
            for (u8 i = 0; i < 16; ++i) c.sse.xmm[0].u8_[i] = 0xFF;
        });
    }
}

// ---------------------------------------------------------------------------
// Carried NZCV. A predecessor block (CMP) leaves its flags live in host NZCV
// and records nzcv_kind; the loop block is then entered with those flags
// carried. INC/DEC leave CF unchanged (Intel SDM Vol. 2 INC, DEC), so the
// CF the F1 loop exits with is the CMP's: it must survive the success path
// (MSR NZCV + emit_persist_live_nzcv + re-dispatch) and the decline path.

TEST(LoopIdiomDiff, CarriedNzcvSurvivesBulkAndDecline) {
    // cmp eax, ebx ; then F1 at kCodeBase + 2.
    std::vector<u8> code = {0x39, 0xD8};
    code.insert(code.end(), std::begin(kF1), std::end(kF1));
    const u64 loop_rip = kCodeBase + 2;
    struct Case { u32 eax, ebx; u64 n; Force force; };
    const Case cases[] = {
        {3, 5, 4096, Force::On},   // CF=1 carried, bulk runs
        {5, 3, 4096, Force::On},   // CF=0 carried, bulk runs
        {3, 5, 2, Force::On},      // real thresholds: inline gate declines
        {3, 5, 2, Force::OnNoThreshold},  // no gate: the helper declines (m_cap 0)
        {3, 5, 4096, Force::Off},
    };
    for (const Case& k : cases) {
        SCOPED_TRACE("eax=" + std::to_string(k.eax) + " n=" + std::to_string(k.n) +
                     " force=" + std::to_string(int(k.force)));
        auto regs = [&](CpuState& c) {
            c.gpr.set32(EAX, k.eax); c.gpr.set32(3 /* EBX */, k.ebx);
            c.gpr.set32(EDI, DST); c.gpr.set32(ECX, static_cast<u32>(k.n));
        };
        const LoopOutcome ref = run_interpreter(code, false, default_data, regs);

        ScopedForce scoped(k.force);
        LoopOutcome out;
        auto mem = make_memory(code, default_data);
        init_cpu(out.cpu, false);
        regs(out.cpu);
        JitCompiler jit(*mem);
        const BasicBlock cmp_block = decode_block(std::span<const u8>(code).first(2), false);
        const BasicBlock loop_block =
            decode_block(std::span<const u8>(code).subspan(2), false, loop_rip);
        CompiledBlock* cmp_cb = jit.compile(cmp_block);
        ASSERT_NE(cmp_cb, nullptr);
        CompiledBlock cmp_copy = *cmp_cb;
        CompiledBlock* loop_cb = jit.compile(loop_block);
        ASSERT_NE(loop_cb, nullptr);
        CompiledBlock loop_copy = *loop_cb;
        (void)vitreo::cpu::jit::test::execute_like_dispatcher(jit, cmp_copy, out.cpu);
        ASSERT_EQ(out.cpu.rip, loop_rip);
        const u64 before = bulk_runs();
        for (u32 guard = 0; guard < 100000 && out.cpu.rip == loop_rip &&
                            !out.cpu.pending_raise.active; ++guard) {
            (void)vitreo::cpu::jit::test::execute_like_dispatcher(jit, loop_copy, out.cpu);
        }
        const bool bulked = bulk_runs() != before;
        out.cpu.materialize_lazy_flags();
        read_data(*mem, out.data);
        expect_same(ref, out, "interpreter vs JIT, carried NZCV", false);
        EXPECT_EQ(ref.cpu.flags.cf(), k.eax < k.ebx);
        // A Force overrides VITREO_WS2 (loop_idiom_gate.hpp), so this holds either way.
        const bool expect_bulk = k.force != Force::Off && k.n == 4096;
        EXPECT_EQ(bulked, expect_bulk);
    }
}
