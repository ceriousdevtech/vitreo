// SPDX-License-Identifier: LGPL-2.1-only
//
// Render-target views: creation, hazards, state read-back, array/multisample windows.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, CreateRenderTargetViewAcceptsTypelessToTyped) {
    // Learn: R8G8B8A8_TYPELESS resources may create an UNORM RTV.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        64, 32, 1, 1, 27, 1, 0, 0, 0x20, 0, 0, // format TYPELESS
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    // D3D11_RENDER_TARGET_VIEW_DESC: Format, ViewDimension, MipSlice
    const std::array<u32, 3> rtv_desc{28, 4, 0}; // UNORM, TEXTURE2D, mip 0
    constexpr GuestAddr RTV_DESC = TEXTURE_DESC + 0x80;
    ASSERT_TRUE(memory.write_bytes(
        RTV_DESC, reinterpret_cast<const Byte*>(rtv_desc.data()),
        sizeof(rtv_desc)).is_ok());

    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, RTV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(memory.read_u64(VIEW_OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, ReleasedRenderTargetViewsDoNotExhaustObjectStorage) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{16, 16, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(TEXTURE_DESC,
        reinterpret_cast<const Byte*>(desc.data()), sizeof(desc)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr retained = memory.read_u64(VIEW_OUT).value();
    set_arg(0, retained);
    d3d11.impl_RenderTargetViewMethod(marshaler, 1);
    ASSERT_EQ(cpu.gpr.rax, 2u);
    d3d11.impl_RenderTargetViewMethod(marshaler, 2);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    // More allocations than fit in the arena, but only two live views.
    for (u32 iteration = 0; iteration < 8192; ++iteration) {
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, 0);
        set_arg(3, VIEW_OUT);
        d3d11.impl_DeviceMethod(marshaler, 9);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << iteration;
        ASSERT_NE(memory.read_u64(VIEW_OUT).value(), retained);
        set_arg(0, memory.read_u64(VIEW_OUT).value());
        d3d11.impl_RenderTargetViewMethod(marshaler, 2);
        ASSERT_EQ(cpu.gpr.rax, 0u);
    }
    set_arg(0, retained);
    d3d11.impl_RenderTargetViewMethod(marshaler, 2);
    ASSERT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(D3D11ManagerTest, RenderTargetAndShaderResourceHazardsBindNull) {
    // Microsoft documents this as a binding contract in both directions:
    // OMSetRenderTargets unbinds every overlapping SRV, and a later
    // *SetShaderResources call installs NULL while that output is bound.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 11> desc{
        64, 64, 2, 1, 28, 1, 0, 0, 0x28, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();

    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(VIEW_OUT).value();
    ASSERT_TRUE(memory.write_u64(VIEW_OUT, rtv).is_ok());

    constexpr std::array<usize, 3> shader_resource_slots{8, 25, 67};
    for (const usize setter : shader_resource_slots) {
        constexpr GuestAddr SRV_OUT = VIEW_OUT + 0x20;
        constexpr GuestAddr SRV_BINDING = VIEW_OUT + 0x28;
        // Start each stage with no output bound so this iteration exercises
        // OM's unbinding direction independently of the preceding stage.
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 0);
        set_arg(3, 0);
        d3d11.impl_ContextMethod(marshaler, 33);
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, 0);
        set_arg(3, SRV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr srv = memory.read_u64(SRV_OUT).value();
        ASSERT_TRUE(memory.write_u64(SRV_BINDING, srv).is_ok());

        // First prove the inverse ordering: the SRV is retained, then the OM
        // bind drops that stage's reference because the subresource overlaps.
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 1);
        set_arg(3, SRV_BINDING);
        d3d11.impl_ContextMethod(marshaler, setter);
        set_arg(0, context);
        set_arg(1, 1);
        set_arg(2, VIEW_OUT);
        set_arg(3, 0);
        d3d11.impl_ContextMethod(marshaler, 33);
        set_arg(0, srv);
        d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
        EXPECT_EQ(cpu.gpr.rax, 0u) << "setter slot " << setter;
        EXPECT_FALSE(kept_alive_by_runtime(
            srv, &D3D11Manager::impl_ShaderResourceViewMethod))
            << "setter slot " << setter;

        // Recreate the SRV while the RTV remains bound. The shader setter
        // must install NULL and therefore take no context-owned reference.
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, 0);
        set_arg(3, SRV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr later_srv = memory.read_u64(SRV_OUT).value();
        ASSERT_TRUE(memory.write_u64(SRV_BINDING, later_srv).is_ok());
        set_arg(0, context);
        set_arg(1, 0);
        set_arg(2, 1);
        set_arg(3, SRV_BINDING);
        d3d11.impl_ContextMethod(marshaler, setter);
        set_arg(0, later_srv);
        d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
        EXPECT_EQ(cpu.gpr.rax, 0u) << "setter slot " << setter;
        EXPECT_FALSE(kept_alive_by_runtime(
            later_srv, &D3D11Manager::impl_ShaderResourceViewMethod))
            << "setter slot " << setter;
    }

    // Hazards are subresource-specific, not whole-resource-specific. An SRV
    // exposing only mip 1 remains legal while the RTV writes mip 0.
    constexpr GuestAddr SRV_DESC = VIEW_OUT + 0x40;
    constexpr GuestAddr SRV_OUT = VIEW_OUT + 0x60;
    constexpr GuestAddr SRV_BINDING = VIEW_OUT + 0x68;
    const std::array<u32, 4> mip1_srv_desc{28, 4, 1, 1};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(mip1_srv_desc.data()),
        sizeof(mip1_srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mip1_srv = memory.read_u64(SRV_OUT).value();
    ASSERT_TRUE(memory.write_u64(SRV_BINDING, mip1_srv).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BINDING);
    d3d11.impl_ContextMethod(marshaler, 8);
    set_arg(0, mip1_srv);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
    // The application's count; the binding keeps the view alive.
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(kept_alive_by_runtime(
        mip1_srv, &D3D11Manager::impl_ShaderResourceViewMethod));
    ASSERT_TRUE(memory.write_u64(SRV_BINDING, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BINDING);
    d3d11.impl_ContextMethod(marshaler, 8);
}

TEST_F(D3D11ManagerTest, RenderTargetViewSelectsAndClearsNonzeroMip) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 11> texture_desc{
        4, 4, 2, 1, 28, 1, 0, 0, 0x20, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(texture_desc.data()),
        sizeof(texture_desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    constexpr GuestAddr RTV_DESC = TEXTURE_DESC + 0x80;
    const std::array<u32, 3> rtv_desc{28, 4, 1};
    ASSERT_TRUE(memory.write_bytes(
        RTV_DESC, reinterpret_cast<const Byte*>(rtv_desc.data()),
        sizeof(rtv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, RTV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    constexpr GuestAddr DESC_OUT = RTV_DESC + 0x20;
    set_arg(0, view);
    set_arg(1, DESC_OUT);
    d3d11.impl_RenderTargetViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(DESC_OUT).value(), 28u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 4).value(), 4u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 8).value(), 1u);

    const std::array<float, 4> color{1.0f, 0.0f, 0.0f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        COLOR, reinterpret_cast<const Byte*>(color.data()),
        sizeof(color)).is_ok());
    set_arg(0, context);
    set_arg(1, view);
    set_arg(2, COLOR);
    d3d11.impl_ContextMethod(marshaler, 50);
    EXPECT_EQ(backend.state.texture_clears, 1u);
    EXPECT_EQ(backend.state.last_texture_clear_mip, 1u);
}

TEST_F(D3D11ManagerTest, OMSetRenderTargetsAndUnorderedAccessViewsBindsTheUavs) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    constexpr GuestAddr UAV_DESC = GPU_BUFFER_DESC + 0x40;
    constexpr GuestAddr UAV_OUT = GPU_BUFFER_OUT + 0x18;
    constexpr GuestAddr UAV_BINDING = GPU_BUFFER_OUT + 0x28;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const std::array<u32, 6> desc{32, 0, 0x80, 0, 0x40, 4};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();

    const std::array<u32, 5> uav_desc{0, 1, 0, 8, 0};
    ASSERT_TRUE(memory.write_bytes(
        UAV_DESC, reinterpret_cast<const Byte*>(uav_desc.data()),
        sizeof(uav_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, buffer);
    set_arg(2, UAV_DESC);
    set_arg(3, UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr uav = memory.read_u64(UAV_OUT).value();
    ASSERT_NE(uav, 0u);
    ASSERT_TRUE(memory.write_u64(UAV_BINDING, uav).is_ok());

    // OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs=0, ppRTVs=0, pDSV=0,
    //   UAVStartSlot=1, NumUAVs=1, ppUAVs, pUAVInitialCounts=0).
    // Learn: the OM keeps a reference to every bound UAV. Dropping the bind
    // silently — as this method used to — means a pass that writes a UAV
    // alongside its render targets writes nowhere, with no error anywhere.
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 1);
    set_arg(5, 1);
    set_arg(6, UAV_BINDING);
    set_arg(7, 0);
    d3d11.impl_ContextMethod(marshaler, 34);

    // The context now holds a reference: releasing the creation reference must
    // leave the view alive (Release answers the application's count, 0).
    set_arg(0, uav);
    d3d11.impl_UnorderedAccessViewMethod(marshaler, 2);  // Release
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(kept_alive_by_runtime(
        uav, &D3D11Manager::impl_UnorderedAccessViewMethod))
        << "the OM bind must retain the UAV";

    // D3D11_KEEP_UNORDERED_ACCESS_VIEWS (0xFFFFFFFF) leaves the UAV binding
    // alone while the render targets are rebound.
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 0);
    set_arg(5, 0xFFFFFFFFu);
    set_arg(6, 0);
    set_arg(7, 0);
    d3d11.impl_ContextMethod(marshaler, 34);
    EXPECT_TRUE(kept_alive_by_runtime(
        uav, &D3D11Manager::impl_UnorderedAccessViewMethod))
        << "KEEP must not drop the existing binding";

    // Explicitly unbinding the slot releases the context's reference.
    ASSERT_TRUE(memory.write_u64(UAV_BINDING, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 1);
    set_arg(5, 1);
    set_arg(6, UAV_BINDING);
    set_arg(7, 0);
    d3d11.impl_ContextMethod(marshaler, 34);
    EXPECT_FALSE(kept_alive_by_runtime(
        uav, &D3D11Manager::impl_UnorderedAccessViewMethod))
        << "unbinding must release the OM reference";
}

// Learn (ID3D11DeviceContext Get* methods): each returns what is currently
// bound, AddRefs every interface handed back, and NULLs slots past the bound
// count. Callers save and restore pipeline state with these — an injected
// overlay does exactly that — so returning nothing leaves the caller's array
// holding whatever it contained before, which it then Releases.
TEST_F(D3D11ManagerTest, StateReadBackReturnsWhatIsBoundAndAddRefsIt) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    constexpr GuestAddr CB_OUT = GPU_BUFFER_OUT + 0x40;
    constexpr GuestAddr CB_BIND = GPU_BUFFER_OUT + 0x60;
    constexpr GuestAddr TOPO_OUT = GPU_BUFFER_OUT + 0x80;
    constexpr GuestAddr VP_COUNT = GPU_BUFFER_OUT + 0x90;
    constexpr GuestAddr VP_OUT = GPU_BUFFER_OUT + 0xA0;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // A constant buffer to bind: ByteWidth, DEFAULT, BIND_CONSTANT_BUFFER.
    const std::array<u32, 6> desc{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);

    ASSERT_TRUE(memory.write_u64(CB_BIND, buffer).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, CB_BIND);
    d3d11.impl_ContextMethod(marshaler, 7);  // VSSetConstantBuffers

    set_arg(0, context);
    set_arg(1, 5);  // IASetPrimitiveTopology: TRIANGLESTRIP
    d3d11.impl_ContextMethod(marshaler, 24);

    const std::array<float, 6> viewport{0.0f, 0.0f, 800.0f, 600.0f, 0.0f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        VP_OUT + 0x40, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, VP_OUT + 0x40);
    d3d11.impl_ContextMethod(marshaler, 44);  // RSSetViewports

    // Poison the out-parameters: leaving them untouched is the bug.
    ASSERT_TRUE(memory.write_u64(CB_OUT, 0xDEADBEEFull).is_ok());
    ASSERT_TRUE(memory.write_u64(CB_OUT + 8, 0xDEADBEEFull).is_ok());

    // VSGetConstantBuffers(StartSlot=0, NumBuffers=2, ppOut)
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 2);
    set_arg(3, CB_OUT);
    d3d11.impl_ContextMethod(marshaler, 72);
    EXPECT_EQ(memory.read_u64(CB_OUT).value(), buffer);
    EXPECT_EQ(memory.read_u64(CB_OUT + 8).value(), 0u)
        << "slots past the bound count must be NULLed, not left alone";

    // IAGetPrimitiveTopology
    set_arg(0, context);
    set_arg(1, TOPO_OUT);
    d3d11.impl_ContextMethod(marshaler, 83);
    EXPECT_EQ(memory.read_u32(TOPO_OUT).value(), 5u);

    // RSGetViewports: NULL array asks for the count.
    ASSERT_TRUE(memory.write_u32(VP_COUNT, 0).is_ok());
    set_arg(0, context);
    set_arg(1, VP_COUNT);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 95);
    EXPECT_EQ(memory.read_u32(VP_COUNT).value(), 1u);

    // ...then the viewport itself round-trips.
    ASSERT_TRUE(memory.write_u32(VP_COUNT, 1).is_ok());
    set_arg(0, context);
    set_arg(1, VP_COUNT);
    set_arg(2, VP_OUT);
    d3d11.impl_ContextMethod(marshaler, 95);
    float width = 0.0f;
    const u32 width_bits = memory.read_u32(VP_OUT + 8).value();
    std::memcpy(&width, &width_bits, sizeof(width));
    EXPECT_FLOAT_EQ(width, 800.0f);

    // Learn: every returned interface is AddRef'd, so releasing the creation
    // reference must leave the buffer alive.
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 2);  // Release
    EXPECT_GT(cpu.gpr.rax, 0u) << "Get* must AddRef what it returns";
}

TEST_F(D3D11ManagerTest, OMGetRenderTargetsReturnsTheBoundViewsAndAddRefsThem) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    constexpr GuestAddr RT_OUT = VIEW_OUT + 0x40;
    constexpr GuestAddr DSV_OUT = VIEW_OUT + 0x60;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // Width, Height, MipLevels, ArraySize, Format, SampleCount, SampleQuality,
    // Usage, BindFlags(RENDER_TARGET), CPUAccess, Misc.
    const std::array<u32, 11> desc{64, 64, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(rtv, 0u);

    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, VIEW_OUT);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 33);  // OMSetRenderTargets

    // Poison the out-parameters: leaving them untouched is the bug, so the
    // test has to be able to see that they were actually written.
    ASSERT_TRUE(memory.write_u64(RT_OUT, 0xDEADBEEFull).is_ok());
    ASSERT_TRUE(memory.write_u64(RT_OUT + 8, 0xDEADBEEFull).is_ok());
    ASSERT_TRUE(memory.write_u64(DSV_OUT, 0xDEADBEEFull).is_ok());

    set_arg(0, context);
    set_arg(1, 2);
    set_arg(2, RT_OUT);
    set_arg(3, DSV_OUT);
    d3d11.impl_ContextMethod(marshaler, 89);  // OMGetRenderTargets

    EXPECT_EQ(memory.read_u64(RT_OUT).value(), rtv);
    // Learn: slots past the bound count are set to NULL, not left alone.
    EXPECT_EQ(memory.read_u64(RT_OUT + 8).value(), 0u);
    // No depth-stencil view was bound, so NULL there too.
    EXPECT_EQ(memory.read_u64(DSV_OUT).value(), 0u);

    // Learn: every returned interface is AddRef'd. Release once and the view
    // must still be alive, holding the caller's reference.
    set_arg(0, rtv);
    d3d11.impl_RenderTargetViewMethod(marshaler, 2);  // Release
    EXPECT_GT(cpu.gpr.rax, 0u) << "OMGetRenderTargets must AddRef";
}

TEST_F(D3D11ManagerTest, RenderTargetViewSelectsTexture2DArrayWindow) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 11> texture_desc{
        8, 8, 2, 4, 28, 1, 0, 0, 0x20, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(texture_desc.data()),
        sizeof(texture_desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    constexpr GuestAddr RTV_DESC = TEXTURE_DESC + 0x80;
    // Texture2DArray: mip 1, slices [1, 3).
    const std::array<u32, 5> rtv_desc{28, 5, 1, 1, 2};
    ASSERT_TRUE(memory.write_bytes(
        RTV_DESC, reinterpret_cast<const Byte*>(rtv_desc.data()),
        sizeof(rtv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, RTV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    constexpr GuestAddr DESC_OUT = RTV_DESC + 0x20;
    set_arg(0, view);
    set_arg(1, DESC_OUT);
    d3d11.impl_RenderTargetViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(DESC_OUT).value(), 28u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 4).value(), 5u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 12).value(), 1u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 16).value(), 2u);

    const std::array<float, 4> color{0.0f, 1.0f, 0.0f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        COLOR, reinterpret_cast<const Byte*>(color.data()),
        sizeof(color)).is_ok());
    set_arg(0, context);
    set_arg(1, view);
    set_arg(2, COLOR);
    d3d11.impl_ContextMethod(marshaler, 50);
    EXPECT_EQ(backend.state.texture_clears, 1u);
    EXPECT_EQ(backend.state.last_texture_clear_mip, 1u);
    EXPECT_EQ(backend.state.last_texture_clear_slice, 1u);
    EXPECT_EQ(backend.state.last_texture_clear_array_size, 2u);
}

TEST_F(D3D11ManagerTest, MultisampledRenderTargetViewsRoundTripDimensions) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr SINGLE_TEXTURE_OUT = DATA_BASE + 0x700;
    constexpr GuestAddr ARRAY_TEXTURE_OUT = DATA_BASE + 0x708;
    constexpr GuestAddr SINGLE_VIEW_OUT = DATA_BASE + 0x710;
    constexpr GuestAddr ARRAY_VIEW_OUT = DATA_BASE + 0x718;
    constexpr GuestAddr RTV_DESC = DATA_BASE + 0x720;
    constexpr GuestAddr DESC_OUT = DATA_BASE + 0x740;

    const auto create_texture = [&](u32 array_size, GuestAddr output) {
        const std::array<u32, 11> desc{
            8, 8, 1, array_size, 28, 4, 0, 0, 0x20, 0, 0,
        };
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, output);
        d3d11.impl_DeviceMethod(marshaler, 5);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(output).value();
    };

    const GuestAddr single_texture = create_texture(1, SINGLE_TEXTURE_OUT);
    const GuestAddr array_texture = create_texture(4, ARRAY_TEXTURE_OUT);
    ASSERT_NE(single_texture, 0u);
    ASSERT_NE(array_texture, 0u);

    const std::array<u32, 3> single_desc{28, 6, 0xA5A5A5A5u};
    ASSERT_TRUE(memory.write_bytes(
        RTV_DESC, reinterpret_cast<const Byte*>(single_desc.data()),
        sizeof(single_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, single_texture);
    set_arg(2, RTV_DESC);
    set_arg(3, SINGLE_VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr single_view = memory.read_u64(SINGLE_VIEW_OUT).value();
    ASSERT_NE(single_view, 0u);

    // Windows writes the whole DESC and zeroes the union a TEXTURE2DMS RTV
    // does not use, whatever the caller passed there (measured,
    // d3d11_view_desc_fill_probe). This used to expect the poison left alone.
    ASSERT_TRUE(memory.write_u32(DESC_OUT + 8, 0x5A5A5A5Au).is_ok());
    set_arg(0, single_view);
    set_arg(1, DESC_OUT);
    d3d11.impl_RenderTargetViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(DESC_OUT).value(), 28u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 4).value(), 6u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 8).value(), 0u);

    const std::array<u32, 4> array_desc{28, 7, 1, 2};
    ASSERT_TRUE(memory.write_bytes(
        RTV_DESC, reinterpret_cast<const Byte*>(array_desc.data()),
        sizeof(array_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, array_texture);
    set_arg(2, RTV_DESC);
    set_arg(3, ARRAY_VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr array_view = memory.read_u64(ARRAY_VIEW_OUT).value();
    ASSERT_NE(array_view, 0u);

    set_arg(0, array_view);
    set_arg(1, DESC_OUT);
    d3d11.impl_RenderTargetViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(DESC_OUT).value(), 28u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 4).value(), 7u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 12).value(), 2u);

    const std::array<float, 4> color{0.25f, 0.5f, 0.75f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        COLOR, reinterpret_cast<const Byte*>(color.data()),
        sizeof(color)).is_ok());
    set_arg(0, context);
    set_arg(1, array_view);
    set_arg(2, COLOR);
    d3d11.impl_ContextMethod(marshaler, 50);
    EXPECT_EQ(backend.state.texture_clears, 1u);
    EXPECT_EQ(backend.state.last_texture_clear_mip, 0u);
    EXPECT_EQ(backend.state.last_texture_clear_slice, 1u);
    EXPECT_EQ(backend.state.last_texture_clear_array_size, 2u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
