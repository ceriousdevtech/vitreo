// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_factory.cpp
/// @brief IDXGIFactory, IDXGIFactory1 and IDXGIFactory2 methods.
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

void DxgiManager::enum_adapter(Marshaler& m, bool version1) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr adapter_out = m.arg_ptr(2);
    if (adapter_out == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(adapter_out, 0);
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (index != 0) {
        dxgi_diag("EnumAdapters%s(%u) -> DXGI_ERROR_NOT_FOUND",
                  version1 ? "1" : "", index);
        m.set_return(dxgi::ERROR_NOT_FOUND);
        return;
    }
    GuestAddr adapter = 0;
    const u32 result = write_single_adapter(
        m, self, adapter_out,
        version1 ? "EnumAdapters1" : "EnumAdapters", &adapter);
    if (result != dxgi::S_OK) {
        m.set_return(result);
        return;
    }
    VITREO_TRACE("[dxgi] EnumAdapters%s(0) -> 0x%llx \"%s\"\n",
                 version1 ? "1" : "",
                 static_cast<unsigned long long>(adapter),
                 m_adapter_info.description.c_str());
    dxgi_diag("EnumAdapters%s(0) -> 0x%llx \"%s\"",
              version1 ? "1" : "",
              static_cast<unsigned long long>(adapter),
              m_adapter_info.description.c_str());
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_EnumAdapters(Marshaler& m) {
    enum_adapter(m, false);
}

void DxgiManager::impl_EnumAdapters1(Marshaler& m) {
    enum_adapter(m, true);
}

void DxgiManager::impl_MakeWindowAssociation(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr window = m.arg_ptr(1);
    const u32 flags = static_cast<u32>(m.arg(2));
    // DXGI_MWA_NO_WINDOW_CHANGES | NO_ALT_ENTER | NO_PRINT_SCREEN are the
    // only flags (measured: 0x8 and 0x100 are INVALID_CALL; a NULL or
    // non-window HWND is accepted with any valid flags).
    constexpr u32 kMwaValid = 0x1u | 0x2u | 0x4u;
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory ||
        (flags & ~kMwaValid) != 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // The association only steers DXGI's own Alt+Enter / Print Screen
    // handling of the window's message queue. Vitreo never switches a chain
    // to exclusive full-screen (SetFullscreenState reports
    // NOT_CURRENTLY_AVAILABLE), so there is no transition to perform or
    // suppress.
    factory->second.window_association = window;
    factory->second.window_association_flags = flags;
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_GetWindowAssociation(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr window_out = m.arg_ptr(1);
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory ||
        window_out == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    // Measured on Windows 11: S_OK and a NULL HWND even right after a
    // MakeWindowAssociation (Learn documents this method as not reliable).
    if (m.write_ptr(window_out, 0).is_err()) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_CreateSwapChain(Marshaler& m) {
    const GuestAddr factory = m.arg_ptr(0);
    const auto object = m_objects.find(factory);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Factory) {
        const GuestAddr swap_chain_out = m.arg_ptr(3);
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (!m_swap_chain_creator) {
        const GuestAddr swap_chain_out = m.arg_ptr(3);
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        dxgi_diag("IDXGIFactory::CreateSwapChain -> E_NOTIMPL "
                  "(no graphics frontend registered)");
        m.set_return(dxgi::E_NOTIMPL);
        return;
    }
    m_swap_chain_creator(m);
}

void DxgiManager::impl_CreateSoftwareAdapter(Marshaler& m) {
    (void)m.arg_ptr(0);
    (void)m.arg_ptr(1);
    const GuestAddr adapter_out = m.arg_ptr(2);
    if (adapter_out == 0) {
        dxgi_diag("IDXGIFactory::CreateSoftwareAdapter -> "
                  "DXGI_ERROR_INVALID_CALL");
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(adapter_out, 0);
    // A software adapter is a DLL implementing the whole user-mode driver
    // interface (Learn); Vitreo hosts no such DDI, so no module can back an
    // adapter. Windows answers S_OK even for a non-rasterizer module
    // (measured) and fails later; the refusal is DXGI's "unsupported".
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED dxgi!IDXGIFactory::"
                     "CreateSoftwareAdapter: software rasterizer DLLs -> "
                     "DXGI_ERROR_UNSUPPORTED\n");
    }
    m.set_return(dxgi::ERROR_UNSUPPORTED);
}

void DxgiManager::impl_IsCurrent(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const auto factory = m_objects.find(self);
    const bool current =
        factory != m_objects.end() &&
        factory->second.type == ObjectType::Factory;
    dxgi_diag("IDXGIFactory1::IsCurrent -> %s",
              current ? "TRUE" : "FALSE");
    m.set_return(current ? 1u : 0u);
}

void DxgiManager::impl_IsWindowedStereoEnabled(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory) {
        m.set_return(0);
        return;
    }
    // Learn: nonzero if stereo is enabled for the adapter. Vitreo does not
    // expose stereo output, so report FALSE.
    dxgi_diag("IDXGIFactory2::IsWindowedStereoEnabled -> FALSE");
    m.set_return(0);
}

void DxgiManager::impl_CreateSwapChainForHwnd(Marshaler& m) {
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr swap_chain_out = m.arg_ptr(6);
    const auto object = m_objects.find(factory);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Factory) {
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (!m_swap_chain_for_hwnd_creator) {
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        dxgi_diag("IDXGIFactory2::CreateSwapChainForHwnd -> E_NOTIMPL "
                  "(no graphics frontend registered)");
        m.set_return(dxgi::E_NOTIMPL);
        return;
    }
    m_swap_chain_for_hwnd_creator(m);
}

void DxgiManager::impl_CreateSwapChainForComposition(Marshaler& m) {
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr swap_chain_out = m.arg_ptr(4);
    const auto object = m_objects.find(factory);
    if (object == m_objects.end() ||
        object->second.type != ObjectType::Factory) {
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (!m_swap_chain_for_composition_creator) {
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        dxgi_diag("IDXGIFactory2::CreateSwapChainForComposition -> "
                  "E_NOTIMPL (no graphics frontend registered)");
        m.set_return(dxgi::E_NOTIMPL);
        return;
    }
    m_swap_chain_for_composition_creator(m);
}

void DxgiManager::impl_RegisterStatus(Marshaler& m, bool stereo,
                                      bool window) {
    // Register{Stereo,Occlusion}StatusWindow(HWND, UINT wMsg, DWORD*) and
    // ...Event(HANDLE, DWORD*). A NULL window, a zero message or a NULL event
    // is INVALID_CALL with the cookie untouched (measured). The registration
    // is real but never fires: Vitreo has no stereo display, and its Present
    // never reports occlusion, so neither status ever changes.
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr target = m.arg_ptr(1);
    const u32 message = window ? static_cast<u32>(m.arg(2)) : 1u;
    const GuestAddr cookie_out = m.arg_ptr(window ? 3 : 2);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || target == 0 ||
        message == 0 || cookie_out == 0) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    const u32 cookie = m_next_status_cookie++;
    if (m.write_u32(cookie_out, cookie).is_err()) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    m_status_cookies[cookie] = stereo;
    dxgi_diag("IDXGIFactory2::Register%sStatus%s -> cookie %u",
              stereo ? "Stereo" : "Occlusion", window ? "Window" : "Event",
              cookie);
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_UnregisterStatus(Marshaler& m, bool stereo) {
    // void Unregister{Stereo,Occlusion}Status(DWORD dwCookie).
    const u32 cookie = static_cast<u32>(m.arg(1));
    const auto found = m_status_cookies.find(cookie);
    if (found != m_status_cookies.end() && found->second == stereo) {
        m_status_cookies.erase(found);
    }
    m.set_return(0);
}

void DxgiManager::impl_GetSharedResourceAdapterLuid(Marshaler& m) {
    // GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid): the LUID
    // of the adapter a shared resource was created on (measured: the
    // device's adapter). There is one adapter.
    const u64 handle = m.arg(1);
    const GuestAddr luid_out = m.arg_ptr(2);
    if (handle == 0 || luid_out == 0 || !m_shared_resource_query ||
        !m_shared_resource_query(handle)) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (m.write_u32(luid_out, ADAPTER_LUID_LOW).is_err() ||
        m.write_u32(luid_out + 4, ADAPTER_LUID_HIGH).is_err()) {
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    m.set_return(dxgi::S_OK);
}

void DxgiManager::impl_Factory2Unsupported(Marshaler& m, const char* method,
                                           usize out_index, u32 out_bytes) {
    const GuestAddr self = m.arg_ptr(0);
    (void)self;
    // A COM out-parameter must be written even when the call fails. Five of
    // these methods have one -- an IDXGISwapChain1**, a LUID*, or a DWORD
    // cookie -- and leaving it untouched hands the guest whatever was on its
    // stack: a swap-chain pointer it may Release, or a cookie it may later pass
    // to UnregisterOcclusionStatus. Index 0 is `this`, so it means "no
    // out-parameter".
    if (out_index != 0 && out_bytes != 0) {
        const GuestAddr out = m.arg_ptr(out_index);
        if (out != 0) {
            // POINTER_OUT means one guest pointer, whose width only the manager
            // knows.
            const u32 width = out_bytes == POINTER_OUT
                                  ? (m_is_64bit ? 8u : 4u)
                                  : out_bytes;
            const std::vector<u8> zeros(width, 0u);
            (void)m_memory.write_bytes(out, zeros.data(), zeros.size());
        }
    }
    dxgi_diag("IDXGIFactory2::%s -> DXGI_ERROR_INVALID_CALL "
              "(not implemented)",
              method);
    m.set_return(dxgi::ERROR_INVALID_CALL);
}

} // namespace vitreo::winapi
