// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_guest_font_loader.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, LoaderRegistrationBalancesGuestComReferences) {
    constexpr GuestAddr LOADER = DATA_BASE + 0x400;
    constexpr GuestAddr LOADER_VTABLE = DATA_BASE + 0x480;
    constexpr GuestAddr LOADER_ADDREF = 0x71000010;
    constexpr GuestAddr LOADER_RELEASE = 0x71000020;
    ASSERT_TRUE(memory.write_u64(LOADER, LOADER_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(
        LOADER_VTABLE + 1 * 8, LOADER_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(
        LOADER_VTABLE + 2 * 8, LOADER_RELEASE).is_ok());
    u32 add_refs = 0;
    u32 releases = 0;
    manager.set_guest_call(
        [&](GuestAddr function, const u64* args, usize count, bool is_64bit) {
            EXPECT_TRUE(is_64bit);
            EXPECT_EQ(count, 1u);
            EXPECT_EQ(args[0], LOADER);
            if (function == LOADER_ADDREF) return u64{++add_refs + 1};
            EXPECT_EQ(function, LOADER_RELEASE);
            ++releases;
            return u64{1};
        });

    const GuestAddr factory = create_factory();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = LOADER;
    manager.impl_RegisterFontCollectionLoader(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(add_refs, 1u);

    manager.impl_RegisterFontCollectionLoader(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x88985006u);
    EXPECT_EQ(add_refs, 1u);

    manager.impl_UnregisterFontCollectionLoader(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(releases, 1u);

    manager.impl_UnregisterFontCollectionLoader(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u);
    EXPECT_EQ(releases, 1u);
}

TEST_F(DWriteManagerTest,
       CustomFontFileReferenceCopiesKeyAndRetainsLoader) {
    constexpr GuestAddr LOADER = DATA_BASE + 0x400;
    constexpr GuestAddr LOADER_VTABLE = DATA_BASE + 0x480;
    constexpr GuestAddr KEY = DATA_BASE + 0x500;
    constexpr GuestAddr FILE_OUT = DATA_BASE + 0x580;
    constexpr GuestAddr KEY_OUT = DATA_BASE + 0x588;
    constexpr GuestAddr KEY_SIZE_OUT = DATA_BASE + 0x590;
    constexpr GuestAddr LOADER_ADDREF = 0x71000010;
    constexpr GuestAddr LOADER_RELEASE = 0x71000020;
    const std::array<u8, 8> original_key{
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
    };
    ASSERT_TRUE(memory.write_u64(LOADER, LOADER_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(
        LOADER_VTABLE + 1 * 8, LOADER_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(
        LOADER_VTABLE + 2 * 8, LOADER_RELEASE).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        KEY, original_key.data(), original_key.size()).is_ok());

    u32 add_refs = 0;
    u32 releases = 0;
    manager.set_guest_call(
        [&](GuestAddr function, const u64* args, usize count,
            bool is_64bit) {
            EXPECT_TRUE(is_64bit);
            EXPECT_EQ(count, 1u);
            EXPECT_EQ(args[0], LOADER);
            if (function == LOADER_ADDREF) return u64{++add_refs + 1};
            EXPECT_EQ(function, LOADER_RELEASE);
            ++releases;
            return u64{1};
        });

    const GuestAddr factory = create_factory();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = LOADER;
    manager.impl_RegisterFontFileLoader(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = KEY;
    cpu.gpr.r8 = original_key.size();
    cpu.gpr.r9 = LOADER;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, FILE_OUT).is_ok());
    manager.impl_CreateCustomFontFileReference(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr file = memory.read_u64(FILE_OUT).value_or(0);
    ASSERT_NE(file, 0u);
    EXPECT_EQ(add_refs, 2u); // Factory registration plus font-file lifetime.

    const std::array<u8, 8> overwritten_key{};
    ASSERT_TRUE(memory.write_bytes(
        KEY, overwritten_key.data(), overwritten_key.size()).is_ok());
    cpu.gpr.rcx = file;
    cpu.gpr.rdx = KEY_OUT;
    cpu.gpr.r8 = KEY_SIZE_OUT;
    manager.impl_FileGetReferenceKey(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(KEY_SIZE_OUT).value_or(0), original_key.size());
    const GuestAddr copied_key = memory.read_u64(KEY_OUT).value_or(0);
    ASSERT_NE(copied_key, 0u);
    EXPECT_NE(copied_key, KEY);
    std::array<u8, 8> observed_key{};
    ASSERT_TRUE(memory.read_bytes(
        copied_key, observed_key.data(), observed_key.size()).is_ok());
    EXPECT_EQ(observed_key, original_key);

    cpu.gpr.rcx = file;
    manager.impl_Release(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(releases, 1u);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = LOADER;
    manager.impl_UnregisterFontFileLoader(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(releases, 2u);
}

namespace {

// Expected values: tools/oracle_probe/dwrite_custom_loader_probe.expected
// (Windows 11, x64 and x86 identical), call names mapped to this harness.
// Fragment offsets and sizes are DirectWrite's own choice and not compared.
const std::vector<std::string> WINDOWS_FULL_LOAD{
    "CreateEnumeratorFromKey", "MoveNext->1", "GetCurrentFontFile 0",
    "CreateStreamFromKey 0", "GetFileSize 0", "GetLastWriteTime 0",
    "Release(stream 0)", "MoveNext->0", "Release(enumerator)",
};
// A file the factory already loaded: the stream is opened and its header
// read, but not loaded again.
const std::vector<std::string> WINDOWS_KNOWN_FILE{
    "CreateEnumeratorFromKey", "MoveNext->1", "GetCurrentFontFile 0",
    "CreateStreamFromKey 0", "GetFileSize 0", "Release(stream 0)",
    "MoveNext->0", "Release(enumerator)",
};

void expect_windows_custom_collection_caching(GuestFontLoader& loader) {
    const auto arial = system_font_bytes(u"Arial");
    const auto georgia = system_font_bytes(u"Georgia");
    ASSERT_FALSE(arial.empty());
    ASSERT_FALSE(georgia.empty());
    ASSERT_NE(arial.size(), georgia.size());
    auto damaged = arial;
    std::fill_n(damaged.begin(), 12, u8{0});
    ASSERT_TRUE(loader.add_file(arial, 0x01D0000000000000ull));
    loader.active = {0};

    const auto settled = [&] {
        EXPECT_TRUE(loader.violations.empty()) << loader.violations.front();
        EXPECT_TRUE(loader.outstanding_fragments.empty());
        EXPECT_EQ(loader.open_streams, 0u);
    };
    const auto families = [&](GuestAddr collection, std::u16string_view only) {
        EXPECT_EQ(loader.family_count(collection), only.empty() ? 0u : 1u);
        if (!only.empty()) EXPECT_TRUE(loader.has_family(collection, std::u16string(only)));
    };

    // first(key=1,arial)
    auto [hr, first] = loader.create_collection(1);
    ASSERT_EQ(hr, 0u);
    families(first, u"Arial");
    EXPECT_EQ(loader.skeleton(), WINDOWS_FULL_LOAD);
    settled();

    // repeat(key=1,arial): no loader calls, a new collection object.
    auto [repeat_hr, repeat] = loader.create_collection(1);
    ASSERT_EQ(repeat_hr, 0u);
    EXPECT_NE(repeat, first);
    EXPECT_TRUE(loader.log.empty());
    families(repeat, u"Arial");

    // changed(key=1,georgia,new size+time): still the cached collection.
    ASSERT_TRUE(loader.set_file(0, georgia, 0x01D0000000000001ull));
    std::tie(hr, std::ignore) = loader.create_collection(1);
    ASSERT_EQ(hr, 0u);
    EXPECT_TRUE(loader.log.empty());
    families(loader.read_ptr(GuestFontLoader::OUT), u"Arial");

    // otherkey(key=2,georgia): the file key is known to the factory.
    auto [other_hr, other] = loader.create_collection(2);
    ASSERT_EQ(other_hr, 0u);
    EXPECT_EQ(loader.skeleton(), WINDOWS_KNOWN_FILE);
    families(other, u"Arial");
    settled();

    // damaged(key=3,zeroed sfnt header): skipped, S_OK, no families.
    ASSERT_TRUE(loader.set_file(0, damaged, 0x01D0000000000002ull));
    auto [damaged_hr, damaged_collection] = loader.create_collection(3);
    ASSERT_EQ(damaged_hr, 0u);
    EXPECT_EQ(loader.skeleton(), WINDOWS_KNOWN_FILE);
    families(damaged_collection, u"");
    settled();

    // fresh_damaged(key=4,filekey=8): never-seen file key, not a font.
    ASSERT_TRUE(loader.add_file(damaged, 0x01D0000000000002ull));
    loader.active = {1};
    auto [fresh_hr, fresh] = loader.create_collection(4);
    ASSERT_EQ(fresh_hr, 0u);
    families(fresh, u"");
    settled();
    loader.active = {0};

    // collection_loader_reregistered(key=1,georgia): enumerated again, the
    // file is still known.
    ASSERT_TRUE(loader.set_file(0, georgia, 0x01D0000000000003ull));
    ASSERT_TRUE(loader.reregister_collection_loader());
    auto [rereg_hr, rereg] = loader.create_collection(1);
    ASSERT_EQ(rereg_hr, 0u);
    EXPECT_EQ(loader.skeleton(), WINDOWS_KNOWN_FILE);
    families(rereg, u"Arial");
    settled();

    // file_loader_reregistered(key=5,georgia): loaded again from the bytes.
    ASSERT_TRUE(loader.reregister_file_loader());
    auto [file_hr, reloaded] = loader.create_collection(5);
    ASSERT_EQ(file_hr, 0u);
    EXPECT_EQ(loader.skeleton(), WINDOWS_FULL_LOAD);
    families(reloaded, u"Georgia");
    settled();
}

}  // namespace

TEST_F(DWriteManagerTest, CustomCollectionCachingMatchesWindowsX64) {
    GuestFontLoader loader(memory, cpu, manager, true);
    ASSERT_TRUE(loader.set_up(IID_ADDRESS));
    expect_windows_custom_collection_caching(loader);
}

TEST_F(DWriteManagerTest, CustomCollectionCachingMatchesWindowsX86) {
    DWriteManager manager32{memory, false, &vfs, &gdi};
    manager32.set_method_thunks(method_thunks());
    cpu.mode = cpu::CpuMode::Protected;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    GuestFontLoader loader(memory, cpu, manager32, false);
    ASSERT_TRUE(loader.set_up(IID_ADDRESS));
    expect_windows_custom_collection_caching(loader);
}

// A new factory has none of another factory's cache: it drives the loader
// in full, even though Vitreo reuses
// the parse of byte-identical files internally. VITREO_DWRITE_FONT_REUSE=0
// re-enumerates on every call and must build the same collections.
TEST_F(DWriteManagerTest, CustomCollectionNewFactoryAndLegacyPathLoadInFull) {
    const auto courier = system_font_bytes(u"Courier New");
    const auto georgia = system_font_bytes(u"Georgia");
    for (const bool reuse : {true, true, false}) {
        DWriteManager fresh{memory, true, &vfs, &gdi};
        fresh.set_method_thunks(method_thunks());
        fresh.set_font_reuse_enabled(reuse);
        GuestFontLoader loader(memory, cpu, fresh, true);
        (void)memory.unmap_region(GuestFontLoader::BASE, GuestFontLoader::REGION_SIZE);
        ASSERT_TRUE(loader.set_up(IID_ADDRESS));
        ASSERT_TRUE(loader.add_file(courier, 1));
        ASSERT_TRUE(loader.add_file(georgia, 1));
        for (int round = 0; round < 2; ++round) {
            auto [hr, collection] = loader.create_collection(1);
            ASSERT_EQ(hr, 0u);
            EXPECT_EQ(loader.family_count(collection), 2u);
            EXPECT_TRUE(loader.has_family(collection, u"Courier New"));
            EXPECT_TRUE(loader.has_family(collection, u"Georgia"));
            if (round == 0) {
                EXPECT_EQ(loader.log.front(), "CreateEnumeratorFromKey");
                EXPECT_EQ(std::count(loader.log.begin(), loader.log.end(),
                                     std::string("CreateStreamFromKey 1")), 1);
            } else {
                EXPECT_EQ(loader.log.empty(), reuse);
            }
        }
        EXPECT_TRUE(loader.violations.empty());
    }
}
