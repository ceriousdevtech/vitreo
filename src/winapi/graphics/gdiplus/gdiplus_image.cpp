// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_image.cpp
/// @brief GDI+ image area: bitmaps (creation, pixels, LockBits, palettes,
/// conversion, rotation, cloning), image getters, and GDI/user32 interop
/// (HBITMAP, HICON, DIBs). Image attributes live in
/// gdiplus_image_attributes.cpp, DrawImage* and cached bitmaps in
/// gdiplus_image_draw.cpp.
///
/// Contract: GdiPlusFlat.h, the Learn Bitmap / Image pages, and
/// tools/oracle_probe/gdiplus_image_probe.c (x86 + x64) for every rule the
/// pages leave out; each such rule names its probe line.

#include "winapi/graphics/gdiplus/gdiplus_image_internal.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/user32/resources/icon_planes.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace vitreo::winapi::gdip {

namespace {

// ImageFormatMemoryBMP {b96b3caa-0728-11d3-9d7b-0000f81ef32e}, in-memory layout.
constexpr u8 kMemoryBmpGuid[16] = {0xaa, 0x3c, 0x6b, 0xb9, 0x28, 0x07, 0xd3, 0x11,
                                   0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e};

constexpr u64 kMaxBitmapBytes = 0x7FFFFFFFull;

ImageItemFinder g_image_item_finder = nullptr;

void finish(Marshaler& m, Status s) { (void)write_status(m, s); }

i32 iarg(Marshaler& m, usize i) { return static_cast<i32>(m.arg32(i)); }

GpImage* image_of(GdiplusManager& mgr, u64 h) { return mgr.objects().get<GpImage>(h); }

GpImage* bitmap_of(GdiplusManager& mgr, u64 h) {
    GpImage* img = image_of(mgr, h);
    return (img != nullptr && img->type == ImageTypeBitmap) ? img : nullptr;
}

void init_memory_bitmap(GpImage& img) { std::memcpy(img.raw_format, kMemoryBmpGuid, 16); }

/// Register @p img and write its handle to @p out.
Status publish(GdiplusManager& mgr, Marshaler& m, GuestAddr out, GpImage img) {
    const u64 h = mgr.objects().add(std::move(img));
    if (!write_handle(m, out, h)) {
        (void)mgr.objects().remove<GpImage>(h);
        return Status::InvalidParameter;
    }
    return Status::Ok;
}

/// Bytes of a ColorPalette with @p count entries (UINT Flags, UINT Count,
/// ARGB Entries[max(count, 1)]): what GdipGetImagePaletteSize answers.
i32 palette_bytes(usize count) { return 8 + 4 * static_cast<i32>(std::max<usize>(count, 1)); }

bool read_palette(Marshaler& m, GuestAddr at, u32& flags, std::vector<u32>& entries, u32 max_count = 256) {
    const auto f = m.read_u32(at);
    const auto c = m.read_u32(at + 4);
    if (f.is_err() || c.is_err()) return false;
    if (c.value() == 0 || c.value() > max_count) return false;
    flags = f.value();
    entries.resize(c.value());
    for (u32 i = 0; i < c.value(); ++i) {
        const auto e = m.read_u32(at + 8 + 4ull * i);
        if (e.is_err()) return false;
        entries[i] = e.value();
    }
    return true;
}

bool write_palette(Marshaler& m, GuestAddr at, u32 flags, const std::vector<u32>& entries) {
    if (m.write_u32(at, flags).is_err() || m.write_u32(at + 4, static_cast<u32>(entries.size())).is_err())
        return false;
    for (usize i = 0; i < entries.size(); ++i)
        if (m.write_u32(at + 8 + 4ull * i, entries[i]).is_err()) return false;
    return true;
}

/// The palette a lock buffer of @p format is read and written in: the
/// bitmap's own for its own format, else GDI+'s default for @p format.
std::vector<u32> lock_palette(const GpImage& img, i32 format) {
    if (pixel_format_canonical(format) == pixel_format_canonical(img.pixel_format)) return img.palette;
    return default_palette(format);
}

/// BitmapData (GdiPlusImaging.h): UINT Width, UINT Height, INT Stride,
/// PixelFormat, VOID* Scan0, UINT_PTR Reserved; pointer-sized fields widen
/// on x64 (16 / 24 bytes vs 16 / 20).
struct BitmapDataView {
    u32 width{0}, height{0};
    i32 stride{0};
    i32 format{0};
    GuestAddr scan0{0};
};

bool read_bitmap_data(Marshaler& m, GuestAddr at, BitmapDataView& v) {
    const auto w = m.read_u32(at), h = m.read_u32(at + 4), s = m.read_u32(at + 8), f = m.read_u32(at + 12);
    const auto p = m.read_ptr(at + 16);
    if (w.is_err() || h.is_err() || s.is_err() || f.is_err() || p.is_err()) return false;
    v.width = w.value();
    v.height = h.value();
    v.stride = static_cast<i32>(s.value());
    v.format = static_cast<i32>(f.value());
    v.scan0 = static_cast<GuestAddr>(p.value());
    return true;
}

bool write_bitmap_data(Marshaler& m, GuestAddr at, const BitmapLock& l, u64 reserved) {
    const usize ptr = m.cpu_is_64bit() ? 8 : 4;
    return m.write_u32(at, static_cast<u32>(l.width)).is_ok() && m.write_u32(at + 4, static_cast<u32>(l.height)).is_ok() &&
           m.write_u32(at + 8, static_cast<u32>(l.stride)).is_ok() &&
           m.write_u32(at + 12, static_cast<u32>(l.format)).is_ok() && m.write_ptr(at + 16, l.scan0).is_ok() &&
           m.write_ptr(at + 16 + ptr, reserved).is_ok();
}

}  // namespace

// ---- Guest memory --------------------------------------------------------------------

GuestScratch::GuestScratch(GdiplusManager& mgr, u64 size, bool zero) : m_mgr(mgr) {
    const GuestAddr heap = mgr.heap().get_process_heap();
    m_addr = mgr.heap().heap_alloc(heap, zero ? 0x08u : 0u, size == 0 ? 1 : size);
}

GuestScratch::~GuestScratch() { guest_free(m_mgr, m_addr); }

void set_image_item_finder(ImageItemFinder finder) { g_image_item_finder = finder; }

Status missing_object(GdiplusManager& mgr, u64 h) {
    return h != 0 && mgr.objects().contains(h) ? Status::ObjectBusy : Status::InvalidParameter;
}

void guest_free(GdiplusManager& mgr, GuestAddr addr) {
    if (addr == 0) return;
    const GuestAddr heap = mgr.heap().get_process_heap();
    (void)mgr.heap().heap_free(heap, 0, addr);
}

void discard_lock(GdiplusManager& mgr, GpImage& image) {
    if (image.locked == 0) return;
    guest_free(mgr, image.lock.allocation);
    image.lock = BitmapLock{};
    image.locked = 0;
}

namespace {

// ---- LockBits / UnlockBits --------------------------------------------------------------

Status lock_bits(GdiplusManager& mgr, Marshaler& m, GpImage& img, GuestAddr rect, u32 mode, i32 format,
                 GuestAddr data) {
    if (data == 0 || !pixel_format_valid(format)) return Status::InvalidParameter;
    if (img.locked != 0) return Status::WrongState;  // probe "locked: relock=8"
    i32 x = 0, y = 0, w = img.width, h = img.height;
    if (rect != 0) {
        RectF r;
        if (!read_rect_i(m, rect, r)) return Status::InvalidParameter;
        x = static_cast<i32>(r.X);
        y = static_cast<i32>(r.Y);
        w = static_cast<i32>(r.Width);
        h = static_cast<i32>(r.Height);
        // probe "rect(...)": inside the bitmap with a positive size.
        if (w <= 0 || h <= 0 || x < 0 || y < 0 || x > img.width - w || y > img.height - h)
            return Status::InvalidParameter;
    }
    const i32 fmt = pixel_format_canonical(format);
    const i32 own = pixel_format_canonical(img.pixel_format);
    const bool reading = (mode & ImageLockModeRead) != 0 || (mode & ImageLockModeWrite) == 0;
    // A write-only 16bppGrayScale lock of another format is granted and
    // its unlock fails (probe "write 00101004=0 unlock=2"); every other
    // conversion GDI+ lacks fails here.
    const bool deferred_failure =
        !conversion_supported(own, fmt) && fmt == PixelFormat16bppGrayScale && !reading;
    if (!conversion_supported(own, fmt) && !deferred_failure) return Status::InvalidParameter;

    const i32 bpp = pixel_format_bits(fmt);
    BitmapLock l;
    l.x = x;
    l.y = y;
    l.width = w;
    l.height = h;
    l.mode = mode;
    l.format = format;
    l.same_format = fmt == own && (static_cast<i64>(x) * bpp) % 8 == 0;
    if ((mode & ImageLockModeUserInputBuf) != 0) {
        BitmapDataView v;
        if (!read_bitmap_data(m, data, v) || v.scan0 == 0) return Status::InvalidParameter;
        l.user_buffer = true;
        l.scan0 = v.scan0;
        l.stride = v.stride;  // used as given (probe "userbuf_stride8", "userbuf_neg")
        l.same_format = false;
    } else if (l.same_format && img.user_scan0 != 0) {
        // The caller's own Scan0 memory: hand out a pointer into it (probe
        // "negstride_lock ... scan0_is_buf=1").
        l.scan0 = static_cast<GuestAddr>(static_cast<i64>(img.user_scan0) + static_cast<i64>(y) * img.stride +
                                         static_cast<i64>(x) * bpp / 8);
        l.stride = img.stride;
    } else {
        l.stride = l.same_format ? image_row_bytes(img) : pixel_format_stride(w, fmt);
        const u64 size = static_cast<u64>(l.stride) * static_cast<u64>(h);
        GuestScratch buf(mgr, size, true);
        if (buf.addr() == 0) return Status::OutOfMemory;
        l.allocation = buf.release();
        l.scan0 = l.allocation + (l.same_format ? static_cast<u64>(x) * bpp / 8 : 0);
    }

    const bool direct = l.same_format && img.user_scan0 != 0 && !l.user_buffer;
    if (!direct && !deferred_failure && (reading || l.same_format)) {
        const std::vector<u32> dpal = lock_palette(img, fmt);
        Quantizer q(dpal);
        std::vector<u8> scratch, line;
        for (i32 j = 0; j < h; ++j) {
            const u8* src = image_row(m, img, y + j, scratch);
            if (src == nullptr) { guest_free(mgr, l.allocation); return Status::InvalidParameter; }
            const GuestAddr dst = static_cast<GuestAddr>(static_cast<i64>(l.scan0) + static_cast<i64>(j) * l.stride);
            if (l.same_format && !l.user_buffer) {
                // Whole stored row (the buffer starts at column 0 of row y).
                const GuestAddr row0 = static_cast<GuestAddr>(static_cast<i64>(l.allocation) +
                                                             static_cast<i64>(j) * l.stride);
                if (m.memory().write_bytes(row0, src, static_cast<usize>(image_row_bytes(img))).is_err()) {
                    guest_free(mgr, l.allocation);
                    return Status::InvalidParameter;
                }
                continue;
            }
            line.assign(static_cast<usize>(pixel_format_stride(w, fmt)), 0);
            if (!convert_row(own, img.palette, src, x, fmt, dpal, line.data(), 0, w, &q)) {
                guest_free(mgr, l.allocation);
                return Status::InvalidParameter;
            }
            const usize bytes = static_cast<usize>((static_cast<i64>(w) * bpp + 7) / 8);
            if (m.memory().write_bytes(dst, line.data(), bytes).is_err()) {
                guest_free(mgr, l.allocation);
                return Status::InvalidParameter;
            }
        }
    }
    if (!write_bitmap_data(m, data, l, 0x00C0FFEEu)) {
        guest_free(mgr, l.allocation);
        return Status::InvalidParameter;
    }
    img.lock = l;
    img.locked = 1;
    return Status::Ok;
}

Status unlock_bits(GdiplusManager& mgr, Marshaler& m, GpImage& img) {
    if (img.locked == 0) return Status::Win32Error;  // probe "unlock_again=7"
    const BitmapLock l = img.lock;
    Status st = Status::Ok;
    const i32 fmt = pixel_format_canonical(l.format);
    const i32 own = pixel_format_canonical(img.pixel_format);
    const bool direct = l.same_format && img.user_scan0 != 0 && !l.user_buffer;
    const bool write_back = !direct && (l.same_format || (l.mode & ImageLockModeWrite) != 0);
    if (write_back) {
        if (!conversion_supported(fmt, own)) {
            st = Status::InvalidParameter;
        } else {
            const std::vector<u32> spal = lock_palette(img, fmt);
            Quantizer q(img.palette);
            const i32 bpp = pixel_format_bits(fmt);
            std::vector<u8> scratch, line;
            for (i32 j = 0; j < l.height && st == Status::Ok; ++j) {
                if (l.same_format && !l.user_buffer) {
                    line.resize(static_cast<usize>(image_row_bytes(img)));
                    const GuestAddr row0 = static_cast<GuestAddr>(static_cast<i64>(l.allocation) +
                                                                 static_cast<i64>(j) * l.stride);
                    if (m.memory().read_bytes(row0, line.data(), line.size()).is_err()) { st = Status::InvalidParameter; break; }
                    u8* dst = image_row_mut(m, img, l.y + j, scratch);
                    if (dst == nullptr) { st = Status::InvalidParameter; break; }
                    std::memcpy(dst, line.data(), line.size());
                    if (!image_row_commit(m, img, l.y + j, scratch)) st = Status::InvalidParameter;
                    continue;
                }
                const usize bytes = static_cast<usize>((static_cast<i64>(l.width) * bpp + 7) / 8);
                line.assign(std::max<usize>(bytes, 1), 0);
                const GuestAddr src = static_cast<GuestAddr>(static_cast<i64>(l.scan0) + static_cast<i64>(j) * l.stride);
                if (m.memory().read_bytes(src, line.data(), bytes).is_err()) { st = Status::InvalidParameter; break; }
                u8* dst = image_row_mut(m, img, l.y + j, scratch);
                if (dst == nullptr) { st = Status::InvalidParameter; break; }
                if (!convert_row(fmt, spal, line.data(), 0, own, img.palette, dst, l.x, l.width, &q)) {
                    st = Status::InvalidParameter;
                    break;
                }
                if (!image_row_commit(m, img, l.y + j, scratch)) st = Status::InvalidParameter;
            }
        }
    }
    discard_lock(mgr, img);
    return st;
}

// ---- Clone / convert ---------------------------------------------------------------------------

Status clone_area(GdiplusManager& mgr, Marshaler& m, i32 x, i32 y, i32 w, i32 h, i32 format, u64 src_h,
                  GuestAddr out) {
    GpImage* src = bitmap_of(mgr, src_h);
    if (src == nullptr) return missing_object(mgr, src_h);
    if (out == 0) return Status::InvalidParameter;
    // Every refusal below is OutOfMemory on Windows (probe "clone_oob",
    // "cloneareaI 00101004=3", "fmtcheck ... clone=3").
    const i32 fmt = format == 0 ? src->pixel_format : format;
    if (!pixel_format_valid(fmt)) return Status::OutOfMemory;
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x > src->width - w || y > src->height - h) return Status::OutOfMemory;
    if (!conversion_supported(src->pixel_format, fmt)) return Status::OutOfMemory;
    if (static_cast<u64>(pixel_format_stride(w, fmt)) * static_cast<u64>(h) > kMaxBitmapBytes) return Status::OutOfMemory;
    GpImage dst = make_bitmap(w, h, fmt);
    init_memory_bitmap(dst);
    dst.dpi_x = src->dpi_x;
    dst.dpi_y = src->dpi_y;
    if (pixel_format_canonical(fmt) == pixel_format_canonical(src->pixel_format) && pixel_format_indexed(fmt)) {
        dst.palette = src->palette;
        dst.palette_flags = src->palette_flags;
    }
    Quantizer q(dst.palette);
    std::vector<u8> scratch;
    for (i32 j = 0; j < h; ++j) {
        const u8* row = image_row(m, *src, y + j, scratch);
        if (row == nullptr) return Status::InvalidParameter;
        u8* drow = dst.bits.data() + static_cast<usize>(j) * static_cast<usize>(dst.stride);
        if (!convert_row(src->pixel_format, src->palette, row, x, fmt, dst.palette, drow, 0, w, &q))
            return Status::OutOfMemory;
    }
    return publish(mgr, m, out, std::move(dst));
}

Status convert_format(GdiplusManager& mgr, Marshaler& m, GpImage& img, i32 format, i32 dither, i32 palette_type,
                      GuestAddr palette, float alpha_percent) {
    if (!pixel_format_valid(format) || dither < 0 || dither > 9 || palette_type < 0 || palette_type > 10)
        return Status::InvalidParameter;
    if (pixel_format_opaque_storage(format) || pixel_format_opaque_storage(img.pixel_format))
        return Status::InvalidParameter;
    const i32 fmt = pixel_format_canonical(format);
    const bool indexed = pixel_format_indexed(fmt);
    std::vector<u32> pal;
    u32 pal_flags = 0;
    if (indexed) {
        // An indexed target needs the caller's palette (probe
        // "convert_fixed_nopal=2"); Solid and ErrorDiffusion map onto any
        // palette, the other dithers only onto a fixed halftone palette
        // (probe "dither0 tgt1=2", "d2 p1=2", "d0 p0=2").
        if (palette == 0) return Status::InvalidParameter;
        if (!read_palette(m, palette, pal_flags, pal, 1u << pixel_format_bits(fmt))) return Status::InvalidParameter;
        if (dither != 1 && dither != 9 && (palette_type < 3 || palette_type > 10)) return Status::InvalidParameter;
    } else if (fmt == pixel_format_canonical(img.pixel_format)) {
        return Status::Ok;
    }
    std::vector<u32> argb;
    if (!image_read_argb(m, img, 0, 0, img.width, img.height, argb)) return Status::InvalidParameter;
    std::vector<u8> bits;
    const i32 threshold = static_cast<i32>(std::ceil(std::clamp(alpha_percent, 0.0f, 100.0f) * 255.0f / 100.0f));
    if (indexed || fmt == PixelFormat16bppRGB555 || fmt == PixelFormat16bppRGB565 ||
        fmt == PixelFormat16bppARGB1555) {
        dither_into(argb, img.width, img.height, fmt, dither, palette_type, pal, threshold, bits);
    } else {
        const i32 stride = pixel_format_stride(img.width, fmt);
        bits.assign(static_cast<usize>(stride) * static_cast<usize>(img.height), 0);
        for (i32 y = 0; y < img.height; ++y)
            for (i32 x = 0; x < img.width; ++x)
                encode_pixel(fmt, bits.data() + static_cast<usize>(y) * static_cast<usize>(stride), x,
                             argb[static_cast<usize>(y) * static_cast<usize>(img.width) + static_cast<usize>(x)],
                             nullptr);
    }
    discard_lock(mgr, img);
    img.bits = std::move(bits);
    img.stride = pixel_format_stride(img.width, fmt);
    img.user_scan0 = 0;
    img.pixel_format = format;
    img.palette = indexed ? pal : std::vector<u32>{};
    img.palette_flags = indexed ? pal_flags : 0;
    img.flags = bitmap_flags_for(fmt) | (img.flags & ~static_cast<u32>(ImageFlagsHasAlpha));
    return Status::Ok;
}

Status initialize_palette(Marshaler& m, GdiplusManager& mgr, GuestAddr palette, i32 type, i32 optimal,
                          bool transparent, u64 bitmap) {
    if (palette == 0) return Status::InvalidParameter;
    if (type == 0) return Status::Ok;  // PaletteTypeCustom: left as the caller filled it
    const auto cap = m.read_u32(palette + 4);
    if (cap.is_err()) return Status::InvalidParameter;
    std::vector<u32> entries;
    u32 flags = 0;
    if (type == 1) {
        GpImage* img = bitmap_of(mgr, bitmap);
        if (img == nullptr || optimal < 2 || optimal > 256) return Status::InvalidParameter;
        std::vector<u32> argb;
        if (!image_read_argb(m, *img, 0, 0, img->width, img->height, argb)) return Status::InvalidParameter;
        entries = optimal_palette(argb, transparent ? optimal - 1 : optimal);
        if (transparent) entries.push_back(0);
    } else {
        if (!fixed_palette(type, entries)) return Status::InvalidParameter;
        flags = static_cast<u32>(type) << 8;  // probe "initpal t2 ... flags=200"
        if (transparent) {
            if (entries.size() < 256) entries.push_back(0);
            else entries.back() = 0;
        }
    }
    if (cap.value() < entries.size()) return Status::GenericError;  // probe "initpal_small=1"
    return write_palette(m, palette, flags, entries) ? Status::Ok : Status::InvalidParameter;
}

// ---- GDI / user32 interop --------------------------------------------------------------------------

Status create_hbitmap(GdiplusManager& mgr, Marshaler& m, GpImage& img, GuestAddr out, u32 background) {
    GdiManager* gdi = mgr.gdi();
    if (gdi == nullptr) return Status::GenericError;
    std::vector<u32> argb;
    if (!image_read_argb(m, img, 0, 0, img.width, img.height, argb)) return Status::InvalidParameter;
    // A 32bpp bottom-up DIB section: each pixel composited over the
    // background's colour, keeping the pixel's own alpha (probe "hbitmap
    // bg=...", "hbitmap_round": round(c*a/255) + round(bg*(255-a)/255)).
    GuestScratch bmi(mgr, 40 + 16, true);
    if (bmi.addr() == 0) return Status::OutOfMemory;
    const GuestAddr hdr = bmi.addr();
    const GuestAddr ppv = bmi.addr() + 40;
    (void)m.write_u32(hdr, 40);
    (void)m.write_u32(hdr + 4, static_cast<u32>(img.width));
    (void)m.write_u32(hdr + 8, static_cast<u32>(img.height));
    (void)m.write_u16(hdr + 12, 1);
    (void)m.write_u16(hdr + 14, 32);
    const Handle hbm = gdi->create_dib_section(0, hdr, 0, ppv, m.cpu_is_64bit());
    if (hbm == 0) return Status::OutOfMemory;
    const auto bits = m.read_ptr(ppv);
    if (bits.is_err() || bits.value() == 0) return Status::OutOfMemory;
    const u32 br = (background >> 16) & 0xFF, bg = (background >> 8) & 0xFF, bb = background & 0xFF;
    auto blend = [](u32 c, u32 a, u32 back) { return (c * a + 127) / 255 + (back * (255 - a) + 127) / 255; };
    std::vector<u8> row(static_cast<usize>(img.width) * 4u);
    for (i32 y = 0; y < img.height; ++y) {
        for (i32 x = 0; x < img.width; ++x) {
            const u32 c = argb[static_cast<usize>(y) * static_cast<usize>(img.width) + static_cast<usize>(x)];
            const u32 a = c >> 24;
            u8* p = row.data() + static_cast<usize>(x) * 4u;
            p[0] = static_cast<u8>(blend(c & 0xFF, a, bb));
            p[1] = static_cast<u8>(blend((c >> 8) & 0xFF, a, bg));
            p[2] = static_cast<u8>(blend((c >> 16) & 0xFF, a, br));
            p[3] = static_cast<u8>(a);
        }
        const GuestAddr dst = bits.value() + static_cast<u64>(img.height - 1 - y) * static_cast<u64>(img.width) * 4u;
        if (m.memory().write_bytes(dst, row.data(), row.size()).is_err()) return Status::GenericError;
    }
    return write_handle(m, out, hbm) ? Status::Ok : Status::InvalidParameter;
}

Status create_hicon(GdiplusManager& mgr, Marshaler& m, GpImage& img, GuestAddr out) {
    ResourceManager* rm = mgr.resources();
    if (rm == nullptr) {
        log_unimplemented("GdipCreateHICONFromBitmap", "no user32 icon manager wired");
        return Status::GenericError;
    }
    std::vector<u32> argb;
    if (!image_read_argb(m, img, 0, 0, img.width, img.height, argb)) return Status::InvalidParameter;
    // GetIconInfo of the result: a premultiplied 32bpp colour bitmap and an
    // AND mask set wherever alpha is not 255; hot spot (w/2, h/2) (probe
    // "hicon", "hicon_mask6", "hicon5x3").
    IconPlanes planes;
    planes.is_icon = true;
    planes.width = static_cast<u32>(img.width);
    planes.height = static_cast<u32>(img.height);
    planes.hotspot_x = planes.width / 2;
    planes.hotspot_y = planes.height / 2;
    planes.color.resize(argb.size() * 4);
    planes.mask.resize(argb.size());
    for (usize i = 0; i < argb.size(); ++i) {
        const u32 c = argb[i], a = c >> 24;
        planes.color[i * 4] = static_cast<u8>((((c >> 16) & 0xFF) * a + 127) / 255);
        planes.color[i * 4 + 1] = static_cast<u8>((((c >> 8) & 0xFF) * a + 127) / 255);
        planes.color[i * 4 + 2] = static_cast<u8>(((c & 0xFF) * a + 127) / 255);
        planes.color[i * 4 + 3] = static_cast<u8>(a);
        planes.mask[i] = a != 255 ? 1 : 0;
    }
    const u32 icon = rm->create_icon_object(std::move(planes));
    if (icon == 0) return Status::OutOfMemory;
    return write_handle(m, out, icon) ? Status::Ok : Status::InvalidParameter;
}

Status bitmap_from_hicon(GdiplusManager& mgr, Marshaler& m, u64 hicon, GuestAddr out) {
    if (out == 0) return Status::InvalidParameter;
    ResourceManager* rm = mgr.resources();
    if (rm == nullptr) {
        log_unimplemented("GdipCreateBitmapFromHICON", "no user32 icon manager wired");
        return Status::GenericError;
    }
    const u32 h = static_cast<u32>(hicon);
    if (rm->icon_kind(h) == ResourceManager::IconKind::StockSystem) {
        log_unimplemented("GdipCreateBitmapFromHICON", "stock system icon artwork (IDI_*/IDC_*)");
        return Status::GenericError;
    }
    const auto planes = rm->icon_planes(h);
    if (!planes || planes->width == 0 || planes->height == 0) return Status::InvalidParameter;
    const i32 w = static_cast<i32>(planes->width), ht = static_cast<i32>(planes->height);
    GpImage img = make_bitmap(w, ht, PixelFormat32bppARGB);
    init_memory_bitmap(img);
    // GDI+ builds the bitmap from the AND mask and the colour bits; it
    // ignores the colour bitmap's alpha (probe "fromhicon_alpha": a
    // premultiplied alpha icon with an all-opaque mask comes back opaque;
    // "fromhicon6": every masked pixel is 0).
    for (i32 y = 0; y < ht; ++y) {
        u8* row = img.bits.data() + static_cast<usize>(y) * static_cast<usize>(img.stride);
        for (i32 x = 0; x < w; ++x) {
            const usize i = static_cast<usize>(y) * planes->width + static_cast<usize>(x);
            u32 c = 0;
            const bool and_bit = i < planes->mask.size() && planes->mask[i] != 0;
            if (planes->monochrome()) {
                const usize xi = i + static_cast<usize>(planes->width) * planes->height;
                const bool xor_bit = xi < planes->mask.size() && planes->mask[xi] != 0;
                if (!and_bit) c = xor_bit ? 0xFFFFFFFFu : 0xFF000000u;
            } else if (!and_bit && i * 4 + 2 < planes->color.size()) {
                c = 0xFF000000u | (static_cast<u32>(planes->color[i * 4]) << 16) |
                    (static_cast<u32>(planes->color[i * 4 + 1]) << 8) | planes->color[i * 4 + 2];
            }
            std::memcpy(row + static_cast<usize>(x) * 4u, &c, 4);  // little-endian B,G,R,A
        }
    }
    return publish(mgr, m, out, std::move(img));
}

/// The GDI+ format for a DIB of @p bpp bits with BI_BITFIELDS masks
/// (all zero for BI_RGB). 0 when GDI+ has none.
i32 format_for_dib(u32 bpp, u32 compression, u32 rmask, u32 gmask, u32 bmask) {
    if (compression == 0) {
        switch (bpp) {
        case 1: return PixelFormat1bppIndexed;
        case 4: return PixelFormat4bppIndexed;
        case 8: return PixelFormat8bppIndexed;
        case 16: return PixelFormat16bppRGB555;
        case 24: return PixelFormat24bppRGB;
        case 32: return PixelFormat32bppRGB;
        default: return 0;
        }
    }
    if (compression == 3) {  // BI_BITFIELDS
        if (bpp == 16 && rmask == 0xF800 && gmask == 0x07E0 && bmask == 0x001F) return PixelFormat16bppRGB565;
        if (bpp == 16 && rmask == 0x7C00 && gmask == 0x03E0 && bmask == 0x001F) return PixelFormat16bppRGB555;
        if (bpp == 32 && rmask == 0xFF0000 && gmask == 0xFF00 && bmask == 0xFF) return PixelFormat32bppRGB;
    }
    return 0;
}

Status bitmap_from_gdi_dib(GdiplusManager& mgr, Marshaler& m, GuestAddr bmi, GuestAddr bits, GuestAddr out) {
    if (bmi == 0 || bits == 0 || out == 0) return Status::InvalidParameter;
    const auto size = m.read_u32(bmi), width = m.read_u32(bmi + 4), height = m.read_u32(bmi + 8);
    const auto bpp16 = m.read_u16(bmi + 14);
    const auto comp = m.read_u32(bmi + 16), used = m.read_u32(bmi + 32);
    if (size.is_err() || width.is_err() || height.is_err() || bpp16.is_err() || comp.is_err() || used.is_err())
        return Status::InvalidParameter;
    const i32 w = static_cast<i32>(width.value());
    const i32 h = static_cast<i32>(height.value());
    const u32 bpp = bpp16.value();
    if (w <= 0 || h == 0 || size.value() < 40) return Status::InvalidParameter;
    u32 masks[3] = {0, 0, 0};
    if (comp.value() == 3) {
        for (u32 i = 0; i < 3; ++i) {
            const auto v = m.read_u32(bmi + 40 + 4 * i);
            if (v.is_err()) return Status::InvalidParameter;
            masks[i] = v.value();
        }
    }
    const i32 fmt = format_for_dib(bpp, comp.value(), masks[0], masks[1], masks[2]);
    if (fmt == 0) return Status::InvalidParameter;  // probe "fromgdidib_rle=2"
    const i32 ah = std::abs(h);
    GpImage img;
    img.type = ImageTypeBitmap;
    img.width = w;
    img.height = ah;
    img.pixel_format = fmt;
    img.flags = bitmap_flags_for(fmt);
    init_memory_bitmap(img);
    // The bitmap aliases the caller's bits (probe "gdidib_aliases"):
    // bottom-up rows read from the last row backwards.
    const i32 stride = pixel_format_stride(w, fmt);
    if (h > 0) {
        img.user_scan0 = bits + static_cast<u64>(ah - 1) * static_cast<u64>(stride);
        img.stride = -stride;
    } else {
        img.user_scan0 = bits;
        img.stride = stride;
    }
    if (pixel_format_indexed(fmt)) {
        // biClrUsed entries (0 = the full 1 << bpp), opaque (probe
        // "gdidib8_pal count=2", "gdidib4_clrused0_pal count=16").
        const u32 full = 1u << bpp;
        const u32 n = used.value() == 0 ? full : std::min(used.value(), full);
        const GuestAddr table = bmi + size.value();
        for (u32 i = 0; i < n; ++i) {
            const auto q = m.read_u32(table + 4 * i);
            if (q.is_err()) return Status::InvalidParameter;
            img.palette.push_back(0xFF000000u | (q.value() & 0xFFFFFF));
        }
    }
    return publish(mgr, m, out, std::move(img));
}

Status bitmap_from_hbitmap(GdiplusManager& mgr, Marshaler& m, u64 hbm, u64 hpal, GuestAddr out) {
    if (out == 0) return Status::InvalidParameter;
    GdiManager* gdi = mgr.gdi();
    if (gdi == nullptr) return Status::GenericError;
    const Handle hb = static_cast<Handle>(hbm);
    if (hbm == 0 || gdi->object_type(hb) != 7 /* OBJ_BITMAP */) return Status::Win32Error;  // probe "fromhbitmap_bad=7"
    // GetObject(DIBSECTION) in the x86 layout: BITMAP (24) + BITMAPINFOHEADER
    // (40) + dsBitfields (12) + dshSection + dsOffset = 84 bytes.
    GuestScratch obj(mgr, 84, true);
    if (obj.addr() == 0) return Status::OutOfMemory;
    const i32 got = gdi->get_object(hb, 84, obj.addr(), false, true);
    if (got < 24) return Status::Win32Error;
    const i32 w = static_cast<i32>(m.read_u32(obj.addr() + 4).value_or(0));
    const i32 h = static_cast<i32>(m.read_u32(obj.addr() + 8).value_or(0));
    const u32 planes = m.read_u16(obj.addr() + 16).value_or(1);
    const u32 bpp = planes * m.read_u16(obj.addr() + 18).value_or(0);
    const bool dib = got >= 84;
    u32 comp = 0, masks[3] = {0, 0, 0};
    if (dib) {
        comp = m.read_u32(obj.addr() + 40).value_or(0);
        for (u32 i = 0; i < 3; ++i) masks[i] = m.read_u32(obj.addr() + 64 + 4 * i).value_or(0);
    }
    i32 fmt = (bpp == 16 && comp == 3) ? format_for_dib(16, 3, masks[0], masks[1], masks[2]) : 0;
    if (fmt == 0) fmt = format_for_dib(bpp, 0, 0, 0, 0);
    if (fmt == 0 || w <= 0 || h <= 0) return Status::InvalidParameter;
    GpImage img = make_bitmap(w, h, fmt);
    init_memory_bitmap(img);
    // GetDIBits in the bitmap's own depth, top-down, colour table included.
    const u32 table = pixel_format_indexed(fmt) ? (1u << bpp) : (fmt == PixelFormat16bppRGB565 ? 3u : 0u);
    GuestScratch bmi(mgr, 40 + 4ull * std::max<u32>(table, 3), true);
    GuestScratch bits(mgr, static_cast<u64>(img.stride) * static_cast<u64>(h), true);
    if (bmi.addr() == 0 || bits.addr() == 0) return Status::OutOfMemory;
    (void)m.write_u32(bmi.addr(), 40);
    (void)m.write_u32(bmi.addr() + 4, static_cast<u32>(w));
    (void)m.write_u32(bmi.addr() + 8, static_cast<u32>(-h));
    (void)m.write_u16(bmi.addr() + 12, 1);
    (void)m.write_u16(bmi.addr() + 14, static_cast<u16>(bpp));
    if (fmt == PixelFormat16bppRGB565) {
        (void)m.write_u32(bmi.addr() + 16, 3);
        (void)m.write_u32(bmi.addr() + 40, 0xF800);
        (void)m.write_u32(bmi.addr() + 44, 0x07E0);
        (void)m.write_u32(bmi.addr() + 48, 0x001F);
    }
    const Handle dc = gdi->create_compatible_dc(0);
    const i32 lines = gdi->get_dib_bits(dc, hb, 0, static_cast<u32>(h), bits.addr(), bmi.addr(), 0);
    (void)gdi->delete_dc(dc);
    if (lines <= 0) return Status::Win32Error;
    if (m.memory().read_bytes(bits.addr(), img.bits.data(), img.bits.size()).is_err()) return Status::Win32Error;
    if (pixel_format_indexed(fmt)) {
        img.palette.assign(table, 0xFF000000u);
        img.palette_flags = 0;
        if (!dib && hpal != 0) {
            // A device-dependent bitmap's indices name entries of hpal.
            GuestScratch pe(mgr, 4ull * table, true);
            const u32 n = gdi->get_palette_entries(static_cast<Handle>(hpal), 0, table, pe.addr());
            for (u32 i = 0; i < n && i < table; ++i) {
                const u32 e = m.read_u32(pe.addr() + 4 * i).value_or(0);  // R, G, B, flags
                img.palette[i] = 0xFF000000u | ((e & 0xFF) << 16) | (e & 0xFF00) | ((e >> 16) & 0xFF);
            }
        } else {
            for (u32 i = 0; i < table; ++i)
                img.palette[i] = 0xFF000000u | (m.read_u32(bmi.addr() + 40 + 4 * i).value_or(0) & 0xFFFFFF);
        }
    }
    return publish(mgr, m, out, std::move(img));
}

}  // namespace

void register_image(GdiplusManager& mgr, HandlerMap& map) {
    // ---- Creation ------------------------------------------------------------------
    // GdipCreateBitmapFromScan0(INT w, INT h, INT stride, PixelFormat, BYTE* scan0, GpBitmap**)
    map["GdipCreateBitmapFromScan0"] = [&mgr](Marshaler& m) {
        const i32 w = iarg(m, 0), h = iarg(m, 1), stride = iarg(m, 2), format = iarg(m, 3);
        const GuestAddr scan0 = m.arg_ptr(4), out = m.arg_ptr(5);
        if (out == 0 || w <= 0 || h <= 0 || !pixel_format_valid(format)) return finish(m, Status::InvalidParameter);
        // A caller buffer needs a non-zero DWORD-multiple stride; it is not
        // checked against the row size (probe "create(2,2,4,...)=0").
        if (scan0 != 0 && (stride == 0 || stride % 4 != 0)) return finish(m, Status::InvalidParameter);
        const u64 bytes = static_cast<u64>(pixel_format_stride(w, format)) * static_cast<u64>(h);
        if (bytes > kMaxBitmapBytes) return finish(m, Status::InvalidParameter);  // probe "create(100000,...)"
        GpImage img;
        if (scan0 != 0) {
            img.type = ImageTypeBitmap;
            img.width = w;
            img.height = h;
            img.pixel_format = format;
            img.user_scan0 = scan0;
            img.stride = stride;
            img.palette = default_palette(format, &img.palette_flags);
            img.flags = bitmap_flags_for(format);
        } else {
            img = make_bitmap(w, h, format);
        }
        init_memory_bitmap(img);
        finish(m, publish(mgr, m, out, std::move(img)));
    };
    // GdipCreateBitmapFromGraphics(INT w, INT h, GpGraphics*, GpBitmap**):
    // 32bppPARGB at the graphics' resolution (probe "fromgraphics").
    map["GdipCreateBitmapFromGraphics"] = [&mgr](Marshaler& m) {
        const i32 w = iarg(m, 0), h = iarg(m, 1);
        const GpGraphics* g = mgr.objects().get<GpGraphics>(handle_arg(m, 2));
        const GuestAddr out = m.arg_ptr(3);
        if (g == nullptr || out == 0 || w <= 0 || h <= 0) return finish(m, Status::InvalidParameter);
        if (static_cast<u64>(w) * static_cast<u64>(h) * 4u > kMaxBitmapBytes) return finish(m, Status::InvalidParameter);
        GpImage img = make_bitmap(w, h, PixelFormat32bppPARGB);
        init_memory_bitmap(img);
        img.dpi_x = g->dpi_x;
        img.dpi_y = g->dpi_y;
        finish(m, publish(mgr, m, out, std::move(img)));
    };
    map["GdipCreateBitmapFromGdiDib"] = [&mgr](Marshaler& m) {
        finish(m, bitmap_from_gdi_dib(mgr, m, m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2)));
    };
    map["GdipCreateBitmapFromHBITMAP"] = [&mgr](Marshaler& m) {
        finish(m, bitmap_from_hbitmap(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(2)));
    };
    map["GdipCreateBitmapFromHICON"] = [&mgr](Marshaler& m) {
        finish(m, bitmap_from_hicon(mgr, m, m.arg(0), m.arg_ptr(1)));
    };
    // GdipCreateHBITMAPFromBitmap(GpBitmap*, HBITMAP*, ARGB background)
    map["GdipCreateHBITMAPFromBitmap"] = [&mgr](Marshaler& m) {
        const u64 bitmap = handle_arg(m, 0);
        const GuestAddr out = m.arg_ptr(1);
        const auto report = [&](Status status) {
            VITREO_TRACE("[gdiplus-diag] GdipCreateHBITMAPFromBitmap bitmap=0x%llx status=%u out=0x%llx hbitmap=0x%llx\n",
                static_cast<unsigned long long>(bitmap), static_cast<u32>(status),
                static_cast<unsigned long long>(out),
                static_cast<unsigned long long>(m.read_ptr(out).value_or(0)));
            finish(m, status);
        };
        GpImage* img = bitmap_of(mgr, bitmap);
        if (img == nullptr) return report(missing_object(mgr, bitmap));
        if (out == 0) return report(Status::InvalidParameter);
        report(create_hbitmap(mgr, m, *img, out, m.arg32(2)));
    };
    map["GdipCreateHICONFromBitmap"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr out = m.arg_ptr(1);
        if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        finish(m, create_hicon(mgr, m, *img, out));
    };

    // ---- Pixels ------------------------------------------------------------------------
    // GdipBitmapGetPixel(GpBitmap*, INT x, INT y, ARGB*)
    map["GdipBitmapGetPixel"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const i32 x = iarg(m, 1), y = iarg(m, 2);
        const GuestAddr out = m.arg_ptr(3);
        if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        if (img->locked != 0) return finish(m, Status::WrongState);  // probe "locked: getpixel=8"
        if (x < 0 || y < 0 || x >= img->width || y >= img->height) return finish(m, Status::InvalidParameter);
        if (pixel_format_opaque_storage(img->pixel_format)) {
            (void)m.write_u32(out, 0);  // probe "getpixel0=2 00000000" (x64)
            return finish(m, Status::InvalidParameter);
        }
        std::vector<u8> scratch;
        const u8* row = image_row(m, *img, y, scratch);
        if (row == nullptr) return finish(m, Status::InvalidParameter);
        (void)m.write_u32(out, decode_pixel(img->pixel_format, img->palette, row, x));
        finish(m, Status::Ok);
    };
    map["GdipBitmapSetPixel"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const i32 x = iarg(m, 1), y = iarg(m, 2);
        const u32 c = m.arg32(3);
        if (img == nullptr) return finish(m, Status::InvalidParameter);
        if (img->locked != 0) return finish(m, Status::WrongState);
        if (x < 0 || y < 0 || x >= img->width || y >= img->height) return finish(m, Status::InvalidParameter);
        if (pixel_format_opaque_storage(img->pixel_format)) return finish(m, Status::InvalidParameter);
        std::vector<u8> scratch;
        u8* row = image_row_mut(m, *img, y, scratch);
        if (row == nullptr) return finish(m, Status::InvalidParameter);
        Quantizer q(img->palette);
        encode_pixel(img->pixel_format, row, x, c, &q);
        finish(m, image_row_commit(m, *img, y, scratch) ? Status::Ok : Status::InvalidParameter);
    };
    // GdipBitmapLockBits(GpBitmap*, const GpRect*, UINT flags, PixelFormat, BitmapData*)
    map["GdipBitmapLockBits"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (img == nullptr) return finish(m, Status::InvalidParameter);
        finish(m, lock_bits(mgr, m, *img, m.arg_ptr(1), m.arg32(2), iarg(m, 3), m.arg_ptr(4)));
    };
    map["GdipBitmapUnlockBits"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (img == nullptr || m.arg_ptr(1) == 0) return finish(m, Status::InvalidParameter);
        finish(m, unlock_bits(mgr, m, *img));
    };
    // GdipBitmapSetResolution(GpBitmap*, REAL xdpi, REAL ydpi): both > 0.
    map["GdipBitmapSetResolution"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const float x = real_arg(m, 1), y = real_arg(m, 2);
        if (img == nullptr || !(x > 0.0f) || !(y > 0.0f)) return finish(m, Status::InvalidParameter);
        img->dpi_x = x;
        img->dpi_y = y;
        finish(m, Status::Ok);
    };
    // GdipBitmapConvertFormat(GpBitmap*, PixelFormat, DitherType, PaletteType, ColorPalette*, REAL alphaThresholdPercent)
    map["GdipBitmapConvertFormat"] = [&mgr](Marshaler& m) {
        GpImage* img = bitmap_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (img == nullptr) return finish(m, Status::InvalidParameter);
        finish(m, convert_format(mgr, m, *img, iarg(m, 1), iarg(m, 2), iarg(m, 3), m.arg_ptr(4), real_arg(m, 5)));
    };
    // GdipInitializePalette(ColorPalette*, PaletteType, INT optimalColors, BOOL useTransparentColor, GpBitmap*)
    map["GdipInitializePalette"] = [&mgr](Marshaler& m) {
        finish(m, initialize_palette(m, mgr, m.arg_ptr(0), iarg(m, 1), iarg(m, 2), m.arg32(3) != 0, handle_arg(m, 4)));
    };
    // HPALETTE GdipCreateHalftonePalette(void): a new GDI palette each call
    // (probe "halftone_same=0").
    map["GdipCreateHalftonePalette"] = [&mgr](Marshaler& m) {
        GdiManager* gdi = mgr.gdi();
        if (gdi == nullptr) { m.set_return(0); return; }
        GuestScratch lp(mgr, 4 + 256 * 4, true);
        if (lp.addr() == 0) { m.set_return(0); return; }
        (void)m.write_u16(lp.addr(), 0x300);
        (void)m.write_u16(lp.addr() + 2, 256);
        const auto& entries = halftone_palette_entries();
        for (usize i = 0; i < 256; ++i) {
            const u32 e = entries[i];  // flags<<24 | RRGGBB -> PALETTEENTRY R, G, B, flags
            const u32 pe = ((e >> 16) & 0xFF) | (e & 0xFF00) | ((e & 0xFF) << 16) | (e & 0xFF000000u);
            (void)m.write_u32(lp.addr() + 4 + 4 * i, pe);
        }
        m.set_return(gdi->create_palette(lp.addr()));
    };

    // ---- Image objects -----------------------------------------------------------------
    map["GdipCloneImage"] = [&mgr](Marshaler& m) {
        const GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr out = m.arg_ptr(1);
        if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        // A clone of a caller-buffer bitmap keeps reading that buffer, as
        // GDI+'s copy-on-write clone does (probe "bufwrite_visible ... clone").
        GpImage copy = *img;
        copy.lock = BitmapLock{};
        copy.locked = 0;
        copy.abort_object = 0;
        finish(m, publish(mgr, m, out, std::move(copy)));
    };
    // GdipCloneBitmapArea(REAL x, REAL y, REAL w, REAL h, PixelFormat, GpBitmap*, GpBitmap**):
    // each REAL rounds independently (probe "clonearea(1.5,0,2.5,1)=3").
    map["GdipCloneBitmapArea"] = [&mgr](Marshaler& m) {
        finish(m, clone_area(mgr, m, gdip_round(real_arg(m, 0)), gdip_round(real_arg(m, 1)), gdip_round(real_arg(m, 2)),
                             gdip_round(real_arg(m, 3)), iarg(m, 4), handle_arg(m, 5), m.arg_ptr(6)));
    };
    map["GdipCloneBitmapAreaI"] = [&mgr](Marshaler& m) {
        finish(m, clone_area(mgr, m, iarg(m, 0), iarg(m, 1), iarg(m, 2), iarg(m, 3), iarg(m, 4), handle_arg(m, 5),
                             m.arg_ptr(6)));
    };
    map["GdipDisposeImage"] = [&mgr](Marshaler& m) {
        const u64 h = handle_arg(m, 0);
        GpImage* img = image_of(mgr, h);
        if (img == nullptr) return finish(m, missing_object(mgr, h));
        discard_lock(mgr, *img);
        (void)mgr.objects().remove<GpImage>(h);
        finish(m, Status::Ok);
    };

    // ---- Getters ----------------------------------------------------------------------
    auto u32_getter = [&mgr](const char* name, u32 (*get)(const GpImage&), HandlerMap& hm) {
        hm[name] = [&mgr, get](Marshaler& m) {
            const GpImage* img = image_of(mgr, handle_arg(m, 0));
            if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
            const GuestAddr out = m.arg_ptr(1);
            if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
            finish(m, m.write_u32(out, get(*img)).is_ok() ? Status::Ok : Status::InvalidParameter);
        };
    };
    u32_getter("GdipGetImageWidth", [](const GpImage& i) { return static_cast<u32>(i.width); }, map);
    u32_getter("GdipGetImageHeight", [](const GpImage& i) { return static_cast<u32>(i.height); }, map);
    u32_getter("GdipGetImageFlags", [](const GpImage& i) { return i.flags; }, map);
    u32_getter("GdipGetImageType", [](const GpImage& i) { return static_cast<u32>(i.type); }, map);
    u32_getter("GdipGetImagePixelFormat", [](const GpImage& i) { return static_cast<u32>(i.pixel_format); }, map);
    u32_getter("GdipGetImagePaletteSize",
               [](const GpImage& i) { return static_cast<u32>(palette_bytes(i.palette.size())); }, map);
    map["GdipGetImageHorizontalResolution"] = [&mgr](Marshaler& m) {
        const GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr out = m.arg_ptr(1);
        if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        finish(m, write_f32(m, out, img->dpi_x) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipGetImageVerticalResolution"] = [&mgr](Marshaler& m) {
        const GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr out = m.arg_ptr(1);
        if (img == nullptr || out == 0) return finish(m, Status::InvalidParameter);
        finish(m, write_f32(m, out, img->dpi_y) ? Status::Ok : Status::InvalidParameter);
    };
    // GdipGetImageDimension(GpImage*, REAL* w, REAL* h): a bitmap's pixel
    // size (probe "bounds=... dim=3,2" at 48x192 dpi).
    map["GdipGetImageDimension"] = [&mgr](Marshaler& m) {
        const GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr w = m.arg_ptr(1), h = m.arg_ptr(2);
        if (img == nullptr || w == 0 || h == 0) return finish(m, Status::InvalidParameter);
        const bool bitmap = img->type == ImageTypeBitmap;
        const float fw = bitmap ? static_cast<float>(img->width) : img->physical_width;
        const float fh = bitmap ? static_cast<float>(img->height) : img->physical_height;
        finish(m, write_f32(m, w, fw) && write_f32(m, h, fh) ? Status::Ok : Status::InvalidParameter);
    };
    // GdipGetImageBounds(GpImage*, GpRectF*, GpUnit*): (0, 0, w, h) UnitPixel for a bitmap.
    map["GdipGetImageBounds"] = [&mgr](Marshaler& m) {
        const GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr r = m.arg_ptr(1), u = m.arg_ptr(2);
        if (img == nullptr || r == 0 || u == 0) return finish(m, Status::InvalidParameter);
        const bool bitmap = img->type == ImageTypeBitmap;
        const RectF b = bitmap ? RectF{0, 0, static_cast<float>(img->width), static_cast<float>(img->height)} : img->bounds;
        const u32 unit = bitmap ? 2u : static_cast<u32>(img->bounds_unit);
        finish(m, write_rect(m, r, b) && m.write_u32(u, unit).is_ok() ? Status::Ok : Status::InvalidParameter);
    };
    // GdipGetImagePalette(GpImage*, ColorPalette*, INT size): size must be
    // GdipGetImagePaletteSize's; an image without a palette writes only
    // Count = 0 (probe "pal24 ... flags=eeeeeeee count=0").
    map["GdipGetImagePalette"] = [&mgr](Marshaler& m) {
        const GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr out = m.arg_ptr(1);
        const i32 size = iarg(m, 2);
        if (img == nullptr || out == 0 || size < palette_bytes(img->palette.size())) return finish(m, Status::InvalidParameter);
        if (img->palette.empty()) return finish(m, m.write_u32(out + 4, 0).is_ok() ? Status::Ok : Status::InvalidParameter);
        finish(m, write_palette(m, out, img->palette_flags, img->palette) ? Status::Ok : Status::InvalidParameter);
    };
    // GdipSetImagePalette(GpImage*, const ColorPalette*): 1..256 entries,
    // stored for any format (probe "setpal32", "setpal1_count4 psize=24").
    map["GdipSetImagePalette"] = [&mgr](Marshaler& m) {
        GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr pal = m.arg_ptr(1);
        if (img == nullptr || pal == 0) return finish(m, Status::InvalidParameter);
        u32 flags = 0;
        std::vector<u32> entries;
        if (!read_palette(m, pal, flags, entries)) return finish(m, Status::InvalidParameter);
        img->palette = std::move(entries);
        img->palette_flags = flags;
        finish(m, Status::Ok);
    };
    // GdipImageRotateFlip(GpImage*, RotateFlipType 0..7)
    map["GdipImageRotateFlip"] = [&mgr](Marshaler& m) {
        GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const i32 type = iarg(m, 1);
        if (img == nullptr || type < 0 || type > 7) return finish(m, Status::InvalidParameter);
        if (img->type != ImageTypeBitmap) {
            log_unimplemented("GdipImageRotateFlip", "rotating a metafile");
            return finish(m, Status::NotImplemented);
        }
        discard_lock(mgr, *img);  // probe "locked: ... rotate=0", then unlock=7
        finish(m, rotate_flip_image(m, *img, type) ? Status::Ok : Status::InvalidParameter);
    };
    map["GdipImageForceValidation"] = [&mgr](Marshaler& m) {
        finish(m, image_of(mgr, handle_arg(m, 0)) != nullptr ? Status::Ok : missing_object(mgr, handle_arg(m, 0)));
    };
    // GdipImageSetAbort(GpImage*, GdiplusAbort*): the object DrawImage polls.
    map["GdipImageSetAbort"] = [&mgr](Marshaler& m) {
        GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        if (img == nullptr) return finish(m, Status::InvalidParameter);
        img->abort_object = m.arg_ptr(1);
        finish(m, Status::Ok);
    };
    auto find_item = [&mgr](Marshaler& m, bool first) {
        GpImage* img = image_of(mgr, handle_arg(m, 0));
        if (img == nullptr) return finish(m, missing_object(mgr, handle_arg(m, 0)));
        const GuestAddr item = m.arg_ptr(1);
        if (img == nullptr || item == 0) return finish(m, Status::InvalidParameter);
        if (g_image_item_finder == nullptr || img->codec.get() == nullptr)
            return finish(m, Status::InvalidParameter);  // probe "finditem=2" (a memory bitmap)
        finish(m, g_image_item_finder(mgr, m, *img, first, item));
    };
    map["GdipFindFirstImageItem"] = [find_item](Marshaler& m) { find_item(m, true); };
    map["GdipFindNextImageItem"] = [find_item](Marshaler& m) { find_item(m, false); };

    register_image_attributes(mgr, map);
    register_image_draw(mgr, map);
}

}  // namespace vitreo::winapi::gdip
