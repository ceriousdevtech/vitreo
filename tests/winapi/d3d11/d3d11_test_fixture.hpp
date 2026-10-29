// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_fixture.hpp
/// @brief D3D11ManagerTest, the base fixture of every D3D11 frontend suite, and the thunk/refcount helpers it needs.

#include "d3d11_test_backend.hpp"
#include "d3d11_test_guids.hpp"

namespace vitreo::winapi::d3d11_test {

/// Fill every DXGI thunk field with a distinct fake address.
///
/// Done by treating MethodThunks as the homogeneous block of GuestAddr it is,
/// rather than naming each field. DxgiManager's vtable builder refuses a zero
/// entry -- a guest call to address zero -- so a fixture that named the fields
/// broke every DXGI-backed test the moment DXGI grew an interface version, which
/// is exactly what adding IDXGIFactory3..7 did. This cannot miss a new field.
[[nodiscard]] inline DxgiManager::MethodThunks sequential_dxgi_thunks(
    GuestAddr first, GuestAddr stride = 0x10) {
    DxgiManager::MethodThunks thunks{};
    static_assert(sizeof(DxgiManager::MethodThunks) % sizeof(GuestAddr) == 0,
                  "MethodThunks must be a homogeneous block of GuestAddr");
    auto* fields = reinterpret_cast<GuestAddr*>(&thunks);
    const usize count = sizeof(thunks) / sizeof(GuestAddr);
    for (usize index = 0; index < count; ++index) {
        fields[index] = first + index * stride;
    }
    return thunks;
}

class D3D11ManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // The process handle table NT shared-resource handles live in, with
        // the close callback runtime_context installs for them.
        handles.set_close_callback([](Handle, HandleType type, void* object) {
            if (type == HandleType::SharedResource) {
                release_shared_resource_handle_object(object);
            }
            return true;
        });
        d3d11.set_handle_table(&handles);
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x3000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = STACK_BASE + 0x800;

        DxgiManager::MethodThunks dxgi_thunks =
            sequential_dxgi_thunks(0x70000010);
        dxgi.set_method_thunks(dxgi_thunks);
        // Continue the D3D11 thunks past the block the DXGI fill consumed, so
        // no two interfaces share an address.
        GuestAddr thunk = 0x70000010 +
            (sizeof(DxgiManager::MethodThunks) / sizeof(GuestAddr)) * 0x10;

        D3D11Manager::MethodThunks d3d11_thunks;
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
        d3d11.set_presentation_view_resolver(
            [](u64 hwnd) -> void* {
                return hwnd == 0
                    ? nullptr
                    : reinterpret_cast<void*>(0x1234);
            });
        dxgi.set_swap_chain_creator(
            [this](Marshaler& marshaler) {
                d3d11.impl_CreateSwapChain(marshaler);
            });
    }

    void set_arg(usize index, u64 value) {
        switch (index) {
        case 0: cpu.gpr.rcx = value; break;
        case 1: cpu.gpr.rdx = value; break;
        case 2: cpu.gpr.r8 = value; break;
        case 3: cpu.gpr.r9 = value; break;
        default:
            ASSERT_TRUE(memory.write_u64(
                cpu.gpr.rsp + 0x28 + (index - 4) * 8, value).is_ok());
            break;
        }
    }

    void prepare_create(GuestAddr adapter = 0,
                        u32 driver = d3d11::DRIVER_TYPE_HARDWARE) {
        for (usize index = 0; index < 10; ++index) set_arg(index, 0);
        set_arg(0, adapter);
        set_arg(1, driver);
        set_arg(6, d3d11::SDK_VERSION);
        set_arg(7, DEVICE_OUT);
        set_arg(8, LEVEL_OUT);
        set_arg(9, CONTEXT_OUT);
    }

    void create_default_device() {
        prepare_create();
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_D3D11CreateDevice(marshaler);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    }

    /// Whether @p object, whose application references are all released,
    /// is still alive because the runtime holds it (a binding, a view).
    /// Release answers only the application's count (0 once it has let go,
    /// measured, d3d11_a12_object_model_probe [rc] "bound VB ... last
    /// external Release=0"), so this asks the way a guest could: AddRef of a
    /// live object answers 1, of a destroyed one 0. The reference taken is
    /// given back.
    bool kept_alive_by_runtime(
        GuestAddr object, void (D3D11Manager::*method)(Marshaler&, usize)) {
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        set_arg(0, object);
        (d3d11.*method)(marshaler, 1);
        const u64 count = cpu.gpr.rax;
        if (count != 0) {
            set_arg(0, object);
            (d3d11.*method)(marshaler, 2);
        }
        return count == 1;
    }

    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr DEVICE_OUT = DATA_BASE + 0x100;
    static constexpr GuestAddr LEVEL_OUT = DATA_BASE + 0x108;
    static constexpr GuestAddr CONTEXT_OUT = DATA_BASE + 0x110;
    static constexpr GuestAddr IID_ADDRESS = DATA_BASE + 0x200;
    static constexpr GuestAddr INTERFACE_OUT = DATA_BASE + 0x220;
    static constexpr GuestAddr ADAPTER_OUT = DATA_BASE + 0x228;
    static constexpr GuestAddr LEVELS = DATA_BASE + 0x300;
    static constexpr GuestAddr TEXTURE_DESC = DATA_BASE + 0x400;
    static constexpr GuestAddr TEXTURE_OUT = DATA_BASE + 0x440;
    static constexpr GuestAddr VIEW_OUT = DATA_BASE + 0x448;
    static constexpr GuestAddr COLOR = DATA_BASE + 0x460;
    static constexpr GuestAddr SWAP_DESC = DATA_BASE + 0x500;
    static constexpr GuestAddr SWAP_OUT = DATA_BASE + 0x550;
    static constexpr GuestAddr FACTORY_OUT = DATA_BASE + 0x558;
    static constexpr GuestAddr BUFFER_OUT = DATA_BASE + 0x560;
    static constexpr GuestAddr GPU_BUFFER_DESC = DATA_BASE + 0x600;
    static constexpr GuestAddr GPU_BUFFER_INITIAL = DATA_BASE + 0x620;
    static constexpr GuestAddr GPU_BUFFER_DATA = DATA_BASE + 0x640;
    static constexpr GuestAddr GPU_BUFFER_OUT = DATA_BASE + 0x680;
    static constexpr GuestAddr RESOURCE_OUT = DATA_BASE + 0x688;
    static constexpr GuestAddr SRV_DESC = DATA_BASE + 0x6A0;
    static constexpr GuestAddr MAPPED_OUT = DATA_BASE + 0x6C0;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    cpu::CpuState cpu;
    TestGraphicsBackend backend;
    DxgiManager dxgi{
        memory, true,
        dxgi::AdapterInfo{
            .description = "Shared Test Metal GPU",
            .vendor_id = dxgi::VENDOR_ID_APPLE,
            .dedicated_video_memory = 8ull * 1024 * 1024 * 1024,
        }};
    HandleTable handles;
    D3D11Manager d3d11{memory, true, backend, dxgi};
};

// ---------------------------------------------------------------------------
// Area 12: the object model, measured on Windows
// (tools/oracle_probe/d3d11_a12_object_model_probe{64,32}.expected and
// d3d11_a12_refmodel_probe{64,32}.expected). Every value below is Windows'.
// ---------------------------------------------------------------------------


/// AddRef + Release through a vtable-slot dispatcher: the count after.
inline u32 count_of(D3D11Manager& d3d11, Marshaler& marshaler, cpu::CpuState& cpu,
             GuestAddr object,
             void (D3D11Manager::*method)(Marshaler&, usize)) {
    cpu.gpr.rcx = object;
    (d3d11.*method)(marshaler, 1);
    cpu.gpr.rcx = object;
    (d3d11.*method)(marshaler, 2);
    return static_cast<u32>(cpu.gpr.rax);
}

}  // namespace vitreo::winapi::d3d11_test
