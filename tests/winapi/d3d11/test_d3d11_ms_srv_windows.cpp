// SPDX-License-Identifier: LGPL-2.1-only
//
// Draw propagation of Texture2DMS shader-resource-view windows.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, DrawPropagatesTexture2DMSShaderResourceViewWindows) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr MS_TEXTURE_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr MS_ARRAY_TEXTURE_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr MS_VIEW_OUT = DATA_BASE + 0x910;
    constexpr GuestAddr MS_ARRAY_VIEW_OUT = DATA_BASE + 0x918;
    constexpr GuestAddr RT_TEXTURE_OUT = DATA_BASE + 0x920;
    constexpr GuestAddr RTV_OUT = DATA_BASE + 0x928;
    constexpr GuestAddr VS_OUT = DATA_BASE + 0x930;
    constexpr GuestAddr PS_OUT = DATA_BASE + 0x938;
    constexpr GuestAddr VS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr PS_BYTECODE = DATA_BASE + 0x1800;
    constexpr GuestAddr BINDINGS = DATA_BASE + 0x1C00;
    constexpr GuestAddr STRIDE = DATA_BASE + 0x1C20;
    constexpr GuestAddr OFFSET = DATA_BASE + 0x1C24;
    constexpr GuestAddr SAMPLER_DESC = DATA_BASE + 0x1C40;
    constexpr GuestAddr SAMPLER_OUT = DATA_BASE + 0x1C80;
    constexpr GuestAddr SAMPLER_OBSERVED = DATA_BASE + 0x1CC0;
    constexpr GuestAddr MIP_TEXTURE_OUT = DATA_BASE + 0x940;
    constexpr GuestAddr MIP_VIEW_OUT = DATA_BASE + 0x948;
    constexpr GuestAddr RASTER_DESC = DATA_BASE + 0x1D00;
    constexpr GuestAddr RASTER_OUT = DATA_BASE + 0x1D40;
    constexpr GuestAddr SCISSOR_RECT = DATA_BASE + 0x1D80;

    const auto create_texture = [&](u32 mip_levels, u32 array_size, u32 sample_count,
                                    u32 bind_flags, GuestAddr output) {
        const std::array<u32, 11> desc{
            16, 16, mip_levels, array_size, 28, sample_count, 0,
            0, bind_flags, 0, 0,
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

    const GuestAddr ms_texture = create_texture(1, 1, 4, 0x8, MS_TEXTURE_OUT);
    const GuestAddr ms_array_texture =
        create_texture(1, 4, 4, 0x8, MS_ARRAY_TEXTURE_OUT);
    const GuestAddr render_target =
        create_texture(1, 1, 1, 0x20, RT_TEXTURE_OUT);
    const GuestAddr mip_texture =
        create_texture(2, 1, 1, 0x8, MIP_TEXTURE_OUT);
    ASSERT_NE(ms_texture, 0u);
    ASSERT_NE(ms_array_texture, 0u);
    ASSERT_NE(render_target, 0u);
    ASSERT_NE(mip_texture, 0u);

    const auto create_srv = [&](GuestAddr texture,
                                const std::array<u32, 4>& desc,
                                GuestAddr output) {
        EXPECT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, SRV_DESC);
        set_arg(3, output);
        d3d11.impl_DeviceMethod(marshaler, 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(output).value();
    };
    const GuestAddr ms_view =
        create_srv(ms_texture, {28, 6, 0, 0}, MS_VIEW_OUT);
    const GuestAddr ms_array_view =
        create_srv(ms_array_texture, {28, 7, 1, 2}, MS_ARRAY_VIEW_OUT);
    const GuestAddr mip_view =
        create_srv(mip_texture, {28, 4, 0, 2}, MIP_VIEW_OUT);
    ASSERT_NE(ms_view, 0u);
    ASSERT_NE(ms_array_view, 0u);
    ASSERT_NE(mip_view, 0u);

    set_arg(0, device);
    set_arg(1, render_target);
    set_arg(2, 0);
    set_arg(3, RTV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr rtv = memory.read_u64(RTV_OUT).value();
    ASSERT_NE(rtv, 0u);

    ASSERT_TRUE(memory.write_bytes(
        VS_BYTECODE, vs_main, sizeof(vs_main)).is_ok());
    set_arg(0, device);
    set_arg(1, VS_BYTECODE);
    set_arg(2, sizeof(vs_main));
    set_arg(3, 0);
    set_arg(4, VS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 12);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr vertex_shader = memory.read_u64(VS_OUT).value();

    ASSERT_TRUE(memory.write_bytes(
        PS_BYTECODE, ps_color, sizeof(ps_color)).is_ok());
    set_arg(0, device);
    set_arg(1, PS_BYTECODE);
    set_arg(2, sizeof(ps_color));
    set_arg(3, 0);
    set_arg(4, PS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 15);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr pixel_shader = memory.read_u64(PS_OUT).value();

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

    set_arg(0, vertex_buffer);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        vertex_buffer, &D3D11Manager::impl_BufferMethod)) << "context must retain the bound vertex buffer";

    set_arg(0, context);
    set_arg(1, vertex_shader);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 11);
    set_arg(0, context);
    set_arg(1, pixel_shader);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 9);

    // D3D11 shader setters retain their shader interface. Dropping the
    // caller-owned references must leave both stages valid for later draws.
    set_arg(0, vertex_shader);
    d3d11.impl_VertexShaderMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        vertex_shader, &D3D11Manager::impl_VertexShaderMethod));
    set_arg(0, pixel_shader);
    d3d11.impl_PixelShaderMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        pixel_shader, &D3D11Manager::impl_PixelShaderMethod));

    const std::array<u32, 13> comparison_sampler_desc{
        0x80, // D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT
        3, 3, 3, // CLAMP
        0,       // MipLODBias
        1,       // MaxAnisotropy
        2,       // D3D11_COMPARISON_LESS
        0, 0, 0, 0, // BorderColor
        0x3f800000, 0x3f800000, // MinLOD=MaxLOD=1
    };
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC,
        reinterpret_cast<const Byte*>(comparison_sampler_desc.data()),
        sizeof(comparison_sampler_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr comparison_sampler =
        memory.read_u64(SAMPLER_OUT).value();
    ASSERT_NE(comparison_sampler, 0u);
    set_arg(0, comparison_sampler);
    set_arg(1, SAMPLER_OBSERVED);
    d3d11.impl_SamplerStateMethod(marshaler, 7);
    // GetDesc reports the canonical description: MaxAnisotropy reads 0 for
    // a filter that is not anisotropic (d3d11_a04_state_objects_probe "smp
    // roundtrip getdesc"; this expected the 1 passed in before the oracle
    // measured it).
    auto reported_desc = comparison_sampler_desc;
    reported_desc[5] = 0;
    for (usize i = 0; i < reported_desc.size(); ++i) {
        EXPECT_EQ(memory.read_u32(SAMPLER_OBSERVED + i * 4).value(),
                  reported_desc[i]);
    }
    ASSERT_TRUE(memory.write_u64(BINDINGS, comparison_sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    d3d11.impl_ContextMethod(marshaler, 26); // VSSetSamplers

    // D3D11 context setters retain bound interfaces. Dropping the caller's
    // references must leave both objects alive for the following draw.
    set_arg(0, comparison_sampler);
    d3d11.impl_SamplerStateMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        comparison_sampler, &D3D11Manager::impl_SamplerStateMethod));

    ASSERT_TRUE(memory.write_u64(BINDINGS, ms_view).is_ok());
    ASSERT_TRUE(memory.write_u64(BINDINGS + 8, ms_array_view).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 2);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 8);
    set_arg(2, 1);
    d3d11.impl_ContextMethod(marshaler, 25); // VSSetShaderResources
    set_arg(0, ms_view);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);  // the application's count
    EXPECT_TRUE(kept_alive_by_runtime(
        ms_view, &D3D11Manager::impl_ShaderResourceViewMethod));

    ASSERT_TRUE(memory.write_u64(BINDINGS, rtv).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, BINDINGS);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 33);
    set_arg(0, context);
    set_arg(1, 4); // D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST
    d3d11.impl_ContextMethod(marshaler, 24);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);

    ASSERT_EQ(backend.state.draws, 1u);
    // The push block: 16 LOD biases, 8 border colours (192 bytes), one
    // buffer-view size per t# (128) and u# (8) slot for the bounds guard, the
    // capture SV_InstanceID bias padded to 16 bytes, the fixed-function word
    // quad (depth offset, SampleMask, viewport depth range), the border
    // colours of s8..s15, the sixteen b# read limits, then the sixteen
    // min/max sampler states.
    EXPECT_EQ(backend.state.last_draw_fragment_constants_size,
              16u * sizeof(float) + 8u * 4u * sizeof(float) +
                  (128u + 8u) * sizeof(u32) + 16u + 16u +
                  8u * 4u * sizeof(float) + 16u * sizeof(u32) +
                  16u * 4u * sizeof(u32));
    const auto& texture2dms = backend.state.last_texture_views[0];
    EXPECT_EQ(texture2dms.dimension,
              graphics::BackendTextureViewDimension::Texture2DMS);
    EXPECT_EQ(texture2dms.most_detailed_mip, 0u);
    EXPECT_EQ(texture2dms.mip_levels, 1u);
    EXPECT_EQ(texture2dms.first_array_slice, 0u);
    EXPECT_EQ(texture2dms.array_size, 1u);
    const auto& texture2dms_array = backend.state.last_texture_views[1];
    EXPECT_EQ(texture2dms_array.dimension,
              graphics::BackendTextureViewDimension::Texture2DMSArray);
    EXPECT_EQ(texture2dms_array.most_detailed_mip, 0u);
    EXPECT_EQ(texture2dms_array.mip_levels, 1u);
    EXPECT_EQ(texture2dms_array.first_array_slice, 1u);
    EXPECT_EQ(texture2dms_array.array_size, 2u);
    EXPECT_TRUE(backend.state.last_draw_vertex_texture_bound);
    const auto& vertex_texture2dms =
        backend.state.last_vertex_texture_views[0];
    EXPECT_EQ(vertex_texture2dms.dimension,
              graphics::BackendTextureViewDimension::Texture2DMS);
    EXPECT_TRUE(backend.state.last_vertex_sampler.comparison_enabled);
    EXPECT_EQ(backend.state.last_vertex_sampler.comparison,
              graphics::BackendCompare::Less);
    EXPECT_TRUE(backend.state.last_draw_comparison_enabled);
    EXPECT_EQ(backend.state.last_draw_sampler_comparison,
              graphics::BackendCompare::Less);
    // MinLOD = MaxLOD = 1 through a point-mip sampler whose min and mag
    // filters agree: the backend clamps are in Metal's LOD space, shifted by
    // 1/64 so Apple's mip rounding (next level from .515625) lands on D3D's
    // floor(LOD + 0.5) (D3D11.3 7.18.10; resolve_sampler).
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_min_clamp, 1.0f + 1.0f / 64);
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_max_clamp, 1.0f + 1.0f / 64);

    // MIN_LINEAR_MAG_MIP_POINT decodes each filter field independently.
    // MaxAnisotropy is ignored for a non-anisotropic filter, including zero.
    auto mixed_filter_desc = comparison_sampler_desc;
    mixed_filter_desc[0] = 0x10;
    mixed_filter_desc[1] = 5; // MIRROR_ONCE
    mixed_filter_desc[2] = 1; // WRAP
    mixed_filter_desc[3] = 2; // MIRROR
    mixed_filter_desc[4] = 0x40000000; // MipLODBias = 2
    mixed_filter_desc[5] = 0;
    mixed_filter_desc[6] = 1;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(mixed_filter_desc.data()),
        sizeof(mixed_filter_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mixed_filter_sampler =
        memory.read_u64(SAMPLER_OUT).value();
    ASSERT_TRUE(memory.write_u64(BINDINGS, mip_view).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 8);
    ASSERT_TRUE(memory.write_u64(BINDINGS, mixed_filter_sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_min_filter,
              graphics::BackendFilter::Linear);
    EXPECT_EQ(backend.state.last_draw_mag_filter,
              graphics::BackendFilter::Nearest);
    EXPECT_EQ(backend.state.last_draw_mip_filter,
              graphics::BackendFilter::Nearest);
    EXPECT_EQ(backend.state.last_draw_max_anisotropy, 1u);
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_bias, 2.0f);
    EXPECT_FLOAT_EQ(backend.state.last_draw_shader_lod_bias, 2.0f);
    EXPECT_EQ(backend.state.last_draw_address_u,
              graphics::BackendAddressMode::MirrorOnce);
    EXPECT_EQ(backend.state.last_draw_address_v,
              graphics::BackendAddressMode::Wrap);
    EXPECT_EQ(backend.state.last_draw_address_w,
              graphics::BackendAddressMode::Mirror);

    auto anisotropic_desc = mixed_filter_desc;
    anisotropic_desc[0] = 0x55;
    anisotropic_desc[5] = 16;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(anisotropic_desc.data()),
        sizeof(anisotropic_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr anisotropic_sampler = memory.read_u64(SAMPLER_OUT).value();
    ASSERT_TRUE(memory.write_u64(BINDINGS, anisotropic_sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_min_filter,
              graphics::BackendFilter::Linear);
    EXPECT_EQ(backend.state.last_draw_mag_filter,
              graphics::BackendFilter::Linear);
    EXPECT_EQ(backend.state.last_draw_mip_filter,
              graphics::BackendFilter::Linear);
    EXPECT_EQ(backend.state.last_draw_max_anisotropy, 16u);

    auto maximum_desc = mixed_filter_desc;
    maximum_desc[0] = 0x195; // MAXIMUM_MIN_MAG_MIP_LINEAR
    maximum_desc[5] = 8;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(maximum_desc.data()),
        sizeof(maximum_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr maximum_sampler = memory.read_u64(SAMPLER_OUT).value();
    ASSERT_TRUE(memory.write_u64(BINDINGS, maximum_sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    // MINIMUM_ / MAXIMUM_ reduce in the translated shader
    // (DxbcTranslationOptions::minmax_samplers): the Metal sampler
    // point-samples each texel the shader asks for, with no reduction, no
    // anisotropy and no LOD clamp of its own (D3D11.3 5.9.4.5.6).
    EXPECT_EQ(backend.state.last_draw_min_filter,
              graphics::BackendFilter::Nearest);
    EXPECT_EQ(backend.state.last_draw_mag_filter,
              graphics::BackendFilter::Nearest);
    EXPECT_EQ(backend.state.last_draw_mip_filter,
              graphics::BackendFilter::Nearest);
    EXPECT_EQ(backend.state.last_draw_max_anisotropy, 1u);
    EXPECT_FLOAT_EQ(backend.state.last_draw_lod_min_clamp, 0.0f);
    // The shader reads the state from push member 13 at s0: the filter,
    // MinLOD, MaxLOD and the effective MaxAnisotropy -- 1 here: a filter
    // that is not anisotropic keeps MaxAnisotropy 0 (the measured canonical
    // desc), served as 1.
    const auto push_u32 = [&](usize offset) {
        const auto& block = backend.state.last_draw_fragment_constants;
        u32 value = 0;
        if (offset + 4 <= block.size()) {
            std::memcpy(&value, block.data() + offset, 4);
        }
        return value;
    };
    EXPECT_EQ(push_u32(graphics::kMinMaxSamplerOffset), 0x195u);
    EXPECT_EQ(push_u32(graphics::kMinMaxSamplerOffset + 12), 1u);

    auto minimum_desc = maximum_desc;
    minimum_desc[0] = 0x115; // MINIMUM_MIN_MAG_MIP_LINEAR
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(minimum_desc.data()),
        sizeof(minimum_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr minimum_sampler = memory.read_u64(SAMPLER_OUT).value();
    ASSERT_TRUE(memory.write_u64(BINDINGS, minimum_sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_min_filter,
              graphics::BackendFilter::Nearest);
    EXPECT_EQ(push_u32(graphics::kMinMaxSamplerOffset), 0x115u);

    // MAXIMUM_ANISOTROPIC with MaxAnisotropy 6: WARP filters with the power
    // of two at or below it (d3d11_minmax_probe [aniso] "x6" = "x4").
    auto reducing_aniso_desc = maximum_desc;
    reducing_aniso_desc[0] = 0x1D5;
    reducing_aniso_desc[5] = 6;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(reducing_aniso_desc.data()),
        sizeof(reducing_aniso_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_u64(BINDINGS,
                                 memory.read_u64(SAMPLER_OUT).value())
                    .is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_max_anisotropy, 1u);
    EXPECT_EQ(push_u32(graphics::kMinMaxSamplerOffset), 0x1D5u);
    EXPECT_EQ(push_u32(graphics::kMinMaxSamplerOffset + 12), 4u);

    // Mixed point/linear min/max filters are served like every other one
    // (measured S_OK on WARP, d3d11_minmax_probe [caps]).
    maximum_desc[0] = 0x190; // MAXIMUM_MIN_LINEAR_MAG_MIP_POINT
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(maximum_desc.data()),
        sizeof(maximum_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    auto border_desc = mixed_filter_desc;
    border_desc[0] = 0;
    border_desc[1] = 4; // BORDER
    border_desc[2] = 3; // CLAMP
    border_desc[3] = 1; // WRAP
    border_desc[7] = border_desc[8] = border_desc[9] =
        border_desc[10] = 0x3f800000; // opaque white float4
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(border_desc.data()),
        sizeof(border_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr border_sampler = memory.read_u64(SAMPLER_OUT).value();
    ASSERT_TRUE(memory.write_u64(BINDINGS, border_sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 10);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_address_u,
              graphics::BackendAddressMode::Border);
    EXPECT_EQ(backend.state.last_draw_address_v,
              graphics::BackendAddressMode::Clamp);
    EXPECT_EQ(backend.state.last_draw_address_w,
              graphics::BackendAddressMode::Wrap);
    EXPECT_EQ(backend.state.last_draw_sampler_border_color, 0xffffffffu);

    // An arbitrary colour (red 0.5) is not a Metal border colour; it is
    // served by the border twins (11.15), so it is accepted. It used to be
    // refused here.
    border_desc[7] = 0x3f000000;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(border_desc.data()),
        sizeof(border_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    // An arbitrary BorderColor remains legal when no address axis can
    // observe it; CreateSamplerState/GetDesc still preserve the descriptor.
    border_desc[1] = 3; // CLAMP
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(border_desc.data()),
        sizeof(border_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    std::array<u32, 10> raster_desc{
        2, 1, 0, 0, 0, 0, 1, 0, 0, 0,
    }; // WIREFRAME, CULL_NONE, DepthClipEnable
    ASSERT_TRUE(memory.write_bytes(
        RASTER_DESC, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, RASTER_DESC);
    set_arg(2, RASTER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(RASTER_OUT).value());
    d3d11.impl_ContextMethod(marshaler, 43);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_fill_mode,
              graphics::BackendFillMode::Lines);
    EXPECT_TRUE(backend.state.last_draw_depth_clip_enabled);
    EXPECT_FLOAT_EQ(backend.state.last_draw_depth_bias, 0.0f);

    const std::array<u32, 4> scissor_rect{1, 1, 8, 8};
    ASSERT_TRUE(memory.write_bytes(
        SCISSOR_RECT, reinterpret_cast<const Byte*>(scissor_rect.data()),
        sizeof(scissor_rect)).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, SCISSOR_RECT);
    d3d11.impl_ContextMethod(marshaler, 45);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_FALSE(backend.state.last_draw_scissor_enabled);

    raster_desc[6] = 0; // DepthClipEnable
    ASSERT_TRUE(memory.write_bytes(
        RASTER_DESC, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, RASTER_DESC);
    set_arg(2, RASTER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(RASTER_OUT).value());
    d3d11.impl_ContextMethod(marshaler, 43);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_FALSE(backend.state.last_draw_depth_clip_enabled);

    raster_desc[3] = static_cast<u32>(-7); // DepthBias
    raster_desc[4] = std::bit_cast<u32>(-0.25f); // DepthBiasClamp
    raster_desc[5] = std::bit_cast<u32>(1.5f); // SlopeScaledDepthBias
    ASSERT_TRUE(memory.write_bytes(
        RASTER_DESC, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, RASTER_DESC);
    set_arg(2, RASTER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(RASTER_OUT).value());
    d3d11.impl_ContextMethod(marshaler, 43);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_FLOAT_EQ(backend.state.last_draw_depth_bias, -7.0f);
    EXPECT_FLOAT_EQ(backend.state.last_draw_depth_bias_slope_scale, 1.5f);
    EXPECT_FLOAT_EQ(backend.state.last_draw_depth_bias_clamp, -0.25f);

    set_arg(0, context);
    set_arg(1, 0); // restore the implicit D3D11 rasterizer state
    d3d11.impl_ContextMethod(marshaler, 43);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_EQ(backend.state.last_draw_cull_mode,
              graphics::BackendCullMode::Back);

    raster_desc[7] = 1; // ScissorEnable
    ASSERT_TRUE(memory.write_bytes(
        RASTER_DESC, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, RASTER_DESC);
    set_arg(2, RASTER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 22);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(RASTER_OUT).value());
    d3d11.impl_ContextMethod(marshaler, 43);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    EXPECT_TRUE(backend.state.last_draw_scissor_enabled);

    raster_desc[0] = 1; // invalid FillMode
    ASSERT_TRUE(memory.write_bytes(
        RASTER_DESC, reinterpret_cast<const Byte*>(raster_desc.data()),
        sizeof(raster_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, RASTER_DESC);
    set_arg(2, RASTER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 22);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // D3D11_MIN_MAXANISOTROPY is 0: an anisotropic filter with MaxAnisotropy
    // 0 is legal, 17 is not (d3d11_a04_state_objects_probe "smp ANISOTROPIC
    // aniso=0 / 17"; 0 was expected to fail here before that measurement).
    anisotropic_desc[5] = 0;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(anisotropic_desc.data()),
        sizeof(anisotropic_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    anisotropic_desc[5] = 17;
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(anisotropic_desc.data()),
        sizeof(anisotropic_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // MinLOD > MaxLOD is legal and MinLOD wins (D3D11.3 7.18.11; Windows
    // accepts MinLOD 3 > MaxLOD 1 and samples mip 3, d3d11_a07_sampler_probe
    // `M6 MinLOD=3 > MaxLOD=1`).
    auto inverted_lod_desc = comparison_sampler_desc;
    inverted_lod_desc[11] = 0x40000000; // MinLOD = 2
    inverted_lod_desc[12] = 0x3f800000; // MaxLOD = 1
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(inverted_lod_desc.data()),
        sizeof(inverted_lod_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
