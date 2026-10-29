// SPDX-License-Identifier: LGPL-2.1-only
//
// Direct3D 10 device: parameter rules, resources, views, stream output.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_d3d10.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D10DeviceTest, CreateDeviceParameterRulesAsMeasured) {
    // [create]: the SDK version (29 / 0x20), a NULL ppDevice, the layer
    // flags, the driver types and D3D10_FEATURE_LEVEL1.
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 0, 0, 0, 28, OUT}),
              kEInvalidArg);
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 0, 0, 0, 0x20, OUT}),
              kEInvalidArg);
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 0, 0, 0, 29, 0}),
              kEInvalidArg);
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice1,
                     {0, 0, 0, 0, 0xA100, 29, OUT}),
              kEInvalidArg);
    for (const u32 layer : {0x2u, 0x4u, 0x400u}) {
        EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                         {0, 0, 0, layer, 29, OUT}),
                  0x887A002Du)
            << layer;
    }
    // REFERENCE: no reference rasterizer installed (x64 Windows).
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 1, 0, 0, 29, OUT}),
              0x887A0004u);
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice1,
                     {0, 1, 0, 0, 0xA100, 0x20, OUT}),
              kENoInterface);
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 3, 0, 0, 29, OUT}),
              kEInvalidArg);  // SOFTWARE with no module
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 4, 0, 0, 29, OUT}),
              kEInvalidArg);
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 0, BASE, 0, 29, OUT}),
              kEInvalidArg);  // HARDWARE with a software module
    for (const u32 level : {0xB000u, 0xB100u, 0x9000u, 0x1234u, 0u}) {
        ASSERT_TRUE(memory.write_u64(OUT, 0x5A5A5A50ull).is_ok());
        EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice1,
                         {0, 0, 0, 0, level, 0x20, OUT}),
                  kEInvalidArg)
            << level;
        EXPECT_EQ(pointer(OUT), 0u) << "the out-parameter is NULLed";
    }
    // NULL is a device of its own (measured S_OK).
    EXPECT_EQ(module(&D3D11Manager::impl_D3D10CreateDevice,
                     {0, 2, 0, 0, 29, OUT}),
              kSOk);
    EXPECT_EQ(release(pointer(OUT)), 0u);

    // GetCreationFlags: SINGLETHREADED, PREVENT_INTERNAL_THREADING_OPTIMIZ-
    // ATIONS and PREVENT_ALTERING_LAYER_SETTINGS stay; BGRA_SUPPORT only
    // from D3D10CreateDevice1; undefined bits are dropped.
    const GuestAddr d10 = create10(false, 0x1 | 0x8 | 0x20 | 0x80 | 0x1000);
    EXPECT_EQ(vcall(d10, kD3D10GetCreationFlags, {}), 0x89u);
    EXPECT_EQ(vcall(d10, kD3D10GetFeatureLevel, {}), 0xA000u);
    EXPECT_EQ(release(d10), 0u);
    const GuestAddr d101 = create10(true, 0x20, 0x9300);
    EXPECT_EQ(vcall(d101, kD3D10GetCreationFlags, {}), 0x20u);
    EXPECT_EQ(vcall(d101, kD3D10GetFeatureLevel, {}), 0x9300u);
    EXPECT_EQ(release(d101), 0u);
}

TEST_F(D3D10DeviceTest, DeviceIsAD3D11DeviceWithAD3D10Facet) {
    const GuestAddr device = create10(true);
    ASSERT_NE(device, 0u);
    // Every slot of the 101-slot vtable is a thunk.
    const GuestAddr vtable = pointer(device);
    for (usize slot = 0; slot < d3d10::DEVICE_METHOD_COUNT; ++slot) {
        EXPECT_NE(pointer(vtable + slot * 8), 0u) << "slot " << slot;
    }
    // [qi] device10: the D3D11 device's graph plus the D3D10 interfaces,
    // one count ("device count=1", "QI(ID3D11Device) ... count=2").
    EXPECT_EQ(qi(device, winapi::IID_ID3D10DEVICE), device);
    EXPECT_EQ(qi(device, winapi::IID_ID3D10DEVICE1), device);
    EXPECT_EQ(release(device), 2u);
    EXPECT_EQ(release(device), 1u);
    const GuestAddr d11 = qi(device, winapi::IID_ID3D11DEVICE);
    ASSERT_NE(d11, 0u);
    EXPECT_NE(d11, device);
    EXPECT_EQ(qi(d11, winapi::IID_ID3D10DEVICE1), device);
    EXPECT_EQ(release(d11), 2u);
    const GuestAddr dxgi_device = qi(device, winapi::IID_IDXGIDEVICE);
    ASSERT_NE(dxgi_device, 0u);
    EXPECT_EQ(qi(dxgi_device, winapi::IID_ID3D10DEVICE), device);
    EXPECT_EQ(release(dxgi_device), 3u);
    EXPECT_EQ(release(dxgi_device), 2u);
    EXPECT_NE(qi(device, winapi::IID_ID3D11DEVICE5), 0u);
    EXPECT_EQ(release(device), 2u);
    // A D3D10 device is thread-safe from creation:
    // "ID3D10Multithread GetMultithreadProtected=1".
    const GuestAddr multithread = qi(device, winapi::IID_ID3D10MULTITHREAD);
    ASSERT_NE(multithread, 0u);
    EXPECT_EQ(vcall(multithread, 6, {}), 1u);
    EXPECT_EQ(release(multithread), 2u);
    EXPECT_EQ(release(d11), 1u);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, ImmediateContextIsOffWhileTheD3D10StateRuns) {
    const GuestAddr device = create10(false);
    const GuestAddr d11 = qi(device, winapi::IID_ID3D11DEVICE);
    ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
    vcall(d11, 40, {OUT});  // GetImmediateContext
    const GuestAddr context = pointer(OUT);
    ASSERT_NE(context, 0u);
    // "D3D10 device: D3D11 context IAGetPrimitiveTopology after D3D10 set
    // LINESTRIP=0": the D3D10 state is the context's, and D3D11's getters
    // read an empty one; its setters change nothing.
    vcall(device, kD3D10IASetPrimitiveTopology, {3});
    ASSERT_TRUE(memory.write_u32(GET, 99).is_ok());
    vcall(device, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 3u);
    ASSERT_TRUE(memory.write_u32(GET, 99).is_ok());
    vcall(context, 83, {GET});
    EXPECT_EQ(word(GET), 0u);
    vcall(context, 24, {1});
    vcall(device, kD3D10IAGetPrimitiveTopology, {GET});
    EXPECT_EQ(word(GET), 3u);
    // A disabled getter reports the empty state: "OMGetBlendState=NULL
    // factor=1,1,1,1 mask=0xffffffff RSGetViewports n=0".
    ASSERT_TRUE(memory.write_u64(PTRS, 0x5A5A5A50ull).is_ok());
    words(U32S, {7, 7, 7, 7, 7});
    vcall(context, 91, {PTRS, U32S, U32S + 16});
    EXPECT_EQ(pointer(PTRS), 0u);
    EXPECT_EQ(word(U32S), 0x3F800000u);
    EXPECT_EQ(word(U32S + 16), 0xFFFFFFFFu);
    // Map / Unmap / Begin / End / GetData stay on (Learn's table).
    const GuestAddr staging = buffer10(device, 16, 3, 0, 0x30000);
    const GuestAddr staging11 = qi(staging, winapi::IID_ID3D11BUFFER);
    ASSERT_NE(staging11, 0u);
    EXPECT_EQ(vcall(context, 14, {staging11, 0, 1, 0, GET}), kSOk);
    vcall(context, 15, {staging11, 0});
    release(staging11);
    release(staging);
    release(context);
    release(d11);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, BuffersTranslateDescriptionsAndMapOnTheResource) {
    const GuestAddr device = create10(false);
    // [obj] "CreateBuffer(misc shared) ... desc={64,0,0x1,0x0,0x2}
    // desc11={64,0,0x1,0x0,0x2,0}".
    const GuestAddr shared = buffer10(device, 64, 0, 0x1, 0, 0x2);
    ASSERT_NE(shared, 0u);
    std::array<u32, 5> desc10{};
    ASSERT_TRUE(memory.write_u32(GET + 20, 0xCCCCCCCCu).is_ok());
    vcall(shared, 12, {GET});
    ASSERT_TRUE(memory.read_bytes(GET, reinterpret_cast<u8*>(desc10.data()),
                                  20).is_ok());
    EXPECT_EQ(desc10, (std::array<u32, 5>{64, 0, 1, 0, 2}));
    EXPECT_EQ(word(GET + 20), 0xCCCCCCCCu) << "D3D10_BUFFER_DESC is 20 bytes";
    const GuestAddr shared11 = qi(shared, winapi::IID_ID3D11BUFFER);
    ASSERT_NE(shared11, 0u);
    vcall(shared11, 10, {GET});
    EXPECT_EQ(word(GET + 16), 2u);
    EXPECT_EQ(word(GET + 20), 0u) << "StructureByteStride";
    EXPECT_EQ(qi(shared11, winapi::IID_ID3D10BUFFER), shared);
    release(shared);
    EXPECT_NE(qi(shared, winapi::IID_ID3D10RESOURCE), 0u);
    release(shared);
    EXPECT_EQ(qi(shared, winapi::IID_ID3D10TEXTURE2D), 0u);
    // GetDevice: the D3D10 device.
    ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
    vcall(shared, 3, {OUT});
    EXPECT_EQ(pointer(OUT), device);
    release(device);
    // SHARED_KEYEDMUTEX (0x10) and every bit D3D10 does not define: refused
    // with the out-parameter NULLed; D3D11's UAV bind too.
    u64 hr = 0;
    for (const u32 misc : {0x8u, 0x10u, 0x20u, 0x40u}) {
        EXPECT_EQ(buffer10(device, 64, 0, 0x8, 0, misc, &hr), 0u) << misc;
        EXPECT_EQ(hr, kEInvalidArg) << misc;
    }
    EXPECT_EQ(buffer10(device, 64, 0, 0x80, 0, 0, &hr), 0u);
    EXPECT_EQ(hr, kEInvalidArg);
    // Map on the resource: [map] "dynamic vb WRITE_DISCARD hr=0 out=obj",
    // "dynamic vb WRITE hr=0x80070057 out=NULL", "default vb ... NULL".
    const GuestAddr dynamic = buffer10(device, 64, 2, 0x1, 0x10000);
    ASSERT_NE(dynamic, 0u);
    ASSERT_TRUE(memory.write_u64(OUT, 0x5A5A5A50ull).is_ok());
    EXPECT_EQ(vcall(dynamic, 10, {4, 0, OUT}), kSOk);
    const GuestAddr mapped = pointer(OUT);
    EXPECT_NE(mapped, 0u);
    EXPECT_NE(mapped, 0x5A5A5A50ull);
    vcall(dynamic, 11, {});
    ASSERT_TRUE(memory.write_u64(OUT, 0x5A5A5A50ull).is_ok());
    EXPECT_EQ(vcall(dynamic, 10, {2, 0, OUT}), kEInvalidArg);
    EXPECT_EQ(pointer(OUT), 0u);
    EXPECT_EQ(vcall(shared, 10, {4, 0, OUT}), kEInvalidArg);
    EXPECT_EQ(release(dynamic), 0u);
    EXPECT_EQ(release(shared11), 1u);
    EXPECT_EQ(release(shared), 0u);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, TexturesRenumberMiscFlagsAndFollowTheFeatureLevel) {
    const GuestAddr d101 = create10(true);
    // D3D10 GDI_COMPATIBLE (0x20) is D3D11's 0x200: "bgra8 gdi 0x20 ...
    // misc=0x20 misc11=0x200"; SHARED_KEYEDMUTEX (0x10) is refused.
    u64 hr = 0;
    const GuestAddr gdi = texture10(d101, 4, 1, 87, 0x28, 0x20, &hr);
    ASSERT_EQ(hr, kSOk);
    vcall(gdi, 12, {GET});
    EXPECT_EQ(word(GET + 40), 0x20u);
    const GuestAddr gdi11 = qi(gdi, winapi::IID_ID3D11TEXTURE2D);
    vcall(gdi11, 10, {GET});
    EXPECT_EQ(word(GET + 40), 0x200u);
    release(gdi11);
    release(gdi);
    EXPECT_EQ(texture10(d101, 4, 1, 87, 0x28, 0x10, &hr), 0u);
    EXPECT_EQ(hr, kEInvalidArg);
    // Cube arrays from 10_1: 12 faces on 10_1 S_OK, on 10_0 E_INVALIDARG.
    const GuestAddr cubes = texture10(d101, 8, 12, 28, 0x8, 0x4, &hr);
    EXPECT_EQ(hr, kSOk);
    release(cubes);
    const GuestAddr d100 = create10(true, 0, 0xA000);
    EXPECT_EQ(texture10(d100, 8, 12, 28, 0x8, 0x4, &hr), 0u);
    EXPECT_EQ(hr, kEInvalidArg);
    // 512 slices at 10_x, not 513.
    const GuestAddr slices = texture10(d100, 4, 512, 61, 0x8, 0, &hr);
    EXPECT_EQ(hr, kSOk);
    release(slices);
    EXPECT_EQ(texture10(d100, 4, 513, 61, 0x8, 0, &hr), 0u);
    EXPECT_EQ(hr, kEInvalidArg);
    // The 10.1 API has BGRA at 10_0; the 10.0 API (D3D10CreateDevice) has
    // only the DXGI 1.0 formats, whatever the flags.
    const GuestAddr bgra = texture10(d100, 4, 1, 87, 0x8, 0, &hr);
    EXPECT_EQ(hr, kSOk);
    release(bgra);
    const GuestAddr d10 = create10(false, 0x20);
    EXPECT_EQ(texture10(d10, 4, 1, 87, 0x8, 0, &hr), 0u);
    EXPECT_EQ(hr, kEInvalidArg);
    ASSERT_TRUE(memory.write_u32(GET, 0xCCCCCCCCu).is_ok());
    EXPECT_EQ(vcall(d10, 90, {85, GET}), kEFail);  // B5G6R5
    EXPECT_EQ(word(GET), 0u);
    // R8G8B8A8 loses D3D11's BACK_BUFFER_CAST on the 10.0 API only
    // ("[fmt] 28 d3d10 ... 0x007ef3f3", "10_1 device ... 0x017ef3f3").
    EXPECT_EQ(vcall(d10, 90, {28, GET}), kSOk);
    EXPECT_EQ(word(GET) & 0xFF000000u, 0u);
    EXPECT_EQ(vcall(d101, 90, {28, GET}), kSOk);
    EXPECT_EQ(word(GET) & 0x01000000u, 0x01000000u);
    // Texture3D / 1D answer their own dimension interface.
    words(DESC, {8, 8, 4, 1, 28, 0, 0x28, 0, 0});
    ASSERT_EQ(vcall(d10, 74, {DESC, 0, OBJ_OUT}), kSOk);
    const GuestAddr volume = pointer(OBJ_OUT);
    EXPECT_NE(qi(volume, winapi::IID_ID3D10TEXTURE3D), 0u);
    release(volume);
    EXPECT_EQ(qi(volume, winapi::IID_ID3D10TEXTURE2D), 0u);
    EXPECT_EQ(release(volume), 0u);
    EXPECT_EQ(release(d10), 0u);
    EXPECT_EQ(release(d100), 0u);
    EXPECT_EQ(release(d101), 0u);
}

TEST_F(D3D10DeviceTest, BlendDescriptionsTranslateAsWindowsReportsThem) {
    const GuestAddr device = create10(false);
    // [obj] the probe's D3D10_BLEND_DESC: RT0 and RT2 enabled, SRC_ALPHA /
    // INV_SRC_ALPHA / ADD, ONE / ZERO / ADD for alpha, masks f 3 f f f f f f.
    std::array<u8, 68> desc{};
    const auto put = [&](usize at, u32 value) {
        std::memcpy(desc.data() + at, &value, 4);
    };
    put(4 + 0 * 4, 1);
    put(4 + 2 * 4, 1);
    put(36, 5);
    put(40, 6);
    put(44, 1);
    put(48, 2);
    put(52, 1);
    put(56, 1);
    for (usize rt = 0; rt < 8; ++rt) desc[60 + rt] = rt == 1 ? 0x3 : 0xF;
    ASSERT_TRUE(memory.write_bytes(DESC, desc.data(), desc.size()).is_ok());
    ASSERT_EQ(vcall(device, kD3D10CreateBlendState, {DESC, OBJ_OUT}), kSOk);
    const GuestAddr blend = pointer(OBJ_OUT);
    // "blend GetDesc a2c=0 enable=10100000 src=5 dst=6 op=1 srcA=2 dstA=1
    // opA=1 mask=f3ffffff".
    std::array<u8, 68> back{};
    vcall(blend, 7, {GET});
    ASSERT_TRUE(memory.read_bytes(GET, back.data(), back.size()).is_ok());
    EXPECT_EQ(back, desc);
    // "blend 11 desc independent=1 rt0={1 5 6 1 2 1 1 f} rt1={0 3}
    // rt2={1 5 f} rt7={0 2 f}".
    // ID3D11BlendState {75b68faa-347d-4159-8f45-a0640f01cd9a} (d3d11.h).
    constexpr GuidBytes kIidD3D11BlendState{
        0xAA, 0x8F, 0xB6, 0x75, 0x7D, 0x34, 0x59, 0x41,
        0x8F, 0x45, 0xA0, 0x64, 0x0F, 0x01, 0xCD, 0x9A};
    const GuestAddr blend11 = qi(blend, kIidD3D11BlendState);
    ASSERT_NE(blend11, 0u);
    vcall(blend11, 7, {GET});
    EXPECT_EQ(word(GET + 4), 1u);
    const auto rt = [&](usize index, usize field) {
        return word(GET + 8 + index * 32 + field * 4);
    };
    EXPECT_EQ(rt(0, 0), 1u);
    EXPECT_EQ(rt(0, 1), 5u);
    EXPECT_EQ(rt(0, 2), 6u);
    EXPECT_EQ(rt(1, 0), 0u);
    EXPECT_EQ(rt(1, 7) & 0xFFu, 0x3u);
    EXPECT_EQ(rt(2, 1), 5u);
    EXPECT_EQ(rt(7, 0), 0u);
    EXPECT_EQ(rt(7, 1), 2u);
    release(blend11);
    // "blend same desc -> same object=1".
    ASSERT_EQ(vcall(device, kD3D10CreateBlendState, {DESC, OBJ_OUT}), kSOk);
    EXPECT_EQ(pointer(OBJ_OUT), blend);
    release(blend);
    EXPECT_NE(qi(blend, winapi::IID_ID3D10BLENDSTATE1), 0u);
    release(blend);
    EXPECT_EQ(release(blend), 0u);
    // "disabled blend GetDesc src=2 dst=1; 11 desc independent=0".
    for (usize i = 0; i < 8; ++i) put(4 + i * 4, 0);
    ASSERT_TRUE(memory.write_bytes(DESC, desc.data(), desc.size()).is_ok());
    ASSERT_EQ(vcall(device, kD3D10CreateBlendState, {DESC, OBJ_OUT}), kSOk);
    const GuestAddr disabled = pointer(OBJ_OUT);
    vcall(disabled, 7, {GET});
    EXPECT_EQ(word(GET + 36), 2u);
    EXPECT_EQ(word(GET + 40), 1u);
    release(disabled);
    // A refused description leaves *ppBlendState untouched ("CreateBlend-
    // State(src 0, RT1 enabled) hr=0x80070057 out=untouched"); so does NULL.
    put(4 + 1 * 4, 1);
    put(36, 0);
    ASSERT_TRUE(memory.write_bytes(DESC, desc.data(), desc.size()).is_ok());
    ASSERT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
    EXPECT_EQ(vcall(device, kD3D10CreateBlendState, {DESC, OBJ_OUT}),
              kEInvalidArg);
    EXPECT_EQ(pointer(OBJ_OUT), 0x5A5A5A50ull);
    EXPECT_EQ(vcall(device, kD3D10CreateBlendState, {0, OBJ_OUT}),
              kEInvalidArg);
    EXPECT_EQ(pointer(OBJ_OUT), 0x5A5A5A50ull);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, DepthStencilViewsAndIntegerViewports) {
    const GuestAddr device = create10(false);
    u64 hr = 0;
    const GuestAddr depth = texture10(device, 16, 1, 45, 0x40, 0, &hr);
    ASSERT_EQ(hr, kSOk);
    // D3D10_DEPTH_STENCIL_VIEW_DESC has no Flags word: {D24S8, TEXTURE2D,
    // MipSlice 1} is "DSV mip 1 of 1 hr=0x80070057", MipSlice 0 succeeds.
    words(DESC, {45, 3, 1});
    ASSERT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
    EXPECT_EQ(vcall(device, kD3D10CreateDepthStencilView,
                    {depth, DESC, OBJ_OUT}),
              kEInvalidArg);
    EXPECT_EQ(pointer(OBJ_OUT), 0u);
    words(DESC, {45, 3, 0});
    ASSERT_EQ(vcall(device, kD3D10CreateDepthStencilView,
                    {depth, DESC, OBJ_OUT}),
              kSOk);
    const GuestAddr dsv = pointer(OBJ_OUT);
    words(GET, {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC});
    vcall(dsv, 8, {GET});
    EXPECT_EQ(word(GET), 45u);
    EXPECT_EQ(word(GET + 4), 3u);
    EXPECT_EQ(word(GET + 8), 0u);
    EXPECT_EQ(word(GET + 20), 0xCCu) << "20 bytes, not D3D11's 24";
    const GuestAddr dsv11 = qi(dsv, winapi::IID_ID3D11DEPTHSTENCILVIEW);
    vcall(dsv11, 8, {GET});
    EXPECT_EQ(word(GET + 8), 0u) << "D3D11 Flags";
    release(dsv11);
    // GetResource hands out the D3D10 facet of the texture.
    ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
    vcall(dsv, 7, {OUT});
    EXPECT_EQ(pointer(OUT), depth);
    release(depth);
    // ClearDepthStencilView carries its float Depth in XMM3.
    const float quarter = 0.25f;
    u32 bits = 0;
    std::memcpy(&bits, &quarter, 4);
    EXPECT_EQ(vcall(device, kD3D10ClearDepthStencilView, {dsv, 1, bits, 0}),
              0u);
    // [getset] "RSGetViewports count=2 n3=2 vp0={1 2 30 40 0.25 0.75}
    // vp1={-5 -6 7 8} vp2.x=0x0".
    const float depths[4] = {0.25f, 0.75f, 0.0f, 1.0f};
    words(DESC, {1, 2, 30, 40});
    ASSERT_TRUE(memory.write_bytes(DESC + 16, reinterpret_cast<const u8*>(depths),
                                   8).is_ok());
    words(DESC + 24, {static_cast<u32>(-5), static_cast<u32>(-6), 7, 8});
    ASSERT_TRUE(memory.write_bytes(DESC + 40,
                                   reinterpret_cast<const u8*>(depths + 2),
                                   8).is_ok());
    vcall(device, kD3D10RSSetViewports, {2, DESC});
    words(U32S, {3});
    for (usize i = 0; i < 18; ++i) {
        ASSERT_TRUE(memory.write_u32(GET + i * 4, 0xCCCCCCCCu).is_ok());
    }
    vcall(device, kD3D10RSGetViewports, {U32S, GET});
    EXPECT_EQ(word(U32S), 2u);
    EXPECT_EQ(word(GET), 1u);
    EXPECT_EQ(word(GET + 4), 2u);
    EXPECT_EQ(word(GET + 8), 30u);
    EXPECT_EQ(word(GET + 12), 40u);
    EXPECT_EQ(word(GET + 16), 0x3E800000u);  // 0.25f
    EXPECT_EQ(static_cast<i32>(word(GET + 24)), -5);
    EXPECT_EQ(static_cast<i32>(word(GET + 28)), -6);
    EXPECT_EQ(word(GET + 48), 0u);
    EXPECT_EQ(release(dsv), 0u);
    EXPECT_EQ(release(depth), 0u);
    EXPECT_EQ(release(device), 0u);
}

TEST_F(D3D10DeviceTest, StreamOutputTextFilterAndShaderRules) {
    const GuestAddr device = create10(false);
    // [getset] "SOGetTargets slot0 same=1 offset=4294967295 slot1=NULL
    // offset=4294967295 slot3 offset=0": D3D11 keeps no offsets, D3D10
    // answers ~0 for the slots the last SOSetTargets covered.
    const GuestAddr so = buffer10(device, 64, 0, 0x10);
    ASSERT_NE(so, 0u);
    ASSERT_TRUE(memory.write_u64(PTRS, so).is_ok());
    ASSERT_TRUE(memory.write_u64(PTRS + 8, 0).is_ok());
    words(U32S, {16, 0xFFFFFFFFu});
    vcall(device, kD3D10SOSetTargets, {2, PTRS, U32S});
    words(GET + 0x100, {77, 77, 77, 77});
    vcall(device, kD3D10SOGetTargets, {4, GET, GET + 0x100});
    EXPECT_EQ(pointer(GET), so);
    EXPECT_EQ(pointer(GET + 8), 0u);
    EXPECT_EQ(word(GET + 0x100), 0xFFFFFFFFu);
    EXPECT_EQ(word(GET + 0x104), 0xFFFFFFFFu);
    EXPECT_EQ(word(GET + 0x10C), 0u);
    release(so);
    vcall(device, kD3D10ClearState, {});
    vcall(device, kD3D10SOGetTargets, {4, GET, GET + 0x100});
    EXPECT_EQ(word(GET + 0x100), 0u);
    // "text filter size after set 7x3=0x0".
    vcall(device, kD3D10SetTextFilterSize, {7, 3});
    words(U32S, {9, 9});
    vcall(device, kD3D10GetTextFilterSize, {U32S, U32S + 4});
    EXPECT_EQ(word(U32S), 0u);
    EXPECT_EQ(word(U32S + 4), 0u);
    EXPECT_EQ(release(so), 0u);

    // Shader bytecode against the stage and the level: "10_0
    // CreateVertexShader(vs_4_1) / (vs_5_0) / (ps bytecode) / (truncated)
    // hr=0x80070057 out=NULL", vs_4_0_level_9_3 and vs_4_1 at 10_1 S_OK.
    namespace sh = d3d10_blobs;
    u64 hr = 0;
    EXPECT_EQ(shader10(device, kD3D10CreateVertexShader, sh::g_vs41,
                       sizeof(sh::g_vs41), &hr),
              0u);
    EXPECT_EQ(hr, kEInvalidArg);
    EXPECT_EQ(shader10(device, kD3D10CreateVertexShader, sh::g_vs50,
                       sizeof(sh::g_vs50), &hr),
              0u);
    EXPECT_EQ(hr, kEInvalidArg);
    EXPECT_EQ(shader10(device, kD3D10CreateVertexShader, sh::g_ps40,
                       sizeof(sh::g_ps40), &hr),
              0u);
    EXPECT_EQ(hr, kEInvalidArg);
    EXPECT_EQ(shader10(device, kD3D10CreateVertexShader, sh::g_vs40, 16, &hr),
              0u);
    EXPECT_EQ(hr, kEInvalidArg);
    const GuestAddr level93 =
        shader10(device, kD3D10CreateVertexShader, sh::g_vs40l93,
                 sizeof(sh::g_vs40l93), &hr);
    EXPECT_EQ(hr, kSOk);
    release(level93);
    const GuestAddr vs = shader10(device, kD3D10CreateVertexShader, sh::g_vs40,
                                  sizeof(sh::g_vs40), &hr);
    ASSERT_EQ(hr, kSOk);
    EXPECT_NE(qi(vs, winapi::IID_ID3D10VERTEXSHADER), 0u);
    release(vs);
    EXPECT_EQ(qi(vs, winapi::IID_ID3D10PIXELSHADER), 0u);
    vcall(device, kD3D10VSSetShader, {vs});
    ASSERT_TRUE(memory.write_u64(OUT, 0).is_ok());
    vcall(device, kD3D10VSGetShader, {OUT});
    EXPECT_EQ(pointer(OUT), vs);
    release(vs);
    vcall(device, kD3D10ClearState, {});
    EXPECT_EQ(release(vs), 0u);
    const GuestAddr d101 = create10(true);
    const GuestAddr vs41 = shader10(d101, kD3D10CreateVertexShader,
                                    sh::g_vs41, sizeof(sh::g_vs41), &hr);
    EXPECT_EQ(hr, kSOk);
    release(vs41);
    release(d101);

    // CreateGeometryShaderWithStreamOutput: D3D10 entries (no Stream), one
    // stride for slot 0; several slots each take one element.
    // x64 D3D10_SO_DECLARATION_ENTRY: name(8) index(4) start count slot pad.
    ASSERT_TRUE(memory.write_bytes(NAMES, reinterpret_cast<const u8*>(
                                              "SV_Position\0COLOR"),
                                   18).is_ok());
    ASSERT_TRUE(memory.write_bytes(CODE, sh::g_gsso40, sizeof(sh::g_gsso40))
                    .is_ok());
    const auto so_entry = [&](usize index, GuestAddr name, u8 start,
                              u8 count, u8 slot) {
        const GuestAddr at = DESC + index * 16;
        ASSERT_TRUE(memory.write_u64(at, name).is_ok());
        ASSERT_TRUE(memory.write_u32(at + 8, 0).is_ok());
        const std::array<u8, 4> tail{start, count, slot, 0};
        ASSERT_TRUE(memory.write_bytes(at + 12, tail.data(), 4).is_ok());
    };
    so_entry(0, NAMES, 0, 4, 0);
    so_entry(1, NAMES + 12, 0, 4, 0);
    const auto create_so = [&](u32 entries, u32 stride) {
        ASSERT_TRUE(memory.write_u64(OBJ_OUT, 0x5A5A5A50ull).is_ok());
        hr = vcall(device, kD3D10CreateGeometryShaderWithStreamOutput,
                   {CODE, sizeof(sh::g_gsso40), DESC, entries, stride,
                    OBJ_OUT});
    };
    create_so(2, 32);
    EXPECT_EQ(hr, kSOk);  // "CreateGeometryShaderWithStreamOutput hr=0"
    const GuestAddr gso = pointer(OBJ_OUT);
    EXPECT_NE(qi(gso, winapi::IID_ID3D10GEOMETRYSHADER), 0u);
    release(gso);
    release(gso);
    create_so(2, 28);
    EXPECT_EQ(hr, kEInvalidArg);  // "(stride 28 < 32) hr=0x80070057"
    so_entry(1, NAMES + 12, 0, 4, 1);
    create_so(2, 0);
    EXPECT_EQ(hr, kSOk);  // "(two slots, stride 0) hr=0"
    release(pointer(OBJ_OUT));
    so_entry(1, NAMES + 12, 0, 2, 1);
    so_entry(2, NAMES + 12, 2, 2, 1);
    create_so(3, 0);
    EXPECT_EQ(hr, kEInvalidArg);  // "(slot 1 has two entries)"
    EXPECT_EQ(release(device), 0u);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
