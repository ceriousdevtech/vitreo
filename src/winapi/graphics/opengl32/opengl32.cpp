// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — OPENGL32.dll.
//
// Every export of Windows' opengl32.dll is registered from the generated
// gl11_exports.inc. GL 1.1 exports run Vitreo's own GL implementation
// (gl_exports.cpp -> gl_cmd_*.cpp) against the calling thread's current
// context. WGL contexts are in gl_wgl.cpp.

#include "opengl32.hpp"
#include "gl_commands.hpp"
#include "gl_context.hpp"
#include "gl_drawable.hpp"
#include "gl_exports.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace vitreo::winapi {

namespace {

constexpr u32 kErrorInvalidFunction = 1;
constexpr u32 kErrorInvalidPixelFormat = 2000;
constexpr u32 kWglSwapMainPlane = 0x00000001;      // wingdi.h WGL_SWAP_MAIN_PLANE
constexpr u32 kWglSwapMultipleMax = 16;            // wingdi.h WGL_SWAPMULTIPLE_MAX

}  // namespace

// ---------------------------------------------------------------- WGL --------

// Contexts, sharing and the current-context rules are in gl_wgl.cpp; the
// window drawable in gl_drawable.cpp.
//
// The swap calls, measured on Windows 11 (tools/oracle_probe/
// wgl_contract_probe64.expected swap-*, swapmulti-*; gfx_media_exports_probe
// .expected). A success leaves the last error alone: the probe's stray gle 6
// on a main-plane success (swap-buffers ret=1 gle=6) is not reproduced,
// because the gfx_media_exports_probe run of the same call leaves it
// untouched, so the condition that sets it is unknown.

void Opengl32Manager::swap_main_plane(u64 hdc) {
    const u64 hwnd = window_of_dc(hdc);
    const std::shared_ptr<gl::GlDrawable> drawable = gl::GlDrawable::for_window(hwnd);
    if (!drawable) return;   // never made current: nothing has been drawn
    (void)drawable->swap();
    // Review Focus 2: the drawable follows the client area at the swap, after
    // the frame drawn at the old size has been presented.
    i32 w = 0, h = 0;
    if (client_size_of_dc(hdc, w, h) && w >= 0 && h >= 0) {
        drawable->ensure_size(static_cast<u32>(w), static_cast<u32>(h));
    }
}

// BOOL SwapBuffers(HDC) == BOOL wglSwapBuffers(HDC). swap-buffers-null-hdc,
// -no-format: FALSE, ERROR_INVALID_HANDLE. A single-buffered format has no
// back buffer: FALSE with the last error untouched when no context or another
// window's context is current (-no-ctx-single, -single-while-other-current),
// TRUE when the calling thread's context draws to this window
// (swap_hint_probe64.expected hint-single-swapbuffers ret=1 lasterr=0); that
// call shows the window as glFlush does. A double-buffered one swaps, with or
// without a current context and for any window (-no-ctx-double,
// -other-window): TRUE. A DC with no window: TRUE (-memdc).
bool Opengl32Manager::swap_buffers(u64 hdc) {
    const i32 index = hdc != 0 ? pixel_format_of_dc(hdc) : 0;
    const gl::GlPixelFormat* format = index != 0 ? gl::gl_pixel_format(index) : nullptr;
    if (format == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return false;
    }
    // swap-buffers-memdc ret=1 gle=untouched: a DC without a window (a memory
    // DC) with a pixel format has nothing to swap and succeeds, whatever its
    // buffering.
    const u64 hwnd = window_of_dc(hdc);
    if (hwnd == 0) return true;
    if (!format->double_buffer) {
        gl::GlContext* ctx = gl::current_gl_context();
        if (ctx == nullptr || !ctx->drawable) return false;
        const std::shared_ptr<gl::GlDrawable> drawable = gl::GlDrawable::for_window(hwnd);
        if (drawable != ctx->drawable) return false;
        drawable->flush();
        return true;
    }
    swap_main_plane(hdc);
    return true;
}

void Opengl32Manager::impl_wglSwapBuffers(Marshaler& m) {
    m.set_return(swap_buffers(m.arg(0)) ? 1 : 0);
}

// BOOL wglSwapLayerBuffers(HDC, UINT fuPlanes). No pixel format has a layer
// plane, so only WGL_SWAP_MAIN_PLANE swaps anything, but a double-buffered DC
// returns TRUE for every fuPlanes (swap-layer-overlay1, -underlay1, -zero,
// -all-bits). A single-buffered DC: FALSE, untouched (-main-plane-single). A
// NULL hdc or a DC without a format: FALSE with the error GetPixelFormat gives
// it: ERROR_INVALID_PIXEL_FORMAT once a SetPixelFormat has succeeded in the
// process (-main-plane-no-format, -null-hdc gle=2000), untouched before
// (gfx_media_exports_probe wglSwapLayerBuffers(NULL,1), (dc,no pf) err=0xdead).
void Opengl32Manager::impl_wglSwapLayerBuffers(Marshaler& m) {
    const u64 hdc = m.arg(0);
    const u32 planes = m.arg32(1);
    const i32 index = hdc != 0 ? pixel_format_of_dc(hdc) : 0;
    const gl::GlPixelFormat* format = index != 0 ? gl::gl_pixel_format(index) : nullptr;
    if (format == nullptr) {
        if (m_pixel_format_ever_set && m_pixel_format_ever_set()) set_last_error(kErrorInvalidPixelFormat);
        m.set_return(0);
        return;
    }
    if (!format->double_buffer) { m.set_return(0); return; }
    if ((planes & kWglSwapMainPlane) != 0) swap_main_plane(hdc);
    m.set_return(1);
}

// DWORD wglSwapMultipleBuffers(UINT n, const WGLSWAP*). WGLSWAP is
// {HDC hdc; UINT uiFlags;}: 8 bytes on x86, 16 on x64. The return is the mask
// of the entries that succeeded, bit i for entry i (swapmulti-two-main 0x3,
// -bad-then-good 0x2, -max16 0xffff). n = 0: 0, untouched (-zero-*). n above
// WGL_SWAPMULTIPLE_MAX: 0 with ERROR_INVALID_FUNCTION and nothing swapped
// (-over-max17, -over-max20). Each entry is a wglSwapLayerBuffers except for
// its errors: a NULL hdc sets ERROR_INVALID_HANDLE (-null-hdc), a DC without
// a format or with a single-buffered one fails with the last error untouched
// (-no-format, -single); any uiFlags succeeds on a double-buffered DC and only
// WGL_SWAP_MAIN_PLANE swaps (-one-overlay1, -one-flags0, -one-all-bits).
// Windows faults on an unreadable array (-null-array EXC=0xc0000005);
// Vitreo stops at the first unreadable entry and returns the mask so far.
void Opengl32Manager::impl_wglSwapMultipleBuffers(Marshaler& m) {
    const u32 n = m.arg32(0);
    const GuestAddr array = m.arg_ptr(1);
    if (n > kWglSwapMultipleMax) {
        set_last_error(kErrorInvalidFunction);
        m.set_return(0);
        return;
    }
    const bool x64 = m.cpu_is_64bit();
    const u32 stride = x64 ? 16u : 8u;
    u32 result = 0;
    for (u32 i = 0; i < n; ++i) {
        const GuestAddr entry = array + static_cast<GuestAddr>(i) * stride;
        const auto hdc = m.read_ptr(entry);
        const auto flags = m.read_u32(entry + (x64 ? 8u : 4u));
        if (array == 0 || !hdc.is_ok() || !flags.is_ok()) {
            gl::gl_log_unimplemented_once("wglSwapMultipleBuffers",
                                          "unreadable WGLSWAP array (Windows faults)");
            break;
        }
        if (hdc.value() == 0) { set_last_error(error_codes::ERROR_INVALID_HANDLE); continue; }
        const i32 index = pixel_format_of_dc(hdc.value());
        const gl::GlPixelFormat* format = index != 0 ? gl::gl_pixel_format(index) : nullptr;
        if (format == nullptr || !format->double_buffer) continue;
        if ((flags.value() & kWglSwapMainPlane) != 0) swap_main_plane(hdc.value());
        result |= 1u << i;
    }
    m.set_return(result);
}

// ---------------------------------------------------------------- export ----

namespace {

/// The handler for a GL 1.1 row: the no-context path, then the command (or,
/// for a command a later task implements, the interim placeholder).
ApiHandler make_gl_handler(Opengl32Manager& mgr, const char* name, const char* kinds,
                           gl::GlHandler handler = nullptr) {
    const gl::GlExportSlots slots = gl::gl_export_slots(kinds);
    const bool is_get_error = std::strcmp(name, "glGetError") == 0;
    if (handler == nullptr) handler = gl::gl_export_handler(name);
    // Spec 5.4: whether a call is compiled into an open display list.
    const gl::GlListRule list_rule = gl::gl_list_rule(name);
    return [&mgr, name, slots, is_get_error, handler, list_rule](Marshaler& m) {
        gl::GlContext* ctx = gl::current_gl_context();
        if (ctx == nullptr) {
            // opengl32's no-context table: nothing happens, outputs stay
            // untouched, the last error is untouched, glGetError answers
            // GL_INVALID_OPERATION on every call and every other export 0/NULL
            // (wgl_contract_probe64.expected: noctx-*).
            m.set_return(is_get_error ? gl::GL_INVALID_OPERATION : 0u);
            return;
        }
        if (handler == nullptr) {
            gl::gl_log_unimplemented_once(name, "implemented in a later task");
            ctx->record_error(gl::GL_INVALID_OPERATION);
            m.set_return(0);
            return;
        }
        m.set_return(0);
        gl::GlCall call(m, *ctx, slots, mgr.scratch_base(), list_rule);
        handler(call);
    };
}

/// A non-GL export with no implementation yet: fails (FALSE/NULL) loudly.
ApiHandler make_other_placeholder(const char* name) {
    const bool later_task = std::strncmp(name, "wgl", 3) == 0;
    return [name, later_task](Marshaler& m) {
        gl::gl_log_unimplemented_once(name, later_task ? "implemented in a later task"
                                                       : "not implemented (no OpenGL metafile support)");
        m.set_return(0);
    };
}

}  // namespace

DllExports create_opengl32_thunks(Opengl32Manager& mgr) {
    DllExports exports;
    auto add = [&](const char* name, u16 ordinal, u16 stdcall_bytes, ApiHandler handler) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(handler), stdcall_bytes});
    };
    auto wgl = [&mgr](void (Opengl32Manager::*impl)(Marshaler&)) -> ApiHandler {
        return [&mgr, impl](Marshaler& m) { (mgr.*impl)(m); };
    };

#define GL11_EXPORT(name, ordinal, stack_bytes, ret, kinds) \
    add(#name, ordinal, stack_bytes, make_gl_handler(mgr, #name, kinds));
#define GL11_OTHER_EXPORT(name, ordinal, stack_bytes) \
    add(#name, ordinal, stack_bytes, make_other_placeholder(#name));
#include "gl11_exports.inc"
#undef GL11_EXPORT
#undef GL11_OTHER_EXPORT

    // The WGL entry points that have behaviour today.
    const std::pair<const char*, void (Opengl32Manager::*)(Marshaler&)> kWgl[] = {
        {"wglCreateContext", &Opengl32Manager::impl_wglCreateContext},
        {"wglDeleteContext", &Opengl32Manager::impl_wglDeleteContext},
        {"wglMakeCurrent", &Opengl32Manager::impl_wglMakeCurrent},
        {"wglGetCurrentContext", &Opengl32Manager::impl_wglGetCurrentContext},
        {"wglGetCurrentDC", &Opengl32Manager::impl_wglGetCurrentDC},
        {"wglShareLists", &Opengl32Manager::impl_wglShareLists},
        {"wglCreateLayerContext", &Opengl32Manager::impl_wglCreateLayerContext},
        {"wglCopyContext", &Opengl32Manager::impl_wglCopyContext},
        {"wglChoosePixelFormat", &Opengl32Manager::impl_wglChoosePixelFormat},
        {"wglDescribePixelFormat", &Opengl32Manager::impl_wglDescribePixelFormat},
        {"wglGetPixelFormat", &Opengl32Manager::impl_wglGetPixelFormat},
        {"wglSetPixelFormat", &Opengl32Manager::impl_wglSetPixelFormat},
        {"wglDescribeLayerPlane", &Opengl32Manager::impl_wglDescribeLayerPlane},
        {"wglSetLayerPaletteEntries", &Opengl32Manager::impl_wglSetLayerPaletteEntries},
        {"wglGetLayerPaletteEntries", &Opengl32Manager::impl_wglGetLayerPaletteEntries},
        {"wglRealizeLayerPalette", &Opengl32Manager::impl_wglRealizeLayerPalette},
        {"wglGetDefaultProcAddress", &Opengl32Manager::impl_wglGetDefaultProcAddress},
        {"wglUseFontBitmapsA", &Opengl32Manager::impl_wglUseFontBitmapsA},
        {"wglUseFontBitmapsW", &Opengl32Manager::impl_wglUseFontBitmapsW},
        {"wglUseFontOutlinesA", &Opengl32Manager::impl_wglUseFontOutlinesA},
        {"wglUseFontOutlinesW", &Opengl32Manager::impl_wglUseFontOutlinesW},
        {"wglGetProcAddress", &Opengl32Manager::impl_wglGetProcAddress},
        {"wglSwapBuffers", &Opengl32Manager::impl_wglSwapBuffers},
        {"wglSwapLayerBuffers", &Opengl32Manager::impl_wglSwapLayerBuffers},
        {"wglSwapMultipleBuffers", &Opengl32Manager::impl_wglSwapMultipleBuffers},
    };
    for (auto& entry : exports.entries) {
        for (const auto& [name, impl] : kWgl) {
            if (entry.name == name) entry.handler = wgl(impl);
        }
    }

    // The entry points wglGetProcAddress / wglGetDefaultProcAddress hand out
    // (GL_EXT_paletted_texture, GL_WIN_swap_hint, glDrawRangeElementsWIN).
    // GDI Generic serves them from inside opengl32, not from its export table
    // (gl11_state_probe64.expected ext-gpa; opengl32 has no such exports), so
    // they are internal entries: absent from the export table, GetProcAddress
    // and every module listing, reachable only through extension_address().
    for (const gl::GlExtensionEntry& e : gl::gl_extension_entries()) {
        const u16 bytes = static_cast<u16>(4 * std::strlen(e.kinds));   // no double arguments
        // The name's storage outlives the handler (gl_extension_entries is static).
        ExportEntry entry{std::string(e.name), 0,
                          make_gl_handler(mgr, e.name.data(), e.kinds, e.handler), bytes};
        entry.internal = true;
        exports.entries.push_back(std::move(entry));
    }
    return exports;
}

} // namespace vitreo::winapi
