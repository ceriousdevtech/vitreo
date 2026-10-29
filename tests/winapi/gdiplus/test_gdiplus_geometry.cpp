// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_geometry.cpp
/// @brief Contract tests for the GDI+ geometry area (gdiplus_geometry.cpp,
/// gdiplus_geometry_math.cpp), run in both bitnesses through
/// gdiplus_fixture.hpp.
///
/// Expected arrays are the Windows 11 output of
/// tools/oracle_probe/gdiplus_geometry_probe.c (x64 .expected; the x86 run
/// agrees within REAL rounding except where noted in
/// .docs/contracts/gdiplus/geometry.md); the probe case name is quoted.

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_pen.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace vitreo::winapi::gdip_test {
namespace {

using gdip::PointF;

std::vector<PointF> parse_points(const std::string& s) {
    std::vector<PointF> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        const auto comma = item.find(',');
        out.push_back({std::strtof(item.substr(0, comma).c_str(), nullptr),
                       std::strtof(item.substr(comma + 1).c_str(), nullptr)});
    }
    return out;
}

std::vector<u8> parse_types(const std::string& s) {
    std::vector<u8> out;
    for (usize i = 0; i + 1 < s.size(); i += 2) {
        out.push_back(static_cast<u8>(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
    }
    return out;
}

struct RectF_ {
    float x, y, w, h;
};

class Gdip_geometry_Test : public GdipTest {
protected:
    static constexpr GuestAddr PTS = BUF;            ///< input points
    static constexpr GuestAddr TYPES = BUF + 0x4000;  ///< input types
    static constexpr GuestAddr RPTS = BUF + 0x8000;   ///< read-back points
    static constexpr GuestAddr RTYPES = BUF + 0x20000;

    u64 new_path(i32 fill = 0) {
        EXPECT_EQ(call("GdipCreatePath", {I(static_cast<u32>(fill)), I(OUT)}), Status::Ok);
        return read_handle(OUT);
    }
    u64 new_matrix(float a, float b, float c, float d, float e, float f) {
        EXPECT_EQ(call("GdipCreateMatrix2", {R(a), R(b), R(c), R(d), R(e), R(f), I(OUT)}),
                  Status::Ok);
        return read_handle(OUT);
    }
    u64 new_pen(float width, i32 unit = 2) {
        gdip::GpPen pen;
        pen.width = width;
        pen.unit = unit;
        return mgr.objects().add(std::move(pen));
    }
    gdip::GpPen& pen_obj(u64 h) { return *mgr.objects().get<gdip::GpPen>(h); }

    GuestAddr put_points(const std::vector<PointF>& pts, GuestAddr at = PTS) {
        for (usize i = 0; i < pts.size(); ++i) {
            write_real(at + i * 8, pts[i].X);
            write_real(at + i * 8 + 4, pts[i].Y);
        }
        return at;
    }
    GuestAddr put_ints(const std::vector<i32>& v, GuestAddr at = PTS) {
        for (usize i = 0; i < v.size(); ++i) write_int(at + i * 4, v[i]);
        return at;
    }
    GuestAddr put_types(const std::vector<u8>& t, GuestAddr at = TYPES) {
        for (usize i = 0; i < t.size(); ++i) EXPECT_TRUE(memory.write_u8(at + i, t[i]).is_ok());
        return at;
    }

    i32 count(u64 p) {
        EXPECT_EQ(call("GdipGetPointCount", {I(p), I(OUT3)}), Status::Ok);
        return read_int(OUT3);
    }
    std::vector<PointF> points(u64 p) {
        const i32 n = count(p);
        std::vector<PointF> out;
        if (n <= 0) return out;
        EXPECT_EQ(call("GdipGetPathPoints", {I(p), I(RPTS), I(static_cast<u32>(n))}), Status::Ok);
        for (i32 i = 0; i < n; ++i) {
            out.push_back({read_real(RPTS + static_cast<u64>(i) * 8),
                           read_real(RPTS + static_cast<u64>(i) * 8 + 4)});
        }
        return out;
    }
    std::vector<u8> types(u64 p) {
        const i32 n = count(p);
        std::vector<u8> out;
        if (n <= 0) return out;
        EXPECT_EQ(call("GdipGetPathTypes", {I(p), I(RTYPES), I(static_cast<u32>(n))}), Status::Ok);
        for (i32 i = 0; i < n; ++i) out.push_back(memory.read_u8(RTYPES + static_cast<u64>(i)).value());
        return out;
    }
    /// Compare the path with a probe line's pts=/types= fields.
    void expect_path(u64 p, const std::string& pts, const std::string& tys, float tol = 1e-4f,
                     const char* what = "") {
        const auto want = parse_points(pts);
        const auto got = points(p);
        ASSERT_EQ(got.size(), want.size()) << what;
        for (usize i = 0; i < want.size(); ++i) {
            const float t = tol + 2e-6f * std::max(std::fabs(want[i].X), std::fabs(want[i].Y));
            EXPECT_NEAR(got[i].X, want[i].X, t) << what << " point " << i;
            EXPECT_NEAR(got[i].Y, want[i].Y, t) << what << " point " << i;
        }
        EXPECT_EQ(types(p), parse_types(tys)) << what;
    }
    void expect_empty(u64 p) { EXPECT_EQ(count(p), 0); }
    std::vector<float> elements(u64 mx) {
        EXPECT_EQ(call("GdipGetMatrixElements", {I(mx), I(BUF)}), Status::Ok);
        std::vector<float> v;
        for (int i = 0; i < 6; ++i) v.push_back(read_real(BUF + 4 * static_cast<u64>(i)));
        return v;
    }
    void expect_elements(u64 mx, std::initializer_list<float> want, float tol = 2e-5f) {
        const auto got = elements(mx);
        usize i = 0;
        for (float w : want) {
            EXPECT_NEAR(got[i], w, tol + 1e-6f * std::fabs(w)) << "element " << i;
            ++i;
        }
    }
    Status arc(u64 p, float x, float y, float w, float h, float s, float sw) {
        return call("GdipAddPathArc", {I(p), R(x), R(y), R(w), R(h), R(s), R(sw)});
    }
    Status line(u64 p, float a, float b, float c, float d) {
        return call("GdipAddPathLine", {I(p), R(a), R(b), R(c), R(d)});
    }
    Status rect(u64 p, float x, float y, float w, float h) {
        return call("GdipAddPathRectangle", {I(p), R(x), R(y), R(w), R(h)});
    }
    Status ellipse(u64 p, float x, float y, float w, float h) {
        return call("GdipAddPathEllipse", {I(p), R(x), R(y), R(w), R(h)});
    }
    Status bezier(u64 p, std::initializer_list<float> v) {
        std::vector<float> a(v);
        return call("GdipAddPathBezier", {I(p), R(a[0]), R(a[1]), R(a[2]), R(a[3]), R(a[4]),
                                          R(a[5]), R(a[6]), R(a[7])});
    }
    RectF_ bounds(u64 p, u64 matrix, u64 pen) {
        EXPECT_EQ(call("GdipGetPathWorldBounds", {I(p), I(OUT2), I(matrix), I(pen)}), Status::Ok);
        return {read_real(OUT2), read_real(OUT2 + 4), read_real(OUT2 + 8), read_real(OUT2 + 12)};
    }
    void put_reals_rect(float x, float y, float w, float h) { write_reals(PTS, {x, y, w, h}); }
};

void expect_rect(RectF_ r, float x, float y, float w, float h, float tol = 1e-3f) {
    EXPECT_NEAR(r.x, x, tol);
    EXPECT_NEAR(r.y, y, tol);
    EXPECT_NEAR(r.w, w, tol);
    EXPECT_NEAR(r.h, h, tol);
}

// ============================================================== Builders

TEST_P(Gdip_geometry_Test, ArcEmitsQuarterTurnBeziersFromTheVisualAngle) {
    struct Case {
        float x, y, w, h, s, sw;
        const char* pts;
        const char* types;
    };
    const Case cases[] = {
        {10, 20, 100, 50, 0, 90, "109.999992,44.9999962;109.999985,58.8071136;87.6142349,69.9999924;60,69.9999924", "00030303"},
        {10, 20, 100, 50, 30, 120, "92.7326813,63.8982124;73.9440918,72.0339203;46.0559235,72.0339203;27.2673187,63.8982201", "00030303"},
        {10, 20, 100, 50, 0, 360, "109.999992,44.9999962;109.999992,58.8071136;87.6142273,69.9999924;60.0000191,69.9999924;32.3857803,70;10.0000114,58.8071213;10,45.0000191;9.9999733,31.1929016;32.3857193,20.0000095;59.9999619,20;87.6141891,19.9999886;109.999969,31.1928616;109.999992,44.9999771", "00030303030303030303030383"},
        {10, 20, 100, 50, 45, -270, "82.3606796,67.360672;107.059601,61.1859436;117.070808,46.1691132;104.721344,33.8196526;92.3718796,21.470192;62.3382187,16.4645863;37.6392937,22.6393223;12.9403763,28.8140602;2.92917252,43.8308907;15.278656,56.1803551;20.116848,61.0185318;27.9629669,64.9415894;37.6393242,67.3606796", "00030303030303030303030303"},
        {10, 20, 100, 50, 10, 400, "107.153755,53.3144722;97.969841,66.3356247;69.4133453,73.1688309;43.3710556,68.5768814;17.3287468,63.9849281;3.66231537,49.7066803;12.8462181,36.6855278;22.0301285,23.6643753;50.5866241,16.8311558;76.6289139,21.423111;102.671219,26.0150604;116.337669,40.2933006;107.153778,53.3144531", "00030303030303030303030383"},
        {10, 20, 100, 50, -30, 45, "92.7326508,26.1017647;110.403877,33.7536201;115.112602,46.50634;104.070633,56.8086586", "00030303"},
        {10, 20, 100, 50, 370, 20, "107.153755,53.314476;104.266136,57.4086075;99.2960358,61.0561981;92.7326736,63.8982162", "00030303"},
        {10, 20, 100, 50, 0, 0.5f, "109.999985,44.9999962;109.999985,45.1454315;109.997452,45.290863;109.992378,45.4362755", "00030303"},
        {10, 20, 100, 50, 89, 2, "60.4363747,69.9990387;60.1454697,70.0003052;59.8545456,70.0003128;59.5636406,69.9990387", "00030303"},
        {10, 20, 100, 50, 200, 170, "19.5759926,30.2869053;35.8276291,19.1241245;67.1006241,16.6621819;89.4261551,24.7879963;107.296593,31.2922802;114.504997,42.8916931;107.153778,53.3144455", "00030303030303"},
        {0, 0, 100, 100, 15, 100, "98.2962799,62.940937;91.1492004,89.6142349;63.732357,105.443367;37.0590553,98.2962799;34.2475128,97.5429306;31.5071201,96.545517;28.8691101,95.3153915", "00030303030303"},
        {5, 5, 40, 80, 60, 75, "40.1185799,71.1861267;32.8874817,87.8856506;20.2566948,89.6993561;11.90693,75.2371597;9.91783714,71.791954;8.28821754,67.5955887;7.11145592,62.888546", "00030303030303"},
    };
    for (const Case& c : cases) {
        const u64 p = new_path();
        EXPECT_EQ(arc(p, c.x, c.y, c.w, c.h, c.s, c.sw), Status::Ok);
        expect_path(p, c.pts, c.types, 2e-4f, "arc");
        call("GdipDeletePath", {I(p)});
    }
    const u64 p = new_path();
    EXPECT_EQ(arc(p, 10, 20, 100, 50, 0, 0), Status::Ok);  // "arc(..,0,0)"
    expect_empty(p);
    // Empty / negative rectangles are InvalidParameter ("arc(0,0,0,50,0,90)").
    EXPECT_EQ(arc(p, 0, 0, 0, 50, 0, 90), Status::InvalidParameter);
    EXPECT_EQ(arc(p, 0, 0, 50, 0, 0, 90), Status::InvalidParameter);
    EXPECT_EQ(arc(p, 0, 0, -50, 50, 0, 90), Status::InvalidParameter);
    expect_empty(p);
    EXPECT_EQ(arc(0, 0, 0, 1, 1, 0, 90), Status::InvalidParameter);  // "arc_nullpath"
    EXPECT_EQ(call("GdipAddPathArcI", {I(p), I(1), I(2), I(30), I(40), R(10), R(100)}), Status::Ok);
    expect_path(p, "30.8705273,24.6220703;29.7844276,35.572422;22.2462101,43.2754974;14.0334463,41.8273697;12.4422121,41.5467911;10.8950653,40.9275017;9.45103645,39.9931335", "00030303030303", 2e-4f, "arcI");
}

TEST_P(Gdip_geometry_Test, EllipsePieRectangleShapes) {
    u64 p = new_path();
    EXPECT_EQ(ellipse(p, 10, 20, 100, 50), Status::Ok);
    expect_path(p, "110,45;110,58.8071175;87.6142349,70;60,70;32.3857613,70;10,58.8071175;10,45;10,31.1928806;32.3857613,20;60,20;87.6142349,20;110,31.1928806;110,45", "00030303030303030303030383");
    call("GdipResetPath", {I(p)});
    EXPECT_EQ(ellipse(p, -5, 3, 7, 11), Status::Ok);
    expect_path(p, "2,8.5;2,11.5375652;0.432996511,14;-1.5,14;-3.43299651,14;-5,11.5375652;-5,8.5;-5,5.46243382;-3.43299651,3;-1.5,3;0.432996511,3;2,5.46243382;2,8.5", "00030303030303030303030383");
    call("GdipResetPath", {I(p)});
    EXPECT_EQ(ellipse(p, 0, 0, -10, 10), Status::Ok);  // any size
    expect_path(p, "-10,5;-10,7.76142359;-7.76142359,10;-5,10;-2.23857641,10;-0,7.76142359;-0,5;-0,2.23857641;-2.23857641,-0;-5,-0;-7.76142359,-0;-10,2.23857641;-10,5", "00030303030303030303030383");

    auto pie = [&](float x, float y, float w, float h, float s, float sw) {
        call("GdipResetPath", {I(p)});
        return call("GdipAddPathPie", {I(p), R(x), R(y), R(w), R(h), R(s), R(sw)});
    };
    EXPECT_EQ(pie(10, 20, 100, 50, 30, 120), Status::Ok);
    expect_path(p, "60,45;92.7326813,63.8982124;73.9440918,72.0339203;46.0559235,72.0339203;27.2673187,63.8982201", "0001030383", 2e-4f);
    EXPECT_EQ(pie(10, 20, 100, 50, 0, 0), Status::Ok);
    expect_path(p, "60,45", "80");
    EXPECT_EQ(pie(10, 20, 100, 50, -45, -90), Status::Ok);
    expect_path(p, "60,45;82.3606415,22.6393108;68.284256,19.1202202;51.7156372,19.1202297;37.6392632,22.6393318", "0001030383", 2e-4f);
    EXPECT_EQ(pie(10, 20, 100, 50, 0, 450), Status::Ok);
    EXPECT_EQ(count(p), 14);
    EXPECT_EQ(types(p), parse_types("0001030303030303030303030383"));
    // The centre is added before the rectangle is rejected ("pie(0,0,0,10,0,90)").
    EXPECT_EQ(pie(0, 0, 0, 10, 0, 90), Status::InvalidParameter);
    expect_path(p, "0,5", "80");

    call("GdipResetPath", {I(p)});
    EXPECT_EQ(rect(p, 1, 2, 30, 40), Status::Ok);
    expect_path(p, "1,2;31,2;31,42;1,42", "00010181");
    call("GdipResetPath", {I(p)});
    for (auto r : {std::array<float, 4>{1, 2, 0, 40}, {1, 2, 30, 0}, {1, 2, -30, 40}, {1, 2, 30, -40}}) {
        EXPECT_EQ(rect(p, r[0], r[1], r[2], r[3]), Status::Ok);
    }
    expect_empty(p);
    put_points({{0, 0}, {10, 10}, {5, 5}, {0, 10}, {20, 20}, {5, 5}});  // rects as REAL quads
    EXPECT_EQ(call("GdipAddPathRectangles", {I(p), I(PTS), I(3)}), Status::Ok);
    expect_path(p, "0,0;10,0;10,10;0,10;20,20;25,20;25,25;20,25", "0001018100010181");
    EXPECT_EQ(call("GdipAddPathRectangles", {I(p), I(PTS), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathRectangles", {I(p), I(0), I(1)}), Status::InvalidParameter);
    call("GdipResetPath", {I(p)});
    put_ints({1, 2, 3, 4});
    EXPECT_EQ(call("GdipAddPathRectanglesI", {I(p), I(PTS), I(1)}), Status::Ok);
    expect_path(p, "1,2;4,2;4,6;1,6", "00010181");
}

TEST_P(Gdip_geometry_Test, LinesJoinTheOpenFigureAndDropARepeatedFirstPoint) {
    u64 p = new_path();
    line(p, 0, 0, 10, 10);
    line(p, 20, 20, 30, 30);
    line(p, 30, 30, 40, 0);
    expect_path(p, "0,0;10,10;20,20;30,30;40,0", "0001010101");  // "lines_connect"
    EXPECT_EQ(call("GdipClosePathFigure", {I(p)}), Status::Ok);
    line(p, 1, 1, 2, 2);
    call("GdipStartPathFigure", {I(p)});
    line(p, 5, 5, 6, 6);
    expect_path(p, "0,0;10,10;20,20;30,30;40,0;1,1;2,2;5,5;6,6", "000101018100010001");

    p = new_path();
    put_points({{3, 4}});
    EXPECT_EQ(call("GdipAddPathLine2", {I(p), I(PTS), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathLine2", {I(p), I(PTS), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathLine2", {I(p), I(0), I(2)}), Status::InvalidParameter);
    put_points({{0, 0}, {0, 0}, {5, 5}, {5, 5}});
    EXPECT_EQ(call("GdipAddPathLine2", {I(p), I(PTS), I(4)}), Status::Ok);
    expect_path(p, "3,4;0,0;0,0;5,5;5,5", "0001010101");  // "line2_dups"

    p = new_path();
    line(p, 3, 3, 3, 3);
    line(p, 3, 3, 7, 7);
    expect_path(p, "3,3;3,3;7,7", "000101");  // "line_after_degenerate"
    p = new_path();
    EXPECT_EQ(call("GdipAddPathLineI", {I(p), I(1), I(2), I(3), I(4)}), Status::Ok);
    put_ints({3, 4, 9, 9});
    EXPECT_EQ(call("GdipAddPathLine2I", {I(p), I(PTS), I(2)}), Status::Ok);
    expect_path(p, "1,2;3,4;9,9", "000101");
}

TEST_P(Gdip_geometry_Test, MixedPrimitivesConnectAsGdiplusDoes) {
    // "mixed_connect": arcs, Béziers, lines and curves join the open figure;
    // rectangles, ellipses, pies, closed curves and polygons are their own
    // closed figures.
    const u64 p = new_path();
    line(p, 0, 0, 10, 0);
    arc(p, 0, 0, 20, 20, 0, 90);
    bezier(p, {1, 1, 2, 2, 3, 3, 4, 4});
    rect(p, 50, 50, 5, 5);
    line(p, 60, 60, 70, 70);
    ellipse(p, 0, 0, 4, 4);
    line(p, 80, 80, 90, 90);
    call("GdipAddPathPie", {I(p), R(0), R(0), R(10), R(10), R(0), R(90)});
    line(p, 1, 2, 3, 4);
    put_points({{10, 10}, {50, 80}, {100, 20}});
    call("GdipAddPathCurve", {I(p), I(PTS), I(3)});
    call("GdipAddPathClosedCurve", {I(p), I(PTS), I(3)});
    call("GdipAddPathPolygon", {I(p), I(PTS), I(3)});
    line(p, 9, 9, 8, 8);
    expect_path(p,
                "0,0;10,0;19.9999981,9.99999905;19.9999981,15.5228453;15.5228472,19.9999962;10.000001,19.9999981;1,1;2,2;3,3;4,4;50,50;55,50;55,55;50,55;60,60;70,70;4,2;4,3.10456944;3.10456944,4;2,4;0.895430565,4;-0,3.10456944;-0,2;-0,0.895430565;0.895430565,-0;2,-0;3.10456944,-0;4,0.895430565;4,2;80,80;90,90;5,5;9.99999905,4.99999952;9.99999905,7.76142263;7.76142359,9.99999809;5.00000048,9.99999905;1,2;3,4;10,10;16.6666641,21.6666641;35,78.3333282;50,80;64.9999924,81.6666565;91.6666641,29.9999981;100,20;10,10;1.66666794,19.9999981;35,78.3333282;50,80;64.9999924,81.6666565;106.666656,31.6666641;100,20;93.3333282,8.33333397;18.3333321,9.53674316e-07;10,10;10,10;50,80;100,20;9,9;8,8",
                "000101030303010303030001018100010003030303030303030303038300010001030383000101030303030303000303030303030303830001810001",
                2e-4f);
}

TEST_P(Gdip_geometry_Test, BeziersNeedThreeNPlusOnePoints) {
    const u64 p = new_path();
    put_points({{0, 0}, {10, 0}, {10, 10}, {0, 10}, {-5, 5}, {-5, 0}, {0, 0}});
    EXPECT_EQ(call("GdipAddPathBeziers", {I(p), I(PTS), I(7)}), Status::Ok);
    for (u32 n : {3u, 5u, 1u}) {
        EXPECT_EQ(call("GdipAddPathBeziers", {I(p), I(PTS), I(n)}), Status::InvalidParameter) << n;
    }
    put_points({{0, 0}, {10, 0}, {10, 10}, {0, 10}});
    EXPECT_EQ(call("GdipAddPathBeziers", {I(p), I(PTS), I(4)}), Status::Ok);
    expect_path(p, "0,0;10,0;10,10;0,10;-5,5;-5,0;0,0;10,0;10,10;0,10", "00030303030303030303");
    EXPECT_EQ(call("GdipAddPathBezierI", {I(p), I(1), I(1), I(2), I(2), I(3), I(3), I(4), I(4)}),
              Status::Ok);
    EXPECT_EQ(count(p), 14);
}

TEST_P(Gdip_geometry_Test, CardinalSplinesUseTensionOverThree) {
    const std::vector<PointF> k3 = {{10, 10}, {50, 80}, {100, 20}};
    const std::vector<PointF> k5 = {{0, 0}, {20, 40}, {50, 10}, {80, 60}, {120, 30}};
    auto run = [&](const std::vector<PointF>& pts, const char* fn, std::initializer_list<Arg> extra) {
        const u64 p = new_path();
        put_points(pts);
        std::vector<Arg> args = {I(p), I(PTS), I(static_cast<u32>(pts.size()))};
        args.insert(args.end(), extra.begin(), extra.end());
        Status s = Status::NotImplemented;
        switch (args.size()) {
        case 3: s = call(fn, {args[0], args[1], args[2]}); break;
        case 4: s = call(fn, {args[0], args[1], args[2], args[3]}); break;
        case 6: s = call(fn, {args[0], args[1], args[2], args[3], args[4], args[5]}); break;
        default: ADD_FAILURE();
        }
        return std::make_pair(s, p);
    };
    auto [s1, p1] = run(k3, "GdipAddPathCurve", {});
    EXPECT_EQ(s1, Status::Ok);
    expect_path(p1, "10,10;16.6666641,21.6666641;35,78.3333282;50,80;64.9999924,81.6666565;91.6666641,29.9999981;100,20", "00030303030303");
    auto [s2, p2] = run(k5, "GdipAddPathCurve2", {R(1)});
    EXPECT_EQ(s2, Status::Ok);
    expect_path(p2, "0,0;6.66666603,13.3333321;3.33333588,36.6666641;20,40;36.6666641,43.3333321;30,3.33333397;50,10;69.9999924,16.666666;56.6666679,53.3333321;80,60;103.333328,66.6666565;106.666656,39.9999962;120,30", "00030303030303030303030303");
    auto [s3, p3] = run(k5, "GdipAddPathCurve2", {R(-1)});
    EXPECT_EQ(s3, Status::Ok);
    expect_path(p3, "0,0;-6.66666698,-13.333334;36.6666679,43.3333321;20,40;3.33333206,36.6666641;70,16.6666679;50,10;29.9999962,3.33333206;103.333336,66.6666641;80,60;56.6666603,53.3333282;133.333328,19.9999981;120,30", "00030303030303030303030303");
    auto [s4, p4] = run(k5, "GdipAddPathCurve2", {R(0)});
    EXPECT_EQ(s4, Status::Ok);
    expect_path(p4, "0,0;0,0;20,40;20,40;20,40;50,10;50,10;50,10;80,60;80,60;80,60;120,30;120,30", "00030303030303030303030303");
    auto [s5, p5] = run(k5, "GdipAddPathCurve3", {I(1), I(2), R(0.5f)});
    EXPECT_EQ(s5, Status::Ok);
    expect_path(p5, "20,40;28.3333321,41.6666641;40,6.66666698;50,10;59.9999962,13.333333;68.3333282,56.6666641;80,60", "00030303030303");
    auto [s6, p6] = run(k5, "GdipAddPathCurve3", {I(3), I(1), R(0.5f)});
    EXPECT_EQ(s6, Status::Ok);
    expect_path(p6, "80,60;91.6666565,63.3333282;113.333328,34.9999962;120,30", "00030303");
    for (auto bad : {std::array<u32, 2>{0, 5}, {4, 1}, {0, 0}, {0xFFFFFFFF, 1}}) {
        auto [s, p] = run(k5, "GdipAddPathCurve3", {I(bad[0]), I(bad[1]), R(0.5f)});
        EXPECT_EQ(s, Status::InvalidParameter);
        expect_empty(p);
    }
    auto [s7, p7] = run({{0, 0}, {30, 40}}, "GdipAddPathCurve", {});
    EXPECT_EQ(s7, Status::Ok);
    expect_path(p7, "0,0;4.99999952,6.66666603;25,33.3333321;30,40", "00030303");
    auto [s8, p8] = run({{0, 0}}, "GdipAddPathCurve", {});
    EXPECT_EQ(s8, Status::InvalidParameter);
    auto [s9, p9] = run(k3, "GdipAddPathClosedCurve", {});
    EXPECT_EQ(s9, Status::Ok);
    expect_path(p9, "10,10;1.66666794,19.9999981;35,78.3333282;50,80;64.9999924,81.6666565;106.666656,31.6666641;100,20;93.3333282,8.33333397;18.3333321,9.53674316e-07;10,10", "00030303030303030383");
    auto [s10, p10] = run(k5, "GdipAddPathClosedCurve2", {R(1)});
    EXPECT_EQ(s10, Status::Ok);
    expect_path(p10, "0,0;-33.3333321,3.33333302;3.33333588,36.6666641;20,40;36.6666641,43.3333321;30,3.33333397;50,10;69.9999924,16.666666;56.6666679,53.3333321;80,60;103.333328,66.6666565;146.666656,49.9999962;120,30;93.3333359,10.0000019;33.3333282,-3.33333302;0,0", "00030303030303030303030303030383");
    auto [s11, p11] = run({{0, 0}, {30, 40}}, "GdipAddPathClosedCurve", {});
    EXPECT_EQ(s11, Status::InvalidParameter);
    auto [s12, p12] = run({{0, 0}, {0, 0}, {10, 10}, {20, 0}}, "GdipAddPathCurve", {});
    EXPECT_EQ(s12, Status::Ok);
    expect_path(p12, "0,0;-0,-0;-1.66666651,-1.66666651;0,0;1.66666651,1.66666651;6.66666698,10;10,10;13.333333,10;18.3333321,1.66666651;20,0", "00030303030303030303");
    // INT variants read GDI+ `Point`.
    const u64 pi = new_path();
    put_ints({10, 10, 50, 80, 100, 20});
    EXPECT_EQ(call("GdipAddPathCurveI", {I(pi), I(PTS), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathClosedCurve2I", {I(pi), I(PTS), I(3), R(0.5f)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathCurve2I", {I(pi), I(PTS), I(3), R(0.5f)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathCurve3I", {I(pi), I(PTS), I(3), I(0), I(1), R(0.5f)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathClosedCurveI", {I(pi), I(PTS), I(3)}), Status::Ok);
    EXPECT_EQ(count(pi), 7 + 10 + 7 + 4 + 10);
    EXPECT_EQ(call("GdipAddPathCurve", {I(pi), I(0), I(3)}), Status::InvalidParameter);
}

TEST_P(Gdip_geometry_Test, PolygonDropsARepeatedLastPoint) {
    const u64 p = new_path();
    put_points({{0, 0}, {10, 0}, {10, 10}, {0, 0}});
    EXPECT_EQ(call("GdipAddPathPolygon", {I(p), I(PTS), I(4)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathPolygon", {I(p), I(PTS), I(3)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathPolygon", {I(p), I(PTS), I(2)}), Status::InvalidParameter);
    expect_path(p, "0,0;10,0;10,10;0,0;10,0;10,10", "000181000181");
    put_ints({0, 0, 5, 0, 5, 5});
    EXPECT_EQ(call("GdipAddPathPolygonI", {I(p), I(PTS), I(3)}), Status::Ok);
    EXPECT_EQ(count(p), 9);
}

// ======================================================== Path object

TEST_P(Gdip_geometry_Test, PathStateGettersAndErrors) {
    EXPECT_EQ(call("GdipCreatePath", {I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeletePath", {I(0)}), Status::InvalidParameter);
    u64 p = new_path(2);  // stored unchecked ("create_fill2")
    EXPECT_EQ(call("GdipGetPathFillMode", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 2);
    EXPECT_EQ(call("GdipSetPathFillMode", {I(p), I(5)}), Status::Ok);
    call("GdipGetPathFillMode", {I(p), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 5);
    EXPECT_EQ(call("GdipGetPathLastPoint", {I(p), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPathPoints", {I(p), I(RPTS), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPathPoints", {I(p), I(RPTS), I(1)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathTypes", {I(p), I(RTYPES), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPointCount", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPointCount", {I(p), I(0)}), Status::InvalidParameter);

    line(p, 1.5f, 2.5f, -0.5f, -1.5f);
    line(p, 0.49f, 2.51f, 3.5f, 4.5f);
    EXPECT_EQ(call("GdipGetPathLastPoint", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2), 3.5f);
    EXPECT_FLOAT_EQ(read_real(OUT2 + 4), 4.5f);
    EXPECT_EQ(call("GdipGetPathPoints", {I(p), I(RPTS), I(3)}), Status::InsufficientBuffer);
    EXPECT_EQ(call("GdipGetPathPoints", {I(p), I(RPTS), I(5)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathTypes", {I(p), I(RTYPES), I(5)}), Status::Ok);
    EXPECT_EQ(call("GdipGetPathPoints", {I(p), I(0), I(4)}), Status::InvalidParameter);
    // "pointsI": GpRound is floor(v + 0.5).
    EXPECT_EQ(call("GdipGetPathPointsI", {I(p), I(RPTS), I(4)}), Status::Ok);
    const i32 want[8] = {2, 3, 0, -1, 0, 3, 4, 5};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(read_int(RPTS + 4 * static_cast<u64>(i)), want[i]) << i;

    // GdipGetPathData: PathData{INT Count; PointF* Points; BYTE* Types}.
    const u64 ps = ptr_size();
    auto set_data = [&](i32 c, u64 pts, u64 tys) {
        write_int(OUT2, c);
        if (x64()) {
            ASSERT_TRUE(memory.write_u64(OUT2 + 8, pts).is_ok());
            ASSERT_TRUE(memory.write_u64(OUT2 + 16, tys).is_ok());
        } else {
            write_int(OUT2 + 4, static_cast<i32>(pts));
            write_int(OUT2 + 8, static_cast<i32>(tys));
        }
    };
    (void)ps;
    set_data(4, RPTS, RTYPES);
    EXPECT_EQ(call("GdipGetPathData", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    EXPECT_FLOAT_EQ(read_real(RPTS + 8), -0.5f);
    EXPECT_EQ(memory.read_u8(RTYPES + 1).value(), 1);
    set_data(3, RPTS, RTYPES);
    EXPECT_EQ(call("GdipGetPathData", {I(p), I(OUT2)}), Status::OutOfMemory);  // "data_short"
    EXPECT_EQ(read_int(OUT2), 3);
    set_data(7, RPTS, RTYPES);
    EXPECT_EQ(call("GdipGetPathData", {I(p), I(OUT2)}), Status::Ok);  // "data_long"
    EXPECT_EQ(read_int(OUT2), 4);
    set_data(-1, RPTS, RTYPES);
    EXPECT_EQ(call("GdipGetPathData", {I(p), I(OUT2)}), Status::InvalidParameter);
    set_data(4, 0, RTYPES);
    EXPECT_EQ(call("GdipGetPathData", {I(p), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetPathData", {I(p), I(0)}), Status::InvalidParameter);

    EXPECT_EQ(call("GdipClonePath", {I(p), I(OUT)}), Status::Ok);
    const u64 q = read_handle(OUT);
    EXPECT_NE(q, p);
    EXPECT_EQ(count(q), 4);
    call("GdipGetPathFillMode", {I(q), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 5);
    EXPECT_EQ(call("GdipResetPath", {I(q)}), Status::Ok);
    expect_empty(q);
    call("GdipGetPathFillMode", {I(q), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 0);  // "reset_fill"
    EXPECT_EQ(count(p), 4);
    EXPECT_EQ(call("GdipDeletePath", {I(q)}), Status::Ok);
    EXPECT_EQ(call("GdipDeletePath", {I(q)}), Status::InvalidParameter);
    // A matrix handle is not a path.
    const u64 mx = new_matrix(1, 0, 0, 1, 0, 0);
    EXPECT_EQ(call("GdipGetPointCount", {I(mx), I(OUT2)}), Status::InvalidParameter);
}

TEST_P(Gdip_geometry_Test, CreatePath2ValidatesTypesAsGdiplus) {
    put_points({{0, 0}, {1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 5}, {6, 6}, {7, 7}});
    auto create = [&](std::vector<u8> t, i32 fill = 0, const char* fn = "GdipCreatePath2") {
        put_types(t);
        poison(OUT, 8, 0);
        const Status s = call(fn, {I(PTS), I(TYPES), I(static_cast<u32>(t.size())),
                                   I(static_cast<u32>(fill)), I(OUT)});
        return std::make_pair(s, read_handle(OUT));
    };
    struct Case {
        std::vector<u8> in;
        const char* out;  ///< expected types, "" = rejected (empty path)
    };
    const Case cases[] = {
        {{0, 1, 0, 1}, "00010001"},          {{0, 1, 0}, ""},
        {{0}, "00"},                         {{0, 0x81}, "0081"},
        {{0, 3, 3, 3}, "00030303"},          {{0, 3, 3, 3, 3, 3, 3}, "00030303030303"},
        {{0, 1, 3, 3, 3}, "0001030303"},     {{0, 3, 3, 3, 1}, "0003030301"},
        {{0, 3, 3, 0x83}, "00030383"},       {{0, 3, 0x83, 3}, "00038303"},
        {{0, 2, 1}, ""},                     {{0, 4, 1}, ""},
        {{0, 0x11, 0x31}, "001131"},         {{0x20, 1, 1}, "000101"},
        {{0x80, 1, 1}, "000101"},            {{0, 0x81, 1, 1}, "00810101"},
        {{1, 1}, "0001"},                    {{3, 3, 3, 3}, "00030303"},
        {{0, 1, 1, 0x40}, ""},               {{0, 1, 1, 0x08}, ""},
        {{0, 1, 0xff}, ""},                  {{0, 1, 0, 3, 3, 3, 0, 1}, "0001000303030001"},
        {{0, 0, 0}, ""},                     {{0, 3, 3, 1}, ""},
        {{0, 7, 0x41}, ""},                  {{0, 1, 0x21, 0, 3, 0xA3}, ""},
    };
    for (const Case& c : cases) {
        auto [s, p] = create(c.in);
        EXPECT_EQ(s, Status::Ok);
        EXPECT_EQ(types(p), parse_types(c.out)) << "case " << c.out;
        call("GdipDeletePath", {I(p)});
    }
    EXPECT_EQ(create({0, 1}, 2).first, Status::OutOfMemory);   // "create2_fill2"
    EXPECT_EQ(create({0, 1}, -1).first, Status::OutOfMemory);  // "c2_fillneg"
    EXPECT_EQ(call("GdipCreatePath2", {I(PTS), I(TYPES), I(0), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePath2", {I(PTS), I(TYPES), I(0xFFFFFFFF), I(0), I(OUT)}), Status::OutOfMemory);
    EXPECT_EQ(call("GdipCreatePath2", {I(0), I(TYPES), I(2), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreatePath2", {I(PTS), I(0), I(2), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreatePath2", {I(PTS), I(TYPES), I(2), I(0), I(0)}), Status::InvalidParameter);
    put_ints({0, 0, 1, 1, 2, 2});
    put_types({0, 1, 1});
    EXPECT_EQ(call("GdipCreatePath2I", {I(PTS), I(TYPES), I(3), I(1), I(OUT)}), Status::Ok);
    const u64 p = read_handle(OUT);
    expect_path(p, "0,0;1,1;2,2", "000101");
    call("GdipGetPathFillMode", {I(p), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 1);
    // A path built from arrays has no open figure: the next line starts one.
    line(p, 2, 2, 3, 3);
    EXPECT_EQ(types(p), parse_types("0001010001"));
}

TEST_P(Gdip_geometry_Test, MarkersCloseAndReverse) {
    u64 p = new_path();
    EXPECT_EQ(call("GdipSetPathMarker", {I(p)}), Status::Ok);  // "marker_empty"
    EXPECT_EQ(call("GdipClosePathFigure", {I(p)}), Status::Ok);
    EXPECT_EQ(call("GdipReversePath", {I(p)}), Status::Ok);
    expect_empty(p);
    line(p, 0, 0, 10, 0);
    call("GdipSetPathMarker", {I(p)});
    call("GdipSetPathMarker", {I(p)});
    line(p, 10, 10, 0, 10);
    call("GdipClosePathFigure", {I(p)});
    call("GdipClosePathFigure", {I(p)});
    bezier(p, {20, 20, 30, 20, 30, 30, 20, 30});
    call("GdipSetPathMarker", {I(p)});
    call("GdipStartPathFigure", {I(p)});
    call("GdipStartPathFigure", {I(p)});
    rect(p, 40, 40, 5, 5);
    line(p, 50, 50, 60, 60);
    const char* pts = "0,0;10,0;10,10;0,10;20,20;30,20;30,30;20,30;40,40;45,40;45,45;40,45;50,50;60,60";
    expect_path(p, pts, "0021018100030323000101810001");  // "markers"

    call("GdipClonePath", {I(p), I(OUT)});
    u64 q = read_handle(OUT);
    EXPECT_EQ(call("GdipReversePath", {I(q)}), Status::Ok);
    expect_path(q, "60,60;50,50;40,45;45,45;45,40;40,40;20,30;30,30;30,20;20,20;0,10;10,10;10,0;0,0", "0001000101a10003030300210181");
    call("GdipClonePath", {I(p), I(OUT)});
    q = read_handle(OUT);
    EXPECT_EQ(call("GdipClosePathFigures", {I(q)}), Status::Ok);
    expect_path(q, pts, "00210181000303a3000101810081");
    call("GdipClonePath", {I(p), I(OUT)});
    q = read_handle(OUT);
    EXPECT_EQ(call("GdipClearPathMarkers", {I(q)}), Status::Ok);
    expect_path(q, pts, "0001018100030303000101810001");

    // "reverse_mixed": the segment type moves with its segment.
    p = new_path();
    arc(p, 0, 0, 10, 10, 0, 90);
    line(p, 0, 5, 0, 0);
    call("GdipSetPathMarker", {I(p)});
    call("GdipClosePathFigure", {I(p)});
    line(p, 50, 50, 60, 60);
    call("GdipReversePath", {I(p)});
    expect_path(p, "60,60;50,50;0,0;0,5;5.00000048,9.99999905;7.76142359,9.99999809;9.99999905,7.76142263;9.99999905,4.99999952", "0021000101030383");

    // "rev_dash": the dash bit stays with its point.
    put_points({{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}});
    put_types({0, 0x11, 0x31, 0, 0x01, 0x81});
    ASSERT_EQ(call("GdipCreatePath2", {I(PTS), I(TYPES), I(6), I(0), I(OUT)}), Status::Ok);
    q = read_handle(OUT);
    call("GdipReversePath", {I(q)});
    expect_path(q, "5,0;4,0;3,0;2,0;1,0;0,0", "0001a1101101");

    // "close_after_start": closing with no open figure is a no-op.
    p = new_path();
    line(p, 0, 0, 1, 1);
    call("GdipStartPathFigure", {I(p)});
    call("GdipClosePathFigure", {I(p)});
    EXPECT_EQ(types(p), parse_types("0001"));
}

TEST_P(Gdip_geometry_Test, AddPathPathConnectRules) {
    u64 p = new_path(), q = new_path();
    line(p, 0, 0, 1, 1);
    line(q, 5, 5, 6, 6);
    line(q, 7, 7, 8, 8);
    call("GdipStartPathFigure", {I(q)});
    line(q, 9, 9, 10, 10);
    EXPECT_EQ(call("GdipAddPathPath", {I(p), I(q), I(1)}), Status::Ok);
    EXPECT_EQ(types(p), parse_types("0001010101010001"));
    EXPECT_EQ(call("GdipAddPathPath", {I(p), I(q), I(0)}), Status::Ok);
    call("GdipClosePathFigure", {I(p)});
    EXPECT_EQ(call("GdipAddPathPath", {I(p), I(q), I(1)}), Status::Ok);
    EXPECT_EQ(types(p), parse_types("0001010101010001000101010081000101010001"));
    EXPECT_EQ(call("GdipAddPathPath", {I(p), I(p), I(1)}), Status::Ok);  // "addpath_self"
    EXPECT_EQ(types(p), parse_types("00010101010100010001010100810001010100010101010101010001000101010081000101010001"));

    p = new_path();
    line(p, 0, 0, 1, 1);
    call("GdipResetPath", {I(q)});
    bezier(q, {1, 1, 2, 2, 3, 3, 4, 4});
    call("GdipAddPathPath", {I(p), I(q), I(1)});
    expect_path(p, "0,0;1,1;2,2;3,3;4,4", "0001030303");  // "addpath_connect_bez"
    call("GdipResetPath", {I(q)});
    EXPECT_EQ(call("GdipAddPathPath", {I(p), I(q), I(1)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathPath", {I(p), I(0), I(1)}), Status::InvalidParameter);

    p = new_path();
    line(p, 0, 0, 1, 1);
    rect(q, 1, 1, 5, 5);
    call("GdipAddPathPath", {I(p), I(q), I(1)});
    expect_path(p, "0,0;1,1;1,1;6,1;6,6;1,6", "000100010181");  // "addpath_connect_closed_rect"
    p = new_path();
    line(p, 0, 0, 1, 1);
    call("GdipResetPath", {I(q)});
    line(q, 1, 1, 2, 2);
    call("GdipSetPathMarker", {I(q)});
    call("GdipAddPathPath", {I(p), I(q), I(1)});
    expect_path(p, "0,0;1,1;2,2", "000121");
}

TEST_P(Gdip_geometry_Test, TransformPath) {
    const u64 p = new_path();
    ellipse(p, 0, 0, 100, 50);
    const u64 mx = new_matrix(2, 0.5f, -0.25f, 1.5f, 10, -20);
    EXPECT_EQ(call("GdipTransformPath", {I(p), I(mx)}), Status::Ok);
    expect_path(p, "203.75,67.5;200.298218,88.2106705;152.72847,93.8071136;97.5,80;42.2715263,66.1928787;0.298220634,38.2106743;3.75,17.5;7.20177937,-3.2106781;54.7715263,-8.80711842;110,5;165.22847,18.8071175;207.201767,46.7893219;203.75,67.5", "00030303030303030303030383", 2e-4f);
    EXPECT_EQ(call("GdipTransformPath", {I(p), I(0)}), Status::Ok);  // "transform_null"
    EXPECT_EQ(count(p), 13);
}

// ============================================================ Flatten

TEST_P(Gdip_geometry_Test, FlattenMatchesGdiplusSubdivision) {
    // Point counts are exact; points agree within one flatness/4 grid step
    // (GDI+'s fixed-point accumulation differs by at most that).
    struct Case {
        float flatness;
        i32 n;
    };
    for (const Case c : {Case{0.25f, 45}, {1.0f, 25}, {0.1f, 65}, {5.0f, 13}, {100.0f, 5}}) {
        const u64 p = new_path();
        ellipse(p, 0, 0, 100, 50);
        EXPECT_EQ(call("GdipFlattenPath", {I(p), I(0), R(c.flatness)}), Status::Ok);
        EXPECT_EQ(count(p), c.n) << c.flatness;
        const auto t = types(p);
        ASSERT_FALSE(t.empty());
        EXPECT_EQ(t.front(), 0);
        EXPECT_EQ(t.back(), 0x81);
        // Every point sits on the flatness/4 grid and near the ellipse.
        for (PointF q : points(p)) {
            const float g = c.flatness / 4;
            EXPECT_NEAR(std::remainder(q.X, g), 0.0f, 1e-3f);
            const float ex = (q.X - 50) / 50, ey = (q.Y - 25) / 25;
            EXPECT_NEAR(std::sqrt(ex * ex + ey * ey), 1.0f, c.flatness / 10 + 0.03f);
        }
    }
    u64 p = new_path();
    ellipse(p, 0, 0, 100, 50);
    call("GdipFlattenPath", {I(p), I(0), R(0.25f)});
    expect_path(p, "100,25;99.75,27.5625;99,30.0625;97.75,32.4375;96.0625,34.75;93.9375,36.9375;91.4375,39;85.375,42.6875;77.9375,45.75;69.4375,48.0625;60.0625,49.5;50,50;39.9375,49.5;30.5625,48.0625;22.0625,45.75;14.6875,42.6875;8.5625,39;6.0625,36.9375;3.9375,34.75;2.25,32.4375;1,30.0625;0.25,27.5625;0,25;0.25,22.4375;1,20;2.25,17.5625;3.9375,15.3125;6.0625,13.125;8.5625,11.0625;14.6875,7.375;22.0625,4.3125;30.5625,2;39.9375,0.5;50,0;60.0625,0.5;69.4375,2;77.9375,4.3125;85.375,7.375;91.4375,11.0625;93.9375,13.125;96.0625,15.3125;97.75,17.5625;99,20;99.75,22.4375;100,25",
                "000101010101010101010101010101010101010101010101010101010101010101010101010101010101010181", 0.0626f);
    // With a matrix: flattened after the transform ("flatten_ellipse_m": 65).
    p = new_path();
    ellipse(p, 0, 0, 100, 50);
    const u64 mx = new_matrix(2, 0.5f, -0.25f, 1.5f, 10, -20);
    EXPECT_EQ(call("GdipFlattenPath", {I(p), I(mx), R(0.25f)}), Status::Ok);
    EXPECT_EQ(count(p), 65);
    struct Bez {
        std::array<float, 8> c;
        i32 n;
        const char* name;
    };
    for (const Bez& b : {Bez{{0, 0, 100, 0, 100, 100, 0, 100}, 33, "flatten_bez"},
                         Bez{{0, 0, 1000, 0, 1000, 1000, 0, 1000}, 65, "flatten_big_bez"},
                         Bez{{-10.3f, -5.1f, 20.7f, -30.2f, 40.1f, 33.3f, 60.9f, -2.6f}, 24, "flatten_neg_bez"},
                         Bez{{0, 0, 10, 0, 20, 0, 30, 0}, 2, "flatten_bez_straight"},
                         Bez{{0, 0, 0.1f, 0, 0.1f, 0.1f, 0, 0.1f}, 2, "flatten_tiny_bez"}}) {
        p = new_path();
        bezier(p, {b.c[0], b.c[1], b.c[2], b.c[3], b.c[4], b.c[5], b.c[6], b.c[7]});
        call("GdipFlattenPath", {I(p), I(0), R(0.25f)});
        EXPECT_EQ(count(p), b.n) << b.name;
    }
    p = new_path();
    bezier(p, {0, 0, 0.1f, 0, 0.1f, 0.1f, 0, 0.1f});
    call("GdipFlattenPath", {I(p), I(0), R(0.25f)});
    expect_path(p, "0,0;0,0.125", "0001");
    // Mixed figure, markers and close kept on the right points ("flatten_mixed").
    p = new_path();
    arc(p, 0, 0, 40, 40, 0, 90);
    line(p, 0, 20, 0, 0);
    call("GdipSetPathMarker", {I(p)});
    call("GdipClosePathFigure", {I(p)});
    bezier(p, {50, 50, 60, 40, 70, 60, 80, 50});
    call("GdipFlattenPath", {I(p), I(0), R(0.25f)});
    expect_path(p, "40,20;39.625,24.0625;38.4375,27.8125;36.5625,31.1875;34.125,34.125;31.1875,36.5625;27.8125,38.4375;24.0625,39.625;20,40;0,20;0,0;50,50;51.875,48.4375;53.75,47.5625;55.625,47.125;57.5,47.1875;61.25,48.25;65,50;68.75,51.75;72.5,52.8125;74.375,52.875;76.25,52.4375;78.125,51.5625;80,50",
                "000101010101010101018100010101010101010101010101", 0.0626f);  // markers dropped
    // Lines are not snapped ("flatten_frac_lines").
    p = new_path();
    line(p, 0.3f, 0.7f, 10.13f, 5.55f);
    line(p, -3.33f, -7.71f, -0.07f, 2.2f);
    call("GdipFlattenPath", {I(p), I(0), R(0.25f)});
    expect_path(p, "0.300000012,0.699999988;10.1300001,5.55000019;-3.32999992,-7.71000004;-0.0700000003,2.20000005", "00010101", 1e-6f);
    // Flatness 0: every point collapses to the origin, a curve to one point.
    p = new_path();
    line(p, 3, 4, 5, 6);
    bezier(p, {5, 6, 10, 10, 20, 0, 30, 5});
    call("GdipFlattenPath", {I(p), I(0), R(0)});
    expect_path(p, "0,0;0,0;0,0", "000101");
    p = new_path();
    EXPECT_EQ(call("GdipFlattenPath", {I(p), I(0), R(0.25f)}), Status::Ok);
    expect_empty(p);
    EXPECT_EQ(call("GdipFlattenPath", {I(0), I(0), R(0.25f)}), Status::InvalidParameter);
}

// ===================================================== Outline / warp

TEST_P(Gdip_geometry_Test, WindingModeOutline) {
    u64 p = new_path();
    rect(p, 0, 0, 50, 30);
    rect(p, 10, 10, 50, 30);
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::Ok);
    expect_path(p, "0,0;50,0;50,9.99999905;60,10;60,40;10,40;10,30;0,30", "0001010101010181");
    p = new_path();
    rect(p, 0, 0, 10, 10);
    rect(p, 20, 0, 10, 10);
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::Ok);
    expect_path(p, "0,0;10,0;10,10;0,10;20,0;30,0;30,10;20,10", "0001018100010181");
    p = new_path();
    rect(p, 0, 0, 30, 30);
    rect(p, 10, 10, 10, 10);
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::Ok);
    expect_path(p, "0,0;30,0;30,30;0,30", "00010181");
    p = new_path();
    rect(p, 0, 0, 30, 30);
    put_points({{10, 10}, {10, 20}, {20, 20}, {20, 10}});
    call("GdipAddPathPolygon", {I(p), I(PTS), I(4)});
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::Ok);
    expect_path(p, "0,0;30,0;30,30;0,30;10,10;10,20;20,20;20,10", "0001018100010181");
    p = new_path();
    put_points({{50, 0}, {79, 90}, {2, 35}, {98, 35}, {21, 90}});
    call("GdipAddPathPolygon", {I(p), I(PTS), I(5)});
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::Ok);
    expect_path(p, "50,0;61.2777748,34.9999962;98,35;68.1483841,56.3225784;79,90;50,69.2857132;21,90;31.8516159,56.3225822;2,35;38.7222176,35", "00010101010101010181", 2e-4f);
    p = new_path();
    line(p, 0, 0, 10, 10);
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::InvalidParameter);
    expect_path(p, "0,0;10,10", "0001");
    p = new_path();
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(0.25f)}), Status::Ok);
    rect(p, 0, 0, 50, 30);
    const u64 mx = new_matrix(2, 0, 0, 2, 5, 5);
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(mx), R(0.25f)}), Status::Ok);
    expect_path(p, "5,5;105,5;105,65;5,65", "00010181");
    p = new_path();
    ellipse(p, 0, 0, 40, 40);
    EXPECT_EQ(call("GdipWindingModeOutline", {I(p), I(0), R(1.0f)}), Status::Ok);
    EXPECT_EQ(count(p), 16);  // "winding_ellipse"
}

TEST_P(Gdip_geometry_Test, WarpPath) {
    put_points({{0, 0}, {200, 20}, {10, 100}, {180, 150}, {5, 5}}, BUF + 0x3000);
    const GuestAddr dst = BUF + 0x3000;
    auto warp = [&](u64 p, u64 mx, u32 n, float sx, float sy, float sw, float sh, u32 mode) {
        return call("GdipWarpPath", {I(p), I(mx), I(dst), I(n), R(sx), R(sy), R(sw), R(sh),
                                     I(mode), R(0.25f)});
    };
    u64 p = new_path();
    rect(p, 0, 0, 100, 100);
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 0), Status::Ok);
    expect_path(p, "-0,-0;199.999985,19.9999981;179.999969,150;10,100", "00010181", 2e-4f);
    p = new_path();
    rect(p, 0, 0, 100, 100);
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 1), Status::Ok);
    expect_path(p, "-0,-0;200,20;180,150;10,100", "00010101");  // close dropped
    p = new_path();
    line(p, 0, 0, 100, 100);
    line(p, 100, 0, 50, 50);
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 0), Status::Ok);
    expect_path(p, "-0,-0;179.999969,150;199.999985,19.9999981;83.0769119,69.2307663", "00010101", 2e-4f);
    for (u32 n : {3u, 1u, 5u, 2u}) {  // a parallelogram on three points
        p = new_path();
        rect(p, 0, 0, 100, 100);
        EXPECT_EQ(warp(p, 0, n, 0, 0, 100, 100, 0), Status::Ok);
        expect_path(p, "-0,-0;200,20;210,120;10,100", "00010181", 2e-4f);
    }
    p = new_path();
    rect(p, 0, 0, 100, 100);
    EXPECT_EQ(warp(p, 0, 0, 0, 0, 100, 100, 0), Status::InvalidParameter);
    EXPECT_EQ(call("GdipWarpPath", {I(p), I(0), I(0), I(4), R(0), R(0), R(100), R(100), I(0), R(0.25f)}),
              Status::InvalidParameter);
    expect_path(p, "0,0;100,0;100,100;0,100", "00010181");
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 2), Status::Ok);  // "warp_mode2"
    expect_empty(p);
    p = new_path();
    rect(p, 0, 0, 100, 100);
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 0, 100, 0), Status::Ok);  // "warp_w0"
    for (PointF q : points(p)) EXPECT_TRUE(std::isnan(q.X));
    p = new_path();
    rect(p, 0, 0, 100, 100);
    const u64 mx = new_matrix(2, 0.5f, -0.25f, 1.5f, 10, -20);
    EXPECT_EQ(warp(p, mx, 4, 0, 0, 100, 100, 0), Status::Ok);
    expect_path(p, "10,-20;404.999969,109.999985;332.499939,294.999969;5,135", "00010181", 3e-4f);
    p = new_path();
    rect(p, 0, 0, 100, 100);
    EXPECT_EQ(warp(p, 0, 4, 10, 20, 50, 40, 0), Status::Ok);
    expect_path(p, "-34.7826157,-60.8695679;606.896484,-75.8620911;362.962891,329.629608;-4.25532007,168.085098", "00010181", 1e-3f);
    // Markers are dropped, close kept ("warp_persp_marker", "warp_persp_tri").
    p = new_path();
    line(p, 10, 30, 90, 60);
    line(p, 90, 60, 20, 80);
    call("GdipSetPathMarker", {I(p)});
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 0), Status::Ok);
    expect_path(p, "17.4647865,35.2112617;163.448242,98.2758484;35.6756706,89.1891785", "000101", 2e-4f);
    // Bilinear lines become flattened parabolas ("warp_bilinear_line_f1").
    p = new_path();
    line(p, 0, 0, 100, 100);
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 1), Status::Ok);
    expect_path(p, "-0,-0;25.78125,15.46875;50.625,31.875;74.53125,49.21875;97.5,67.5;119.53125,86.71875;140.625,106.875;160.78125,127.96875;180,150", "000101010101010101");
    p = new_path();
    line(p, 10, 30, 90, 60);
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 1), Status::Ok);
    expect_path(p, "22.0999985,32.8999977;60.3749924,46.8749924;97.7499847,61.7499924;134.224976,77.5249863;169.799988,94.1999893", "0001010101", 2e-4f);
    p = new_path();
    EXPECT_EQ(warp(p, 0, 4, 0, 0, 100, 100, 0), Status::Ok);
    expect_empty(p);
}

// ============================================================ Bounds

TEST_P(Gdip_geometry_Test, WorldBounds) {
    u64 p = new_path();
    expect_rect(bounds(p, 0, 0), 0, 0, 0, 0);
    ellipse(p, 10, 20, 100, 50);
    expect_rect(bounds(p, 0, 0), 10, 20, 100, 50);
    const u64 mx = new_matrix(2, 0.5f, -0.25f, 1.5f, 10, -20);
    expect_rect(bounds(p, mx, 0), 12.5f, 15, 212.5f, 125);
    const u64 pen4 = new_pen(4);
    expect_rect(bounds(p, 0, pen4), -10, 0, 140, 90);
    expect_rect(bounds(p, mx, pen4), -7.5f, -5, 252.5f, 165);
    EXPECT_EQ(call("GdipGetPathWorldBoundsI", {I(p), I(OUT2), I(mx), I(0)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 12);
    EXPECT_EQ(read_int(OUT2 + 4), 15);
    EXPECT_EQ(read_int(OUT2 + 8), 214);
    EXPECT_EQ(read_int(OUT2 + 12), 126);
    EXPECT_EQ(call("GdipGetPathWorldBounds", {I(p), I(0), I(0), I(0)}), Status::InvalidParameter);

    p = new_path();
    bezier(p, {0, 0, 50, -40, 100, 140, 150, 100});
    expect_rect(bounds(p, 0, 0), 0, -40, 150, 180);  // control points count
    p = new_path();
    line(p, 0.4f, 0.6f, 10.2f, 7.7f);
    call("GdipGetPathWorldBoundsI", {I(p), I(OUT2), I(0), I(0)});
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(read_int(OUT2 + 4), 0);
    EXPECT_EQ(read_int(OUT2 + 8), 12);
    EXPECT_EQ(read_int(OUT2 + 12), 9);
    p = new_path();
    line(p, -0.6f, -1.5f, 10.5f, 7.4f);
    call("GdipGetPathWorldBoundsI", {I(p), I(OUT2), I(0), I(0)});
    EXPECT_EQ(read_int(OUT2), -1);
    EXPECT_EQ(read_int(OUT2 + 4), -2);
    EXPECT_EQ(read_int(OUT2 + 8), 13);
    EXPECT_EQ(read_int(OUT2 + 12), 11);

    // Pen inflation ("bp_*").
    p = new_path();
    line(p, 0, 0, 100, 0);
    expect_rect(bounds(p, 0, pen4), -4, -4, 108, 8);
    expect_rect(bounds(p, 0, new_pen(1)), -1, -1, 102, 2);
    expect_rect(bounds(p, 0, new_pen(4, 4)), -384, -384, 868, 768);
    u64 anchor = new_pen(4);
    pen_obj(anchor).end_cap = gdip::LineCapArrowAnchor;
    expect_rect(bounds(p, 0, anchor), -12, -12, 124, 24);
    const u64 m3 = new_matrix(3, 0, 0, 3, 0, 0);
    expect_rect(bounds(p, m3, pen4), -4, -4, 308, 8);
    line(p, 100, 0, 100, 100);
    expect_rect(bounds(p, 0, pen4), -20, -20, 140, 140);
    u64 m2 = new_pen(4);
    pen_obj(m2).miter_limit = 2;
    expect_rect(bounds(p, 0, m2), -4, -4, 108, 108);
    u64 bevel = new_pen(4);
    pen_obj(bevel).line_join = gdip::LineJoinBevel;
    expect_rect(bounds(p, 0, bevel), -4, -4, 108, 108);
    u64 clipped = new_pen(4);
    pen_obj(clipped).line_join = gdip::LineJoinMiterClipped;
    expect_rect(bounds(p, 0, clipped), -20, -20, 140, 140);
    expect_rect(bounds(p, 0, new_pen(0)), 0, 0, 100, 100);
    expect_rect(bounds(p, 0, new_pen(20)), -100, -100, 300, 300);
    EXPECT_EQ(call("GdipGetPathWorldBoundsI", {I(p), I(OUT2), I(0), I(pen4)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2 + 8), 141);
    p = new_path();
    bezier(p, {0, 0, 10, 0, 20, 0, 30, 0});
    expect_rect(bounds(p, 0, pen4), -20, -20, 70, 40);  // "bp_bez_w4"
}

TEST_P(Gdip_geometry_Test, OutlineVisibility) {
    const u64 p = new_path();
    line(p, 0, 0, 100, 0);
    const u64 pen4 = new_pen(4);
    EXPECT_EQ(call("GdipIsOutlineVisiblePathPoint", {I(p), R(50), R(1), I(0), I(0), I(OUT2)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsOutlineVisiblePathPoint", {I(p), R(50), R(1), I(pen4), I(0), I(0)}),
              Status::InvalidParameter);
    write_int(OUT2, 7);
    EXPECT_EQ(call("GdipIsOutlineVisiblePathPoint", {I(p), R(50), R(0), I(new_pen(0)), I(0), I(OUT2)}),
              Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);  // "ov_w0"
    const u64 empty = new_path();
    write_int(OUT2, 7);
    EXPECT_EQ(call("GdipIsOutlineVisiblePathPoint", {I(empty), R(0), R(0), I(pen4), I(0), I(OUT2)}),
              Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);

    // The widened outline comes from the draw area (widen_path).
    gdip::GpPath probe_outline;
    gdip::GpPath src;
    gdip::path_add_lines(src, {{0, 0}, {100, 0}});
    gdip::widen_path(src, pen_obj(pen4), nullptr, gdip::kFlatnessDefault, probe_outline);
    if (probe_outline.points.empty()) {
        GTEST_SKIP() << "widen_path (gdiplus_pen.cpp) does not produce outlines yet";
    }
    auto visible = [&](u64 pen, float x, float y) {
        write_int(OUT2, 7);
        EXPECT_EQ(call("GdipIsOutlineVisiblePathPoint", {I(p), R(x), R(y), I(pen), I(0), I(OUT2)}),
                  Status::Ok);
        return read_int(OUT2);
    };
    const u64 pen3 = new_pen(3);
    const float ys[] = {-2.6f, -2.5f, -2.4f, -2, -1.6f, -1.5f, -1.4f, -1, 0, 1, 1.4f, 1.5f, 1.6f, 2, 2.4f};
    const int w3[] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    const int w4[] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    for (int i = 0; i < 15; ++i) {
        EXPECT_EQ(visible(pen3, 50, ys[i]), w3[i]) << "ov_w3 y=" << ys[i];
        EXPECT_EQ(visible(pen4, 50, ys[i]), w4[i]) << "ov_w4 y=" << ys[i];
    }
    const float xs[] = {-0.6f, -0.5f, -0.4f, 0, 0.4f, 99.4f, 99.5f, 99.6f, 100, 100.4f};
    const int wx[] = {0, 1, 1, 1, 1, 1, 0, 0, 0, 0};
    for (int i = 0; i < 10; ++i) EXPECT_EQ(visible(pen4, xs[i], 0), wx[i]) << "ov_w4_x x=" << xs[i];
    write_int(OUT2, 7);
    EXPECT_EQ(call("GdipIsOutlineVisiblePathPointI", {I(p), I(50), I(1), I(pen4), I(0), I(OUT2)}),
              Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
}

// ============================================================ Matrix

TEST_P(Gdip_geometry_Test, MatrixArithmetic) {
    EXPECT_EQ(call("GdipCreateMatrix", {I(0)}), Status::InvalidParameter);
    ASSERT_EQ(call("GdipCreateMatrix", {I(OUT)}), Status::Ok);
    const u64 m = read_handle(OUT);
    expect_elements(m, {1, 0, 0, 1, 0, 0});
    EXPECT_EQ(call("GdipTranslateMatrix", {I(m), R(10), R(20), I(0)}), Status::Ok);
    expect_elements(m, {1, 0, 0, 1, 10, 20});
    EXPECT_EQ(call("GdipScaleMatrix", {I(m), R(2), R(3), I(1)}), Status::Ok);
    expect_elements(m, {2, 0, 0, 3, 20, 60});
    EXPECT_EQ(call("GdipRotateMatrix", {I(m), R(30), I(0)}), Status::Ok);
    expect_elements(m, {1.73205078f, 1.49999976f, -0.999999881f, 2.59807611f, 20, 60});
    EXPECT_EQ(call("GdipRotateMatrix", {I(m), R(90), I(1)}), Status::Ok);
    expect_elements(m, {-1.49999952f, 1.73205066f, -2.59807611f, -0.999999702f, -59.9999962f, 20.0000019f});
    EXPECT_EQ(call("GdipShearMatrix", {I(m), R(0.5f), R(0.25f), I(0)}), Status::Ok);
    expect_elements(m, {-2.14951873f, 1.48205066f, -3.34807587f, -0.133974373f, -59.9999962f, 20.0000019f});
    EXPECT_EQ(call("GdipShearMatrix", {I(m), R(0.5f), R(0.25f), I(1)}), Status::Ok);
    expect_elements(m, {-1.4084934f, 0.944670975f, -3.41506314f, -0.97099334f, -49.9999962f, 5.00000286f});
    EXPECT_EQ(call("GdipTranslateMatrix", {I(m), R(1), R(1), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTranslateMatrix", {I(m), R(1), R(1), I(0xFFFFFFFF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipInvertMatrix", {I(m)}), Status::Ok);
    expect_elements(m, {-0.211372763f, -0.205642715f, 0.743415236f, -0.306610912f, -14.285717f, -8.74908066f});
    const u64 m2 = new_matrix(1, 2, 3, 4, 5, 6);
    EXPECT_EQ(call("GdipMultiplyMatrix", {I(m), I(m2), I(0)}), Status::Ok);
    expect_elements(m, {1.27545762f, -0.818864584f, 2.33954263f, -1.84337187f, -10.8820906f, -11.6169605f}, 1e-4f);
    EXPECT_EQ(call("GdipMultiplyMatrix", {I(m), I(m2), I(1)}), Status::Ok);
    expect_elements(m, {-1.18113613f, -0.724543095f, -3.19057298f, -2.69440222f, -40.732975f, -62.2320251f}, 1e-4f);
    EXPECT_EQ(call("GdipMultiplyMatrix", {I(m), I(m), I(0)}), Status::ObjectBusy);
    EXPECT_EQ(call("GdipMultiplyMatrix", {I(m), I(m2), I(3)}), Status::InvalidParameter);

    // Singular: not invertible, Invert refuses and leaves it.
    EXPECT_EQ(call("GdipSetMatrixElements", {I(m2), R(1), R(2), R(2), R(4), R(5), R(6)}), Status::Ok);
    EXPECT_EQ(call("GdipIsMatrixInvertible", {I(m2), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipInvertMatrix", {I(m2)}), Status::InvalidParameter);
    expect_elements(m2, {1, 2, 2, 4, 5, 6});
    call("GdipSetMatrixElements", {I(m2), R(1e-7f), R(0), R(0), R(1e-7f), R(0), R(0)});
    call("GdipIsMatrixInvertible", {I(m2), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipInvertMatrix", {I(m2)}), Status::Ok);
    expect_elements(m2, {10000000, 0, 0, 10000000, 0, 0}, 2);
    call("GdipSetMatrixElements", {I(m2), R(1e-20f), R(0), R(0), R(1), R(0), R(0)});
    call("GdipIsMatrixInvertible", {I(m2), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 1);

    auto identity = [&](float a, float b, float c, float d, float e, float f) {
        call("GdipSetMatrixElements", {I(m2), R(a), R(b), R(c), R(d), R(e), R(f)});
        EXPECT_EQ(call("GdipIsMatrixIdentity", {I(m2), I(OUT2)}), Status::Ok);
        return read_int(OUT2);
    };
    EXPECT_EQ(identity(1.0005f, 0, 0, 1, 0, 0), 1);
    EXPECT_EQ(identity(1.001f, 0, 0, 1, 0, 0), 0);
    EXPECT_EQ(identity(1, 0.0005f, 0, 1, 0, 0), 1);
    EXPECT_EQ(identity(1, 0.001f, 0, 1, 0, 0), 0);
    EXPECT_EQ(identity(1, 0, 0, 1, 1e-6f, 0), 0);
    EXPECT_EQ(identity(1, 0, 0, 1, 0, 1e-6f), 0);
    EXPECT_EQ(identity(1, 0, 0, 1, 0, 0), 1);

    call("GdipSetMatrixElements", {I(m), R(1), R(2), R(3), R(4), R(5), R(6)});
    call("GdipSetMatrixElements", {I(m2), R(1), R(2), R(3), R(4), R(5), R(6.000001f)});
    EXPECT_EQ(call("GdipIsMatrixEqual", {I(m), I(m2), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    call("GdipSetMatrixElements", {I(m2), R(1), R(2), R(3), R(4), R(5), R(6)});
    call("GdipIsMatrixEqual", {I(m), I(m2), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipIsMatrixEqual", {I(m), I(0), I(OUT2)}), Status::InvalidParameter);

    // Composition order checks ("shear_check", "rot_check", ...).
    auto reset = [&] { call("GdipSetMatrixElements", {I(m), R(1), R(2), R(3), R(4), R(5), R(6)}); };
    reset();
    call("GdipShearMatrix", {I(m), R(0.5f), R(0.25f), I(0)});
    expect_elements(m, {1.75f, 3, 3.5f, 5, 5, 6});
    reset();
    call("GdipRotateMatrix", {I(m), R(30), I(0)});
    expect_elements(m, {2.36602497f, 3.73205042f, 2.09807611f, 2.46410155f, 5, 6});
    reset();
    call("GdipScaleMatrix", {I(m), R(2), R(3), I(0)});
    expect_elements(m, {2, 4, 9, 12, 5, 6});
    reset();
    call("GdipTranslateMatrix", {I(m), R(7), R(8), I(0)});
    expect_elements(m, {1, 2, 3, 4, 36, 52});
    call("GdipSetMatrixElements", {I(m), R(1), R(0), R(0), R(1), R(0), R(0)});
    call("GdipRotateMatrix", {I(m), R(90), I(0)});
    expect_elements(m, {7.54978942e-08f, 0.99999994f, -0.99999994f, 7.54978942e-08f, 0, 0}, 1e-6f);

    EXPECT_EQ(call("GdipCloneMatrix", {I(m2), I(OUT)}), Status::Ok);
    const u64 c = read_handle(OUT);
    expect_elements(c, {1, 2, 3, 4, 5, 6});
    EXPECT_EQ(call("GdipGetMatrixElements", {I(m), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteMatrix", {I(c)}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteMatrix", {I(c)}), Status::InvalidParameter);
}

TEST_P(Gdip_geometry_Test, MatrixTransformsPointsAndBuildsFromParallelogram) {
    const u64 m = new_matrix(0.5f, 0.25f, -0.75f, 1.5f, 10.3f, -0.5f);
    put_points({{1, 2}, {-3, 0.5f}, {0, 0}});
    EXPECT_EQ(call("GdipTransformMatrixPoints", {I(m), I(PTS), I(3)}), Status::Ok);
    const float want[6] = {9.30000019f, 2.75f, 8.42500019f, -0.5f, 10.3000002f, -0.5f};
    for (int i = 0; i < 6; ++i) EXPECT_NEAR(read_real(PTS + 4 * static_cast<u64>(i)), want[i], 1e-5f);
    put_ints({1, 2, -3, 5, 0, 0});
    EXPECT_EQ(call("GdipTransformMatrixPointsI", {I(m), I(PTS), I(3)}), Status::Ok);
    const i32 wi[6] = {9, 3, 5, 6, 10, 0};
    for (int i = 0; i < 6; ++i) EXPECT_EQ(read_int(PTS + 4 * static_cast<u64>(i)), wi[i]);
    put_points({{1, 2}});
    EXPECT_EQ(call("GdipVectorTransformMatrixPoints", {I(m), I(PTS), I(1)}), Status::Ok);
    EXPECT_NEAR(read_real(PTS), -1, 1e-6f);
    EXPECT_NEAR(read_real(PTS + 4), 3.25f, 1e-6f);
    put_ints({1, 3});
    EXPECT_EQ(call("GdipVectorTransformMatrixPointsI", {I(m), I(PTS), I(1)}), Status::Ok);
    EXPECT_EQ(read_int(PTS), -2);
    EXPECT_EQ(read_int(PTS + 4), 5);
    EXPECT_EQ(call("GdipTransformMatrixPoints", {I(m), I(PTS), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformMatrixPoints", {I(m), I(PTS), I(0xFFFFFFFF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipTransformMatrixPoints", {I(m), I(0), I(1)}), Status::InvalidParameter);
    // GpRound on the halves ("xformI_half").
    const u64 h = new_matrix(0.5f, 0, 0, 0.5f, 0, 0);
    put_ints({1, 3, -1, -3, 5, 7});
    call("GdipTransformMatrixPointsI", {I(h), I(PTS), I(3)});
    const i32 wh[6] = {1, 2, 0, -1, 3, 4};
    for (int i = 0; i < 6; ++i) EXPECT_EQ(read_int(PTS + 4 * static_cast<u64>(i)), wh[i]);

    put_reals_rect(10, 20, 100, 50);
    put_points({{0, 0}, {200, 10}, {-20, 100}}, PTS + 0x100);
    EXPECT_EQ(call("GdipCreateMatrix3", {I(PTS), I(PTS + 0x100), I(OUT)}), Status::Ok);
    expect_elements(read_handle(OUT), {2, 0.1f, -0.4f, 2, -12, -41}, 1e-4f);
    put_ints({10, 20, 100, 50});
    put_ints({0, 0, 200, 10, -20, 100}, PTS + 0x100);
    EXPECT_EQ(call("GdipCreateMatrix3I", {I(PTS), I(PTS + 0x100), I(OUT)}), Status::Ok);
    expect_elements(read_handle(OUT), {2, 0.1f, -0.4f, 2, -12, -41}, 1e-4f);
    put_reals_rect(10, 20, 0, 50);
    put_points({{0, 0}, {200, 10}, {-20, 100}}, PTS + 0x100);
    EXPECT_EQ(call("GdipCreateMatrix3", {I(PTS), I(PTS + 0x100), I(OUT)}), Status::OutOfMemory);
    put_reals_rect(10, 20, -100, 50);
    EXPECT_EQ(call("GdipCreateMatrix3", {I(PTS), I(PTS + 0x100), I(OUT)}), Status::Ok);
    expect_elements(read_handle(OUT), {-2.00000024f, -0.100000009f, -0.400000036f, 2, 28, -39.0000038f}, 1e-4f);
    EXPECT_EQ(call("GdipCreateMatrix3", {I(0), I(PTS + 0x100), I(OUT)}), Status::InvalidParameter);
}

// ========================================================= Iterator

TEST_P(Gdip_geometry_Test, PathIteratorCursors) {
    // The "markers" path: [0..3] closed, [4..7] Bézier, [8..11] closed, [12..13].
    const u64 p = new_path();
    line(p, 0, 0, 10, 0);
    call("GdipSetPathMarker", {I(p)});
    line(p, 10, 10, 0, 10);
    call("GdipClosePathFigure", {I(p)});
    bezier(p, {20, 20, 30, 20, 30, 30, 20, 30});
    call("GdipSetPathMarker", {I(p)});
    call("GdipStartPathFigure", {I(p)});
    rect(p, 40, 40, 5, 5);
    line(p, 50, 50, 60, 60);
    ASSERT_EQ(call("GdipCreatePathIter", {I(OUT), I(p)}), Status::Ok);
    const u64 it = read_handle(OUT);
    auto get = [&](const char* fn) {
        EXPECT_EQ(call(fn, {I(it), I(OUT2)}), Status::Ok);
        return read_int(OUT2);
    };
    EXPECT_EQ(get("GdipPathIterGetCount"), 14);
    EXPECT_EQ(get("GdipPathIterGetSubpathCount"), 4);
    EXPECT_EQ(get("GdipPathIterHasCurve"), 1);
    EXPECT_EQ(get("GdipPathIterIsValid"), 1);
    const GuestAddr RC = BUF + 0x100, S = RC + 4, E = RC + 8, C = RC + 12, T = RC + 16;
    auto sub = [&] {
        write_ints(RC, {-7, -7, -7, 7});
        EXPECT_EQ(call("GdipPathIterNextSubpath", {I(it), I(RC), I(S), I(E), I(C)}), Status::Ok);
        return std::array<i32, 4>{read_int(RC), read_int(S), read_int(E), read_int(C)};
    };
    auto type = [&] {
        write_ints(RC, {-7, -7, -7});
        EXPECT_TRUE(memory.write_u8(T, 0xEE).is_ok());
        EXPECT_EQ(call("GdipPathIterNextPathType", {I(it), I(RC), I(T), I(S), I(E)}), Status::Ok);
        return std::array<i32, 4>{read_int(RC), memory.read_u8(T).value(), read_int(S), read_int(E)};
    };
    auto marker = [&] {
        write_ints(RC, {-7, -7, -7});
        EXPECT_EQ(call("GdipPathIterNextMarker", {I(it), I(RC), I(S), I(E)}), Status::Ok);
        return std::array<i32, 3>{read_int(RC), read_int(S), read_int(E)};
    };
    using A4 = std::array<i32, 4>;
    using A3 = std::array<i32, 3>;
    EXPECT_EQ(type(), (A4{0, 0xEE, -7, -7}));  // nothing before a subpath
    EXPECT_EQ(sub(), (A4{4, 0, 3, 1}));
    EXPECT_EQ(type(), (A4{4, 1, 0, 3}));
    EXPECT_EQ(type(), (A4{0, 0xEE, -7, -7}));
    EXPECT_EQ(sub(), (A4{4, 4, 7, 0}));
    EXPECT_EQ(type(), (A4{4, 3, 4, 7}));
    EXPECT_EQ(sub(), (A4{4, 8, 11, 1}));
    EXPECT_EQ(sub(), (A4{2, 12, 13, 0}));
    EXPECT_EQ(sub(), (A4{0, 0, 0, 1}));
    EXPECT_EQ(sub(), (A4{0, 0, 0, 1}));
    EXPECT_EQ(marker(), (A3{2, 0, 1}));
    EXPECT_EQ(marker(), (A3{6, 2, 7}));
    EXPECT_EQ(marker(), (A3{6, 8, 13}));
    EXPECT_EQ(marker(), (A3{0, -7, -7}));

    // Subpath / marker paths ("iter_markers").
    EXPECT_EQ(call("GdipPathIterRewind", {I(it)}), Status::Ok);
    const u64 sp = new_path(1);
    write_int(C, 7);
    EXPECT_EQ(call("GdipPathIterNextSubpathPath", {I(it), I(RC), I(sp), I(C)}), Status::Ok);
    EXPECT_EQ(read_int(RC), 4);
    EXPECT_EQ(read_int(C), 1);
    expect_path(sp, "0,0;10,0;10,10;0,10", "00210181");
    call("GdipGetPathFillMode", {I(sp), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 1);
    line(sp, 50, 50, 60, 60);  // the copy has no open figure
    EXPECT_EQ(types(sp), parse_types("002101810001"));
    EXPECT_EQ(call("GdipPathIterNextSubpathPath", {I(it), I(RC), I(0), I(C)}), Status::Ok);
    call("GdipPathIterRewind", {I(it)});
    EXPECT_EQ(call("GdipPathIterNextMarkerPath", {I(it), I(RC), I(sp)}), Status::Ok);
    expect_path(sp, "0,0;10,0", "0021");
    EXPECT_EQ(call("GdipPathIterNextMarkerPath", {I(it), I(RC), I(sp)}), Status::Ok);
    EXPECT_EQ(read_int(RC), 6);
    EXPECT_EQ(call("GdipPathIterNextMarkerPath", {I(it), I(RC), I(sp)}), Status::Ok);
    expect_path(sp, "40,40;45,40;45,45;40,45;50,50;60,60", "000101810001");
    EXPECT_EQ(call("GdipPathIterNextMarkerPath", {I(it), I(RC), I(sp)}), Status::Ok);
    EXPECT_EQ(read_int(RC), 0);
    EXPECT_EQ(count(sp), 6);  // left alone past the end
    // Enumerate continues from the last marker section ("enum3" at 8).
    const GuestAddr EP = BUF + 0x400, ET = BUF + 0x800;
    auto enumerate = [&](u32 n) {
        write_int(RC, -7);
        EXPECT_EQ(call("GdipPathIterEnumerate", {I(it), I(RC), I(EP), I(ET), I(n)}), Status::Ok);
        return read_int(RC);
    };
    EXPECT_EQ(enumerate(3), 3);
    EXPECT_FLOAT_EQ(read_real(EP), 40);
    EXPECT_EQ(enumerate(64), 3);
    auto copy = [&](i32 s, i32 e) {
        write_int(RC, -7);
        EXPECT_EQ(call("GdipPathIterCopyData", {I(it), I(RC), I(EP), I(ET), I(static_cast<u32>(s)),
                                                I(static_cast<u32>(e))}),
                  Status::Ok);
        return read_int(RC);
    };
    EXPECT_EQ(copy(2, 4), 3);
    EXPECT_FLOAT_EQ(read_real(EP), 10);
    EXPECT_FLOAT_EQ(read_real(EP + 4), 10);
    EXPECT_EQ(memory.read_u8(ET).value(), 1);
    EXPECT_EQ(copy(4, 2), 0);
    EXPECT_EQ(copy(-1, 2), 0);
    EXPECT_EQ(copy(0, 1000), 0);
    EXPECT_EQ(copy(13, 13), 1);
    EXPECT_EQ(call("GdipPathIterEnumerate", {I(it), I(0), I(EP), I(ET), I(2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipPathIterNextSubpath", {I(it), I(0), I(S), I(E), I(C)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipPathIterGetCount", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipPathIterGetCount", {I(it), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreatePathIter", {I(0), I(p)}), Status::InvalidParameter);

    // Snapshot at creation ("iter_after_add", "iter_after_delete").
    line(p, 70, 70, 80, 80);
    EXPECT_EQ(get("GdipPathIterGetCount"), 14);
    call("GdipDeletePath", {I(p)});
    EXPECT_EQ(get("GdipPathIterGetCount"), 14);
    EXPECT_EQ(call("GdipDeletePathIter", {I(it)}), Status::Ok);
    EXPECT_EQ(call("GdipDeletePathIter", {I(it)}), Status::InvalidParameter);
}

TEST_P(Gdip_geometry_Test, PathIteratorEnumerateSharesTheSubpathCursor) {
    // "iter2_src": [0..2] marker at 2; [3..7] Bézier + line, marker at 7; [8..9].
    const u64 p = new_path();
    line(p, 0, 0, 1, 1);
    line(p, 1, 1, 2, 2);
    call("GdipSetPathMarker", {I(p)});
    call("GdipStartPathFigure", {I(p)});
    bezier(p, {10, 10, 11, 11, 12, 12, 13, 13});
    line(p, 13, 13, 14, 14);
    call("GdipSetPathMarker", {I(p)});
    call("GdipStartPathFigure", {I(p)});
    line(p, 20, 20, 21, 21);
    ASSERT_EQ(types(p), parse_types("00012100030303210001"));
    ASSERT_EQ(call("GdipCreatePathIter", {I(OUT), I(p)}), Status::Ok);
    const u64 it = read_handle(OUT);
    const GuestAddr RC = BUF + 0x100, S = RC + 4, E = RC + 8, C = RC + 12, T = RC + 16;
    const GuestAddr EP = BUF + 0x400, ET = BUF + 0x800;
    auto enumerate = [&](u32 n) {
        write_int(RC, -7);
        EXPECT_EQ(call("GdipPathIterEnumerate", {I(it), I(RC), I(EP), I(ET), I(n)}), Status::Ok);
        return std::make_pair(read_int(RC), read_real(EP));
    };
    auto sub = [&] {
        call("GdipPathIterNextSubpath", {I(it), I(RC), I(S), I(E), I(C)});
        return std::array<i32, 3>{read_int(RC), read_int(S), read_int(E)};
    };
    auto marker = [&] {
        call("GdipPathIterNextMarker", {I(it), I(RC), I(S), I(E)});
        return std::array<i32, 3>{read_int(RC), read_int(S), read_int(E)};
    };
    using A3 = std::array<i32, 3>;
    EXPECT_EQ(enumerate(2), std::make_pair(2, 0.0f));
    EXPECT_EQ(enumerate(2), std::make_pair(2, 2.0f));
    EXPECT_EQ(enumerate(64), std::make_pair(6, 11.0f));
    EXPECT_EQ(enumerate(64).first, 0);
    call("GdipPathIterRewind", {I(it)});
    EXPECT_EQ(enumerate(64), std::make_pair(10, 0.0f));
    call("GdipPathIterRewind", {I(it)});
    EXPECT_EQ(sub(), (A3{3, 0, 2}));
    EXPECT_EQ(enumerate(64), std::make_pair(7, 10.0f));
    EXPECT_EQ(enumerate(64).first, 0);
    EXPECT_EQ(sub(), (A3{0, 0, 0}));
    call("GdipPathIterRewind", {I(it)});
    EXPECT_EQ(marker(), (A3{3, 0, 2}));
    EXPECT_EQ(enumerate(64), std::make_pair(10, 0.0f));
    EXPECT_EQ(marker(), (A3{5, 3, 7}));
    EXPECT_EQ(sub(), (A3{4, 4, 7}));  // "sub_after_mark2"
    EXPECT_EQ(marker(), (A3{2, 8, 9}));
    call("GdipPathIterRewind", {I(it)});
    sub();
    EXPECT_EQ(sub(), (A3{5, 3, 7}));
    write_ints(RC, {-7, -7, -7});
    call("GdipPathIterNextPathType", {I(it), I(RC), I(T), I(S), I(E)});
    EXPECT_EQ((A3{read_int(RC), read_int(S), read_int(E)}), (A3{4, 3, 6}));
    EXPECT_EQ(memory.read_u8(T).value(), 3);
    EXPECT_EQ(marker(), (A3{3, 0, 2}));
    write_int(RC, -7);
    call("GdipPathIterNextPathType", {I(it), I(RC), I(T), I(S), I(E)});
    EXPECT_EQ(read_int(RC), 0);  // a marker ends path-type iteration ("type2")
    EXPECT_EQ(enumerate(64), std::make_pair(10, 0.0f));
    EXPECT_EQ(enumerate(0).first, 0);
    EXPECT_EQ(enumerate(0xFFFFFFFF).first, 0);

    // "iter_mixed": runs share their boundary point.
    const u64 q = new_path();
    arc(q, 0, 0, 10, 10, 0, 90);
    line(q, 0, 5, 0, 0);
    call("GdipSetPathMarker", {I(q)});
    call("GdipClosePathFigure", {I(q)});
    line(q, 50, 50, 60, 60);
    ASSERT_EQ(call("GdipCreatePathIter", {I(OUT), I(q)}), Status::Ok);
    const u64 it2 = read_handle(OUT);
    write_ints(RC, {-7, -7, -7, 7});
    call("GdipPathIterNextSubpath", {I(it2), I(RC), I(S), I(E), I(C)});
    EXPECT_EQ((std::array<i32, 4>{read_int(RC), read_int(S), read_int(E), read_int(C)}),
              (std::array<i32, 4>{6, 0, 5, 1}));
    call("GdipPathIterNextPathType", {I(it2), I(RC), I(T), I(S), I(E)});
    EXPECT_EQ((A3{read_int(RC), read_int(S), read_int(E)}), (A3{4, 0, 3}));
    EXPECT_EQ(memory.read_u8(T).value(), 3);
    call("GdipPathIterNextPathType", {I(it2), I(RC), I(T), I(S), I(E)});
    EXPECT_EQ((A3{read_int(RC), read_int(S), read_int(E)}), (A3{3, 3, 5}));
    EXPECT_EQ(memory.read_u8(T).value(), 1);
}

TEST_P(Gdip_geometry_Test, PathIteratorOverNothing) {
    ASSERT_EQ(call("GdipCreatePathIter", {I(OUT), I(0)}), Status::Ok);  // "iter_null"
    const u64 it = read_handle(OUT);
    const GuestAddr RC = BUF + 0x100, S = RC + 4, E = RC + 8, C = RC + 12;
    call("GdipPathIterGetCount", {I(it), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 0);
    call("GdipPathIterHasCurve", {I(it), I(OUT2)});
    EXPECT_EQ(read_int(OUT2), 0);
    write_ints(RC, {-7, -7, -7, 7});
    EXPECT_EQ(call("GdipPathIterNextSubpath", {I(it), I(RC), I(S), I(E), I(C)}), Status::Ok);
    EXPECT_EQ(read_int(RC), 0);
    EXPECT_EQ(read_int(S), -7);  // not written for an empty path
    EXPECT_EQ(read_int(C), 7);
    write_ints(RC, {-7, -7, -7});
    EXPECT_EQ(call("GdipPathIterNextMarker", {I(it), I(RC), I(S), I(E)}), Status::Ok);
    EXPECT_EQ(read_int(RC), 0);
    EXPECT_EQ(read_int(S), -7);
}

INSTANTIATE_GDIP_BITNESS(Gdip_geometry_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
