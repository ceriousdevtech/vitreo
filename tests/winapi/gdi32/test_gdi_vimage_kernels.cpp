/// @file test_gdi_vimage_kernels.cpp
/// @brief WS7: the vImage eligibility predicate and kernels, each proved
/// byte-exact against the software path (bitmap_codec / alpha_blend_checked).

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/bitmap_codec.hpp>
#include <winapi/graphics/gdi32/gdi_vimage.hpp>

#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
namespace accel = vitreo::winapi::gdi::accel;
using accel::Kernel;
using accel::Op;

namespace {

constexpr i64 kBig = i64{1} << 20;

gdi::PixelFormat fmt(u16 bpp, bool bitfields = false, std::array<u32, 3> masks = {}) {
    gdi::PixelFormat f;
    f.bpp = bpp;
    f.bitfields = bitfields;
    f.masks = masks;
    if (bpp <= 8) f.palette.assign(1u << bpp, 0x123456u);
    return f;
}

accel::Request req(Op op, const gdi::PixelFormat* f = nullptr) {
    accel::Request r;
    r.op = op;
    r.format = f;
    r.pixels = kBig;
    return r;
}

Kernel k(const accel::Request& r) { return accel::classify(r).kernel; }

}  // namespace

TEST(ClassifyTable, MatchesPlanTable) {
    accel::ScopedForce on(true);
    const auto f32 = fmt(32);
    const auto f32bf = fmt(32, true, {0x00FF0000, 0x0000FF00, 0x000000FF});
    const auto f32bf_hi = fmt(32, true, {0xFF00, 0xFF0000, 0xFF000000});
    const auto f32bf_narrow = fmt(32, true, {0x7C00, 0x3E0, 0x1F});
    const auto f32bf_dup = fmt(32, true, {0xFF, 0xFF, 0xFF00});
    const auto f24 = fmt(24);
    const auto f16 = fmt(16);
    const auto f16bf = fmt(16, true, {0xF800, 0x07E0, 0x001F});
    const auto f8 = fmt(8), f4 = fmt(4), f1 = fmt(1);

    // E1..E6, X1: whole-surface decode. The BI_RGB 32/24bpp and palette
    // rows go through the measured 1.2x gate (ClassifyTable.ConversionGate);
    // byte bitfields ship at every site.
    EXPECT_EQ(k(req(Op::Decode, &f32bf)), Kernel::Bytes32);
    EXPECT_EQ(k(req(Op::Decode, &f32bf_hi)), Kernel::Bytes32);
    EXPECT_EQ(k(req(Op::Decode, &f32bf_narrow)), Kernel::None);
    EXPECT_EQ(k(req(Op::Decode, &f32bf_dup)), Kernel::None);
    EXPECT_EQ(k(req(Op::Decode, &f16)), Kernel::None);
    EXPECT_EQ(k(req(Op::Decode, &f16bf)), Kernel::None);
    {
        auto r = req(Op::DibToSurface, &f4);
        r.first = 1;
        EXPECT_EQ(k(r), Kernel::None);
        r.first = 2;
        EXPECT_EQ(k(r), Kernel::Pal4);
        auto r1 = req(Op::DibToSurface, &f1);
        r1.first = 4;
        EXPECT_EQ(k(r1), Kernel::None);
        r1.first = 8;
        EXPECT_EQ(k(r1), Kernel::Pal1);
        auto r8 = req(Op::DibToSurface, &f8);
        r8.first = 5;
        EXPECT_EQ(k(r8), Kernel::Pal8);
    }
    // E7, E8, X2, X3: encode.
    EXPECT_EQ(k(req(Op::Encode, &f32bf)), Kernel::Bytes32);
    EXPECT_EQ(k(req(Op::Encode, &f32bf_narrow)), Kernel::None);
    EXPECT_EQ(k(req(Op::Encode, &f16)), Kernel::None);
    EXPECT_EQ(k(req(Op::Encode, &f8)), Kernel::None);
    EXPECT_EQ(k(req(Op::Encode, &f4)), Kernel::None);
    EXPECT_EQ(k(req(Op::Encode, &f1)), Kernel::None);
    // E9: quantize (the gate decides whether the kernel ships).
    EXPECT_EQ(k(req(Op::Quantize, &f16)), Kernel::None);
    EXPECT_EQ(k(req(Op::Quantize, &f8)), Kernel::None);
    EXPECT_EQ(k(req(Op::Quantize, &f1)), Kernel::None);
    EXPECT_EQ(k(req(Op::Quantize, nullptr)), Kernel::None);

    // E10: copy.
    EXPECT_EQ(k(req(Op::Copy)), Kernel::CopyRgba);
    {
        auto r = req(Op::Copy);
        r.rop_index = 0x66;
        EXPECT_EQ(k(r), Kernel::None);
        for (i32 mode = 1; mode <= 4; ++mode) {
            auto s = req(Op::Copy);
            s.stretched = true;
            s.stretch_mode = mode;
            EXPECT_EQ(k(s), Kernel::None) << mode;
        }
        auto m = req(Op::Copy);
        m.mirrored = true;
        EXPECT_EQ(k(m), Kernel::None);
        auto c = req(Op::Copy);
        c.complex_clip = true;
        EXPECT_EQ(k(c), Kernel::None);
        auto sm = req(Op::Copy);
        sm.src_mono = true;
        EXPECT_EQ(k(sm), Kernel::None);
        auto dm = req(Op::Copy);
        dm.dst_mono = true;
        EXPECT_EQ(k(dm), Kernel::None);
    }

    // E11: DIB to surface.
    EXPECT_EQ(k(req(Op::DibToSurface, &f32)), Kernel::Bgrx32);
    EXPECT_EQ(k(req(Op::DibToSurface, &f32bf)), Kernel::Bytes32);
    EXPECT_EQ(k(req(Op::DibToSurface, &f24)), Kernel::Bgr24);
    EXPECT_EQ(k(req(Op::DibToSurface, &f8)), Kernel::Pal8);
    EXPECT_EQ(k(req(Op::DibToSurface, &f4)), Kernel::Pal4);
    EXPECT_EQ(k(req(Op::DibToSurface, &f1)), Kernel::Pal1);
    EXPECT_EQ(k(req(Op::DibToSurface, &f16)), Kernel::None);
    {
        auto p = req(Op::DibToSurface, &f8);
        p.pal_colors = true;
        EXPECT_EQ(k(p), Kernel::None);
        auto d = req(Op::DibToSurface, &f32);
        d.dst_mono = true;
        EXPECT_EQ(k(d), Kernel::None);
        auto r = req(Op::DibToSurface, &f32);
        r.rop_index = 0xEE;
        EXPECT_EQ(k(r), Kernel::None);
        auto s = req(Op::DibToSurface, &f32);
        s.stretched = true;
        EXPECT_EQ(k(s), Kernel::None);
        auto m = req(Op::DibToSurface, &f32);
        m.mirrored = true;
        EXPECT_EQ(k(m), Kernel::None);
        auto c = req(Op::DibToSurface, &f32);
        c.complex_clip = true;
        EXPECT_EQ(k(c), Kernel::None);
        auto a = req(Op::DibToSurface, &f4);
        a.first = 3;
        EXPECT_EQ(k(a), Kernel::None);
    }

    // E12..E14: AlphaBlend.
    {
        auto a = req(Op::AlphaBlend);
        a.alpha_format = 1;
        a.const_alpha = 255;
        EXPECT_EQ(k(a), Kernel::AlphaPremul);
        a.const_alpha = 128;
        EXPECT_EQ(k(a), Kernel::AlphaPremulConst);
        a.const_alpha = 0;
        EXPECT_EQ(k(a), Kernel::AlphaPremulConst);
        auto c = req(Op::AlphaBlend);
        c.alpha_format = 0;
        c.const_alpha = 255;
        EXPECT_EQ(k(c), Kernel::AlphaConst);
        c.const_alpha = 7;
        EXPECT_EQ(k(c), Kernel::AlphaConst);
        auto same = a;
        same.same_surface = true;
        EXPECT_EQ(k(same), Kernel::None);
        auto op = a;
        op.blend_op = 1;
        EXPECT_EQ(k(op), Kernel::None);
        auto af = a;
        af.alpha_format = 2;
        EXPECT_EQ(k(af), Kernel::None);
        auto st = a;
        st.stretched = true;
        EXPECT_EQ(k(st), Kernel::None);
        auto cc = a;
        cc.complex_clip = true;
        EXPECT_EQ(k(cc), Kernel::None);
        auto sm = a;
        sm.src_mono = true;
        EXPECT_EQ(k(sm), Kernel::None);
        auto dm = a;
        dm.dst_mono = true;
        EXPECT_EQ(k(dm), Kernel::None);
    }

    // A null format never classifies.
    EXPECT_EQ(k(req(Op::Decode, nullptr)), Kernel::None);

    // The kill switch turns every row off.
    accel::ScopedForce off(false);
    EXPECT_FALSE(accel::enabled());
    EXPECT_EQ(k(req(Op::Decode, &f32bf)), Kernel::None);
    EXPECT_EQ(k(req(Op::Encode, &f32bf)), Kernel::None);
    {
        accel::ScopedIgnoreGate all;
        EXPECT_EQ(k(req(Op::Decode, &f32)), Kernel::None);
        EXPECT_EQ(k(req(Op::Quantize, &f24)), Kernel::None);
    }
    EXPECT_EQ(k(req(Op::Copy)), Kernel::None);
    EXPECT_EQ(k(req(Op::DibToSurface, &f8)), Kernel::None);
    auto a = req(Op::AlphaBlend);
    a.alpha_format = 1;
    EXPECT_EQ(k(a), Kernel::None);
}

TEST(ClassifyTable, ConversionKernelsExist) {
    // With the gate lifted every pixel-exact whole-surface kernel answers,
    // at every site and row order (the A/B tests prove them byte-exact).
    accel::ScopedForce on(true);
    accel::ScopedIgnoreGate all;
    const auto f32 = fmt(32), f24 = fmt(24), f16 = fmt(16);
    const auto f8 = fmt(8), f4 = fmt(4), f1 = fmt(1);
    for (const auto site : {accel::Site::DibSection, accel::Site::DibBits, accel::Site::DdbBits}) {
        for (const bool bu : {false, true}) {
            auto at = [&](Op op, const gdi::PixelFormat* f) {
                auto r = req(op, f);
                r.site = site;
                r.bottom_up = bu;
                return k(r);
            };
            EXPECT_EQ(at(Op::Decode, &f32), Kernel::Bgrx32);
            EXPECT_EQ(at(Op::Decode, &f24), Kernel::Bgr24);
            EXPECT_EQ(at(Op::Decode, &f8), Kernel::Pal8);
            EXPECT_EQ(at(Op::Decode, &f4), Kernel::Pal4);
            EXPECT_EQ(at(Op::Decode, &f1), Kernel::Pal1);
            EXPECT_EQ(at(Op::Decode, &f16), Kernel::None);
            EXPECT_EQ(at(Op::Encode, &f32), Kernel::Bgrx32);
            EXPECT_EQ(at(Op::Encode, &f24), Kernel::Bgr24);
            EXPECT_EQ(at(Op::Encode, &f8), Kernel::None);
            EXPECT_EQ(at(Op::Quantize, &f24), Kernel::ClearFourth);
            EXPECT_EQ(at(Op::Quantize, &f16), Kernel::None);
        }
    }
}

TEST(ClassifyTable, ConversionGate) {
    accel::ScopedForce on(true);
    using accel::Site;
    const auto f32 = fmt(32), f24 = fmt(24), f8 = fmt(8), f4 = fmt(4), f1 = fmt(1);
    auto at = [](Op op, Site site, const gdi::PixelFormat* f, bool bu, i64 pixels) {
        auto r = req(op, f);
        r.site = site;
        r.bottom_up = bu;
        r.pixels = pixels;
        return k(r);
    };
    // The measured table (gdi_vimage.cpp kConversions): each shipped row at
    // its crossover and one pixel below it; each software row at any size.
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f32, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f32, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f24, false, 1024 * 1024), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f24, false, 1024 * 1024 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f24, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f8, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f8, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f4, false, 1920 * 1080), Kernel::Pal4);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f4, false, 1920 * 1080 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f4, true, 1024 * 1024), Kernel::Pal4);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f4, true, 1024 * 1024 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f1, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibSection, &f1, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibSection, &f32, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibSection, &f32, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibSection, &f24, false, 2 * 2), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Encode, Site::DibSection, &f24, false, 2 * 2 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibSection, &f24, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f32, false, 4 * 4), Kernel::Bgrx32);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f32, false, 4 * 4 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f32, true, 32 * 32), Kernel::Bgrx32);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f32, true, 32 * 32 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f24, false, 16 * 16), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f24, false, 16 * 16 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f24, true, 64 * 64), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f24, true, 64 * 64 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f8, false, 128 * 128), Kernel::Pal8);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f8, false, 128 * 128 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f8, true, 1024 * 1024), Kernel::Pal8);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f8, true, 1024 * 1024 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f4, false, 64 * 64), Kernel::Pal4);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f4, false, 64 * 64 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f4, true, 1024 * 1024), Kernel::Pal4);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f4, true, 1024 * 1024 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f1, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DibBits, &f1, true, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f32, false, 1 * 1), Kernel::Bgrx32);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f32, false, 1 * 1 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f32, true, 1 * 1), Kernel::Bgrx32);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f32, true, 1 * 1 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f24, false, 1 * 1), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f24, false, 1 * 1 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f24, true, 1 * 1), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Encode, Site::DibBits, &f24, true, 1 * 1 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DdbBits, &f32, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DdbBits, &f24, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DdbBits, &f8, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DdbBits, &f4, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Decode, Site::DdbBits, &f1, false, kBig), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DdbBits, &f32, false, 1 * 1), Kernel::Bgrx32);
    EXPECT_EQ(at(Op::Encode, Site::DdbBits, &f32, false, 1 * 1 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Encode, Site::DdbBits, &f24, false, 2 * 2), Kernel::Bgr24);
    EXPECT_EQ(at(Op::Encode, Site::DdbBits, &f24, false, 2 * 2 - 1), Kernel::None);
    EXPECT_EQ(at(Op::Quantize, Site::DdbBits, &f24, false, 1 * 1), Kernel::ClearFourth);
    EXPECT_EQ(at(Op::Quantize, Site::DdbBits, &f24, false, 1 * 1 - 1), Kernel::None);
    // Total: a row nobody measured (a bottom-up DDB) answers software, the
    // software reason carries the measured ratio, and min_pixels(Request)
    // matches what classify applies.
    EXPECT_EQ(at(Op::Encode, Site::DdbBits, &f24, true, kBig), Kernel::None);
    {
        auto r = req(Op::Decode, &f32);
        r.site = Site::DibSection;
        EXPECT_NE(std::string(accel::classify(r).reason).find("1.2x gate"), std::string::npos);
        EXPECT_EQ(accel::min_pixels(r), 0);
        auto q = req(Op::Encode, &f24);
        q.site = Site::DibBits;
        EXPECT_EQ(accel::min_pixels(q), 1);
    }
}

TEST(ClassifyTable, UnscaledIgnoresStretchMode) {
    accel::ScopedForce on(true);
    for (i32 mode = 1; mode <= 4; ++mode) {
        auto r = req(Op::Copy);
        r.stretch_mode = mode;
        EXPECT_EQ(k(r), Kernel::CopyRgba) << mode;
    }
}

TEST(ClassifyTable, BelowCrossoverIsSoftware) {
    accel::ScopedForce on(true);
    for (u8 i = 1; i < static_cast<u8>(Kernel::Count); ++i) {
        const auto kernel = static_cast<Kernel>(i);
        EXPECT_GE(accel::min_pixels(kernel), 1) << int(i);
    }
    auto r = req(Op::Copy);
    r.pixels = accel::min_pixels(Kernel::CopyRgba) - 1;
    EXPECT_EQ(k(r), Kernel::None);
    r.pixels = accel::min_pixels(Kernel::CopyRgba);
    EXPECT_EQ(k(r), Kernel::CopyRgba);
}

TEST(KillSwitch, ScopedForceNests) {
    const bool initial = accel::enabled();
    {
        accel::ScopedForce on(true);
        EXPECT_TRUE(accel::enabled());
        {
            accel::ScopedForce off(false);
            EXPECT_FALSE(accel::enabled());
        }
        EXPECT_TRUE(accel::enabled());
    }
    EXPECT_EQ(accel::enabled(), initial);
}

// =============================================================================
// Task 2: row codecs against bitmap_codec
// =============================================================================

namespace {

std::mt19937& rng() {
    static std::mt19937 g(0x5737);
    return g;
}

void fill_random(std::vector<u8>& v) {
    for (auto& b : v) b = static_cast<u8>(rng()() & 0xFFu);
}

/// Bytes of a run of @p count pixels starting at pixel @p first.
usize run_bytes(u16 bpp, i32 first, i32 count) {
    return (static_cast<usize>(first + count) * bpp + 7u) / 8u;
}

struct DecodeCase {
    const char* name;
    gdi::PixelFormat f;
    std::vector<i32> firsts;
};

std::vector<DecodeCase> decode_cases() {
    std::vector<DecodeCase> out;
    out.push_back({"32", fmt(32), {0, 5}});
    out.push_back({"32bf-bgr", fmt(32, true, {0x00FF0000, 0x0000FF00, 0x000000FF}), {0, 5}});
    out.push_back({"32bf-rgb", fmt(32, true, {0x000000FF, 0x0000FF00, 0x00FF0000}), {0, 5}});
    out.push_back({"32bf-hi", fmt(32, true, {0x0000FF00, 0x00FF0000, 0xFF000000}), {0}});
    out.push_back({"24", fmt(24), {0, 5}});
    for (const usize n : {usize{2}, usize{100}, usize{256}}) {
        DecodeCase c{"8", fmt(8), {0, 5}};
        c.f.palette.resize(n);
        for (auto& e : c.f.palette) e = rng()() & 0x00FFFFFFu;
        out.push_back(std::move(c));
    }
    {
        DecodeCase c{"4", fmt(4), {0, 2}};
        for (auto& e : c.f.palette) e = rng()() & 0x00FFFFFFu;
        out.push_back(std::move(c));
        DecodeCase c7{"4-short", fmt(4), {0}};
        c7.f.palette.resize(7);
        for (auto& e : c7.f.palette) e = rng()() & 0x00FFFFFFu;
        out.push_back(std::move(c7));
    }
    {
        DecodeCase c{"1", fmt(1), {0, 8}};
        c.f.palette = {0x00123456u, 0x00FEDCBAu};
        out.push_back(std::move(c));
    }
    return out;
}

/// Decode through vImage and through gdi::decode_row; compare whole buffers
/// (so canary bytes past the run must also match).
void check_decode(const DecodeCase& c, i32 width, i32 rows, usize stride_extra, usize dst_extra,
                  i32 first, bool opaque, bool negative) {
    accel::Request r = req(Op::DibToSurface, &c.f);
    r.first = first;
    const Kernel kernel = accel::classify(r).kernel;
    ASSERT_NE(kernel, Kernel::None) << c.name;
    const usize min_stride = run_bytes(c.f.bpp, first, width);
    const usize stride = min_stride + stride_extra;
    std::vector<u8> src(stride * static_cast<usize>(rows));
    fill_random(src);
    const usize dst_stride = static_cast<usize>(width) * 4u + dst_extra;
    std::vector<u8> want(dst_stride * static_cast<usize>(rows), 0xA5);
    std::vector<u8> got = want;
    const u8* base = negative ? src.data() + stride * static_cast<usize>(rows - 1) : src.data();
    const std::ptrdiff_t sstride = negative ? -static_cast<std::ptrdiff_t>(stride)
                                            : static_cast<std::ptrdiff_t>(stride);
    for (i32 y = 0; y < rows; ++y) {
        u8* d = want.data() + static_cast<usize>(y) * dst_stride;
        gdi::decode_row(c.f, base + y * sstride, d, width, first);
        if (opaque) {
            for (i32 x = 0; x < width; ++x) d[x * 4 + 3] = 0xFF;
        }
    }
    ASSERT_TRUE(accel::decode_rows(kernel, c.f, base, sstride, got.data(), dst_stride, width, rows,
                                   first, opaque));
    ASSERT_EQ(got, want) << c.name << " w=" << width << " rows=" << rows << " stride+"
                         << stride_extra << " dst+" << dst_extra << " first=" << first
                         << " opaque=" << opaque << " neg=" << negative;
}

}  // namespace

TEST(DecodeRows, MatchesSoftware) {
    accel::ScopedForce on(true);
    for (const auto& c : decode_cases()) {
        for (const i32 width : {1, 2, 3, 7, 8, 9, 31, 33, 1920, 1921}) {
            for (const i32 rows : {1, 3, 17}) {
                for (const usize sx : {usize{0}, usize{4}, usize{64}}) {
                    for (const usize dx : {usize{0}, usize{16}}) {
                        for (const i32 first : c.firsts) {
                            for (const bool opaque : {false, true}) {
                                check_decode(c, width, rows, sx, dx, first, opaque, false);
                                if (HasFatalFailure()) return;
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(DecodeRows, NegativeStrideMatchesSoftware) {
    accel::ScopedForce on(true);
    for (const auto& c : decode_cases()) {
        for (const i32 width : {1, 3, 9, 33, 1921}) {
            for (const i32 rows : {1, 3, 17}) {
                for (const usize sx : {usize{0}, usize{4}}) {
                    for (const i32 first : c.firsts) {
                        for (const bool opaque : {false, true}) {
                            check_decode(c, width, rows, sx, 16, first, opaque, true);
                            if (HasFatalFailure()) return;
                        }
                    }
                }
            }
        }
    }
}

TEST(EncodeRows, MatchesSoftwareAndPreservesPadding) {
    accel::ScopedForce on(true);
    accel::ScopedIgnoreGate all;
    const gdi::PixelFormat formats[] = {
        fmt(32),
        fmt(24),
        fmt(32, true, {0x00FF0000, 0x0000FF00, 0x000000FF}),
        fmt(32, true, {0x000000FF, 0x0000FF00, 0x00FF0000}),
        fmt(32, true, {0x0000FF00, 0x00FF0000, 0xFF000000}),
        fmt(32, true, {0xFF000000, 0x000000FF, 0x00FF0000}),
    };
    for (const auto& f : formats) {
        const Kernel kernel = accel::classify(req(Op::Encode, &f)).kernel;
        ASSERT_NE(kernel, Kernel::None);
        for (const i32 width : {1, 2, 3, 7, 33, 1920, 1921}) {
            for (const i32 rows : {1, 3, 17}) {
                for (const usize sx : {usize{0}, usize{4}, usize{64}}) {
                    for (const bool negative : {false, true}) {
                        const usize stride = run_bytes(f.bpp, 0, width) + sx;
                        const usize src_stride = static_cast<usize>(width) * 4u + 8u;
                        std::vector<u8> rgba(src_stride * static_cast<usize>(rows));
                        fill_random(rgba);
                        std::vector<u8> want(stride * static_cast<usize>(rows));
                        fill_random(want);
                        std::vector<u8> got = want;
                        const std::ptrdiff_t ds = negative ? -static_cast<std::ptrdiff_t>(stride)
                                                           : static_cast<std::ptrdiff_t>(stride);
                        const usize base_off = negative ? stride * static_cast<usize>(rows - 1) : 0;
                        for (i32 y = 0; y < rows; ++y) {
                            gdi::encode_row(f, rgba.data() + static_cast<usize>(y) * src_stride,
                                            want.data() + base_off + y * ds, width);
                        }
                        ASSERT_TRUE(accel::encode_rows(kernel, f, rgba.data(), src_stride,
                                                       got.data() + base_off, ds, width, rows));
                        ASSERT_EQ(got, want) << f.bpp << " bf=" << f.bitfields << " w=" << width
                                             << " rows=" << rows << " neg=" << negative;
                    }
                }
            }
        }
    }
}

TEST(ClearFourth, MatchesQuantize24) {
    accel::ScopedForce on(true);
    const gdi::PixelFormat f = gdi::ddb_pixel_format(24);
    for (const i32 width : {1, 3, 33, 1921}) {
        const i32 rows = 5;
        const usize stride = static_cast<usize>(width) * 4u + 12u;
        std::vector<u8> want(stride * rows);
        fill_random(want);
        std::vector<u8> got = want;
        std::vector<u8> packed(gdi::ddb_stride(width, 24) + 4u);
        for (i32 y = 0; y < rows; ++y) {
            std::fill(packed.begin(), packed.end(), u8{0});
            u8* row = want.data() + static_cast<usize>(y) * stride;
            gdi::encode_row(f, row, packed.data(), width);
            gdi::decode_row(f, packed.data(), row, width);
        }
        ASSERT_TRUE(accel::clear_fourth_byte(got.data(), stride, width, rows));
        ASSERT_EQ(got, want) << width;
    }
}

TEST(Kernels, CountersAdvance) {
    accel::ScopedForce on(true);
    const auto f = fmt(24);
    std::vector<u8> src(3 * 4), dst(4 * 4);
    const u64 before = accel::kernel_runs(Kernel::Bgr24);
    ASSERT_TRUE(accel::decode_rows(Kernel::Bgr24, f, src.data(), 12, dst.data(), 16, 4, 1, 0, false));
    EXPECT_EQ(accel::kernel_runs(Kernel::Bgr24), before + 1);
    ASSERT_TRUE(accel::encode_rows(Kernel::Bgr24, f, dst.data(), 16, src.data(), 12, 4, 1));
    EXPECT_EQ(accel::kernel_runs(Kernel::Bgr24), before + 2);
    const u64 clears = accel::kernel_runs(Kernel::ClearFourth);
    ASSERT_TRUE(accel::clear_fourth_byte(dst.data(), 16, 4, 1));
    EXPECT_EQ(accel::kernel_runs(Kernel::ClearFourth), clears + 1);
}

// =============================================================================
// Task 3: copy and AlphaBlend kernels, exhaustive proofs
// =============================================================================

namespace {

// The two formulas of GdiManager::alpha_blend_checked, verbatim.
u32 round255(u32 v) { return (v + 127u) / 255u; }

u32 ref_premul(u32 s, u32 d, u32 ca) {
    u32 sp[4];
    for (u32 c = 0; c < 4; ++c) sp[c] = round255(((s >> (8 * c)) & 0xFFu) * ca);
    const u32 inv = 255u - sp[3];
    u32 out = 0;
    for (u32 c = 0; c < 4; ++c) {
        const u32 v = sp[c] + round255(((d >> (8 * c)) & 0xFFu) * inv);
        out |= std::min(v, 255u) << (8 * c);
    }
    return out;
}

u32 ref_const(u32 s, u32 d, u32 ca) {
    u32 out = 0;
    for (u32 c = 0; c < 4; ++c) {
        const u32 v = round255(((s >> (8 * c)) & 0xFFu) * ca + ((d >> (8 * c)) & 0xFFu) * (255u - ca));
        out |= v << (8 * c);
    }
    return out;
}

u32 load32(const u8* p) {
    u32 v;
    std::memcpy(&v, p, 4);
    return v;
}
void store32(u8* p, u32 v) { std::memcpy(p, &v, 4); }

/// A 4096x4096 image pair covering every (sa, sc, dc): pixel n has source
/// colour channels sc = (n >> 8) & 255, source alpha sa = n >> 16, and every
/// destination byte dc = n & 255 (so the destination alpha also sees every
/// (sa, da) pair).
void exhaustive_pair(std::vector<u8>& src, std::vector<u8>& dst, u32 variant) {
    constexpr usize kN = usize{1} << 24;
    src.resize(kN * 4u);
    dst.resize(kN * 4u);
    for (usize n = 0; n < kN; ++n) {
        const u8 sa = static_cast<u8>(n >> 16), sc = static_cast<u8>(n >> 8), dc = static_cast<u8>(n);
        // Rotate which colour channel carries sc so channel position is
        // exercised too; the other two carry the complements.
        u8* s = src.data() + n * 4u;
        s[0] = s[1] = s[2] = sc;
        s[(variant + n) % 3] = static_cast<u8>(255 - sc);
        s[3] = sa;
        u8* d = dst.data() + n * 4u;
        d[0] = d[1] = d[2] = d[3] = dc;
    }
}

void check_blend(Kernel kernel, u8 ca, bool per_pixel) {
    std::vector<u8> src, dst;
    exhaustive_pair(src, dst, ca);
    std::vector<u8> want(dst.size());
    for (usize n = 0; n < src.size() / 4u; ++n) {
        const u32 s = load32(src.data() + n * 4u), d = load32(dst.data() + n * 4u);
        store32(want.data() + n * 4u, per_pixel ? ref_premul(s, d, ca) : ref_const(s, d, ca));
    }
    ASSERT_TRUE(accel::alpha_blend_rect(kernel, src.data(), 4096u * 4u, dst.data(), 4096u * 4u,
                                        4096, 4096, ca));
    for (usize n = 0; n < src.size() / 4u; ++n) {
        if (std::memcmp(dst.data() + n * 4u, want.data() + n * 4u, 4) != 0) {
            const u32 s = load32(src.data() + n * 4u);
            FAIL() << "kernel " << int(kernel) << " ca=" << int(ca) << " n=" << n << " src=" << std::hex
                   << s << " vImage=" << load32(dst.data() + n * 4u)
                   << " GDI=" << load32(want.data() + n * 4u);
        }
    }
}

}  // namespace

TEST(AlphaPremul, ExhaustiveSca255) {
    accel::ScopedForce on(true);
    check_blend(Kernel::AlphaPremul, 255, true);
}

TEST(AlphaPremulConst, MatrixStageExhaustive) {
    // Every (SCA, s) pair of the premultiply stage: over a zero destination
    // the blend adds round255(0 * inv) = 0, so the result is exactly
    // sp_c = round255(s_c * SCA) on all four bytes.
    accel::ScopedForce on(true);
    for (u32 ca = 0; ca < 255; ++ca) {
        std::vector<u8> src(256 * 4), dst(256 * 4, 0);
        for (u32 s = 0; s < 256; ++s) {
            for (u32 c = 0; c < 4; ++c) src[s * 4 + c] = static_cast<u8>(s);
        }
        ASSERT_TRUE(accel::alpha_blend_rect(Kernel::AlphaPremulConst, src.data(), 1024, dst.data(),
                                            1024, 256, 1, static_cast<u8>(ca)));
        for (u32 s = 0; s < 256; ++s) {
            for (u32 c = 0; c < 4; ++c) {
                ASSERT_EQ(dst[s * 4 + c], round255(s * ca)) << "ca=" << ca << " s=" << s;
            }
        }
    }
}

TEST(AlphaPremulConst, EndToEndSampled) {
    accel::ScopedForce on(true);
    for (const u8 ca : {u8{0}, u8{1}, u8{127}, u8{128}, u8{254}}) {
        check_blend(Kernel::AlphaPremulConst, ca, true);
        if (HasFatalFailure()) return;
    }
}

TEST(AlphaConst, Exhaustive) {
    // Every (SCA, s, d): 256 images of 256x256, one per SCA, all 4 bytes.
    accel::ScopedForce on(true);
    std::vector<u8> src(256 * 256 * 4), dst(src.size());
    for (u32 ca = 0; ca < 256; ++ca) {
        for (u32 s = 0; s < 256; ++s) {
            for (u32 d = 0; d < 256; ++d) {
                const usize n = (s * 256 + d) * 4u;
                for (u32 c = 0; c < 4; ++c) {
                    src[n + c] = static_cast<u8>(s ^ (c * 0x55));
                    dst[n + c] = static_cast<u8>(d ^ (c * 0x33));
                }
            }
        }
        std::vector<u8> want(dst.size());
        for (usize n = 0; n < src.size(); n += 4) {
            store32(want.data() + n, ref_const(load32(src.data() + n), load32(dst.data() + n), ca));
        }
        ASSERT_TRUE(accel::alpha_blend_rect(Kernel::AlphaConst, src.data(), 1024, dst.data(), 1024,
                                            256, 256, static_cast<u8>(ca)));
        ASSERT_EQ(dst, want) << "ca=" << ca;
    }
}

TEST(CopyRgba, OverlapAllDirections) {
    accel::ScopedForce on(true);
    constexpr i32 W = 64, H = 64, w = 37, h = 29, x0 = 13, y0 = 17;
    const usize stride = W * 4u;
    for (i32 dy = -3; dy <= 3; ++dy) {
        for (i32 dx = -3; dx <= 3; ++dx) {
            for (const bool opaque : {false, true}) {
                std::vector<u8> buf(stride * H);
                fill_random(buf);
                std::vector<u8> want = buf;
                const std::vector<u8> snap = buf;
                for (i32 y = 0; y < h; ++y) {
                    for (i32 x = 0; x < w; ++x) {
                        u8* d = want.data() + static_cast<usize>(((y0 + dy + y) * W + x0 + dx + x) * 4);
                        std::memcpy(d, snap.data() + static_cast<usize>(((y0 + y) * W + x0 + x) * 4), 4);
                        if (opaque) d[3] = 0xFF;
                    }
                }
                const u8* src = buf.data() + static_cast<usize>((y0 * W + x0) * 4);
                u8* dst = buf.data() + static_cast<usize>(((y0 + dy) * W + x0 + dx) * 4);
                ASSERT_TRUE(accel::copy_rgba_rect(src, stride, dst, stride, w, h, true, opaque));
                ASSERT_EQ(buf, want) << dx << "," << dy << " opaque=" << opaque;
            }
        }
    }
    // Distinct surfaces, unaligned rect.
    std::vector<u8> a(stride * H), b(stride * H);
    fill_random(a);
    fill_random(b);
    std::vector<u8> want = b;
    for (i32 y = 0; y < h; ++y) {
        std::memcpy(want.data() + (static_cast<usize>(y + 3) * W + 5) * 4u,
                    a.data() + (static_cast<usize>(y + 1) * W + 2) * 4u, w * 4u);
    }
    ASSERT_TRUE(accel::copy_rgba_rect(a.data() + (W + 2) * 4u, stride, b.data() + (3 * W + 5) * 4u,
                                      stride, w, h, false, false));
    EXPECT_EQ(b, want);
}
