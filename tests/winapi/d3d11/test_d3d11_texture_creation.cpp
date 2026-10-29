// SPDX-License-Identifier: LGPL-2.1-only
//
// Shader-resource view dimensions, texture creation/copies, block-compressed and volume textures.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// Learn (D3D11_SHADER_RESOURCE_VIEW_DESC): ViewDimension selects the union
// member; an array view dimension does not require the resource to hold more
// than one slice. A one-slice Texture2D is viewable as TEXTURE2DARRAY with
// ArraySize == 1.
TEST_F(D3D11ManagerTest, ShaderResourceViewAcceptsSingleSliceArrayDimension) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> texture_desc{
        4, 4, 1, 1, 28, 1, 0, 0, 0x8, 0, 0,
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

    // Format, TEXTURE2DARRAY, MostDetailedMip, MipLevels, FirstArraySlice,
    // ArraySize.
    const std::array<u32, 6> srv_desc{28, 5, 0, 1, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
        sizeof(srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    constexpr GuestAddr DESC_OUT = SRV_DESC + 0x40;
    set_arg(0, view);
    set_arg(1, DESC_OUT);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(DESC_OUT).value(), 28u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 4).value(), 5u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 12).value(), 1u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 16).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 20).value(), 1u);
}

// D3D11_TEXCUBE_ARRAY_SRV counts whole cubes: a six-face TEXTURECUBE resource
// is viewable as TEXTURECUBEARRAY with NumCubes == 1.
TEST_F(D3D11ManagerTest, ShaderResourceViewAcceptsSingleCubeArrayDimension) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> texture_desc{
        4, 4, 1, 6, 28, 1, 0, 0, 0x8, 0, 0x4,
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

    // Format, TEXTURECUBEARRAY, MostDetailedMip, MipLevels, First2DArrayFace,
    // NumCubes.
    const std::array<u32, 6> cube_array_desc{28, 10, 0, 1, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(cube_array_desc.data()),
        sizeof(cube_array_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    constexpr GuestAddr DESC_OUT = SRV_DESC + 0x40;
    set_arg(0, view);
    set_arg(1, DESC_OUT);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 4).value(), 10u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 16).value(), 0u);
    EXPECT_EQ(memory.read_u32(DESC_OUT + 20).value(), 1u);

    // A plain TEXTURECUBE view of the same resource still works and covers all
    // six faces.
    const std::array<u32, 4> cube_desc{28, 9, 0, 1};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(cube_desc.data()),
        sizeof(cube_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
}

// A cube view still requires D3D11_RESOURCE_MISC_TEXTURECUBE on the resource;
// a multisample view dimension does not require a multisampled resource.
TEST_F(D3D11ManagerTest, ShaderResourceViewRejectsIncompatibleDimensions) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> texture_desc{
        4, 4, 1, 6, 28, 1, 0, 0, 0x8, 0, 0,
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

    for (const u32 dimension : {9u, 10u, 8u, 2u}) {
        const std::array<u32, 6> srv_desc{28, dimension, 0, 1, 0, 1};
        ASSERT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
            sizeof(srv_desc)).is_ok());
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, SRV_DESC);
        set_arg(3, VIEW_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "dimension " << dimension;
    }
    // A multisample dimension over this one-sample texture is legal on
    // Windows (d3d11_a03_views_probe "srv tex2d as 2DMS / 2DMSARRAY"): it
    // binds as the 2D mip and a shader variant reads it (corrected: this
    // expected E_NOTIMPL while no variant existed).
    for (const u32 dimension : {6u, 7u}) {
        const std::array<u32, 6> srv_desc{28, dimension, 0, 1, 0, 1};
        ASSERT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
            sizeof(srv_desc)).is_ok());
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, SRV_DESC);
        set_arg(3, VIEW_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "dimension " << dimension;
    }
}

// Learn (ID3D11DeviceContext::CopySubresourceRegion): source and destination
// must be the same resource type with compatible formats and must not be
// mapped; every usage combination is otherwise legal, so a staging source may
// be copied into a DEFAULT destination (this is the standard Unity texture
// upload path). CopyResource already does this; the region form must too.
TEST_F(D3D11ManagerTest, CopiesStagingTextureRegionIntoDefaultTexture) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> staging_desc{
        1, 1, 1, 1, 28, 1, 0, 3, 0, 0x30000, 0,
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

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 2); // D3D11_MAP_WRITE
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    ASSERT_TRUE(memory.write_u32(mapped, 0x11223344u).is_ok());
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    const std::array<u32, 11> default_desc{
        1, 1, 1, 1, 28, 1, 0, 0, 0x8, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(default_desc.data()),
        sizeof(default_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr gpu_texture = memory.read_u64(VIEW_OUT).value();

    set_arg(0, context);
    set_arg(1, gpu_texture);
    set_arg(2, 0);
    set_arg(3, 0);
    set_arg(4, 0);
    set_arg(5, 0);
    set_arg(6, staging);
    set_arg(7, 0);
    set_arg(8, 0);
    d3d11.impl_ContextMethod(marshaler, 46);
    EXPECT_EQ(backend.state.texture_updates, 1u);
    EXPECT_EQ(backend.state.last_texture_update,
              std::vector<u8>({0x44u, 0x33u, 0x22u, 0x11u}));
}

TEST_F(D3D11ManagerTest, CreatesAndClearsMetalBackedRenderTarget) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 11> desc{
        64, 32, 1, 1, 28, 1, 0, 0, 0x20, 0, 0,
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
    EXPECT_EQ(backend.state.texture_creations, 1u);

    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    const std::array<float, 4> color{0.25f, 0.5f, 0.75f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        COLOR, reinterpret_cast<const Byte*>(color.data()),
        sizeof(color)).is_ok());
    set_arg(0, context);
    set_arg(1, view);
    set_arg(2, COLOR);
    d3d11.impl_ContextMethod(marshaler, 50);
    EXPECT_EQ(backend.state.texture_clears, 1u);
    // D3D11.3 3.2.3.6: an R8G8B8A8_UNORM view stores round-to-nearest-even
    // codes (0.25 -> 64, 0.5 -> 128, 0.75 -> 191; a06 probe RTV lines), and
    // the backend is handed exactly those codes' values. Updated from the
    // old pass-through expectation, which the oracle's tie cases disprove.
    EXPECT_EQ(backend.state.last_clear,
              (std::array<float, 4>{64.0f / 255.0f, 128.0f / 255.0f,
                                    191.0f / 255.0f, 1.0f}));

    set_arg(0, context);
    // Flush is IDL slot 111 (ClearState is 110).
    d3d11.impl_ContextMethod(marshaler, 111);
    EXPECT_EQ(backend.state.flushes, 1u);
}

// Learn D3D11_BIND_FLAG: bind flags combine freely with a bitwise OR except
// D3D11_BIND_CONSTANT_BUFFER, so VERTEX_BUFFER|SHADER_RESOURCE|
// UNORDERED_ACCESS (0x89) is a legal DEFAULT-usage buffer and must reach the
// backend with unordered_access set. Unordered access is a GPU write, so the
// flag is only valid on DEFAULT usage.
TEST_F(D3D11ManagerTest, CreatesDefaultBufferWithUnorderedAccessBind) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    // ByteWidth, Usage(DEFAULT), BindFlags, CPUAccessFlags, MiscFlags, Stride.
    const std::array<u32, 6> desc{16, 0, 0x89, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(memory.read_u64(GPU_BUFFER_OUT).value(), 0u);
    EXPECT_EQ(backend.state.last_buffer_desc.byte_width, 16u);
    EXPECT_TRUE(backend.state.last_buffer_desc.unordered_access);
    EXPECT_TRUE(backend.state.last_buffer_desc.vertex_buffer);
    EXPECT_TRUE(backend.state.last_buffer_desc.shader_resource);

    // DYNAMIC, IMMUTABLE and STAGING cannot carry unordered access.
    for (const u32 usage : {1u, 2u, 3u}) {
        const std::array<u32, 6> bad{
            16, usage, 0x89, usage == 2u ? 0x10000u : 0u, 0, 0};
        ASSERT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(bad.data()),
            sizeof(bad)).is_ok());
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "usage=" << usage;
    }
}

TEST_F(D3D11ManagerTest, ObjectStorageGrowsPastOneChunkOfInterfaces) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_BUFFER_DESC: ByteWidth, Usage, BindFlags, CPUAccessFlags,
    // MiscFlags, StructureByteStride. A constant buffer is the smallest
    // interface UE5 allocates in bulk — one per material per pass.
    const std::array<u32, 6> desc{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    // One 0x40-byte interface per object in a STORAGE_SIZE arena caps the
    // process at a few thousand live objects. Exceed that: the arena has to
    // map another chunk, not report E_OUTOFMEMORY to the guest.
    constexpr usize kObjectSize = 0x40;
    const usize beyond_one_chunk = (STORAGE_SIZE / kObjectSize) + 512;
    std::vector<GuestAddr> created;
    created.reserve(beyond_one_chunk);
    for (usize i = 0; i < beyond_one_chunk; ++i) {
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK)
            << "CreateBuffer #" << i << " of " << beyond_one_chunk;
        const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
        ASSERT_NE(buffer, 0u) << "CreateBuffer #" << i;
        created.push_back(buffer);
    }

    // Every interface must be distinct and independently addressable —
    // growing must not recycle a live object into a second one.
    std::unordered_set<GuestAddr> unique(created.begin(), created.end());
    EXPECT_EQ(unique.size(), created.size());

    // And growth must actually have happened rather than the objects having
    // fit after all: the addresses cannot all lie in one chunk.
    const auto [lo, hi] =
        std::minmax_element(created.begin(), created.end());
    EXPECT_GT(*hi - *lo, STORAGE_SIZE);
}

TEST_F(D3D11ManagerTest, CreatesBlockCompressedTexturesAcrossTheWholeBcFamily) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_TEXTURE2D_DESC: Width, Height, MipLevels, ArraySize, Format,
    // SampleDesc.Count, SampleDesc.Quality, Usage, BindFlags, CPUAccessFlags,
    // MiscFlags.
    const auto create_bc = [&](u32 dxgi_format, u32 width, u32 height,
                               u32 mips) {
        const std::array<u32, 11> desc{
            width, height, mips, 1, dxgi_format, 1, 0, 0, 0x8, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, TEXTURE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 5);
    };

    // The exact descriptor UE5 refused on: a 256x256 BC5 normal map streamed
    // in at its 64x64 mip. BC5 is the two-channel block format every modern
    // engine stores tangent-space normal maps in (Learn, Texture Block
    // Compression in Direct3D 11), so refusing it means refusing the game's
    // materials.
    create_bc(83, 64, 64, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "BC5_UNORM";
    EXPECT_NE(memory.read_u64(TEXTURE_OUT).value(), 0u);
    EXPECT_EQ(backend.state.last_texture_desc.pixel_format,
              graphics::PixelFormat::BC5_RGUnorm);

    // Every remaining BC format, each mapping to its own Metal format rather
    // than collapsing onto a neighbour: a wrong BC format decodes to garbage
    // rather than failing.
    struct BcCase {
        u32 dxgi;
        graphics::PixelFormat expected;
        const char* name;
    };
    const std::array<BcCase, 12> cases{{
        {72, graphics::PixelFormat::BC1_RGBA_sRGB,      "BC1_UNORM_SRGB"},
        {75, graphics::PixelFormat::BC2_RGBA_sRGB,      "BC2_UNORM_SRGB"},
        {78, graphics::PixelFormat::BC3_RGBA_sRGB,      "BC3_UNORM_SRGB"},
        {80, graphics::PixelFormat::BC4_RUnorm,         "BC4_UNORM"},
        {81, graphics::PixelFormat::BC4_RSnorm,         "BC4_SNORM"},
        {84, graphics::PixelFormat::BC5_RGSnorm,        "BC5_SNORM"},
        {95, graphics::PixelFormat::BC6H_RGBUfloat,     "BC6H_UF16"},
        {96, graphics::PixelFormat::BC6H_RGBFloat,      "BC6H_SF16"},
        {99, graphics::PixelFormat::BC7_RGBAUnorm_sRGB, "BC7_UNORM_SRGB"},
        // TYPELESS resources are legal and resolve to the unsigned member of
        // their family; the view supplies the interpretation.
        {82, graphics::PixelFormat::BC5_RGUnorm,        "BC5_TYPELESS"},
        {79, graphics::PixelFormat::BC4_RUnorm,         "BC4_TYPELESS"},
        {70, graphics::PixelFormat::BC1_RGBA,           "BC1_TYPELESS"},
    }};
    for (const auto& c : cases) {
        create_bc(c.dxgi, 64, 64, 1);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << c.name;
        EXPECT_EQ(backend.state.last_texture_desc.pixel_format, c.expected)
            << c.name;
    }

    // Block footprints must follow the format, not the family: BC1 and BC4
    // pack a 4x4 block into 8 bytes, the rest into 16.
    EXPECT_EQ(graphics::compressed_block_bytes(
                  graphics::PixelFormat::BC4_RUnorm), 8u);
    EXPECT_EQ(graphics::compressed_block_bytes(
                  graphics::PixelFormat::BC5_RGUnorm), 16u);
    EXPECT_EQ(graphics::compressed_block_bytes(
                  graphics::PixelFormat::BC6H_RGBFloat), 16u);
}

TEST_F(D3D11ManagerTest, CreatesRenderTargetVolumeTexture) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr TEX3D_DESC = TEXTURE_DESC;
    constexpr GuestAddr TEX3D_OUT = TEXTURE_OUT;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_TEXTURE3D_DESC: Width, Height, Depth, MipLevels, Format, Usage,
    // BindFlags, CPUAccessFlags, MiscFlags.
    const auto create_texture_3d = [&](const std::array<u32, 9>& desc) {
        EXPECT_TRUE(memory.write_bytes(
            TEX3D_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEX3D_DESC);
        set_arg(2, 0);
        set_arg(3, TEX3D_OUT);
        d3d11.impl_DeviceMethod(marshaler, 6);
    };

    // Learn D3D11_TEXTURE3D_DESC: a volume texture is a legal render target on
    // its own — a 3D render-target view selects a mip and a range of W slices.
    // This is the shape UE5 uses for a GPU-filled colour lookup table.
    // 1x1x1, B8G8R8A8_TYPELESS, DEFAULT, SHADER_RESOURCE | RENDER_TARGET.
    create_texture_3d({1, 1, 1, 1, 90, 0, 0x8 | 0x20, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(TEX3D_OUT).value(), 0u);
    EXPECT_TRUE(backend.state.last_texture_3d_desc.render_target);
    EXPECT_TRUE(backend.state.last_texture_3d_desc.shader_resource);

    // Shader-resource only stays the common case and must not gain the usage.
    create_texture_3d({4, 4, 4, 1, 28, 0, 0x8, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_FALSE(backend.state.last_texture_3d_desc.render_target);

    // Render-target only is legal too.
    create_texture_3d({4, 4, 4, 1, 28, 0, 0x20, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_TRUE(backend.state.last_texture_3d_desc.render_target);
    EXPECT_FALSE(backend.state.last_texture_3d_desc.shader_resource);

    // Learn D3D11_RESOURCE_MISC_GENERATE_MIPS: the resource must be both a
    // render target and a shader resource.
    create_texture_3d({4, 4, 4, 3, 28, 0, 0x8, 0, 0x1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_texture_3d({4, 4, 4, 3, 28, 0, 0x8 | 0x20, 0, 0x1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    // Learn D3D11_BIND_UNORDERED_ACCESS: a volume is a legal UAV resource
    // (RWTexture3D<T>), the shape UE5 fills on the GPU.
    create_texture_3d({4, 4, 4, 1, 28, 0, 0x8 | 0x80, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "unordered access";
    EXPECT_TRUE(backend.state.last_texture_3d_desc.unordered_access);

    // A bind flag a volume texture cannot carry (DEPTH_STENCIL).
    create_texture_3d({4, 4, 4, 1, 28, 0, 0x8 | 0x40, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "depth stencil";
    // Corrected by Windows (d3d11_a02_create_contracts_probe "D bind0
    // DEF"): a DEFAULT volume with no bind flag -- a copy target -- is legal.
    create_texture_3d({4, 4, 4, 1, 28, 0, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "no bind flags";
}

TEST_F(D3D11ManagerTest, RenderTargetViewDimensionIsAViewProperty) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr RTV_DESC = TEXTURE_DESC + 0x80;
    constexpr GuestAddr RTV_OUT = VIEW_OUT;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto create_texture = [&](u32 array_size, GuestAddr out) {
        // D3D11_TEXTURE2D_DESC: 64x64, DXGI_FORMAT_R16G16B16A16_FLOAT (10),
        // DEFAULT, SHADER_RESOURCE | RENDER_TARGET, 1 mip.
        const std::array<u32, 11> tex{64,  64, 1, array_size, 10, 1,
                                      0,   0,  0x8 | 0x20,    0,  0};
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(tex.data()),
            sizeof(tex)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, out);
        d3d11.impl_DeviceMethod(marshaler, 5);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(out).value();
    };
    const GuestAddr single = create_texture(1, TEXTURE_OUT);
    const GuestAddr arrayed = create_texture(4, TEXTURE_OUT + 0x10);
    ASSERT_NE(single, 0u);
    ASSERT_NE(arrayed, 0u);

    // D3D11_RENDER_TARGET_VIEW_DESC + D3D11_TEX2D_ARRAY_RTV:
    // Format, ViewDimension, MipSlice, FirstArraySlice, ArraySize.
    const auto create_rtv = [&](GuestAddr resource,
                                const std::array<u32, 5>& rtv) {
        EXPECT_TRUE(memory.write_bytes(
            RTV_DESC, reinterpret_cast<const Byte*>(rtv.data()),
            sizeof(rtv)).is_ok());
        EXPECT_TRUE(memory.write_u64(RTV_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, resource);
        set_arg(2, RTV_DESC);
        set_arg(3, RTV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 9);
    };

    // Learn D3D11_RTV_DIMENSION: every Texture2D is an array — of one element
    // when ArraySize is 1 — so TEXTURE2DARRAY (5) over a single-slice
    // resource is legal, and so is TEXTURE2D (4) over an array resource,
    // which addresses slice 0.
    create_rtv(single, {10, 5, 0, 0, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "array view of a single slice";
    EXPECT_NE(memory.read_u64(RTV_OUT).value(), 0u);
    create_rtv(single, {10, 4, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "plain 2D view";
    create_rtv(arrayed, {10, 4, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "2D view of an array resource";
    create_rtv(arrayed, {10, 5, 0, 2, 2});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "a window inside the array";

    // The resource properties are still enforced: this resource is neither
    // multisampled nor a volume.
    create_rtv(single, {10, 5, 0, 1, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "slice past the array";
    // A multisample dimension over a one-sample Texture2D is legal: it
    // renders as the single-sample view (d3d11_a03_views_probe "rtv tex2d as
    // 2DMS"). This expected E_INVALIDARG before the oracle measured it.
    create_rtv(single, {10, 6, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "TEXTURE2DMS on a "
                                           "single-sampled resource";
    create_rtv(single, {10, 8, 0, 0, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "TEXTURE3D on a 2D "
                                                   "resource";
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
