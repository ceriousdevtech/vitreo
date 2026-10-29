// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_codec_effects.cpp
/// @brief Contract tests for the GDI+ codec area (effects, histograms), both
/// bitnesses. Expected values: tools/oracle_probe/gdiplus_codec_probe.expected
/// (x64) / .x86.expected ("effect.*", "hist*", "r2.*" lines).

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_codec.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>

#include <array>

namespace vitreo::winapi::gdip_test {
namespace {

using gdip::Guid;
using gdip::make_guid;

const Guid kBlur = make_guid(0x633c80a4, 0x1843, 0x482b, 0x9e, 0xf2, 0xbe, 0x28, 0x34, 0xc5, 0xfd, 0xd4);
const Guid kSharpen = make_guid(0x63cbf3ee, 0xc526, 0x402c, 0x8f, 0x71, 0x62, 0xc5, 0x40, 0xbf, 0x51, 0x42);
const Guid kColorMatrix = make_guid(0x718f2615, 0x7933, 0x40e3, 0xa5, 0x11, 0x5f, 0x68, 0xfe, 0x14, 0xdd, 0x74);
const Guid kColorLUT = make_guid(0xa7ce72a9, 0x0f7f, 0x40d7, 0xb3, 0xcc, 0xd0, 0xc0, 0x2d, 0x5c, 0x32, 0x12);
const Guid kBrightness = make_guid(0xd3a1dbe1, 0x8ec4, 0x4c17, 0x9f, 0x4c, 0xea, 0x97, 0xad, 0x1c, 0x34, 0x3d);
const Guid kHSL = make_guid(0x8b2dd6c3, 0xeb07, 0x4d87, 0xa5, 0xf0, 0x71, 0x08, 0xe2, 0x6a, 0x9c, 0x5f);
const Guid kLevels = make_guid(0x99c354ec, 0x2a31, 0x4f3a, 0x8c, 0x34, 0x17, 0xa8, 0x03, 0xb3, 0x3a, 0x25);
const Guid kTint = make_guid(0x1077af00, 0x2848, 0x4441, 0x94, 0x89, 0x44, 0xad, 0x4c, 0x2d, 0x7a, 0x2c);
const Guid kColorBalance = make_guid(0x537e597d, 0x251e, 0x48da, 0x96, 0x64, 0x29, 0xca, 0x49, 0x6b, 0x70, 0xf8);
const Guid kRedEye = make_guid(0x74d29d05, 0x69a4, 0x4266, 0x95, 0x49, 0x3c, 0xc5, 0x28, 0x36, 0xb6, 0x32);
const Guid kColorCurve = make_guid(0xdd6a0022, 0x58e4, 0x4a67, 0x9d, 0x9b, 0xd4, 0x8e, 0xb8, 0x81, 0xa5, 0x3d);

constexpr GuestAddr GUIDBUF = 0x10000 + 0x800;
constexpr GuestAddr PARAMS = 0x10000 + 0x2000;
constexpr GuestAddr AUXOUT = 0x10000 + 0x40;
constexpr GuestAddr AUXSIZE = 0x10000 + 0x50;
constexpr GuestAddr ROI = 0x10000 + 0x60;
constexpr GuestAddr HIST = 0x10000 + 0x4000;  // 4 x 1 KiB

class Gdip_codec_effects_Test : public GdipTest {
protected:
    void write_ptr_at(GuestAddr at, u64 v) {
        if (x64()) ASSERT_TRUE(memory.write_u64(at, v).is_ok());
        else ASSERT_TRUE(memory.write_u32(at, static_cast<u32>(v)).is_ok());
    }
    /// GdipCreateEffect with the GUID passed by value (x86: four dwords).
    Status create(const Guid& g, GuestAddr out = OUT) {
        if (x64()) {
            EXPECT_TRUE(memory.write_bytes(GUIDBUF, g.data(), 16).is_ok());
            return call("GdipCreateEffect", {I(GUIDBUF), I(out)});
        }
        u32 d[4];
        std::memcpy(d, g.data(), 16);
        return call("GdipCreateEffect", {I(d[0]), I(d[1]), I(d[2]), I(d[3]), I(out)});
    }
    u64 effect(const Guid& g) {
        EXPECT_EQ(create(g), Status::Ok);
        return read_handle();
    }
    Status set_ints(u64 e, std::initializer_list<i32> v) {
        write_ints(PARAMS, v);
        return call("GdipSetEffectParameters", {I(e), I(PARAMS), I(v.size() * 4)});
    }
    u32 psize(u64 e) {
        EXPECT_EQ(call("GdipGetEffectParameterSize", {I(e), I(OUT2)}), Status::Ok);
        return static_cast<u32>(read_int(OUT2));
    }
    /// The probe's 4x4 test image: (x+y)%4 -> red, green, blue, 50%-alpha white.
    u64 pattern(i32 format = gdip::PixelFormat32bppARGB) {
        gdip::GpImage img = gdip::make_bitmap(4, 4, format);
        static const u32 px[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0x80FFFFFFu};
        std::vector<u32> argb(16);
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) argb[static_cast<usize>(y * 4 + x)] = px[(x + y) % 4];
        EXPECT_TRUE(gdip::write_argb(marshaler, img, argb));
        return mgr.objects().add(std::move(img));
    }
    /// The probe's gradient: A=FF, R=80x, G=80y, B=30(x+y).
    u64 gradient() {
        gdip::GpImage img = gdip::make_bitmap(4, 4, gdip::PixelFormat32bppARGB);
        std::vector<u32> argb(16);
        for (u32 y = 0; y < 4; ++y)
            for (u32 x = 0; x < 4; ++x)
                argb[y * 4 + x] = 0xFF000000u | ((x * 80) << 16) | ((y * 80) << 8) | ((x + y) * 30);
        EXPECT_TRUE(gdip::write_argb(marshaler, img, argb));
        return mgr.objects().add(std::move(img));
    }
    std::vector<u32> pixels(u64 h) {
        std::vector<u32> argb;
        EXPECT_TRUE(gdip::read_argb(marshaler, *mgr.objects().get<gdip::GpImage>(h), argb));
        return argb;
    }
    Status apply(u64 bmp, u64 e, GuestAddr roi = 0, bool aux = false) {
        poison(AUXOUT, 8, 0);
        poison(AUXSIZE, 4, 0);
        return call("GdipBitmapApplyEffect", {I(bmp), I(e), I(roi), I(aux ? 1 : 0), I(AUXOUT), I(AUXSIZE)});
    }
    std::vector<u8> aux_bytes() {
        const u64 at = x64() ? memory.read_u64(AUXOUT).value() : memory.read_u32(AUXOUT).value();
        std::vector<u8> out(1024);
        EXPECT_NE(at, 0u);
        if (at != 0) EXPECT_TRUE(memory.read_bytes(at, out.data(), out.size()).is_ok());
        return out;
    }
};

TEST_P(Gdip_codec_effects_Test, CreateAndDefaults) {
    // effect.bogus: Win32Error and *effect = NULL; NULL out: InvalidParameter.
    poison(OUT);
    EXPECT_EQ(create(make_guid(0x12345678, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)), Status::Win32Error);
    EXPECT_EQ(read_handle(), 0u);
    EXPECT_EQ(create(kBlur, 0), Status::InvalidParameter);
    EXPECT_EQ(call("GdipDeleteEffect", {I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetEffectParameterSize", {I(0), I(OUT2)}), Status::InvalidParameter);

    struct Row { Guid g; u32 size; std::vector<u8> def; };
    const std::vector<Row> rows = {
        {kBlur, 8, {0, 0, 0x20, 0x41, 0, 0, 0, 0}},
        {kSharpen, 8, {0, 0, 0x20, 0x41, 0, 0, 0x48, 0x42}},
        {kColorMatrix, 100, {}},
        {kColorLUT, 1024, {}},
        {kBrightness, 8, {0, 0, 0, 0, 0, 0, 0, 0}},
        {kHSL, 12, std::vector<u8>(12, 0)},
        {kLevels, 12, {100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {kTint, 8, std::vector<u8>(8, 0)},
        {kColorBalance, 12, std::vector<u8>(12, 0)},
        {kRedEye, x64() ? 16u : 8u, {}},
        {kColorCurve, 12, {2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    };
    for (const Row& r : rows) {
        const u64 e = effect(r.g);
        EXPECT_EQ(psize(e), r.size);
        if (!r.def.empty()) {
            write_int(OUT3, static_cast<i32>(r.size));
            EXPECT_EQ(call("GdipGetEffectParameters", {I(e), I(OUT3), I(PARAMS)}), Status::Ok);
            std::vector<u8> got(r.size);
            ASSERT_TRUE(memory.read_bytes(PARAMS, got.data(), got.size()).is_ok());
            EXPECT_EQ(got, r.def);
        }
        // getsmall: InvalidParameter and *size untouched.
        write_int(OUT3, static_cast<i32>(r.size - 1));
        EXPECT_EQ(call("GdipGetEffectParameters", {I(e), I(OUT3), I(PARAMS)}), Status::InvalidParameter);
        EXPECT_EQ(read_int(OUT3), static_cast<i32>(r.size - 1));
        EXPECT_EQ(call("GdipDeleteEffect", {I(e)}), Status::Ok);
    }
}

TEST_P(Gdip_codec_effects_Test, SetSizeAndRanges) {
    const u64 b = effect(kBlur);
    write_reals(PARAMS, {2.0f});
    write_int(PARAMS + 4, 0);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(b), I(PARAMS), I(9)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(b), I(PARAMS), I(7)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(b), I(PARAMS), I(8)}), Status::Ok);
    write_real(PARAMS, 256.0f);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(b), I(PARAMS), I(8)}), Status::Ok);
    write_real(PARAMS, 300.0f);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(b), I(PARAMS), I(8)}), Status::InvalidParameter);
    write_real(PARAMS, -1.0f);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(b), I(PARAMS), I(8)}), Status::InvalidParameter);
    // A refused Set keeps the last good parameters (256.0).
    write_int(OUT3, 8);
    EXPECT_EQ(call("GdipGetEffectParameters", {I(b), I(OUT3), I(PARAMS)}), Status::Ok);
    EXPECT_FLOAT_EQ(read_real(PARAMS), 256.0f);

    const u64 s = effect(kSharpen);
    write_reals(PARAMS, {2.0f, 101.0f});
    EXPECT_EQ(call("GdipSetEffectParameters", {I(s), I(PARAMS), I(8)}), Status::InvalidParameter);
    write_reals(PARAMS, {2.0f, -1.0f});
    EXPECT_EQ(call("GdipSetEffectParameters", {I(s), I(PARAMS), I(8)}), Status::InvalidParameter);

    const u64 bc = effect(kBrightness);
    EXPECT_EQ(set_ints(bc, {300, 0}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(bc, {0, 150}), Status::InvalidParameter);
    const u64 lv = effect(kLevels);
    EXPECT_EQ(set_ints(lv, {101, 0, 0}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(lv, {50, 0, 60}), Status::Ok);  // shadow > highlight accepted
    EXPECT_EQ(set_ints(effect(kTint), {181, 0}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(effect(kHSL), {0, 0, 101}), Status::InvalidParameter);
    const u64 cc = effect(kColorCurve);
    EXPECT_EQ(set_ints(cc, {8, 0, 0}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(cc, {1, 0, 300}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(cc, {0, 0, 300}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(cc, {6, 1, -50}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(cc, {7, 1, -50}), Status::InvalidParameter);
    EXPECT_EQ(set_ints(cc, {2, 0, 40}), Status::Ok);
    write_int(OUT3, 12);
    EXPECT_EQ(call("GdipGetEffectParameters", {I(cc), I(OUT3), I(PARAMS)}), Status::Ok);
    EXPECT_EQ(read_int(PARAMS), 2);
    EXPECT_EQ(read_int(PARAMS + 8), 40);
}

TEST_P(Gdip_codec_effects_Test, ColorMatrixAndRoi) {
    const u64 e = effect(kColorMatrix);
    // Unset: nothing to read or apply (the real DLL faults; refused here).
    const u64 bmp = pattern();
    EXPECT_EQ(apply(bmp, e), Status::InvalidParameter);
    float mtx[25] = {};
    mtx[0] = 0.5f; mtx[6] = 1; mtx[12] = 1; mtx[18] = 1; mtx[24] = 1; mtx[20] = 0.1f;
    ASSERT_TRUE(memory.write_bytes(PARAMS, reinterpret_cast<const u8*>(mtx), sizeof mtx).is_ok());
    ASSERT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(100)}), Status::Ok);
    ASSERT_EQ(apply(bmp, e, 0, true), Status::Ok);
    // effect.colormatrix applyaux: no table.
    EXPECT_EQ(read_int(AUXSIZE), 0);
    const std::vector<u32> want = {0xff990000, 0xff1aff00, 0xff1a00ff, 0x8099ffff, 0xff1aff00, 0xff1a00ff,
                                   0x8099ffff, 0xff990000, 0xff1a00ff, 0x8099ffff, 0xff990000, 0xff1aff00,
                                   0x8099ffff, 0xff990000, 0xff1aff00, 0xff1a00ff};
    EXPECT_EQ(pixels(bmp), want);

    // effect24roi: RECT {1,1,3,3} is right/bottom exclusive.
    const u64 b24 = pattern(gdip::PixelFormat24bppRGB);
    write_ints(ROI, {1, 1, 3, 3});
    ASSERT_EQ(apply(b24, e, ROI), Status::Ok);
    const std::vector<u32> want24 = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff00ff00, 0xff1a00ff,
                                     0xff99ffff, 0xffff0000, 0xff0000ff, 0xff99ffff, 0xff990000, 0xff00ff00,
                                     0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff};
    EXPECT_EQ(pixels(b24), want24);
    // roi-outside / roi-empty succeed.
    write_ints(ROI, {-1, -1, 10, 10});
    EXPECT_EQ(apply(b24, e, ROI), Status::Ok);
    write_ints(ROI, {2, 2, 2, 2});
    EXPECT_EQ(apply(b24, e, ROI), Status::Ok);
    EXPECT_EQ(apply(0, e), Status::InvalidParameter);

    // createapply: whole image, 32bppARGB out, outputRect = 0,0,4,4.
    write_ptr_at(BUF, b24);
    poison(OUT3, 16, 0);
    ASSERT_EQ(call("GdipBitmapCreateApplyEffect",
                   {I(BUF), I(1), I(e), I(0), I(OUT3), I(OUT), I(0), I(0), I(0)}),
              Status::Ok);
    const u64 outb = read_handle();
    EXPECT_EQ(mgr.objects().get<gdip::GpImage>(outb)->pixel_format, gdip::PixelFormat32bppARGB);
    EXPECT_EQ(read_int(OUT3 + 8), 4);
    EXPECT_EQ(read_int(OUT3 + 12), 4);
    EXPECT_EQ(call("GdipBitmapCreateApplyEffect",
                   {I(BUF), I(2), I(e), I(0), I(OUT3), I(OUT), I(0), I(0), I(0)}),
              Status::InvalidParameter);
}

TEST_P(Gdip_codec_effects_Test, ColorLutPixelsAndAux) {
    const u64 e = effect(kColorLUT);
    std::vector<u8> lut(1024);
    for (int k = 0; k < 256; ++k) {
        lut[static_cast<usize>(k)] = static_cast<u8>(255 - k);
        lut[static_cast<usize>(256 + k)] = static_cast<u8>(k);
        lut[static_cast<usize>(512 + k)] = static_cast<u8>(k / 2);
        lut[static_cast<usize>(768 + k)] = static_cast<u8>(k);
    }
    ASSERT_TRUE(memory.write_bytes(PARAMS, lut.data(), lut.size()).is_ok());
    ASSERT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(1024)}), Status::Ok);
    const u64 bmp = pattern();
    ASSERT_EQ(apply(bmp, e, 0, true), Status::Ok);
    EXPECT_EQ(read_int(AUXSIZE), 1024);
    EXPECT_EQ(aux_bytes(), lut);
    const std::vector<u32> p = pixels(bmp);
    EXPECT_EQ(p[0], 0xff7f00ffu);
    EXPECT_EQ(p[1], 0xff00ffffu);
    EXPECT_EQ(p[2], 0xff000000u);
    EXPECT_EQ(p[3], 0x807fff00u);
    // createapply keeps the input format for a LUT effect.
    const u64 b24 = pattern(gdip::PixelFormat24bppRGB);
    write_ptr_at(BUF, b24);
    ASSERT_EQ(call("GdipBitmapCreateApplyEffect",
                   {I(BUF), I(1), I(e), I(0), I(0), I(OUT), I(0), I(0), I(0)}),
              Status::Ok);
    EXPECT_EQ(mgr.objects().get<gdip::GpImage>(read_handle())->pixel_format, gdip::PixelFormat24bppRGB);
}

TEST_P(Gdip_codec_effects_Test, LookupTablesMatchOracle) {
    auto table = [&](const Guid& g, std::initializer_list<i32> v) {
        const u64 e = effect(g);
        EXPECT_EQ(set_ints(e, v), Status::Ok);
        EXPECT_EQ(apply(gradient(), e, 0, true), Status::Ok);
        EXPECT_EQ(read_int(AUXSIZE), 1024);
        return aux_bytes();
    };
    auto at = [](const std::vector<u8>& t, int ch, std::initializer_list<int> idx) {
        std::vector<int> out;
        for (int i : idx) out.push_back(t[static_cast<usize>(ch * 256 + i)]);
        return out;
    };
    const std::initializer_list<int> step = {0, 32, 64, 96, 128, 160, 192, 224, 255};
    // effect.brightness applyaux (b=50, c=20): first entries.
    EXPECT_EQ(at(table(kBrightness, {50, 20}), 0, {0, 1, 2, 3, 4, 5, 9, 63}),
              (std::vector<int>{24, 26, 27, 28, 29, 31, 36, 103}));
    EXPECT_EQ(at(table(kBrightness, {-50, 0}), 0, step), (std::vector<int>{0, 0, 14, 46, 78, 110, 142, 174, 205}));
    EXPECT_EQ(at(table(kBrightness, {0, 50}), 0, step), (std::vector<int>{0, 0, 1, 65, 129, 193, 255, 255, 255}));
    EXPECT_EQ(at(table(kBrightness, {0, -50}), 2, step), (std::vector<int>{64, 80, 96, 112, 128, 144, 160, 176, 191}));
    EXPECT_EQ(at(table(kBrightness, {0, -100}), 1, step), (std::vector<int>(9, 128)));
    EXPECT_EQ(at(table(kBrightness, {0, 100}), 0, step), (std::vector<int>{0, 0, 0, 0, 255, 255, 255, 255, 255}));
    EXPECT_EQ(at(table(kLevels, {80, 0, 0}), 0, step), (std::vector<int>{0, 40, 80, 120, 160, 200, 240, 255, 255}));
    EXPECT_EQ(at(table(kLevels, {100, 0, 20}), 0, step), (std::vector<int>{0, 0, 16, 56, 96, 136, 176, 216, 255}));
    EXPECT_EQ(at(table(kLevels, {100, 50, 0}), 0, step), (std::vector<int>{0, 90, 128, 156, 181, 202, 221, 239, 255}));
    EXPECT_EQ(at(table(kLevels, {100, -50, 0}), 0, step), (std::vector<int>{0, 4, 16, 36, 64, 100, 145, 197, 255}));
    EXPECT_EQ(at(table(kLevels, {60, 30, 10}), 0, step), (std::vector<int>{0, 40, 121, 176, 222, 255, 255, 255, 255}));
    const auto cb = table(kColorBalance, {50, 0, -100});
    EXPECT_EQ(at(cb, 2, {0, 32, 96, 160, 192}), (std::vector<int>{0, 48, 144, 240, 255}));
    EXPECT_EQ(at(cb, 1, {32, 96}), (std::vector<int>{32, 96}));
    EXPECT_EQ(at(cb, 0, {32, 255}), (std::vector<int>{0, 0}));
    // ColorCurve: density, midtone, black saturation (exact), channel select.
    EXPECT_EQ(at(table(kColorCurve, {1, 0, 50}), 0, step), (std::vector<int>{50, 82, 114, 146, 178, 210, 242, 255, 255}));
    const auto dens_red = table(kColorCurve, {1, 1, -50});
    EXPECT_EQ(at(dens_red, 2, {0, 32, 64, 96}), (std::vector<int>{0, 0, 14, 46}));
    EXPECT_EQ(at(dens_red, 0, {32, 96}), (std::vector<int>{32, 96}));
    EXPECT_EQ(at(table(kColorCurve, {5, 0, 50}), 0, step), (std::vector<int>{0, 90, 128, 156, 181, 202, 221, 239, 255}));
    EXPECT_EQ(at(table(kColorCurve, {7, 0, 50}), 0, step), (std::vector<int>{0, 0, 17, 57, 97, 137, 177, 216, 255}));
    // HSL: hue on R, saturation on G, lightness on B; alpha identity.
    const auto hsl = table(kHSL, {90, 50, 50});
    EXPECT_EQ(at(hsl, 2, {0, 32, 192, 224}), (std::vector<int>{64, 96, 0, 32}));
    EXPECT_EQ(at(hsl, 1, {32, 64, 192}), (std::vector<int>{48, 96, 255}));
    EXPECT_EQ(at(hsl, 0, {0, 32, 128}), (std::vector<int>{128, 160, 255}));
    EXPECT_EQ(at(hsl, 3, {0, 77, 255}), (std::vector<int>{0, 77, 255}));
    EXPECT_EQ(at(table(kHSL, {-180, 0, -50}), 0, step), (std::vector<int>{0, 0, 0, 0, 1, 33, 65, 97, 128}));
}

TEST_P(Gdip_codec_effects_Test, HslPixels) {
    // r2.hsl 0,-100,0: grey = HSL lightness; r2.hsl 0,0,50 on black = 808080.
    const u64 e = effect(kHSL);
    ASSERT_EQ(set_ints(e, {0, -100, 0}), Status::Ok);
    u64 g = gradient();
    ASSERT_EQ(apply(g, e), Status::Ok);
    const auto p = pixels(g);
    EXPECT_EQ(p[0], 0xff000000u);
    EXPECT_EQ(p[1], 0xff282828u);
    EXPECT_EQ(p[2], 0xff505050u);
    EXPECT_EQ(p[5], 0xff464646u);
    ASSERT_EQ(set_ints(e, {0, 0, 50}), Status::Ok);
    g = gradient();
    ASSERT_EQ(apply(g, e), Status::Ok);
    EXPECT_EQ(pixels(g)[0], 0xff808080u);
}

TEST_P(Gdip_codec_effects_Test, BlurEdgesAndNoAux) {
    const u64 e = effect(kBlur);
    // The default (radius 10) applies without a Set (effect.blur-unset).
    EXPECT_EQ(apply(gradient(), e), Status::Ok);
    write_reals(PARAMS, {1.0f});
    write_int(PARAMS + 4, 0);
    ASSERT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(8)}), Status::Ok);
    u64 g = gradient();
    ASSERT_EQ(apply(g, e, 0, true), Status::Ok);
    EXPECT_EQ(read_int(AUXSIZE), 0);
    for (u32 c : pixels(g)) EXPECT_EQ(c >> 24, 0xFFu);   // r2.blur r=1: edges stay opaque
    write_int(PARAMS + 4, 1);
    ASSERT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(8)}), Status::Ok);
    g = gradient();
    ASSERT_EQ(apply(g, e), Status::Ok);
    const auto p = pixels(g);
    EXPECT_LT(p[0] >> 24, p[1] >> 24);                  // r2.blur r=1 expand: corner ca, edge e3
    EXPECT_LT(p[1] >> 24, 0xFFu);
    EXPECT_EQ(p[5] >> 24, 0xFFu);                        // interior ff
}

TEST_P(Gdip_codec_effects_Test, RedEyeParameters) {
    const u64 e = effect(kRedEye);
    const usize ptr = ptr_size();
    // Unset (zero areas): apply refused (effect.redeye apply).
    EXPECT_EQ(apply(pattern(), e), Status::InvalidParameter);
    const GuestAddr rects = BUF + 0x200;
    write_ints(rects, {0, 0, 3, 3});
    write_int(PARAMS, 1);
    write_ptr_at(PARAMS + ptr, rects);
    // sizeof(RedEyeCorrectionParams) alone is refused.
    EXPECT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(2 * ptr)}), Status::InvalidParameter);
    ASSERT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(2 * ptr + 16)}), Status::Ok);
    EXPECT_EQ(psize(e), 2 * ptr + 16);
    poison(BUF + 0x400, 64);
    write_int(OUT3, static_cast<i32>(2 * ptr + 16));
    ASSERT_EQ(call("GdipGetEffectParameters", {I(e), I(OUT3), I(BUF + 0x400)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0x400), 1);
    const u64 areas = ptr == 8 ? memory.read_u64(BUF + 0x400 + 8).value() : memory.read_u32(BUF + 0x400 + 4).value();
    EXPECT_EQ(areas, BUF + 0x400 + 2 * ptr);   // points into the returned tail
    EXPECT_EQ(read_int(BUF + 0x400 + 2 * ptr + 8), 3);
    // r2.redeye apply: the pattern (red touching the area edge) is unchanged.
    const u64 bmp = pattern();
    const auto before = pixels(bmp);
    EXPECT_EQ(apply(bmp, e), Status::Ok);
    EXPECT_EQ(pixels(bmp), before);
    write_int(PARAMS, 0);
    EXPECT_EQ(call("GdipSetEffectParameters", {I(e), I(PARAMS), I(2 * ptr)}), Status::InvalidParameter);
}

TEST_P(Gdip_codec_effects_Test, Histogram) {
    for (i32 f : {-1, 0, 3, 7, 8}) {
        poison(OUT2, 4);
        EXPECT_EQ(call("GdipBitmapGetHistogramSize", {I(static_cast<u32>(f)), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), 256);
    }
    EXPECT_EQ(call("GdipBitmapGetHistogramSize", {I(0), I(0)}), Status::InvalidParameter);
    const GuestAddr c0 = HIST, c1 = HIST + 1024, c2 = HIST + 2048, c3 = HIST + 3072;
    auto count = [&](GuestAddr ch, int i) { return read_int(ch + static_cast<u64>(i) * 4); };
    const u64 b = pattern();
    // hist f=0 (ARGB): A 128x4, 255x12; R/G/B 0x8, 255x8.
    poison(HIST, 4096, 0xFF);
    ASSERT_EQ(call("GdipBitmapGetHistogram", {I(b), I(0), I(256), I(c0), I(c1), I(c2), I(c3)}), Status::Ok);
    EXPECT_EQ(count(c0, 128), 4);
    EXPECT_EQ(count(c0, 255), 12);
    EXPECT_EQ(count(c0, 0), 0);
    EXPECT_EQ(count(c1, 0), 8);
    EXPECT_EQ(count(c3, 255), 8);
    // f=1 (PARGB): white premultiplied to 128.
    ASSERT_EQ(call("GdipBitmapGetHistogram", {I(b), I(1), I(256), I(c0), I(c1), I(c2), I(c3)}), Status::Ok);
    EXPECT_EQ(count(c1, 128), 4);
    EXPECT_EQ(count(c1, 255), 4);
    // Channel-pointer rules and entry count.
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(0), I(255), I(c0), I(c1), I(c2), I(c3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(0), I(257), I(c0), I(c1), I(c2), I(c3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(0), I(256), I(c0), I(0), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(0), I(256), I(c0), I(c1), I(c2), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(2), I(256), I(c0), I(c1), I(c2), I(c3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(3), I(256), I(c0), I(c1), I(c2), I(c3)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(b), I(8), I(256), I(c0), I(0), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(0), I(0), I(256), I(c0), I(c1), I(c2), I(c3)}), Status::InvalidParameter);

    // r2.hist on the gradient.
    const u64 g = gradient();
    ASSERT_EQ(call("GdipBitmapGetHistogram", {I(g), I(2), I(256), I(c0), I(c1), I(c2), I(0)}), Status::Ok);
    EXPECT_EQ(count(c0, 80), 4);
    EXPECT_EQ(count(c2, 90), 4);
    EXPECT_EQ(count(c2, 0), 1);
    ASSERT_EQ(call("GdipBitmapGetHistogram", {I(g), I(3), I(256), I(c0), I(0), I(0), I(0)}), Status::Ok);
    for (int v : {0, 19, 38, 58, 59, 79, 98, 117, 119, 138, 157, 176, 178, 197, 216, 236})
        EXPECT_EQ(count(c0, v), 1) << "gray " << v;
    ASSERT_EQ(call("GdipBitmapGetHistogram", {I(g), I(4), I(256), I(c0), I(0), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(count(c0, 90), 4);
    EXPECT_EQ(count(c0, 180), 1);
    ASSERT_EQ(call("GdipBitmapGetHistogram", {I(g), I(6), I(256), I(c0), I(0), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(count(c0, 240), 4);

    // hist 8bpp: indexed bitmaps are counted through their palette.
    const u64 p8 = mgr.objects().add(gdip::make_bitmap(4, 4, gdip::PixelFormat8bppIndexed));
    EXPECT_EQ(call("GdipBitmapGetHistogram", {I(p8), I(0), I(256), I(c0), I(c1), I(c2), I(c3)}), Status::Ok);
}

INSTANTIATE_GDIP_BITNESS(Gdip_codec_effects_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
