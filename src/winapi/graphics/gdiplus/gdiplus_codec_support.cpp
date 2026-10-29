// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_codec_support.cpp
/// @brief Codec-area plumbing: per-image state, GUIDs, guest calls and
/// IStreams, guest files, and the pixel-format conversions every encoder and
/// effect reads images through.

#include "winapi/graphics/gdiplus/gdiplus_codec.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vitreo::winapi::gdip {

// ---- CodecRef ---------------------------------------------------------------------

CodecRef::CodecRef(const CodecRef& other) {
    if (other.m_state) {
        auto copy = std::make_shared<CodecState>(*other.m_state);
        copy->session.reset();
        m_state = std::move(copy);
    }
}

CodecRef& CodecRef::operator=(const CodecRef& other) {
    if (this != &other) {
        CodecRef tmp(other);
        m_state = std::move(tmp.m_state);
    }
    return *this;
}

CodecRef::~CodecRef() = default;

CodecState& CodecRef::ensure() {
    if (!m_state) m_state = std::make_shared<CodecState>();
    return *m_state;
}

// ---- GUIDs --------------------------------------------------------------------------

bool read_guid(Marshaler& m, GuestAddr at, Guid& out) {
    if (at == 0) return false;
    return m.memory().read_bytes(at, out.data(), out.size()).is_ok();
}

bool write_guid(Marshaler& m, GuestAddr at, const Guid& g) {
    return at != 0 && m.memory().write_bytes(at, g.data(), g.size()).is_ok();
}

// ---- Guest calls, memory, streams, files ----------------------------------------------

GuestCaller guest_caller(GdiplusManager& mgr) {
    if (mgr.gdi() == nullptr) return {};
    GdiManager::GuestCall call = mgr.gdi()->guest_call();
    if (!call) return {};
    return [call = std::move(call)](GuestAddr f, const u64* args, usize n, bool is64) {
        return call(f, args, n, is64);
    };
}

GuestBlock::GuestBlock(GdiplusManager& mgr, u64 size) : m_mgr(mgr) {
    const GuestAddr heap = mgr.heap().get_process_heap();
    m_addr = mgr.heap().heap_alloc(heap, 0, size == 0 ? 1 : size);
}

GuestBlock::~GuestBlock() {
    if (m_addr != 0) (void)m_mgr.heap().heap_free(m_mgr.heap().get_process_heap(), 0, m_addr);
}

GuestStream::GuestStream(GdiplusManager& mgr, Marshaler& m, GuestAddr stream)
    : m_mgr(mgr), m_m(m), m_stream(stream), m_call(guest_caller(mgr)), m_64(m.cpu_is_64bit()) {}

u64 GuestStream::call(usize slot, std::initializer_list<u64> args) {
    const auto vtbl = m_m.read_ptr(m_stream);
    if (vtbl.is_err() || vtbl.value() == 0) return 0x80004005u;  // E_FAIL
    const auto fn = m_m.read_ptr(vtbl.value() + slot * (m_64 ? 8 : 4));
    if (fn.is_err() || fn.value() == 0) return 0x80004005u;
    std::vector<u64> a;
    a.reserve(args.size() + 1);
    a.push_back(m_stream);
    a.insert(a.end(), args.begin(), args.end());
    return m_call(fn.value(), a.data(), a.size(), m_64);
}

static bool hr_ok(u64 hr) { return static_cast<i32>(static_cast<u32>(hr)) >= 0; }

bool GuestStream::seek(i64 move, u32 origin, u64* new_position) {
    if (!usable()) return false;
    GuestBlock pos(m_mgr, 8);
    if (pos.addr() == 0) return false;
    const u64 mv = static_cast<u64>(move);
    // Seek(this, LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition):
    // the LARGE_INTEGER is two stack dwords on x86, one register on x64.
    const u64 hr = m_64 ? call(5, {mv, origin, pos.addr()})
                        : call(5, {mv & 0xFFFFFFFFu, mv >> 32, origin, pos.addr()});
    if (!hr_ok(hr)) return false;
    if (new_position != nullptr) {
        const auto v = m_m.read_u64(pos.addr());
        if (v.is_err()) return false;
        *new_position = v.value();
    }
    return true;
}

std::optional<u64> GuestStream::tell() {
    u64 p = 0;
    if (!seek(0, 1, &p)) return std::nullopt;
    return p;
}

std::optional<u64> GuestStream::read(u8* out, u64 size) {
    if (!usable()) return std::nullopt;
    GuestBlock buf(m_mgr, size + 8);
    if (buf.addr() == 0) return std::nullopt;
    const GuestAddr count_at = buf.addr() + size;
    (void)m_m.write_u32(count_at, 0);
    const u64 hr = call(3, {buf.addr(), size, count_at});
    if (!hr_ok(hr)) return std::nullopt;
    const auto got = m_m.read_u32(count_at);
    if (got.is_err()) return std::nullopt;
    const u64 n = std::min<u64>(got.value(), size);
    if (n != 0 && m_m.memory().read_bytes(buf.addr(), out, n).is_err()) return std::nullopt;
    return n;
}

std::optional<std::vector<u8>> GuestStream::read_to_end() {
    std::vector<u8> all;
    constexpr u64 kChunk = 64 * 1024;
    std::vector<u8> chunk(kChunk);
    for (;;) {
        const auto n = read(chunk.data(), kChunk);
        if (!n) return std::nullopt;
        if (*n == 0) break;
        all.insert(all.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*n));
        if (*n < kChunk) break;
    }
    return all;
}

bool GuestStream::write(std::span<const u8> data) {
    if (!usable()) return false;
    constexpr usize kChunk = 64 * 1024;
    GuestBlock buf(m_mgr, std::min(data.size(), kChunk) + 8);
    if (buf.addr() == 0) return false;
    for (usize at = 0; at < data.size();) {
        const usize n = std::min(kChunk, data.size() - at);
        const GuestAddr count_at = buf.addr() + std::min(data.size(), kChunk);
        if (m_m.memory().write_bytes(buf.addr(), data.data() + at, n).is_err()) return false;
        (void)m_m.write_u32(count_at, 0);
        const u64 hr = call(4, {buf.addr(), n, count_at});
        const auto wrote = m_m.read_u32(count_at);
        if (!hr_ok(hr) || wrote.is_err() || wrote.value() != n) return false;
        at += n;
    }
    return true;
}

std::optional<std::vector<u8>> read_guest_file(GdiplusManager& mgr, const std::string& path) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr || path.empty()) return std::nullopt;
    auto opened = vfs->open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                            capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;
    std::vector<u8> bytes;
    std::vector<u8> chunk(64 * 1024);
    for (;;) {
        auto r = vfs->read(opened.value(), chunk);
        if (r.is_err() || r.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(r.value()));
    }
    (void)vfs->close(opened.value());
    return bytes;
}

bool write_guest_file(GdiplusManager& mgr, const std::string& path, std::span<const u8> data) {
    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr || path.empty()) return false;
    auto opened = vfs->open(path, capsule::AccessMode::Write, capsule::ShareMode::None,
                            capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;
    bool ok = true;
    for (usize at = 0; at < data.size();) {
        auto w = vfs->write(opened.value(), data.subspan(at));
        if (w.is_err() || w.value() == 0) { ok = false; break; }
        at += w.value();
    }
    (void)vfs->close(opened.value());
    return ok;
}

std::string read_path(Marshaler& m, GuestAddr wide) {
    if (wide == 0) return {};
    auto s = m.read_wide_string(wide);
    return s.is_ok() ? s.value() : std::string{};
}

// ---- Pixels -----------------------------------------------------------------------------

i32 stride_for(i32 width, i32 format) {
    const i64 bits = static_cast<i64>(width) * pixel_format_bits(format);
    return static_cast<i32>(((bits + 31) / 32) * 4);
}

namespace {

/// GDI+'s extended (48/64 bpp) formats hold linear light in 0..8192
/// (scRGB, gamma 1.0); 8-bit channels are sRGB-encoded (gamma 2.2).
u8 linear_to_8(u16 v) {
    if (v >= 8192) return 255;
    return static_cast<u8>(std::lround(std::pow(static_cast<double>(v) / 8192.0, 1.0 / 2.2) * 255.0));
}
u16 to_linear(u8 v) {
    return static_cast<u16>(std::lround(std::pow(static_cast<double>(v) / 255.0, 2.2) * 8192.0));
}

bool rows_of(Marshaler& m, const GpImage& image, std::vector<u8>& rows) {
    const usize row = static_cast<usize>(std::abs(image.stride));
    const usize size = row * static_cast<usize>(image.height);
    if (image.user_scan0 == 0) {
        if (image.bits.size() < size) return false;
        rows.assign(image.bits.begin(), image.bits.begin() + static_cast<std::ptrdiff_t>(size));
        return true;
    }
    rows.resize(size);
    for (i32 y = 0; y < image.height; ++y) {
        const GuestAddr at = image.user_scan0 + static_cast<i64>(y) * image.stride;
        if (m.memory().read_bytes(at, rows.data() + static_cast<usize>(y) * row, row).is_err()) return false;
    }
    return true;
}

bool store_rows(Marshaler& m, GpImage& image, const std::vector<u8>& rows) {
    const usize row = static_cast<usize>(std::abs(image.stride));
    if (image.user_scan0 == 0) {
        image.bits = rows;
        return true;
    }
    for (i32 y = 0; y < image.height; ++y) {
        const GuestAddr at = image.user_scan0 + static_cast<i64>(y) * image.stride;
        if (m.memory().write_bytes(at, rows.data() + static_cast<usize>(y) * row, row).is_err()) return false;
    }
    return true;
}

u32 nearest_index(const std::vector<u32>& palette, u32 c) {
    u32 best = 0;
    i64 best_d = -1;
    for (u32 i = 0; i < palette.size(); ++i) {
        const u32 p = palette[i];
        if (p == c) return i;
        i64 d = 0;
        for (int s = 0; s < 32; s += 8) {
            const i64 k = static_cast<i64>((p >> s) & 0xFF) - static_cast<i64>((c >> s) & 0xFF);
            d += k * k;
        }
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return best;
}

u32 decode_pixel(const u8* row, i32 x, i32 format, const std::vector<u32>& palette) {
    auto pal = [&](u32 i) { return i < palette.size() ? palette[i] : 0xFF000000u; };
    switch (format) {
        case PixelFormat1bppIndexed: return pal((row[x >> 3] >> (7 - (x & 7))) & 1);
        case PixelFormat4bppIndexed: return pal((x & 1) ? (row[x >> 1] & 0x0F) : (row[x >> 1] >> 4));
        case PixelFormat8bppIndexed: return pal(row[x]);
        case PixelFormat16bppGrayScale: {
            u16 v = 0;
            std::memcpy(&v, row + x * 2, 2);
            const u32 g = linear_to_8(v);
            return 0xFF000000u | (g << 16) | (g << 8) | g;
        }
        case PixelFormat16bppRGB555:
        case PixelFormat16bppARGB1555:
        case PixelFormat16bppRGB565: {
            u16 v = 0;
            std::memcpy(&v, row + x * 2, 2);
            u32 r, g, b, a = 0xFF;
            if (format == PixelFormat16bppRGB565) {
                r = (v >> 11) & 31; g = (v >> 5) & 63; b = v & 31;
                r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
            } else {
                r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
                r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
                if (format == PixelFormat16bppARGB1555) a = (v & 0x8000) ? 0xFF : 0;
            }
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
        case PixelFormat24bppRGB: {
            const u8* p = row + x * 3;
            return 0xFF000000u | (u32{p[2]} << 16) | (u32{p[1]} << 8) | p[0];
        }
        case PixelFormat32bppRGB: {
            const u8* p = row + x * 4;
            return 0xFF000000u | (u32{p[2]} << 16) | (u32{p[1]} << 8) | p[0];
        }
        case PixelFormat32bppARGB: {
            u32 v = 0;
            std::memcpy(&v, row + x * 4, 4);
            return v;
        }
        case PixelFormat32bppPARGB: {
            const u8* p = row + x * 4;
            const u32 a = p[3];
            if (a == 0) return 0;
            auto un = [a](u32 c) { return std::min<u32>(255, (c * 255 + a / 2) / a); };
            return (a << 24) | (un(p[2]) << 16) | (un(p[1]) << 8) | un(p[0]);
        }
        case PixelFormat48bppRGB: {
            u16 c[3];
            std::memcpy(c, row + x * 6, 6);
            return 0xFF000000u | (u32{linear_to_8(c[2])} << 16) | (u32{linear_to_8(c[1])} << 8) | linear_to_8(c[0]);
        }
        case PixelFormat64bppARGB:
        case PixelFormat64bppPARGB: {
            u16 c[4];
            std::memcpy(c, row + x * 8, 8);
            const u32 a = c[3] >= 8192 ? 255 : static_cast<u32>((c[3] * 255u + 4096u) / 8192u);
            if (format == PixelFormat64bppPARGB && c[3] != 0) {
                for (int i = 0; i < 3; ++i)
                    c[i] = static_cast<u16>(std::min<u32>(8192u, (u32{c[i]} * 8192u + c[3] / 2u) / c[3]));
            }
            return (a << 24) | (u32{linear_to_8(c[2])} << 16) | (u32{linear_to_8(c[1])} << 8) | linear_to_8(c[0]);
        }
        default: return 0;
    }
}

void encode_pixel(u8* row, i32 x, i32 format, u32 c, const std::vector<u32>& palette) {
    const u32 a = c >> 24, r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    switch (format) {
        case PixelFormat1bppIndexed: {
            const u32 i = nearest_index(palette, c) & 1;
            row[x >> 3] = static_cast<u8>((row[x >> 3] & ~(0x80 >> (x & 7))) | (i << (7 - (x & 7))));
            break;
        }
        case PixelFormat4bppIndexed: {
            const u32 i = nearest_index(palette, c) & 15;
            row[x >> 1] = (x & 1) ? static_cast<u8>((row[x >> 1] & 0xF0) | i) : static_cast<u8>((row[x >> 1] & 0x0F) | (i << 4));
            break;
        }
        case PixelFormat8bppIndexed: row[x] = static_cast<u8>(nearest_index(palette, c)); break;
        case PixelFormat16bppGrayScale: {
            const u16 v = to_linear(static_cast<u8>(std::lround(0.299 * r + 0.587 * g + 0.114 * b)));
            std::memcpy(row + x * 2, &v, 2);
            break;
        }
        case PixelFormat16bppRGB555: {
            const u16 v = static_cast<u16>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
            std::memcpy(row + x * 2, &v, 2);
            break;
        }
        case PixelFormat16bppARGB1555: {
            const u16 v = static_cast<u16>((a >= 128 ? 0x8000 : 0) | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
            std::memcpy(row + x * 2, &v, 2);
            break;
        }
        case PixelFormat16bppRGB565: {
            const u16 v = static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            std::memcpy(row + x * 2, &v, 2);
            break;
        }
        case PixelFormat24bppRGB: {
            u8* p = row + x * 3;
            p[0] = static_cast<u8>(b); p[1] = static_cast<u8>(g); p[2] = static_cast<u8>(r);
            break;
        }
        case PixelFormat32bppRGB: {
            u8* p = row + x * 4;
            p[0] = static_cast<u8>(b); p[1] = static_cast<u8>(g); p[2] = static_cast<u8>(r); p[3] = 0xFF;
            break;
        }
        case PixelFormat32bppARGB: std::memcpy(row + x * 4, &c, 4); break;
        case PixelFormat32bppPARGB: {
            u8* p = row + x * 4;
            p[0] = static_cast<u8>((b * a + 127) / 255);
            p[1] = static_cast<u8>((g * a + 127) / 255);
            p[2] = static_cast<u8>((r * a + 127) / 255);
            p[3] = static_cast<u8>(a);
            break;
        }
        case PixelFormat48bppRGB: {
            const u16 v[3] = {to_linear(static_cast<u8>(b)), to_linear(static_cast<u8>(g)), to_linear(static_cast<u8>(r))};
            std::memcpy(row + x * 6, v, 6);
            break;
        }
        case PixelFormat64bppARGB:
        case PixelFormat64bppPARGB: {
            const u16 la = static_cast<u16>((a * 8192u + 127u) / 255u);
            u16 v[4] = {to_linear(static_cast<u8>(b)), to_linear(static_cast<u8>(g)), to_linear(static_cast<u8>(r)), la};
            if (format == PixelFormat64bppPARGB)
                for (int i = 0; i < 3; ++i) v[i] = static_cast<u16>((u32{v[i]} * la + 4096u) / 8192u);
            std::memcpy(row + x * 8, v, 8);
            break;
        }
        default: break;
    }
}

bool format_supported(i32 f) {
    switch (f) {
        case PixelFormat1bppIndexed: case PixelFormat4bppIndexed: case PixelFormat8bppIndexed:
        case PixelFormat16bppGrayScale: case PixelFormat16bppRGB555: case PixelFormat16bppRGB565:
        case PixelFormat16bppARGB1555: case PixelFormat24bppRGB: case PixelFormat32bppRGB:
        case PixelFormat32bppARGB: case PixelFormat32bppPARGB: case PixelFormat48bppRGB:
        case PixelFormat64bppARGB: case PixelFormat64bppPARGB:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool read_argb(Marshaler& m, const GpImage& image, std::vector<u32>& argb) {
    if (image.type != ImageTypeBitmap || !format_supported(image.pixel_format)) {
        if (image.type == ImageTypeBitmap) log_unimplemented("codec", "pixel read of PixelFormat32bppCMYK / undefined format");
        return false;
    }
    std::vector<u8> rows;
    if (!rows_of(m, image, rows)) return false;
    const usize row = static_cast<usize>(std::abs(image.stride));
    argb.resize(static_cast<usize>(image.width) * static_cast<usize>(image.height));
    for (i32 y = 0; y < image.height; ++y) {
        const u8* r = rows.data() + static_cast<usize>(y) * row;
        for (i32 x = 0; x < image.width; ++x)
            argb[static_cast<usize>(y) * static_cast<usize>(image.width) + static_cast<usize>(x)] =
                decode_pixel(r, x, image.pixel_format, image.palette);
    }
    return true;
}

bool write_argb(Marshaler& m, GpImage& image, const std::vector<u32>& argb) {
    if (image.type != ImageTypeBitmap || !format_supported(image.pixel_format)) return false;
    if (argb.size() != static_cast<usize>(image.width) * static_cast<usize>(image.height)) return false;
    std::vector<u8> rows;
    if (!rows_of(m, image, rows)) return false;
    const usize row = static_cast<usize>(std::abs(image.stride));
    for (i32 y = 0; y < image.height; ++y) {
        u8* r = rows.data() + static_cast<usize>(y) * row;
        for (i32 x = 0; x < image.width; ++x)
            encode_pixel(r, x, image.pixel_format,
                         argb[static_cast<usize>(y) * static_cast<usize>(image.width) + static_cast<usize>(x)],
                         image.palette);
    }
    return store_rows(m, image, rows);
}

void argb_to_frame(const std::vector<u32>& argb, i32 width, i32 height, i32 format, const std::vector<u32>& palette,
                   DecodedFrame& out) {
    out.width = width;
    out.height = height;
    out.pixel_format = format;
    out.stride = stride_for(width, format);
    out.palette = palette;
    out.bits.assign(static_cast<usize>(out.stride) * static_cast<usize>(height), 0);
    for (i32 y = 0; y < height; ++y) {
        u8* r = out.bits.data() + static_cast<usize>(y) * static_cast<usize>(out.stride);
        for (i32 x = 0; x < width; ++x)
            encode_pixel(r, x, format, argb[static_cast<usize>(y) * static_cast<usize>(width) + static_cast<usize>(x)],
                         palette);
    }
}

void apply_frame(GpImage& image, DecodedFrame frame) {
    image.type = ImageTypeBitmap;
    image.width = frame.width;
    image.height = frame.height;
    image.pixel_format = frame.pixel_format;
    image.stride = frame.stride;
    image.bits = std::move(frame.bits);
    image.user_scan0 = 0;
    image.palette = std::move(frame.palette);
    image.palette_flags = frame.palette_flags;
    image.dpi_x = frame.dpi_x;
    image.dpi_y = frame.dpi_y;
    image.flags = frame.flags;
}

}  // namespace vitreo::winapi::gdip
