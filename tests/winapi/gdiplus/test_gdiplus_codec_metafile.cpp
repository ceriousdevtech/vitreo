// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_codec_metafile.cpp
/// @brief Contract tests for the GDI+ codec area (metafiles), both bitnesses.
///
/// Expected values come from tools/oracle_probe/gdiplus_codec_probe.c on
/// winoracle (mf.* lines): the EMF header bytes are the ones Windows
/// recorded for CreateEnhMetaFile(frame 0,0,2000,1000) + Rectangle, the
/// EMF+ records are those GdipRecordMetafile wrote for one FillRectangle.

#include "gdiplus_fixture.hpp"

#include <winapi/graphics/gdiplus/gdiplus_graphics.hpp>
#include <winapi/graphics/gdiplus/gdiplus_image.hpp>

namespace vitreo::winapi::gdip_test {
namespace {

void put32(std::vector<u8>& v, u32 x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<u8>(x >> (8 * i)));
}
void put16(std::vector<u8>& v, u16 x) {
    v.push_back(static_cast<u8>(x));
    v.push_back(static_cast<u8>(x >> 8));
}
void set32(std::vector<u8>& v, usize at, u32 x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<u8>(x >> (8 * i));
}

/// probe "mf.emf hdr raw" (ENHMETAHEADER3 Windows wrote), description removed.
std::vector<u8> emf_header() {
    std::vector<u8> h;
    for (u32 x : {1u, 88u, 10u, 10u, 49u, 39u, 0u, 0u, 2000u, 1000u, 0x464D4520u, 0x10000u, 0u, 0u}) put32(h, x);
    put16(h, 1);
    put16(h, 0);
    for (u32 x : {0u, 0u, 0u, 1024u, 768u, 271u, 203u}) put32(h, x);
    return h;
}

void finish(std::vector<u8>& emf, u32 records) {
    set32(emf, 48, static_cast<u32>(emf.size()));
    set32(emf, 52, records);
}

/// EMR_HEADER, EMR_RECTANGLE(10,10,49,39), EMR_EOF (probe "mf.enum").
std::vector<u8> plain_emf() {
    std::vector<u8> e = emf_header();
    for (u32 x : {0x2Bu, 24u, 10u, 10u, 49u, 39u}) put32(e, x);
    for (u32 x : {0x0Eu, 20u, 0u, 16u, 20u}) put32(e, x);
    finish(e, 3);
    return e;
}

void emfplus_comment(std::vector<u8>& e, const std::vector<u8>& records) {
    put32(e, 70);
    put32(e, static_cast<u32>(12 + 4 + records.size()));
    put32(e, static_cast<u32>(4 + records.size()));
    put32(e, 0x2B464D45u);
    e.insert(e.end(), records.begin(), records.end());
}

/// EMF+ dual: header, EmfPlusHeader comment (probe "rec type=0x4001 flags=0x1
/// data=0210c0db010000006000000060000000"), FillRects, a down-level GDI
/// rectangle, EmfPlusEndOfFile, EMR_EOF.
std::vector<u8> plus_emf() {
    std::vector<u8> e = emf_header();
    std::vector<u8> r;
    put16(r, 0x4001); put16(r, 1); put32(r, 28); put32(r, 16);
    for (u32 x : {0xdbc01002u, 1u, 96u, 96u}) put32(r, x);
    emfplus_comment(e, r);
    r.clear();
    put16(r, 0x400a); put16(r, 0xc000); put32(r, 28); put32(r, 16);
    for (u32 x : {0xff00ff00u, 1u, 0x00020001u, 0x0014001eu}) put32(r, x);
    emfplus_comment(e, r);
    for (u32 x : {0x2Bu, 24u, 1u, 2u, 31u, 22u}) put32(e, x);
    r.clear();
    put16(r, 0x4002); put16(r, 0); put32(r, 12); put32(r, 0);
    emfplus_comment(e, r);
    for (u32 x : {0x0Eu, 20u, 0u, 16u, 20u}) put32(e, x);
    finish(e, 6);
    return e;
}

struct Call {
    u32 type, flags, size;
    u64 data;
    std::vector<u8> bytes;
};

class Gdip_codec_metafile_Test : public GdipTest {
protected:
    void SetUp() override {
        GdipTest::SetUp();
        gdi.set_guest_call([this](GuestAddr fn, const u64* a, usize n, bool) -> u64 {
            EXPECT_EQ(fn, kCallback);
            EXPECT_EQ(n, 5u);
            Call c{static_cast<u32>(a[0]), static_cast<u32>(a[1]), static_cast<u32>(a[2]), a[3], {}};
            c.bytes.resize(c.size);
            if (c.size) EXPECT_TRUE(memory.read_bytes(a[3], c.bytes.data(), c.size).is_ok());
            EXPECT_EQ(a[4], kCookie);
            calls.push_back(std::move(c));
            return calls.size() < stop_after ? 1 : 0;
        });
        graphics = mgr.objects().add(gdip::GpGraphics{});
    }

    u64 create(const std::vector<u8>& emf, Handle* hemf_out = nullptr) {
        const Handle hemf = gdi.set_enh_metafile_bits(emf);
        EXPECT_NE(hemf, 0u);
        if (hemf_out) *hemf_out = hemf;
        EXPECT_EQ(call("GdipCreateMetafileFromEmf", {I(hemf), I(0), I(OUT)}), Status::Ok);
        return read_handle();
    }

    static constexpr GuestAddr kCallback = 0x7700;
    static constexpr u64 kCookie = 0x5A5A;
    std::vector<Call> calls;
    usize stop_after = 1000;
    u64 graphics = 0;
};

TEST_P(Gdip_codec_metafile_Test, HeaderOfPlainEmfMatchesWindows) {
    const std::vector<u8> emf = plain_emf();
    const u64 mf = create(emf);
    ASSERT_NE(mf, 0u);
    poison(BUF, 140);
    ASSERT_EQ(call("GdipGetMetafileHeaderFromMetafile", {I(mf), I(BUF)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0), 3);                       // MetafileTypeEmf
    EXPECT_EQ(read_int(BUF + 4), static_cast<i32>(emf.size()));
    EXPECT_EQ(read_int(BUF + 8), 0x10000);
    EXPECT_EQ(read_int(BUF + 12), 0);
    EXPECT_NEAR(read_real(BUF + 16), 95.976f, 0.001f);
    EXPECT_NEAR(read_real(BUF + 20), 96.095f, 0.001f);
    EXPECT_EQ(read_int(BUF + 24), 0);
    EXPECT_EQ(read_int(BUF + 28), 0);
    EXPECT_EQ(read_int(BUF + 32), 77);
    EXPECT_EQ(read_int(BUF + 36), 39);
    EXPECT_EQ(read_int(BUF + 40), 1);                      // EmfHeader.iType
    EXPECT_EQ(read_int(BUF + 44), 88);                     // nSize as GDI+ reports it
    EXPECT_EQ(read_int(BUF + 48), 10);                     // rclBounds
    EXPECT_EQ(read_int(BUF + 128), 0);                     // EmfPlusHeaderSize
    EXPECT_EQ(read_int(BUF + 132), 0);
    EXPECT_EQ(read_int(BUF + 136), 0);
    std::vector<u8> a(140), b(140);
    ASSERT_TRUE(memory.read_bytes(BUF, a.data(), 140).is_ok());
    Handle hemf = gdi.set_enh_metafile_bits(emf);
    ASSERT_EQ(call("GdipGetMetafileHeaderFromEmf", {I(hemf), I(BUF)}), Status::Ok);
    ASSERT_TRUE(memory.read_bytes(BUF, b.data(), 140).is_ok());
    EXPECT_EQ(a, b);

    const auto* img = mgr.objects().get<gdip::GpImage>(mf);
    ASSERT_NE(img, nullptr);
    EXPECT_EQ(img->type, gdip::ImageTypeMetafile);
    EXPECT_EQ(img->width, 77);
    EXPECT_EQ(img->height, 39);
    EXPECT_EQ(img->pixel_format, 0x22009);
    EXPECT_EQ(img->flags, 0x50003u);
}

TEST_P(Gdip_codec_metafile_Test, EnumeratesEveryGdiRecordOfPlainEmf) {
    const u64 mf = create(plain_emf());
    write_reals(OUT2, {0, 0});
    ASSERT_EQ(call("GdipEnumerateMetafileDestPoint", {I(graphics), I(mf), I(OUT2), I(kCallback), I(kCookie), I(0)}),
              Status::Ok);
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].type, 1u);
    EXPECT_EQ(calls[0].size, 80u);
    EXPECT_EQ(calls[0].bytes[0], 10);     // data starts at rclBounds
    EXPECT_EQ(calls[1].type, 0x2Bu);
    EXPECT_EQ(calls[1].size, 16u);
    EXPECT_EQ(calls[1].bytes[8], 49);
    EXPECT_EQ(calls[2].type, 0x0Eu);
    EXPECT_EQ(calls[2].size, 12u);
    EXPECT_EQ(calls[2].flags, 0u);
}

TEST_P(Gdip_codec_metafile_Test, EmfPlusEnumeratesPlusRecordsNotDownLevel) {
    const u64 mf = create(plus_emf());
    poison(BUF, 140);
    ASSERT_EQ(call("GdipGetMetafileHeaderFromMetafile", {I(mf), I(BUF)}), Status::Ok);
    EXPECT_EQ(read_int(BUF + 0), 5);                                // EmfPlusDual
    EXPECT_EQ(static_cast<u32>(read_int(BUF + 8)), 0xdbc01002u);
    EXPECT_EQ(read_int(BUF + 12), 1);
    EXPECT_EQ(read_int(BUF + 128), 28);
    EXPECT_EQ(read_int(BUF + 132), 96);
    EXPECT_EQ(read_int(BUF + 136), 96);

    write_reals(OUT2, {0, 0, 4, 4});
    write_reals(OUT3, {0, 0, 10, 10});
    ASSERT_EQ(call("GdipEnumerateMetafileSrcRectDestRect",
                   {I(graphics), I(mf), I(OUT2), I(OUT3), I(2), I(kCallback), I(kCookie), I(0)}),
              Status::Ok);
    ASSERT_EQ(calls.size(), 5u);
    const u32 types[5] = {1, 0x4001, 0x400a, 0x4002, 0x0e};
    for (usize i = 0; i < 5; ++i) EXPECT_EQ(calls[i].type, types[i]) << i;
    EXPECT_EQ(calls[1].flags, 1u);
    EXPECT_EQ(calls[1].size, 16u);
    EXPECT_EQ(calls[1].bytes[0], 0x02);
    EXPECT_EQ(calls[2].flags, 0xc000u);
    EXPECT_EQ(calls[3].size, 0u);
    EXPECT_EQ(calls[3].data, 0u);      // probe: data=(null) for a 0-byte record
}

TEST_P(Gdip_codec_metafile_Test, CallbackFalseStopsEnumeration) {
    const u64 mf = create(plain_emf());
    stop_after = 2;
    write_ints(OUT2, {0, 0, 4, 4});
    ASSERT_EQ(call("GdipEnumerateMetafileDestRectI", {I(graphics), I(mf), I(OUT2), I(kCallback), I(kCookie), I(0)}),
              Status::Ok);
    EXPECT_EQ(calls.size(), 2u);
}

TEST_P(Gdip_codec_metafile_Test, EnumerationValidation) {
    const u64 mf = create(plain_emf());
    write_reals(OUT2, {0, 0});
    // probe "mf.enumnullcb st=2", "mf.enumnullg st=2".
    EXPECT_EQ(call("GdipEnumerateMetafileDestPoint", {I(graphics), I(mf), I(OUT2), I(0), I(0), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipEnumerateMetafileDestPoint", {I(0), I(mf), I(OUT2), I(kCallback), I(0), I(0)}),
              Status::InvalidParameter);
    EXPECT_EQ(call("GdipEnumerateMetafileDestPoint", {I(graphics), I(graphics), I(OUT2), I(kCallback), I(0), I(0)}),
              Status::InvalidParameter);
    EXPECT_TRUE(calls.empty());
}

TEST_P(Gdip_codec_metafile_Test, HemfHandOffInvalidatesMetafile) {
    Handle hemf = 0;
    const u64 mf = create(plain_emf(), &hemf);
    poison(OUT2);
    ASSERT_EQ(call("GdipGetHemfFromMetafile", {I(mf), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_handle(OUT2), hemf);                       // "mf.gethemf same=1"
    EXPECT_EQ(call("GdipGetHemfFromMetafile", {I(mf), I(OUT2)}), Status::InvalidParameter);   // gethemf2
    EXPECT_NE(gdi.enh_metafile(hemf), nullptr);
}

TEST_P(Gdip_codec_metafile_Test, RasterizationLimitStates) {
    const u64 mf = create(plain_emf());
    EXPECT_EQ(call("GdipGetMetafileDownLevelRasterizationLimit", {I(mf), I(OUT2)}), Status::WrongState);
    EXPECT_EQ(call("GdipSetMetafileDownLevelRasterizationLimit", {I(mf), I(300)}), Status::WrongState);
    gdip::GpImage bmp;
    const u64 b = mgr.objects().add(bmp);
    EXPECT_EQ(call("GdipGetMetafileDownLevelRasterizationLimit", {I(b), I(OUT2)}), Status::WrongState);
    // "mf.hdr.bitmap st=0".
    EXPECT_EQ(call("GdipGetMetafileHeaderFromMetafile", {I(b), I(BUF)}), Status::Ok);
    EXPECT_EQ(read_int(BUF), 0);
}

TEST_P(Gdip_codec_metafile_Test, CreationErrors) {
    poison(OUT);
    EXPECT_EQ(call("GdipCreateMetafileFromEmf", {I(0x1234), I(0), I(OUT)}), Status::InvalidParameter);
    EXPECT_EQ(read_handle(), 0u);
    EXPECT_EQ(call("GdipCreateMetafileFromEmf", {I(0x1234), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetMetafileHeaderFromEmf", {I(0x1234), I(BUF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetMetafileHeaderFromWmf", {I(0), I(0), I(BUF)}), Status::InvalidParameter);
}

TEST_P(Gdip_codec_metafile_Test, UnimplementedPathsFailLoudly) {
    const u64 mf = create(plain_emf());
    EXPECT_EQ(call("GdipPlayMetafileRecord", {I(mf), I(0x2B), I(0), I(0), I(0)}), Status::NotImplemented);
    EXPECT_EQ(call("GdipPlayMetafileRecord", {I(0), I(0x2B), I(0), I(0), I(0)}), Status::InvalidParameter);
    write_reals(OUT2, {0, 0, 100, 50});
    // probe "mf.record.nullhdc st=2" / "mf.record.badtype st=2".
    EXPECT_EQ(call("GdipRecordMetafile", {I(0), I(5), I(OUT2), I(2), I(0), I(OUT)}), Status::InvalidParameter);
    Handle hemf = 0;
    (void)create(plain_emf(), &hemf);
    EXPECT_EQ(call("GdipEmfToWmfBits", {I(hemf), I(0), I(0), I(8), I(0)}), static_cast<Status>(0));
    write_int(OUT3, -1);
    const u64 plus = create(plus_emf());
    // "mf.convert-of-plus st=2 fail=0".
    EXPECT_EQ(call("GdipConvertToEmfPlus", {I(graphics), I(plus), I(OUT3), I(4), I(0), I(OUT)}),
              Status::InvalidParameter);
    EXPECT_EQ(read_int(OUT3), 0);
    EXPECT_EQ(call("GdipConvertToEmfPlus", {I(graphics), I(mf), I(OUT3), I(4), I(0), I(OUT)}),
              Status::NotImplemented);
}

INSTANTIATE_GDIP_BITNESS(Gdip_codec_metafile_Test);

}  // namespace
}  // namespace vitreo::winapi::gdip_test
