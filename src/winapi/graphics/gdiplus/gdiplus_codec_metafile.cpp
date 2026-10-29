// SPDX-License-Identifier: LGPL-2.1-only
/// @file gdiplus_codec_metafile.cpp
/// @brief GDI+ metafiles: EMF / EMF+ objects, MetafileHeader, record
/// enumeration, HENHMETAFILE hand-off, the down-level rasterization limit.
///
/// Sources: GdiPlusMetaHeader.h (MetafileHeader, ENHMETAHEADER3,
/// WmfPlaceableFileHeader), GdiPlusEnums.h (MetafileType, EmfType,
/// MetafileFrameUnit, EmfPlusRecordType = EMR_* for GDI records), [MS-EMF]
/// (EMR_HEADER, EMR_COMMENT), [MS-EMFPLUS] (records inside "EMF+" GDI
/// comments: Type u16, Flags u16, Size u32, DataSize u32, data; the
/// EmfPlusHeader record's Version/EmfPlusFlags/LogicalDpiX/Y; EmfPlusGetDC
/// re-enables the following GDI records), and the winoracle probe
/// tools/oracle_probe/gdiplus_codec_probe.c (mf.* lines) for everything
/// Learn leaves open: header field values, image size rounding, image
/// flags, error statuses and the enumerated record lists.
///
/// Not implemented (each fails at the call with log_unimplemented):
/// playback (GdipPlayMetafileRecord; DrawImage of a metafile) needs an EMF /
/// EMF+ interpreter; recording (GdipRecordMetafile*) needs every Draw/Fill
/// export to journal EMF+ records; WMF input needs GDI's WMF->EMF conversion
/// (GDI+ converts every WMF to EMF on load: probe "mf.wmf hdr type=3");
/// GdipEmfToWmfBits and GdipConvertToEmfPlus* need record translators.

#include "winapi/graphics/gdiplus/gdiplus_codec.hpp"
#include "winapi/graphics/gdiplus/gdiplus.hpp"
#include "winapi/graphics/gdiplus/gdiplus_graphics.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <cmath>
#include <cstring>

namespace vitreo::winapi::gdip {

namespace {

constexpr u32 kEmrHeader = 1;
constexpr u32 kEmrEof = 14;
constexpr u32 kEmrComment = 70;
constexpr u32 kEmfSignature = 0x464D4520u;      // " EMF"
constexpr u32 kEmfPlusComment = 0x2B464D45u;    // "EMF+"
constexpr u32 kWmfPlaceableKey = 0x9AC6CDD7u;
constexpr u32 kEmfPlusHeader = 0x4001;
constexpr u32 kEmfPlusGetDC = 0x4004;

// MetafileType
constexpr u32 kTypeEmf = 3;
constexpr u32 kTypeEmfPlusOnly = 4;
constexpr u32 kTypeEmfPlusDual = 5;

/// sizeof(MetafileHeader): ten 4-byte fields, ENHMETAHEADER3 (88), three INTs.
constexpr usize kHeaderSize = 140;
constexpr usize kEnhHeader3Size = 88;

u32 le32(std::span<const u8> b, usize o) {
    u32 v = 0;
    if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
    return v;
}
u16 le16(std::span<const u8> b, usize o) {
    u16 v = 0;
    if (o + 2 <= b.size()) std::memcpy(&v, b.data() + o, 2);
    return v;
}
i32 sle32(std::span<const u8> b, usize o) { return static_cast<i32>(le32(b, o)); }

}  // namespace

/// The parsed state of one metafile object (CodecState::metafile).
struct MetafileData {
    std::vector<u8> emf;          ///< the EMF record stream (nBytes long)
    u8 header[kHeaderSize]{};     ///< MetafileHeader as GDI+ reports it
    u32 type{kTypeEmf};
    i32 x{0}, y{0}, width{0}, height{0};
    float dpi_x{96}, dpi_y{96};
    /// The HENHMETAFILE it was created from (GdipGetHemfFromMetafile hands it back).
    Handle hemf{0};
    bool owns_hemf{false};        ///< deleteEmf = TRUE: GDI+ deletes it with the object
    bool hemf_taken{false};       ///< GdipGetHemfFromMetafile ran: the object is dead
    bool recording{false};        ///< a GdipRecordMetafile* object (never created here)
    u32 raster_limit{96};
    GdiManager* gdi{nullptr};

    ~MetafileData() {
        if (owns_hemf && gdi != nullptr && hemf != 0) (void)gdi->delete_enh_metafile(hemf);
    }
};

namespace {

/// Validate an EMF record stream as GDI does (EMR_HEADER first, " EMF",
/// nBytes covering DWORD-sized records) and build its MetafileHeader.
std::shared_ptr<MetafileData> parse_emf(std::span<const u8> in) {
    if (in.size() < kEnhHeader3Size || le32(in, 0) != kEmrHeader || le32(in, 40) != kEmfSignature) return {};
    const u32 total = le32(in, 48);
    if (total < kEnhHeader3Size || total > in.size()) return {};
    auto d = std::make_shared<MetafileData>();
    d->emf.assign(in.begin(), in.begin() + total);
    const std::span<const u8> b(d->emf);
    for (usize at = 0; at < b.size();) {
        const u32 size = le32(b, at + 4);
        if (size < 8 || (size & 3) != 0 || at + size > b.size()) return {};
        at += size;
    }
    const u32 hdr_size = le32(b, 4);

    u32 version = le32(b, 44);    // nVersion
    u32 plus_flags = 0;
    i32 plus_header_size = 0;
    i32 logical_x = 0, logical_y = 0;
    d->type = kTypeEmf;
    // EMF+: the record after EMR_HEADER is an "EMF+" comment holding EmfPlusHeader.
    const usize second = hdr_size;
    if (second + 16 + 28 <= b.size() && le32(b, second) == kEmrComment && le32(b, second + 12) == kEmfPlusComment &&
        le16(b, second + 16) == kEmfPlusHeader) {
        const usize rec = second + 16;
        const u16 flags = le16(b, rec + 2);
        d->type = (flags & 1) ? kTypeEmfPlusDual : kTypeEmfPlusOnly;
        plus_header_size = static_cast<i32>(le32(b, rec + 4));
        version = le32(b, rec + 12);
        plus_flags = le32(b, rec + 16);
        logical_x = sle32(b, rec + 20);
        logical_y = sle32(b, rec + 24);
    }

    // Device scale from the reference device (probe: 1024 px / 271 mm =
    // DpiX 95.976). The pixel bounds are the inclusive-inclusive frame
    // (.01 mm) in device pixels: frame 0,0,2000,1000 -> 77x39, 0,0,2620,1295 -> 100x50.
    const i32 dev_cx = sle32(b, 72), dev_cy = sle32(b, 76);
    const i32 mm_cx = sle32(b, 80), mm_cy = sle32(b, 84);
    const double sx = mm_cx > 0 ? static_cast<double>(dev_cx) / (mm_cx * 100.0) : 96.0 / 2540.0;
    const double sy = mm_cy > 0 ? static_cast<double>(dev_cy) / (mm_cy * 100.0) : 96.0 / 2540.0;
    d->dpi_x = static_cast<float>(sx * 2540.0);
    d->dpi_y = static_cast<float>(sy * 2540.0);
    const i32 fl = sle32(b, 24), ft = sle32(b, 28), fr = sle32(b, 32), fb = sle32(b, 36);
    const i32 left = static_cast<i32>(std::lround(fl * sx)), top = static_cast<i32>(std::lround(ft * sy));
    d->x = left;
    d->y = top;
    d->width = static_cast<i32>(std::lround(fr * sx)) - left + 1;
    d->height = static_cast<i32>(std::lround(fb * sy)) - top + 1;

    u8* h = d->header;
    auto put = [h](usize o, u32 v) { std::memcpy(h + o, &v, 4); };
    put(0, d->type);
    put(4, total);
    put(8, version);
    put(12, plus_flags);
    std::memcpy(h + 16, &d->dpi_x, 4);
    std::memcpy(h + 20, &d->dpi_y, 4);
    put(24, static_cast<u32>(d->x));
    put(28, static_cast<u32>(d->y));
    put(32, static_cast<u32>(d->width));
    put(36, static_cast<u32>(d->height));
    // ENHMETAHEADER3 with nSize = sizeof(ENHMETAHEADER3) (probe "hdr raw": 0x58).
    std::memcpy(h + 40, b.data(), kEnhHeader3Size);
    put(44, static_cast<u32>(kEnhHeader3Size));
    put(128, static_cast<u32>(plus_header_size));
    put(132, static_cast<u32>(logical_x));
    put(136, static_cast<u32>(logical_y));
    return d;
}

/// Learn GetPhysicalDimension: a metafile's size in .01 mm (its frame).
void fill_image(GpImage& img, const std::shared_ptr<MetafileData>& d) {
    img.type = ImageTypeMetafile;
    img.width = d->width;
    img.height = d->height;
    img.pixel_format = PixelFormat32bppRGB;   // probe "mf.img pf=0x22009"
    img.dpi_x = d->dpi_x;
    img.dpi_y = d->dpi_y;
    img.flags = 0x50003;                       // probe "mf.img flags=0x50003"
    std::memcpy(img.raw_format, kImageFormatEMF.data(), 16);
    img.bits.clear();
    img.stride = 0;
    img.palette.clear();
    img.bounds = {static_cast<float>(d->x), static_cast<float>(d->y), static_cast<float>(d->width),
                  static_cast<float>(d->height)};
    img.bounds_unit = 2;   // UnitPixel
    const std::span<const u8> b(d->emf);
    img.physical_width = static_cast<float>(sle32(b, 32) - sle32(b, 24));
    img.physical_height = static_cast<float>(sle32(b, 36) - sle32(b, 28));
    CodecState& cs = img.codec.ensure();
    cs.format = SourceFormat::Emf;
    cs.source = d->emf;
    cs.dimension = kFrameDimensionPage;
    cs.frame_count = 1;
    cs.active_frame = 0;
    cs.properties.clear();
    cs.metafile = d;
}

bool is_placeable_wmf(std::span<const u8> b) { return b.size() >= 22 && le32(b, 0) == kWmfPlaceableKey; }
/// A standard METAHEADER (mtType 1/2, mtHeaderSize 9 words).
bool is_plain_wmf(std::span<const u8> b) {
    return b.size() >= 18 && (le16(b, 0) == 1 || le16(b, 0) == 2) && le16(b, 2) == 9;
}

GpImage* metafile_image(GdiplusManager& mgr, u64 h, MetafileData** data) {
    GpImage* img = mgr.objects().get<GpImage>(h);
    if (img == nullptr || img->type != ImageTypeMetafile) return nullptr;
    CodecState* cs = codec_of(*img);
    if (cs == nullptr || !cs->metafile || cs->metafile->hemf_taken) return nullptr;
    if (data) *data = cs->metafile.get();
    return img;
}

Status add_image(GdiplusManager& mgr, Marshaler& m, GuestAddr out, const std::shared_ptr<MetafileData>& d) {
    auto img = std::make_shared<GpImage>();
    fill_image(*img, d);
    const u64 h = mgr.objects().add(std::move(img));
    write_handle(m, out, h);
    return Status::Ok;
}

Status wmf_unimplemented(std::string_view api) {
    log_unimplemented(api, "WMF input (needs GDI's WMF->EMF record conversion)");
    return Status::NotImplemented;
}

// ---- Enumeration ---------------------------------------------------------------------

struct Rec {
    u32 type;
    u32 flags;
    u32 size;
    usize data;   ///< offset in the EMF bytes
};

/// The records GDI+ hands an EnumerateMetafileProc, in order (probe
/// "mf.enum" / "mf.rec.enum"): every GDI record of a plain EMF; for an EMF+
/// file the EMF+ records in place of their comments, plus EMR_HEADER, EMR_EOF
/// and the GDI records an EmfPlusGetDC re-enables until the next EMF+ record.
std::vector<Rec> records_of(const MetafileData& d) {
    std::vector<Rec> out;
    const std::span<const u8> b(d.emf);
    const bool plus = d.type == kTypeEmfPlusOnly || d.type == kTypeEmfPlusDual;
    bool get_dc = false;
    for (usize at = 0; at + 8 <= b.size();) {
        const u32 t = le32(b, at), size = le32(b, at + 4);
        if (size < 8) break;
        if (t == kEmrComment && size >= 16 && le32(b, at + 12) == kEmfPlusComment) {
            const usize end = std::min<usize>(at + size, at + 12 + le32(b, at + 8));
            for (usize p = at + 16; p + 12 <= end;) {
                const u32 rs = le32(b, p + 4);
                if (rs < 12 || p + rs > end) break;
                const u32 ds = std::min<u32>(le32(b, p + 8), rs - 12);
                const u32 rt = le16(b, p);
                out.push_back({rt, le16(b, p + 2), ds, p + 12});
                get_dc = rt == kEmfPlusGetDC;
                p += rs;
            }
        } else if (!plus || t == kEmrHeader || t == kEmrEof || get_dc) {
            out.push_back({t, 0, size - 8, at + 8});
        }
        at += size;
    }
    return out;
}

/// Shared tail of the twelve GdipEnumerateMetafile* exports, after the
/// destination geometry was read.
Status enumerate(GdiplusManager& mgr, Marshaler& m, u64 graphics, u64 metafile, GuestAddr callback, u64 data,
                 u64 attributes) {
    if (mgr.objects().get<GpGraphics>(graphics) == nullptr || callback == 0) return Status::InvalidParameter;
    if (attributes != 0 && mgr.objects().get<GpImageAttributes>(attributes) == nullptr) return Status::InvalidParameter;
    MetafileData* d = nullptr;
    if (metafile_image(mgr, metafile, &d) == nullptr) return Status::InvalidParameter;
    const GuestCaller call = guest_caller(mgr);
    if (!call) {
        log_unimplemented("GdipEnumerateMetafile", "no guest caller installed");
        return Status::GenericError;
    }
    const std::vector<Rec> recs = records_of(*d);
    // The records live in guest memory for the callback's data pointer.
    const std::vector<u8> bytes = d->emf;
    GuestBlock block(mgr, bytes.size());
    if (block.addr() == 0) return Status::OutOfMemory;
    if (m.memory().write_bytes(block.addr(), bytes.data(), bytes.size()).is_err()) return Status::OutOfMemory;
    const bool is64 = m.cpu_is_64bit();
    for (const Rec& r : recs) {
        const u64 args[5] = {r.type, r.flags, r.size, r.size ? block.addr() + r.data : 0, data};
        // EnumerateMetafileProc returns FALSE to stop the enumeration.
        if (static_cast<u32>(call(callback, args, 5, is64)) == 0) break;
    }
    return Status::Ok;
}

bool read_dest_points(Marshaler& m, GuestAddr at, i32 count, bool integer) {
    std::vector<PointF> pts;
    if (at == 0) return false;
    return integer ? read_points_i(m, at, count, pts) : read_points(m, at, count, pts);
}

bool read_any_rect(Marshaler& m, GuestAddr at, bool integer) {
    RectF r;
    if (at == 0) return false;
    return integer ? read_rect_i(m, at, r) : read_rect(m, at, r);
}

/// Unit (GdiPlusEnums.h): UnitWorld .. UnitMillimeter.
bool valid_unit(u32 u) { return u <= 6; }

}  // namespace

// ---- Shared entry points (gdiplus_codec.hpp) ------------------------------------------------

bool looks_like_metafile(std::span<const u8> bytes) {
    // The EMF and WMF decoders' signatures (GdipGetImageDecoders): " EMF" at
    // offset 40, or the placeable key D7CDC69A.
    return (bytes.size() >= 44 && le32(bytes, 40) == kEmfSignature) || is_placeable_wmf(bytes);
}

Status create_metafile_image(GdiplusManager& mgr, std::vector<u8> bytes, GpImage& out) {
    if (is_placeable_wmf(bytes)) return wmf_unimplemented("GdipLoadImageFromFile");
    auto d = parse_emf(bytes);
    if (!d) return Status::OutOfMemory;   // GdipLoadImageFromFile's status for an undecodable file
    d->gdi = mgr.gdi();
    fill_image(out, d);
    return Status::Ok;
}

// ---- Exports ---------------------------------------------------------------------------------

void register_codec_metafile(GdiplusManager& mgr, HandlerMap& map) {
    // GdipCreateMetafileFromEmf(HENHMETAFILE, BOOL deleteEmf, GpMetafile**)
    map["GdipCreateMetafileFromEmf"] = [&mgr](Marshaler& m) {
        const Handle hemf = static_cast<Handle>(m.arg(0));
        const bool del = m.arg32(1) != 0;
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) { write_status(m, Status::InvalidParameter); return; }
        const std::vector<u8>* bits = mgr.gdi() ? mgr.gdi()->enh_metafile(hemf) : nullptr;
        std::shared_ptr<MetafileData> d = bits ? parse_emf(*bits) : nullptr;
        if (!d) { write_handle(m, out, 0); write_status(m, Status::InvalidParameter); return; }
        d->gdi = mgr.gdi();
        d->hemf = hemf;
        d->owns_hemf = del;
        write_status(m, add_image(mgr, m, out, d));
    };

    auto from_bytes = [&mgr](Marshaler& m, std::optional<std::vector<u8>> bytes, GuestAddr out,
                             std::string_view api) {
        if (!bytes) { write_handle(m, out, 0); write_status(m, Status::GenericError); return; }
        if (is_placeable_wmf(*bytes) || is_plain_wmf(*bytes)) {
            write_handle(m, out, 0);
            write_status(m, wmf_unimplemented(api));
            return;
        }
        auto d = parse_emf(*bytes);
        // Not a metafile: probe "mf.file.png st=1".
        if (!d) { write_handle(m, out, 0); write_status(m, Status::GenericError); return; }
        d->gdi = mgr.gdi();
        write_status(m, add_image(mgr, m, out, d));
    };

    // GdipCreateMetafileFromFile(const WCHAR*, GpMetafile**)
    map["GdipCreateMetafileFromFile"] = [&mgr, from_bytes](Marshaler& m) {
        const GuestAddr name = m.arg_ptr(0), out = m.arg_ptr(1);
        if (name == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        from_bytes(m, read_guest_file(mgr, read_path(m, name)), out, "GdipCreateMetafileFromFile");
    };
    // GdipCreateMetafileFromStream(IStream*, GpMetafile**): reads to the end (probe "mf.stream pos=180").
    map["GdipCreateMetafileFromStream"] = [&mgr, from_bytes](Marshaler& m) {
        const GuestAddr stream = m.arg_ptr(0), out = m.arg_ptr(1);
        if (stream == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        GuestStream s(mgr, m, stream);
        if (!s.usable()) { write_handle(m, out, 0); write_status(m, Status::GenericError); return; }
        from_bytes(m, s.read_to_end(), out, "GdipCreateMetafileFromStream");
    };
    // GdipCreateMetafileFromWmf(HMETAFILE, BOOL, const WmfPlaceableFileHeader*, GpMetafile**)
    map["GdipCreateMetafileFromWmf"] = [&mgr](Marshaler& m) {
        const Handle hwmf = static_cast<Handle>(m.arg(0));
        const GuestAddr out = m.arg_ptr(3);
        if (out == 0 || mgr.gdi() == nullptr || !mgr.gdi()->has_metafile(hwmf)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        write_status(m, wmf_unimplemented("GdipCreateMetafileFromWmf"));
    };
    // GdipCreateMetafileFromWmfFile(const WCHAR*, const WmfPlaceableFileHeader*, GpMetafile**)
    map["GdipCreateMetafileFromWmfFile"] = [&mgr](Marshaler& m) {
        const GuestAddr name = m.arg_ptr(0), out = m.arg_ptr(2);
        if (name == 0 || out == 0) { write_status(m, Status::InvalidParameter); return; }
        if (!read_guest_file(mgr, read_path(m, name))) { write_status(m, Status::GenericError); return; }
        write_status(m, wmf_unimplemented("GdipCreateMetafileFromWmfFile"));
    };

    // ---- Headers ----
    auto put_header = [](Marshaler& m, GuestAddr at, const MetafileData& d) {
        return m.memory().write_bytes(at, d.header, kHeaderSize).is_ok() ? Status::Ok : Status::InvalidParameter;
    };
    map["GdipGetMetafileHeaderFromEmf"] = [&mgr, put_header](Marshaler& m) {
        const Handle hemf = static_cast<Handle>(m.arg(0));
        const GuestAddr hdr = m.arg_ptr(1);
        const std::vector<u8>* bits = mgr.gdi() ? mgr.gdi()->enh_metafile(hemf) : nullptr;
        auto d = bits && hdr ? parse_emf(*bits) : nullptr;
        write_status(m, d ? put_header(m, hdr, *d) : Status::InvalidParameter);
    };
    map["GdipGetMetafileHeaderFromFile"] = [&mgr, put_header](Marshaler& m) {
        const GuestAddr name = m.arg_ptr(0), hdr = m.arg_ptr(1);
        if (name == 0 || hdr == 0) { write_status(m, Status::InvalidParameter); return; }
        const auto bytes = read_guest_file(mgr, read_path(m, name));
        if (bytes && (is_placeable_wmf(*bytes) || is_plain_wmf(*bytes))) {
            write_status(m, wmf_unimplemented("GdipGetMetafileHeaderFromFile"));
            return;
        }
        auto d = bytes ? parse_emf(*bytes) : nullptr;
        // Not a metafile: probe "mf.hdrfile.png st=2".
        write_status(m, d ? put_header(m, hdr, *d) : Status::InvalidParameter);
    };
    map["GdipGetMetafileHeaderFromStream"] = [&mgr, put_header](Marshaler& m) {
        const GuestAddr stream = m.arg_ptr(0), hdr = m.arg_ptr(1);
        if (stream == 0 || hdr == 0) { write_status(m, Status::InvalidParameter); return; }
        GuestStream s(mgr, m, stream);
        const auto bytes = s.usable() ? s.read_to_end() : std::nullopt;
        if (bytes && (is_placeable_wmf(*bytes) || is_plain_wmf(*bytes))) {
            write_status(m, wmf_unimplemented("GdipGetMetafileHeaderFromStream"));
            return;
        }
        auto d = bytes ? parse_emf(*bytes) : nullptr;
        write_status(m, d ? put_header(m, hdr, *d) : Status::InvalidParameter);
    };
    map["GdipGetMetafileHeaderFromMetafile"] = [&mgr, put_header](Marshaler& m) {
        const u64 h = m.arg(0);
        const GuestAddr hdr = m.arg_ptr(1);
        GpImage* img = mgr.objects().get<GpImage>(h);
        if (img == nullptr || hdr == 0) { write_status(m, Status::InvalidParameter); return; }
        MetafileData* d = nullptr;
        if (metafile_image(mgr, h, &d) != nullptr) { write_status(m, put_header(m, hdr, *d)); return; }
        if (img->type == ImageTypeBitmap) {
            // Probe "mf.hdr.bitmap st=0": a bitmap succeeds; it has no
            // metafile header, so every field reads MetafileTypeInvalid / 0.
            const u8 zero[kHeaderSize]{};
            write_status(m, m.memory().write_bytes(hdr, zero, kHeaderSize).is_ok() ? Status::Ok
                                                                                  : Status::InvalidParameter);
            return;
        }
        write_status(m, Status::InvalidParameter);
    };
    // GdipGetMetafileHeaderFromWmf(HMETAFILE, const WmfPlaceableFileHeader*, MetafileHeader*):
    // winoracle answers InvalidParameter for a valid HMETAFILE with a valid
    // placeable header (probe "mf.wmf.hdrfromwmf st=2"), and for NULL arguments.
    map["GdipGetMetafileHeaderFromWmf"] = [](Marshaler& m) {
        (void)m.arg(0);
        write_status(m, Status::InvalidParameter);
    };

    // GdipGetHemfFromMetafile(GpMetafile*, HENHMETAFILE*): the metafile is
    // unusable afterwards (probe "mf.width-after st=2", "mf.gethemf2 st=2").
    map["GdipGetHemfFromMetafile"] = [&mgr](Marshaler& m) {
        const u64 h = m.arg(0);
        const GuestAddr out = m.arg_ptr(1);
        MetafileData* d = nullptr;
        GpImage* img = metafile_image(mgr, h, &d);
        if (img == nullptr || out == 0 || mgr.gdi() == nullptr) { write_status(m, Status::InvalidParameter); return; }
        Handle hemf = d->hemf;
        if (hemf == 0 || mgr.gdi()->enh_metafile(hemf) == nullptr) hemf = mgr.gdi()->set_enh_metafile_bits(d->emf);
        if (hemf == 0) { write_status(m, Status::GenericError); return; }
        d->hemf = hemf;
        d->owns_hemf = false;   // the caller owns it now
        d->hemf_taken = true;
        img->type = ImageTypeUnknown;
        img->width = 0;
        img->height = 0;
        write_handle(m, out, hemf);
        write_status(m, Status::Ok);
    };

    // ---- Down-level rasterization limit (recording metafiles only) ----
    map["GdipGetMetafileDownLevelRasterizationLimit"] = [&mgr](Marshaler& m) {
        const u64 h = m.arg(0);
        const GuestAddr out = m.arg_ptr(1);
        GpImage* img = mgr.objects().get<GpImage>(h);
        if (img == nullptr || out == 0) { write_status(m, Status::InvalidParameter); return; }
        MetafileData* d = nullptr;
        if (metafile_image(mgr, h, &d) == nullptr) {
            // Probe "mf.limit.bitmap st=8".
            write_status(m, img->type == ImageTypeBitmap ? Status::WrongState : Status::InvalidParameter);
            return;
        }
        if (!d->recording) { write_status(m, Status::WrongState); return; }   // "mf.limit st=8"
        (void)m.write_u32(out, d->raster_limit);
        write_status(m, Status::Ok);
    };
    map["GdipSetMetafileDownLevelRasterizationLimit"] = [&mgr](Marshaler& m) {
        const u64 h = m.arg(0);
        const u32 dpi = m.arg32(1);
        GpImage* img = mgr.objects().get<GpImage>(h);
        if (img == nullptr) { write_status(m, Status::InvalidParameter); return; }
        MetafileData* d = nullptr;
        if (metafile_image(mgr, h, &d) == nullptr) {
            write_status(m, img->type == ImageTypeBitmap ? Status::WrongState : Status::InvalidParameter);
            return;
        }
        if (!d->recording) { write_status(m, Status::WrongState); return; }   // "mf.setlimit st=8"
        // "setlimit5 st=2", "setlimit0 st=0 limit=96" (0 keeps it), "setlimit50 -> 50".
        if (dpi != 0 && dpi < 10) { write_status(m, Status::InvalidParameter); return; }
        if (dpi != 0) d->raster_limit = dpi;
        write_status(m, Status::Ok);
    };

    // ---- Enumeration (12 forms) ----
    // DestPoint(g, mf, const PointF*, cb, data, attr)
    for (const bool integer : {false, true}) {
        const std::string sfx = integer ? "I" : "";
        map["GdipEnumerateMetafileDestPoint" + sfx] = [&mgr, integer](Marshaler& m) {
            if (!read_dest_points(m, m.arg_ptr(2), 1, integer)) { write_status(m, Status::InvalidParameter); return; }
            write_status(m, enumerate(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(3), m.arg(4), m.arg(5)));
        };
        // DestPoints(g, mf, const PointF*, INT count, cb, data, attr)
        map["GdipEnumerateMetafileDestPoints" + sfx] = [&mgr, integer](Marshaler& m) {
            const i32 count = static_cast<i32>(m.arg32(3));
            if (count != 3 || !read_dest_points(m, m.arg_ptr(2), count, integer)) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            write_status(m, enumerate(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(4), m.arg(5), m.arg(6)));
        };
        // DestRect(g, mf, const RectF*, cb, data, attr)
        map["GdipEnumerateMetafileDestRect" + sfx] = [&mgr, integer](Marshaler& m) {
            if (!read_any_rect(m, m.arg_ptr(2), integer)) { write_status(m, Status::InvalidParameter); return; }
            write_status(m, enumerate(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(3), m.arg(4), m.arg(5)));
        };
        // SrcRectDestPoint(g, mf, const PointF*, const RectF* src, Unit, cb, data, attr)
        map["GdipEnumerateMetafileSrcRectDestPoint" + sfx] = [&mgr, integer](Marshaler& m) {
            if (!read_dest_points(m, m.arg_ptr(2), 1, integer) || !read_any_rect(m, m.arg_ptr(3), integer) ||
                !valid_unit(m.arg32(4))) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            write_status(m, enumerate(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(5), m.arg(6), m.arg(7)));
        };
        // SrcRectDestPoints(g, mf, const PointF*, INT count, const RectF* src, Unit, cb, data, attr)
        map["GdipEnumerateMetafileSrcRectDestPoints" + sfx] = [&mgr, integer](Marshaler& m) {
            const i32 count = static_cast<i32>(m.arg32(3));
            if (count != 3 || !read_dest_points(m, m.arg_ptr(2), count, integer) ||
                !read_any_rect(m, m.arg_ptr(4), integer) || !valid_unit(m.arg32(5))) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            write_status(m, enumerate(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(6), m.arg(7), m.arg(8)));
        };
        // SrcRectDestRect(g, mf, const RectF* dest, const RectF* src, Unit, cb, data, attr)
        map["GdipEnumerateMetafileSrcRectDestRect" + sfx] = [&mgr, integer](Marshaler& m) {
            if (!read_any_rect(m, m.arg_ptr(2), integer) || !read_any_rect(m, m.arg_ptr(3), integer) ||
                !valid_unit(m.arg32(4))) {
                write_status(m, Status::InvalidParameter);
                return;
            }
            write_status(m, enumerate(mgr, m, m.arg(0), m.arg(1), m.arg_ptr(5), m.arg(6), m.arg(7)));
        };
    }

    // GdipPlayMetafileRecord(const GpMetafile*, EmfPlusRecordType, UINT flags, UINT dataSize, const BYTE*)
    map["GdipPlayMetafileRecord"] = [&mgr](Marshaler& m) {
        const u32 size = m.arg32(3);
        if (metafile_image(mgr, m.arg(0), nullptr) == nullptr || (size != 0 && m.arg_ptr(4) == 0)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        log_unimplemented("GdipPlayMetafileRecord", "EMF / EMF+ record playback");
        write_status(m, Status::NotImplemented);
    };

    // ---- Recording ----
    // Validation measured: NULL hdc "mf.record.nullhdc st=2", EmfType 7
    // "mf.record.badtype st=2"; a NULL frame rect is allowed.
    auto record = [&mgr](Marshaler& m, usize first, bool needs_target, std::string_view api) {
        const Handle hdc = static_cast<Handle>(m.arg(first));
        const u32 type = m.arg32(first + 1);
        const u32 unit = m.arg32(first + 3);
        const GuestAddr out = m.arg_ptr(first + 5);
        const bool target_ok = !needs_target || m.arg_ptr(0) != 0;
        if (!target_ok || out == 0 || mgr.gdi() == nullptr || !mgr.gdi()->has_dc(hdc) ||
            type < kTypeEmf || type > kTypeEmfPlusDual || unit < 2 || unit > 7) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        log_unimplemented(api, "metafile recording (Draw/Fill exports do not journal EMF+ records)");
        write_status(m, Status::NotImplemented);
    };
    map["GdipRecordMetafile"] = [record](Marshaler& m) { record(m, 0, false, "GdipRecordMetafile"); };
    map["GdipRecordMetafileI"] = [record](Marshaler& m) { record(m, 0, false, "GdipRecordMetafileI"); };
    map["GdipRecordMetafileFileName"] = [record](Marshaler& m) { record(m, 1, true, "GdipRecordMetafileFileName"); };
    map["GdipRecordMetafileFileNameI"] = [record](Marshaler& m) { record(m, 1, true, "GdipRecordMetafileFileNameI"); };
    map["GdipRecordMetafileStream"] = [record](Marshaler& m) { record(m, 1, true, "GdipRecordMetafileStream"); };
    map["GdipRecordMetafileStreamI"] = [record](Marshaler& m) { record(m, 1, true, "GdipRecordMetafileStreamI"); };

    // GdipEmfToWmfBits(HENHMETAFILE, UINT cbData16, LPBYTE, INT mapMode, INT flags): bytes, 0 on failure.
    map["GdipEmfToWmfBits"] = [&mgr](Marshaler& m) {
        const Handle hemf = static_cast<Handle>(m.arg(0));
        if (mgr.gdi() != nullptr && mgr.gdi()->enh_metafile(hemf) != nullptr)
            log_unimplemented("GdipEmfToWmfBits", "EMF -> WMF record conversion");
        m.set_return(0);
    };

    // GdipConvertToEmfPlus[ToFile|ToStream](const GpGraphics*, GpMetafile*, INT* failFlag,
    //   [WCHAR* file | IStream*], EmfType, const WCHAR* desc, GpMetafile** out)
    auto convert = [&mgr](Marshaler& m, bool has_target, std::string_view api) {
        const usize k = has_target ? 1 : 0;
        const GuestAddr fail = m.arg_ptr(2);
        const u32 type = m.arg32(3 + k);
        const GuestAddr out = m.arg_ptr(5 + k);
        MetafileData* d = nullptr;
        if (mgr.objects().get<GpGraphics>(m.arg(0)) == nullptr || metafile_image(mgr, m.arg(1), &d) == nullptr ||
            out == 0 || (has_target && m.arg_ptr(3) == 0) || (type != kTypeEmfPlusOnly && type != kTypeEmfPlusDual)) {
            write_status(m, Status::InvalidParameter);
            return;
        }
        if (d->type != kTypeEmf) {
            // Already EMF+: probe "mf.convert-of-plus st=2 fail=0".
            if (fail != 0) (void)m.write_u32(fail, 0);
            write_status(m, Status::InvalidParameter);
            return;
        }
        log_unimplemented(api, "EMF -> EMF+ record conversion");
        write_status(m, Status::NotImplemented);
    };
    map["GdipConvertToEmfPlus"] = [convert](Marshaler& m) { convert(m, false, "GdipConvertToEmfPlus"); };
    map["GdipConvertToEmfPlusToFile"] = [convert](Marshaler& m) { convert(m, true, "GdipConvertToEmfPlusToFile"); };
    map["GdipConvertToEmfPlusToStream"] = [convert](Marshaler& m) { convert(m, true, "GdipConvertToEmfPlusToStream"); };
}

}  // namespace vitreo::winapi::gdip
