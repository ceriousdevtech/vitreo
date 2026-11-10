/// @file test_d3d9_formats.cpp
/// @brief Format and multisample negotiation, and CreateDevice failure paths.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, CheckDeviceFormatLegacyBumpRequiresSignedDuDvFormat) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    constexpr u32 D3DUSAGE_QUERY_LEGACYBUMPMAP = 0x00008000u;
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 D3DRTYPE_TEXTURE, 60 /* D3DFMT_V8U8 */}),
              D3D_OK);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 D3DRTYPE_TEXTURE, 61 /* D3DFMT_L6V5U5 */}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 D3DRTYPE_TEXTURE, 62 /* D3DFMT_X8L8V8U8 */}),
              D3D_OK);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 D3DRTYPE_TEXTURE, 64 /* D3DFMT_V16U16 */}),
              D3D_OK);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 D3DRTYPE_TEXTURE, 117 /* D3DFMT_CxV8U8 */}),
              D3D_OK);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22,
                                 D3DUSAGE_QUERY_LEGACYBUMPMAP,
                                 1 /* D3DRTYPE_SURFACE */, 60}),
              D3DERR_NOTAVAILABLE);
}

TEST_F(D3D9ManagerTest, CheckDeviceMultiSampleTypeReportsSupportedModes) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    const GuestAddr quality = SCRATCH_ADDR;
    ASSERT_TRUE(memory_.write_u32(quality, 0xFFFFFFFFu).is_ok());
    EXPECT_EQ(invoke_method(11, {object, 0, 1, 22, 1, 0, static_cast<u32>(quality)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(quality).value(), 1u);

    ASSERT_TRUE(memory_.write_u32(quality, 0xFFFFFFFFu).is_ok());
    EXPECT_EQ(invoke_method(11, {object, 0, 1, 22, 1, 4, static_cast<u32>(quality)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(quality).value(), 1u);

    ASSERT_TRUE(memory_.write_u32(quality, 0xFFFFFFFFu).is_ok());
    EXPECT_EQ(invoke_method(11, {object, 0, 1, 22, 1, 3, static_cast<u32>(quality)}),
              D3DERR_NOTAVAILABLE);
    EXPECT_EQ(memory_.read_u32(quality).value(), 0u);

    ASSERT_TRUE(memory_.write_u32(quality, 0xFFFFFFFFu).is_ok());
    EXPECT_EQ(invoke_method(11, {object, 0, 1, 22, 1, 1, static_cast<u32>(quality)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(quality).value(), 3u);
}

TEST_F(D3D9ManagerTest, CreateDeviceWithMultisampleRecordsSampleCount) {
    const u32 factory = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(factory, 0u);
    write_present_parameters(640, 480);
    std::array<u32, 14> pp{};
    ASSERT_TRUE(memory_.read_bytes(PRESENT_ADDR,
                                   reinterpret_cast<Byte*>(pp.data()),
                                   sizeof(pp))
                    .is_ok());
    pp[4] = 4; // MultiSampleType = 4x
    ASSERT_TRUE(memory_.write_bytes(PRESENT_ADDR,
                                    reinterpret_cast<const Byte*>(pp.data()),
                                    sizeof(pp))
                    .is_ok());
    EXPECT_EQ(invoke_method(16, {factory, 0, 1, TEST_HWND, 0x00000040,
                                 static_cast<u32>(PRESENT_ADDR),
                                 static_cast<u32>(DEVICE_OUT_ADDR)}),
              D3D_OK);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.sample_count, 4u);
}

TEST_F(D3D9ManagerTest, MultisampleBackBufferDrawUsesMatchingSampleCount) {
    const u32 factory = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(factory, 0u);
    write_present_parameters(640, 480, true);
    std::array<u32, 14> pp{};
    ASSERT_TRUE(memory_.read_bytes(PRESENT_ADDR,
                                   reinterpret_cast<Byte*>(pp.data()),
                                   sizeof(pp))
                    .is_ok());
    pp[4] = 4; // MultiSampleType = 4x
    ASSERT_TRUE(memory_.write_bytes(PRESENT_ADDR,
                                    reinterpret_cast<const Byte*>(pp.data()),
                                    sizeof(pp))
                    .is_ok());
    ASSERT_EQ(invoke_method(16, {factory, 0, 1, TEST_HWND, 0x00000040,
                                 static_cast<u32>(PRESENT_ADDR),
                                 static_cast<u32>(DEVICE_OUT_ADDR)}),
              D3D_OK);
    const u32 device = memory_.read_u32(DEVICE_OUT_ADDR).value();
    ASSERT_NE(device, 0u);
    ASSERT_NE(bind_triangle_stream(device), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.last_pipeline_desc.sample_count, 4u);
}

TEST_F(D3D9ManagerTest, CreateDeviceRejectsLockableMultisampleBackBuffer) {
    const u32 factory = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(factory, 0u);
    write_present_parameters(640, 480);
    std::array<u32, 14> pp{};
    ASSERT_TRUE(memory_.read_bytes(PRESENT_ADDR,
                                   reinterpret_cast<Byte*>(pp.data()),
                                   sizeof(pp))
                    .is_ok());
    pp[4] = 4; // MultiSampleType
    pp[11] = 1; // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
    ASSERT_TRUE(memory_.write_bytes(PRESENT_ADDR,
                                    reinterpret_cast<const Byte*>(pp.data()),
                                    sizeof(pp))
                    .is_ok());
    EXPECT_EQ(invoke_method(16, {factory, 0, 1, TEST_HWND, 0x00000040,
                                 static_cast<u32>(PRESENT_ADDR),
                                 static_cast<u32>(DEVICE_OUT_ADDR)}),
              D3DERR_NOTAVAILABLE);
}

TEST_F(D3D9ManagerTest, CreateRenderTargetMultisampleIsNotLockable) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 16, 16, 21, 4, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(surface, 0u);
    EXPECT_EQ(backend_.record.last_texture_desc.sample_count, 4u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 12,
                     {surface, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x40 + 16).value(), 4u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {surface, static_cast<u32>(SCRATCH_ADDR + 0x80), 0, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, DepthTextureCreatesDepthAttachmentAndRejectsManagedPool) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    for (u32 format : {80u, 75u}) {
        ASSERT_EQ(invoke(D3D9InterfaceId::Device, 23,
                         {device, 64, 64, 1, 0x2u /* D3DUSAGE_DEPTHSTENCIL */, format,
                          D3DPOOL_DEFAULT, static_cast<u32>(SCRATCH_ADDR), 0}), D3D_OK);
        const auto texture = memory_.read_u32(SCRATCH_ADDR).value();
        ASSERT_NE(texture, 0u);
        EXPECT_TRUE(backend_.record.last_texture_desc.depth_stencil);
        EXPECT_FALSE(backend_.record.last_texture_desc.render_target);
        EXPECT_FALSE(backend_.record.last_texture_desc.cpu_writable);
        EXPECT_EQ(backend_.record.last_texture_desc.pixel_format,
                  format == 80 ? graphics::PixelFormat::D16Unorm
                               : graphics::PixelFormat::D32Float_S8Uint);
        ASSERT_EQ(invoke(D3D9InterfaceId::Texture, 18,
                         {texture, 0, static_cast<u32>(SCRATCH_ADDR + 8)}), D3D_OK);
        const auto surface = memory_.read_u32(SCRATCH_ADDR + 8).value();
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 39, {device, surface}), D3D_OK);
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                         {device, 64, 64, 1, 0x2u /* D3DUSAGE_DEPTHSTENCIL */, format,
                          D3DPOOL_MANAGED, static_cast<u32>(SCRATCH_ADDR), 0}), D3DERR_INVALIDCALL);
    }
}

TEST_F(D3D9ManagerTest, CheckDeviceFormatFloatTextureCanBeCreated) {
    // Learn: CheckDeviceFormat and CreateTexture must agree. Advertising
    // D3DFMT_A16B16G16R16F then returning NOTAVAILABLE with *ppTexture=0
    // leaves a guest that ignores HRESULT to call GetLOD on NULL.
    constexpr u32 D3DFMT_A16B16G16R16F = 113;
    constexpr u32 D3DRTYPE_TEXTURE = 3;
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    EXPECT_EQ(invoke_method(10, {object, 0, 1, 22, 0, D3DRTYPE_TEXTURE,
                                 D3DFMT_A16B16G16R16F}),
              D3D_OK);

    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                     {device, 8, 8, 1, 0, D3DFMT_A16B16G16R16F,
                      D3DPOOL_DEFAULT, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 12, {texture}), 0u);
}

TEST_F(D3D9ManagerTest, CheckDeviceFormatBumpAndPackedTexturesCanBeCreated) {
    // Same Check/Create agreement for formats that were once advertised
    // while d3d9_backend_format still returned Unknown (V8U8, Q8W8V8U8,
    // A2B10G10R10). Learn D3DFORMAT + CheckDeviceFormat / CreateTexture.
    constexpr u32 D3DFMT_A2B10G10R10 = 31;
    constexpr u32 D3DFMT_V8U8 = 60;
    constexpr u32 D3DFMT_X8L8V8U8 = 62;
    constexpr u32 D3DFMT_Q8W8V8U8 = 63;
    constexpr u32 D3DFMT_V16U16 = 64;
    constexpr u32 D3DFMT_CxV8U8 = 117;
    constexpr u32 D3DRTYPE_TEXTURE = 3;
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    for (u32 format : {D3DFMT_A2B10G10R10, D3DFMT_V8U8,
                       D3DFMT_X8L8V8U8, D3DFMT_Q8W8V8U8, D3DFMT_V16U16,
                       D3DFMT_CxV8U8}) {
        EXPECT_EQ(invoke_method(10, {object, 0, 1, 22, 0, D3DRTYPE_TEXTURE,
                                     format}),
                  D3D_OK)
            << "CheckDeviceFormat format=" << format;
        ASSERT_TRUE(memory_.write_u32(SCRATCH_ADDR, 0xDEADBEEFu).is_ok());
        EXPECT_EQ(invoke(D3D9InterfaceId::Device, 23,
                         {device, 8, 8, 1, 0, format, D3DPOOL_DEFAULT,
                          static_cast<u32>(SCRATCH_ADDR), 0}),
                  D3D_OK)
            << "CreateTexture format=" << format;
        const u32 texture = memory_.read_u32(SCRATCH_ADDR).value();
        ASSERT_NE(texture, 0u) << "CreateTexture format=" << format;
        EXPECT_EQ(invoke(D3D9InterfaceId::Texture, 12, {texture}), 0u);
    }
}

TEST_F(D3D9ManagerTest, CreateDeviceRejectsMissingVertexProcessingMode) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    write_present_parameters(640, 480);
    ASSERT_TRUE(memory_.write_u32(DEVICE_OUT_ADDR, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(
        invoke_method(16, {object, 0, 1, TEST_HWND, 0,
                           static_cast<u32>(PRESENT_ADDR),
                           static_cast<u32>(DEVICE_OUT_ADDR)}),
        D3DERR_INVALIDCALL);
    EXPECT_EQ(memory_.read_u32(DEVICE_OUT_ADDR).value(), 0u);
}

TEST_F(D3D9ManagerTest, CreateDeviceFailsWithoutFabricatingDeviceObject) {
    // No presentable native view for this window, so no usable device exists.
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    write_present_parameters(640, 480);
    ASSERT_TRUE(memory_.write_u32(PRESENT_ADDR + 28, 0x4242u).is_ok());
    ASSERT_TRUE(memory_.write_u32(DEVICE_OUT_ADDR, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(
        invoke_method(16, {object, 0, 1, 0x4242u, 0x40,
                           static_cast<u32>(PRESENT_ADDR),
                           static_cast<u32>(DEVICE_OUT_ADDR)}),
        D3DERR_NOTAVAILABLE);
    EXPECT_EQ(memory_.read_u32(DEVICE_OUT_ADDR).value(), 0u);
    EXPECT_EQ(backend_.record.swap_chains_created, 0u);
}

TEST_F(D3D9ManagerTest, CreateDeviceFailsWhenBackendRejectsTheSwapChain) {
    backend_.record.fail_swap_chain = true;
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    write_present_parameters(640, 480);
    ASSERT_TRUE(memory_.write_u32(DEVICE_OUT_ADDR, 0xDEADBEEFu).is_ok());
    EXPECT_EQ(
        invoke_method(16, {object, 0, 1, TEST_HWND, 0x40,
                           static_cast<u32>(PRESENT_ADDR),
                           static_cast<u32>(DEVICE_OUT_ADDR)}),
        D3DERR_NOTAVAILABLE);
    EXPECT_EQ(memory_.read_u32(DEVICE_OUT_ADDR).value(), 0u);
}

// IDirect3D9::CheckDeviceFormat (slot 10) answers per usage and resource
// type, and agrees with Create*. Expected values: an RTX 4080
// (tools/oracle_probe/d3d9_format_probe32.winoracle-gpu.expected).
TEST_F(D3D9ManagerTest, CheckDeviceFormatAnswersPerUsageAndResourceType) {
    const u32 object = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(object, 0u);
    constexpr u32 X8R8G8B8 = 22, A8R8G8B8 = 21, A1R5G5B5 = 25, A4R4G4B4 = 26,
                  L8 = 50, V8U8 = 60, D24S8 = 75, D16_LOCKABLE = 70;
    constexpr u32 DXT1 = 0x31545844u, INTZ = 0x5A544E49u;
    constexpr u32 RT = 0x1, DS = 0x2, AUTOGEN = 0x400;
    constexpr u32 SURFACE = 1, TEXTURE = 3, VOLUMETEXTURE = 4, CUBE = 5;
    const auto check = [&](u32 usage, u32 rtype, u32 format) {
        return invoke_method(10, {object, 0, 1, X8R8G8B8, usage, rtype, format});
    };
    EXPECT_EQ(check(RT, SURFACE, A8R8G8B8), D3D_OK);
    EXPECT_EQ(check(RT, TEXTURE, DXT1), D3DERR_NOTAVAILABLE);
    EXPECT_EQ(check(RT, SURFACE, A4R4G4B4), D3DERR_NOTAVAILABLE);
    EXPECT_EQ(check(0, SURFACE, DXT1), D3DERR_NOTAVAILABLE);   // no DXT offscreen
    EXPECT_EQ(check(0, SURFACE, L8), D3DERR_NOTAVAILABLE);
    EXPECT_EQ(check(0, CUBE, V8U8), D3DERR_NOTAVAILABLE);
    EXPECT_EQ(check(0, VOLUMETEXTURE, DXT1), D3D_OK);
    EXPECT_EQ(check(DS, SURFACE, D24S8), D3D_OK);
    EXPECT_EQ(check(DS, TEXTURE, D24S8), D3D_OK);              // shadow maps
    EXPECT_EQ(check(DS, SURFACE, D16_LOCKABLE), D3DERR_NOTAVAILABLE);
    EXPECT_EQ(check(0, CUBE, D24S8), D3DERR_NOTAVAILABLE);
    // DEPTHSTENCIL with a colour format, including the runtime's FOURCCs, is
    // an invalid call; a vendor FOURCC is just unsupported here.
    EXPECT_EQ(check(DS, SURFACE, X8R8G8B8), D3DERR_INVALIDCALL);
    EXPECT_EQ(check(DS, TEXTURE, DXT1), D3DERR_INVALIDCALL);
    EXPECT_EQ(check(DS, TEXTURE, INTZ), D3DERR_NOTAVAILABLE);
    // AUTOGENMIPMAP on a format without automatic mipmaps is D3DOK_NOAUTOGEN.
    EXPECT_EQ(check(AUTOGEN, TEXTURE, A8R8G8B8), D3D_OK);
    EXPECT_EQ(check(AUTOGEN, TEXTURE, A1R5G5B5), D3DOK_NOAUTOGEN);
    EXPECT_EQ(check(AUTOGEN, TEXTURE, DXT1), D3DOK_NOAUTOGEN);
}

TEST_F(D3D9ManagerTest, CreateRefusesWhatCheckDeviceFormatRefuses) {
    const auto [factory, device] = create_device();
    constexpr u32 X8R8G8B8 = 22, D24S8 = 75, DXT1 = 0x31545844u, L8 = 50;
    // CreateDepthStencilSurface (29) with a colour format.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 29,
                     {device, 16, 16, X8R8G8B8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 29,
                     {device, 16, 16, D24S8, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    // CreateRenderTarget (28) with DXT1.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 16, 16, DXT1, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3DERR_INVALIDCALL);
    // CreateOffscreenPlainSurface (36): L8 refuses in DEFAULT, and SCRATCH
    // is not limited by device format support.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 16, 16, L8, D3DPOOL_DEFAULT,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 16, 16, L8, D3DPOOL_SCRATCH,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
}
