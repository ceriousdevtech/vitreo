#pragma once
/// @file gdi_vimage.hpp
/// @brief WS7: the GDI raster work Accelerate/vImage can do bit-exactly.
///
/// GDI keeps every surface as RGBA8 (SoftBitmap). The software loops in
/// bitmap_codec.cpp and raster_ops.cpp are the contract; this unit only
/// replaces a loop where a vImage primitive produces the same bytes for every
/// input. `classify()` is the one predicate that decides it: a total function
/// of the operation, raster op, formats, stretch/mirror, clip shape and blend
/// function. Every call site asks it and, when it answers Kernel::None, runs
/// its unchanged software loop.
///
/// Whole-surface conversions (DIB section sync, Get/SetDIBits, DDB bits and
/// the 24bpp DDB quantize) are keyed by (operation, direction, site, format,
/// row order): a row ships only if GdiVimageBench measured it at least 1.2x
/// faster than the software loop at 1920x1080, and only from its measured
/// crossover up; every other row answers software with its measured ratio.
///
/// Kill switch: VITREO_WS7=0 (read once) forces every call onto the software
/// path; tests and the A/B benchmark use ScopedForce instead.

#include <vitreo/common.hpp>

#include <cstddef>

namespace vitreo::winapi::gdi {
struct PixelFormat;
}

namespace vitreo::winapi::gdi::accel {

/// True unless VITREO_WS7=0 or a ScopedForce(false) is live.
[[nodiscard]] bool enabled() noexcept;

/// Force the lever on or off for the current process while alive; nests and
/// restores the previous state on destruction.
class ScopedForce {
public:
    explicit ScopedForce(bool on) noexcept;
    ~ScopedForce();
    ScopedForce(const ScopedForce&) = delete;
    ScopedForce& operator=(const ScopedForce&) = delete;

private:
    int m_previous;
};

enum class Kernel : u8 {
    None,
    Bgrx32,           ///< 32bpp BI_RGB <-> RGBA (channel permute)
    Bytes32,          ///< 32bpp BI_BITFIELDS with byte-aligned masks
    Bgr24,            ///< 24bpp <-> RGBA
    Pal8,             ///< 8bpp palette decode
    Pal4,             ///< 4bpp palette decode, byte-aligned run
    Pal1,             ///< 1bpp palette decode, byte-aligned run
    ClearFourth,      ///< 24bpp DDB quantize: byte 3 = 0
    CopyRgba,         ///< unscaled SRCCOPY between RGBA surfaces
    AlphaPremul,      ///< AlphaBlend AC_SRC_ALPHA, SourceConstantAlpha 255
    AlphaPremulConst, ///< AlphaBlend AC_SRC_ALPHA, SourceConstantAlpha < 255
    AlphaConst,       ///< AlphaBlend without AC_SRC_ALPHA
    Count
};

enum class Op : u8 { Decode, Encode, Quantize, Copy, DibToSurface, AlphaBlend, Count };

/// Where a whole-surface Decode/Encode runs (ignored by the other ops).
enum class Site : u8 {
    DibSection,  ///< DIB section sync from/to guest memory
    DibBits,     ///< SetDIBits / GetDIBits band
    DdbBits,     ///< CreateBitmap / SetBitmapBits / GetBitmapBits rows
};

struct Request {
    Op op = Op::Decode;
    const PixelFormat* format = nullptr;  ///< Decode/Encode/Quantize/DibToSurface
    Site site = Site::DibSection;         ///< Decode/Encode only
    bool bottom_up = false;               ///< Decode/Encode: scan lines run upward in memory
    u8 rop_index = 0xCC;                  ///< ROP3 bits 16..23
    bool stretched = false;               ///< |src extent| != |dst extent|
    bool mirrored = false;                ///< any negative extent
    bool complex_clip = false;            ///< clip valid with a rectangle list
    bool src_mono = false;
    bool dst_mono = false;
    bool pal_colors = false;              ///< DIB_PAL_COLORS source
    bool same_surface = false;            ///< AlphaBlend source == destination
    i32 stretch_mode = 3;                 ///< ignored unless stretched
    u8 blend_op = 0;
    u8 alpha_format = 0;
    u8 const_alpha = 255;
    i32 first = 0;                        ///< first pixel of a decoded row run
    i64 pixels = 0;                       ///< work size, compared with the crossover
};

struct Verdict {
    Kernel kernel = Kernel::None;
    const char* reason = "";
};

[[nodiscard]] Verdict classify(const Request& r) noexcept;

/// Number of times each kernel ran (relaxed counters, for tests).
[[nodiscard]] u64 kernel_runs(Kernel k) noexcept;
void reset_kernel_runs() noexcept;

/// The smallest work size (pixels) from which a kernel is used: below it the
/// software loop is at least as fast (measured crossover).
[[nodiscard]] i64 min_pixels(Kernel k) noexcept;

/// The threshold classify() applies to @p r's kernel: the conversion row's
/// own crossover for Decode/Encode/Quantize, else min_pixels(kernel). 0 when
/// @p r classifies as software.
[[nodiscard]] i64 min_pixels(const Request& r) noexcept;

/// For the crossover benchmark only: while alive, classify() ignores the
/// thresholds so the kernels can be timed below them.
class ScopedIgnoreCrossover {
public:
    ScopedIgnoreCrossover() noexcept;
    ~ScopedIgnoreCrossover();
    ScopedIgnoreCrossover(const ScopedIgnoreCrossover&) = delete;
    ScopedIgnoreCrossover& operator=(const ScopedIgnoreCrossover&) = delete;

private:
    bool m_previous;
};

/// For tests and the benchmark only: while alive, every whole-surface
/// conversion row that has a pixel-exact kernel uses it (threshold 1 pixel),
/// including the rows the 1.2x gate keeps on software, so they can be
/// proved pixel-exact and re-measured.
class ScopedIgnoreGate {
public:
    ScopedIgnoreGate() noexcept;
    ~ScopedIgnoreGate();
    ScopedIgnoreGate(const ScopedIgnoreGate&) = delete;
    ScopedIgnoreGate& operator=(const ScopedIgnoreGate&) = delete;

private:
    bool m_previous;
};

/// For the benchmark only: pin one op on or off while alive, over
/// ScopedForce and the kill switch, so one direction of a two-way operation
/// (DIB section sync) can be timed with the other held fixed.
class ScopedForceOp {
public:
    ScopedForceOp(Op op, bool on) noexcept;
    ~ScopedForceOp();
    ScopedForceOp(const ScopedForceOp&) = delete;
    ScopedForceOp& operator=(const ScopedForceOp&) = delete;

private:
    Op m_op;
    int m_previous;
};

// ---- Row codecs -------------------------------------------------------------

/// Decode @p rows scan lines of @p width pixels, starting at pixel @p first of
/// each, into RGBA. Source row r is at src + r*src_stride (src_stride may be
/// negative). @p force_opaque writes 0xFF into byte 3 (a window surface's
/// store rule); otherwise byte 3 is what gdi::decode_row leaves. Returns false
/// only if vImage reported an error; the caller then runs its software loop.
bool decode_rows(Kernel kernel, const PixelFormat& f, const u8* src, std::ptrdiff_t src_stride,
                 u8* rgba, usize rgba_stride, i32 width, i32 rows, i32 first, bool force_opaque);

/// Encode @p rows RGBA rows of @p width pixels into scan lines starting at
/// pixel 0 (Bgrx32, Bytes32, Bgr24). Bytes past width*Bpp in each row are
/// untouched.
bool encode_rows(Kernel kernel, const PixelFormat& f, const u8* rgba, usize rgba_stride, u8* dst,
                 std::ptrdiff_t dst_stride, i32 width, i32 rows);

/// The 24bpp DDB quantize: encode to 24bpp and decode back leaves byte 3 = 0.
bool clear_fourth_byte(u8* rgba, usize stride, i32 width, i32 rows);


// ---- Composites -------------------------------------------------------------

/// Copy width x rows RGBA pixels. @p overlap: src and dst share a surface and
/// may overlap; the result equals copying from a snapshot of the source.
/// @p force_opaque sets byte 3 of every written pixel to 0xFF.
bool copy_rgba_rect(const u8* src, usize src_stride, u8* dst, usize dst_stride, i32 width,
                    i32 rows, bool overlap, bool force_opaque);

/// AlphaBlend an unscaled rectangle onto dst in place. @p kernel is
/// AlphaPremul, AlphaPremulConst or AlphaConst from classify().
bool alpha_blend_rect(Kernel kernel, const u8* src, usize src_stride, u8* dst, usize dst_stride,
                      i32 width, i32 rows, u8 const_alpha);

}  // namespace vitreo::winapi::gdi::accel
