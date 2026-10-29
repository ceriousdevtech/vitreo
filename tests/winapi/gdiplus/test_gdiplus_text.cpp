// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_text.cpp
/// @brief Contract tests for the GDI+ text area (gdiplus_text.cpp,
/// gdiplus_text_layout.cpp), run in both bitnesses through
/// gdiplus_fixture.hpp. Every expected value comes from Windows 11:
/// tools/oracle_probe/gdiplus_text_probe.c (.expected / .x86.expected),
/// for the fonts macOS ships as the same files (Arial, Times New Roman,
/// Courier New, Microsoft Sans Serif, Tahoma).

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>
#include <winapi/graphics/gdiplus/gdiplus_pen_brush.hpp>
#include <winapi/graphics/gdiplus/gdiplus_region.hpp>
#include <winapi/graphics/gdiplus/gdiplus_geometry.hpp>
#include <winapi/graphics/gdiplus/gdiplus_text.hpp>

#include <cmath>
#include <fstream>
#include <iterator>

namespace vitreo::winapi::gdip_test {
namespace {

constexpr float kTol = 0.011f;  // probe values are printed to 4 decimals

class Gdip_text_Test : public GdipTest {
protected:
    static constexpr GuestAddr STR = BUF + 0x200;    ///< string argument
    static constexpr GuestAddr RECT = BUF + 0x40;    ///< layout rectangle
    static constexpr GuestAddr BB = BUF + 0x80;      ///< bounding box out
    static constexpr GuestAddr CP = BUF + 0xA0;
    static constexpr GuestAddr LINES = BUF + 0xA4;

    u64 family(const std::u16string& name, u64 collection = 0) {
        write_wide(STR, name);
        poison(OUT);
        const Status s = call("GdipCreateFontFamilyFromName", {I(STR), I(collection), I(OUT)});
        return s == Status::Ok ? read_handle() : 0;
    }
    u64 font(u64 fam, float em, i32 style = 0, i32 unit = 2) {
        poison(OUT);
        EXPECT_EQ(call("GdipCreateFont", {I(fam), R(em), I(static_cast<u32>(style)), I(static_cast<u32>(unit)), I(OUT)}),
                  Status::Ok);
        return read_handle();
    }
    u64 font_named(const std::u16string& name, float em, i32 style = 0, i32 unit = 2) {
        return font(family(name), em, style, unit);
    }
    u64 format(i32 flags = 0) {
        EXPECT_EQ(call("GdipCreateStringFormat", {I(static_cast<u32>(flags)), I(0), I(OUT)}), Status::Ok);
        return read_handle();
    }
    u64 typographic() {
        EXPECT_EQ(call("GdipStringFormatGetGenericTypographic", {I(OUT)}), Status::Ok);
        return read_handle();
    }
    /// A 32bpp ARGB bitmap graphics (96 dpi), filled with @p fill.
    u64 graphics(i32 w = 400, i32 h = 200, u32 fill = 0xFFFFFFFF) {
        gdip::GpImage img;
        img.width = w;
        img.height = h;
        img.pixel_format = gdip::PixelFormat32bppARGB;
        img.stride = w * 4;
        img.bits.resize(static_cast<usize>(w) * static_cast<usize>(h) * 4u);
        for (usize i = 0; i < img.bits.size(); i += 4) std::memcpy(&img.bits[i], &fill, 4);
        image = mgr.objects().add(std::move(img));
        gdip::GpGraphics g;
        g.kind = gdip::TargetKind::Image;
        g.image = image;
        return mgr.objects().add(std::move(g));
    }
    gdip::GpGraphics& gref(u64 g) { return *mgr.objects().get<gdip::GpGraphics>(g); }

    struct Measured {
        Status status{Status::Ok};
        float x{0}, y{0}, w{0}, h{0};
        i32 cp{0}, lines{0};
    };
    Measured measure(u64 g, const std::u16string& text, u64 fnt, gdip::RectF r, u64 fmt, i32 length = -1) {
        write_wide(STR, text);
        write_reals(RECT, {r.X, r.Y, r.Width, r.Height});
        poison(BB, 16);
        poison(CP, 8);
        Measured out;
        out.status = call("GdipMeasureString", {I(g), I(STR), I(static_cast<u32>(length)), I(fnt), I(RECT), I(fmt),
                                                I(BB), I(CP), I(LINES)});
        out.x = read_real(BB);
        out.y = read_real(BB + 4);
        out.w = read_real(BB + 8);
        out.h = read_real(BB + 12);
        out.cp = read_int(CP);
        out.lines = read_int(LINES);
        return out;
    }
    void expect_box(const Measured& m, float x, float y, float w, float h, i32 cp, i32 lines,
                    const char* what) {
        EXPECT_EQ(m.status, Status::Ok) << what;
        EXPECT_NEAR(m.x, x, kTol) << what;
        EXPECT_NEAR(m.y, y, kTol) << what;
        EXPECT_NEAR(m.w, w, kTol) << what;
        EXPECT_NEAR(m.h, h, kTol) << what;
        EXPECT_EQ(m.cp, cp) << what;
        EXPECT_EQ(m.lines, lines) << what;
    }
    std::u16string family_name(u64 fam) {
        poison(BUF + 0x400, 64);
        EXPECT_EQ(call("GdipGetFamilyName", {I(fam), I(BUF + 0x400), I(0)}), Status::Ok);
        std::u16string s;
        for (usize i = 0; i < 32; ++i) {
            const u16 c = memory.read_u16(BUF + 0x400 + i * 2).value();
            if (c == 0) break;
            s.push_back(static_cast<char16_t>(c));
        }
        return s;
    }
    u32 metric(const char* api, u64 fam, i32 style = 0) {
        poison(OUT2, 4);
        EXPECT_EQ(call(api, {I(fam), I(static_cast<u32>(style)), I(OUT2)}), Status::Ok) << api;
        return memory.read_u16(OUT2).value();
    }

    u64 image{0};
};

// ---- Font families -----------------------------------------------------------

TEST_P(Gdip_text_Test, FamilyMetricsMatchWindows) {
    struct Row { const char16_t* name; const char16_t* reported; u32 asc, desc, ls; };
    // gdiplus_text_probe "family ...": em 2048 for all.
    const Row rows[] = {
        {u"Arial", u"Arial", 1854, 434, 2355},
        {u"arial", u"Arial", 1854, 434, 2355},
        {u"Times New Roman", u"Times New Roman", 1825, 443, 2355},
        {u"Courier New", u"Courier New", 1705, 615, 2320},
        {u"MS Shell Dlg", u"Microsoft Sans Serif", 1888, 430, 2318},
        {u"MS Shell Dlg 2", u"Tahoma", 2049, 423, 2472},
        // Windows also aliases Helvetica -> Arial through its registry
        // FontSubstitutes; Vitreo keeps only the documented MS Shell Dlg
        // aliases (.docs/contracts/gdi32.md), so Helvetica is the host face.
        {u"Tahoma", u"Tahoma", 2049, 423, 2472},
    };
    for (const Row& r : rows) {
        const u64 f = family(r.name);
        ASSERT_NE(f, 0u);
        EXPECT_EQ(family_name(f), r.reported);
        for (i32 style = 0; style < 4; ++style) {
            EXPECT_EQ(metric("GdipGetEmHeight", f, style), 2048u);
            EXPECT_EQ(metric("GdipGetCellAscent", f, style), r.asc);
            EXPECT_EQ(metric("GdipGetCellDescent", f, style), r.desc);
            EXPECT_EQ(metric("GdipGetLineSpacing", f, style), r.ls);
        }
        // Any style integer answers (probe "em style16/-1": 2048).
        EXPECT_EQ(metric("GdipGetEmHeight", f, 16), 2048u);
        EXPECT_EQ(metric("GdipGetEmHeight", f, -1), 2048u);
        for (i32 style : {0, 1, 2, 3, 4, 8, 15, 16, -1}) {
            poison(OUT2, 4);
            EXPECT_EQ(call("GdipIsStyleAvailable", {I(f), I(static_cast<u32>(style)), I(OUT2)}), Status::Ok);
            EXPECT_EQ(read_int(OUT2), 1);
        }
    }
}

TEST_P(Gdip_text_Test, FamilyLookupFailuresAndSharing) {
    for (const char16_t* name : {u"MS Sans Serif", u"System", u"Arial Bold", u"NoSuchFontXYZ", u"", u"Courier"}) {
        write_wide(STR, name);
        poison(OUT);
        EXPECT_EQ(call("GdipCreateFontFamilyFromName", {I(STR), I(0), I(OUT)}), Status::FontFamilyNotFound);
        EXPECT_EQ(read_handle(), 0u);  // *fontFamily = NULL
    }
    EXPECT_EQ(call("GdipCreateFontFamilyFromName", {I(0), I(0), I(OUT)}), Status::InvalidParameter);
    write_wide(STR, u"Arial");
    EXPECT_EQ(call("GdipCreateFontFamilyFromName", {I(STR), I(0), I(0)}), Status::InvalidParameter);

    // One object per family: create twice, clone, delete thrice, still valid.
    const u64 a = family(u"Arial"), b = family(u"ARIAL");
    EXPECT_EQ(a, b);
    EXPECT_EQ(call("GdipCloneFontFamily", {I(a), I(OUT)}), Status::Ok);
    EXPECT_EQ(read_handle(), a);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(call("GdipDeleteFontFamily", {I(a)}), Status::Ok);
    EXPECT_EQ(family_name(a), u"Arial");
    EXPECT_EQ(call("GdipDeleteFontFamily", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneFontFamily", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneFontFamily", {I(a), I(0)}), Status::InvalidParameter);
    // GetFamilyName: any LANGID, NULL buffer is Ok; NULL family is not.
    EXPECT_EQ(call("GdipGetFamilyName", {I(a), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetFamilyName", {I(0), I(BUF + 0x400), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetFamilyName", {I(a), I(BUF + 0x400), I(0x411)}), Status::Ok);
    // Metric getters: NULL out / family.
    EXPECT_EQ(call("GdipGetEmHeight", {I(a), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetEmHeight", {I(0), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipIsStyleAvailable", {I(a), I(0), I(0)}), Status::InvalidParameter);
    // A font is not a family.
    const u64 f = font(a, 10);
    EXPECT_EQ(call("GdipGetEmHeight", {I(f), I(0), I(OUT2)}), Status::InvalidParameter);
}

TEST_P(Gdip_text_Test, GenericFamilies) {
    struct G { const char* api; const char16_t* name; };
    for (const G g : {G{"GdipGetGenericFontFamilySansSerif", u"Microsoft Sans Serif"},
                      G{"GdipGetGenericFontFamilySerif", u"Times New Roman"},
                      G{"GdipGetGenericFontFamilyMonospace", u"Courier New"}}) {
        poison(OUT);
        ASSERT_EQ(call(g.api, {I(OUT)}), Status::Ok);
        const u64 first = read_handle();
        EXPECT_EQ(family_name(first), g.name);
        EXPECT_EQ(call("GdipDeleteFontFamily", {I(first)}), Status::Ok);
        ASSERT_EQ(call(g.api, {I(OUT)}), Status::Ok);
        EXPECT_EQ(read_handle(), first);  // shared, survives delete
        EXPECT_EQ(call(g.api, {I(0)}), Status::InvalidParameter);
    }
}

// ---- Fonts --------------------------------------------------------------------

TEST_P(Gdip_text_Test, CreateFontValidation) {
    const u64 arial = family(u"Arial");
    for (i32 unit : {-1, 1, 7}) {
        poison(OUT);
        EXPECT_EQ(call("GdipCreateFont", {I(arial), R(12), I(0), I(static_cast<u32>(unit)), I(OUT)}),
                  Status::InvalidParameter);
        EXPECT_EQ(memory.read_u32(OUT).value(), 0xCCCCCCCCu);  // untouched
    }
    for (float em : {0.0f, -1.0f}) {
        EXPECT_EQ(call("GdipCreateFont", {I(arial), R(em), I(0), I(2), I(OUT)}), Status::InvalidParameter);
    }
    EXPECT_EQ(call("GdipCreateFont", {I(arial), R(0.001f), I(0), I(2), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipCreateFont", {I(0), R(12), I(0), I(2), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCreateFont", {I(arial), R(12), I(0), I(2), I(0)}), Status::InvalidParameter);
    // Any style integer is stored (probe "create style=-1/16/32").
    for (i32 style : {-1, 0, 5, 15, 16, 32}) {
        const u64 f = font(arial, 20, style);
        EXPECT_EQ(call("GdipGetFontStyle", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), style);
    }
}

TEST_P(Gdip_text_Test, FontGettersAndHeights) {
    const u64 g = graphics(64, 64);
    struct Row { i32 unit; float h_null, h_g, h96, h72; i32 lf_height; };
    // gdiplus_text_probe "font unitN" (Arial 12; -1: GetFontHeight(NULL) is
    // only defined for World/Pixel).
    const Row rows[] = {
        {0, 13.7988f, 13.7988f, 13.7988f, 13.7988f, -12},
        {2, 13.7988f, 13.7988f, 13.7988f, 13.7988f, -12},
        {3, -1, 18.3984f, 18.3984f, 13.7988f, -16},
        {4, -1, 1324.6875f, 1324.6875f, 993.5156f, -1152},
        {5, -1, 4.4156f, 4.4156f, 3.3117f, -4},
        {6, -1, 52.1530f, 52.1530f, 39.1148f, -45},
    };
    const u64 arial = family(u"Arial");
    for (const Row& r : rows) {
        const u64 f = font(arial, 12, 0, r.unit);
        EXPECT_EQ(call("GdipGetFontSize", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_FLOAT_EQ(read_real(OUT2), 12.0f);
        EXPECT_EQ(call("GdipGetFontUnit", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), r.unit);
        if (r.h_null >= 0) {
            EXPECT_EQ(call("GdipGetFontHeight", {I(f), I(0), I(OUT2)}), Status::Ok);
            EXPECT_NEAR(read_real(OUT2), r.h_null, 1e-3f);
        } else {
            EXPECT_EQ(call("GdipGetFontHeight", {I(f), I(0), I(OUT2)}), Status::InvalidParameter);
        }
        EXPECT_EQ(call("GdipGetFontHeight", {I(f), I(g), I(OUT2)}), Status::Ok);
        EXPECT_NEAR(read_real(OUT2), r.h_g, 1e-3f) << r.unit;
        EXPECT_EQ(call("GdipGetFontHeightGivenDPI", {I(f), R(96), I(OUT2)}), Status::Ok);
        EXPECT_NEAR(read_real(OUT2), r.h96, 1e-3f);
        EXPECT_EQ(call("GdipGetFontHeightGivenDPI", {I(f), R(72), I(OUT2)}), Status::Ok);
        EXPECT_NEAR(read_real(OUT2), r.h72, 1e-3f);
        poison(BUF + 0x400, 92);
        ASSERT_EQ(call("GdipGetLogFontW", {I(f), I(g), I(BUF + 0x400)}), Status::Ok);
        EXPECT_EQ(read_int(BUF + 0x400), r.lf_height) << r.unit;
        EXPECT_EQ(read_int(BUF + 0x404), 0);
        EXPECT_EQ(read_int(BUF + 0x410), 400);
        EXPECT_EQ(memory.read_u32(BUF + 0x414).value(), 0u);  // italic/ul/so/charset
        EXPECT_EQ(memory.read_u32(BUF + 0x418).value(), 0u);  // precision/quality/pitch
        EXPECT_EQ(memory.read_u16(BUF + 0x41C).value(), u'A');
        EXPECT_EQ(memory.read_u16(BUF + 0x41C + 10).value(), 0u);  // "Arial\0"
        // Family is the shared object.
        EXPECT_EQ(call("GdipGetFamily", {I(f), I(OUT)}), Status::Ok);
        EXPECT_EQ(read_handle(), arial);
    }
    // Times / Courier em 10 (probe "font 1/2 ..."); page unit and scale
    // change GetFontHeight(graphics) but not GetLogFont.
    const u64 courier = font_named(u"Courier New", 10, 0, 3);
    EXPECT_EQ(call("GdipGetFontHeight", {I(courier), I(g), I(OUT2)}), Status::Ok);
    EXPECT_NEAR(read_real(OUT2), 15.1042f, 1e-3f);
    const u64 pt10 = font(arial, 10, 1, 3);
    auto& gs = gref(g).state;
    gs.page_unit = 3;
    EXPECT_EQ(call("GdipGetFontHeight", {I(pt10), I(g), I(OUT2)}), Status::Ok);
    EXPECT_NEAR(read_real(OUT2), 11.4990f, 1e-3f);
    gs.page_unit = 6;
    EXPECT_EQ(call("GdipGetFontHeight", {I(pt10), I(g), I(OUT2)}), Status::Ok);
    EXPECT_NEAR(read_real(OUT2), 4.0566f, 1e-3f);
    ASSERT_EQ(call("GdipGetLogFontW", {I(pt10), I(g), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0x400), -13);
    EXPECT_EQ(read_int(BUF + 0x410), 700);
    gs.page_unit = 2;
    gs.page_scale = 2;
    EXPECT_EQ(call("GdipGetFontHeight", {I(pt10), I(g), I(OUT2)}), Status::Ok);
    EXPECT_NEAR(read_real(OUT2), 7.6660f, 1e-3f);
    gs.page_scale = 1;
    gs.page_unit = 3;
    const u64 world10 = font(arial, 10, 1, 0);
    EXPECT_EQ(call("GdipGetFontHeight", {I(world10), I(g), I(OUT2)}), Status::Ok);
    EXPECT_NEAR(read_real(OUT2), 11.4990f, 1e-3f);
    ASSERT_EQ(call("GdipGetLogFontW", {I(world10), I(g), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0x400), -13);  // 10 pt in pixels
    gs.page_unit = 1;
    // LOGFONTA: 60 bytes, ANSI face.
    poison(BUF + 0x400, 64);
    ASSERT_EQ(call("GdipGetLogFontA", {I(pt10), I(g), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0x400), -13);
    EXPECT_EQ(memory.read_u8(BUF + 0x41C).value(), 'A');
    EXPECT_EQ(memory.read_u8(BUF + 0x421).value(), 0u);
    EXPECT_EQ(memory.read_u8(BUF + 0x43C).value(), 0xCCu);  // nothing past 60 bytes
    // Style bits -> LOGFONT.
    const u64 all = font(arial, 20, -1);
    ASSERT_EQ(call("GdipGetLogFontW", {I(all), I(g), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0x410), 700);
    EXPECT_EQ(memory.read_u8(BUF + 0x414).value(), 1u);
    EXPECT_EQ(memory.read_u8(BUF + 0x415).value(), 1u);
    EXPECT_EQ(memory.read_u8(BUF + 0x416).value(), 1u);
    // Errors.
    EXPECT_EQ(call("GdipGetLogFontW", {I(pt10), I(0), I(BUF + 0x400)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLogFontW", {I(pt10), I(g), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetLogFontW", {I(0), I(g), I(BUF + 0x400)}), Status::InvalidParameter);
    for (const char* api : {"GdipGetFontStyle", "GdipGetFontSize", "GdipGetFontUnit", "GdipGetFamily"}) {
        EXPECT_EQ(call(api, {I(pt10), I(0)}), Status::InvalidParameter) << api;
        EXPECT_EQ(call(api, {I(0), I(OUT2)}), Status::InvalidParameter) << api;
    }
    // Clone: a new font on the same family.
    ASSERT_EQ(call("GdipCloneFont", {I(pt10), I(OUT)}), Status::Ok);
    const u64 clone = read_handle();
    EXPECT_NE(clone, pt10);
    EXPECT_EQ(call("GdipGetFamily", {I(clone), I(OUT)}), Status::Ok);
    EXPECT_EQ(read_handle(), arial);
    EXPECT_EQ(call("GdipDeleteFont", {I(clone)}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteFont", {I(clone)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteFont", {I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_text_Test, FontFromLogfontAndDC) {
    const Handle dc = gdi.create_compatible_dc(0);
    ASSERT_NE(dc, 0u);
    auto logfont = [&](i32 height, i32 weight, u8 italic, u8 under, const std::u16string& face) {
        std::vector<u8> zero(92, 0);
        ASSERT_TRUE(memory.write_bytes(BUF + 0x400, zero.data(), zero.size()).is_ok());
        write_int(BUF + 0x400, height);
        write_int(BUF + 0x410, weight);
        ASSERT_TRUE(memory.write_u8(BUF + 0x414, italic).is_ok());
        ASSERT_TRUE(memory.write_u8(BUF + 0x415, under).is_ok());
        ASSERT_TRUE(memory.write_u8(BUF + 0x416, under).is_ok());
        write_wide(BUF + 0x41C, face);
    };
    struct Row { i32 height, weight; u8 italic, under; const char16_t* face; float size; i32 style; };
    // gdiplus_text_probe "fromlogfontW ..." (unit World throughout).
    const Row rows[] = {
        {-13, 400, 0, 0, u"Arial", 13, 0},
        {0, 400, 0, 0, u"Arial", 16, 0},
        {-20, 400, 1, 0, u"Arial", 20, 2},
        {-100, 400, 0, 0, u"Times New Roman", 100, 0},
        {-13, 600, 0, 0, u"MS Shell Dlg", 13, 1},
    };
    for (const Row& r : rows) {
        logfont(r.height, r.weight, r.italic, r.under, r.face);
        poison(OUT);
        ASSERT_EQ(call("GdipCreateFontFromLogfontW", {I(dc), I(BUF + 0x400), I(OUT)}), Status::Ok) << r.height;
        const u64 f = read_handle();
        EXPECT_EQ(call("GdipGetFontSize", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_FLOAT_EQ(read_real(OUT2), r.size);
        EXPECT_EQ(call("GdipGetFontUnit", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), 0);
        EXPECT_EQ(call("GdipGetFontStyle", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), r.style);
    }
    logfont(-13, 400, 0, 0, u"MS Shell Dlg");
    ASSERT_EQ(call("GdipCreateFontFromLogfontW", {I(dc), I(BUF + 0x400), I(OUT)}), Status::Ok);
    ASSERT_EQ(call("GdipGetFamily", {I(read_handle()), I(OUT)}), Status::Ok);
    EXPECT_EQ(family_name(read_handle()), u"Microsoft Sans Serif");
    // Underline + strikeout bits.
    logfont(-17, 400, 0, 1, u"Arial");
    ASSERT_EQ(call("GdipCreateFontFromLogfontW", {I(dc), I(BUF + 0x400), I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipGetFontStyle", {I(read_handle()), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 12);
    // Faces GDI+ cannot name: NotTrueTypeFont and *font = NULL.
    for (const char16_t* face : {u"NoSuchFontXYZ", u"System", u""}) {
        logfont(-13, 600, 0, 0, face);
        poison(OUT);
        EXPECT_EQ(call("GdipCreateFontFromLogfontW", {I(dc), I(BUF + 0x400), I(OUT)}), Status::NotTrueTypeFont);
        EXPECT_EQ(read_handle(), 0u);
    }
    logfont(-13, 400, 0, 0, u"Arial");
    poison(OUT);
    EXPECT_EQ(call("GdipCreateFontFromLogfontW", {I(0), I(BUF + 0x400), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0xCCCCCCCCu);
    EXPECT_EQ(call("GdipCreateFontFromLogfontW", {I(0x1234), I(BUF + 0x400), I(OUT)}), Status::NotTrueTypeFont);
    EXPECT_EQ(read_handle(), 0u);
    EXPECT_EQ(call("GdipCreateFontFromLogfontW", {I(dc), I(0), I(OUT)}), Status::InvalidParameter);
    // LOGFONTA: Courier New -16 italic.
    std::vector<u8> zero(60, 0);
    ASSERT_TRUE(memory.write_bytes(BUF + 0x500, zero.data(), zero.size()).is_ok());
    write_int(BUF + 0x500, -16);
    ASSERT_TRUE(memory.write_u8(BUF + 0x514, 1).is_ok());
    const char face[] = "Courier New";
    ASSERT_TRUE(memory.write_bytes(BUF + 0x51C, reinterpret_cast<const u8*>(face), sizeof(face)).is_ok());
    ASSERT_EQ(call("GdipCreateFontFromLogfontA", {I(dc), I(BUF + 0x500), I(OUT)}), Status::Ok);
    const u64 fa = read_handle();
    EXPECT_EQ(call("GdipGetFontSize", {I(fa), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2), 16.0f);
    EXPECT_EQ(call("GdipGetFontStyle", {I(fa), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 2);
    // FromDC: the selected font (Times New Roman -15 bold), and the stock
    // System font (NotTrueTypeFont).
    poison(OUT);
    EXPECT_EQ(call("GdipCreateFontFromDC", {I(dc), I(OUT)}), Status::NotTrueTypeFont);
    EXPECT_EQ(read_handle(), 0u);
    GdiManager::FontInfo info;
    info.height = -15;
    info.lf_weight = 700;
    info.weight = 700;
    info.face_name = u"Times New Roman";
    const Handle hf = gdi.create_font(info);
    const Handle old = gdi.select_object(dc, hf);
    ASSERT_EQ(call("GdipCreateFontFromDC", {I(dc), I(OUT)}), Status::Ok);
    const u64 fd = read_handle();
    EXPECT_EQ(call("GdipGetFontSize", {I(fd), I(OUT2)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT2), 15.0f);
    EXPECT_EQ(call("GdipGetFontStyle", {I(fd), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    (void)gdi.select_object(dc, old);
    EXPECT_EQ(call("GdipCreateFontFromDC", {I(0), I(OUT)}), Status::InvalidParameter);
}

// ---- Font collections ------------------------------------------------------------

TEST_P(Gdip_text_Test, InstalledCollection) {
    ASSERT_EQ(call("GdipNewInstalledFontCollection", {I(OUT)}), Status::Ok);
    const u64 c = read_handle();
    ASSERT_EQ(call("GdipNewInstalledFontCollection", {I(OUT)}), Status::Ok);
    EXPECT_EQ(read_handle(), c);  // one per process
    ASSERT_EQ(call("GdipGetFontCollectionFamilyCount", {I(c), I(OUT2)}), Status::Ok);
    const i32 n = read_int(OUT2);
    EXPECT_GT(n, 10);
    ASSERT_EQ(call("GdipGetFontCollectionFamilyList", {I(c), I(4), I(BUF + 0x400), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 4);
    // The list hands out the same family objects CreateFontFamilyFromName does.
    const usize ptr = ptr_size();
    const GuestAddr all = DATA + 0x40000;
    ASSERT_EQ(call("GdipGetFontCollectionFamilyList", {I(c), I(static_cast<u32>(n + 8)), I(all), I(OUT2)}),
              Status::Ok);
    EXPECT_EQ(read_int(OUT2), n);
    const u64 arial = family(u"Arial", c);
    bool found = false;
    for (i32 i = 0; i < n; ++i) found |= read_handle(all + static_cast<u64>(i) * ptr) == arial;
    EXPECT_TRUE(found);
    // numSought <= 0: Ok, none (probe "count0", "neg").
    poison(OUT2, 4);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyList", {I(c), I(0), I(BUF + 0x400), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyList", {I(c), I(static_cast<u32>(-1)), I(BUF + 0x400), I(OUT2)}),
              Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyList", {I(c), I(4), I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyList", {I(c), I(4), I(BUF + 0x400), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyList", {I(0), I(4), I(BUF + 0x400), I(OUT2)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyCount", {I(0), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyCount", {I(c), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipNewInstalledFontCollection", {I(0)}), Status::InvalidParameter);
    // Deleting the installed collection through the private API: Ok, *p = NULL.
    write_int(OUT3, 0);
    ASSERT_TRUE(memory.write_bytes(OUT3, reinterpret_cast<const u8*>(&c), ptr).is_ok());
    EXPECT_EQ(call("GdipDeletePrivateFontCollection", {I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT3), 0u);
    ASSERT_EQ(call("GdipGetFontCollectionFamilyCount", {I(c), I(OUT2)}), Status::Ok);
}

TEST_P(Gdip_text_Test, PrivateCollection) {
    ASSERT_EQ(call("GdipNewPrivateFontCollection", {I(OUT)}), Status::Ok);
    const u64 p = read_handle();
    ASSERT_EQ(call("GdipGetFontCollectionFamilyCount", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    poison(OUT2, 4);
    ASSERT_EQ(call("GdipGetFontCollectionFamilyList", {I(p), I(4), I(BUF + 0x400), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    write_wide(STR, u"C:\\nope\\nofont.ttf");
    EXPECT_EQ(call("GdipPrivateAddFontFile", {I(p), I(STR)}), Status::FileNotFound);
    EXPECT_EQ(call("GdipPrivateAddFontFile", {I(p), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipPrivateAddFontFile", {I(0), I(STR)}), Status::InvalidParameter);
    // Memory fonts: junk is accepted and adds nothing; bad args refused.
    std::vector<u8> junk(256, 0x41);
    ASSERT_TRUE(memory.write_bytes(BUF + 0x400, junk.data(), junk.size()).is_ok());
    EXPECT_EQ(call("GdipPrivateAddMemoryFont", {I(p), I(BUF + 0x400), I(256)}), Status::Ok);
    ASSERT_EQ(call("GdipGetFontCollectionFamilyCount", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0);
    EXPECT_EQ(call("GdipPrivateAddMemoryFont", {I(p), I(0), I(10)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipPrivateAddMemoryFont", {I(p), I(BUF + 0x400), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipPrivateAddMemoryFont", {I(p), I(BUF + 0x400), I(static_cast<u32>(-1))}),
              Status::InvalidParameter);
    // A real face: the host's copy of Courier New (the same file Windows ships).
    std::ifstream in("/System/Library/Fonts/Supplemental/Courier New.ttf", std::ios::binary);
    if (!in) GTEST_SKIP() << "host has no Courier New.ttf";
    std::vector<u8> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const GuestAddr at = DATA + 0x20000;
    ASSERT_LT(ttf.size(), DATA_SIZE - 0x20000);
    ASSERT_TRUE(memory.write_bytes(at, ttf.data(), ttf.size()).is_ok());
    ASSERT_EQ(call("GdipPrivateAddMemoryFont", {I(p), I(at), I(static_cast<u32>(ttf.size()))}), Status::Ok);
    std::vector<u8> zero(ttf.size(), 0);  // GDI+ copied it
    ASSERT_TRUE(memory.write_bytes(at, zero.data(), zero.size()).is_ok());
    ASSERT_EQ(call("GdipGetFontCollectionFamilyCount", {I(p), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    const u64 cf = family(u"Courier New", p);
    ASSERT_NE(cf, 0u);
    EXPECT_EQ(family_name(cf), u"Courier New");
    EXPECT_EQ(metric("GdipGetCellAscent", cf), 1705u);
    EXPECT_EQ(metric("GdipGetLineSpacing", cf), 2320u);
    for (i32 style = 0; style < 4; ++style) {
        EXPECT_EQ(call("GdipIsStyleAvailable", {I(cf), I(static_cast<u32>(style)), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), 1);
        EXPECT_EQ(call("GdipCreateFont", {I(cf), R(10), I(static_cast<u32>(style)), I(2), I(OUT)}), Status::Ok);
    }
    EXPECT_NE(cf, family(u"Courier New"));  // not the installed family object
    write_wide(STR, u"Arial");
    poison(OUT);
    EXPECT_EQ(call("GdipCreateFontFamilyFromName", {I(STR), I(p), I(OUT)}), Status::FontFamilyNotFound);
    EXPECT_EQ(read_handle(), 0u);
    ASSERT_EQ(call("GdipGetFontCollectionFamilyList", {I(p), I(4), I(BUF + 0x400), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(read_handle(BUF + 0x400), cf);
    // Delete: *p = NULL; NULL pointer refused; pointer to NULL is Ok.
    ASSERT_TRUE(memory.write_bytes(OUT3, reinterpret_cast<const u8*>(&p), ptr_size()).is_ok());
    EXPECT_EQ(call("GdipDeletePrivateFontCollection", {I(OUT3)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT3), 0u);
    EXPECT_EQ(call("GdipGetFontCollectionFamilyCount", {I(p), I(OUT2)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeletePrivateFontCollection", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeletePrivateFontCollection", {I(OUT3)}), Status::Ok);
}

// ---- String formats ----------------------------------------------------------------

TEST_P(Gdip_text_Test, StringFormatDefaultsAndGenerics) {
    struct Want { i32 flags, align, line, trim, hotkey, tabs, ranges; };
    auto check = [&](u64 f, const Want& w, const char* what) {
        EXPECT_EQ(call("GdipGetStringFormatFlags", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.flags) << what;
        EXPECT_EQ(call("GdipGetStringFormatAlign", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.align) << what;
        EXPECT_EQ(call("GdipGetStringFormatLineAlign", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.line) << what;
        EXPECT_EQ(call("GdipGetStringFormatTrimming", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.trim) << what;
        EXPECT_EQ(call("GdipGetStringFormatHotkeyPrefix", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.hotkey) << what;
        EXPECT_EQ(call("GdipGetStringFormatTabStopCount", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.tabs) << what;
        EXPECT_EQ(call("GdipGetStringFormatMeasurableCharacterRangeCount", {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), w.ranges) << what;
        poison(OUT3, 8);
        EXPECT_EQ(call("GdipGetStringFormatDigitSubstitution", {I(f), I(OUT3), I(OUT3 + 4)}), Status::Ok);
        EXPECT_EQ(memory.read_u16(OUT3).value(), 0u) << what;
        EXPECT_EQ(read_int(OUT3 + 4), 0) << what;
    };
    // gdiplus_text_probe "fresh(...)", "genericdefault", "generictypographic".
    EXPECT_EQ(call("GdipCreateStringFormat", {I(0), I(0), I(OUT)}), Status::Ok);
    check(read_handle(), {0, 0, 0, 1, 0, 0, 0}, "fresh");
    EXPECT_EQ(call("GdipCreateStringFormat", {I(0x7FFFFFFF), I(0x409), I(OUT)}), Status::Ok);
    check(read_handle(), {0x7FFFFFFF, 0, 0, 1, 0, 0, 0}, "fresh 7fffffff");
    EXPECT_EQ(call("GdipCreateStringFormat", {I(0xFFFFFFFF), I(0xFFFF), I(OUT)}), Status::Ok);
    check(read_handle(), {-1, 0, 0, 1, 0, 0, 0}, "fresh -1");
    EXPECT_EQ(call("GdipCreateStringFormat", {I(0), I(0), I(0)}), Status::InvalidParameter);
    ASSERT_EQ(call("GdipStringFormatGetGenericDefault", {I(OUT)}), Status::Ok);
    const u64 d = read_handle();
    ASSERT_EQ(call("GdipStringFormatGetGenericDefault", {I(OUT)}), Status::Ok);
    EXPECT_EQ(read_handle(), d);
    check(d, {0, 0, 0, 1, 0, 0, 0}, "generic default");
    const u64 t = typographic();
    EXPECT_EQ(typographic(), t);
    check(t, {0x6004, 0, 0, 0, 0, 0, 0}, "generic typographic");
    EXPECT_EQ(call("GdipStringFormatGetGenericDefault", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipStringFormatGetGenericTypographic", {I(0)}), Status::InvalidParameter);
    // Generic formats are shared and mutable, and survive delete.
    EXPECT_EQ(call("GdipSetStringFormatAlign", {I(d), I(1)}), Status::Ok);
    ASSERT_EQ(call("GdipStringFormatGetGenericDefault", {I(OUT)}), Status::Ok);
    EXPECT_EQ(call("GdipGetStringFormatAlign", {I(read_handle()), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
    EXPECT_EQ(call("GdipSetStringFormatAlign", {I(d), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteStringFormat", {I(d)}), Status::Ok);
    check(d, {0, 0, 0, 1, 0, 0, 0}, "generic default after delete");
    // A clone is a separate object with the source's state.
    ASSERT_EQ(call("GdipCloneStringFormat", {I(t), I(OUT)}), Status::Ok);
    const u64 c = read_handle();
    EXPECT_NE(c, t);
    check(c, {0x6004, 0, 0, 0, 0, 0, 0}, "clone typographic");
    EXPECT_EQ(call("GdipSetStringFormatFlags", {I(c), I(0)}), Status::Ok);
    check(t, {0x6004, 0, 0, 0, 0, 0, 0}, "typographic after clone change");
    EXPECT_EQ(call("GdipDeleteStringFormat", {I(c)}), Status::Ok);
    EXPECT_EQ(call("GdipDeleteStringFormat", {I(c)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneStringFormat", {I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipCloneStringFormat", {I(t), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteStringFormat", {I(0)}), Status::InvalidParameter);
}

TEST_P(Gdip_text_Test, StringFormatSetters) {
    const u64 f = format();
    struct S { const char* set; const char* get; i32 lo, hi; };
    for (const S s : {S{"GdipSetStringFormatAlign", "GdipGetStringFormatAlign", 0, 2},
                      S{"GdipSetStringFormatLineAlign", "GdipGetStringFormatLineAlign", 0, 2},
                      S{"GdipSetStringFormatTrimming", "GdipGetStringFormatTrimming", 0, 5},
                      S{"GdipSetStringFormatHotkeyPrefix", "GdipGetStringFormatHotkeyPrefix", 0, 2}}) {
        for (i32 v = s.lo; v <= s.hi; ++v) {
            EXPECT_EQ(call(s.set, {I(f), I(static_cast<u32>(v))}), Status::Ok) << s.set;
            EXPECT_EQ(call(s.get, {I(f), I(OUT2)}), Status::Ok);
            EXPECT_EQ(read_int(OUT2), v);
        }
        // Out of range: refused, value kept.
        EXPECT_EQ(call(s.set, {I(f), I(static_cast<u32>(s.hi + 1))}), Status::InvalidParameter) << s.set;
        EXPECT_EQ(call(s.set, {I(f), I(static_cast<u32>(-1))}), Status::InvalidParameter) << s.set;
        EXPECT_EQ(call(s.get, {I(f), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), s.hi);
        EXPECT_EQ(call(s.set, {I(0), I(0)}), Status::InvalidParameter);
        EXPECT_EQ(call(s.get, {I(f), I(0)}), Status::InvalidParameter);
    }
    EXPECT_EQ(call("GdipSetStringFormatFlags", {I(f), I(0x1234)}), Status::Ok);
    EXPECT_EQ(call("GdipGetStringFormatFlags", {I(f), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 0x1234);
    EXPECT_EQ(call("GdipGetStringFormatFlags", {I(0), I(OUT2)}), Status::InvalidParameter);
    // Digit substitution: any value stored; outputs optional.
    for (i32 v : {-1, 0, 3, 4}) {
        EXPECT_EQ(call("GdipSetStringFormatDigitSubstitution", {I(f), I(0x401), I(static_cast<u32>(v))}), Status::Ok);
        EXPECT_EQ(call("GdipGetStringFormatDigitSubstitution", {I(f), I(OUT3), I(OUT3 + 4)}), Status::Ok);
        EXPECT_EQ(memory.read_u16(OUT3).value(), 0x401u);
        EXPECT_EQ(read_int(OUT3 + 4), v);
    }
    EXPECT_EQ(call("GdipGetStringFormatDigitSubstitution", {I(f), I(0), I(OUT3 + 4)}), Status::Ok);
    EXPECT_EQ(call("GdipGetStringFormatDigitSubstitution", {I(f), I(OUT3), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipGetStringFormatDigitSubstitution", {I(f), I(0), I(0)}), Status::Ok);
}

TEST_P(Gdip_text_Test, TabStopsAndRanges) {
    const u64 f = format();
    write_reals(BUF + 0x400, {10, 20, 30, 40});
    ASSERT_EQ(call("GdipSetStringFormatTabStops", {I(f), R(5), I(3), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(call("GdipGetStringFormatTabStopCount", {I(f), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    auto get = [&](i32 count) {
        write_reals(BUF + 0x500, {-9, -9, -9, -9, -9});
        write_real(OUT3, -9);
        return call("GdipGetStringFormatTabStops", {I(f), I(static_cast<u32>(count)), I(OUT3), I(BUF + 0x500)});
    };
    EXPECT_EQ(get(3), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(OUT3), 5);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x500), 10);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x508), 30);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x50C), -9);
    EXPECT_EQ(get(2), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x504), 20);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x508), -9);
    EXPECT_EQ(get(5), Status::Ok);  // writes the three it has
    EXPECT_FLOAT_EQ(read_real(BUF + 0x508), 30);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x50C), -9);
    EXPECT_EQ(get(0), Status::Ok);  // first offset only
    EXPECT_FLOAT_EQ(read_real(OUT3), 5);
    EXPECT_FLOAT_EQ(read_real(BUF + 0x500), -9);
    EXPECT_EQ(call("GdipGetStringFormatTabStops", {I(f), I(3), I(0), I(BUF + 0x500)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetStringFormatTabStops", {I(f), I(3), I(OUT3), I(0)}), Status::InvalidParameter);
    // count <= 0: Ok, unchanged; NULL tabs refused; negatives NotImplemented.
    EXPECT_EQ(call("GdipSetStringFormatTabStops", {I(f), R(7), I(0), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(call("GdipSetStringFormatTabStops", {I(f), R(7), I(static_cast<u32>(-1)), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(call("GdipGetStringFormatTabStopCount", {I(f), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipSetStringFormatTabStops", {I(f), R(7), I(2), I(0)}), Status::InvalidParameter);
    write_reals(BUF + 0x400, {-5, 10});
    EXPECT_EQ(call("GdipSetStringFormatTabStops", {I(f), R(0), I(2), I(BUF + 0x400)}), Status::NotImplemented);
    write_reals(BUF + 0x400, {10, 20});
    EXPECT_EQ(call("GdipSetStringFormatTabStops", {I(f), R(-3), I(2), I(BUF + 0x400)}), Status::NotImplemented);

    // Measurable ranges: up to 32.
    for (i32 i = 0; i < 40; ++i) write_ints(BUF + 0x600 + static_cast<u64>(i) * 8, {i, 1});
    auto count = [&] {
        EXPECT_EQ(call("GdipGetStringFormatMeasurableCharacterRangeCount", {I(f), I(OUT2)}), Status::Ok);
        return read_int(OUT2);
    };
    EXPECT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(f), I(3), I(BUF + 0x600)}), Status::Ok);
    EXPECT_EQ(count(), 3);
    EXPECT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(f), I(32), I(BUF + 0x600)}), Status::Ok);
    EXPECT_EQ(count(), 32);
    EXPECT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(f), I(33), I(BUF + 0x600)}),
              Status::ValueOverflow);
    EXPECT_EQ(count(), 32);
    EXPECT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(f), I(0), I(BUF + 0x600)}), Status::Ok);
    EXPECT_EQ(count(), 0);
    EXPECT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(f), I(2), I(0)}), Status::InvalidParameter);
    write_ints(BUF + 0x600, {-1, 1});
    EXPECT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(f), I(1), I(BUF + 0x600)}), Status::Ok);
    // A clone carries tabs and ranges.
    ASSERT_EQ(call("GdipCloneStringFormat", {I(f), I(OUT)}), Status::Ok);
    const u64 c = read_handle();
    EXPECT_EQ(call("GdipGetStringFormatTabStopCount", {I(c), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);
    EXPECT_EQ(call("GdipGetStringFormatMeasurableCharacterRangeCount", {I(c), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
}

// ---- MeasureString -------------------------------------------------------------------

TEST_P(Gdip_text_Test, MeasureStringSingleLines) {
    const u64 g = graphics();
    const u64 typo = typographic();
    struct Row { const char16_t* fam; float em; const char16_t* text; float dw, dh, tw, th; i32 cp, lines; };
    // gdiplus_text_probe "measure hint0 fN emE tK def|typo" (NULL format / typographic).
    const Row rows[] = {
        {u"Arial", 10, u"Hello World", 56.5609f, 12.4219f, 51.6797f, 11.1719f, 11, 1},
        {u"Arial", 10, u"x", 8.4847f, 12.4219f, 5.0f, 11.1719f, 1, 1},
        {u"Arial", 10, u"W", 13.0550f, 12.4219f, 9.4385f, 11.1719f, 1, 1},
        {u"Arial", 10, u"iiii", 12.4935f, 12.4219f, 8.8867f, 11.1719f, 4, 1},
        {u"Arial", 10, u" ", 3.3333f, 12.4219f, 0.0f, 11.1719f, 1, 1},
        {u"Arial", 10, u"a b ", 17.6497f, 12.4219f, 13.9014f, 11.1719f, 4, 1},
        {u"Arial", 10, u"Ag\nBy", 15.9277f, 24.2480f, 12.2314f, 22.9980f, 5, 2},
        {u"Arial", 10, u"a\tb", 49.0576f, 12.7490f, 11.1279f, 11.4990f, 3, 1},
        {u"Arial", 10, u"A\r\nB\n", 10.2002f, 24.2480f, 6.6699f, 22.9980f, 5, 2},
        {u"Arial", 20, u"Hello World", 113.1217f, 24.8438f, 103.3594f, 22.3438f, 11, 1},
        {u"Arial", 20, u"Ag\nBy", 31.8555f, 48.4961f, 24.4629f, 45.9961f, 5, 2},
        {u"Arial", 20, u"a\tb", 98.1152f, 25.4980f, 22.2559f, 22.9980f, 3, 1},
        {u"Times New Roman", 10, u"Hello World", 55.1009f, 12.3242f, 50.2637f, 11.0742f, 11, 1},
        {u"Times New Roman", 10, u"iiii", 14.7786f, 12.3242f, 11.1133f, 11.0742f, 4, 1},
        {u"Times New Roman", 10, u"a\tb", 48.4814f, 12.7490f, 9.4434f, 11.4990f, 3, 1},
        {u"Times New Roman", 20, u"Hello World", 110.2018f, 24.6484f, 100.5273f, 22.1484f, 11, 1},
        {u"Courier New", 10, u"Hello World", 71.3314f, 12.5781f, 66.0107f, 11.3281f, 11, 1},
        {u"Courier New", 10, u"iiii", 28.0599f, 12.5781f, 24.0039f, 11.3281f, 4, 1},
        {u"Courier New", 10, u"Ag\nBy", 15.6934f, 23.9062f, 12.0020f, 22.6562f, 5, 2},
        {u"Courier New", 10, u"a\tb", 49.5117f, 12.5781f, 12.0068f, 11.3281f, 3, 1},
    };
    for (const Row& r : rows) {
        const u64 f = font_named(r.fam, r.em);
        const std::string what = std::string(r.fam[0] == u'A' ? "Arial" : r.fam[0] == u'T' ? "Times" : "Courier") +
                                 " " + std::to_string(r.em) + " " + std::to_string(r.cp);
        expect_box(measure(g, r.text, f, {0, 0, 0, 0}, 0), 0, 0, r.dw, r.dh, r.cp, r.lines, what.c_str());
        expect_box(measure(g, r.text, f, {0, 0, 0, 0}, typo), 0, 0, r.tw, r.th, r.cp, r.lines, what.c_str());
    }
    // The generic default format measures as NULL does for plain text.
    ASSERT_EQ(call("GdipStringFormatGetGenericDefault", {I(OUT)}), Status::Ok);
    const u64 gdef = read_handle();
    const u64 arial10 = font_named(u"Arial", 10);
    expect_box(measure(g, u"Hello World", arial10, {0, 0, 0, 0}, gdef), 0, 0, 56.5609f, 12.4219f, 11, 1, "gdef");
    // Empty string / length 0: an empty box, nothing fitted.
    expect_box(measure(g, u"", arial10, {0, 0, 0, 0}, 0), 0, 0, 0, 0, 0, 0, "empty");
    expect_box(measure(g, u"abc", arial10, {0, 0, 0, 0}, 0, 0), 0, 0, 0, 0, 0, 0, "len0");
    const u64 arial20 = font_named(u"Arial", 20);
    expect_box(measure(g, u"abcdef", arial20, {0, 0, 0, 0}, format(), 3), 0, 0, 39.8796f, 24.8438f, 3, 1, "len3");
    expect_box(measure(g, u"abc", arial20, {5, 5, -10, -10}, format()), 5, 5, 39.8796f, 24.8438f, 3, 1, "negrect");
    // Styles: real bold-italic face advances; underline/strikeout change nothing.
    expect_box(measure(g, u"Hello World", font_named(u"Arial", 20, 3), {0, 0, 0, 0}, typo), 0, 0, 111.1035f,
               22.3438f, 11, 1, "bolditalic");
    expect_box(measure(g, u"Hello World", font_named(u"Arial", 20, 12), {0, 0, 0, 0}, typo), 0, 0, 103.3594f,
               22.3438f, 11, 1, "underline strike");
    // Simple-line cases that stay the cell tall (probe "simple ...", "complex nbsp/hyphen", "accented").
    const u64 fmt = format();
    expect_box(measure(g, u"Hello World", arial20, {0, 0, 300, 0}, fmt), 0, 0, 113.1217f, 24.8438f, 11, 1, "w300");
    expect_box(measure(g, u"Hello\u00A0World", arial20, {0, 0, 300, 0}, fmt), 0, 0, 113.1217f, 24.8438f, 11, 1,
               "nbsp");
    expect_box(measure(g, u"well-known", arial20, {0, 0, 300, 0}, fmt), 0, 0, 108.5514f, 24.8438f, 10, 1, "hyphen");
    expect_box(measure(g, u"\u00E9t\u00E9", arial20, {0, 0, 300, 0}, fmt), 0, 0, 35.2995f, 24.8438f, 3, 1,
               "accented");
}

TEST_P(Gdip_text_Test, MeasureStringUnitsAndPage) {
    const u64 g = graphics();
    const u64 typo = typographic();
    const u64 pt12 = font_named(u"Arial", 12, 0, 3);
    auto& s = gref(g).state;
    expect_box(measure(g, u"Hello", pt12, {0, 0, 0, 0}, typo), 0, 0, 36.4609f, 17.8750f, 5, 1, "display");
    s.page_unit = 3;
    expect_box(measure(g, u"Hello", pt12, {0, 0, 0, 0}, typo), 0, 0, 27.3457f, 13.4062f, 5, 1, "point");
    s.page_unit = 6;
    expect_box(measure(g, u"Hello", pt12, {0, 0, 0, 0}, typo), 0, 0, 9.6470f, 4.7294f, 5, 1, "mm");
    s.page_unit = 2;
    s.page_scale = 2;
    expect_box(measure(g, u"Hello", pt12, {0, 0, 0, 0}, typo), 0, 0, 18.2305f, 8.9375f, 5, 1, "px scale2");
    expect_box(measure(g, u"Hello", font_named(u"Arial", 20, 0, 0), {0, 0, 0, 0}, typo), 0, 0, 45.5762f, 22.3438f,
               5, 1, "world scale2");
}

TEST_P(Gdip_text_Test, MeasureStringWrapAndTrim) {
    const u64 g = graphics();
    const u64 f = font_named(u"Arial", 20);
    const u64 fmt = format();
    const std::u16string para = u"The quick brown fox jumps over the lazy dog";
    struct W { float w, h; float bw, bh; i32 cp, lines; };
    // probe "wrap w=..": layout (10, 5, w, h), trimming Character.
    for (const W r : {W{40, 0, 39.8730f, 370.4688f, 43, 16}, W{40, 50, 35.2930f, 48.4961f, 5, 2},
                      W{80, 0, 77.6562f, 186.4844f, 43, 8}, W{80, 50, 78.7891f, 48.4961f, 12, 2},
                      W{120, 0, 117.7441f, 94.4922f, 43, 4}, W{120, 50, 117.7148f, 48.4961f, 22, 2},
                      W{160, 0, 157.7930f, 71.4941f, 43, 3}, W{160, 50, 157.7930f, 48.4961f, 32, 2},
                      W{200, 0, 190.9961f, 71.4941f, 43, 3}, W{200, 50, 190.9961f, 48.4961f, 40, 2}}) {
        const std::string what = "wrap w=" + std::to_string(r.w) + " h=" + std::to_string(r.h);
        expect_box(measure(g, para, f, {10, 5, r.w, r.h}, fmt), 10, 5, r.bw, r.bh, r.cp, r.lines, what.c_str());
    }
    struct F { i32 flags; float x, bw, bh; i32 cp, lines; float tw; };
    // probe "flags=0x.. w100h50" and "flags=0x.. trailing" ("ab  ", zero rect).
    for (const F r : {F{0x1000, 10, 95.9766f, 25.4980f, 10, 1, 29.5768f}, F{0x2000, 10, 95.9766f, 48.4961f, 20, 2, 29.5768f},
                      F{0x4000, 10, 95.9766f, 48.4961f, 20, 2, 29.5768f}, F{0x0004, 10, 95.9766f, 48.4961f, 20, 2, 29.5768f},
                      F{0x0800, 10, 100.0f, 48.4961f, 20, 2, 41.0221f}, F{0x0001, 14.0234f, 95.9766f, 48.4961f, 20, 2, 29.5703f},
                      F{0x6004, 10, 95.9766f, 48.4961f, 20, 2, 29.5768f}, F{0x3000, 10, 95.9766f, 25.4980f, 10, 1, 29.5768f}}) {
        const u64 ff = format(r.flags);
        const std::string what = "flags " + std::to_string(r.flags);
        expect_box(measure(g, para, f, {10, 5, 100, 50}, ff), r.x, 5, r.bw, r.bh, r.cp, r.lines, what.c_str());
        const Measured t = measure(g, u"ab  ", f, {0, 0, 0, 0}, ff);
        EXPECT_NEAR(t.w, r.tw, kTol) << what;
        EXPECT_EQ(t.cp, 4) << what;
        if (r.flags == 0x0001) {
            EXPECT_NEAR(t.x, -29.5703f, kTol);  // RTL: right-aligned on the origin
            EXPECT_NEAR(t.h, 25.4980f, kTol);
        } else {
            EXPECT_NEAR(t.h, 24.8438f, kTol) << what;
        }
    }
    struct T { i32 trim; float wrap_w; i32 wrap_cp, wrap_lines; float nowrap_w; i32 nowrap_cp; };
    // probe "trim=N w100h24" / "trim=N nowrap w100h24" (EllipsisPath not
    // matched exactly: see .docs/contracts/gdiplus/text.md).
    for (const T r : {T{0, 95.9766f, 43, 5, 100.0f, 43}, T{1, 95.9766f, 10, 1, 95.9766f, 10},
                      T{2, 95.9766f, 10, 1, 95.9766f, 10}, T{3, 95.9766f, 7, 1, 95.3711f, 7},
                      T{4, 95.9766f, 4, 1, 62.1582f, 4}}) {
        const u64 tf = format();
        ASSERT_EQ(call("GdipSetStringFormatTrimming", {I(tf), I(static_cast<u32>(r.trim))}), Status::Ok);
        const std::string what = "trim " + std::to_string(r.trim);
        expect_box(measure(g, para, f, {0, 0, 100, 24}, tf), 0, 0, r.wrap_w, 24, r.wrap_cp, r.wrap_lines, what.c_str());
        ASSERT_EQ(call("GdipSetStringFormatFlags", {I(tf), I(0x1000)}), Status::Ok);
        expect_box(measure(g, para, f, {0, 0, 100, 24}, tf), 0, 0, r.nowrap_w, 24, r.nowrap_cp, 1, what.c_str());
    }
    struct E { i32 trim; float w; float bw; i32 cp; };
    // probe "ellipsis trim=N nowrap w=..", layout height 30.
    for (const E r : {E{3, 60, 50.7031f, 2}, E{3, 120, 115.9766f, 10}, E{3, 200, 189.2383f, 17},
                      E{4, 60, 50.7031f, 2}, E{4, 120, 115.9766f, 10}, E{4, 200, 177.7930f, 16}}) {
        const u64 tf = format(0x1000);
        ASSERT_EQ(call("GdipSetStringFormatTrimming", {I(tf), I(static_cast<u32>(r.trim))}), Status::Ok);
        const std::string what = "ellipsis " + std::to_string(r.trim) + " w=" + std::to_string(r.w);
        expect_box(measure(g, para, f, {0, 0, r.w, 30}, tf), 0, 0, r.bw, 25.4980f, r.cp, 1, what.c_str());
    }
    // Partly visible lines (probe "partial ...", "partial-scan", "partial h=").
    struct P { i32 trim, limit; float h; float bw, bh; i32 cp, lines; };
    for (const P r : {P{0, 0, 10, 95.9766f, 10, 43, 5}, P{0, 0, 47, 95.9766f, 47, 43, 5},
                      P{0, 1, 10, 0, 2.5f, 0, 0}, P{0, 1, 23, 95.9766f, 23, 10, 1},
                      P{0, 1, 30, 95.9766f, 25.4980f, 10, 1}, P{0, 1, 46, 95.9766f, 46, 20, 2},
                      P{1, 0, 10, 95.9766f, 10, 10, 1}, P{1, 0, 28, 95.9766f, 25.4980f, 10, 1},
                      P{1, 0, 29, 95.9766f, 29, 20, 2}, P{1, 0, 48, 95.9766f, 48, 20, 2},
                      P{1, 0, 52, 95.9766f, 52, 28, 3}, P{1, 1, 22, 0, 2.5f, 0, 0},
                      P{1, 1, 47, 95.9766f, 47, 20, 2}}) {
        const u64 pf = format(r.limit ? 0x2000 : 0);
        ASSERT_EQ(call("GdipSetStringFormatTrimming", {I(pf), I(static_cast<u32>(r.trim))}), Status::Ok);
        const std::string what = "partial trim=" + std::to_string(r.trim) + " limit=" + std::to_string(r.limit) +
                                 " h=" + std::to_string(r.h);
        expect_box(measure(g, para, f, {0, 0, 100, r.h}, pf), 0, 0, r.bw, r.bh, r.cp, r.lines, what.c_str());
    }
    // Hyphen break (probe "hyphen narrow").
    expect_box(measure(g, u"well-known words", f, {0, 0, 70, 0}, fmt), 0, 0, 66.2012f, 71.4941f, 16, 3, "hyphen");
}

TEST_P(Gdip_text_Test, MeasureStringAlignment) {
    const u64 g = graphics();
    const u64 f = font_named(u"Arial", 20);
    const float xs[3] = {10, 83.1950f, 156.3900f}, ys[3] = {5, 42.5781f, 80.1562f};
    const float zx[3] = {50, 23.1950f, -3.6100f}, zy[3] = {50, 37.5781f, 25.1562f};
    for (i32 a = 0; a < 3; ++a) {
        for (i32 l = 0; l < 3; ++l) {
            const u64 fmt = format();
            ASSERT_EQ(call("GdipSetStringFormatAlign", {I(fmt), I(static_cast<u32>(a))}), Status::Ok);
            ASSERT_EQ(call("GdipSetStringFormatLineAlign", {I(fmt), I(static_cast<u32>(l))}), Status::Ok);
            const std::string what = "align " + std::to_string(a) + "," + std::to_string(l);
            expect_box(measure(g, u"Hello", f, {10, 5, 200, 100}, fmt), xs[a], ys[l], 53.61f, 24.8438f, 5, 1,
                       what.c_str());
            expect_box(measure(g, u"Hello", f, {50, 50, 0, 0}, fmt), zx[a], zy[l], 53.61f, 24.8438f, 5, 1,
                       what.c_str());
        }
    }
    // The full layout path kerns and pads by 341/2048 em; a lone plain line
    // does neither (gdiplus_text_probe "World wide" vs "Hello\nWorld wide").
    const u64 near = format();
    expect_box(measure(g, u"World wide", f, {100, 0, 0, 0}, near), 100, 0, 108.5417f, 24.8438f, 10, 1, "kern simple");
    expect_box(measure(g, u"Wo\nWo", f, {100, 0, 0, 0}, near), 100, 0, 37.1875f, 48.4961f, 5, 2, "kern complex");
    expect_box(measure(g, u"ab\nab", f, {100, 0, 0, 0}, near), 100, 0, 29.5703f, 48.4961f, 5, 2, "pad complex");
    expect_box(measure(g, u"ab\nabc", f, {100, 0, 0, 0}, near), 100, 0, 39.8730f, 48.4961f, 6, 2, "ab abc");
    // Multi-line: the box is the widest line, aligned (probe "multiline").
    for (i32 a = 1; a <= 2; ++a) {
        const u64 fmt = format();
        ASSERT_EQ(call("GdipSetStringFormatAlign", {I(fmt), I(static_cast<u32>(a))}), Status::Ok);
        expect_box(measure(g, u"The quick brown fox jumps", f, {10, 0, 150, 0}, fmt), a == 1 ? 37.0117f : 64.0234f,
                   0, 95.9766f, 71.4941f, 25, 3, "multiline");
        expect_box(measure(g, u"Hello\nWorld wide", f, {100, 0, 0, 0}, fmt), a == 1 ? 45.9180f : -8.1641f, 0,
                   108.1641f, 48.4961f, 16, 2, "multiline zero");
    }
}

TEST_P(Gdip_text_Test, MeasureStringHotkeysAndTabs) {
    const u64 g = graphics();
    const u64 f = font_named(u"Arial", 20);
    const float widths[3] = {97.1159f, 69.6289f, 69.6289f};
    const float heights[3] = {24.8438f, 25.4980f, 25.4980f};
    for (i32 h = 0; h < 3; ++h) {
        const u64 fmt = format();
        ASSERT_EQ(call("GdipSetStringFormatHotkeyPrefix", {I(fmt), I(static_cast<u32>(h))}), Status::Ok);
        expect_box(measure(g, u"&File &&x", f, {0, 0, 0, 0}, fmt), 0, 0, widths[h], heights[h], 9, 1, "hotkey");
    }
    // Tab stops (probe "tabpos setN": widths of "a\tb\tc\td\te", NoClip).
    struct Tab { float first; std::initializer_list<float> tabs; bool typo; float width; };
    const Tab sets[] = {
        {0, {}, false, 62.8223f},      {0, {}, true, 54.5312f},     {0, {50, 30}, false, 168.1152f},
        {20, {50, 30}, false, 168.1152f}, {0, {25}, false, 118.1152f}, {0, {50, 30}, true, 161.1230f},
        {15, {40}, true, 171.1230f},  {0, {10, 0, 30}, false, 138.1152f},
    };
    for (const Tab& t : sets) {
        u64 fmt = 0;
        if (t.typo) {
            ASSERT_EQ(call("GdipCloneStringFormat", {I(typographic()), I(OUT)}), Status::Ok);
            fmt = read_handle();
        } else {
            fmt = format();
        }
        if (t.tabs.size() != 0) {
            write_reals(BUF + 0x400, t.tabs);
            ASSERT_EQ(call("GdipSetStringFormatTabStops",
                           {I(fmt), R(t.first), I(static_cast<u32>(t.tabs.size())), I(BUF + 0x400)}),
                      Status::Ok);
        }
        ASSERT_EQ(call("GdipSetStringFormatFlags", {I(fmt), I(0x4000)}), Status::Ok);
        const Measured m = measure(g, u"a\tb\tc\td\te", f, {0, 0, 0, 0}, fmt);
        EXPECT_NEAR(m.w, t.width, kTol) << t.first << " " << t.tabs.size() << " " << t.typo;
        EXPECT_EQ(m.cp, 9);
    }
}

TEST_P(Gdip_text_Test, MeasureStringErrors) {
    const u64 g = graphics();
    const u64 f = font_named(u"Arial", 20);
    write_wide(STR, u"a");
    write_reals(RECT, {0, 0, 0, 0});
    EXPECT_EQ(call("GdipMeasureString", {I(0), I(STR), I(1), I(f), I(RECT), I(0), I(BB), I(CP), I(LINES)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureString", {I(g), I(0), I(1), I(f), I(RECT), I(0), I(BB), I(CP), I(LINES)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureString", {I(g), I(STR), I(1), I(0), I(RECT), I(0), I(BB), I(CP), I(LINES)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureString", {I(g), I(STR), I(1), I(f), I(0), I(0), I(BB), I(CP), I(LINES)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureString", {I(g), I(STR), I(1), I(f), I(RECT), I(0), I(0), I(CP), I(LINES)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureString", {I(g), I(STR), I(static_cast<u32>(-2)), I(f), I(RECT), I(0), I(BB), I(CP),
                                         I(LINES)}),
              Status::OutOfMemory);
    // codepointsFitted / linesFilled are optional.
    ASSERT_EQ(call("GdipMeasureString", {I(g), I(STR), I(1), I(f), I(RECT), I(0), I(BB), I(0), I(0)}), Status::Ok);
    EXPECT_NEAR(read_real(BB + 8), 18.1217f, kTol);
}

// ---- MeasureCharacterRanges -------------------------------------------------------------

TEST_P(Gdip_text_Test, MeasureCharacterRanges) {
    const u64 g = graphics();
    const u64 f = font_named(u"Arial", 20);
    const u64 fmt = format();
    write_ints(BUF + 0x600, {0, 1, 2, 3, 6, 5, 4, 0});
    ASSERT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(fmt), I(4), I(BUF + 0x600)}), Status::Ok);
    std::vector<u64> regions;
    for (int i = 0; i < 5; ++i) regions.push_back(mgr.objects().add(gdip::GpRegion{}));
    const usize ptr = ptr_size();
    for (usize i = 0; i < regions.size(); ++i) {
        ASSERT_TRUE(memory.write_bytes(BUF + 0x700 + i * ptr, reinterpret_cast<const u8*>(&regions[i]), ptr).is_ok());
    }
    auto bounds = [&](usize i) {
        const gdip::RegionScans s = gdip::region_raster(*mgr.objects().get<gdip::GpRegion>(regions[i]), gdip::Affine{});
        i32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        s.bounds(x0, y0, x1, y1);
        return std::array<i32, 4>{x0, y0, x1 - x0, y1 - y0};
    };
    auto run = [&](const std::u16string& text, gdip::RectF r, u64 format_h, i32 count) {
        write_wide(STR, text);
        write_reals(RECT, {r.X, r.Y, r.Width, r.Height});
        return call("GdipMeasureCharacterRanges", {I(g), I(STR), I(static_cast<u32>(-1)), I(f), I(RECT), I(format_h),
                                                   I(static_cast<u32>(count)), I(BUF + 0x700)});
    };
    // probe "ranges st=0 [14,20,15,23] [40,20,21,23] [66,20,54,23] [0,0,0,0]".
    ASSERT_EQ(run(u"Hello World", {10, 20, 300, 100}, fmt, 4), Status::Ok);
    EXPECT_EQ(bounds(0), (std::array<i32, 4>{14, 20, 15, 23}));
    EXPECT_EQ(bounds(1), (std::array<i32, 4>{40, 20, 21, 23}));
    EXPECT_EQ(bounds(2), (std::array<i32, 4>{66, 20, 54, 23}));
    EXPECT_EQ(bounds(3), (std::array<i32, 4>{0, 0, 0, 0}));
    // Wrapped in 60 px: "World" spans lines 2 and 3.
    ASSERT_EQ(run(u"Hello World", {0, 0, 60, 100}, fmt, 4), Status::Ok);
    EXPECT_EQ(bounds(0), (std::array<i32, 4>{4, 0, 15, 23}));
    EXPECT_EQ(bounds(1), (std::array<i32, 4>{30, 0, 21, 23}));
    EXPECT_EQ(bounds(2), (std::array<i32, 4>{4, 23, 42, 46}));
    EXPECT_EQ(run(u"Hello World", {10, 20, 300, 100}, typographic(), 4), Status::Ok);  // no ranges
    EXPECT_EQ(run(u"Hello World", {10, 20, 300, 100}, fmt, 3), Status::InvalidParameter);
    EXPECT_EQ(run(u"Hello World", {10, 20, 300, 100}, fmt, 5), Status::Ok);
    EXPECT_EQ(run(u"Hello", {10, 20, 300, 100}, fmt, 4), Status::InvalidParameter);  // range past the text
    EXPECT_EQ(run(u"Hello World", {10, 20, 300, 100}, 0, 4), Status::InvalidParameter);
    // A zero layout rectangle clips every range away (probe "ranges multiline zero-rect").
    write_ints(BUF + 0x600, {0, 2, 3, 2});
    ASSERT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(fmt), I(2), I(BUF + 0x600)}), Status::Ok);
    ASSERT_EQ(run(u"ab\ncd", {5, 5, 0, 0}, fmt, 2), Status::Ok);
    EXPECT_EQ(bounds(0), (std::array<i32, 4>{0, 0, 0, 0}));
    EXPECT_EQ(bounds(1), (std::array<i32, 4>{0, 0, 0, 0}));
    // Tab positions (probe "tabpos set2": NoClip, stops 50, 30 -> 54, 84, 124, 154).
    const u64 tf = format(0x4000);
    write_reals(BUF + 0x400, {50, 30});
    ASSERT_EQ(call("GdipSetStringFormatTabStops", {I(tf), R(0), I(2), I(BUF + 0x400)}), Status::Ok);
    write_ints(BUF + 0x600, {0, 1, 2, 1, 4, 1, 6, 1, 8, 1});
    ASSERT_EQ(call("GdipSetStringFormatMeasurableCharacterRanges", {I(tf), I(5), I(BUF + 0x600)}), Status::Ok);
    ASSERT_EQ(run(u"a\tb\tc\td\te", {0, 0, 0, 0}, tf, 5), Status::Ok);
    const i32 starts[5] = {3, 54, 84, 124, 154}, ends[5] = {15, 65, 94, 135, 165};
    for (usize i = 0; i < 5; ++i) {
        const auto b = bounds(i);
        // The first cell starts one pixel right of Windows' here (see
        // .docs/contracts/gdiplus/text.md, "zero-rectangle range start").
        if (i == 0) EXPECT_NEAR(b[0], starts[i], 1) << i; else EXPECT_EQ(b[0], starts[i]) << i;
        EXPECT_EQ(b[0] + b[2], ends[i]) << i;
    }
}

// ---- Driver strings ----------------------------------------------------------------------

TEST_P(Gdip_text_Test, MeasureDriverString) {
    const u64 g = graphics();
    const u64 f = font_named(u"Arial", 20);
    write_ints(BUF + 0x400, {0});
    ASSERT_TRUE(memory.write_u16(BUF + 0x400, 'H').is_ok());
    ASSERT_TRUE(memory.write_u16(BUF + 0x402, 'i').is_ok());
    ASSERT_TRUE(memory.write_u16(BUF + 0x404, '!').is_ok());
    ASSERT_TRUE(memory.write_u16(BUF + 0x406, 0).is_ok());
    write_reals(BUF + 0x440, {10, 30, 30, 30, 40, 35});
    struct D { i32 flags; float x, y, w, h; };
    // probe "driver measure flags=N" (cmap lookup; vertical with cmap).
    for (const D d : {D{1, 10, 11.8945f, 35.5566f, 27.3438f}, D{5, 10, 11.8945f, 24.4434f, 22.3438f},
                      D{9, 10, 11.8945f, 35.5566f, 27.3438f}, D{13, 10, 11.8945f, 24.4434f, 22.3438f},
                      D{3, -1.1719f, 30, 52.3438f, 14.4434f}, D{7, -1.1719f, 30, 22.3438f, 24.4434f}}) {
        poison(BB, 16);
        ASSERT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x400), I(3), I(f), I(BUF + 0x440),
                                                   I(static_cast<u32>(d.flags)), I(0), I(BB)}),
                  Status::Ok);
        EXPECT_NEAR(read_real(BB), d.x, kTol) << d.flags;
        EXPECT_NEAR(read_real(BB + 4), d.y, kTol) << d.flags;
        EXPECT_NEAR(read_real(BB + 8), d.w, kTol) << d.flags;
        EXPECT_NEAR(read_real(BB + 12), d.h, kTol) << d.flags;
    }
    // Glyph indices: Arial's H, i, ! are glyphs 43, 76, 4.
    ASSERT_TRUE(memory.write_u16(BUF + 0x410, 43).is_ok());
    ASSERT_TRUE(memory.write_u16(BUF + 0x412, 76).is_ok());
    ASSERT_TRUE(memory.write_u16(BUF + 0x414, 4).is_ok());
    ASSERT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x410), I(3), I(f), I(BUF + 0x440), I(0), I(0), I(BB)}),
              Status::Ok);
    EXPECT_NEAR(read_real(BB + 8), 35.5566f, kTol);
    // Length -1 is NUL-terminated; 0 is an empty box; flags are not validated.
    ASSERT_EQ(call("GdipMeasureDriverString",
                   {I(g), I(BUF + 0x400), I(static_cast<u32>(-1)), I(f), I(BUF + 0x440), I(5), I(0), I(BB)}),
              Status::Ok);
    EXPECT_NEAR(read_real(BB + 8), 24.4434f, kTol);
    ASSERT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x400), I(0), I(f), I(BUF + 0x440), I(1), I(0), I(BB)}),
              Status::Ok);
    EXPECT_EQ(read_real(BB + 8), 0.0f);
    EXPECT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x400), I(2), I(f), I(BUF + 0x440), I(16), I(0), I(BB)}),
              Status::Ok);
    EXPECT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x400), I(2), I(f), I(0), I(1), I(0), I(BB)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x400), I(2), I(f), I(BUF + 0x440), I(1), I(0), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureDriverString", {I(g), I(0), I(2), I(f), I(BUF + 0x440), I(1), I(0), I(BB)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureDriverString", {I(g), I(BUF + 0x400), I(2), I(0), I(BUF + 0x440), I(1), I(0), I(BB)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipMeasureDriverString", {I(0), I(BUF + 0x400), I(2), I(f), I(BUF + 0x440), I(1), I(0), I(BB)}),
              Status::InvalidParameter);
    // DrawDriverString: NULL brush refused; flags not validated; length 0 Ok.
    const u64 brush = mgr.objects().add(gdip::GpBrush{});
    EXPECT_EQ(call("GdipDrawDriverString", {I(g), I(BUF + 0x400), I(2), I(f), I(0), I(BUF + 0x440), I(1), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawDriverString", {I(g), I(BUF + 0x400), I(2), I(f), I(brush), I(BUF + 0x440), I(16), I(0)}),
              Status::Ok);
    EXPECT_EQ(call("GdipDrawDriverString", {I(g), I(BUF + 0x400), I(0), I(f), I(brush), I(BUF + 0x440), I(1), I(0)}),
              Status::Ok);
}

// ---- AddPathString --------------------------------------------------------------------------

TEST_P(Gdip_text_Test, AddPathString) {
    const u64 arial = family(u"Arial");
    auto path = [&] { return mgr.objects().add(gdip::GpPath{}); };
    // probe "pathstring '0'": "I", em 100 at (10, 20), NULL format.
    const u64 p = path();
    write_wide(STR, u"I");
    write_reals(RECT, {10, 20, 0, 0});
    ASSERT_EQ(call("GdipAddPathString", {I(p), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(100), I(RECT), I(0)}),
              Status::Ok);
    const gdip::GpPath& gp = *mgr.objects().get<gdip::GpPath>(p);
    ASSERT_EQ(gp.points.size(), 4u);
    EXPECT_EQ(gp.types, (std::vector<u8>{0x00, 0x01, 0x01, 0xA1}));
    const float want[4][2] = {{35.977f, 110.527f}, {35.977f, 38.945f}, {45.449f, 38.945f}, {45.449f, 110.527f}};
    // The four corners in some rotation: compare as a set.
    for (const auto& w : want) {
        bool hit = false;
        for (const auto& pt : gp.points) hit |= std::fabs(pt.X - w[0]) < 0.02f && std::fabs(pt.Y - w[1]) < 0.02f;
        EXPECT_TRUE(hit) << w[0] << "," << w[1];
    }
    EXPECT_EQ(gp.fill_mode, 0);  // the path's fill mode is left alone
    // "Hi": a marker after each glyph (H: one contour; i: two).
    const u64 hi = path();
    write_wide(STR, u"Hi");
    ASSERT_EQ(call("GdipAddPathString", {I(hi), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(100), I(RECT), I(0)}),
              Status::Ok);
    const auto& types = mgr.objects().get<gdip::GpPath>(hi)->types;
    ASSERT_EQ(types.size(), 20u);
    EXPECT_EQ(types[11], 0xA1);
    EXPECT_EQ(types[15], 0x81);
    EXPECT_EQ(types[19], 0xA1);
    // Typographic: no padding (probe "pathstring typo I": x 9.326).
    const u64 pt = path();
    write_wide(STR, u"I");
    write_reals(RECT, {0, 0, 0, 0});
    ASSERT_EQ(call("GdipAddPathString",
                   {I(pt), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(100), I(RECT), I(typographic())}),
              Status::Ok);
    const gdip::RectF b = gdip::path_bounds(*mgr.objects().get<gdip::GpPath>(pt), nullptr);
    EXPECT_NEAR(b.X, 9.326f, 0.02f);
    EXPECT_NEAR(b.Y, 18.945f, 0.02f);
    EXPECT_NEAR(b.Width, 9.473f, 0.02f);
    EXPECT_NEAR(b.Height, 71.582f, 0.02f);
    // AddPathStringI: integer rectangle, bold face (probe: x 13.418, w 7.227).
    const u64 pi = path();
    write_ints(RECT, {10, 20, 0, 0});
    ASSERT_EQ(call("GdipAddPathStringI",
                   {I(pi), I(STR), I(static_cast<u32>(-1)), I(arial), I(1), R(50), I(RECT), I(typographic())}),
              Status::Ok);
    const gdip::RectF bi = gdip::path_bounds(*mgr.objects().get<gdip::GpPath>(pi), nullptr);
    EXPECT_NEAR(bi.X, 13.418f, 0.05f);
    EXPECT_NEAR(bi.Width, 7.227f, 0.05f);
    EXPECT_NEAR(bi.Y, 29.473f, 0.05f);
    // Empty and space-only strings add nothing.
    const u64 pe = path();
    write_wide(STR, u" ");
    write_reals(RECT, {0, 0, 0, 0});
    ASSERT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(10), I(RECT), I(0)}),
              Status::Ok);
    EXPECT_TRUE(mgr.objects().get<gdip::GpPath>(pe)->points.empty());
    // Errors (probe "err 50..58").
    EXPECT_EQ(call("GdipAddPathString", {I(0), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(10), I(RECT), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(0), I(static_cast<u32>(-1)), I(arial), I(0), R(10), I(RECT), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(static_cast<u32>(-1)), I(0), I(0), R(10), I(RECT), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(10), I(0), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(static_cast<u32>(-1)), I(arial), I(0), R(0), I(RECT), I(0)}),
              Status::GenericError);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(0), I(arial), I(0), R(10), I(RECT), I(0)}), Status::Ok);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(static_cast<u32>(-2)), I(arial), I(0), R(10), I(RECT), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipAddPathString", {I(pe), I(STR), I(1), I(arial), I(16), R(10), I(RECT), I(0)}), Status::Ok);
}

// ---- DrawString -----------------------------------------------------------------------------

TEST_P(Gdip_text_Test, DrawStringPlacesGlyphs) {
    const u64 f = font_named(u"Arial", 40);
    gdip::GpBrush black;
    black.color = 0xFF000000;
    const u64 brush = mgr.objects().add(black);
    struct Ink { i32 count{0}, x0{9999}, y0{9999}, x1{-1}, y1{-1}; };
    auto ink = [&](u64) {
        Ink k;
        const auto& bits = mgr.objects().get<gdip::GpImage>(image)->bits;
        for (i32 y = 0; y < 60; ++y) {
            for (i32 x = 0; x < 200; ++x) {
                const u8 b = bits[(static_cast<usize>(y) * 200 + static_cast<usize>(x)) * 4];
                if (b < 128) {
                    ++k.count;
                    k.x0 = std::min(k.x0, x);
                    k.y0 = std::min(k.y0, y);
                    k.x1 = std::max(k.x1, x);
                    k.y1 = std::max(k.y1, y);
                }
            }
        }
        return k;
    };
    auto draw = [&](u64 g, const std::u16string& text, gdip::RectF r, u64 fmt) {
        write_wide(STR, text);
        write_reals(RECT, {r.X, r.Y, r.Width, r.Height});
        return call("GdipDrawString", {I(g), I(STR), I(static_cast<u32>(-1)), I(f), I(RECT), I(fmt), I(brush)});
    };
    // probe "draw I hint0": ink 20..23 x 12..40 (1/6 em pad, cell-ascent baseline).
    u64 g = graphics(200, 60);
    ASSERT_EQ(draw(g, u"I", {10, 5, 0, 0}, 0), Status::Ok);
    Ink k = ink(g);
    EXPECT_NEAR(k.x0, 20, 1);
    EXPECT_NEAR(k.x1, 23, 1);
    EXPECT_NEAR(k.y0, 12, 1);
    EXPECT_NEAR(k.y1, 40, 1);
    // Typographic: no pad (14..16).
    g = graphics(200, 60);
    ASSERT_EQ(draw(g, u"I", {10, 5, 0, 0}, typographic()), Status::Ok);
    k = ink(g);
    EXPECT_NEAR(k.x0, 14, 1);
    EXPECT_NEAR(k.x1, 16, 1);
    // Clipped to the layout rectangle unless NoClip.
    g = graphics(200, 60);
    ASSERT_EQ(draw(g, u"IIII", {10, 5, 30, 20}, 0), Status::Ok);
    k = ink(g);
    EXPECT_NEAR(k.y1, 24, 1);
    EXPECT_LE(k.x1, 39);
    g = graphics(200, 60);
    ASSERT_EQ(draw(g, u"IIII", {10, 5, 30, 20}, format(0x4000)), Status::Ok);
    k = ink(g);
    EXPECT_NEAR(k.y1, 40, 1);
    // Underline: across the advance, below the baseline (probe 10..21 x ..46).
    const u64 ul = font_named(u"Arial", 40, 4);
    g = graphics(200, 60);
    write_wide(STR, u"I");
    write_reals(RECT, {10, 5, 0, 0});
    ASSERT_EQ(call("GdipDrawString", {I(g), I(STR), I(static_cast<u32>(-1)), I(ul), I(RECT), I(typographic()), I(brush)}),
              Status::Ok);
    k = ink(g);
    EXPECT_NEAR(k.x0, 10, 1);
    EXPECT_NEAR(k.x1, 21, 1);
    EXPECT_NEAR(k.y1, 46, 1);
    // DrawDriverString: glyphs at their baseline positions (probe "driver II": 24..66 x 16..44).
    g = graphics(200, 60);
    ASSERT_TRUE(memory.write_u16(BUF + 0x400, 'I').is_ok());
    ASSERT_TRUE(memory.write_u16(BUF + 0x402, 'I').is_ok());
    write_reals(BUF + 0x440, {20, 45, 60, 45});
    ASSERT_EQ(call("GdipDrawDriverString", {I(g), I(BUF + 0x400), I(2), I(f), I(brush), I(BUF + 0x440), I(1), I(0)}),
              Status::Ok);
    k = ink(g);
    EXPECT_NEAR(k.x0, 24, 1);
    EXPECT_NEAR(k.x1, 66, 1);
    EXPECT_NEAR(k.y0, 16, 1);
    EXPECT_NEAR(k.y1, 44, 1);
    // Errors (probe "err 59..65").
    write_wide(STR, u"a");
    write_reals(RECT, {0, 0, 0, 0});
    EXPECT_EQ(call("GdipDrawString", {I(0), I(STR), I(1), I(f), I(RECT), I(0), I(brush)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawString", {I(g), I(0), I(1), I(f), I(RECT), I(0), I(brush)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawString", {I(g), I(STR), I(1), I(0), I(RECT), I(0), I(brush)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawString", {I(g), I(STR), I(1), I(f), I(0), I(0), I(brush)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawString", {I(g), I(STR), I(1), I(f), I(RECT), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDrawString", {I(g), I(STR), I(0), I(f), I(RECT), I(0), I(brush)}), Status::Ok);
    EXPECT_EQ(call("GdipDrawString", {I(g), I(STR), I(static_cast<u32>(-2)), I(f), I(RECT), I(0), I(brush)}),
              Status::OutOfMemory);
}

INSTANTIATE_GDIP_BITNESS(Gdip_text_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
