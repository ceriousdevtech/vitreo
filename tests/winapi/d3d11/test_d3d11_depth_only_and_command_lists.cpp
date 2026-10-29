// SPDX-License-Identifier: LGPL-2.1-only
//
// Depth-only render pass, deferred binding state and Finish/Execute round trip.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest,
       DrawSubmitsDepthOnlyRenderPassWithPixelShaderDisabled) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr DSV_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr VS_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr VS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr BINDINGS = DATA_BASE + 0x1C00;
    constexpr GuestAddr STRIDE = DATA_BASE + 0x1C20;
    constexpr GuestAddr OFFSET = DATA_BASE + 0x1C24;
    constexpr GuestAddr DSV_DESC = DATA_BASE + 0x1C40;
    constexpr GuestAddr DSV_OBSERVED = DATA_BASE + 0x1C60;
    constexpr GuestAddr DSS_DESC = DATA_BASE + 0x1C80;
    constexpr GuestAddr DSS_OUT = DATA_BASE + 0x1D40;
    constexpr GuestAddr DSS_OBSERVED = DATA_BASE + 0x1D80;
    constexpr GuestAddr SRV_DESC = DATA_BASE + 0x1CC0;
    constexpr GuestAddr SRV_OUT = DATA_BASE + 0x1CE0;
    constexpr GuestAddr SRV_BINDING = DATA_BASE + 0x1D00;
    constexpr GuestAddr WRITABLE_DSV_OUT = DATA_BASE + 0x1D20;
    constexpr GuestAddr STENCIL_SRV_DESC = DATA_BASE + 0x1DC0;
    constexpr GuestAddr STENCIL_SRV_OUT = DATA_BASE + 0x1DE0;
    constexpr GuestAddr READONLY_BOTH_DSV_OUT = DATA_BASE + 0x1E00;

    const std::array<u32, 11> depth_desc{
        16, 16, 2, 3, 19, 1, 0, 0, 0x48, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(depth_desc.data()),
        sizeof(depth_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr depth_texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(depth_texture, 0u);

    const std::array<u32, 6> dsv_desc{
        20, 4, 1, 1, 1, 2,
    };
    ASSERT_TRUE(memory.write_bytes(
        DSV_DESC, reinterpret_cast<const Byte*>(dsv_desc.data()),
        sizeof(dsv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, depth_texture);
    set_arg(2, DSV_DESC);
    set_arg(3, DSV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 10);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr dsv = memory.read_u64(DSV_OUT).value();
    ASSERT_NE(dsv, 0u);
    set_arg(0, dsv);
    set_arg(1, DSV_OBSERVED);
    d3d11.impl_DepthStencilViewMethod(marshaler, 8);
    for (usize i = 0; i < dsv_desc.size(); ++i) {
        EXPECT_EQ(memory.read_u32(DSV_OBSERVED + i * 4).value(), dsv_desc[i]);
    }

    const std::array<u32, 6> srv_desc{21, 5, 1, 1, 1, 2};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
        sizeof(srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, depth_texture);
    set_arg(2, SRV_DESC);
    set_arg(3, SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr depth_srv = memory.read_u64(SRV_OUT).value();
    ASSERT_NE(depth_srv, 0u);

    set_arg(0, context);
    set_arg(1, dsv);
    set_arg(2, 1); // D3D11_CLEAR_DEPTH
    set_arg(3, 0x3F000000u); // 0.5f
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 53);
    EXPECT_EQ(backend.state.depth_clears, 1u);
    EXPECT_EQ(backend.state.last_depth_clear_mip, 1u);
    EXPECT_EQ(backend.state.last_depth_clear_slice, 1u);
    EXPECT_EQ(backend.state.last_depth_clear_array_size, 2u);
    EXPECT_TRUE(backend.state.last_depth_clear_depth);
    EXPECT_FALSE(backend.state.last_depth_clear_stencil);

    set_arg(0, context);
    set_arg(1, dsv);
    set_arg(2, 2); // D3D11_CLEAR_STENCIL
    set_arg(3, 0x3F800000u); // ignored depth
    set_arg(4, 0x5Au);
    d3d11.impl_ContextMethod(marshaler, 53);

    EXPECT_EQ(backend.state.depth_clears, 2u);
    EXPECT_FALSE(backend.state.last_depth_clear_depth);
    EXPECT_TRUE(backend.state.last_depth_clear_stencil);

    // x64 ABI: FLOAT Depth, the fourth argument, travels in XMM3; R9 holds
    // whatever the caller last left there. Reading R9 cleared UE's
    // reversed-Z depth to 1.0 and hid the whole world behind sky and fog.
    set_arg(0, context);
    set_arg(1, dsv);
    set_arg(2, 1); // D3D11_CLEAR_DEPTH
    set_arg(3, 0x3F800000u);  // garbage 1.0f bits in R9
    cpu.sse.xmm[3].u64_[0] = 0;  // the real argument: 0.0f
    cpu.sse.xmm[3].u64_[1] = 0;
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 53);
    EXPECT_EQ(backend.state.last_depth_clear_value, 0.0f)
        << "the depth clear value must come from XMM3";

    const auto create_shader = [&](GuestAddr bytecode_address,
                                   const unsigned char* bytecode,
                                   usize bytecode_size, usize device_slot,
                                   GuestAddr output) {
        EXPECT_TRUE(memory.write_bytes(
            bytecode_address, bytecode, bytecode_size).is_ok());
        set_arg(0, device);
        set_arg(1, bytecode_address);
        set_arg(2, bytecode_size);
        set_arg(3, 0);
        set_arg(4, output);
        d3d11.impl_DeviceMethod(marshaler, device_slot);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(output).value();
    };
    const GuestAddr vertex_shader = create_shader(
        VS_BYTECODE, vs_main, sizeof(vs_main), 12, VS_OUT);
    ASSERT_NE(vertex_shader, 0u);

    // DepthEnable=TRUE, DepthWriteMask=ALL, DepthFunc=LESS. The read-only
    // DSV must suppress the write without disabling the depth comparison.
    const std::array<u32, 13> depth_state_desc{
        1, 1, 2, 1, 0x00003CA5u,
        2, 4, 3, 6,
        6, 7, 8, 3,
    };
    ASSERT_TRUE(memory.write_bytes(
        DSS_DESC, reinterpret_cast<const Byte*>(depth_state_desc.data()),
        sizeof(depth_state_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, DSS_DESC);
    set_arg(2, DSS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 21);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr depth_state = memory.read_u64(DSS_OUT).value();
    ASSERT_NE(depth_state, 0u);
    const std::array<u8, 52> zero_desc{};
    ASSERT_TRUE(memory.write_bytes(
        DSS_OBSERVED, reinterpret_cast<const Byte*>(zero_desc.data()),
        zero_desc.size()).is_ok());
    set_arg(0, depth_state);
    set_arg(1, DSS_OBSERVED);
    d3d11.impl_DepthStencilStateMethod(marshaler, 7);
    std::array<u8, 52> observed_desc{};
    ASSERT_TRUE(memory.read_bytes(
        DSS_OBSERVED, reinterpret_cast<Byte*>(observed_desc.data()),
        observed_desc.size()).is_ok());
    EXPECT_EQ(0, std::memcmp(observed_desc.data(), depth_state_desc.data(),
                             observed_desc.size()));

    const std::array<u32, 6> vertex_buffer_desc{
        48, 0, 0x1, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC,
        reinterpret_cast<const Byte*>(vertex_buffer_desc.data()),
        sizeof(vertex_buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr vertex_buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(vertex_buffer, 0u);

    ASSERT_TRUE(memory.write_u64(BINDINGS, vertex_buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(STRIDE, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSET, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    set_arg(4, STRIDE);
    set_arg(5, OFFSET);
    d3d11.impl_ContextMethod(marshaler, 18);

    set_arg(0, context);
    set_arg(1, vertex_shader);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 11);
    set_arg(0, context);
    set_arg(1, 0); // PSSetShader(NULL) disables the pixel shader stage.
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 9);

    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, dsv);
    d3d11.impl_ContextMethod(marshaler, 33);
    set_arg(0, context);
    set_arg(1, depth_state);
    set_arg(2, 0x5Au);
    d3d11.impl_ContextMethod(marshaler, 36);
    ASSERT_TRUE(memory.write_u64(SRV_BINDING, depth_srv).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BINDING);
    d3d11.impl_ContextMethod(marshaler, 8);
    set_arg(0, context);
    set_arg(1, 4); // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    d3d11.impl_ContextMethod(marshaler, 24);

    // Bind a buffer UAV at u2 alongside the render targets. Learn
    // (OMSetRenderTargetsAndUnorderedAccessViews): UAVStartSlot is the absolute
    // u# index for the graphics pipeline. The bind used to be recorded in context
    // state and then dropped -- BackendDrawCall had no u# arrays -- so a pass
    // writing a UAV beside its targets wrote nowhere, with no error anywhere.
    constexpr GuestAddr UAV_BUF_DESC = GPU_BUFFER_DESC + 0x80;
    constexpr GuestAddr UAV_BUF_OUT = GPU_BUFFER_OUT + 0x40;
    constexpr GuestAddr OM_UAV_DESC = GPU_BUFFER_DESC + 0xC0;
    constexpr GuestAddr OM_UAV_OUT = GPU_BUFFER_OUT + 0x50;
    constexpr GuestAddr OM_UAV_BINDING = GPU_BUFFER_OUT + 0x60;
    constexpr u32 UAV_SLOT = 2;
    // ByteWidth, Usage(DEFAULT), Bind(UNORDERED_ACCESS), CPUAccess,
    // Misc(BUFFER_STRUCTURED), StructureByteStride.
    const std::array<u32, 6> uav_buffer_desc{32, 0, 0x80, 0, 0x40, 4};
    ASSERT_TRUE(memory.write_bytes(
        UAV_BUF_DESC, reinterpret_cast<const Byte*>(uav_buffer_desc.data()),
        sizeof(uav_buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, UAV_BUF_DESC);
    set_arg(2, 0);
    set_arg(3, UAV_BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr uav_buffer = memory.read_u64(UAV_BUF_OUT).value();
    // D3D11_UNORDERED_ACCESS_VIEW_DESC + D3D11_BUFFER_UAV:
    // Format(UNKNOWN), ViewDimension(BUFFER=1), FirstElement, NumElements, Flags.
    const std::array<u32, 5> om_uav_desc{0, 1, 0, 8, 0};
    ASSERT_TRUE(memory.write_bytes(
        OM_UAV_DESC, reinterpret_cast<const Byte*>(om_uav_desc.data()),
        sizeof(om_uav_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, uav_buffer);
    set_arg(2, OM_UAV_DESC);
    set_arg(3, OM_UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr om_uav = memory.read_u64(OM_UAV_OUT).value();
    ASSERT_NE(om_uav, 0u);
    ASSERT_TRUE(memory.write_u64(OM_UAV_BINDING, om_uav).is_ok());
    set_arg(0, context);
    set_arg(1, 0xFFFFFFFFu);  // KEEP the render targets bound above
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, UAV_SLOT);
    set_arg(5, 1);
    set_arg(6, OM_UAV_BINDING);
    set_arg(7, 0);
    d3d11.impl_ContextMethod(marshaler, 34);

    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);

    EXPECT_EQ(backend.state.draws, 1u);
    EXPECT_TRUE(backend.state.last_draw_uav_buffer_bound[UAV_SLOT])
        << "the OM-bound UAV must reach the draw at its absolute u# slot";
    for (unsigned slot = 0;
         slot < backend.state.last_draw_uav_buffer_bound.size(); ++slot) {
        if (slot == UAV_SLOT) continue;
        EXPECT_FALSE(backend.state.last_draw_uav_buffer_bound[slot])
            << "u" << slot << " was never bound";
    }
    EXPECT_EQ(backend.state.pipeline_creations, 1u);
    EXPECT_TRUE(backend.state.last_pipeline_fragment_empty);
    EXPECT_EQ(backend.state.last_pass_color_targets, 0u);
    EXPECT_TRUE(backend.state.last_pass_has_depth);
    EXPECT_EQ(backend.state.last_pass_depth_level, 1u);
    EXPECT_EQ(backend.state.last_pass_depth_slice, 1u);
    EXPECT_EQ(backend.state.last_pass_depth_array_size, 2u);
    EXPECT_FALSE(backend.state.last_draw_depth_write_enabled);
    EXPECT_TRUE(backend.state.last_draw_texture_bound);
    EXPECT_TRUE(backend.state.last_draw_stencil_enabled);
    EXPECT_EQ(backend.state.last_draw_stencil_read_mask, 0xA5u);
    EXPECT_EQ(backend.state.last_draw_stencil_write_mask, 0x3Cu);
    EXPECT_EQ(backend.state.last_draw_stencil_reference, 0x5Au);
    EXPECT_EQ(backend.state.last_draw_front_stencil.stencil_fail,
              graphics::BackendStencilOperation::Zero);
    EXPECT_EQ(backend.state.last_draw_front_stencil.depth_fail,
              graphics::BackendStencilOperation::IncrementClamp);
    EXPECT_EQ(backend.state.last_draw_front_stencil.pass,
              graphics::BackendStencilOperation::Replace);
    EXPECT_EQ(backend.state.last_draw_front_stencil.compare,
              graphics::BackendCompare::NotEqual);
    EXPECT_EQ(backend.state.last_draw_back_stencil.stencil_fail,
              graphics::BackendStencilOperation::Invert);
    EXPECT_EQ(backend.state.last_draw_back_stencil.depth_fail,
              graphics::BackendStencilOperation::IncrementWrap);
    EXPECT_EQ(backend.state.last_draw_back_stencil.pass,
              graphics::BackendStencilOperation::DecrementWrap);
    EXPECT_EQ(backend.state.last_draw_back_stencil.compare,
              graphics::BackendCompare::Equal);

    const std::array<u32, 6> stencil_srv_desc{22, 5, 1, 1, 1, 2};
    ASSERT_TRUE(memory.write_bytes(
        STENCIL_SRV_DESC,
        reinterpret_cast<const Byte*>(stencil_srv_desc.data()),
        sizeof(stencil_srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, depth_texture);
    set_arg(2, STENCIL_SRV_DESC);
    set_arg(3, STENCIL_SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr stencil_srv = memory.read_u64(STENCIL_SRV_OUT).value();
    ASSERT_NE(stencil_srv, 0u);
    auto readonly_both_desc = dsv_desc;
    readonly_both_desc[2] = 3;
    ASSERT_TRUE(memory.write_bytes(
        DSV_DESC, reinterpret_cast<const Byte*>(readonly_both_desc.data()),
        sizeof(readonly_both_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, depth_texture);
    set_arg(2, DSV_DESC);
    set_arg(3, READONLY_BOTH_DSV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 10);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr readonly_both_dsv =
        memory.read_u64(READONLY_BOTH_DSV_OUT).value();
    ASSERT_TRUE(memory.write_u64(SRV_BINDING, stencil_srv).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, readonly_both_dsv);
    d3d11.impl_ContextMethod(marshaler, 33);
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BINDING);
    d3d11.impl_ContextMethod(marshaler, 8);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_TRUE(backend.state.last_draw_texture_bound);
    EXPECT_EQ(backend.state.last_texture_views[0].pixel_format,
              graphics::PixelFormat::X32_Stencil8);

    // Rebinding the same subresources through a writable DSV conflicts with
    // the SRV. Windows nulls the conflicting input bind.
    auto writable_dsv_desc = dsv_desc;
    writable_dsv_desc[2] = 0;
    ASSERT_TRUE(memory.write_bytes(
        DSV_DESC, reinterpret_cast<const Byte*>(writable_dsv_desc.data()),
        sizeof(writable_dsv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, depth_texture);
    set_arg(2, DSV_DESC);
    set_arg(3, WRITABLE_DSV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 10);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr writable_dsv = memory.read_u64(WRITABLE_DSV_OUT).value();
    ASSERT_NE(writable_dsv, 0u);
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, writable_dsv);
    d3d11.impl_ContextMethod(marshaler, 33);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_FALSE(backend.state.last_draw_texture_bound);
    EXPECT_EQ(backend.state.last_pass_attachmentless_width, 0u)
        << "a pass with a depth attachment is not attachment-less";

    // Learn (OMSetRenderTargetsAndUnorderedAccessViews, D3D11.1 UAV-only
    // rendering): with no RTV and no DSV the draw still rasterizes, bounded
    // by the viewport, at ForcedSampleCount samples (one when zero); the
    // pixel shader's UAV writes are its output. It used to be dropped.
    constexpr GuestAddr VIEWPORT = DATA_BASE + 0x1E40;
    const std::array<float, 6> viewport{10.0f, 20.0f, 320.5f, 200.0f,
                                        0.0f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        VIEWPORT, reinterpret_cast<const Byte*>(viewport.data()),
        sizeof(viewport)).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, VIEWPORT);
    d3d11.impl_ContextMethod(marshaler, 44);  // RSSetViewports
    set_arg(0, context);
    set_arg(1, 0);  // NumRTVs = 0: unbind every RTV
    set_arg(2, 0);
    set_arg(3, 0);  // no DSV
    set_arg(4, UAV_SLOT);
    set_arg(5, 1);
    set_arg(6, OM_UAV_BINDING);
    set_arg(7, 0);
    d3d11.impl_ContextMethod(marshaler, 34);
    const unsigned draws_before = backend.state.draws;
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.draws, draws_before + 1)
        << "a draw with no RTV and no DSV must still be submitted";
    EXPECT_EQ(backend.state.last_pass_color_targets, 0u);
    EXPECT_FALSE(backend.state.last_pass_has_depth);
    EXPECT_EQ(backend.state.last_pass_attachmentless_width, 331u)
        << "ceil(TopLeftX + Width)";
    EXPECT_EQ(backend.state.last_pass_attachmentless_height, 220u);
    EXPECT_EQ(backend.state.last_pass_attachmentless_samples, 1u);
    EXPECT_TRUE(backend.state.last_pipeline_attachmentless);
    EXPECT_TRUE(backend.state.last_draw_uav_buffer_bound[UAV_SLOT]);

    // Learn D3D11_TEX3D_RTV: a Texture3D view covers W slices
    // [FirstWSlice, FirstWSlice + WSize), and SV_RenderTargetArrayIndex picks
    // one of them per primitive. The pass must carry the whole window, not
    // just FirstWSlice (UE draws its 32^3 colour LUT this way).
    constexpr GuestAddr VOL_DESC = DATA_BASE + 0x1F00;
    constexpr GuestAddr VOL_OUT = DATA_BASE + 0x1F40;
    constexpr GuestAddr VOL_RTV_DESC = DATA_BASE + 0x1F50;
    constexpr GuestAddr VOL_RTV_OUT = DATA_BASE + 0x1F70;
    constexpr GuestAddr VOL_RTV_BINDING = DATA_BASE + 0x1F78;
    // D3D11_TEXTURE3D_DESC: 4x4x8, 1 mip, R8G8B8A8_UNORM (28), DEFAULT,
    // SHADER_RESOURCE | RENDER_TARGET.
    const std::array<u32, 9> volume_desc{4, 4, 8, 1, 28, 0, 0x8 | 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        VOL_DESC, reinterpret_cast<const Byte*>(volume_desc.data()),
        sizeof(volume_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, VOL_DESC);
    set_arg(2, 0);
    set_arg(3, VOL_OUT);
    d3d11.impl_DeviceMethod(marshaler, 6);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr volume = memory.read_u64(VOL_OUT).value();
    // Format, ViewDimension(TEXTURE3D=8), MipSlice, FirstWSlice=2, WSize=4.
    const std::array<u32, 5> volume_rtv{28, 8, 0, 2, 4};
    ASSERT_TRUE(memory.write_bytes(
        VOL_RTV_DESC, reinterpret_cast<const Byte*>(volume_rtv.data()),
        sizeof(volume_rtv)).is_ok());
    set_arg(0, device);
    set_arg(1, volume);
    set_arg(2, VOL_RTV_DESC);
    set_arg(3, VOL_RTV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_u64(VOL_RTV_BINDING,
                                 memory.read_u64(VOL_RTV_OUT).value()).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, VOL_RTV_BINDING);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 33);  // OMSetRenderTargets
    const unsigned draws_before_volume = backend.state.draws;
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.draws, draws_before_volume + 1);
    EXPECT_TRUE(backend.state.last_pass_volume_target0);
    EXPECT_EQ(backend.state.last_pass_depth_plane0, 2u) << "FirstWSlice";
    EXPECT_EQ(backend.state.last_pass_color_array_size0, 4u)
        << "WSize: the slices SV_RenderTargetArrayIndex can reach";
}

// ---------------------------------------------------------------------------
// Deferred contexts
// ---------------------------------------------------------------------------

// A deferred context is a second ID3D11DeviceContext with its OWN binding state
// whose operations are recorded rather than executed, until FinishCommandList
// hands them over as an ID3D11CommandList.
//
// The state being per-context is the load-bearing part. It used to be keyed by
// DEVICE, which was harmless only because CreateDeferredContext returned
// E_NOTIMPL: a deferred context would otherwise have shared the immediate
// context's bindings, and each would have corrupted the other.
TEST_F(D3D11ManagerTest, DeferredContextHasItsOwnBindingState) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr BUF_DESC = DATA_BASE + 0x1010;
    constexpr GuestAddr BUF_OUT = DATA_BASE + 0x1050;
    constexpr GuestAddr BINDING = DATA_BASE + 0x1060;
    constexpr GuestAddr READBACK = DATA_BASE + 0x1070;

    // ContextFlags is reserved and must be zero.
    set_arg(0, device);
    set_arg(1, 1);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG)
        << "ContextFlags is reserved; a non-zero value must be refused";

    ASSERT_TRUE(memory.write_u64(CTX_OUT, 0).is_ok());
    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();
    ASSERT_NE(deferred, 0u);
    ASSERT_NE(deferred, immediate) << "a deferred context is a distinct object";

    // A constant buffer, bound on the DEFERRED context only.
    const std::array<u32, 6> desc{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUF_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUF_DESC);
    set_arg(2, 0);
    set_arg(3, BUF_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr cbuffer = memory.read_u64(BUF_OUT).value();
    ASSERT_NE(cbuffer, 0u);

    ASSERT_TRUE(memory.write_u64(BINDING, cbuffer).is_ok());
    set_arg(0, deferred);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDING);
    d3d11.impl_ContextMethod(marshaler, 7);  // VSSetConstantBuffers

    // The deferred context sees it...
    ASSERT_TRUE(memory.write_u64(READBACK, 0).is_ok());
    set_arg(0, deferred);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, READBACK);
    d3d11.impl_ContextMethod(marshaler, 72);  // VSGetConstantBuffers
    EXPECT_EQ(memory.read_u64(READBACK).value(), cbuffer);

    // ...and the immediate context does NOT.
    ASSERT_TRUE(memory.write_u64(READBACK, 0xA5A5ull).is_ok());
    set_arg(0, immediate);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, READBACK);
    d3d11.impl_ContextMethod(marshaler, 72);
    EXPECT_EQ(memory.read_u64(READBACK).value(), 0u)
        << "the immediate context shares binding state with the deferred one";
}

// FinishCommandList is valid only on a deferred context, and the list it returns
// is executed by ExecuteCommandList -- which may run it more than once.
TEST_F(D3D11ManagerTest, FinishAndExecuteCommandListRoundTrip) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr immediate = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr CTX_OUT = DATA_BASE + 0x1000;
    constexpr GuestAddr LIST_OUT = DATA_BASE + 0x1010;

    // Learn: FinishCommandList on an IMMEDIATE context returns
    // DXGI_ERROR_INVALID_CALL. Returning a list would hand the guest a recording
    // of nothing.
    ASSERT_TRUE(memory.write_u64(LIST_OUT, 0xA5A5ull).is_ok());
    set_arg(0, immediate);
    set_arg(1, 1);
    set_arg(2, LIST_OUT);
    d3d11.impl_ContextMethod(marshaler, 114);
    EXPECT_EQ(cpu.gpr.rax, DXGI_ERROR_INVALID_CALL);
    EXPECT_EQ(memory.read_u64(LIST_OUT).value(), 0u);

    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, CTX_OUT);
    d3d11.impl_DeviceMethod(marshaler, 27);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr deferred = memory.read_u64(CTX_OUT).value();
    ASSERT_NE(deferred, 0u);

    ASSERT_TRUE(memory.write_u64(LIST_OUT, 0).is_ok());
    set_arg(0, deferred);
    set_arg(1, 1);  // RestoreDeferredContextState
    set_arg(2, LIST_OUT);
    d3d11.impl_ContextMethod(marshaler, 114);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr list = memory.read_u64(LIST_OUT).value();
    ASSERT_NE(list, 0u);

    // GetContextFlags on the list reports what the context was created with.
    set_arg(0, list);
    d3d11.impl_CommandListMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    // Command lists are the runtime's own recordings (DriverCommandLists is
    // FALSE, as on WARP), replayed call by call -- never a backend list. The
    // old expectation of a backend submission per Execute described that
    // implementation, not Windows. What Windows guarantees is below.
    set_arg(0, immediate);
    set_arg(1, list);
    set_arg(2, 1);  // RestoreContextState
    d3d11.impl_ContextMethod(marshaler, 58);  // ExecuteCommandList

    // Learn: the list stays valid, so it may be executed again.
    set_arg(0, immediate);
    set_arg(1, list);
    set_arg(2, 1);
    d3d11.impl_ContextMethod(marshaler, 58);
    set_arg(0, list);
    d3d11.impl_CommandListMethod(marshaler, 1);  // AddRef
    EXPECT_EQ(cpu.gpr.rax, 2u) << "a command list must survive being executed";
    set_arg(0, list);
    d3d11.impl_CommandListMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(backend.state.list_submissions, 0u);

    // The deferred context keeps recording after finishing, so it can build a
    // second list without being recreated.
    ASSERT_TRUE(memory.write_u64(LIST_OUT, 0).is_ok());
    set_arg(0, deferred);
    set_arg(1, 1);
    set_arg(2, LIST_OUT);
    d3d11.impl_ContextMethod(marshaler, 114);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK)
        << "FinishCommandList must leave the context able to record again";
    EXPECT_NE(memory.read_u64(LIST_OUT).value(), 0u);

    // An address that is not a live list is a caller error, not a crash. The
    // method is void; the immediate context's state is left alone.
    const unsigned clears = backend.state.texture_clears;
    set_arg(0, immediate);
    set_arg(1, list + 0x4000);
    set_arg(2, 1);
    d3d11.impl_ContextMethod(marshaler, 58);
    EXPECT_EQ(backend.state.texture_clears, clears);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
