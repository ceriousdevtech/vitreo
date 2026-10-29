// SPDX-License-Identifier: LGPL-2.1-only
//
// Extended sharing: mip/array/buffer sharing and planar/packed video textures.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_sharing.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ExtendedSharingTest, CapabilitiesReportExtendedSharing) {
    // [caps] "OPTIONS.ExtendedResourceSharing ... 1" (BOOL[13]) and
    // "OPTIONS4.ExtendedNV12SharedTexture ... 1".
    EXPECT_EQ(feature(5, 56, 13), 1u);
    EXPECT_EQ(feature(17, 4, 0), 1u);
    // Learn D3D11_SHARED_RESOURCE_TIER: TIER_1 is ExtendedResourceSharing
    // with ExtendedNV12SharedTextureSupported. WARP answers TIER_3; TIER_2
    // and 3 add sharing with Direct3D 12, whose OpenSharedHandle this
    // runtime does not implement.
    EXPECT_EQ(feature(19, 4, 0), 1u);
    // [caps] SHAREABLE: block-compressed, depth and planar video formats
    // share too ("SHAREABLE BC1_UNORM 1", "D32_FLOAT 1", "NV12 1", "P010 1",
    // "P016 1"); B5G6R5 shares without the bit ("SHAREABLE B5G6R5_UNORM 0").
    for (const u32 format : {71u, 40u, 103u, 104u, 105u}) {
        SCOPED_TRACE(format);
        EXPECT_NE(support2(format) & 0x400u, 0u);
    }
    EXPECT_EQ(support2(85) & 0x400u, 0u);
}

TEST_F(D3D11ExtendedSharingTest, NtHandlesShareMipmappedAndArrayTextures) {
    // [shapes] "mips2 SHNT", "array2 SHNT", "R16G16B16A16_FLOAT mips3 array2
    // SHNT" create and open with the creator's description; the NTHANDLE
    // exclusions stay ("TEXTURECUBE SHNT", "MSAA 4x SHNT" E_INVALIDARG).
    struct Shape {
        u32 format, mips, array;
    };
    for (const Shape shape : {Shape{87, 2, 1}, Shape{87, 1, 2},
                              Shape{10, 3, 2}}) {
        SCOPED_TRACE(testing::Message() << shape.format << " m" << shape.mips
                                        << " a" << shape.array);
        const GuestAddr texture =
            make(shape.format, 0x802, 0x28, shape.mips, shape.array);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        u64 handle = 0;
        ASSERT_EQ(create_nt(texture, 0x80000001u, handle), d3d11::S_OK);
        GuestAddr opened = 0;
        ASSERT_EQ(open(48, handle, winapi::IID_ID3D11TEXTURE2D, opened),
                  d3d11::S_OK);
        const std::array<u32, 11> expected{16, 8, shape.mips, shape.array,
                                           shape.format, 1, 0, 0, 0x28, 0,
                                           0x802};
        EXPECT_EQ(desc_of(opened), expected);
        (void)handles.close(static_cast<Handle>(handle));
        release_texture(opened);
        release_texture(texture);
    }
    (void)make(87, 0x806, 0x28, 1, 6);
    EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
    (void)make(87, 0x802, 0x28, 1, 1, 4);
    EXPECT_EQ(last_hr, d3d11::E_INVALIDARG);
    // Without NTHANDLE a multisampled or cube texture shares ("MSAA 4x SH",
    // "TEXTURECUBE SH" kmt set).
    const GuestAddr msaa = make(87, 0x2, 0x28, 1, 1, 4);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    u64 handle = 0;
    EXPECT_EQ(get_kmt(msaa, handle), d3d11::S_OK);
    EXPECT_NE(handle, 0u);
    release_texture(msaa);
}

TEST_F(D3D11ExtendedSharingTest, RestrictSharedResourceFlagsShare) {
    // [shapes] "RESTRICT_SHARED_RESOURCE SH" x0x2002, "..._DRIVER SH"
    // x0x4002, "RESTRICT_SHARED_RESOURCE|_DRIVER SH" x0x6002 and the SHNT
    // forms create and open in the creating process with their flags.
    for (const u32 misc : {0x2002u, 0x4002u, 0x6002u, 0x2802u, 0x6802u}) {
        SCOPED_TRACE(misc);
        const GuestAddr texture = make(87, misc);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        const bool nt = (misc & 0x800u) != 0;
        u64 handle = 0;
        ASSERT_EQ(nt ? create_nt(texture, 0x80000001u, handle)
                     : get_kmt(texture, handle),
                  d3d11::S_OK);
        GuestAddr opened = 0;
        ASSERT_EQ(open(nt ? 48 : 28, handle, winapi::IID_ID3D11TEXTURE2D,
                       opened),
                  d3d11::S_OK);
        EXPECT_EQ(desc_of(opened)[10], misc);
        if (nt) (void)handles.close(static_cast<Handle>(handle));
        release_texture(opened);
        release_texture(texture);
    }
}

TEST_F(D3D11ExtendedSharingTest, SharedBufferOpensAsABufferWithItsDesc) {
    // [handles] "CreateBuffer SHARED ... kmt=0x00000000/set open=0x0";
    // d3d11_share_ers_probe "BUF vertex SH ... od=256/u0/b0x1/c0x0/x0x2/s0"
    // and [iid] "BUF vertex SH as ID3D11Texture2D hr=0x80004002".
    const GuestAddr buffer = make_buffer(256, 0, 0x1, 0, 0x2);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    u64 handle = 0;
    ASSERT_EQ(get_kmt(buffer, handle), d3d11::S_OK);
    ASSERT_NE(handle, 0u);
    GuestAddr opened = 0;
    ASSERT_EQ(open(28, handle, winapi::IID_ID3D11BUFFER, opened),
              d3d11::S_OK);
    ASSERT_NE(opened, 0u);
    EXPECT_NE(opened, buffer);
    set_arg(0, opened);
    set_arg(1, SCRATCH);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_BufferMethod(marshaler, 10);  // GetDesc
    std::array<u32, 6> desc{};
    ASSERT_TRUE(memory.read_bytes(SCRATCH, reinterpret_cast<Byte*>(desc.data()),
                                  sizeof(desc)).is_ok());
    EXPECT_EQ(desc, (std::array<u32, 6>{256, 0, 0x1, 0, 0x2, 0}));
    GuestAddr as_texture = 0;
    EXPECT_EQ(open(28, handle, winapi::IID_ID3D11TEXTURE2D, as_texture),
              d3d11::E_NOINTERFACE);
    EXPECT_EQ(as_texture, 0u);
    // [iid] "BUF raw SRV|UAV KM as IDXGIKeyedMutex hr=0x00000000".
    const GuestAddr keyed = make_buffer(256, 0, 0x88, 0, 0x120);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    u64 keyed_handle = 0;
    ASSERT_EQ(get_kmt(keyed, keyed_handle), d3d11::S_OK);
    GuestAddr keyed_opened = 0;
    ASSERT_EQ(open(28, keyed_handle, winapi::IID_IDXGIKEYEDMUTEX,
                   keyed_opened),
              d3d11::S_OK);
    // Its keyed mutex hands ownership over like a texture's ("BUF raw
    // SRV|UAV KM ... release=0x00000000").
    EXPECT_EQ(sync(keyed, 8, 0, 0), d3d11::S_OK);
    EXPECT_EQ(sync(keyed, 9, 1, 0), d3d11::S_OK);
    EXPECT_EQ(sync(keyed_opened - 0x28, 8, 1, 0), d3d11::S_OK);
}

TEST_F(D3D11ExtendedSharingTest, OpenedStagingResourceDoesNotMap) {
    // d3d11_share_ers_probe "2D STAGING rw SH ... map=0x8007000e copyout=
    // match copyin=done creator=match": an opening of a shared STAGING
    // texture does not Map; the creator's does, and sees the same bytes.
    const GuestAddr staging = make(87, 0x2, 0, 1, 1, 1, 3, 0x30000);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    u64 handle = 0;
    ASSERT_EQ(get_kmt(staging, handle), d3d11::S_OK);
    GuestAddr opened = 0;
    ASSERT_EQ(open(28, handle, winapi::IID_ID3D11TEXTURE2D, opened),
              d3d11::S_OK);
    EXPECT_EQ(map(context_b, opened, 0, 1), kOutOfMemory);
    ASSERT_EQ(map(context, staging, 0, 2), d3d11::S_OK);  // WRITE
    const GuestAddr data = memory.read_u64(SCRATCH).value();
    ASSERT_NE(data, 0u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(), 64u);  // RowPitch
    ASSERT_TRUE(memory.write_u32(data, 0x11223344u).is_ok());
    unmap(context, staging, 0);
    ASSERT_EQ(map(context, staging, 0, 1), d3d11::S_OK);  // READ
    EXPECT_EQ(memory.read_u32(memory.read_u64(SCRATCH).value()).value(),
              0x11223344u);
    unmap(context, staging, 0);
    EXPECT_GE(backend.state.shared_publications, 1u);
    release_texture(opened);
    release_texture(staging);
}

// Planar video textures (Learn DXGI_FORMAT NV12 / P010 / P016), measured on
// Windows: tools/oracle_probe/d3d11_nv12_probe.expected.
TEST_F(D3D11ExtendedSharingTest, PlanarVideoTexturesFollowTheMeasuredRules) {
    struct Row {
        u32 w, h, mips, array, samples, bind, misc, format;
        u32 hr;
    };
    const Row rows[] = {
        // [create NV12] "size 16x8/2x2/4096x2 OK", "15x8, 16x7, 1x1, 1x2,
        // 2x1 IA".
        {16, 8, 1, 1, 1, 0x8, 0, 103, d3d11::S_OK},
        {2, 2, 1, 1, 1, 0x8, 0, 103, d3d11::S_OK},
        {4096, 2, 1, 1, 1, 0x8, 0, 103, d3d11::S_OK},
        {15, 8, 1, 1, 1, 0x8, 0, 103, d3d11::E_INVALIDARG},
        {16, 7, 1, 1, 1, 0x8, 0, 103, d3d11::E_INVALIDARG},
        {1, 1, 1, 1, 1, 0x8, 0, 103, d3d11::E_INVALIDARG},
        // "mips 2 IA", "mips 0 IA", "array 2 OK", "array 3/6 IA".
        {16, 8, 2, 1, 1, 0x8, 0, 103, d3d11::E_INVALIDARG},
        {16, 8, 0, 1, 1, 0x8, 0, 103, d3d11::E_INVALIDARG},
        {16, 8, 1, 2, 1, 0x8, 0, 103, d3d11::S_OK},
        {16, 8, 1, 3, 1, 0x8, 0, 103, d3d11::E_INVALIDARG},
        // "bind 0 / SRV|RTV|UAV OK", "DSV IA", "ENCODER OK" (NV12 only:
        // "[create P010] bind ENCODER IA"), "msaa4 IA".
        {16, 8, 1, 1, 1, 0x0, 0, 103, d3d11::S_OK},
        {16, 8, 1, 1, 1, 0xA8, 0, 103, d3d11::S_OK},
        {16, 8, 1, 1, 1, 0x40, 0, 103, d3d11::E_INVALIDARG},
        {16, 8, 1, 1, 1, 0x400, 0, 103, d3d11::S_OK},
        {16, 8, 1, 1, 1, 0x400, 0, 104, d3d11::E_INVALIDARG},
        {16, 8, 1, 1, 4, 0x20, 0, 103, d3d11::E_INVALIDARG},
        // "misc GENERATE_MIPS OK", "TEXTURECUBE IA", "RESOURCE_CLAMP OK".
        {16, 16, 1, 1, 1, 0x28, 0x1, 103, d3d11::S_OK},
        {16, 16, 1, 6, 1, 0x8, 0x4, 103, d3d11::E_INVALIDARG},
        {16, 16, 1, 1, 1, 0x8, 0x80, 103, d3d11::S_OK},
        {16, 8, 1, 2, 1, 0xA8, 0, 104, d3d11::S_OK},
        {16, 8, 1, 2, 1, 0xA8, 0, 105, d3d11::S_OK},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(testing::Message()
                     << row.format << " " << row.w << "x" << row.h << " m"
                     << row.mips << " a" << row.array << " s" << row.samples
                     << " b0x" << std::hex << row.bind << " x0x" << row.misc);
        const std::array<u32, 11> desc{row.w, row.h, row.mips, row.array,
                                       row.format, row.samples, 0, 0,
                                       row.bind, 0, row.misc};
        ASSERT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        ASSERT_TRUE(memory.write_u64(TEXTURE_OUT, 0).is_ok());
        EXPECT_EQ(dispatch_device(device, 5, {TEXTURE_DESC, 0, TEXTURE_OUT}),
                  row.hr);
        release_texture(memory.read_u64(TEXTURE_OUT).value());
    }
    // [layout NV12] "staging 16x8 array2 ... map0 row=16 depth=128 map1
    // row=16 depth=128 map2=0x80070057": one subresource per slice holding
    // both planes, DepthPitch the luma plane's.
    const GuestAddr staging = make(103, 0, 0, 1, 2, 1, 3, 0x30000);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    for (u32 sub = 0; sub < 2; ++sub) {
        ASSERT_EQ(map(context, staging, sub, 1), d3d11::S_OK);
        EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(), 16u);
        EXPECT_EQ(memory.read_u32(SCRATCH + 12).value(), 128u);
        unmap(context, staging, sub);
    }
    EXPECT_EQ(map(context, staging, 2, 1), d3d11::E_INVALIDARG);
    release_texture(staging);
}

TEST_F(D3D11ExtendedSharingTest, PlanarVideoViewsSelectAPlane) {
    // [views NV12]: R8_UNORM / R8_UINT views are plane 0, R8G8_UNORM /
    // R8G8_UINT plane 1; a NULL desc, UNKNOWN, NV12 and R8_SNORM are
    // E_INVALIDARG; GetDesc1 reports the plane ("srv R8G8_UNORM 2D ...
    // plane=1"); *View1 takes only the named plane ("srv1 R8G8_UNORM
    // plane1" OK, plane0 IA). [views P010]: R32_UINT is a chroma UAV only.
    const GuestAddr nv12 = make(103, 0, 0xA8);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    const GuestAddr p010 = make(104, 0, 0xA8);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    const auto srv = [&](GuestAddr texture, u32 format, bool with_desc) {
        const std::array<u32, 4> desc{format, 4, 0, 1};  // TEXTURE2D
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()), sizeof(desc))
                        .is_ok());
        EXPECT_TRUE(memory.write_u64(OUT, 0).is_ok());
        const u32 hr = dispatch_device(device, 7,
                                       {texture, with_desc ? DESC : 0, OUT});
        return std::pair<u32, GuestAddr>{hr, memory.read_u64(OUT).value()};
    };
    EXPECT_EQ(srv(nv12, 0, false).first, d3d11::E_INVALIDARG);
    // UNKNOWN (the resource's own format), NV12, R8_SNORM, R16_UNORM.
    for (const u32 bad : {0u, 103u, 63u, 56u}) {
        SCOPED_TRACE(bad);
        EXPECT_EQ(srv(nv12, bad, true).first, d3d11::E_INVALIDARG);
    }
    const auto [luma_hr, luma] = srv(nv12, 61, true);
    EXPECT_EQ(luma_hr, d3d11::S_OK);
    const auto [chroma_hr, chroma] = srv(nv12, 49, true);
    ASSERT_EQ(chroma_hr, d3d11::S_OK);
    // ID3D11ShaderResourceView1::GetDesc1 (slot 9): PlaneSlice at +16.
    set_arg(0, chroma);
    set_arg(1, SCRATCH);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 9);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 49u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 16).value(), 1u);
    set_arg(0, luma);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 9);
    EXPECT_EQ(memory.read_u32(SCRATCH + 16).value(), 0u);
    // CreateShaderResourceView1 (device slot 57): D3D11_TEX2D_SRV1
    // {MostDetailedMip, MipLevels, PlaneSlice}.
    const auto srv1 = [&](u32 format, u32 plane) {
        const std::array<u32, 5> desc{format, 4, 0, 1, plane};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()), sizeof(desc))
                        .is_ok());
        return dispatch_device(device, 57, {nv12, DESC, OUT});
    };
    EXPECT_EQ(srv1(49, 1), d3d11::S_OK);
    EXPECT_EQ(srv1(49, 0), d3d11::E_INVALIDARG);
    EXPECT_EQ(srv1(61, 0), d3d11::S_OK);
    EXPECT_EQ(srv1(61, 1), d3d11::E_INVALIDARG);
    // UAVs (device slot 8): R32_UINT names P010's chroma plane only.
    const auto uav = [&](GuestAddr texture, u32 format) {
        const std::array<u32, 3> desc{format, 4, 0};  // TEXTURE2D, MipSlice
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()), sizeof(desc))
                        .is_ok());
        return dispatch_device(device, 8, {texture, DESC, OUT});
    };
    EXPECT_EQ(uav(p010, 42), d3d11::S_OK);
    EXPECT_EQ(uav(nv12, 42), d3d11::E_INVALIDARG);
    EXPECT_EQ(uav(p010, 35), d3d11::S_OK);
    EXPECT_EQ(uav(p010, 56), d3d11::S_OK);
    EXPECT_EQ(uav(nv12, 50), d3d11::S_OK);
    release_texture(p010);
    release_texture(nv12);
}

// The packed and palettized video formats (Learn DXGI_FORMAT; AYUV, Y410,
// Y416, YUY2, Y210, Y216, 420_OPAQUE, P8, A8P8), measured on Windows:
// tools/oracle_probe/d3d11_video_formats_probe.expected [create].
TEST_F(D3D11ExtendedSharingTest, PackedVideoTexturesFollowTheMeasuredRules) {
    struct Row {
        u32 w, h, mips, array, samples, usage, bind, cpu, misc, format;
        bool initial;
        u32 hr;
    };
    constexpr u32 IA = d3d11::E_INVALIDARG;
    constexpr u32 OK = d3d11::S_OK;
    const Row rows[] = {
        // [create AYUV] "size 7x4 OK", "mips 0 got=4", "bind RTV OK",
        // "bind DECODER IA", "misc GENERATE_MIPS OK", "msaa2 SRV IA".
        {7, 4, 1, 1, 1, 0, 0x0, 0, 0, 100, false, OK},
        {8, 4, 0, 1, 1, 0, 0x8, 0, 0, 100, false, OK},
        {8, 4, 1, 1, 1, 0, 0x20, 0, 0, 100, false, OK},
        {8, 4, 1, 1, 1, 0, 0x200, 0, 0, 100, false, IA},
        {8, 8, 0, 1, 1, 0, 0x28, 0, 0x1, 100, false, OK},
        {8, 4, 1, 1, 2, 0, 0x8, 0, 0, 100, false, IA},
        // [create Y410] "mips 2 IA", "bind RTV IA", "bind UAV OK".
        {8, 4, 2, 1, 1, 0, 0x8, 0, 0, 101, false, IA},
        {8, 4, 1, 1, 1, 0, 0x20, 0, 0, 101, false, IA},
        {8, 4, 1, 1, 1, 0, 0x80, 0, 0, 101, false, OK},
        // [create Y416] "mips 2 OK", "misc GENERATE_MIPS IA".
        {8, 4, 2, 1, 1, 0, 0x8, 0, 0, 102, false, OK},
        {8, 8, 0, 1, 1, 0, 0x28, 0, 0x1, 102, false, IA},
        // [create YUY2] "size 7x4 / 1x2 IA", "size 8x3 / 2x1 OK",
        // "mips 2 IA", "bind SRV|UAV OK", "bind RTV IA"; Y210 / Y216 alike.
        {7, 4, 1, 1, 1, 0, 0x8, 0, 0, 107, false, IA},
        {1, 2, 1, 1, 1, 0, 0x8, 0, 0, 107, false, IA},
        {8, 3, 1, 1, 1, 0, 0x8, 0, 0, 107, false, OK},
        {2, 1, 1, 1, 1, 0, 0x8, 0, 0, 107, false, OK},
        {8, 4, 2, 1, 1, 0, 0x8, 0, 0, 107, false, IA},
        {8, 4, 1, 1, 1, 0, 0x88, 0, 0, 107, false, OK},
        {8, 4, 1, 1, 1, 0, 0x20, 0, 0, 108, false, IA},
        {7, 4, 1, 1, 1, 0, 0x8, 0, 0, 109, false, IA},
        // [create 420_OPAQUE] "size 8x4 / 6x2 OK", "8x3 IA", "array 2 OK",
        // "array 6 IA", "bind SRV IA", "usage DEFAULT init IA", "STAGING
        // IA", "misc SHARED 0 OK", "SHARED|NT 0 OK".
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0, 106, false, OK},
        {8, 3, 1, 1, 1, 0, 0x0, 0, 0, 106, false, IA},
        {8, 4, 1, 2, 1, 0, 0x0, 0, 0, 106, false, OK},
        {8, 4, 1, 6, 1, 0, 0x0, 0, 0, 106, false, IA},
        {8, 4, 1, 1, 1, 0, 0x8, 0, 0, 106, false, IA},
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0, 106, true, IA},
        {8, 4, 1, 1, 1, 3, 0x0, 0x20000, 0, 106, false, IA},
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0x2, 106, false, OK},
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0x802, 106, false, OK},
        // [create P8] "size 7x4 OK", "bind SRV IA", "usage DEFAULT init
        // OK", "STAGING RW init OK", "mips 2 IA", "misc SHARED 0 OK",
        // "SHARED|NT 0 IA" (no SHAREABLE); A8P8 alike.
        {7, 4, 1, 1, 1, 0, 0x0, 0, 0, 113, false, OK},
        {8, 4, 1, 1, 1, 0, 0x8, 0, 0, 113, false, IA},
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0, 113, true, OK},
        {8, 4, 1, 1, 1, 3, 0x0, 0x30000, 0, 114, true, OK},
        {8, 4, 2, 1, 1, 0, 0x0, 0, 0, 114, false, IA},
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0x2, 113, false, OK},
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0x802, 113, false, IA},
        // [create NV11] / [create P208]: nothing creates.
        {8, 4, 1, 1, 1, 0, 0x0, 0, 0, 110, false, IA},
        {8, 4, 1, 1, 1, 3, 0x0, 0x20000, 0, 130, false, IA},
    };
    constexpr GuestAddr INIT = DATA_BASE + 0x1B80;
    constexpr GuestAddr PIXELS = DATA_BASE + 0x2C00;
    ASSERT_TRUE(memory.write_u64(INIT, PIXELS).is_ok());
    ASSERT_TRUE(memory.write_u32(INIT + 8, 64).is_ok());  // SysMemPitch
    ASSERT_TRUE(memory.write_u32(INIT + 12, 0).is_ok());
    for (const Row& row : rows) {
        SCOPED_TRACE(testing::Message()
                     << row.format << " " << row.w << "x" << row.h << " m"
                     << row.mips << " a" << row.array << " s" << row.samples
                     << " u" << row.usage << " b0x" << std::hex << row.bind
                     << " x0x" << row.misc << (row.initial ? " init" : ""));
        const std::array<u32, 11> desc{row.w, row.h, row.mips, row.array,
                                       row.format, row.samples, 0, row.usage,
                                       row.bind, row.cpu, row.misc};
        ASSERT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        ASSERT_TRUE(memory.write_u64(TEXTURE_OUT, 0).is_ok());
        EXPECT_EQ(dispatch_device(device, 5,
                                  {TEXTURE_DESC, row.initial ? INIT : 0,
                                   TEXTURE_OUT}),
                  row.hr);
        release_texture(memory.read_u64(TEXTURE_OUT).value());
    }
    // [caps]: the FORMAT_SUPPORT2 answers, less YUY2's DISPLAYABLE (no
    // displayable-surface support here) -- "fs2=00000000/00000480" (AYUV,
    // Y410, Y416, Y210, Y216), "00010480" (YUY2), "00000400" (420_OPAQUE).
    EXPECT_EQ(support2(100), 0x480u);
    EXPECT_EQ(support2(101), 0x480u);
    EXPECT_EQ(support2(102), 0x480u);
    EXPECT_EQ(support2(107), 0x480u);
    EXPECT_EQ(support2(108), 0x480u);
    EXPECT_EQ(support2(109), 0x480u);
    EXPECT_EQ(support2(106), 0x400u);
}

TEST_F(D3D11ExtendedSharingTest, PackedVideoStorageViewsAndPixelPairs) {
    // Learn DXGI_FORMAT: a YUY2 / Y210 / Y216 pixel pair is one texel of the
    // element view (R8G8B8A8 / R16G16B16A16), so the host texture is that
    // format at half the width; AYUV / Y410 / Y416 are one texel a pixel.
    struct Storage { u32 format; u32 width; graphics::PixelFormat pixel; };
    const Storage storages[] = {
        {100, 16, graphics::PixelFormat::RGBA8Unorm},
        {101, 16, graphics::PixelFormat::RGB10A2Unorm},
        {102, 16, graphics::PixelFormat::RGBA16Unorm},
        {107, 8, graphics::PixelFormat::RGBA8Unorm},
        {108, 8, graphics::PixelFormat::RGBA16Unorm},
        {109, 8, graphics::PixelFormat::RGBA16Unorm},
    };
    for (const Storage& storage : storages) {
        SCOPED_TRACE(storage.format);
        const GuestAddr texture = make(storage.format, 0, 0x88);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        EXPECT_EQ(backend.state.last_texture_desc.width, storage.width);
        EXPECT_EQ(backend.state.last_texture_desc.pixel_format, storage.pixel);
        // Viewed as UNORM, UINT and (AYUV, Y410, YUY2) R32_UINT UAVs.
        EXPECT_TRUE(backend.state.last_texture_desc.pixel_format_view);
        release_texture(texture);
    }
    // [layout ...] "staging 16x8 array2 ... map0 row=R depth=D": R is the
    // pixel pair's bytes / 2 a pixel (YUY2 32, Y210 64), or the texel's.
    const std::pair<u32, u32> pitches[] = {
        {100, 64}, {101, 64}, {102, 128}, {107, 32}, {108, 64}, {113, 16},
        {114, 32}};
    for (const auto& [format, row_pitch] : pitches) {
        SCOPED_TRACE(format);
        const GuestAddr staging = make(format, 0, 0, 1, 2, 1, 3, 0x30000);
        ASSERT_EQ(last_hr, d3d11::S_OK);
        ASSERT_EQ(map(context, staging, 1, 1), d3d11::S_OK);
        EXPECT_EQ(memory.read_u32(SCRATCH + 8).value(), row_pitch);
        EXPECT_EQ(memory.read_u32(SCRATCH + 12).value(), row_pitch * 8u);
        unmap(context, staging, 1);
        release_texture(staging);
    }

    // [views YUY2]: R8G8B8A8_UNORM / _UINT SRVs and UAVs, R32_UINT UAVs
    // only; NULL, UNKNOWN, YUY2 itself, R8G8B8A8_TYPELESS / _SNORM refused;
    // the legacy R8G8_B8G8_UNORM SRV WARP creates is refused here as
    // unimplemented. [views Y210]: R16G16B16A16 only, no R32_UINT.
    const GuestAddr yuy2 = make(107, 0, 0x88);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    const GuestAddr y210 = make(108, 0, 0x88);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    const GuestAddr ayuv = make(100, 0, 0xA8);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    const auto view = [&](usize slot, GuestAddr texture, u32 format,
                          bool with_desc) {
        // SRV {Format, TEXTURE2D, MostDetailedMip 0, MipLevels 1}; RTV and
        // UAV {Format, TEXTURE2D, MipSlice 0}.
        const std::array<u32, 4> desc{format, 4, 0, 1};
        EXPECT_TRUE(memory.write_bytes(
            DESC, reinterpret_cast<const Byte*>(desc.data()), sizeof(desc))
                        .is_ok());
        EXPECT_TRUE(memory.write_u64(OUT, 0).is_ok());
        return dispatch_device(device, slot,
                               {texture, with_desc ? DESC : 0, OUT});
    };
    constexpr usize SRV = 7, UAV = 8, RTV = 9;
    EXPECT_EQ(view(SRV, yuy2, 0, false), d3d11::E_INVALIDARG);
    for (const u32 bad : {0u, 107u, 27u, 31u, 68u}) {
        SCOPED_TRACE(bad);
        EXPECT_EQ(view(SRV, yuy2, bad, true), d3d11::E_INVALIDARG);
    }
    EXPECT_EQ(view(SRV, yuy2, 28, true), d3d11::S_OK);
    EXPECT_EQ(view(SRV, yuy2, 30, true), d3d11::S_OK);
    EXPECT_EQ(view(SRV, yuy2, 42, true), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(UAV, yuy2, 42, true), d3d11::S_OK);
    EXPECT_EQ(view(UAV, yuy2, 28, true), d3d11::S_OK);
    EXPECT_EQ(view(SRV, y210, 11, true), d3d11::S_OK);
    EXPECT_EQ(view(SRV, y210, 12, true), d3d11::S_OK);
    EXPECT_EQ(view(UAV, y210, 42, true), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(SRV, y210, 10, true), d3d11::E_INVALIDARG);
    EXPECT_EQ(view(RTV, ayuv, 28, true), d3d11::S_OK);
    EXPECT_EQ(view(RTV, ayuv, 0, false), d3d11::E_INVALIDARG);

    // UpdateSubresource (context slot 48) and CopySubresourceRegion (46)
    // address pixel pairs: [layout YUY2] "box2,1-6,3" writes, "box1,1-5,3"
    // and "box0,2-7,4" drop; "copy@2,1<-0,0-4,2" copies, "copy@1,1" and a
    // box from x 1 drop. The host texture is addressed in texels (x / 2).
    constexpr GuestAddr BOX = DATA_BASE + 0x1B90;
    const auto update = [&](std::array<u32, 6> box) {
        EXPECT_TRUE(memory.write_bytes(
            BOX, reinterpret_cast<const Byte*>(box.data()), sizeof(box))
                        .is_ok());
        set_arg(0, context);
        set_arg(1, yuy2);
        set_arg(2, 0);
        set_arg(3, BOX);
        set_arg(4, DATA_BASE + 0x2C00);
        set_arg(5, 64);
        set_arg(6, 0);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 48);
    };
    const unsigned updates = backend.state.texture_updates;
    update({2, 1, 0, 6, 3, 1});
    ASSERT_EQ(backend.state.texture_updates, updates + 1u);
    EXPECT_EQ(backend.state.last_texture_update_region,
              (std::array<unsigned, 6>{0, 0, 1, 1, 2, 2}));
    EXPECT_EQ(backend.state.last_texture_update_row_pitch, 64u);
    update({1, 1, 0, 5, 3, 1});
    update({0, 2, 0, 7, 4, 1});
    EXPECT_EQ(backend.state.texture_updates, updates + 1u);

    const GuestAddr source = make(107, 0, 0x88);
    ASSERT_EQ(last_hr, d3d11::S_OK);
    const auto copy = [&](u32 x, u32 y, std::array<u32, 6> box) {
        EXPECT_TRUE(memory.write_bytes(
            BOX, reinterpret_cast<const Byte*>(box.data()), sizeof(box))
                        .is_ok());
        set_arg(0, context);
        set_arg(1, yuy2);
        set_arg(2, 0);
        set_arg(3, x);
        set_arg(4, y);
        set_arg(5, 0);
        set_arg(6, source);
        set_arg(7, 0);
        set_arg(8, BOX);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 46);
    };
    const usize copies = backend.state.copy_regions.size();
    copy(2, 1, {0, 0, 0, 4, 2, 1});
    ASSERT_EQ(backend.state.copy_regions.size(), copies + 1u);
    EXPECT_EQ(backend.state.copy_regions.back(),
              (std::array<unsigned, 6>{1, 1, 0, 0, 2, 2}));
    copy(1, 1, {0, 0, 0, 4, 2, 1});
    copy(0, 0, {1, 1, 0, 5, 3, 1});
    copy(0, 0, {0, 0, 0, 3, 2, 1});
    EXPECT_EQ(backend.state.copy_regions.size(), copies + 1u);
    // Cross-format copies (a YUY2 texture and an R8G8B8A8_UNORM one of its
    // storage) write nothing ("cr<-el", "csr<-el": all zero on WARP).
    EXPECT_FALSE(dxgi_copy_formats_compatible(107, 28));
    EXPECT_FALSE(dxgi_reinterpret_copy_compatible(107, 28));
    release_texture(source);
    release_texture(ayuv);
    release_texture(y210);
    release_texture(yuy2);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
