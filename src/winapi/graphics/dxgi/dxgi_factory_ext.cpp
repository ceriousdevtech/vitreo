// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_factory_ext.cpp
/// @brief IDXGIFactory3 through IDXGIFactory7 methods.
///        Split out of dxgi.cpp as verbatim code motion.
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/graphics/dxgi/dxgi_internal.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/guest_com.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/common/virtual_display_modes.hpp"
#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/common/live_diag.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <thread>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

/// DXGI_FEATURE.
constexpr u32 DXGI_FEATURE_PRESENT_ALLOW_TEARING = 0;

} // namespace

u32 DxgiManager::write_single_adapter(Marshaler& m, GuestAddr factory,
                                     GuestAddr adapter_out,
                                     const char* caller, GuestAddr* created) {
    if (created != nullptr) *created = 0;
    if (m_adapter_info.description.empty()) {
        dxgi_diag("%s -> DXGI_ERROR_NOT_FOUND (no host Metal adapter)", caller);
        return dxgi::ERROR_NOT_FOUND;
    }
    const GuestAddr adapter = create_adapter(factory);
    if (adapter == 0 || m.write_ptr(adapter_out, adapter).is_err()) {
        if (adapter != 0) (void)release_object(adapter);
        return dxgi::E_POINTER;
    }
    if (created != nullptr) *created = adapter;
    return dxgi::S_OK;
}

void DxgiManager::impl_GetCreationFlags(Marshaler& m) {
    // IDXGIFactory3::GetCreationFlags returns UINT, not an HRESULT: the
    // CreateDXGIFactory2 flags the factory was made with (measured).
    const GuestAddr self = m.arg_ptr(0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(0);
        return;
    }
    m.set_return(factory->second.creation_flags);
}

void DxgiManager::impl_EnumAdapterByLuid(Marshaler& m) {
    // IDXGIFactory4::EnumAdapterByLuid(LUID AdapterLuid, REFIID riid,
    //                                  void **ppvAdapter).
    // A LUID is 8 bytes by value: one argument slot on x64, two on x86.
    const GuestAddr self = m.arg_ptr(0);
    usize next = 1;
    u32 low = 0;
    u32 high = 0;
    if (m_is_64bit) {
        const u64 luid = m.arg(1);
        low = static_cast<u32>(luid & 0xFFFFFFFFull);
        high = static_cast<u32>(luid >> 32);
        next = 2;
    } else {
        low = static_cast<u32>(m.arg(1));
        high = static_cast<u32>(m.arg(2));
        next = 3;
    }
    const GuestAddr iid_address = m.arg_ptr(next);
    const GuestAddr adapter_out = m.arg_ptr(next + 1);
    if (adapter_out == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(adapter_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (!iid_is_supported(iid_address, ObjectType::Adapter)) {
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }
    if (low != ADAPTER_LUID_LOW || high != ADAPTER_LUID_HIGH) {
        dxgi_diag("EnumAdapterByLuid(%08x:%08x) -> DXGI_ERROR_NOT_FOUND",
                  high, low);
        m.set_return(dxgi::ERROR_NOT_FOUND);
        return;
    }
    m.set_return(write_single_adapter(m, self, adapter_out,
                                     "EnumAdapterByLuid"));
}

void DxgiManager::impl_EnumWarpAdapter(Marshaler& m) {
    // IDXGIFactory4::EnumWarpAdapter(REFIID riid, void **ppvAdapter).
    //
    // WARP is Microsoft's software rasterizer. There is none here, and
    // CreateSoftwareAdapter already refuses for the same reason, so reporting
    // NOT_FOUND keeps the two consistent. Handing back the hardware adapter
    // instead would have a guest that deliberately asked for a software device
    // -- usually to work around a GPU bug, or to render deterministically --
    // silently get the GPU.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr adapter_out = m.arg_ptr(2);
    if (adapter_out != 0) (void)m.write_ptr(adapter_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    dxgi_diag("EnumWarpAdapter -> DXGI_ERROR_NOT_FOUND (no software adapter)");
    m.set_return(dxgi::ERROR_NOT_FOUND);
}

void DxgiManager::impl_FactoryCheckFeatureSupport(Marshaler& m) {
    // IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE Feature,
    //                                    void *pFeatureSupportData,
    //                                    UINT FeatureSupportDataSize).
    const GuestAddr self = m.arg_ptr(0);
    const u32 feature = static_cast<u32>(m.arg(1));
    const GuestAddr data = m.arg_ptr(2);
    const u32 size = static_cast<u32>(m.arg(3));
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (feature != DXGI_FEATURE_PRESENT_ALLOW_TEARING) {
        // Measured on Windows 2026-09-22: an unrecognised DXGI_FEATURE is
        // DXGI_ERROR_INVALID_CALL, not DXGI_ERROR_UNSUPPORTED. Learn documents
        // neither, and the guess was wrong.
        dxgi_diag("IDXGIFactory5::CheckFeatureSupport feature=%u -> "
                  "DXGI_ERROR_INVALID_CALL", feature);
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // A wrong size is INVALID_CALL (measured, sizes 0 and 8). Windows
    // dereferences a NULL buffer; that is refused here instead.
    if (size != sizeof(u32)) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (data == 0) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    // DXGI_FEATURE_PRESENT_ALLOW_TEARING: a BOOL. Vitreo's presenter is always
    // synchronised to the display, so tearing is not available -- and answering
    // FALSE definitively is the point of the method. A guest that is told TRUE
    // passes DXGI_PRESENT_ALLOW_TEARING and a 0 sync interval, and a presenter
    // that then blocks on vblank gives it a frame pace it did not ask for.
    if (m.write_u32(data, 0).is_err()) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    dxgi_diag("IDXGIFactory5::CheckFeatureSupport ALLOW_TEARING -> FALSE");
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_EnumAdapterByGpuPreference(Marshaler& m) {
    // IDXGIFactory6::EnumAdapterByGpuPreference(UINT Adapter,
    //   DXGI_GPU_PREFERENCE GpuPreference, REFIID riid, void **ppvAdapter).
    const GuestAddr self = m.arg_ptr(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const u32 preference = static_cast<u32>(m.arg(2));
    const GuestAddr iid_address = m.arg_ptr(3);
    const GuestAddr adapter_out = m.arg_ptr(4);
    if (adapter_out == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(adapter_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (!iid_is_supported(iid_address, ObjectType::Adapter)) {
        m.set_return(dxgi::E_NOINTERFACE);
        return;
    }
    // Learn: the enumeration is ordered by the requested preference. With one
    // adapter every ordering is the same one-element list, so the preference is
    // read and ignored rather than rejected -- an engine asking for
    // HIGH_PERFORMANCE must get the GPU, not an error.
    if (index != 0) {
        dxgi_diag("EnumAdapterByGpuPreference(%u, pref=%u) -> "
                  "DXGI_ERROR_NOT_FOUND", index, preference);
        m.set_return(dxgi::ERROR_NOT_FOUND);
        return;
    }
    m.set_return(write_single_adapter(m, self, adapter_out,
                                     "EnumAdapterByGpuPreference"));
}

void DxgiManager::impl_RegisterAdaptersChangedEvent(Marshaler& m) {
    // IDXGIFactory7::RegisterAdaptersChangedEvent(HANDLE hEvent,
    //                                             DWORD *pdwCookie).
    //
    // The registration succeeds and the event never fires, which is correct
    // rather than a stub: this reports *adapter* arrival and removal, and the
    // host exposes exactly one Metal adapter for the life of the process, so
    // there is no change to report. Refusing instead would make a guest that
    // relies on the notification to finish initialising wait for an error it
    // cannot act on.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr event = m.arg_ptr(1);
    const GuestAddr cookie_out = m.arg_ptr(2);
    // A refused registration leaves *pdwCookie untouched (measured).
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || event == 0 ||
        cookie_out == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    const u32 cookie = m_next_adapters_changed_cookie++;
    if (m.write_u32(cookie_out, cookie).is_err()) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    m_adapters_changed_cookies.insert(cookie);
    dxgi_diag("RegisterAdaptersChangedEvent -> cookie %u (the adapter set never "
              "changes, so the event never fires)", cookie);
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_UnregisterAdaptersChangedEvent(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 cookie = static_cast<u32>(m.arg(1));
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // A cookie this factory never issued (or one already unregistered) is
    // DXGI_ERROR_NOT_FOUND (measured).
    if (m_adapters_changed_cookies.erase(cookie) == 0) {
        m.set_return(dxgi::ERROR_NOT_FOUND);
        return;
    }
    m.set_return(dxgi::S_OK);
}

} // namespace vitreo::winapi
