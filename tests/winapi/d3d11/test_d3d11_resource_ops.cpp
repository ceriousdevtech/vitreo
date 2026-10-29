// SPDX-License-Identifier: LGPL-2.1-only
//
// Texel encode conversions and resource operations (clears, copies, updates, maps).
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_surface.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// ---------------------------------------------------------------------------
// Area 06: resource operations on the context. Every expected value is a line
// of tools/oracle_probe/d3d11_a06_resource_ops_probe.expected (Windows,
// WARP) or of Learn / the D3D11.3 functional spec it names.
// ---------------------------------------------------------------------------


/// A texel as the a06 probe prints it: bytes in memory order, a space after
/// every four.
std::string probe_hex(const winapi::d3d11_texel::TexelBytes& texel, u32 bytes) {
    std::string out;
    char digits[3];
    for (u32 i = 0; i < bytes; ++i) {
        std::snprintf(digits, sizeof(digits), "%02x", texel[i]);
        out += digits;
        if ((i % 4u) == 3u && i + 1 < bytes) out += ' ';
    }
    return out;
}

std::string probe_hex(const std::vector<u8>& bytes) {
    winapi::d3d11_texel::TexelBytes texel{};
    std::copy_n(bytes.begin(), std::min<usize>(bytes.size(), 16), texel.begin());
    return probe_hex(texel, static_cast<u32>(std::min<usize>(bytes.size(), 16)));
}

std::string encode_uint_hex(u32 format, const std::array<u32, 4>& values) {
    const auto* layout = winapi::d3d11_texel::layout_for(format);
    EXPECT_NE(layout, nullptr) << "format " << format;
    if (layout == nullptr) return {};
    return probe_hex(winapi::d3d11_texel::encode_uint(*layout, values),
                     layout->texel_bytes);
}

std::string encode_float_hex(u32 format, const std::array<float, 4>& color,
                             winapi::d3d11_texel::FloatClear rule) {
    const auto* layout = winapi::d3d11_texel::layout_for(format);
    EXPECT_NE(layout, nullptr) << "format " << format;
    if (layout == nullptr) return {};
    winapi::d3d11_texel::TexelBytes texel{};
    if (!winapi::d3d11_texel::encode_float(*layout, color, rule, texel)) {
        return "dropped";
    }
    return probe_hex(texel, layout->texel_bytes);
}


TEST(D3D11TexelEncodeTest, UavUintStoresTheLowBitsOfEachValue) {
    // D3D11.3 5.2.3.2; a06 probe "UAV UAV <fmt> Uint u0" lines.
    const std::array<u32, 4> u0{0x12345678u, 0x9ABCDEF0u, 0x3F800000u,
                                0xFFFFFFFFu};
    EXPECT_EQ(encode_uint_hex(2, u0), "78563412 f0debc9a 0000803f ffffffff");
    EXPECT_EQ(encode_uint_hex(10, u0), "7856f0de 0000ffff");
    EXPECT_EQ(encode_uint_hex(28, u0), "78f000ff");
    EXPECT_EQ(encode_uint_hex(24, u0), "78c20bc0");
    EXPECT_EQ(encode_uint_hex(26, u0), "78863700");
    EXPECT_EQ(encode_uint_hex(34, u0), "7856f0de");
    EXPECT_EQ(encode_uint_hex(56, u0), "7856");
    EXPECT_EQ(encode_uint_hex(61, u0), "78");
    EXPECT_EQ(encode_uint_hex(87, u0), "00f078ff") << "B8G8R8A8: R at byte 2";
    const std::array<u32, 4> u1{1, 2, 3, 4};
    EXPECT_EQ(encode_uint_hex(24, u1), "01083000");
    EXPECT_EQ(encode_uint_hex(26, u1), "0110c000");
    EXPECT_EQ(encode_uint_hex(87, u1), "03020104");
    // Typed buffer lines: "UAV buffer typed R8G8B8A8_UNORM [0,2) Uint".
    EXPECT_EQ(encode_uint_hex(28, {0x11223344u, 0x55667788u, 0x3F800000u,
                                   0x180u}),
              "44880080");
}

TEST(D3D11TexelEncodeTest, UavFloatConvertsPerTheSpecAndRefusesIntegers) {
    using winapi::d3d11_texel::FloatClear;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const std::array<float, 4> f0{0.5f, -0.25f, 1.7f, 0.3333f};
    const std::array<float, 4> f1{300.7f, -5.5f, nan, inf};
    const auto uav = FloatClear::UnorderedAccessView;
    // a06 probe "UAV UAV <fmt> Float f0/f1" lines.
    EXPECT_EQ(encode_float_hex(10, f0, uav), "003800b4 cc3e5535");
    EXPECT_EQ(encode_float_hex(10, f1, uav), "b25c80c5 007e007c");
    EXPECT_EQ(encode_float_hex(11, f0, uav), "00800000 ffff5355");
    EXPECT_EQ(encode_float_hex(11, f1, uav), "ffff0000 0000ffff");
    EXPECT_EQ(encode_float_hex(13, f0, uav), "004000e0 ff7fa92a");
    EXPECT_EQ(encode_float_hex(13, f1, uav), "ff7f0180 0000ff7f");
    EXPECT_EQ(encode_float_hex(28, f0, uav), "8000ff55");
    EXPECT_EQ(encode_float_hex(31, f1, uav), "7f81007f");
    EXPECT_EQ(encode_float_hex(24, f0, uav), "0002f07f");
    EXPECT_EQ(encode_float_hex(24, f1, uav), "ff0300c0");
    EXPECT_EQ(encode_float_hex(26, f0, uav), "8003807d");
    EXPECT_EQ(encode_float_hex(26, f1, uav), "cb05c0ff");
    EXPECT_EQ(encode_float_hex(87, f0, uav), "ff008055");
    // "On other UAVs, the operation is invalid": the probe's a5 prefill stays.
    EXPECT_EQ(encode_float_hex(42, f0, uav), "dropped");
    EXPECT_EQ(encode_float_hex(30, f0, uav), "dropped");
    EXPECT_EQ(encode_float_hex(25, f0, uav), "dropped");
}

TEST(D3D11TexelEncodeTest, RenderTargetConversionsMatchWarp) {
    using winapi::d3d11_texel::FloatClear;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    // The a06 probe's seven RTV colours.
    const std::array<std::array<float, 4>, 7> c{{
        {0.5f, -0.25f, 1.7f, 0.3333f},
        {0.2f, 0.7f, 0.0019f, 0.99f},
        {0.5f / 255.0f, 1.5f / 255.0f, 2.5f / 255.0f, 127.5f / 255.0f},
        {300.7f, -5.5f, 70000.0f, 3.0e9f},
        {4294967040.0f, -2147483648.0f, 1e20f, -1e20f},
        {nan, inf, -inf, -0.0f},
        {-1.0f, -0.999f, 1.0f / 65535.0f, 65504.0f},
    }};
    const auto rule = FloatClear::ClearView;
    // Float16: round toward zero, finite overflow saturates (3.2.2).
    const std::array<const char*, 7> half{
        "003800b4 cc3e5535", "66329939 c817eb3b", "0418061e 05210038",
        "b25c80c5 ff7bff7b", "ff7bfffb ff7bfffb", "007e007c 00fc0080",
        "00bcfdbb 0001ff7b"};
    for (usize i = 0; i < c.size(); ++i) {
        EXPECT_EQ(encode_float_hex(10, c[i], rule), half[i]) << "c" << i;
    }
    // *NORM ties go to even; sRGB per 3.2.3.8.
    EXPECT_EQ(encode_float_hex(28, c[2], rule), "00020280");
    EXPECT_EQ(encode_float_hex(11, c[2], rule), "80008201 82020080");
    EXPECT_EQ(encode_float_hex(87, c[2], rule), "02020080");
    EXPECT_EQ(encode_float_hex(13, c[6], rule), "01802280 0000ff7f");
    EXPECT_EQ(encode_float_hex(29, c[0], rule), "bc00ff55");
    EXPECT_EQ(encode_float_hex(29, c[1], rule), "7cda06fc");
    EXPECT_EQ(encode_float_hex(29, c[2], rule), "06121980");
    EXPECT_EQ(encode_float_hex(65, c[0], rule), "55") << "A8 takes Color[3]";
    // R11G11B10: NaN is all ones, negatives are 0.
    EXPECT_EQ(encode_float_hex(26, c[5], rule), "ff073e00");
    // Integer channels (the RTV lines agree with ClearView's truncation).
    EXPECT_EQ(encode_float_hex(3, c[3], rule),
              "2c010000 00000000 70110100 005ed0b2");
    EXPECT_EQ(encode_float_hex(4, c[4], rule),
              "ffffff7f 00000080 ffffff7f 00000080");
}

TEST(D3D11TexelEncodeTest, ClearViewIntegersTruncateAndClamp) {
    // a06 probe "CLV RGBA8_UINT ..." lines; D3D11.3 5.2.3.3.
    using winapi::d3d11_texel::FloatClear;
    EXPECT_EQ(encode_float_hex(30, {235.7f, -3.0f, 300.0f, 1.9f},
                               FloatClear::ClearView),
              "eb00ff01");
    EXPECT_EQ(encode_float_hex(30, {17.9f, 18.2f, 19.5f, 20.5f},
                               FloatClear::ClearView),
              "11121314");
    EXPECT_EQ(encode_float_hex(42, {7.9f, 0, 0, 0}, FloatClear::ClearView),
              "07000000");
}

TEST(D3D11TexelEncodeTest, QuantizedRenderTargetColoursAreTheStoredCodes) {
    // A render-pass clear converts the colour itself; the frontend hands it
    // the exact value of the code D3D stores, so any correctly rounding
    // conversion lands on the oracle's bits.
    namespace t = winapi::d3d11_texel;
    const auto q = [](u32 format, std::array<float, 4> color) {
        return t::quantize_render_target_color(*t::layout_for(format), color);
    };
    const auto r8 = q(28, {0.5f / 255.0f, 1.5f / 255.0f, 2.5f / 255.0f, 0.5f});
    EXPECT_EQ(r8[0], 0.0f);
    EXPECT_EQ(r8[1], 2.0f / 255.0f);
    EXPECT_EQ(r8[2], 2.0f / 255.0f);
    EXPECT_EQ(r8[3], 128.0f / 255.0f);
    const auto h = q(10, {1.7f, 300.7f, 1e20f, -1e20f});
    EXPECT_EQ(h[0], 1.69921875f) << "0x3ECC, toward zero";
    EXPECT_EQ(h[1], 300.5f) << "0x5CB2";
    EXPECT_EQ(h[2], 65504.0f) << "largest finite, not INF";
    EXPECT_EQ(h[3], -65504.0f);
    const auto srgb = q(29, {0.5f, 0.0f, 1.0f, 0.3333f});
    EXPECT_EQ(t::float_to_srgb(srgb[0], 8), 0xBCu);
    EXPECT_EQ(t::float_to_unorm(srgb[3], 8), 0x55u) << "alpha is linear";
    const auto u = q(30, {7.5f, -1.0f, 300.0f, 0.25f});
    EXPECT_EQ(u, (std::array<float, 4>{7.5f, -1.0f, 300.0f, 0.25f}))
        << "integer channels are left to the backend's conversion";
}

TEST_F(D3D11ResourceOpsTest, UavClearsFillUavOnlyTexturesWithTheViewsTexel) {
    // V1/V2: a texture without BIND_RENDER_TARGET, every bit exact.
    const GuestAddr t = tex2d({2, 1, 1, 1, 24, 1, 0, 0, 0x80, 0, 0});
    const GuestAddr uav = make_view(8, t, {24, 4, 0});
    clear_uav(false, uav, {0x12345678u, 0x9ABCDEF0u, 0x3F800000u, 0xFFFFFFFFu});
    ASSERT_EQ(backend.state.texel_fills.size(), 1u);
    const auto& fill = backend.state.texel_fills[0];
    EXPECT_EQ(probe_hex(fill.texel), "78c20bc0")
        << "a06: UAV UAV R10G10B10A2_UNORM Uint u0";
    EXPECT_EQ((std::array<unsigned, 7>{fill.mip, fill.x, fill.y, fill.z,
                                       fill.width, fill.height, fill.depth}),
              (std::array<unsigned, 7>{0, 0, 0, 0, 2, 1, 1}));
    clear_uav(true, uav, {bits(0.5f), bits(-0.25f), bits(1.7f),
                          bits(0.3333f)});
    ASSERT_EQ(backend.state.texel_fills.size(), 2u);
    EXPECT_EQ(probe_hex(backend.state.texel_fills[1].texel), "0002f07f");

    // V3: Float on an integer view never reaches the driver.
    const GuestAddr ti = tex2d({2, 1, 1, 1, 42, 1, 0, 0, 0x80, 0, 0});
    const GuestAddr uav_i = make_view(8, ti, {42, 4, 0});
    backend.state.texel_fills.clear();
    clear_uav(true, uav_i, {bits(0.5f), 0, 0, 0});
    EXPECT_TRUE(backend.state.texel_fills.empty());
    clear_uav(false, uav_i, {1, 2, 3, 4});
    ASSERT_EQ(backend.state.texel_fills.size(), 1u);
    EXPECT_EQ(probe_hex(backend.state.texel_fills[0].texel), "01000000");
}

TEST_F(D3D11ResourceOpsTest, UavClearsCoverExactlyTheViewsWindow) {
    // V6: Texture2DArray view of slice 1 only.
    const GuestAddr a = tex2d({2, 1, 1, 3, 42, 1, 0, 0, 0x80, 0, 0});
    const GuestAddr uav_a = make_view(8, a, {42, 5, 0, 1, 1});
    clear_uav(false, uav_a, {7, 7, 7, 7});
    ASSERT_EQ(backend.state.texel_fills.size(), 1u);
    EXPECT_EQ(backend.state.texel_fills[0].z, 1u) << "first slice";
    EXPECT_EQ(backend.state.texel_fills[0].depth, 1u) << "slice count";
    // V8: mip 1 of a 4x2 texture is 2x1.
    const GuestAddr m = tex2d({4, 2, 2, 1, 42, 1, 0, 0, 0x80, 0, 0});
    const GuestAddr uav_m = make_view(8, m, {42, 4, 1});
    clear_uav(false, uav_m, {7, 7, 7, 7});
    ASSERT_EQ(backend.state.texel_fills.size(), 2u);
    EXPECT_EQ(backend.state.texel_fills[1].mip, 1u);
    EXPECT_EQ(backend.state.texel_fills[1].width, 2u);
    EXPECT_EQ(backend.state.texel_fills[1].height, 1u);
    // V7: a Texture3D view of W slices 1..2 of 4.
    const GuestAddr v = tex3d({2, 1, 4, 1, 42, 0, 0x80, 0, 0});
    const GuestAddr uav_v = make_view(8, v, {42, 8, 0, 1, 2});
    clear_uav(false, uav_v, {7, 7, 7, 7});
    ASSERT_EQ(backend.state.texel_fills.size(), 3u);
    const auto& fill = backend.state.texel_fills[2];
    EXPECT_TRUE(fill.volume);
    EXPECT_EQ((std::array<unsigned, 3>{fill.z, fill.depth, fill.width}),
              (std::array<unsigned, 3>{1, 2, 2}));
}

TEST_F(D3D11ResourceOpsTest, UavClearsOnBuffersFollowTheViewKind) {
    const std::vector<u8> cc(32, 0xCC);
    // V5: a typed R8G8B8A8_UNORM buffer view, per component.
    const GuestAddr typed =
        buffer({8, 0, 0x80, 0, 0, 0}, init_data(cc, 0));
    const GuestAddr uav_t = make_view(8, typed, {28, 1, 0, 2, 0});
    backend.state.buffer_region_copies.clear();
    clear_uav(false, uav_t, {0x11223344u, 0x55667788u, 0x3F800000u, 0x180u});
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_contents,
              (std::vector<u8>{0x44, 0x88, 0x00, 0x80, 0x44, 0x88, 0x00,
                               0x80}));
    clear_uav(true, uav_t, {bits(0.5f), bits(-0.25f), bits(1.7f),
                            bits(2.5f)});
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 2u);
    EXPECT_EQ(backend.state.buffer_region_copies[1].src_contents,
              (std::vector<u8>{0x80, 0x00, 0xFF, 0xFF, 0x80, 0x00, 0xFF,
                               0xFF}));
    // V3: Float on a raw view is dropped; Uint replicates Values[0].
    const GuestAddr raw =
        buffer({32, 0, 0x80, 0, 0x20, 0}, init_data(cc, 0));
    const GuestAddr uav_r = make_view(8, raw, {39, 1, 4, 2, 1});
    backend.state.buffer_region_copies.clear();
    clear_uav(true, uav_r, {bits(0.5f), 0, 0, 0});
    EXPECT_TRUE(backend.state.buffer_region_copies.empty());
    clear_uav(false, uav_r, {0x11223344u, 5, 6, 7});
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 16u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_contents,
              (std::vector<u8>{0x44, 0x33, 0x22, 0x11, 0x44, 0x33, 0x22,
                               0x11}));
}

// A buffer UAV clear whose element is one repeated byte needs no pattern
// buffer: it is a backend fill, still on the GPU timeline. Anything else, or a
// range the fill contract refuses, keeps the copy from a pattern.
TEST_F(D3D11ResourceOpsTest, UniformByteBufferUavClearsAreBackendFills) {
    const std::vector<u8> cc(32, 0xCC);
    // Raw view over words [4, 6): Uint replicates Values[0] as a raw word.
    const GuestAddr raw = buffer({32, 0, 0x80, 0, 0x20, 0}, init_data(cc, 0));
    const GuestAddr uav_r = make_view(8, raw, {39, 1, 4, 2, 1});
    for (const u32 word : {0x00000000u, 0xFFFFFFFFu}) {
        backend.state.buffer_region_copies.clear();
        backend.state.buffer_fills.clear();
        clear_uav(false, uav_r, {word, 5, 6, 7});
        EXPECT_TRUE(backend.state.buffer_region_copies.empty()) << word;
        ASSERT_EQ(backend.state.buffer_fills.size(), 1u) << word;
        EXPECT_EQ(backend.state.buffer_fills[0].offset, 16u);
        EXPECT_EQ(backend.state.buffer_fills[0].bytes, 8u);
        EXPECT_EQ(backend.state.buffer_fills[0].value,
                  static_cast<u8>(word & 0xFF));
    }
    // Not one repeated byte: the pattern copy.
    backend.state.buffer_region_copies.clear();
    backend.state.buffer_fills.clear();
    clear_uav(false, uav_r, {0x01010102u, 0, 0, 0});
    EXPECT_TRUE(backend.state.buffer_fills.empty());
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 16u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_contents,
              (std::vector<u8>{0x02, 0x01, 0x01, 0x01, 0x02, 0x01, 0x01,
                               0x01}));
    // A typed R8_UINT view over bytes [1, 4): uniform, but the fill refuses
    // the unaligned range, so the clear falls back to the copy.
    const GuestAddr typed = buffer({8, 0, 0x80, 0, 0, 0}, init_data(cc, 0));
    const GuestAddr uav_t = make_view(8, typed, {62, 1, 1, 3, 0});
    backend.state.buffer_region_copies.clear();
    backend.state.buffer_fills.clear();
    clear_uav(false, uav_t, {7, 0, 0, 0});
    EXPECT_TRUE(backend.state.buffer_fills.empty());
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_contents,
              (std::vector<u8>{7, 7, 7}));
}

TEST_F(D3D11ResourceOpsTest, RenderTargetClearsConvertIntoTheViewsFormat) {
    // T3: R8G8B8A8_TYPELESS with an _SRGB view clears through that view.
    const GuestAddr t = tex2d({2, 1, 1, 1, 27, 1, 0, 0, 0x20, 0, 0});
    const GuestAddr rtv = make_view(9, t, {29, 4, 0});
    write_words(A6_ARG, {bits(0.5f), bits(-0.25f), bits(7.0f),
                         bits(0.3333f)});
    context_call(50, {rtv, A6_ARG});
    ASSERT_EQ(backend.state.clear_view_formats.size(), 1u);
    EXPECT_EQ(backend.state.clear_view_formats[0],
              graphics::pixel_format_from_dxgi(29));
    // a06: RTV typeless RGBA8_TYPELESS as SRGB -> bc00ff55.
    EXPECT_EQ(winapi::d3d11_texel::float_to_srgb(backend.state.last_clear[0], 8),
              0xBCu);
    EXPECT_EQ(backend.state.last_clear[1], 0.0f);
    EXPECT_EQ(backend.state.last_clear[2], 1.0f);
    EXPECT_EQ(winapi::d3d11_texel::float_to_unorm(backend.state.last_clear[3], 8),
              0x55u);
}

TEST_F(D3D11ResourceOpsTest, DepthClearsOutsideTheViewportRangeAreDropped) {
    // D3: D3D11.3 5.2.3.1 and the a06 probe DSV lines.
    const GuestAddr d = tex2d({2, 1, 1, 1, 40, 1, 0, 0, 0x40, 0, 0});
    const GuestAddr dsv = make_view(10, d, {});
    const auto clear = [&](float depth) {
        cpu.sse.xmm[3].u64_[0] = bits(depth);
        cpu.sse.xmm[3].u64_[1] = 0;
        context_call(53, {dsv, 3, bits(depth), 0xA5});
    };
    const unsigned before = backend.state.depth_clears;
    clear(1.5f);
    clear(-0.5f);
    clear(std::numeric_limits<float>::quiet_NaN());
    EXPECT_EQ(backend.state.depth_clears, before) << "stencil too";
    clear(0.25f);
    EXPECT_EQ(backend.state.depth_clears, before + 1);
    EXPECT_EQ(backend.state.last_depth_clear_value, 0.25f);
    // D2: D16 0.25 stores 0x4000 (round to nearest), handed over as that
    // code nudged up a quarter step.
    const GuestAddr d16 = tex2d({2, 1, 1, 1, 55, 1, 0, 0, 0x40, 0, 0});
    const GuestAddr dsv16 = make_view(10, d16, {});
    cpu.sse.xmm[3].u64_[0] = bits(0.25f);
    context_call(53, {dsv16, 1, bits(0.25f), 0});
    EXPECT_EQ(backend.state.last_depth_clear_value,
              static_cast<float>((16384.0 + 0.25) / 65535.0));
}

TEST_F(D3D11ResourceOpsTest, GenerateMipsHonoursFlagFormatAndViewWindow) {
    // G2/G3/G7 (Learn, GenerateMips).
    const auto srv = [&](u32 format, u32 misc, u32 most, u32 mips) {
        const GuestAddr t =
            tex2d({4, 4, 3, 1, format, 1, 0, 0, 0x28, 0, misc});
        return make_view(7, t, {format, 4, most, mips});
    };
    context_call(54, {srv(28, 1, 1, 2)});
    ASSERT_EQ(backend.state.mip_windows.size(), 1u);
    EXPECT_EQ(backend.state.mip_windows[0],
              (std::array<unsigned, 4>{1, 2, 0, 1}));
    context_call(54, {srv(28, 0, 0, 3)});  // no GENERATE_MIPS
    context_call(54, {srv(42, 1, 0, 3)});  // R32_UINT: no MIP_AUTOGEN
    context_call(54, {srv(28, 1, 2, 1)});  // one level: nothing below
    EXPECT_EQ(backend.state.mip_windows.size(), 1u);
    EXPECT_EQ(backend.state.mip_generations, 0u);
}

TEST_F(D3D11ResourceOpsTest, ResolveRefusesAFormatWithoutMultisampleResolve) {
    // R3: D3D11.3 5.6.6; a06 probe "RES typeless -> typeless, fmt UINT".
    const GuestAddr ms = tex2d({2, 1, 1, 1, 27, 4, 0, 0, 0x20, 0, 0});
    const GuestAddr dst = tex2d({2, 1, 1, 1, 27, 1, 0, 0, 0x20, 0, 0});
    context_call(57, {dst, 0, ms, 0, 30});
    EXPECT_EQ(backend.state.texture_resolves, 0u);
    context_call(57, {dst, 0, ms, 0, 28});
    EXPECT_EQ(backend.state.texture_resolves, 1u);
}

TEST_F(D3D11ResourceOpsTest, TextureCopiesCoverTheLegalPairs) {
    const auto region = [&](GuestAddr dst, u32 dst_sub, u32 x, u32 y, u32 z,
                            GuestAddr src, u32 src_sub,
                            std::initializer_list<u32> box) {
        GuestAddr box_address = 0;
        if (box.size() != 0) {
            write_words(A6_ARG, box);
            box_address = A6_ARG;
        }
        context_call(46, {dst, dst_sub, x, y, z, src, src_sub, box_address});
    };
    // C9: one texture, mip 0 -> mip 1.
    const GuestAddr t = tex2d({4, 4, 2, 1, 28, 1, 0, 0, 0x8, 0, 0});
    region(t, 1, 0, 0, 0, t, 0, {0, 0, 0, 2, 2, 1});
    ASSERT_EQ(backend.state.element_copies.size(), 1u);
    EXPECT_EQ(backend.state.element_copies[0],
              (std::array<unsigned, 13>{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2,
                                        1}));
    // C10: the same subresource, overlapping.
    region(t, 0, 1, 0, 0, t, 0, {0, 0, 0, 3, 1, 1});
    EXPECT_EQ(backend.state.element_copies.size(), 2u);
    // C11: RGBA8_UNORM -> RGBA8_UINT, one family, different host formats.
    const GuestAddr unorm = tex2d({4, 4, 1, 1, 28, 1, 0, 0, 0x8, 0, 0});
    const GuestAddr uint = tex2d({4, 4, 1, 1, 30, 1, 0, 0, 0x8, 0, 0});
    region(uint, 0, 0, 0, 0, unorm, 0, {});
    EXPECT_EQ(backend.state.element_copies.size(), 3u);
    context_call(47, {uint, unorm});  // CopyResource
    EXPECT_EQ(backend.state.element_copies.size(), 4u);
    // C14: BC1 16x16 -> R32G32_UINT 4x4 (Learn, D3D10.1 conversion).
    const GuestAddr bc1 = tex2d({16, 16, 1, 1, 71, 1, 0, 0, 0x8, 0, 0});
    const GuestAddr rg32 = tex2d({4, 4, 1, 1, 17, 1, 0, 0, 0x8, 0, 0});
    region(rg32, 0, 0, 0, 0, bc1, 0, {});
    ASSERT_EQ(backend.state.element_copies.size(), 5u);
    EXPECT_EQ(backend.state.element_copies[4][10], 16u) << "source texels";
    context_call(47, {bc1, rg32});
    EXPECT_EQ(backend.state.element_copies.size(), 6u);
    const GuestAddr rg32_16 = tex2d({16, 16, 1, 1, 17, 1, 0, 0, 0x8, 0, 0});
    context_call(47, {rg32_16, bc1});
    EXPECT_EQ(backend.state.element_copies.size(), 6u)
        << "a06: CopyResource BC1 16x16 -> R32G32_UINT 16x16 is dropped";
    // C16: a Texture3D box.
    const GuestAddr v0 = tex3d({4, 4, 4, 1, 42, 0, 0x8, 0, 0});
    const GuestAddr v1 = tex3d({4, 4, 4, 1, 42, 0, 0x8, 0, 0});
    region(v1, 0, 0, 2, 1, v0, 0, {1, 1, 1, 3, 2, 3});
    ASSERT_EQ(backend.state.element_copies.size(), 7u);
    EXPECT_EQ(backend.state.element_copies[6],
              (std::array<unsigned, 13>{0, 0, 0, 2, 1, 0, 0, 1, 1, 1, 2, 1,
                                        2}));
    // C19/C26/C22: dropped pairs.
    const unsigned blits = backend.state.texture_region_copies;
    const GuestAddr immutable = tex2d({4, 4, 1, 1, 28, 1, 0, 1, 0x8, 0, 0},
                                      init_data(std::vector<u8>(64), 16));
    region(immutable, 0, 0, 0, 0, unorm, 0, {});
    const GuestAddr line = [&] {
        write_words(A6_DESC, {8, 1, 1, 28, 0, 0x8, 0, 0});
        EXPECT_EQ(device_call(4, {A6_DESC, 0, A6_OUT}), d3d11::S_OK);
        return memory.read_u64(A6_OUT).value();
    }();
    const GuestAddr wide = tex2d({8, 1, 1, 1, 28, 1, 0, 0, 0x8, 0, 0});
    region(line, 0, 0, 0, 0, wide, 0, {});
    const GuestAddr ds0 = tex2d({4, 4, 1, 1, 40, 1, 0, 0, 0x40, 0, 0});
    const GuestAddr ds1 = tex2d({4, 4, 1, 1, 40, 1, 0, 0, 0x40, 0, 0});
    region(ds1, 0, 0, 0, 0, ds0, 0, {0, 0, 0, 2, 1, 1});
    EXPECT_EQ(backend.state.texture_region_copies, blits);
    EXPECT_EQ(backend.state.element_copies.size(), 7u);
    region(ds1, 0, 0, 0, 0, ds0, 0, {});
    EXPECT_EQ(backend.state.texture_region_copies, blits + 1)
        << "the whole subresource is legal";
}

TEST_F(D3D11ResourceOpsTest, BufferCopiesWithinOneBufferBehaveAsMemmove) {
    const std::vector<u8> seq(32, 0x11);
    const GuestAddr a = buffer({32, 0, 0x1, 0, 0, 0}, init_data(seq, 0));
    const auto region = [&](GuestAddr dst, u32 x, GuestAddr src, u32 left,
                            u32 right) {
        write_words(A6_ARG, {left, 0, 0, right, 1, 1});
        context_call(46, {dst, 0, x, 0, 0, src, 0, A6_ARG});
    };
    backend.state.buffer_region_copies.clear();
    region(a, 24, a, 4, 12);  // C2, apart
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 24u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_offset, 4u);
    region(a, 4, a, 0, 16);  // overlapping: through a transient buffer
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 3u);
    EXPECT_EQ(backend.state.buffer_region_copies[1].dst_offset, 0u);
    EXPECT_EQ(backend.state.buffer_region_copies[1].src_offset, 0u);
    EXPECT_EQ(backend.state.buffer_region_copies[2].dst_offset, 4u);
    EXPECT_EQ(backend.state.buffer_region_copies[2].src_byte_width, 16u);
    // C5/C6: IMMUTABLE destination; structured strides; partial structures.
    const GuestAddr imm = buffer({32, 1, 0x1, 0, 0, 0}, init_data(seq, 0));
    region(imm, 0, a, 4, 12);
    const GuestAddr s16 = buffer({64, 0, 0x8, 0, 0x40, 16}, init_data(
        std::vector<u8>(64), 0));
    const GuestAddr s8 = buffer({64, 0, 0x8, 0, 0x40, 8}, init_data(
        std::vector<u8>(64), 0));
    region(s16, 0, s16, 8, 24);
    EXPECT_EQ(backend.state.buffer_region_copies.size(), 3u);
    const unsigned copies = backend.state.buffer_copies;
    context_call(47, {imm, a});
    context_call(47, {s8, s16});
    EXPECT_EQ(backend.state.buffer_copies, copies);
    const GuestAddr plain = buffer({64, 0, 0x1, 0, 0, 0}, init_data(
        std::vector<u8>(64), 0));
    context_call(47, {plain, s16});
    EXPECT_EQ(backend.state.buffer_copies, copies + 1)
        << "structured -> linear is legal (5.6.5)";
}

TEST_F(D3D11ResourceOpsTest, UpdateSubresourceWritesOnlyTheBox) {
    // U16: BC1, a 4-aligned box, rows of blocks SrcRowPitch apart.
    const GuestAddr bc1 = tex2d({16, 16, 1, 1, 71, 1, 0, 0, 0x8, 0, 0});
    std::vector<u8> src(80);
    for (usize i = 0; i < src.size(); ++i) src[i] = static_cast<u8>(i);
    ASSERT_TRUE(memory.write_bytes(A6_DATA, src.data(), src.size()).is_ok());
    write_words(A6_ARG, {4, 4, 0, 12, 8, 1});
    const unsigned updates = backend.state.texture_updates;
    context_call(48, {bc1, 0, A6_ARG, A6_DATA, 40, 0});
    ASSERT_EQ(backend.state.texture_updates, updates + 1);
    EXPECT_EQ(backend.state.last_texture_update_region,
              (std::array<unsigned, 6>{0, 0, 4, 4, 8, 4}));
    EXPECT_EQ(backend.state.last_texture_update_row_pitch, 16u);
    EXPECT_EQ(backend.state.last_texture_update,
              std::vector<u8>(src.begin(), src.begin() + 16));
    // U17: a Texture3D box reads its rows and slices, nothing past them.
    const GuestAddr v = tex3d({4, 2, 4, 1, 42, 0, 0x8, 0, 0});
    write_words(A6_ARG, {1, 1, 1, 3, 2, 3});
    context_call(48, {v, 0, A6_ARG, A6_DATA, 12, 40});
    ASSERT_EQ(backend.state.volume_region_updates.size(), 1u);
    EXPECT_EQ(backend.state.volume_region_updates[0],
              (std::array<unsigned, 9>{0, 1, 1, 1, 2, 1, 2, 8, 8}));
    std::vector<u8> expected(src.begin(), src.begin() + 8);
    expected.insert(expected.end(), src.begin() + 40, src.begin() + 48);
    EXPECT_EQ(backend.state.last_volume_update, expected);
    // U11: a DYNAMIC texture is not a destination.
    const GuestAddr dyn = tex2d({4, 4, 1, 1, 28, 1, 0, 2, 0x8, 0x10000, 0});
    context_call(48, {dyn, 0, 0, A6_DATA, 16, 0});
    EXPECT_EQ(backend.state.texture_updates, updates + 1);
}

// Each buffer write names the ordering its API defines (graphics::
// BufferWrite), so the backend knows which may go in place. The steamwebhelper
// GPU process (ANGLE on D3D11) logged 2-byte non-DISCARD writes at +110,
// +114, ... into a bound 1 MiB buffer: that is Unmap of a WRITE_NO_OVERWRITE
// map uploading exactly the bytes that changed -- in place by the
// application's promise (Learn D3D11_MAP), not a hazard. Every other buffer
// write is ordered after the work already recorded.
TEST_F(D3D11ResourceOpsTest, BufferWritesNameTheirOrderingContract) {
    using graphics::BufferWrite;
    const auto map_write = [&](GuestAddr resource, u32 map_type, u32 at,
                               const std::vector<u8>& bytes) {
        context_call(14, {resource, 0, map_type, 0, A6_OUT});
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
        const GuestAddr mapped = memory.read_u64(A6_OUT).value();
        ASSERT_NE(mapped, 0u);
        ASSERT_TRUE(memory.write_bytes(mapped + at, bytes.data(), bytes.size())
                        .is_ok());
        context_call(15, {resource, 0});
    };
    // A DYNAMIC 1 MiB vertex buffer: DISCARD, then NO_OVERWRITE appends.
    const GuestAddr dynamic = buffer({1u << 20, 2, 0x1, 0x10000, 0, 0});
    map_write(dynamic, 4, 0, std::vector<u8>(256, 0x11));
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::Discard);
    const unsigned updates = backend.state.buffer_updates;
    map_write(dynamic, 5, 110, {0xAB, 0xCD});
    ASSERT_EQ(backend.state.buffer_updates, updates + 1);
    EXPECT_EQ(backend.state.last_buffer_update_offset, 110u);
    EXPECT_EQ(backend.state.last_buffer_update, (std::vector<u8>{0xAB, 0xCD}));
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::NoOverwrite);
    // UpdateSubresource of a DEFAULT buffer is a command in the stream.
    const GuestAddr target = buffer({256, 0, 0x1, 0, 0, 0},
                                    init_data(std::vector<u8>(256), 0));
    const std::vector<u8> src(32, 0x5C);
    ASSERT_TRUE(memory.write_bytes(A6_DATA, src.data(), src.size()).is_ok());
    write_words(A6_ARG, {16, 0, 0, 48, 1, 1});
    context_call(48, {target, 0, A6_ARG, A6_DATA, 0, 0});
    EXPECT_EQ(backend.state.last_buffer_update_offset, 16u);
    EXPECT_EQ(backend.state.last_buffer_update, src);
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::Ordered);
    // A STAGING buffer mapped WRITE: Map waited for its copies; the upload
    // is still a plain write.
    const GuestAddr staging = buffer({256, 3, 0, 0x30000, 0, 0});
    map_write(staging, 2, 8, {1, 2, 3, 4});
    EXPECT_EQ(backend.state.last_buffer_update_offset, 8u);
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::Ordered);
}

// Learn D3D11_COPY_FLAGS: NO_OVERWRITE "The existing contents of the resource
// cannot be overwritten" (the application's promise: the bytes go in place),
// DISCARD "The existing contents of the resource are undefined and can be
// discarded". A 16-byte partial update of an in-flight constant-buffer ring
// is legal only with one of them, and must not rename the whole buffer.
TEST_F(D3D11ResourceOpsTest, UpdateSubresource1CopyFlagsNameTheirOrdering) {
    using graphics::BufferWrite;
    const GuestAddr ring = buffer({256, 0, 0x4 /* CONSTANT_BUFFER */, 0, 0, 0},
                                  init_data(std::vector<u8>(256), 0));
    const std::vector<u8> src(16, 0x6E);
    ASSERT_TRUE(memory.write_bytes(A6_DATA, src.data(), src.size()).is_ok());
    write_words(A6_ARG, {32, 0, 0, 48, 1, 1});
    const unsigned updates = backend.state.buffer_updates;
    context_call(116, {ring, 0, A6_ARG, A6_DATA, 0, 0, 1});
    ASSERT_EQ(backend.state.buffer_updates, updates + 1);
    EXPECT_EQ(backend.state.last_buffer_update_offset, 32u);
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::NoOverwrite);
    context_call(116, {ring, 0, A6_ARG, A6_DATA, 0, 0, 2});
    ASSERT_EQ(backend.state.buffer_updates, updates + 2);
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::Discard);
    // Flags 0 behave like UpdateSubresource: ordered (here on a whole
    // constant buffer, the only form 0 allows).
    context_call(116, {ring, 0, 0, A6_DATA, 0, 0, 0});
    ASSERT_EQ(backend.state.buffer_updates, updates + 3);
    EXPECT_EQ(backend.state.last_buffer_update_mode, BufferWrite::Ordered);
}

TEST_F(D3D11ResourceOpsTest, MapBehavesAsMeasured) {
    // M16c: WRITE_DISCARD on a dynamic texture renames its storage.
    const GuestAddr dyn = tex2d({4, 4, 1, 1, 28, 1, 0, 2, 0x8, 0x10000, 0});
    context_call(14, {dyn, 0, 4, 0, A6_OUT});
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    context_call(15, {dyn, 0});
    EXPECT_EQ(backend.state.storage_discards, 1u);
    // M7: mapping a mapped subresource again succeeds, same pointer.
    const GuestAddr staging =
        tex2d({4, 4, 1, 1, 28, 1, 0, 3, 0, 0x30000, 0});
    context_call(14, {staging, 0, 1, 0, A6_OUT});
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    const GuestAddr first = memory.read_u64(A6_OUT).value();
    context_call(14, {staging, 0, 3, 0, A6_OUT + 0x20});
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(A6_OUT + 0x20).value(), first);
    // L3: GetResourceMinLOD of a resource without RESOURCE_CLAMP reads 0
    // whatever SetResourceMinLOD was given (a06 probe LOD lines).
    cpu.sse.xmm[1].u64_[0] = bits(3.0f);
    context_call(55, {staging, bits(3.0f)});
    cpu.sse.xmm[0].u64_[0] = bits(9.0f);
    context_call(56, {staging});
    EXPECT_EQ(static_cast<u32>(cpu.sse.xmm[0].u64_[0]), bits(0.0f));
}

TEST_F(D3D11ResourceOpsTest, ClearViewConvertsAndCoversEverySlice) {
    // W3: an array RTV of slices 1-2 with one rect.
    const GuestAddr a = tex2d({2, 1, 1, 3, 28, 1, 0, 0, 0x20, 0, 0});
    const GuestAddr rtv = make_view(9, a, {28, 5, 0, 1, 2});
    write_words(A6_ARG, {bits(1.0f), bits(0.5f), bits(0.25f), bits(1.0f)});
    write_words(A6_ARG + 0x20, {1, 0, 2, 1});
    context_call(132, {rtv, A6_ARG, A6_ARG + 0x20, 1});
    ASSERT_EQ(backend.state.texel_fills.size(), 1u);
    const auto& fill = backend.state.texel_fills[0];
    EXPECT_EQ(probe_hex(fill.texel), "ff8040ff");
    EXPECT_EQ((std::array<unsigned, 6>{fill.x, fill.y, fill.width,
                                       fill.height, fill.z, fill.depth}),
              (std::array<unsigned, 6>{1, 0, 1, 1, 1, 2}));
    // W5: a UINT UAV, no rects: 7.9 -> 7.
    const GuestAddr u = tex2d({4, 1, 1, 1, 42, 1, 0, 0, 0x80, 0, 0});
    const GuestAddr uav = make_view(8, u, {42, 4, 0});
    write_words(A6_ARG, {bits(7.9f), 0, 0, 0});
    context_call(132, {uav, A6_ARG, 0, 0});
    ASSERT_EQ(backend.state.texel_fills.size(), 2u);
    EXPECT_EQ(probe_hex(backend.state.texel_fills[1].texel), "07000000");
    EXPECT_EQ(backend.state.texel_fills[1].width, 4u);
    // W6: a buffer UAV rect is an element range, converted.
    const GuestAddr b = buffer({16, 0, 0x80, 0, 0, 0},
                               init_data(std::vector<u8>(16, 0xCC), 0));
    const GuestAddr buav = make_view(8, b, {42, 1, 0, 4, 0});
    backend.state.buffer_region_copies.clear();
    write_words(A6_ARG + 0x20, {1, 0, 3, 1});
    context_call(132, {buav, A6_ARG, A6_ARG + 0x20, 1});
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].dst_offset, 4u);
    EXPECT_EQ(backend.state.buffer_region_copies[0].src_contents,
              (std::vector<u8>{7, 0, 0, 0, 7, 0, 0, 0}));
    // W7: a DSV of a depth-only resource takes Color[0] as depth.
    const GuestAddr d = tex2d({2, 1, 1, 1, 40, 1, 0, 0, 0x40, 0, 0});
    const GuestAddr dsv = make_view(10, d, {});
    const unsigned depth_clears = backend.state.depth_clears;
    write_words(A6_ARG, {bits(1.0f), bits(1.0f), bits(1.0f), bits(1.0f)});
    context_call(132, {dsv, A6_ARG, 0, 0});
    EXPECT_EQ(backend.state.depth_clears, depth_clears + 1);
    EXPECT_EQ(backend.state.last_depth_clear_value, 1.0f);
}

TEST_F(D3D11ResourceOpsTest, StagingCopiesAreQueuedAndMapDoNotWaitPolls) {
    // M15/item 9: Learn (CopyResource) "an asynchronous call"; Map with
    // D3D11_MAP_FLAG_DO_NOT_WAIT answers DXGI_ERROR_WAS_STILL_DRAWING until
    // the copy has executed (a06 gpu probe: DNW texture 0x887a000a, then
    // S_OK with the data).
    backend.state.async_readbacks = true;
    const GuestAddr gpu = tex2d({4, 4, 1, 1, 28, 1, 0, 0, 0x8, 0, 0});
    const GuestAddr staging =
        tex2d({4, 4, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0});
    context_call(47, {staging, gpu});  // CopyResource
    EXPECT_TRUE(memory.write_u64(A6_OUT, 0x1234).is_ok());
    context_call(14, {staging, 0, 1, 0x100000, A6_OUT});
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x887A000Au);
    EXPECT_EQ(memory.read_u64(A6_OUT).value(), 0u) << "pData is NULL";
    backend.state.readbacks_ready = true;
    context_call(14, {staging, 0, 1, 0x100000, A6_OUT});
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    const GuestAddr data = memory.read_u64(A6_OUT).value();
    EXPECT_EQ(memory.read_u32(data).value(), 0x5A5A5A5Au);
    EXPECT_EQ(memory.read_u32(data + 3 * 16 + 12).value(), 0x5A5A5A5Au);
    context_call(15, {staging, 0});
    EXPECT_EQ(backend.state.readback_waits, 0u);
    // Without DO_NOT_WAIT, Map waits for the copy.
    backend.state.readbacks_ready = false;
    context_call(47, {staging, gpu});
    context_call(14, {staging, 0, 1, 0, A6_OUT});
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(backend.state.readback_waits, 1u);
}

TEST_F(D3D11ResourceOpsTest, PackedDepthStencilUploadsSplitIntoPlanes) {
    // C24: STAGING D24_UNORM_S8_UINT -> DEFAULT (Learn CopyResource; the a06
    // probe's words 0x11000001, 0x22000002, 0x33800000, 0x44ffffff).
    const GuestAddr staging =
        tex2d({4, 1, 1, 1, 45, 1, 0, 3, 0, 0x10000, 0});
    const GuestAddr gpu = tex2d({4, 1, 1, 1, 45, 1, 0, 0, 0x40, 0, 0});
    context_call(14, {staging, 0, 2, 0, A6_OUT});  // Map(WRITE)
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    const GuestAddr data = memory.read_u64(A6_OUT).value();
    write_words(data, {0x11000001u, 0x22000002u, 0x33800000u, 0x44FFFFFFu});
    context_call(15, {staging, 0});
    context_call(47, {gpu, staging});
    ASSERT_EQ(backend.state.uploaded_stencil_plane,
              (std::vector<u8>{0x11, 0x22, 0x33, 0x44}));
    ASSERT_EQ(backend.state.uploaded_depth_plane.size(), 4u);
    EXPECT_EQ(backend.state.uploaded_depth_plane[0],
              static_cast<float>(1.0 / 16777215.0));
    EXPECT_EQ(backend.state.uploaded_depth_plane[2],
              static_cast<float>(0x800000 / 16777215.0));
    EXPECT_EQ(backend.state.uploaded_depth_plane[3], 1.0f);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
