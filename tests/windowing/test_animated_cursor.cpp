// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS
//
// RIFF/ACON animated cursor decoding.
//
// The Sims 3 hands its loading cursor to CreateIconFromResource as a whole
// .ani file. Vitreo decoded only the RT_CURSOR layout, so the call returned
// NULL and the game fell back to LoadCursor(NULL, IDC_WAIT) -- which is why
// the application's animated loading cursor did not appear.

#include "windowing/resources/resource_walker.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace {

using namespace vitreo;
using namespace vitreo::windowing;

void put_u16(std::vector<u8>& out, u16 v) {
    out.push_back(static_cast<u8>(v & 0xFF));
    out.push_back(static_cast<u8>(v >> 8));
}
void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>((v >> (i * 8)) & 0xFF));
}
void put_tag(std::vector<u8>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

/// A 2x2 32bpp cursor DIB (XOR rows then a 1bpp AND mask), bottom-up, with
/// every pixel set to @p value in B/G/R and opaque alpha.
std::vector<u8> make_dib(u8 value) {
    std::vector<u8> dib;
    put_u32(dib, 40);            // biSize
    put_u32(dib, 2);             // biWidth
    put_u32(dib, 4);             // biHeight = 2 * visual height
    put_u16(dib, 1);             // biPlanes
    put_u16(dib, 32);            // biBitCount
    for (int i = 0; i < 6; ++i) put_u32(dib, 0);  // rest of BITMAPINFOHEADER
    for (int i = 0; i < 4; ++i) {                 // XOR: 4 BGRA pixels
        dib.push_back(value);
        dib.push_back(value);
        dib.push_back(value);
        dib.push_back(0xFF);
    }
    for (int row = 0; row < 2; ++row) {           // AND: 2 rows, DWORD aligned
        for (int i = 0; i < 4; ++i) dib.push_back(0);
    }
    return dib;
}

/// Wrap a DIB as a single-image .cur file with the given hotspot.
std::vector<u8> make_cur(u8 value, u16 hot_x, u16 hot_y) {
    const std::vector<u8> dib = make_dib(value);
    std::vector<u8> cur;
    put_u16(cur, 0);   // idReserved
    put_u16(cur, 2);   // idType = cursor
    put_u16(cur, 1);   // idCount
    cur.push_back(2);  // bWidth
    cur.push_back(2);  // bHeight
    cur.push_back(0);  // bColorCount
    cur.push_back(0);  // bReserved
    put_u16(cur, hot_x);
    put_u16(cur, hot_y);
    put_u32(cur, static_cast<u32>(dib.size()));
    put_u32(cur, 22);  // dwImageOffset
    cur.insert(cur.end(), dib.begin(), dib.end());
    return cur;
}

void put_chunk(std::vector<u8>& out, const char* tag, const std::vector<u8>& body) {
    put_tag(out, tag);
    put_u32(out, static_cast<u32>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    if (body.size() & 1u) out.push_back(0);  // RIFF chunks are WORD aligned
}

struct AniOptions {
    std::vector<u32> rates;      // per step, jiffies; empty = use default
    std::vector<u32> sequence;   // step -> frame; empty = no `seq ` chunk
    u32 default_rate = 6;        // jiffies
};

std::vector<u8> make_ani(const std::vector<u8>& frame_values,
                         const AniOptions& opts = {}) {
    std::vector<u8> anih;
    put_u32(anih, 36);                                       // cbSize
    put_u32(anih, static_cast<u32>(frame_values.size()));     // nFrames
    put_u32(anih, opts.sequence.empty()
                      ? static_cast<u32>(frame_values.size())
                      : static_cast<u32>(opts.sequence.size()));  // nSteps
    put_u32(anih, 2);                                        // iWidth
    put_u32(anih, 2);                                        // iHeight
    put_u32(anih, 32);                                       // iBitCount
    put_u32(anih, 1);                                        // nPlanes
    put_u32(anih, opts.default_rate);                        // iDispRate
    put_u32(anih, 0x1u | (opts.sequence.empty() ? 0u : 0x2u));  // AF_ICON|AF_SEQUENCE

    std::vector<u8> fram;
    put_tag(fram, "fram");
    for (u8 v : frame_values) {
        put_chunk(fram, "icon", make_cur(v, 1, 1));
    }

    std::vector<u8> body;
    put_tag(body, "ACON");
    put_chunk(body, "anih", anih);
    if (!opts.rates.empty()) {
        std::vector<u8> rate;
        for (u32 r : opts.rates) put_u32(rate, r);
        put_chunk(body, "rate", rate);
    }
    if (!opts.sequence.empty()) {
        std::vector<u8> seq;
        for (u32 sq : opts.sequence) put_u32(seq, sq);
        put_chunk(body, "seq ", seq);
    }
    put_chunk(body, "LIST", fram);

    std::vector<u8> ani;
    put_tag(ani, "RIFF");
    put_u32(ani, static_cast<u32>(body.size()));
    ani.insert(ani.end(), body.begin(), body.end());
    return ani;
}

TEST(AnimatedCursor, DecodesEveryFrameWithGeometryAndHotspot) {
    const std::vector<u8> ani = make_ani({0x10, 0x20, 0x30});
    const DecodedAnimatedCursor out =
        decode_ani_cursor(ani.data(), static_cast<u32>(ani.size()));

    ASSERT_EQ(out.frames.size(), 3u);
    EXPECT_EQ(out.width, 2u);
    EXPECT_EQ(out.height, 2u);
    EXPECT_EQ(out.hotspot_x, 1u);
    EXPECT_EQ(out.hotspot_y, 1u);
    for (const auto& frame : out.frames) {
        EXPECT_EQ(frame.size(), 2u * 2u * 4u);
    }
    // Frames must stay distinct and in order: reusing one image for all steps
    // would animate nothing while still looking like a working decode.
    EXPECT_EQ(out.frames[0][0], 0x10);
    EXPECT_EQ(out.frames[1][0], 0x20);
    EXPECT_EQ(out.frames[2][0], 0x30);
}

TEST(AnimatedCursor, DefaultRateAppliesWhenNoRateChunk) {
    AniOptions opts;
    opts.default_rate = 6;  // 6 jiffies = 100 ms
    const std::vector<u8> ani = make_ani({0x10, 0x20}, opts);
    const DecodedAnimatedCursor out =
        decode_ani_cursor(ani.data(), static_cast<u32>(ani.size()));

    ASSERT_EQ(out.delays_ms.size(), 2u);
    EXPECT_EQ(out.delays_ms[0], 100u);
    EXPECT_EQ(out.delays_ms[1], 100u);
}

TEST(AnimatedCursor, PerStepRatesAreHonoured) {
    AniOptions opts;
    opts.rates = {3, 12};  // 50 ms, 200 ms
    const std::vector<u8> ani = make_ani({0x10, 0x20}, opts);
    const DecodedAnimatedCursor out =
        decode_ani_cursor(ani.data(), static_cast<u32>(ani.size()));

    ASSERT_EQ(out.delays_ms.size(), 2u);
    EXPECT_EQ(out.delays_ms[0], 50u);
    EXPECT_EQ(out.delays_ms[1], 200u);
}

TEST(AnimatedCursor, SequenceChunkExpandsIntoDisplayOrder) {
    AniOptions opts;
    opts.sequence = {0, 1, 0};  // frame 0 shown twice
    const std::vector<u8> ani = make_ani({0x10, 0x20}, opts);
    const DecodedAnimatedCursor out =
        decode_ani_cursor(ani.data(), static_cast<u32>(ani.size()));

    ASSERT_EQ(out.frames.size(), 3u);
    EXPECT_EQ(out.frames[0][0], 0x10);
    EXPECT_EQ(out.frames[1][0], 0x20);
    EXPECT_EQ(out.frames[2][0], 0x10);
}

TEST(AnimatedCursor, ZeroRateBecomesOneJiffyNotZero) {
    AniOptions opts;
    opts.rates = {0, 0};
    const std::vector<u8> ani = make_ani({0x10, 0x20}, opts);
    const DecodedAnimatedCursor out =
        decode_ani_cursor(ani.data(), static_cast<u32>(ani.size()));

    ASSERT_EQ(out.delays_ms.size(), 2u);
    // A zero delay would spin the host timer at full speed.
    EXPECT_GT(out.delays_ms[0], 0u);
    EXPECT_GT(out.delays_ms[1], 0u);
}

TEST(AnimatedCursor, RejectsNonAconData) {
    // A plain RT_CURSOR must not be mistaken for an animated one — the
    // caller falls through to the still decoder on an empty result.
    const std::vector<u8> dib = make_dib(0x55);
    std::vector<u8> rt{1, 0, 1, 0};
    rt.insert(rt.end(), dib.begin(), dib.end());
    EXPECT_TRUE(
        decode_ani_cursor(rt.data(), static_cast<u32>(rt.size())).frames.empty());

    const std::vector<u8> junk{'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'};
    EXPECT_TRUE(
        decode_ani_cursor(junk.data(), static_cast<u32>(junk.size())).frames.empty());
    EXPECT_TRUE(decode_ani_cursor(nullptr, 0).frames.empty());
}

TEST(AnimatedCursor, TruncatedAniDoesNotOverrun) {
    const std::vector<u8> ani = make_ani({0x10, 0x20, 0x30});
    // Every prefix must be refused or partially decoded, never read past end.
    for (u32 cut = 12; cut < ani.size(); cut += 3) {
        const DecodedAnimatedCursor out = decode_ani_cursor(ani.data(), cut);
        for (const auto& frame : out.frames) {
            EXPECT_EQ(frame.size(), 2u * 2u * 4u) << "cut=" << cut;
        }
    }
}

TEST(CurFile, DecodesHotspotFromDirectoryEntry) {
    // The hotspot lives in the ICONDIRENTRY, not in a 4-byte prefix — reading
    // it the RT_CURSOR way yields garbage coordinates.
    const std::vector<u8> cur = make_cur(0x77, 1, 0);
    const DecodedCursor out =
        decode_cur_file(cur.data(), static_cast<u32>(cur.size()));

    ASSERT_FALSE(out.rgba.empty());
    EXPECT_EQ(out.width, 2u);
    EXPECT_EQ(out.height, 2u);
    EXPECT_EQ(out.hotspot_x, 1u);
    EXPECT_EQ(out.hotspot_y, 0u);
}

TEST(CurFile, RejectsPngCompressedEntry) {
    std::vector<u8> cur = make_cur(0x11, 0, 0);
    static const u8 kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(cur.data() + 22, kPng, sizeof(kPng));
    // Decoding a PNG header as a BITMAPINFOHEADER would produce a plausible
    // but wrong image rather than an honest failure.
    EXPECT_TRUE(
        decode_cur_file(cur.data(), static_cast<u32>(cur.size())).rgba.empty());
}

} // namespace
