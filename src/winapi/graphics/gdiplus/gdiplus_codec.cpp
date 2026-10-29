// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_codec.cpp
/// @brief GDI+ area: the codec list, decoding (file/stream/resource),
/// encoding and multi-frame saving, encoder parameter lists, property items,
/// frames, thumbnails, raw format, item data, GdipTestControl.
///
/// Contract: Learn (Image / Bitmap / ImageCodecInfo / EncoderParameters /
/// PropertyItem pages, "Using Image Encoders and Decoders"), the SDK headers,
/// and the statuses/layouts measured by tools/oracle_probe/gdiplus_codec_probe.c.
/// Effects and metafiles live in gdiplus_codec_effects.cpp / _metafile.cpp.

#include "winapi/graphics/gdiplus/gdiplus_codec.hpp"
#include "winapi/graphics/gdiplus/gdiplus_codec_formats.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::gdip {

/// An open multi-frame save (EncoderValueMultiFrame ... EncoderValueFlush).
struct SaveSession {
    SourceFormat encoder{SourceFormat::Tiff};
    GuestAddr stream{0};          ///< the guest IStream (kept alive by the caller, per Learn)
    std::string path;             ///< or the file
    fmt::EncodeOptions options;
    std::vector<fmt::EncodeFrame> frames;
};

namespace {

// ---- The built-in codecs ----------------------------------------------------------------
//
// CLSIDs, names, descriptions, extensions and MIME types: Learn "Retrieving
// the Class Identifier for an Encoder" / "Listing Installed Encoders" and the
// ImageCodecInfo class page. Signatures: the file-format specifications
// (BMP "BM", JPEG SOI, GIF87a/89a, EMF " EMF" at offset 40 of the
// ENHMETAHEADER, placeable-WMF key 0x9AC6CDD7, TIFF "II"/"MM", the PNG
// signature, ICONDIR reserved/type). Flags: ImageCodecFlags (GdiPlusImaging.h):
// Builtin | SupportBitmap | Decoder [| Encoder].

struct CodecInfo {
    Guid clsid;
    Guid format;
    const char16_t* name;
    const char16_t* desc;
    const char16_t* ext;
    const char16_t* mime;
    u32 flags;
    u32 sig_count;
    u32 sig_size;
    std::vector<u8> sig;
    std::vector<u8> mask;
    SourceFormat source;
};

constexpr Guid codec_clsid(u8 n) { return make_guid(0x557cf400u + n, 0x1a04, 0x11d3, 0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e); }
constexpr u32 kBuiltinDecoder = 0x00010000 | 0x4 | 0x2;
constexpr u32 kBuiltinCodec = kBuiltinDecoder | 0x1;

const std::vector<CodecInfo>& decoders() {
    static const std::vector<CodecInfo> list = [] {
        std::vector<u8> emf_sig(44, 0), emf_mask(44, 0);
        std::memcpy(emf_sig.data() + 40, " EMF", 4);
        std::fill(emf_mask.begin() + 40, emf_mask.end(), 0xFF);
        return std::vector<CodecInfo>{
            {codec_clsid(0), kImageFormatBMP, u"Built-in BMP Codec", u"BMP", u"*.BMP;*.DIB;*.RLE", u"image/bmp", kBuiltinCodec, 1, 2, {'B', 'M'}, {0xFF, 0xFF}, SourceFormat::Bmp},
            {codec_clsid(1), kImageFormatJPEG, u"Built-in JPEG Codec", u"JPEG", u"*.JPG;*.JPEG;*.JPE;*.JFIF", u"image/jpeg", kBuiltinCodec, 1, 2, {0xFF, 0xD8}, {0xFF, 0xFF}, SourceFormat::Jpeg},
            {codec_clsid(2), kImageFormatGIF, u"Built-in GIF Codec", u"GIF", u"*.GIF", u"image/gif", kBuiltinCodec, 2, 6, {'G', 'I', 'F', '8', '9', 'a', 'G', 'I', 'F', '8', '7', 'a'}, std::vector<u8>(12, 0xFF), SourceFormat::Gif},
            {codec_clsid(3), kImageFormatEMF, u"Built-in EMF Codec", u"EMF", u"*.EMF", u"image/x-emf", kBuiltinDecoder, 1, 44, emf_sig, emf_mask, SourceFormat::Emf},
            {codec_clsid(4), kImageFormatWMF, u"Built-in WMF Codec", u"WMF", u"*.WMF", u"image/x-wmf", kBuiltinDecoder, 1, 4, {0xD7, 0xCD, 0xC6, 0x9A}, {0xFF, 0xFF, 0xFF, 0xFF}, SourceFormat::Wmf},
            {codec_clsid(5), kImageFormatTIFF, u"Built-in TIFF Codec", u"TIFF", u"*.TIF;*.TIFF", u"image/tiff", kBuiltinCodec, 2, 2, {'I', 'I', 'M', 'M'}, {0xFF, 0xFF, 0xFF, 0xFF}, SourceFormat::Tiff},
            {codec_clsid(6), kImageFormatPNG, u"Built-in PNG Codec", u"PNG", u"*.PNG", u"image/png", kBuiltinCodec, 1, 8, {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}, std::vector<u8>(8, 0xFF), SourceFormat::Png},
            {codec_clsid(7), kImageFormatIcon, u"Built-in ICO Codec", u"ICO", u"*.ICO", u"image/x-icon", kBuiltinDecoder, 1, 4, {0x00, 0x00, 0x01, 0x00}, {0xFF, 0xFF, 0xFF, 0xFF}, SourceFormat::Icon},
        };
    }();
    return list;
}

const std::vector<CodecInfo>& encoders() {
    static const std::vector<CodecInfo> list = [] {
        std::vector<CodecInfo> out;
        for (const CodecInfo& c : decoders()) if (c.flags & 1) out.push_back(c);
        return out;
    }();
    return list;
}

const CodecInfo* encoder_by_clsid(const Guid& g) {
    for (const CodecInfo& c : encoders()) if (c.clsid == g) return &c;
    return nullptr;
}

usize u16len(const char16_t* s) { usize n = 0; while (s && s[n]) ++n; return n; }

/// ImageCodecInfo array, then each codec's strings and signature bytes
/// packed in order (name, description, extension, MIME, patterns, masks):
/// the layout the probe's "off" lines show. The DllName of built-ins is NULL.
u32 codec_list_size(const std::vector<CodecInfo>& list, bool x64) {
    u32 size = static_cast<u32>(list.size()) * (x64 ? 104u : 76u);
    for (const CodecInfo& c : list)
        size += static_cast<u32>((u16len(c.name) + u16len(c.desc) + u16len(c.ext) + u16len(c.mime) + 4) * 2 + c.sig.size() * 2);
    return size;
}

void codec_list_handler(Marshaler& m, const std::vector<CodecInfo>& list) {
    // GdipGetImage{De,En}coders(UINT num, UINT size, ImageCodecInfo* out): the
    // count and size must be exactly the *Size values, else GenericError (probe).
    const u32 num = m.arg32(0), size = m.arg32(1);
    const GuestAddr out = m.arg_ptr(2);
    const bool x64 = m.cpu_is_64bit();
    if (out == 0 || num != list.size() || size != codec_list_size(list, x64)) {
        write_status(m, Status::GenericError);
        return;
    }
    const u32 rec = x64 ? 104 : 76, ptr = x64 ? 8 : 4;
    GuestAddr tail = out + static_cast<u64>(list.size()) * rec;
    for (usize i = 0; i < list.size(); ++i) {
        const CodecInfo& c = list[i];
        const GuestAddr r = out + i * rec;
        (void)write_guid(m, r, c.clsid);
        (void)write_guid(m, r + 16, c.format);
        auto str = [&](const char16_t* s) {
            const GuestAddr at = tail;
            (void)write_wstr(m, at, std::u16string_view(s, u16len(s)));
            tail += (u16len(s) + 1) * 2;
            return at;
        };
        const GuestAddr name = str(c.name), desc = str(c.desc), ext = str(c.ext), mime = str(c.mime);
        const GuestAddr sig = tail;
        (void)m.memory().write_bytes(sig, c.sig.data(), c.sig.size());
        tail += c.sig.size();
        const GuestAddr mask = tail;
        (void)m.memory().write_bytes(mask, c.mask.data(), c.mask.size());
        tail += c.mask.size();
        (void)m.write_ptr(r + 32, name);
        (void)m.write_ptr(r + 32 + ptr, 0);
        (void)m.write_ptr(r + 32 + 2 * ptr, desc);
        (void)m.write_ptr(r + 32 + 3 * ptr, ext);
        (void)m.write_ptr(r + 32 + 4 * ptr, mime);
        const GuestAddr d = r + 32 + 5 * ptr;
        (void)m.write_u32(d, c.flags);
        (void)m.write_u32(d + 4, 1);   // Version
        (void)m.write_u32(d + 8, c.sig_count);
        (void)m.write_u32(d + 12, c.sig_size);
        (void)m.write_ptr(d + 16, sig);
        (void)m.write_ptr(d + 16 + ptr, mask);
    }
    write_status(m, Status::Ok);
}

void codec_size_handler(Marshaler& m, const std::vector<CodecInfo>& list) {
    const GuestAddr num = m.arg_ptr(0), size = m.arg_ptr(1);
    if (num == 0 || size == 0) { write_status(m, Status::InvalidParameter); return; }
    (void)m.write_u32(num, static_cast<u32>(list.size()));
    (void)m.write_u32(size, codec_list_size(list, m.cpu_is_64bit()));
    write_status(m, Status::Ok);
}

// ---- Encoder parameters --------------------------------------------------------------------

// GdiPlusImaging.h encoder GUIDs.
constexpr Guid kEncCompression = make_guid(0xe09d739d, 0xccd4, 0x44ee, 0x8e, 0xba, 0x3f, 0xbf, 0x8b, 0xe4, 0xfc, 0x58);
constexpr Guid kEncColorDepth = make_guid(0x66087055, 0xad66, 0x4c7c, 0x9a, 0x18, 0x38, 0xa2, 0x31, 0x0b, 0x83, 0x37);
constexpr Guid kEncQuality = make_guid(0x1d5be4b5, 0xfa4a, 0x452d, 0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb);
constexpr Guid kEncTransformation = make_guid(0x8d0eb2d1, 0xa58e, 0x4ea8, 0xaa, 0x14, 0x10, 0x80, 0x74, 0xb7, 0xb6, 0xf9);
constexpr Guid kEncLuminanceTable = make_guid(0xedb33bce, 0x0266, 0x4a77, 0xb9, 0x04, 0x27, 0x21, 0x60, 0x99, 0xe7, 0x17);
constexpr Guid kEncChrominanceTable = make_guid(0xf2e455dc, 0x09b3, 0x4316, 0x82, 0x60, 0x67, 0x6a, 0xda, 0x32, 0x48, 0x1c);
constexpr Guid kEncSaveFlag = make_guid(0x292266fc, 0xac40, 0x47bf, 0x8c, 0xfc, 0xa8, 0x5b, 0x89, 0xa6, 0x55, 0xde);
constexpr Guid kEncImageItems = make_guid(0x63875e13, 0x1f1d, 0x45ab, 0x91, 0x95, 0xa2, 0x9b, 0x60, 0x66, 0xa6, 0x50);
constexpr Guid kEncSaveAsCMYK = make_guid(0xa219bbc9, 0x0a9d, 0x4005, 0xa3, 0xee, 0x3a, 0x42, 0x1b, 0x8b, 0xb0, 0x6c);

// EncoderParameterValueType / EncoderValue (GdiPlusEnums.h).
constexpr u32 kTypeShort = 3, kTypeLong = 4, kTypeLongRange = 6, kTypePointer = 9;
constexpr u32 kValueMultiFrame = 18, kValueLastFrame = 19, kValueFlush = 20, kValueTime = 21, kValuePage = 23;

struct ParamSpec { Guid guid; u32 type; std::vector<i32> values; };

/// The parameter list each encoder advertises (probe plist[*], both bitnesses).
std::vector<ParamSpec> param_specs(SourceFormat enc) {
    switch (enc) {
        case SourceFormat::Jpeg:
            return {{kEncTransformation, kTypeLong, {13, 14, 15, 16, 17}}, {kEncQuality, kTypeLongRange, {0, 100}},
                    {kEncLuminanceTable, kTypeShort, {}}, {kEncChrominanceTable, kTypeShort, {}},
                    {kEncImageItems, kTypePointer, {}}};
        case SourceFormat::Gif: return {{kEncImageItems, kTypePointer, {}}, {kEncSaveFlag, kTypeLong, {kValueMultiFrame}}};
        case SourceFormat::Tiff:
            return {{kEncCompression, kTypeLong, {2, 3, 5, 4, 6}}, {kEncColorDepth, kTypeLong, {1, 4, 8, 24, 32}},
                    {kEncSaveFlag, kTypeLong, {kValueMultiFrame}}, {kEncSaveAsCMYK, kTypeLong, {1}}};
        case SourceFormat::Png: return {{kEncImageItems, kTypePointer, {}}};
        default: return {};
    }
}

/// EncoderParameters size: Count padded to pointer alignment, the array,
/// the values packed, the total rounded up to pointer size (probe plist sizes).
u32 param_list_size(const std::vector<ParamSpec>& specs, bool x64) {
    const u32 ptr = x64 ? 8 : 4;
    u32 size = ptr + static_cast<u32>(specs.size()) * (24 + ptr);
    for (const auto& s : specs) size += static_cast<u32>(s.values.size()) * 4;
    return (size + ptr - 1) / ptr * ptr;
}

u32 param_count(const ParamSpec& s) { return s.type == kTypeLongRange ? static_cast<u32>(s.values.size() / 2) : static_cast<u32>(s.values.size()); }

struct GuestParam { Guid guid; u32 count; u32 type; GuestAddr value; };

/// Read a guest EncoderParameters. nullopt: unreadable. Count 0 is
/// InvalidParameter for every encoder (probe param[*] count0).
std::optional<std::vector<GuestParam>> read_params(Marshaler& m, GuestAddr at) {
    std::vector<GuestParam> out;
    if (at == 0) return out;
    const bool x64 = m.cpu_is_64bit();
    const u32 ptr = x64 ? 8 : 4;
    const auto count = m.read_u32(at);
    if (count.is_err() || count.value() > 4096) return std::nullopt;
    for (u32 i = 0; i < count.value(); ++i) {
        const GuestAddr p = at + ptr + static_cast<u64>(i) * (24 + ptr);
        GuestParam g{};
        if (!read_guid(m, p, g.guid)) return std::nullopt;
        const auto n = m.read_u32(p + 16), t = m.read_u32(p + 20);
        const auto v = m.read_ptr(p + 24);
        if (n.is_err() || t.is_err() || v.is_err()) return std::nullopt;
        g.count = n.value(); g.type = t.value(); g.value = v.value();
        out.push_back(g);
    }
    return out;
}

std::optional<i32> long_value(Marshaler& m, const GuestParam& p) {
    if (p.type != kTypeLong || p.count != 1 || p.value == 0) return std::nullopt;
    const auto v = m.read_u32(p.value);
    if (v.is_err()) return std::nullopt;
    return static_cast<i32>(v.value());
}

// ---- Image access ---------------------------------------------------------------------------

GpImage* image_arg(GdiplusManager& mgr, Marshaler& m, usize i) {
    GpImage* img = mgr.objects().get<GpImage>(handle_arg(m, i));
    if (img && img->type == ImageTypeUnknown) return nullptr;   // GdipGetHemfFromMetafile killed it
    return img;
}

SourceFormat format_of(const GpImage& img) {
    if (const CodecState* c = codec_of(img)) return c->format;
    return img.type == ImageTypeMetafile ? SourceFormat::Emf : SourceFormat::Memory;
}

/// Property items live in the codec state; a plain bitmap gets one on first edit.
std::vector<Property>* props_of(GpImage& img) {
    CodecState* c = codec_of(img);
    return c ? &c->properties : nullptr;
}

u32 prop_header(bool x64) { return x64 ? 24 : 16; }

bool write_prop(Marshaler& m, GuestAddr at, GuestAddr value_at, const Property& p) {
    return m.write_u32(at, p.id).is_ok() && m.write_u32(at + 4, static_cast<u32>(p.value.size())).is_ok() &&
           m.write_u16(at + 8, p.type).is_ok() && m.write_u16(at + 10, 0).is_ok() &&
           (!m.cpu_is_64bit() || m.write_u32(at + 12, 0).is_ok()) &&
           m.write_ptr(at + (m.cpu_is_64bit() ? 16 : 12), value_at).is_ok() &&
           (p.value.empty() || m.memory().write_bytes(value_at, p.value.data(), p.value.size()).is_ok());
}

// ---- Decoding into objects ----------------------------------------------------------------------------

/// Build a GpImage from encoded bytes. @p bitmap_only: the CreateBitmap*
/// family (a metafile source would have to be rasterized).
Status decode_object(GdiplusManager& mgr, std::vector<u8> bytes, bool bitmap_only, GpImage& out) {
    const auto format = fmt::sniff(bytes);
    if (!format) return Status::OutOfMemory;
    if (*format == SourceFormat::Emf || *format == SourceFormat::Wmf) {
        if (bitmap_only) {
            log_unimplemented("GdipCreateBitmapFromFile/Stream", "rasterizing a metafile source into a bitmap");
            return Status::NotImplemented;
        }
        return create_metafile_image(mgr, std::move(bytes), out);
    }
    fmt::Decoded d;
    const Status st = fmt::decode(*format, bytes, 0, d);
    if (st != Status::Ok) return Status::OutOfMemory;
    std::vector<Property> props = std::move(d.frame.properties);
    apply_frame(out, std::move(d.frame));
    for (const CodecInfo& c : decoders())
        if (c.source == *format) std::memcpy(out.raw_format, c.format.data(), 16);
    CodecState& cs = out.codec.ensure();
    cs.format = *format;
    cs.source = std::move(bytes);
    cs.dimension = d.dimension;
    cs.frame_count = d.frame_count;
    cs.active_frame = 0;
    cs.properties = std::move(props);
    return Status::Ok;
}

void finish_create(GdiplusManager& mgr, Marshaler& m, GuestAddr out, Status st, GpImage&& img) {
    if (st != Status::Ok) {
        (void)write_handle(m, out, 0);
        write_status(m, st);
        return;
    }
    (void)write_handle(m, out, mgr.objects().add(std::move(img)));
    write_status(m, Status::Ok);
}

/// GdipLoadImageFromFile[ICM] / GdipCreateBitmapFromFile[ICM].
void from_file(GdiplusManager& mgr, Marshaler& m, bool bitmap_only) {
    const GuestAddr name = m.arg_ptr(0), out = m.arg_ptr(1);
    if (name == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
    (void)write_handle(m, out, 0);
    auto bytes = read_guest_file(mgr, read_path(m, name));
    // Probe err.*: an unreadable or unknown file is OutOfMemory for
    // LoadImage and InvalidParameter for CreateBitmap.
    const Status fail = bitmap_only ? Status::InvalidParameter : Status::OutOfMemory;
    if (!bytes || bytes->empty()) { write_status(m, fail); return; }
    GpImage img;
    Status st = decode_object(mgr, std::move(*bytes), bitmap_only, img);
    if (st == Status::OutOfMemory) st = fail;
    finish_create(mgr, m, out, st, std::move(img));
}

/// GdipLoadImageFromStream[ICM] / GdipCreateBitmapFromStream[ICM].
void from_stream(GdiplusManager& mgr, Marshaler& m, bool bitmap_only) {
    const GuestAddr stream = m.arg_ptr(0), out = m.arg_ptr(1);
    if (stream == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
    (void)write_handle(m, out, 0);
    GuestStream s(mgr, m, stream);
    if (!s.usable()) {
        log_unimplemented("GdipLoadImageFromStream", "no guest-call path for the IStream");
        write_status(m, Status::NotImplemented);
        return;
    }
    // All four stream loaders start at absolute zero, even when the caller
    // left the cursor at/past EOF (Windows stream-position oracle, both ABIs).
    if (!s.seek(0, 0)) { write_status(m, Status::InvalidParameter); return; }
    auto bytes = s.read_to_end();
    if (!bytes || bytes->empty()) { (void)s.seek(0, 0); write_status(m, Status::InvalidParameter); return; }
    const auto format = fmt::sniff(*bytes);
    GpImage img;
    Status st = decode_object(mgr, std::move(*bytes), bitmap_only, img);
    // Failed decodes and TIFF return to absolute zero regardless of the
    // incoming cursor (gdiplus_stream_position_probe). Other existing codec
    // cursor fidelity gaps are recorded in the owning codec contract.
    if (st != Status::Ok || format == SourceFormat::Tiff) (void)s.seek(0, 0);
    if (st == Status::OutOfMemory) st = Status::InvalidParameter;
    finish_create(mgr, m, out, st, std::move(img));
}

// ---- PE resources (GdipCreateBitmapFromResource) ----------------------------------------------------------

std::optional<std::vector<u8>> find_bitmap_resource(Marshaler& m, GuestAddr base, GuestAddr name) {
    auto r32 = [&](GuestAddr a) { auto v = m.read_u32(a); return v.is_ok() ? v.value() : 0u; };
    auto r16 = [&](GuestAddr a) { auto v = m.read_u16(a); return v.is_ok() ? v.value() : u16{0}; };
    if (base == 0 || r16(base) != 0x5A4D) return std::nullopt;
    const GuestAddr nt = base + r32(base + 0x3C);
    if (r32(nt) != 0x4550) return std::nullopt;
    const u16 magic = r16(nt + 24);
    const GuestAddr dirs = nt + 24 + (magic == 0x20B ? 112 : 96);
    const u32 rva = r32(dirs + 2 * 8);
    if (rva == 0) return std::nullopt;
    const GuestAddr root = base + rva;
    std::u16string want;
    const bool by_id = name <= 0xFFFF;
    if (!by_id) want = read_wstr(m, name);
    // Find @p id (or name) in the directory at @p dir; returns the entry's offset field.
    auto lookup = [&](GuestAddr dir, bool id_match, u32 id, const std::u16string& str) -> std::optional<u32> {
        const u16 named = r16(dir + 12), ids = r16(dir + 14);
        for (u32 i = 0; i < u32{named} + ids; ++i) {
            const GuestAddr e = dir + 16 + i * 8;
            const u32 n = r32(e), off = r32(e + 4);
            if (id_match && !(n & 0x80000000u) && n == id) return off;
            if (!id_match && (n & 0x80000000u)) {
                const GuestAddr s = root + (n & 0x7FFFFFFF);
                const u16 len = r16(s);
                std::u16string k = read_wstr(m, s + 2, len);
                auto up = [](std::u16string v) { for (auto& c : v) if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32); return v; };
                if (up(k) == up(str)) return off;
            }
        }
        return std::nullopt;
    };
    const auto type = lookup(root, true, 2, {});   // RT_BITMAP
    if (!type || !(*type & 0x80000000u)) return std::nullopt;
    const auto entry = lookup(root + (*type & 0x7FFFFFFF), by_id, static_cast<u32>(name), want);
    if (!entry) return std::nullopt;
    u32 off = *entry;
    if (off & 0x80000000u) {   // language level: the first language
        const GuestAddr lang = root + (off & 0x7FFFFFFF);
        if (r16(lang + 12) + r16(lang + 14) == 0) return std::nullopt;
        off = r32(lang + 16 + 4);
    }
    const GuestAddr data = root + off;
    const u32 data_rva = r32(data), size = r32(data + 4);
    if (size < 40 || size > (64u << 20)) return std::nullopt;
    std::vector<u8> dib(size);
    if (m.memory().read_bytes(base + data_rva, dib.data(), size).is_err()) return std::nullopt;
    // A resource bitmap is a packed DIB: give it the BITMAPFILEHEADER a .bmp has.
    const u32 hsize = dib[0] | (dib[1] << 8) | (dib[2] << 16) | (u32{dib[3]} << 24);
    u32 bpp = hsize == 12 ? (dib[10] | (dib[11] << 8)) : (dib[14] | (dib[15] << 8));
    u32 colors = hsize >= 40 ? (dib[32] | (dib[33] << 8) | (dib[34] << 16) | (u32{dib[35]} << 24)) : 0;
    const u32 comp = hsize >= 40 ? dib[16] : 0;
    if (bpp <= 8 && colors == 0) colors = 1u << bpp;
    u32 off_bits = 14 + hsize + colors * (hsize == 12 ? 3 : 4) + ((hsize == 40 && comp == 3) ? 12 : 0);
    std::vector<u8> file = {'B', 'M'};
    auto p32 = [&](u32 v) { for (int i = 0; i < 4; ++i) file.push_back(static_cast<u8>(v >> (8 * i))); };
    p32(14 + size); p32(0); p32(off_bits);
    file.insert(file.end(), dib.begin(), dib.end());
    return file;
}

// ---- Encoding ------------------------------------------------------------------------------------------------

Status frame_for_encode(Marshaler& m, const GpImage& img, fmt::EncodeFrame& f) {
    if (img.type != ImageTypeBitmap) {
        log_unimplemented("GdipSaveImageTo*", "encoding a metafile (needs rasterization)");
        return Status::NotImplemented;
    }
    f.width = img.width;
    f.height = img.height;
    f.source_format = img.pixel_format & 0x00FFFFFF;
    f.palette = img.palette;
    f.dpi_x = img.dpi_x;
    f.dpi_y = img.dpi_y;
    if (const CodecState* c = codec_of(img)) f.properties = c->properties;
    if (f.source_format == PixelFormat32bppCMYK) {
        log_unimplemented("GdipSaveImageTo*", "encoding PixelFormat32bppCMYK pixels");
        return Status::NotImplemented;
    }
    if (!read_argb(m, img, f.argb)) return Status::GenericError;
    if (f.source_format == PixelFormat64bppARGB && img.user_scan0 == 0) {
        f.raw.resize(static_cast<usize>(img.width) * img.height * 8);
        for (i32 y = 0; y < img.height; ++y)
            std::memcpy(f.raw.data() + static_cast<usize>(y) * img.width * 8, img.bits.data() + static_cast<usize>(y) * img.stride,
                        static_cast<usize>(img.width) * 8);
    }
    return Status::Ok;
}

Status encode_frames(SourceFormat enc, const std::vector<fmt::EncodeFrame>& frames, const fmt::EncodeOptions& o,
                     std::vector<u8>& out) {
    switch (enc) {
        case SourceFormat::Bmp: return fmt::encode_bmp(frames[0], out);
        case SourceFormat::Png: return fmt::encode_png(frames[0], out);
        case SourceFormat::Gif: return fmt::encode_gif(frames, out);
        case SourceFormat::Tiff: return fmt::encode_tiff(frames, o, out);
        case SourceFormat::Jpeg: return fmt::encode_jpeg(frames[0], o, out);
        default: return Status::InvalidParameter;
    }
}

struct SaveTarget { GuestAddr stream{0}; std::string path; };

Status deliver(GdiplusManager& mgr, Marshaler& m, const SaveTarget& t, const std::vector<u8>& bytes) {
    if (t.stream != 0) {
        GuestStream s(mgr, m, t.stream);
        if (!s.usable()) {
            log_unimplemented("GdipSaveImageToStream", "no guest-call path for the IStream");
            return Status::NotImplemented;
        }
        return s.write(bytes) ? Status::Ok : Status::Win32Error;
    }
    return write_guest_file(mgr, t.path, bytes) ? Status::Ok : Status::Win32Error;
}

/// Parse the parameters for encoder @p enc. Fills options, multi-frame flag.
Status parse_options(Marshaler& m, const std::vector<GuestParam>& params, SourceFormat enc, const GpImage& img,
                     fmt::EncodeOptions& o, bool& multi) {
    multi = false;
    for (const GuestParam& p : params) {
        if (p.guid == kEncTransformation) {
            // Probe param[*] p3: from a non-JPEG source every encoder refuses it.
            if (format_of(img) != SourceFormat::Jpeg || enc != SourceFormat::Jpeg) return Status::InvalidParameter;
            log_unimplemented("GdipSaveImageToStream", "JPEG lossless transformation (EncoderTransformation)");
            return Status::NotImplemented;
        }
        if (enc == SourceFormat::Jpeg && p.guid == kEncQuality) {
            const auto q = long_value(m, p);
            if (!q) return Status::InvalidParameter;   // probe quality-short / quality-2vals
            o.jpeg_quality = std::clamp(*q, 0, 100);
        } else if (enc == SourceFormat::Jpeg && (p.guid == kEncLuminanceTable || p.guid == kEncChrominanceTable)) {
            if (p.type != kTypeShort || p.count != 64 || p.value == 0) return Status::InvalidParameter;
            log_unimplemented("GdipSaveImageToStream", "custom JPEG quantization tables");
            return Status::NotImplemented;
        } else if (enc == SourceFormat::Tiff && p.guid == kEncCompression) {
            const auto v = long_value(m, p);
            if (!v || *v < 2 || *v > 6) return Status::InvalidParameter;
            o.tiff_compression = *v;
        } else if (enc == SourceFormat::Tiff && p.guid == kEncColorDepth) {
            const auto v = long_value(m, p);
            if (!v || (*v != 1 && *v != 4 && *v != 8 && *v != 24 && *v != 32)) return Status::InvalidParameter;
            o.color_depth = *v;
        } else if (enc == SourceFormat::Tiff && p.guid == kEncSaveAsCMYK) {
            const auto v = long_value(m, p);
            if (!v) return Status::InvalidParameter;
            o.save_as_cmyk = *v != 0;
        } else if (p.guid == kEncSaveFlag) {
            const auto v = long_value(m, p);
            if (v && *v == static_cast<i32>(kValueMultiFrame) && (enc == SourceFormat::Tiff || enc == SourceFormat::Gif)) multi = true;
        } else if (p.guid == kEncImageItems && p.count != 0) {
            log_unimplemented("GdipSaveImageToStream", "EncoderImageItems");
            return Status::NotImplemented;
        }
        // Other parameters are not this encoder's: ignored (probe param[*]).
    }
    return Status::Ok;
}

void save_handler(GdiplusManager& mgr, Marshaler& m, bool to_stream) {
    GpImage* img = image_arg(mgr, m, 0);
    const GuestAddr target = m.arg_ptr(1), clsid_at = m.arg_ptr(2), params_at = m.arg_ptr(3);
    if (!img || target == 0 || clsid_at == 0) { write_status(m, Status::InvalidParameter); return; }
    Guid clsid{};
    if (!read_guid(m, clsid_at, clsid)) { write_status(m, Status::InvalidParameter); return; }
    const CodecInfo* enc = encoder_by_clsid(clsid);
    if (!enc) { write_status(m, Status::FileNotFound); return; }   // probe save.bogus
    const auto params = read_params(m, params_at);
    if (!params || (params_at != 0 && params->empty())) { write_status(m, Status::InvalidParameter); return; }
    fmt::EncodeOptions o;
    bool multi = false;
    if (const Status st = parse_options(m, *params, enc->source, *img, o, multi); st != Status::Ok) { write_status(m, st); return; }
    fmt::EncodeFrame frame;
    if (const Status st = frame_for_encode(m, *img, frame); st != Status::Ok) { write_status(m, st); return; }
    SaveTarget t;
    if (to_stream) t.stream = target; else t.path = read_path(m, target);
    if (!to_stream && t.path.empty()) { write_status(m, Status::InvalidParameter); return; }
    if (multi) {
        // Frames accumulate until EncoderValueFlush (GdipSaveAdd).
        auto session = std::make_shared<SaveSession>();
        session->encoder = enc->source;
        session->stream = t.stream;
        session->path = t.path;
        session->options = o;
        session->frames.push_back(std::move(frame));
        img->codec.ensure().session = std::move(session);
        write_status(m, Status::Ok);
        return;
    }
    std::vector<u8> bytes;
    Status st = encode_frames(enc->source, {frame}, o, bytes);
    if (st == Status::Ok) st = deliver(mgr, m, t, bytes);
    write_status(m, st);
}

/// GdipSaveAdd(image, params) / GdipSaveAddImage(image, newImage, params).
void save_add_handler(GdiplusManager& mgr, Marshaler& m, bool with_image) {
    GpImage* img = image_arg(mgr, m, 0);
    GpImage* add = with_image ? image_arg(mgr, m, 1) : img;
    const GuestAddr params_at = m.arg_ptr(with_image ? 2 : 1);
    if (!img || !add || params_at == 0) { write_status(m, Status::InvalidParameter); return; }
    const auto params = read_params(m, params_at);
    if (!params || params->empty()) { write_status(m, Status::InvalidParameter); return; }
    CodecState* cs = codec_of(*img);
    if (!cs || !cs->session) { write_status(m, Status::Win32Error); return; }   // probe saveadd.nosession
    SaveSession& s = *cs->session;
    std::optional<i32> flag;
    for (const GuestParam& p : *params) if (p.guid == kEncSaveFlag) flag = long_value(m, p);
    if (!flag) { write_status(m, Status::InvalidParameter); return; }
    if (*flag == static_cast<i32>(kValueFlush)) {
        if (with_image) { write_status(m, Status::InvalidParameter); return; }
        auto session = std::move(cs->session);
        std::vector<u8> bytes;
        Status st = encode_frames(session->encoder, session->frames, session->options, bytes);
        if (st == Status::Ok) st = deliver(mgr, m, SaveTarget{session->stream, session->path}, bytes);
        write_status(m, st);
        return;
    }
    if (*flag == static_cast<i32>(kValueLastFrame)) { write_status(m, Status::Ok); return; }
    const bool page = *flag == static_cast<i32>(kValuePage), time = *flag == static_cast<i32>(kValueTime);
    // Probe multi[2]: a GIF session refuses FrameDimensionPage (GenericError); r2.gifmulti: Time works.
    if ((s.encoder == SourceFormat::Gif && !time) || (s.encoder == SourceFormat::Tiff && !page)) {
        write_status(m, Status::GenericError);
        return;
    }
    fmt::EncodeFrame frame;
    if (const Status st = frame_for_encode(m, *add, frame); st != Status::Ok) { write_status(m, st); return; }
    s.frames.push_back(std::move(frame));
    write_status(m, Status::Ok);
}

void encoder_params_size(GdiplusManager& mgr, Marshaler& m) {
    GpImage* img = image_arg(mgr, m, 0);
    const GuestAddr clsid_at = m.arg_ptr(1), size_at = m.arg_ptr(2);
    Guid clsid{};
    if (!img || clsid_at == 0 || !read_guid(m, clsid_at, clsid)) { write_status(m, Status::InvalidParameter); return; }
    const CodecInfo* enc = encoder_by_clsid(clsid);
    if (!enc) { write_status(m, Status::FileNotFound); return; }      // probe plist bogus
    const auto specs = param_specs(enc->source);
    if (specs.empty()) { write_status(m, Status::NotImplemented); return; }   // probe plist[0] (BMP)
    if (size_at == 0) { write_status(m, Status::InvalidParameter); return; }
    (void)m.write_u32(size_at, param_list_size(specs, m.cpu_is_64bit()));
    write_status(m, Status::Ok);
}

void encoder_params_list(GdiplusManager& mgr, Marshaler& m) {
    GpImage* img = image_arg(mgr, m, 0);
    const GuestAddr clsid_at = m.arg_ptr(1), out = m.arg_ptr(3);
    const u32 size = m.arg32(2);
    Guid clsid{};
    if (!img || clsid_at == 0 || out == 0 || !read_guid(m, clsid_at, clsid)) { write_status(m, Status::InvalidParameter); return; }
    const CodecInfo* enc = encoder_by_clsid(clsid);
    if (!enc) { write_status(m, Status::FileNotFound); return; }
    const auto specs = param_specs(enc->source);
    if (specs.empty()) { write_status(m, Status::NotImplemented); return; }
    const bool x64 = m.cpu_is_64bit();
    if (size != param_list_size(specs, x64)) { write_status(m, Status::InvalidParameter); return; }   // probe small/big
    const u32 ptr = x64 ? 8 : 4;
    (void)m.write_u32(out, static_cast<u32>(specs.size()));
    if (x64) (void)m.write_u32(out + 4, 0);
    GuestAddr values = out + ptr + static_cast<u64>(specs.size()) * (24 + ptr);
    for (usize i = 0; i < specs.size(); ++i) {
        const GuestAddr p = out + ptr + i * (24 + ptr);
        (void)write_guid(m, p, specs[i].guid);
        (void)m.write_u32(p + 16, param_count(specs[i]));
        (void)m.write_u32(p + 20, specs[i].type);
        (void)m.write_ptr(p + 24, specs[i].values.empty() ? 0 : values);
        for (i32 v : specs[i].values) { (void)m.write_u32(values, static_cast<u32>(v)); values += 4; }
    }
    write_status(m, Status::Ok);
}

// ---- Frames ---------------------------------------------------------------------------------------------------

Guid dimension_of(const GpImage& img) {
    const CodecState* c = codec_of(img);
    return c && c->format != SourceFormat::Memory ? c->dimension : kFrameDimensionPage;
}
u32 frames_of(const GpImage& img) {
    const CodecState* c = codec_of(img);
    return c && c->format != SourceFormat::Memory ? c->frame_count : 1;
}

void frame_count_handler(GdiplusManager& mgr, Marshaler& m) {
    GpImage* img = image_arg(mgr, m, 0);
    const GuestAddr dim_at = m.arg_ptr(1), out = m.arg_ptr(2);
    Guid dim{};
    if (!img || out == 0 || !read_guid(m, dim_at, dim)) { write_status(m, Status::InvalidParameter); return; }
    const SourceFormat f = format_of(*img);
    if (f == SourceFormat::Icon) { (void)m.write_u32(out, 1); write_status(m, Status::Ok); return; }  // probe icon.ico
    if (dim != dimension_of(*img)) {
        if (f == SourceFormat::Gif) (void)m.write_u32(out, 0);   // probe single/anim.gif framecount-res n=0
        write_status(m, Status::Win32Error);
        return;
    }
    (void)m.write_u32(out, frames_of(*img));
    write_status(m, Status::Ok);
}

void select_frame_handler(GdiplusManager& mgr, Marshaler& m) {
    GpImage* img = image_arg(mgr, m, 0);
    const GuestAddr dim_at = m.arg_ptr(1);
    const u32 index = m.arg32(2);
    Guid dim{};
    if (!img || !read_guid(m, dim_at, dim)) { write_status(m, Status::InvalidParameter); return; }
    if (img->locked) { write_status(m, Status::WrongState); return; }
    if (dim != dimension_of(*img) || index >= frames_of(*img)) { write_status(m, Status::Win32Error); return; }
    CodecState* cs = codec_of(*img);
    if (!cs || cs->format == SourceFormat::Memory || cs->format == SourceFormat::Emf || cs->format == SourceFormat::Wmf ||
        cs->active_frame == index) {
        write_status(m, Status::Ok);
        return;
    }
    fmt::Decoded d;
    const Status st = fmt::decode(cs->format, cs->source, index, d);
    if (st != Status::Ok) { write_status(m, Status::Win32Error); return; }
    cs->properties = std::move(d.frame.properties);
    cs->active_frame = index;
    apply_frame(*img, std::move(d.frame));
    write_status(m, Status::Ok);
}

// ---- Thumbnails ------------------------------------------------------------------------------------------------

void thumbnail_handler(GdiplusManager& mgr, Marshaler& m) {
    GpImage* img = image_arg(mgr, m, 0);
    u32 w = m.arg32(1), h = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
    if (img->type != ImageTypeBitmap) {
        log_unimplemented("GdipGetImageThumbnail", "thumbnail of a metafile (needs rasterization)");
        write_status(m, Status::NotImplemented);
        return;
    }
    GpImage th;
    if (w == 0 && h == 0) {
        // Probe exif2.jpg: with no size requested, the embedded EXIF thumbnail is returned as decoded.
        if (const CodecState* c = codec_of(*img)) {
            for (const Property& p : c->properties) {
                DecodedFrame f;
                if (p.id == 0x501B && fmt::decode_thumbnail(p.value, f)) {
                    apply_frame(th, std::move(f));
                    std::memcpy(th.raw_format, kImageFormatMemoryBMP.data(), 16);
                    (void)write_handle(m, out, mgr.objects().add(std::move(th)));
                    write_status(m, Status::Ok);
                    return;
                }
            }
        }
        w = h = 120;   // probe thumb00: 120x120
    }
    if (w == 0 || h == 0) { write_status(m, Status::InvalidParameter); return; }
    std::vector<u32> argb, scaled;
    if (!read_argb(m, *img, argb) || !fmt::imageio_scale(argb, img->width, img->height, static_cast<i32>(w), static_cast<i32>(h), scaled)) {
        write_status(m, Status::OutOfMemory);
        return;
    }
    // A 32bppPARGB bitmap (probe pf=0xe200b); imageio_scale yields premultiplied B,G,R,A.
    DecodedFrame f;
    f.width = static_cast<i32>(w);
    f.height = static_cast<i32>(h);
    f.pixel_format = PixelFormat32bppPARGB;
    f.stride = stride_for(f.width, f.pixel_format);
    f.bits.resize(static_cast<usize>(f.stride) * h);
    std::memcpy(f.bits.data(), scaled.data(), f.bits.size());
    f.dpi_x = img->dpi_x;
    f.dpi_y = img->dpi_y;
    f.flags = 0x2;
    apply_frame(th, std::move(f));
    std::memcpy(th.raw_format, kImageFormatMemoryBMP.data(), 16);
    (void)write_handle(m, out, mgr.objects().add(std::move(th)));
    write_status(m, Status::Ok);
}

}  // namespace

void register_codec(GdiplusManager& mgr, HandlerMap& map) {
    register_codec_effects(mgr, map);
    register_codec_metafile(mgr, map);

    map["GdipGetImageDecodersSize"] = [](Marshaler& m) { codec_size_handler(m, decoders()); };
    map["GdipGetImageEncodersSize"] = [](Marshaler& m) { codec_size_handler(m, encoders()); };
    map["GdipGetImageDecoders"] = [](Marshaler& m) { codec_list_handler(m, decoders()); };
    map["GdipGetImageEncoders"] = [](Marshaler& m) { codec_list_handler(m, encoders()); };

    // ICM: GDI+ would apply an embedded ICC profile; the pixels here are not
    // color-managed (recorded in .docs/contracts/gdiplus/codec.md).
    map["GdipLoadImageFromFile"] = [&mgr](Marshaler& m) { from_file(mgr, m, false); };
    map["GdipLoadImageFromFileICM"] = [&mgr](Marshaler& m) { from_file(mgr, m, false); };
    map["GdipCreateBitmapFromFile"] = [&mgr](Marshaler& m) { from_file(mgr, m, true); };
    map["GdipCreateBitmapFromFileICM"] = [&mgr](Marshaler& m) { from_file(mgr, m, true); };
    map["GdipLoadImageFromStream"] = [&mgr](Marshaler& m) { from_stream(mgr, m, false); };
    map["GdipLoadImageFromStreamICM"] = [&mgr](Marshaler& m) { from_stream(mgr, m, false); };
    map["GdipCreateBitmapFromStream"] = [&mgr](Marshaler& m) {
        const GuestAddr stream = m.arg_ptr(0), out = m.arg_ptr(1);
        from_stream(mgr, m, true);
        VITREO_TRACE("[gdiplus-diag] GdipCreateBitmapFromStream stream=0x%llx status=%u out=0x%llx bitmap=0x%llx\n",
            static_cast<unsigned long long>(stream),
            m.cpu() ? static_cast<u32>(m.cpu()->gpr.rax) : 0xffffffffu,
            static_cast<unsigned long long>(out),
            static_cast<unsigned long long>(m.read_ptr(out).value_or(0)));
    };
    map["GdipCreateBitmapFromStreamICM"] = [&mgr](Marshaler& m) { from_stream(mgr, m, true); };

    map["GdipCreateBitmapFromResource"] = [&mgr](Marshaler& m) {
        const GuestAddr inst = m.arg_ptr(0), name = m.arg_ptr(1), out = m.arg_ptr(2);
        if (inst == 0 || name == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)write_handle(m, out, 0);
        auto file = find_bitmap_resource(m, inst, name);
        if (!file) { write_status(m, Status::InvalidParameter); return; }
        GpImage img;
        Status st = decode_object(mgr, std::move(*file), true, img);
        if (st == Status::Ok) std::memcpy(img.raw_format, kImageFormatMemoryBMP.data(), 16);
        if (st == Status::OutOfMemory) st = Status::InvalidParameter;
        finish_create(mgr, m, out, st, std::move(img));
    };
    map["GdipCreateBitmapFromDirectDrawSurface"] = [](Marshaler& m) {
        const GuestAddr surface = m.arg_ptr(0), out = m.arg_ptr(1);
        if (surface == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        log_unimplemented("GdipCreateBitmapFromDirectDrawSurface", "IDirectDrawSurface7 Lock/GetSurfaceDesc import");
        write_status(m, Status::NotImplemented);
    };

    map["GdipSaveImageToFile"] = [&mgr](Marshaler& m) { save_handler(mgr, m, false); };
    map["GdipSaveImageToStream"] = [&mgr](Marshaler& m) { save_handler(mgr, m, true); };
    map["GdipSaveAdd"] = [&mgr](Marshaler& m) { save_add_handler(mgr, m, false); };
    map["GdipSaveAddImage"] = [&mgr](Marshaler& m) { save_add_handler(mgr, m, true); };
    map["GdipGetEncoderParameterListSize"] = [&mgr](Marshaler& m) { encoder_params_size(mgr, m); };
    map["GdipGetEncoderParameterList"] = [&mgr](Marshaler& m) { encoder_params_list(mgr, m); };

    map["GdipGetImageRawFormat"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        Guid g{};
        std::memcpy(g.data(), img->raw_format, 16);
        if (g == Guid{}) g = img->type == ImageTypeMetafile ? kImageFormatEMF : kImageFormatMemoryBMP;
        (void)write_guid(m, out, g);
        write_status(m, Status::Ok);
    };
    map["GdipGetImageThumbnail"] = [&mgr](Marshaler& m) { thumbnail_handler(mgr, m); };

    // ---- Frames ----
    map["GdipImageGetFrameDimensionsCount"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        (void)m.write_u32(out, 1);
        write_status(m, Status::Ok);
    };
    map["GdipImageGetFrameDimensionsList"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr out = m.arg_ptr(1);
        const u32 count = m.arg32(2);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        if (count != 1) { write_status(m, Status::Win32Error); return; }   // probe dimlist+1
        (void)write_guid(m, out, dimension_of(*img));
        write_status(m, Status::Ok);
    };
    map["GdipImageGetFrameCount"] = [&mgr](Marshaler& m) { frame_count_handler(mgr, m); };
    map["GdipImageSelectActiveFrame"] = [&mgr](Marshaler& m) { select_frame_handler(mgr, m); };

    // ---- Property items ----
    map["GdipGetPropertyCount"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr out = m.arg_ptr(1);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const auto* p = props_of(*img);
        (void)m.write_u32(out, img->type == ImageTypeBitmap && p ? static_cast<u32>(p->size()) : 0);
        write_status(m, Status::Ok);
    };
    map["GdipGetPropertyIdList"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const u32 n = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        if (img->type != ImageTypeBitmap) { write_status(m, Status::NotImplemented); return; }   // probe mf idlist0
        const auto* p = props_of(*img);
        const usize have = p ? p->size() : 0;
        if (n != have) { write_status(m, Status::InvalidParameter); return; }
        for (usize i = 0; i < have; ++i) (void)m.write_u32(out + i * 4, (*p)[i].id);
        write_status(m, Status::Ok);
    };
    map["GdipGetPropertyItemSize"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const u32 id = m.arg32(1);
        const GuestAddr out = m.arg_ptr(2);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        if (const auto* p = props_of(*img))
            for (const Property& x : *p)
                if (x.id == id) {
                    (void)m.write_u32(out, prop_header(m.cpu_is_64bit()) + static_cast<u32>(x.value.size()));
                    write_status(m, Status::Ok);
                    return;
                }
        write_status(m, Status::PropertyNotFound);
    };
    map["GdipGetPropertyItem"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const u32 id = m.arg32(1), size = m.arg32(2);
        const GuestAddr out = m.arg_ptr(3);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        const u32 hdr = prop_header(m.cpu_is_64bit());
        if (const auto* p = props_of(*img))
            for (const Property& x : *p)
                if (x.id == id) {
                    if (size != hdr + x.value.size()) { write_status(m, Status::InvalidParameter); return; }
                    write_status(m, write_prop(m, out, out + hdr, x) ? Status::Ok : Status::InvalidParameter);
                    return;
                }
        write_status(m, Status::PropertyNotFound);
    };
    map["GdipGetPropertySize"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr total = m.arg_ptr(1), num = m.arg_ptr(2);
        if (!img || total == 0 || num == 0) { write_status(m, Status::InvalidParameter); return; }
        if (img->type != ImageTypeBitmap) { write_status(m, Status::NotImplemented); return; }   // probe mf propsize
        const auto* p = props_of(*img);
        u32 t = 0;
        if (p) for (const Property& x : *p) t += prop_header(m.cpu_is_64bit()) + static_cast<u32>(x.value.size());
        (void)m.write_u32(total, t);
        (void)m.write_u32(num, p ? static_cast<u32>(p->size()) : 0);
        write_status(m, Status::Ok);
    };
    map["GdipGetAllPropertyItems"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const u32 total = m.arg32(1), num = m.arg32(2);
        const GuestAddr out = m.arg_ptr(3);
        if (!img || out == 0) { write_status(m, Status::InvalidParameter); return; }
        if (img->type != ImageTypeBitmap) { write_status(m, Status::NotImplemented); return; }
        const auto* p = props_of(*img);
        const u32 hdr = prop_header(m.cpu_is_64bit());
        const usize n = p ? p->size() : 0;
        u32 t = 0;
        if (p) for (const Property& x : *p) t += hdr + static_cast<u32>(x.value.size());
        if (num != n || total != t || n == 0) { write_status(m, Status::InvalidParameter); return; }
        // Headers first, then the values packed in order (probe allitems offsets).
        GuestAddr value = out + n * hdr;
        for (usize i = 0; i < n; ++i) {
            if (!write_prop(m, out + i * hdr, value, (*p)[i])) { write_status(m, Status::InvalidParameter); return; }
            value += (*p)[i].value.size();
        }
        write_status(m, Status::Ok);
    };
    map["GdipRemovePropertyItem"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const u32 id = m.arg32(1);
        if (!img) { write_status(m, Status::InvalidParameter); return; }
        if (auto* p = props_of(*img)) {
            const auto it = std::find_if(p->begin(), p->end(), [id](const Property& x) { return x.id == id; });
            if (it != p->end()) { p->erase(it); write_status(m, Status::Ok); return; }
        }
        write_status(m, Status::GenericError);   // probe remove-again
    };
    map["GdipSetPropertyItem"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr item = m.arg_ptr(1);
        if (!img || item == 0) { write_status(m, Status::InvalidParameter); return; }
        if (img->type != ImageTypeBitmap) {
            log_unimplemented("GdipSetPropertyItem", "property items on a metafile");
            write_status(m, Status::NotImplemented);
            return;
        }
        const auto id = m.read_u32(item), len = m.read_u32(item + 4);
        const auto type = m.read_u16(item + 8);
        const auto value = m.read_ptr(item + (m.cpu_is_64bit() ? 16 : 12));
        if (id.is_err() || len.is_err() || type.is_err() || value.is_err() || len.value() > (64u << 20)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        // Probe propedit: any type and a zero length are stored as given.
        Property p{id.value(), type.value(), std::vector<u8>(len.value())};
        if (len.value() != 0 && (value.value() == 0 || m.memory().read_bytes(value.value(), p.value.data(), p.value.size()).is_err())) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        auto& props = img->codec.ensure().properties;
        const auto it = std::find_if(props.begin(), props.end(), [&](const Property& x) { return x.id == p.id; });
        if (it != props.end()) *it = std::move(p); else props.push_back(std::move(p));
        write_status(m, Status::Ok);
    };

    map["GdipGetImageItemData"] = [&mgr](Marshaler& m) {
        GpImage* img = image_arg(mgr, m, 0);
        const GuestAddr item = m.arg_ptr(1);
        if (!img || item == 0) { write_status(m, Status::InvalidParameter); return; }
        // Probe itemdata: BMP/TIFF/ICO decoders have no item support
        // (NotImplemented); the others hold no items, so no ImageItemData
        // can name one (InvalidParameter).
        const SourceFormat f = format_of(*img);
        write_status(m, (f == SourceFormat::Bmp || f == SourceFormat::Tiff || f == SourceFormat::Icon) ? Status::NotImplemented
                                                                                                        : Status::InvalidParameter);
    };

    map["GdipTestControl"] = [](Marshaler& m) {
        // GpTestControlEnum (GdiPlusEnums.h): ForceBilinear 0, NoICM 1, GetBuildNumber 2.
        const u32 control = m.arg32(0);
        const GuestAddr param = m.arg_ptr(1);
        if (control == 2) {
            if (param == 0) { write_status(m, Status::InvalidParameter); return; }
            // The GDI+ build this implementation answers for (probe "testcontrol build").
            (void)m.write_u32(param, 10011);
            write_status(m, Status::Ok);
            return;
        }
        write_status(m, control <= 1 ? Status::Ok : Status::InvalidParameter);
    };
}

}  // namespace vitreo::winapi::gdip
