// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file dxgi_test_fixture.hpp
/// @brief DxgiManagerTest, the base fixture every DXGI frontend suite shares, and the GUIDs it needs.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Named (not anonymous) namespace: gtest requires every TEST_F of one suite to
// use the identical fixture type, so the fixture must be one type across the
// translation units of the suite.
namespace vitreo::winapi::dxgi_test {

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};
static_assert(sizeof(Guid) == 16);

constexpr Guid IID_IDXGIFACTORY1{
    0x770AAE78u, 0xF26Fu, 0x4DBAu,
    {0xA8, 0x29, 0x25, 0x3C, 0x83, 0xD1, 0xB3, 0x87}};
constexpr Guid IID_IDXGIFACTORY{
    0x7B7166ECu, 0x21C7u, 0x44AEu,
    {0xB2, 0x1A, 0xC9, 0xAE, 0x32, 0x1A, 0xE3, 0x69}};
constexpr Guid IID_IDXGIDEVICE{
    0x54EC77FAu, 0x1377u, 0x44E6u,
    {0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C}};
constexpr Guid IID_IDXGIADAPTER{
    0x2411E7E1u, 0x12ACu, 0x4CCFu,
    {0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0}};
constexpr Guid IID_IDXGIOUTPUT3{
    0x8A6BB301u, 0x7E7Eu, 0x41F4u,
    {0xA8, 0xE0, 0x5B, 0x32, 0xF7, 0xF9, 0x9B, 0x18}};
constexpr Guid IID_IDXGIOUTPUT6{
    0x068346E8u, 0xAAECu, 0x4B84u,
    {0xAD, 0xD7, 0x13, 0x7F, 0x51, 0x3F, 0x77, 0xA1}};
constexpr Guid PRIVATE_DATA_GUID{
    0xAABBCCDDu, 0x1122u, 0x3344u,
    {0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC}};

class DxgiManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = STACK_BASE + 0x800;
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
            // IDXGIFactory3..7. ensure_storage refuses a vtable with a zero
            // entry -- a guest call to address zero -- so a fixture that forgets
            // one makes every factory creation fail. That refusal has now caught
            // a missing thunk loop three times in this tree.
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
        method_thunks = thunks;
        manager.set_method_thunks(thunks);
        detected_manager.set_method_thunks(thunks);
        manager.set_output_resolver([] {
            return std::vector<dxgi::OutputInfo>{dxgi::OutputInfo{
                .device_name = "\\\\.\\DISPLAY1",
                .left = 0,
                .top = 0,
                .right = 2560,
                .bottom = 1440,
                .monitor = 0x000D0001,
                .bits_per_color = 10,
                .refresh_rate_hz = 120.0,
            }};
        });
        write_guid(IID_ADDRESS, IID_IDXGIFACTORY1);
    }

    void write_guid(GuestAddr address, const Guid& guid) {
        ASSERT_TRUE(memory.write_bytes(
            address, reinterpret_cast<const Byte*>(&guid), sizeof(guid)).is_ok());
    }

    GuestAddr create_factory() { return create_factory(manager); }

    GuestAddr create_factory(DxgiManager& target) {
        cpu.gpr.rcx = IID_ADDRESS;
        cpu.gpr.rdx = OUTPUT_ADDRESS;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        target.impl_CreateDXGIFactory1(marshaler);
        EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
        return memory.read_u64(OUTPUT_ADDRESS).value();
    }

    GuestAddr enum_adapter(GuestAddr factory) {
        return enum_adapter(manager, factory);
    }
    /// CreateDXGIFactory2(flags, riid, out) in the x64 convention.
    u32 create_factory2(u32 flags) {
        cpu.gpr.rcx = flags;
        cpu.gpr.rdx = IID_ADDRESS;
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        (void)memory.write_u64(OUTPUT_ADDRESS, 0);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_CreateDXGIFactory2(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }


    GuestAddr enum_adapter(DxgiManager& target, GuestAddr factory) {
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = 0;
        cpu.gpr.r8 = ADAPTER_ADDRESS;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        target.impl_EnumAdapters1(marshaler);
        EXPECT_EQ(cpu.gpr.rax, dxgi::S_OK);
        return memory.read_u64(ADAPTER_ADDRESS).value();
    }

    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr IID_ADDRESS = DATA_BASE + 0x100;
    static constexpr GuestAddr OUTPUT_ADDRESS = DATA_BASE + 0x200;
    static constexpr GuestAddr ADAPTER_ADDRESS = DATA_BASE + 0x208;
    static constexpr GuestAddr DESC_ADDRESS = DATA_BASE + 0x300;
    static constexpr GuestAddr PRIVATE_GUID_ADDRESS = DATA_BASE + 0x700;
    static constexpr GuestAddr PRIVATE_VALUE_ADDRESS = DATA_BASE + 0x720;
    static constexpr GuestAddr PRIVATE_SIZE_ADDRESS = DATA_BASE + 0x730;
    static constexpr GuestAddr PRIVATE_OUTPUT_ADDRESS = DATA_BASE + 0x740;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    static constexpr GuestAddr QUERY_THUNK = 0x70000010;
    static constexpr GuestAddr ADDREF_THUNK = 0x70000020;
    static constexpr GuestAddr RELEASE_THUNK = 0x70000030;
    static constexpr GuestAddr SET_PRIVATE_THUNK = 0x70000040;
    static constexpr GuestAddr SET_PRIVATE_INTERFACE_THUNK = 0x70000050;
    static constexpr GuestAddr GET_PRIVATE_THUNK = 0x70000060;
    static constexpr GuestAddr GET_PARENT_THUNK = 0x70000070;
    static constexpr GuestAddr ENUM_ADAPTERS_THUNK = 0x70000080;
    static constexpr GuestAddr MAKE_WINDOW_THUNK = 0x70000090;
    static constexpr GuestAddr GET_WINDOW_THUNK = 0x700000A0;
    static constexpr GuestAddr CREATE_SWAPCHAIN_THUNK = 0x700000B0;
    static constexpr GuestAddr CREATE_SOFTWARE_THUNK = 0x700000C0;
    static constexpr GuestAddr ENUM_ADAPTERS1_THUNK = 0x700000D0;
    static constexpr GuestAddr IS_CURRENT_THUNK = 0x700000E0;
    static constexpr GuestAddr ENUM_OUTPUTS_THUNK = 0x700000F0;
    static constexpr GuestAddr GET_DESC_THUNK = 0x70000100;
    static constexpr GuestAddr CHECK_SUPPORT_THUNK = 0x70000110;
    static constexpr GuestAddr GET_DESC1_THUNK = 0x70000120;
    static constexpr GuestAddr OUTPUT_METHOD_BASE = 0x70000200;

    DxgiManager::MethodThunks method_thunks{};
    GuestMemory memory;
    cpu::CpuState cpu;
    DxgiManager manager{
        memory, true,
        dxgi::AdapterInfo{
            .description = "Test Apple GPU",
            .vendor_id = dxgi::VENDOR_ID_APPLE,
            // The discrete-class profile detect_adapter_info builds: the
            // working set as dedicated video memory, nothing shared.
            .dedicated_video_memory = 8ull * 1024 * 1024 * 1024,
        }};
    /// A manager that detects its own adapter info, so the per-capsule
    /// advertised identity actually reaches DXGI_ADAPTER_DESC.
    DxgiManager detected_manager{memory, true};
};

}  // namespace vitreo::winapi::dxgi_test
