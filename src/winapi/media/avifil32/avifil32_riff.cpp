// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo -- avifil32 RIFF readers (AVI file handler, WAVE file handler).
//
// AVI RIFF File Reference:
// https://learn.microsoft.com/en-us/windows/win32/directshow/avi-riff-file-reference
// Behaviour the reference leaves open is measured: tools/oracle_probe/
// avifil32_probe.c on the files of avifil32_fixtures.h.

#include "winapi/media/avifil32/avifil32_riff.hpp"
#include "winapi/media/avifil32/avifil32_contract.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::avifil32 {

namespace {

constexpr u32 kRiff = fourcc('R', 'I', 'F', 'F');
constexpr u32 kList = fourcc('L', 'I', 'S', 'T');
constexpr u32 kAvi = fourcc('A', 'V', 'I', ' ');
constexpr u32 kWave = fourcc('W', 'A', 'V', 'E');
constexpr u32 kHdrl = fourcc('h', 'd', 'r', 'l');
constexpr u32 kStrl = fourcc('s', 't', 'r', 'l');
constexpr u32 kMovi = fourcc('m', 'o', 'v', 'i');
constexpr u32 kRec = fourcc('r', 'e', 'c', ' ');
constexpr u32 kAvih = fourcc('a', 'v', 'i', 'h');
constexpr u32 kStrh = fourcc('s', 't', 'r', 'h');
constexpr u32 kStrf = fourcc('s', 't', 'r', 'f');
constexpr u32 kStrd = fourcc('s', 't', 'r', 'd');
constexpr u32 kStrn = fourcc('s', 't', 'r', 'n');
constexpr u32 kIdx1 = fourcc('i', 'd', 'x', '1');
constexpr u32 kJunk = fourcc('J', 'U', 'N', 'K');
constexpr u32 kFmt = fourcc('f', 'm', 't', ' ');
constexpr u32 kData = fourcc('d', 'a', 't', 'a');
constexpr u16 kTwoccDb = static_cast<u16>('d' | ('b' << 8));
constexpr u16 kTwoccPc = static_cast<u16>('p' | ('c' << 8));
/// Header chunks larger than this are not read into memory.
constexpr u32 kMaxHeaderChunk = 16u << 20;

u32 le32(const u8* p) {
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
           (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24);
}
u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }

bool read32(ByteSource& src, u64 at, u32& v) {
    u8 b[4];
    if (!src.read(at, b, 4)) return false;
    v = le32(b);
    return true;
}

bool read_bytes(ByteSource& src, u64 at, u32 n, std::vector<u8>& out) {
    if (n > kMaxHeaderChunk) return false;
    out.resize(n);
    return n == 0 || src.read(at, out.data(), n);
}

int hex_value(u8 c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// The stream a movi chunk id ("##xx") belongs to, or -1.
int stream_of(u32 ckid) {
    const int hi = hex_value(static_cast<u8>(ckid & 0xFF));
    const int lo = hex_value(static_cast<u8>((ckid >> 8) & 0xFF));
    if (hi < 0 || lo < 0) return -1;
    return hi * 16 + lo;
}

u16 twocc_of(u32 ckid) { return static_cast<u16>(ckid >> 16); }

void parse_strh(const std::vector<u8>& d, AviStreamHeader& h) {
    u8 b[56]{};
    std::memcpy(b, d.data(), std::min<usize>(d.size(), sizeof b));
    h.fcc_type = le32(b + 0);
    h.fcc_handler = le32(b + 4);
    h.flags = le32(b + 8);
    h.priority = le16(b + 12);
    h.language = le16(b + 14);
    h.initial_frames = le32(b + 16);
    h.scale = le32(b + 20);
    h.rate = le32(b + 24);
    h.start = le32(b + 28);
    h.length = le32(b + 32);
    h.suggested_buffer = le32(b + 36);
    h.quality = le32(b + 40);
    h.sample_size = le32(b + 44);
    for (int i = 0; i < 4; ++i) h.frame[i] = static_cast<i16>(le16(b + 48 + i * 2));
}

void parse_avih(const std::vector<u8>& d, AviMainHeader& h) {
    u8 b[40]{};
    std::memcpy(b, d.data(), std::min<usize>(d.size(), sizeof b));
    h.usec_per_frame = le32(b + 0);
    h.max_bytes_per_sec = le32(b + 4);
    h.padding = le32(b + 8);
    h.flags = le32(b + 12);
    h.total_frames = le32(b + 16);
    h.initial_frames = le32(b + 20);
    h.streams = le32(b + 24);
    h.suggested_buffer = le32(b + 28);
    h.width = le32(b + 32);
    h.height = le32(b + 36);
}

/// Bounds of a chunk list: [begin, end) of child headers.
struct Span {
    u64 begin;
    u64 end;
};

bool parse_strl(ByteSource& src, Span span, AviStream& s) {
    bool have_header = false;
    for (u64 at = span.begin; at + 8 <= span.end;) {
        u32 id = 0, size = 0;
        if (!read32(src, at, id) || !read32(src, at + 4, size)) return false;
        const u64 data = at + 8;
        if (data + size > span.end) size = static_cast<u32>(span.end - data);
        std::vector<u8> bytes;
        if (id == kStrh) {
            if (!read_bytes(src, data, size, bytes)) return false;
            parse_strh(bytes, s.header);
            have_header = true;
        } else if (id == kStrf) {
            if (!read_bytes(src, data, size, s.format)) return false;
        } else if (id == kStrn) {
            if (!read_bytes(src, data, size, bytes)) return false;
            s.has_name = true;
            s.name.assign(bytes.begin(), std::find(bytes.begin(), bytes.end(), 0));
        } else if (id != kStrd && id != kJunk) {
            if (!read_bytes(src, data, size, bytes)) return false;
            s.extra.push_back(DataChunk{id, std::move(bytes)});
        }
        at = data + size + (size & 1);
    }
    return have_header;
}

/// Adds the chunk @p ckid at @p data_offset to its stream (a chunk of no
/// stream is ignored).
void add_stream_chunk(AviFileModel& file, u32 ckid, u64 data_offset, u32 size, u32 ordinal,
                      bool key) {
    const int index = stream_of(ckid);
    if (index < 0 || static_cast<usize>(index) >= file.streams.size()) return;
    AviStream& s = file.streams[static_cast<usize>(index)];
    const i64 next = static_cast<i64>(s.header.start) + static_cast<i64>(s.total_samples);
    if (twocc_of(ckid) == kTwoccPc) {
        // The AVIPALCHANGE bytes are loaded by load_palette_change.
        PaletteChange pc;
        pc.pos = next;
        pc.ordinal = ordinal;
        s.palette_changes.push_back(std::move(pc));
        return;
    }
    StreamChunk c;
    c.offset = data_offset;
    c.size = size;
    c.first = next;
    c.samples = s.header.sample_size != 0 ? size / s.header.sample_size : 1;
    c.ordinal = ordinal;
    c.key = key;
    s.chunks.push_back(c);
    s.total_samples += c.samples;
}

bool load_palette_change(ByteSource& src, AviFileModel& file, u32 ckid, u64 data_offset,
                         u32 size) {
    const int index = stream_of(ckid);
    if (index < 0 || static_cast<usize>(index) >= file.streams.size()) return true;
    AviStream& s = file.streams[static_cast<usize>(index)];
    if (twocc_of(ckid) != kTwoccPc || s.palette_changes.empty()) return true;
    auto& data = s.palette_changes.back().data;
    data.resize(std::min<u32>(size, kMaxHeaderChunk));
    return data.empty() || src.read(data_offset, data.data(), data.size());
}

bool index_from_idx1(ByteSource& src, AviFileModel& file, u64 idx_data, u32 idx_size,
                     u64 movi_type_pos) {
    const u32 count = idx_size / 16;
    if (count == 0) return false;
    // Offsets are relative to the 'movi' list type when the first entry does
    // not name its own chunk header in absolute terms.
    u8 first[16];
    if (!src.read(idx_data, first, 16)) return false;
    u64 base = movi_type_pos;
    u32 probe = 0;
    if (read32(src, le32(first + 8), probe) && probe == le32(first)) base = 0;
    std::vector<u8> table(static_cast<usize>(count) * 16);
    if (!src.read(idx_data, table.data(), table.size())) return false;
    for (u32 i = 0; i < count; ++i) {
        const u8* e = table.data() + static_cast<usize>(i) * 16;
        const u32 ckid = le32(e);
        const u32 flags = le32(e + 4);
        const u64 header = base + le32(e + 8);
        const u32 size = le32(e + 12);
        if ((flags & AVIIF_LIST) != 0 || ckid == kRec) continue;
        const bool key = (flags & AVIIF_KEYFRAME) != 0 || twocc_of(ckid) == kTwoccDb;
        add_stream_chunk(file, ckid, header + 8, size, i, key);
        if (!load_palette_change(src, file, ckid, header + 8, size)) return false;
    }
    return true;
}

void index_from_scan(ByteSource& src, AviFileModel& file, Span movi, u32& ordinal) {
    for (u64 at = movi.begin; at + 8 <= movi.end;) {
        u32 id = 0, size = 0;
        if (!read32(src, at, id) || !read32(src, at + 4, size)) return;
        const u64 data = at + 8;
        if (data + size > movi.end) size = static_cast<u32>(movi.end - data);
        const u32 my_ordinal = ordinal++;
        if (id == kList) {
            u32 type = 0;
            if (size >= 4 && read32(src, data, type) && type == kRec) {
                index_from_scan(src, file, Span{data + 4, data + size}, ordinal);
            }
        } else {
            const int index = stream_of(id);
            if (index >= 0 && static_cast<usize>(index) < file.streams.size()) {
                const bool first =
                    file.streams[static_cast<usize>(index)].chunks.empty();
                add_stream_chunk(file, id, data, size, my_ordinal, first);
                (void)load_palette_change(src, file, id, data, size);
            }
        }
        at = data + size + (size & 1);
    }
}

} // namespace

bool MemorySource::read(u64 offset, void* dst, usize n) {
    if (offset > bytes_.size() || n > bytes_.size() - offset) return false;
    if (n != 0) std::memcpy(dst, bytes_.data() + offset, n);
    return true;
}

u32 riff_form_type(ByteSource& src) {
    u8 h[12];
    if (!src.read(0, h, 12) || le32(h) != kRiff) return 0;
    return le32(h + 8);
}

u32 parse_avi(ByteSource& src, AviFileModel& out) {
    if (riff_form_type(src) != kAvi) return AVIERR_BADFORMAT;
    u32 riff_size = 0;
    if (!read32(src, 4, riff_size)) return AVIERR_FILEREAD;
    const u64 riff_end = std::min<u64>(src.size(), 8ull + riff_size);
    bool have_header = false;
    Span movi{0, 0};
    bool have_movi = false;
    u64 idx_data = 0;
    u32 idx_size = 0;
    bool have_idx1 = false;

    for (u64 at = 12; at + 8 <= riff_end;) {
        u32 id = 0, size = 0;
        if (!read32(src, at, id) || !read32(src, at + 4, size)) return AVIERR_FILEREAD;
        const u64 data = at + 8;
        if (data + size > riff_end) size = static_cast<u32>(riff_end - data);
        u32 type = 0;
        if (id == kList && size >= 4 && !read32(src, data, type)) return AVIERR_FILEREAD;
        if (id == kList && type == kHdrl) {
            const u64 end = data + size;
            for (u64 h = data + 4; h + 8 <= end;) {
                u32 hid = 0, hsize = 0;
                if (!read32(src, h, hid) || !read32(src, h + 4, hsize)) return AVIERR_FILEREAD;
                const u64 hdata = h + 8;
                if (hdata + hsize > end) hsize = static_cast<u32>(end - hdata);
                u32 htype = 0;
                if (hid == kList && hsize >= 4 && !read32(src, hdata, htype)) {
                    return AVIERR_FILEREAD;
                }
                std::vector<u8> bytes;
                if (hid == kAvih) {
                    if (!read_bytes(src, hdata, hsize, bytes)) return AVIERR_FILEREAD;
                    parse_avih(bytes, out.header);
                    have_header = true;
                } else if (hid == kList && htype == kStrl) {
                    AviStream s;
                    if (parse_strl(src, Span{hdata + 4, hdata + hsize}, s)) {
                        out.streams.push_back(std::move(s));
                    }
                } else if (hid != kJunk) {
                    if (!read_bytes(src, hdata, hsize, bytes)) return AVIERR_FILEREAD;
                    out.extra.push_back(DataChunk{hid, std::move(bytes)});
                }
                h = hdata + hsize + (hsize & 1);
            }
        } else if (id == kList && type == kMovi) {
            if (!have_movi) {
                movi = Span{data + 4, data + size};
                have_movi = true;
            }
        } else if (id == kIdx1) {
            if (!have_idx1) {
                idx_data = data;
                idx_size = size;
                have_idx1 = true;
            }
        } else if (id != kJunk) {
            std::vector<u8> bytes;
            if (!read_bytes(src, data, size, bytes)) return AVIERR_FILEREAD;
            out.extra.push_back(DataChunk{id, std::move(bytes)});
        }
        at = data + size + (size & 1);
    }
    if (!have_header) return AVIERR_BADFORMAT;
    if (have_idx1 && have_movi &&
        index_from_idx1(src, out, idx_data, idx_size, movi.begin - 4)) {
        return AVIERR_OK;
    }
    for (auto& s : out.streams) {
        s.chunks.clear();
        s.palette_changes.clear();
        s.total_samples = 0;
    }
    if (have_movi) {
        u32 ordinal = 0;
        index_from_scan(src, out, movi, ordinal);
    }
    return AVIERR_OK;
}

u32 parse_wave(ByteSource& src, WaveFileModel& out) {
    if (riff_form_type(src) != kWave) return AVIERR_BADFORMAT;
    u32 riff_size = 0;
    if (!read32(src, 4, riff_size)) return AVIERR_BADFORMAT;
    const u64 riff_end = std::min<u64>(src.size(), 8ull + riff_size);
    bool have_fmt = false, have_data = false;
    for (u64 at = 12; at + 8 <= riff_end;) {
        u32 id = 0, size = 0;
        if (!read32(src, at, id) || !read32(src, at + 4, size)) break;
        const u64 data = at + 8;
        if (data + size > riff_end) size = static_cast<u32>(riff_end - data);
        if (id == kFmt && !have_fmt) {
            if (!read_bytes(src, data, size, out.format)) return AVIERR_BADFORMAT;
            have_fmt = true;
        } else if (id == kData && !have_data) {
            out.data_offset = data;
            out.data_size = size;
            have_data = true;
        }
        at = data + size + (size & 1);
    }
    if (!have_fmt || !have_data || out.format.size() < 14) return AVIERR_BADFORMAT;
    out.samples_per_sec = le32(out.format.data() + 4);
    out.avg_bytes_per_sec = le32(out.format.data() + 8);
    out.block_align = le16(out.format.data() + 12);
    if (out.block_align == 0) return AVIERR_BADFORMAT;
    return AVIERR_OK;
}

} // namespace vitreo::winapi::avifil32
