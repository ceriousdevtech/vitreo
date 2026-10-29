// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_core_remote_proxy.cpp
/// @brief CoCreateInstance remote proxy tests: IUnknown, typed and automation proxies.

#include "com_test_common.hpp"
#include "com_test_memory_stream_fixture.hpp"

TEST_F(MemoryStreamTest, CoCreateInstanceBuildsIUnknownProxyForRemoteFactory) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xC40;
    constexpr GuestAddr factory = DATA_BASE + 0xD00;
    constexpr GuestAddr factory_vtable = DATA_BASE + 0xD20;
    constexpr GuestAddr object = DATA_BASE + 0xD80;
    constexpr GuestAddr object_vtable = DATA_BASE + 0xDA0;
    constexpr GuestAddr create_method = 0x00451000;
    constexpr GuestAddr release_method = 0x00452000;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid{0x00000000u, 0x0000u, 0x0000u,
        {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());
    ASSERT_TRUE(memory.write_u32(factory, factory_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(factory_vtable + 12, create_method).is_ok());
    ASSERT_TRUE(memory.write_u32(object, object_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(object_vtable + 8, release_method).is_ok());

    u32 creates = 0;
    u32 releases = 0;
    ipc::RemoteMemoryServer server(memory, vmm);
    server.set_guest_call_handler(
        [&](GuestAddr function,
            std::span<const u64> arguments,
            bool /*caller_elevated*/) -> std::optional<u64> {
            if (function == create_method && arguments.size() == 4) {
                ++creates;
                EXPECT_EQ(arguments[0], factory);
                EXPECT_EQ(arguments[1], 0u);
                Guid remote_iid{};
                EXPECT_TRUE(memory.read_bytes(
                    arguments[2], reinterpret_cast<Byte*>(&remote_iid),
                    sizeof(remote_iid)).is_ok());
                EXPECT_EQ(std::memcmp(&remote_iid, &iid, sizeof(iid)), 0);
                EXPECT_TRUE(memory.write_u32(arguments[3], object).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            if (function == release_method && arguments.size() == 1) {
                ++releases;
                EXPECT_EQ(arguments[0], object);
                return 0;
            }
            return std::nullopt;
        });
    ASSERT_TRUE(server.start(false));

    const auto class_table_root = std::filesystem::temp_directory_path() /
        ("vitreo-com-proxy-" + std::to_string(::getpid()));
    std::error_code class_table_error;
    std::filesystem::remove_all(class_table_root, class_table_error);
    manager.set_capsule_root(class_table_root.string());
    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    ipc::ComClassRegistration published(
        class_table_root.string(), class_id,
        static_cast<u32>(::getpid()), factory, 4, 1, 4, false);
    ASSERT_TRUE(published.valid());

    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto create = find_export("CoCreateInstance");
    auto release = find_export("Vitreo_RemoteProxy_Release");
    ASSERT_NE(create, exports.entries.end());
    ASSERT_NE(release, exports.entries.end());

    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr proxy = memory.read_u32(output_ptr).value();
    ASSERT_NE(proxy, 0u);
    EXPECT_TRUE(task_memory.owns(proxy));
    EXPECT_EQ(creates, 1u);

    set_args({static_cast<u32>(proxy)});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_FALSE(task_memory.owns(proxy));
    EXPECT_EQ(releases, 1u);
    std::filesystem::remove_all(class_table_root, class_table_error);
}

TEST_F(MemoryStreamTest, RemoteProxyQueryInterfaceBuildsTypedProxyAndKeepsIdentity) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_unknown_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr iid_typed_ptr = DATA_BASE + 0xC40;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xC60;
    constexpr GuestAddr identity_output_ptr = DATA_BASE + 0xC80;
    constexpr GuestAddr factory = DATA_BASE + 0xD00;
    constexpr GuestAddr factory_vtable = DATA_BASE + 0xD20;
    constexpr GuestAddr unknown_object = DATA_BASE + 0xD80;
    constexpr GuestAddr unknown_vtable = DATA_BASE + 0xDA0;
    constexpr GuestAddr typed_object = DATA_BASE + 0xDC0;
    constexpr GuestAddr typed_vtable = DATA_BASE + 0xDE0;
    constexpr GuestAddr create_method = 0x00451000;
    constexpr GuestAddr query_method = 0x00452000;
    constexpr GuestAddr release_method = 0x00453000;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid_unknown{0x00000000u, 0x0000u, 0x0000u,
        {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
    constexpr Guid iid_typed{0x87654321u, 0xDCBAu, 0x1234u,
        {0x90,0x10,0x21,0x32,0x43,0x54,0x65,0x76}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_unknown_ptr, iid_unknown).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_typed_ptr, iid_typed).is_ok());
    ASSERT_TRUE(memory.write_u32(factory, factory_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(factory_vtable + 12, create_method).is_ok());
    ASSERT_TRUE(memory.write_u32(unknown_object, unknown_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(unknown_vtable, query_method).is_ok());
    ASSERT_TRUE(memory.write_u32(unknown_vtable + 8, release_method).is_ok());
    ASSERT_TRUE(memory.write_u32(typed_object, typed_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(typed_vtable, query_method).is_ok());
    ASSERT_TRUE(memory.write_u32(typed_vtable + 8, release_method).is_ok());

    manager.set_automation_layout_resolver(
        [&](const Guid& requested)
            -> std::optional<ComCoreManager::AutomationInterfaceLayout> {
            EXPECT_EQ(std::memcmp(&requested, &iid_typed, sizeof(iid_typed)), 0);
            return ComCoreManager::AutomationInterfaceLayout{};
        });

    u32 queries = 0;
    u32 releases = 0;
    ipc::RemoteMemoryServer server(memory, vmm);
    server.set_guest_call_handler(
        [&](GuestAddr function,
            std::span<const u64> arguments,
            bool /*caller_elevated*/) -> std::optional<u64> {
            if (function == create_method && arguments.size() == 4) {
                EXPECT_TRUE(memory.write_u32(arguments[3], unknown_object).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            if (function == query_method && arguments.size() == 3) {
                ++queries;
                Guid requested{};
                EXPECT_TRUE(memory.read_bytes(
                    arguments[1], reinterpret_cast<Byte*>(&requested),
                    sizeof(requested)).is_ok());
                EXPECT_EQ(std::memcmp(&requested, &iid_typed, sizeof(iid_typed)), 0);
                EXPECT_TRUE(memory.write_u32(arguments[2], typed_object).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            if (function == release_method && arguments.size() == 1) {
                ++releases;
                EXPECT_TRUE(arguments[0] == unknown_object ||
                            arguments[0] == typed_object);
                return 0;
            }
            return std::nullopt;
        });
    ASSERT_TRUE(server.start(false));

    const auto class_table_root = std::filesystem::temp_directory_path() /
        ("vitreo-com-query-proxy-" + std::to_string(::getpid()));
    std::error_code class_table_error;
    std::filesystem::remove_all(class_table_root, class_table_error);
    manager.set_capsule_root(class_table_root.string());
    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    ipc::ComClassRegistration published(
        class_table_root.string(), class_id,
        static_cast<u32>(::getpid()), factory, 4, 1, 4, false);
    ASSERT_TRUE(published.valid());

    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto create = find_export("CoCreateInstance");
    auto query = find_export("Vitreo_RemoteProxy_QueryInterface");
    auto release = find_export("Vitreo_RemoteProxy_Release");
    ASSERT_NE(create, exports.entries.end());
    ASSERT_NE(query, exports.entries.end());
    ASSERT_NE(release, exports.entries.end());

    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_unknown_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr identity_proxy = memory.read_u32(output_ptr).value();
    ASSERT_NE(identity_proxy, 0u);

    set_args({static_cast<u32>(identity_proxy), static_cast<u32>(iid_typed_ptr),
              static_cast<u32>(output_ptr)});
    query->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr typed_proxy = memory.read_u32(output_ptr).value();
    ASSERT_NE(typed_proxy, 0u);
    EXPECT_NE(typed_proxy, identity_proxy);
    EXPECT_EQ(queries, 1u);

    // Querying IUnknown through the typed proxy preserves COM identity and is
    // handled by the client proxy without another round trip.
    set_args({static_cast<u32>(typed_proxy), static_cast<u32>(iid_unknown_ptr),
              static_cast<u32>(identity_output_ptr)});
    query->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(identity_output_ptr).value(), identity_proxy);
    EXPECT_EQ(queries, 1u);

    set_args({static_cast<u32>(identity_proxy)});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    set_args({static_cast<u32>(typed_proxy)});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    set_args({static_cast<u32>(identity_proxy)});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(releases, 2u);
    std::filesystem::remove_all(class_table_root, class_table_error);
}

TEST_F(MemoryStreamTest, RemoteAutomationProxyCopiesBstrAndScalarResults) {
    constexpr GuestAddr clsid_ptr = DATA_BASE + 0xC00;
    constexpr GuestAddr iid_ptr = DATA_BASE + 0xC20;
    constexpr GuestAddr output_ptr = DATA_BASE + 0xC40;
    constexpr GuestAddr factory = DATA_BASE + 0xD00;
    constexpr GuestAddr factory_vtable = DATA_BASE + 0xD20;
    constexpr GuestAddr object = DATA_BASE + 0xD80;
    constexpr GuestAddr object_vtable = DATA_BASE + 0xDA0;
    constexpr GuestAddr create_method = 0x00451000;
    constexpr GuestAddr release_method = 0x00452000;
    constexpr GuestAddr scalar_method = 0x00453000;
    constexpr GuestAddr bstr_result_method = 0x00454000;
    constexpr GuestAddr scalar_result_method = 0x00455000;
    constexpr GuestAddr local_bstr_block = DATA_BASE + 0xE00;
    constexpr GuestAddr local_bstr = local_bstr_block + 4;
    constexpr GuestAddr remote_bstr_block = DATA_BASE + 0xE40;
    constexpr GuestAddr remote_bstr = remote_bstr_block + 4;
    constexpr GuestAddr bstr_output = DATA_BASE + 0xE80;
    constexpr GuestAddr scalar_output = DATA_BASE + 0xE90;
    constexpr GuestAddr local_lpwstr = DATA_BASE + 0xEA0;
    constexpr GuestAddr local_lpstr = DATA_BASE + 0xEC0;
    constexpr u32 scalar_result = 0x00040001;
    constexpr Guid clsid{0x12345678u, 0xABCDu, 0x4321u,
        {0x80,0x00,0x11,0x22,0x33,0x44,0x55,0x66}};
    constexpr Guid iid{0x87654321u, 0xDCBAu, 0x1234u,
        {0x90,0x10,0x21,0x32,0x43,0x54,0x65,0x76}};
    ASSERT_TRUE(marshaler.write_struct<Guid>(clsid_ptr, clsid).is_ok());
    ASSERT_TRUE(marshaler.write_struct<Guid>(iid_ptr, iid).is_ok());
    ASSERT_TRUE(memory.write_u32(factory, factory_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(factory_vtable + 12, create_method).is_ok());
    ASSERT_TRUE(memory.write_u32(object, object_vtable).is_ok());
    ASSERT_TRUE(memory.write_u32(object_vtable + 8, release_method).is_ok());
    ASSERT_TRUE(memory.write_u32(object_vtable + 12, scalar_method).is_ok());
    ASSERT_TRUE(memory.write_u32(
        object_vtable + 16, bstr_result_method).is_ok());
    ASSERT_TRUE(memory.write_u32(
        object_vtable + 20, scalar_result_method).is_ok());
    constexpr std::array<u16, 6> string_data{'a', 'l', 'p', 'h', 'a', 0};
    ASSERT_TRUE(memory.write_u32(local_bstr_block, 10).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        local_bstr, reinterpret_cast<const Byte*>(string_data.data()),
        sizeof(string_data)).is_ok());
    constexpr std::array<u16, 5> result_data{'b', 'e', 't', 'a', 0};
    ASSERT_TRUE(memory.write_u32(remote_bstr_block, 8).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        remote_bstr, reinterpret_cast<const Byte*>(result_data.data()),
        sizeof(result_data)).is_ok());
    constexpr std::array<u16, 5> wide_data{'w', 'i', 'd', 'e', 0};
    ASSERT_TRUE(memory.write_bytes(
        local_lpwstr, reinterpret_cast<const Byte*>(wide_data.data()),
        sizeof(wide_data)).is_ok());
    constexpr std::array<char, 5> ansi_data{'a', 'n', 's', 'i', 0};
    ASSERT_TRUE(memory.write_bytes(
        local_lpstr, reinterpret_cast<const Byte*>(ansi_data.data()),
        sizeof(ansi_data)).is_ok());

    manager.set_automation_layout_resolver(
        [&](const Guid& requested)
            -> std::optional<ComCoreManager::AutomationInterfaceLayout> {
            EXPECT_EQ(std::memcmp(&requested, &iid, sizeof(iid)), 0);
            ComCoreManager::AutomationMethodLayout method{};
            method.vtable_offset = 12;
            method.parameter_count = 4;
            method.parameter_vts = {
                3, 8, 31, 30};  // VT_I4, VT_BSTR, VT_LPWSTR, VT_LPSTR
            method.parameter_flags = {1, 1, 1, 1};  // PARAMFLAG_FIN
            method.name = "SetValue";
            ComCoreManager::AutomationMethodLayout bstr_result{};
            bstr_result.vtable_offset = 16;
            bstr_result.parameter_count = 1;
            bstr_result.parameter_vts = {8};  // VT_BSTR
            bstr_result.parameter_flags = {10};  // FOUT | FRETVAL
            bstr_result.name = "GetText";
            ComCoreManager::AutomationMethodLayout scalar_result_layout{};
            scalar_result_layout.vtable_offset = 20;
            scalar_result_layout.parameter_count = 1;
            scalar_result_layout.parameter_vts = {3};  // VT_I4
            scalar_result_layout.parameter_flags = {2};  // PARAMFLAG_FOUT
            scalar_result_layout.name = "GetValue";
            return ComCoreManager::AutomationInterfaceLayout{
                {method, bstr_result, scalar_result_layout}};
        });
    manager.set_automation_bstr_allocator(
        [&](Marshaler& target, std::span<const Byte> utf16_bytes) {
            const GuestAddr base = task_memory.allocate(
                4u + utf16_bytes.size() + 2u);
            if (base == 0 ||
                target.write_u32(
                    base, static_cast<u32>(utf16_bytes.size())).is_err() ||
                target.memory().write_bytes(
                    base + 4u, utf16_bytes.data(),
                    utf16_bytes.size()).is_err() ||
                target.write_u16(
                    base + 4u + utf16_bytes.size(), 0).is_err()) {
                return GuestAddr{0};
            }
            return base + 4u;
        });

    u32 creates = 0;
    u32 scalar_calls = 0;
    u32 releases = 0;
    ipc::RemoteMemoryServer server(memory, vmm);
    server.set_guest_call_handler(
        [&](GuestAddr function,
            std::span<const u64> arguments,
            bool /*caller_elevated*/) -> std::optional<u64> {
            if (function == create_method && arguments.size() == 4) {
                ++creates;
                EXPECT_EQ(arguments[0], factory);
                EXPECT_EQ(arguments[1], 0u);
                Guid remote_iid{};
                EXPECT_TRUE(memory.read_bytes(
                    arguments[2], reinterpret_cast<Byte*>(&remote_iid),
                    sizeof(remote_iid)).is_ok());
                EXPECT_EQ(std::memcmp(&remote_iid, &iid, sizeof(iid)), 0);
                EXPECT_TRUE(memory.write_u32(arguments[3], object).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            if (function == scalar_method && arguments.size() == 5) {
                ++scalar_calls;
                EXPECT_EQ(arguments[0], object);
                EXPECT_EQ(arguments[1], 42u);
                const GuestAddr copied_bstr = arguments[2];
                EXPECT_NE(copied_bstr, local_bstr);
                EXPECT_EQ(memory.read_u32(copied_bstr - 4).value(), 10u);
                std::array<u16, 6> copied_data{};
                EXPECT_TRUE(memory.read_bytes(
                    copied_bstr, reinterpret_cast<Byte*>(copied_data.data()),
                    sizeof(copied_data)).is_ok());
                EXPECT_EQ(copied_data, string_data);
                EXPECT_NE(arguments[3], local_lpwstr);
                std::array<u16, 5> copied_wide{};
                EXPECT_TRUE(memory.read_bytes(
                    arguments[3], reinterpret_cast<Byte*>(copied_wide.data()),
                    sizeof(copied_wide)).is_ok());
                EXPECT_EQ(copied_wide, wide_data);
                EXPECT_NE(arguments[4], local_lpstr);
                std::array<char, 5> copied_ansi{};
                EXPECT_TRUE(memory.read_bytes(
                    arguments[4], reinterpret_cast<Byte*>(copied_ansi.data()),
                    sizeof(copied_ansi)).is_ok());
                EXPECT_EQ(copied_ansi, ansi_data);
                return static_cast<u64>(scalar_result);
            }
            if (function == bstr_result_method && arguments.size() == 2) {
                EXPECT_TRUE(memory.write_u32(
                    arguments[1], remote_bstr).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            if (function == scalar_result_method && arguments.size() == 2) {
                EXPECT_TRUE(memory.write_u32(arguments[1], 77).is_ok());
                return static_cast<u64>(com::S_OK);
            }
            if (function == release_method && arguments.size() == 1) {
                ++releases;
                EXPECT_EQ(arguments[0], object);
                return 0;
            }
            return std::nullopt;
        });
    ASSERT_TRUE(server.start(false));

    const auto class_table_root = std::filesystem::temp_directory_path() /
        ("vitreo-com-scalar-proxy-" + std::to_string(::getpid()));
    std::error_code class_table_error;
    std::filesystem::remove_all(class_table_root, class_table_error);
    manager.set_capsule_root(class_table_root.string());
    ipc::ComClassId class_id{};
    std::memcpy(class_id.data(), &clsid, sizeof(clsid));
    ipc::ComClassRegistration published(
        class_table_root.string(), class_id,
        static_cast<u32>(::getpid()), factory, 4, 1, 4, false);
    ASSERT_TRUE(published.valid());

    auto exports = create_com_core_thunks(manager);
    const auto find_export = [&](std::string_view name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
    };
    auto create = find_export("CoCreateInstance");
    auto release = find_export("Vitreo_RemoteProxy_Release");
    ASSERT_NE(create, exports.entries.end());
    ASSERT_NE(release, exports.entries.end());

    set_args({static_cast<u32>(clsid_ptr), 0, 4,
              static_cast<u32>(iid_ptr), static_cast<u32>(output_ptr)});
    create->handler(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr proxy = memory.read_u32(output_ptr).value();
    ASSERT_NE(proxy, 0u);
    const GuestAddr proxy_vtable = memory.read_u32(proxy).value();
    const GuestAddr method_alias = memory.read_u32(proxy_vtable + 12).value();
    ASSERT_NE(method_alias, 0u);
    EXPECT_EQ(registry.resolve_stdcall_bytes(method_alias), 20u);
    auto method_handler = registry.resolve_handler(method_alias);
    ASSERT_TRUE(method_handler.is_ok());

    set_args({static_cast<u32>(proxy), 42, static_cast<u32>(local_bstr),
              static_cast<u32>(local_lpwstr), static_cast<u32>(local_lpstr)});
    cpu.rip = method_alias;
    (*method_handler.value())(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), scalar_result);
    EXPECT_EQ(creates, 1u);
    EXPECT_EQ(scalar_calls, 1u);

    const GuestAddr bstr_result_alias =
        memory.read_u32(proxy_vtable + 16).value();
    auto bstr_result_handler = registry.resolve_handler(bstr_result_alias);
    ASSERT_TRUE(bstr_result_handler.is_ok());
    ASSERT_TRUE(memory.write_u32(bstr_output, 0xCCCCCCCCu).is_ok());
    set_args({static_cast<u32>(proxy), static_cast<u32>(bstr_output)});
    cpu.rip = bstr_result_alias;
    (*bstr_result_handler.value())(marshaler);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    const GuestAddr copied_result = memory.read_u32(bstr_output).value();
    ASSERT_NE(copied_result, 0u);
    EXPECT_NE(copied_result, remote_bstr);
    EXPECT_EQ(memory.read_u32(copied_result - 4).value(), 8u);
    std::array<u16, 5> copied_result_data{};
    ASSERT_TRUE(memory.read_bytes(
        copied_result, reinterpret_cast<Byte*>(copied_result_data.data()),
        sizeof(copied_result_data)).is_ok());
    EXPECT_EQ(copied_result_data, result_data);

    const GuestAddr scalar_result_alias =
        memory.read_u32(proxy_vtable + 20).value();
    auto scalar_result_handler = registry.resolve_handler(scalar_result_alias);
    ASSERT_TRUE(scalar_result_handler.is_ok());
    ASSERT_TRUE(memory.write_u32(scalar_output, 0xCCCCCCCCu).is_ok());
    set_args({static_cast<u32>(proxy), static_cast<u32>(scalar_output)});
    cpu.rip = scalar_result_alias;
    (*scalar_result_handler.value())(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), com::S_OK);
    EXPECT_EQ(memory.read_u32(scalar_output).value(), 77u);

    set_args({static_cast<u32>(proxy)});
    release->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_FALSE(task_memory.owns(proxy));
    EXPECT_EQ(releases, 1u);
    std::filesystem::remove_all(class_table_root, class_table_error);
}
