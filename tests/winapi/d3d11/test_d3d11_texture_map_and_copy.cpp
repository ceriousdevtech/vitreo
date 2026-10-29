// SPDX-License-Identifier: LGPL-2.1-only
//
// Staging/dynamic texture maps, boxed copies, SRV recycling and depth-stencil state.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "../../oracle_data/d3d11_oracle/d3d11_shaders.h"

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, StagingBc3TextureUsesCompressedBlockPitch) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // D3D11_USAGE_STAGING, BC3_UNORM, CPU write. BC3 stores one 16-byte block
    // per 4x4 texels, so a 256x256 subresource is 1024 bytes by 64 block rows.
    const std::array<u32, 11> staging_desc{
        256, 256, 1, 1, 77, 1, 0, 3, 0, 0x10000, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(staging_desc.data()),
        sizeof(staging_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr staging = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(staging, 0u);
    EXPECT_EQ(backend.state.texture_creations, 0u);

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 2); // D3D11_MAP_WRITE
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), 1024u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), 65536u);
}

TEST_F(D3D11ManagerTest, MapsAndUploadsDynamicTextureWithWriteDiscard) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // 4x4 BGRA8, DYNAMIC, shader-resource bind, CPU write access.
    const std::array<u32, 11> desc{
        4, 4, 1, 1, 87, 1, 0, 2, 8, 0x10000, 0,
    };
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
    ASSERT_NE(texture, 0u);
    EXPECT_TRUE(backend.state.last_texture_desc.cpu_writable);

    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, 4); // D3D11_MAP_WRITE_DISCARD
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    ASSERT_NE(mapped, 0u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), 16u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), 64u);

    std::array<u8, 64> pixels{};
    for (usize index = 0; index < pixels.size(); ++index) {
        pixels[index] = static_cast<u8>(index ^ 0x5Au);
    }
    ASSERT_TRUE(memory.write_bytes(
        mapped, reinterpret_cast<const Byte*>(pixels.data()),
        pixels.size()).is_ok());
    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    EXPECT_EQ(backend.state.texture_updates, 1u);
    EXPECT_EQ(backend.state.last_texture_update_row_pitch, 16u);
    EXPECT_EQ(backend.state.last_texture_update,
              std::vector<u8>(pixels.begin(), pixels.end()));

    // WRITE_NO_OVERWRITE is not valid for dynamic textures.
    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, 5);
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
}

TEST_F(D3D11ManagerTest, CopiesBoxedGpuTextureRegionIntoStagingTexture) {
    // CopySubresourceRegion texture coordinates are texels and D3D11_BOX's
    // right/bottom edges are exclusive. Exercise the GPU-readback path used
    // to copy one rendered pixel into a CPU-readable staging texture.
    // Metal must commit pending draws before that readback; otherwise ANGLE
    // / Skia GPU probes observe an empty RT and skip compositing the UI.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> source_desc{
        4, 3, 1, 1, 87, 1, 0, 0, 0x28, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(source_desc.data()),
        sizeof(source_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source = memory.read_u64(TEXTURE_OUT).value();

    const std::array<u32, 11> staging_desc{
        1, 1, 1, 1, 87, 1, 0, 3, 0, 0x20000, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(staging_desc.data()),
        sizeof(staging_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr staging = memory.read_u64(VIEW_OUT).value();

    backend.state.native_texture_readback.resize(4 * 3 * 4);
    for (usize index = 0;
         index < backend.state.native_texture_readback.size(); ++index) {
        backend.state.native_texture_readback[index] =
            static_cast<u8>(index);
    }
    constexpr GuestAddr box_address = DATA_BASE + 0x700;
    const std::array<u32, 6> box{
        2, 1, 0, 3, 2, 1,
    };
    ASSERT_TRUE(memory.write_bytes(
        box_address, reinterpret_cast<const Byte*>(box.data()),
        sizeof(box)).is_ok());

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 0);
    set_arg(5, 0);
    set_arg(6, source);
    set_arg(7, 0);
    set_arg(8, box_address);
    d3d11.impl_ContextMethod(marshaler, 46);
    ASSERT_EQ(backend.state.texture_native_readbacks, 1u);

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 1); // D3D11_MAP_READ
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    std::array<u8, 4> pixel{};
    ASSERT_TRUE(memory.read_bytes(
        mapped, reinterpret_cast<Byte*>(pixel.data()), pixel.size()).is_ok());
    EXPECT_EQ(pixel, (std::array<u8, 4>{24, 25, 26, 27}));
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);
}

// D3D11.3 19.5.1: a BC mip smaller than a block has a physical size rounded up
// to 4, and regions copied from it must be whole blocks, so (0,0)-(4,4) is
// the box for its 2x2 and 1x1 mips (Windows copies them:
// tools/oracle_probe/d3d11_bc_mip_tail_copy_probe.expected). The GPU copy is
// of the texels that exist; a box past the physical size is still refused.
// Learn OMSetDepthStencilState: NULL binds the default depth-stencil state
// (DepthEnable TRUE, DepthWriteMask ALL, DepthFunc LESS, StencilEnable FALSE).
// D3D11.3 17.9: "When DepthEnable is false, depth test and depth buffer write
// are not performed, regardless of any other settings."
// Learn D3D11_SHADER_RESOURCE_VIEW_DESC.Format: the view reads the resource
// as that format. An sRGB view of a TYPELESS texture (how UE creates colour
// textures) must reach the backend as an sRGB view, or every sample returns
// raw UNORM values -- the washed-out world measured with
// tools/oracle_probe/d3d11_table_texture_views_probe.c.
TEST_F(D3D11ManagerTest, ShaderResourceViewFormatReachesTheBackendView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr TEX_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr VIEW_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr RT_OUT = DATA_BASE + 0x910;
    constexpr GuestAddr RTV_OUT = DATA_BASE + 0x918;
    constexpr GuestAddr VS_OUT = DATA_BASE + 0x920;
    constexpr GuestAddr PS_OUT = DATA_BASE + 0x928;
    constexpr GuestAddr VS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr PS_BYTECODE = DATA_BASE + 0x1800;
    constexpr GuestAddr BINDINGS = DATA_BASE + 0x1C00;
    const auto create_texture = [&](u32 format, u32 bind, GuestAddr out) {
        const std::array<u32, 11> desc{16, 16, 1, 1, format, 1, 0, 0, bind, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 5);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    // R8G8B8A8_TYPELESS (27) viewed as R8G8B8A8_UNORM_SRGB (29).
    const GuestAddr texture = create_texture(27, 0x8, TEX_OUT);
    const std::array<u32, 4> view_desc{29, 4, 0, 1};  // TEXTURE2D, mip 0, 1
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(view_desc.data()),
        sizeof(view_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    const GuestAddr rt = create_texture(28, 0x20, RT_OUT);
    set_arg(0, device);
    set_arg(1, rt);
    set_arg(2, 0);
    set_arg(3, RTV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_bytes(VS_BYTECODE, vs_main, sizeof(vs_main)).is_ok());
    set_arg(0, device);
    set_arg(1, VS_BYTECODE);
    set_arg(2, sizeof(vs_main));
    set_arg(3, 0);
    set_arg(4, VS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 12);
    ASSERT_TRUE(memory.write_bytes(PS_BYTECODE, ps_color, sizeof(ps_color)).is_ok());
    set_arg(0, device);
    set_arg(1, PS_BYTECODE);
    set_arg(2, sizeof(ps_color));
    set_arg(3, 0);
    set_arg(4, PS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 15);
    set_arg(0, context);
    set_arg(1, memory.read_u64(VS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 11);
    set_arg(0, context);
    set_arg(1, memory.read_u64(PS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 9);
    ASSERT_TRUE(memory.write_u64(BINDINGS, view).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 8);  // PSSetShaderResources
    ASSERT_TRUE(memory.write_u64(BINDINGS, memory.read_u64(RTV_OUT).value()).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, BINDINGS);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 33);
    set_arg(0, context);
    set_arg(1, 4);
    d3d11.impl_ContextMethod(marshaler, 24);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_EQ(backend.state.last_texture_views[0].pixel_format,
              graphics::PixelFormat::RGBA8Unorm_sRGB);
}

// The draw path memoizes SRV lookups by guest address. Releasing a view
// erases it and frees its address for the next view, so the memo must forget
// that address: a recycled view has to reach the backend as ITSELF, not as the
// erased one it replaced (whose node is gone).
TEST_F(D3D11ManagerTest, RecycledShaderResourceViewAddressBindsTheNewView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr TEX_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr VIEW_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr RT_OUT = DATA_BASE + 0x910;
    constexpr GuestAddr RTV_OUT = DATA_BASE + 0x918;
    constexpr GuestAddr VS_OUT = DATA_BASE + 0x920;
    constexpr GuestAddr PS_OUT = DATA_BASE + 0x928;
    constexpr GuestAddr VS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr PS_BYTECODE = DATA_BASE + 0x1800;
    constexpr GuestAddr BINDINGS = DATA_BASE + 0x1C00;
    const auto create_texture = [&](u32 format, u32 bind, GuestAddr out) {
        const std::array<u32, 11> desc{16, 16, 1, 1, format, 1, 0, 0, bind, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 5);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    // R8G8B8A8_TYPELESS (27) viewed first as R8G8B8A8_UNORM (28), which
    // needs no reinterpreting backend view, then as UNORM_SRGB (29).
    const GuestAddr texture = create_texture(27, 0x8, TEX_OUT);
    const std::array<u32, 4> view_desc{28, 4, 0, 1};  // TEXTURE2D, mip 0, 1
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(view_desc.data()),
        sizeof(view_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    const GuestAddr rt = create_texture(28, 0x20, RT_OUT);
    set_arg(0, device);
    set_arg(1, rt);
    set_arg(2, 0);
    set_arg(3, RTV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_bytes(VS_BYTECODE, vs_main, sizeof(vs_main)).is_ok());
    set_arg(0, device);
    set_arg(1, VS_BYTECODE);
    set_arg(2, sizeof(vs_main));
    set_arg(3, 0);
    set_arg(4, VS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 12);
    ASSERT_TRUE(memory.write_bytes(PS_BYTECODE, ps_color, sizeof(ps_color)).is_ok());
    set_arg(0, device);
    set_arg(1, PS_BYTECODE);
    set_arg(2, sizeof(ps_color));
    set_arg(3, 0);
    set_arg(4, PS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 15);
    set_arg(0, context);
    set_arg(1, memory.read_u64(VS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 11);
    set_arg(0, context);
    set_arg(1, memory.read_u64(PS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 9);
    ASSERT_TRUE(memory.write_u64(BINDINGS, view).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 8);  // PSSetShaderResources
    ASSERT_TRUE(memory.write_u64(BINDINGS, memory.read_u64(RTV_OUT).value()).is_ok());
    set_arg(0, context);
    set_arg(1, 1);
    set_arg(2, BINDINGS);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 33);
    set_arg(0, context);
    set_arg(1, 4);
    d3d11.impl_ContextMethod(marshaler, 24);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    ASSERT_EQ(backend.state.draws, 1u);
    ASSERT_NE(backend.state.last_texture_views[0].pixel_format,
              graphics::PixelFormat::RGBA8Unorm_sRGB);

    // Unbind and release the view, then view the same texture as sRGB.
    ASSERT_TRUE(memory.write_u64(BINDINGS, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 8);  // PSSetShaderResources(NULL)
    set_arg(0, view);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 2);  // Release
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const std::array<u32, 4> srgb_desc{29, 4, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srgb_desc.data()),
        sizeof(srgb_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr recycled = memory.read_u64(VIEW_OUT).value();
    ASSERT_EQ(recycled, view) << "object storage no longer recycles the "
                                 "freed address; this test needs another way "
                                 "to land a view on it";
    ASSERT_TRUE(memory.write_u64(BINDINGS, recycled).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDINGS);
    d3d11.impl_ContextMethod(marshaler, 8);
    set_arg(0, context);
    set_arg(1, 3);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 13);
    ASSERT_EQ(backend.state.draws, 2u);
    EXPECT_EQ(backend.state.last_texture_views[0].pixel_format,
              graphics::PixelFormat::RGBA8Unorm_sRGB);
}


TEST_F(D3D11ManagerTest, NullDepthStencilStateIsTheDefaultAndDepthEnableGatesWrites) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr DEPTH_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr DSV_OUT = DATA_BASE + 0x908;
    constexpr GuestAddr VS_OUT = DATA_BASE + 0x910;
    constexpr GuestAddr PS_OUT = DATA_BASE + 0x918;
    constexpr GuestAddr DSS_OUT = DATA_BASE + 0x920;
    constexpr GuestAddr DSS_DESC = DATA_BASE + 0x940;
    constexpr GuestAddr VS_BYTECODE = DATA_BASE + 0x1000;
    constexpr GuestAddr PS_BYTECODE = DATA_BASE + 0x1800;

    // 16x16 D24_UNORM_S8_UINT (45), BIND_DEPTH_STENCIL.
    const std::array<u32, 11> depth_desc{16, 16, 1, 1, 45, 1, 0, 0, 0x40, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(depth_desc.data()),
        sizeof(depth_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, DEPTH_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, device);
    set_arg(1, memory.read_u64(DEPTH_OUT).value());
    set_arg(2, 0);
    set_arg(3, DSV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 10);  // CreateDepthStencilView
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr dsv = memory.read_u64(DSV_OUT).value();

    ASSERT_TRUE(memory.write_bytes(
        VS_BYTECODE, vs_main, sizeof(vs_main)).is_ok());
    set_arg(0, device);
    set_arg(1, VS_BYTECODE);
    set_arg(2, sizeof(vs_main));
    set_arg(3, 0);
    set_arg(4, VS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 12);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_bytes(
        PS_BYTECODE, ps_color, sizeof(ps_color)).is_ok());
    set_arg(0, device);
    set_arg(1, PS_BYTECODE);
    set_arg(2, sizeof(ps_color));
    set_arg(3, 0);
    set_arg(4, PS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 15);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(VS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 11);
    set_arg(0, context);
    set_arg(1, memory.read_u64(PS_OUT).value());
    set_arg(2, 0);
    set_arg(3, 0);
    d3d11.impl_ContextMethod(marshaler, 9);
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 0);
    set_arg(3, dsv);
    d3d11.impl_ContextMethod(marshaler, 33);  // OMSetRenderTargets
    set_arg(0, context);
    set_arg(1, 4);
    d3d11.impl_ContextMethod(marshaler, 24);
    const auto draw = [&] {
        set_arg(0, context);
        set_arg(1, 3);
        set_arg(2, 0);
        d3d11.impl_ContextMethod(marshaler, 13);
    };

    draw();  // no depth-stencil state bound
    ASSERT_EQ(backend.state.draws, 1u);
    EXPECT_TRUE(backend.state.last_draw_depth_test_enabled);
    EXPECT_TRUE(backend.state.last_draw_depth_write_enabled);
    EXPECT_EQ(backend.state.last_draw_depth_compare,
              graphics::BackendCompare::Less);
    EXPECT_FALSE(backend.state.last_draw_stencil_enabled);

    // DepthEnable FALSE, DepthWriteMask ALL, DepthFunc LESS, stencil off.
    const std::array<u32, 13> dss{0, 1, 2, 0, 0x0000FFFFu,
                                  1, 1, 1, 8, 1, 1, 1, 8};
    ASSERT_TRUE(memory.write_bytes(
        DSS_DESC, reinterpret_cast<const Byte*>(dss.data()),
        sizeof(dss)).is_ok());
    set_arg(0, device);
    set_arg(1, DSS_DESC);
    set_arg(2, DSS_OUT);
    d3d11.impl_DeviceMethod(marshaler, 21);  // CreateDepthStencilState
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, context);
    set_arg(1, memory.read_u64(DSS_OUT).value());
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 36);  // OMSetDepthStencilState
    draw();
    ASSERT_EQ(backend.state.draws, 2u);
    EXPECT_FALSE(backend.state.last_draw_depth_test_enabled);
    EXPECT_FALSE(backend.state.last_draw_depth_write_enabled)
        << "DepthEnable FALSE writes no depth whatever the write mask says";
}

TEST_F(D3D11ManagerTest, CopiesBlockCompressedMipTailsWithWholeBlockBoxes) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr BOX = DATA_BASE + 0x900;

    // 16x16 BC1_UNORM (71), five mips: 16, 8, 4, 2, 1.
    const std::array<u32, 11> desc{16, 16, 5, 1, 71, 1, 0, 0, 0x8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    const auto create = [&](GuestAddr out) {
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 5);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    const GuestAddr source = create(TEXTURE_OUT);
    const GuestAddr destination = create(VIEW_OUT);
    const auto copy_mip = [&](u32 mip, u32 box_extent) {
        const std::array<u32, 6> box{0, 0, 0, box_extent, box_extent, 1};
        ASSERT_TRUE(memory.write_bytes(
            BOX, reinterpret_cast<const Byte*>(box.data()),
            sizeof(box)).is_ok());
        set_arg(0, context);
        set_arg(1, destination);
        set_arg(2, mip);
        set_arg(3, 0);
        set_arg(4, 0);
        set_arg(5, 0);
        set_arg(6, source);
        set_arg(7, mip);
        set_arg(8, BOX);
        d3d11.impl_ContextMethod(marshaler, 46);  // CopySubresourceRegion
    };

    copy_mip(3, 4);  // 2x2 mip, one whole block
    ASSERT_EQ(backend.state.texture_region_copies, 1u);
    EXPECT_EQ(backend.state.last_copy_src_mip, 3u);
    EXPECT_EQ(backend.state.last_copy_width, 2u);
    EXPECT_EQ(backend.state.last_copy_height, 2u);

    copy_mip(4, 4);  // 1x1 mip
    ASSERT_EQ(backend.state.texture_region_copies, 2u);
    EXPECT_EQ(backend.state.last_copy_width, 1u);
    EXPECT_EQ(backend.state.last_copy_height, 1u);

    copy_mip(3, 8);  // past the physical 4x4: refused
    EXPECT_EQ(backend.state.texture_region_copies, 2u);
}

TEST_F(D3D11ManagerTest, CopiesBetweenNonzeroTextureArraySubresources) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // 4x4, two mips, three slices. D3D11CalcSubresource(mip, slice, 2):
    // source mip 1/slice 2 = 5; destination mip 1/slice 1 = 3.
    const std::array<u32, 11> desc{
        4, 4, 2, 3, 87, 1, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source = memory.read_u64(TEXTURE_OUT).value();

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr destination = memory.read_u64(VIEW_OUT).value();

    set_arg(0, context);
    set_arg(1, destination);
    set_arg(2, 3);
    set_arg(3, 0);
    set_arg(4, 0);
    set_arg(5, 0);
    set_arg(6, source);
    set_arg(7, 5);
    set_arg(8, 0);
    d3d11.impl_ContextMethod(marshaler, 46);

    EXPECT_EQ(backend.state.texture_region_copies, 1u);
    EXPECT_EQ(backend.state.last_copy_dst_mip, 1u);
    EXPECT_EQ(backend.state.last_copy_dst_slice, 1u);
    EXPECT_EQ(backend.state.last_copy_src_mip, 1u);
    EXPECT_EQ(backend.state.last_copy_src_slice, 2u);
    EXPECT_EQ(backend.state.last_copy_width, 2u);
    EXPECT_EQ(backend.state.last_copy_height, 2u);
}

TEST_F(D3D11ManagerTest, CopyResourceCopiesEveryMipAndArraySlice) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> desc{
        4, 4, 2, 3, 87, 1, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source = memory.read_u64(TEXTURE_OUT).value();

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr destination = memory.read_u64(VIEW_OUT).value();

    set_arg(0, context);
    set_arg(1, destination);
    set_arg(2, source);
    d3d11.impl_ContextMethod(marshaler, 47);

    const std::vector<std::array<unsigned, 4>> expected{
        {0, 0, 0, 0}, {1, 0, 1, 0},
        {0, 1, 0, 1}, {1, 1, 1, 1},
        {0, 2, 0, 2}, {1, 2, 1, 2},
    };
    EXPECT_EQ(backend.state.texture_region_copies, 6u);
    EXPECT_EQ(backend.state.copied_subresources, expected);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
