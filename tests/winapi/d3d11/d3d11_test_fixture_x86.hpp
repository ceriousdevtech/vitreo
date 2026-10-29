// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_fixture_x86.hpp
/// @brief D3D11ManagerX86Test: the fixture that drives the manager through the 32-bit stdcall frames.

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {

/// x86 CreateDevice: native-width vtables and pointer outputs (Spotify/CEF GPU).
class D3D11ManagerX86Test : public ::testing::Test {
protected:
    void SetUp() override {
        d3d11.set_handle_table(&handles);
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x3000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;

        DxgiManager::MethodThunks dxgi_thunks =
            sequential_dxgi_thunks(0x70000010);
        dxgi.set_method_thunks(dxgi_thunks);
        // Continue the D3D11 thunks past the block the DXGI fill consumed, so
        // no two interfaces share an address.
        GuestAddr thunk = 0x70000010 +
            (sizeof(DxgiManager::MethodThunks) / sizeof(GuestAddr)) * 0x10;

        D3D11Manager::MethodThunks d3d11_thunks{};
        for (GuestAddr& value : d3d11_thunks.device) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.context) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.dxgi_device) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.buffer) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.texture_2d) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.dxgi_resource) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.shader_resource_view) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.unordered_access_view) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.render_target_view) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.depth_stencil_view) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.query) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.swap_chain) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.swap_chain_media) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.annotation) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.vertex_shader) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.pixel_shader) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.compute_shader) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.input_layout) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.blend_state) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.rasterizer_state) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.depth_stencil_state) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.sampler_state) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.video_context) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.fence) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.command_list) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.geometry_shader) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.hull_shader) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.domain_shader) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.dxgi_surface) {
            value = thunk;
            thunk += 0x10;
        }
        for (GuestAddr& value : d3d11_thunks.device_context_state) {
            value = thunk;
            thunk += 0x10;
        }
        d3d11.set_method_thunks(d3d11_thunks);
    }

    void set_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(
            cpu.sp() + 4 + index * 4, value).is_ok());
    }

    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr DEVICE_OUT = DATA_BASE + 0x100;
    static constexpr GuestAddr LEVEL_OUT = DATA_BASE + 0x108;
    static constexpr GuestAddr CONTEXT_OUT = DATA_BASE + 0x110;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    cpu::CpuState cpu;
    TestGraphicsBackend backend;
    DxgiManager dxgi{
        memory, false,
        dxgi::AdapterInfo{
            .description = "Shared Test Metal GPU",
            .vendor_id = dxgi::VENDOR_ID_APPLE,
            .dedicated_video_memory = 8ull * 1024 * 1024 * 1024,
        }};
    HandleTable handles;
    D3D11Manager d3d11{memory, false, backend, dxgi};
};

}  // namespace vitreo::winapi::d3d11_test
