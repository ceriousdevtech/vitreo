// SPDX-License-Identifier: LGPL-2.1-only
//
// Device5/event queries, blend state, compute shader creation/binding and blend factors.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, QueryInterfaceDevice5AndCreateEventQuery) {
    prepare_create();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();

    constexpr std::array<u8, 16> IID_DEVICE5{
        0x02, 0xE2, 0xFD, 0x8F, 0xE7, 0xA0, 0xDF, 0x45,
        0x9E, 0x01, 0xE8, 0x37, 0x80, 0x1B, 0x5E, 0xA0};
    constexpr GuestAddr IID_ADDR = DATA_BASE + 0x220;
    constexpr GuestAddr QI_OUT = DATA_BASE + 0x230;
    ASSERT_TRUE(memory.write_bytes(IID_ADDR, IID_DEVICE5.data(), 16).is_ok());
    set_arg(0, device);
    set_arg(1, IID_ADDR);
    set_arg(2, QI_OUT);
    d3d11.impl_DeviceMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(QI_OUT).value(), device);

    constexpr GuestAddr QUERY_DESC = DATA_BASE + 0x240;
    constexpr GuestAddr QUERY_OUT = DATA_BASE + 0x250;
    ASSERT_TRUE(memory.write_u32(QUERY_DESC, 0).is_ok()); // EVENT
    ASSERT_TRUE(memory.write_u32(QUERY_DESC + 4, 0).is_ok());
    set_arg(0, device);
    set_arg(1, QUERY_DESC);
    set_arg(2, QUERY_OUT);
    d3d11.impl_DeviceMethod(marshaler, 24);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr query = memory.read_u64(QUERY_OUT).value();
    ASSERT_NE(query, 0u);

    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    set_arg(0, query);
    d3d11.impl_QueryMethod(marshaler, 7); // GetDataSize
    EXPECT_EQ(cpu.gpr.rax, 4u);

    set_arg(0, context);
    set_arg(1, query);
    d3d11.impl_ContextMethod(marshaler, 28); // End (Begin is disabled)
    // The GPU reaches the End (EventAndTimestampQueriesAnswerFromTheGpuPoint
    // covers the not-yet-reached S_FALSE side).
    ASSERT_FALSE(backend.state.gpu_points.empty());
    backend.state.gpu_points.back()->is_ready = true;
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 29); // status poll
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    constexpr GuestAddr EVENT_OUT = DATA_BASE + 0x258;
    ASSERT_TRUE(memory.write_u32(EVENT_OUT, 0).is_ok());
    set_arg(0, context);
    set_arg(1, query);
    set_arg(2, EVENT_OUT);
    set_arg(3, 4);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 29);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(EVENT_OUT).value(), 1u);

    set_arg(3, 1); // wrong size
    d3d11.impl_ContextMethod(marshaler, 29);
    EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::E_INVALIDARG);

    set_arg(0, query);
    d3d11.impl_QueryMethod(marshaler, 2); // Release
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(D3D11ManagerTest, CreateBlendStateAndGeometryShaderHonestFail) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr BLEND_DESC = DATA_BASE + 0x280;
    constexpr GuestAddr BLEND_OUT = DATA_BASE + 0x3C0;
    constexpr GuestAddr BLEND_OBSERVED = DATA_BASE + 0x800;
    for (u32 i = 0; i < 66; ++i) {
        ASSERT_TRUE(memory.write_u32(BLEND_DESC + i * 4, 0u)
                        .is_ok());
    }
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 4, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 36, 0xFu).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 12, 2).is_ok()); // SRC ONE
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 16, 1).is_ok()); // DEST ZERO
    constexpr GuestAddr RT1 = BLEND_DESC + 8 + 32;
    ASSERT_TRUE(memory.write_u32(RT1, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 4, 5).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 8, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 12, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 16, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 20, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 24, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(RT1 + 28, 0x3).is_ok());

    set_arg(0, device);
    set_arg(1, BLEND_DESC);
    set_arg(2, BLEND_OUT);
    d3d11.impl_DeviceMethod(marshaler, 20);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr blend_state = memory.read_u64(BLEND_OUT).value();
    ASSERT_NE(blend_state, 0u);

    set_arg(0, blend_state);
    set_arg(1, BLEND_OBSERVED);
    d3d11.impl_BlendStateMethod(marshaler, 7);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 4).value(), 1u);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 8 + 32).value(), 1u);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 8 + 32 + 4).value(), 5u);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 8 + 32 + 8).value(), 6u);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 8 + 32 + 12).value(), 2u);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 8 + 32 + 24).value(), 3u);
    EXPECT_EQ(memory.read_u32(BLEND_OBSERVED + 8 + 32 + 28).value(), 0x3u);

    set_arg(0, device);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, BLEND_OUT);
    d3d11.impl_DeviceMethod(marshaler, 13); // CreateGeometryShader
    // Geometry shaders are served since 11.16 (D3D11GeometryTest); a NULL
    // bytecode is now the argument error it is for every Create*Shader.
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    set_arg(0, device);
    set_arg(1, 5); // D3D11_OPTIONS
    set_arg(2, COLOR);
    set_arg(3, 56);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // ExtendedResourceSharing is BOOL[13]. It promises the D3D11.1 extended
    // shared-format set and shapes (Learn, Direct3D 11.1 features); the
    // answer is the backend's (supports_extended_sharing): this test backend
    // shares single images only, so FALSE here (the Metal backend: TRUE,
    // D3D11ExtendedSharingTest).
    EXPECT_EQ(memory.read_u32(COLOR + 13 * 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(COLOR + 0).value(), 0u); // OutputMergerLogicOp
}

TEST_F(D3D11ManagerTest, DispatchBindsAVolumeUnorderedAccessView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr CS_OUT = DATA_BASE + 0x1200;
    constexpr GuestAddr TEX3D_DESC = DATA_BASE + 0x1300;
    constexpr GuestAddr TEX3D_OUT = DATA_BASE + 0x1340;
    constexpr GuestAddr UAV_DESC = DATA_BASE + 0x1350;
    constexpr GuestAddr UAV_OUT = DATA_BASE + 0x1370;
    constexpr GuestAddr UAV_BIND = DATA_BASE + 0x1380;
    ASSERT_TRUE(memory.write_bytes(CS_BYTECODE, cs_lifetime,
                                   sizeof(cs_lifetime)).is_ok());
    set_arg(0, device);
    set_arg(1, CS_BYTECODE);
    set_arg(2, sizeof(cs_lifetime));
    set_arg(3, 0);
    set_arg(4, CS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 18);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr shader = memory.read_u64(CS_OUT).value();
    set_arg(0, context);
    set_arg(1, shader);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 69); // CSSetShader

    // Learn D3D11_BIND_UNORDERED_ACCESS on a Texture3D: 8x8x4 R8_UNORM (61),
    // SHADER_RESOURCE | UNORDERED_ACCESS — the volume a compute shader fills.
    const std::array<u32, 9> tex{8, 8, 4, 1, 61, 0, 0x8 | 0x80, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEX3D_DESC, reinterpret_cast<const Byte*>(tex.data()),
        sizeof(tex)).is_ok());
    set_arg(0, device);
    set_arg(1, TEX3D_DESC);
    set_arg(2, 0);
    set_arg(3, TEX3D_OUT);
    d3d11.impl_DeviceMethod(marshaler, 6);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr volume = memory.read_u64(TEX3D_OUT).value();
    ASSERT_NE(volume, 0u);
    EXPECT_TRUE(backend.state.last_texture_3d_desc.unordered_access);

    // D3D11_UNORDERED_ACCESS_VIEW_DESC + D3D11_TEX3D_UAV:
    // Format, ViewDimension(TEXTURE3D=8), MipSlice, FirstWSlice, WSize.
    const auto create_uav = [&](const std::array<u32, 5>& uav) {
        EXPECT_TRUE(memory.write_bytes(
            UAV_DESC, reinterpret_cast<const Byte*>(uav.data()),
            sizeof(uav)).is_ok());
        EXPECT_TRUE(memory.write_u64(UAV_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, volume);
        set_arg(2, UAV_DESC);
        set_arg(3, UAV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 8);
    };
    create_uav({61, 4, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "TEXTURE2D over a volume";
    create_uav({61, 8, 0, 0, 5});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "window past the depth";
    create_uav({61, 8, 0, 0, 0xFFFFFFFFu});
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << "WSize -1 = every slice";
    const GuestAddr uav = memory.read_u64(UAV_OUT).value();
    ASSERT_NE(uav, 0u);

    ASSERT_TRUE(memory.write_u64(UAV_BIND, uav).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, UAV_BIND);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 68); // CSSetUnorderedAccessViews
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, 1);
    set_arg(3, 1);
    d3d11.impl_ContextMethod(marshaler, 41); // Dispatch
    EXPECT_EQ(backend.state.dispatches, 1u);
    EXPECT_TRUE(backend.state.last_dispatch_has_uav_3d)
        << "the volume itself reaches the backend's UAV slot";
    EXPECT_FALSE(backend.state.last_dispatch_has_uav);

    // A W-slice window binds the whole mip; the shader takes the window
    // from the size table (corrected: it was not bound; Windows
    // d3d11_view_line_probe [vol] "mip0 W{1,1} mark": z counts from
    // FirstWSlice).
    create_uav({61, 8, 0, 1, 2});
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr window = memory.read_u64(UAV_OUT).value();
    ASSERT_TRUE(memory.write_u64(UAV_BIND, window).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, UAV_BIND);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 68);
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, 1);
    set_arg(3, 1);
    d3d11.impl_ContextMethod(marshaler, 41);
    EXPECT_EQ(backend.state.dispatches, 2u);
    EXPECT_TRUE(backend.state.last_dispatch_has_uav_3d);
    constexpr usize kWindowWord =
        graphics::kGuardSizeTableOffset / 4 + graphics::kGuardSizeSrvSlots;
    ASSERT_GT(backend.state.last_dispatch_constants.size(), kWindowWord);
    EXPECT_EQ(backend.state.last_dispatch_constants[kWindowWord],
              (2u << graphics::kVolumeUavWindowSizeShift) | 1u);
}

TEST_F(D3D11ManagerTest, ComputeShaderCreationAndBindingOwnReferences) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr CS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr CS_OUT = DATA_BASE + 0x1200;
    constexpr GuestAddr CS_GET_OUT = DATA_BASE + 0x1208;
    constexpr GuestAddr CLASS_COUNT_OUT = DATA_BASE + 0x1210;
    constexpr GuestAddr TEXTURE_DESC = DATA_BASE + 0x1300;
    constexpr GuestAddr TEXTURE_OUT = DATA_BASE + 0x1340;
    constexpr GuestAddr UAV_DESC = DATA_BASE + 0x1350;
    constexpr GuestAddr UAV_OUT = DATA_BASE + 0x1370;
    constexpr GuestAddr UAV_BIND = DATA_BASE + 0x1380;
    constexpr GuestAddr UAV_OBSERVED = DATA_BASE + 0x1390;
    constexpr GuestAddr SOURCE_TEXTURE_OUT = DATA_BASE + 0x13A0;
    constexpr GuestAddr SOURCE_SRV_OUT = DATA_BASE + 0x13A8;
    constexpr GuestAddr CONFLICT_SRV_OUT = DATA_BASE + 0x13B0;
    constexpr GuestAddr SRV_BIND = DATA_BASE + 0x13B8;
    constexpr GuestAddr SAMPLER_DESC = DATA_BASE + 0x13C0;
    constexpr GuestAddr SAMPLER_OUT = DATA_BASE + 0x1400;
    constexpr GuestAddr SAMPLER_BIND = DATA_BASE + 0x1408;
    constexpr GuestAddr BUFFER_DESC = DATA_BASE + 0x1420;
    constexpr GuestAddr BUFFER_OUT = DATA_BASE + 0x1440;
    constexpr GuestAddr BUFFER_BIND = DATA_BASE + 0x1448;
    constexpr GuestAddr FORMAT_SUPPORT = DATA_BASE + 0x1450;
    ASSERT_TRUE(memory.write_bytes(CS_BYTECODE, cs_lifetime,
                                   sizeof(cs_lifetime)).is_ok());

    set_arg(0, device);
    set_arg(1, CS_BYTECODE);
    set_arg(2, sizeof(cs_lifetime));
    set_arg(3, 0);
    set_arg(4, CS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 18);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr shader = memory.read_u64(CS_OUT).value();
    ASSERT_NE(shader, 0u);

    set_arg(0, context);
    set_arg(1, shader);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 69); // CSSetShader

    // Release answers the application's count; the binding keeps the
    // shader alive (measured, a12 [rc] / a05 "Release-after-bind=0").
    set_arg(0, shader);
    d3d11.impl_ComputeShaderMethod(marshaler, 2); // caller Release
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(
        kept_alive_by_runtime(shader, &D3D11Manager::impl_ComputeShaderMethod));

    ASSERT_TRUE(memory.write_u32(CLASS_COUNT_OUT, 8).is_ok());
    set_arg(0, context);
    set_arg(1, CS_GET_OUT);
    set_arg(2, 0);
    set_arg(3, CLASS_COUNT_OUT);
    d3d11.impl_ContextMethod(marshaler, 107); // CSGetShader
    EXPECT_EQ(memory.read_u64(CS_GET_OUT).value(), shader);
    EXPECT_EQ(memory.read_u32(CLASS_COUNT_OUT).value(), 0u);

    set_arg(0, shader);
    d3d11.impl_ComputeShaderMethod(marshaler, 2); // returned reference
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(
        kept_alive_by_runtime(shader, &D3D11Manager::impl_ComputeShaderMethod));

    const u32 texture_fields[] = {
        4, 4, 1, 1, 28, 1, 0, 0, 0x88, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(texture_fields),
        sizeof(texture_fields)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5); // CreateTexture2D
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    EXPECT_TRUE(backend.state.last_texture_desc.unordered_access);

    ASSERT_TRUE(memory.write_u32(UAV_DESC, 28).is_ok());
    ASSERT_TRUE(memory.write_u32(UAV_DESC + 4, 4).is_ok());
    ASSERT_TRUE(memory.write_u32(UAV_DESC + 8, 0).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, UAV_DESC);
    set_arg(3, UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr uav = memory.read_u64(UAV_OUT).value();
    ASSERT_NE(uav, 0u);
    set_arg(0, uav);
    set_arg(1, UAV_OBSERVED);
    d3d11.impl_UnorderedAccessViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(UAV_OBSERVED).value(), 28u);
    EXPECT_EQ(memory.read_u32(UAV_OBSERVED + 4).value(), 4u);
    EXPECT_EQ(memory.read_u32(UAV_OBSERVED + 8).value(), 0u);

    // Independent R32_FLOAT SRV + sampler + cbuffer bindings use the D3D
    // compute namespaces t0/s0/b0 while u0 remains independently bound.
    const u32 source_texture_fields[] = {
        4, 4, 1, 1, 41, 1, 0, 0, 0x8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(source_texture_fields),
        sizeof(source_texture_fields)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, SOURCE_TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source_texture = memory.read_u64(SOURCE_TEXTURE_OUT).value();
    set_arg(0, device);
    set_arg(1, source_texture);
    set_arg(2, 0);
    set_arg(3, SOURCE_SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source_srv = memory.read_u64(SOURCE_SRV_OUT).value();

    // MIN_MAG_MIP_POINT with MipLODBias 1.5 (0x3FC00000): both reach the
    // compute push block, which the translated shader reads for sample_l /
    // sample_d / sample_c_lz (D3D11.3 22.4.18/20) and point-mip rounding.
    const std::array<u32, 13> sampler_desc{
        0, 3, 3, 3, 0x3FC00000u, 1, 1, 0, 0, 0, 0, 0, 0x7f800000u};
    ASSERT_TRUE(memory.write_bytes(
        SAMPLER_DESC, reinterpret_cast<const Byte*>(sampler_desc.data()),
        sizeof(sampler_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, SAMPLER_DESC);
    set_arg(2, SAMPLER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 23);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr sampler = memory.read_u64(SAMPLER_OUT).value();

    const std::array<u32, 6> buffer_desc{16, 0, 4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        BUFFER_DESC, reinterpret_cast<const Byte*>(buffer_desc.data()),
        sizeof(buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr constant_buffer = memory.read_u64(BUFFER_OUT).value();

    set_arg(0, device);
    set_arg(1, 41); // DXGI_FORMAT_R32_FLOAT
    set_arg(2, FORMAT_SUPPORT);
    d3d11.impl_DeviceMethod(marshaler, 29);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u32(FORMAT_SUPPORT).value() & 0x2000000u, 0u);
    ASSERT_TRUE(memory.write_u32(FORMAT_SUPPORT, 41).is_ok());
    set_arg(0, device);
    set_arg(1, 3); // D3D11_FEATURE_FORMAT_SUPPORT2
    set_arg(2, FORMAT_SUPPORT);
    set_arg(3, 8);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(FORMAT_SUPPORT + 4).value() & 0xc0u, 0xc0u);

    ASSERT_TRUE(memory.write_u64(SRV_BIND, source_srv).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BIND);
    d3d11.impl_ContextMethod(marshaler, 67);
    ASSERT_TRUE(memory.write_u64(SAMPLER_BIND, sampler).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SAMPLER_BIND);
    d3d11.impl_ContextMethod(marshaler, 70);
    ASSERT_TRUE(memory.write_u64(BUFFER_BIND, constant_buffer).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BUFFER_BIND);
    d3d11.impl_ContextMethod(marshaler, 71);

    ASSERT_TRUE(memory.write_u64(UAV_BIND, uav).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, UAV_BIND);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 68); // CSSetUnorderedAccessViews
    set_arg(0, context);
    set_arg(1, 2);
    set_arg(2, 3);
    set_arg(3, 4);
    d3d11.impl_ContextMethod(marshaler, 41); // Dispatch
    EXPECT_EQ(backend.state.compute_pipeline_creations, 1u);
    EXPECT_EQ(backend.state.dispatches, 1u);
    EXPECT_EQ(backend.state.last_dispatch_groups,
              (std::array<unsigned, 3>{2, 3, 4}));
    EXPECT_EQ(backend.state.last_dispatch_threads,
              (std::array<unsigned, 3>{1, 1, 1}));
    EXPECT_TRUE(backend.state.last_compute_pipeline_has_msl);
    EXPECT_TRUE(backend.state.last_compute_entry_is_main0);
    EXPECT_TRUE(backend.state.last_dispatch_has_uav);
    EXPECT_EQ(backend.state.last_dispatch_uav_mip, 0u);
    EXPECT_TRUE(backend.state.last_dispatch_has_srv);
    EXPECT_TRUE(backend.state.last_dispatch_has_sampler);
    EXPECT_TRUE(backend.state.last_dispatch_has_constant_buffer);
    // Push block member 0 is s#'s MipLODBias, member 4 the point-mip mask.
    // This sampler is point-mip with matching min/mag filters, so its bias
    // carries the 1/64 tie nudge (1.5 + 1/64 = 0x3FC20000) and member 6
    // flags it for the explicit-LOD rounding to take back out.
    ASSERT_EQ(backend.state.last_dispatch_constants.size(),
              graphics::kPushBlockBytes / 4u);
    EXPECT_EQ(backend.state.last_dispatch_constants[0], 0x3FC20000u);
    EXPECT_EQ(backend.state.last_dispatch_constants
                  [graphics::kPointMipNudgeMaskOffset / 4u],
              1u);
    EXPECT_EQ(backend.state.last_dispatch_constants[1], 0u);
    EXPECT_EQ(backend.state.last_dispatch_constants
                  [graphics::kPointMipMaskOffset / 4u],
              1u);

    // A second Dispatch of the same shader and variant reuses the pipeline:
    // building one per call was a Metal library and pipeline state each time.
    set_arg(0, context);
    set_arg(1, 5);
    set_arg(2, 1);
    set_arg(3, 1);
    d3d11.impl_ContextMethod(marshaler, 41); // Dispatch
    EXPECT_EQ(backend.state.dispatches, 2u);
    EXPECT_EQ(backend.state.last_dispatch_groups,
              (std::array<unsigned, 3>{5, 1, 1}));
    EXPECT_EQ(backend.state.compute_pipeline_creations, 1u);

    // Learn CSSetShaderResources: an SRV overlapping a bound output is
    // installed as NULL. The same resource is both SRV/UAV-capable here.
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, CONFLICT_SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr conflict_srv = memory.read_u64(CONFLICT_SRV_OUT).value();
    ASSERT_TRUE(memory.write_u64(SRV_BIND, conflict_srv).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BIND);
    d3d11.impl_ContextMethod(marshaler, 67);
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, 1);
    set_arg(3, 1);
    d3d11.impl_ContextMethod(marshaler, 41);
    EXPECT_FALSE(backend.state.last_dispatch_has_srv);

    // The inverse order also resolves the hazard: binding a UAV unbinds an
    // overlapping SRV from every shader stage before Dispatch.
    ASSERT_TRUE(memory.write_u64(UAV_BIND, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, UAV_BIND);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 68);
    ASSERT_TRUE(memory.write_u64(SRV_BIND, conflict_srv).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, SRV_BIND);
    d3d11.impl_ContextMethod(marshaler, 67);
    ASSERT_TRUE(memory.write_u64(UAV_BIND, uav).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, UAV_BIND);
    set_arg(4, 0);
    d3d11.impl_ContextMethod(marshaler, 68);
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, 1);
    set_arg(3, 1);
    d3d11.impl_ContextMethod(marshaler, 41);
    EXPECT_TRUE(backend.state.last_dispatch_has_uav);
    EXPECT_FALSE(backend.state.last_dispatch_has_srv);

    set_arg(0, conflict_srv);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    set_arg(0, uav);
    d3d11.impl_UnorderedAccessViewMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(kept_alive_by_runtime(
        uav, &D3D11Manager::impl_UnorderedAccessViewMethod));
    set_arg(0, texture);
    d3d11.impl_Texture2DMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(
        kept_alive_by_runtime(texture, &D3D11Manager::impl_Texture2DMethod));

    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 110); // ClearState
    set_arg(0, shader);
    d3d11.impl_ComputeShaderMethod(marshaler, 1);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    set_arg(0, uav);
    d3d11.impl_UnorderedAccessViewMethod(marshaler, 1);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    set_arg(0, device);
    set_arg(1, CS_BYTECODE);
    set_arg(2, sizeof(cs_lifetime));
    set_arg(3, 0);
    set_arg(4, 0);
    d3d11.impl_DeviceMethod(marshaler, 18);
    EXPECT_EQ(cpu.gpr.rax, 1u); // S_FALSE validation-only success.
}

TEST_F(D3D11ManagerTest, BlendFactorsMapAndOmSetStoresDynamicBlendColor) {
    EXPECT_EQ(d3d11_backend_blend_factor(16), graphics::BackendBlendFactor::Source1Color);
    EXPECT_EQ(d3d11_backend_blend_factor(17), graphics::BackendBlendFactor::OneMinusSource1Color);
    EXPECT_EQ(d3d11_backend_blend_factor(18), graphics::BackendBlendFactor::Source1Alpha);
    EXPECT_EQ(d3d11_backend_blend_factor(19), graphics::BackendBlendFactor::OneMinusSource1Alpha);
    EXPECT_EQ(d3d11_backend_blend_factor(11),
              graphics::BackendBlendFactor::SourceAlphaSaturated);
    EXPECT_EQ(d3d11_backend_blend_factor(14),
              graphics::BackendBlendFactor::BlendColor);
    EXPECT_EQ(d3d11_backend_blend_factor(15),
              graphics::BackendBlendFactor::OneMinusBlendColor);

    create_default_device();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr FACTOR_IN = DATA_BASE + 0x900;
    constexpr GuestAddr FACTOR_OUT = DATA_BASE + 0x920;
    constexpr GuestAddr MASK_OUT = DATA_BASE + 0x940;
    const std::array<float, 4> factor{0.125f, 0.25f, 0.5f, 0.75f};
    ASSERT_TRUE(memory.write_bytes(
        FACTOR_IN, reinterpret_cast<const Byte*>(factor.data()),
        sizeof(factor)).is_ok());

    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, FACTOR_IN);
    set_arg(3, 0xA5A55A5Au);
    d3d11.impl_ContextMethod(marshaler, 35); // OMSetBlendState
    set_arg(1, 0);
    set_arg(2, FACTOR_OUT);
    set_arg(3, MASK_OUT);
    d3d11.impl_ContextMethod(marshaler, 91); // OMGetBlendState

    std::array<float, 4> observed{};
    ASSERT_TRUE(memory.read_bytes(
        FACTOR_OUT, reinterpret_cast<Byte*>(observed.data()),
        sizeof(observed)).is_ok());
    EXPECT_EQ(observed, factor);
    EXPECT_EQ(memory.read_u32(MASK_OUT).value(), 0xA5A55A5Au);

    // Learn: a null BlendFactor stores {1,1,1,1}; it does not retain the
    // previously supplied constant.
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, 0xFFFFFFFFu);
    d3d11.impl_ContextMethod(marshaler, 35);
    set_arg(1, 0);
    set_arg(2, FACTOR_OUT);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 91);
    ASSERT_TRUE(memory.read_bytes(
        FACTOR_OUT, reinterpret_cast<Byte*>(observed.data()),
        sizeof(observed)).is_ok());
    EXPECT_EQ(observed,
              (std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}));

    // SRC1_* is a supported blend state; shader output routing is selected at draw.
    constexpr GuestAddr BLEND_DESC = DATA_BASE + 0xA00;
    constexpr GuestAddr BLEND_OUT = DATA_BASE + 0xB20;
    for (u32 i = 0; i < 66; ++i) {
        ASSERT_TRUE(memory.write_u32(BLEND_DESC + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 8, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 12, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 16, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 20, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 24, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 28, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 32, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 36, 0xF).is_ok());
    ASSERT_TRUE(memory.write_u64(BLEND_OUT, 0xDEADBEEFu).is_ok());
    set_arg(0, memory.read_u64(DEVICE_OUT).value());
    set_arg(1, BLEND_DESC);
    set_arg(2, BLEND_OUT);
    d3d11.impl_DeviceMethod(marshaler, 20);
    EXPECT_EQ(cpu.gpr.rax & 0xFFFFFFFFu, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(BLEND_OUT).value(), 0u);

    ASSERT_TRUE(memory.write_u32(BLEND_DESC + 12, 14).is_ok());
    set_arg(0, memory.read_u64(DEVICE_OUT).value());
    set_arg(1, BLEND_DESC);
    set_arg(2, BLEND_OUT);
    d3d11.impl_DeviceMethod(marshaler, 20);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr blend_state = memory.read_u64(BLEND_OUT).value();
    ASSERT_NE(blend_state, 0u);
    set_arg(0, context);
    set_arg(1, blend_state);
    set_arg(2, FACTOR_IN);
    set_arg(3, 0xFFFFFFFFu);
    d3d11.impl_ContextMethod(marshaler, 35);
    set_arg(0, blend_state);
    d3d11.impl_BlendStateMethod(marshaler, 2); // caller Release
    // The application's count (measured, a12 [rc] "bound blend ... last
    // Release=0"); the binding keeps the state alive.
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(kept_alive_by_runtime(blend_state,
                                      &D3D11Manager::impl_BlendStateMethod))
        << "context must retain the bound state";
    constexpr GuestAddr STATE_OBSERVED = DATA_BASE + 0xB30;
    set_arg(0, context);
    set_arg(1, STATE_OBSERVED);
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 91);
    EXPECT_EQ(memory.read_u64(STATE_OBSERVED).value(), blend_state);
    set_arg(0, blend_state);
    d3d11.impl_BlendStateMethod(marshaler, 2); // release OMGet reference
    set_arg(0, context);
    d3d11.impl_ContextMethod(marshaler, 110); // releases bound reference
}

// Learn (Typed unordered access view (UAV) loads): R32_FLOAT always supports a
// typed load, and a further group — R32G32B32A32, R16G16B16A16, R8G8B8A8_UNORM,
// R16_FLOAT, R8_UNORM and their integer siblings — is all-or-nothing, reported
// through D3D11_FEATURE_DATA_D3D11_OPTIONS2::TypedUAVLoadAdditionalFormats.
// Vitreo binds every UAV texture as Metal access::read_write, which is the set
// MTLReadWriteTextureTier2 enumerates, so it reports that group in both
// directions.
//
// This reporting used to be a two-format allowlist (R32_FLOAT, R8G8B8A8_UNORM),
// which told a guest the device could not random-write an R16_FLOAT or
// R16G16B16A16_FLOAT surface even though CreateUnorderedAccessView accepts any
// format the backend can express. An engine that asks before it allocates
// believes that answer: a Unity title failed RenderTexture.Create for every
// enableRandomWrite target, every frame, and rendered nothing.
// D3D11_RESOURCE_MISC_FLAG's Remarks claim the misc flags "cannot be used when
// creating resources with D3D11_CPU_ACCESS flags". Taken literally that
// refused UE5's dynamic byte-address buffer and killed start-up.
//
// The blanket Remark is wrong, and the same page contradicts it: HW_PROTECTED
// separately lists "No CPU access flags can be specified" among ITS own
// restrictions, which is redundant if the rule covered every flag.
//
// Measured on real Windows at feature level 11.0 --
// tools/oracle_probe/d3d11_buffer_misc_probe.c, output recorded in its
// .expected. The buffer-SHAPE flags all succeed alongside CPU_ACCESS_WRITE,
// and a SHARING flag is the one that genuinely conflicts.
TEST_F(D3D11ManagerTest, BufferShapeMiscFlagsCoexistWithCpuWrite) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // {ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags, Stride}
    struct Case {
        const char* name;
        std::array<u32, 6> desc;
        bool expect_ok;
    };
    for (const Case& c : {
             // The exact descriptor UE5 creates.
             Case{"DYNAMIC|WRITE|ALLOW_RAW_VIEWS SRV|VERTEX_BUFFER",
                  {1024, 2, 0x9, 0x10000, 0x20, 0}, true},
             Case{"DYNAMIC|WRITE|BUFFER_STRUCTURED SRV",
                  {1024, 2, 0x8, 0x10000, 0x40, 16}, true},
             Case{"DYNAMIC|WRITE|DRAWINDIRECT_ARGS VERTEX_BUFFER",
                  {1024, 2, 0x1, 0x10000, 0x10, 0}, true},
             // A sharing flag with CPU access is what Windows actually refuses.
             Case{"DYNAMIC|WRITE|SHARED SRV",
                  {1024, 2, 0x8, 0x10000, 0x2, 0}, false},
         }) {
        SCOPED_TRACE(c.name);
        ASSERT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(c.desc.data()),
            sizeof(c.desc)).is_ok());
        ASSERT_TRUE(memory.write_u64(GPU_BUFFER_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        if (c.expect_ok) {
            EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
            EXPECT_NE(memory.read_u64(GPU_BUFFER_OUT).value(), 0u);
        } else {
            EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
        }
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
