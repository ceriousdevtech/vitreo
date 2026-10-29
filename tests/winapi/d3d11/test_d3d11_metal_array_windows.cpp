// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_d3d11_metal_array_windows.cpp
/// @brief Texture-array view windows on the real Metal backend, through the
///        D3D11 front-end: clears and draws through TEXTURE2DARRAY RTVs
///        {MipSlice, FirstArraySlice, ArraySize}, TEXTURE2DARRAY / TEXTURE2D
///        SRVs read by Texture2D AND Texture2DArray declarations (Load,
///        GetDimensions; Sample in a pixel shader), and RWTexture2D(Array)
///        stores through TEXTURE2DARRAY UAV windows.
///
/// The sequence is tools/oracle_probe/d3d11_array_window_probe.c's, with its
/// fxc shaders (d3d11_array_window_shaders.h); every expected value is a line
/// of its d3d11_array_window_probe.expected (Windows, WARP; x86 == x64).
/// Not compared: the compute SampleLevel records (cs_ld2d [3], cs_ld2da [6]
/// and [7]), which read through an unbound sampler -- the default-sampler
/// contract, a separate gap (.docs/contracts/d3d11.md).

#include <gtest/gtest.h>

#include "d3d11_test_metal_vt_feedback.hpp"
#include "../../oracle_data/d3d11_array_window_shaders.h"

#include <array>
#include <bit>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace vitreo::winapi::d3d11_test {
namespace {

/// The 8-hex-digit words (@p decimal: the decimal numbers) of the .expected
/// line labelled @p label.
std::vector<u32> measured(const std::string& label, bool decimal = false) {
    std::ifstream in(VITREO_ORACLE_PROBE_DIR
                     "/d3d11_array_window_probe.expected");
    std::string line;
    while (std::getline(in, line)) {
        if (line.size() <= label.size() ||
            line.compare(0, label.size(), label) != 0 ||
            (line[label.size()] != ' ' && line[label.size()] != ':')) {
            continue;
        }
        std::istringstream tokens(line.substr(label.size() + 1));
        std::vector<u32> words;
        std::string token;
        while (tokens >> token) {
            bool number = decimal || token.size() == 8;
            for (const char c : token) {
                const auto u = static_cast<unsigned char>(c);
                number = number && (decimal ? std::isdigit(u) : std::isxdigit(u));
            }
            if (number) {
                words.push_back(static_cast<u32>(
                    std::stoul(token, nullptr, decimal ? 10 : 16)));
            }
        }
        return words;
    }
    ADD_FAILURE() << "no line \"" << label << "\" in the .expected";
    return {};
}

class ArrayWindowTest : public VtFeedbackTest {
protected:
    static constexpr u32 kSide = 4;

    void bind_params(const std::array<u32, 16>& words) {
        if (m_params == 0) {
            m_params = buffer(64, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0,
                              words.data());
            const std::array<u64, 1> list{m_params};
            context_call(16, {0, 1, put(list)});  // PSSetConstantBuffers
            context_call(71, {0, 1, put(list)});  // CSSetConstantBuffers
            return;
        }
        context_call(48, {m_params, 0, 0, put(words), 64, 64});
    }
    static std::array<u32, 16> colour(u32 r, u32 g, u32 b) {
        std::array<u32, 16> p{};
        p[0] = std::bit_cast<u32>(r / 255.0f);
        p[1] = std::bit_cast<u32>(g / 255.0f);
        p[2] = std::bit_cast<u32>(b / 255.0f);
        p[3] = std::bit_cast<u32>(1.0f);
        return p;
    }
    GuestAddr rtv_array(GuestAddr t, u32 mip, u32 first, u32 size) {
        const std::array<u32, 5> desc{kBgra8Unorm,
                                      D3D11_RTV_DIMENSION_TEXTURE2DARRAY, mip,
                                      first, size};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(9, {t, put(desc), out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    GuestAddr srv(GuestAddr t, u32 dimension, u32 mip, u32 mips, u32 first,
                  u32 size) {
        const std::array<u32, 6> desc{kBgra8Unorm, dimension, mip, mips, first,
                                      size};
        const GuestAddr out = out_slot();
        EXPECT_EQ(device_call(7, {t, put(desc), out}), d3d11::S_OK);
        return memory.read_u64(out).value();
    }
    void clear(GuestAddr rtv, u32 r, u32 g, u32 b) {
        const std::array<float, 4> c{r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
        context_call(50, {rtv, put(c)});  // ClearRenderTargetView
    }
    /// Every subresource of the 4x4 @p t ({mips} x {slices}, @p format)
    /// against the .expected lines "<label> slice s mip m:" (rows 0 and 1).
    void expect_subresources(GuestAddr t, u32 format, u32 mips, u32 slices,
                             const std::string& label) {
        const GuestAddr staging =
            texture(kSide, kSide, mips, slices, format, D3D11_USAGE_STAGING, 0,
                    D3D11_CPU_ACCESS_READ);
        context_call(47, {staging, t});  // CopyResource
        for (u32 s = 0; s < slices; ++s) {
            for (u32 m = 0; m < mips; ++m) {
                const std::string what = label + " slice " + std::to_string(s) +
                                         " mip " + std::to_string(m);
                const u32 sub = m + s * mips;
                const Mapped map_out = map(staging, sub, D3D11_MAP_READ);
                ASSERT_EQ(map_out.hr, d3d11::S_OK) << what;
                const u32 w = kSide >> m;
                std::vector<u32> got;
                for (u32 y = 0; y < 2 && y < w; ++y) {
                    const auto row =
                        words_at(map_out.data + y * map_out.row_pitch, w);
                    got.insert(got.end(), row.begin(), row.end());
                }
                unmap(staging, sub);
                EXPECT_EQ(got, measured(what)) << what;
            }
        }
    }
    /// One dispatch of @p cs with @p view at t@p slot; records [0, @p n)
    /// against "<label>[i]".
    void expect_cs(GuestAddr cs, u32 slot, GuestAddr view, u32 n,
                   const std::string& label) {
        const GuestAddr out =
            buffer(8 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                   D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, nullptr);
        const std::array<u64, 1> srvs{view};
        const std::array<u64, 1> uavs{buffer_uav(out, 0, 8)};
        context_call(67, {slot, 1, put(srvs)});    // CSSetShaderResources
        context_call(68, {0, 1, put(uavs), 0});    // CSSetUnorderedAccessViews
        context_call(69, {cs, 0, 0});              // CSSetShader
        context_call(41, {1, 1, 1});               // Dispatch
        const std::array<u64, 1> none{0};
        context_call(67, {slot, 1, put(none)});
        context_call(68, {0, 1, put(none), 0});
        const auto words = read_buffer(out, 8 * 16);
        ASSERT_EQ(words.size(), 32u);
        for (u32 i = 0; i < n; ++i) {
            const std::string what = label + "[" + std::to_string(i) + "]";
            const std::vector<u32> got(words.begin() + 4 * i,
                                       words.begin() + 4 * i + 4);
            EXPECT_EQ(got, measured(what)) << what;
        }
    }
    /// A 4x4 draw of @p ps with @p view at t@p slot into an R8G8B8A8 target;
    /// texels (0,0) and (3,3) against "<label>".
    void expect_ps(GuestAddr ps, u32 slot, GuestAddr view,
                   const std::string& label) {
        const GuestAddr target =
            texture(kSide, kSide, 1, 1, kRgba8Unorm, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET, 0);
        const GuestAddr rtv = render_target_view(target, kRgba8Unorm, 0);
        pipeline(ps);
        if (HasFatalFailure()) return;
        viewport(0, 0, kSide, kSide);
        const std::array<u64, 1> list{rtv};
        context_call(33, {1, put(list), 0});  // OMSetRenderTargets
        const std::array<float, 4> zero{0, 0, 0, 0};
        context_call(50, {rtv, put(zero)});
        const std::array<u64, 1> srvs{view};
        context_call(8, {slot, 1, put(srvs)});  // PSSetShaderResources
        draw();
        const std::array<u64, 1> none{0};
        context_call(8, {slot, 1, put(none)});
        context_call(33, {0, 0, 0});
        const auto texels =
            read_uint_level(target, kRgba8Unorm, kSide, kSide, 1, 0);
        ASSERT_EQ(texels.size(), kSide * kSide);
        EXPECT_EQ((std::vector<u32>{texels[0], texels[15]}), measured(label))
            << label;
    }

    GuestAddr m_params{0};
};

TEST_F(ArrayWindowTest, ViewWindowsAndDeclaredDimensionsMatchWarp) {
    // [rtv]: (slice s, mip m) clears to (0x10 + 0x30 s, 0x80 + 0x40 m, 0x40).
    const GuestAddr colour_tex =
        texture(kSide, kSide, 2, 3, kBgra8Unorm, D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    const GuestAddr single =
        texture(kSide, kSide, 2, 1, kBgra8Unorm, D3D11_USAGE_DEFAULT,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0);
    ASSERT_TRUE(colour_tex != 0 && single != 0);
    for (u32 s = 0; s < 3; ++s) {
        for (u32 m = 0; m < 2; ++m) {
            clear(rtv_array(colour_tex, m, s, 1), 0x10 + 0x30 * s,
                  0x80 + 0x40 * m, 0x40);
        }
    }
    for (u32 m = 0; m < 2; ++m) {
        clear(rtv_array(single, m, 0, 1), 0xE0, 0x80 + 0x40 * m, 0x40);
    }
    expect_subresources(colour_tex, kBgra8Unorm, 2, 3, "rtv cleared");

    const GuestAddr ps_cbcol = shader(15, g_ps_cbcol, sizeof(g_ps_cbcol));
    ASSERT_NE(ps_cbcol, 0u);
    struct Draw {
        u32 mip, first, size;
        float vp_w, vp_h;
        u32 r, g, b;
    };
    const Draw draws[] = {
        {0, 1, 2, 4, 4, 0x30, 0x30, 0x30},  // index 0 of the view: slice 1
        {0, 1, 1, 4, 4, 0xC0, 0x30, 0x90},
        {1, 2, 1, 2, 2, 0x20, 0xF0, 0x60},
        {0, 0, 3, 2, 4, 0x50, 0x50, 0x50},  // the left half of slice 0
    };
    pipeline(ps_cbcol);
    if (HasFatalFailure()) return;
    for (const Draw& d : draws) {
        const std::array<u64, 1> list{rtv_array(colour_tex, d.mip, d.first,
                                                d.size)};
        bind_params(colour(d.r, d.g, d.b));
        context_call(33, {1, put(list), 0});
        viewport(0, 0, d.vp_w, d.vp_h);
        draw();
        context_call(33, {0, 0, 0});
    }
    expect_subresources(colour_tex, kBgra8Unorm, 2, 3, "rtv drawn");

    // [srv]
    const GuestAddr cs_ld2d = shader(18, g_cs_ld2d, sizeof(g_cs_ld2d));
    const GuestAddr cs_ld2da = shader(18, g_cs_ld2da, sizeof(g_cs_ld2da));
    const GuestAddr ps_ld2d = shader(15, g_ps_ld2d, sizeof(g_ps_ld2d));
    const GuestAddr ps_smp2d = shader(15, g_ps_smp2d, sizeof(g_ps_smp2d));
    const GuestAddr ps_ld2da = shader(15, g_ps_ld2da, sizeof(g_ps_ld2da));
    ASSERT_TRUE(cs_ld2d && cs_ld2da && ps_ld2d && ps_smp2d && ps_ld2da);
    struct View {
        u32 mip, mips, first, size;
        const char* tag;
    };
    const View views[] = {
        {0, 2, 1, 1, "a"}, {0, 2, 1, 2, "b"}, {1, 1, 2, 1, "c"},
        {0, 1, 0, 3, "d"},
    };
    for (const View& v : views) {
        SCOPED_TRACE(v.tag);
        const std::string tag = std::string("srv ") + v.tag;
        const GuestAddr view =
            srv(colour_tex, D3D11_SRV_DIMENSION_TEXTURE2DARRAY, v.mip, v.mips,
                v.first, v.size);
        expect_cs(cs_ld2d, 0, view, 3, tag + " cs Texture2D");
        expect_cs(cs_ld2da, 1, view, 6, tag + " cs Texture2DArray");
        expect_ps(ps_ld2d, 0, view, tag + " ps Texture2D Load");
        expect_ps(ps_smp2d, 0, view, tag + " ps Texture2D Sample");
        for (u32 index = 0; index < 2; ++index) {
            std::array<u32, 16> p{};
            p[8] = index;  // u0.x
            bind_params(p);
            expect_ps(ps_ld2da, 1, view,
                      tag + " ps Texture2DArray [" + std::to_string(index) +
                          "]");
        }
    }
    // A TEXTURE2D view read through Texture2DArray (one element), a one-slice
    // 2DARRAY view through Texture2D, and a TEXTURE2D view of slice 0 of the
    // 3-slice texture (CreateShaderResourceView accepts it on Windows).
    const GuestAddr e =
        srv(single, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 2, 0, 0);
    expect_cs(cs_ld2d, 0, e, 3, "srv e cs Texture2D");
    expect_cs(cs_ld2da, 1, e, 6, "srv e cs Texture2DArray");
    bind_params({});
    expect_ps(ps_ld2da, 1, e, "srv e ps Texture2DArray [0]");
    const GuestAddr f =
        srv(single, D3D11_SRV_DIMENSION_TEXTURE2DARRAY, 0, 2, 0, 1);
    expect_cs(cs_ld2d, 0, f, 3, "srv f cs Texture2D");
    const GuestAddr g =
        srv(colour_tex, D3D11_SRV_DIMENSION_TEXTURE2D, 0, 2, 0, 0);
    expect_cs(cs_ld2d, 0, g, 3, "srv g cs Texture2D");
    expect_cs(cs_ld2da, 1, g, 6, "srv g cs Texture2DArray");

    // [uav]: slice s, texel i starts at 0x11110000 (s + 1) + i.
    const GuestAddr cs_uav2da = shader(18, g_cs_uav2da, sizeof(g_cs_uav2da));
    const GuestAddr cs_uav2d = shader(18, g_cs_uav2d, sizeof(g_cs_uav2d));
    ASSERT_TRUE(cs_uav2da && cs_uav2d);
    struct Uav {
        GuestAddr cs;
        u32 slot, first, size;
        const char* label;
    };
    const Uav uavs[] = {
        {cs_uav2da, 1, 1, 2, "uav 2DARRAY {first 1, size 2} RWTexture2DArray"},
        {cs_uav2da, 1, 2, 1, "uav 2DARRAY {first 2, size 1} RWTexture2DArray"},
        {cs_uav2d, 2, 1, 1, "uav 2DARRAY {first 1, size 1} RWTexture2D"},
    };
    for (const Uav& u : uavs) {
        SCOPED_TRACE(u.label);
        std::array<std::array<u32, 16>, 3> init{};
        std::vector<std::pair<GuestAddr, u32>> inits;
        for (u32 s = 0; s < 3; ++s) {
            for (u32 i = 0; i < 16; ++i) init[s][i] = 0x11110000u * (s + 1) + i;
            inits.emplace_back(put(init[s]), 16);
        }
        const GuestAddr tex =
            texture(kSide, kSide, 1, 3, kR32Uint, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_UNORDERED_ACCESS, 0, inits);
        const std::array<u32, 5> desc{kR32Uint,
                                      D3D11_UAV_DIMENSION_TEXTURE2DARRAY, 0,
                                      u.first, u.size};
        GuestAddr out = out_slot();
        ASSERT_EQ(device_call(8, {tex, put(desc), out}), d3d11::S_OK);
        const GuestAddr window = memory.read_u64(out).value();
        const GuestAddr dims =
            buffer(8 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                   D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, nullptr);
        std::array<u64, 3> set{buffer_uav(dims, 0, 8), 0, 0};
        set[u.slot] = window;
        context_call(68, {0, 3, put(set), 0});  // CSSetUnorderedAccessViews
        context_call(69, {u.cs, 0, 0});
        context_call(41, {1, 1, 1});
        const std::array<u64, 3> none{0, 0, 0};
        context_call(68, {0, 3, put(none), 0});
        const auto words = read_buffer(dims, 8 * 16);
        ASSERT_EQ(words.size(), 32u);
        EXPECT_EQ((std::vector<u32>{words[0], words[1], words[2]}),
                  measured(std::string(u.label) + " dims", true));
        expect_subresources(tex, kR32Uint, 1, 3, u.label);
    }
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test
