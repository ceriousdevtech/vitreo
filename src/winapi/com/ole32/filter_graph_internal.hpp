// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file filter_graph_internal.hpp
/// @brief File-local constants, GUIDs, thunk-name tables and vtable helpers shared by
/// the filter_graph*.cpp translation units. Everything sits in an anonymous
/// namespace: each including TU gets its own internal-linkage copy (same
/// pattern as com_wasapi_internal.hpp). Include from the filter_graph*.cpp
/// files only; never together with ole_guid_internal.hpp or
/// com_wasapi_internal.hpp, whose non-anonymous IID_IUNKNOWN / guid_equal
/// would be ambiguous with the anonymous copies here.

#include "filter_graph.hpp"
#include "audio/contracts/media_player.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace vitreo::winapi {

namespace {

/// Graph-lifetime diagnostics, sharing VITREO_MEDIA_DIAG with the decoder so
/// one flag shows the whole pipeline: which graph rendered which file, which
/// graphs were told to run, and when a graph (and its player) went away.
/// Without this, two graphs playing the same file are indistinguishable from
/// one graph playing twice.
[[nodiscard]] bool graph_diag_enabled() {
    static const bool enabled = std::getenv("VITREO_MEDIA_DIAG") != nullptr;
    return enabled;
}

[[maybe_unused]] void graph_diag(const char* format, ...) {
    if (!graph_diag_enabled()) return;
    std::fprintf(stderr, "[graph-diag] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}

// SDK winerror.h: HRESULT E_OUTOFMEMORY.
constexpr u32 E_OUTOFMEMORY = 0x8007000E;
constexpr u32 E_NOINTERFACE = 0x80004002;
constexpr u32 E_NOTIMPL = 0x80004001;
constexpr u32 E_POINTER = 0x80004003;
constexpr u32 E_ABORT = 0x80004004;
constexpr u32 E_FAIL = 0x80004005;
constexpr u32 VFW_S_STATE_INTERMEDIATE = 0x00040237u;
constexpr u32 VFW_E_CANNOT_RENDER = 0x80040218u;
constexpr u32 VFW_E_NOT_FOUND = 0x80040216u;
constexpr u32 VFW_E_WRONG_STATE = 0x80040227u;
// SDK vfwmsgs.h.
constexpr u32 VFW_S_DUPLICATE_NAME = 0x0004022Du;
constexpr u32 VFW_E_ENUM_OUT_OF_SYNC = 0x80040203u;
constexpr u32 VFW_E_NOT_CONNECTED = 0x80040209u;
constexpr u32 VFW_E_BUFFER_OVERFLOW = 0x8004020Du;
constexpr u32 VFW_E_NOT_COMMITTED = 0x80040211u;
constexpr u32 VFW_E_SAMPLE_TIME_NOT_SET = 0x80040249u;
constexpr u32 VFW_E_MEDIA_TIME_NOT_SET = 0x80040251u;
constexpr u32 VFW_S_CANT_CUE = 0x00040268u;
constexpr u32 VFW_S_NO_STOP_TIME = 0x00040270u;
constexpr u32 EC_COMPLETE = 0x01;
constexpr u32 EC_USERABORT = 0x02;
constexpr u32 EC_ERRORABORT = 0x03;
constexpr u32 AM_MEDIAEVENT_NONOTIFY = 0x01;
constexpr u32 kFallbackEventHandleBase = 0x00EC0000u;

constexpr Guid IID_IUNKNOWN{
    0x00000000u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr Guid CLSID_FILTERGRAPH{
    0xE436EBB3u, 0x524Fu, 0x11CEu,
    {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IGRAPHBUILDER{
    0x56A868A9u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IFILTERGRAPH{
    0x56A8689Fu, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IMEDIACONTROL{
    0x56A868B1u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IBASICAUDIO{
    0x56A868B3u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IMEDIAEVENT{
    0x56A868B6u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IMEDIAEVENTEX{
    0x56A868C0u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// strmif.h / DirectShow: IMediaSeeking (not the 56A868A6 quartz neighbor GUID).
constexpr Guid IID_IMEDIASEEKING{
    0x36B73880u, 0xC2C8u, 0x11CFu,
    {0x8B, 0x46, 0x00, 0x80, 0x5F, 0x6C, 0xEF, 0x60}};
// control.h / DirectShow: IVideoWindow (Filter Graph Manager forwards this).
constexpr Guid IID_IVIDEOWINDOW{
    0x56A868B4u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// control.h / DirectShow: IBasicVideo (Filter Graph Manager forwards this).
constexpr Guid IID_IBASICVIDEO{
    0x56A868B5u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// control.h / DirectShow: IMediaPosition (Filter Graph Manager exposes this).
constexpr Guid IID_IMEDIAPOSITION{
    0x56A868B2u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// strmif.h: IMediaEventSink — filters QI the graph for this during
// JoinFilterGraph.
constexpr Guid IID_IMEDIAEVENTSINK{
    0x56A868A2u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// strmif.h / uuids.h (printed by the winoracle probe, dshow_graph_misc).
constexpr Guid IID_IENUMFILTERS{
    0x56A86893u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
// strmif.h IAMFilterMiscFlags {2DD74950-A890-11D1-ABE8-00A0C905F375}.
constexpr Guid IID_IAMFILTERMISCFLAGS{
    0x2DD74950u, 0xA890u, 0x11D1u,
    {0xAB, 0xE8, 0x00, 0xA0, 0xC9, 0x05, 0xF3, 0x75}};
constexpr u32 AM_FILTER_MISC_FLAGS_IS_RENDERER = 0x1;
constexpr Guid IID_IPIN{
    0x56A86891u, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IMEMINPUTPIN{
    0x56A8689Du, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid IID_IMEDIASAMPLE{
    0x56A8689Au, 0x0AD4u, 0x11CEu,
    {0xB0, 0x3A, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid MEDIATYPE_VIDEO{
    0x73646976u, 0x0000u, 0x0010u,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
constexpr Guid MEDIASUBTYPE_RGB32{
    0xE436EB7Eu, 0x524Fu, 0x11CEu,
    {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid MEDIASUBTYPE_RGB24{
    0xE436EB7Du, 0x524Fu, 0x11CEu,
    {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}};
constexpr Guid MEDIASUBTYPE_YV12{
    0x32315659u, 0x0000u, 0x0010u,
    {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
// uuids.h TIME_FORMAT_MEDIA_TIME.
constexpr Guid TIME_FORMAT_MEDIA_TIME{
    0x7B785574u, 0x8C82u, 0x11CFu,
    {0xBC, 0x0C, 0x00, 0xAA, 0x00, 0xAC, 0x74, 0xF6}};
constexpr Guid FORMAT_VIDEOINFO{
    0x05589F80u, 0xC356u, 0x11CEu,
    {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A}};
constexpr u32 FOURCC_YV12 = 0x32315659u;
constexpr u32 VFW_E_TYPE_NOT_ACCEPTED = 0x8004022Au;
constexpr i32 OATRUE = -1;
constexpr u32 PINDIR_INPUT = 0;
constexpr u32 PINDIR_OUTPUT = 1;
// Layouts measured with MSVC against the Windows 11 SDK (10.0.26100)
// strmif.h / amvideo.h on winoracle, both bitnesses
// (tools/oracle_probe/dshow_layout_probe{32,64}.expected).
struct AmMediaTypeLayout {
    u32 size;
    u32 punk;       // IUnknown* pUnk
    u32 cb_format;  // ULONG cbFormat
    u32 pb_format;  // BYTE* pbFormat
};
constexpr AmMediaTypeLayout kAmMediaType32{72, 60, 64, 68};
constexpr AmMediaTypeLayout kAmMediaType64{88, 64, 72, 80};
// Fields before pUnk are identical in both: majortype 0, subtype 16,
// bFixedSizeSamples 32, bTemporalCompression 36, lSampleSize 40,
// formattype 44.
constexpr u32 AM_MEDIA_TYPE_MAX_BYTES = 88;
struct PinInfoLayout {
    u32 size;
    u32 dir;      // PIN_DIRECTION (4 bytes)
    u32 name;     // WCHAR achName[MAX_PIN_NAME = 128]
};
constexpr PinInfoLayout kPinInfo32{264, 4, 8};
constexpr PinInfoLayout kPinInfo64{272, 8, 12};
[[maybe_unused]] [[nodiscard]] constexpr const AmMediaTypeLayout& am_media_type_layout(
    bool x64) {
    return x64 ? kAmMediaType64 : kAmMediaType32;
}
[[maybe_unused]] [[nodiscard]] constexpr const PinInfoLayout& pin_info_layout(bool x64) {
    return x64 ? kPinInfo64 : kPinInfo32;
}
// VIDEOINFOHEADER is pointer-free: 88 bytes in both (rcSource 0,
// rcTarget 16, dwBitRate 32, dwBitErrorRate 36, AvgTimePerFrame 40,
// bmiHeader 48). BITMAPINFOHEADER: 40 bytes, biWidth 4, biHeight 8,
// biPlanes 12, biBitCount 14, biCompression 16, biSizeImage 20.
constexpr u32 VIDEOINFOHEADER_BYTES = 88;
constexpr u32 BITMAPINFOHEADER_BYTES = 40;
constexpr i32 OAFALSE = 0;

constexpr u32 WS_CHILD = 0x40000000u;

// SDK control.h IVideoWindow methods after IDispatch, in vtable order.
constexpr std::array<std::string_view, 39> kVideoWindowMethodNames{{
    "Vitreo_FilterGraph_VideoWindow_put_Caption",
    "Vitreo_FilterGraph_VideoWindow_get_Caption",
    "Vitreo_FilterGraph_VideoWindow_put_WindowStyle",
    "Vitreo_FilterGraph_VideoWindow_get_WindowStyle",
    "Vitreo_FilterGraph_VideoWindow_put_WindowStyleEx",
    "Vitreo_FilterGraph_VideoWindow_get_WindowStyleEx",
    "Vitreo_FilterGraph_VideoWindow_put_AutoShow",
    "Vitreo_FilterGraph_VideoWindow_get_AutoShow",
    "Vitreo_FilterGraph_VideoWindow_put_WindowState",
    "Vitreo_FilterGraph_VideoWindow_get_WindowState",
    "Vitreo_FilterGraph_VideoWindow_put_BackgroundPalette",
    "Vitreo_FilterGraph_VideoWindow_get_BackgroundPalette",
    "Vitreo_FilterGraph_VideoWindow_put_Visible",
    "Vitreo_FilterGraph_VideoWindow_get_Visible",
    "Vitreo_FilterGraph_VideoWindow_put_Left",
    "Vitreo_FilterGraph_VideoWindow_get_Left",
    "Vitreo_FilterGraph_VideoWindow_put_Width",
    "Vitreo_FilterGraph_VideoWindow_get_Width",
    "Vitreo_FilterGraph_VideoWindow_put_Top",
    "Vitreo_FilterGraph_VideoWindow_get_Top",
    "Vitreo_FilterGraph_VideoWindow_put_Height",
    "Vitreo_FilterGraph_VideoWindow_get_Height",
    "Vitreo_FilterGraph_VideoWindow_put_Owner",
    "Vitreo_FilterGraph_VideoWindow_get_Owner",
    "Vitreo_FilterGraph_VideoWindow_put_MessageDrain",
    "Vitreo_FilterGraph_VideoWindow_get_MessageDrain",
    "Vitreo_FilterGraph_VideoWindow_get_BorderColor",
    "Vitreo_FilterGraph_VideoWindow_put_BorderColor",
    "Vitreo_FilterGraph_VideoWindow_get_FullScreenMode",
    "Vitreo_FilterGraph_VideoWindow_put_FullScreenMode",
    "Vitreo_FilterGraph_VideoWindow_SetWindowForeground",
    "Vitreo_FilterGraph_VideoWindow_NotifyOwnerMessage",
    "Vitreo_FilterGraph_VideoWindow_SetWindowPosition",
    "Vitreo_FilterGraph_VideoWindow_GetWindowPosition",
    "Vitreo_FilterGraph_VideoWindow_GetMinIdealImageSize",
    "Vitreo_FilterGraph_VideoWindow_GetMaxIdealImageSize",
    "Vitreo_FilterGraph_VideoWindow_GetRestorePosition",
    "Vitreo_FilterGraph_VideoWindow_HideCursor",
    "Vitreo_FilterGraph_VideoWindow_IsCursorHidden",
}};

constexpr std::array<u16, 39> kVideoWindowStdcallBytes{{
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 20, 20, 20, 12, 12, 20, 8, 8,
}};

// SDK control.h IBasicVideo methods after IDispatch, in vtable order.
constexpr std::array<std::string_view, 32> kBasicVideoMethodNames{{
    "Vitreo_FilterGraph_BasicVideo_get_AvgTimePerFrame",
    "Vitreo_FilterGraph_BasicVideo_get_BitRate",
    "Vitreo_FilterGraph_BasicVideo_get_BitErrorRate",
    "Vitreo_FilterGraph_BasicVideo_get_VideoWidth",
    "Vitreo_FilterGraph_BasicVideo_get_VideoHeight",
    "Vitreo_FilterGraph_BasicVideo_put_SourceLeft",
    "Vitreo_FilterGraph_BasicVideo_get_SourceLeft",
    "Vitreo_FilterGraph_BasicVideo_put_SourceWidth",
    "Vitreo_FilterGraph_BasicVideo_get_SourceWidth",
    "Vitreo_FilterGraph_BasicVideo_put_SourceTop",
    "Vitreo_FilterGraph_BasicVideo_get_SourceTop",
    "Vitreo_FilterGraph_BasicVideo_put_SourceHeight",
    "Vitreo_FilterGraph_BasicVideo_get_SourceHeight",
    "Vitreo_FilterGraph_BasicVideo_put_DestinationLeft",
    "Vitreo_FilterGraph_BasicVideo_get_DestinationLeft",
    "Vitreo_FilterGraph_BasicVideo_put_DestinationWidth",
    "Vitreo_FilterGraph_BasicVideo_get_DestinationWidth",
    "Vitreo_FilterGraph_BasicVideo_put_DestinationTop",
    "Vitreo_FilterGraph_BasicVideo_get_DestinationTop",
    "Vitreo_FilterGraph_BasicVideo_put_DestinationHeight",
    "Vitreo_FilterGraph_BasicVideo_get_DestinationHeight",
    "Vitreo_FilterGraph_BasicVideo_SetSourcePosition",
    "Vitreo_FilterGraph_BasicVideo_GetSourcePosition",
    "Vitreo_FilterGraph_BasicVideo_SetDefaultSourcePosition",
    "Vitreo_FilterGraph_BasicVideo_SetDestinationPosition",
    "Vitreo_FilterGraph_BasicVideo_GetDestinationPosition",
    "Vitreo_FilterGraph_BasicVideo_SetDefaultDestinationPosition",
    "Vitreo_FilterGraph_BasicVideo_GetVideoSize",
    "Vitreo_FilterGraph_BasicVideo_GetVideoPaletteEntries",
    "Vitreo_FilterGraph_BasicVideo_GetCurrentImage",
    "Vitreo_FilterGraph_BasicVideo_IsUsingDefaultSource",
    "Vitreo_FilterGraph_BasicVideo_IsUsingDefaultDestination",
}};

constexpr std::array<u16, 32> kBasicVideoStdcallBytes{{
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    20, 20, 4, 20, 20, 4, 12, 20, 12, 4, 4,
}};

// SDK control.h IMediaPosition methods after IDispatch, in vtable order.
constexpr std::array<std::string_view, 11> kMediaPositionMethodNames{{
    "Vitreo_FilterGraph_MediaPosition_get_Duration",
    "Vitreo_FilterGraph_MediaPosition_put_CurrentPosition",
    "Vitreo_FilterGraph_MediaPosition_get_CurrentPosition",
    "Vitreo_FilterGraph_MediaPosition_get_StopTime",
    "Vitreo_FilterGraph_MediaPosition_put_StopTime",
    "Vitreo_FilterGraph_MediaPosition_get_PrerollTime",
    "Vitreo_FilterGraph_MediaPosition_put_PrerollTime",
    "Vitreo_FilterGraph_MediaPosition_put_Rate",
    "Vitreo_FilterGraph_MediaPosition_get_Rate",
    "Vitreo_FilterGraph_MediaPosition_CanSeekForward",
    "Vitreo_FilterGraph_MediaPosition_CanSeekBackward",
}};

constexpr std::array<u16, 11> kMediaPositionStdcallBytes{{
    8, 12, 8, 8, 12, 8, 12, 12, 8, 8, 8,
}};

// IPin methods after IUnknown, strmif.h vtable order.
constexpr std::array<std::string_view, 15> kOutputPinMethodNames{{
    "Vitreo_FilterGraph_OutputPin_Connect",
    "Vitreo_FilterGraph_OutputPin_ReceiveConnection",
    "Vitreo_FilterGraph_OutputPin_Disconnect",
    "Vitreo_FilterGraph_OutputPin_ConnectedTo",
    "Vitreo_FilterGraph_OutputPin_ConnectionMediaType",
    "Vitreo_FilterGraph_OutputPin_QueryPinInfo",
    "Vitreo_FilterGraph_OutputPin_QueryDirection",
    "Vitreo_FilterGraph_OutputPin_QueryId",
    "Vitreo_FilterGraph_OutputPin_QueryAccept",
    "Vitreo_FilterGraph_OutputPin_EnumMediaTypes",
    "Vitreo_FilterGraph_OutputPin_QueryInternalConnections",
    "Vitreo_FilterGraph_OutputPin_EndOfStream",
    "Vitreo_FilterGraph_OutputPin_BeginFlush",
    "Vitreo_FilterGraph_OutputPin_EndFlush",
    "Vitreo_FilterGraph_OutputPin_NewSegment",
}};
constexpr std::array<u16, 15> kOutputPinStdcallBytes{{
    // dshow_method_table.inc (IPin): QueryInternalConnections is 12.
    12, 12, 4, 8, 8, 8, 8, 8, 8, 8, 12, 4, 4, 4, 28,
}};

// IEnumFilters, strmif.h vtable order including IUnknown (7 slots).
constexpr std::array<std::string_view, 7> kEnumFiltersMethodNames{{
    "Vitreo_FilterGraph_EnumFilters_QueryInterface",
    "Vitreo_FilterGraph_EnumFilters_AddRef",
    "Vitreo_FilterGraph_EnumFilters_Release",
    "Vitreo_FilterGraph_EnumFilters_Next",
    "Vitreo_FilterGraph_EnumFilters_Skip",
    "Vitreo_FilterGraph_EnumFilters_Reset",
    "Vitreo_FilterGraph_EnumFilters_Clone",
}};
constexpr std::array<u16, 7> kEnumFiltersStdcallBytes{{12, 4, 4, 16, 8, 4, 8}};

// IMediaSample methods after IUnknown, strmif.h vtable order.
constexpr std::array<std::string_view, 16> kMediaSampleMethodNames{{
    "Vitreo_FilterGraph_MediaSample_GetPointer",
    "Vitreo_FilterGraph_MediaSample_GetSize",
    "Vitreo_FilterGraph_MediaSample_GetTime",
    "Vitreo_FilterGraph_MediaSample_SetTime",
    "Vitreo_FilterGraph_MediaSample_IsSyncPoint",
    "Vitreo_FilterGraph_MediaSample_SetSyncPoint",
    "Vitreo_FilterGraph_MediaSample_IsPreroll",
    "Vitreo_FilterGraph_MediaSample_SetPreroll",
    "Vitreo_FilterGraph_MediaSample_GetActualDataLength",
    "Vitreo_FilterGraph_MediaSample_SetActualDataLength",
    "Vitreo_FilterGraph_MediaSample_GetMediaType",
    "Vitreo_FilterGraph_MediaSample_SetMediaType",
    "Vitreo_FilterGraph_MediaSample_IsDiscontinuity",
    "Vitreo_FilterGraph_MediaSample_SetDiscontinuity",
    "Vitreo_FilterGraph_MediaSample_GetMediaTime",
    "Vitreo_FilterGraph_MediaSample_SetMediaTime",
}};
constexpr std::array<u16, 16> kMediaSampleStdcallBytes{{
    8, 4, 12, 12, 4, 8, 4, 8, 4, 8, 8, 8, 4, 8, 12, 12,
}};

[[maybe_unused]] [[nodiscard]] bool guid_equal(const Guid& a, const Guid& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::memcmp(a.Data4, b.Data4, sizeof(a.Data4)) == 0;
}

[[maybe_unused]] [[nodiscard]] VResult<GuestAddr> get_filter_graph_thunk(
    ThunkRegistry* thunks, std::string_view name) {
    return thunks->get_thunk_address("ole32.dll", name);
}

template<usize Count>
[[nodiscard]] bool load_filter_graph_thunks(
    ThunkRegistry* thunks,
    const std::array<std::string_view, Count>& names,
    std::array<GuestAddr, Count>& out) {
    if (thunks == nullptr) {
        return false;
    }
    for (usize i = 0; i < Count; ++i) {
        auto addr = get_filter_graph_thunk(thunks, names[i]);
        if (addr.is_err()) {
            return false;
        }
        out[i] = addr.value();
    }
    return true;
}

/// Guest pointer width: every interface pointer, vtable slot, out-pointer
/// and LONG_PTR the graph touches is this wide (8 on x64, 4 on x86).
[[maybe_unused]] [[nodiscard]] GuestAddr guest_pointer_size(const Marshaler& m) {
    return m.cpu_is_64bit() ? 8u : 4u;
}

/// Write `count` pointer-width vtable slots.
[[maybe_unused]] void write_vtable(Marshaler& m, GuestAddr vtable,
                                   const GuestAddr* methods, usize count) {
    const GuestAddr stride = guest_pointer_size(m);
    for (usize i = 0; i < count; ++i) {
        (void)m.write_ptr(vtable + static_cast<GuestAddr>(i) * stride,
                          methods[i]);
    }
}

/// `[vitreo] UNIMPLEMENTED quartz!<what>` once per call site.
#define VITREO_QUARTZ_UNIMPLEMENTED_ONCE(what)                               \
    do {                                                                     \
        static std::atomic<bool> vitreo_quartz_logged_{false};               \
        if (!vitreo_quartz_logged_.exchange(true)) {                         \
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED quartz!%s\n", what); \
        }                                                                    \
    } while (0)

} // namespace

} // namespace vitreo::winapi
