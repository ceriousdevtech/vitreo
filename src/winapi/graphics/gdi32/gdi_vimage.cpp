// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi_vimage.cpp
/// @brief WS7: eligibility table, crossover constants and vImage kernels.
///
/// Every formula a kernel relies on is quoted from the Accelerate SDK headers
/// (vImage.framework/Headers/Conversion.h, Alpha.h, Transform.h) and proved
/// against the software path by tests/winapi/gdi32/test_gdi_vimage_kernels.cpp
/// (exhaustively for the AlphaBlend arithmetic).

#include "winapi/graphics/gdi32/gdi_vimage.hpp"

#include "winapi/graphics/gdi32/bitmap_codec.hpp"

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

namespace vitreo::winapi::gdi::accel {

namespace {

std::atomic<int> g_force{-1};
std::atomic<bool> g_ignore_crossover{false};
std::atomic<bool> g_ignore_gate{false};
/// Per-op pin (ScopedForceOp): 0 none, 1 off, 2 on.
std::array<std::atomic<int>, static_cast<usize>(Op::Count)> g_op_force{};

std::array<std::atomic<u64>, static_cast<usize>(Kernel::Count)> g_runs{};

inline void count(Kernel k) noexcept {
    g_runs[static_cast<usize>(k)].fetch_add(1, std::memory_order_relaxed);
}

// Crossover: the smallest work size (pixels) from which a kernel is used.
// Rule: the smallest sampled n x n (1, 2, 4 .. 256) from which vImage is at
// least 10% faster at that size and at every larger sample, over every
// benchmark row that uses the kernel (GdiVimageBench.Crossover, median of 5
// interleaved reps, software ns / vImage ns):
//
//   row                        n=1   2     4     8     16    32    ...  256
//   Sync32Bitfields            1.03  1.13  1.38  1.81  3.00  5.19       16.97
//   SetDIBits32Bitfields       0.98  0.99  1.17  1.43  2.25  4.45       17.97
//   GetDIBits32Bitfields       1.23  1.37  1.72  2.47  3.60  5.65       15.52
//   StretchDIBits32BfBottomUp  1.49  2.00  3.56  7.88  19.2  44.4       218
//   StretchDIBits32TopDown     1.59  2.22  4.35  11.3  35.0  108        408
//   StretchDIBits32BottomUp    1.59  2.11  3.75  8.40  20.5  51.4       223
//   StretchDIBits24TopDown     1.53  2.17  4.22  11.2  34.3  99.3       248
//   StretchDIBits24BottomUp    1.51  2.06  3.69  7.95  19.5  45.4       216
//   StretchDIBits8TopDown      1.02  0.86  1.51  2.96  6.94  19.0       50.7
//   StretchDIBits8BottomUp     1.04  1.06  1.28  1.86  3.60  7.05       29.9
//   StretchDIBits4BottomUp     1.02  0.99  1.25  1.96  3.52  6.91       30.0
//   StretchDIBits1BottomUp     0.53  0.43  0.43  0.62  1.08  1.95       12.4
//   BitBltMem / Window / Scroll  >= 1.24 at every n
//   AlphaBlendPremul           1.01  1.37  2.49  7.91  17.5  26.6       30.4
//   AlphaBlendPremulConst      0.94  1.24  2.22  4.48  6.31  7.08       7.11
//   AlphaBlendConst            1.00  1.30  2.58  6.48  13.9  19.3       20.1
//
// These per-kernel thresholds serve the blits and AlphaBlend and the
// Bytes32 conversions; the other whole-surface conversions carry their own
// in kConversions. Every ratio not shown (n = 64, 128) is >= 3.6.
constexpr std::array<i64, static_cast<usize>(Kernel::Count)> kMinPixels = {
    0,        // None
    1,        // Bgrx32: n=1
    4 * 4,    // Bytes32: n=4 (SetDIBits32Bitfields)
    1,        // Bgr24: n=1
    4 * 4,    // Pal8: n=4 (StretchDIBits8)
    4 * 4,    // Pal4: n=4
    32 * 32,  // Pal1: n=32 (two expand passes cost more than the loop below)
    1,        // ClearFourth: only through kConversions (its own threshold)
    1,        // CopyRgba: n=1
    2 * 2,    // AlphaPremul: n=2
    2 * 2,    // AlphaPremulConst: n=2
    2 * 2,    // AlphaConst: n=2
};

/// Byte index k of a mask that is exactly 0xFF << 8k, else -1.
[[nodiscard]] int byte_of(u32 mask) noexcept {
    for (int k = 0; k < 4; ++k) {
        if (mask == (0xFFu << (8 * k))) return k;
    }
    return -1;
}

/// The three masks are distinct whole bytes.
[[nodiscard]] bool byte_masks(const PixelFormat& f) noexcept {
    const int r = byte_of(f.masks[0]), g = byte_of(f.masks[1]), b = byte_of(f.masks[2]);
    return r >= 0 && g >= 0 && b >= 0 && r != g && g != b && r != b;
}

constexpr Verdict none(const char* why) noexcept { return {Kernel::None, why}; }

[[nodiscard]] Verdict decode_kernel(const PixelFormat* f, i32 first) noexcept {
    if (f == nullptr) return none("no format");
    switch (f->bpp) {
    case 32:
        if (!f->bitfields) return {Kernel::Bgrx32, ""};
        return byte_masks(*f) ? Verdict{Kernel::Bytes32, ""} : none("non-byte bitfields");
    case 24: return {Kernel::Bgr24, ""};
    case 16: return none("vImage 5/6-bit widening rounds; GDI replicates");
    case 8: return {Kernel::Pal8, ""};
    case 4: return first % 2 == 0 ? Verdict{Kernel::Pal4, ""} : none("4bpp run not byte-aligned");
    case 1: return first % 8 == 0 ? Verdict{Kernel::Pal1, ""} : none("1bpp run not byte-aligned");
    default: return none("undecodable depth");
    }
}

[[nodiscard]] Verdict encode_kernel(const PixelFormat* f) noexcept {
    if (f == nullptr) return none("no format");
    switch (f->bpp) {
    case 32:
        if (!f->bitfields) return {Kernel::Bgrx32, ""};
        return byte_masks(*f) ? Verdict{Kernel::Bytes32, ""} : none("non-byte bitfields");
    case 24: return {Kernel::Bgr24, ""};
    case 16: return none("vImage narrowing rounds; GDI truncates");
    default: return none("nearest-palette search");
    }
}

[[nodiscard]] Verdict quantize_kernel(const PixelFormat* f) noexcept {
    if (f == nullptr) return none("no format");
    return f->bpp == 24 ? Verdict{Kernel::ClearFourth, ""}
                        : none("quantize needs bit replication or a palette");
}

/// One whole-surface conversion row of the 1.2x gate. @p software is null
/// when the row ships (from @p min_pixels up), else the measured reason it
/// stays on the software loop.
struct Conversion {
    Op op;
    Site site;
    Kernel kernel;
    bool bottom_up;
    i64 min_pixels;
    const char* software;
};

// The 1.2x gate (GdiVimageBench.Gate1080p, sw/vImage at 1920x1080, the
// range over four runs started at load <= 5.6; one at load 15 was
// discarded). A row ships when every run is >= 1.2x, from the smallest
// sampled n x n (1 .. 1024, then 1080p) at which the median ratio is
// >= 1.1x there and at every larger sample (GdiVimageBench.Crossover). The
// sync encode rows hold the decode on software in both arms, so their
// ratio is a lower bound. Software: the memory-bound BI_RGB 32bpp sync,
// every DDB decode, every 1bpp whole-surface decode, the 8bpp sync decode
// and the bottom-up BI_RGB sync rows (vImage goes a row at a time there).
// DDB rows are top-down by definition (no bottom-up row exists).
constexpr Conversion kConversions[] = {
    {Op::Decode, Site::DibSection, Kernel::Bgrx32, false, 0, "below 1.2x gate: SyncDecode32TopDown 0.98-1.05x at 1080p"},
    {Op::Decode, Site::DibSection, Kernel::Bgrx32, true, 0, "below 1.2x gate: SyncDecode32BottomUp 0.71-0.87x at 1080p"},
    {Op::Decode, Site::DibSection, Kernel::Bgr24, false, 1024 * 1024, nullptr},  // SyncDecode24TopDown: 1080p 1.29-1.44x, >= 1.1x from n=1024
    {Op::Decode, Site::DibSection, Kernel::Bgr24, true, 0, "below 1.2x gate: SyncDecode24BottomUp 0.83-0.87x at 1080p"},
    {Op::Decode, Site::DibSection, Kernel::Pal8, false, 0, "below 1.2x gate: SyncDecode8TopDown 1.11-1.20x at 1080p"},
    {Op::Decode, Site::DibSection, Kernel::Pal8, true, 0, "below 1.2x gate: SyncDecode8BottomUp 1.15-1.26x at 1080p"},
    {Op::Decode, Site::DibSection, Kernel::Pal4, false, 1920 * 1080, nullptr},  // SyncDecode4TopDown: 1080p 1.23-1.27x, >= 1.1x from 1080p
    {Op::Decode, Site::DibSection, Kernel::Pal4, true, 1024 * 1024, nullptr},  // SyncDecode4BottomUp: 1080p 1.29-1.36x, >= 1.1x from n=1024
    {Op::Decode, Site::DibSection, Kernel::Pal1, false, 0, "below 1.2x gate: SyncDecode1TopDown 1.15-1.19x at 1080p"},
    {Op::Decode, Site::DibSection, Kernel::Pal1, true, 0, "below 1.2x gate: SyncDecode1BottomUp 0.88-0.94x at 1080p"},
    {Op::Encode, Site::DibSection, Kernel::Bgrx32, false, 0, "below 1.2x gate: SyncEncode32TopDown 1.14-1.19x at 1080p"},
    {Op::Encode, Site::DibSection, Kernel::Bgrx32, true, 0, "below 1.2x gate: SyncEncode32BottomUp 1.03-1.07x at 1080p"},
    {Op::Encode, Site::DibSection, Kernel::Bgr24, false, 2 * 2, nullptr},  // SyncEncode24TopDown: 1080p 1.40-1.44x, >= 1.1x from n=2
    {Op::Encode, Site::DibSection, Kernel::Bgr24, true, 0, "below 1.2x gate: SyncEncode24BottomUp 1.12-1.16x at 1080p"},
    {Op::Decode, Site::DibBits, Kernel::Bgrx32, false, 4 * 4, nullptr},  // SetDIBits32TopDown: 1080p 2.41-3.14x, >= 1.1x from n=4
    {Op::Decode, Site::DibBits, Kernel::Bgrx32, true, 32 * 32, nullptr},  // SetDIBits32BottomUp: 1080p 2.00-2.06x, >= 1.1x from n=32
    {Op::Decode, Site::DibBits, Kernel::Bgr24, false, 16 * 16, nullptr},  // SetDIBits24TopDown: 1080p 3.35-3.69x, >= 1.1x from n=16
    {Op::Decode, Site::DibBits, Kernel::Bgr24, true, 64 * 64, nullptr},  // SetDIBits24BottomUp: 1080p 2.20-2.22x, >= 1.1x from n=64
    {Op::Decode, Site::DibBits, Kernel::Pal8, false, 128 * 128, nullptr},  // SetDIBits8TopDown: 1080p 1.30-1.33x, >= 1.1x from n=128
    {Op::Decode, Site::DibBits, Kernel::Pal8, true, 1024 * 1024, nullptr},  // SetDIBits8BottomUp: 1080p 1.22-1.32x, >= 1.1x from n=1024
    {Op::Decode, Site::DibBits, Kernel::Pal4, false, 64 * 64, nullptr},  // SetDIBits4TopDown: 1080p 1.36-1.42x, >= 1.1x from n=64
    {Op::Decode, Site::DibBits, Kernel::Pal4, true, 1024 * 1024, nullptr},  // SetDIBits4BottomUp: 1080p 1.44-1.53x, >= 1.1x from n=1024
    {Op::Decode, Site::DibBits, Kernel::Pal1, false, 0, "below 1.2x gate: SetDIBits1TopDown 1.19-1.31x at 1080p"},
    {Op::Decode, Site::DibBits, Kernel::Pal1, true, 0, "below 1.2x gate: SetDIBits1BottomUp 1.03-1.10x at 1080p"},
    {Op::Encode, Site::DibBits, Kernel::Bgrx32, false, 1 * 1, nullptr},  // GetDIBits32TopDown: 1080p 2.29-2.59x, >= 1.1x from n=1
    {Op::Encode, Site::DibBits, Kernel::Bgrx32, true, 1 * 1, nullptr},  // GetDIBits32BottomUp: 1080p 1.72-1.87x, >= 1.1x from n=1
    {Op::Encode, Site::DibBits, Kernel::Bgr24, false, 1 * 1, nullptr},  // GetDIBits24TopDown: 1080p 2.58-2.82x, >= 1.1x from n=1
    {Op::Encode, Site::DibBits, Kernel::Bgr24, true, 1 * 1, nullptr},  // GetDIBits24BottomUp: 1080p 1.63-1.74x, >= 1.1x from n=1
    {Op::Decode, Site::DdbBits, Kernel::Bgrx32, false, 0, "below 1.2x gate: CreateBitmap32 0.99-1.00x at 1080p"},
    {Op::Decode, Site::DdbBits, Kernel::Bgr24, false, 0, "below 1.2x gate: CreateBitmap24 0.99-1.04x at 1080p"},
    {Op::Decode, Site::DdbBits, Kernel::Pal8, false, 0, "below 1.2x gate: CreateBitmap8 1.09-1.18x at 1080p"},
    {Op::Decode, Site::DdbBits, Kernel::Pal4, false, 0, "below 1.2x gate: CreateBitmap4 1.16-1.23x at 1080p"},
    {Op::Decode, Site::DdbBits, Kernel::Pal1, false, 0, "below 1.2x gate: CreateBitmap1 1.12-1.18x at 1080p"},
    {Op::Encode, Site::DdbBits, Kernel::Bgrx32, false, 1 * 1, nullptr},  // GetBitmapBits32: 1080p 1.43-1.51x, >= 1.1x from n=1
    {Op::Encode, Site::DdbBits, Kernel::Bgr24, false, 2 * 2, nullptr},  // GetBitmapBits24: 1080p 1.41-1.68x, >= 1.1x from n=2
    {Op::Quantize, Site::DdbBits, Kernel::ClearFourth, false, 1 * 1, nullptr},  // Quantize24: 1080p 2.34-2.64x, >= 1.1x from n=1
};

/// Whole-surface conversions (DIB section sync, Get/SetDIBits, DDB bits,
/// DDB quantize) go through kConversions; byte bitfields (Bytes32) keep the
/// per-kernel crossover, its generic software loop is 15-25x slower
/// everywhere. A (op, site, kernel, row order) with no row answers software.
[[nodiscard]] const Conversion* conversion_row(const Request& r, Kernel k) noexcept {
    const bool any_site = r.op == Op::Quantize;
    for (const Conversion& c : kConversions) {
        if (c.op == r.op && c.kernel == k &&
            (any_site || (c.site == r.site && c.bottom_up == r.bottom_up))) {
            return &c;
        }
    }
    return nullptr;
}

[[nodiscard]] bool gated(const Request& r, Kernel k) noexcept {
    return (r.op == Op::Decode || r.op == Op::Encode || r.op == Op::Quantize) &&
           k != Kernel::Bytes32 && k != Kernel::None;
}

/// The conversion gate over a table verdict; @p threshold receives the
/// crossover in force for the answer.
[[nodiscard]] Verdict conversion_gate(const Request& r, Verdict v, i64& threshold) noexcept {
    threshold = v.kernel == Kernel::None ? 0 : kMinPixels[static_cast<usize>(v.kernel)];
    if (!gated(r, v.kernel)) return v;
    const Conversion* c = conversion_row(r, v.kernel);
    if (g_ignore_gate.load(std::memory_order_relaxed)) {
        threshold = 1;
        return v;
    }
    if (c == nullptr) {
        threshold = 0;
        return none("conversion row not measured");
    }
    if (c->software != nullptr) {
        threshold = 0;
        return none(c->software);
    }
    threshold = c->min_pixels;
    return v;
}

[[nodiscard]] Verdict table(const Request& r) noexcept {
    switch (r.op) {
    case Op::Decode: return decode_kernel(r.format, r.first);
    case Op::Encode: return encode_kernel(r.format);
    case Op::Quantize: return quantize_kernel(r.format);
    case Op::Copy:
    case Op::DibToSurface:
        if (r.rop_index != 0xCC) return none("rop is not SRCCOPY");
        if (r.stretched) return none("stretched: GDI centre sampling/DDA");
        if (r.mirrored) return none("mirrored");
        if (r.complex_clip) return none("complex clip region");
        if (r.dst_mono) return none("monochrome destination");
        if (r.op == Op::Copy) {
            if (r.src_mono) return none("monochrome source");
            return {Kernel::CopyRgba, ""};
        }
        if (r.pal_colors) return none("DIB_PAL_COLORS");
        return decode_kernel(r.format, r.first);
    case Op::AlphaBlend:
        if (r.blend_op != 0) return none("BlendOp is not AC_SRC_OVER");
        if (r.alpha_format > 1) return none("AlphaFormat > AC_SRC_ALPHA");
        if (r.stretched) return none("stretched");
        if (r.mirrored) return none("mirrored");
        if (r.complex_clip) return none("complex clip region");
        if (r.same_surface) return none("same surface");
        if (r.src_mono || r.dst_mono) return none("monochrome surface");
        if (r.alpha_format == 0) return {Kernel::AlphaConst, ""};
        return r.const_alpha == 255 ? Verdict{Kernel::AlphaPremul, ""}
                                    : Verdict{Kernel::AlphaPremulConst, ""};
    case Op::Count: break;
    }
    return none("unknown op");
}

}  // namespace

bool enabled() noexcept {
    static const bool env = environment_flag_enabled("VITREO_WS7", true);
    const int force = g_force.load(std::memory_order_relaxed);
    return force < 0 ? env : force != 0;
}

namespace {

[[nodiscard]] bool op_enabled(Op op) noexcept {
    const usize i = static_cast<usize>(op);
    const int pin = i < g_op_force.size() ? g_op_force[i].load(std::memory_order_relaxed) : 0;
    return pin == 0 ? enabled() : pin == 2;
}

/// classify() with the threshold it applied.
[[nodiscard]] Verdict classify_with(const Request& r, i64& threshold) noexcept {
    threshold = 0;
    if (!op_enabled(r.op)) return none("VITREO_WS7=0");
    const Verdict v = conversion_gate(r, table(r), threshold);
    if (v.kernel == Kernel::None) return v;
    if (r.pixels < threshold &&
        !(r.pixels > 0 && g_ignore_crossover.load(std::memory_order_relaxed))) {
        return none("below crossover");
    }
    return v;
}

}  // namespace

ScopedForce::ScopedForce(bool on) noexcept
    : m_previous(g_force.exchange(on ? 1 : 0, std::memory_order_relaxed)) {}

ScopedForce::~ScopedForce() { g_force.store(m_previous, std::memory_order_relaxed); }

Verdict classify(const Request& r) noexcept {
    i64 threshold = 0;
    return classify_with(r, threshold);
}

i64 min_pixels(const Request& r) noexcept {
    Request big = r;
    big.pixels = INT64_MAX;
    i64 threshold = 0;
    return classify_with(big, threshold).kernel == Kernel::None ? 0 : threshold;
}

u64 kernel_runs(Kernel k) noexcept {
    return g_runs[static_cast<usize>(k)].load(std::memory_order_relaxed);
}

void reset_kernel_runs() noexcept {
    for (auto& c : g_runs) c.store(0, std::memory_order_relaxed);
}

i64 min_pixels(Kernel k) noexcept { return kMinPixels[static_cast<usize>(k)]; }

ScopedIgnoreCrossover::ScopedIgnoreCrossover() noexcept
    : m_previous(g_ignore_crossover.exchange(true, std::memory_order_relaxed)) {}

ScopedIgnoreCrossover::~ScopedIgnoreCrossover() {
    g_ignore_crossover.store(m_previous, std::memory_order_relaxed);
}

ScopedIgnoreGate::ScopedIgnoreGate() noexcept
    : m_previous(g_ignore_gate.exchange(true, std::memory_order_relaxed)) {}

ScopedIgnoreGate::~ScopedIgnoreGate() { g_ignore_gate.store(m_previous, std::memory_order_relaxed); }

ScopedForceOp::ScopedForceOp(Op op, bool on) noexcept
    : m_op(op),
      m_previous(g_op_force[static_cast<usize>(op)].exchange(on ? 2 : 1, std::memory_order_relaxed)) {}

ScopedForceOp::~ScopedForceOp() {
    g_op_force[static_cast<usize>(m_op)].store(m_previous, std::memory_order_relaxed);
}

// =============================================================================
// Row codecs
// =============================================================================

namespace {

/// Rows per band for the two-pass palette kernels: the intermediate stays in
/// cache, and each pass still runs over many rows (vImage tiles a band
/// across threads).
constexpr i32 kBandRows = 32;

[[nodiscard]] vImage_Buffer buffer(const void* data, i32 width, i32 rows, usize stride) noexcept {
    vImage_Buffer b;
    b.data = const_cast<void*>(data);
    b.height = static_cast<vImagePixelCount>(rows);
    b.width = static_cast<vImagePixelCount>(width);
    b.rowBytes = stride;
    return b;
}

/// Scratch rows owned by the calling thread (vImage's own worker threads
/// only touch the buffers handed to them).
[[nodiscard]] u8* scratch(std::vector<u8>& v, usize bytes) {
    if (v.size() < bytes) v.resize(bytes);
    return v.data();
}

/// Decode-side byte-3 rule: 0 (gdi::decode_row) or 0xFF (window surface).
[[nodiscard]] inline u8 alpha_of(bool force_opaque) noexcept { return force_opaque ? 0xFF : 0x00; }

/// Bytes32 decode: RGBA byte c comes from source byte (index of mask c);
/// byte 3 is inserted.
bool decode_bytes32(const PixelFormat& f, const vImage_Buffer& src, const vImage_Buffer& dst,
                    bool force_opaque, vImage_Flags flags) {
    const uint8_t map[4] = {static_cast<uint8_t>(byte_of(f.masks[0])),
                            static_cast<uint8_t>(byte_of(f.masks[1])),
                            static_cast<uint8_t>(byte_of(f.masks[2])), 0};
    const Pixel_8888 bg = {0, 0, 0, alpha_of(force_opaque)};
    return vImagePermuteChannelsWithMaskedInsert_ARGB8888(&src, &dst, map, 0x1, bg, flags) ==
           kvImageNoError;
}

/// The Pal8 table: entry i laid out so bytes 1..3 are R, G, B
/// (vImageLookupTable_Planar8toPlanar24), 0 past the palette (as
/// gdi::decode_row's put_index).
void palette_table(const PixelFormat& f, uint32_t (&table)[256]) noexcept {
    for (u32 i = 0; i < 256; ++i) {
        const u32 c = i < f.palette.size() ? f.palette[i] : 0u;
        const u32 r = (c >> 16) & 0xFFu, g = (c >> 8) & 0xFFu, b = c & 0xFFu;
        table[i] = (r << 8) | (g << 16) | (b << 24);
    }
}

/// Indices (one byte per pixel) -> RGBA through the palette, in bands.
bool indices_to_rgba(const uint32_t (&table)[256], const vImage_Buffer& idx,
                     const vImage_Buffer& dst, bool force_opaque, vImage_Flags flags,
                     std::vector<u8>& rgb_scratch) {
    const usize rgb_stride = static_cast<usize>(dst.width) * 3u;
    u8* rgb = scratch(rgb_scratch, rgb_stride * static_cast<usize>(std::min<vImagePixelCount>(
                                                    dst.height, kBandRows)));
    for (vImagePixelCount y = 0; y < dst.height; y += kBandRows) {
        const i32 n = static_cast<i32>(std::min<vImagePixelCount>(kBandRows, dst.height - y));
        const vImage_Buffer in = buffer(static_cast<const u8*>(idx.data) + y * idx.rowBytes,
                                        static_cast<i32>(dst.width), n, idx.rowBytes);
        const vImage_Buffer mid = buffer(rgb, static_cast<i32>(dst.width), n, rgb_stride);
        const vImage_Buffer out = buffer(static_cast<u8*>(dst.data) + y * dst.rowBytes,
                                         static_cast<i32>(dst.width), n, dst.rowBytes);
        if (vImageLookupTable_Planar8toPlanar24(&in, &mid, table, flags) != kvImageNoError) {
            return false;
        }
        if (vImageConvert_RGB888toRGBA8888(&mid, nullptr, alpha_of(force_opaque), &out, false,
                                           flags) != kvImageNoError) {
            return false;
        }
    }
    return true;
}

/// One block of rows with a positive source stride.
bool decode_block(Kernel kernel, const PixelFormat& f, const u8* src, usize src_stride, u8* rgba,
                  usize rgba_stride, i32 width, i32 rows, i32 first, bool force_opaque,
                  vImage_Flags flags) {
    thread_local std::vector<u8> idx_scratch;
    thread_local std::vector<u8> rgb_scratch;
    const vImage_Buffer dst = buffer(rgba, width, rows, rgba_stride);
    switch (kernel) {
    case Kernel::Bgrx32: {
        // RGBA = (B,G,R,X)[2,1,0,3] (gdi::decode_row 32bpp BI_RGB keeps X).
        const vImage_Buffer in = buffer(src + static_cast<usize>(first) * 4u, width, rows, src_stride);
        const uint8_t map[4] = {2, 1, 0, 3};
        if (force_opaque) {
            const Pixel_8888 bg = {0, 0, 0, 0xFF};
            return vImagePermuteChannelsWithMaskedInsert_ARGB8888(&in, &dst, map, 0x1, bg,
                                                                  flags) == kvImageNoError;
        }
        return vImagePermuteChannels_ARGB8888(&in, &dst, map, flags) == kvImageNoError;
    }
    case Kernel::Bytes32: {
        const vImage_Buffer in = buffer(src + static_cast<usize>(first) * 4u, width, rows, src_stride);
        return decode_bytes32(f, in, dst, force_opaque, flags);
    }
    case Kernel::Bgr24: {
        const vImage_Buffer in = buffer(src + static_cast<usize>(first) * 3u, width, rows, src_stride);
        return vImageConvert_BGR888toRGBA8888(&in, nullptr, alpha_of(force_opaque), &dst, false,
                                              flags) == kvImageNoError;
    }
    case Kernel::Pal8: {
        uint32_t table[256];
        palette_table(f, table);
        const vImage_Buffer in = buffer(src + first, width, rows, src_stride);
        return indices_to_rgba(table, in, dst, force_opaque, flags, rgb_scratch);
    }
    case Kernel::Pal4:
    case Kernel::Pal1: {
        uint32_t table[256];
        palette_table(f, table);
        // Expand the packed indices to one byte each (identity colours;
        // the headers' big-endian bit order is the DIB's), then Pal8.
        static constexpr Pixel_8 kIdentity[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                                                  8, 9, 10, 11, 12, 13, 14, 15};
        const bool four = kernel == Kernel::Pal4;
        const u8* run = src + (four ? first / 2 : first / 8);
        const usize idx_stride = static_cast<usize>(width);
        u8* idx = scratch(idx_scratch, idx_stride * static_cast<usize>(std::min(rows, kBandRows)));
        for (i32 y = 0; y < rows; y += kBandRows) {
            const i32 n = std::min(kBandRows, rows - y);
            const vImage_Buffer packed =
                buffer(run + static_cast<usize>(y) * src_stride, width, n, src_stride);
            const vImage_Buffer planar = buffer(idx, width, n, idx_stride);
            const vImage_Error e = four ? vImageConvert_Indexed4toPlanar8(&packed, &planar, kIdentity, flags)
                                        : vImageConvert_Indexed1toPlanar8(&packed, &planar, kIdentity, flags);
            if (e != kvImageNoError) return false;
            const vImage_Buffer out =
                buffer(rgba + static_cast<usize>(y) * rgba_stride, width, n, rgba_stride);
            if (!indices_to_rgba(table, planar, out, force_opaque, flags, rgb_scratch)) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

bool encode_block(Kernel kernel, const PixelFormat& f, const u8* rgba, usize rgba_stride, u8* dst,
                  usize dst_stride, i32 width, i32 rows, vImage_Flags flags) {
    const vImage_Buffer in = buffer(rgba, width, rows, rgba_stride);
    switch (kernel) {
    case Kernel::Bgrx32: {
        // (B,G,R,X) = RGBA[2,1,0,3]: gdi::encode_row 32bpp BI_RGB stores
        // byte 3 as it is.
        const vImage_Buffer out = buffer(dst, width, rows, dst_stride);
        const uint8_t map[4] = {2, 1, 0, 3};
        return vImagePermuteChannels_ARGB8888(&in, &out, map, flags) == kvImageNoError;
    }
    case Kernel::Bgr24: {
        const vImage_Buffer out = buffer(dst, width, rows, dst_stride);
        return vImageConvert_RGBA8888toBGR888(&in, &out, flags) == kvImageNoError;
    }
    case Kernel::Bytes32: {
        // Output byte of mask c takes RGBA byte c; the byte no mask covers
        // is 0 (gdi::encode_row stores only the masked bits).
        const vImage_Buffer out = buffer(dst, width, rows, dst_stride);
        uint8_t map[4] = {0, 0, 0, 0};
        uint8_t covered = 0;
        for (u8 c = 0; c < 3; ++c) {
            const int k = byte_of(f.masks[c]);
            map[k] = c;
            covered |= static_cast<uint8_t>(0x8u >> k);
        }
        const Pixel_8888 bg = {0, 0, 0, 0};
        return vImagePermuteChannelsWithMaskedInsert_ARGB8888(
                   &in, &out, map, static_cast<uint8_t>(~covered & 0xFu), bg, flags) == kvImageNoError;
    }
    default:
        return false;
    }
}

}  // namespace

bool decode_rows(Kernel kernel, const PixelFormat& f, const u8* src, std::ptrdiff_t src_stride,
                 u8* rgba, usize rgba_stride, i32 width, i32 rows, i32 first, bool force_opaque) {
    if (width <= 0 || rows <= 0) return true;
    bool ok = true;
    if (src_stride >= 0) {
        ok = decode_block(kernel, f, src, static_cast<usize>(src_stride), rgba, rgba_stride, width,
                          rows, first, force_opaque, kvImageNoFlags);
    } else {
        // vImage_Buffer.rowBytes is unsigned: a bottom-up image goes a row
        // at a time.
        const usize step = static_cast<usize>(-src_stride);
        for (i32 y = 0; y < rows && ok; ++y) {
            ok = decode_block(kernel, f, src + y * src_stride, step,
                              rgba + static_cast<usize>(y) * rgba_stride, rgba_stride, width, 1,
                              first, force_opaque, kvImageDoNotTile);
        }
    }
    if (ok) count(kernel);
    return ok;
}

bool encode_rows(Kernel kernel, const PixelFormat& f, const u8* rgba, usize rgba_stride, u8* dst,
                 std::ptrdiff_t dst_stride, i32 width, i32 rows) {
    if (width <= 0 || rows <= 0) return true;
    bool ok = true;
    if (dst_stride >= 0) {
        ok = encode_block(kernel, f, rgba, rgba_stride, dst, static_cast<usize>(dst_stride), width,
                          rows, kvImageNoFlags);
    } else {
        const usize step = static_cast<usize>(-dst_stride);
        for (i32 y = 0; y < rows && ok; ++y) {
            ok = encode_block(kernel, f, rgba + static_cast<usize>(y) * rgba_stride, rgba_stride,
                              dst + y * dst_stride, step, width, 1, kvImageDoNotTile);
        }
    }
    if (ok) count(kernel);
    return ok;
}

bool clear_fourth_byte(u8* rgba, usize stride, i32 width, i32 rows) {
    if (width <= 0 || rows <= 0) return true;
    const vImage_Buffer b = buffer(rgba, width, rows, stride);
    const bool ok = vImageOverwriteChannelsWithScalar_ARGB8888(0, &b, &b, 0x1, kvImageNoFlags) ==
                    kvImageNoError;
    if (ok) count(Kernel::ClearFourth);
    return ok;
}

// =============================================================================
// Composites
// =============================================================================

bool copy_rgba_rect(const u8* src, usize src_stride, u8* dst, usize dst_stride, i32 width,
                    i32 rows, bool overlap, bool force_opaque) {
    if (width <= 0 || rows <= 0) return true;
    const usize bytes = static_cast<usize>(width) * 4u;
    if (!overlap) {
        for (i32 y = 0; y < rows; ++y) {
            std::memcpy(dst + static_cast<usize>(y) * dst_stride,
                        src + static_cast<usize>(y) * src_stride, bytes);
        }
    } else if (dst > src) {
        // Destination below/right of the source: last row first, so every
        // source row is read before a destination row covers it (the
        // software path reads a snapshot).
        for (i32 y = rows - 1; y >= 0; --y) {
            std::memmove(dst + static_cast<usize>(y) * dst_stride,
                         src + static_cast<usize>(y) * src_stride, bytes);
        }
    } else {
        for (i32 y = 0; y < rows; ++y) {
            std::memmove(dst + static_cast<usize>(y) * dst_stride,
                         src + static_cast<usize>(y) * src_stride, bytes);
        }
    }
    if (force_opaque) {
        const vImage_Buffer b = buffer(dst, width, rows, dst_stride);
        if (vImageOverwriteChannelsWithScalar_ARGB8888(0xFF, &b, &b, 0x1, kvImageNoFlags) !=
            kvImageNoError) {
            return false;
        }
    }
    count(Kernel::CopyRgba);
    return true;
}

bool alpha_blend_rect(Kernel kernel, const u8* src, usize src_stride, u8* dst, usize dst_stride,
                      i32 width, i32 rows, u8 const_alpha) {
    if (width <= 0 || rows <= 0) return true;
    const vImage_Buffer top = buffer(src, width, rows, src_stride);
    const vImage_Buffer bottom = buffer(dst, width, rows, dst_stride);
    bool ok = false;
    switch (kernel) {
    case Kernel::AlphaPremul:
        // Alpha.h: c = top + ((255 - topA) * bottom + 127) / 255, the
        // per-pixel branch of alpha_blend_checked with SCA = 255.
        ok = vImagePremultipliedAlphaBlend_RGBA8888(&top, &bottom, &bottom, kvImageNoFlags) ==
             kvImageNoError;
        break;
    case Kernel::AlphaPremulConst: {
        // Stage 1 (Transform.h): s'_c = (s_c * SCA + 127) / 255 on all four
        // bytes, a diagonal matrix with divisor 255 and post-bias 127;
        // stage 2 is the AlphaPremul blend of s' over the destination.
        thread_local std::vector<u8> premul_scratch;
        const i16 a = const_alpha;
        const int16_t matrix[16] = {a, 0, 0, 0, 0, a, 0, 0, 0, 0, a, 0, 0, 0, 0, a};
        const int32_t post_bias[4] = {127, 127, 127, 127};
        const usize stride = static_cast<usize>(width) * 4u;
        u8* mid = scratch(premul_scratch, stride * static_cast<usize>(std::min(rows, kBandRows)));
        ok = true;
        for (i32 y = 0; y < rows && ok; y += kBandRows) {
            const i32 n = std::min(kBandRows, rows - y);
            const vImage_Buffer in = buffer(src + static_cast<usize>(y) * src_stride, width, n, src_stride);
            const vImage_Buffer sp = buffer(mid, width, n, stride);
            const vImage_Buffer out = buffer(dst + static_cast<usize>(y) * dst_stride, width, n, dst_stride);
            ok = vImageMatrixMultiply_ARGB8888(&in, &sp, matrix, 255, nullptr, post_bias,
                                               kvImageNoFlags) == kvImageNoError &&
                 vImagePremultipliedAlphaBlend_RGBA8888(&sp, &out, &out, kvImageNoFlags) ==
                     kvImageNoError;
        }
        break;
    }
    case Kernel::AlphaConst: {
        // Alpha.h with srcTopAlpha = 255 on every byte:
        // (t*SCA*255 + (65025 - 255*SCA)*b + 127*255) / 65025
        //   = (t*SCA + (255 - SCA)*b + 127) / 255, the constant-only branch.
        // Each row is viewed as Planar8 of 4*width bytes (every byte blends).
        thread_local std::vector<u8> ones;
        const usize plane = static_cast<usize>(width) * 4u;
        const i32 band = std::min(rows, kBandRows);
        if (ones.size() < plane * static_cast<usize>(band)) ones.assign(plane * static_cast<usize>(band), 0xFF);
        ok = true;
        for (i32 y = 0; y < rows && ok; y += kBandRows) {
            const i32 n = std::min(kBandRows, rows - y);
            const vImage_Buffer t = buffer(src + static_cast<usize>(y) * src_stride,
                                           static_cast<i32>(plane), n, src_stride);
            const vImage_Buffer ta = buffer(ones.data(), static_cast<i32>(plane), n, plane);
            const vImage_Buffer b = buffer(dst + static_cast<usize>(y) * dst_stride,
                                           static_cast<i32>(plane), n, dst_stride);
            ok = vImagePremultipliedConstAlphaBlend_Planar8(&t, const_alpha, &ta, &b, &b,
                                                            kvImageNoFlags) == kvImageNoError;
        }
        break;
    }
    default:
        return false;
    }
    if (ok) count(kernel);
    return ok;
}

}  // namespace vitreo::winapi::gdi::accel
