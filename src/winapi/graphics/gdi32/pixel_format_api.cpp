// SPDX-License-Identifier: LGPL-2.1-only
/// @file pixel_format_api.cpp
/// @brief ChoosePixelFormat / DescribePixelFormat / GetPixelFormat /
///        SetPixelFormat, shared by gdi32 and opengl32 (see the header).

#include "winapi/graphics/gdi32/pixel_format_api.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {

namespace {
constexpr u32 kErrorInvalidPixelFormat = 2000;
}

// https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-choosepixelformat
void pixel_format_choose(GdiManager&, Marshaler& m) {
    // wgl_contract_probe64.expected choose-null-hdc / wglpf-choose-null-hdc: 0
    // with ERROR_INVALID_HANDLE. choose-null-ppfd faults on Windows
    // (wglpf-choose-null-ppfd EXC=0xc0000005); Vitreo refuses with 0,
    // ERROR_INVALID_PARAMETER instead of faulting.
    if (m.arg(0) == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const GuestAddr ppfd = m.arg_ptr(1);
    if (ppfd == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    // Only the fields that decide the choice are read; nSize and nVersion
    // are not validated (choose-nsize-*, choose-nversion-*,
    // wglpf-choose-nsize0). An unreadable descriptor is a request of zeros.
    u8 pfd[gl::kPixelFormatDescriptorSize]{};
    (void)m.memory().read_bytes(ppfd, pfd, sizeof pfd);
    gl::PixelFormatRequest request;
    request.flags = static_cast<u32>(pfd[4]) | (static_cast<u32>(pfd[5]) << 8) |
                    (static_cast<u32>(pfd[6]) << 16) | (static_cast<u32>(pfd[7]) << 24);
    request.pixel_type = pfd[8];
    request.color_bits = pfd[9];
    request.alpha_bits = pfd[16];
    request.accum_bits = pfd[18];
    request.depth_bits = pfd[23];
    request.stencil_bits = pfd[24];
    m.set_return(static_cast<u32>(gl::choose_gl_pixel_format(request)));
}

// https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-describepixelformat
void pixel_format_describe(GdiManager&, Marshaler& m) {
    // describe-* / wglpf-describe-*: NULL hdc is 0 with ERROR_INVALID_HANDLE
    // (also when only counting); a NULL ppfd or nBytes 0 only counts the
    // formats (nothing written, last error untouched); an index outside
    // 1..count, or nBytes 1..39, is 0 with ERROR_INVALID_PARAMETER and the
    // struct untouched; nBytes >= 40 writes exactly 40 bytes.
    if (m.arg(0) == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    const auto count = static_cast<u32>(gl::gl_pixel_formats().size());
    const i32 format = static_cast<i32>(m.arg32(1));
    const u32 nbytes = m.arg32(2);
    const GuestAddr ppfd = m.arg_ptr(3);
    if (ppfd == 0 || nbytes == 0) {
        m.set_return(count);
        return;
    }
    const gl::GlPixelFormat* described = gl::gl_pixel_format(format);
    if (described == nullptr || nbytes < gl::kPixelFormatDescriptorSize) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto descriptor = gl::gl_pixel_format_descriptor(*described);
    (void)m.memory().write_bytes(ppfd, descriptor.data(), descriptor.size());
    m.set_return(count);
}

void pixel_format_get(GdiManager& gdi, Marshaler& m) {
    const u32 format = static_cast<u32>(gdi.pixel_format(static_cast<Handle>(m.arg(0))));
    // wglpf-getpf-unset, wglpf-getpf-null-hdc: a DC without a format (or NULL)
    // is 0 with ERROR_INVALID_PIXEL_FORMAT once any SetPixelFormat has
    // succeeded in the process; before that the last error is untouched
    // (getpf-unset, and a fresh process running the same call).
    if (format == 0 && gdi.pixel_format_ever_set()) {
        set_last_error(kErrorInvalidPixelFormat);
    }
    m.set_return(format);
}

void pixel_format_set(GdiManager& gdi, Marshaler& m) {
    // A NULL DC fails with ERROR_INVALID_HANDLE; an index outside 1..count is
    // ERROR_INVALID_PARAMETER; a window's format is set once: the same format
    // again is TRUE, another one is FALSE with ERROR_INVALID_PIXEL_FORMAT
    // (setpf-*, wglpf-setpf-*, gfx_media_exports_probe.expected).
    const auto hdc = static_cast<Handle>(m.arg(0));
    const auto format = static_cast<i32>(m.arg32(1));
    const u32 error = gdi.set_pixel_format(hdc, format,
                                           static_cast<i32>(gl::gl_pixel_formats().size()));
    if (error != 0) { set_last_error(error); m.set_return(0); return; }
    m.set_return(1);  // TRUE
}

}  // namespace vitreo::winapi
