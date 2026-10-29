// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_output.cpp
/// @brief IDXGIAdapter::EnumOutputs and the DXGI_OUTPUT_DESC,
///        DXGI_OUTPUT_DESC1 and DXGI_MODE_DESC writers.
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

constexpr u32 DXGI_MODE_ROTATION_IDENTITY = 1;
constexpr u32 DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE = 1;

} // namespace

void DxgiManager::impl_EnumOutputs(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr output_out = m.arg_ptr(2);
    // EnumOutputs(i, NULL) is E_INVALIDARG (measured).
    if (output_out == 0) {
        m.set_return(dxgi::E_INVALIDARG);
        return;
    }
    (void)m.write_ptr(output_out, 0);
    const auto adapter = m_objects.find(self);
    if (adapter == m_objects.end() ||
        adapter->second.type != ObjectType::Adapter) {
        dxgi_diag("IDXGIAdapter::EnumOutputs -> "
                  "DXGI_ERROR_INVALID_CALL");
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    }
    const std::vector<dxgi::OutputInfo> outputs = output_infos();
    if (index >= outputs.size()) {
        dxgi_diag("IDXGIAdapter::EnumOutputs(%u) -> DXGI_ERROR_NOT_FOUND",
                  index);
        m.set_return(dxgi::ERROR_NOT_FOUND);
        return;
    }
    const GuestAddr output = create_output(self, outputs[index]);
    if (output == 0 || m.write_ptr(output_out, output).is_err()) {
        if (output != 0) (void)release_object(output);
        m.set_return(dxgi::E_POINTER);
        return;
    }
    dxgi_diag("IDXGIAdapter::EnumOutputs(%u) -> 0x%llx %s "
              "rect=(%d,%d)-(%d,%d)",
              index, static_cast<unsigned long long>(output),
              outputs[index].device_name.c_str(), outputs[index].left,
              outputs[index].top, outputs[index].right,
              outputs[index].bottom);
    m.set_return(dxgi::S_OK);
}

bool DxgiManager::write_output_desc(
    Marshaler& m, GuestAddr address, const dxgi::OutputInfo& info) const {
    if (address == 0) return false;
    const usize size = m_is_64bit ? 96 : 92;
    std::vector<u8> bytes(size, 0);
    if (m_memory.write_bytes(address, bytes.data(), bytes.size()).is_err()) {
        return false;
    }
    const usize name_length = std::min<usize>(info.device_name.size(), 31);
    for (usize index = 0; index < name_length; ++index) {
        if (m_memory.write_u16(
                address + index * sizeof(u16),
                static_cast<u8>(info.device_name[index])).is_err()) {
            return false;
        }
    }
    if (m.write_u32(address + 64, static_cast<u32>(info.left)).is_err() ||
        m.write_u32(address + 68, static_cast<u32>(info.top)).is_err() ||
        m.write_u32(address + 72, static_cast<u32>(info.right)).is_err() ||
        m.write_u32(address + 76, static_cast<u32>(info.bottom)).is_err() ||
        m.write_u32(address + 80, 1).is_err() ||
        m.write_u32(address + 84, DXGI_MODE_ROTATION_IDENTITY).is_err()) {
        return false;
    }
    return m_is_64bit
        ? m_memory.write_u64(address + 88, info.monitor).is_ok()
        : m_memory.write_u32(address + 88,
                             static_cast<u32>(info.monitor)).is_ok();
}

bool DxgiManager::write_output_desc1(
    Marshaler& m, GuestAddr address, const dxgi::OutputInfo& info) const {
    if (address == 0) return false;
    const usize structure_size = m_is_64bit ? 152 : 144;
    const usize extension_offset = m_is_64bit ? 96 : 92;
    std::vector<u8> bytes(structure_size, 0);
    if (m_memory.write_bytes(address, bytes.data(), bytes.size()).is_err() ||
        !write_output_desc(m, address, info)) {
        return false;
    }

    // The current Vitreo desktop pipeline is SDR/sRGB. Microsoft documents
    // these Rec.709/D65 values as the standard SDR defaults when display
    // chromaticity and luminance metadata are unavailable.
    constexpr std::array<float, 8> chromaticity{
        0.6400f, 0.3300f, // red
        0.3000f, 0.6000f, // green
        0.1500f, 0.0600f, // blue
        0.3127f, 0.3290f, // D65 white
    };
    if (m.write_u32(address + extension_offset, info.bits_per_color).is_err() ||
        m.write_u32(address + extension_offset + 4, 0).is_err()) {
        return false;
    }
    for (usize index = 0; index < chromaticity.size(); ++index) {
        if (m.write_u32(address + extension_offset + 8 + index * 4,
                        std::bit_cast<u32>(chromaticity[index])).is_err()) {
            return false;
        }
    }
    // Min/Max/MaxFullFrameLuminance remain zero because Cocoa does not expose
    // EDID luminance metadata through NSScreen's stable public contract.
    return true;
}

bool DxgiManager::write_mode_desc(
    Marshaler& m, GuestAddr address, const dxgi::OutputInfo& info,
    u32 format) const {
    if (address == 0 || info.right <= info.left || info.bottom <= info.top) {
        return false;
    }
    std::array<u8, 28> bytes{};
    if (m_memory.write_bytes(address, bytes.data(), bytes.size()).is_err()) {
        return false;
    }
    u32 refresh_numerator = 0;
    u32 refresh_denominator = 0;
    if (std::isfinite(info.refresh_rate_hz) && info.refresh_rate_hz > 0.0) {
        u64 numerator = static_cast<u64>(
            std::llround(info.refresh_rate_hz * 1000.0));
        u64 denominator = 1000;
        const u64 divisor = std::gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
        refresh_numerator = static_cast<u32>(
            std::min<u64>(numerator, 0xFFFFFFFFull));
        refresh_denominator = static_cast<u32>(denominator);
    }
    return m.write_u32(address, static_cast<u32>(info.right - info.left)).is_ok() &&
           m.write_u32(address + 4,
                       static_cast<u32>(info.bottom - info.top)).is_ok() &&
           m.write_u32(address + 8, refresh_numerator).is_ok() &&
           m.write_u32(address + 12, refresh_denominator).is_ok() &&
           m.write_u32(address + 16, format).is_ok() &&
           m.write_u32(address + 20,
                       DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE).is_ok() &&
           m.write_u32(address + 24, 0).is_ok();
}

} // namespace vitreo::winapi
