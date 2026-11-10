// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// FXSAVE / FXRSTOR / XSAVE / XRSTOR image faults, interpreter and JIT.
//
// Every row of tests/oracle_data/fxsave_fault_probe.expected (Windows 11,
// x64 in long-mode blocks, x86 in 32-bit ones) is replayed: an image straddling a
// denied page that follows it (layout A) or precedes it (layout B), the
// denied page PAGE_NOACCESS or PAGE_READONLY, XSAVE/XRSTOR with RFBM 3 or 7.
// Windows reports [0] = 1 for the saves and 0 for the restores, and [1] as
// recorded there: the image's last byte when that byte is inaccessible, else
// its first inaccessible byte; XRSTOR reports the XSAVE header's byte 520
// first. A faulting image instruction writes no memory and no register.
//
// The expected values come from that file, not from either engine.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
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
constexpr u64 kDenied = 0x8000;  // the denied page, inside the rig's mapping
constexpr u8 kRsi = static_cast<u8>(RegIndex::RSI);

struct Row {
    bool layout_a = true;   // denied page follows the image start
    bool noaccess = true;   // PAGE_NOACCESS, else PAGE_READONLY
    std::string op;
    unsigned rfbm = 3;
    unsigned split = 0;     // image bytes before (A) / inside (B) the boundary
    bool faults = false;
    unsigned write = 0;
    long reported = 0;      // relative to the image start
};

std::vector<Row> load_rows(const char* file) {
    const auto path = std::filesystem::path(__FILE__).parent_path()
                          .parent_path().parent_path() / "tests/oracle_data" / file;
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << path;
    std::vector<Row> rows;
    bool layout_a = true, noaccess = true;
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("layout ", 0) == 0) {
            layout_a = line.find("layout A") == 0;
            noaccess = line.find("NOACCESS") != std::string::npos;
            continue;
        }
        Row r;
        r.layout_a = layout_a;
        r.noaccess = noaccess;
        std::istringstream s(line);
        std::string rfbm, split;
        if (!(s >> r.op >> rfbm >> split)) continue;
        if (rfbm.rfind("rfbm=", 0) != 0) continue;
        r.rfbm = static_cast<unsigned>(std::stoul(rfbm.substr(5)));
        r.split = static_cast<unsigned>(std::stoul(split.substr(6)));
        const auto w_at = line.find("write=");
        r.faults = w_at != std::string::npos;
        if (r.faults) {
            r.write = static_cast<unsigned>(std::stoul(line.substr(w_at + 6)));
            r.reported = std::stol(line.substr(line.find("img") + 3));
        }
        rows.push_back(r);
    }
    return rows;
}

std::vector<u8> encoding(const std::string& op) {
    if (op == "fxsave") return {0x0F, 0xAE, 0x06};
    if (op == "fxrstor") return {0x0F, 0xAE, 0x0E};
    if (op == "xsave") return {0x0F, 0xAE, 0x26};
    if (op == "xrstor") return {0x0F, 0xAE, 0x2E};
    return {};
}

w::JitRun run_jit(w::CryptoRig& rig, const std::vector<u8>& code,
                  const CpuState& init) {
    w::JitRun r = rig.jit(code, init);
    if (!r.compiled) r = rig.jit(code, init, /*allow_delegate=*/true);
    return r;
}

std::vector<u8> snapshot(w::CryptoRig& rig) {
    std::vector<u8> out(0x3000, 0xEE);
    for (u64 i = 0; i < out.size(); ++i) {
        auto b = rig.mem().read_u8(kDenied - 0x1000 + i);
        if (b) out[i] = b.value();
    }
    return out;
}

void expect_vectors(const CpuState& got, const CpuState& init) {
    for (int i = 0; i < 16; ++i) {
        SCOPED_TRACE(::testing::Message() << "xmm" << i);
        EXPECT_EQ(got.sse.xmm[i].u64_[0], init.sse.xmm[i].u64_[0]);
        EXPECT_EQ(got.sse.xmm[i].u64_[1], init.sse.xmm[i].u64_[1]);
        EXPECT_EQ(got.avx.ymm_high[i].u64_[0], init.avx.ymm_high[i].u64_[0]);
        EXPECT_EQ(got.avx.ymm_high[i].u64_[1], init.avx.ymm_high[i].u64_[1]);
    }
    EXPECT_EQ(got.sse.mxcsr, init.sse.mxcsr);
}

void replay(const char* file, bool bits64) {
    const std::vector<Row> rows = load_rows(file);
    ASSERT_GT(rows.size(), 100u);
    for (w::MemCfg cfg : w::kAllCfgs) {
        w::CryptoRig rig;
        if (w::cfg_bits64(cfg) != bits64 || !rig.init(cfg)) continue;
        SCOPED_TRACE(w::cfg_name(cfg));
        for (const Row& row : rows) {
            SCOPED_TRACE(::testing::Message()
                         << (row.layout_a ? "A " : "B ")
                         << (row.noaccess ? "NOACCESS " : "READONLY ") << row.op
                         << " rfbm=" << row.rfbm << " split=" << row.split);
            // A zeroed area is a valid image for every form; stage it, then
            // deny the page.
            ASSERT_TRUE(rig.mem().protect_region(kDenied, 0x1000,
                                                 MemoryProtection::ReadWrite));
            const std::vector<u8> zero(0x3000, 0);
            ASSERT_TRUE(rig.mem().write_bytes(kDenied - 0x1000, zero.data(),
                                              zero.size()));
            ASSERT_TRUE(rig.mem().protect_region(
                kDenied, 0x1000,
                row.noaccess ? MemoryProtection::None : MemoryProtection::Read));
            const u64 img = row.layout_a ? kDenied - row.split
                                         : kDenied + 0x1000 - row.split;
            CpuState init = rig.base();
            init.xcr0 = 7;
            init.gpr.set64(kRsi, img);
            init.gpr.set64(static_cast<u8>(RegIndex::RAX), row.rfbm);
            init.gpr.set64(static_cast<u8>(RegIndex::RDX), 0);
            const std::vector<u8> code = encoding(row.op);
            const std::vector<u8> before = snapshot(rig);
            const u64 want = img + static_cast<u64>(row.reported);

            CpuState interpreted = init;
            ASSERT_TRUE(rig.mem().write_bytes(w::kCode, code.data(), code.size()));
            Interpreter interpreter(interpreted, rig.mem());
            const ExecResult ir = interpreter.run(1);
            if (!row.faults) {
                EXPECT_EQ(ir, ExecResult::Continue);
            } else {
                SCOPED_TRACE("interpreter");
                ASSERT_EQ(ir, ExecResult::Exception);
                const ExecContext& ctx = interpreter.context();
                EXPECT_EQ(cpu_exception_to_status(ctx.exception), kAccessViolation);
                EXPECT_EQ(ctx.exception_error, row.write);
                EXPECT_EQ(ctx.exception_addr, want)
                    << std::hex << "got img+0x" << ctx.exception_addr - img;
                expect_vectors(interpreted, init);
                EXPECT_EQ(snapshot(rig), before);
            }

            // Re-stage: a non-faulting save wrote the image.
            ASSERT_TRUE(rig.mem().protect_region(kDenied, 0x1000,
                                                 MemoryProtection::ReadWrite));
            ASSERT_TRUE(rig.mem().write_bytes(kDenied - 0x1000, zero.data(),
                                              zero.size()));
            ASSERT_TRUE(rig.mem().protect_region(
                kDenied, 0x1000,
                row.noaccess ? MemoryProtection::None : MemoryProtection::Read));
            const w::JitRun r = run_jit(rig, code, init);
            ASSERT_TRUE(r.compiled);
            if (!row.faults) {
                EXPECT_FALSE(r.cpu.pending_raise.active);
            } else {
                SCOPED_TRACE("jit");
                ASSERT_TRUE(r.cpu.pending_raise.active);
                EXPECT_EQ(r.cpu.pending_raise.code, kAccessViolation);
                EXPECT_EQ(r.cpu.pending_raise.args[0], row.write);
                EXPECT_EQ(r.cpu.pending_raise.args[1], want)
                    << std::hex << "got img+0x"
                    << r.cpu.pending_raise.args[1] - img;
                EXPECT_EQ(r.cpu.rip, w::kCode);
                expect_vectors(r.cpu, init);
                EXPECT_EQ(snapshot(rig), before);
            }
        }
        ASSERT_TRUE(rig.mem().protect_region(kDenied, 0x1000,
                                             MemoryProtection::ReadWrite));
    }
}

}  // namespace

TEST(XstateFaultContract, ImageFaultsMatchWindowsX64Recording) {
    replay("fxsave_fault_probe.expected", /*bits64=*/true);
}

TEST(XstateFaultContract, ImageFaultsMatchWindowsX86Recording) {
    replay("fxsave_fault_probe.x86.expected", /*bits64=*/false);
}
