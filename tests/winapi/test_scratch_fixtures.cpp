// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_scratch_fixtures.cpp
/// @brief Vitreo's fixed guest-address scratch regions must not overlap.
///
/// These fixtures are reserved in the *guest's* address space at startup. Two
/// that overlap do not fail loudly: the second reservation is refused and that
/// subsystem then hands out addresses another one owns, so the two silently
/// share memory. Three of them (crypt32, opengl32, glu32) sat inside ws2_32's
/// megabyte for exactly this reason, which showed up only as unexplained
/// VirtualAlloc failures in an IPC trace.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include "winapi/networking/ws2_32/ws2_32.hpp"
#include "winapi/system/crypt32/crypt32.hpp"
#include "winapi/graphics/opengl32/opengl32.hpp"
#include "winapi/graphics/glu32/glu32.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

struct Fixture {
    const char* name;
    GuestAddr base;
    u64 size;
    [[nodiscard]] u64 end() const { return static_cast<u64>(base) + size; }
};

} // namespace

TEST(ScratchFixturesTest, ReservedGuestRegionsDoNotOverlap) {
    std::vector<Fixture> fixtures{
        {"ws2_32", WS2_SCRATCH_BASE, WS2_SCRATCH_SIZE},
        {"crypt32", CRYPT32_SCRATCH_BASE, CRYPT32_SCRATCH_SIZE},
        {"opengl32", OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE},
        {"glu32", GLU32_SCRATCH_BASE, GLU32_SCRATCH_SIZE},
    };
    std::sort(fixtures.begin(), fixtures.end(),
              [](const Fixture& a, const Fixture& b) { return a.base < b.base; });

    for (usize i = 1; i < fixtures.size(); ++i) {
        const Fixture& previous = fixtures[i - 1];
        const Fixture& current = fixtures[i];
        EXPECT_LE(previous.end(), static_cast<u64>(current.base))
            << previous.name << " [" << std::hex << previous.base << ", "
            << previous.end() << ") overlaps " << current.name << " ["
            << current.base << ", " << current.end() << ")";
    }
}

TEST(ScratchFixturesTest, NoFixtureSitsInsideTheWs2Reservation) {
    // The specific regression: ws2_32 reserves a full megabyte, and a base
    // chosen as "0x5E0xxxxx, near ws2_32" lands inside it rather than beside.
    const u64 ws2_begin = WS2_SCRATCH_BASE;
    const u64 ws2_end = ws2_begin + WS2_SCRATCH_SIZE;
    const std::array<Fixture, 3> others{{
        {"crypt32", CRYPT32_SCRATCH_BASE, CRYPT32_SCRATCH_SIZE},
        {"opengl32", OPENGL32_SCRATCH_BASE, OPENGL32_SCRATCH_SIZE},
        {"glu32", GLU32_SCRATCH_BASE, GLU32_SCRATCH_SIZE},
    }};
    for (const Fixture& f : others) {
        const bool inside = f.base >= ws2_begin && f.base < ws2_end;
        EXPECT_FALSE(inside)
            << f.name << " base 0x" << std::hex << f.base
            << " is inside the ws2_32 reservation [0x" << ws2_begin << ", 0x"
            << ws2_end << ")";
    }
}
