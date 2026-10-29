// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_core_registration.cpp
/// @brief Running object table, item monikers, ProgIDFromCLSID and class-object
/// registration tests.

#include "com_test_common.hpp"
#include "com_test_memory_stream_fixture.hpp"

TEST_F(MemoryStreamTest, GetRunningObjectTableReturnsOwnedProcessSingleton) {
    constexpr GuestAddr first_out = DATA_BASE + 0x700;
    constexpr GuestAddr second_out = DATA_BASE + 0x704;
    constexpr GuestAddr missing_moniker = DATA_BASE + 0x800;
    auto exports = create_com_core_thunks(manager);
    auto get_rot = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "GetRunningObjectTable";
        });
    auto is_running = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "Vitreo_ROT_IsRunning";
        });
    auto release = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "Vitreo_ROT_Release";
        });
    ASSERT_NE(get_rot, exports.entries.end());
    ASSERT_NE(is_running, exports.entries.end());
    ASSERT_NE(release, exports.entries.end());
    EXPECT_EQ(get_rot->stdcall_bytes, 8u);

    set_args({0, static_cast<u32>(first_out)});
    get_rot->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr first = memory.read_u32(first_out).value();
    ASSERT_NE(first, 0u);
    const GuestAddr vtable = memory.read_u32(first).value();
    ASSERT_NE(vtable, 0u);
    for (usize slot = 0; slot < 10; ++slot) {
        EXPECT_NE(memory.read_u32(vtable + slot * 4).value(), 0u);
    }

    set_args({0, static_cast<u32>(second_out)});
    get_rot->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(second_out).value(), first);

    set_args({static_cast<u32>(first), static_cast<u32>(missing_moniker)});
    is_running->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_FALSE);

    set_args({static_cast<u32>(first)});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 2u);
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);

    set_args({1, static_cast<u32>(first_out)});
    get_rot->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x8000FFFFu);
}

TEST_F(MemoryStreamTest, ItemMonikersExposeStableCaseInsensitiveIdentity) {
    constexpr GuestAddr delimiter = DATA_BASE + 0x900;
    constexpr GuestAddr first_item = DATA_BASE + 0x920;
    constexpr GuestAddr equal_item = DATA_BASE + 0x960;
    constexpr GuestAddr other_item = DATA_BASE + 0x9A0;
    constexpr GuestAddr outputs = DATA_BASE + 0xA00;
    constexpr GuestAddr hashes = DATA_BASE + 0xA20;
    constexpr GuestAddr display_out = DATA_BASE + 0xA40;
    constexpr GuestAddr system_type = DATA_BASE + 0xA44;
    ASSERT_TRUE(marshaler.write_wide_string(delimiter, "!").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(first_item, "EmbedObj1").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(equal_item, "embedobj1").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(other_item, "EmbedObj2").is_ok());

    auto exports = create_com_core_thunks(manager);
    auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto create = find_export("CreateItemMoniker");
    auto equal = find_export("Vitreo_ItemMoniker_IsEqual");
    auto hash = find_export("Vitreo_ItemMoniker_Hash");
    auto display = find_export("Vitreo_ItemMoniker_GetDisplayName");
    auto system = find_export("Vitreo_ItemMoniker_IsSystemMoniker");
    auto release = find_export("Vitreo_ItemMoniker_Release");
    ASSERT_NE(create, exports.entries.end());
    ASSERT_NE(equal, exports.entries.end());
    ASSERT_NE(hash, exports.entries.end());
    ASSERT_NE(display, exports.entries.end());
    ASSERT_NE(system, exports.entries.end());
    ASSERT_NE(release, exports.entries.end());

    const std::array<GuestAddr, 3> items{{first_item, equal_item, other_item}};
    std::array<GuestAddr, 3> monikers{};
    for (usize i = 0; i < items.size(); ++i) {
        set_args({static_cast<u32>(delimiter), static_cast<u32>(items[i]),
                  static_cast<u32>(outputs + i * 4)});
        create->handler(marshaler);
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
        monikers[i] = memory.read_u32(outputs + i * 4).value();
        ASSERT_NE(monikers[i], 0u);
    }

    set_args({static_cast<u32>(monikers[0]), static_cast<u32>(monikers[1])});
    equal->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    set_args({static_cast<u32>(monikers[0]), static_cast<u32>(monikers[2])});
    equal->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_FALSE);

    for (usize i = 0; i < 2; ++i) {
        set_args({static_cast<u32>(monikers[i]),
                  static_cast<u32>(hashes + i * 4)});
        hash->handler(marshaler);
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    }
    EXPECT_EQ(memory.read_u32(hashes).value(),
              memory.read_u32(hashes + 4).value());

    set_args({static_cast<u32>(monikers[0]), 0, 0,
              static_cast<u32>(display_out)});
    display->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr display_name = memory.read_u32(display_out).value();
    ASSERT_NE(display_name, 0u);
    EXPECT_EQ(marshaler.read_wide_string(display_name).value(), "!EmbedObj1");
    manager.co_task_mem_free_guest(display_name);

    set_args({static_cast<u32>(monikers[0]), static_cast<u32>(system_type)});
    system->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(system_type).value(), 4u);

    for (GuestAddr moniker : monikers) {
        set_args({static_cast<u32>(moniker)});
        release->handler(marshaler);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    }
}

TEST_F(MemoryStreamTest, ProgIDFromCLSIDReadsHKCRAndOwnsReturnedString) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xB00;
    constexpr GuestAddr out_ptr = DATA_BASE + 0xB20;
    constexpr GuestAddr progid_ptr = DATA_BASE + 0xB40;
    constexpr GuestAddr reverse_clsid_ptr = DATA_BASE + 0xB80;
    constexpr Guid clsid{0x12345678u, 0x1234u, 0x5678u,
        {0x90,0xAB,0xCD,0xEF,0x01,0x23,0x45,0x67}};
    capsule::Registry capsule_registry;
    manager.set_registry(&capsule_registry);
    auto key = capsule_registry.open_key(
        capsule::HKEY_CLASSES_ROOT,
        L"CLSID\\{12345678-1234-5678-90AB-CDEF01234567}\\ProgID",
        true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz, std::wstring(L"Vitreo.Contract.1")}).is_ok());
    capsule_registry.close_key(key.value());
    key = capsule_registry.open_key(
        capsule::HKEY_CLASSES_ROOT, L"Vitreo.Contract.1\\CLSID", true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"{12345678-1234-5678-90AB-CDEF01234567}")}).is_ok());
    capsule_registry.close_key(key.value());
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());

    auto exports = create_com_core_thunks(manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "ProgIDFromCLSID";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 8u);

    set_args({static_cast<u32>(clsid_ptr), static_cast<u32>(out_ptr)});
    entry->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr progid = memory.read_u32(out_ptr).value();
    ASSERT_NE(progid, 0u);
    EXPECT_EQ(marshaler.read_wide_string(progid).value(), "Vitreo.Contract.1");
    manager.co_task_mem_free_guest(progid);

    constexpr Guid missing{0x87654321u, 0x4321u, 0x8765u,
        {0x09,0xBA,0xDC,0xFE,0x10,0x32,0x54,0x76}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, missing).is_ok());
    ASSERT_TRUE(memory.write_u32(out_ptr, 0xCCCCCCCCu).is_ok());
    set_args({static_cast<u32>(clsid_ptr), static_cast<u32>(out_ptr)});
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80040154u);
    EXPECT_EQ(memory.read_u32(out_ptr).value(), 0u);

    auto reverse = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "CLSIDFromProgID";
        });
    ASSERT_NE(reverse, exports.entries.end());
    EXPECT_EQ(reverse->stdcall_bytes, 8u);
    ASSERT_TRUE(marshaler.write_wide_string(
        progid_ptr, "Vitreo.Contract.1").is_ok());
    set_args({static_cast<u32>(progid_ptr),
              static_cast<u32>(reverse_clsid_ptr)});
    reverse->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    auto reverse_clsid = marshaler.read_struct<Guid>(reverse_clsid_ptr);
    ASSERT_TRUE(reverse_clsid.is_ok());
    EXPECT_EQ(std::memcmp(&reverse_clsid.value(), &clsid, sizeof(Guid)), 0);

    const Guid poison{0xCCCCCCCCu, 0xCCCCu, 0xCCCCu,
        {0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(reverse_clsid_ptr, poison).is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(
        progid_ptr, "Vitreo.Contract.Missing").is_ok());
    reverse->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::CO_E_CLASSSTRING);
    reverse_clsid = marshaler.read_struct<Guid>(reverse_clsid_ptr);
    ASSERT_TRUE(reverse_clsid.is_ok());
    EXPECT_EQ(std::memcmp(&reverse_clsid.value(), &poison, sizeof(Guid)), 0);

    key = capsule_registry.open_key(
        capsule::HKEY_CLASSES_ROOT, L"Vitreo.Contract.Broken\\CLSID", true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz, std::wstring(L"not-a-clsid")}).is_ok());
    capsule_registry.close_key(key.value());
    ASSERT_TRUE(marshaler.write_wide_string(
        progid_ptr, "Vitreo.Contract.Broken").is_ok());
    reverse->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::CO_E_CLASSSTRING);
}

TEST_F(MemoryStreamTest, ClassObjectRegistrationsHaveIndependentOwnedCookies) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr factory = DATA_BASE + 0xC20;
    constexpr GuestAddr factory_vtable = DATA_BASE + 0xC40;
    constexpr GuestAddr first_cookie_ptr = DATA_BASE + 0xC80;
    constexpr GuestAddr second_cookie_ptr = DATA_BASE + 0xC84;
    constexpr GuestAddr addref = 0x00421000;
    constexpr GuestAddr release_method = 0x00422000;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    const auto class_table_root = std::filesystem::temp_directory_path() /
        ("vitreo-com-core-" + std::to_string(::getpid()));
    std::error_code class_table_error;
    std::filesystem::remove_all(class_table_root, class_table_error);
    manager.set_capsule_root(class_table_root.string());
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(memory.write_u32(factory, factory_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(factory_vtable + 4, addref).is_ok());
    ASSERT_TRUE(memory.write_u32(factory_vtable + 8, release_method).is_ok());

    std::vector<GuestAddr> calls;
    manager.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) {
            EXPECT_FALSE(is_64bit);
            EXPECT_EQ(count, 1u);
            EXPECT_EQ(args[0], factory);
            calls.push_back(method);
            return static_cast<u64>(1);
        });
    auto exports = create_com_core_thunks(manager);
    auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto register_class = find_export("CoRegisterClassObject");
    auto revoke_class = find_export("CoRevokeClassObject");
    ASSERT_NE(register_class, exports.entries.end());
    ASSERT_NE(revoke_class, exports.entries.end());
    EXPECT_EQ(register_class->stdcall_bytes, 20u);
    EXPECT_EQ(revoke_class->stdcall_bytes, 4u);

    for (GuestAddr cookie_ptr : {first_cookie_ptr, second_cookie_ptr}) {
        set_args({static_cast<u32>(clsid_ptr), static_cast<u32>(factory),
                  4, 1, static_cast<u32>(cookie_ptr)});
        register_class->handler(marshaler);
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    }
    const u32 first_cookie = memory.read_u32(first_cookie_ptr).value();
    const u32 second_cookie = memory.read_u32(second_cookie_ptr).value();
    EXPECT_NE(first_cookie, second_cookie);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{addref, addref}));
    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    ipc::ComClassSnapshot published{};
    ASSERT_TRUE(ipc::find_com_class(
        class_table_root.string(), class_id, 4, published));
    EXPECT_EQ(published.owner_pid, static_cast<u32>(::getpid()));
    EXPECT_TRUE(published.cookie == first_cookie ||
                published.cookie == second_cookie);

    set_args({first_cookie});
    revoke_class->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    set_args({second_cookie});
    revoke_class->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{
        addref, addref, release_method, release_method}));
    EXPECT_FALSE(ipc::find_com_class(
        class_table_root.string(), class_id, 4, published));
    std::filesystem::remove_all(class_table_root, class_table_error);

    set_args({first_cookie});
    revoke_class->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);
    EXPECT_EQ(calls.size(), 4u);
}
