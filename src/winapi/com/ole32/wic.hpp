#pragma once
/// @file wic.hpp
/// @brief Windows Imaging Component in-proc classes (windowscodecs.dll) served
/// by CoCreateInstance: CLSID_WICImagingFactory and the decoder, frame and
/// format-converter objects it hands out.
///
/// Contract: https://learn.microsoft.com/en-us/windows/win32/api/wincodec/

#include "winapi/com/ole32/com_core.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// True for CLSID_WICImagingFactory (v1) and CLSID_WICImagingFactory2.
[[nodiscard]] bool wic_is_imaging_factory_clsid(const Guid& clsid);

/// CoCreateInstance path for the imaging factory.
[[nodiscard]] u32 wic_co_create_instance(
    ComCoreManager& mgr, Marshaler& m, const Guid& iid, GuestAddr outer,
    GuestAddr ppv);

// Method entry points. Every one takes the object as argument 0 (`this`).
// They are exported through ole32's thunk table as Vitreo_WIC_*; the names
// here exist so tests can drive the objects without the registry.
[[nodiscard]] u32 wic_query_interface(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_add_ref(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_release(ComCoreManager& mgr, Marshaler& m);

[[nodiscard]] u32 wic_factory_create_decoder_from_filename(
    ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_factory_create_decoder_from_stream(
    ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_factory_create_palette(ComCoreManager&,Marshaler&);
[[nodiscard]] u32 wic_palette_method(ComCoreManager&,Marshaler&,usize slot);
[[nodiscard]] u32 wic_factory_create_format_converter(
    ComCoreManager& mgr, Marshaler& m);

[[nodiscard]] u32 wic_decoder_get_container_format(
    ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_decoder_get_frame_count(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_decoder_get_frame(ComCoreManager& mgr, Marshaler& m);

[[nodiscard]] u32 wic_source_get_size(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_source_get_pixel_format(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_source_get_resolution(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_source_copy_palette(ComCoreManager&,Marshaler&);
[[nodiscard]] u32 wic_source_copy_pixels(ComCoreManager& mgr, Marshaler& m);

[[nodiscard]] u32 wic_converter_initialize(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_converter_can_convert(ComCoreManager& mgr, Marshaler& m);

/// windowscodecs.dll!DllGetClassObject: an IClassFactory for the imaging
/// factory coclasses, CLASS_E_CLASSNOTAVAILABLE for any other CLSID.
[[nodiscard]] u32 wic_dll_get_class_object(ComCoreManager& mgr, Marshaler& m);
[[nodiscard]] u32 wic_class_factory_create_instance(ComCoreManager& mgr,
                                                    Marshaler& m);
[[nodiscard]] u32 wic_class_factory_lock_server(ComCoreManager& mgr,
                                                Marshaler& m);
/// windowscodecs.dll!WICConvertBitmapSource.
[[nodiscard]] u32 wic_convert_bitmap_source(ComCoreManager& mgr, Marshaler& m);

void append_wic_thunks(DllExports& exports, ComCoreManager& mgr);

/// The windowscodecs.dll export table (wincodec.h / libwindowscodecs.a):
/// DllGetClassObject, DllCanUnloadNow, the flat WIC functions and every
/// *_Proxy vtable forwarder.
[[nodiscard]] DllExports create_windowscodecs_thunks(ComCoreManager& mgr);

} // namespace vitreo::winapi
