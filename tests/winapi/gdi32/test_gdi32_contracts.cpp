/// @file test_gdi32_contracts.cpp
/// @brief GDI object and raster contracts measured on Windows 11.
///
/// Expected values come from the committed Windows captures in
/// tools/oracle_probe (s3gdi_bitmap_probe, s3gdi_blit_probe,
/// s3gdi_info_probe, s3gdi_misc_probe), quoted next to each check or read
/// straight from the .expected file.

#include <gtest/gtest.h>
#include <winapi/graphics/gdi32/gdi32.hpp>
#include <winapi/graphics/gdi32/bitmap_codec.hpp>
#include <winapi/graphics/gdi32/bmp_decode.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kMem = 0x10000;
constexpr GuestAddr kStack = 0x30000;
constexpr GuestAddr kRet = 0xDEAD0000;

class GdiContractTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(kMem, 0x40000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(GdiManager::GDI_DIB_SCRATCH_BASE,
                                       GdiManager::GDI_DIB_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        mgr_.set_guest_memory(&memory_);
        exports_ = make_gdi32_exports(mgr_);
    }

    /// Call a gdi32 export with the given bitness; returns RAX.
    u64 call(const std::string& name, const std::vector<u64>& args, bool is64) {
        cpu_ = cpu::CpuState{};
        if (is64) {
            cpu_.mode = cpu::CpuMode::Long;
            cpu_.gpr.rsp = kStack;
            const u64 regs[4] = {args.size() > 0 ? args[0] : 0, args.size() > 1 ? args[1] : 0,
                                 args.size() > 2 ? args[2] : 0, args.size() > 3 ? args[3] : 0};
            cpu_.gpr.rcx = regs[0];
            cpu_.gpr.rdx = regs[1];
            cpu_.gpr.r8 = regs[2];
            cpu_.gpr.r9 = regs[3];
            (void)memory_.write_u64(kStack, kRet);
            for (usize i = 4; i < args.size(); ++i) {
                (void)memory_.write_u64(kStack + 0x28 + (i - 4) * 8, args[i]);
            }
        } else {
            cpu_.init_protected_mode();
            cpu_.set_sp(kStack);
            (void)memory_.write_u32(kStack, static_cast<u32>(kRet));
            for (usize i = 0; i < args.size(); ++i) {
                (void)memory_.write_u32(kStack + 4 + i * 4, static_cast<u32>(args[i]));
            }
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        auto it = std::find_if(exports_.entries.begin(), exports_.entries.end(),
                               [&](const auto& e) { return e.name == name; });
        EXPECT_NE(it, exports_.entries.end()) << name;
        if (it == exports_.entries.end()) return 0;
        it->handler(m);
        return is64 ? cpu_.gpr.rax : static_cast<u32>(cpu_.gpr.rax);
    }

    /// A 32bpp top-down DIB section selected into a new memory DC.
    struct Dib {
        Handle dc{0};
        Handle bmp{0};
        GuestAddr bits{0};
    };
    Dib make_dib32(i32 w, i32 h, GuestAddr bmi = kMem + 0x100) {
        (void)memory_.write_u32(bmi + 0, 40);
        (void)memory_.write_u32(bmi + 4, static_cast<u32>(w));
        (void)memory_.write_u32(bmi + 8, static_cast<u32>(-h));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, 32);
        (void)memory_.write_u32(bmi + 16, 0);
        (void)memory_.write_u32(bmi + 32, 0);
        Dib d;
        d.bmp = mgr_.create_dib_section(0, bmi, 0, kMem + 0x80, false);
        d.bits = memory_.read_u32(kMem + 0x80).value_or(0);
        d.dc = mgr_.create_compatible_dc(0);
        (void)mgr_.select_object(d.dc, d.bmp);
        return d;
    }
    void fill(const Dib& d, i32 count, u32 value) {
        for (i32 i = 0; i < count; ++i) (void)memory_.write_u32(d.bits + i * 4, value);
    }
    u32 px(const Dib& d, i32 i) { return memory_.read_u32(d.bits + i * 4).value_or(0xEEEEEEEE); }

    static std::vector<std::string> expected_lines(const char* file) {
        std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);) lines.push_back(line);
        return lines;
    }

    HandleTable handles_;
    GdiManager mgr_{handles_};
    GuestMemory memory_;
    cpu::CpuState cpu_;
    DllExports exports_;
};

TEST_F(GdiContractTest, CreateBitmapRoundsDepthAndAnswersBitmapByBitness) {
    // s3gdi_bitmap_probe "CreateBitmap formats": planes*bits rounds up to
    // 1/4/8/16/24/32, rows are WORD aligned, BITMAP is 32 bytes on x64 and 24
    // on x86 with bmBits NULL.
    struct Case { u32 planes, bits; u16 bpp; u32 width_bytes; };
    const Case cases[] = {{1, 0, 1, 2}, {1, 3, 4, 4}, {1, 5, 8, 6}, {1, 15, 16, 10},
                          {1, 17, 24, 16}, {1, 25, 32, 20}, {2, 1, 4, 4}, {3, 8, 24, 16},
                          {32, 1, 32, 20}};
    for (bool is64 : {true, false}) {
        for (const auto& c : cases) {
            const Handle b = mgr_.create_bitmap(5, 3, c.planes, c.bits, 0);
            ASSERT_NE(b, 0u);
            const u64 n = call("GetObjectW", {b, 64, kMem + 0x400}, is64);
            EXPECT_EQ(n, is64 ? 32u : 24u);
            EXPECT_EQ(memory_.read_u32(kMem + 0x404).value(), 5u);
            EXPECT_EQ(memory_.read_u32(kMem + 0x408).value(), 3u);
            EXPECT_EQ(memory_.read_u32(kMem + 0x40C).value(), c.width_bytes) << c.bits;
            EXPECT_EQ(memory_.read_u16(kMem + 0x412).value(), c.bpp) << c.bits;
            EXPECT_EQ(call("GetObjectW", {b, 0, 0}, is64), is64 ? 32u : 24u);
            EXPECT_TRUE(mgr_.delete_object(b));
        }
    }
    // More than 32 bits, or a negative size: NULL with ERROR_INVALID_PARAMETER.
    set_last_error(57005);
    EXPECT_EQ(call("CreateBitmap", {5, 3, 1, 33, 0}, true), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    set_last_error(57005);
    EXPECT_EQ(call("CreateBitmap", {5, 3, 33, 1, 0}, false), 0u);
    EXPECT_EQ(get_last_error(), 87u);
    EXPECT_EQ(call("CreateBitmap", {static_cast<u64>(-1) & 0xFFFFFFFF, 3, 1, 1, 0}, false), 0u);
    // Zero size answers the stock 1x1 monochrome bitmap every time.
    const Handle z = mgr_.create_bitmap(0, 3, 1, 32, 0);
    EXPECT_EQ(z, mgr_.get_stock_object(DEFAULT_BITMAP_ID));
    EXPECT_EQ(z, mgr_.create_bitmap(0, 0, 1, 1, 0));
    EXPECT_TRUE(mgr_.delete_object(z));
    EXPECT_EQ(mgr_.object_type(z), OBJ_BITMAP_TYPE);
}

TEST_F(GdiContractTest, BitmapBitsRoundTripKeepsPaddingAndReservedByte) {
    // s3gdi_bitmap_probe "mono8x2 gbb=4 bytes=a55a3cc3", "rgb32 3x2 gbb=24".
    const u8 mono[] = {0xA5, 0x5A, 0x3C, 0xC3};
    (void)memory_.write_bytes(kMem, mono, 4);
    const Handle m = mgr_.create_bitmap(8, 2, 1, 1, kMem);
    EXPECT_EQ(mgr_.get_bitmap_bits(m, 16, kMem + 0x100), 4);
    EXPECT_EQ(memory_.read_u32(kMem + 0x100).value(), 0xC33C5AA5u);
    EXPECT_EQ(mgr_.get_bitmap_bits(m, 0, kMem + 0x100), 0);
    EXPECT_EQ(mgr_.get_bitmap_bits(m, 100, 0), 4);
    const u8 rgb[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};
    (void)memory_.write_bytes(kMem, rgb, sizeof(rgb));
    const Handle c = mgr_.create_bitmap(3, 2, 1, 32, kMem);
    EXPECT_EQ(mgr_.get_bitmap_bits(c, 32, kMem + 0x200), 24);
    for (u32 i = 0; i < 24; ++i) EXPECT_EQ(memory_.read_u8(kMem + 0x200 + i).value(), rgb[i]);
    // SetBitmapBits answers the bytes used ("sbb32 cb4 ret=4", "cb100 ret=24").
    EXPECT_EQ(mgr_.set_bitmap_bits(c, 4, kMem), 4);
    EXPECT_EQ(mgr_.set_bitmap_bits(c, 100, kMem), 24);
    // A new bitmap is zero-filled ("initial contents").
    const Handle zero = mgr_.create_bitmap(2, 1, 1, 32, 0);
    EXPECT_EQ(mgr_.get_bitmap_bits(zero, 8, kMem + 0x300), 8);
    EXPECT_EQ(memory_.read_u64(kMem + 0x300).value(), 0u);
}

TEST_F(GdiContractTest, CompatibleBitmapFollowsTheDcsSelectedFormat) {
    const Handle screen = mgr_.get_dc(0);
    const Handle memdc = mgr_.create_compatible_dc(screen);
    // "ccb screen 4x2 ... bpp=32", "ccb fresh-memdc 4x2 ... wb=2,bpp=1".
    const Handle s = mgr_.create_compatible_bitmap(screen, 4, 2);
    const Handle m = mgr_.create_compatible_bitmap(memdc, 4, 2);
    EXPECT_EQ(call("GetObjectW", {s, 32, kMem}, true), 32u);
    EXPECT_EQ(memory_.read_u16(kMem + 18).value(), 32u);
    EXPECT_EQ(call("GetObjectW", {m, 32, kMem}, true), 32u);
    EXPECT_EQ(memory_.read_u16(kMem + 18).value(), 1u);
    EXPECT_EQ(memory_.read_u32(kMem + 12).value(), 2u);
    // "ccb NULL dc h=NULL err=57005".
    set_last_error(57005);
    EXPECT_EQ(call("CreateCompatibleBitmap", {0, 4, 2}, true), 0u);
    EXPECT_EQ(get_last_error(), 57005u);
    // "ccb dib-selected getobj(ds)=104 bpp=32 biH=5".
    const Dib d = make_dib32(4, 2);
    const Handle fromdib = mgr_.create_compatible_bitmap(d.dc, 3, 5);
    EXPECT_EQ(call("GetObjectW", {fromdib, 104, kMem}, true), 104u);
    EXPECT_EQ(memory_.read_u32(kMem + 32 + 8).value(), 5u);
    EXPECT_NE(memory_.read_u64(kMem + 24).value(), 0u);
}

TEST_F(GdiContractTest, DibSectionObjectLayoutIsBitnessSpecific) {
    // "dib32 cb=..." rows: BITMAP below sizeof(DIBSECTION), DIBSECTION at it,
    // 0 below sizeof(BITMAP).
    const Dib d = make_dib32(3, 2);
    struct Row { i32 cb; i32 x64; i32 x86; };
    const Row rows[] = {{4, 0, 0}, {20, 0, 0}, {24, 0, 24}, {28, 0, 24}, {32, 32, 24},
                        {84, 32, 84}, {100, 32, 84}, {104, 104, 84}, {108, 104, 84}};
    for (const auto& r : rows) {
        EXPECT_EQ(call("GetObjectW", {d.bmp, static_cast<u64>(r.cb), kMem + 0x400}, true),
                  static_cast<u64>(r.x64)) << r.cb;
        EXPECT_EQ(call("GetObjectW", {d.bmp, static_cast<u64>(r.cb), kMem + 0x400}, false),
                  static_cast<u64>(r.x86)) << r.cb;
    }
    // x64 DIBSECTION: bmBits at 24, dsBmih at 32 (positive height), size
    // image, dshSection at 88 and dsOffset at 96.
    ASSERT_EQ(call("GetObjectW", {d.bmp, 104, kMem + 0x400}, true), 104u);
    EXPECT_EQ(memory_.read_u64(kMem + 0x400 + 24).value(), d.bits);
    EXPECT_EQ(memory_.read_u32(kMem + 0x400 + 32).value(), 40u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x400 + 40).value(), 2u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x400 + 52).value(), 24u);
    EXPECT_EQ(memory_.read_u64(kMem + 0x400 + 88).value(), 0u);
    // x86: bmBits at 20, dshSection at 76.
    ASSERT_EQ(call("GetObjectW", {d.bmp, 84, kMem + 0x400}, false), 84u);
    EXPECT_EQ(memory_.read_u32(kMem + 0x400 + 20).value(), d.bits);
    EXPECT_EQ(memory_.read_u32(kMem + 0x400 + 24).value(), 40u);
}

TEST_F(GdiContractTest, GetDibitsQueryAndScanNumbering) {
    const Handle screen = mgr_.get_dc(0);
    const Handle mem = mgr_.create_compatible_dc(0);
    const Handle b = mgr_.create_compatible_bitmap(screen, 3, 2);
    (void)mgr_.select_object(mem, b);
    const u32 colors[6] = {0x302010, 0x0000FF, 0x00FF00, 0xFF0000, 0xFFFFFF, 0x808080};
    for (i32 i = 0; i < 6; ++i) (void)mgr_.set_pixel(mem, i % 3, i / 3, colors[i]);
    (void)mgr_.select_object(mem, mgr_.get_stock_object(DEFAULT_BITMAP_ID));
    const GuestAddr bmi = kMem + 0x100;
    auto header = [&](i32 w, i32 h, u16 bpp, u32 comp) {
        std::vector<u8> zero(40 + 256 * 4, 0);
        (void)memory_.write_bytes(bmi, zero.data(), zero.size());
        (void)memory_.write_u32(bmi, 40);
        (void)memory_.write_u32(bmi + 4, static_cast<u32>(w));
        (void)memory_.write_u32(bmi + 8, static_cast<u32>(h));
        (void)memory_.write_u16(bmi + 12, 1);
        (void)memory_.write_u16(bmi + 14, bpp);
        (void)memory_.write_u32(bmi + 16, comp);
    };
    // "query0 ret=1 hdr=3,2,1,32,3,img=24".
    header(0, 0, 0, 0);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 0, 0, bmi, 0), 1);
    EXPECT_EQ(memory_.read_u32(bmi + 4).value(), 3u);
    EXPECT_EQ(memory_.read_u32(bmi + 8).value(), 2u);
    EXPECT_EQ(memory_.read_u16(bmi + 14).value(), 32u);
    EXPECT_EQ(memory_.read_u32(bmi + 16).value(), 3u);
    EXPECT_EQ(memory_.read_u32(bmi + 20).value(), 24u);
    // "query16bf ret=1 img=16 masks=7c00,3e0,1f".
    header(3, 2, 16, 3);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 2, 0, bmi, 0), 1);
    EXPECT_EQ(memory_.read_u32(bmi + 40).value(), 0x7C00u);
    EXPECT_EQ(memory_.read_u32(bmi + 44).value(), 0x03E0u);
    EXPECT_EQ(memory_.read_u32(bmi + 48).value(), 0x001Fu);
    // "gdib32 bu": bottom row first.
    header(3, 2, 32, 0);
    const GuestAddr out = kMem + 0x800;
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 2, out, bmi, 0), 2);
    EXPECT_EQ(memory_.read_u32(out).value(), 0x000000FFu);   // ff000000 = blue BGRA
    EXPECT_EQ(memory_.read_u32(out + 12).value(), 0x00102030u);
    // "gdib32 td start1 ret=1": scan 1 counts from the bottom = the top row.
    header(3, -2, 32, 0);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 1, 1, out, bmi, 0), 1);
    EXPECT_EQ(memory_.read_u32(out).value(), 0x00102030u);
    // "gdib32 h4 ret=2": a taller DIB holds the bitmap at its top.
    header(3, 4, 32, 0);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 4, out, bmi, 0), 2);
    EXPECT_EQ(memory_.read_u32(out).value(), 0u);
    EXPECT_EQ(memory_.read_u32(out + 24).value(), 0x000000FFu);
    // "gdib16 bu": 555 truncation, 808080 -> 0x4210.
    header(3, 2, 16, 0);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 2, out, bmi, 0), 2);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x001Fu);
    EXPECT_EQ(memory_.read_u16(out + 4).value(), 0x4210u);
    EXPECT_EQ(memory_.read_u16(out + 8).value(), 0x0886u);
    // "gdib4 bu" (Euclidean nearest over the VGA table): cf 70 / 09 a0.
    header(3, 2, 4, 0);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 2, out, bmi, 0), 2);
    EXPECT_EQ(memory_.read_u16(out).value(), 0x70CFu);
    EXPECT_EQ(memory_.read_u16(out + 4).value(), 0xA009u);
    // "gdib7": unsupported depth fails; a NULL DC is ERROR_INVALID_PARAMETER.
    header(3, 2, 7, 0);
    EXPECT_EQ(mgr_.get_dib_bits(mem, b, 0, 2, out, bmi, 0), 0);
    u32 error = 0;
    header(3, 2, 32, 0);
    EXPECT_EQ(mgr_.get_dib_bits(0, b, 0, 2, out, bmi, 0, &error), 0);
    EXPECT_EQ(error, 87u);
}

TEST_F(GdiContractTest, ObjectStructsForPensBrushesFontsAndPalettes) {
    // LOGPEN 16 bytes, negative width stored as its magnitude.
    const Handle pen = mgr_.create_pen(0, -4, 0x030201);
    EXPECT_EQ(call("GetObjectW", {pen, 16, kMem}, true), 16u);
    EXPECT_EQ(memory_.read_u32(kMem + 4).value(), 4u);
    EXPECT_EQ(call("GetObjectW", {pen, 8, kMem}, true), 0u);  // "pen cb8 ret=0"
    // LOGBRUSH: 16 bytes on x64, 12 on x86; hatch keeps its index.
    const Handle hatch = mgr_.create_hatch_brush(4, 0x090807);
    EXPECT_EQ(call("GetObjectW", {hatch, 0, 0}, true), 16u);
    EXPECT_EQ(call("GetObjectW", {hatch, 0, 0}, false), 12u);
    EXPECT_EQ(call("GetObjectW", {hatch, 16, kMem}, true), 16u);
    EXPECT_EQ(memory_.read_u32(kMem).value(), 2u);
    EXPECT_EQ(memory_.read_u64(kMem + 8).value(), 4u);
    EXPECT_EQ(mgr_.create_hatch_brush(99, 0), 0u);  // "indirect-hatch99 h=NULL"
    // Stock objects from the probe.
    EXPECT_EQ(call("GetObjectW", {mgr_.get_stock_object(GRAY_BRUSH_ID), 16, kMem}, true), 16u);
    EXPECT_EQ(memory_.read_u32(kMem + 4).value(), 0x808080u);
    EXPECT_EQ(call("GetObjectW", {mgr_.get_stock_object(NULL_BRUSH_ID), 16, kMem}, true), 16u);
    EXPECT_EQ(memory_.read_u32(kMem).value(), 1u);  // BS_NULL
    EXPECT_EQ(call("GetObjectW", {mgr_.get_stock_object(WHITE_PEN_ID), 16, kMem}, true), 16u);
    EXPECT_EQ(memory_.read_u32(kMem + 4).value(), 0u);
    // Fonts: W 92 / A 60 for NULL, partial copies, 356 max
    // ("font cb30 ret=30", s3gdi_info_probe "big ret=356").
    GdiManager::FontInfo lf;
    lf.height = -13;
    lf.lf_weight = 600;
    lf.italic = true;
    lf.char_set = 1;
    lf.quality = 5;
    lf.pitch_and_family = 0x22;
    lf.face_name = u"Segoe UI";
    const Handle font = mgr_.create_font(lf);
    EXPECT_EQ(call("GetObjectW", {font, 0, 0}, true), 92u);
    EXPECT_EQ(call("GetObjectA", {font, 0, 0}, true), 60u);
    EXPECT_EQ(call("GetObjectW", {font, 30, kMem}, true), 30u);
    EXPECT_EQ(call("GetObjectW", {font, 108, kMem}, true), 108u);
    EXPECT_EQ(call("GetObjectW", {font, 512, kMem}, true), 356u);
    EXPECT_EQ(memory_.read_u32(kMem + 16).value(), 600u);
    EXPECT_EQ(memory_.read_u8(kMem + 27).value(), 0x22u);
    EXPECT_EQ(memory_.read_u16(kMem + 28).value(), u'S');
    EXPECT_EQ(memory_.read_u32(kMem + 348).value(), 0x08007664u);
    EXPECT_EQ(call("GetObjectA", {font, 60, kMem}, true), 60u);
    EXPECT_EQ(memory_.read_u8(kMem + 28).value(), u8{'S'});
    // Stock SYSTEM_FONT (s3gdi_info_probe "stockfont 13").
    EXPECT_EQ(call("GetObjectW", {mgr_.get_stock_object(SYSTEM_FONT_ID), 92, kMem}, true), 92u);
    EXPECT_EQ(memory_.read_u32(kMem).value(), 16u);
    EXPECT_EQ(memory_.read_u32(kMem + 16).value(), 700u);
    // Regions and DCs: 0 with ERROR_INVALID_HANDLE; unknown: error untouched.
    const Handle rgn = mgr_.create_rect_rgn(0, 0, 4, 4);
    set_last_error(57005);
    EXPECT_EQ(call("GetObjectW", {rgn, 32, kMem}, true), 0u);
    EXPECT_EQ(get_last_error(), 6u);
    set_last_error(57005);
    EXPECT_EQ(call("GetObjectW", {0x12345678, 32, kMem}, true), 0u);
    EXPECT_EQ(get_last_error(), 57005u);
    EXPECT_EQ(mgr_.object_type(mgr_.create_compatible_dc(0)), OBJ_MEMDC_TYPE);
    EXPECT_EQ(mgr_.object_type(mgr_.get_dc(0)), OBJ_DC_TYPE);
}

TEST_F(GdiContractTest, SelectObjectBitmapRulesAndDeferredDelete) {
    const Handle screen = mgr_.get_dc(0);
    const Handle m1 = mgr_.create_compatible_dc(0);
    const Handle m2 = mgr_.create_compatible_dc(0);
    const Handle b = mgr_.create_compatible_bitmap(screen, 2, 2);
    const Handle old = mgr_.select_object(m1, b);
    EXPECT_EQ(old, mgr_.get_stock_object(DEFAULT_BITMAP_ID));
    EXPECT_EQ(mgr_.select_object(m2, b), 0u);      // one DC at a time
    EXPECT_EQ(mgr_.select_object(screen, b), 0u);  // memory DCs only
    u32 error = 0;
    EXPECT_EQ(mgr_.select_object(0, b, &error), 0u);
    EXPECT_EQ(error, 6u);
    error = 0;
    EXPECT_EQ(mgr_.select_object(m1, mgr_.get_stock_object(DEFAULT_PALETTE_ID), &error), 0u);
    EXPECT_EQ(error, 1u);
    // A brush deleted while selected: TRUE, GetObject fails, freed on deselect.
    const Handle br = mgr_.create_solid_brush(0x123456);
    const Handle ob = mgr_.select_object(m1, br);
    EXPECT_TRUE(mgr_.delete_object(br));
    EXPECT_EQ(mgr_.get_object(br, 16, kMem, true), 0);
    EXPECT_EQ(mgr_.select_object(m1, ob), br);
    EXPECT_EQ(mgr_.object_type(br), 0u);
    // The DC's deletion releases its bitmap; a second DeleteDC fails.
    EXPECT_TRUE(mgr_.delete_dc(m1));
    EXPECT_FALSE(mgr_.delete_dc(m1));
    EXPECT_NE(mgr_.select_object(m2, b), 0u);
}

TEST_F(GdiContractTest, RasterOpsRunOnAllThirtyTwoBits) {
    // s3gdi_blit_probe "# ROP3 boolean on full 32 bits": S=0ff00ff0,
    // D=00ff00ff, brush 00ccaa33.
    const Dib dst = make_dib32(1, 1, kMem + 0x100);
    const Dib src = make_dib32(1, 1, kMem + 0x200);
    const Handle brush = mgr_.create_solid_brush(0x00CCAA33);
    (void)mgr_.select_object(dst.dc, brush);
    int checked = 0;
    for (const auto& line : expected_lines("s3gdi_blit_probe.expected")) {
        unsigned rop = 0, want = 0;
        int r = 0;
        if (std::sscanf(line.c_str(), "rop %x r=%d -> %x", &rop, &r, &want) != 3) continue;
        fill(src, 1, 0x0FF00FF0);
        fill(dst, 1, 0x00FF00FF);
        ASSERT_TRUE(mgr_.bit_blt(dst.dc, 0, 0, 1, 1, src.dc, 0, 0, rop));
        EXPECT_EQ(px(dst, 0), want) << std::hex << rop;
        ++checked;
    }
    EXPECT_EQ(checked, 35);
    // GDI drawing leaves 0 in the reserved byte ("FillRect row0", "SetPixel").
    fill(dst, 1, 0x80AABBCC);
    ASSERT_TRUE(mgr_.fill_rect_gdi(dst.dc, 0, 0, 1, 1, 0x0000FF));
    EXPECT_EQ(px(dst, 0), 0x00FF0000u);
    fill(dst, 1, 0x80AABBCC);
    EXPECT_EQ(mgr_.set_pixel(dst.dc, 0, 0, 0x030201), 0x030201u);
    EXPECT_EQ(px(dst, 0), 0x00010203u);
    // PatBlt refuses a ROP that reads a source ("PatBlt SRCCOPY(invalid)").
    EXPECT_FALSE(mgr_.pat_blt(dst.dc, 0, 0, 1, 1, 0x00CC0020));
}

TEST_F(GdiContractTest, MonochromeConversionUsesDcColours) {
    // "mono->color SRCCOPY text=112233 bk=445566": 0 bits -> text colour,
    // 1 bits -> background, reserved byte 0.
    const Handle screen = mgr_.get_dc(0);
    (void)screen;
    const u8 pat[] = {0xA5, 0x00};
    (void)memory_.write_bytes(kMem, pat, 2);
    const Handle mono = mgr_.create_bitmap(8, 1, 1, 1, kMem);
    const Handle mdc = mgr_.create_compatible_dc(0);
    (void)mgr_.select_object(mdc, mono);
    const Dib dst = make_dib32(8, 1);
    (void)mgr_.set_text_color(dst.dc, 0x332211);
    (void)mgr_.set_bk_color(dst.dc, 0x665544);
    ASSERT_TRUE(mgr_.bit_blt(dst.dc, 0, 0, 8, 1, mdc, 0, 0, 0x00CC0020));
    EXPECT_EQ(px(dst, 0), 0x00445566u);
    EXPECT_EQ(px(dst, 1), 0x00112233u);
    // Colour -> mono: 1 exactly where the pixel equals the source DC's
    // background, reserved byte included ("color->mono srcbk=ffffff 8000").
    const u32 src_px[8] = {0x00FFFFFF, 0x00102030, 0x80102030, 0x00808080,
                           0x007F7F7F, 0x00FF0000, 0xFFFFFFFF, 0x00FEFEFE};
    for (u32 i = 0; i < 8; ++i) (void)memory_.write_u32(dst.bits + i * 4, src_px[i]);
    (void)mgr_.set_bk_color(dst.dc, 0xFFFFFF);
    ASSERT_TRUE(mgr_.bit_blt(mdc, 0, 0, 8, 1, dst.dc, 0, 0, 0x00CC0020));
    EXPECT_EQ(mgr_.get_bitmap_bits(mono, 2, kMem + 0x10), 2);
    EXPECT_EQ(memory_.read_u8(kMem + 0x10).value(), 0x80u);
    // SetPixel on mono takes the nearer of black and white.
    EXPECT_EQ(mgr_.set_pixel(mdc, 0, 0, 0x808080), 0xFFFFFFu);
    EXPECT_EQ(mgr_.set_pixel(mdc, 0, 0, 0x7F7F7F), 0u);
}

TEST_F(GdiContractTest, AlphaBlendMatchesTheMeasuredRounding) {
    const Dib dst = make_dib32(1, 1, kMem + 0x100);
    const Dib src = make_dib32(1, 1, kMem + 0x200);
    int checked = 0;
    for (const auto& line : expected_lines("s3gdi_blit_probe.expected")) {
        int a = 0;
        unsigned s = 0, d = 0;
        if (std::sscanf(line.c_str(), "AB sweep a=%d src=%x dst=%x", &a, &s, &d) != 3) continue;
        fill(src, 1, s);
        fill(dst, 1, 0xC0336699);
        ASSERT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 0, 255, 1), 0u);
        EXPECT_EQ(px(dst, 0), d) << line;
        ++checked;
    }
    for (const auto& line : expected_lines("s3gdi_blit_probe.expected")) {
        int ca = 0;
        unsigned d = 0;
        if (std::sscanf(line.c_str(), "AB const sweep ca=%d dst=%x", &ca, &d) != 2) continue;
        fill(src, 1, 0x7F10E0F0);
        fill(dst, 1, 0xC0336699);
        ASSERT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 0,
                                           static_cast<u8>(ca), 0), 0u);
        EXPECT_EQ(px(dst, 0), d) << line;
        ++checked;
    }
    EXPECT_EQ(checked, 21);
    // Rejections: bad BlendOp/AlphaFormat, negative size, source overrun = 87.
    EXPECT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 1, 255, 1), 87u);
    EXPECT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, 1, 1, src.dc, 0, 0, 1, 1, 0, 255, 2), 87u);
    EXPECT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, -1, 1, src.dc, 0, 0, 1, 1, 0, 255, 1), 87u);
    EXPECT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, 1, 1, src.dc, 1, 0, 1, 1, 0, 255, 1), 87u);
    EXPECT_EQ(mgr_.alpha_blend_checked(dst.dc, 0, 0, 1, 1, 0xBAD, 0, 0, 1, 1, 0, 255, 1),
              0xFFFFFFFFu);
}

TEST_F(GdiContractTest, HatchBrushesPaintTheMeasuredPatterns) {
    // hatch probe: HS_DIAGCROSS rows, background colour between the lines.
    const Dib dst = make_dib32(8, 8);
    (void)mgr_.set_bk_color(dst.dc, 0x302010);
    const Handle h = mgr_.create_hatch_brush(5, 0x0000FF);
    ASSERT_TRUE(mgr_.fill_rect_with_brush(dst.dc, 0, 0, 8, 8, h));
    const char* rows[8] = {"X......X", ".X....X.", "..X..X..", "...XX...",
                           "...XX...", "..X..X..", ".X....X.", "X......X"};
    for (i32 y = 0; y < 8; ++y) {
        for (i32 x = 0; x < 8; ++x) {
            EXPECT_EQ(px(dst, y * 8 + x), rows[y][x] == 'X' ? 0x00FF0000u : 0x00102030u)
                << x << "," << y;
        }
    }
}

TEST_F(GdiContractTest, StretchSamplesPixelCentresAndCombinesWhenShrinking) {
    // "stretch down 8x4->3x2 mode=3" and "mode=1".
    const Dib src = make_dib32(8, 4, kMem + 0x100);
    const Dib dst = make_dib32(8, 4, kMem + 0x200);
    for (i32 i = 0; i < 32; ++i) {
        (void)memory_.write_u32(src.bits + i * 4,
                                0x00100000u * static_cast<u32>(i % 8) +
                                    0x00001000u * static_cast<u32>(i / 8) + 0x10);
    }
    EXPECT_EQ(mgr_.set_stretch_blt_mode(dst.dc, 3), 1);
    fill(dst, 32, 0x80AABBCC);
    ASSERT_TRUE(mgr_.stretch_blt(dst.dc, 0, 0, 3, 2, src.dc, 0, 0, 8, 4, 0x00CC0020));
    EXPECT_EQ(px(dst, 0), 0x00101010u);
    EXPECT_EQ(px(dst, 1), 0x00401010u);
    EXPECT_EQ(px(dst, 2), 0x00601010u);
    EXPECT_EQ(px(dst, 8), 0x00103010u);
    (void)mgr_.set_stretch_blt_mode(dst.dc, 1);
    fill(dst, 32, 0x80AABBCC);
    ASSERT_TRUE(mgr_.stretch_blt(dst.dc, 0, 0, 3, 2, src.dc, 0, 0, 8, 4, 0x00CC0020));
    EXPECT_EQ(px(dst, 0), 0x00000010u);
    EXPECT_EQ(px(dst, 2), 0x00400010u);
    EXPECT_EQ(px(dst, 8), 0x00002010u);
    // "stretch mirror x": dest (3,0,-3) covers x 1..3 mirrored.
    (void)mgr_.set_stretch_blt_mode(dst.dc, 3);
    fill(dst, 32, 0x80AABBCC);
    ASSERT_TRUE(mgr_.stretch_blt(dst.dc, 3, 0, -3, 1, src.dc, 0, 0, 3, 1, 0x00CC0020));
    EXPECT_EQ(px(dst, 0), 0x80AABBCCu);
    EXPECT_EQ(px(dst, 1), 0x00200010u);
    EXPECT_EQ(px(dst, 3), 0x00000010u);
    // BitBlt with a negative width draws nothing and succeeds.
    fill(dst, 32, 0x80AABBCC);
    EXPECT_TRUE(mgr_.bit_blt(dst.dc, 3, 0, -2, 1, src.dc, 0, 0, 0x00CC0020));
    EXPECT_EQ(px(dst, 2), 0x80AABBCCu);
}

TEST_F(GdiContractTest, GradientFillRectangleAndTriangle) {
    // s3gdi_blit_probe "GF rect H" and "GF triangle".
    const Dib dst = make_dib32(8, 4);
    const GuestAddr v = kMem + 0x400, mesh = kMem + 0x500;
    const u16 verts[3][6] = {{0, 0, 0xFF00, 0x0000, 0x0000, 0x8000},
                             {8, 4, 0x0000, 0x0000, 0xFF00, 0xFF00},
                             {0, 4, 0x0000, 0xFF00, 0x0000, 0x0000}};
    for (u32 i = 0; i < 3; ++i) {
        (void)memory_.write_u32(v + i * 16, verts[i][0]);
        (void)memory_.write_u32(v + i * 16 + 4, verts[i][1]);
        for (u32 c = 0; c < 4; ++c) (void)memory_.write_u16(v + i * 16 + 8 + c * 2, verts[i][2 + c]);
    }
    (void)memory_.write_u32(mesh, 0);
    (void)memory_.write_u32(mesh + 4, 1);
    (void)memory_.write_u32(mesh + 8, 2);
    fill(dst, 32, 0x80AABBCC);
    ASSERT_EQ(mgr_.gradient_fill(dst.dc, v, 2, mesh, 1, 0), 0u);
    EXPECT_EQ(px(dst, 0), 0x80FF0000u);
    EXPECT_EQ(px(dst, 1), 0x8FDF001Fu);
    EXPECT_EQ(px(dst, 7), 0xEF1F00DFu);
    fill(dst, 32, 0x80AABBCC);
    ASSERT_EQ(mgr_.gradient_fill(dst.dc, v, 3, mesh, 1, 2), 0u);
    const u32 want[4][8] = {
        {0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc},
        {0x60bf3f00, 0x7fbf1f1f, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc},
        {0x407f7f00, 0x5f7f5f1f, 0x7f7f3f3f, 0x9f7f1f5f, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc, 0x80aabbcc},
        {0x203fbf00, 0x3f3f9f1f, 0x5f3f7f3f, 0x7f3f5f5f, 0x9f3f3f7f, 0xbf3f1f9f, 0x80aabbcc, 0x80aabbcc}};
    for (i32 y = 0; y < 4; ++y) {
        for (i32 x = 0; x < 8; ++x) EXPECT_EQ(px(dst, y * 8 + x), want[y][x]) << x << "," << y;
    }
    EXPECT_EQ(mgr_.gradient_fill(dst.dc, v, 2, mesh, 1, 7), 87u);
    (void)memory_.write_u32(mesh + 4, 5);
    EXPECT_EQ(mgr_.gradient_fill(dst.dc, v, 2, mesh, 1, 0), 0xFFFFFFFFu);
}

// gdi32!GdiGradientFill is the entry point msimg32!GradientFill forwards to:
// same ABI, same result (s3gdi_blit_probe "GF rect H"), BOOL with the last
// error set on a bad mode. Both bitnesses.
TEST_F(GdiContractTest, GdiGradientFillExportMatchesGradientFill) {
    for (const bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        const Dib dst = make_dib32(8, 4);
        const GuestAddr v = kMem + 0x400, mesh = kMem + 0x500;
        const u16 verts[2][6] = {{0, 0, 0xFF00, 0x0000, 0x0000, 0x8000},
                                 {8, 4, 0x0000, 0x0000, 0xFF00, 0xFF00}};
        for (u32 i = 0; i < 2; ++i) {
            (void)memory_.write_u32(v + i * 16, verts[i][0]);
            (void)memory_.write_u32(v + i * 16 + 4, verts[i][1]);
            for (u32 c = 0; c < 4; ++c) (void)memory_.write_u16(v + i * 16 + 8 + c * 2, verts[i][2 + c]);
        }
        (void)memory_.write_u32(mesh, 0);
        (void)memory_.write_u32(mesh + 4, 1);
        fill(dst, 32, 0x80AABBCC);
        EXPECT_EQ(call("GdiGradientFill", {dst.dc, v, 2, mesh, 1, 0}, is64), 1u);
        EXPECT_EQ(px(dst, 0), 0x80FF0000u);
        EXPECT_EQ(px(dst, 1), 0x8FDF001Fu);
        EXPECT_EQ(px(dst, 7), 0xEF1F00DFu);
        set_last_error(0);
        EXPECT_EQ(call("GdiGradientFill", {dst.dc, v, 2, mesh, 1, 7}, is64), 0u);
        EXPECT_EQ(get_last_error(), 87u);
    }
}

TEST_F(GdiContractTest, SynthesisedColourTablesFollowTheirDefinitions) {
    // GetDIBits remarks (Learn): 1bpp is black and white, 4bpp "identical to
    // the standard VGA palette", 8bpp "a general mix of 256 colors defined by
    // GDI" that includes the 20 default-palette colours. The VGA colours are
    // HTML 3.2's sixteen colour names ("the standard 16 colors supported with
    // the Windows VGA palette").
    constexpr u32 kBlack = 0x000000, kSilver = 0xC0C0C0, kGray = 0x808080, kWhite = 0xFFFFFF,
                  kMaroon = 0x800000, kRed = 0xFF0000, kPurple = 0x800080, kFuchsia = 0xFF00FF,
                  kGreen = 0x008000, kLime = 0x00FF00, kOlive = 0x808000, kYellow = 0xFFFF00,
                  kNavy = 0x000080, kBlue = 0x0000FF, kTeal = 0x008080, kAqua = 0x00FFFF;
    const std::array<u32, 16> vga = {kBlack, kMaroon, kGreen, kOlive,  kNavy, kPurple, kTeal, kGray,
                                     kSilver, kRed,   kLime,  kYellow, kBlue, kFuchsia, kAqua, kWhite};
    EXPECT_EQ(gdi::kGdiDefaultPalette4, vga);

    // Where each VGA colour sits is measured (s3gdi_info_probe "paletteN
    // vga_index"; at 8bpp only the static first/last ten are searched).
    const std::vector<u32> pal1 = gdi::ddb_pixel_format(1).palette;
    const std::vector<u32> pal4(gdi::kGdiDefaultPalette4.begin(), gdi::kGdiDefaultPalette4.end());
    const std::vector<u32> pal8(gdi::kGdiDefaultPalette8.begin(), gdi::kGdiDefaultPalette8.end());
    usize checked = 0;
    for (const auto& line : expected_lines("s3gdi_info_probe.expected")) {
        if (line.rfind("palette", 0) != 0) continue;
        const int bpp = std::atoi(line.c_str() + 7);
        const std::vector<u32>& table = bpp == 1 ? pal1 : bpp == 4 ? pal4 : pal8;
        EXPECT_NE(line.find("entries=" + std::to_string(1u << bpp) + " "), std::string::npos) << line;
        EXPECT_EQ(table.size(), 1u << bpp);
        std::string rest = line.substr(line.find("vga_index:") + 10);
        char* p = rest.data();
        for (usize v = 0; v < 16; ++v) {
            char* end = nullptr;
            const long at = std::strtol(p, &end, 10);
            ASSERT_NE(end, p) << line;
            p = end;
            if (at < 0) {
                EXPECT_EQ(std::count(table.begin(), table.end(), vga[v]), 0) << bpp << "bpp vga " << v;
            } else {
                ASSERT_LT(static_cast<usize>(at), table.size());
                EXPECT_EQ(table[static_cast<usize>(at)], vga[v]) << bpp << "bpp vga " << v;
            }
        }
        ++checked;
    }
    EXPECT_EQ(checked, 3u);

    // Vitreo's 8bpp mix (.docs/contracts/gdi32.md "GetDIBits colour
    // tables"): the four extra static colours complete a 0x20-step grey ramp
    // 0x40..0xE0 with Gray and Silver; entries 10..225 are the 6x6x6 cube
    // (levels 51 * n, red slowest), 226..245 the greys round(255 k / 21).
    auto grey = [](u32 v) { return (v << 16) | (v << 8) | v; };
    EXPECT_EQ(pal8[8], grey(0x40));
    EXPECT_EQ(pal8[9], grey(0x60));
    EXPECT_EQ(pal8[246], grey(0xA0));
    EXPECT_EQ(pal8[247], grey(0xE0));
    usize i = 10;
    for (u32 r = 0; r <= 255; r += 51)
        for (u32 g = 0; g <= 255; g += 51)
            for (u32 b = 0; b <= 255; b += 51) EXPECT_EQ(pal8[i++], (r << 16) | (g << 8) | b) << i;
    for (u32 k = 1; k <= 20; ++k) {
        EXPECT_EQ(pal8[i++], grey(static_cast<u32>(std::lround(255.0 * k / 21.0)))) << k;
    }
    EXPECT_EQ(i, 246u);

    // The default palette (DEFAULT_PALETTE, DIB_PAL_COLORS with no palette
    // selected) is the 20 static colours: GetDeviceCaps NUMRESERVED, the
    // 16 VGA colours plus 4 more (Learn, "Default Palette").
    const Handle dc = mgr_.create_compatible_dc(0);
    const u64 stock = call("GetStockObject", {15}, true);  // DEFAULT_PALETTE
    ASSERT_NE(stock, 0u);
    ASSERT_EQ(call("GetPaletteEntries", {stock, 0, 0, 0}, true), 20u);
    ASSERT_EQ(call("GetPaletteEntries", {stock, 0, 256, kMem + 0x9000}, true), 20u);
    std::vector<u32> def;
    for (u32 k = 0; k < 20; ++k) {
        const u32 pe = memory_.read_u32(kMem + 0x9000 + k * 4).value();
        def.push_back(((pe & 0xFF) << 16) | (pe & 0xFF00) | ((pe >> 16) & 0xFF));
        EXPECT_EQ(pe >> 24, 0u) << k;
    }
    EXPECT_EQ(call("GetPaletteEntries", {stock, 18, 5, kMem + 0x9000}, true), 2u);
    EXPECT_EQ(static_cast<i32>(call("GetDeviceCaps", {dc, 106}, true)), 20);  // NUMRESERVED
    for (u32 c : vga) EXPECT_EQ(std::count(def.begin(), def.end(), c), 1) << std::hex << c;
    for (u32 k = 0; k < 10; ++k) {
        EXPECT_EQ(def[k], pal8[k]);
        EXPECT_EQ(def[10 + k], pal8[246 + k]);
    }

    // GetSystemPaletteEntries: the same 256 entries, peFlags 0 (the static
    // entries "correspond to the colors in the default palette", Learn,
    // "System Palette").
    EXPECT_EQ(mgr_.get_system_palette_entries(dc, 0, 256, 0), 256u);
    ASSERT_EQ(mgr_.get_system_palette_entries(dc, 0, 256, kMem + 0x8000), 256u);
    for (u32 k = 0; k < 256; ++k) {
        const u32 pe = memory_.read_u32(kMem + 0x8000 + k * 4).value();
        const u32 rgb = ((pe & 0xFF) << 16) | (pe & 0xFF00) | ((pe >> 16) & 0xFF);
        EXPECT_EQ(rgb, pal8[k]) << k;
        EXPECT_EQ(pe >> 24, 0u) << k;
    }
    EXPECT_EQ(mgr_.get_system_palette_entries(dc, 250, 10, kMem + 0x8000), 6u);
}

TEST_F(GdiContractTest, CodecPalettesAreTheMeasuredTables) {
    // s3gdi_info_probe "nearest4": Euclidean nearest over the 16-colour table
    // matches Windows for every sampled colour.
    std::vector<std::string> lines = expected_lines("s3gdi_info_probe.expected");
    std::vector<u32> nearest;
    for (const auto& line : lines) {
        if (line.rfind("nearest4:", 0) != 0) continue;
        std::string rest = line.substr(9);
        for (char* p = rest.data(); *p;) {
            char* end = nullptr;
            const long v = std::strtol(p, &end, 10);
            if (end == p) break;
            nearest.push_back(static_cast<u32>(v));
            p = end;
        }
    }
    ASSERT_EQ(nearest.size(), 144u);
    usize n = 0;
    for (u32 r = 0; r < 256; r += 51) {
        for (u32 g = 0; g < 256; g += 51) {
            for (u32 b = 0; b < 256; b += 85) {
                EXPECT_EQ(gdi::nearest_palette_index(gdi::kGdiDefaultPalette4, static_cast<u8>(r),
                                                     static_cast<u8>(g), static_cast<u8>(b)),
                          nearest[n]) << r << "," << g << "," << b;
                ++n;
            }
        }
    }
}


// LoadBitmap / LoadImage(IMAGE_BITMAP, LR_DEFAULTCOLOR) of a 1 bpp DIB:
// tools/oracle_probe/mono_dib_load_probe.expected (x86 == x64). 8x2, left
// half index 0, right half index 1. Monochrome only for a black/white table
// in either order; every other table is a display-depth (32) colour bitmap
// keeping the table's colours -- Solitaire's red cards are {red,white}.
TEST(LoadedMonoDib, CompatibleBitmapIsMonochromeOnlyForBlackWhiteTables) {
    struct Rgb { u8 r, g, b; };
    const auto make = [](bool core, Rgb c0, Rgb c1) {
        std::vector<u8> dib;
        const auto put16 = [&](u32 v) { dib.push_back(u8(v)); dib.push_back(u8(v >> 8)); };
        const auto put32 = [&](u32 v) { put16(v & 0xFFFF); put16(v >> 16); };
        if (core) { put32(12); put16(8); put16(2); put16(1); put16(1); }
        else { put32(40); put32(8); put32(2); put16(1); put16(1); for (int i = 0; i < 6; ++i) put32(0); }
        for (const Rgb c : {c0, c1}) {
            dib.push_back(c.b); dib.push_back(c.g); dib.push_back(c.r);
            if (!core) dib.push_back(0);
        }
        const u32 bits_at = 14 + static_cast<u32>(dib.size());
        for (int row = 0; row < 2; ++row) { dib.push_back(0x0F); dib.push_back(0); dib.push_back(0); dib.push_back(0); }
        std::vector<u8> file = {'B', 'M'};
        const u32 size = 14 + static_cast<u32>(dib.size());
        for (u32 v : {size, 0u, bits_at}) for (int i = 0; i < 4; ++i) file.push_back(u8(v >> (8 * i)));
        file.insert(file.end(), dib.begin(), dib.end());
        return file;
    };
    const Rgb K{0, 0, 0}, W{255, 255, 255}, R{255, 0, 0}, B{0, 0, 255};
    struct Case { const char* name; bool core; Rgb c0, c1; u16 bpp; };
    const Case cases[] = {
        {"res1 info {black,white}", false, K, W, 1},
        {"res2 info {white,black}", false, W, K, 1},
        {"res3 info {red,white}", false, R, W, 32},
        {"res4 info {red,blue}", false, R, B, 32},
        {"res5 core {black,white}", true, K, W, 1},
        {"res6 core {red,white}", true, R, W, 32},
        {"res7 info {black,black}", false, K, K, 32},
    };
    for (const Case& c : cases) {
        const auto file = make(c.core, c.c0, c.c1);
        const auto decoded = decode_bmp_file(file);
        ASSERT_TRUE(decoded.has_value()) << c.name;
        EXPECT_EQ(compatible_bitmap_bpp(*decoded, 32), c.bpp) << c.name;
        // GetPixel of index 0 (x=0) and index 1 (x=7): the table's colours.
        const auto at = [&](int x) {
            const u8* p = &decoded->rgba[static_cast<size_t>(x) * 4];
            return Rgb{p[0], p[1], p[2]};
        };
        for (const auto& [x, want] : {std::pair{0, c.c0}, std::pair{7, c.c1}}) {
            const Rgb got = at(x);
            EXPECT_TRUE(got.r == want.r && got.g == want.g && got.b == want.b)
                << c.name << " x=" << x;
        }
    }
}

}  // namespace
