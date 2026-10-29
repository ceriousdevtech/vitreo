#pragma once
/// @file opengl32.hpp
/// @brief OPENGL32.dll: Vitreo's own OpenGL 1.1 implementation and WGL.
///
/// Every export in gl11_exports.inc is registered. GL 1.1 exports marshal
/// their arguments into the internal command functions (gl_exports.hpp,
/// gl_commands.hpp) against the calling thread's current context
/// (current_gl_context()); with no current context every gl* export behaves
/// as Windows' no-context dispatch table does. The WGL exports keep their
/// WGL contexts, sharing and the current-context rules are in gl_wgl.cpp; the
/// window drawable and the swap calls in gl_drawable.cpp / opengl32.cpp.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "winapi/graphics/opengl32/gl_drawable.hpp"

#include <functional>
#include <memory>

namespace vitreo::winapi {

class Marshaler;
struct WglState;
namespace gl { class GlContext; }

// Small scratch region used to publish guest-visible static strings
// (glGetString results). Mapped during DLL registration.
//
// This is only the PREFERRED base. A guest may legitimately VirtualAlloc the
// same fixed range before Vitreo registers the DLL — a guest may claim
// 0x5E000000 plus 1 MiB outright — so the reservation falls back to any free
// gap and publishes the real base through set_scratch_base().
static constexpr GuestAddr OPENGL32_SCRATCH_BASE = 0x5ED10000;
static constexpr u32       OPENGL32_SCRATCH_SIZE = 0x1000;

class Opengl32Manager {
public:
    Opengl32Manager();
    ~Opengl32Manager();
    Opengl32Manager(const Opengl32Manager&) = delete;
    Opengl32Manager& operator=(const Opengl32Manager&) = delete;

    void set_thunk_registry(ThunkRegistry* registry) noexcept {
        m_registry = registry;
    }

    /// gdi32's pixel-format bookkeeping (GetPixelFormat): wglSwapBuffers
    /// fails on a DC whose window has no pixel format.
    void set_pixel_format_query(std::function<i32(u64 hdc)> query) {
        m_pixel_format_of = std::move(query);
    }

    /// gdi32's pixel-format calls, which wglChoosePixelFormat,
    /// wglDescribePixelFormat, wglGetPixelFormat and wglSetPixelFormat run
    /// (one code path with ChoosePixelFormat etc.; pixel_format_api.hpp). Each
    /// reads its arguments from the marshaler. Unset: those exports fail.
    struct PixelFormatApi {
        std::function<void(Marshaler&)> choose, describe, get, set;
    };
    void set_pixel_format_api(PixelFormatApi api) { m_pixel_format_api = std::move(api); }

    /// The window behind a DC and its client size, for the first
    /// wglMakeCurrent of a context (the initial viewport and scissor box) and
    /// for the drawable (Task 6). @p window_of returns the HWND (0 when the DC
    /// has none); @p client_size writes the drawable's width/height in pixels
    /// for a window DC (client area) or a memory DC (its bitmap).
    void set_drawable_queries(std::function<u64(u64 hdc)> window_of,
                              std::function<bool(u64 hdc, i32& width, i32& height)> client_size) {
        m_window_of_dc = std::move(window_of);
        m_client_size_of_dc = std::move(client_size);
    }
    [[nodiscard]] u64 window_of_dc(u64 hdc) const {
        return m_window_of_dc ? m_window_of_dc(hdc) : 0;
    }
    [[nodiscard]] bool client_size_of_dc(u64 hdc, i32& width, i32& height) const {
        return m_client_size_of_dc && m_client_size_of_dc(hdc, width, height);
    }
    [[nodiscard]] i32 pixel_format_of_dc(u64 hdc) const {
        return m_pixel_format_of ? m_pixel_format_of(hdc) : 0;
    }

    /// gdi32's "has any SetPixelFormat succeeded in this process"
    /// (GdiManager::pixel_format_ever_set): wglSwapLayerBuffers then fails a
    /// DC without a format with ERROR_INVALID_PIXEL_FORMAT, as GetPixelFormat
    /// does. Unset: false.
    void set_pixel_format_ever_set_query(std::function<bool()> query) {
        m_pixel_format_ever_set = std::move(query);
    }

    /// Makes a window's presentation target (production: the HWND's native
    /// view through the backend's create_swap_chain, as D3D9 does). Unset:
    /// GL windows render but present nothing.
    void set_swap_chain_factory(gl::GlSwapChainFactory factory) { m_swap_chain_factory = std::move(factory); }

    /// SwapBuffers(hdc), which gdi32!SwapBuffers and wglSwapBuffers both run:
    /// the measured result and last error (wgl_contract_probe64.expected
    /// swap-buffers-*), the exchange and present of the window's drawable,
    /// then a resize to the client area (Review Focus 2).
    [[nodiscard]] bool swap_buffers(u64 hdc);

    /// Where the scratch region actually landed. Defaults to the preferred
    /// base; DLL registration overrides it when that address was taken.
    void set_scratch_base(GuestAddr base) noexcept { m_scratch_base = base; }
    [[nodiscard]] GuestAddr scratch_base() const noexcept { return m_scratch_base; }

    /// Tests only: the GlContext behind an HGLRC (nullptr when the handle is
    /// not a live context). No production code may call it: the pointer is
    /// returned after the WGL lock is released, so it is valid only while the
    /// caller itself guarantees no wglDeleteContext of @p hglrc can run (a
    /// single-threaded test, or the context current on the calling thread).
    [[nodiscard]] gl::GlContext* context_for_testing(u64 hglrc) const;

    // ---- WGL (gl_wgl.cpp): HGLRCs are handle-table entries holding a
    // GlContext, its pixel format and its share group; the current context is
    // thread-local. ----
    void impl_wglCreateContext(Marshaler& m);
    void impl_wglCreateLayerContext(Marshaler& m);
    void impl_wglCopyContext(Marshaler& m);
    void impl_wglChoosePixelFormat(Marshaler& m);
    void impl_wglDescribePixelFormat(Marshaler& m);
    void impl_wglGetPixelFormat(Marshaler& m);
    void impl_wglSetPixelFormat(Marshaler& m);
    void impl_wglDescribeLayerPlane(Marshaler& m);
    void impl_wglSetLayerPaletteEntries(Marshaler& m);
    void impl_wglGetLayerPaletteEntries(Marshaler& m);
    void impl_wglRealizeLayerPalette(Marshaler& m);
    void impl_wglGetDefaultProcAddress(Marshaler& m);
    void impl_wglUseFontBitmapsA(Marshaler& m);
    void impl_wglUseFontBitmapsW(Marshaler& m);
    void impl_wglUseFontOutlinesA(Marshaler& m);
    void impl_wglUseFontOutlinesW(Marshaler& m);
    void impl_wglDeleteContext(Marshaler& m);
    void impl_wglMakeCurrent(Marshaler& m);
    void impl_wglGetCurrentContext(Marshaler& m);
    void impl_wglGetCurrentDC(Marshaler& m);
    void impl_wglShareLists(Marshaler& m);
    void impl_wglGetProcAddress(Marshaler& m);
    void impl_wglSwapBuffers(Marshaler& m);
    void impl_wglSwapLayerBuffers(Marshaler& m);
    void impl_wglSwapMultipleBuffers(Marshaler& m);

    /// wglMakeCurrent's drawable rule: attaches the drawable of @p hdc's
    /// window to @p ctx (sized to the client area), or releases it when
    /// @p hdc is 0. The previous drawable is flushed (Learn wglMakeCurrent).
    void bind_drawable(gl::GlContext& ctx, u64 hdc);

    /// The thunk address of the extension entry point named at @p name_addr,
    /// or 0 (wglGetProcAddress / wglGetDefaultProcAddress).
    [[nodiscard]] GuestAddr extension_address(Marshaler& m, GuestAddr name_addr) const;

private:
    GuestAddr m_scratch_base{OPENGL32_SCRATCH_BASE};
    ThunkRegistry* m_registry{nullptr};
    std::function<i32(u64)> m_pixel_format_of;
    PixelFormatApi m_pixel_format_api;
    std::function<u64(u64)> m_window_of_dc;
    std::function<bool(u64, i32&, i32&)> m_client_size_of_dc;
    std::function<bool()> m_pixel_format_ever_set;
    gl::GlSwapChainFactory m_swap_chain_factory;

    /// The swap of one main plane on a double-buffered DC: the drawable's
    /// exchange and present, then the resize.
    void swap_main_plane(u64 hdc);
    std::shared_ptr<WglState> m_wgl;
};

/// Every opengl32.dll export, from gl11_exports.inc (names, ordinals, x86
/// stack bytes), plus the extension entry points wglGetProcAddress /
/// wglGetDefaultProcAddress hand out (gl::gl_extension_entries()) as
/// internal entries: Windows serves those from opengl32's internals, not its
/// export table, so they are not opengl32.dll exports here either.
DllExports create_opengl32_thunks(Opengl32Manager& mgr);

} // namespace vitreo::winapi
