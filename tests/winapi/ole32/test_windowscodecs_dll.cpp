// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_windowscodecs_dll.cpp
/// @brief windowscodecs.dll module surface, x86 and x64: the export set of
/// mingw-w64's i686 libwindowscodecs.a (name and stdcall @N), the in-proc
/// server entry points (combaseapi.h DllGetClassObject / IClassFactory), the
/// *_Proxy vtable forwarders (Learn: "Proxy function for the <Method> method")
/// and the loud failures.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/com/ole32/com_core.hpp>
#include <winapi/com/ole32/wic.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

struct LibExport { const char* name; u16 bytes; };
// llvm-nm i686-w64-mingw32/lib/libwindowscodecs.a: every `_Name@N` text symbol.
const LibExport kImportLib[] = {
    {"IEnumString_Next_WIC_Proxy", 16},
    {"IEnumString_Reset_WIC_Proxy", 4},
    {"IPropertyBag2_Write_Proxy", 16},
    {"IWICBitmapClipper_Initialize_Proxy", 12},
    {"IWICBitmapCodecInfo_DoesSupportAnimation_Proxy", 8},
    {"IWICBitmapCodecInfo_DoesSupportLossless_Proxy", 8},
    {"IWICBitmapCodecInfo_DoesSupportMultiframe_Proxy", 8},
    {"IWICBitmapCodecInfo_GetContainerFormat_Proxy", 8},
    {"IWICBitmapCodecInfo_GetDeviceManufacturer_Proxy", 16},
    {"IWICBitmapCodecInfo_GetDeviceModels_Proxy", 16},
    {"IWICBitmapCodecInfo_GetFileExtensions_Proxy", 16},
    {"IWICBitmapCodecInfo_GetMimeTypes_Proxy", 16},
    {"IWICBitmapDecoder_CopyPalette_Proxy", 8},
    {"IWICBitmapDecoder_GetColorContexts_Proxy", 16},
    {"IWICBitmapDecoder_GetDecoderInfo_Proxy", 8},
    {"IWICBitmapDecoder_GetFrameCount_Proxy", 8},
    {"IWICBitmapDecoder_GetFrame_Proxy", 12},
    {"IWICBitmapDecoder_GetMetadataQueryReader_Proxy", 8},
    {"IWICBitmapDecoder_GetPreview_Proxy", 8},
    {"IWICBitmapDecoder_GetThumbnail_Proxy", 8},
    {"IWICBitmapEncoder_Commit_Proxy", 4},
    {"IWICBitmapEncoder_CreateNewFrame_Proxy", 12},
    {"IWICBitmapEncoder_GetEncoderInfo_Proxy", 8},
    {"IWICBitmapEncoder_GetMetadataQueryWriter_Proxy", 8},
    {"IWICBitmapEncoder_Initialize_Proxy", 12},
    {"IWICBitmapEncoder_SetPalette_Proxy", 8},
    {"IWICBitmapEncoder_SetThumbnail_Proxy", 8},
    {"IWICBitmapFlipRotator_Initialize_Proxy", 12},
    {"IWICBitmapFrameDecode_GetColorContexts_Proxy", 16},
    {"IWICBitmapFrameDecode_GetMetadataQueryReader_Proxy", 8},
    {"IWICBitmapFrameDecode_GetThumbnail_Proxy", 8},
    {"IWICBitmapFrameEncode_Commit_Proxy", 4},
    {"IWICBitmapFrameEncode_GetMetadataQueryWriter_Proxy", 8},
    {"IWICBitmapFrameEncode_Initialize_Proxy", 8},
    {"IWICBitmapFrameEncode_SetColorContexts_Proxy", 12},
    {"IWICBitmapFrameEncode_SetResolution_Proxy", 20},
    {"IWICBitmapFrameEncode_SetSize_Proxy", 12},
    {"IWICBitmapFrameEncode_SetThumbnail_Proxy", 8},
    {"IWICBitmapFrameEncode_WriteSource_Proxy", 12},
    {"IWICBitmapLock_GetDataPointer_STA_Proxy", 12},
    {"IWICBitmapLock_GetStride_Proxy", 8},
    {"IWICBitmapScaler_Initialize_Proxy", 20},
    {"IWICBitmapSource_CopyPalette_Proxy", 8},
    {"IWICBitmapSource_CopyPixels_Proxy", 20},
    {"IWICBitmapSource_GetPixelFormat_Proxy", 8},
    {"IWICBitmapSource_GetResolution_Proxy", 12},
    {"IWICBitmapSource_GetSize_Proxy", 12},
    {"IWICBitmap_Lock_Proxy", 16},
    {"IWICBitmap_SetPalette_Proxy", 8},
    {"IWICBitmap_SetResolution_Proxy", 20},
    {"IWICColorContext_InitializeFromMemory_Proxy", 12},
    {"IWICComponentFactory_CreateMetadataWriterFromReader_Proxy", 16},
    {"IWICComponentFactory_CreateQueryWriterFromBlockWriter_Proxy", 12},
    {"IWICComponentInfo_GetAuthor_Proxy", 16},
    {"IWICComponentInfo_GetCLSID_Proxy", 8},
    {"IWICComponentInfo_GetFriendlyName_Proxy", 16},
    {"IWICComponentInfo_GetSpecVersion_Proxy", 16},
    {"IWICComponentInfo_GetVersion_Proxy", 16},
    {"IWICFastMetadataEncoder_Commit_Proxy", 4},
    {"IWICFastMetadataEncoder_GetMetadataQueryWriter_Proxy", 8},
    {"IWICFormatConverter_Initialize_Proxy", 32},
    {"IWICImagingFactory_CreateBitmapClipper_Proxy", 8},
    {"IWICImagingFactory_CreateBitmapFlipRotator_Proxy", 8},
    {"IWICImagingFactory_CreateBitmapFromHBITMAP_Proxy", 20},
    {"IWICImagingFactory_CreateBitmapFromHICON_Proxy", 12},
    {"IWICImagingFactory_CreateBitmapFromMemory_Proxy", 32},
    {"IWICImagingFactory_CreateBitmapFromSource_Proxy", 16},
    {"IWICImagingFactory_CreateBitmapScaler_Proxy", 8},
    {"IWICImagingFactory_CreateBitmap_Proxy", 24},
    {"IWICImagingFactory_CreateComponentInfo_Proxy", 12},
    {"IWICImagingFactory_CreateDecoderFromFileHandle_Proxy", 20},
    {"IWICImagingFactory_CreateDecoderFromFilename_Proxy", 24},
    {"IWICImagingFactory_CreateDecoderFromStream_Proxy", 20},
    {"IWICImagingFactory_CreateEncoder_Proxy", 16},
    {"IWICImagingFactory_CreateFastMetadataEncoderFromDecoder_Proxy", 12},
    {"IWICImagingFactory_CreateFastMetadataEncoderFromFrameDecode_Proxy", 12},
    {"IWICImagingFactory_CreateFormatConverter_Proxy", 8},
    {"IWICImagingFactory_CreatePalette_Proxy", 8},
    {"IWICImagingFactory_CreateQueryWriterFromReader_Proxy", 16},
    {"IWICImagingFactory_CreateQueryWriter_Proxy", 16},
    {"IWICImagingFactory_CreateStream_Proxy", 8},
    {"IWICMetadataBlockReader_GetCount_Proxy", 8},
    {"IWICMetadataBlockReader_GetReaderByIndex_Proxy", 12},
    {"IWICMetadataQueryReader_GetContainerFormat_Proxy", 8},
    {"IWICMetadataQueryReader_GetEnumerator_Proxy", 8},
    {"IWICMetadataQueryReader_GetLocation_Proxy", 16},
    {"IWICMetadataQueryReader_GetMetadataByName_Proxy", 12},
    {"IWICMetadataQueryWriter_RemoveMetadataByName_Proxy", 8},
    {"IWICMetadataQueryWriter_SetMetadataByName_Proxy", 12},
    {"IWICPalette_GetColorCount_Proxy", 8},
    {"IWICPalette_GetColors_Proxy", 16},
    {"IWICPalette_GetType_Proxy", 8},
    {"IWICPalette_HasAlpha_Proxy", 8},
    {"IWICPalette_InitializeCustom_Proxy", 12},
    {"IWICPalette_InitializeFromBitmap_Proxy", 16},
    {"IWICPalette_InitializeFromPalette_Proxy", 8},
    {"IWICPalette_InitializePredefined_Proxy", 12},
    {"IWICPixelFormatInfo_GetBitsPerPixel_Proxy", 8},
    {"IWICPixelFormatInfo_GetChannelCount_Proxy", 8},
    {"IWICPixelFormatInfo_GetChannelMask_Proxy", 20},
    {"IWICStream_InitializeFromIStream_Proxy", 8},
    {"IWICStream_InitializeFromMemory_Proxy", 12},
    {"WICConvertBitmapSource", 12},
    {"WICCreateBitmapFromSection", 28},
    {"WICCreateBitmapFromSectionEx", 32},
    {"WICCreateColorContext_Proxy", 8},
    {"WICCreateImagingFactory_Proxy", 8},
    {"WICGetMetadataContentSize", 12},
    {"WICMapGuidToShortName", 16},
    {"WICMapSchemaToName", 20},
    {"WICMapShortNameToGuid", 8},
    {"WICMatchMetadataContent", 16},
    {"WICSerializeMetadataContent", 16},
    {"WICSetEncoderFormat_Proxy", 16},
};

constexpr Guid kClsidFactory{0xCACAF262u, 0x9370u, 0x4615u, {0xA1, 0x3B, 0x9F, 0x55, 0x39, 0xDA, 0x4C, 0x0A}};
constexpr Guid kClsidFactory2{0x317D06E8u, 0x5F24u, 0x433Du, {0xBD, 0xF7, 0x79, 0xCE, 0x68, 0xD8, 0xAB, 0xC2}};
constexpr Guid kClsidBmpDecoder{0x6B462062u, 0x7CBFu, 0x400Du, {0x9F, 0xDB, 0x81, 0x3D, 0xD1, 0x0F, 0x27, 0x78}};
constexpr Guid kIidUnknown{0, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr Guid kIidClassFactory{1, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr Guid kIidImagingFactory{0xEC5EC8A9u, 0xC395u, 0x4314u, {0x9C, 0x77, 0x54, 0xD7, 0xA9, 0x35, 0xFF, 0x70}};
constexpr Guid kIidBitmapSource{0x00000120u, 0xA8F2u, 0x4877u, {0xBA, 0x0A, 0xFD, 0x2B, 0x66, 0x45, 0xFB, 0x94}};

constexpr u32 S_OK_ = 0, S_FALSE_ = 1, E_NOTIMPL_ = 0x80004001u;
constexpr u32 E_NOINTERFACE_ = 0x80004002u, E_POINTER_ = 0x80004003u;
constexpr u32 E_INVALIDARG_ = 0x80070057u;
constexpr u32 CLASS_E_NOAGGREGATION_ = 0x80040110u;
constexpr u32 CLASS_E_CLASSNOTAVAILABLE_ = 0x80040111u;

} // namespace

class WindowsCodecsDll : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr DATA = 0x00100000;
    static constexpr GuestAddr STACK = 0x00200000;
    static constexpr GuestAddr OUT = DATA + 0x300;

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(DATA, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(ComCoreManager::COM_SCRATCH_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.mode = x64() ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        cpu_.gpr.rsp = STACK + 0x400;
        ASSERT_TRUE(registry_.register_dll("ole32.dll", create_com_core_thunks(mgr_)).is_ok());
        mgr_.set_thunk_registry(&registry_);
        exports_ = create_windowscodecs_thunks(mgr_);
    }
    [[nodiscard]] bool x64() const { return GetParam(); }

    const ExportEntry* find(const std::string& name) const {
        for (const auto& e : exports_.entries) if (e.name == name) return &e;
        return nullptr;
    }
    void put_ptr(GuestAddr at, u64 v) {
        if (x64()) (void)memory_.write_u64(at, v);
        else (void)memory_.write_u32(at, static_cast<u32>(v));
    }
    u64 get_ptr(GuestAddr at) {
        return x64() ? memory_.read_u64(at).value() : memory_.read_u32(at).value();
    }
    void put_guid(GuestAddr at, const Guid& g) {
        ASSERT_TRUE(memory_.write_bytes(at, reinterpret_cast<const Byte*>(&g), sizeof g).is_ok());
    }
    /// Lay out a call: Win64 registers + stack slots, or x86 stdcall stack.
    Marshaler frame(const std::vector<u64>& args) {
        for (usize i = 0; i < args.size(); ++i) {
            if (x64()) {
                switch (i) {
                case 0: cpu_.gpr.rcx = args[i]; break;
                case 1: cpu_.gpr.rdx = args[i]; break;
                case 2: cpu_.gpr.r8 = args[i]; break;
                case 3: cpu_.gpr.r9 = args[i]; break;
                default: (void)memory_.write_u64(cpu_.gpr.rsp + 0x28 + (i - 4) * 8, args[i]);
                }
            } else {
                (void)memory_.write_u32(cpu_.gpr.rsp + 4 + i * 4, static_cast<u32>(args[i]));
            }
        }
        Marshaler m{memory_};
        m.attach_cpu(cpu_);
        return m;
    }
    u32 invoke(const std::string& name, const std::vector<u64>& args) {
        const ExportEntry* e = find(name);
        EXPECT_NE(e, nullptr) << name;
        if (e == nullptr) return 0xFFFFFFFFu;
        auto m = frame(args);
        e->handler(m);
        return static_cast<u32>(x64() ? cpu_.gpr.rax : (cpu_.gpr.rax & 0xFFFFFFFFu));
    }

    GuestMemory memory_;
    cpu::CpuState cpu_;
    ComCoreManager mgr_;
    ThunkRegistry registry_{0x70000000};
    DllExports exports_;
};

INSTANTIATE_TEST_SUITE_P(Bitness, WindowsCodecsDll, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

TEST_P(WindowsCodecsDll, ExportsEveryImportLibFunctionWithItsStdcallFrame) {
    for (const auto& lib : kImportLib) {
        const ExportEntry* e = find(lib.name);
        ASSERT_NE(e, nullptr) << lib.name;
        EXPECT_EQ(e->stdcall_bytes, lib.bytes) << lib.name;
    }
    const ExportEntry* gco = find("DllGetClassObject");
    ASSERT_NE(gco, nullptr);
    EXPECT_EQ(gco->stdcall_bytes, 12u);
    ASSERT_NE(find("DllCanUnloadNow"), nullptr);
    EXPECT_EQ(exports_.entries.size(), std::size(kImportLib) + 2);
}

TEST_P(WindowsCodecsDll, DllGetClassObjectServesTheImagingFactoryClasses) {
    const GuestAddr clsid = DATA, iid = DATA + 0x20, iid2 = DATA + 0x40;
    put_guid(iid, kIidClassFactory);
    for (const Guid& c : {kClsidFactory, kClsidFactory2}) {
        put_guid(clsid, c);
        put_ptr(OUT, 0xBAD);
        ASSERT_EQ(invoke("DllGetClassObject", {clsid, iid, OUT}), S_OK_);
        const GuestAddr cf = get_ptr(OUT);
        ASSERT_NE(cf, 0u);
        // IClassFactory::CreateInstance(NULL, IID_IWICImagingFactory, &out)
        put_guid(iid2, kIidImagingFactory);
        put_ptr(OUT + 0x10, 0xBAD);
        auto m = frame({cf, 0, iid2, OUT + 0x10});
        ASSERT_EQ(wic_class_factory_create_instance(mgr_, m), S_OK_);
        EXPECT_NE(get_ptr(OUT + 0x10), 0u);
        // Aggregation is refused and *ppvObject is NULL.
        put_ptr(OUT + 0x10, 0xBAD);
        auto agg = frame({cf, 0x1234, iid2, OUT + 0x10});
        EXPECT_EQ(wic_class_factory_create_instance(mgr_, agg), CLASS_E_NOAGGREGATION_);
        EXPECT_EQ(get_ptr(OUT + 0x10), 0u);
        // An interface the factory object lacks.
        put_guid(iid2, kIidBitmapSource);
        put_ptr(OUT + 0x10, 0xBAD);
        auto bad = frame({cf, 0, iid2, OUT + 0x10});
        EXPECT_EQ(wic_class_factory_create_instance(mgr_, bad), E_NOINTERFACE_);
        EXPECT_EQ(get_ptr(OUT + 0x10), 0u);
        auto lock = frame({cf, 1});
        EXPECT_EQ(wic_class_factory_lock_server(mgr_, lock), S_OK_);
        // The class factory answers IUnknown and IClassFactory.
        put_guid(iid2, kIidUnknown);
        auto qi = frame({cf, iid2, OUT + 0x10});
        EXPECT_EQ(wic_query_interface(mgr_, qi), S_OK_);
        EXPECT_EQ(get_ptr(OUT + 0x10), cf);
    }
    // A CLSID this module does not serve: CLASS_E_CLASSNOTAVAILABLE, *ppv NULL.
    put_guid(clsid, kClsidBmpDecoder);
    put_ptr(OUT, 0xBAD);
    EXPECT_EQ(invoke("DllGetClassObject", {clsid, iid, OUT}), CLASS_E_CLASSNOTAVAILABLE_);
    EXPECT_EQ(get_ptr(OUT), 0u);
    // A served CLSID asked for an interface other than IUnknown/IClassFactory.
    put_guid(clsid, kClsidFactory);
    put_guid(iid2, kIidImagingFactory);
    put_ptr(OUT, 0xBAD);
    EXPECT_EQ(invoke("DllGetClassObject", {clsid, iid2, OUT}), E_NOINTERFACE_);
    EXPECT_EQ(get_ptr(OUT), 0u);
    EXPECT_EQ(invoke("DllGetClassObject", {clsid, iid, 0}), E_INVALIDARG_);
    EXPECT_EQ(invoke("DllCanUnloadNow", {}), S_FALSE_);
}

TEST_P(WindowsCodecsDll, CreateImagingFactoryProxyHandsOutTheFactory) {
    put_ptr(OUT, 0xBAD);
    ASSERT_EQ(invoke("WICCreateImagingFactory_Proxy", {0x0237u, OUT}), S_OK_);
    const GuestAddr factory = get_ptr(OUT);
    ASSERT_NE(factory, 0u);
    put_guid(DATA, kIidImagingFactory);
    auto qi = frame({factory, DATA, OUT + 0x10});
    EXPECT_EQ(wic_query_interface(mgr_, qi), S_OK_);
    EXPECT_EQ(invoke("WICCreateImagingFactory_Proxy", {0x0237u, 0}), E_INVALIDARG_);
}

TEST_P(WindowsCodecsDll, ProxiesCallTheMethodThroughTheObjectsVtable) {
    struct Seen { GuestAddr fn{0}; std::vector<u64> args; };
    Seen seen;
    mgr_.set_guest_call([&](GuestAddr fn, const u64* args, usize n, bool) -> u64 {
        seen.fn = fn;
        seen.args.assign(args, args + n);
        return 0x12345678u;
    });
    // A guest object: *obj = vtable, vtable[slot] = method address.
    const GuestAddr obj = DATA + 0x1000, vtbl = DATA + 0x1100;
    const u32 ptr = x64() ? 8u : 4u;
    put_ptr(obj, vtbl);
    for (u32 slot = 0; slot < 40; ++slot) put_ptr(vtbl + slot * ptr, 0x400000u + slot);

    // IWICBitmapSource::CopyPixels is slot 7 (wincodec.h); 5 parameters.
    EXPECT_EQ(invoke("IWICBitmapSource_CopyPixels_Proxy", {obj, 0x11, 0x22, 0x33, 0x44}),
              0x12345678u);
    EXPECT_EQ(seen.fn, 0x400007u);
    EXPECT_EQ(seen.args, (std::vector<u64>{obj, 0x11, 0x22, 0x33, 0x44}));

    // IWICBitmapLock::GetDataPointer (slot 5) behind the _STA_ spelling.
    EXPECT_EQ(invoke("IWICBitmapLock_GetDataPointer_STA_Proxy", {obj, 0x55, 0x66}), 0x12345678u);
    EXPECT_EQ(seen.fn, 0x400005u);
    // IEnumString::Next is slot 3; Reset is slot 5 (objidl.h).
    (void)invoke("IEnumString_Reset_WIC_Proxy", {obj});
    EXPECT_EQ(seen.fn, 0x400005u);
    EXPECT_EQ(seen.args.size(), 1u);
    (void)invoke("IEnumString_Next_WIC_Proxy", {obj, 1, 2, 3});
    EXPECT_EQ(seen.fn, 0x400003u);
    // WICCreateColorContext_Proxy(pFactory, pp) = IWICImagingFactory slot 15.
    (void)invoke("WICCreateColorContext_Proxy", {obj, 0x77});
    EXPECT_EQ(seen.fn, 0x40000Fu);
    EXPECT_EQ(seen.args, (std::vector<u64>{obj, 0x77}));

    if (!x64()) {
        // x86: SetResolution(this, double, double) is @20 -- both doubles
        // travel as their stack dwords.
        (void)invoke("IWICBitmap_SetResolution_Proxy", {obj, 1, 2, 3, 4});
        EXPECT_EQ(seen.fn, 0x40000Au);
        EXPECT_EQ(seen.args, (std::vector<u64>{obj, 1, 2, 3, 4}));
    }
    // A NULL THIS_PTR never reaches guest code.
    seen.fn = 0;
    EXPECT_EQ(invoke("IWICBitmapSource_GetSize_Proxy", {0, 1, 2}), E_POINTER_);
    EXPECT_EQ(seen.fn, 0u);
}

TEST_P(WindowsCodecsDll, UnimplementedFlatFunctionsFailWithENotImpl) {
    for (const char* name : {"WICSetEncoderFormat_Proxy", "WICCreateBitmapFromSection",
                             "WICCreateBitmapFromSectionEx", "WICMapGuidToShortName",
                             "WICMapShortNameToGuid", "WICMapSchemaToName",
                             "WICGetMetadataContentSize", "WICMatchMetadataContent",
                             "WICSerializeMetadataContent"}) {
        EXPECT_EQ(invoke(name, {0, 0, 0, 0, 0, 0, 0, 0}), E_NOTIMPL_) << name;
    }
}
