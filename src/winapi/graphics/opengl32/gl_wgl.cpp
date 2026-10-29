// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo — OPENGL32.dll WGL contexts, sharing and the current-context rules.
//
// An HGLRC is an entry in a handle table holding a GlContext, the pixel format
// it was created for and its share group. The current context is thread-local;
// wglMakeCurrent installs it through set_current_gl_context_for_thread so every
// gl* export reaches it through current_gl_context(). There is no host OpenGL
// here: the contexts are Vitreo objects.
//
// Every rule below cites the Windows measurement it implements
// (tools/oracle_probe/wgl_contract_probe64.expected, identical in the 32-bit
// file). A success return leaves the last error alone (the probes print
// "gle=untouched").

#include "gl_wgl.hpp"
#include "gl_exports.hpp"
#include "gl_commands.hpp"
#include "gl_context.hpp"
#include "gl_drawable.hpp"
#include "gl_pixel_formats.hpp"
#include "opengl32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kErrorInvalidHandle = 6;
constexpr u32 kErrorNotSupported = 50;
constexpr u32 kErrorInvalidParameter = 87;
constexpr u32 kErrorBusy = 170;               // measured: context current on another thread
constexpr u32 kErrorProcNotFound = 127;
constexpr u32 kErrorInvalidPixelFormat = 2000;

}  // namespace

struct WglContext {
    u64 handle{0};
    i32 pixel_format{0};
    std::unique_ptr<gl::GlContext> gl;
    bool current{false};            ///< current on some thread
    std::thread::id owner;          ///< that thread
};

struct WglState {
    std::mutex mutex;
    std::unordered_map<u64, WglContext> table;
    u64 next_handle{0x00010000};

    WglContext* find(u64 handle) {
        const auto it = table.find(handle);
        return it == table.end() ? nullptr : &it->second;
    }
    [[nodiscard]] bool current_elsewhere(const WglContext& c) const {
        return c.current && c.owner != std::this_thread::get_id();
    }
    [[nodiscard]] usize members_of(const gl::GlShareGroup* group) const {
        usize n = 0;
        for (const auto& [h, c] : table) {
            if (c.gl->share_group.get() == group) ++n;
        }
        return n;
    }
};

namespace {

/// Lock order (opengl32; .docs/opengl-implementation.md "Lock order"):
///
///   WglState mutex  ->  gdi32 / HwndManager locks (the DC queries
///                       wglMakeCurrent and wglCreateContext make; each copies
///                       what it reads and returns)
///   drawable registry mutex (GlDrawable::attach / detach), alone
///   drawable mutex  ->  share-group lock (a pending batch records textures)
///   present lock (try-locked only)  ->  drawable mutex, briefly
///
/// - The WglState mutex is never held while a drawable's mutex is taken: a
///   released context's drawable is flushed after the unlock
///   (FlushReleasedOnExit), wglMakeCurrent binds the new drawable after it,
///   and wglDeleteContext destroys the GlContext (whose batcher records its
///   pending draws under the drawable's mutex) after it.
/// - No drawable lock is held while anything waits for the main thread:
///   creating, resizing, presenting into and destroying a window's swap
///   chain synchronise with it (MetalSwapchain's run_on_main_sync), and the
///   main thread takes the drawable mutex itself: DestroyWindow's
///   WindowManager destroyed callback runs GlDrawable::detach ->
///   window_destroyed, and a GL call there on a context bound to the window
///   draws. GlDrawable presents after releasing its mutex, under the present
///   lock, which nothing ever blocks on (gl_drawable.hpp, Locks).
thread_local std::vector<std::shared_ptr<gl::GlDrawable>> t_released;

/// Declared BEFORE the lock_guard of the WglState mutex, so its destructor
/// runs after the unlock: flushes what the call released.
struct FlushReleasedOnExit {
    FlushReleasedOnExit() = default;
    FlushReleasedOnExit(const FlushReleasedOnExit&) = delete;
    FlushReleasedOnExit& operator=(const FlushReleasedOnExit&) = delete;
    ~FlushReleasedOnExit() {
        std::vector<std::shared_ptr<gl::GlDrawable>> released;
        released.swap(t_released);
        for (auto& d : released) d->flush();
    }
};

/// A context stops being current: Learn wglMakeCurrent, "OpenGL flushes any
/// previous rendering context that was current to the calling thread", so its
/// open pass is submitted (and a single-buffered window shows it). The
/// drawable is moved out here (lock held) and flushed by FlushReleasedOnExit
/// after the WglState mutex is released.
void release_context(WglContext& c) {
    c.current = false;
    if (c.gl->drawable) t_released.push_back(std::move(c.gl->drawable));
    c.gl->drawable.reset();
}

/// The calling thread's binding. Its destructor releases the context when the
/// thread ends, so a context is not left owned by a dead thread.
struct ThreadBinding {
    gl::WglBinding binding;
    std::weak_ptr<WglState> state;

    ~ThreadBinding() {
        if (binding.hglrc == 0) return;
        if (const auto s = state.lock()) {
            std::shared_ptr<gl::GlDrawable> drawable;
            {
                std::lock_guard<std::mutex> lock(s->mutex);
                if (WglContext* c = s->find(binding.hglrc)) {
                    c->current = false;
                    drawable = std::move(c->gl->drawable);
                    c->gl->drawable.reset();
                }
            }
            if (drawable) drawable->flush();
        }
    }
};
thread_local ThreadBinding t_thread;

/// Releases the calling thread's current context of @p st (lock held).
void release_current_locked(WglState& st) {
    if (t_thread.binding.hglrc == 0) return;
    if (t_thread.state.lock().get() == &st) {
        if (WglContext* c = st.find(t_thread.binding.hglrc)) release_context(*c);
    }
    t_thread.binding = {};
    t_thread.state.reset();
    gl::set_current_gl_context_for_thread(nullptr);
}

}  // namespace

namespace gl {

u64 wgl_handle_of(const GlContext* ctx) noexcept { return ctx != nullptr ? ctx->wgl_handle : 0; }

WglBinding wgl_current_binding() noexcept { return t_thread.binding; }

}  // namespace gl

Opengl32Manager::Opengl32Manager() : m_wgl(std::make_shared<WglState>()) {}

Opengl32Manager::~Opengl32Manager() {
    // Do not leave this thread pointing at a context that dies with the table.
    FlushReleasedOnExit flush_released;
    std::lock_guard<std::mutex> lock(m_wgl->mutex);
    release_current_locked(*m_wgl);
}

gl::GlContext* Opengl32Manager::context_for_testing(u64 hglrc) const {
    std::lock_guard<std::mutex> lock(m_wgl->mutex);
    const WglContext* c = m_wgl->find(hglrc);
    return c != nullptr ? c->gl.get() : nullptr;
}

namespace {

/// wglCreateContext / wglCreateLayerContext(layer 0). Returns the HGLRC or 0
/// with the last error set.
u64 create_context(Opengl32Manager& mgr, WglState& st, u64 hdc) {
    // create-null-hdc: NULL, 6. create-no-pixel-format / -memdc-no-format: NULL, 2000.
    if (hdc == 0) { set_last_error(kErrorInvalidHandle); return 0; }
    const i32 index = mgr.pixel_format_of_dc(hdc);
    const gl::GlPixelFormat* format = index != 0 ? gl::gl_pixel_format(index) : nullptr;
    if (format == nullptr) { set_last_error(kErrorInvalidPixelFormat); return 0; }
    auto context = std::make_unique<gl::GlContext>(gl::gl_framebuffer_config(*format));
    std::lock_guard<std::mutex> lock(st.mutex);
    const u64 handle = st.next_handle;
    st.next_handle += 4;
    context->wgl_handle = handle;
    WglContext& entry = st.table[handle];
    entry.handle = handle;
    entry.pixel_format = index;
    entry.gl = std::move(context);
    return handle;     // success: the last error is untouched (create-ok)
}

}  // namespace

void Opengl32Manager::impl_wglCreateContext(Marshaler& m) {
    m.set_return(create_context(*this, *m_wgl, m.arg(0)));
}

// HGLRC wglCreateLayerContext(HDC, int iLayerPlane). Windows' generic
// implementation has no layer planes (every pixel format reports cLayer 0:
// wgl_contract_probe64.expected pf-list "layer=0"); layer 0 is the main plane
// and creates a normal context. No probe line measures a non-zero layer;
// Learn only documents NULL on failure, so it fails like an out-of-range
// argument: ERROR_INVALID_PARAMETER (not measured).
void Opengl32Manager::impl_wglCreateLayerContext(Marshaler& m) {
    const u64 hdc = m.arg(0);
    const i32 layer = static_cast<i32>(m.arg32(1));
    if (layer != 0 && hdc != 0 && pixel_format_of_dc(hdc) != 0) {
        set_last_error(kErrorInvalidParameter);
        m.set_return(0);
        return;
    }
    m.set_return(create_context(*this, *m_wgl, hdc));
}

// BOOL wglDeleteContext(HGLRC).
void Opengl32Manager::impl_wglDeleteContext(Marshaler& m) {
    WglState& st = *m_wgl;
    FlushReleasedOnExit flush_released;   // after the unlock, after `doomed`
    // Destroyed after the unlock: ~GlContext's batcher records draws still
    // pending under the drawable's mutex (lock order above; final review,
    // Finding 5). Declared here, so it dies after `lock` and before the flush
    // of the drawable it records into.
    std::unique_ptr<gl::GlContext> doomed;
    std::lock_guard<std::mutex> lock(st.mutex);
    WglContext* c = st.find(m.arg(0));
    if (c == nullptr) {
        // delete-twice / delete-null / delete-garbage: 0, 6.
        set_last_error(kErrorInvalidHandle);
        m.set_return(0);
        return;
    }
    if (st.current_elsewhere(*c)) {
        // delete-second-thread-current-ctx: 0, 170.
        set_last_error(kErrorBusy);
        m.set_return(0);
        return;
    }
    // delete-current: TRUE, and the context is released first (cur-ctx-after=NULL).
    if (c->current && t_thread.binding.hglrc == c->handle) release_current_locked(st);
    // The share group lives on while another context holds it
    // (share-source-deleted-visibility).
    doomed = std::move(c->gl);
    st.table.erase(c->handle);
    m.set_return(1);
}

// BOOL wglMakeCurrent(HDC, HGLRC).
void Opengl32Manager::impl_wglMakeCurrent(Marshaler& m) {
    WglState& st = *m_wgl;
    const u64 hdc = m.arg(0);
    const u64 hglrc = m.arg(1);
    FlushReleasedOnExit flush_released;   // after the unlock
    std::unique_lock<std::mutex> lock(st.mutex);

    if (hglrc == 0) {
        if (hdc == 0 && t_thread.binding.hglrc == 0) {
            // make-null-null-nothing-current (and on a second thread): FALSE, 6.
            set_last_error(kErrorInvalidHandle);
            m.set_return(0);
            return;
        }
        // make-null-null-releases, make-dc-null-ctx-releases: TRUE;
        // make-dc-null-ctx-nothing-current: TRUE with nothing current.
        release_current_locked(st);
        m.set_return(1);
        return;
    }
    if (hdc == 0) {
        // make-null-dc-ctx-nothing-current / -while-current: FALSE, 6, and the
        // current context stays current.
        set_last_error(kErrorInvalidHandle);
        m.set_return(0);
        return;
    }
    WglContext* c = st.find(hglrc);
    if (c == nullptr) {
        // make-deleted-hglrc / make-garbage-hglrc: FALSE, 6, nothing current after.
        release_current_locked(st);
        set_last_error(kErrorInvalidHandle);
        m.set_return(0);
        return;
    }
    const i32 dc_format = pixel_format_of_dc(hdc);
    if (dc_format == 0 || dc_format != c->pixel_format) {
        // make-dc-without-pixel-format, make-mismatched-format(-rev), -while-current,
        // make-window-format-ctx-onto-bitmap-dc: FALSE, 2000; the previously
        // current context is released.
        release_current_locked(st);
        set_last_error(kErrorInvalidPixelFormat);
        m.set_return(0);
        return;
    }
    if (st.current_elsewhere(*c)) {
        // make-second-thread-same-ctx-same-dc / -other-dc: FALSE, 170.
        release_current_locked(st);
        set_last_error(kErrorBusy);
        m.set_return(0);
        return;
    }
    if (!(c->current && t_thread.binding.hglrc == hglrc)) {
        release_current_locked(st);    // make-switch-to-ctxB
        c->current = true;
        c->owner = std::this_thread::get_id();
        t_thread.state = m_wgl;
        gl::set_current_gl_context_for_thread(c->gl.get());
        if (!c->gl->has_been_current()) {
            // viewport-initial: the first make-current sets viewport and
            // scissor to the window; later calls (viewport-remake-same,
            // -after-resize-remake) do not.
            i32 w = 0, h = 0;
            if (!client_size_of_dc(hdc, w, h)) { w = 0; h = 0; }
            c->gl->on_first_make_current(w, h);
        }
    }
    // make-same-again, make-same-format-other-window: the DC may change.
    t_thread.binding = gl::WglBinding{hglrc, hdc, dc_format};
    // The DC's window drawable, at the current client size: a resize since
    // the last make-current reallocates it, the viewport stays (Review Focus 2).
    // Bound after the unlock (lock order: never the WglState mutex around a
    // drawable's); the context is current here, so no other thread can
    // delete it meanwhile (delete-second-thread-current-ctx: 170).
    gl::GlContext* const bound = c->gl.get();
    lock.unlock();
    bind_drawable(*bound, hdc);
    m.set_return(1);
}

void Opengl32Manager::bind_drawable(gl::GlContext& ctx, u64 hdc) {
    std::shared_ptr<gl::GlDrawable> drawable;
    if (hdc != 0) {
        i32 w = 0, h = 0;
        if (!client_size_of_dc(hdc, w, h)) { w = 0; h = 0; }
        drawable = gl::GlDrawable::attach(window_of_dc(hdc), pixel_format_of_dc(hdc),
                                          static_cast<u32>(std::max(w, 0)), static_cast<u32>(std::max(h, 0)),
                                          m_swap_chain_factory);
    }
    // The DC changed windows: the old drawable is flushed after the unlock.
    if (ctx.drawable && ctx.drawable != drawable) t_released.push_back(std::move(ctx.drawable));
    ctx.drawable = std::move(drawable);
}

void Opengl32Manager::impl_wglGetCurrentContext(Marshaler& m) {
    m.set_return(t_thread.binding.hglrc);
}

void Opengl32Manager::impl_wglGetCurrentDC(Marshaler& m) {
    m.set_return(t_thread.binding.hdc);
}

// BOOL wglShareLists(HGLRC src, HGLRC dst): dst joins src's share group.
void Opengl32Manager::impl_wglShareLists(Marshaler& m) {
    WglState& st = *m_wgl;
    std::lock_guard<std::mutex> lock(st.mutex);
    WglContext* src = st.find(m.arg(0));
    WglContext* dst = st.find(m.arg(1));
    if (src == nullptr || dst == nullptr) {
        // share-null-src / -null-dst / -deleted-dst: FALSE, 6.
        set_last_error(kErrorInvalidHandle);
        m.set_return(0);
        return;
    }
    if (st.current_elsewhere(*src) || st.current_elsewhere(*dst)) {
        // share-second-thread-current-ctx: FALSE, 170 (checked before the
        // self-share rule: the probe shares a context with itself).
        set_last_error(kErrorBusy);
        m.set_return(0);
        return;
    }
    const gl::GlShareGroup* src_group = src->gl->share_group.get();
    const gl::GlShareGroup* dst_group = dst->gl->share_group.get();
    if (src == dst || src_group == dst_group || st.members_of(dst_group) > 1 ||
        dst_group->has_objects()) {
        // share-self, share-already-shared, share-after-gentextures-dst/-both:
        // FALSE, 87. Success (share-before, share-after-gentextures with only
        // the source populated, share-src-current, share-different-format)
        // leaves the last error alone.
        set_last_error(kErrorInvalidParameter);
        m.set_return(0);
        return;
    }
    // Safe to swap the destination's group here: a context is used only by
    // the thread it is current on, and dst is current on no other thread
    // (refused with 170 above, share-second-thread-current-ctx), nor can it
    // become current meanwhile (wglMakeCurrent takes this same WGL lock). Its
    // old group is no other context's (members_of == 1) and holds no objects,
    // so nothing can be adding to it either.
    dst->gl->share_group = src->gl->share_group;
    dst->gl->bound_objects = {};
    m.set_return(1);
}

// BOOL wglCopyContext(HGLRC src, HGLRC dst, UINT mask): copies the attribute
// groups named by @p mask (glPushAttrib's bits) from src to dst. Learn
// documents TRUE/FALSE only; no probe line measures it. Both handles must be
// valid (6). A source or destination current on another thread is refused
// with the measured busy code of the other calls (170): its state is being
// changed there without a lock, so reading it (the source) is as much a race
// as writing it (final review, Finding 6). Unmeasured on Windows.
void Opengl32Manager::impl_wglCopyContext(Marshaler& m) {
    WglState& st = *m_wgl;
    const u32 mask = m.arg32(2);
    std::lock_guard<std::mutex> lock(st.mutex);
    WglContext* src = st.find(m.arg(0));
    WglContext* dst = st.find(m.arg(1));
    if (src == nullptr || dst == nullptr) {
        set_last_error(kErrorInvalidHandle);
        m.set_return(0);
        return;
    }
    if (st.current_elsewhere(*src) || st.current_elsewhere(*dst)) {
        set_last_error(kErrorBusy);
        m.set_return(0);
        return;
    }
    if (src != dst) gl::gl_copy_attrib_groups(dst->gl->state, src->gl->state, mask);
    m.set_return(1);
}

// The guest address of extension entry point @p name (an internal opengl32
// entry, create_opengl32_thunks),
// or 0 when GDI Generic serves no such name (gl11_state_probe64.expected
// ext-gpa: glAddSwapHintRectWIN, glColorTableEXT, glColorSubTableEXT,
// glGetColorTableEXT, glGetColorTableParameter{iv,fv}EXT,
// glDrawRangeElementsWIN; names are case-sensitive, glcolortableext is NULL).
GuestAddr Opengl32Manager::extension_address(Marshaler& m, GuestAddr name_addr) const {
    if (name_addr == 0 || m_registry == nullptr) return 0;
    auto name = m.read_ansi_string(name_addr);
    if (name.is_err()) return 0;
    for (const gl::GlExtensionEntry& e : gl::gl_extension_entries()) {
        if (e.name == name.value()) {
            auto addr = m_registry->get_thunk_address("opengl32.dll", e.name, true);
            return addr.is_ok() ? addr.value() : 0;
        }
    }
    return 0;
}

// PROC wglGetProcAddress(LPCSTR). GL 1.1 core names are not returned
// (gpa-ctx glVertex3f, glGenTextures, glDrawArrays: NULL). With no current
// context: NULL, ERROR_INVALID_HANDLE and the name is not read (gpa-noctx,
// including name=(NULL)). With a context: the extension entry points GDI
// Generic serves (gpa-ctx glColorTableEXT / glAddSwapHintRectWIN non-null,
// last error untouched; ext-gpa), NULL with ERROR_PROC_NOT_FOUND (127) for
// any other name. Windows answers a NULL name there with an access violation
// (gpa-ctx name=(NULL)); Vitreo returns NULL/127 instead of faulting.
void Opengl32Manager::impl_wglGetProcAddress(Marshaler& m) {
    const GuestAddr name = m.arg_ptr(0);
    if (gl::current_gl_context() == nullptr) {
        set_last_error(kErrorInvalidHandle);
        m.set_return(0);
        return;
    }
    const GuestAddr addr = extension_address(m, name);
    if (addr == 0) set_last_error(kErrorProcNotFound);
    m.set_return(addr);
}

// ---- opengl32's pixel-format exports: gdi32's calls (each wglpf-X line equals its -gdi line).

namespace {
void forward_pixel_format(const std::function<void(Marshaler&)>& call, Marshaler& m) {
    if (!call) { set_last_error(kErrorNotSupported); m.set_return(0); return; }
    call(m);
}
}  // namespace

void Opengl32Manager::impl_wglChoosePixelFormat(Marshaler& m) { forward_pixel_format(m_pixel_format_api.choose, m); }
void Opengl32Manager::impl_wglDescribePixelFormat(Marshaler& m) { forward_pixel_format(m_pixel_format_api.describe, m); }
void Opengl32Manager::impl_wglGetPixelFormat(Marshaler& m) { forward_pixel_format(m_pixel_format_api.get, m); }
void Opengl32Manager::impl_wglSetPixelFormat(Marshaler& m) { forward_pixel_format(m_pixel_format_api.set, m); }

// ---- Layer planes. No pixel format has one (pf-list layer=0), so every plane
// above the main plane is rejected exactly as Windows does.

// int wglDescribeLayerPlane(HDC, int iPixelFormat, int iLayerPlane, UINT nBytes,
// LPLAYERPLANEDESCRIPTOR). wgllayer-describe-*: NULL hdc 0/6; plane 0 is 0 with
// the last error untouched; every other plane (also with nBytes 0, a NULL
// buffer, a bad format index, a DC without a format) is 0/87; nothing is written.
void Opengl32Manager::impl_wglDescribeLayerPlane(Marshaler& m) {
    const u64 hdc = m.arg(0);
    const i32 plane = static_cast<i32>(m.arg32(2));
    if (hdc == 0) set_last_error(kErrorInvalidHandle);
    else if (plane != 0) set_last_error(kErrorInvalidParameter);
    m.set_return(0);
}

namespace {
/// The shared rule of the layer-palette calls (wgllayer-setpal/getpal/realize-*):
/// a NULL hdc or a DC without a pixel format is 2000; then plane 0 is FALSE
/// with the last error untouched, and any other plane (count 0, NULL colours
/// included) is FALSE with 87. Always FALSE: there is no layer palette.
void layer_palette_call(const Opengl32Manager& mgr, u64 hdc, i32 plane, Marshaler& m) {
    if (hdc == 0 || mgr.pixel_format_of_dc(hdc) == 0) set_last_error(kErrorInvalidPixelFormat);
    else if (plane != 0) set_last_error(kErrorInvalidParameter);
    m.set_return(0);
}
}  // namespace

// int wglSetLayerPaletteEntries(HDC, int iLayerPlane, int iStart, int cEntries, const COLORREF*).
void Opengl32Manager::impl_wglSetLayerPaletteEntries(Marshaler& m) {
    layer_palette_call(*this, m.arg(0), static_cast<i32>(m.arg32(1)), m);
}
// int wglGetLayerPaletteEntries(...): the output buffer stays untouched.
void Opengl32Manager::impl_wglGetLayerPaletteEntries(Marshaler& m) {
    layer_palette_call(*this, m.arg(0), static_cast<i32>(m.arg32(1)), m);
}
// BOOL wglRealizeLayerPalette(HDC, int iLayerPlane, BOOL bRealize).
void Opengl32Manager::impl_wglRealizeLayerPalette(Marshaler& m) {
    layer_palette_call(*this, m.arg(0), static_cast<i32>(m.arg32(1)), m);
}

// PROC wglGetDefaultProcAddress(LPCSTR). wgldefproc-*: NULL with ERROR_PROC_NOT_FOUND
// (127) for core and unknown names, with or without a current context (unlike
// wglGetProcAddress, whose no-context answer is 6). The extension entry points
// are returned with or without a context (wgldefproc-noctx glColorTableEXT /
// glAddSwapHintRectWIN non-null), the same pointers wglGetProcAddress gives
// (ext-defproc same-as-gpa=1). Windows faults on a NULL name; Vitreo returns
// NULL/127.
void Opengl32Manager::impl_wglGetDefaultProcAddress(Marshaler& m) {
    const GuestAddr addr = extension_address(m, m.arg_ptr(0));
    if (addr == 0) set_last_error(kErrorProcNotFound);
    m.set_return(addr);
}

// wglUseFontBitmaps{A,W} / wglUseFontOutlines{A,W} turn glyphs into display
// lists; both need display lists (Task 12) and font rasterisation (Phase 2).
// They fail with the documented failure value (FALSE) and say so once.
namespace {
void font_lists_unavailable(Marshaler& m, const char* api) {
    gl::gl_log_unimplemented_once(api, "glyph display lists are not built (Phase 2)");
    set_last_error(kErrorNotSupported);
    m.set_return(0);
}
}  // namespace

void Opengl32Manager::impl_wglUseFontBitmapsA(Marshaler& m) { font_lists_unavailable(m, "wglUseFontBitmapsA"); }
void Opengl32Manager::impl_wglUseFontBitmapsW(Marshaler& m) { font_lists_unavailable(m, "wglUseFontBitmapsW"); }
void Opengl32Manager::impl_wglUseFontOutlinesA(Marshaler& m) { font_lists_unavailable(m, "wglUseFontOutlinesA"); }
void Opengl32Manager::impl_wglUseFontOutlinesW(Marshaler& m) { font_lists_unavailable(m, "wglUseFontOutlinesW"); }

}  // namespace vitreo::winapi
