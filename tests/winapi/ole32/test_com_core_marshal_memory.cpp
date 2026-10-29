// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_core_marshal_memory.cpp
/// @brief Local-server publication, proxy/stub registration, marshal-data release, MTA
/// usage, CoGetMalloc, task memory and SHCreateMemStream tests.

#include "com_test_common.hpp"
#include "com_test_memory_stream_fixture.hpp"

TEST_F(MemoryStreamTest, CoCreateInstanceWaitsForLocalServerPublication) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xC40;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid{0x00000000u, 0x0000u, 0x0000u,
        {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());

    const auto class_table_root = std::filesystem::temp_directory_path() /
        ("vitreo-com-launch-wait-" + std::to_string(::getpid()));
    std::error_code class_table_error;
    std::filesystem::remove_all(class_table_root, class_table_error);
    manager.set_capsule_root(class_table_root.string());
    manager.set_local_server_registration_timeout(
        std::chrono::milliseconds(50));

    capsule::Registry capsule_registry;
    manager.set_registry(&capsule_registry);
    auto server_key = capsule_registry.open_key(
        capsule::HKEY_CURRENT_USER,
        L"Software\\Classes\\CLSID\\{12345678-ABCD-4321-8000-112233445566}"
        L"\\LocalServer32",
        true);
    ASSERT_TRUE(server_key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        server_key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"C:\\Program Files\\Contract\\server.exe")})
                    .is_ok());

    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    std::unique_ptr<ipc::ComClassRegistration> published;
    u32 activations = 0;
    manager.set_local_server_activator(
        [&](const std::wstring&, const std::wstring&, u32& error) {
            ++activations;
            published = std::make_unique<ipc::ComClassRegistration>(
                class_table_root.string(), class_id, 23, 0x12346000,
                4, 1, 4, false);
            error = 0;
            return published->valid();
        });

    auto exports = create_com_core_thunks(manager);
    auto create = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& entry) {
            return entry.name == "CoCreateInstance";
        });
    ASSERT_NE(create, exports.entries.end());
    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              com::CLASS_E_CLASSNOTAVAILABLE);
    EXPECT_EQ(memory.read_u32(output_ptr).value(), 0u);
    EXPECT_EQ(activations, 1u);
    published.reset();
    std::filesystem::remove_all(class_table_root, class_table_error);
}

TEST_F(MemoryStreamTest, ProxyStubRegistrationOverridesRegistryLookup) {
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xD00;
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xD20;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xD40;
    constexpr Guid iid{0x11111111u, 0x2222u, 0x3333u,
        {0x84,0x44,0x55,0x66,0x77,0x88,0x99,0xAA}};
    constexpr Guid first{0xAAAAAAAAu, 0xBBBBu, 0xCCCCu,
        {0x8D,0xDD,0xEE,0xFF,0x10,0x20,0x30,0x40}};
    constexpr Guid replacement{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x01,0x02,0x03,0x04,0x05,0x06}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());

    capsule::Registry capsule_registry;
    manager.set_registry(&capsule_registry);
    auto exports = create_com_core_thunks(manager);
    auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto register_ps = find_export("CoRegisterPSClsid");
    auto get_ps = find_export("CoGetPSClsid");
    ASSERT_NE(register_ps, exports.entries.end());
    ASSERT_NE(get_ps, exports.entries.end());

    for (const Guid& clsid : {first, replacement}) {
        ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
        set_args({static_cast<u32>(iid_ptr), static_cast<u32>(clsid_ptr)});
        register_ps->handler(marshaler);
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    }
    set_args({static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    get_ps->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    auto registered_output = marshaler.read_struct<Guid>(output_ptr);
    ASSERT_TRUE(registered_output.is_ok());
    EXPECT_EQ(std::memcmp(&registered_output.value(), &replacement,
                          sizeof(Guid)), 0);

    constexpr Guid registry_iid{0x22222222u, 0x3333u, 0x4444u,
        {0x85,0x55,0x66,0x77,0x88,0x99,0xAA,0xBB}};
    constexpr Guid registry_clsid{0x87654321u, 0xDCBAu, 0x1234u,
        {0x80,0x00,0x10,0x20,0x30,0x40,0x50,0x60}};
    auto key = capsule_registry.open_key(
        capsule::HKEY_CLASSES_ROOT,
        L"Interface\\{22222222-3333-4444-8555-66778899AABB}\\ProxyStubClsid32",
        true);
    ASSERT_TRUE(key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"{87654321-DCBA-1234-8000-102030405060}")}).is_ok());
    capsule_registry.close_key(key.value());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, registry_iid).is_ok());
    set_args({static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    get_ps->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    auto registry_output = marshaler.read_struct<Guid>(output_ptr);
    ASSERT_TRUE(registry_output.is_ok());
    EXPECT_EQ(std::memcmp(&registry_output.value(), &registry_clsid,
                          sizeof(Guid)), 0);

    constexpr Guid missing_iid{0x33333333u, 0x4444u, 0x5555u,
        {0x86,0x66,0x77,0x88,0x99,0xAA,0xBB,0xCC}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, missing_iid).is_ok());
    set_args({static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    get_ps->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80040155u);
}

TEST_F(MemoryStreamTest, ReleaseMarshalDataConsumesPacketButNotStream) {
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xE00;
    constexpr GuestAddr object = DATA_BASE + 0xE20;
    constexpr GuestAddr object_vtable = DATA_BASE + 0xE40;
    constexpr GuestAddr stream_out = DATA_BASE + 0xE80;
    constexpr GuestAddr addref = 0x00431000;
    constexpr GuestAddr release_method = 0x00432000;
    constexpr Guid iid{0x11111111u, 0x2222u, 0x3333u,
        {0x80,0x00,0x44,0x55,0x66,0x77,0x88,0x99}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());
    ASSERT_TRUE(memory.write_u32(object, object_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(object_vtable + 4, addref).is_ok());
    ASSERT_TRUE(memory.write_u32(object_vtable + 8, release_method).is_ok());

    std::vector<GuestAddr> calls;
    manager.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) {
            EXPECT_FALSE(is_64bit);
            EXPECT_EQ(count, 1u);
            EXPECT_EQ(args[0], object);
            calls.push_back(method);
            return static_cast<u64>(1);
        });
    auto exports = create_com_core_thunks(manager);
    auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto marshal = find_export("CoMarshalInterThreadInterfaceInStream");
    auto release_data = find_export("CoReleaseMarshalData");
    ASSERT_NE(marshal, exports.entries.end());
    ASSERT_NE(release_data, exports.entries.end());
    EXPECT_EQ(release_data->stdcall_bytes, 4u);

    set_args({static_cast<u32>(iid_ptr), static_cast<u32>(object),
              static_cast<u32>(stream_out)});
    marshal->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr stream = memory.read_u32(stream_out).value();
    ASSERT_NE(stream, 0u);
    EXPECT_TRUE(task_memory.owns(stream));
    EXPECT_EQ(calls, (std::vector<GuestAddr>{addref}));

    set_args({static_cast<u32>(stream)});
    release_data->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{addref, release_method}));
    EXPECT_TRUE(task_memory.owns(stream));

    release_data->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);
    EXPECT_EQ(calls.size(), 2u);

    manager.memory_stream_release(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_FALSE(task_memory.owns(stream));
}

TEST_F(MemoryStreamTest, MTAUsageExportsWriteX86CookieAndBalanceOnce) {
    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto increment = find_export("CoIncrementMTAUsage");
    const auto decrement = find_export("CoDecrementMTAUsage");
    ASSERT_NE(increment, exports.entries.end());
    ASSERT_NE(decrement, exports.entries.end());
    EXPECT_EQ(increment->stdcall_bytes, 4u);
    EXPECT_EQ(decrement->stdcall_bytes, 4u);

    constexpr GuestAddr cookie_out = DATA_BASE + 0x40;
    ASSERT_TRUE(memory.write_u64(cookie_out, 0xCCCCCCCCCCCCCCCCull).is_ok());
    set_args({static_cast<u32>(cookie_out)});
    increment->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const u32 cookie = memory.read_u32(cookie_out).value();
    EXPECT_NE(cookie, 0u);
    EXPECT_EQ(memory.read_u32(cookie_out + 4).value(), 0xCCCCCCCCu);

    set_args({cookie});
    decrement->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    set_args({cookie});
    decrement->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);

    set_args({0});
    increment->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);
}

TEST_F(MemoryStreamTest, CoGetMallocReturnsIMallocBackedByTaskMemory) {
    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto get_malloc = find_export("CoGetMalloc");
    const auto alloc_method = find_export("Vitreo_IMalloc_Alloc");
    const auto free_method = find_export("Vitreo_IMalloc_Free");
    const auto did_alloc = find_export("Vitreo_IMalloc_DidAlloc");
    const auto get_size = find_export("Vitreo_IMalloc_GetSize");
    ASSERT_NE(get_malloc, exports.entries.end());
    ASSERT_NE(alloc_method, exports.entries.end());
    ASSERT_NE(free_method, exports.entries.end());
    ASSERT_NE(did_alloc, exports.entries.end());
    ASSERT_NE(get_size, exports.entries.end());

    constexpr GuestAddr OUT_PTR = DATA_BASE + 0x40;
    set_args({0, static_cast<u32>(OUT_PTR)});  // MEMCTX must be 1
    get_malloc->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::E_INVALIDARG);

    ASSERT_TRUE(memory.write_u32(OUT_PTR, 0).is_ok());
    set_args({1, static_cast<u32>(OUT_PTR)});
    get_malloc->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr imalloc = memory.read_u32(OUT_PTR).value();
    ASSERT_NE(imalloc, 0u);

    set_args({static_cast<u32>(imalloc), 48});
    alloc_method->handler(marshaler);
    const GuestAddr block = cpu.gpr.rax;
    ASSERT_NE(block, 0u);
    EXPECT_TRUE(task_memory.owns(block));
    EXPECT_EQ(task_memory.allocation_size(block), 48u);

    set_args({static_cast<u32>(imalloc), static_cast<u32>(block)});
    get_size->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 48u);
    did_alloc->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    set_args({static_cast<u32>(imalloc), static_cast<u32>(block)});
    free_method->handler(marshaler);
    EXPECT_FALSE(task_memory.owns(block));
}

TEST_F(MemoryStreamTest, CoTaskMemoryExportsReallocatePreserveAndFreeGuestData) {
    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto alloc_export = find_export("CoTaskMemAlloc");
    const auto realloc_export = find_export("CoTaskMemRealloc");
    const auto free_export = find_export("CoTaskMemFree");
    ASSERT_NE(alloc_export, exports.entries.end());
    ASSERT_NE(realloc_export, exports.entries.end());
    ASSERT_NE(free_export, exports.entries.end());

    set_args({32});
    alloc_export->handler(marshaler);
    const GuestAddr original = cpu.gpr.rax;
    ASSERT_NE(original, 0u);
    ASSERT_EQ(task_memory.allocation_size(original), 32u);
    ASSERT_TRUE(memory.write_u32(original, 0xC0DEC0DEu).is_ok());

    set_args({static_cast<u32>(original), 64});
    realloc_export->handler(marshaler);
    const GuestAddr grown = cpu.gpr.rax;
    ASSERT_NE(grown, 0u);
    EXPECT_EQ(task_memory.allocation_size(grown), 64u);
    EXPECT_EQ(memory.read_u32(grown).value(), 0xC0DEC0DEu);

    set_args({static_cast<u32>(grown), 0});
    realloc_export->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_FALSE(task_memory.owns(grown));

    set_args({0, 0});
    realloc_export->handler(marshaler);
    const GuestAddr zero_size = cpu.gpr.rax;
    ASSERT_NE(zero_size, 0u);
    EXPECT_EQ(task_memory.allocation_size(zero_size), 1u);

    set_args({static_cast<u32>(zero_size)});
    free_export->handler(marshaler);
    EXPECT_FALSE(task_memory.owns(zero_size));

    set_args({0});
    free_export->handler(marshaler);
}

TEST_F(MemoryStreamTest, ShCreateMemStreamOrdinal12ProvidesFunctionalIStream) {
    constexpr GuestAddr INITIAL = DATA_BASE + 0x100;
    constexpr GuestAddr READ_BUFFER = DATA_BASE + 0x200;
    constexpr GuestAddr COUNT = DATA_BASE + 0x300;
    constexpr GuestAddr POSITION = DATA_BASE + 0x308;
    constexpr GuestAddr CLONE_OUT = DATA_BASE + 0x310;
    constexpr GuestAddr WRITE_BUFFER = DATA_BASE + 0x320;
    const std::array<u8, 5> initial{{'a', 'b', 'c', 'd', 'e'}};
    ASSERT_TRUE(memory.write_bytes(
        INITIAL, initial.data(), initial.size()).is_ok());

    auto exports = create_shlwapi_thunks(nullptr, &manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.ordinal == 12;
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->name, "SHCreateMemStream");

    set_args({static_cast<u32>(INITIAL), static_cast<u32>(initial.size())});
    entry->handler(marshaler);
    const GuestAddr stream = cpu.gpr.rax;
    ASSERT_NE(stream, 0u);
    EXPECT_TRUE(task_memory.owns(stream));

    auto vtable = memory.read_u32(stream);
    ASSERT_TRUE(vtable.is_ok());
    auto read_thunk =
        registry.get_thunk_address("ole32.dll", "Vitreo_MemoryStream_Read");
    ASSERT_TRUE(read_thunk.is_ok());
    EXPECT_EQ(memory.read_u32(vtable.value() + 3 * 4).value(),
              static_cast<u32>(read_thunk.value()));

    set_args({static_cast<u32>(stream), static_cast<u32>(READ_BUFFER),
              3, static_cast<u32>(COUNT)});
    manager.memory_stream_read(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(COUNT).value(), 3u);
    std::array<u8, 3> first{};
    ASSERT_TRUE(memory.read_bytes(
        READ_BUFFER, first.data(), first.size()).is_ok());
    EXPECT_EQ(first, (std::array<u8, 3>{{'a', 'b', 'c'}}));

    set_args({static_cast<u32>(stream), static_cast<u32>(READ_BUFFER),
              4, static_cast<u32>(COUNT)});
    manager.memory_stream_read(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_FALSE);
    EXPECT_EQ(memory.read_u32(COUNT).value(), 2u);

    // Seek to the start and clone. The clone gets its own seek pointer while
    // sharing the same backing store.
    set_args({static_cast<u32>(stream), 0, 0, 0,
              static_cast<u32>(POSITION)});
    manager.memory_stream_seek(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u64(POSITION).value(), 0u);

    set_args({static_cast<u32>(stream), static_cast<u32>(CLONE_OUT)});
    manager.memory_stream_clone(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr clone = memory.read_u32(CLONE_OUT).value();
    ASSERT_NE(clone, 0u);
    EXPECT_TRUE(task_memory.owns(clone));

    const std::array<u8, 2> replacement{{'X', 'Y'}};
    ASSERT_TRUE(memory.write_bytes(
        WRITE_BUFFER, replacement.data(), replacement.size()).is_ok());
    set_args({static_cast<u32>(clone), static_cast<u32>(WRITE_BUFFER),
              2, static_cast<u32>(COUNT)});
    manager.memory_stream_write(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);

    set_args({static_cast<u32>(stream), static_cast<u32>(READ_BUFFER),
              5, static_cast<u32>(COUNT)});
    manager.memory_stream_read(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    std::array<u8, 5> shared{};
    ASSERT_TRUE(memory.read_bytes(
        READ_BUFFER, shared.data(), shared.size()).is_ok());
    EXPECT_EQ(shared, (std::array<u8, 5>{{'X', 'Y', 'c', 'd', 'e'}}));

    set_args({static_cast<u32>(clone)});
    manager.memory_stream_release(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_FALSE(task_memory.owns(clone));
    set_args({static_cast<u32>(stream)});
    manager.memory_stream_release(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_FALSE(task_memory.owns(stream));
}
