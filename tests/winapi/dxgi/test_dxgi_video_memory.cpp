// SPDX-License-Identifier: LGPL-2.1-only
// Split from the former monolithic test_dxgi.cpp; the shared fixture lives
// in dxgi_test_fixture.hpp.

#include "dxgi_test_fixture.hpp"

#include <array>
#include <utility>

namespace vitreo::winapi::dxgi_test {
namespace {

// QueryVideoMemoryInfo under the discrete-class profile: LOCAL is the
// dedicated pool (Budget <= DedicatedVideoMemory), NON_LOCAL the empty
// system-memory pool behind SharedSystemMemory 0 (Learn D3D12_MEMORY_POOL:
// on a non-UMA adapter NON_LOCAL is L0, system memory). The NON_LOCAL zeros,
// the argument errors, the reservation refusal and the cookies are
// dxgi_adapter3_probe{64,32}.expected (Windows).
TEST_F(DxgiManagerTest, Adapter3QueryVideoMemoryInfoMatchesDiscreteProfile) {
    constexpr Guid IID_IDXGIADAPTER3{
        0x645967A4u, 0x1392u, 0x4310u,
        {0xA7, 0x98, 0x80, 0x53, 0xCE, 0x3E, 0x93, 0xFD}};
    constexpr GuestAddr INFO_ADDRESS = DESC_ADDRESS;
    constexpr GuestAddr COOKIE_ADDRESS = DATA_BASE + 0x210;
    constexpr u64 kDedicated = 8ull * 1024 * 1024 * 1024;

    const GuestAddr adapter = enum_adapter(create_factory());
    write_guid(IID_ADDRESS, IID_IDXGIADAPTER3);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_QueryInterface(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), adapter);

    constexpr Guid IID_IDXGIADAPTER4{
        0x3C8D99D1u, 0x4FBFu, 0x4181u,
        {0xA8, 0x2C, 0xAF, 0x66, 0xBF, 0x7B, 0xD2, 0x4E}};
    write_guid(IID_ADDRESS, IID_IDXGIADAPTER4);
    manager.impl_QueryInterface(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = DESC_ADDRESS;
    manager.impl_GetDesc3(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    // DXGI_ADAPTER_DESC3 x64: the DESC2 layout, Flags DXGI_ADAPTER_FLAG3.
    const u64 dedicated = memory.read_u64(DESC_ADDRESS + 272).value();
    const u64 shared = memory.read_u64(DESC_ADDRESS + 288).value();
    EXPECT_EQ(dedicated, kDedicated);
    EXPECT_EQ(memory.read_u64(DESC_ADDRESS + 280).value(), 0u);
    EXPECT_EQ(shared, 0u);
    EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 304).value(), 0u)
        << "DXGI_ADAPTER_FLAG3_NONE";
    write_guid(IID_ADDRESS, IID_IDXGIADAPTER3);

    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0; // DXGI_MEMORY_SEGMENT_GROUP_LOCAL
    cpu.gpr.r9 = INFO_ADDRESS;
    manager.impl_QueryVideoMemoryInfo(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS).value(), kDedicated);
    EXPECT_LE(memory.read_u64(INFO_ADDRESS).value(), dedicated)
        << "the LOCAL budget is the dedicated pool's";
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 8).value(), 0u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 16).value(), kDedicated / 2);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 24).value(), 0u);

    cpu.gpr.r8 = 1; // DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL
    manager.impl_QueryVideoMemoryInfo(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS).value(), shared)
        << "the NON_LOCAL budget is the SharedSystemMemory pool's";
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS).value(), 0u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 8).value(), 0u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 16).value(), 0u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 24).value(), 0u);

    cpu.gpr.rdx = 1; // NodeIndex 1
    cpu.gpr.r8 = 0;
    manager.impl_QueryVideoMemoryInfo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_INVALIDARG);

    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 2;
    manager.impl_QueryVideoMemoryInfo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_INVALIDARG);

    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 0;
    manager.impl_QueryVideoMemoryInfo(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_INVALIDARG);

    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 1;
    manager.impl_SetVideoMemoryReservation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_INVALIDARG);

    ASSERT_TRUE(memory.write_u32(COOKIE_ADDRESS, 0xDEADu).is_ok());
    cpu.gpr.rcx = adapter;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = COOKIE_ADDRESS;
    manager.impl_RegisterVideoMemoryBudgetChangeNotificationEvent(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u32(COOKIE_ADDRESS).value(), 0xDEADu);

    cpu.gpr.rdx = 0x100;
    manager.impl_RegisterVideoMemoryBudgetChangeNotificationEvent(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(COOKIE_ADDRESS).value(), 1u);

    cpu.gpr.r8 = 0;
    manager.impl_RegisterVideoMemoryBudgetChangeNotificationEvent(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::ERROR_INVALID_CALL);

    ASSERT_TRUE(memory.write_u32(COOKIE_ADDRESS, 0xDEADu).is_ok());
    cpu.gpr.r8 = COOKIE_ADDRESS;
    manager.impl_RegisterHardwareContentProtectionTeardownStatusEvent(
        marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u32(COOKIE_ADDRESS).value(), 1u);

    cpu.gpr.rdx = 1;
    manager.impl_UnregisterVideoMemoryBudgetChangeNotification(marshaler);
    manager.impl_UnregisterHardwareContentProtectionTeardownStatus(marshaler);
}

// The x86 layouts of the same discrete-class answers. DXGI_ADAPTER_DESC
// (dxgi.h) with 4-byte SIZE_Ts: DedicatedVideoMemory / DedicatedSystemMemory
// / SharedSystemMemory at 272 / 276 / 280, AdapterLuid at 284, size 292;
// DESC1 adds Flags at 292; DESC2 the two preemption granularities at 296 and
// 300. DXGI_QUERY_VIDEO_MEMORY_INFO is four UINT64s on both bitnesses
// (dxgi1_4.h). The working set here fits a 32-bit SIZE_T.
TEST_F(DxgiManagerTest, Adapter3QueryVideoMemoryInfoX86Layout) {
    constexpr Guid IID_IDXGIADAPTER3{
        0x645967A4u, 0x1392u, 0x4310u,
        {0xA7, 0x98, 0x80, 0x53, 0xCE, 0x3E, 0x93, 0xFD}};
    constexpr GuestAddr INFO_ADDRESS = DESC_ADDRESS;
    constexpr u64 kDedicated = 3ull * 1024 * 1024 * 1024;

    cpu.mode = cpu::CpuMode::Protected;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    DxgiManager x86{
        memory, false,
        dxgi::AdapterInfo{
            .description = "Test Apple GPU",
            .vendor_id = dxgi::VENDOR_ID_APPLE,
            .dedicated_video_memory = kDedicated,
        }};
    const DxgiManager::MethodThunks thunks{
        .query_interface = QUERY_THUNK,
        .add_ref = ADDREF_THUNK,
        .release = RELEASE_THUNK,
        .set_private_data = SET_PRIVATE_THUNK,
        .set_private_data_interface = SET_PRIVATE_INTERFACE_THUNK,
        .get_private_data = GET_PRIVATE_THUNK,
        .get_parent = GET_PARENT_THUNK,
        .enum_adapters = ENUM_ADAPTERS_THUNK,
        .make_window_association = MAKE_WINDOW_THUNK,
        .get_window_association = GET_WINDOW_THUNK,
        .create_swap_chain = CREATE_SWAPCHAIN_THUNK,
        .create_software_adapter = CREATE_SOFTWARE_THUNK,
        .enum_adapters1 = ENUM_ADAPTERS1_THUNK,
        .is_current = IS_CURRENT_THUNK,
        .is_windowed_stereo_enabled = IS_CURRENT_THUNK + 0x10,
        .create_swap_chain_for_hwnd = CREATE_SWAPCHAIN_THUNK + 0x10,
        .create_swap_chain_for_core_window = CREATE_SWAPCHAIN_THUNK + 0x20,
        .create_swap_chain_for_composition = CREATE_SWAPCHAIN_THUNK + 0x30,
        .get_shared_resource_adapter_luid = GET_DESC1_THUNK + 0x10,
        .register_stereo_status_window = GET_DESC1_THUNK + 0x20,
        .register_stereo_status_event = GET_DESC1_THUNK + 0x30,
        .unregister_stereo_status = GET_DESC1_THUNK + 0x40,
        .register_occlusion_status_window = GET_DESC1_THUNK + 0x50,
        .register_occlusion_status_event = GET_DESC1_THUNK + 0x60,
        .unregister_occlusion_status = GET_DESC1_THUNK + 0x70,
        .get_creation_flags = GET_DESC1_THUNK + 0x80,
        .enum_adapter_by_luid = GET_DESC1_THUNK + 0x90,
        .enum_warp_adapter = GET_DESC1_THUNK + 0xA0,
        .factory_check_feature_support = GET_DESC1_THUNK + 0xB0,
        .enum_adapter_by_gpu_preference = GET_DESC1_THUNK + 0xC0,
        .register_adapters_changed_event = GET_DESC1_THUNK + 0xD0,
        .unregister_adapters_changed_event = GET_DESC1_THUNK + 0xE0,
        .enum_outputs = ENUM_OUTPUTS_THUNK,
        .get_desc = GET_DESC_THUNK,
        .check_interface_support = CHECK_SUPPORT_THUNK,
        .get_desc1 = GET_DESC1_THUNK,
        .get_desc2 = GET_DESC1_THUNK + 0x80,
        .register_hwcp_teardown_event = GET_DESC1_THUNK + 0x100,
        .unregister_hwcp_teardown = GET_DESC1_THUNK + 0x110,
        .query_video_memory_info = GET_DESC1_THUNK + 0x120,
        .set_video_memory_reservation = GET_DESC1_THUNK + 0x130,
        .register_budget_change_event = GET_DESC1_THUNK + 0x140,
        .unregister_budget_change = GET_DESC1_THUNK + 0x150,
        .get_desc3 = GET_DESC1_THUNK + 0x160,
        .output = {
            OUTPUT_METHOD_BASE + 0x00, OUTPUT_METHOD_BASE + 0x10,
            OUTPUT_METHOD_BASE + 0x20, OUTPUT_METHOD_BASE + 0x30,
            OUTPUT_METHOD_BASE + 0x40, OUTPUT_METHOD_BASE + 0x50,
            OUTPUT_METHOD_BASE + 0x60, OUTPUT_METHOD_BASE + 0x70,
            OUTPUT_METHOD_BASE + 0x80, OUTPUT_METHOD_BASE + 0x90,
            OUTPUT_METHOD_BASE + 0xA0, OUTPUT_METHOD_BASE + 0xB0,
            OUTPUT_METHOD_BASE + 0xC0, OUTPUT_METHOD_BASE + 0xD0,
            OUTPUT_METHOD_BASE + 0xE0, OUTPUT_METHOD_BASE + 0xF0,
            OUTPUT_METHOD_BASE + 0x100, OUTPUT_METHOD_BASE + 0x110,
            OUTPUT_METHOD_BASE + 0x120, OUTPUT_METHOD_BASE + 0x130,
            OUTPUT_METHOD_BASE + 0x140, OUTPUT_METHOD_BASE + 0x150,
            OUTPUT_METHOD_BASE + 0x160, OUTPUT_METHOD_BASE + 0x170,
            OUTPUT_METHOD_BASE + 0x180, OUTPUT_METHOD_BASE + 0x190,
            OUTPUT_METHOD_BASE + 0x1A0, OUTPUT_METHOD_BASE + 0x1B0,
            OUTPUT_METHOD_BASE + 0x1C0,  // IDXGIOutput6::CheckHardwareCompositionSupport
        },
    };
    x86.set_method_thunks(thunks);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(IID_ADDRESS))
                    .is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, static_cast<u32>(OUTPUT_ADDRESS))
                    .is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    x86.impl_CreateDXGIFactory1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr factory = memory.read_u32(OUTPUT_ADDRESS).value();
    ASSERT_NE(factory, 0u);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(factory)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(
        memory.write_u32(cpu.sp() + 12, static_cast<u32>(ADAPTER_ADDRESS))
            .is_ok());
    x86.impl_EnumAdapters1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr adapter = memory.read_u32(ADAPTER_ADDRESS).value();
    ASSERT_NE(adapter, 0u);

    write_guid(IID_ADDRESS, IID_IDXGIADAPTER3);
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(adapter)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, static_cast<u32>(IID_ADDRESS))
                    .is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, static_cast<u32>(OUTPUT_ADDRESS))
                    .is_ok());
    x86.impl_QueryInterface(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);

    // GetDesc / GetDesc1 / GetDesc2 / GetDesc3(this, pDesc).
    using DescCall = void (DxgiManager::*)(Marshaler&);
    const std::array<std::pair<DescCall, u32>, 4> descs{{
        {&DxgiManager::impl_GetDesc, 292u},
        {&DxgiManager::impl_GetDesc1, 296u},
        {&DxgiManager::impl_GetDesc2, 304u},
        {&DxgiManager::impl_GetDesc3, 304u},
    }};
    for (const auto& [call, size] : descs) {
        for (u32 at = 0; at < 320; at += 4) {
            ASSERT_TRUE(memory.write_u32(DESC_ADDRESS + at, 0xCDCDCDCDu).is_ok());
        }
        ASSERT_TRUE(
            memory.write_u32(cpu.sp() + 4, static_cast<u32>(adapter)).is_ok());
        ASSERT_TRUE(memory.write_u32(cpu.sp() + 8,
                                     static_cast<u32>(DESC_ADDRESS)).is_ok());
        (x86.*call)(marshaler);
        ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK) << "size " << size;
        EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 256).value(),
                  dxgi::VENDOR_ID_APPLE);
        EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 272).value(), kDedicated)
            << "DedicatedVideoMemory, size " << size;
        EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 276).value(), 0u)
            << "DedicatedSystemMemory, size " << size;
        EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 280).value(), 0u)
            << "SharedSystemMemory, size " << size;
        EXPECT_NE(memory.read_u32(DESC_ADDRESS + 284).value(), 0xCDCDCDCDu)
            << "AdapterLuid.LowPart, size " << size;
        if (size >= 296) {
            EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 292).value(), 0u)
                << "Flags, size " << size;
        }
        if (size >= 304) {
            EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 296).value(), 0u);
            EXPECT_EQ(memory.read_u32(DESC_ADDRESS + 300).value(), 0u);
        }
        EXPECT_EQ(memory.read_u32(DESC_ADDRESS + size).value(), 0xCDCDCDCDu)
            << "nothing past the structure, size " << size;
    }

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(adapter)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, static_cast<u32>(INFO_ADDRESS))
                    .is_ok());
    x86.impl_QueryVideoMemoryInfo(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS).value(), kDedicated)
        << "LOCAL Budget = DedicatedVideoMemory";
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 8).value(), 0u);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 16).value(), kDedicated / 2);
    EXPECT_EQ(memory.read_u64(INFO_ADDRESS + 24).value(), 0u);

    // NON_LOCAL: SharedSystemMemory's pool, empty.
    for (u32 at = 0; at < 32; at += 4) {
        ASSERT_TRUE(memory.write_u32(INFO_ADDRESS + at, 0xCDCDCDCDu).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 1).is_ok());
    x86.impl_QueryVideoMemoryInfo(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    for (u32 at = 0; at < 32; at += 8) {
        EXPECT_EQ(memory.read_u64(INFO_ADDRESS + at).value(), 0u) << at;
    }
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 0).is_ok());

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 20, 0).is_ok());
    x86.impl_SetVideoMemoryReservation(marshaler);
    EXPECT_EQ(cpu.gpr.rax, dxgi::E_INVALIDARG);
}

}  // namespace
}  // namespace vitreo::winapi::dxgi_test
