// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_create_device.cpp
/// @brief D3D10CreateDevice(1) and D3D10CreateDeviceAndSwapChain(1): the
///        parameter rules and the D3D11 device a D3D10 device is. Split
///        out of d3d10_device.cpp as verbatim code motion.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d10/d3d10_device_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 kDxgiErrorSdkComponentMissing = 0x887A002Du;
/// HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND).
constexpr u32 kHresultProcNotFound = 0x8007007Fu;

/// The D3D10_CREATE_DEVICE_FLAG bits (d3d10.h) that ask for an SDK layer --
/// DEBUG, SWITCH_TO_REF, DEBUGGABLE -- and fail without one
/// (DXGI_ERROR_SDK_COMPONENT_MISSING, measured). Every other bit is
/// accepted; GetCreationFlags keeps SINGLETHREADED, PREVENT_INTERNAL_
/// THREADING_OPTIMIZATIONS and PREVENT_ALTERING_LAYER_SETTINGS_FROM_REGISTRY,
/// and BGRA_SUPPORT from D3D10CreateDevice1 only (measured, each bit alone).
constexpr u32 kD3D10LayerFlags = 0x2u | 0x4u | 0x400u;
constexpr u32 kD3D10KeptFlags = 0x1u | 0x8u | 0x80u;
constexpr u32 kD3D10BgraSupport = 0x20u;

} // namespace

// -----------------------------------------------------------------------------
// D3D10CreateDevice(1) / D3D10CreateDeviceAndSwapChain(1)
// -----------------------------------------------------------------------------

void D3D11Manager::impl_D3D10CreateDevice(Marshaler& m) {
    d3d10_create_device(m, false, false);
}

void D3D11Manager::impl_D3D10CreateDevice1(Marshaler& m) {
    d3d10_create_device(m, true, false);
}

void D3D11Manager::impl_D3D10CreateDeviceAndSwapChain(Marshaler& m) {
    d3d10_create_device(m, false, true);
}

void D3D11Manager::impl_D3D10CreateDeviceAndSwapChain1(Marshaler& m) {
    d3d10_create_device(m, true, true);
}

void D3D11Manager::d3d10_create_device(Marshaler& m, bool api_10_1,
                                       bool with_swap_chain) {
    // D3D10CreateDevice(pAdapter, DriverType, Software, Flags, SDKVersion,
    // ppDevice); D3D10CreateDevice1 adds HardwareLevel before SDKVersion;
    // the AndSwapChain variants add pSwapChainDesc and ppSwapChain before
    // ppDevice.
    const char* entry =
        api_10_1 ? (with_swap_chain ? "D3D10CreateDeviceAndSwapChain1"
                                    : "D3D10CreateDevice1")
                 : (with_swap_chain ? "D3D10CreateDeviceAndSwapChain"
                                    : "D3D10CreateDevice");
    const GuestAddr adapter_in = m.arg_ptr(0);
    const u32 driver = m.arg32(1);
    const GuestAddr software = m.arg_ptr(2);
    const u32 flags = m.arg32(3);
    usize next = 4;
    const u32 level =
        api_10_1 ? m.arg32(next++) : d3d10::FEATURE_LEVEL_10_0;
    const u32 sdk = m.arg32(next++);
    const GuestAddr desc_address = with_swap_chain ? m.arg_ptr(next++) : 0;
    const GuestAddr swap_chain_out = with_swap_chain ? m.arg_ptr(next++) : 0;
    const GuestAddr device_out = m.arg_ptr(next);
    if (device_out != 0) (void)m.write_ptr(device_out, 0);
    if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
    const auto fail = [&](u32 hr, const char* why) {
        d3d11_diag("%s -> 0x%08x (%s)", entry, hr, why);
        m.set_return(hr);
    };

    // The parameter rules, measured (d3d10_contract_probe [create]).
    if (device_out == 0) {
        fail(kEInvalidArg, "NULL ppDevice");
        return;
    }
    if (sdk != (api_10_1 ? d3d10::SDK_VERSION_10_1 : d3d10::SDK_VERSION)) {
        fail(kEInvalidArg, "SDK version");
        return;
    }
    if ((flags & kD3D10LayerFlags) != 0) {
        fail(kDxgiErrorSdkComponentMissing, "debug layer flag");
        return;
    }
    if (adapter_in != 0) {
        // An explicit adapter takes D3D10_DRIVER_TYPE_HARDWARE only.
        if (driver != d3d10::DRIVER_TYPE_HARDWARE || software != 0 ||
            !m_dxgi.is_adapter_object(adapter_in)) {
            fail(kEInvalidArg, "adapter with a driver type");
            return;
        }
    } else {
        switch (driver) {
        case d3d10::DRIVER_TYPE_HARDWARE:
        case d3d10::DRIVER_TYPE_NULL:
            if (software != 0) {
                fail(kEInvalidArg, "software module without SOFTWARE");
                return;
            }
            break;
        case d3d10::DRIVER_TYPE_REFERENCE:
            if (software != 0) {
                fail(kEInvalidArg, "software module without SOFTWARE");
                return;
            }
            // No reference rasterizer is installed (the Graphics Tools'
            // d3d11ref.dll): measured on x64 Windows, whose System32 has
            // none, D3D10CreateDevice answers DXGI_ERROR_UNSUPPORTED and
            // D3D10CreateDevice1 E_NOINTERFACE.
            fail(api_10_1 ? kENoInterface : kDxgiErrorUnsupported,
                 "no reference rasterizer");
            return;
        case d3d10::DRIVER_TYPE_WARP:
            // As D3D11CreateDevice (contract audit area 01): no WARP adapter
            // exists here, so a device claiming to be one is not handed out.
            fail(kDxgiErrorUnsupported, "no WARP adapter");
            return;
        case d3d10::DRIVER_TYPE_SOFTWARE:
            if (software == 0) {
                fail(kEInvalidArg, "SOFTWARE without a module");
                return;
            }
            if (!module_exports_user_mode_driver(software)) {
                fail(kHresultProcNotFound, "module has no OpenAdapter10");
                return;
            }
            {
                static std::once_flag reported;
                std::call_once(reported, [entry] {
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d10!%s: "
                                 "D3D10_DRIVER_TYPE_SOFTWARE rasterizer "
                                 "module\n",
                                 entry);
                });
            }
            fail(kDxgiErrorUnsupported, "software rasterizer module");
            return;
        default:
            fail(kEInvalidArg, "driver type");
            return;
        }
    }
    if (level != d3d10::FEATURE_LEVEL_10_1 &&
        level != d3d10::FEATURE_LEVEL_10_0 &&
        level != d3d10::FEATURE_LEVEL_9_3 &&
        level != d3d10::FEATURE_LEVEL_9_2 &&
        level != d3d10::FEATURE_LEVEL_9_1) {
        fail(kEInvalidArg, "feature level");
        return;
    }
    if (!d3d10::d3d10_enabled()) {
        fail(kDxgiErrorUnsupported, "VITREO_D3D10=0");
        return;
    }
    if (!m_backend.available()) {
        fail(kEFail, "Metal unavailable");
        return;
    }
    if (!ensure_storage() || !m_d3d10_ready) {
        fail(kEFail, "the D3D10 vtables are not wired");
        return;
    }

    GuestAddr adapter = adapter_in;
    if (adapter != 0) {
        if (!m_dxgi.retain_adapter(adapter)) {
            fail(kEInvalidArg, "adapter");
            return;
        }
    } else {
        adapter = m_dxgi.create_default_adapter();
        if (adapter == 0) {
            fail(kEFail, "no adapter");
            return;
        }
    }
    const u32 kept =
        flags & (kD3D10KeptFlags | (api_10_1 ? kD3D10BgraSupport : 0u));
    const GuestAddr owner = allocate_device(adapter, level, kept);
    auto device = m_devices.find(owner);
    if (owner == 0 || device == m_devices.end() ||
        device->second.d3d10_device_interface == 0) {
        fail(kEFail, "device storage");
        return;
    }
    Device& d = device->second;
    // The immediate context runs the D3D10 state from the start; the D3D10
    // interfaces answer, D3D11's context methods are off.
    d.d3d10_interfaces = true;
    d.d3d10_active = true;
    d.d3d10_api_10_0 = !api_10_1;
    // A D3D10 device is thread-safe from creation (measured:
    // ID3D10Multithread::GetMultithreadProtected is TRUE).
    d.multithread_protected = true;

    GuestAddr swap_chain = 0;
    u32 result = kSOk;
    if (with_swap_chain && desc_address != 0 && swap_chain_out != 0) {
        // Measured: a NULL description or ppSwapChain creates the device
        // alone and succeeds.
        result = create_device_swap_chain(d.device_interface, desc_address,
                                          adapter, swap_chain);
        if (swap_chain == 0) {
            destroy_if_unreferenced(owner);
            fail(result, "swap chain");
            return;
        }
    }
    ++d.device_ref_count;
    if (m.write_ptr(device_out, d.d3d10_device_interface).is_err() ||
        (swap_chain != 0 &&
         m.write_ptr(swap_chain_out, swap_chain).is_err())) {
        (void)m.write_ptr(device_out, 0);
        if (swap_chain != 0) (void)release_swap_chain(swap_chain);
        --d.device_ref_count;
        destroy_if_unreferenced(owner);
        fail(kEPointer, "output");
        return;
    }
    d3d11_diag("%s -> 0x%08x level=0x%x flags=0x%x device=0x%llx "
               "swap_chain=0x%llx",
               entry, result, level, kept,
               static_cast<unsigned long long>(d.d3d10_device_interface),
               static_cast<unsigned long long>(swap_chain));
    m.set_return(result);
}

} // namespace vitreo::winapi
