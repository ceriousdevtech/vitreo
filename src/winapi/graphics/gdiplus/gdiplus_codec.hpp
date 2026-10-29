#pragma once
/// @file gdiplus_codec.hpp
/// @brief GDI+ codec area: decoders/encoders, property items, frames,
/// effects, metafiles. Shared between the area's translation units
/// (gdiplus_codec*.cpp / .mm); other areas reach it only through GpImage.
///
/// Contract sources: Windows SDK GdiPlusImaging.h, GdiPlusFlat.h,
/// GdiPlusEffects.h, GdiPlusMetaHeader.h, GdiPlusColorMatrix.h; Microsoft
/// Learn (Image, Bitmap, Metafile, Effect classes); and the winoracle probe
/// tools/oracle_probe/gdiplus_codec_probe.c (.expected = x64,
/// .x86.expected = x86), which supplies every value Learn does not state.

#include "winapi/graphics/gdiplus/gdiplus_core.hpp"
#include "winapi/graphics/gdiplus/gdiplus_image.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi {
class GdiplusManager;
class Marshaler;
}

namespace vitreo::winapi::gdip {

// ---- GUIDs -------------------------------------------------------------------

/// A GUID in its in-memory (little-endian Data1..3) byte layout.
using Guid = std::array<u8, 16>;

[[nodiscard]] constexpr Guid make_guid(u32 d1, u16 d2, u16 d3, u8 a, u8 b, u8 c, u8 d, u8 e, u8 f, u8 g,
                                       u8 h) {
    return Guid{static_cast<u8>(d1), static_cast<u8>(d1 >> 8), static_cast<u8>(d1 >> 16),
                static_cast<u8>(d1 >> 24), static_cast<u8>(d2), static_cast<u8>(d2 >> 8),
                static_cast<u8>(d3), static_cast<u8>(d3 >> 8), a, b, c, d, e, f, g, h};
}

// GdiPlusImaging.h
inline constexpr Guid kImageFormatUndefined = make_guid(0xb96b3ca9, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatMemoryBMP = make_guid(0xb96b3caa, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatBMP = make_guid(0xb96b3cab, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatEMF = make_guid(0xb96b3cac, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatWMF = make_guid(0xb96b3cad, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatJPEG = make_guid(0xb96b3cae, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatPNG = make_guid(0xb96b3caf, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatGIF = make_guid(0xb96b3cb0, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatTIFF = make_guid(0xb96b3cb1, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatEXIF = make_guid(0xb96b3cb2, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kImageFormatIcon = make_guid(0xb96b3cb5, 0x0728, 0x11d3, 0x9d, 0x7b, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e);
inline constexpr Guid kFrameDimensionTime = make_guid(0x6aedbd6d, 0x3fb5, 0x418a, 0x83, 0xa6, 0x7f, 0x45, 0x22, 0x9d, 0xc8, 0x72);
inline constexpr Guid kFrameDimensionResolution = make_guid(0x84236f7b, 0x3bd3, 0x428f, 0x8d, 0xab, 0x4e, 0xa1, 0x43, 0x9c, 0xa3, 0x15);
inline constexpr Guid kFrameDimensionPage = make_guid(0x7462dc86, 0x6180, 0x4c7e, 0x8e, 0x3f, 0xee, 0x73, 0x33, 0xa7, 0xa4, 0x83);

[[nodiscard]] bool read_guid(Marshaler& m, GuestAddr at, Guid& out);
bool write_guid(Marshaler& m, GuestAddr at, const Guid& g);

// ---- Property items ------------------------------------------------------------

/// PropertyTagType* (GdiPlusImaging.h).
enum PropertyType : u16 {
    PropertyTypeByte = 1,
    PropertyTypeASCII = 2,
    PropertyTypeShort = 3,
    PropertyTypeLong = 4,
    PropertyTypeRational = 5,
    PropertyTypeUndefined = 7,
    PropertyTypeSLONG = 9,
    PropertyTypeSRational = 10,
};

/// One GDI+ PropertyItem, held host-side in the order GDI+ reports it.
struct Property {
    u32 id{0};
    u16 type{0};
    std::vector<u8> value;
};

// ---- Per-image codec state ----------------------------------------------------

/// The codec an image was decoded by (its raw format).
enum class SourceFormat : u8 { Memory, Bmp, Jpeg, Gif, Emf, Wmf, Tiff, Png, Icon };

/// One frame decoded into a GDI+ pixel format.
struct DecodedFrame {
    i32 width{0};
    i32 height{0};
    i32 pixel_format{PixelFormat32bppARGB};
    i32 stride{0};
    std::vector<u8> bits;          ///< top-down rows of @p stride bytes
    std::vector<u32> palette;      ///< ARGB entries for indexed formats
    u32 palette_flags{0};          ///< PaletteFlags
    float dpi_x{96.0f};
    float dpi_y{96.0f};
    u32 flags{0};                  ///< ImageFlags
    std::vector<Property> properties;
};

struct SaveSession;    ///< an open multi-frame SaveImageTo*/SaveAdd sequence (gdiplus_codec.cpp)
struct MetafileData;   ///< a metafile's records and header (gdiplus_codec_metafile.cpp)

struct CodecState {
    SourceFormat format{SourceFormat::Memory};
    std::vector<u8> source;        ///< the encoded bytes the image was decoded from
    Guid dimension{kFrameDimensionPage};
    u32 frame_count{1};
    u32 active_frame{0};
    /// The current frame's property items (editable by Set/RemovePropertyItem).
    std::vector<Property> properties;
    std::shared_ptr<SaveSession> session;     ///< never copied with the image
    std::shared_ptr<MetafileData> metafile;   ///< shared read-only records
};

/// The codec state of @p image, null when it has none (a plain bitmap).
[[nodiscard]] inline CodecState* codec_of(const GpImage& image) { return image.codec.get(); }

// ---- Guest calls, memory, files and IStreams ------------------------------------

/// Invoke guest code (the injected GdiManager caller). Returns nullopt when
/// no caller is installed: the export must then fail rather than pretend.
using GuestCaller = std::function<u64(GuestAddr function, const u64* args, usize count, bool is_64bit)>;
[[nodiscard]] GuestCaller guest_caller(GdiplusManager& mgr);

/// Guest heap memory from the process heap (what GdipAlloc hands out), freed
/// on destruction unless released.
class GuestBlock {
public:
    GuestBlock(GdiplusManager& mgr, u64 size);
    ~GuestBlock();
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    [[nodiscard]] GuestAddr addr() const noexcept { return m_addr; }
    /// Keep the memory (it now belongs to the guest; GdipFree releases it).
    GuestAddr release() noexcept { const GuestAddr a = m_addr; m_addr = 0; return a; }

private:
    GdiplusManager& m_mgr;
    GuestAddr m_addr{0};
};

/// A guest IStream, called through its own vtable (objidl.h slot order:
/// QueryInterface, AddRef, Release, Read, Write, Seek, SetSize, CopyTo,
/// Commit, Revert, LockRegion, UnlockRegion, Stat, Clone).
class GuestStream {
public:
    GuestStream(GdiplusManager& mgr, Marshaler& m, GuestAddr stream);
    [[nodiscard]] bool usable() const noexcept { return m_stream != 0 && static_cast<bool>(m_call); }
    /// IStream::Seek. @p origin is STREAM_SEEK_SET/CUR/END. False on a failed HRESULT.
    bool seek(i64 move, u32 origin, u64* new_position = nullptr);
    /// Current position (Seek(0, CUR)).
    [[nodiscard]] std::optional<u64> tell();
    /// IStream::Read up to @p size bytes; the count read, nullopt on a failed HRESULT.
    [[nodiscard]] std::optional<u64> read(u8* out, u64 size);
    /// Everything from the current position to the end of the stream.
    [[nodiscard]] std::optional<std::vector<u8>> read_to_end();
    /// IStream::Write all of @p data. False on a failed HRESULT or short write.
    bool write(std::span<const u8> data);

private:
    u64 call(usize slot, std::initializer_list<u64> args);
    GdiplusManager& m_mgr;
    Marshaler& m_m;
    GuestAddr m_stream;
    GuestCaller m_call;
    bool m_64;
};

/// Read a whole guest file (VFS path in UTF-8). nullopt when it cannot be opened.
[[nodiscard]] std::optional<std::vector<u8>> read_guest_file(GdiplusManager& mgr, const std::string& path);
/// Create/truncate and write a guest file. False on failure.
bool write_guest_file(GdiplusManager& mgr, const std::string& path, std::span<const u8> data);
/// The UTF-8 path of a guest WCHAR* (empty when unreadable).
[[nodiscard]] std::string read_path(Marshaler& m, GuestAddr wide);

// ---- Pixels ---------------------------------------------------------------------

/// Bytes per row GDI+ uses for @p width pixels of @p format (DWORD aligned).
[[nodiscard]] i32 stride_for(i32 width, i32 format);
/// @p image's pixels as non-premultiplied ARGB (0xAARRGGBB), top-down,
/// converted from its own format exactly as GDI+ reads them. False when the
/// pixels are unreadable (a metafile, or guest Scan0 memory unmapped).
[[nodiscard]] bool read_argb(Marshaler& m, const GpImage& image, std::vector<u32>& argb);
/// Store @p argb into @p image in its own format (indexed formats map each
/// pixel to the nearest palette entry).
[[nodiscard]] bool write_argb(Marshaler& m, GpImage& image, const std::vector<u32>& argb);
/// Convert @p argb into a frame of @p format (palette given for indexed).
void argb_to_frame(const std::vector<u32>& argb, i32 width, i32 height, i32 format,
                   const std::vector<u32>& palette, DecodedFrame& out);
/// Install @p frame into @p image (pixels, format, palette, dpi, flags).
void apply_frame(GpImage& image, DecodedFrame frame);

// ---- Metafiles (gdiplus_codec_metafile.cpp) -----------------------------------------

/// Turn EMF / placeable-WMF bytes into a metafile GpImage (type Metafile).
/// Status as GdipLoadImageFromFile reports it on failure.
[[nodiscard]] Status create_metafile_image(GdiplusManager& mgr, std::vector<u8> bytes, GpImage& out);
/// True when @p bytes start like an EMF or a placeable WMF (the codec signatures).
[[nodiscard]] bool looks_like_metafile(std::span<const u8> bytes);

// ---- Registrars of the area's other translation units ---------------------------------

void register_codec_effects(GdiplusManager& mgr, HandlerMap& map);    ///< gdiplus_codec_effects.cpp
void register_codec_metafile(GdiplusManager& mgr, HandlerMap& map);   ///< gdiplus_codec_metafile.cpp

}  // namespace vitreo::winapi::gdip
