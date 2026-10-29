#pragma once
/// @file gl_drawable.hpp
/// @brief The GL default framebuffer of a window (Design 3): back and front
///        BGRA8 textures, a depth/stencil texture when the pixel format has
///        one, the open render pass GL draws are batched into (Design 1),
///        glClear (Design 2, spec 4.2.3), the swap and presentation through a
///        BackendSwapChain on the window's view.
///
/// One drawable per HWND (the pixel format is the window's, set once), shared
/// by every context made current on a DC of that window. wglMakeCurrent
/// attaches it to the context (GlContext::drawable) and resizes it to the
/// client area; SwapBuffers swaps and then resizes. Everything renders through
/// graphics::active_backend_device(); there is no host OpenGL.
///
/// Coordinates. Textures are stored top row first (what the swap chain
/// presents). GL window coordinates have (0, 0) at the bottom-left
/// (spec 2.11, 4.1.2): window_rect_to_target() and viewport_to_target() flip
/// a GL rectangle into the target's left/top/right/bottom.
///
/// For the draw tasks:
/// - Target selection: the texture pointers target()/draw_targets() return are
///   valid only until the next swap (which exchanges back and front), resize
///   or detach; never cache them across calls. A draw re-queries them, and
///   open_pass()/record_draw() select the targets themselves under the
///   drawable's lock.
///   draw_targets(draw_buffer) are the colour buffers a
///   glDrawBuffer mode names (GL_FRONT_AND_BACK and GL_LEFT name both on a
///   double-buffered format); target(read_buffer) is the single buffer a
///   glReadBuffer mode names. depth_stencil() is null for a 0/0 format.
/// - Open pass: open_pass(draw_buffer) returns the pass on those targets,
///   opening it (loading the current contents) when none is open or the open
///   one is on other targets. pass_layout() is what a pipeline drawing into it
///   must match. record_draw() submits into it and counts a pending draw.
/// - A pass ends (end_pass()) at SwapBuffers, glFlush, glFinish, a readback,
///   a draw-buffer change, a resize, and when a context is released. Tasks 10
///   and 11 must end it before uploading into or reading a texture the pass
///   uses (glCopyTex*, glReadPixels, glCopyPixels).
/// - Readback: read_color / read_depth / read_stencil end the pass and copy
///   the whole buffer, top row first.
///
/// Locks. mutex() (the drawable's, recursive) guards the buffers, the pass
/// and the pending batch; it is never held while anything waits for the main
/// thread. Creating, resizing and destroying the window's swap chain, and
/// presenting into it (MetalSwapchain's first fit of its layer), synchronise
/// with the main thread, and the main thread takes this lock (DestroyWindow's
/// WindowManager callback runs detach(); a GL call there on a context bound to
/// the window draws). So swap(), flush() and finish() pick what to present
/// under the lock and present after releasing it, under the separate present
/// lock, which nothing ever waits for. Each present posts its request into a
/// one-request slot under the lock (latest wins: a newer frame replaces one
/// not yet shown); whoever holds the present lock presents the slot's
/// requests until it is empty, and a thread that finds the lock held leaves
/// its request to that thread and returns at once. So the newest frame is
/// always shown, and the main thread never blocks behind a presenting thread.
/// The texture being presented is never made the back buffer by a swap on
/// another thread meanwhile: that swap gives the back buffer a copy of it. window_destroyed() takes the swap chain out under the lock and
/// releases it after; a present still using it finishes first, and the last
/// reference destroys it outside every drawable lock. Callers never hold
/// mutex() when they call swap(), flush(), finish() or window_destroyed().

#include <vitreo/common.hpp>
#include "graphics/contracts/format_converter.hpp"
#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vitreo::graphics {
class BackendPipeline;
class BackendRenderPass;
class BackendSwapChain;
class BackendTexture2D;
class GraphicsBackendDevice;
struct BackendDrawCall;
}  // namespace vitreo::graphics

namespace vitreo::winapi::gl {

/// Which colour buffers a glDrawBuffer / glReadBuffer token names (spec 4.2.1,
/// Table 4.4). nullopt for a token that is not a buffer name.
struct GlBufferSet {
    bool front_left{false}, front_right{false}, back_left{false}, back_right{false};
    u32 aux{0xFFFFFFFFu};   ///< AUXi index, or none
};
[[nodiscard]] std::optional<GlBufferSet> gl_buffers_named(u32 mode) noexcept;

/// Makes the window's presentation target. Production resolves the HWND's
/// native view (as D3D9 does) and calls create_swap_chain; tests inject a
/// fake. Null: the window is not presentable (rendering still works).
using GlSwapChainFactory =
    std::function<std::unique_ptr<graphics::BackendSwapChain>(u64 hwnd, u32 width, u32 height)>;

/// Draws a context has batched for this drawable but not yet recorded into
/// its pass (gl_backend_draw.hpp, Design 1). The drawable records them before
/// anything that must come after them: ending the pass (swap, flush, finish,
/// readback, resize, draw-buffer change, release, window destruction) and a
/// glClear. flush_into() runs under the drawable's lock; the batch was
/// already detached, so recording into the pass from it cannot recurse.
class GlPendingBatch {
public:
    virtual void flush_into(GlDrawable& drawable) = 0;

protected:
    ~GlPendingBatch() = default;
};

/// What a pipeline drawing into the open pass must match.
struct GlPassLayout {
    u32 color_count{0};                     ///< attachments 0..color_count-1, all color_format
    graphics::PixelFormat color_format{graphics::PixelFormat::BGRA8Unorm};
    graphics::PixelFormat depth_format{graphics::PixelFormat::Unknown};   ///< Unknown: none
    bool has_stencil{false};
    u32 width{0};
    u32 height{0};
};

/// One glClear, resolved from the context state (gl_cmd_framebuffer.cpp).
struct GlClearParams {
    bool color{false};                      ///< GL_COLOR_BUFFER_BIT
    bool depth{false};                      ///< GL_DEPTH_BUFFER_BIT
    bool stencil{false};                    ///< GL_STENCIL_BUFFER_BIT
    u32 draw_buffer{0};                     ///< GL_DRAW_BUFFER
    std::array<float, 4> color_value{};     ///< clamped clear colour
    std::array<bool, 4> color_mask{true, true, true, true};
    float depth_value{1.0f};
    bool depth_mask{true};
    u32 stencil_value{0};
    u32 stencil_mask{0xFFFFFFFFu};
    bool scissor{false};                    ///< GL_SCISSOR_TEST
    std::array<i32, 4> scissor_box{};       ///< x, y, width, height (window coordinates)
};

class GlDrawable {
public:
    /// A drawable of @p format for @p hwnd, allocated at width x height
    /// (0 x 0 allocates nothing until ensure_size gives a size).
    GlDrawable(graphics::GraphicsBackendDevice& device, const GlPixelFormat& format, i32 format_index,
               u64 hwnd, u32 width, u32 height, GlSwapChainFactory factory);
    ~GlDrawable();
    GlDrawable(const GlDrawable&) = delete;
    GlDrawable& operator=(const GlDrawable&) = delete;

    // ---- Per-window registry ----
    /// The drawable of @p hwnd, or null.
    [[nodiscard]] static std::shared_ptr<GlDrawable> for_window(u64 hwnd);
    /// The drawable of @p hwnd for pixel format @p format_index, created on
    /// the active backend device when it does not exist (or was made for
    /// another format), and resized to width x height. Null when there is no
    /// backend device, no window, or the index is not a format.
    [[nodiscard]] static std::shared_ptr<GlDrawable> attach(u64 hwnd, i32 format_index, u32 width,
                                                            u32 height, const GlSwapChainFactory& factory);
    /// The window is being destroyed (WindowManager's destroyed callback):
    /// forgets its drawable, so a reused HWND value gets a fresh one, and
    /// drops its swap chain under the drawable's lock, so a context still
    /// current on it presents nothing into the dead view.
    static void detach(u64 hwnd);
    /// Drops every drawable (tests).
    static void detach_all();

    [[nodiscard]] const GlPixelFormat& format() const noexcept { return m_format; }
    [[nodiscard]] i32 format_index() const noexcept { return m_format_index; }
    [[nodiscard]] u64 window() const noexcept { return m_hwnd; }
    [[nodiscard]] u32 width() const;
    [[nodiscard]] u32 height() const;
    /// Reallocates every buffer at width x height when the size changed
    /// (ending the open pass; the new contents are undefined and start
    /// cleared). The swap chain follows at the next present. A 0 x 0 client
    /// area (a minimised window) keeps the last allocation, so the frame drawn
    /// right after a restore is not lost (final review, Finding 14); nothing
    /// is allocated until a first non-zero size.
    void ensure_size(u32 width, u32 height);
    /// detach(): end the pass, drop the swap chain (destroyed after the lock is
    /// released), never present again.
    void window_destroyed();

    // ---- Targets ----
    /// The one colour buffer a glReadBuffer mode names (GL_FRONT, GL_LEFT,
    /// GL_FRONT_LEFT: front; GL_BACK, GL_BACK_LEFT: back), or null.
    [[nodiscard]] graphics::BackendTexture2D* target(u32 buffer);
    /// The colour buffers a glDrawBuffer mode names (0, 1 or 2 entries).
    [[nodiscard]] u32 draw_targets(u32 draw_buffer, std::array<graphics::BackendTexture2D*, 2>& out);
    [[nodiscard]] graphics::BackendTexture2D* depth_stencil();
    /// Colour channels that exist (bit 0 red .. bit 3 alpha): 0x7 for a format
    /// without alpha, whose alpha stays 1 (spec 4.3.2 reads it as 1).
    [[nodiscard]] u32 color_channels() const noexcept { return m_format.alpha_bits != 0 ? 0xFu : 0x7u; }

    // ---- The open pass ----
    [[nodiscard]] GlPassLayout pass_layout(u32 draw_buffer);
    /// The pass on draw_targets(draw_buffer) and the depth/stencil buffer,
    /// opened when needed. Null when there is nothing to draw into.
    [[nodiscard]] graphics::BackendRenderPass* open_pass(u32 draw_buffer);
    /// Records @p call into the pass open_pass(draw_buffer) gives; counts it.
    [[nodiscard]] bool record_draw(u32 draw_buffer, const graphics::BackendDrawCall& call);
    [[nodiscard]] bool pass_open() const;
    [[nodiscard]] u32 pending_draws() const;
    /// Passes open_pass has opened (diagnostics and tests).
    [[nodiscard]] u32 passes_opened() const;
    /// Submits and closes the open pass, if any (recording the pending batch
    /// first).
    void end_pass();

    // ---- Pending batch (Design 1) ----
    /// The lock that serialises the drawable and the batch pending on it: a
    /// context appends to its batch while holding it, so a flush from another
    /// thread (a clear by another context, the window's destruction) never
    /// sees a half-appended batch. Recursive.
    [[nodiscard]] std::recursive_mutex& mutex() const noexcept { return m_mutex; }
    /// Makes @p batch the one pending on this drawable; a different batch
    /// pending before it is recorded first. Call with mutex() held.
    void set_pending_batch(GlPendingBatch* batch);
    /// Forgets @p batch if it is the pending one (its owner is going away or
    /// has recorded it). Call with mutex() held.
    void drop_pending_batch(GlPendingBatch* batch) noexcept;
    /// Records the pending batch, if any, into the pass.
    void flush_pending_batch();

    // ---- Clears (spec 4.2.3) ----
    void clear(const GlClearParams& params);

    // ---- Presentation ----
    /// SwapBuffers on a double-buffered format: ends the pass, exchanges back
    /// and front (PFD_SWAP_EXCHANGE) and presents the new front. False for a
    /// single-buffered format (nothing happens). With GL_WIN_swap_hint
    /// rectangles pending, each rectangle is copied from the back buffer into
    /// the front, not their bounding rectangle (a point between two disjoint
    /// rectangles keeps the old front: hint-disjoint-rects-*); the rest of the
    /// front keeps its contents, the back is unchanged, and the rectangles are
    /// cleared (swap_hint_probe64.expected: hint-one-rect-green,
    /// -next-swap-blue, -two-rects-*, -finish-*).
    bool swap();
    /// glAddSwapHintRectWIN: GL window rectangle (x, y, w, h), w and h > 0,
    /// for the next swap of this window. Rectangles belong to the window, not
    /// the context: they survive glFlush / glFinish, a context's release and
    /// re-binding, the context visiting another window, another context
    /// becoming current on this window, and a swap of another window, and a
    /// swap with no context current uses them (hint-across-makecurrent-*,
    /// hint-e1 .. -e3, -e5, -e6, hint-drop-f1 .. -f9). A single-buffered
    /// window keeps none (hint-single-*).
    void add_swap_hint(i32 x, i32 y, i32 width, i32 height);
    /// glFlush: ends the pass and submits it; a single-buffered format
    /// presents its front buffer.
    void flush();
    /// glFinish: glFlush, then waits until the GPU has finished.
    void finish();

    // ---- Readback (the whole buffer, top row first) ----
    /// BGRA8 bytes of the colour buffer target(buffer) names.
    [[nodiscard]] bool read_color(u32 buffer, std::vector<u8>& bgra);
    /// Depth values as floats in [0, 1].
    [[nodiscard]] bool read_depth(std::vector<float>& depth);
    [[nodiscard]] bool read_stencil(std::vector<u8>& stencil);

    // ---- GL window coordinates -> target coordinates ----
    /// GL rectangle (x, y, width, height), bottom-left origin, to the target's
    /// left/top/right/bottom, clipped to the drawable (empty when outside).
    [[nodiscard]] std::array<int, 4> window_rect_to_target(i32 x, i32 y, i32 width, i32 height) const;
    /// A glViewport / glDepthRange as a backend viewport {x, y, w, h, near, far}.
    [[nodiscard]] std::array<float, 6> viewport_to_target(i32 x, i32 y, i32 width, i32 height,
                                                          double depth_near, double depth_far) const;

private:
    void allocate_locked(u32 width, u32 height);
    void end_pass_locked();
    void flush_pending_batch_locked();
    graphics::BackendTexture2D* target_locked(u32 buffer);
    u32 draw_targets_locked(u32 draw_buffer, std::array<graphics::BackendTexture2D*, 2>& out);
    graphics::BackendRenderPass* open_pass_locked(u32 draw_buffer);
    /// What a present shows: the front buffer after the exchange (or the
    /// single buffer), with the size it was drawn at. GL_WIN_swap_hint
    /// rectangles were already copied into it under mutex().
    struct PresentRequest {
        std::shared_ptr<graphics::BackendTexture2D> source;
        u32 width{0};
        u32 height{0};
    };
    /// Posts the current front buffer into the one-request slot (latest wins;
    /// mutex() held).
    void post_present_locked();
    /// Presents the slot's requests with mutex() not held until the slot is
    /// empty, or returns at once when another thread is presenting this
    /// window (that thread takes the request; Locks, above).
    void run_presents();
    void present_one(const PresentRequest& request);
    bool draw_clear_quad_locked(const GlClearParams& p, bool color, bool depth, bool stencil);
    graphics::BackendPipeline* clear_pipeline_locked(u32 color_count, u32 write_mask);

    graphics::GraphicsBackendDevice& m_device;
    const GlPixelFormat m_format;
    const i32 m_format_index;
    const u64 m_hwnd;
    GlSwapChainFactory m_factory;

    mutable std::recursive_mutex m_mutex;
    u32 m_width{0};
    u32 m_height{0};
    /// Shared, so a present outside the lock keeps its source alive across a
    /// resize or a swap on another thread.
    std::shared_ptr<graphics::BackendTexture2D> m_back;       ///< null for single-buffered
    std::shared_ptr<graphics::BackendTexture2D> m_front;
    std::unique_ptr<graphics::BackendTexture2D> m_depth_stencil;
    /// Shared, so window_destroyed() can take it out while a present uses it.
    std::shared_ptr<graphics::BackendSwapChain> m_swap_chain;
    bool m_swap_chain_tried{false};
    bool m_window_destroyed{false};
    /// Serialises the swap-chain calls made outside mutex(); only ever
    /// try-locked (Locks, above).
    std::mutex m_present_mutex;
    /// The newest present not yet taken by the presenting thread.
    std::optional<PresentRequest> m_pending_present;
    /// The texture being presented now (outside mutex()); swap() never makes
    /// it the back buffer.
    std::shared_ptr<graphics::BackendTexture2D> m_presenting_source;

    std::unique_ptr<graphics::BackendRenderPass> m_pass;
    std::array<graphics::BackendTexture2D*, 2> m_pass_targets{};
    u32 m_pass_target_count{0};
    u32 m_pending_draws{0};
    u32 m_passes_opened{0};

    std::unordered_map<u32, std::unique_ptr<graphics::BackendPipeline>> m_clear_pipelines;
    /// Pending GL_WIN_swap_hint rectangles (GL window coordinates).
    std::vector<std::array<i32, 4>> m_swap_hints;
    GlPendingBatch* m_pending_batch{nullptr};
};

}  // namespace vitreo::winapi::gl
