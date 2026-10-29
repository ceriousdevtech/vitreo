// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// A guest reads CPUID and then uses what it was told it has. Advertising a
// feature bit whose instructions the CPU cannot execute therefore turns into a
// guest crash at an arbitrary later point, far from the CPUID call.
//
// tests/cpu/test_cpuid_extended.cpp asserts that the feature *bits* are set.
// This asserts the other half: that a representative instruction from each of
// those features actually executes. The two together are the rule "do not
// advertise what you cannot run".
//
// Scope, honestly: one form per feature is a tripwire, not a proof. A feature
// can be advertised and mostly implemented with a specific form missing, and
// this will not notice. It exists to catch the failure mode where a bit gets
// set ahead of the lowering work.

#include <gtest/gtest.h>

#include <vector>

#include "core/cpu/interpreter/interpreter.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

namespace {
struct Probe {
    const char* iset;
    const char* name;
    std::vector<u8> code;
};
}  // namespace

TEST(AdvertisedIsa, InterpreterExecutesOneFormPerAdvertisedFeature) {
    // One representative form per advertised feature bit.
    const std::vector<Probe> probes = {
        {"SSE3",   "haddps xmm0,xmm1",   {0xF2,0x0F,0x7C,0xC1}},
        {"SSE3",   "movddup xmm0,xmm1",  {0xF2,0x0F,0x12,0xC1}},
        {"SSSE3",  "pshufb xmm0,xmm1",   {0x66,0x0F,0x38,0x00,0xC1}},
        {"SSSE3",  "phaddw xmm0,xmm1",   {0x66,0x0F,0x38,0x01,0xC1}},
        {"SSSE3",  "palignr xmm0,xmm1,4",{0x66,0x0F,0x3A,0x0F,0xC1,0x04}},
        {"SSE4.1", "pmulld xmm0,xmm1",   {0x66,0x0F,0x38,0x40,0xC1}},
        {"SSE4.1", "pmovzxbw xmm0,xmm1", {0x66,0x0F,0x38,0x30,0xC1}},
        {"SSE4.1", "roundps xmm0,xmm1,0",{0x66,0x0F,0x3A,0x08,0xC1,0x00}},
        {"SSE4.1", "pblendw xmm0,xmm1,5",{0x66,0x0F,0x3A,0x0E,0xC1,0x05}},
        {"SSE4.2", "pcmpgtq xmm0,xmm1",  {0x66,0x0F,0x38,0x37,0xC1}},
        {"SSE4.2", "crc32 eax,ecx",      {0xF2,0x0F,0x38,0xF1,0xC1}},
        {"POPCNT", "popcnt eax,ecx",     {0xF3,0x0F,0xB8,0xC1}},
        {"AES",    "aesenc xmm0,xmm1",   {0x66,0x0F,0x38,0xDC,0xC1}},
        {"PCLMUL", "pclmulqdq x0,x1,0",  {0x66,0x0F,0x3A,0x44,0xC1,0x00}},
        {"AVX",    "vaddps x0,x1,x2",    {0xC5,0xF0,0x58,0xC2}},
        {"AVX2",   "vpaddd y0,y1,y2",    {0xC5,0xF5,0xFE,0xC2}},
        {"BMI1",   "andn eax,ecx,edx",   {0xC4,0xE2,0x70,0xF2,0xC2}},
        {"BMI2",   "shlx eax,ecx,edx",   {0xC4,0xE2,0x69,0xF7,0xC1}},
        {"FMA",    "vfmadd132ps",        {0xC4,0xE2,0x71,0x98,0xC2}},
        {"F16C",   "vcvtph2ps x0,x1",    {0xC4,0xE2,0x79,0x13,0xC1}},
        {"MOVBE",  "movbe eax,[r15]",    {0x41,0x0F,0x38,0xF0,0x07}},
    };
    for (const auto& p : probes) {
        GuestMemory mem;
        ASSERT_TRUE(mem.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite));
        ASSERT_TRUE(mem.write_bytes(0x10000, reinterpret_cast<const Byte*>(p.code.data()), p.code.size()));
        CpuState cpu; cpu.init_long_mode();
        cpu.rip = 0x10000;
        cpu.gpr.set64(15, 0x11000);
        Interpreter interp(cpu, mem);
        const ExecResult r = interp.step();
        const bool ok = (r == ExecResult::Continue || r == ExecResult::Branch);
        EXPECT_TRUE(ok) << p.iset << " / " << p.name
                        << ": CPUID advertises this feature but the interpreter "
                           "returned ExecResult " << int(r);
    }
}
