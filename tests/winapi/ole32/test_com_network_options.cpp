// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_com_network_options.cpp
/// @brief IGlobalOptions and INetworkListManager tests (x86 and x64).

#include "com_test_common.hpp"
#include "com_test_shell_link_fixture.hpp"

TEST_F(ShellLinkEncodingTest, GlobalOptionsSetAndQueryProcessProperty) {
    constexpr Guid clsid_global_options{
        0x0000034Bu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    constexpr Guid iid_global_options{
        0x0000015Bu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    constexpr GuestAddr value_out = OUT_ADDR + 4;

    write_guid(CLSID_ADDR, clsid_global_options);
    write_guid(IID_ADDR, iid_global_options);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 options = read_u32(OUT_ADDR);
    ASSERT_NE(options, 0u);
    const u32 vtable = read_u32(options);
    for (u32 slot = 0; slot < 5; ++slot) {
        EXPECT_NE(read_u32(vtable + slot * 4), 0u);
    }

    // COMGLB_EXCEPTION_HANDLING=1, COMGLB_EXCEPTION_DONOT_HANDLE_ANY=3.
    set_args({options, 1, 3});
    auto set_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.global_options_set(set_marshaler), com::S_OK);

    set_args({options, 1, value_out});
    auto query_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.global_options_query(query_marshaler), com::S_OK);
    EXPECT_EQ(read_u32(value_out), 3u);

    set_args({options});
    auto release_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.global_options_release(release_marshaler), 0u);
}

TEST_F(ShellLinkEncodingTest, NetworkListManagerReportsInternetConnectivity) {
    constexpr Guid clsid_nlm{
        0xDCB00C01u, 0x570Fu, 0x4A9Bu,
        {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    constexpr Guid iid_nlm{
        0xDCB00000u, 0x570Fu, 0x4A9Bu,
        {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    constexpr GuestAddr connectivity_out = OUT_ADDR + 4;
    constexpr GuestAddr connected_out = OUT_ADDR + 8;

    write_guid(CLSID_ADDR, clsid_nlm);
    write_guid(IID_ADDR, iid_nlm);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 manager = read_u32(OUT_ADDR);
    ASSERT_NE(manager, 0u);
    const u32 vtable = read_u32(manager);
    for (u32 slot = 0; slot < 16; ++slot) {
        EXPECT_NE(read_u32(vtable + slot * 4), 0u) << "slot " << slot;
    }

    set_args({manager, connectivity_out});
    auto connectivity_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.network_list_manager_get_connectivity(connectivity_marshaler),
              com::S_OK);
    EXPECT_EQ(read_u32(connectivity_out), 0x70u);

    set_args({manager, connected_out});
    auto connected_marshaler = make_marshaler();
    EXPECT_EQ(
        mgr_.network_list_manager_get_is_connected(connected_marshaler),
        com::S_OK);
    EXPECT_EQ(read_u32(connected_out) & 0xFFFFu, 0xFFFFu);

    set_args({manager});
    auto release_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.network_list_manager_release(release_marshaler), 0u);
}

TEST_F(ShellLinkEncodingTest, NetworkConnectionEnumeratorIsDispatchDerived) {
    constexpr Guid clsid_nlm{
        0xDCB00C01u, 0x570Fu, 0x4A9Bu,
        {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    constexpr Guid iid_nlm{
        0xDCB00000u, 0x570Fu, 0x4A9Bu,
        {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    constexpr GuestAddr enum_out = OUT_ADDR + 4;
    constexpr GuestAddr fetched_out = OUT_ADDR + 8;
    constexpr GuestAddr element_out = OUT_ADDR + 12;

    write_guid(CLSID_ADDR, clsid_nlm);
    write_guid(IID_ADDR, iid_nlm);
    set_args({CLSID_ADDR, 0, 1, IID_ADDR, OUT_ADDR});
    auto create_marshaler = make_marshaler();
    ASSERT_EQ(mgr_.co_create_instance(create_marshaler), com::S_OK);
    const u32 manager = read_u32(OUT_ADDR);
    ASSERT_NE(manager, 0u);

    set_args({manager, enum_out});
    auto connections_marshaler = make_marshaler();
    ASSERT_EQ(
        mgr_.network_list_manager_get_network_connections(connections_marshaler),
        com::S_OK);
    const u32 enumerator = read_u32(enum_out);
    ASSERT_NE(enumerator, 0u);
    const u32 enum_vtable = read_u32(enumerator);
    ASSERT_EQ(enum_vtable, enumerator + 4);

    // IEnumNetworkConnections : IDispatch — IUnknown(3) + IDispatch(4) then
    // get__NewEnum, Next, Skip, Reset, Clone.
    const char* expected[] = {
        "Vitreo_NetworkListManager_Enum_QueryInterface",
        "Vitreo_NetworkListManager_Enum_AddRef",
        "Vitreo_NetworkListManager_Enum_Release",
        "Vitreo_NetworkListManager_GetTypeInfoCount",
        "Vitreo_NetworkListManager_GetTypeInfo",
        "Vitreo_NetworkListManager_GetIDsOfNames",
        "Vitreo_NetworkListManager_Invoke",
        "Vitreo_NetworkListManager_Enum_NewEnum",
        "Vitreo_NetworkListManager_Enum_Next",
        "Vitreo_NetworkListManager_Enum_Skip",
        "Vitreo_NetworkListManager_Enum_Reset",
        "Vitreo_NetworkListManager_Enum_Clone",
    };
    for (u32 slot = 0; slot < 12; ++slot) {
        auto expected_addr =
            registry_.get_thunk_address("ole32.dll", expected[slot]);
        ASSERT_TRUE(expected_addr.is_ok()) << expected[slot];
        EXPECT_EQ(read_u32(enum_vtable + slot * 4),
                  static_cast<u32>(expected_addr.value()))
            << "slot " << slot << " should be " << expected[slot];
    }

    // Next at slot 8 is the slot Chromium's network change notifier calls.
    // A connected host yields one INetworkConnection, then S_FALSE.
    ASSERT_TRUE(memory_.write_u32(fetched_out, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(memory_.write_u32(element_out, 0).is_ok());
    set_args({enumerator, 1, element_out, fetched_out});
    auto next_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.network_list_manager_enum_next(next_marshaler), com::S_OK);
    EXPECT_EQ(read_u32(fetched_out), 1u);
    const u32 connection = read_u32(element_out);
    EXPECT_NE(connection, 0u);

    constexpr GuestAddr connected_out = OUT_ADDR + 16;
    ASSERT_TRUE(memory_.write_u16(connected_out, 0).is_ok());
    set_args({connection, connected_out});
    auto connected = make_marshaler();
    EXPECT_EQ(mgr_.network_connection_get_is_connected(connected), com::S_OK);
    EXPECT_EQ(memory_.read_u16(connected_out).value(), 0xFFFFu);

    set_args({enumerator, 1, element_out, fetched_out});
    auto next_again = make_marshaler();
    EXPECT_EQ(mgr_.network_list_manager_enum_next(next_again), com::S_FALSE);
    EXPECT_EQ(read_u32(fetched_out), 0u);

    set_args({enumerator});
    auto release_marshaler = make_marshaler();
    EXPECT_EQ(mgr_.network_list_manager_enum_release(release_marshaler), 0u);
}

TEST(NetworkListManagerX64Test, CoCreateInstanceVtableUsesFull64BitPointers) {
    constexpr GuestAddr kData = 0x00100000;
    constexpr GuestAddr kStack = 0x00200000;
    constexpr GuestAddr kCom = ComCoreManager::COM_SCRATCH_BASE;
    constexpr GuestAddr kThunkBase = 0x7FFB00000000ull;
    constexpr GuestAddr kOut = kData + 0x80;
    constexpr Guid kClsid{
        0xDCB00C01u, 0x570Fu, 0x4A9Bu,
        {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};
    constexpr Guid kIid{
        0xDCB00000u, 0x570Fu, 0x4A9Bu,
        {0x8D, 0x69, 0x19, 0x9F, 0xDB, 0xA5, 0x72, 0x3B}};

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite)
                    .is_ok());
    ASSERT_TRUE(memory.map_region(kCom, 0x10000, MemoryProtection::ReadWrite)
                    .is_ok());
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_manager{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_manager};
    ComCoreManager mgr{task_memory};
    ThunkRegistry registry{kThunkBase};
    ASSERT_TRUE(
        registry.register_dll("ole32.dll", create_com_core_thunks(mgr)).is_ok());
    mgr.set_thunk_registry(&registry);
    (void)mgr.co_initialize();  // CoCreateInstance needs an apartment (measured)

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x400;
    ASSERT_TRUE(memory.write_bytes(
        kData, reinterpret_cast<const Byte*>(&kClsid), sizeof(kClsid)).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        kData + 0x20, reinterpret_cast<const Byte*>(&kIid), sizeof(kIid)).is_ok());
    cpu.gpr.rcx = kData;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = kData + 0x20;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kOut).is_ok());

    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_EQ(mgr.co_create_instance(marshaler), com::S_OK);
    const GuestAddr object = memory.read_u64(kOut).value();
    ASSERT_NE(object, 0u);
    const GuestAddr vtable = memory.read_u64(object).value();
    EXPECT_EQ(vtable, object + 8);
    for (u32 slot = 0; slot < 16; ++slot) {
        const u64 method = memory.read_u64(vtable + slot * 8).value();
        EXPECT_EQ(method & 0xFFFFFFFF00000000ull, kThunkBase) << "slot " << slot;
    }

    constexpr GuestAddr kQiOut = kOut + 0x10;
    cpu.gpr.rcx = object;
    cpu.gpr.rdx = kData + 0x20;
    cpu.gpr.r8 = kQiOut;
    Marshaler qi_marshaler{memory};
    qi_marshaler.attach_cpu(cpu);
    ASSERT_EQ(
        mgr.network_list_manager_query_interface(qi_marshaler), com::S_OK);
    EXPECT_EQ(memory.read_u64(kQiOut).value(), object);

    constexpr GuestAddr kEnumOut = kOut + 0x20;
    cpu.gpr.rcx = object;
    cpu.gpr.rdx = kEnumOut;
    Marshaler enum_marshaler{memory};
    enum_marshaler.attach_cpu(cpu);
    ASSERT_EQ(
        mgr.network_list_manager_get_network_connections(enum_marshaler),
        com::S_OK);
    const GuestAddr enumerator = memory.read_u64(kEnumOut).value();
    const GuestAddr enum_vtable = memory.read_u64(enumerator).value();
    EXPECT_EQ(enum_vtable, enumerator + 8);
    for (u32 slot = 0; slot < 12; ++slot) {
        const u64 method = memory.read_u64(enum_vtable + slot * 8).value();
        EXPECT_EQ(method & 0xFFFFFFFF00000000ull, kThunkBase) << "slot " << slot;
    }
}

// S7 audit: CLSID_GlobalOptions handed x64 callers a 4-byte vtable and a
// 4-byte *ppv, so Chromium/CEF's options->Set(COMGLB_EXCEPTION_HANDLING, ...)
// jumped through half a pointer. IGlobalOptions::Query writes a ULONG_PTR.
TEST(GlobalOptionsX64Test, CoCreateInstanceUsesPointerWidthVtableAndQuery) {
    constexpr GuestAddr kData = 0x00100000;
    constexpr GuestAddr kStack = 0x00200000;
    constexpr GuestAddr kCom = ComCoreManager::COM_SCRATCH_BASE;
    constexpr GuestAddr kThunkBase = 0x7FFB00000000ull;
    constexpr GuestAddr kOut = kData + 0x80;
    constexpr Guid kClsid{0x0000034Bu, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
    constexpr Guid kIid{0x0000015Bu, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kCom, 0x10000, MemoryProtection::ReadWrite).is_ok());
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap_manager{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap_manager};
    ComCoreManager mgr{task_memory};
    ThunkRegistry registry{kThunkBase};
    ASSERT_TRUE(registry.register_dll("ole32.dll", create_com_core_thunks(mgr)).is_ok());
    mgr.set_thunk_registry(&registry);
    (void)mgr.co_initialize();

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x400;
    ASSERT_TRUE(memory.write_bytes(kData, reinterpret_cast<const Byte*>(&kClsid), 16).is_ok());
    ASSERT_TRUE(memory.write_bytes(kData + 0x20, reinterpret_cast<const Byte*>(&kIid), 16).is_ok());
    ASSERT_TRUE(memory.write_u64(kOut, 0xAAAAAAAAAAAAAAAAull).is_ok());
    cpu.gpr.rcx = kData;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = kData + 0x20;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kOut).is_ok());
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_EQ(mgr.co_create_instance(marshaler), com::S_OK);
    const GuestAddr object = memory.read_u64(kOut).value();
    ASSERT_NE(object, 0u);
    ASSERT_EQ(object >> 32, 0u);  // the whole 8-byte *ppv was written
    const GuestAddr vtable = memory.read_u64(object).value();
    for (u32 slot = 0; slot < 5; ++slot) {
        EXPECT_EQ(memory.read_u64(vtable + slot * 8).value() & 0xFFFFFFFF00000000ull,
                  kThunkBase) << "slot " << slot;
    }

    // Set(COMGLB_EXCEPTION_HANDLING=1, COMGLB_EXCEPTION_DONOT_HANDLE_ANY=3)
    cpu.gpr.rcx = object;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = 3;
    Marshaler set{memory};
    set.attach_cpu(cpu);
    EXPECT_EQ(mgr.global_options_set(set), com::S_OK);
    constexpr GuestAddr kValue = kOut + 0x10;
    ASSERT_TRUE(memory.write_u64(kValue, 0xBBBBBBBBBBBBBBBBull).is_ok());
    cpu.gpr.rcx = object;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = kValue;
    Marshaler query{memory};
    query.attach_cpu(cpu);
    EXPECT_EQ(mgr.global_options_query(query), com::S_OK);
    EXPECT_EQ(memory.read_u64(kValue).value(), 3u);
}
