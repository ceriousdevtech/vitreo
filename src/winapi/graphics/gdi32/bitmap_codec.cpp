// SPDX-License-Identifier: LGPL-2.1-only

/// @file bitmap_codec.cpp
/// @brief Scan-line <-> RGBA conversion for DIBs and device-dependent bitmaps.

#include "winapi/graphics/gdi32/bitmap_codec.hpp"

#include "core/memory/guest_memory.hpp"

#include <bit>
#include <cstring>
#include <limits>

namespace vitreo::winapi::gdi {

namespace {

[[nodiscard]] inline u32 load_le(const u8* p, u32 bytes) noexcept {
    u32 v = 0;
    for (u32 i = 0; i < bytes; ++i) v |= static_cast<u32>(p[i]) << (8 * i);
    return v;
}

inline void store_le(u8* p, u32 v, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i) p[i] = static_cast<u8>(v >> (8 * i));
}

/// One BI_BITFIELDS channel: where it sits and how wide it is.
struct Channel {
    u32 shift = 0;
    u32 bits = 0;
};

[[nodiscard]] Channel channel_of(u32 mask) noexcept {
    if (mask == 0) return {};
    const u32 shift = static_cast<u32>(std::countr_zero(mask));
    return {shift, static_cast<u32>(std::popcount(mask >> shift))};
}

/// Widen an n-bit channel to 8 bits by repeating its bit pattern, which is
/// how GDI expands 555/565 and narrower bitfields (1 -> 0x08 for 5 bits,
/// 33 -> 0x86 for 6 bits, 1 -> 0x11 for 4 bits; s3gdi_blit_probe).
[[nodiscard]] u8 widen(u32 v, u32 bits) noexcept {
    if (bits == 0) return 0;
    if (bits >= 8) return static_cast<u8>(v >> (bits - 8));
    u32 out = 0;
    for (i32 shift = 8 - static_cast<i32>(bits); shift > -static_cast<i32>(bits);
         shift -= static_cast<i32>(bits)) {
        out |= shift >= 0 ? (v << shift) : (v >> -shift);
    }
    return static_cast<u8>(out);
}

/// Narrow an 8-bit channel to n bits by truncation (s3gdi_bitmap_probe
/// "gdib16": 0x80 -> 16 for 5 bits).
[[nodiscard]] u32 narrow(u8 v, u32 bits) noexcept {
    if (bits == 0) return 0;
    if (bits >= 8) return static_cast<u32>(v) << (bits - 8);
    return static_cast<u32>(v) >> (8 - bits);
}

constexpr std::array<u32, 3> kMasks555 = {0x7C00, 0x03E0, 0x001F};

}  // namespace

PixelFormat ddb_pixel_format(u16 bpp) {
    PixelFormat f;
    f.bpp = bpp;
    switch (bpp) {
    case 1: f.palette = {0x000000, 0xFFFFFF}; break;
    case 4: f.palette.assign(kGdiDefaultPalette4.begin(), kGdiDefaultPalette4.end()); break;
    case 8: f.palette.assign(kGdiDefaultPalette8.begin(), kGdiDefaultPalette8.end()); break;
    default: break;
    }
    return f;
}

u32 nearest_palette_index(std::span<const u32> palette, u8 r, u8 g, u8 b) noexcept {
    u32 best = 0;
    u32 best_distance = std::numeric_limits<u32>::max();
    for (u32 i = 0; i < palette.size(); ++i) {
        const i32 dr = static_cast<i32>(r) - static_cast<i32>((palette[i] >> 16) & 0xFF);
        const i32 dg = static_cast<i32>(g) - static_cast<i32>((palette[i] >> 8) & 0xFF);
        const i32 db = static_cast<i32>(b) - static_cast<i32>(palette[i] & 0xFF);
        const u32 d = static_cast<u32>(dr * dr + dg * dg + db * db);
        if (d < best_distance) {
            best_distance = d;
            best = i;
            if (d == 0) break;
        }
    }
    return best;
}

void decode_row(const PixelFormat& f, const u8* row, u8* rgba, i32 count, i32 first) {
    auto put_index = [&](u8* d, u32 index) {
        const u32 c = index < f.palette.size() ? f.palette[index] : 0u;
        d[0] = static_cast<u8>(c >> 16);
        d[1] = static_cast<u8>(c >> 8);
        d[2] = static_cast<u8>(c);
        d[3] = 0;
    };
    switch (f.bpp) {
    case 1:
        for (i32 i = 0; i < count; ++i) {
            const i32 x = first + i;
            put_index(rgba + i * 4, (row[x >> 3] >> (7 - (x & 7))) & 1u);
        }
        return;
    case 4:
        for (i32 i = 0; i < count; ++i) {
            const i32 x = first + i;
            const u8 byte = row[x >> 1];
            put_index(rgba + i * 4, (x & 1) ? (byte & 0x0Fu) : (byte >> 4));
        }
        return;
    case 8:
        for (i32 i = 0; i < count; ++i) put_index(rgba + i * 4, row[first + i]);
        return;
    case 24:
        for (i32 i = 0; i < count; ++i) {
            const u8* p = row + (first + i) * 3;
            u8* d = rgba + i * 4;
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = 0;
        }
        return;
    case 16:
    case 32: {
        const u32 bytes = f.bpp / 8u;
        if (f.bpp == 32 && !f.bitfields) {
            for (i32 i = 0; i < count; ++i) {
                const u8* p = row + (first + i) * 4;
                u8* d = rgba + i * 4;
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = p[3];
            }
            return;
        }
        const auto& m = f.bitfields ? f.masks : kMasks555;
        const Channel cr = channel_of(m[0]), cg = channel_of(m[1]), cb = channel_of(m[2]);
        for (i32 i = 0; i < count; ++i) {
            const u32 v = load_le(row + (first + i) * static_cast<i32>(bytes), bytes);
            u8* d = rgba + i * 4;
            d[0] = widen((v & m[0]) >> cr.shift, cr.bits);
            d[1] = widen((v & m[1]) >> cg.shift, cg.bits);
            d[2] = widen((v & m[2]) >> cb.shift, cb.bits);
            d[3] = 0;
        }
        return;
    }
    default:
        std::memset(rgba, 0, static_cast<usize>(count) * 4u);
        return;
    }
}

void encode_row(const PixelFormat& f, const u8* rgba, u8* row, i32 count, i32 first) {
    // Real images repeat a handful of colours, so a direct-mapped memo keeps
    // the nearest-colour search off the per-pixel path for indexed formats.
    struct Memo { u32 key = 0xFFFFFFFFu; u32 index = 0; };
    std::array<Memo, 256> memo{};
    auto index_of = [&](const u8* p) {
        const u32 key = (static_cast<u32>(p[0]) << 16) | (static_cast<u32>(p[1]) << 8) | p[2];
        Memo& slot = memo[(key ^ (key >> 8) ^ (key >> 16)) & 0xFFu];
        if (slot.key != key) {
            slot.key = key;
            slot.index = nearest_palette_index(f.palette, p[0], p[1], p[2]);
        }
        return slot.index;
    };
    switch (f.bpp) {
    case 1:
        for (i32 i = 0; i < count; ++i) {
            const i32 x = first + i;
            const u8 bit = static_cast<u8>(0x80u >> (x & 7));
            if (index_of(rgba + i * 4) & 1u) row[x >> 3] |= bit;
            else row[x >> 3] &= static_cast<u8>(~bit);
        }
        return;
    case 4:
        for (i32 i = 0; i < count; ++i) {
            const i32 x = first + i;
            const u8 v = static_cast<u8>(index_of(rgba + i * 4) & 0x0Fu);
            u8& b = row[x >> 1];
            b = (x & 1) ? static_cast<u8>((b & 0xF0) | v) : static_cast<u8>((b & 0x0F) | (v << 4));
        }
        return;
    case 8:
        for (i32 i = 0; i < count; ++i) row[first + i] = static_cast<u8>(index_of(rgba + i * 4));
        return;
    case 24:
        for (i32 i = 0; i < count; ++i) {
            const u8* p = rgba + i * 4;
            u8* d = row + (first + i) * 3;
            d[0] = p[2]; d[1] = p[1]; d[2] = p[0];
        }
        return;
    case 16:
    case 32: {
        const u32 bytes = f.bpp / 8u;
        if (f.bpp == 32 && !f.bitfields) {
            for (i32 i = 0; i < count; ++i) {
                const u8* p = rgba + i * 4;
                u8* d = row + (first + i) * 4;
                d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; d[3] = p[3];
            }
            return;
        }
        const auto& m = f.bitfields ? f.masks : kMasks555;
        const Channel cr = channel_of(m[0]), cg = channel_of(m[1]), cb = channel_of(m[2]);
        for (i32 i = 0; i < count; ++i) {
            const u8* p = rgba + i * 4;
            const u32 v = ((narrow(p[0], cr.bits) << cr.shift) & m[0]) |
                          ((narrow(p[1], cg.bits) << cg.shift) & m[1]) |
                          ((narrow(p[2], cb.bits) << cb.shift) & m[2]);
            store_le(row + (first + i) * static_cast<i32>(bytes), v, bytes);
        }
        return;
    }
    default:
        return;
    }
}

u32 DibHeader::color_count() const noexcept {
    if (bpp > 8) return 0;
    const u32 full = 1u << bpp;
    if (core() || clr_used == 0) return full;
    return clr_used < full ? clr_used : full;
}

GuestAddr DibHeader::mask_address() const noexcept {
    // BITMAPINFOHEADER is followed by the three masks; V2..V5 headers carry
    // them at the same offset inside the header.
    return address + 40;
}

GuestAddr DibHeader::color_table() const noexcept {
    if (header_size == 40 && compression == kBiBitfields) return address + 52;
    return address + header_size;
}

bool read_dib_header(GuestMemory& memory, GuestAddr bmi, DibHeader& out) {
    out = {};
    if (bmi == 0) return false;
    const auto size = memory.read_u32(bmi);
    if (size.is_err()) return false;
    out.address = bmi;
    out.header_size = size.value();
    if (out.header_size == 12) {
        u8 core[12]{};
        if (memory.read_bytes(bmi, core, sizeof(core)).is_err()) return false;
        out.width = static_cast<i32>(load_le(core + 4, 2));
        out.height = static_cast<i32>(load_le(core + 6, 2));
        out.planes = static_cast<u16>(load_le(core + 8, 2));
        out.bpp = static_cast<u16>(load_le(core + 10, 2));
        return true;
    }
    if (out.header_size < 40 || out.header_size > 124) return false;
    u8 h[40]{};
    if (memory.read_bytes(bmi, h, sizeof(h)).is_err()) return false;
    out.width = static_cast<i32>(load_le(h + 4, 4));
    out.height = static_cast<i32>(load_le(h + 8, 4));
    out.planes = static_cast<u16>(load_le(h + 12, 2));
    out.bpp = static_cast<u16>(load_le(h + 14, 2));
    out.compression = load_le(h + 16, 4);
    out.size_image = load_le(h + 20, 4);
    out.x_pels_per_meter = static_cast<i32>(load_le(h + 24, 4));
    out.y_pels_per_meter = static_cast<i32>(load_le(h + 28, 4));
    out.clr_used = load_le(h + 32, 4);
    out.clr_important = load_le(h + 36, 4);
    if (out.compression == kBiBitfields) {
        u8 m[12]{};
        if (memory.read_bytes(out.mask_address(), m, sizeof(m)).is_err()) return false;
        out.masks = {load_le(m, 4), load_le(m + 4, 4), load_le(m + 8, 4)};
    }
    return true;
}

bool dib_is_decodable(const DibHeader& h) noexcept {
    switch (h.bpp) {
    case 1: case 4: case 8: case 24:
        return h.compression == kBiRgb;
    case 16: case 32:
        if (h.compression == kBiRgb) return true;
        return h.compression == kBiBitfields &&
               (h.masks[0] | h.masks[1] | h.masks[2]) != 0;
    default:
        return false;
    }
}

bool dib_pixel_format(GuestMemory& memory, const DibHeader& h, u32 usage,
                      std::span<const u32> logical_palette, PixelFormat& out) {
    out = {};
    out.bpp = h.bpp;
    if ((h.bpp == 16 || h.bpp == 32) && h.compression == kBiBitfields) {
        out.bitfields = true;
        out.masks = h.masks;
    }
    const u32 entries = h.color_count();
    if (entries == 0) return true;
    out.palette.resize(entries);
    if (usage == kDibPalColors) {
        std::vector<u8> words(entries * 2u);
        if (memory.read_bytes(h.color_table(), words.data(), words.size()).is_err()) return false;
        for (u32 i = 0; i < entries; ++i) {
            // Indices wrap around the logical palette (a DIB_PAL_COLORS index
            // 25 against the 20-entry default palette is entry 5).
            if (logical_palette.empty()) return false;
            const u32 index = load_le(words.data() + i * 2u, 2);
            out.palette[i] = logical_palette[index % logical_palette.size()];
        }
        return true;
    }
    const u32 entry_size = h.core() ? 3u : 4u;
    std::vector<u8> table(entries * entry_size);
    if (memory.read_bytes(h.color_table(), table.data(), table.size()).is_err()) return false;
    for (u32 i = 0; i < entries; ++i) {
        const u8* q = table.data() + i * entry_size;  // B, G, R[, reserved]
        out.palette[i] = (static_cast<u32>(q[2]) << 16) | (static_cast<u32>(q[1]) << 8) | q[0];
    }
    return true;
}

}  // namespace vitreo::winapi::gdi
