// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_gdiplus_codec.cpp
/// @brief Contract tests for the GDI+ codec area (gdiplus_codec.cpp), run in
/// both bitnesses through gdiplus_fixture.hpp. Expected values come from
/// tools/oracle_probe/gdiplus_codec_probe.expected (x64) / .x86.expected,
/// measured on winoracle with the fixtures in tests/winapi/gdiplus/codec/.

#include "gdiplus_fixture.hpp"
#include <winapi/graphics/gdi32/gdi32_internal.hpp>

#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <filesystem>
#include <fstream>
#include <map>

namespace vitreo::winapi::gdip_test {
namespace {

namespace fs = std::filesystem;

// GUIDs (GdiPlusImaging.h) in memory layout.
const u8 kFrameTime[16] = {0x6d, 0xbd, 0xed, 0x6a, 0xb5, 0x3f, 0x8a, 0x41, 0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72};
const u8 kFramePage[16] = {0x86, 0xdc, 0x62, 0x74, 0x80, 0x61, 0x7e, 0x4c, 0x8e, 0x3f, 0xee, 0x73, 0x33, 0xa7, 0xa4, 0x83};
const u8 kEncQuality[16] = {0xb5, 0xe4, 0x5b, 0x1d, 0x4a, 0xfa, 0x2d, 0x45, 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb};
const u8 kEncTransformation[16] = {0xd1, 0xb2, 0x0e, 0x8d, 0x8e, 0xa5, 0xa8, 0x4e, 0xaa, 0x14, 0x10, 0x80, 0x74, 0xb7, 0xb6, 0xf9};
const u8 kEncSaveFlag[16] = {0xfc, 0x66, 0x22, 0x29, 0x40, 0xac, 0xbf, 0x47, 0x8c, 0xfc, 0xa8, 0x5b, 0x89, 0xa6, 0x55, 0xde};
// Encoder CLSIDs {557CF40n-1A04-11D3-9A73-0000F81EF32E}: n = 0 BMP, 1 JPEG, 2 GIF, 5 TIFF, 6 PNG.
void clsid(u8 out[16], u8 n) {
    const u8 base[16] = {0x00, 0xf4, 0x7c, 0x55, 0x04, 0x1a, 0xd3, 0x11, 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e};
    std::memcpy(out, base, 16);
    out[0] = static_cast<u8>(n);
}

class Gdip_codec_Test : public GdipTest {
protected:
    static constexpr GuestAddr GUIDS = DATA + 0x200;      ///< scratch GUIDs
    static constexpr GuestAddr NAME = DATA + 0x400;       ///< a path
    static constexpr GuestAddr PARAMS = DATA + 0x800;     ///< EncoderParameters
    static constexpr GuestAddr STREAM = DATA + 0xC00;     ///< fake IStream object
    static constexpr GuestAddr VTBL = DATA + 0xC40;
    static constexpr GuestAddr FN = 0xF0000;              ///< fake vtable function addresses
    static constexpr GuestAddr BIG = DATA + 0x20000;      ///< large outputs

    void SetUp() override {
        GdipTest::SetUp();
        const fs::path dir = fs::path(__FILE__).parent_path() / "codec";
        out_dir = fs::temp_directory_path() / ("gdip_codec_" + std::to_string(::getpid()) + (x64() ? "_64" : "_32"));
        fs::create_directories(out_dir);
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(dir.string())).is_ok());
        ASSERT_TRUE(vfs.mount_drive('D', std::make_unique<capsule::HostFileSystem>(out_dir.string())).is_ok());
        mgr.set_vfs(&vfs);
        // A host-side IStream behind a guest vtable: every slot address is FN + slot*16.
        const usize p = ptr_size();
        put_ptr(STREAM, VTBL);
        for (usize s = 0; s < 14; ++s) {
            const u64 f = FN + s * 16;
            put_ptr(VTBL + s * p, f);
        }
        gdi.set_guest_call([this](GuestAddr f, const u64* a, usize n, bool is64) -> u64 { return stream_call(f, a, n, is64); });
        handlers = gdiplus_handlers(mgr);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(out_dir, ec);
    }

    u64 stream_call(GuestAddr f, const u64* a, usize n, bool is64) {
        const usize slot = (f - FN) / 16;
        ++calls[slot];
        switch (slot) {
            case 3: {  // Read(this, pv, cb, pcbRead)
                const u64 k = std::min<u64>(a[2], stream_data.size() - std::min<u64>(stream_pos, stream_data.size()));
                if (k) EXPECT_TRUE(memory.write_bytes(a[1], stream_data.data() + stream_pos, k).is_ok());
                stream_pos += k;
                if (a[3]) EXPECT_TRUE(memory.write_u32(a[3], static_cast<u32>(k)).is_ok());
                return 0;
            }
            case 4: {  // Write(this, pv, cb, pcbWritten)
                std::vector<u8> d(a[2]);
                EXPECT_TRUE(memory.read_bytes(a[1], d.data(), d.size()).is_ok());
                if (stream_data.size() < stream_pos + d.size()) stream_data.resize(stream_pos + d.size());
                std::memcpy(stream_data.data() + stream_pos, d.data(), d.size());
                stream_pos += d.size();
                if (a[3]) EXPECT_TRUE(memory.write_u32(a[3], static_cast<u32>(d.size())).is_ok());
                return 0;
            }
            case 5: {  // Seek
                const i64 move = is64 ? static_cast<i64>(a[1]) : static_cast<i64>((a[2] << 32) | (a[1] & 0xFFFFFFFFu));
                const u64 origin = is64 ? a[2] : a[3];
                const u64 out = is64 ? a[3] : a[4];
                const i64 base = origin == 0 ? 0 : origin == 1 ? static_cast<i64>(stream_pos) : static_cast<i64>(stream_data.size());
                stream_pos = static_cast<u64>(base + move);
                if (out) EXPECT_TRUE(memory.write_u64(out, stream_pos).is_ok());
                return 0;
            }
            default:
                ADD_FAILURE() << "unexpected IStream slot " << slot << " args=" << n;
                return 0x80004001u;
        }
    }

    static std::vector<u8> fixture(const std::string& name) {
        std::ifstream f(fs::path(__FILE__).parent_path() / "codec" / name, std::ios::binary);
        return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }

    Status load(const std::string& file, u64& img, const char* api = "GdipLoadImageFromFile") {
        write_wide(NAME, u"C:\\" + std::u16string(file.begin(), file.end()));
        poison(OUT);
        const Status st = call(api, {I(NAME), I(OUT)});
        img = read_handle(OUT);
        return st;
    }
    u64 must_load(const std::string& file) {
        u64 img = 0;
        EXPECT_EQ(load(file, img), Status::Ok) << file;
        return img;
    }
    i32 query(const char* api, u64 img) {
        poison(OUT2, 4);
        EXPECT_EQ(call(api, {I(img), I(OUT2)}), Status::Ok) << api;
        return read_int(OUT2);
    }
    float query_real(const char* api, u64 img) {
        EXPECT_EQ(call(api, {I(img), I(OUT2)}), Status::Ok) << api;
        return read_real(OUT2);
    }
    u32 pixel(u64 img, i32 x, i32 y) {
        EXPECT_EQ(call("GdipBitmapGetPixel", {I(img), I(static_cast<u64>(x)), I(static_cast<u64>(y)), I(OUT3)}), Status::Ok);
        return memory.read_u32(OUT3).value();
    }
    std::vector<u32> pixels4x4(u64 img) {
        std::vector<u32> v;
        for (i32 y = 0; y < 4; ++y) for (i32 x = 0; x < 4; ++x) v.push_back(pixel(img, x, y));
        return v;
    }
    std::vector<u32> prop_ids(u64 img) {
        const i32 n = query("GdipGetPropertyCount", img);
        std::vector<u32> ids(static_cast<usize>(n));
        EXPECT_EQ(call("GdipGetPropertyIdList", {I(img), I(static_cast<u64>(n)), I(BIG)}), Status::Ok);
        for (i32 i = 0; i < n; ++i) ids[static_cast<usize>(i)] = memory.read_u32(BIG + i * 4).value();
        return ids;
    }
    /// The property's (type, value bytes).
    std::pair<u16, std::vector<u8>> prop(u64 img, u32 id) {
        EXPECT_EQ(call("GdipGetPropertyItemSize", {I(img), I(id), I(OUT2)}), Status::Ok);
        const u32 size = memory.read_u32(OUT2).value();
        EXPECT_EQ(call("GdipGetPropertyItem", {I(img), I(id), I(size), I(BIG)}), Status::Ok);
        const u32 len = memory.read_u32(BIG + 4).value();
        const u16 type = memory.read_u16(BIG + 8).value();
        const u64 value = x64() ? memory.read_u64(BIG + 16).value() : memory.read_u32(BIG + 12).value();
        EXPECT_EQ(value, BIG + (x64() ? 24u : 16u));
        EXPECT_EQ(size, len + (x64() ? 24u : 16u));
        std::vector<u8> v(len);
        if (len) EXPECT_TRUE(memory.read_bytes(value, v.data(), len).is_ok());
        return {type, v};
    }
    void put_ptr(GuestAddr at, u64 v) {
        ASSERT_TRUE((x64() ? memory.write_u64(at, v) : memory.write_u32(at, static_cast<u32>(v))).is_ok());
    }
    void write_guid(GuestAddr at, const u8 g[16]) { ASSERT_TRUE(memory.write_bytes(at, g, 16).is_ok()); }
    /// One-parameter EncoderParameters at PARAMS with a LONG value.
    void one_param(const u8 guid[16], u32 type, u32 count, u32 value) {
        const usize p = ptr_size();
        write_int(PARAMS, 1);
        write_guid(PARAMS + p, guid);
        write_int(PARAMS + p + 16, static_cast<i32>(count));
        write_int(PARAMS + p + 20, static_cast<i32>(type));
        const u64 v = PARAMS + 0x100;
        put_ptr(PARAMS + p + 24, v);
        write_int(PARAMS + 0x100, static_cast<i32>(value));
    }
    Status save_to_stream(u64 img, u8 encoder, GuestAddr params = 0) {
        stream_data.clear();
        stream_pos = 0;
        clsid(enc, encoder);
        write_guid(GUIDS, enc);
        return call("GdipSaveImageToStream", {I(img), I(STREAM), I(GUIDS), I(params)});
    }
    Status load_stream(const std::vector<u8>& bytes, u64& img, const char* api = "GdipLoadImageFromStream") {
        stream_data = bytes;
        stream_pos = 0;
        poison(OUT);
        const Status st = call(api, {I(STREAM), I(OUT)});
        img = read_handle(OUT);
        return st;
    }

    capsule::Vfs vfs;
    fs::path out_dir;
    std::vector<u8> stream_data;
    u64 stream_pos{0};
    std::map<usize, int> calls;
    u8 enc[16]{};
};

// ---- Codec lists (probe dec.* / enc.*) --------------------------------------------------

TEST_P(Gdip_codec_Test, DecoderAndEncoderListsMatchTheProbeLayout) {
    ASSERT_EQ(call("GdipGetImageDecodersSize", {I(OUT), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT), 8);
    // The probe's sizes (2064 / 1696, encoders 1264 / 1040) include trailing slack
    // past the last mask byte; the strings and signatures end at 1720 / 1496 and
    // 1056 / 916 there (dec[7]/enc[4] "off" lines), which is what Vitreo reports.
    EXPECT_EQ(read_int(OUT2), x64() ? 1720 : 1496);
    ASSERT_EQ(call("GdipGetImageEncodersSize", {I(OUT), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT), 5);
    const i32 esize = read_int(OUT2);
    EXPECT_EQ(esize, x64() ? 1056 : 916);
    EXPECT_EQ(call("GdipGetImageEncoders", {I(5), I(static_cast<u64>(esize - 1)), I(BIG)}), Status::GenericError);
    EXPECT_EQ(call("GdipGetImageEncoders", {I(5), I(static_cast<u64>(esize + 100)), I(BIG)}), Status::GenericError);
    EXPECT_EQ(call("GdipGetImageEncoders", {I(4), I(static_cast<u64>(esize)), I(BIG)}), Status::GenericError);
    EXPECT_EQ(call("GdipGetImageEncoders", {I(5), I(static_cast<u64>(esize)), I(0)}), Status::GenericError);
    ASSERT_EQ(call("GdipGetImageEncoders", {I(5), I(static_cast<u64>(esize)), I(BIG)}), Status::Ok);
    const usize rec = x64() ? 104 : 76, p = ptr_size();
    auto ptr = [&](GuestAddr at) { return x64() ? memory.read_u64(at).value() : memory.read_u32(at).value(); };
    // enc[0] off name=520 desc=558 ext=566 mime=602 sig=622 mask=624 (x64); 380/418/426/462/482/484 (x86).
    EXPECT_EQ(ptr(BIG + 32) - BIG, x64() ? 520u : 380u);
    EXPECT_EQ(ptr(BIG + 32 + p), 0u);  // DllName
    EXPECT_EQ(ptr(BIG + 32 + 2 * p) - BIG, x64() ? 558u : 418u);
    EXPECT_EQ(ptr(BIG + 32 + 3 * p) - BIG, x64() ? 566u : 426u);
    EXPECT_EQ(ptr(BIG + 32 + 4 * p) - BIG, x64() ? 602u : 462u);
    EXPECT_EQ(ptr(BIG + 48 + 5 * p) - BIG, x64() ? 622u : 482u);
    EXPECT_EQ(ptr(BIG + 48 + 6 * p) - BIG, x64() ? 624u : 484u);
    EXPECT_EQ(memory.read_u32(BIG + 32 + 5 * p).value(), 0x10007u);
    // enc[3] is TIFF: two 2-byte signatures; enc[4] PNG at name offset 962 / 822.
    EXPECT_EQ(memory.read_u32(BIG + 3 * rec + 32 + 5 * p + 8).value(), 2u);
    EXPECT_EQ(ptr(BIG + 4 * rec + 32) - BIG, x64() ? 962u : 822u);
    EXPECT_EQ(memory.read_u8(BIG + 16).value(), 0xab);  // FormatID ImageFormatBMP {b96b3cab-...}
    std::u16string mime = gdip::read_wstr(marshaler, ptr(BIG + 1 * rec + 32 + 4 * p));
    EXPECT_EQ(mime, u"image/jpeg");
    // Decoders: EMF (index 3) is decode-only with a 44-byte signature.
    ASSERT_EQ(call("GdipGetImageDecoders", {I(8), I(x64() ? 1720u : 1496u), I(BIG)}), Status::Ok);
    EXPECT_EQ(memory.read_u32(BIG + 3 * rec + 32 + 5 * p).value(), 0x10006u);
    EXPECT_EQ(memory.read_u32(BIG + 3 * rec + 32 + 5 * p + 12).value(), 44u);
}

// ---- Decoding (probe file:*) -------------------------------------------------------------------------

struct DecodeCase { const char* file; i32 format; u32 flags; float dpi; i32 palette; };

TEST_P(Gdip_codec_Test, DecodedFormatsFlagsAndResolutionMatchTheProbe) {
    const DecodeCase cases[] = {
        {"rgb24.bmp", 0x21808, 0x13010, 96.012f, 0}, {"argb32.bmp", 0x22009, 0x13010, 96.012f, 0},
        {"pal8.bmp", 0x30803, 0x13010, 96.012f, 4},  {"pal4.bmp", 0x30402, 0x13010, 72.009f, 16},
        {"mono1.bmp", 0x30101, 0x13010, 96.012f, 2}, {"rgb555.bmp", 0x22009, 0x13010, 96.012f, 0},
        {"rgb565.bmp", 0x22009, 0x13010, 96.012f, 0}, {"rle8.bmp", 0x22009, 0x13010, 96.012f, 0},
        {"v5alpha.bmp", 0x26200a, 0x13012, 96.012f, 0}, {"noppm.bmp", 0x21808, 0x12010, 96.0f, 0},
        {"topdown.bmp", 0x21808, 0x12010, 96.0f, 0}, {"rgb24.png", 0x21808, 0x12010, 96.0f, 0},
        {"rgba32.png", 0x26200a, 0x12012, 96.0f, 0}, {"pal8.png", 0x26200a, 0x12012, 96.0f, 0},
        {"pal2bit.png", 0x26200a, 0x12012, 96.0f, 0}, {"gray8.png", 0x26200a, 0x12042, 96.0f, 0},
        {"graya.png", 0x26200a, 0x12012, 96.0f, 0},  {"gray16.png", 0x26200a, 0x12042, 96.0f, 0},
        {"rgb48.png", 0x26200a, 0x12012, 96.0f, 0},  {"rgba64.png", 0x26200a, 0x12012, 96.0f, 0},
        {"paltrns.png", 0x26200a, 0x12012, 96.0f, 0}, {"meta.png", 0x21808, 0x13010, 72.009f, 0},
        {"rgb24.jpg", 0x21808, 0x12010, 96.0f, 0},   {"gray.jpg", 0x30803, 0x12040, 96.0f, 256},
        {"exif.jpg", 0x21808, 0x13010, 72.0f, 0},    {"exif2.jpg", 0x21808, 0x13010, 300.0f, 0},
        {"single.gif", 0x30803, 0x13012, 96.0f, 4},  {"anim.gif", 0x26200a, 0x13010, 96.0f, 0},
        {"two.tif", 0x21808, 0x12010, 96.0f, 0},     {"rgba.tif", 0x26200a, 0x12012, 96.0f, 0},
        {"icon.ico", 0x26200a, 0x12012, 96.0f, 0},
    };
    for (const DecodeCase& c : cases) {
        SCOPED_TRACE(c.file);
        const u64 img = must_load(c.file);
        EXPECT_EQ(query("GdipGetImagePixelFormat", img), c.format);
        EXPECT_EQ(static_cast<u32>(query("GdipGetImageFlags", img)), c.flags);
        EXPECT_NEAR(query_real("GdipGetImageHorizontalResolution", img), c.dpi, 0.01f);
        EXPECT_EQ(query("GdipGetImageWidth", img), 4);
        if (c.palette) {
            const i32 size = query("GdipGetImagePaletteSize", img);
            EXPECT_EQ(size, 8 + 4 * c.palette);
        }
    }
}

TEST_P(Gdip_codec_Test, DecodedPixelsMatchTheProbe) {
    const std::vector<u32> rgb = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xffff0000,
                                  0xff0000ff, 0xffffffff, 0xffff0000, 0xff00ff00, 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff};
    const std::vector<u32> rgba = {0xffff0000, 0x8000ff00, 0x000000ff, 0xffffffff, 0xff00ff00, 0x800000ff, 0x00ffffff, 0xffff0000,
                                   0xff0000ff, 0x80ffffff, 0x00ff0000, 0xff00ff00, 0xffffffff, 0x80ff0000, 0x0000ff00, 0xff0000ff};
    for (const char* f : {"rgb24.bmp", "pal8.bmp", "pal4.bmp", "rgb555.bmp", "rgb565.bmp", "rle8.bmp", "topdown.bmp", "rgb24.png",
                          "pal8.png", "pal2bit.png", "rgb48.png", "chunks.png", "two.tif"}) {
        SCOPED_TRACE(f);
        EXPECT_EQ(pixels4x4(must_load(f)), rgb);
    }
    for (const char* f : {"rgba32.png", "rgba64.png", "v5alpha.bmp", "rgba.tif", "icon.ico"}) {
        SCOPED_TRACE(f);
        EXPECT_EQ(pixels4x4(must_load(f)), rgba);
    }
    // Transparent GIF entry: 00ffffff; gray PNG: the gray values as stored.
    const std::vector<u32> gif = pixels4x4(must_load("single.gif"));
    EXPECT_EQ(gif[3], 0x00ffffffu);
    EXPECT_EQ(gif[0], 0xffff0000u);
    EXPECT_EQ(pixel(must_load("gray8.png"), 0, 0), 0xff4c4c4cu);
    EXPECT_EQ(pixel(must_load("graya.png"), 1, 0), 0x80969696u);
    const std::vector<u32> trns = pixels4x4(must_load("paltrns.png"));
    EXPECT_EQ(trns[1], 0x8000ff00u);
}

TEST_P(Gdip_codec_Test, LoadFailuresUseTheProbeStatuses) {
    u64 img = 0;
    EXPECT_EQ(load("does_not_exist.png", img), Status::OutOfMemory);
    EXPECT_EQ(img, 0u);
    EXPECT_EQ(load("does_not_exist.png", img, "GdipCreateBitmapFromFile"), Status::InvalidParameter);
    EXPECT_EQ(img, 0u);
    EXPECT_EQ(load("gen_fixtures.py", img), Status::OutOfMemory);
    EXPECT_EQ(load("gen_fixtures.py", img, "GdipCreateBitmapFromFile"), Status::InvalidParameter);
    EXPECT_EQ(call("GdipLoadImageFromFile", {I(0), I(OUT)}), Status::InvalidParameter);
    write_wide(NAME, u"C:\\rgb24.png");
    EXPECT_EQ(call("GdipLoadImageFromFile", {I(NAME), I(0)}), Status::InvalidParameter);
    std::vector<u8> junk(64, 0x5A);
    EXPECT_EQ(load_stream(junk, img), Status::InvalidParameter);
    EXPECT_EQ(img, 0u);
    EXPECT_EQ(stream_pos, 0u);  // probe err.stream.junk pos=0
    EXPECT_EQ(load_stream({}, img), Status::InvalidParameter);
    EXPECT_EQ(call("GdipLoadImageFromStream", {I(0), I(OUT)}), Status::InvalidParameter);
}

TEST_P(Gdip_codec_Test, StreamDecodingLeavesThePositionAsMeasured) {
    u64 img = 0;
    const auto png = fixture("rgba32.png");
    ASSERT_EQ(load_stream(png, img, "GdipCreateBitmapFromStream"), Status::Ok);
    EXPECT_EQ(query("GdipGetImagePixelFormat", img), 0x26200a);
    EXPECT_EQ(stream_pos, png.size());  // probe stream pos=96
    ASSERT_EQ(load_stream(fixture("two.tif"), img), Status::Ok);
    EXPECT_EQ(stream_pos, 0u);          // probe two.tif stream pos=0
    EXPECT_GT(calls[3], 0);
}

// Windows gdiplus_stream_position_probe{32,64}.expected: all four loaders
// decode the complete stream from zero, independent of the incoming cursor.
TEST_P(Gdip_codec_Test, StreamLoadersRewindInitialCursorAndResetDecodeFailures) {
    for (const char* api : {"GdipCreateBitmapFromStream", "GdipCreateBitmapFromStreamICM",
                            "GdipLoadImageFromStream", "GdipLoadImageFromStreamICM"}) {
        for (const char* file : {"rgba32.png", "two.tif"}) {
            const auto bytes = fixture(file);
            ASSERT_FALSE(bytes.empty());
            for (u64 initial : {u64{0}, static_cast<u64>(bytes.size()/2),
                                static_cast<u64>(bytes.size()), static_cast<u64>(bytes.size()+7)}) {
                SCOPED_TRACE(api);
                SCOPED_TRACE(file);
                SCOPED_TRACE(initial);
                stream_data = bytes; stream_pos = initial; calls.clear(); poison(OUT);
                ASSERT_EQ(call(api, {I(STREAM), I(OUT)}), Status::Ok);
                const u64 image = read_handle();
                ASSERT_NE(image, 0u);
                EXPECT_EQ(query("GdipGetImageWidth", image), 4);
                EXPECT_EQ(query("GdipGetImageHeight", image), 4);
                EXPECT_EQ(stream_pos, std::string_view(file) == "two.tif" ? 0u : bytes.size());
                EXPECT_EQ(call("GdipDisposeImage", {I(image)}), Status::Ok);
            }
        }
        for (const auto& bad : {std::vector<u8>{}, std::vector<u8>(64,0x5a)}) {
            for (u64 initial : {u64{0}, static_cast<u64>(bad.size()/2),
                                static_cast<u64>(bad.size()), static_cast<u64>(bad.size()+7)}) {
                SCOPED_TRACE(api);
                SCOPED_TRACE(initial);
                stream_data=bad; stream_pos=initial; calls.clear(); poison(OUT);
                EXPECT_EQ(call(api,{I(STREAM),I(OUT)}),Status::InvalidParameter);
                EXPECT_EQ(read_handle(),0u);
                EXPECT_EQ(stream_pos,0u);
            }
        }
    }
}

TEST_P(Gdip_codec_Test, StreamBitmapAtEofBecomesSelectableGdiPixelsAndBlits) {
    ASSERT_TRUE(memory.map_region(GdiManager::GDI_DIB_SCRATCH_BASE,
        GdiManager::GDI_DIB_SCRATCH_SIZE, MemoryProtection::ReadWrite).is_ok());
    stream_data = fixture("rgba32.png");
    ASSERT_FALSE(stream_data.empty());
    stream_pos = stream_data.size();
    ASSERT_EQ(call("GdipCreateBitmapFromStream", {I(STREAM), I(OUT)}), Status::Ok);
    const u64 image = read_handle();
    ASSERT_NE(image, 0u);
    ASSERT_EQ(call("GdipCreateHBITMAPFromBitmap", {I(image), I(OUT2), I(0xffffffffu)}), Status::Ok);
    const Handle bitmap = static_cast<Handle>(read_handle(OUT2));
    ASSERT_NE(bitmap, 0u);
    // GetHBITMAP creates independently owned GDI storage, not a GDI+ token.
    ASSERT_EQ(call("GdipDisposeImage", {I(image)}), Status::Ok);
    const Handle source = gdi.create_compatible_dc(0);
    ASSERT_NE(source, 0u);
    const Handle old_source = gdi.select_object(source, bitmap);
    ASSERT_NE(old_source, 0u);
    // Existing Windows codec oracle supplies opaque red at (0,0), blue (3,3).
    EXPECT_EQ(gdi.get_pixel(source, 0, 0), 0x000000ffu);
    EXPECT_EQ(gdi.get_pixel(source, 3, 3), 0x00ff0000u);
    const Handle destination = gdi.create_compatible_dc(source);
    const Handle target = gdi.create_compatible_bitmap(source, 4, 4);
    ASSERT_NE(destination, 0u); ASSERT_NE(target, 0u);
    const Handle old_destination = gdi.select_object(destination, target);
    ASSERT_NE(old_destination, 0u);
    ASSERT_TRUE(gdi.bit_blt(destination, 0, 0, 4, 4, source, 0, 0, SRCCOPY));
    EXPECT_EQ(gdi.get_pixel(destination, 0, 0), 0x000000ffu);
    EXPECT_EQ(gdi.get_pixel(destination, 3, 3), 0x00ff0000u);
    EXPECT_EQ(gdi.select_object(source, old_source), bitmap);
    EXPECT_EQ(gdi.select_object(destination, old_destination), target);
    EXPECT_TRUE(gdi.delete_dc(source)); EXPECT_TRUE(gdi.delete_dc(destination));
    EXPECT_TRUE(gdi.delete_object(bitmap)); EXPECT_TRUE(gdi.delete_object(target));
}

// ---- Properties -------------------------------------------------------------------------------------------

TEST_P(Gdip_codec_Test, ExifPropertiesFollowTheProbeOrderAndValues) {
    const u64 img = must_load("exif2.jpg");
    const std::vector<u32> want = {0x010f, 0x0112, 0x011a, 0x011b, 0x0128, 0x829a, 0x8827, 0x9000, 0x9003, 0x9204, 0x9286, 0x0000,
                                   0x0001, 0x0002, 0x501b, 0x5023, 0x502d, 0x502e, 0x5030, 0x0201, 0x0202, 0x5091, 0x5090};
    EXPECT_EQ(prop_ids(img), want);
    const auto uc = prop(img, 0x9286);
    EXPECT_EQ(uc.first, 7);
    EXPECT_EQ(uc.second, (std::vector<u8>{0x68, 0, 0x65, 0, 0x79, 0, 0x21, 0, 0, 0}));
    const auto expo = prop(img, 0x829a);
    EXPECT_EQ(expo.first, 5);
    EXPECT_EQ(expo.second, (std::vector<u8>{1, 0, 0, 0, 0x7d, 0, 0, 0}));
    EXPECT_EQ(prop(img, 0x501b).second.size(), 650u);
    const auto lum = prop(img, 0x5090);
    EXPECT_EQ(lum.first, 3);
    ASSERT_EQ(lum.second.size(), 128u);
    EXPECT_EQ(lum.second[0], 3);   // natural order: 03 02 02 03 05 08 0a 0c
    EXPECT_EQ(lum.second[2], 2);
    EXPECT_EQ(lum.second[14], 0x0c);
    // Missing id, list count rules, total size.
    EXPECT_EQ(call("GdipGetPropertyItemSize", {I(img), I(0xBEEF), I(OUT2)}), Status::PropertyNotFound);
    EXPECT_EQ(call("GdipGetPropertyIdList", {I(img), I(24), I(BIG)}), Status::InvalidParameter);
    // Embedded thumbnail with a 0x0 request: 2x2 24bppRGB (probe thumb00).
    ASSERT_EQ(call("GdipGetImageThumbnail", {I(img), I(0), I(0), I(OUT), I(0), I(0)}), Status::Ok);
    const u64 th = read_handle(OUT);
    EXPECT_EQ(query("GdipGetImageWidth", th), 2);
    EXPECT_EQ(query("GdipGetImagePixelFormat", th), 0x21808);
}

TEST_P(Gdip_codec_Test, PngAndGifPropertiesMatchTheProbe) {
    const u64 png = must_load("chunks.png");
    const std::vector<u32> want = {0x0302, 0x8773, 0x0301, 0x013e, 0x013f, 0x0303, 0x0132, 0x010e,
                                   0x8298, 0x0131, 0x0110, 0x9286, 0x5110, 0x5111, 0x5112};
    EXPECT_EQ(prop_ids(png), want);
    EXPECT_EQ(prop(png, 0x0132).second, (std::vector<u8>{'2', '0', '2', '6', ':', '1', '0', ':', '2', '7', ' ', '1', '3', ':', '1', '4', ':', '1', '5', 0}));
    EXPECT_EQ(prop(png, 0x0301).second, (std::vector<u8>{0xa0, 0x86, 0x01, 0, 0x8f, 0xb1, 0, 0}));
    EXPECT_EQ(prop(png, 0x8773).second.size(), 0u);
    // GetPropertySize / GetAllPropertyItems: headers then packed values (x86 probe: [0302@240] ... [5112@374]).
    ASSERT_EQ(call("GdipGetPropertySize", {I(png), I(OUT2), I(OUT3)}), Status::Ok);
    const u32 total = memory.read_u32(OUT2).value();
    EXPECT_EQ(read_int(OUT3), 15);
    ASSERT_EQ(call("GdipGetAllPropertyItems", {I(png), I(total), I(15), I(BIG)}), Status::Ok);
    const u64 v0 = x64() ? memory.read_u64(BIG + 16).value() : memory.read_u32(BIG + 12).value();
    EXPECT_EQ(v0 - BIG, 15u * (x64() ? 24u : 16u));
    EXPECT_EQ(call("GdipGetAllPropertyItems", {I(png), I(total - 1), I(15), I(BIG)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetAllPropertyItems", {I(png), I(total), I(16), I(BIG)}), Status::InvalidParameter);

    const u64 gif = must_load("anim.gif");
    EXPECT_EQ(prop_ids(gif), (std::vector<u32>{0x5100, 0x5101, 0x5102, 0x5103}));
    EXPECT_EQ(prop(gif, 0x5100).second, (std::vector<u8>{0x0a, 0, 0, 0, 0x19, 0, 0, 0}));
    EXPECT_EQ(prop(gif, 0x5101).second, (std::vector<u8>{3, 0}));
    EXPECT_EQ(prop_ids(must_load("single.gif")), (std::vector<u32>{0x5100, 0x5101, 0x5102, 0x5103, 0x5104}));
    EXPECT_EQ(prop_ids(must_load("rgb24.png")), (std::vector<u32>{0x5110, 0x5111, 0x5112}));
    EXPECT_EQ(prop_ids(must_load("rgb24.jpg")), (std::vector<u32>{0x5091, 0x5090}));
    EXPECT_EQ(prop_ids(must_load("two.tif")),
              (std::vector<u32>{0x100, 0x101, 0x102, 0x103, 0x106, 0x111, 0x115, 0x116, 0x117, 0x11c}));
}

TEST_P(Gdip_codec_Test, SetAndRemovePropertyItemFollowTheProbe) {
    const u64 img = must_load("exif.jpg");
    // PropertyItem {id, length, type, value}
    write_int(BUF, 0x0112);
    write_int(BUF + 4, 4);
    ASSERT_TRUE(memory.write_u16(BUF + 8, 4).is_ok());
    const u64 v = BUF + 0x40;
    put_ptr(BUF + (x64() ? 16 : 12), v);
    write_int(BUF + 0x40, 7);
    EXPECT_EQ(call("GdipSetPropertyItem", {I(img), I(BUF)}), Status::Ok);
    write_int(BUF, 0x9999);
    EXPECT_EQ(call("GdipSetPropertyItem", {I(img), I(BUF)}), Status::Ok);
    EXPECT_EQ(call("GdipRemovePropertyItem", {I(img), I(0x010F)}), Status::Ok);
    EXPECT_EQ(call("GdipRemovePropertyItem", {I(img), I(0x010F)}), Status::GenericError);
    EXPECT_EQ(prop_ids(img), (std::vector<u32>{0x0110, 0x0112, 0x011a, 0x011b, 0x0128, 0x0132, 0x5091, 0x5090, 0x9999}));
    const auto o = prop(img, 0x0112);
    EXPECT_EQ(o.first, 4);
    EXPECT_EQ(o.second, (std::vector<u8>{7, 0, 0, 0}));
    EXPECT_EQ(call("GdipSetPropertyItem", {I(img), I(0)}), Status::InvalidParameter);
    // A clone is independent.
    ASSERT_EQ(call("GdipCloneImage", {I(img), I(OUT)}), Status::Ok);
    const u64 clone = read_handle(OUT);
    EXPECT_EQ(call("GdipRemovePropertyItem", {I(clone), I(0x9999)}), Status::Ok);
    EXPECT_EQ(prop_ids(img).back(), 0x9999u);
}

// ---- Frames ---------------------------------------------------------------------------------------------------

TEST_P(Gdip_codec_Test, FramesFollowTheProbe) {
    const u64 gif = must_load("partial.gif");
    EXPECT_EQ(query("GdipImageGetFrameDimensionsCount", gif), 1);
    ASSERT_EQ(call("GdipImageGetFrameDimensionsList", {I(gif), I(BUF), I(1)}), Status::Ok);
    std::vector<u8> g(16);
    ASSERT_TRUE(memory.read_bytes(BUF, g.data(), 16).is_ok());
    EXPECT_EQ(std::memcmp(g.data(), kFrameTime, 16), 0);
    EXPECT_EQ(call("GdipImageGetFrameDimensionsList", {I(gif), I(BUF), I(2)}), Status::Win32Error);
    write_guid(GUIDS, kFrameTime);
    ASSERT_EQ(call("GdipImageGetFrameCount", {I(gif), I(GUIDS), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 2);
    write_guid(GUIDS + 16, kFramePage);
    poison(OUT2, 4);
    EXPECT_EQ(call("GdipImageGetFrameCount", {I(gif), I(GUIDS + 16), I(OUT2)}), Status::Win32Error);
    EXPECT_EQ(read_int(OUT2), 0);  // a GIF writes 0 (probe framecount-res n=0)
    EXPECT_EQ(call("GdipImageSelectActiveFrame", {I(gif), I(GUIDS), I(2)}), Status::Win32Error);
    EXPECT_EQ(call("GdipImageSelectActiveFrame", {I(gif), I(GUIDS + 16), I(0)}), Status::Win32Error);
    ASSERT_EQ(call("GdipImageSelectActiveFrame", {I(gif), I(GUIDS), I(1)}), Status::Ok);
    // Probe partial.gif#f1: frame 1 composited over frame 0.
    const std::vector<u32> f1 = {0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xff00ff00, 0xff0000ff, 0xffffffff, 0xffff0000,
                                 0xff0000ff, 0xffffffff, 0xff0000ff, 0xff00ff00, 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff};
    EXPECT_EQ(pixels4x4(gif), f1);
    EXPECT_EQ(prop(gif, 0x5100).second, (std::vector<u8>{7, 0, 0, 0, 9, 0, 0, 0}));

    const u64 tif = must_load("two.tif");
    ASSERT_EQ(call("GdipImageSelectActiveFrame", {I(tif), I(GUIDS + 16), I(1)}), Status::Ok);
    EXPECT_EQ(pixel(tif, 0, 0), 0xffffffffu);
    EXPECT_EQ(pixel(tif, 1, 0), 0xff0000ffu);
    // Icons answer 1 for any dimension (probe icon.ico framecount-res).
    const u64 ico = must_load("icon.ico");
    ASSERT_EQ(call("GdipImageGetFrameCount", {I(ico), I(GUIDS), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
}

// ---- Encoding -------------------------------------------------------------------------------------------------

TEST_P(Gdip_codec_Test, EncodersRoundTripAndWriteTheMeasuredLayouts) {
    ASSERT_EQ(call("GdipCreateBitmapFromScan0", {I(4), I(4), I(0), I(0x26200a), I(0), I(OUT)}), Status::Ok);
    const u64 bmp = read_handle(OUT);
    const u32 px[4] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0x80FFFFFF};
    for (i32 y = 0; y < 4; ++y)
        for (i32 x = 0; x < 4; ++x)
            ASSERT_EQ(call("GdipBitmapSetPixel", {I(bmp), I(static_cast<u64>(x)), I(static_cast<u64>(y)), I(px[(x + y) % 4])}), Status::Ok);
    // BMP: 32bpp BI_RGB, 118 bytes, the alpha byte kept (probe save[0]32 head).
    ASSERT_EQ(save_to_stream(bmp, 0), Status::Ok);
    ASSERT_EQ(stream_data.size(), 118u);
    EXPECT_EQ(stream_data[28], 32);
    EXPECT_EQ(stream_data[38], 0xc4);  // 3780 ppm
    EXPECT_EQ((std::vector<u8>(stream_data.begin() + 54, stream_data.begin() + 58)), (std::vector<u8>{0xff, 0xff, 0xff, 0x80}));
    u64 back = 0;
    ASSERT_EQ(load_stream(stream_data, back), Status::Ok);
    EXPECT_EQ(query("GdipGetImagePixelFormat", back), 0x22009);
    // PNG keeps alpha: reload 32bppARGB with the same pixels.
    ASSERT_EQ(save_to_stream(bmp, 6), Status::Ok);
    ASSERT_EQ(load_stream(stream_data, back), Status::Ok);
    EXPECT_EQ(query("GdipGetImagePixelFormat", back), 0x26200a);
    EXPECT_EQ(pixel(back, 3, 0), 0x80ffffffu);
    EXPECT_EQ(prop_ids(back), (std::vector<u32>{0x0303, 0x0301, 0x5110, 0x5111, 0x5112}));
    // GIF: 8bppIndexed on the halftone palette (probe r2.gifindices 210 36 5 251 ...).
    ASSERT_EQ(save_to_stream(bmp, 2), Status::Ok);
    ASSERT_EQ(load_stream(stream_data, back), Status::Ok);
    EXPECT_EQ(query("GdipGetImagePixelFormat", back), 0x30803);
    EXPECT_EQ(pixel(back, 0, 0), 0xffff0000u);
    EXPECT_EQ(prop(back, 0x5104).second, std::vector<u8>{0xfc});
    // TIFF: 32bppARGB.
    ASSERT_EQ(save_to_stream(bmp, 5), Status::Ok);
    ASSERT_EQ(load_stream(stream_data, back), Status::Ok);
    EXPECT_EQ(query("GdipGetImagePixelFormat", back), 0x26200a);
    EXPECT_EQ(pixel(back, 1, 0), 0xff00ff00u);
    // JPEG: 24bppRGB.
    ASSERT_EQ(save_to_stream(bmp, 1), Status::Ok);
    ASSERT_EQ(load_stream(stream_data, back), Status::Ok);
    EXPECT_EQ(query("GdipGetImagePixelFormat", back), 0x21808);
    // Save to a file.
    write_wide(NAME, u"D:\\out.png");
    clsid(enc, 6);
    write_guid(GUIDS, enc);
    ASSERT_EQ(call("GdipSaveImageToFile", {I(bmp), I(NAME), I(GUIDS), I(0)}), Status::Ok);
    EXPECT_TRUE(fs::exists(out_dir / "out.png"));
}

TEST_P(Gdip_codec_Test, EncoderParameterRulesFollowTheProbe) {
    ASSERT_EQ(call("GdipCreateBitmapFromScan0", {I(4), I(4), I(0), I(0x26200a), I(0), I(OUT)}), Status::Ok);
    const u64 bmp = read_handle(OUT);
    // Parameter list sizes (probe plist[*]): BMP none, JPEG 200/172, GIF 80/64, TIFF 184/164, PNG 40/32.
    const std::pair<u8, i32> sizes[] = {{1, x64() ? 200 : 172}, {2, x64() ? 80 : 64}, {5, x64() ? 184 : 164}, {6, x64() ? 40 : 32}};
    for (auto [n, size] : sizes) {
        clsid(enc, n);
        write_guid(GUIDS, enc);
        ASSERT_EQ(call("GdipGetEncoderParameterListSize", {I(bmp), I(GUIDS), I(OUT2)}), Status::Ok);
        EXPECT_EQ(read_int(OUT2), size);
        EXPECT_EQ(call("GdipGetEncoderParameterList", {I(bmp), I(GUIDS), I(static_cast<u64>(size - 1)), I(BIG)}), Status::InvalidParameter);
        ASSERT_EQ(call("GdipGetEncoderParameterList", {I(bmp), I(GUIDS), I(static_cast<u64>(size)), I(BIG)}), Status::Ok);
    }
    // TIFF list: Compression values 2 3 5 4 6 at offset 136 / 116.
    clsid(enc, 5);
    write_guid(GUIDS, enc);
    ASSERT_EQ(call("GdipGetEncoderParameterList", {I(bmp), I(GUIDS), I(x64() ? 184u : 164u), I(BIG)}), Status::Ok);
    EXPECT_EQ(read_int(BIG), 4);
    const u64 v = x64() ? memory.read_u64(BIG + 8 + 24).value() : memory.read_u32(BIG + 4 + 24).value();
    EXPECT_EQ(v - BIG, x64() ? 136u : 116u);
    EXPECT_EQ(read_int(static_cast<GuestAddr>(v) + 8), 5);
    clsid(enc, 0);
    write_guid(GUIDS, enc);
    EXPECT_EQ(call("GdipGetEncoderParameterListSize", {I(bmp), I(GUIDS), I(OUT2)}), Status::NotImplemented);
    const u8 bogus[16] = {0x78, 0x56, 0x34, 0x12};
    write_guid(GUIDS, bogus);
    EXPECT_EQ(call("GdipGetEncoderParameterListSize", {I(bmp), I(GUIDS), I(OUT2)}), Status::FileNotFound);
    EXPECT_EQ(call("GdipSaveImageToStream", {I(bmp), I(STREAM), I(GUIDS), I(0)}), Status::FileNotFound);
    EXPECT_EQ(call("GdipSaveImageToStream", {I(bmp), I(STREAM), I(0), I(0)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipSaveImageToStream", {I(bmp), I(0), I(GUIDS), I(0)}), Status::InvalidParameter);
    // JPEG quality must be one LONG; other encoders ignore it.
    one_param(kEncQuality, 3, 1, 50);
    EXPECT_EQ(save_to_stream(bmp, 1, PARAMS), Status::InvalidParameter);
    EXPECT_EQ(save_to_stream(bmp, 6, PARAMS), Status::Ok);
    one_param(kEncQuality, 4, 1, 50);
    EXPECT_EQ(save_to_stream(bmp, 1, PARAMS), Status::Ok);
    // Transformation from a non-JPEG source: InvalidParameter for every encoder.
    one_param(kEncTransformation, 4, 1, 13);
    for (u8 e : {0, 1, 2, 5, 6}) EXPECT_EQ(save_to_stream(bmp, e, PARAMS), Status::InvalidParameter);
    // Count 0: InvalidParameter.
    write_int(PARAMS, 0);
    EXPECT_EQ(save_to_stream(bmp, 0, PARAMS), Status::InvalidParameter);
}

TEST_P(Gdip_codec_Test, MultiFrameTiffSessionFollowsTheProbe) {
    ASSERT_EQ(call("GdipCreateBitmapFromScan0", {I(4), I(4), I(0), I(0x26200a), I(0), I(OUT)}), Status::Ok);
    const u64 bmp = read_handle(OUT);
    ASSERT_EQ(call("GdipCreateBitmapFromScan0", {I(4), I(4), I(0), I(0x21808), I(0), I(OUT)}), Status::Ok);
    const u64 f2 = read_handle(OUT);
    one_param(kEncSaveFlag, 4, 1, 20);
    EXPECT_EQ(call("GdipSaveAdd", {I(bmp), I(PARAMS)}), Status::Win32Error);  // no session
    EXPECT_EQ(call("GdipSaveAdd", {I(bmp), I(0)}), Status::InvalidParameter);
    one_param(kEncSaveFlag, 4, 1, 18);
    ASSERT_EQ(save_to_stream(bmp, 5, PARAMS), Status::Ok);
    write_int(PARAMS + 0x100, 23);   // FrameDimensionPage
    EXPECT_EQ(call("GdipSaveAddImage", {I(bmp), I(f2), I(PARAMS)}), Status::Ok);
    EXPECT_EQ(call("GdipSaveAddImage", {I(bmp), I(f2), I(PARAMS)}), Status::Ok);
    EXPECT_EQ(call("GdipSaveAddImage", {I(bmp), I(f2), I(0)}), Status::InvalidParameter);
    write_int(PARAMS + 0x100, 20);   // Flush
    ASSERT_EQ(call("GdipSaveAdd", {I(bmp), I(PARAMS)}), Status::Ok);
    EXPECT_EQ(call("GdipSaveAdd", {I(bmp), I(PARAMS)}), Status::Win32Error);
    u64 back = 0;
    ASSERT_EQ(load_stream(stream_data, back), Status::Ok);
    write_guid(GUIDS + 16, kFramePage);
    ASSERT_EQ(call("GdipImageGetFrameCount", {I(back), I(GUIDS + 16), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 3);   // probe multi[3] frames=3
    // A PNG session never opens (probe multi[4] add=7).
    one_param(kEncSaveFlag, 4, 1, 18);
    ASSERT_EQ(save_to_stream(bmp, 6, PARAMS), Status::Ok);
    write_int(PARAMS + 0x100, 23);
    EXPECT_EQ(call("GdipSaveAddImage", {I(bmp), I(f2), I(PARAMS)}), Status::Win32Error);
}

// ---- Miscellany ------------------------------------------------------------------------------------------------

TEST_P(Gdip_codec_Test, RawFormatThumbnailItemDataAndTestControl) {
    ASSERT_EQ(call("GdipCreateBitmapFromScan0", {I(4), I(4), I(0), I(0x26200a), I(0), I(OUT)}), Status::Ok);
    const u64 bmp = read_handle(OUT);
    ASSERT_EQ(call("GdipGetImageRawFormat", {I(bmp), I(BUF)}), Status::Ok);
    EXPECT_EQ(memory.read_u8(BUF).value(), 0xaa);  // ImageFormatMemoryBMP
    EXPECT_EQ(call("GdipGetImageRawFormat", {I(0), I(BUF)}), Status::InvalidParameter);
    const u64 png = must_load("rgb24.png");
    ASSERT_EQ(call("GdipGetImageRawFormat", {I(png), I(BUF)}), Status::Ok);
    EXPECT_EQ(memory.read_u8(BUF).value(), 0xaf);  // ImageFormatPNG
    ASSERT_EQ(call("GdipGetImageThumbnail", {I(png), I(0), I(0), I(OUT), I(0), I(0)}), Status::Ok);
    const u64 th = read_handle(OUT);
    EXPECT_EQ(query("GdipGetImageWidth", th), 120);
    EXPECT_EQ(query("GdipGetImagePixelFormat", th), 0xe200b);
    ASSERT_EQ(call("GdipGetImageThumbnail", {I(png), I(2), I(3), I(OUT), I(0), I(0)}), Status::Ok);
    EXPECT_EQ(query("GdipGetImageHeight", read_handle(OUT)), 3);
    // Item data (probe itemdata): BMP NotImplemented, PNG / memory InvalidParameter.
    EXPECT_EQ(call("GdipGetImageItemData", {I(must_load("rgb24.bmp")), I(BUF)}), Status::NotImplemented);
    EXPECT_EQ(call("GdipGetImageItemData", {I(png), I(BUF)}), Status::InvalidParameter);
    EXPECT_EQ(call("GdipGetImageItemData", {I(bmp), I(BUF)}), Status::InvalidParameter);
    // Test control (probe testcontrol).
    ASSERT_EQ(call("GdipTestControl", {I(2), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 10011);
    EXPECT_EQ(call("GdipTestControl", {I(0), I(OUT2)}), Status::Ok);
    EXPECT_EQ(call("GdipTestControl", {I(1), I(OUT2)}), Status::Ok);
    EXPECT_EQ(call("GdipTestControl", {I(3), I(OUT2)}), Status::InvalidParameter);
    // Scan0 bitmaps: one Page frame, no properties.
    EXPECT_EQ(query("GdipGetPropertyCount", bmp), 0);
    EXPECT_EQ(call("GdipGetPropertyIdList", {I(bmp), I(0), I(BUF)}), Status::Ok);
    write_guid(GUIDS, kFramePage);
    ASSERT_EQ(call("GdipImageGetFrameCount", {I(bmp), I(GUIDS), I(OUT2)}), Status::Ok);
    EXPECT_EQ(read_int(OUT2), 1);
}

INSTANTIATE_GDIP_BITNESS(Gdip_codec_Test);

}  // namespace

}  // namespace vitreo::winapi::gdip_test
