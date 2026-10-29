// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_region.cpp
/// @brief Contract tests for the GDI+ region area (gdiplus_region.cpp), run in
/// both bitnesses through gdiplus_fixture.hpp. Expected values come from
/// tools/oracle_probe/gdiplus_region_probe{32,64}.expected (Windows 11).

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>
#include <winapi/graphics/gdiplus/gdiplus_pen_brush.hpp>
#include <winapi/graphics/gdiplus/gdiplus_region.hpp>

#include <array>

namespace vitreo::winapi::gdip_test {
namespace {

using Rect4 = std::array<i32, 4>;

class Gdip_region_Test : public GdipTest {
protected:
    static constexpr GuestAddr RECT = BUF;
    static constexpr GuestAddr DBUF = BUF + 0x2000;  ///< region data / scans buffer

    u64 region_rect(float x, float y, float w, float h) {
        write_reals(RECT, {x, y, w, h});
        EXPECT_EQ(call("GdipCreateRegionRect", {I(RECT), I(OUT)}), Status::Ok);
        return read_handle(OUT);
    }
    u64 region_infinite() {
        EXPECT_EQ(call("GdipCreateRegion", {I(OUT)}), Status::Ok);
        return read_handle(OUT);
    }
    u64 region_empty() {
        const u64 r = region_infinite();
        EXPECT_EQ(call("GdipSetEmpty", {I(r)}), Status::Ok);
        return r;
    }
    void combine_rect(u64 r, float x, float y, float w, float h, i32 mode) {
        write_reals(RECT, {x, y, w, h});
        EXPECT_EQ(call("GdipCombineRegionRect", {I(r), I(RECT), I(u64(mode))}), Status::Ok);
    }
    u64 matrix(float a, float b, float c, float d, float e, float f) {
        return mgr.objects().add(gdip::GpMatrix{gdip::Affine{a, b, c, d, e, f}});
    }
    u64 path(std::vector<gdip::PointF> pts, std::vector<u8> types, i32 fill = 0) {
        gdip::GpPath p;
        p.points = std::move(pts);
        p.types = std::move(types);
        p.fill_mode = fill;
        return mgr.objects().add(std::move(p));
    }
    u64 rect_path(float x, float y, float w, float h, i32 fill = 0) {
        return path({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, {0, 1, 1, 0x81}, fill);
    }
    /// The 13-point ellipse GDI+ builds for AddPathEllipse(x, y, 10, 10)
    /// (probe "ellipse" / "rp.*" region data).
    u64 ellipse10(float x, float y) {
        const float k = 2.7614237f;  // 5 * 0.5522847
        std::vector<gdip::PointF> p = {
            {10, 5}, {10, 5 + k}, {5 + k, 10}, {5, 10}, {5 - k, 10}, {0, 5 + k}, {0, 5},
            {0, 5 - k}, {5 - k, 0}, {5, 0}, {5 + k, 0}, {10, 5 - k}, {10, 5}};
        for (auto& q : p) { q.X += x; q.Y += y; }
        return path(p, {0, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0x83});
    }
    /// A 32bppARGB bitmap graphics of @p w x @p h; @p image receives the image.
    u64 graphics(i32 w, i32 h, u64* image = nullptr) {
        gdip::GpImage img;
        img.width = w;
        img.height = h;
        img.pixel_format = gdip::PixelFormat32bppARGB;
        img.stride = w * 4;
        img.bits.assign(static_cast<usize>(w * h * 4), 0);
        const u64 ih = mgr.objects().add(std::move(img));
        if (image) *image = ih;
        gdip::GpGraphics g;
        g.kind = gdip::TargetKind::Image;
        g.image = ih;
        return mgr.objects().add(std::move(g));
    }
    gdip::GpGraphics& G(u64 h) { return *mgr.objects().get<gdip::GpGraphics>(h); }
    void world(u64 g, float a, float b, float c, float d, float e, float f) {
        G(g).state.world = gdip::Affine{a, b, c, d, e, f};
    }
    std::vector<u32> data(u64 r) {
        EXPECT_EQ(call("GdipGetRegionDataSize", {I(r), I(OUT2)}), Status::Ok);
        const u32 n = memory.read_u32(OUT2).value();
        EXPECT_EQ(call("GdipGetRegionData", {I(r), I(DBUF), I(n), I(OUT3)}), Status::Ok);
        EXPECT_EQ(memory.read_u32(OUT3).value(), n);
        std::vector<u32> w;
        for (u32 i = 0; i < n; i += 4) w.push_back(memory.read_u32(DBUF + i).value());
        return w;
    }
    std::vector<Rect4> scans(u64 r, u64 m) {
        EXPECT_EQ(call("GdipGetRegionScansCount", {I(r), I(OUT2), I(m)}), Status::Ok);
        const u32 n = memory.read_u32(OUT2).value();
        EXPECT_EQ(call("GdipGetRegionScansI", {I(r), I(DBUF), I(OUT3), I(m)}), Status::Ok);
        EXPECT_EQ(memory.read_u32(OUT3).value(), n);
        std::vector<Rect4> v;
        for (u32 i = 0; i < n; ++i)
            v.push_back({read_int(DBUF + i * 16), read_int(DBUF + i * 16 + 4), read_int(DBUF + i * 16 + 8),
                         read_int(DBUF + i * 16 + 12)});
        return v;
    }
    std::array<float, 4> bounds(u64 r, u64 g) {
        EXPECT_EQ(call("GdipGetRegionBounds", {I(r), I(g), I(OUT2)}), Status::Ok);
        return {read_real(OUT2), read_real(OUT2 + 4), read_real(OUT2 + 8), read_real(OUT2 + 12)};
    }
    Rect4 boundsI(u64 r, u64 g) {
        EXPECT_EQ(call("GdipGetRegionBoundsI", {I(r), I(g), I(OUT2)}), Status::Ok);
        return {read_int(OUT2), read_int(OUT2 + 4), read_int(OUT2 + 8), read_int(OUT2 + 12)};
    }
    bool flag(const char* api, u64 r, u64 g) {
        poison(OUT3, 4);
        EXPECT_EQ(call(api, {I(r), I(g), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    }
    bool vis(u64 r, float x, float y, u64 g = 0) {
        poison(OUT3, 4);
        EXPECT_EQ(call("GdipIsVisibleRegionPoint", {I(r), R(x), R(y), I(g), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    }
    bool visrect(u64 r, float x, float y, float w, float h, u64 g = 0) {
        poison(OUT3, 4);
        EXPECT_EQ(call("GdipIsVisibleRegionRect", {I(r), R(x), R(y), R(w), R(h), I(g), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    }
    std::array<float, 4> getf(const char* api, u64 g) {
        poison(OUT2, 16);
        EXPECT_EQ(call(api, {I(g), I(OUT2)}), Status::Ok);
        return {read_real(OUT2), read_real(OUT2 + 4), read_real(OUT2 + 8), read_real(OUT2 + 12)};
    }
    Rect4 geti(const char* api, u64 g) {
        poison(OUT2, 16);
        EXPECT_EQ(call(api, {I(g), I(OUT2)}), Status::Ok);
        return {read_int(OUT2), read_int(OUT2 + 4), read_int(OUT2 + 8), read_int(OUT2 + 12)};
    }
    bool gflag(const char* api, u64 g) {
        poison(OUT3, 4);
        EXPECT_EQ(call(api, {I(g), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    }
    std::vector<u32> clip_data(u64 g) {
        const u64 r = region_rect(0, 0, 1, 1);
        EXPECT_EQ(call("GdipGetClip", {I(g), I(r)}), Status::Ok);
        return data(r);
    }
    bool gvis(u64 g, float x, float y) {
        poison(OUT3, 4);
        EXPECT_EQ(call("GdipIsVisiblePoint", {I(g), R(x), R(y), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    }
    bool gvisrect(u64 g, float x, float y, float w, float h) {
        poison(OUT3, 4);
        EXPECT_EQ(call("GdipIsVisibleRect", {I(g), R(x), R(y), R(w), R(h), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    }
    void setclip(u64 g, float x, float y, float w, float h, i32 mode) {
        EXPECT_EQ(call("GdipSetClipRect", {I(g), R(x), R(y), R(w), R(h), I(u64(mode))}), Status::Ok);
    }
    u32 pixel(u64 image, i32 x, i32 y) {
        const auto* img = mgr.objects().get<gdip::GpImage>(image);
        const usize i = static_cast<usize>(y * img->stride + x * 4);
        return u32(img->bits[i]) | (u32(img->bits[i + 1]) << 8) | (u32(img->bits[i + 2]) << 16) |
               (u32(img->bits[i + 3]) << 24);
    }
    /// Columns [x0, x1] of row @p y holding a non-zero pixel ({-1,-1} if none).
    std::pair<i32, i32> row_extent(u64 image, i32 y, i32 width) {
        i32 a = -1, b = -1;
        for (i32 x = 0; x < width; ++x)
            if (pixel(image, x, y) != 0) { if (a < 0) a = x; b = x; }
        return {a, b};
    }
};

// ---- Lifetime and validation ------------------------------------------------

TEST_P(Gdip_region_Test, ErrorStatuses) {
    EXPECT_EQ(call("GdipCreateRegion", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateRegionRect", {I(0), I(OUT)}), Status::InvalidParameter);
    write_reals(RECT, {0, 0, 10, 10});
    EXPECT_EQ(call("GdipCreateRegionRect", {I(RECT), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateRegionPath", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteRegion", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneRegion", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetEmpty", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetInfinite", {I(0)}), Status::InvalidParameter);
    const u64 r = region_rect(0, 0, 10, 10);
    EXPECT_EQ(call("GdipCloneRegion", {I(r), I(0)}), Status::InvalidParameter);
    write_reals(RECT, {0, 0, 10, 10});
    for (i32 mode : {-1, 6, 7}) {
        EXPECT_EQ(call("GdipCombineRegionRect", {I(r), I(RECT), I(u64(u32(mode)))}), Status::InvalidParameter);
    }
    for (i32 mode = 0; mode <= 5; ++mode)
        EXPECT_EQ(call("GdipCombineRegionRect", {I(r), I(RECT), I(u64(mode))}), Status::Ok);
    EXPECT_EQ(call("GdipCombineRegionRect", {I(r), I(0), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCombineRegionPath", {I(r), I(0), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCombineRegionRegion", {I(r), I(0), I(1)}), Status::InvalidParameter);
    // Self-combine is ObjectBusy before the mode is checked.
    EXPECT_EQ(call("GdipCombineRegionRegion", {I(r), I(r), I(2)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipCombineRegionRegion", {I(r), I(r), I(8)}), Status::ObjectBusy);
    const u64 g = graphics(10, 10);
    EXPECT_EQ(call("GdipGetRegionBounds", {I(r), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionBounds", {I(r), I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsEmptyRegion", {I(r), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsInfiniteRegion", {I(r), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsEqualRegion", {I(r), I(r), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsVisibleRegionPoint", {I(r), R(5), R(5), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionScansCount", {I(r), I(OUT2), I(0)}), Status::InvalidParameter);
    const u64 id = matrix(1, 0, 0, 1, 0, 0);
    EXPECT_EQ(call("GdipGetRegionScansCount", {I(r), I(0), I(id)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionScans", {I(r), I(DBUF), I(0), I(id)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionScans", {I(r), I(0), I(OUT2), I(id)}), Status::Ok);
    EXPECT_EQ(call("GdipGetRegionDataSize", {I(r), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionData", {I(r), I(0), I(64), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionData", {I(r), I(DBUF), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetRegionHRgn", {I(r), I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateRegionHrgn", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateRegionHrgn", {I(0x12345678), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformRegion", {I(r), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipFillRegion", {I(g), I(0), I(r)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsVisiblePathPoint", {I(0), R(1), R(1), I(0), I(OUT2)}), Status::InvalidParameter);
    // Wrong object type through a region handle.
    EXPECT_EQ(call("GdipSetEmpty", {I(g)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteRegion", {I(r)}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteRegion", {I(r)}), Status::InvalidParameter);
}

// ---- Serialized form ---------------------------------------------------------

TEST_P(Gdip_region_Test, DataOfLeaves) {
    const u64 inf = region_infinite();
    EXPECT_EQ(data(inf), (std::vector<u32>{0x0c, 0xa322349b, 0xdbc01002, 0, 0x10000003}));
    const u64 em = region_empty();
    EXPECT_EQ(data(em), (std::vector<u32>{0x0c, 0x1b9e53fe, 0xdbc01002, 0, 0x10000002}));
    const u64 r = region_rect(1.5f, 2.5f, 10, 20);
    EXPECT_EQ(data(r), (std::vector<u32>{0x1c, 0x35485608, 0xdbc01002, 0, 0x10000000, 0x3fc00000,
                                         0x40200000, 0x41200000, 0x41a00000}));
    write_ints(RECT, {1, 2, 3, 4});
    ASSERT_EQ(call("GdipCreateRegionRectI", {I(RECT), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)), (std::vector<u32>{0x1c, 0xecbb90f7, 0xdbc01002, 0, 0x10000000,
                                                        0x3f800000, 0x40000000, 0x40400000, 0x40800000}));
    // A negative extent is kept as given.
    EXPECT_EQ(data(region_rect(10, 10, -5, -5))[7], 0xc0a00000u);
    // Integral path: INT16 points (flag 0x4000), types padded to 4.
    ASSERT_EQ(call("GdipCreateRegionPath", {I(rect_path(1, 2, 3, 4)), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)),
              (std::vector<u32>{0x30, 0x366e7052, 0xdbc01002, 0, 0x10000001, 0x20, 0xdbc01002, 4, 0x4000,
                                0x00020001, 0x00020004, 0x00060004, 0x00060001, 0x81010100}));
    // Fractional + winding: REAL points, flag 0x2000.
    ASSERT_EQ(call("GdipCreateRegionPath", {I(rect_path(1.25f, 2, 3, 4.5f, 1)), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)),
              (std::vector<u32>{0x40, 0x8310dd97, 0xdbc01002, 0, 0x10000001, 0x30, 0xdbc01002, 4, 0x2000,
                                0x3fa00000, 0x40000000, 0x40880000, 0x40000000, 0x40880000, 0x40d00000,
                                0x3fa00000, 0x40d00000, 0x81010100}));
    // Empty path: count 0, no compression flag.
    ASSERT_EQ(call("GdipCreateRegionPath", {I(path({}, {})), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)),
              (std::vector<u32>{0x1c, 0xe5c2ac69, 0xdbc01002, 0, 0x10000001, 0x0c, 0xdbc01002, 0, 0}));
    // Open 3-point path; 40000 does not fit INT16; -32768..32767 does.
    ASSERT_EQ(call("GdipCreateRegionPath", {I(path({{0, 0}, {10, 0}, {0, 10}}, {0, 1, 1})), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)),
              (std::vector<u32>{0x2c, 0xaca5cfe4, 0xdbc01002, 0, 0x10000001, 0x1c, 0xdbc01002, 3, 0x4000,
                                0, 0x0a, 0x000a0000, 0x00010100}));
    ASSERT_EQ(call("GdipCreateRegionPath", {I(rect_path(40000, 0, 3, 4)), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT))[1], 0x11779814u);
    ASSERT_EQ(call("GdipCreateRegionPath", {I(rect_path(-32768, -32768, 65535, 4)), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT))[1], 0x3e27795cu);
}

TEST_P(Gdip_region_Test, DataOfCombineTrees) {
    const u64 u = region_rect(0, 0, 10, 10);
    combine_rect(u, 5, 5, 10, 10, 2);
    EXPECT_EQ(data(u), (std::vector<u32>{0x34, 0xec074a08, 0xdbc01002, 2, 2, 0x10000000, 0, 0, 0x41200000,
                                         0x41200000, 0x10000000, 0x40a00000, 0x40a00000, 0x41200000,
                                         0x41200000}));
    // (a|b)&c vs a|(b&c): preorder, count = 2 * combine nodes.
    const u64 x = region_rect(0, 0, 10, 10);
    combine_rect(x, 5, 5, 10, 10, 2);
    combine_rect(x, 2, 2, 4, 20, 1);
    EXPECT_EQ(data(x)[1], 0x19238617u);
    EXPECT_EQ(data(x)[3], 4u);
    const u64 y = region_rect(5, 5, 10, 10);
    combine_rect(y, 2, 2, 4, 20, 1);
    const u64 z = region_rect(0, 0, 10, 10);
    ASSERT_EQ(call("GdipCombineRegionRegion", {I(z), I(y), I(2)}), Status::Ok);
    EXPECT_EQ(data(z)[1], 0x1b39354fu);
    ASSERT_EQ(call("GdipCloneRegion", {I(z), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT))[1], 0x1b39354fu);
    ASSERT_EQ(call("GdipCombineRegionRegion", {I(x), I(z), I(3)}), Status::Ok);
    EXPECT_EQ(data(x)[1], 0x5a0d7178u);
    EXPECT_EQ(data(x)[3], 10u);
    // Replace drops the tree.
    combine_rect(z, 0, 0, 10, 10, 0);
    EXPECT_EQ(data(z)[1], 0x7dc7ee9du);
}

TEST_P(Gdip_region_Test, CombineFoldsEmptyAndInfinite) {
    // checksum of the result per mode (probe "inf.*", "empty.*", "rect.*.inf", "rect.*.empty")
    const u32 kRect5 = 0xf997e1da, kRect0 = 0x7dc7ee9d, kInf = 0xa322349b, kEmpty = 0x1b9e53fe;
    const u32 inf_on_rect[6] = {kRect5, kRect5, kInf, 0xd5e973b3, 0xd1e080e0, kEmpty};
    const u32 empty_on_rect[6] = {kRect5, kEmpty, kRect5, kRect5, kEmpty, kRect5};
    const u32 rect_on_inf[6] = {kInf, kRect0, kInf, 0x031fb21b, kEmpty, 0x06a3bc55};
    const u32 rect_on_empty[6] = {kEmpty, kEmpty, kRect0, kRect0, kRect0, kEmpty};
    for (i32 mode = 0; mode <= 5; ++mode) {
        SCOPED_TRACE(mode);
        u64 r = region_infinite();
        combine_rect(r, 5, 5, 10, 10, mode);
        EXPECT_EQ(data(r)[1], inf_on_rect[mode]);
        r = region_empty();
        combine_rect(r, 5, 5, 10, 10, mode);
        EXPECT_EQ(data(r)[1], empty_on_rect[mode]);
        r = region_rect(0, 0, 10, 10);
        ASSERT_EQ(call("GdipCombineRegionRegion", {I(r), I(region_infinite()), I(u64(mode))}), Status::Ok);
        EXPECT_EQ(data(r)[1], rect_on_inf[mode]);
        r = region_rect(0, 0, 10, 10);
        ASSERT_EQ(call("GdipCombineRegionRegion", {I(r), I(region_empty()), I(u64(mode))}), Status::Ok);
        EXPECT_EQ(data(r)[1], rect_on_empty[mode]);
    }
}

TEST_P(Gdip_region_Test, GetRegionDataPartialAndRgnDataRoundTrip) {
    const u64 r = region_rect(1, 2, 3, 4);
    // Too small: whole fields streamed, size patched, no checksum.
    poison(DBUF, 64);
    poison(OUT3, 4);
    EXPECT_EQ(call("GdipGetRegionData", {I(r), I(DBUF), I(20), I(OUT3)}), Status::InsufficientBuffer);
    EXPECT_EQ(memory.read_u32(OUT3).value(), 20u);
    EXPECT_EQ(memory.read_u32(DBUF).value(), 12u);
    EXPECT_EQ(memory.read_u32(DBUF + 4).value(), 0u);
    EXPECT_EQ(memory.read_u32(DBUF + 16).value(), 0x10000000u);
    EXPECT_EQ(memory.read_u32(DBUF + 20).value(), 0xccccccccu);
    poison(DBUF, 64);
    EXPECT_EQ(call("GdipGetRegionData", {I(r), I(DBUF), I(4), I(OUT3)}), Status::InsufficientBuffer);
    EXPECT_EQ(memory.read_u32(OUT3).value(), 4u);
    EXPECT_EQ(memory.read_u32(DBUF).value(), 0xccccccccu);
    // A bigger buffer reports the real size; NULL sizeFilled is fine.
    EXPECT_EQ(call("GdipGetRegionData", {I(r), I(DBUF), I(52), I(OUT3)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(OUT3).value(), 36u);
    EXPECT_EQ(call("GdipGetRegionData", {I(r), I(DBUF), I(36), I(0)}), Status::Ok);

    // Round trip of a path-xor-rect tree.
    const u64 e = ellipse10(0, 0);
    ASSERT_EQ(call("GdipCreateRegionPath", {I(e), I(OUT)}), Status::Ok);
    const u64 t = read_handle(OUT);
    combine_rect(t, 5, 5, 10, 10, 3);
    const std::vector<u32> w = data(t);
    ASSERT_EQ(call("GdipCreateRegionRgnData", {I(DBUF), I(w.size() * 4), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)), w);
    // Extra bytes after the stream are accepted; anything short or with a
    // bad checksum/version/count is GenericError.
    EXPECT_EQ(call("GdipCreateRegionRgnData", {I(DBUF), I(w.size() * 4 + 4), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreateRegionRgnData", {I(DBUF), I(w.size() * 4 - 1), I(OUT)}), Status::GenericError);
    EXPECT_EQ(call("GdipCreateRegionRgnData", {I(DBUF), I(0), I(OUT)}), Status::GenericError);
    EXPECT_EQ(call("GdipCreateRegionRgnData", {I(0), I(64), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateRegionRgnData", {I(DBUF), I(64), I(0)}), Status::InvalidParameter);
    ASSERT_TRUE(memory.write_u8(DBUF + 4, memory.read_u8(DBUF + 4).value() ^ 1).is_ok());
    EXPECT_EQ(call("GdipCreateRegionRgnData", {I(DBUF), I(w.size() * 4), I(OUT)}), Status::GenericError);
}

TEST_P(Gdip_region_Test, RgnDataValidation) {
    auto crc = [](const std::vector<u32>& w) {
        u32 c = 0;
        for (usize i = 8; i < w.size() * 4; ++i) {
            c ^= static_cast<u8>(w[i / 4] >> (8 * (i % 4)));
            for (int k = 0; k < 8; ++k) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320u : 0);
        }
        return c;
    };
    auto load = [&](std::vector<u32> w) {
        w[0] = static_cast<u32>(w.size() * 4 - 8);
        w[1] = crc(w);
        for (usize i = 0; i < w.size(); ++i) write_int(DBUF + i * 4, static_cast<i32>(w[i]));
        return call("GdipCreateRegionRgnData", {I(DBUF), I(w.size() * 4), I(OUT)});
    };
    const u32 R = 0x10000000;
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, R, 0x3f800000, 0x40000000, 0x40400000, 0x40800000}), Status::Ok);
    EXPECT_EQ(load({0, 0, 0xdbc01001, 0, R, 0x3f800000, 0x40000000, 0x40400000, 0x40800000}), Status::Ok);
    // The region is re-serialized at version 0xDBC01002.
    EXPECT_EQ(data(read_handle(OUT))[2], 0xdbc01002u);
    EXPECT_EQ(load({0, 0, 0x12345678, 0, R, 0x3f800000, 0x40000000, 0x40400000, 0x40800000}),
              Status::GenericError);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 2, R, 0x3f800000, 0x40000000, 0x40400000, 0x40800000}),
              Status::GenericError);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, R, 0x3f800000}), Status::GenericError);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 2, 3, 0x10000003, 0x10000002}), Status::Ok);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, 0x10000003, 0x10000002}), Status::Ok);  // trailing node ignored
    EXPECT_EQ(data(read_handle(OUT)).size(), 5u);
    const u32 P = 0x10000001;
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x18, 0xdbc01002, 2, 0x4000, 0x00020001, 0x00040003, 0x100}),
              Status::Ok);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x18, 0xdbc01001, 2, 0x4000, 0x00020001, 0x00040003, 0x100}),
              Status::Ok);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x14, 0xdbc01002, 2, 0x4000, 0x00020001, 0x00040003, 0x100}),
              Status::GenericError);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x20, 0xdbc01002, 2, 0x4000, 0x00020001, 0x00040003, 0x100, 0}),
              Status::GenericError);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x18, 0x12345678, 2, 0x4000, 0x00020001, 0x00040003, 0x100}),
              Status::GenericError);
    EXPECT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x18, 0xdbc01002, 2, 0x4000, 0x00020001, 0x00040003, 0x0207}),
              Status::GenericError);
    // A closed 3-point integral path parses and rasterizes as a triangle.
    ASSERT_EQ(load({0, 0, 0xdbc01002, 0, P, 0x1c, 0xdbc01002, 3, 0x4000, 0, 0x0a, 0x000a0000, 0x00810100}),
              Status::Ok);
    const auto s = scans(read_handle(OUT), matrix(1, 0, 0, 1, 0, 0));
    ASSERT_EQ(s.size(), 10u);
    EXPECT_EQ(s[0], (Rect4{0, 0, 10, 1}));
    EXPECT_EQ(s[9], (Rect4{0, 9, 1, 1}));
}

// ---- Raster: scans, bounds, flags, visibility --------------------------------

TEST_P(Gdip_region_Test, ScansOfRectsAndInfinite) {
    const u64 id = matrix(1, 0, 0, 1, 0, 0);
    const u64 sc = matrix(2, 0, 0, 3, 1, 0);
    const u64 r = region_rect(1.5f, 2.5f, 10, 20);
    EXPECT_EQ(scans(r, id), (std::vector<Rect4>{{2, 3, 10, 20}}));
    EXPECT_EQ(scans(r, sc), (std::vector<Rect4>{{4, 8, 20, 60}}));
    // Float scans carry the same integers.
    ASSERT_EQ(call("GdipGetRegionScans", {I(r), I(DBUF), I(OUT3), I(sc)}), Status::Ok);
    EXPECT_EQ(read_real(DBUF), 4.0f);
    EXPECT_EQ(read_real(DBUF + 12), 60.0f);
    const u64 rot = matrix(0.70710678f, 0.70710678f, -0.70710678f, 0.70710678f, 0, 0);
    const auto rs = scans(r, rot);
    ASSERT_EQ(rs.size(), 20u);
    EXPECT_EQ(rs[0], (Rect4{-1, 4, 2, 1}));
    EXPECT_EQ(rs[6], (Rect4{-7, 10, 14, 1}));
    EXPECT_EQ(rs[13], (Rect4{-14, 17, 14, 1}));
    EXPECT_EQ(rs[19], (Rect4{-8, 23, 2, 1}));
    EXPECT_TRUE(scans(region_rect(0.2f, 0.2f, 0.5f, 0.5f), id).empty());
    EXPECT_EQ(scans(region_rect(0.5f, 0.5f, 1, 1), id), (std::vector<Rect4>{{1, 1, 1, 1}}));
    EXPECT_EQ(scans(region_rect(0.49f, 0.51f, 1, 1), id), (std::vector<Rect4>{{1, 1, 1, 1}}));
    EXPECT_EQ(scans(region_rect(0.3f, 0.3f, 10.4f, 10.4f), id), (std::vector<Rect4>{{1, 1, 10, 10}}));
    EXPECT_EQ(scans(region_rect(-0.5f, -0.5f, 10, 10), id), (std::vector<Rect4>{{0, 0, 10, 10}}));
    EXPECT_TRUE(scans(region_rect(1.5f, 1.5f, 0.5f, 10), id).empty());
    EXPECT_TRUE(scans(region_rect(10, 10, -5, -5), id).empty());
    // Infinite: the device square, whatever the matrix.
    EXPECT_EQ(scans(region_infinite(), sc), (std::vector<Rect4>{{-4194304, -4194304, 8388608, 8388608}}));
    EXPECT_TRUE(scans(region_empty(), id).empty());

    const u64 u = region_rect(0, 0, 10, 10);
    combine_rect(u, 5, 5, 10, 10, 2);
    EXPECT_EQ(scans(u, id), (std::vector<Rect4>{{0, 0, 10, 5}, {0, 5, 15, 5}, {5, 10, 10, 5}}));
    const u64 xo = region_rect(0, 0, 10, 10);
    combine_rect(xo, 5, 5, 10, 10, 3);
    EXPECT_EQ(scans(xo, id), (std::vector<Rect4>{{0, 0, 10, 5}, {0, 5, 5, 5}, {10, 5, 5, 5}, {5, 10, 10, 5}}));
    const u64 cm = region_rect(0, 0, 10, 10);
    combine_rect(cm, 5, 5, 10, 10, 5);
    EXPECT_EQ(scans(cm, id), (std::vector<Rect4>{{10, 5, 5, 5}, {5, 10, 10, 5}}));
    const u64 ix = region_infinite();
    combine_rect(ix, 5, 5, 10, 10, 3);
    EXPECT_EQ(scans(ix, id), (std::vector<Rect4>{{-4194304, -4194304, 8388608, 4194309},
                                                 {-4194304, 5, 4194309, 10},
                                                 {15, 5, 4194289, 10},
                                                 {-4194304, 15, 8388608, 4194289}}));
    const u64 fu = region_rect(0.3f, 0.3f, 5, 5);
    combine_rect(fu, 2.2f, 2.2f, 5, 5, 2);
    EXPECT_EQ(scans(fu, id), (std::vector<Rect4>{{1, 1, 5, 2}, {1, 3, 7, 3}, {3, 6, 5, 2}}));
}

TEST_P(Gdip_region_Test, ScansOfPaths) {
    const u64 id = matrix(1, 0, 0, 1, 0, 0);
    ASSERT_EQ(call("GdipCreateRegionPath", {I(ellipse10(0, 0)), I(OUT)}), Status::Ok);
    const u64 e = read_handle(OUT);
    EXPECT_EQ(scans(e, id), (std::vector<Rect4>{{3, 1, 5, 1}, {2, 2, 7, 1}, {1, 3, 9, 2}, {0, 5, 10, 1},
                                                {1, 6, 9, 2}, {2, 8, 7, 1}, {3, 9, 5, 1}}));
    // Under scale(2, 3) + (1, 0): interior bands exact; curve rows within a pixel.
    const auto big = scans(e, matrix(2, 0, 0, 3, 1, 0));
    ASSERT_FALSE(big.empty());
    EXPECT_EQ(big.front()[1], 1);
    EXPECT_EQ(big.back()[1] + big.back()[3], 30);
    bool has_widest = false;
    for (const Rect4& q : big) has_widest |= (q == Rect4{1, 15, 20, 1});
    EXPECT_TRUE(has_widest);
    // Open triangle: closed for filling, left/top inclusive.
    ASSERT_EQ(call("GdipCreateRegionPath", {I(path({{0, 0}, {10, 0}, {0, 10}}, {0, 1, 1})), I(OUT)}), Status::Ok);
    const auto tri = scans(read_handle(OUT), id);
    ASSERT_EQ(tri.size(), 10u);
    for (i32 y = 0; y < 10; ++y) EXPECT_EQ(tri[usize(y)], (Rect4{0, y, 10 - y, 1}));
    // Fractional winding rectangle.
    ASSERT_EQ(call("GdipCreateRegionPath", {I(rect_path(1.25f, 2, 3, 4.5f, 1)), I(OUT)}), Status::Ok);
    EXPECT_EQ(scans(read_handle(OUT), id), (std::vector<Rect4>{{2, 2, 3, 5}}));
    // Alternate vs winding overlap.
    const std::vector<gdip::PointF> two = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {5, 5}, {15, 5}, {15, 15}, {5, 15}};
    const std::vector<u8> tt = {0, 1, 1, 0x81, 0, 1, 1, 0x81};
    ASSERT_EQ(call("GdipCreateRegionPath", {I(path(two, tt, 0)), I(OUT)}), Status::Ok);
    EXPECT_FALSE(vis(read_handle(OUT), 7, 7));
    ASSERT_EQ(call("GdipCreateRegionPath", {I(path(two, tt, 1)), I(OUT)}), Status::Ok);
    EXPECT_TRUE(vis(read_handle(OUT), 7, 7));
}

TEST_P(Gdip_region_Test, BoundsExactLeafRasterTree) {
    const u64 g = graphics(100, 100);
    const u64 r = region_rect(1.5f, 2.5f, 10, 20);
    EXPECT_EQ(bounds(r, g), (std::array<float, 4>{1.5f, 2.5f, 10, 20}));
    EXPECT_EQ(boundsI(r, g), (Rect4{2, 3, 10, 20}));
    EXPECT_EQ(bounds(region_rect(10, 10, -5, -5), g), (std::array<float, 4>{10, 10, -5, -5}));
    EXPECT_EQ(boundsI(region_rect(0.49f, 0.51f, 1, 1), g), (Rect4{0, 1, 1, 1}));
    EXPECT_EQ(boundsI(region_rect(0.2f, 0.2f, 0.5f, 0.5f), g), (Rect4{0, 0, 1, 1}));
    EXPECT_EQ(bounds(region_infinite(), g), (std::array<float, 4>{-4194304, -4194304, 8388608, 8388608}));
    EXPECT_EQ(bounds(region_empty(), g), (std::array<float, 4>{0, 0, 0, 0}));
    ASSERT_EQ(call("GdipCreateRegionPath", {I(ellipse10(0, 0)), I(OUT)}), Status::Ok);
    EXPECT_EQ(bounds(read_handle(OUT), g), (std::array<float, 4>{0, 0, 10, 10}));
    // Bézier control points count (probe "pathbezier").
    ASSERT_EQ(call("GdipCreateRegionPath",
                   {I(path({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, {0, 3, 3, 0x83})), I(OUT)}),
              Status::Ok);
    EXPECT_EQ(bounds(read_handle(OUT), g), (std::array<float, 4>{0, 0, 10, 10}));
    // A single leaf ignores the graphics transform.
    world(g, 2, 0, 0, 2, 5, 5);
    EXPECT_EQ(bounds(region_rect(10, 20, 30, 40), g), (std::array<float, 4>{10, 20, 30, 40}));
    world(g, 1, 0, 0, 1, 0, 0);
    // Trees: device raster bounds, mapped back to world.
    const u64 fu = region_rect(0.3f, 0.3f, 5, 5);
    combine_rect(fu, 2.2f, 2.2f, 5, 5, 2);
    EXPECT_EQ(bounds(fu, g), (std::array<float, 4>{1, 1, 7, 7}));
    world(g, 2, 0, 0, 2, 0, 0);
    EXPECT_EQ(bounds(fu, g), (std::array<float, 4>{0.5f, 0.5f, 7, 7}));
    EXPECT_EQ(boundsI(fu, g), (Rect4{1, 1, 7, 7}));
    const u64 ie = region_infinite();
    combine_rect(ie, 0, 0, 10, 10, 4);
    EXPECT_EQ(bounds(ie, g), (std::array<float, 4>{-2097152, -2097152, 4194304, 4194304}));
    world(g, 1, 0, 0, 1, 0.5f, 0.5f);
    const u64 eh = region_rect(0, 0, 10, 10);
    combine_rect(eh, 0, 0, 10, 5, 4);
    EXPECT_EQ(bounds(eh, g), (std::array<float, 4>{0.5f, 5.5f, 10, 5}));
    EXPECT_EQ(boundsI(eh, g), (Rect4{1, 6, 10, 5}));
    world(g, 1, 0, 0, 1, 0, 0);
    EXPECT_EQ(bounds(eh, g), (std::array<float, 4>{0, 5, 10, 5}));
    const u64 dj = region_rect(0, 0, 10, 10);
    combine_rect(dj, 20, 0, 10, 10, 1);
    EXPECT_EQ(bounds(dj, g), (std::array<float, 4>{0, 0, 0, 0}));
    // rect ∩ ellipse(5,5,10,10): rows/columns 6..9.
    const u64 rp = region_rect(0, 0, 10, 10);
    ASSERT_EQ(call("GdipCombineRegionPath", {I(rp), I(ellipse10(5, 5)), I(1)}), Status::Ok);
    EXPECT_EQ(bounds(rp, g), (std::array<float, 4>{6, 6, 4, 4}));
}

TEST_P(Gdip_region_Test, EmptyInfiniteEqual) {
    const u64 g = graphics(100, 100);
    EXPECT_TRUE(flag("GdipIsInfiniteRegion", region_infinite(), g));
    EXPECT_FALSE(flag("GdipIsEmptyRegion", region_infinite(), g));
    EXPECT_TRUE(flag("GdipIsEmptyRegion", region_empty(), g));
    EXPECT_FALSE(flag("GdipIsInfiniteRegion", region_empty(), g));
    const u64 tiny = region_rect(0.2f, 0.2f, 0.5f, 0.5f);
    EXPECT_TRUE(flag("GdipIsEmptyRegion", tiny, g));
    EXPECT_TRUE(flag("GdipIsEmptyRegion", region_rect(10, 10, 0, 5), g));
    EXPECT_TRUE(flag("GdipIsInfiniteRegion", region_rect(-4194304, -4194304, 8388608, 8388608), g));
    const u64 ix = region_infinite();
    combine_rect(ix, 5, 5, 10, 10, 3);
    EXPECT_FALSE(flag("GdipIsInfiniteRegion", ix, g));
    EXPECT_FALSE(flag("GdipIsEmptyRegion", ix, g));
    const u64 dj = region_rect(0, 0, 10, 10);
    combine_rect(dj, 20, 0, 10, 10, 1);
    EXPECT_TRUE(flag("GdipIsEmptyRegion", dj, g));
    world(g, 2, 0, 0, 2, 0, 0);
    EXPECT_FALSE(flag("GdipIsEmptyRegion", tiny, g));
    EXPECT_TRUE(flag("GdipIsInfiniteRegion", region_infinite(), g));
    world(g, 1, 0, 0, 1, 0, 0);
    // Translating infinite keeps it infinite.
    const u64 ti = region_infinite();
    ASSERT_EQ(call("GdipTranslateRegion", {I(ti), R(5), R(5)}), Status::Ok);
    EXPECT_TRUE(flag("GdipIsInfiniteRegion", ti, g));

    auto equal = [&](u64 a, u64 b) {
        poison(OUT3, 4);
        EXPECT_EQ(call("GdipIsEqualRegion", {I(a), I(b), I(g), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    };
    const u64 a = region_rect(0, 0, 10, 10);
    EXPECT_TRUE(equal(a, region_rect(0, 0, 10, 10)));
    const u64 halves = region_rect(0, 0, 10, 5);
    combine_rect(halves, 0, 5, 10, 5, 2);
    EXPECT_TRUE(equal(a, halves));
    EXPECT_FALSE(equal(a, region_rect(0, 0, 10, 10.3f)));
    EXPECT_FALSE(equal(a, region_rect(0, 0, 10, 10.2f)));
    EXPECT_TRUE(equal(region_infinite(), region_infinite()));
    EXPECT_FALSE(equal(region_infinite(), a));
    EXPECT_TRUE(equal(region_empty(), region_rect(3, 3, 0, 0)));
    ASSERT_EQ(call("GdipCreateRegionPath", {I(rect_path(1, 2, 3, 4)), I(OUT)}), Status::Ok);
    EXPECT_TRUE(equal(read_handle(OUT), region_rect(1, 2, 3, 4)));
}

TEST_P(Gdip_region_Test, VisiblePointAndRect) {
    const u64 r = region_rect(0, 0, 10, 10);
    EXPECT_TRUE(vis(r, 0, 0));
    EXPECT_FALSE(vis(r, 9.99f, 9.99f));
    EXPECT_FALSE(vis(r, 10, 5));
    EXPECT_TRUE(vis(r, -0.01f, 5));
    EXPECT_TRUE(vis(r, -0.5f, 5));
    EXPECT_FALSE(vis(r, -0.51f, 5));
    EXPECT_FALSE(vis(r, 9.5f, 5));
    poison(OUT3, 4);
    EXPECT_EQ(call("GdipIsVisibleRegionPointI", {I(r), I(9), I(9), I(0), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 1);
    EXPECT_EQ(call("GdipIsVisibleRegionPointI", {I(r), I(10), I(5), I(0), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 0);
    const u64 h = region_rect(0.5f, 0.5f, 10, 10);
    EXPECT_TRUE(vis(h, 0.5f, 5));
    EXPECT_FALSE(vis(h, 0.4f, 5));
    EXPECT_TRUE(vis(h, 10.4f, 5));
    EXPECT_FALSE(vis(h, 10.5f, 5));
    EXPECT_FALSE(vis(h, 0, 5));
    // Through a graphics: world (2x + 5).
    const u64 g = graphics(100, 100);
    world(g, 2, 0, 0, 2, 5, 5);
    const u64 w = region_rect(10, 20, 30, 40);
    EXPECT_TRUE(vis(w, 11, 21, g));
    EXPECT_FALSE(vis(w, 39.9f, 59.9f, g));
    EXPECT_TRUE(vis(w, 9.8f, 21, g));
    EXPECT_FALSE(vis(region_infinite(), 5e6f, 0));
    EXPECT_FALSE(vis(region_empty(), 0, 0));
    const u64 ie = region_infinite();
    combine_rect(ie, 0, 0, 10, 10, 4);
    EXPECT_FALSE(vis(ie, 5, 5));
    EXPECT_TRUE(vis(ie, 4194303, 5));
    EXPECT_FALSE(vis(ie, 4194304, 5));
    EXPECT_TRUE(vis(ie, -4194304, 5));

    EXPECT_TRUE(visrect(r, 2, 2, 3, 3));
    EXPECT_TRUE(visrect(r, 9, 9, 5, 5));
    EXPECT_FALSE(visrect(r, 10, 10, 5, 5));
    EXPECT_FALSE(visrect(r, -5, -5, 5, 5));
    EXPECT_TRUE(visrect(r, -5, -5, 5.5f, 5.5f));
    EXPECT_FALSE(visrect(r, 9.6f, 0, 1, 1));
    EXPECT_FALSE(visrect(r, 9.4f, 0, 1, 1));
    EXPECT_FALSE(visrect(r, 3, 3, 0, 0));
    EXPECT_FALSE(visrect(r, 3, 3, -1, -1));
    poison(OUT3, 4);
    EXPECT_EQ(call("GdipIsVisibleRegionRectI", {I(r), I(9), I(9), I(5), I(5), I(0), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 1);
}

TEST_P(Gdip_region_Test, TransformAndTranslate) {
    const u64 a = region_rect(1, 2, 3, 4);
    ASSERT_EQ(call("GdipTranslateRegion", {I(a), R(0.5f), R(-1)}), Status::Ok);
    EXPECT_EQ(data(a)[1], 0x641d6203u);
    ASSERT_EQ(call("GdipTranslateRegionI", {I(a), I(2), I(3)}), Status::Ok);
    EXPECT_EQ(data(a)[1], 0x9894508cu);
    ASSERT_EQ(call("GdipTransformRegion", {I(a), I(matrix(2, 0, 0, 3, 1, 0))}), Status::Ok);
    EXPECT_EQ(data(a)[1], 0x0456efc2u);  // still a rect: 8,12,6,12
    // Rotation turns a rect into its corners, no close flag.
    const u64 rr = region_rect(1, 2, 3, 4);
    ASSERT_EQ(call("GdipTransformRegion", {I(rr), I(matrix(0, 1, -1, 0, 0, 0))}), Status::Ok);
    EXPECT_EQ(data(rr), (std::vector<u32>{0x30, 0x4545e079, 0xdbc01002, 0, 0x10000001, 0x20, 0xdbc01002, 4,
                                          0x4000, 0x0001fffe, 0x0004fffe, 0x0004fffa, 0x0001fffa, 0x01010100}));
    // Flip keeps a (normalized) rect; shear makes a path.
    const u64 fl = region_rect(1, 2, 3, 4);
    ASSERT_EQ(call("GdipTransformRegion", {I(fl), I(matrix(-1, 0, 0, 1, 0, 0))}), Status::Ok);
    EXPECT_EQ(data(fl)[1], 0xd1551c51u);
    const u64 sh = region_rect(1, 2, 3, 4);
    ASSERT_EQ(call("GdipTransformRegion", {I(sh), I(matrix(1, 0, 0.5f, 1, 0, 0))}), Status::Ok);
    EXPECT_EQ(data(sh)[1], 0xe569f3bdu);
    // A degenerate matrix makes a zero path (empty).
    const u64 zr = region_rect(1, 2, 3, 4);
    ASSERT_EQ(call("GdipTransformRegion", {I(zr), I(matrix(0, 0, 0, 0, 0, 0))}), Status::Ok);
    EXPECT_EQ(data(zr)[1], 0x9ad18326u);
    // Empty / infinite unchanged; trees transform leaf by leaf.
    const u64 inf = region_infinite();
    ASSERT_EQ(call("GdipTransformRegion", {I(inf), I(matrix(2, 0, 0, 3, 1, 0))}), Status::Ok);
    EXPECT_EQ(data(inf)[1], 0xa322349bu);
    const u64 u = region_rect(0, 0, 10, 10);
    combine_rect(u, 5, 5, 10, 10, 2);
    ASSERT_EQ(call("GdipTranslateRegion", {I(u), R(1), R(1)}), Status::Ok);
    EXPECT_EQ(data(u)[1], 0x27f9ca5cu);
    ASSERT_EQ(call("GdipTransformRegion", {I(u), I(matrix(2, 0, 0, 3, 1, 0))}), Status::Ok);
    EXPECT_EQ(data(u)[1], 0xef7ac480u);
    const u64 ru = region_rect(0, 0, 10, 10);
    combine_rect(ru, 5, 5, 10, 10, 2);
    ASSERT_EQ(call("GdipTransformRegion", {I(ru), I(matrix(0, 1, -1, 0, 0, 0))}), Status::Ok);
    EXPECT_EQ(data(ru)[1], 0x347a9bf9u);
}

TEST_P(Gdip_region_Test, HrgnConversion) {
    const u64 g = graphics(100, 100);
    auto box = [&](u64 hrgn) {
        ClipRect c;
        EXPECT_TRUE(gdi.get_region_rect(static_cast<Handle>(hrgn), c));
        return Rect4{c.left, c.top, c.right, c.bottom};
    };
    const u64 r = region_rect(1.5f, 2.5f, 10, 20);
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(r), I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(box(read_handle(OUT2)), (Rect4{2, 3, 12, 23}));
    const u64 r2 = region_rect(1.4f, 2.6f, 10.2f, 20.2f);
    world(g, 2, 0, 0, 2, 0, 0);
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(r2), I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(box(read_handle(OUT2)), (Rect4{3, 6, 24, 46}));
    world(g, 1, 0, 0, 1, 0, 0);
    // NULL graphics: identity.
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(r2), I(0), I(OUT2)}), Status::Ok);
    EXPECT_EQ(box(read_handle(OUT2)), (Rect4{2, 3, 12, 23}));
    poison(OUT2, 8, 0x11);
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(region_infinite()), I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT2), 0u);
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(region_empty()), I(g), I(OUT2)}), Status::Ok);
    ClipRect em;
    ASSERT_TRUE(gdi.get_region_rect(static_cast<Handle>(read_handle(OUT2)), em));
    EXPECT_TRUE(em.parts().empty());
    ASSERT_EQ(call("GdipCreateRegionPath", {I(ellipse10(0, 0)), I(OUT)}), Status::Ok);
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(read_handle(OUT)), I(g), I(OUT2)}), Status::Ok);
    ClipRect el;
    ASSERT_TRUE(gdi.get_region_rect(static_cast<Handle>(read_handle(OUT2)), el));
    EXPECT_EQ((Rect4{el.left, el.top, el.right, el.bottom}), (Rect4{0, 1, 10, 10}));
    EXPECT_EQ(el.parts().size(), 7u);
    const u64 ie = region_infinite();
    combine_rect(ie, 0, 0, 10, 10, 4);
    ASSERT_EQ(call("GdipGetRegionHRgn", {I(ie), I(g), I(OUT2)}), Status::Ok);
    EXPECT_EQ(box(read_handle(OUT2)), (Rect4{-4194304, -4194304, 4194304, 4194304}));

    // HRGN -> region: one closed winding figure per rectangle.
    const Handle one = gdi.create_rect_rgn(3, 4, 10, 12);
    ASSERT_EQ(call("GdipCreateRegionHrgn", {I(one), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)),
              (std::vector<u32>{0x30, 0x35d46c12, 0xdbc01002, 0, 0x10000001, 0x20, 0xdbc01002, 4, 0x6000,
                                0x00040003, 0x0004000a, 0x000c000a, 0x000c0003, 0x81010100}));
    const Handle none = gdi.create_rect_rgn(0, 0, 0, 0);
    ASSERT_EQ(call("GdipCreateRegionHrgn", {I(none), I(OUT)}), Status::Ok);
    EXPECT_EQ(data(read_handle(OUT)),
              (std::vector<u32>{0x1c, 0xdd8fea89, 0xdbc01002, 0, 0x10000001, 0x0c, 0xdbc01002, 0, 0x2000}));
    ClipRect a, b;
    ASSERT_TRUE(gdi.get_region_rect(one, a));
    const Handle two = gdi.create_rect_rgn(0, 0, 10, 10);
    const Handle other = gdi.create_rect_rgn(20, 5, 30, 15);
    ASSERT_TRUE(gdi.get_region_rect(two, a));
    ASSERT_TRUE(gdi.get_region_rect(other, b));
    ASSERT_TRUE(gdi.set_region(two, ClipRect::combine(a, b, 2)));
    ASSERT_EQ(call("GdipCreateRegionHrgn", {I(two), I(OUT)}), Status::Ok);
    EXPECT_EQ(scans(read_handle(OUT), matrix(1, 0, 0, 1, 0, 0)),
              (std::vector<Rect4>{{0, 0, 10, 5}, {0, 5, 10, 5}, {20, 5, 10, 5}, {20, 10, 10, 5}}));
}

TEST_P(Gdip_region_Test, FillRegionPixels) {
    u64 image = 0;
    const u64 g = graphics(20, 20, &image);
    const u64 red = mgr.objects().add(gdip::GpBrush{gdip::BrushTypeSolidColor, 0xFFFF0000u});
    const u64 r = region_rect(2.5f, 2.5f, 5, 5);
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(r)}), Status::Ok);
    EXPECT_EQ(row_extent(image, 4, 20), (std::pair<i32, i32>{3, 7}));
    EXPECT_EQ(pixel(image, 3, 3), 0xFFFF0000u);
    EXPECT_EQ(pixel(image, 2, 4), 0u);
    EXPECT_EQ(row_extent(image, 2, 20), (std::pair<i32, i32>{-1, -1}));
    EXPECT_EQ(row_extent(image, 7, 20), (std::pair<i32, i32>{3, 7}));
    EXPECT_EQ(row_extent(image, 8, 20), (std::pair<i32, i32>{-1, -1}));
    // Antialiasing does not change a region fill.
    auto& bits = mgr.objects().get<gdip::GpImage>(image)->bits;
    std::fill(bits.begin(), bits.end(), 0);
    G(g).state.smoothing_mode = 4;
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(r)}), Status::Ok);
    EXPECT_EQ(row_extent(image, 4, 20), (std::pair<i32, i32>{3, 7}));
    EXPECT_EQ(pixel(image, 7, 7), 0xFFFF0000u);
    G(g).state.smoothing_mode = 0;
    // World translation, then a graphics clip.
    std::fill(bits.begin(), bits.end(), 0);
    world(g, 1, 0, 0, 1, 5, 5);
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(region_rect(0, 0, 10, 10))}), Status::Ok);
    EXPECT_EQ(row_extent(image, 7, 20), (std::pair<i32, i32>{5, 14}));
    world(g, 1, 0, 0, 1, 0, 0);
    std::fill(bits.begin(), bits.end(), 0);
    setclip(g, 4, 0, 3, 20, 0);
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(region_rect(2, 2, 10, 10))}), Status::Ok);
    EXPECT_EQ(row_extent(image, 4, 20), (std::pair<i32, i32>{4, 6}));
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    // PixelOffsetModeHalf shifts device pixels by half.
    std::fill(bits.begin(), bits.end(), 0);
    G(g).state.pixel_offset_mode = 4;
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(region_rect(2, 2, 10, 10))}), Status::Ok);
    EXPECT_EQ(row_extent(image, 4, 20), (std::pair<i32, i32>{2, 11}));
    G(g).state.pixel_offset_mode = 0;
    // Infinite fills everything; empty is Ok.
    std::fill(bits.begin(), bits.end(), 0);
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(region_infinite())}), Status::Ok);
    EXPECT_EQ(pixel(image, 0, 0), 0xFFFF0000u);
    EXPECT_EQ(pixel(image, 19, 19), 0xFFFF0000u);
    EXPECT_EQ(call("GdipFillRegion", {I(g), I(red), I(region_empty())}), Status::Ok);
    // Ellipse rows (probe "fill ellipse rows").
    std::fill(bits.begin(), bits.end(), 0);
    ASSERT_EQ(call("GdipCreateRegionPath", {I(ellipse10(0, 0)), I(OUT)}), Status::Ok);
    ASSERT_EQ(call("GdipFillRegion", {I(g), I(red), I(read_handle(OUT))}), Status::Ok);
    const std::pair<i32, i32> rows[11] = {{-1, -1}, {3, 7}, {2, 8}, {1, 9}, {1, 9}, {0, 9},
                                          {1, 9},   {1, 9}, {2, 8}, {3, 7}, {-1, -1}};
    for (i32 y = 0; y <= 10; ++y) EXPECT_EQ(row_extent(image, y, 20), rows[y]) << "row " << y;
}

TEST_P(Gdip_region_Test, IsVisiblePathPoint) {
    const u64 p = rect_path(0, 0, 10, 10);
    auto pv = [&](float x, float y, u64 g) {
        poison(OUT3, 4);
        EXPECT_EQ(call("GdipIsVisiblePathPoint", {I(p), R(x), R(y), I(g), I(OUT3)}), Status::Ok);
        return read_int(OUT3) != 0;
    };
    EXPECT_TRUE(pv(0, 0, 0));
    EXPECT_TRUE(pv(5, 5, 0));
    EXPECT_FALSE(pv(9.99f, 9.99f, 0));
    EXPECT_FALSE(pv(10, 5, 0));
    EXPECT_TRUE(pv(-0.01f, 5, 0));
    EXPECT_FALSE(pv(9.5f, 5, 0));
    EXPECT_TRUE(pv(-0.5f, 5, 0));
    const u64 g = graphics(100, 100);
    world(g, 2, 0, 0, 2, 0, 0);
    EXPECT_TRUE(pv(9.5f, 5, g));
    EXPECT_TRUE(pv(9.51f, 5, g));
    EXPECT_FALSE(pv(-0.5f, 5, g));
    EXPECT_FALSE(pv(9.99f, 9.99f, g));
    world(g, 1, 0, 0, 1, 0, 0);
    setclip(g, 0, 0, 3, 3, 0);
    EXPECT_TRUE(pv(5, 5, g));  // the graphics clip does not apply
    poison(OUT3, 4);
    EXPECT_EQ(call("GdipIsVisiblePathPointI", {I(p), I(10), I(5), I(0), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 0);
    EXPECT_EQ(call("GdipIsVisiblePathPoint", {I(p), R(5), R(5), I(0), I(0)}), Status::InvalidParameter);
    poison(OUT3, 4);
    EXPECT_EQ(call("GdipIsVisiblePathPoint", {I(path({}, {})), R(0), R(0), I(0), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 0);
}

// ---- Graphics clip ---------------------------------------------------------

TEST_P(Gdip_region_Test, ClipDefaultsAndRect) {
    const u64 g = graphics(100, 100);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{-4194304, -4194304, 8388608, 8388608}));
    EXPECT_EQ(geti("GdipGetClipBoundsI", g), (Rect4{-4194304, -4194304, 8388608, 8388608}));
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{0, 0, 100, 100}));
    EXPECT_EQ(geti("GdipGetVisibleClipBoundsI", g), (Rect4{0, 0, 100, 100}));
    EXPECT_FALSE(gflag("GdipIsClipEmpty", g));
    EXPECT_FALSE(gflag("GdipIsVisibleClipEmpty", g));
    EXPECT_EQ(clip_data(g)[1], 0xa322349bu);
    EXPECT_TRUE(gvis(g, 0, 0));
    EXPECT_FALSE(gvis(g, 99.9f, 99.9f));
    EXPECT_TRUE(gvis(g, -0.5f, 50));
    EXPECT_FALSE(gvis(g, 99.5f, 50));
    EXPECT_TRUE(gvisrect(g, 90, 90, 20, 20));
    EXPECT_FALSE(gvisrect(g, 100, 100, 5, 5));
    EXPECT_TRUE(gvisrect(g, -5, -5, 5.5f, 5.5f));
    poison(OUT3, 4);
    EXPECT_EQ(call("GdipIsVisiblePointI", {I(g), I(100), I(5), I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_int(OUT3), 0);

    setclip(g, 10, 10, 50, 50, 0);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{10, 10, 50, 50}));
    EXPECT_EQ(clip_data(g), (std::vector<u32>{0x1c, 0xc1268d0d, 0xdbc01002, 0, 0x10000000, 0x41200000,
                                              0x41200000, 0x42480000, 0x42480000}));
    EXPECT_TRUE(gvis(g, 9.5f, 30));
    EXPECT_FALSE(gvis(g, 59.9f, 30));
    EXPECT_FALSE(gvisrect(g, 0, 0, 10, 10));
    EXPECT_TRUE(gvisrect(g, 0, 0, 10.5f, 10.5f));
    // The clip stays on the page when the world transform changes.
    world(g, 2, 0, 0, 2, 0, 0);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{5, 5, 25, 25}));
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{5, 5, 25, 25}));
    EXPECT_EQ(clip_data(g)[1], 0x952701d1u);
    EXPECT_TRUE(gvis(g, 4.9f, 5));
    EXPECT_FALSE(gvis(g, 29.9f, 5));
    ASSERT_EQ(call("GdipTranslateClip", {I(g), R(5), R(5)}), Status::Ok);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{10, 10, 25, 25}));
    ASSERT_EQ(call("GdipTranslateClipI", {I(g), I(u64(u32(-5))), I(u64(u32(-5)))}), Status::Ok);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{5, 5, 25, 25}));
    setclip(g, 10, 10, 10, 10, 1);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{10, 10, 10, 10}));
    EXPECT_EQ(clip_data(g)[1], 0x0ff19d7du);
    setclip(g, 0, 0, 10, 10, 0);
    world(g, 1, 0, 0, 1, 0, 0);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{0, 0, 20, 20}));
    setclip(g, 5, 5, 10, 10, 4);
    EXPECT_EQ(clip_data(g)[1], 0x0ddf528cu);
    setclip(g, 0, 0, 30, 30, 5);
    EXPECT_EQ(clip_data(g)[1], 0xe566b5fdu);
    setclip(g, 0, 0, 40, 40, 3);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{0, 0, 40, 40}));
    setclip(g, 50, 50, 10, 10, 2);
    EXPECT_EQ(clip_data(g)[1], 0x57fbf789u);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{0, 0, 60, 60}));
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    EXPECT_EQ(clip_data(g)[1], 0xa322349bu);
    EXPECT_EQ(call("GdipSetClipRect", {I(g), R(0), R(0), R(1), R(1), I(6)}), Status::InvalidParameter);
}

TEST_P(Gdip_region_Test, ClipEdgeCases) {
    const u64 g = graphics(100, 100);
    setclip(g, 20, 20, 0, 0, 0);  // zero extent -> empty
    EXPECT_EQ(clip_data(g)[1], 0x1b9e53feu);
    EXPECT_TRUE(gflag("GdipIsClipEmpty", g));
    EXPECT_TRUE(gflag("GdipIsVisibleClipEmpty", g));
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{0, 0, 0, 0}));
    setclip(g, 20, 20, -5, -5, 0);  // negative extent normalized
    EXPECT_EQ(clip_data(g)[1], 0xa747129fu);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{15, 15, 5, 5}));
    EXPECT_FALSE(gflag("GdipIsClipEmpty", g));
    setclip(g, 0.2f, 0.2f, 0.5f, 0.5f, 0);  // no pixel
    EXPECT_TRUE(gflag("GdipIsClipEmpty", g));
    EXPECT_EQ(clip_data(g)[1], 0x35eeaa8du);
    setclip(g, 200, 200, 10, 10, 0);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{200, 200, 10, 10}));
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_FALSE(gflag("GdipIsClipEmpty", g));
    EXPECT_TRUE(gflag("GdipIsVisibleClipEmpty", g));
    setclip(g, -50, -50, 100, 100, 0);
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{0, 0, 50, 50}));
    setclip(g, 10.3f, 10.6f, 20.2f, 20.2f, 0);  // bounds are the raster's
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{11, 11, 20, 20}));
    EXPECT_EQ(clip_data(g)[1], 0x3e1cc7e9u);
    ASSERT_EQ(call("GdipSetClipRectI", {I(g), I(10), I(20), I(30), I(40), I(0)}), Status::Ok);
    EXPECT_EQ(clip_data(g)[1], 0x3b3e8e6bu);
    setclip(g, 0, 0, 50, 50, 0);
    setclip(g, 10, 10, 0, 0, 1);
    EXPECT_EQ(clip_data(g)[1], 0x1b9e53feu);
    setclip(g, 0, 0, 50, 50, 0);
    setclip(g, 10, 10, 0, 0, 2);
    EXPECT_EQ(clip_data(g)[1], 0xeaeda96fu);
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    setclip(g, 0, 0, 50, 50, 1);
    EXPECT_EQ(clip_data(g)[1], 0xeaeda96fu);
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    setclip(g, 0, 0, 50, 50, 4);
    EXPECT_EQ(clip_data(g)[1], 0xc29ac855u);
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{0, 0, 100, 100}));
    EXPECT_TRUE(gvis(g, 60, 60));
    EXPECT_FALSE(gvis(g, 10, 10));
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    setclip(g, 0, 0, 50, 50, 2);
    EXPECT_EQ(clip_data(g)[1], 0xa322349bu);
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    ASSERT_EQ(call("GdipTranslateClip", {I(g), R(3), R(3)}), Status::Ok);
    EXPECT_EQ(clip_data(g)[1], 0xa322349bu);
    setclip(g, 90, 90, 20, 20, 0);
    EXPECT_TRUE(gvisrect(g, 95, 95, 10, 10));
    EXPECT_FALSE(gvisrect(g, 100, 100, 10, 10));
    EXPECT_FALSE(gvisrect(g, 99.6f, 99.6f, 10, 10));
}

TEST_P(Gdip_region_Test, ClipUnderTransformsAndUnits) {
    const u64 g = graphics(100, 100);
    // Rotated world: the default clip bounds are the infinite square mapped back.
    world(g, 0.70710678f, 0.70710678f, -0.70710678f, 0.70710678f, 50, 0);
    const auto rb = getf("GdipGetVisibleClipBounds", g);
    EXPECT_NEAR(rb[0], -35.3553f, 1e-3);
    EXPECT_NEAR(rb[2], 141.421f, 1e-3);
    setclip(g, 0, 0, 20, 20, 0);
    const auto cb = getf("GdipGetClipBounds", g);
    EXPECT_NEAR(cb[0], -9.1924f, 1e-3);
    EXPECT_NEAR(cb[2], 39.598f, 1e-3);
    EXPECT_EQ(geti("GdipGetClipBoundsI", g), (Rect4{-9, -9, 40, 40}));
    EXPECT_EQ(clip_data(g)[4], 0x10000001u);  // a path in world space
    world(g, 1, 0, 0, 1, 0, 0);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{36, 1, 28, 28}));
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    world(g, 0.70710678f, 0.70710678f, -0.70710678f, 0.70710678f, 50, 0);
    setclip(g, 0, 0, 10, 10, 0);
    ASSERT_EQ(call("GdipTranslateClip", {I(g), R(10), R(0)}), Status::Ok);
    const auto tb = getf("GdipGetClipBounds", g);
    EXPECT_NEAR(tb[0], 6.36396f, 1e-3);
    EXPECT_NEAR(tb[1], -4.24264f, 1e-3);
    EXPECT_NEAR(tb[2], 19.0919f, 1e-3);
    world(g, 1, 0, 0, 1, 0, 0);
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    // Millimetres: 100 px = 26.4583 mm.
    G(g).state.page_unit = gdip::UnitMillimeter;
    const auto mm = getf("GdipGetVisibleClipBounds", g);
    EXPECT_NEAR(mm[2], 26.45833f, 1e-4);
    EXPECT_EQ(geti("GdipGetVisibleClipBoundsI", g), (Rect4{0, 0, 26, 26}));
    setclip(g, 1, 1, 10, 10, 0);
    const auto mr = getf("GdipGetClipBounds", g);
    EXPECT_NEAR(mr[0], 1.058333f, 1e-4);
    EXPECT_NEAR(mr[2], 10.054166f, 1e-4);
    G(g).state.page_unit = gdip::UnitPixel;
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{4, 4, 38, 38}));
    ASSERT_EQ(call("GdipResetClip", {I(g)}), Status::Ok);
    // PixelOffsetModeHalf.
    G(g).state.pixel_offset_mode = 4;
    EXPECT_EQ(getf("GdipGetClipBounds", g),
              (std::array<float, 4>{-4194303.5f, -4194303.5f, 8388608, 8388608}));
    EXPECT_EQ(geti("GdipGetClipBoundsI", g), (Rect4{-4194303, -4194303, 8388608, 8388608}));
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{0.5f, 0.5f, 100, 100}));
    EXPECT_EQ(geti("GdipGetVisibleClipBoundsI", g), (Rect4{1, 1, 100, 100}));
    setclip(g, 10, 10, 10, 10, 0);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{10.5f, 10.5f, 10, 10}));
    EXPECT_EQ(geti("GdipGetClipBoundsI", g), (Rect4{11, 11, 10, 10}));
    EXPECT_TRUE(gvis(g, 10, 10));
    EXPECT_FALSE(gvis(g, 9.6f, 10));
    G(g).state.pixel_offset_mode = 0;
    EXPECT_EQ(clip_data(g)[1], 0xb5a752b1u);  // device 9.5,9.5,10,10
}

TEST_P(Gdip_region_Test, ClipFromPathRegionHrgnGraphics) {
    const u64 g = graphics(100, 100);
    ASSERT_EQ(call("GdipCreateRegion", {I(OUT)}), Status::Ok);
    // SetClipPath (ellipse 10,10,20,20 = ellipse10 scaled by 2 around +10).
    const u64 big = path({}, {});
    {
        auto* p = mgr.objects().get<gdip::GpPath>(ellipse10(0, 0));
        auto* q = mgr.objects().get<gdip::GpPath>(big);
        *q = *p;
        for (auto& pt : q->points) { pt.X = pt.X * 2 + 10; pt.Y = pt.Y * 2 + 10; }
    }
    ASSERT_EQ(call("GdipSetClipPath", {I(g), I(big), I(0)}), Status::Ok);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{10, 11, 20, 19}));
    EXPECT_TRUE(gvis(g, 20, 20));
    EXPECT_FALSE(gvis(g, 11, 11));
    ASSERT_EQ(call("GdipSetClipPath", {I(g), I(rect_path(0, 0, 10, 10, 1)), I(0)}), Status::Ok);
    EXPECT_EQ(clip_data(g)[1], 0xddb1ed7cu);
    // SetClipRegion maps the region through the world transform; the
    // region itself is untouched; GetClip maps back.
    world(g, 1, 0, 0, 1, 10, 10);
    const u64 r = region_rect(5, 6, 7, 8);
    ASSERT_EQ(call("GdipSetClipRegion", {I(g), I(r), I(0)}), Status::Ok);
    EXPECT_EQ(data(r)[1], 0x917cf60cu);
    EXPECT_EQ(clip_data(g)[1], 0x917cf60cu);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{5, 6, 7, 8}));
    // HRGN clip is device units.
    world(g, 2, 0, 0, 2, 0, 0);
    const Handle h = gdi.create_rect_rgn(10, 10, 30, 30);
    ASSERT_EQ(call("GdipSetClipHrgn", {I(g), I(h), I(0)}), Status::Ok);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{5, 5, 10, 10}));
    EXPECT_EQ(clip_data(g)[1], 0x8a9ccc7cu);
    EXPECT_EQ(call("GdipSetClipHrgn", {I(g), I(0x12345678), I(0)}), Status::InvalidParameter);
    world(g, 1, 0, 0, 1, 0, 0);
    setclip(g, 10, 10, 40, 40, 0);
    const Handle h2 = gdi.create_rect_rgn(0, 0, 30, 30);
    ASSERT_EQ(call("GdipSetClipHrgn", {I(g), I(h2), I(1)}), Status::Ok);
    EXPECT_EQ(clip_data(g)[1], 0x3bec9e2fu);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{10, 10, 20, 20}));
    // SetClipGraphics copies the source's device clip.
    const u64 g2 = graphics(50, 50);
    setclip(g2, 1, 2, 3, 4, 0);
    world(g, 2, 0, 0, 2, 0, 0);
    ASSERT_EQ(call("GdipSetClipGraphics", {I(g), I(g2), I(0)}), Status::Ok);
    EXPECT_EQ(clip_data(g)[1], 0xf866819du);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{0.5f, 1, 1.5f, 2}));
    ASSERT_EQ(call("GdipResetClip", {I(g2)}), Status::Ok);
    ASSERT_EQ(call("GdipSetClipGraphics", {I(g), I(g2), I(0)}), Status::Ok);
    EXPECT_EQ(getf("GdipGetClipBounds", g), (std::array<float, 4>{-2097152, -2097152, 4194304, 4194304}));
    EXPECT_EQ(getf("GdipGetVisibleClipBounds", g), (std::array<float, 4>{0, 0, 50, 50}));
    EXPECT_EQ(call("GdipSetClipGraphics", {I(g), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetClipRegion", {I(g), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetClipPath", {I(g), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetClip", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetClipBounds", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsClipEmpty", {I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsVisiblePoint", {I(g), R(1), R(1), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipResetClip", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTranslateClip", {I(0), R(1), R(1)}), Status::InvalidParameter);
    // A saved state's clip is never edited in place.
    world(g, 1, 0, 0, 1, 0, 0);
    setclip(g, 0, 0, 10, 10, 0);
    const auto saved = G(g).state.clip;
    setclip(g, 5, 5, 10, 10, 2);
    EXPECT_EQ(saved->kind, gdip::GpRegion::Kind::Rect);
    // Between GetDC and ReleaseDC every clip call is ObjectBusy.
    G(g).hdc_out = true;
    EXPECT_EQ(call("GdipResetClip", {I(g)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipGetClipBounds", {I(g), I(OUT2)}), Status::ObjectBusy);
    G(g).hdc_out = false;
}

INSTANTIATE_GDIP_BITNESS(Gdip_region_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
