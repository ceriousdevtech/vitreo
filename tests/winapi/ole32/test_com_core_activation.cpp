// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_core_activation.cpp
/// @brief CoCreateInstance activation tests: registered factories, local services and
/// servers.

#include "com_test_common.hpp"
#include "com_test_memory_stream_fixture.hpp"

TEST_F(MemoryStreamTest, CoCreateInstanceUsesRegisteredClassFactory) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr factory = DATA_BASE + 0xC40;
    constexpr GuestAddr factory_vtable = DATA_BASE + 0xC80;
    constexpr GuestAddr cookie_ptr = DATA_BASE + 0xCC0;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xCC4;
    constexpr GuestAddr object = DATA_BASE + 0xD00;
    constexpr GuestAddr addref = 0x00421000;
    constexpr GuestAddr release_method = 0x00422000;
    constexpr GuestAddr create_instance = 0x00423000;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid{0x87654321u, 0xDCBAu, 0x1234u,
        {0x80,0x00,0x66,0x55,0x44,0x33,0x22,0x11}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());
    ASSERT_TRUE(memory.write_u32(factory, factory_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(factory_vtable + 4, addref).is_ok());
    ASSERT_TRUE(memory.write_u32(
        factory_vtable + 8, release_method).is_ok());
    ASSERT_TRUE(memory.write_u32(
        factory_vtable + 12, create_instance).is_ok());
    ASSERT_TRUE(memory.write_u32(output_ptr, 0xCCCCCCCCu).is_ok());

    std::vector<GuestAddr> calls;
    manager.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) {
            EXPECT_FALSE(is_64bit);
            calls.push_back(method);
            if (method == create_instance) {
                EXPECT_EQ(count, 4u);
                EXPECT_EQ(args[0], factory);
                EXPECT_EQ(args[1], 0u);
                EXPECT_EQ(args[2], iid_ptr);
                EXPECT_EQ(args[3], output_ptr);
                EXPECT_TRUE(memory.write_u32(output_ptr, object).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            EXPECT_EQ(count, 1u);
            EXPECT_EQ(args[0], factory);
            return static_cast<u64>(1);
        });

    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto register_class = find_export("CoRegisterClassObject");
    auto create = find_export("CoCreateInstance");
    auto resume = find_export("CoResumeClassObjects");
    auto add_server_ref = find_export("CoAddRefServerProcess");
    auto release_server_ref = find_export("CoReleaseServerProcess");
    ASSERT_NE(register_class, exports.entries.end());
    ASSERT_NE(create, exports.entries.end());
    ASSERT_NE(resume, exports.entries.end());
    ASSERT_NE(add_server_ref, exports.entries.end());
    ASSERT_NE(release_server_ref, exports.entries.end());

    // CLSCTX_LOCAL_SERVER is the context published by EXE class objects.
    // REGCLS_SUSPENDED holds activation until the server has registered every
    // class and calls CoResumeClassObjects.
    set_args({static_cast<u32>(clsid_ptr), static_cast<u32>(factory),
              4, 4, static_cast<u32>(cookie_ptr)});
    register_class->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);

    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              com::REGDB_E_CLASSNOTREG);

    set_args({});
    resume->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);

    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(output_ptr).value(), object);
    EXPECT_EQ(calls, (std::vector<GuestAddr>{
        addref, addref, create_instance, release_method}));

    set_args({});
    add_server_ref->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    set_args({});
    release_server_ref->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);

    // Reaching zero suspends every class object and prevents new activation.
    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              com::REGDB_E_CLASSNOTREG);
}

TEST_F(MemoryStreamTest, CoCreateInstanceStartsRegisteredLocalService) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xC40;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid{0x00000000u, 0x0000u, 0x0000u,
        {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());

    capsule::Registry capsule_registry;
    manager.set_registry(&capsule_registry);
    manager.set_local_server_registration_timeout(
        std::chrono::milliseconds(0));
    auto class_key = capsule_registry.open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"Software\\Classes\\CLSID\\{12345678-ABCD-4321-8000-112233445566}",
        true);
    ASSERT_TRUE(class_key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        class_key.value(), L"AppID", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"{87654321-DCBA-1234-8000-665544332211}")}).is_ok());
    auto appid_key = capsule_registry.open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"Software\\Classes\\AppID\\{87654321-DCBA-1234-8000-665544332211}",
        true);
    ASSERT_TRUE(appid_key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        appid_key.value(), L"LocalService", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"VitreoContractService")}).is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        appid_key.value(), L"ServiceParameters", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"--contract-parameter")}).is_ok());
    auto server_key = capsule_registry.open_key(
        capsule::HKEY_LOCAL_MACHINE,
        L"Software\\Classes\\CLSID\\{12345678-ABCD-4321-8000-112233445566}"
        L"\\LocalServer32",
        true);
    ASSERT_TRUE(server_key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        server_key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"C:\\Program Files\\Contract\\fallback.exe")})
                    .is_ok());

    u32 activations = 0;
    u32 local_server_activations = 0;
    manager.set_local_service_activator(
        [&](const std::wstring& name, const std::wstring& parameters,
            u32& error) {
            ++activations;
            EXPECT_EQ(name, L"VitreoContractService");
            EXPECT_EQ(parameters, L"--contract-parameter");
            error = 0;
            return true;
        });
    manager.set_local_server_activator(
        [&](const std::wstring&, const std::wstring&, u32&) {
            ++local_server_activations;
            return true;
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
              0x80080005u /* CO_E_SERVER_EXEC_FAILURE */);
    EXPECT_EQ(memory.read_u32(output_ptr).value(), 0u);
    EXPECT_EQ(activations, 1u);
    EXPECT_EQ(local_server_activations, 0u);
}

TEST_F(MemoryStreamTest, CoCreateInstanceStartsRegisteredLocalServer) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xC40;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid{0x00000000u, 0x0000u, 0x0000u,
        {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());

    capsule::Registry capsule_registry;
    manager.set_registry(&capsule_registry);
    manager.set_local_server_registration_timeout(
        std::chrono::milliseconds(0));
    auto server_key = capsule_registry.open_key(
        capsule::HKEY_CURRENT_USER,
        L"Software\\Classes\\CLSID\\{12345678-ABCD-4321-8000-112233445566}"
        L"\\LocalServer32",
        true);
    ASSERT_TRUE(server_key.is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        server_key.value(), L"", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"\"C:\\Program Files\\Contract\\server.exe\" "
                         L"--registered")}).is_ok());
    ASSERT_TRUE(capsule_registry.set_value(
        server_key.value(), L"ServerExecutable", capsule::RegValue{
            capsule::RegType::Sz,
            std::wstring(L"C:\\Program Files\\Contract\\server.exe")})
                    .is_ok());

    u32 activations = 0;
    manager.set_local_server_activator(
        [&](const std::wstring& executable, const std::wstring& command_line,
            u32& error) {
            ++activations;
            EXPECT_EQ(executable,
                      L"C:\\Program Files\\Contract\\server.exe");
            EXPECT_EQ(command_line,
                      L"\"C:\\Program Files\\Contract\\server.exe\" "
                      L"--registered -Embedding");
            error = 0;
            return true;
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
              0x80080005u /* CO_E_SERVER_EXEC_FAILURE */);
    EXPECT_EQ(memory.read_u32(output_ptr).value(), 0u);
    EXPECT_EQ(activations, 1u);
}

TEST_F(MemoryStreamTest, CoCreateInstanceUsesPublishedRemoteClassBeforeLaunch) {
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
        ("vitreo-com-remote-" + std::to_string(::getpid()));
    std::error_code class_table_error;
    std::filesystem::remove_all(class_table_root, class_table_error);
    manager.set_capsule_root(class_table_root.string());

    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    ipc::ComClassRegistration published(
        class_table_root.string(), class_id, 17, 0x12345000, 4, 1, 4,
        false);
    ASSERT_TRUE(published.valid());

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

    u32 activations = 0;
    manager.set_local_server_activator(
        [&](const std::wstring&, const std::wstring&, u32&) {
            ++activations;
            return true;
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
    EXPECT_EQ(activations, 0u);
    std::filesystem::remove_all(class_table_root, class_table_error);
}
