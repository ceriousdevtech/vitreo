/// @file test_gdi32_geometry.cpp
/// @brief GDI regions, clipping, transforms, polygons and paths measured on
///        Windows 11 (tools/oracle_probe/s3gdi_region_probe.expected).

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kMem = 0x10000;
constexpr GuestAddr kStack = 0x30000;

class GdiGeometryTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kMem, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE, GdiManager::GDI_DIB_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        mgr_.set_guest_memory(&memory_);
        exports_ = make_gdi32_exports(mgr_);
        // The probe's 64x48 32bpp top-down DIB section.
        const GuestAddr bmi = kMem + 0x100;
        (void)memory_.write_u32(bmi + 0, 40);
        (void)memory_.write_u32(bmi + 4, 64);
        (void)memory_.write_u32(bmi + 8, static_cast<u32>(-48));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 32);
        dib_ = mgr_.create_dib_section(0, bmi, 0, kMem + 0x80, false);
        bits_ = memory_.read_u32(kMem + 0x80).value_or(0);
        dc_ = mgr_.create_compatible_dc(0);
        (void)mgr_.select_object(dc_, dib_);
    }

    u64 call(const std::string& name, const std::vector<u64>& args) {
        cpu_ = cpu::CpuState{};
        cpu_.mode = cpu::CpuMode::Long;
        cpu_.gpr.rsp = kStack;
        cpu_.gpr.rcx = args.size() > 0 ? args[0] : 0;
        cpu_.gpr.rdx = args.size() > 1 ? args[1] : 0;
        cpu_.gpr.r8 = args.size() > 2 ? args[2] : 0;
        cpu_.gpr.r9 = args.size() > 3 ? args[3] : 0;
        for (usize i = 4; i < args.size(); ++i) (void)memory_.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
                               [&](const auto& e) { return e.name == name; });
        EXPECT_NE(it, exports_.entries.end()) << name;
        if (it == exports_.entries.end()) return 0;
        it->handler(m);
        return cpu_.gpr.rax;
    }

    static std::string expected_line(const std::string& prefix) {
        std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/s3gdi_region_probe.expected");
        for (std::string line; std::getline(in, line);) {
            if (line.rfind(prefix, 0) == 0) return line;
        }
        ADD_FAILURE() << "no line " << prefix;
        return {};
    }

    void reset() {
        for (i32 i = 0; i < 64 * 48; ++i) (void)memory_.write_u32(bits_ + i * 4, kFill);
    }
    /// "n=.. box=.." as the probe prints its coverage.
    std::string coverage() {
        i32 l = 64, t = 48, r = -1, b = -1, n = 0;
        for (i32 y = 0; y < 48; ++y) {
            for (i32 x = 0; x < 64; ++x) {
                if (memory_.read_u32(bits_ + (y * 64 + x) * 4).value() == kFill) continue;
                ++n;
                l = std::min(l, x);
                t = std::min(t, y);
                r = std::max(r, x);
                b = std::max(b, y);
            }
        }
        return "n=" + std::to_string(n) + " box=" + std::to_string(l) + "," + std::to_string(t) + "," +
               std::to_string(r + 1) + "," + std::to_string(b + 1);
    }
    static std::string coverage_of(const std::string& line) {
        const auto n = line.find(" n=");
        const auto box_end = line.find(' ', line.find("box="));
        return line.substr(n + 1, box_end - n - 1);
    }
    GuestAddr points(GuestAddr at, const std::vector<std::pair<i32, i32>>& pts) {
        for (usize i = 0; i < pts.size(); ++i) {
            (void)memory_.write_u32(at + i * 8, static_cast<u32>(pts[i].first));
            (void)memory_.write_u32(at + i * 8 + 4, static_cast<u32>(pts[i].second));
        }
        return at;
    }
    std::string rect_at(GuestAddr at) {
        std::string s;
        for (u32 i = 0; i < 4; ++i) s += (i ? "," : "") + std::to_string(static_cast<i32>(memory_.read_u32(at + i * 4).value()));
        return s;
    }
    std::string rgn_box(Handle rgn) {
        const u64 type = call("GetRgnBox", {rgn, kMem + 0x500});
        return "type=" + std::to_string(type) + " box=" + rect_at(kMem + 0x500);
    }

    static constexpr u32 kFill = 0x00112233;
    HandleTable handles_;
    GdiManager mgr_{handles_};
    GuestMemory memory_;
    cpu::CpuState cpu_;
    DllExports exports_;
    Handle dib_{0};
    Handle dc_{0};
    GuestAddr bits_{0};
};

TEST_F(GdiGeometryTest, RectRegionsAreNormalised) {
    // "empty type=1 box=0,0,0,0", "inverted type=2 box=0,0,10,10",
    // "after setrectrgn inverted type=2 box=10,5,20,30".
    EXPECT_EQ(rgn_box(call("CreateRectRgn", {5, 5, 5, 9})), "type=1 box=0,0,0,0");
    EXPECT_EQ(rgn_box(call("CreateRectRgn", {10, 10, 0, 0})), "type=2 box=0,0,10,10");
    const Handle r = static_cast<Handle>(call("CreateRectRgn", {0, 0, 10, 10}));
    set_last_error(0xDEAD);
    EXPECT_EQ(call("SetRectRgn", {r, 20, 30, 10, 5}), 1u);
    EXPECT_EQ(rgn_box(r), "type=2 box=10,5,20,30");
    EXPECT_EQ(get_last_error(), 0xDEADu);
    // A triangle region: "polygon tri type=3 box=0,0,10,8".
    (void)points(kMem + 0x600, {{0, 0}, {10, 0}, {5, 8}});
    EXPECT_EQ(rgn_box(call("CreatePolygonRgn", {kMem + 0x600, 3, 1})), "type=3 box=0,0,10,8");
}

TEST_F(GdiGeometryTest, ClipRegionsAreDeviceUnitsAndClipBoxLogical) {
    // "clip vporg clipbox=2 0,0,5,5 getcliprgn=1 10,10,15,15",
    // "clip rgn vporg clipbox=2 -6,-6,10,10 getcliprgn=1 4,4,20,20",
    // and IntersectClipRect / ExcludeClipRect answer COMPLEXREGION.
    const Handle probe = static_cast<Handle>(call("CreateRectRgn", {0, 0, 0, 0}));
    EXPECT_EQ(call("GetClipRgn", {dc_, probe}), 0u);
    (void)call("SetViewportOrgEx", {dc_, 10, 10, 0});
    EXPECT_EQ(call("IntersectClipRect", {dc_, 0, 0, 5, 5}), 3u);
    EXPECT_EQ(call("GetClipBox", {dc_, kMem + 0x500}), 2u);
    EXPECT_EQ(rect_at(kMem + 0x500), "0,0,5,5");
    EXPECT_EQ(call("GetClipRgn", {dc_, probe}), 1u);
    EXPECT_EQ(rgn_box(probe), "type=2 box=10,10,15,15");
    const Handle c = static_cast<Handle>(call("CreateRectRgn", {4, 4, 20, 20}));
    EXPECT_EQ(call("SelectClipRgn", {dc_, c}), 2u);
    (void)call("GetClipBox", {dc_, kMem + 0x500});
    EXPECT_EQ(rect_at(kMem + 0x500), "-6,-6,10,10");
    EXPECT_EQ(call("ExtSelectClipRgn", {dc_, 0, 1}), 0u);  // RGN_AND with NULL
    EXPECT_EQ(call("ExtSelectClipRgn", {dc_, 0, 5}), 2u);  // RGN_COPY NULL: no clip
    EXPECT_EQ(call("GetClipRgn", {dc_, probe}), 0u);
    (void)call("SetViewportOrgEx", {dc_, 0, 0, 0});
    EXPECT_EQ(call("ExcludeClipRect", {dc_, 100, 100, 110, 110}), 3u);
    EXPECT_EQ(call("SelectClipRgn", {dc_, 0}), 2u);
    EXPECT_EQ(call("OffsetClipRgn", {dc_, 3, 3}), 2u);
    (void)call("IntersectClipRect", {dc_, 0, 0, 10, 10});
    EXPECT_EQ(call("OffsetClipRgn", {dc_, 3, 3}), 2u);
    (void)call("GetClipBox", {dc_, kMem + 0x500});
    EXPECT_EQ(rect_at(kMem + 0x500), "3,3,13,13");
}

TEST_F(GdiGeometryTest, SaveAndRestoreDcLevels) {
    // "savedc=1", "savedc2=2", "clip level1 ... 1,1,3,3", "restoredc(5)=0".
    EXPECT_EQ(call("SaveDC", {dc_}), 1u);
    (void)call("IntersectClipRect", {dc_, 1, 1, 3, 3});
    EXPECT_EQ(call("SaveDC", {dc_}), 2u);
    (void)call("IntersectClipRect", {dc_, 2, 2, 3, 3});
    EXPECT_EQ(call("RestoreDC", {dc_, static_cast<u64>(-1)}), 1u);
    (void)call("GetClipBox", {dc_, kMem + 0x500});
    EXPECT_EQ(rect_at(kMem + 0x500), "1,1,3,3");
    EXPECT_EQ(call("RestoreDC", {dc_, 1}), 1u);
    EXPECT_EQ(call("GetClipBox", {dc_, kMem + 0x500}), 2u);
    EXPECT_EQ(rect_at(kMem + 0x500), "0,0,64,48");
    set_last_error(0xDEAD);
    EXPECT_EQ(call("RestoreDC", {dc_, 5}), 0u);
    EXPECT_EQ(call("RestoreDC", {dc_, static_cast<u64>(-1)}), 0u);
    EXPECT_EQ(call("SaveDC", {0}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(GdiGeometryTest, WorldTransformsComposeAndConvertPoints) {
    auto xform = [&](GuestAddr at, std::array<float, 6> v) {
        (void)memory_.write_bytes(at, reinterpret_cast<const u8*>(v.data()), 24);
        return at;
    };
    auto read_xform = [&]() {
        float v[6] = {};
        EXPECT_EQ(call("GetWorldTransform", {dc_, kMem + 0x700}), 1u);
        (void)memory_.read_bytes(kMem + 0x700, reinterpret_cast<u8*>(v), 24);
        std::string s;
        for (int i = 0; i < 6; ++i) s += (i ? "," : "") + std::to_string(static_cast<int>(v[i]));
        return s;
    };
    EXPECT_EQ(read_xform(), "1,0,0,1,0,0");  // GM_COMPATIBLE still answers
    EXPECT_EQ(call("SetWorldTransform", {dc_, xform(kMem + 0x600, {2, 0, 0, 3, 5, 7})}), 0u);
    EXPECT_EQ(call("SetGraphicsMode", {dc_, 2}), 1u);
    EXPECT_EQ(call("SetWorldTransform", {dc_, kMem + 0x600}), 1u);
    const GuestAddr t = xform(kMem + 0x620, {1, 0, 0, 1, 10, 20});
    EXPECT_EQ(call("ModifyWorldTransform", {dc_, t, 2}), 1u);  // MWT_LEFTMULTIPLY
    EXPECT_EQ(read_xform(), "2,0,0,3,25,67");
    EXPECT_EQ(call("ModifyWorldTransform", {dc_, t, 3}), 1u);  // MWT_RIGHTMULTIPLY
    EXPECT_EQ(read_xform(), "2,0,0,3,35,87");
    (void)points(kMem + 0x800, {{0, 0}, {100, 100}});
    EXPECT_EQ(call("DPtoLP", {dc_, kMem + 0x800, 2}), 1u);
    EXPECT_EQ(rect_at(kMem + 0x800), "-18,-29,32,4");  // halves to even
    (void)points(kMem + 0x800, {{1, 1}, {3, 4}});
    EXPECT_EQ(call("LPtoDP", {dc_, kMem + 0x800, 2}), 1u);
    EXPECT_EQ(rect_at(kMem + 0x800), "37,90,41,99");
    EXPECT_EQ(call("ModifyWorldTransform", {dc_, t, 7}), 0u);
    EXPECT_EQ(call("SetWorldTransform", {dc_, xform(kMem + 0x640, {0, 0, 0, 0, 1, 1})}), 0u);
    EXPECT_EQ(call("SetGraphicsMode", {dc_, 1}), 2u);
    EXPECT_EQ(call("SetGraphicsMode", {dc_, 3}), 0u);
}

TEST_F(GdiGeometryTest, MappingModesExtentsAndPenState) {
    EXPECT_EQ(call("SetMapMode", {dc_, 8}), 1u);  // MM_ANISOTROPIC
    EXPECT_EQ(call("SetWindowExtEx", {dc_, 100, 200, kMem + 0x500}), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x500).value(), 1u);
    EXPECT_EQ(call("SetViewportExtEx", {dc_, 50, static_cast<u64>(-100), 0}), 1u);
    (void)points(kMem + 0x800, {{0, 0}, {100, 200}});
    (void)call("DPtoLP", {dc_, kMem + 0x800, 2});
    EXPECT_EQ(rect_at(kMem + 0x800), "0,0,200,-400");
    EXPECT_EQ(call("SetMapMode", {dc_, 1}), 8u);
    EXPECT_EQ(call("SetMapMode", {dc_, 0}), 0u);
    EXPECT_EQ(call("SetMapMode", {dc_, 9}), 0u);
    EXPECT_EQ(call("SetViewportExtEx", {dc_, 5, 5, 0}), 1u);  // ignored in MM_TEXT
    EXPECT_EQ(call("SetMapMode", {dc_, 7}), 1u);             // MM_ISOTROPIC
    (void)call("SetWindowExtEx", {dc_, 100, 100, 0});
    (void)call("SetViewportExtEx", {dc_, 50, 200, 0});
    EXPECT_EQ(call("GetViewportExtEx", {dc_, kMem + 0x500}), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x500).value(), 50u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x504).value(), 50u);
    // Miter limit and arc direction are stored and validated.
    EXPECT_EQ(call("SetArcDirection", {dc_, 2}), 1u);
    EXPECT_EQ(call("GetArcDirection", {dc_}), 2u);
    EXPECT_EQ(call("SetArcDirection", {dc_, 3}), 0u);
    EXPECT_TRUE(mgr_.set_miter_limit(dc_, 4.0f, nullptr));
    float old = 0;
    EXPECT_TRUE(mgr_.set_miter_limit(dc_, 10.0f, &old));
    EXPECT_EQ(old, 4.0f);
    EXPECT_FALSE(mgr_.set_miter_limit(dc_, 0.5f, nullptr));
}

TEST_F(GdiGeometryTest, PolygonsAndPolylinesCoverWhatWindowsCovers) {
    const Handle red = mgr_.create_pen(0, 1, 0x0000FF);
    const Handle blue = mgr_.create_solid_brush(0xFF0000);
    (void)mgr_.select_object(dc_, red);
    (void)mgr_.select_object(dc_, blue);
    const GuestAddr tri = points(kMem + 0x600, {{5, 5}, {25, 5}, {15, 25}});
    reset();
    EXPECT_EQ(call("Polyline", {dc_, tri, 3}), 1u);
    EXPECT_EQ(coverage(), coverage_of(expected_line("polyline tri")));
    reset();
    EXPECT_EQ(call("Polygon", {dc_, tri, 3}), 1u);
    EXPECT_EQ(coverage(), coverage_of(expected_line("polygon tri n=")));
    (void)mgr_.select_object(dc_, mgr_.get_stock_object(8));  // NULL_PEN
    reset();
    (void)call("Polygon", {dc_, tri, 3});
    EXPECT_EQ(coverage(), coverage_of(expected_line("polygon tri nullpen")));
    const GuestAddr star = points(kMem + 0x680, {{20, 2}, {30, 30}, {5, 12}, {35, 12}, {10, 30}});
    reset();
    (void)call("Polygon", {dc_, star, 5});
    EXPECT_EQ(coverage(), coverage_of(expected_line("star alternate")));
    (void)call("SetPolyFillMode", {dc_, 2});
    reset();
    (void)call("Polygon", {dc_, star, 5});
    EXPECT_EQ(coverage(), coverage_of(expected_line("star winding")));
    set_last_error(0xDEAD);
    EXPECT_EQ(call("Polyline", {dc_, tri, 1}), 0u);
    EXPECT_EQ(call("Polygon", {dc_, tri, 1}), 0u);
    EXPECT_EQ(call("Polyline", {0, tri, 3}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(GdiGeometryTest, PathsFillStrokeClipAndWiden) {
    const Handle red = mgr_.create_pen(0, 1, 0x0000FF);
    const Handle blue = mgr_.create_solid_brush(0xFF0000);
    (void)mgr_.select_object(dc_, red);
    (void)mgr_.select_object(dc_, blue);
    set_last_error(0xDEAD);
    EXPECT_EQ(call("FillPath", {dc_}), 0u);
    EXPECT_EQ(call("EndPath", {dc_}), 0u);
    EXPECT_EQ(call("CloseFigure", {dc_}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(call("BeginPath", {dc_}), 1u);
    (void)call("MoveToEx", {dc_, 5, 5, 0});
    (void)call("LineTo", {dc_, 30, 5});
    (void)call("LineTo", {dc_, 30, 20});
    EXPECT_EQ(call("CloseFigure", {dc_}), 1u);
    (void)call("MoveToEx", {dc_, 35, 5, 0});
    EXPECT_EQ(call("PolyBezierTo", {dc_, points(kMem + 0x600, {{40, 5}, {60, 20}, {40, 40}}), 3}), 1u);
    EXPECT_EQ(call("FillPath", {dc_}), 0u);  // still open
    EXPECT_EQ(call("EndPath", {dc_}), 1u);
    EXPECT_EQ(call("GetPath", {dc_, 0, 0, 0}), 7u);
    reset();
    EXPECT_EQ(call("FillPath", {dc_}), 1u);
    EXPECT_EQ(coverage(), coverage_of(expected_line("fillpath n=")));
    EXPECT_EQ(call("FillPath", {dc_}), 0u);  // consumed

    (void)call("BeginPath", {dc_});
    (void)call("MoveToEx", {dc_, 5, 5, 0});
    (void)call("LineTo", {dc_, 30, 5});
    (void)call("LineTo", {dc_, 30, 20});
    (void)call("CloseFigure", {dc_});
    (void)call("EndPath", {dc_});
    reset();
    EXPECT_EQ(call("StrokePath", {dc_}), 1u);
    EXPECT_EQ(coverage(), coverage_of(expected_line("strokepath n=")));

    (void)call("BeginPath", {dc_});
    (void)call("Rectangle", {dc_, 4, 4, 20, 16});
    (void)call("EndPath", {dc_});
    reset();
    EXPECT_EQ(call("StrokeAndFillPath", {dc_}), 1u);
    EXPECT_EQ(coverage(), coverage_of(expected_line("strokeandfill rect")));

    (void)call("BeginPath", {dc_});
    (void)call("Ellipse", {dc_, 2, 2, 30, 22});
    (void)call("EndPath", {dc_});
    reset();
    (void)call("FillPath", {dc_});
    EXPECT_EQ(coverage(), coverage_of(expected_line("fill ellipse path")));

    (void)call("BeginPath", {dc_});
    (void)call("MoveToEx", {dc_, 10, 10, 0});
    (void)call("LineTo", {dc_, 40, 10});
    (void)call("LineTo", {dc_, 40, 30});
    (void)call("LineTo", {dc_, 10, 30});
    (void)call("CloseFigure", {dc_});
    (void)call("EndPath", {dc_});
    EXPECT_EQ(call("SelectClipPath", {dc_, 5}), 1u);
    EXPECT_EQ(call("GetClipBox", {dc_, kMem + 0x500}), 2u);
    EXPECT_EQ(rect_at(kMem + 0x500), "10,10,40,30");
    (void)call("SelectClipRgn", {dc_, 0});

    (void)mgr_.select_object(dc_, mgr_.create_pen(0, 5, 0x00FF00));
    (void)call("BeginPath", {dc_});
    (void)call("MoveToEx", {dc_, 10, 10, 0});
    (void)call("LineTo", {dc_, 40, 10});
    (void)call("EndPath", {dc_});
    EXPECT_EQ(call("WidenPath", {dc_}), 1u);
    reset();
    EXPECT_EQ(call("FillPath", {dc_}), 1u);
    EXPECT_EQ(coverage(), coverage_of(expected_line("fill widened n=")));

    // An empty closed path fills nothing and succeeds.
    (void)call("BeginPath", {dc_});
    (void)call("EndPath", {dc_});
    EXPECT_EQ(call("GetPath", {dc_, 0, 0, 0}), 0u);
    EXPECT_EQ(call("FillPath", {dc_}), 1u);
}

TEST_F(GdiGeometryTest, DeviceCapsForMemoryDc) {
    // "caps ..." rows: every DC answers the display's values; a bad DC or
    // index is 0 with the error untouched.
    const std::pair<i32, i32> rows[] = {{0, 16384}, {2, 1},  {12, 32},   {14, 1},   {16, -1},  {24, -1},
                                        {40, 36},   {44, 51}, {88, 96},  {106, 20}, {108, 24}, {38, 32409},
                                        {28, 511},  {30, 254}, {32, 255}, {34, 30727}, {36, 1}, {119, 1}, {999, 0}};
    for (const auto& [index, value] : rows) {
        EXPECT_EQ(static_cast<i32>(call("GetDeviceCaps", {dc_, static_cast<u64>(index)})), value) << index;
    }
    set_last_error(0xDEAD);
    EXPECT_EQ(call("GetDeviceCaps", {0, 12}), 0u);
    EXPECT_EQ(get_last_error(), 0xDEADu);
}

TEST_F(GdiGeometryTest, AbortDocFailsForEveryDcWithoutAJob) {
    // shlwapi_regsec_shell_probe{,.x86}.expected abortdoc_*: no DC has a print
    // job (StartDoc never starts one here), so AbortDoc is SP_ERROR with
    // ERROR_INVALID_HANDLE for a memory, NULL or bogus DC.
    const auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
                                 [](const auto& e) { return e.name == "AbortDoc"; });
    ASSERT_NE(it, exports_.entries.end());
    EXPECT_EQ(it->stdcall_bytes, 4u);
    for (u64 dc : {static_cast<u64>(dc_), u64{0}, u64{0x1234}}) {
        set_last_error(0xDEAD);
        EXPECT_EQ(static_cast<i32>(call("AbortDoc", {dc})), -1) << dc;
        EXPECT_EQ(get_last_error(), 6u) << dc;
    }
}

}  // namespace
