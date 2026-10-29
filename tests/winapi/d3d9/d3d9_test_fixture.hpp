#pragma once

/// @file d3d9_test_fixture.hpp
/// @brief The D3D9ManagerTest fixture shared by the D3D9 frontend contract
///        tests: a D3D9Manager over guest memory and the recording backend.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <graphics/contracts/backend_device.hpp>
#include <winapi/graphics/d3d9/d3d9.hpp>
#include <winapi/graphics/d3d9/d3d9_state.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>

#include "d3d9_test_backend.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <string_view>
#include <utility>
#include <vector>

namespace vitreo::winapi::d3d9_test {

// =============================================================================
// Test Fixture
// =============================================================================

class D3D9ManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto res = memory_.map_region(MEM_BASE, 0x20000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        auto stack_res = memory_.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        cpu_.init_protected_mode();
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        marshaler_.attach_cpu(cpu_);
        mgr_.set_memory(&memory_, false);
        mgr_.set_backend(&backend_);
        mgr_.set_display_size_resolver(
            [](u32& width, u32& height) {
                width = 1920;
                height = 1080;
                return true;
            });
        mgr_.set_presentation_view_resolver(
            [this](u32 hwnd) -> void* {
                return hwnd == TEST_HWND ? &native_view_token_ : nullptr;
            });

        GuestAddr thunk = 0x70010000;
        for (usize index = 0; index < D3D9_INTERFACE_COUNT; ++index) {
            const auto iface = static_cast<D3D9InterfaceId>(index);
            const usize count = d3d9_interface_method_count(iface);
            D3D9Manager::InterfaceThunks thunks(count, GuestAddr{0});
            for (usize slot = 0; slot < count; ++slot) {
                thunks[slot] = thunk;
                thunk += 0x10;
            }
            interface_thunks_[index] = thunks;
            mgr_.set_interface_thunks(iface, std::move(thunks));
        }
        std::copy_n(interface_thunks_[0].begin(), D3D9_METHOD_COUNT,
                    method_thunks_.begin());
        exports_ = create_d3d9_thunks(mgr_);
    }

    ExportEntry& export_named(std::string_view name) {
        auto entry = std::find_if(
            exports_.entries.begin(), exports_.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        EXPECT_NE(entry, exports_.entries.end());
        return *entry;
    }

    u32 invoke(D3D9InterfaceId iface, usize slot,
               std::initializer_list<u32> args) {
        usize index = 0;
        for (const u32 argument : args) {
            EXPECT_TRUE(memory_.write_u32(
                cpu_.gpr.rsp + 4 + index * sizeof(u32), argument).is_ok());
            ++index;
        }
        export_named(d3d9_method_export_name(iface, slot)).handler(marshaler_);
        // DrawPrimitive / DrawIndexedPrimitive / Draw*UP / Clear / GPU
        // ColorFill enqueue on the D3D9 CS. Tests inspect the mock backend
        // immediately, so wait.
        if (iface == D3D9InterfaceId::Device &&
            (slot == 35 || slot == 43 || slot == 81 || slot == 82 ||
             slot == 83 || slot == 84)) {
            mgr_.d3d9_cs_flush();
        }
        return static_cast<u32>(cpu_.gpr.rax);
    }

    /// invoke() without its wait for the command stream: what the guest
    /// sees, with the worker still recording behind it.
    u32 invoke_queued(D3D9InterfaceId iface, usize slot,
                      std::initializer_list<u32> args) {
        usize index = 0;
        for (const u32 argument : args) {
            EXPECT_TRUE(memory_.write_u32(
                cpu_.gpr.rsp + 4 + index * sizeof(u32), argument).is_ok());
            ++index;
        }
        export_named(d3d9_method_export_name(iface, slot)).handler(marshaler_);
        return static_cast<u32>(cpu_.gpr.rax);
    }

    u32 invoke_method(usize slot, std::initializer_list<u32> args) {
        return invoke(D3D9InterfaceId::Direct3D9, slot, args);
    }

    /// Write a minimal windowed D3DPRESENT_PARAMETERS at PRESENT_ADDR.
    void write_present_parameters(u32 width, u32 height,
                                  bool auto_depth_stencil = false) {
        std::array<u32, 14> pp{};
        pp[0] = width;
        pp[1] = height;
        pp[2] = 22;  // D3DFMT_X8R8G8B8
        pp[3] = 1;   // BackBufferCount
        pp[6] = 1;   // D3DSWAPEFFECT_DISCARD
        pp[7] = TEST_HWND;
        pp[8] = 1;   // Windowed
        pp[9] = auto_depth_stencil ? 1u : 0u;
        pp[10] = auto_depth_stencil ? 75u : 0u;  // D3DFMT_D24S8
        ASSERT_TRUE(memory_.write_bytes(
            PRESENT_ADDR, reinterpret_cast<const Byte*>(pp.data()),
            sizeof(pp)).is_ok());
    }

    /// Create an IDirect3D9 and a device on it; returns {factory, device}.
    std::pair<u32, u32> create_device(bool auto_depth_stencil = false) {
        const u32 factory = mgr_.direct3d_create9(D3D_SDK_VERSION);
        EXPECT_NE(factory, 0u);
        write_present_parameters(640, 480, auto_depth_stencil);
        const u32 hr = invoke_method(
            16, {factory, 0, 1 /* D3DDEVTYPE_HAL */, TEST_HWND,
                 0x00000040 /* HARDWARE_VERTEXPROCESSING */,
                 static_cast<u32>(PRESENT_ADDR),
                 static_cast<u32>(DEVICE_OUT_ADDR)});
        EXPECT_EQ(hr, D3D_OK);
        return {factory, memory_.read_u32(DEVICE_OUT_ADDR).value()};
    }

    /// Give @p device a vertex buffer on stream 0 and a matching FVF.
    /// Defaults to D3DFVF_XYZ | D3DFVF_DIFFUSE (16 bytes per vertex).
    u32 bind_triangle_stream(u32 device, u32 fvf = 0x002 | 0x040) {
        const u32 stride = fvf_stride(fvf);
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 26,
                         {device, stride * 3, 0, fvf, 0,
                          static_cast<u32>(SCRATCH_ADDR + 0x600), 0}),
                  D3D_OK);
        const u32 buffer = memory_.read_u32(SCRATCH_ADDR + 0x600).value();
        EXPECT_NE(buffer, 0u);
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 89, {device, fvf}), D3D_OK);
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 100,
                         {device, 0, buffer, 0, stride}),
                  D3D_OK);
        return buffer;
    }

    /// vs_3_0: dcl_position v0; dcl_texcoord0 v3; m4x4 oPos, v0, c0;
    /// mov oT0, v3.
    static std::vector<u32> simple_vertex_shader_tokens() {
        // D3DSI_GETREGTYPE: type bits 2:0 in 30:28, bits 4:3 in 12:11.
        auto reg = [](u32 type) {
            return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
        };
        constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
        return {
            0xFFFE0300u,
            // dcl_position v0
            31u | (2u << 24), 0u, DST_MASK | reg(1) | 0u,
            // dcl_texcoord0 v3
            31u | (2u << 24), 5u, DST_MASK | reg(1) | 3u,
            // m4x4 oPos.xyzw, v0, c0
            20u | (3u << 24), DST_MASK | reg(4) | 0u,
            0x80000000u | reg(1) | 0u | (0xE4u << 16),
            0x80000000u | reg(2) | 0u | (0xE4u << 16),
            // mov oT0, v3
            1u | (2u << 24), DST_MASK | reg(6) | 0u,
            0x80000000u | reg(1) | 3u | (0xE4u << 16),
            0x0000FFFFu,
        };
    }

    /// ps_3_0: dcl_texcoord0 v0; mov oC0, v0.
    static std::vector<u32> simple_pixel_shader_tokens() {
        auto reg = [](u32 type) {
            return ((type & 0x7u) << 28) | (((type >> 3) & 0x3u) << 11);
        };
        constexpr u32 DST_MASK = 0x80000000u | (0xFu << 16);
        return {
            0xFFFF0300u,
            31u | (2u << 24), 5u, DST_MASK | reg(1) | 0u,
            1u | (2u << 24), DST_MASK | reg(8) | 0u,
            0x80000000u | reg(1) | 0u | (0xE4u << 16),
            0x0000FFFFu,
        };
    }

    void write_shader_bytecode(GuestAddr where, const std::vector<u32>& t) {
        for (usize i = 0; i < t.size(); ++i) {
            ASSERT_TRUE(memory_.write_u32(where + i * 4, t[i]).is_ok());
        }
    }

    u32 create_vertex_shader(u32 device) {
        write_shader_bytecode(SHADER_ADDR, simple_vertex_shader_tokens());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 91,
                         {device, static_cast<u32>(SHADER_ADDR),
                          static_cast<u32>(SCRATCH_ADDR + 0x800)}),
                  D3D_OK);
        return memory_.read_u32(SCRATCH_ADDR + 0x800).value();
    }

    u32 create_pixel_shader(u32 device) {
        write_shader_bytecode(SHADER_ADDR + 0x200,
                              simple_pixel_shader_tokens());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 106,
                         {device, static_cast<u32>(SHADER_ADDR + 0x200),
                          static_cast<u32>(SCRATCH_ADDR + 0x804)}),
                  D3D_OK);
        return memory_.read_u32(SCRATCH_ADDR + 0x804).value();
    }

    static u32 fvf_stride(u32 fvf) {
        const u32 position = fvf & 0x400Eu;
        u32 stride = 12; // XYZ and XYZBn start with three position floats.
        if (position == 0x004) stride = 16;
        if (position >= 0x006 && position <= 0x00C) {
            stride += ((position - 0x006) / 2u + 1u) * 4u;
        }
        if ((fvf & 0x010) != 0) stride += 12;
        if ((fvf & 0x020) != 0) stride += 4;
        if ((fvf & 0x040) != 0) stride += 4;
        if ((fvf & 0x080) != 0) stride += 4;
        stride += ((fvf & 0xF00) >> 8) * 8;
        return stride;
    }

    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    static constexpr GuestAddr IDENT_ADDR     = MEM_BASE + 0x100;
    static constexpr GuestAddr MODE_ADDR      = MEM_BASE + 0x1800;
    static constexpr GuestAddr CAPS_ADDR      = MEM_BASE + 0x2000;
    static constexpr GuestAddr IID_ADDR       = MEM_BASE + 0x2F00;
    static constexpr GuestAddr DEVICE_OUT_ADDR = MEM_BASE + 0x3000;
    static constexpr GuestAddr PRESENT_ADDR   = MEM_BASE + 0x3100;
    static constexpr GuestAddr SCRATCH_ADDR   = MEM_BASE + 0x4000;
    static constexpr GuestAddr SHADER_ADDR    = MEM_BASE + 0x8000;
    static constexpr u32 TEST_HWND = 0x00010004;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    Marshaler marshaler_{memory_};
    TestGraphicsBackend backend_;
    D3D9Manager mgr_;
    D3D9Manager::MethodThunks method_thunks_{};
    std::array<D3D9Manager::InterfaceThunks, D3D9_INTERFACE_COUNT>
        interface_thunks_{};
    int native_view_token_{0};
    DllExports exports_;
};

} // namespace vitreo::winapi::d3d9_test
