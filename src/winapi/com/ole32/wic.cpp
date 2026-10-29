// SPDX-License-Identifier: LGPL-2.1-only
/// @file wic.cpp
/// @brief Native WIC factory, decoder/frame and format-converter objects.
/// Authoritative interfaces: Microsoft Learn IWICImagingFactory,
/// IWICBitmapDecoder and IWICBitmapSource; ABI constants: Windows SDK.
/// Filename BMP and native-byte stream PNG are supported; unsupported
/// codecs, native layouts and interface methods fail explicitly.

#include "wic.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/bmp_decode.hpp"
#include "graphics/common/image_codec.hpp"
#include "graphics/common/png_decode.hpp"
#include <atomic>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

namespace {

#include "wic_stream_contract.inc"
#include "wic_indexed_contract.inc"

constexpr u32 kSOk = 0;
constexpr u32 kSFalse = 1;
constexpr u32 kENotImpl = 0x80004001u;
constexpr u32 kENoInterface = 0x80004002u;
constexpr u32 kEPointer = 0x80004003u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kEOutOfMemory = 0x8007000Eu;
constexpr u32 kClassENoAggregation = 0x80040110u;
constexpr u32 kClassENotAvailable = 0x80040111u;
constexpr u32 kHrFileNotFound = 0x80070002u; // HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
constexpr u32 kWincodecWrongState = 0x88982F04u;
constexpr u32 kWincodecNotInitialized = 0x88982F0Cu;
constexpr u32 kWincodecPaletteUnavailable = 0x88982F45u;
constexpr u32 kWincodecComponentNotFound = 0x88982F50u;
constexpr u32 kWincodecBadHeader = 0x88982F61u;
constexpr u32 kWincodecFrameMissing = 0x88982F62u;
constexpr u32 kWincodecUnsupportedPixelFormat = 0x88982F80u;
constexpr u32 kWincodecUnsupportedOperation = 0x88982F81u;
constexpr u32 kWincodecInsufficientBuffer = 0x88982F8Cu;

constexpr Guid kClsidWicImagingFactory{
    0xCACAF262u, 0x9370u, 0x4615u, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
constexpr Guid kClsidWicImagingFactory2{
    0x317D06E8u, 0x5F24u, 0x433Du, {0xBD, 0xF7, 0x79, 0xCE, 0x68, 0xD8, 0xAB, 0xC2}};
constexpr Guid kIidUnknown{
    0x00000000u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid kIidImagingFactory{
    0xEC5EC8A9u, 0xC395u, 0x4314u, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
constexpr Guid kIidImagingFactory2{
    0x7B816B45u, 0x1996u, 0x4476u, {0xB1, 0x32, 0xDE, 0x9E, 0x24, 0x7C, 0x8A, 0xF0}};
constexpr Guid kIidBitmapDecoder{
    0x9EDDE9E7u, 0x8DEEu, 0x47EAu, {0x99, 0xDF, 0xE6, 0xFA, 0xF2, 0xED, 0x44, 0xBF}};
constexpr Guid kIidBitmapSource{
    0x00000120u, 0xA8F2u, 0x4877u, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr Guid kIidBitmapFrameDecode{
    0x3B16811Bu, 0x6A43u, 0x4EC9u, {0xA8, 0x13, 0x3D, 0x93, 0x0C, 0x13, 0xB9, 0x40}};
constexpr Guid kIidFormatConverter{
    0x00000301u, 0xA8F2u, 0x4877u, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};
constexpr Guid kIidClassFactory{
    0x00000001u, 0x0000u, 0x0000u, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid kContainerFormatBmp{
    0x0AF1D87Eu, 0xFCFEu, 0x4188u, {0xBD, 0xEB, 0xA7, 0x90, 0x64, 0x71, 0xCB, 0xE3}};

// Every WIC pixel format GUID is {6FDDC324-4E03-4BFE-B185-3D77768DC9xx}: the
// last byte indexes the format (wincodec.h).
constexpr u8 kPf1bppIndexed = 0x01;
constexpr u8 kPf4bppIndexed = 0x03;
constexpr u8 kPf8bppIndexed = 0x04;
constexpr u8 kPf24bppBGR = 0x0C;
constexpr u8 kPf32bppBGR = 0x0E;
constexpr u8 kPf32bppBGRA = 0x0F;
constexpr u8 kPf32bppPBGRA = 0x10;
constexpr u8 kPf32bppRGBA = 0x1F;

constexpr Guid pixel_format_guid(u8 index) {
    return Guid{0x6FDDC324u, 0x4E03u, 0x4BFEu,
                {0xB1, 0x85, 0x3D, 0x77, 0x76, 0x8D, 0xC9, index}};
}

bool same_guid(const Guid& a, const Guid& b) {
    return std::memcmp(&a, &b, sizeof(Guid)) == 0;
}

/// Index of a WIC pixel format GUID, or nullopt for a GUID outside the family.
std::optional<u8> pixel_format_index(const Guid& g) {
    const Guid probe = pixel_format_guid(g.Data4[7]);
    if (!same_guid(probe, g)) return std::nullopt;
    return g.Data4[7];
}

u32 indexed_bits(u8 format){
 if(format==GUID_WICPixelFormat1bppIndexed.Data4[7])return 1;
 if(format==GUID_WICPixelFormat2bppIndexed.Data4[7])return 2;
 if(format==GUID_WICPixelFormat4bppIndexed.Data4[7])return 4;
 if(format==GUID_WICPixelFormat8bppIndexed.Data4[7])return 8;
 return 0;
}

/// Formats this tier can write out of an RGBA image, with bytes per pixel.
u32 emit_bytes_per_pixel(u8 format) {
    switch (format) {
    case GUID_WICPixelFormat8bppGray.Data4[7]: return 1;
    case kPf24bppBGR: return 3;
    case kPf32bppBGR:
    case kPf32bppBGRA:
    case kPf32bppPBGRA:
    case kPf32bppRGBA: return 4;
    default: return 0;
    }
}

/// The frame's native pixel format for a BMP of the given depth
/// (the BMP codec reports the stored depth, not a converted one).
u8 native_format_for_depth(u16 bpp) {
    switch (bpp) {
    case 1: return kPf1bppIndexed;
    case 4: return kPf4bppIndexed;
    case 8: return kPf8bppIndexed;
    case 24: return kPf24bppBGR;
    default: return kPf32bppBGR;
    }
}

struct StreamLease {
    GuestMemory* memory{};
    ComCoreManager::GuestCall call;
    std::weak_ptr<void> lifetime;
    GuestAddr stream{};
    bool is64{};
    std::atomic_flag pixel_decode = ATOMIC_FLAG_INIT;
    ~StreamLease() {
        if (lifetime.expired() || !call) return;
        u64 table=0,fn=0;
        if(memory->read_bytes(stream,reinterpret_cast<Byte*>(&table),is64?8:4).is_err())return;
        const auto address=table+IStream_Release*(is64?8:4);
        if(memory->read_bytes(address,reinterpret_cast<Byte*>(&fn),is64?8:4).is_err()||!fn)return;
        const u64 args[]={stream};
        (void)call(fn,args,1,is64);
    }
};

struct Image {
    Guid container{GUID_ContainerFormatBmp};
    std::shared_ptr<StreamLease> stream;
    std::vector<u8> png_prefix;
    std::vector<u8> jpeg_encoded; // native decoder buffers compressed source at initialization
    u64 png_data_offset{0};
    u8 png_depth{0},png_color{0},png_interlace{0};
    std::vector<u32> palette;
    std::vector<u8> indices;
    std::vector<u8> index_rows;
    usize index_stride{0};
    double dpi_x{96},dpi_y{96};

    u32 width{0};
    u32 height{0};
    u16 bits_per_pixel{0};
    std::vector<u8> rgba; // top-down RGBA8, opaque
};

/// ClassFactory: the IClassFactory windowscodecs.dll!DllGetClassObject
/// hands out for the imaging factory coclasses.
enum class Kind { Factory, Decoder, Frame, Converter, ClassFactory, Palette };

struct Object {
    Kind kind{Kind::Factory};
    u32 refs{1};
    std::shared_ptr<Image> image; // decoder, frame, initialized converter
    u8 format{0};                        // pixel format the source reports
    bool initialized{false};             // converter: Initialize succeeded
    std::vector<u32> colors;
    u32 palette_type{WICBitmapPaletteTypeCustom};
};

} // namespace
// One object namespace per process/capsule; identical guest addresses in
// separate runtimes cannot alias. Weak leases expire before map teardown.
class WicContext {
public:
    std::mutex mutex;
    std::unordered_map<GuestAddr,Object> objects;
};
std::shared_ptr<WicContext> create_wic_context(){return std::make_shared<WicContext>();}
namespace {
WicContext& context(ComCoreManager& mgr){return *mgr.wic_context();}

std::optional<Object> find_object(ComCoreManager& mgr,GuestAddr self) {
    std::lock_guard lock(context(mgr).mutex);
    auto it = context(mgr).objects.find(self);
    if (it == context(mgr).objects.end()) return std::nullopt;
    return it->second;
}

// Method thunk names, in vtable order. The counts are the interface's slot
// counts: a shifted vtable calls the wrong method with the wrong pop count.
constexpr std::array<std::string_view, 29> kFactoryMethods{{
    "Vitreo_WIC_QueryInterface", "Vitreo_WIC_AddRef", "Vitreo_WIC_Release",
    "Vitreo_WIC_Factory_CreateDecoderFromFilename",
    "Vitreo_WIC_Factory_CreateDecoderFromStream",
    "Vitreo_WIC_Factory_CreateDecoderFromFileHandle",
    "Vitreo_WIC_Factory_CreateComponentInfo",
    "Vitreo_WIC_Factory_CreateDecoder",
    "Vitreo_WIC_Factory_CreateEncoder",
    "Vitreo_WIC_Factory_CreatePalette",
    "Vitreo_WIC_Factory_CreateFormatConverter",
    "Vitreo_WIC_Factory_CreateBitmapScaler",
    "Vitreo_WIC_Factory_CreateBitmapClipper",
    "Vitreo_WIC_Factory_CreateBitmapFlipRotator",
    "Vitreo_WIC_Factory_CreateStream",
    "Vitreo_WIC_Factory_CreateColorContext",
    "Vitreo_WIC_Factory_CreateColorTransformer",
    "Vitreo_WIC_Factory_CreateBitmap",
    "Vitreo_WIC_Factory_CreateBitmapFromSource",
    "Vitreo_WIC_Factory_CreateBitmapFromSourceRect",
    "Vitreo_WIC_Factory_CreateBitmapFromMemory",
    "Vitreo_WIC_Factory_CreateBitmapFromHBITMAP",
    "Vitreo_WIC_Factory_CreateBitmapFromHICON",
    "Vitreo_WIC_Factory_CreateComponentEnumerator",
    "Vitreo_WIC_Factory_CreateFastMetadataEncoderFromDecoder",
    "Vitreo_WIC_Factory_CreateFastMetadataEncoderFromFrameDecode",
    "Vitreo_WIC_Factory_CreateQueryWriter",
    "Vitreo_WIC_Factory_CreateQueryWriterFromReader",
    "Vitreo_WIC_Factory_CreateImageEncoder", // IWICImagingFactory2
}};

constexpr std::array<std::string_view, 14> kDecoderMethods{{
    "Vitreo_WIC_QueryInterface", "Vitreo_WIC_AddRef", "Vitreo_WIC_Release",
    "Vitreo_WIC_Decoder_QueryCapability",
    "Vitreo_WIC_Decoder_Initialize",
    "Vitreo_WIC_Decoder_GetContainerFormat",
    "Vitreo_WIC_Decoder_GetDecoderInfo",
    "Vitreo_WIC_Decoder_CopyPalette",
    "Vitreo_WIC_Decoder_GetMetadataQueryReader",
    "Vitreo_WIC_Decoder_GetPreview",
    "Vitreo_WIC_Decoder_GetColorContexts",
    "Vitreo_WIC_Decoder_GetThumbnail",
    "Vitreo_WIC_Decoder_GetFrameCount",
    "Vitreo_WIC_Decoder_GetFrame",
}};

constexpr std::array<std::string_view, 11> kFrameMethods{{
    "Vitreo_WIC_QueryInterface", "Vitreo_WIC_AddRef", "Vitreo_WIC_Release",
    "Vitreo_WIC_Source_GetSize",
    "Vitreo_WIC_Source_GetPixelFormat",
    "Vitreo_WIC_Source_GetResolution",
    "Vitreo_WIC_Source_CopyPalette",
    "Vitreo_WIC_Source_CopyPixels",
    "Vitreo_WIC_Frame_GetMetadataQueryReader",
    "Vitreo_WIC_Frame_GetColorContexts",
    "Vitreo_WIC_Frame_GetThumbnail",
}};

constexpr std::array<std::string_view, 10> kConverterMethods{{
    "Vitreo_WIC_QueryInterface", "Vitreo_WIC_AddRef", "Vitreo_WIC_Release",
    "Vitreo_WIC_Source_GetSize",
    "Vitreo_WIC_Source_GetPixelFormat",
    "Vitreo_WIC_Source_GetResolution",
    "Vitreo_WIC_Source_CopyPalette",
    "Vitreo_WIC_Source_CopyPixels",
    "Vitreo_WIC_Converter_Initialize",
    "Vitreo_WIC_Converter_CanConvert",
}};

constexpr std::array<std::string_view, 5> kClassFactoryMethods{{
    "Vitreo_WIC_QueryInterface", "Vitreo_WIC_AddRef", "Vitreo_WIC_Release",
    "Vitreo_WIC_ClassFactory_CreateInstance",
    "Vitreo_WIC_ClassFactory_LockServer",
}};

GuestAddr make_object(ComCoreManager& mgr, Marshaler& m,
                      std::span<const std::string_view> names, Object state) {
    const GuestAddr object = mgr.allocate_builtin_object(m, names);
    if (object == 0) return 0;
    std::lock_guard lock(context(mgr).mutex);
    context(mgr).objects[object] = std::move(state);
    return object;
}

bool write_guid(Marshaler& m, GuestAddr at, const Guid& g) {
    return m.memory().write_bytes(
        at, reinterpret_cast<const Byte*>(&g), sizeof(Guid)).is_ok();
}

bool write_f64(Marshaler& m, GuestAddr at, double v) {
    return m.memory().write_bytes(
        at, reinterpret_cast<const Byte*>(&v), sizeof(v)).is_ok();
}

struct WicRect {
    i32 x;
    i32 y;
    i32 width;
    i32 height;
};

/// IWICBitmapSource::CopyPixels: prc selects the rows and columns (NULL is
/// the whole bitmap), cbStride is the destination row pitch, and the buffer
/// must hold every selected row at that pitch — WIC reports a short buffer
/// as WINCODEC_ERR_INSUFFICIENTBUFFER rather than writing a partial image.
u32 copy_pixels(Marshaler& m, const Image& image, u8 format, GuestAddr rect_ptr,
                u32 stride, u32 buffer_size, GuestAddr buffer) {
    const u32 bytes_per_pixel = emit_bytes_per_pixel(format);
    const u32 packed_depth=indexed_bits(format);
    if (bytes_per_pixel == 0&&!packed_depth) return kWincodecUnsupportedOperation;
    if(packed_depth&&image.indices.empty())return kENotImpl;
    if (buffer == 0) return kEInvalidArg;

    WicRect rect{0, 0, static_cast<i32>(image.width),
                 static_cast<i32>(image.height)};
    if (rect_ptr != 0) {
        auto r = m.read_struct<WicRect>(rect_ptr);
        if (r.is_err()) return kEInvalidArg;
        rect = r.value();
    }
    if (rect.x < 0 || rect.y < 0 || rect.width <= 0 || rect.height <= 0 ||
        static_cast<u64>(rect.x) + static_cast<u64>(rect.width) > image.width ||
        static_cast<u64>(rect.y) + static_cast<u64>(rect.height) > image.height) {
        return kEInvalidArg;
    }
    const u64 row_bytes = packed_depth?(static_cast<u64>(rect.width)*packed_depth+7)/8:static_cast<u64>(rect.width) * bytes_per_pixel;
    if (stride < row_bytes) return kEInvalidArg;
    const u64 required =
        static_cast<u64>(stride) * (static_cast<u64>(rect.height) - 1) + row_bytes;
    if (buffer_size < required) return kWincodecInsufficientBuffer;

    std::vector<u8> row(static_cast<usize>(row_bytes));
    for (i32 y = 0; y < rect.height; ++y) {
        if(packed_depth){
            if(rect.x==0&&u32(rect.width)==image.width&&!image.index_rows.empty()){
                const auto* source=image.index_rows.data()+usize(rect.y+y)*image.index_stride;
                if(m.memory().write_bytes(buffer+u64(y)*stride,source,row.size()).is_err())return kEPointer;
                continue;
            }
            std::fill(row.begin(),row.end(),0);
            const u64 bits=u64(rect.width)*packed_depth;
            if(bits%8&&m.memory().read_bytes(buffer+u64(y)*stride+row_bytes-1,&row.back(),1).is_err())return kEPointer;
            for(i32 x=0;x<rect.width;++x){const u32 index=image.indices[(usize(rect.y+y)*image.width)+rect.x+x];const usize bit=usize(x)*packed_depth;const u32 shift=8-packed_depth-bit%8,mask=((1u<<packed_depth)-1)<<shift;row[bit/8]=static_cast<u8>((row[bit/8]&~mask)|((index<<shift)&mask));}
            if(m.memory().write_bytes(buffer+u64(y)*stride,row.data(),row.size()).is_err())return kEPointer;
            continue;
        }
        const u8* src = &image.rgba[
            (static_cast<usize>(rect.y + y) * image.width +
             static_cast<usize>(rect.x)) * 4];
        u8* dst = row.data();
        for (i32 x = 0; x < rect.width; ++x, src += 4, dst += bytes_per_pixel) {
            switch (format) {
            case GUID_WICPixelFormat8bppGray.Data4[7]:
                dst[0]=src[0];
                break;
            case kPf24bppBGR:
                dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0];
                break;
            case kPf32bppRGBA:
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3];
                break;
            case kPf32bppPBGRA:
                dst[0] = (u32(src[2])*src[3]+127)/255;
                dst[1] = (u32(src[1])*src[3]+127)/255;
                dst[2] = (u32(src[0])*src[3]+127)/255;
                dst[3] = src[3];
                break;
            default: // Straight BGRA/BGR output.
                dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
                break;
            }
        }
        if (m.memory().write_bytes(
                buffer + static_cast<GuestAddr>(y) * stride,
                reinterpret_cast<const Byte*>(row.data()), row.size()).is_err()) {
            return kEPointer;
        }
    }
    return kSOk;
}

u32 not_impl(Marshaler& m, usize argc) {
    for (usize i = 0; i < argc; ++i) (void)m.arg(i);
    return kENotImpl;
}

} // namespace

bool wic_is_imaging_factory_clsid(const Guid& clsid) {
    return same_guid(clsid, kClsidWicImagingFactory) ||
           same_guid(clsid, kClsidWicImagingFactory2);
}

u32 wic_co_create_instance(ComCoreManager& mgr, Marshaler& m, const Guid& iid,
                           GuestAddr outer, GuestAddr ppv) {
    // IClassFactory::CreateInstance: *ppvObject is NULL on every failure.
    if (ppv == 0) return kEPointer;
    (void)m.write_ptr(ppv, 0);
    if (outer != 0) return kClassENoAggregation;
    if (!same_guid(iid, kIidUnknown) && !same_guid(iid, kIidImagingFactory) &&
        !same_guid(iid, kIidImagingFactory2)) {
        return kENoInterface;
    }
    Object state;
    state.kind = Kind::Factory;
    const GuestAddr object = make_object(mgr, m, kFactoryMethods, state);
    if (object == 0) return kClassENotAvailable;
    if (m.write_ptr(ppv, object).is_err()) {
        std::lock_guard lock(context(mgr).mutex);
        context(mgr).objects.erase(object);
        return kEInvalidArg;
    }
    return kSOk;
}

// ---------------------------------------------------------------------------
// IUnknown, shared by every object kind
// ---------------------------------------------------------------------------

u32 wic_query_interface(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr riid = m.arg_ptr(1);
    const GuestAddr ppv = m.arg_ptr(2);
    if (ppv == 0) return kEPointer;
    (void)m.write_ptr(ppv, 0);
    auto iid = m.read_struct<Guid>(riid);
    if (iid.is_err()) return kEInvalidArg;

    std::lock_guard lock(context(mgr).mutex);
    auto it = context(mgr).objects.find(self);
    if (it == context(mgr).objects.end()) return kENoInterface;
    bool supported = same_guid(iid.value(), kIidUnknown);
    switch (it->second.kind) {
    case Kind::Factory:
        supported = supported || same_guid(iid.value(), kIidImagingFactory) ||
                    same_guid(iid.value(), kIidImagingFactory2);
        break;
    case Kind::Decoder:
        supported = supported || same_guid(iid.value(), kIidBitmapDecoder);
        break;
    case Kind::Frame:
        supported = supported || same_guid(iid.value(), kIidBitmapSource) ||
                    same_guid(iid.value(), kIidBitmapFrameDecode);
        break;
    case Kind::Converter:
        supported = supported || same_guid(iid.value(), kIidBitmapSource) ||
                    same_guid(iid.value(), kIidFormatConverter);
        break;
    case Kind::Palette:
        supported = supported || same_guid(iid.value(), kIidPalette);
        break;
    case Kind::ClassFactory:
        supported = supported || same_guid(iid.value(), kIidClassFactory);
        break;
    }
    if (!supported) return kENoInterface;
    ++it->second.refs;
    if (m.write_ptr(ppv, self).is_err()) {
        --it->second.refs;
        return kEPointer;
    }
    return kSOk;
}

u32 wic_add_ref(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    std::lock_guard lock(context(mgr).mutex);
    auto it = context(mgr).objects.find(self);
    if (it == context(mgr).objects.end()) return 0;
    return ++it->second.refs;
}

u32 wic_release(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self=m.arg_ptr(0);
    std::optional<Object> destroyed;
    u32 remaining=0;
    {
      std::lock_guard lock(context(mgr).mutex);
      auto it=context(mgr).objects.find(self);
      if(it==context(mgr).objects.end()) return 0;
      remaining=--it->second.refs;
      if(!remaining){destroyed=std::move(it->second);context(mgr).objects.erase(it);}
    }
    // The last decoder/frame/converter ref releases its stream outside the lock.
    destroyed.reset();
    return remaining;
}

// ---------------------------------------------------------------------------
// IWICImagingFactory
// ---------------------------------------------------------------------------

u32 wic_factory_create_decoder_from_filename(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT CreateDecoderFromFilename(LPCWSTR wzFilename,
    //     const GUID* pguidVendor, DWORD dwDesiredAccess,
    //     WICDecodeOptions metadataOptions, IWICBitmapDecoder** ppIDecoder)
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr filename_ptr = m.arg_ptr(1);
    (void)m.arg_ptr(2);
    (void)m.arg(3);
    (void)m.arg(4);
    const GuestAddr out = m.arg_ptr(5);
    if (out == 0) return kEPointer;
    (void)m.write_ptr(out, 0);
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::Factory) return kEInvalidArg;
    if (filename_ptr == 0) return kEInvalidArg;
    auto path = m.read_wide_string(filename_ptr);
    if (path.is_err() || path.value().empty()) return kEInvalidArg;

    capsule::Vfs* vfs = mgr.vfs();
    if (vfs == nullptr) return kHrFileNotFound;
    auto opened = vfs->open(path.value(), capsule::AccessMode::Read,
                            capsule::ShareMode::Read,
                            capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return kHrFileNotFound;
    std::vector<u8> file;
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto read = vfs->read(opened.value(), chunk);
        if (read.is_err() || read.value() == 0) break;
        file.insert(file.end(), chunk.begin(),
                    chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)vfs->close(opened.value());

    // Container detection is by signature, as WIC's codec selection is: a
    // file no installed codec claims is COMPONENTNOTFOUND, a BMP whose
    // header does not parse is BADHEADER.
    if (file.size() < 2 || file[0] != 'B' || file[1] != 'M') {
        return kWincodecComponentNotFound;
    }
    auto decoded = decode_bmp_file(std::span<const u8>(file.data(), file.size()));
    if (!decoded) return kWincodecBadHeader;

    auto image = std::make_shared<Image>();
    image->width = static_cast<u32>(decoded->width);
    image->height = static_cast<u32>(decoded->height);
    image->bits_per_pixel = decoded->bits_per_pixel;
    image->rgba = std::move(decoded->rgba);

    Object state;
    state.kind = Kind::Decoder;
    state.image = image;
    state.format = native_format_for_depth(image->bits_per_pixel);
    const GuestAddr decoder = make_object(mgr, m, kDecoderMethods, state);
    if (decoder == 0) return kEOutOfMemory;
    if (m.write_ptr(out, decoder).is_err()) {
        std::lock_guard lock(context(mgr).mutex);
        context(mgr).objects.erase(decoder);
        return kEPointer;
    }
    return kSOk;
}

namespace {
u32 wic_gap(std::string_view api,std::string_view text) {
    static std::mutex mutex;
    static std::unordered_map<std::string,bool> warned;
    std::lock_guard lock(mutex);
    const std::string key=std::string(api)+":"+std::string(text);
    if (!warned[key]) {
      warned[key]=true;
      std::fprintf(stderr,"[vitreo] UNIMPLEMENTED windowscodecs.dll!%.*s: %.*s\n",int(api.size()),api.data(),int(text.size()),text.data());
    }
    return kENotImpl;
}
u32 stream_gap(std::string_view text) {return wic_gap("IWICImagingFactory::CreateDecoderFromStream",text);}
struct WicGuestStream {
    ComCoreManager& manager; Marshaler& m; GuestAddr stream; GuestAddr scratch{};
    static constexpr usize chunk=0x10000;
    usize capacity,size;
    WicGuestStream(ComCoreManager& manager,Marshaler& m,GuestAddr stream,usize requested=chunk)
      :manager(manager),m(m),stream(stream),capacity(std::max(requested,chunk)),size((capacity/chunk+1)*chunk) {
        scratch=m.memory().map_first_fit(0x60000000,size,MemoryProtection::ReadWrite);
    }
    ~WicGuestStream(){if(scratch)(void)m.memory().unmap_region(scratch,size);}
    u32 seek(u64 move,u32 origin,u64* position=nullptr) {
      if(!scratch)return kEOutOfMemory;
      const u64 args64[]={stream,move,origin,scratch+capacity};
      const u64 args32[]={stream,static_cast<u32>(move),move>>32,origin,scratch+capacity};
      const auto hr=m.cpu_is_64bit()?manager.call_guest_slot(m,stream,IStream_Seek,args64,4)
                                   :manager.call_guest_slot(m,stream,IStream_Seek,args32,5);
      if(static_cast<i32>(hr)<0)return hr;
      if(position){const auto p=m.read_u64(scratch+capacity);if(p.is_err())return kEPointer;*position=p.value();}
      return kSOk;
    }
    u32 read(usize requested,std::vector<u8>& bytes) {
      if(!scratch)return kEOutOfMemory;
      requested=std::min(requested,capacity);
      (void)m.write_u32(scratch+capacity,0);
      const u64 args[]={stream,scratch,requested,scratch+capacity};
      const u32 hr=manager.call_guest_slot(m,stream,IStream_Read,args,4);
      if(static_cast<i32>(hr)<0)return hr;
      const auto count=m.read_u32(scratch+capacity);
      if(count.is_err()||count.value()>requested)return E_FAIL;
      bytes.resize(count.value());
      if(count.value()&&m.memory().read_bytes(scratch,bytes.data(),count.value()).is_err())return kEPointer;
      return kSOk;
    }
    u32 extent(u64& size) {
      if(!scratch)return kEOutOfMemory;
      const u64 args[]={stream,scratch,STATFLAG_NONAME};
      const u32 hr=manager.call_guest_slot(m,stream,IStream_Stat,args,3);
      if(static_cast<i32>(hr)<0)return hr;
      const auto value=m.read_u64(scratch+(m.cpu_is_64bit()?StatSizeOffset64:StatSizeOffset32));
      if(value.is_err())return kEPointer;size=value.value();return kSOk;
    }
    u32 all(std::vector<u8>& bytes) {
      u32 hr=seek(0,STREAM_SEEK_SET);if(hr)return hr;
      std::vector<u8> part;
      for(;;){hr=read(chunk,part);if(hr)return hr;bytes.insert(bytes.end(),part.begin(),part.end());if(part.size()<chunk)return kSOk;}
    }
};
u32 decode_png_pixels(ComCoreManager& mgr,Marshaler& m,const std::shared_ptr<Image>& image,Image& pixels) {
    // IWICBitmapDecoder::Initialize: encoded pixels are decoded each time
    // CopyPixels is called. No frontend mutex spans guest stream callbacks.
    // Full MTA transaction scheduling is not implemented. Reject overlap
    // without waiting: arbitrary guest callbacks can reenter this frontend.
    auto& claim=image->stream->pixel_decode;
    if(claim.test_and_set(std::memory_order_acquire))
      return wic_gap("IWICBitmapSource::CopyPixels","overlapping/reentrant pixel decoding on one stream");
    struct DecodeClaim {
      std::atomic_flag& claim;
      ~DecodeClaim(){claim.clear(std::memory_order_release);}
    } release_claim{claim};
    WicGuestStream input(mgr,m,image->stream->stream);
    // The decoder already consumed IHDR. Retain that native metadata;
    // subsequent CopyPixels reads the actual IDAT stream, not its IEND CRC.
    std::vector<u8> bytes=image->png_prefix;
    u32 hr=input.seek(image->png_data_offset,STREAM_SEEK_SET);if(hr)return hr;
    std::vector<u8> part;
    for(;;){
      hr=input.read(8,part);if(hr)return hr;
      if(part.size()!=8)return wic_gap("IWICBitmapSource::CopyPixels","truncated PNG chunk header");
      const u32 length=(u32(part[0])<<24)|(u32(part[1])<<16)|(u32(part[2])<<8)|part[3];
      const bool end=std::memcmp(part.data()+4,"IEND",4)==0;
      bytes.insert(bytes.end(),part.begin(),part.end());
      if(end)break; // Windows does not consume the IEND CRC.
      u64 remaining=u64(length)+4;
      while(remaining){
        const usize requested=static_cast<usize>(std::min<u64>(remaining,WicGuestStream::chunk));
        hr=input.read(requested,part);if(hr)return hr;
        if(part.size()!=requested)return wic_gap("IWICBitmapSource::CopyPixels","truncated PNG chunk payload");
        bytes.insert(bytes.end(),part.begin(),part.end());remaining-=part.size();
      }
    }
    if(image->png_color==3){
      std::vector<u8> compressed;
      for(usize at=8;at+8<=bytes.size();){const u32 length=(u32(bytes[at])<<24)|(u32(bytes[at+1])<<16)|(u32(bytes[at+2])<<8)|bytes[at+3];if(std::memcmp(bytes.data()+at+4,"IEND",4)==0)break;if(u64(at)+12+length>bytes.size())return wic_gap("IWICBitmapSource::CopyPixels","truncated indexed PNG chunk");if(std::memcmp(bytes.data()+at+4,"IDAT",4)==0)compressed.insert(compressed.end(),bytes.begin()+at+8,bytes.begin()+at+8+length);at+=12+length;}
      graphics::png_decode::Pixels decoded;
      if(!graphics::png_decode::samples({image->width,image->height,image->png_depth,3,image->png_interlace},compressed,decoded))return wic_gap("IWICBitmapSource::CopyPixels","invalid indexed PNG samples");
      pixels.width=image->width;pixels.height=image->height;pixels.palette=image->palette;pixels.index_stride=decoded.stride;pixels.index_rows=std::move(decoded.rows);pixels.indices.resize(usize(pixels.width)*pixels.height);pixels.rgba.resize(pixels.indices.size()*4);
      for(usize y=0;y<pixels.height;++y)for(usize x=0;x<pixels.width;++x){const usize bit=x*image->png_depth;const u32 index=(pixels.index_rows[y*pixels.index_stride+bit/8]>>(8-image->png_depth-bit%8))&((1u<<image->png_depth)-1);if(index>=image->palette.size())return wic_gap("IWICBitmapSource::CopyPixels","index outside PNG palette");const usize out=y*pixels.width+x;pixels.indices[out]=static_cast<u8>(index);const u32 color=image->palette[index];pixels.rgba[out*4]=color>>16;pixels.rgba[out*4+1]=color>>8;pixels.rgba[out*4+2]=color;pixels.rgba[out*4+3]=color>>24;}
      return kSOk;
    }
    i32 width=0,height=0;std::vector<u32> argb;
    const auto decoded=graphics::image_codec::decode(bytes,0,width,height,argb,graphics::image_codec::PixelPolicy::NativeBytes);
    if(decoded==graphics::image_codec::DecodeResult::UnsupportedLayout)return wic_gap("IWICBitmapSource::CopyPixels","ImageIO native-byte-preserving layout unavailable");
    if(decoded!=graphics::image_codec::DecodeResult::Success)return wic_gap("IWICBitmapSource::CopyPixels","PNG payload unsupported by native byte-preserving decoder");
    if(width!=image->width||height!=image->height)return WINCODEC_ERR_BADIMAGE;
    pixels.width=image->width;pixels.height=image->height;
    pixels.rgba.resize(argb.size()*4);
    for(usize i=0;i<argb.size();++i){const u32 px=argb[i];pixels.rgba[i*4]=px>>16;pixels.rgba[i*4+1]=px>>8;pixels.rgba[i*4+2]=px;pixels.rgba[i*4+3]=px>>24;}
    return kSOk;
}

u32 be32(std::span<const u8> bytes,usize at){return (u32(bytes[at])<<24)|(u32(bytes[at+1])<<16)|(u32(bytes[at+2])<<8)|bytes[at+3];}
bool is_png(std::span<const u8> bytes){return bytes.size()>=8&&std::memcmp(bytes.data(),"\211PNG\r\n\032\n",8)==0;}
bool is_bmp(std::span<const u8> bytes){return bytes.size()>=2&&bytes[0]=='B'&&bytes[1]=='M';}
}

u32 wic_factory_create_palette(ComCoreManager& mgr,Marshaler& m){
    const auto self=m.arg_ptr(0),out=m.arg_ptr(1);
    if(!out)return kEInvalidArg;
    auto factory=find_object(mgr,self);if(!factory||factory->kind!=Kind::Factory)return kEInvalidArg;
    if(m.write_ptr(out,0).is_err()){(void)m.raise_access_violation(true,out);return kEInvalidArg;}
    Object state;state.kind=Kind::Palette;
    const auto palette=make_object(mgr,m,kPaletteMethods,std::move(state));
    if(!palette)return kEOutOfMemory;
    if(m.write_ptr(out,palette).is_err()){std::lock_guard lock(context(mgr).mutex);context(mgr).objects.erase(palette);(void)m.raise_access_violation(true,out);return kEInvalidArg;}
    return kSOk;
}
u32 wic_palette_method(ComCoreManager& mgr,Marshaler& m,usize slot){
    const auto self=m.arg_ptr(0);auto palette=find_object(mgr,self);
    if(!palette||palette->kind!=Kind::Palette)return kEInvalidArg;
    const auto write=[&](GuestAddr at,u32 value){if(!at)return kEInvalidArg;if(m.write_u32(at,value).is_err()){(void)m.raise_access_violation(true,at);return kEInvalidArg;}return kSOk;};
    const auto update=[&](std::vector<u32> colors,u32 type){std::lock_guard lock(context(mgr).mutex);auto it=context(mgr).objects.find(self);if(it==context(mgr).objects.end())return kEInvalidArg;it->second.colors=std::move(colors);it->second.palette_type=type;return kSOk;};
    switch(slot){
    case palette_InitializeCustom:{const auto colors=m.arg_ptr(1);const u32 count=static_cast<u32>(m.arg(2));if(count>256||(!colors&&count))return kEInvalidArg;std::vector<u32> values(count);if(count&&m.memory().read_bytes(colors,reinterpret_cast<Byte*>(values.data()),count*sizeof(u32)).is_err()){(void)m.raise_access_violation(false,colors);return kEInvalidArg;}return update(std::move(values),WICBitmapPaletteTypeCustom);}
    case palette_InitializePredefined:{const auto type=static_cast<u32>(m.arg(1));const bool transparent=m.arg(2)!=0;for(const auto& entry:predefined_palettes)if(entry.type==type&&entry.transparent==transparent)return update(std::vector<u32>(entry.colors.begin(),entry.colors.begin()+entry.count),entry.type);return kEInvalidArg;}
    case palette_InitializeFromPalette:{const auto source=m.arg_ptr(1);if(!source)return kEInvalidArg;auto src=find_object(mgr,source);if(src&&src->kind==Kind::Palette)return update(src->colors,src->palette_type);return wic_gap("IWICPalette::InitializeFromPalette","external palette interface copying");}
    case palette_InitializeFromBitmap:return wic_gap("IWICPalette::InitializeFromBitmap","optimized palette generation");
    case palette_GetType:return write(m.arg_ptr(1),palette->palette_type);
    case palette_GetColorCount:return write(m.arg_ptr(1),static_cast<u32>(palette->colors.size()));
    case palette_GetColors:{const u32 requested=static_cast<u32>(m.arg(1));const auto colors=m.arg_ptr(2),actual=m.arg_ptr(3);if(!colors||!actual)return kEInvalidArg;const auto count=std::min<usize>(requested,palette->colors.size());if(count&&m.memory().write_bytes(colors,reinterpret_cast<const Byte*>(palette->colors.data()),count*sizeof(u32)).is_err()){(void)m.raise_access_violation(true,colors);return kEInvalidArg;}return write(actual,static_cast<u32>(count));}
    case palette_HasAlpha:return write(m.arg_ptr(1),std::any_of(palette->colors.begin(),palette->colors.end(),[](u32 color){return (color>>24)!=255;}));
    case palette_IsGrayscale:return write(m.arg_ptr(1),std::all_of(palette->colors.begin(),palette->colors.end(),[](u32 color){return (color>>24)==255&&((color>>16)&255)==((color>>8)&255)&&((color>>8)&255)==(color&255);}));
    case palette_IsBlackWhite:return write(m.arg_ptr(1),palette->colors.size()==2&&((palette->colors[0]==0xff000000&&palette->colors[1]==0xffffffff)||(palette->colors[1]==0xff000000&&palette->colors[0]==0xffffffff)));
    default:return wic_gap("IWICPalette","unknown method");
    }
}

namespace {u32 source_state(ComCoreManager&,GuestAddr,Object&);}
u32 wic_source_copy_palette(ComCoreManager& mgr,Marshaler& m){
    const auto self=m.arg_ptr(0),palette=m.arg_ptr(1);if(!palette)return kEInvalidArg;
    Object source;if(const auto hr=source_state(mgr,self,source);hr!=kSOk)return hr;
    if(!indexed_bits(source.format)||source.image->palette.empty())return kWincodecPaletteUnavailable;
    auto destination=find_object(mgr,palette);
    if(destination&&destination->kind==Kind::Palette){std::lock_guard lock(context(mgr).mutex);auto it=context(mgr).objects.find(palette);if(it==context(mgr).objects.end())return kEInvalidArg;it->second.colors=source.image->palette;it->second.palette_type=WICBitmapPaletteTypeCustom;return kSOk;}
    if(!mgr.guest_call())return wic_gap("IWICBitmapSource::CopyPalette","guest palette interface dispatcher unavailable");
    WicGuestStream scratch(mgr,m,palette);if(!scratch.scratch)return kEOutOfMemory;
    if(m.memory().write_bytes(scratch.scratch,reinterpret_cast<const u8*>(source.image->palette.data()),source.image->palette.size()*sizeof(u32)).is_err())return kEPointer;
    const u64 args[]={palette,scratch.scratch,source.image->palette.size()};return mgr.call_guest_slot(m,palette,palette_InitializeCustom,args,3);
}

u32 wic_factory_create_decoder_from_stream(ComCoreManager& mgr,Marshaler& m) {
    const GuestAddr self=m.arg_ptr(0),stream=m.arg_ptr(1),vendor=m.arg_ptr(2),out=m.arg_ptr(4);
    const u32 mode=static_cast<u32>(m.arg(3));
    // Windows leaves *out unchanged on every measured factory failure.
    if(!stream||!out)return kEInvalidArg;
    const auto factory=find_object(mgr,self);
    if(!factory||factory->kind!=Kind::Factory)return kEInvalidArg;
    if(vendor&&m.read_struct<Guid>(vendor).is_err()) { (void)m.raise_access_violation(false,vendor);return kEInvalidArg; }
    if(!mgr.guest_call())return stream_gap("guest IStream dispatcher unavailable");
    WicGuestStream input(mgr,m,stream);
    u64 initial=0;
    u32 hr=input.seek(0,STREAM_SEEK_CUR,&initial);if(hr)return hr;
    std::vector<u8> signature;
    hr=input.read(24,signature);if(hr)return hr;
    hr=input.seek(initial,STREAM_SEEK_SET);if(hr)return hr;
    // Arbitration examines the current prefix; EOF allows pattern probing
    // from the beginning (measured on Windows, independently of GDI+).
    const bool jpeg=signature.size()>=3&&signature[0]==0xff&&signature[1]==0xd8&&signature[2]==0xff;
    if(!signature.empty()&&!is_png(signature)&&!is_bmp(signature)&&!jpeg){
      if((signature.size()>=3&&signature[0]==0xff&&signature[1]==0xd8&&signature[2]==0xff)||
         (signature.size()>=6&&std::memcmp(signature.data(),"GIF8",4)==0)||
         (signature.size()>=4&&((signature[0]=='I'&&signature[1]=='I')||(signature[0]=='M'&&signature[1]=='M')))||
         (signature.size()>=4&&signature[0]==0&&signature[1]==0&&signature[2]==1&&signature[3]==0))
        return stream_gap("JPEG/GIF/TIFF/ICO native stream codec unsupported");
      return kWincodecComponentNotFound;
    }
    if(initial&&!signature.empty())return stream_gap("embedded nonzero-offset image arbitration");
    u64 stream_size=0;hr=input.extent(stream_size);if(hr)return hr;
    hr=input.seek(0,STREAM_SEEK_SET);if(hr)return hr;
    std::vector<u8> bytes;
    if(!jpeg){hr=input.read(8,bytes);if(hr)return hr;
      if(bytes.size()>=3&&bytes[0]==0xff&&bytes[1]==0xd8&&bytes[2]==0xff){(void)input.seek(initial,STREAM_SEEK_SET);return WINCODEC_ERR_BADHEADER;}
    }
    auto image=std::make_shared<Image>();
    u8 format=0;
    if(jpeg){
      if(stream_size>0xffffffffu)return stream_gap("JPEG compressed stream exceeds IStream ULONG read capacity");
      WicGuestStream compressed(mgr,m,stream,static_cast<usize>(stream_size));
      hr=compressed.seek(0,STREAM_SEEK_SET);if(hr)return hr;
      hr=compressed.read(static_cast<usize>(stream_size),image->jpeg_encoded);if(hr)return hr;
      if(image->jpeg_encoded.size()!=stream_size)return WINCODEC_ERR_BADIMAGE;
      graphics::image_codec::Info info;
      if(graphics::image_codec::info(image->jpeg_encoded,0,info)!=graphics::image_codec::DecodeResult::Success){(void)input.seek(initial,STREAM_SEEK_SET);return WINCODEC_ERR_BADHEADER;}
      if(info.depth!=8||(info.color!=graphics::image_codec::ColorModel::Rgb&&info.color!=graphics::image_codec::ColorModel::Gray))return stream_gap("JPEG native precision/CMYK color layout");
      image->container=GUID_ContainerFormatJpeg;image->width=info.width;image->height=info.height;
      if(info.has_dpi){image->dpi_x=info.dpi_x;image->dpi_y=info.dpi_y;}
      format=info.color==graphics::image_codec::ColorModel::Gray?GUID_WICPixelFormat8bppGray.Data4[7]:GUID_WICPixelFormat24bppBGR.Data4[7];
    }else if(is_png(bytes)){
      image->container=GUID_ContainerFormatPng;
      image->png_prefix=bytes;
      bool header=false,idat=false;
      u64 offset=8,first_idat=0,palette_offset=0;
      u32 palette_length=0;
      std::vector<u8> transparency;
      const auto unknown=[&](){(void)input.seek(initial,STREAM_SEEK_SET);return WINCODEC_ERR_UNKNOWNIMAGEFORMAT;};
      for(;;){
        hr=input.read(8,bytes);if(hr)return hr;
        if(bytes.size()!=8){
          if(idat){hr=input.seek(first_idat,STREAM_SEEK_SET);if(hr)return hr;break;}
          return unknown();
        }
        const u32 length=be32(bytes,0);
        const std::array<u8,4> kind{bytes[4],bytes[5],bytes[6],bytes[7]};
        const auto chunk_header=bytes;
        if(!header&&std::memcmp(kind.data(),"IHDR",4)!=0)return unknown();
        if(std::memcmp(kind.data(),"IDAT",4)==0&&!idat){
          if(offset+8>=stream_size)return unknown();
          if(image->png_color==3&&!palette_length)return unknown();
          idat=true;first_idat=offset+8;image->png_data_offset=offset;
          if(mode==WICDecodeMetadataCacheOnDemand||image->png_color==3)break;
        }
        if(offset+12+u64(length)>stream_size){
          if(idat){hr=input.seek(first_idat,STREAM_SEEK_SET);if(hr)return hr;break;}
          return unknown();
        }
        if(std::memcmp(kind.data(),"IEND",4)==0){if(!idat)return unknown();break;}
        if(std::memcmp(kind.data(),"IHDR",4)==0){
          if(header||length!=13)return WINCODEC_ERR_BADIMAGE;
          hr=input.read(17,bytes);if(hr)return hr;
          if(bytes.size()!=17)return unknown();
          image->png_prefix.insert(image->png_prefix.end(),chunk_header.begin(),chunk_header.end());
          image->png_prefix.insert(image->png_prefix.end(),bytes.begin(),bytes.end());
          const u8 depth=bytes[8],color=bytes[9];
          if(!((color==2||color==6)&&depth==8)&&!(color==3&&(depth==1||depth==2||depth==4||depth==8)))return stream_gap("PNG native gray/high-depth pixel layout");
          image->png_depth=depth;image->png_color=color;image->png_interlace=bytes[12];
          image->width=be32(bytes,0);image->height=be32(bytes,4);
          if(!image->width||!image->height)return WINCODEC_ERR_BADIMAGE;
          format=color==3?(depth==1?GUID_WICPixelFormat1bppIndexed:depth==2?GUID_WICPixelFormat2bppIndexed:depth==4?GUID_WICPixelFormat4bppIndexed:GUID_WICPixelFormat8bppIndexed).Data4[7]:color==6?GUID_WICPixelFormat32bppBGRA.Data4[7]:GUID_WICPixelFormat24bppBGR.Data4[7];
          header=true;
        }else if(std::memcmp(kind.data(),"PLTE",4)==0&&image->png_color==3){
          if(!length||length%3||length/3>(1u<<image->png_depth))return stream_gap("invalid indexed PNG palette length");
          palette_offset=offset;palette_length=length;
          hr=input.seek(offset+12+length,STREAM_SEEK_SET);if(hr)return hr;
        }else if(std::memcmp(kind.data(),"tRNS",4)==0){
          if(image->png_color!=3)return stream_gap("PNG color-key transparency native pixel selection");
          if(length>palette_length/3)return stream_gap("invalid indexed PNG transparency table");
          hr=input.read(length+4,bytes);if(hr)return hr;if(bytes.size()!=length+4)return unknown();transparency.assign(bytes.begin(),bytes.begin()+length);
        }else if(std::memcmp(kind.data(),"pHYs",4)==0&&length==9){
          hr=input.read(13,bytes);if(hr)return hr;if(bytes.size()!=13)return WINCODEC_ERR_BADIMAGE;
          if(bytes[8]==1){image->dpi_x=be32(bytes,0)*0.0254;image->dpi_y=be32(bytes,4)*0.0254;}
        }else{
          hr=input.seek(offset+12+length,STREAM_SEEK_SET);if(hr)return hr;
        }
        offset+=12+length;
      }
      if(image->png_color==3){
        // Native indexed initialization scans info first, then loads PLTE last.
        // This actual read naturally leaves OnDemand at the palette chunk end.
        hr=input.seek(palette_offset,STREAM_SEEK_SET);if(hr)return hr;
        hr=input.read(palette_length+12,bytes);if(hr)return hr;if(bytes.size()!=palette_length+12)return unknown();
        image->palette.resize(palette_length/3);
        for(usize i=0;i<image->palette.size();++i)image->palette[i]=(u32(i<transparency.size()?transparency[i]:255)<<24)|(u32(bytes[8+i*3])<<16)|(u32(bytes[9+i*3])<<8)|bytes[10+i*3];
        if(mode!=WICDecodeMetadataCacheOnDemand){
          // Native OnLoad enumerates metadata after palette initialization.
          // It reads metadata payloads, skips compressed IDAT/PLTE, and ends
          // at the IEND data boundary; no read-all/rewind cursor projection.
          hr=input.seek(8,STREAM_SEEK_SET);if(hr)return hr;u64 at=8;
          for(;;){
            hr=input.read(8,bytes);if(hr)return hr;if(bytes.size()!=8)return unknown();const u32 length=be32(bytes,0);
            if(std::memcmp(bytes.data()+4,"IEND",4)==0)break;
            const bool skip=std::memcmp(bytes.data()+4,"IDAT",4)==0||std::memcmp(bytes.data()+4,"PLTE",4)==0;
            if(u64(length)+12+at>stream_size)return stream_gap("truncated indexed PNG metadata scan");
            if(skip){hr=input.seek(at+12+length,STREAM_SEEK_SET);if(hr)return hr;}
            else{u64 remaining=u64(length)+4;while(remaining){const usize count=std::min<u64>(remaining,WicGuestStream::chunk);hr=input.read(count,bytes);if(hr)return hr;if(bytes.size()!=count)return unknown();remaining-=count;}}
            at+=12+length;
          }
        }
      }

    }else if(is_bmp(bytes)){
      return stream_gap("BMP stream decoder native format and cursor policy");
    }else{
      return kWincodecComponentNotFound;
    }
    // Retain one stream reference across decoder, frame and converter lifetime.
    const u64 args[]={stream};
    (void)mgr.call_guest_slot(m,stream,IStream_AddRef,args,1);
    auto lease=std::make_shared<StreamLease>();
    lease->memory=&m.memory();lease->call=mgr.guest_call();lease->lifetime=mgr.wic_lifetime();lease->stream=stream;lease->is64=m.cpu_is_64bit();
    image->stream=std::move(lease);
    Object state;state.kind=Kind::Decoder;state.image=image;state.format=format;
    const auto decoder=make_object(mgr,m,kDecoderMethods,std::move(state));
    if(!decoder)return kEOutOfMemory;
    if(m.write_ptr(out,decoder).is_err()){
      std::optional<Object> destroyed;
      {std::lock_guard lock(context(mgr).mutex);destroyed=std::move(context(mgr).objects.at(decoder));context(mgr).objects.erase(decoder);}
      (void)m.raise_access_violation(true,out);return kEPointer;
    }
    return kSOk;
}

u32 wic_factory_create_format_converter(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT CreateFormatConverter(IWICFormatConverter** ppIFormatConverter)
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kEPointer;
    (void)m.write_ptr(out, 0);
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::Factory) return kEInvalidArg;
    Object state;
    state.kind = Kind::Converter;
    const GuestAddr converter = make_object(mgr, m, kConverterMethods, state);
    if (converter == 0) return kEOutOfMemory;
    if (m.write_ptr(out, converter).is_err()) {
        std::lock_guard lock(context(mgr).mutex);
        context(mgr).objects.erase(converter);
        return kEPointer;
    }
    return kSOk;
}

// ---------------------------------------------------------------------------
// IWICBitmapDecoder
// ---------------------------------------------------------------------------

u32 wic_decoder_get_container_format(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kEPointer;
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::Decoder) return kEInvalidArg;
    return write_guid(m, out, object->image->container) ? kSOk : kEPointer;
}

u32 wic_decoder_get_frame_count(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kEPointer;
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::Decoder) return kEInvalidArg;
    // A BMP container holds exactly one frame.
    return m.write_u32(out, 1).is_ok() ? kSOk : kEPointer;
}

u32 wic_decoder_get_frame(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT GetFrame(UINT index, IWICBitmapFrameDecode** ppIBitmapFrame)
    const GuestAddr self = m.arg_ptr(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return kEPointer;
    (void)m.write_ptr(out, 0);
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::Decoder) return kEInvalidArg;
    if (index != 0) return kWincodecFrameMissing;
    Object state;
    state.kind = Kind::Frame;
    state.image = object->image;
    state.format = object->format;
    state.initialized = true;
    const GuestAddr frame = make_object(mgr, m, kFrameMethods, state);
    if (frame == 0) return kEOutOfMemory;
    if (m.write_ptr(out, frame).is_err()) {
        std::lock_guard lock(context(mgr).mutex);
        context(mgr).objects.erase(frame);
        return kEPointer;
    }
    return kSOk;
}

// ---------------------------------------------------------------------------
// IWICBitmapSource (frames and converters)
// ---------------------------------------------------------------------------

namespace {

/// A source with pixels: a decoded frame, or a converter after Initialize.
u32 source_state(ComCoreManager& mgr,GuestAddr self, Object& out) {
    auto object = find_object(mgr,self);
    if (!object) return kEInvalidArg;
    if (object->kind != Kind::Frame && object->kind != Kind::Converter) {
        return kEInvalidArg;
    }
    if (!object->initialized || !object->image) return kWincodecNotInitialized;
    out = *object;
    return kSOk;
}

} // namespace

u32 wic_source_get_size(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr width_out = m.arg_ptr(1);
    const GuestAddr height_out = m.arg_ptr(2);
    if (width_out == 0 || height_out == 0) return kEPointer;
    Object object;
    if (const u32 hr = source_state(mgr,self, object); hr != kSOk) return hr;
    if (m.write_u32(width_out, object.image->width).is_err() ||
        m.write_u32(height_out, object.image->height).is_err()) {
        return kEPointer;
    }
    return kSOk;
}

u32 wic_source_get_pixel_format(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) return kEPointer;
    Object object;
    if (const u32 hr = source_state(mgr,self, object); hr != kSOk) return hr;
    return write_guid(m, out, pixel_format_guid(object.format)) ? kSOk : kEPointer;
}

u32 wic_source_get_resolution(ComCoreManager& mgr, Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr x_out = m.arg_ptr(1);
    const GuestAddr y_out = m.arg_ptr(2);
    if (x_out == 0 || y_out == 0) return kEPointer;
    Object object;
    if (const u32 hr = source_state(mgr,self, object); hr != kSOk) return hr;
    // The BMP decoder reports the file's pels-per-meter; the decoder here
    // does not carry it, so the DIB default of 96 DPI is reported.
    if (!write_f64(m, x_out, object.image->dpi_x) || !write_f64(m, y_out, object.image->dpi_y)) return kEPointer;
    return kSOk;
}

u32 wic_source_copy_pixels(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT CopyPixels(const WICRect* prc, UINT cbStride, UINT cbBufferSize,
    //                    BYTE* pbBuffer)
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr rect = m.arg_ptr(1);
    const u32 stride = static_cast<u32>(m.arg(2));
    const u32 size = static_cast<u32>(m.arg(3));
    const GuestAddr buffer = m.arg_ptr(4);
    Object object;
    if (const u32 hr = source_state(mgr,self, object); hr != kSOk) return hr;
    if(!object.image->jpeg_encoded.empty()){
      Image pixels;i32 width=0,height=0;std::vector<u32> argb;
      const auto result=graphics::image_codec::decode(object.image->jpeg_encoded,0,width,height,argb,graphics::image_codec::PixelPolicy::NativeBytes);
      if(result!=graphics::image_codec::DecodeResult::Success)return wic_gap("IWICBitmapSource::CopyPixels","JPEG native ImageIO decode/layout unavailable");
      if(width!=object.image->width||height!=object.image->height)return WINCODEC_ERR_BADIMAGE;
      pixels.width=width;pixels.height=height;pixels.rgba.resize(argb.size()*4);
      for(usize i=0;i<argb.size();++i){pixels.rgba[i*4]=argb[i]>>16;pixels.rgba[i*4+1]=argb[i]>>8;pixels.rgba[i*4+2]=argb[i];pixels.rgba[i*4+3]=argb[i]>>24;}
      return copy_pixels(m,pixels,object.format,rect,stride,size,buffer);
    }
    if(object.image->stream){
      Image pixels;
      if(const auto hr=decode_png_pixels(mgr,m,object.image,pixels);hr!=kSOk)return hr;
      return copy_pixels(m,pixels,object.format,rect,stride,size,buffer);
    }
    return copy_pixels(m, *object.image, object.format, rect, stride, size, buffer);
}

// ---------------------------------------------------------------------------
// IWICFormatConverter
// ---------------------------------------------------------------------------

u32 wic_converter_initialize(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT Initialize(IWICBitmapSource* pISource, REFWICPixelFormatGUID
    //     dstFormat, WICBitmapDitherType dither, IWICPalette* pIPalette,
    //     double alphaThresholdPercent, WICBitmapPaletteType paletteTranslate)
    // Dithering, palettes and the alpha threshold only matter for indexed
    // or reduced-depth destinations, which this tier does not produce.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr source = m.arg_ptr(1);
    const GuestAddr format_ptr = m.arg_ptr(2);
    if (source == 0 || format_ptr == 0) return kEInvalidArg;
    auto format = m.read_struct<Guid>(format_ptr);
    if (format.is_err()) return kEInvalidArg;
    const auto index = pixel_format_index(format.value());
    if (!index || emit_bytes_per_pixel(*index) == 0) {
        return kWincodecUnsupportedPixelFormat;
    }
    Object src;
    if (const u32 hr = source_state(mgr,source, src); hr != kSOk) return hr;
    if(*index==GUID_WICPixelFormat8bppGray.Data4[7]&&src.format!=*index)return wic_gap("IWICFormatConverter::Initialize","color to grayscale conversion");

    std::lock_guard lock(context(mgr).mutex);
    auto it = context(mgr).objects.find(self);
    if (it == context(mgr).objects.end() || it->second.kind != Kind::Converter) {
        return kEInvalidArg;
    }
    if (it->second.initialized) return kWincodecWrongState;
    it->second.image = src.image;
    it->second.format = *index;
    it->second.initialized = true;
    return kSOk;
}

u32 wic_converter_can_convert(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT CanConvert(REFWICPixelFormatGUID src, REFWICPixelFormatGUID dst,
    //                    BOOL* pfCanConvert)
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr src_ptr = m.arg_ptr(1);
    const GuestAddr dst_ptr = m.arg_ptr(2);
    const GuestAddr out = m.arg_ptr(3);
    if (out == 0 || src_ptr == 0 || dst_ptr == 0) return kEPointer;
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::Converter) return kEInvalidArg;
    auto src = m.read_struct<Guid>(src_ptr);
    auto dst = m.read_struct<Guid>(dst_ptr);
    if (src.is_err() || dst.is_err()) return kEInvalidArg;
    const auto src_index = pixel_format_index(src.value());
    const auto dst_index = pixel_format_index(dst.value());
    const bool can = src_index.has_value() && dst_index.has_value() &&
                     emit_bytes_per_pixel(*dst_index) != 0 &&
                     (*dst_index!=GUID_WICPixelFormat8bppGray.Data4[7]||*src_index==*dst_index);
    return m.write_u32(out, can ? 1u : 0u).is_ok() ? kSOk : kEPointer;
}

// ---------------------------------------------------------------------------
// IClassFactory (windowscodecs.dll!DllGetClassObject)
// ---------------------------------------------------------------------------

u32 wic_dll_get_class_object(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
    const GuestAddr rclsid = m.arg_ptr(0);
    const GuestAddr riid = m.arg_ptr(1);
    const GuestAddr ppv = m.arg_ptr(2);
    if (ppv == 0) return kEInvalidArg;
    (void)m.write_ptr(ppv, 0);
    auto clsid = m.read_struct<Guid>(rclsid);
    auto iid = m.read_struct<Guid>(riid);
    if (rclsid == 0 || riid == 0 || clsid.is_err() || iid.is_err()) {
        return kEInvalidArg;
    }
    if (!wic_is_imaging_factory_clsid(clsid.value())) return kClassENotAvailable;
    if (!same_guid(iid.value(), kIidUnknown) &&
        !same_guid(iid.value(), kIidClassFactory)) {
        return kENoInterface;
    }
    Object state;
    state.kind = Kind::ClassFactory;
    const GuestAddr factory = make_object(mgr, m, kClassFactoryMethods, state);
    if (factory == 0) return kEOutOfMemory;
    if (m.write_ptr(ppv, factory).is_err()) {
        std::lock_guard lock(context(mgr).mutex);
        context(mgr).objects.erase(factory);
        return kEInvalidArg;
    }
    return kSOk;
}

u32 wic_class_factory_create_instance(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject)
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr outer = m.arg_ptr(1);
    const GuestAddr riid = m.arg_ptr(2);
    const GuestAddr ppv = m.arg_ptr(3);
    if (ppv == 0) return kEPointer;
    (void)m.write_ptr(ppv, 0);
    auto object = find_object(mgr,self);
    if (!object || object->kind != Kind::ClassFactory) return kEInvalidArg;
    auto iid = m.read_struct<Guid>(riid);
    if (riid == 0 || iid.is_err()) return kEInvalidArg;
    return wic_co_create_instance(mgr, m, iid.value(), outer, ppv);
}

u32 wic_class_factory_lock_server(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT LockServer(BOOL fLock): built-in modules are never unloaded,
    // so the lock count has no observable effect beyond success.
    (void)m.arg_ptr(0);
    (void)m.arg(1);
    return kSOk;
}

u32 wic_convert_bitmap_source(ComCoreManager& mgr, Marshaler& m) {
    // HRESULT WICConvertBitmapSource(REFWICPixelFormatGUID dstFormat,
    //     IWICBitmapSource* pISrc, IWICBitmapSource** ppIDst)
    // Learn: a source already in dstFormat is returned AddRef'd; otherwise
    // a dstFormat format converter is created and initialized with pISrc.
    const GuestAddr format_ptr = m.arg_ptr(0);
    const GuestAddr source = m.arg_ptr(1);
    const GuestAddr out = m.arg_ptr(2);
    if (out == 0) return kEInvalidArg;
    (void)m.write_ptr(out, 0);
    if (format_ptr == 0 || source == 0) return kEInvalidArg;
    auto format = m.read_struct<Guid>(format_ptr);
    if (format.is_err()) return kEInvalidArg;
    if (!find_object(mgr,source)) {
        // A guest-implemented IWICBitmapSource: the converter here reads
        // pixels only from Vitreo's own decoded frames.
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED windowscodecs!WICConvertBitmapSource: "
                "source is a caller-implemented IWICBitmapSource\n");
        });
        return kENotImpl;
    }
    Object src;
    if (const u32 hr = source_state(mgr,source, src); hr != kSOk) return hr;
    const auto index = pixel_format_index(format.value());
    if (index && *index == src.format) {
        std::lock_guard lock(context(mgr).mutex);
        auto it = context(mgr).objects.find(source);
        if (it == context(mgr).objects.end()) return kEInvalidArg;
        ++it->second.refs;
        if (m.write_ptr(out, source).is_err()) {
            --it->second.refs;
            return kEPointer;
        }
        return kSOk;
    }
    if (!index || emit_bytes_per_pixel(*index) == 0) {
        return kWincodecUnsupportedPixelFormat;
    }
    Object state;
    state.kind = Kind::Converter;
    state.image = src.image;
    state.format = *index;
    state.initialized = true;
    const GuestAddr converter = make_object(mgr, m, kConverterMethods, state);
    if (converter == 0) return kEOutOfMemory;
    if (m.write_ptr(out, converter).is_err()) {
        std::lock_guard lock(context(mgr).mutex);
        context(mgr).objects.erase(converter);
        return kEPointer;
    }
    return kSOk;
}

// ---------------------------------------------------------------------------
// Thunk table
// ---------------------------------------------------------------------------

void append_wic_thunks(DllExports& exports, ComCoreManager& mgr) {
    using Fn = u32 (*)(ComCoreManager&, Marshaler&);
    const auto add = [&](std::string_view name, u16 bytes, Fn fn) {
        exports.entries.push_back(ExportEntry{
            std::string(name), 0,
            [&mgr, fn](Marshaler& m) { m.set_return(fn(mgr, m)); }, bytes});
    };
    // Methods this tier does not model: E_NOTIMPL with the interface's real
    // stdcall frame size, so a 32-bit caller's stack is left balanced.
    const auto stub = [&](std::string_view name, u16 bytes) {
        const usize argc = bytes / 4u;
        exports.entries.push_back(ExportEntry{
            std::string(name), 0,
            [argc,name](Marshaler& m) { (void)not_impl(m,argc);m.set_return(wic_gap(name,"interface method unsupported")); }, bytes});
    };

    for(usize index=0;index<std::size(palette_methods);++index){
      const auto& method=palette_methods[index];
      exports.entries.push_back({method.name,0,[&mgr,index](Marshaler& m){m.set_return(wic_palette_method(mgr,m,index+3));},method.bytes});
    }
    add("Vitreo_WIC_QueryInterface", 12, wic_query_interface);
    add("Vitreo_WIC_AddRef", 4, wic_add_ref);
    add("Vitreo_WIC_Release", 4, wic_release);

    add("Vitreo_WIC_Factory_CreateDecoderFromFilename", 24,
        wic_factory_create_decoder_from_filename);
    add("Vitreo_WIC_Factory_CreateDecoderFromStream", CreateDecoderFromStreamStdcallBytes,
        wic_factory_create_decoder_from_stream);
    stub("Vitreo_WIC_Factory_CreateDecoderFromFileHandle", 20);
    stub("Vitreo_WIC_Factory_CreateComponentInfo", 12);
    stub("Vitreo_WIC_Factory_CreateDecoder", 16);
    stub("Vitreo_WIC_Factory_CreateEncoder", 16);
    add("Vitreo_WIC_Factory_CreatePalette", 8, wic_factory_create_palette);
    add("Vitreo_WIC_Factory_CreateFormatConverter", 8,
        wic_factory_create_format_converter);
    stub("Vitreo_WIC_Factory_CreateBitmapScaler", 8);
    stub("Vitreo_WIC_Factory_CreateBitmapClipper", 8);
    stub("Vitreo_WIC_Factory_CreateBitmapFlipRotator", 8);
    stub("Vitreo_WIC_Factory_CreateStream", 8);
    stub("Vitreo_WIC_Factory_CreateColorContext", 8);
    stub("Vitreo_WIC_Factory_CreateColorTransformer", 8);
    stub("Vitreo_WIC_Factory_CreateBitmap", 24);
    stub("Vitreo_WIC_Factory_CreateBitmapFromSource", 16);
    stub("Vitreo_WIC_Factory_CreateBitmapFromSourceRect", 24);
    stub("Vitreo_WIC_Factory_CreateBitmapFromMemory", 32);
    stub("Vitreo_WIC_Factory_CreateBitmapFromHBITMAP", 20);
    stub("Vitreo_WIC_Factory_CreateBitmapFromHICON", 12);
    stub("Vitreo_WIC_Factory_CreateComponentEnumerator", 16);
    stub("Vitreo_WIC_Factory_CreateFastMetadataEncoderFromDecoder", 12);
    stub("Vitreo_WIC_Factory_CreateFastMetadataEncoderFromFrameDecode", 12);
    stub("Vitreo_WIC_Factory_CreateQueryWriter", 16);
    stub("Vitreo_WIC_Factory_CreateQueryWriterFromReader", 16);
    stub("Vitreo_WIC_Factory_CreateImageEncoder", 12);

    stub("Vitreo_WIC_Decoder_QueryCapability", 12);
    // A decoder from CreateDecoderFromFilename is already initialized.
    exports.entries.push_back(ExportEntry{
        "Vitreo_WIC_Decoder_Initialize", 0,
        [](Marshaler& m) { (void)not_impl(m, 3); m.set_return(kWincodecWrongState); },
        12});
    add("Vitreo_WIC_Decoder_GetContainerFormat", 8,
        wic_decoder_get_container_format);
    stub("Vitreo_WIC_Decoder_GetDecoderInfo", 8);
    exports.entries.push_back(ExportEntry{
        "Vitreo_WIC_Decoder_CopyPalette", 0,
        [](Marshaler& m) { (void)not_impl(m, 2); m.set_return(kWincodecPaletteUnavailable); },
        8});
    stub("Vitreo_WIC_Decoder_GetMetadataQueryReader", 8);
    stub("Vitreo_WIC_Decoder_GetPreview", 8);
    stub("Vitreo_WIC_Decoder_GetColorContexts", 16);
    stub("Vitreo_WIC_Decoder_GetThumbnail", 8);
    add("Vitreo_WIC_Decoder_GetFrameCount", 8, wic_decoder_get_frame_count);
    add("Vitreo_WIC_Decoder_GetFrame", 12, wic_decoder_get_frame);

    add("Vitreo_WIC_Source_GetSize", 12, wic_source_get_size);
    add("Vitreo_WIC_Source_GetPixelFormat", 8, wic_source_get_pixel_format);
    add("Vitreo_WIC_Source_GetResolution", 12, wic_source_get_resolution);
    add("Vitreo_WIC_Source_CopyPalette",8,wic_source_copy_palette);
    add("Vitreo_WIC_Source_CopyPixels", 20, wic_source_copy_pixels);
    stub("Vitreo_WIC_Frame_GetMetadataQueryReader", 8);
    stub("Vitreo_WIC_Frame_GetColorContexts", 16);
    stub("Vitreo_WIC_Frame_GetThumbnail", 8);

    add("Vitreo_WIC_Converter_Initialize", 32, wic_converter_initialize);
    add("Vitreo_WIC_Converter_CanConvert", 16, wic_converter_can_convert);

    add("Vitreo_WIC_ClassFactory_CreateInstance", 16,
        wic_class_factory_create_instance);
    add("Vitreo_WIC_ClassFactory_LockServer", 8, wic_class_factory_lock_server);
}

} // namespace vitreo::winapi
