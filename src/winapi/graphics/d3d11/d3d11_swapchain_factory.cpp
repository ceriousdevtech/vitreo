// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_factory.cpp
/// @brief DXGI swap-chain descriptor marshaling (DXGI_SWAP_CHAIN_DESC,
///        DXGI_SWAP_CHAIN_DESC1) and the factory entry points:
///        IDXGIFactory::CreateSwapChain and
///        IDXGIFactory2::CreateSwapChainForHwnd / ForComposition.
///        Split out of d3d11_swapchain.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_swapchain_internal.hpp"
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

bool D3D11Manager::read_swap_chain_desc(
    GuestAddr address, SwapChainDesc& desc) const {
    if (address == 0) return false;
    const auto read32 = [&](usize offset, u32& value) {
        const auto field = m_memory.read_u32(address + offset);
        if (field.is_err()) return false;
        value = field.value();
        return true;
    };
    // DXGI_SWAP_CHAIN_DESC: fixed UINT fields through BufferCount, then a
    // pointer-sized HWND (offset 44 on x86, 48 on x64 after 4-byte pad).
    const usize window_offset = m_is_64bit ? 48u : 44u;
    const usize after_window = window_offset + (m_is_64bit ? 8u : 4u);
    GuestAddr window = 0;
    if (!read_pointer(address + window_offset, window)) return false;
    return
        read32(0, desc.width) &&
        read32(4, desc.height) &&
        read32(8, desc.refresh_numerator) &&
        read32(12, desc.refresh_denominator) &&
        read32(16, desc.format) &&
        read32(20, desc.scanline_ordering) &&
        read32(24, desc.scaling) &&
        read32(28, desc.sample_count) &&
        read32(32, desc.sample_quality) &&
        read32(36, desc.buffer_usage) &&
        read32(40, desc.buffer_count) &&
        ((desc.output_window = window), true) &&
        read32(after_window, desc.windowed) &&
        read32(after_window + 4, desc.swap_effect) &&
        read32(after_window + 8, desc.flags);
}

bool D3D11Manager::write_swap_chain_desc(
    GuestAddr address, const SwapChainDesc& desc) {
    if (address == 0) return false;
    const auto write32 = [&](usize offset, u32 value) {
        return m_memory.write_u32(address + offset, value).is_ok();
    };
    const usize window_offset = m_is_64bit ? 48u : 44u;
    const usize after_window = window_offset + (m_is_64bit ? 8u : 4u);
    if (!(write32(0, desc.width) &&
          write32(4, desc.height) &&
          write32(8, desc.refresh_numerator) &&
          write32(12, desc.refresh_denominator) &&
          write32(16, desc.format) &&
          write32(20, desc.scanline_ordering) &&
          write32(24, desc.scaling) &&
          write32(28, desc.sample_count) &&
          write32(32, desc.sample_quality) &&
          write32(36, desc.buffer_usage) &&
          write32(40, desc.buffer_count))) {
        return false;
    }
    if (m_is_64bit && !write32(44, 0)) return false;
    if (!write_pointer(address + window_offset, desc.output_window)) {
        return false;
    }
    return write32(after_window, desc.windowed) &&
           write32(after_window + 4, desc.swap_effect) &&
           write32(after_window + 8, desc.flags) &&
           (!m_is_64bit || write32(after_window + 12, 0));
}

u32 D3D11Manager::create_device_swap_chain(GuestAddr device_interface,
                                           GuestAddr desc_address,
                                           GuestAddr adapter,
                                           GuestAddr& swap_chain) {
    // What D3D11CreateDeviceAndSwapChain does with its description, for
    // D3D10CreateDeviceAndSwapChain(1): an IDXGIFactory::CreateSwapChain
    // chain parented to the adapter's factory.
    swap_chain = 0;
    SwapChainDesc swap_desc{};
    if (!read_swap_chain_desc(desc_address, swap_desc)) {
        return d3d11::E_INVALIDARG;
    }
    swap_desc.desc1_scaling = kDxgiScalingStretch;
    swap_desc.alpha_mode = kDxgiAlphaIgnore;
    if (swap_desc.output_window == 0) return DXGI_ERROR_INVALID_CALL;
    return create_swap_chain(device_interface, swap_desc,
                             SwapChainApi::Legacy, 0,
                             m_dxgi.parent_factory_of(adapter), 0,
                             swap_chain);
}

void D3D11Manager::impl_CreateSwapChain(Marshaler& m) {
    // IDXGIFactory::CreateSwapChain(this, pDevice, pDesc, ppSwapChain).
    // Measured: a NULL device, NULL description, NULL OutputWindow, or a
    // "device" that is really the immediate context fail with INVALID_CALL
    // and leave *ppSwapChain untouched; every later failure NULLs it. A NULL
    // ppSwapChain is INVALID_CALL per Learn (Windows throws instead).
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr device = m.arg_ptr(1);
    const GuestAddr desc_address = m.arg_ptr(2);
    const GuestAddr swap_chain_out = m.arg_ptr(3);
    SwapChainDesc desc{};
    InterfaceType device_type{};
    Device* device_record = find_device(device, &device_type);
    if (swap_chain_out == 0 || device_record == nullptr ||
        (device_type != InterfaceType::Device &&
         device_type != InterfaceType::D3D10Device &&
         device_type != InterfaceType::DxgiDevice) ||
        !read_swap_chain_desc(desc_address, desc) ||
        desc.output_window == 0) {
        d3d11_diag("IDXGIFactory::CreateSwapChain device=0x%llx desc=0x%llx "
                   "out=0x%llx -> DXGI_ERROR_INVALID_CALL (argument)",
                   static_cast<unsigned long long>(device),
                   static_cast<unsigned long long>(desc_address),
                   static_cast<unsigned long long>(swap_chain_out));
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(swap_chain_out, 0);
    // A legacy description has no DESC1 fields: GetDesc1 reports STRETCH
    // scaling and IGNORE alpha for it (measured).
    desc.desc1_scaling = kDxgiScalingStretch;
    desc.alpha_mode = kDxgiAlphaIgnore;
    GuestAddr swap_chain = 0;
    const u32 result = create_swap_chain(device, desc, SwapChainApi::Legacy,
                                         0, factory, 0, swap_chain);
    if (swap_chain == 0 || m.write_ptr(swap_chain_out, swap_chain).is_err()) {
        if (swap_chain != 0) (void)release_swap_chain(swap_chain);
        d3d11_diag(
            "IDXGIFactory::CreateSwapChain device=0x%llx %ux%u fmt=%u "
            "hwnd=0x%llx -> 0x%08x",
            static_cast<unsigned long long>(device),
            desc.width, desc.height, desc.format,
            static_cast<unsigned long long>(desc.output_window), result);
        m.set_return(swap_chain == 0 ? result : d3d11::E_POINTER);
        return;
    }
    d3d11_diag(
        "IDXGIFactory::CreateSwapChain device=0x%llx -> 0x%llx "
        "%ux%u fmt=%u buffers=%u usage=0x%x hwnd=0x%llx hr=0x%08x",
        static_cast<unsigned long long>(device),
        static_cast<unsigned long long>(swap_chain),
        desc.width, desc.height, desc.format, desc.buffer_count,
        desc.buffer_usage,
        static_cast<unsigned long long>(desc.output_window), result);
    m.set_return(result);
}

bool D3D11Manager::read_swap_chain_desc1(GuestAddr address,
                                         SwapChainDesc& desc,
                                         u32& stereo) const {
    // DXGI_SWAP_CHAIN_DESC1 (dxgi1_2.h): twelve UINT-sized fields, the same
    // layout on x86 and x64.
    if (address == 0) return false;
    const auto read32 = [&](usize offset, u32& value) {
        const auto field = m_memory.read_u32(address + offset);
        if (field.is_err()) return false;
        value = field.value();
        return true;
    };
    return read32(0, desc.width) && read32(4, desc.height) &&
           read32(8, desc.format) && read32(12, stereo) &&
           read32(16, desc.sample_count) &&
           read32(20, desc.sample_quality) &&
           read32(24, desc.buffer_usage) &&
           read32(28, desc.buffer_count) &&
           read32(32, desc.desc1_scaling) &&
           read32(36, desc.swap_effect) && read32(40, desc.alpha_mode) &&
           read32(44, desc.flags);
}

void D3D11Manager::impl_CreateSwapChainForHwnd(Marshaler& m) {
    // Learn IDXGIFactory2::CreateSwapChainForHwnd:
    // args: this, pDevice, hWnd, pDesc (DESC1), pFullscreenDesc, pRestrict,
    //       ppSwapChain
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr device = m.arg_ptr(1);
    const GuestAddr hwnd = m.arg_ptr(2);
    const GuestAddr desc1_address = m.arg_ptr(3);
    const GuestAddr fullscreen_desc = m.arg_ptr(4);
    const GuestAddr restrict_output = m.arg_ptr(5);
    const GuestAddr swap_chain_out = m.arg_ptr(6);
    SwapChainDesc desc{};
    u32 stereo = 0;
    // NULL device / hWnd / pDesc: INVALID_CALL, out untouched (measured).
    if (swap_chain_out == 0 || device == 0 || hwnd == 0 ||
        !read_swap_chain_desc1(desc1_address, desc, stereo) ||
        (restrict_output != 0 && !m_dxgi.is_output_object(restrict_output))) {
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(swap_chain_out, 0);
    desc.output_window = hwnd;
    desc.windowed = 1;
    // Without a fullscreen description the legacy mode fields are zero, as
    // GetFullscreenDesc/GetDesc then report (measured: 0/0 refresh).
    if (fullscreen_desc != 0) {
        // DXGI_SWAP_CHAIN_FULLSCREEN_DESC: RefreshRate {Numerator,
        // Denominator}, ScanlineOrdering, Scaling, Windowed.
        const auto read_fs = [&](usize offset, u32& value) {
            const auto field = m_memory.read_u32(fullscreen_desc + offset);
            if (field.is_err()) return false;
            value = field.value();
            return true;
        };
        if (!read_fs(0, desc.refresh_numerator) ||
            !read_fs(4, desc.refresh_denominator) ||
            !read_fs(8, desc.scanline_ordering) ||
            !read_fs(12, desc.scaling) || !read_fs(16, desc.windowed)) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
    }
    GuestAddr swap_chain = 0;
    const u32 result = create_swap_chain(device, desc, SwapChainApi::Hwnd,
                                         stereo, factory, restrict_output,
                                         swap_chain);
    if (swap_chain == 0 || m.write_ptr(swap_chain_out, swap_chain).is_err()) {
        if (swap_chain != 0) (void)release_swap_chain(swap_chain);
        d3d11_diag(
            "IDXGIFactory2::CreateSwapChainForHwnd device=0x%llx "
            "%ux%u fmt=%u hwnd=0x%llx -> 0x%08x",
            static_cast<unsigned long long>(device),
            desc.width, desc.height, desc.format,
            static_cast<unsigned long long>(hwnd), result);
        m.set_return(swap_chain == 0 ? result : d3d11::E_POINTER);
        return;
    }
    d3d11_diag(
        "IDXGIFactory2::CreateSwapChainForHwnd device=0x%llx -> 0x%llx "
        "%ux%u fmt=%u buffers=%u usage=0x%x hwnd=0x%llx hr=0x%08x",
        static_cast<unsigned long long>(device),
        static_cast<unsigned long long>(swap_chain),
        desc.width, desc.height, desc.format, desc.buffer_count,
        desc.buffer_usage,
        static_cast<unsigned long long>(hwnd), result);
    m.set_return(result);
}

void D3D11Manager::impl_CreateSwapChainForComposition(Marshaler& m) {
    // Learn IDXGIFactory2::CreateSwapChainForComposition:
    // args: this, pDevice, pDesc1, pRestrictToOutput, ppSwapChain.
    const GuestAddr factory = m.arg_ptr(0);
    const GuestAddr device = m.arg_ptr(1);
    const GuestAddr desc1_address = m.arg_ptr(2);
    const GuestAddr restrict_output = m.arg_ptr(3);
    const GuestAddr swap_chain_out = m.arg_ptr(4);
    SwapChainDesc desc{};
    u32 stereo = 0;
    // A composition chain has no window to size itself from: a zero Width or
    // Height is refused before *ppSwapChain is touched (measured).
    if (swap_chain_out == 0 || device == 0 ||
        !read_swap_chain_desc1(desc1_address, desc, stereo) ||
        desc.width == 0 || desc.height == 0 ||
        (restrict_output != 0 && !m_dxgi.is_output_object(restrict_output))) {
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    (void)m.write_ptr(swap_chain_out, 0);
    desc.windowed = 1;
    GuestAddr swap_chain = 0;
    const u32 result = create_swap_chain(device, desc,
                                         SwapChainApi::Composition, stereo,
                                         factory, restrict_output, swap_chain);
    if (swap_chain == 0 || m.write_ptr(swap_chain_out, swap_chain).is_err()) {
        if (swap_chain != 0) (void)release_swap_chain(swap_chain);
        m.set_return(swap_chain == 0 ? result : d3d11::E_POINTER);
        return;
    }
    d3d11_diag(
        "IDXGIFactory2::CreateSwapChainForComposition device=0x%llx "
        "-> 0x%llx %ux%u fmt=%u buffers=%u alpha=%u",
        static_cast<unsigned long long>(device),
        static_cast<unsigned long long>(swap_chain),
        desc.width, desc.height, desc.format, desc.buffer_count,
        desc.alpha_mode);
    m.set_return(result);
}

} // namespace vitreo::winapi
