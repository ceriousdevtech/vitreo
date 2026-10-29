// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxgi_output_methods.cpp
/// @brief The IDXGIOutput .. IDXGIOutput6 vtable dispatch
///        (impl_OutputMethod).
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

constexpr u32 DXGI_FORMAT_R8G8B8A8_UNORM = 28;
constexpr u32 DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29;
constexpr u32 DXGI_FORMAT_B8G8R8A8_UNORM = 87;
constexpr u32 DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91;

} // namespace

void DxgiManager::impl_OutputMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto object = m_objects.find(self);
    if (object == m_objects.end() || object->second.type != ObjectType::Output) {
        m.set_return(slot == 1 || slot == 2 ? 0 : dxgi::ERROR_INVALID_CALL);
        return;
    }
    if (slot == 0) { impl_QueryInterface(m); return; }
    if (slot == 1) { impl_AddRef(m); return; }
    if (slot == 2) { impl_Release(m); return; }
    if (slot == 3) { impl_SetPrivateData(m); return; }
    if (slot == 4) { impl_SetPrivateDataInterface(m); return; }
    if (slot == 5) { impl_GetPrivateData(m); return; }
    if (slot == 6) { impl_GetParent(m); return; }

    // The description is the display's current one (DesktopCoordinates
    // follow an exclusive full-screen mode change); `object` is not used
    // past this point because the lookup releases the API lock.
    const dxgi::OutputInfo stored = object->second.output_info;
    const dxgi::OutputInfo info = output_info_of(self).value_or(stored);
    const auto [native_width, native_height] = dxgi::output_native_size(info);
    switch (slot) {
    case 7: {
        const GuestAddr description = m.arg_ptr(1);
        if (!write_output_desc(m, description, info)) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        dxgi_diag("IDXGIOutput::GetDesc self=0x%llx -> %s "
                  "rect=(%d,%d)-(%d,%d) monitor=0x%llx",
                  static_cast<unsigned long long>(self),
                  info.device_name.c_str(), info.left, info.top, info.right,
                  info.bottom, static_cast<unsigned long long>(info.monitor));
        m.set_return(dxgi::S_OK);
        return;
    }
    case 8:
    case 19: {
        const u32 format = static_cast<u32>(m.arg(1));
        (void)m.arg(2); // DXGI_ENUM_MODES flags do not exclude the current mode.
        const GuestAddr count_address = m.arg_ptr(3);
        const GuestAddr descriptions = m.arg_ptr(4);
        if (count_address == 0) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        const bool supported =
            format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        const auto modes = virtual_display_modes(native_width, native_height);
        const u32 required = supported ? static_cast<u32>(modes.size()) : 0u;
        const u32 capacity = m_memory.read_u32(count_address).value_or(0);
        // Learn GetDisplayModeList: with pDesc NULL, *pNumModes receives the
        // number of modes; otherwise it receives the number RETURNED in
        // pDesc, and DXGI_ERROR_MORE_DATA says the array was too small.
        const u32 returned =
            descriptions == 0 ? required : std::min(capacity, required);
        if (m.write_u32(count_address, returned).is_err()) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        if (descriptions == 0 || required == 0) {
            m.set_return(dxgi::S_OK);
            return;
        }
        const usize stride = slot == 19 ? 32u : 28u;
        for (u32 index = 0; index < returned; ++index) {
            auto mode_info = info;
            mode_info.left = mode_info.top = 0;
            mode_info.right = static_cast<i32>(modes[index].width);
            mode_info.bottom = static_cast<i32>(modes[index].height);
            const GuestAddr destination = descriptions + index * stride;
            if (!write_mode_desc(m, destination, mode_info, format) ||
                (slot == 19 && m.write_u32(destination + 28, 0).is_err())) {
                m.set_return(dxgi::ERROR_INVALID_CALL);
                return;
            }
        }
        dxgi_diag("IDXGIOutput::GetDisplayModeList format=%u -> "
                  "%ux%u %.3fHz", format, native_width, native_height,
                  info.refresh_rate_hz);
        m.set_return(returned < required ? dxgi::ERROR_MORE_DATA
                                         : dxgi::S_OK);
        return;
    }
    case 9: {
        const GuestAddr requested = m.arg_ptr(1);
        const GuestAddr closest = m.arg_ptr(2);
        const GuestAddr concerned_device = m.arg_ptr(3);
        if (requested == 0 || closest == 0) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        const u32 width = m_memory.read_u32(requested).value_or(0);
        const u32 height = m_memory.read_u32(requested + 4).value_or(0);
        u32 format = m_memory.read_u32(requested + 16).value_or(0);
        if ((width == 0) != (height == 0) ||
            (format == 0 && concerned_device == 0)) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        if (format == 0) format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const bool supported =
            format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if (!supported) {
            m.set_return(dxgi::ERROR_NOT_FOUND);
            return;
        }
        auto mode_info = info;
        const auto mode = closest_virtual_display_mode(
            native_width, native_height, width, height);
        mode_info.left = mode_info.top = 0;
        mode_info.right = static_cast<i32>(mode.width);
        mode_info.bottom = static_cast<i32>(mode.height);
        if (!write_mode_desc(m, closest, mode_info, format)) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        m.set_return(dxgi::S_OK);
        return;
    }
    case 20: {
        const GuestAddr requested = m.arg_ptr(1);
        const GuestAddr closest = m.arg_ptr(2);
        const GuestAddr concerned_device = m.arg_ptr(3);
        if (requested == 0 || closest == 0) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        const u32 width = m_memory.read_u32(requested).value_or(0);
        const u32 height = m_memory.read_u32(requested + 4).value_or(0);
        u32 format = m_memory.read_u32(requested + 16).value_or(0);
        const u32 stereo = m_memory.read_u32(requested + 28).value_or(0);
        if ((width == 0) != (height == 0) || stereo != 0 ||
            (format == 0 && concerned_device == 0)) {
            m.set_return(stereo != 0 ? dxgi::ERROR_NOT_FOUND
                                     : dxgi::ERROR_INVALID_CALL);
            return;
        }
        if (format == 0) format = DXGI_FORMAT_B8G8R8A8_UNORM;
        const bool supported =
            format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if (!supported) {
            m.set_return(dxgi::ERROR_NOT_FOUND);
            return;
        }
        auto mode_info = info;
        const auto mode = closest_virtual_display_mode(
            native_width, native_height, width, height);
        mode_info.left = mode_info.top = 0;
        mode_info.right = static_cast<i32>(mode.width);
        mode_info.bottom = static_cast<i32>(mode.height);
        if (!write_mode_desc(m, closest, mode_info, format) ||
            m.write_u32(closest + 28, 0).is_err()) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        m.set_return(dxgi::S_OK);
        return;
    }
    case 13:  // GetGammaControlCapabilities
    case 14:  // SetGammaControl
    case 15:  // GetGammaControl
    case 16:  // SetDisplaySurface
    case 17:  // GetDisplaySurfaceData
    case 18:  // GetFrameStatistics
    case 21:  // GetDisplaySurfaceData1
        // Learn attaches the same note to each of these: "Calling this method is
        // only supported while in full-screen mode", and GetFrameStatistics
        // documents DXGI_ERROR_INVALID_CALL as its failure code.
        //
        // `TakeOwnership` never succeeds in this frontend (see slot 12), so a
        // guest can never be in that state, which makes INVALID_CALL the
        // factually correct answer for every reachable call -- and a better one
        // than E_NOTIMPL, which an engine may read as a driver fault rather than
        // as "not in the mode this needs".
        //
        // The out-parameters on 13, 15 and 18 are deliberately left untouched.
        // That is the opposite of the IDXGIFactory2 stubs, which must null
        // theirs: those hand back an interface pointer the guest may Release or
        // a cookie it may pass back in, while these are plain structs
        // (DXGI_GAMMA_CONTROL alone is 12 KB) that Windows does not document
        // clearing and that a caller reads only after checking the HRESULT.
        dxgi_diag("IDXGIOutput vtable slot %zu -> DXGI_ERROR_INVALID_CALL "
                  "(needs full-screen output ownership, which this frontend "
                  "never grants)", slot);
        m.set_return(dxgi::ERROR_INVALID_CALL);
        return;
    case 22: {
        const GuestAddr duplicate_output = m.arg_ptr(2);
        if (duplicate_output != 0) (void)m.write_ptr(duplicate_output, 0);
        // Desktop duplication is not implemented by the Metal backend. Learn
        // DuplicateOutput lists DXGI_ERROR_UNSUPPORTED for a desktop that
        // cannot be duplicated (E_NOTIMPL is only for Windows 7).
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED dxgi!IDXGIOutput1::"
                         "DuplicateOutput: desktop duplication -> "
                         "DXGI_ERROR_UNSUPPORTED\n");
        }
        m.set_return(dxgi::ERROR_UNSUPPORTED);
        return;
    }
    case 23:
        // No multiplane-overlay composition path is implemented.
        dxgi_diag("IDXGIOutput2::SupportsOverlays -> FALSE");
        m.set_return(0);
        return;
    case 24: {
        (void)m.arg(1);
        const GuestAddr concerned_device = m.arg_ptr(2);
        const GuestAddr flags = m.arg_ptr(3);
        if (concerned_device == 0 || flags == 0 ||
            m.write_u32(flags, 0).is_err()) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        dxgi_diag("IDXGIOutput3::CheckOverlaySupport -> S_OK flags=0");
        m.set_return(dxgi::S_OK);
        return;
    }
    case 25: {
        (void)m.arg(1); // Format
        (void)m.arg(2); // ColorSpace
        const GuestAddr concerned_device = m.arg_ptr(3);
        const GuestAddr flags = m.arg_ptr(4);
        if (concerned_device == 0 || flags == 0 ||
            m.write_u32(flags, 0).is_err()) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        dxgi_diag("IDXGIOutput4::CheckOverlayColorSpaceSupport -> "
                  "S_OK flags=0");
        m.set_return(dxgi::S_OK);
        return;
    }
    case 26: {
        const GuestAddr duplicate_output = m.arg_ptr(5);
        if (duplicate_output != 0) (void)m.write_ptr(duplicate_output, 0);
        // Desktop duplication is not implemented by the Metal backend. Learn
        // DuplicateOutput lists DXGI_ERROR_UNSUPPORTED for a desktop that
        // cannot be duplicated (E_NOTIMPL is only for Windows 7).
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED dxgi!IDXGIOutput1::"
                         "DuplicateOutput: desktop duplication -> "
                         "DXGI_ERROR_UNSUPPORTED\n");
        }
        m.set_return(dxgi::ERROR_UNSUPPORTED);
        return;
    }
    case 27: {
        const GuestAddr description = m.arg_ptr(1);
        if (!write_output_desc1(m, description, info)) {
            m.set_return(dxgi::ERROR_INVALID_CALL);
            return;
        }
        dxgi_diag("IDXGIOutput6::GetDesc1 -> %s bits=%u color_space=SDR",
                  info.device_name.c_str(), info.bits_per_color);
        m.set_return(dxgi::S_OK);
        return;
    }
    case 10: {
        // Learn IDXGIOutput::WaitForVBlank: block until the next vertical
        // blank, then return S_OK. Compositors drive their frame clock from
        // this call, so returning E_NOTIMPL leaves them without a tick.
        const double refresh_hz =
            std::isfinite(info.refresh_rate_hz) && info.refresh_rate_hz > 0.0
                ? info.refresh_rate_hz
                : 60.0;
        const auto period = std::chrono::nanoseconds(
            static_cast<i64>(1'000'000'000.0 / refresh_hz));
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now);
        const auto remainder = elapsed % period;
        {
            ApiUnlock unlocked(*this);
            std::this_thread::sleep_for(period - remainder);
        }
        m.set_return(dxgi::S_OK);
        return;
    }
    case 11: {
        // TakeOwnership(pDevice, Exclusive): exclusive output ownership is
        // what full-screen mode rests on, and Vitreo never grants it (see
        // SetFullscreenState). Learn: apps should not call this directly.
        static bool logged = false;
        if (!logged) {
            logged = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED dxgi!IDXGIOutput::"
                         "TakeOwnership: exclusive output ownership -> "
                         "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE\n");
        }
        m.set_return(m.arg_ptr(1) == 0 ? dxgi::E_INVALIDARG
                                       : 0x887A0022u);
        return;
    }
    case 12:
        // ReleaseOwnership has no return value.
        m.set_return(0);
        return;
    case 28: {
        // Learn IDXGIOutput6::CheckHardwareCompositionSupport: pFlags receives
        // DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAGS. The presenter composites
        // through Core Animation, with no hardware overlay stretching of a
        // fullscreen, windowed or cursor plane, so no flag is set.
        const GuestAddr flags = m.arg_ptr(1);
        if (flags == 0) {
            m.set_return(dxgi::E_INVALIDARG);
            return;
        }
        if (m_memory.write_u32(flags, 0).is_err()) {
            m.set_return(dxgi::E_INVALIDARG);
            return;
        }
        m.set_return(dxgi::S_OK);
        return;
    }
    default:
        dxgi_diag("IDXGIOutput vtable slot %zu -> E_NOTIMPL", slot);
        m.set_return(dxgi::E_NOTIMPL);
        return;
    }
}

} // namespace vitreo::winapi
