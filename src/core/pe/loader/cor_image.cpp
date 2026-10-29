// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
/// @file cor_image.cpp
/// @brief COR20 image facts and the 64-bit loader's PE32 -> PE32+ rewrite.

#include "core/pe/loader/cor_image.hpp"

#include <cstring>

namespace vitreo::pe {

namespace {

u32 rd32(std::span<const u8> f, usize off) {
    u32 v = 0;
    if (off + 4 <= f.size()) std::memcpy(&v, f.data() + off, 4);
    return v;
}
u16 rd16(std::span<const u8> f, usize off) {
    u16 v = 0;
    if (off + 2 <= f.size()) std::memcpy(&v, f.data() + off, 2);
    return v;
}

struct Headers {
    usize nt;        ///< "PE\0\0"
    usize opt;       ///< optional header
    u16 opt_size;
    u16 sections;
    bool pe32plus;
};

std::optional<Headers> headers(std::span<const u8> f) {
    if (f.size() < 0x40 || rd16(f, 0) != 0x5A4D) return std::nullopt;
    const usize nt = rd32(f, 0x3C);
    if (nt + 24 > f.size() || rd32(f, nt) != 0x00004550) return std::nullopt;
    Headers h{nt, nt + 24, rd16(f, nt + 20), rd16(f, nt + 6), false};
    const u16 magic = rd16(f, h.opt);
    if (magic != 0x10b && magic != 0x20b) return std::nullopt;
    h.pe32plus = magic == 0x20b;
    if (h.opt + h.opt_size > f.size()) return std::nullopt;
    return h;
}

std::optional<usize> rva_to_offset(std::span<const u8> f, const Headers& h, u32 rva) {
    const usize table = h.opt + h.opt_size;
    for (u16 i = 0; i < h.sections; ++i) {
        const usize s = table + 40u * i;
        if (s + 40 > f.size()) return std::nullopt;
        const u32 vsize = rd32(f, s + 8), va = rd32(f, s + 12), rsize = rd32(f, s + 16), raw = rd32(f, s + 20);
        const u32 span = vsize > rsize ? vsize : rsize;
        if (rva >= va && rva < va + span) {
            const usize off = static_cast<usize>(raw) + (rva - va);
            return off < f.size() ? std::optional<usize>(off) : std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<CorImageInfo> read_cor_image(std::span<const u8> file) {
    const auto h = headers(file);
    if (!h) return std::nullopt;
    const usize dirs = h->opt + (h->pe32plus ? 112 : 96);
    const u32 count = rd32(file, dirs - 4);
    if (count <= 14 || dirs + 15 * 8 > h->opt + h->opt_size) return std::nullopt;
    const u32 cor_rva = rd32(file, dirs + 14 * 8);
    if (cor_rva == 0 || rd32(file, dirs + 14 * 8 + 4) == 0) return std::nullopt;
    const auto cor = rva_to_offset(file, *h, cor_rva);
    // IMAGE_COR20_HEADER: cb, Major/MinorRuntimeVersion, MetaData{rva,size}, Flags.
    if (!cor || *cor + 20 > file.size()) return std::nullopt;
    CorImageInfo info;
    info.pe32plus = h->pe32plus;
    info.flags = rd32(file, *cor + 16);
    // Metadata root (ECMA-335 II.24.2.1): signature 0x424A5342, then at
    // +12 the version length and at +16 the version string.
    if (const auto md = rva_to_offset(file, *h, rd32(file, *cor + 8));
        md && rd32(file, *md) == 0x424A5342u) {
        const u32 len = rd32(file, *md + 12);
        if (len <= 255 && *md + 16 + len <= file.size()) {
            const char* s = reinterpret_cast<const char*>(file.data() + *md + 16);
            info.metadata_version.assign(s, strnlen(s, len));
        }
    }
    return info;
}

bool rewrite_il_only_pe32_as_pe32plus(std::vector<u8>& file) {
    const auto cor = read_cor_image(file);
    if (!il_only_pe32_runs_64bit(cor)) return false;
    const auto h = headers(file);
    if (!h || h->opt_size < 96) return false;
    const usize old_table = h->opt + h->opt_size;
    const usize table_bytes = 40u * h->sections;
    const usize size_of_headers = rd32(file, h->opt + 60);
    if (old_table + table_bytes + 16 > size_of_headers || size_of_headers > file.size()) return false;

    const usize o = h->opt;
    const u32 image_base = rd32(file, o + 28);
    const u32 stack_reserve = rd32(file, o + 72), stack_commit = rd32(file, o + 76);
    const u32 heap_reserve = rd32(file, o + 80), heap_commit = rd32(file, o + 84);
    // LoaderFlags, NumberOfRvaAndSizes, the data directories, then the
    // section table: one contiguous run that moves up 16 bytes.
    const usize tail_from = o + 88;
    const usize tail_to = old_table + table_bytes;
    std::memmove(file.data() + tail_from + 16, file.data() + tail_from, tail_to - tail_from);

    auto put16 = [&](usize at, u16 v) { std::memcpy(file.data() + at, &v, 2); };
    auto put64 = [&](usize at, u64 v) { std::memcpy(file.data() + at, &v, 8); };
    put16(h->nt + 20, static_cast<u16>(h->opt_size + 16));
    put16(o, 0x20b);
    put64(o + 24, image_base);
    put64(o + 72, stack_reserve);
    put64(o + 80, stack_commit);
    put64(o + 88, heap_reserve);
    put64(o + 96, heap_commit);
    return true;
}

}  // namespace vitreo::pe
