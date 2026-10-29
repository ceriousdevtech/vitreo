// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include "core/pe/loader/load_config.hpp"

using namespace vitreo;
using namespace vitreo::pe;

namespace {

constexpr GuestAddr IMAGE_BASE = 0x00400000;
constexpr u32 LOAD_CONFIG_RVA = 0x1000;
constexpr u32 COOKIE_RVA = 0x2000;

ParsedPe make_pe(bool is_64bit) {
    ParsedPe pe{};
    pe.is_64bit = is_64bit;
    pe.num_data_directories = DIR_COUNT;
    pe.data_directories[DIR_LOAD_CONFIG].VirtualAddress = LOAD_CONFIG_RVA;
    pe.data_directories[DIR_LOAD_CONFIG].Size = is_64bit ? 0x60 : 0x40;
    return pe;
}

} // namespace

TEST(LoadConfigTest, InitializesX64SecurityCookieBeforeImageCode) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(true);

    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA, 0x60).is_ok());
    ASSERT_TRUE(memory.write_u64(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x58,
        IMAGE_BASE + COOKIE_RVA).is_ok());
    constexpr u64 linker_default = 0x00002B992DDFA232ull;
    ASSERT_TRUE(memory.write_u64(
        IMAGE_BASE + COOKIE_RVA, linker_default).is_ok());

    ASSERT_TRUE(initialize_load_config_security_cookie(
        memory, pe, IMAGE_BASE));
    const u64 cookie = memory.read_u64(IMAGE_BASE + COOKIE_RVA).value();
    EXPECT_NE(cookie, 0u);
    EXPECT_NE(cookie, linker_default);
    EXPECT_EQ(cookie & 0xFFFF000000000000ull, 0u);
}

TEST(LoadConfigTest, InitializesX86SecurityCookieBeforeImageCode) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(false);

    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA, 0x40).is_ok());
    ASSERT_TRUE(memory.write_u32(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x3C,
        static_cast<u32>(IMAGE_BASE + COOKIE_RVA)).is_ok());
    constexpr u32 linker_default = 0xBB40E64Eu;
    ASSERT_TRUE(memory.write_u32(
        IMAGE_BASE + COOKIE_RVA, linker_default).is_ok());

    ASSERT_TRUE(initialize_load_config_security_cookie(
        memory, pe, IMAGE_BASE));
    const u32 cookie = memory.read_u32(IMAGE_BASE + COOKIE_RVA).value();
    EXPECT_NE(cookie, 0u);
    EXPECT_NE(cookie, linker_default);
    EXPECT_NE(cookie & 0xFFFF0000u, 0u);
}

// XP-era /GS images preset a 16-bit cookie (e.g. spider.exe's 0x0000BB40) and
// their __security_check_cookie fast-fails any cookie with a nonzero high
// word. The loader's replacement must keep the high word zero.
TEST(LoadConfigTest, PreservesX86SixteenBitCookieShape) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(false);

    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA, 0x40).is_ok());
    ASSERT_TRUE(memory.write_u32(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x3C,
        static_cast<u32>(IMAGE_BASE + COOKIE_RVA)).is_ok());
    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + COOKIE_RVA, 0xBB40u).is_ok());

    ASSERT_TRUE(initialize_load_config_security_cookie(
        memory, pe, IMAGE_BASE));
    const u32 cookie = memory.read_u32(IMAGE_BASE + COOKIE_RVA).value();
    EXPECT_NE(cookie, 0u);
    EXPECT_EQ(cookie & 0xFFFF0000u, 0u)
        << "a 16-bit preset cookie was replaced with a 32-bit one";
}

// A preset that is neither the linker default nor 16-bit belongs to the
// image's own CRT init; the loader leaves it untouched.
TEST(LoadConfigTest, LeavesNonDefaultX86CookieAlone) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(false);

    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA, 0x40).is_ok());
    ASSERT_TRUE(memory.write_u32(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x3C,
        static_cast<u32>(IMAGE_BASE + COOKIE_RVA)).is_ok());
    constexpr u32 custom = 0x12345678u;
    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + COOKIE_RVA, custom).is_ok());

    ASSERT_TRUE(initialize_load_config_security_cookie(
        memory, pe, IMAGE_BASE));
    EXPECT_EQ(memory.read_u32(IMAGE_BASE + COOKIE_RVA).value(), custom);
}

TEST(LoadConfigTest, TemporarilyMakesCookieRegionWritableAndRestoresIt) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x2000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE + COOKIE_RVA, 0x1000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(true);

    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA, 0x60).is_ok());
    ASSERT_TRUE(memory.write_u64(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x58,
        IMAGE_BASE + COOKIE_RVA).is_ok());
    ASSERT_TRUE(memory.write_u64(
        IMAGE_BASE + COOKIE_RVA, 0x00002B992DDFA232ull).is_ok());
    ASSERT_TRUE(memory.protect_region(
        IMAGE_BASE + COOKIE_RVA, 0x1000, MemoryProtection::Read).is_ok());

    ASSERT_TRUE(initialize_load_config_security_cookie(
        memory, pe, IMAGE_BASE));
    GuestAddr region_base = 0;
    usize region_size = 0;
    MemoryProtection protection = MemoryProtection::None;
    ASSERT_TRUE(memory.query_region(
        IMAGE_BASE + COOKIE_RVA, region_base, region_size, protection));
    EXPECT_EQ(protection, MemoryProtection::Read);
}

TEST(LoadConfigTest, InitializesX64GuardCfPointersAndRestoresProtection) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(true);
    pe.data_directories[DIR_LOAD_CONFIG].Size = 0x80;

    constexpr GuestAddr check_variable = IMAGE_BASE + COOKIE_RVA;
    constexpr GuestAddr dispatch_variable = check_variable + sizeof(u64);
    constexpr GuestAddr check_target = 0x7D001000;
    constexpr GuestAddr dispatch_target = 0x7D001010;
    ASSERT_TRUE(memory.write_u32(
        IMAGE_BASE + LOAD_CONFIG_RVA, 0x80).is_ok());
    ASSERT_TRUE(memory.write_u64(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x70, check_variable).is_ok());
    ASSERT_TRUE(memory.write_u64(
        IMAGE_BASE + LOAD_CONFIG_RVA + 0x78, dispatch_variable).is_ok());
    ASSERT_TRUE(memory.protect_region(
        IMAGE_BASE + COOKIE_RVA, 0x1000, MemoryProtection::Read).is_ok());

    ASSERT_TRUE(initialize_load_config_guard_cf(
        memory, pe, IMAGE_BASE, check_target, dispatch_target));
    EXPECT_EQ(memory.read_u64(check_variable).value(), check_target);
    EXPECT_EQ(memory.read_u64(dispatch_variable).value(), dispatch_target);

    GuestAddr region_base = 0;
    usize region_size = 0;
    MemoryProtection protection = MemoryProtection::None;
    ASSERT_TRUE(memory.query_region(
        check_variable, region_base, region_size, protection));
    EXPECT_EQ(protection, MemoryProtection::Read);
}

TEST(LoadConfigTest, DoesNotReadGuardCfFieldsOutsideDeclaredStructure) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(true);
    pe.data_directories[DIR_LOAD_CONFIG].Size = 0x80;
    ASSERT_TRUE(memory.write_u32(
        IMAGE_BASE + LOAD_CONFIG_RVA, 0x70).is_ok());

    EXPECT_TRUE(initialize_load_config_guard_cf(
        memory, pe, IMAGE_BASE, 0x7D001000, 0x7D001010));
}

// The whole point of the guest-side stubs is that a guarded indirect call
// lands on real, executable guest instructions instead of a host thunk. If
// either stub ever stops being the exact byte sequence Windows uses when CFG
// is not enforced, guarded calls silently change behaviour: a wrong check stub
// clobbers the target register, and a wrong dispatch stub either loses the
// tail jump or double-pushes a return address.
TEST(LoadConfigTest, GuardCfStubsAreExecutableGuestNoOps) {
    GuestMemory memory;
    const GuardCfStubs stubs = install_guard_cf_stubs(memory);
    ASSERT_NE(stubs.check, 0u);
    ASSERT_NE(stubs.dispatch, 0u);
    EXPECT_NE(stubs.check, stubs.dispatch);

    // _guard_check_icall_nop: `ret`. Preserves every register and the flags,
    // which is the calling convention the guarded call site relies on.
    EXPECT_EQ(memory.read_u8(stubs.check).value(), 0xC3u);
    // _guard_dispatch_icall_nop: `jmp eax` / `jmp rax` (FF /4). A tail jump,
    // because the guest's CALL already pushed the final target's return
    // address.
    EXPECT_EQ(memory.read_u8(stubs.dispatch).value(), 0xFFu);
    EXPECT_EQ(memory.read_u8(stubs.dispatch + 1).value(), 0xE0u);

    GuestAddr region_base = 0;
    usize region_size = 0;
    MemoryProtection protection = MemoryProtection::None;
    ASSERT_TRUE(memory.query_region(
        stubs.check, region_base, region_size, protection));
    EXPECT_TRUE(has_flag(protection, MemoryProtection::Execute));
    EXPECT_FALSE(has_flag(protection, MemoryProtection::Write));

    // One page per process: repeat calls must not re-map or move the stubs.
    const GuardCfStubs again = install_guard_cf_stubs(memory);
    EXPECT_EQ(again.check, stubs.check);
    EXPECT_EQ(again.dispatch, stubs.dispatch);
}

// initialize_load_config_guard_cf must accept the stub addresses and publish
// them into the image's pointer variables, i.e. the guest never sees a thunk.
TEST(LoadConfigTest, PublishesGuestStubsIntoX86GuardCfVariables) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        IMAGE_BASE, 0x3000, MemoryProtection::ReadWrite).is_ok());
    auto pe = make_pe(false);
    pe.data_directories[DIR_LOAD_CONFIG].Size = 0x50;

    constexpr GuestAddr check_variable = IMAGE_BASE + COOKIE_RVA;
    constexpr GuestAddr dispatch_variable = check_variable + sizeof(u32);
    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA, 0x50).is_ok());
    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA + 0x48,
                                 static_cast<u32>(check_variable)).is_ok());
    ASSERT_TRUE(memory.write_u32(IMAGE_BASE + LOAD_CONFIG_RVA + 0x4C,
                                 static_cast<u32>(dispatch_variable)).is_ok());

    const GuardCfStubs stubs = install_guard_cf_stubs(memory);
    ASSERT_NE(stubs.check, 0u);
    ASSERT_TRUE(initialize_load_config_guard_cf(
        memory, pe, IMAGE_BASE, stubs.check, stubs.dispatch));
    EXPECT_EQ(memory.read_u32(check_variable).value(),
              static_cast<u32>(stubs.check));
    EXPECT_EQ(memory.read_u32(dispatch_variable).value(),
              static_cast<u32>(stubs.dispatch));
}
