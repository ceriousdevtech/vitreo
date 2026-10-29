// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_create_device.cpp
/// @brief The device-creation exports: feature-level selection, request
///        validation, D3D11CreateDevice, D3D11CreateDeviceAndSwapChain and
///        D3D11On12CreateDevice. Split out of d3d11.cpp as verbatim code
///        motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

u32 D3D11Manager::select_feature_level(GuestAddr levels, u32 level_count,
                                       bool& invalid) const {
    // Learn D3D11CreateDevice, measured on Windows
    // (d3d11_a01_device_caps_probe "levels ..." lines):
    // - pFeatureLevels NULL with a count is E_INVALIDARG; NULL with 0 is the
    //   default list {11_0, 10_1, 10_0, 9_3, 9_2, 9_1}.
    // - A non-NULL array with count 0 names no level at all:
    //   DXGI_ERROR_UNSUPPORTED (selected level 0), not E_INVALIDARG.
    // - The WHOLE array is validated before any level is chosen: an unknown
    //   value after a usable one ({11_0, 0xffff}) is still E_INVALIDARG.
    // - There is no count limit (33 and 40 entries succeed).
    invalid = false;
    if (levels == 0) {
        invalid = level_count != 0;
        return invalid ? 0 : d3d11::FEATURE_LEVEL_11_0;
    }
    u32 selected = 0;
    for (u32 index = 0; index < level_count; ++index) {
        const auto level = m_memory.read_u32(
            levels + static_cast<GuestAddr>(index) * sizeof(u32));
        if (level.is_err()) {
            invalid = true;
            return 0;
        }
        switch (level.value()) {
        case d3d11::FEATURE_LEVEL_12_2:
        case d3d11::FEATURE_LEVEL_12_1:
        case d3d11::FEATURE_LEVEL_12_0:
        case d3d11::FEATURE_LEVEL_11_1:
            // Valid levels above the Direct3D 11.0 contract this frontend
            // exposes. The Direct3D 11.3 runtime skips levels the device
            // cannot reach and creates it at the first one it can (measured:
            // WARP answers {11_1, 11_0} with 11_1, d3d11_fl111_probe "fl").
            // FL 11_1 is not offered because the GPU cannot rasterize what
            // it requires: target-independent rasterization -- a
            // ForcedSampleCount of 4, 8 or 16 over single-sample render
            // targets -- and UAV-only rendering at ForcedSampleCount 16
            // (D3D11.3 3.5.6.1-2, 3.5.6.4: "required for Feature Level 11_1
            // hardware"; Learn D3D11_FEATURE_DATA_D3D11_OPTIONS). Metal
            // rasterizes only at its attachments' own sample count, and at
            // most at a count the device supports: Apple GPUs refuse a
            // rasterSampleCount of 8 or 16. The other FL 11_1 requirements
            // this device reports as optional caps at 11_0 (logic ops,
            // constant-buffer windows, NO_OVERWRITE maps, ...), except
            // UAVs in the vertex/hull/domain/geometry stages and u8..u63,
            // which exist only at 11_1 and are not built.
            break;
        case d3d11::FEATURE_LEVEL_11_0:
        case d3d11::FEATURE_LEVEL_10_1:
        case d3d11::FEATURE_LEVEL_10_0:
        case d3d11::FEATURE_LEVEL_9_3:
        case d3d11::FEATURE_LEVEL_9_2:
        case d3d11::FEATURE_LEVEL_9_1:
            if (selected == 0) selected = level.value();
            break;
        default:
            invalid = true;
            return 0;
        }
    }
    return selected;
}

bool D3D11Manager::module_exports_user_mode_driver(GuestAddr module) const {
    // PE/COFF: e_lfanew at +0x3C; the export directory is data directory 0,
    // at optional-header offset 96 (PE32) or 112 (PE32+).
    const auto read32 = [this](GuestAddr address) -> std::optional<u32> {
        const auto value = m_memory.read_u32(address);
        if (value.is_err()) return std::nullopt;
        return value.value();
    };
    if (module == 0) return false;
    const auto e_lfanew = read32(module + 0x3C);
    if (!e_lfanew) return false;
    const GuestAddr nt = module + *e_lfanew;
    const auto signature = read32(nt);
    const auto magic = m_memory.read_u16(nt + 24);
    if (!signature || *signature != 0x00004550u || magic.is_err()) {
        return false;
    }
    const GuestAddr directory =
        nt + 24 + (magic.value() == 0x20Bu ? 112u : 96u);
    const auto export_rva = read32(directory);
    if (!export_rva || *export_rva == 0) return false;
    const GuestAddr exports = module + *export_rva;
    const auto name_count = read32(exports + 24);
    const auto names_rva = read32(exports + 32);
    if (!name_count || !names_rva) return false;
    for (u32 index = 0; index < *name_count && index < 65536u; ++index) {
        const auto name_rva = read32(module + *names_rva + index * 4u);
        if (!name_rva) return false;
        char name[16]{};
        if (m_memory.read_bytes(module + *name_rva,
                                reinterpret_cast<Byte*>(name),
                                sizeof(name) - 1).is_err()) {
            continue;
        }
        if (std::strcmp(name, "OpenAdapter10") == 0 ||
            std::strcmp(name, "OpenAdapter10_2") == 0) {
            return true;
        }
    }
    return false;
}

u32 D3D11Manager::validate_device_request(
    GuestAddr adapter, u32 driver_type, GuestAddr software, u32 flags,
    GuestAddr levels, u32 level_count, u32 sdk_version,
    u32& selected_level) const {
    selected_level = 0;
    if (sdk_version != d3d11::SDK_VERSION) return d3d11::E_INVALIDARG;
    // Measured: every creation-flag bit outside the kept and dropped sets asks
    // for the SDK layers (DEBUG, SWITCH_TO_REF, DEBUGGABLE, undefined bits),
    // which a system without Graphics Tools answers with
    // DXGI_ERROR_SDK_COMPONENT_MISSING before looking at anything else but
    // the SDK version.
    if ((flags & ~(d3d11::CREATE_DEVICE_KEPT_FLAGS |
                   d3d11::CREATE_DEVICE_DROPPED_FLAGS)) != 0) {
        return d3d11::DXGI_ERROR_SDK_COMPONENT_MISSING;
    }
    // Learn D3D11CreateDevice: with an explicit adapter the driver type must
    // be UNKNOWN; Software is "NULL otherwise" -- non-NULL only for
    // D3D_DRIVER_TYPE_SOFTWARE, which in turn cannot take a NULL one.
    if (adapter != 0) {
        if (driver_type != d3d11::DRIVER_TYPE_UNKNOWN || software != 0 ||
            !m_dxgi.is_adapter_object(adapter)) {
            return d3d11::E_INVALIDARG;
        }
    } else {
        switch (driver_type) {
        case d3d11::DRIVER_TYPE_HARDWARE:
        case d3d11::DRIVER_TYPE_NULL:
        case d3d11::DRIVER_TYPE_REFERENCE:
        case d3d11::DRIVER_TYPE_WARP:
            if (software != 0) return d3d11::E_INVALIDARG;
            break;
        case d3d11::DRIVER_TYPE_SOFTWARE:
            if (software == 0) return d3d11::E_INVALIDARG;
            break;
        default:
            return d3d11::E_INVALIDARG;
        }
    }
    bool invalid_levels = false;
    selected_level =
        select_feature_level(levels, level_count, invalid_levels);
    if (invalid_levels) return d3d11::E_INVALIDARG;
    if (adapter == 0) {
        switch (driver_type) {
        case d3d11::DRIVER_TYPE_REFERENCE:
            // The reference rasterizer ships only with the Graphics Tools
            // (d3d11ref.dll); a system without them answers
            // DXGI_ERROR_UNSUPPORTED (measured, x64 Windows).
            d3d11_diag("D3D11CreateDevice -> DXGI_ERROR_UNSUPPORTED "
                       "(no reference rasterizer)");
            return d3d11::DXGI_ERROR_UNSUPPORTED;
        case d3d11::DRIVER_TYPE_WARP:
            // Decision (2026-09-28 contract audit, area 01): there is no
            // software rasterizer here, and DXGI answers EnumWarpAdapter with
            // DXGI_ERROR_NOT_FOUND. Creating the Metal device under the WARP
            // name would hand back an adapter that is not WARP, so the device
            // answers as a system without WARP does.
            d3d11_diag("D3D11CreateDevice -> DXGI_ERROR_UNSUPPORTED "
                       "(no WARP adapter)");
            return d3d11::DXGI_ERROR_UNSUPPORTED;
        case d3d11::DRIVER_TYPE_SOFTWARE:
            // Windows loads the module's user-mode driver entry points;
            // without them it answers HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND)
            // (measured). A module that has them would have to run as a
            // rasterizer, which this runtime cannot host.
            if (!module_exports_user_mode_driver(software)) {
                return d3d11::HRESULT_PROC_NOT_FOUND;
            }
            {
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED d3d11!D3D11CreateDevice: "
                        "D3D_DRIVER_TYPE_SOFTWARE rasterizer module\n");
                }
            }
            return d3d11::DXGI_ERROR_UNSUPPORTED;
        default:
            break;
        }
    }
    if (selected_level == 0) return d3d11::DXGI_ERROR_UNSUPPORTED;
    return d3d11::S_OK;
}

void D3D11Manager::impl_D3D11CreateDevice(Marshaler& m) {
    const GuestAddr requested_adapter = m.arg_ptr(0);
    const u32 driver_type = static_cast<u32>(m.arg(1));
    const GuestAddr software = m.arg_ptr(2);
    const u32 flags = static_cast<u32>(m.arg(3));
    const GuestAddr feature_levels = m.arg_ptr(4);
    const u32 feature_level_count = static_cast<u32>(m.arg(5));
    const u32 sdk_version = static_cast<u32>(m.arg(6));
    const GuestAddr device_out = m.arg_ptr(7);
    const GuestAddr feature_level_out = m.arg_ptr(8);
    const GuestAddr context_out = m.arg_ptr(9);

    if (device_out != 0) (void)m.write_ptr(device_out, 0);
    if (feature_level_out != 0) (void)m.write_u32(feature_level_out, 0);
    if (context_out != 0) (void)m.write_ptr(context_out, 0);

    d3d11_diag(
        "D3D11CreateDevice adapter=0x%llx driver=%u flags=0x%x "
        "levels=%u sdk=%u outputs(device=%s level=%s context=%s)",
        static_cast<unsigned long long>(requested_adapter), driver_type,
        flags, feature_level_count, sdk_version,
        device_out != 0 ? "yes" : "no",
        feature_level_out != 0 ? "yes" : "no",
        context_out != 0 ? "yes" : "no");

    u32 selected_level = 0;
    // /tmp/vitreo-d3d11-caps: what was asked, what was chosen, the result.
    const CreateDeviceLog caps_log(m, "D3D11CreateDevice", driver_type, flags,
                                   feature_levels, feature_level_count,
                                   &selected_level);
    const u32 request_hr = validate_device_request(
        requested_adapter, driver_type, software, flags, feature_levels,
        feature_level_count, sdk_version, selected_level);
    if (request_hr != d3d11::S_OK) {
        m.set_return(request_hr);
        return;
    }

    if (!m_backend.available()) {
        d3d11_diag("D3D11CreateDevice -> E_FAIL (Metal unavailable)");
        m.set_return(d3d11::E_FAIL);
        return;
    }

    // Learn D3D11CreateDevice: with ppDevice and ppImmediateContext both
    // NULL the call only validates, writes *pFeatureLevel and returns S_FALSE
    // (measured: "out fl" / "out none" -> 0x00000001).
    if (device_out == 0 && context_out == 0) {
        if (feature_level_out != 0 &&
            m.write_u32(feature_level_out, selected_level).is_err()) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag("D3D11CreateDevice probe -> S_FALSE feature=0x%x",
                   selected_level);
        m.set_return(1u);  // S_FALSE
        return;
    }

    GuestAddr adapter = requested_adapter;
    if (adapter != 0) {
        if (!m_dxgi.retain_adapter(adapter)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
    } else {
        adapter = m_dxgi.create_default_adapter();
        if (adapter == 0) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
    }

    const GuestAddr owner = allocate_device(
        adapter, selected_level, flags & d3d11::CREATE_DEVICE_KEPT_FLAGS);
    auto device = m_devices.find(owner);
    if (owner == 0 || device == m_devices.end()) {
        m.set_return(d3d11::E_FAIL);
        return;
    }
    if (device_out != 0) ++device->second.device_ref_count;
    // The immediate context the caller receives holds the device while the
    // caller holds it (measured: device count 2 with ppImmediateContext, 1
    // without).
    if (context_out != 0) {
        ++device->second.context_ref_count;
        ++device->second.device_ref_count;
    }

    const bool writes_ok =
        (device_out == 0 ||
         m.write_ptr(device_out, device->second.device_interface).is_ok()) &&
        (feature_level_out == 0 ||
         m.write_u32(feature_level_out, selected_level).is_ok()) &&
        (context_out == 0 ||
         m.write_ptr(context_out,
                     device->second.context_interface).is_ok());
    if (!writes_ok) {
        if (device_out != 0) (void)m.write_ptr(device_out, 0);
        if (feature_level_out != 0) (void)m.write_u32(feature_level_out, 0);
        if (context_out != 0) (void)m.write_ptr(context_out, 0);
        device->second.device_ref_count = 0;
        device->second.context_ref_count = 0;
        destroy_if_unreferenced(owner);
        m.set_return(d3d11::E_POINTER);
        return;
    }

    d3d11_diag(
        "D3D11CreateDevice -> S_OK feature=0x%x device=0x%llx "
        "context=0x%llx adapter=0x%llx Metal=\"%s\"",
        selected_level,
        static_cast<unsigned long long>(
            device_out != 0 ? device->second.device_interface : 0),
        static_cast<unsigned long long>(
            context_out != 0 ? device->second.context_interface : 0),
        static_cast<unsigned long long>(adapter),
        m_backend.backend_device_name().c_str());
    diagnostics::phase_mark(diagnostics::LoadPhase::FirstDevice);
    m.set_return(d3d11::S_OK);
}

void D3D11Manager::impl_D3D11CreateDeviceAndSwapChain(Marshaler& m) {
    const GuestAddr requested_adapter = m.arg_ptr(0);
    const u32 driver_type = static_cast<u32>(m.arg(1));
    const GuestAddr software = m.arg_ptr(2);
    const u32 flags = static_cast<u32>(m.arg(3));
    const GuestAddr feature_levels = m.arg_ptr(4);
    const u32 feature_level_count = static_cast<u32>(m.arg(5));
    const u32 sdk_version = static_cast<u32>(m.arg(6));
    const GuestAddr swap_desc_address = m.arg_ptr(7);
    const GuestAddr swap_chain_out = m.arg_ptr(8);
    const GuestAddr device_out = m.arg_ptr(9);
    const GuestAddr feature_level_out = m.arg_ptr(10);
    const GuestAddr context_out = m.arg_ptr(11);
    if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
    if (device_out != 0) (void)m.write_ptr(device_out, 0);
    if (feature_level_out != 0) (void)m.write_u32(feature_level_out, 0);
    if (context_out != 0) (void)m.write_ptr(context_out, 0);

    // Learn D3D11CreateDeviceAndSwapChain, measured on Windows
    // (d3d11_a01_device_caps_probe_swap.expected):
    // - the device parameters follow D3D11CreateDevice exactly;
    // - a swap chain is created only when BOTH pSwapChainDesc and ppSwapChain
    //   are given. A NULL desc, a NULL ppSwapChain, or both, create the
    //   device alone and return S_OK (a given ppSwapChain reads NULL);
    // - with no device, context or swap-chain output the call validates only
    //   and returns S_FALSE after writing *pFeatureLevel.
    u32 selected_level = 0;
    const CreateDeviceLog caps_log(m, "D3D11CreateDeviceAndSwapChain",
                                   driver_type, flags, feature_levels,
                                   feature_level_count, &selected_level);
    const u32 request_hr = validate_device_request(
        requested_adapter, driver_type, software, flags, feature_levels,
        feature_level_count, sdk_version, selected_level);
    if (request_hr != d3d11::S_OK) {
        m.set_return(request_hr);
        return;
    }
    if (!m_backend.available()) {
        d3d11_diag("D3D11CreateDeviceAndSwapChain -> E_FAIL "
                   "(Metal unavailable)");
        m.set_return(d3d11::E_FAIL);
        return;
    }
    const bool wants_swap_chain =
        swap_desc_address != 0 && swap_chain_out != 0;
    SwapChainDesc swap_desc{};
    if (wants_swap_chain &&
        !read_swap_chain_desc(swap_desc_address, swap_desc)) {
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    if (device_out == 0 && context_out == 0 && !wants_swap_chain) {
        if (feature_level_out != 0 &&
            m.write_u32(feature_level_out, selected_level).is_err()) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(1u);  // S_FALSE
        return;
    }

    GuestAddr adapter = requested_adapter;
    if (adapter != 0) {
        if (!m_dxgi.retain_adapter(adapter)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
    } else {
        adapter = m_dxgi.create_default_adapter();
        if (adapter == 0) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
    }

    const GuestAddr owner = allocate_device(
        adapter, selected_level, flags & d3d11::CREATE_DEVICE_KEPT_FLAGS);
    auto device = m_devices.find(owner);
    if (owner == 0 || device == m_devices.end()) {
        m.set_return(d3d11::E_FAIL);
        return;
    }
    // Learn D3D11CreateDeviceAndSwapChain: a NULL pSwapChainDesc or
    // ppSwapChain creates the device alone.
    GuestAddr swap_chain = 0;
    // S_OK, or DXGI_STATUS_OCCLUDED for a chain created windowed from a
    // Windowed=FALSE description.
    u32 swap_chain_result = d3d11::S_OK;
    if (wants_swap_chain) {
        // The chain's parent is the factory the adapter came from, as for any
        // IDXGIFactory::CreateSwapChain call.
        swap_desc.desc1_scaling = kDxgiScalingStretch;
        swap_desc.alpha_mode = kDxgiAlphaIgnore;
        swap_chain_result =
            swap_desc.output_window == 0
                ? DXGI_ERROR_INVALID_CALL
                : create_swap_chain(device->second.device_interface,
                                    swap_desc, SwapChainApi::Legacy, 0,
                                    m_dxgi.parent_factory_of(adapter), 0,
                                    swap_chain);
        if (swap_chain == 0) {
            destroy_if_unreferenced(owner);
            m.set_return(swap_chain_result);
            return;
        }
    }

    if (device_out != 0) ++device->second.device_ref_count;
    // The immediate context the caller receives holds the device while the
    // caller holds it (measured: device count 2 with ppImmediateContext, 1
    // without).
    if (context_out != 0) {
        ++device->second.context_ref_count;
        ++device->second.device_ref_count;
    }
    const bool writes_ok =
        (swap_chain == 0 ||
         m.write_ptr(swap_chain_out, swap_chain).is_ok()) &&
        (device_out == 0 ||
         m.write_ptr(device_out,
                     device->second.device_interface).is_ok()) &&
        (feature_level_out == 0 ||
         m.write_u32(feature_level_out, selected_level).is_ok()) &&
        (context_out == 0 ||
         m.write_ptr(context_out,
                     device->second.context_interface).is_ok());
    if (!writes_ok) {
        if (device_out != 0 && device->second.device_ref_count != 0) {
            --device->second.device_ref_count;
        }
        if (context_out != 0 && device->second.context_ref_count != 0) {
            --device->second.context_ref_count;
            if (device->second.device_ref_count != 0) {
                --device->second.device_ref_count;
            }
        }
        if (swap_chain != 0) (void)release_swap_chain(swap_chain);
        if (swap_chain_out != 0) (void)m.write_ptr(swap_chain_out, 0);
        if (device_out != 0) (void)m.write_ptr(device_out, 0);
        if (feature_level_out != 0) (void)m.write_u32(feature_level_out, 0);
        if (context_out != 0) (void)m.write_ptr(context_out, 0);
        destroy_if_unreferenced(owner);
        m.set_return(d3d11::E_POINTER);
        return;
    }
    d3d11_diag(
        "D3D11CreateDeviceAndSwapChain -> S_OK device=0x%llx "
        "context=0x%llx swap_chain=0x%llx",
        static_cast<unsigned long long>(device->second.device_interface),
        static_cast<unsigned long long>(device->second.context_interface),
        static_cast<unsigned long long>(swap_chain));
    diagnostics::phase_mark(diagnostics::LoadPhase::FirstDevice);
    m.set_return(swap_chain_result);
}

// HRESULT D3D11On12CreateDevice(IUnknown* pDevice, UINT Flags,
//     const D3D_FEATURE_LEVEL* pFeatureLevels, UINT FeatureLevels,
//     IUnknown* const* ppCommandQueues, UINT NumQueues, UINT NodeMask,
//     ID3D11Device** ppDevice, ID3D11DeviceContext** ppImmediateContext,
//     D3D_FEATURE_LEVEL* pChosenFeatureLevel)          (d3d11on12.h, 10 args)
//
// Measured on Windows 11 (tools/oracle_probe/gfx_media_exports_probe
// .expected): ppDevice and ppImmediateContext are NULLed on failure while
// pChosenFeatureLevel is left untouched; a NULL pDevice is E_INVALIDARG, and
// an object that is not a D3D12 device is QueryInterface'd exactly once (for
// ID3D12Device) and then refused with E_INVALIDARG.
//
// A real Vitreo D3D12 device (VITREO_D3D12=1 only) cannot be wrapped yet: the
// D3D12 frontend has no ID3D12Resource for CreateWrappedResource to adopt and
// no queue that D3D11 work could be submitted to, so that case fails loudly.
void D3D11Manager::impl_D3D11On12CreateDevice(Marshaler& m) {
    constexpr u32 kEInvalidArg = 0x80070057u;
    constexpr u32 kENotImpl = 0x80004001u;
    const GuestAddr d3d12_device = m.arg_ptr(0);
    const GuestAddr out_device = m.arg_ptr(7);
    const GuestAddr out_context = m.arg_ptr(8);
    if (out_device != 0) (void)write_pointer(out_device, 0);
    if (out_context != 0) (void)write_pointer(out_context, 0);
    if (d3d12_device == 0) {
        m.set_return(kEInvalidArg);
        return;
    }
    bool is_d3d12 = m_is_d3d12_device && m_is_d3d12_device(d3d12_device);
    if (!is_d3d12 && m_guest_call) {
        // QueryInterface(IID_ID3D12Device) through the object's own vtable,
        // as Windows does. The scratch block holds the IID and the out slot.
        static constexpr std::array<u8, 16> kIidD3D12Device{
            0xf1, 0x19, 0x98, 0x18, 0xb6, 0x1d, 0x57, 0x4b,
            0xbe, 0x54, 0x18, 0x21, 0x33, 0x9b, 0x85, 0xf7};  // {189819f1-1db6-4b57-be54-1821339b85f7}
        GuestAddr vtable = 0, query = 0;
        const GuestAddr scratch = allocate_object(0);
        if (scratch != 0 && read_pointer(d3d12_device, vtable) && vtable != 0 &&
            read_pointer(vtable, query) && query != 0 &&
            m_memory.write_bytes(scratch + 0x10, kIidD3D12Device.data(),
                                 kIidD3D12Device.size()).is_ok() &&
            write_pointer(scratch + 0x20, 0)) {
            const u64 args[3] = {d3d12_device, scratch + 0x10, scratch + 0x20};
            const auto caller = m_guest_call;
            u32 hr;
            {
                DxgiManager::ApiUnlock unlocked(m_dxgi);
                hr = static_cast<u32>(caller(query, args, 3));
            }
            GuestAddr resolved = 0;
            if ((hr & 0x80000000u) == 0 && read_pointer(scratch + 0x20, resolved) &&
                resolved != 0) {
                is_d3d12 = m_is_d3d12_device && m_is_d3d12_device(resolved);
                (void)guest_reference(resolved, COM_SLOT_RELEASE);
            }
        }
        if (scratch != 0) free_object(scratch);
    }
    if (!is_d3d12) {
        m.set_return(kEInvalidArg);
        return;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::fprintf(stderr,
            "[vitreo] UNIMPLEMENTED d3d11!D3D11On12CreateDevice: the D3D12 "
            "frontend has no resources or submission queue to layer D3D11 on\n");
    }
    m.set_return(kENotImpl);
}

} // namespace vitreo::winapi
