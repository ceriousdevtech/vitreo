// SPDX-License-Identifier: LGPL-2.1-only
/// @file gl_drawable.cpp
/// @brief The GL default framebuffer of a window on the shared backend
///        (gl_drawable.hpp): buffers, the open pass, glClear, swap, present.

#include "winapi/graphics/opengl32/gl_drawable.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/backend_render_pass.hpp"
#include "graphics/contracts/backend_swap_chain.hpp"
#include "winapi/graphics/opengl32/gl_commands.hpp"
#include "winapi/graphics/opengl32/gl_enums.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace vitreo::winapi::gl {

std::optional<GlBufferSet> gl_buffers_named(u32 mode) noexcept {
    GlBufferSet s;
    switch (mode) {
    case GL_FRONT_LEFT: s.front_left = true; break;
    case GL_FRONT_RIGHT: s.front_right = true; break;
    case GL_BACK_LEFT: s.back_left = true; break;
    case GL_BACK_RIGHT: s.back_right = true; break;
    case GL_FRONT: s.front_left = s.front_right = true; break;
    case GL_BACK: s.back_left = s.back_right = true; break;
    case GL_LEFT: s.front_left = s.back_left = true; break;
    case GL_RIGHT: s.front_right = s.back_right = true; break;
    case GL_FRONT_AND_BACK: s.front_left = s.front_right = s.back_left = s.back_right = true; break;
    case GL_AUX0: case GL_AUX1: case GL_AUX2: case GL_AUX3: s.aux = mode - GL_AUX0; break;
    default: return std::nullopt;
    }
    return s;
}

namespace {

// SwapBuffers presents with one vertical blank of wait: the default swap
// interval of WGL_EXT_swap_control (Vitreo exposes no extension to change it).
constexpr unsigned kPresentInterval = 1;

struct Registry {
    std::mutex mutex;
    std::unordered_map<u64, std::shared_ptr<GlDrawable>> by_window;
};
Registry& registry() {
    static Registry* r = new Registry();   // immortal: drawables may die during exit
    return *r;
}

std::unique_ptr<graphics::BackendTexture2D> make_color(graphics::GraphicsBackendDevice& device, u32 w,
                                                       u32 h) {
    graphics::BackendTexture2DDesc desc{};
    desc.width = w;
    desc.height = h;
    desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    desc.render_target = true;
    desc.shader_resource = true;
    auto t = device.create_texture_2d(desc, nullptr, 0);
    if (t && !t->valid()) t.reset();
    return t;
}

// The clear quad: four vertices from the vertex id covering the viewport, at
// the clear depth (vertex constants: float4, x = depth), writing the clear
// colour (fragment constants: float4) to every colour attachment.
std::string clear_vertex_msl() {
    return R"(#include <metal_stdlib>
using namespace metal;
struct GlClearOut { float4 position [[position]]; };
vertex GlClearOut gl_clear_vertex(uint vid [[vertex_id]], constant float4& depth [[buffer(0)]]) {
    GlClearOut o;
    o.position = float4((vid & 1u) ? 1.0 : -1.0, (vid & 2u) ? 1.0 : -1.0, depth.x, 1.0);
    return o;
}
)";
}

std::string clear_fragment_msl(u32 color_count) {
    std::string s = "#include <metal_stdlib>\nusing namespace metal;\n";
    if (color_count == 0) {
        s += "fragment void gl_clear_fragment() {}\n";
        return s;
    }
    s += "struct GlClearColor {";
    for (u32 i = 0; i < color_count; ++i) {
        s += " float4 c" + std::to_string(i) + " [[color(" + std::to_string(i) + ")]];";
    }
    s += " };\nfragment GlClearColor gl_clear_fragment(constant float4& color [[buffer(0)]]) {\n"
         "    GlClearColor o;\n";
    for (u32 i = 0; i < color_count; ++i) s += "    o.c" + std::to_string(i) + " = color;\n";
    s += "    return o;\n}\n";
    return s;
}

}  // namespace

// ---- Registry ------------------------------------------------------------------

std::shared_ptr<GlDrawable> GlDrawable::for_window(u64 hwnd) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lock(r.mutex);
    const auto it = r.by_window.find(hwnd);
    return it == r.by_window.end() ? nullptr : it->second;
}

std::shared_ptr<GlDrawable> GlDrawable::attach(u64 hwnd, i32 format_index, u32 width, u32 height,
                                               const GlSwapChainFactory& factory) {
    static std::atomic<bool> no_device{false}, no_window{false};
    const GlPixelFormat* format = gl_pixel_format(format_index);
    if (format == nullptr) return nullptr;
    if (hwnd == 0) {
        // A DC without a window (a memory DC): rendering into a bitmap needs a
        // PFD_DRAW_TO_BITMAP format, which no Vitreo format has (Phase 2).
        gl_log_once(no_window, "UNIMPLEMENTED opengl32: rendering to a DC without a window "
                            "(PFD_DRAW_TO_BITMAP) has no drawable");
        return nullptr;
    }
    graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
    if (device == nullptr) {
        gl_log_once(no_device, "no graphics backend device: GL rendering has no drawable");
        return nullptr;
    }
    std::shared_ptr<GlDrawable> d;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        auto& slot = r.by_window[hwnd];
        if (!slot || slot->format_index() != format_index || &slot->m_device != device) {
            // A window's pixel format is set once; a different one means the
            // HWND value now names another window.
            slot = std::make_shared<GlDrawable>(*device, *format, format_index, hwnd, width, height,
                                                factory);
        }
        d = slot;
    }
    d->ensure_size(width, height);
    return d;
}

void GlDrawable::detach(u64 hwnd) {
    std::shared_ptr<GlDrawable> d;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        const auto it = r.by_window.find(hwnd);
        if (it == r.by_window.end()) return;
        d = std::move(it->second);
        r.by_window.erase(it);
    }
    d->window_destroyed();
}

void GlDrawable::detach_all() {
    std::unordered_map<u64, std::shared_ptr<GlDrawable>> all;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        all.swap(r.by_window);
    }
    for (auto& [h, d] : all) d->window_destroyed();
}

void GlDrawable::window_destroyed() {
    std::shared_ptr<graphics::BackendSwapChain> chain;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        end_pass_locked();
        // A context still current on the dead window keeps this object alive;
        // it must never present into the view the window owned.
        chain = std::move(m_swap_chain);
        m_swap_chain.reset();
        m_swap_chain_tried = true;
        m_window_destroyed = true;
        m_pending_present.reset();
        m_factory = nullptr;
    }
    // ~MetalSwapchain synchronises with the main thread: released with no
    // drawable lock held (final review, Finding 4). A present still using it
    // holds the last reference and destroys it when it ends.
    chain.reset();
}

// ---- Construction and size -------------------------------------------------------

GlDrawable::GlDrawable(graphics::GraphicsBackendDevice& device, const GlPixelFormat& format,
                       i32 format_index, u64 hwnd, u32 width, u32 height, GlSwapChainFactory factory)
    : m_device(device), m_format(format), m_format_index(format_index), m_hwnd(hwnd),
      m_factory(std::move(factory)) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    allocate_locked(width, height);
}

GlDrawable::~GlDrawable() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    end_pass_locked();
}

u32 GlDrawable::width() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_width;
}

u32 GlDrawable::height() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_height;
}

void GlDrawable::ensure_size(u32 width, u32 height) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (width == m_width && height == m_height) return;
    // A minimised window's 0 x 0 client area keeps what it had.
    if ((width == 0 || height == 0) && m_width != 0 && m_height != 0) return;
    allocate_locked(width, height);
}

void GlDrawable::allocate_locked(u32 width, u32 height) {
    end_pass_locked();
    m_back.reset();
    m_front.reset();
    m_depth_stencil.reset();
    m_width = width;
    m_height = height;
    if (width == 0 || height == 0) return;   // a zero-sized client area draws nothing

    m_front = make_color(m_device, width, height);
    if (m_format.double_buffer) m_back = make_color(m_device, width, height);
    const GlBackendTargets targets = gl_backend_targets(m_format);
    if (targets.depth_stencil != graphics::PixelFormat::Unknown) {
        graphics::BackendTexture2DDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.pixel_format = targets.depth_stencil;
        desc.depth_stencil = true;
        m_depth_stencil = m_device.create_texture_2d(desc, nullptr, 0);
        if (m_depth_stencil && !m_depth_stencil->valid()) m_depth_stencil.reset();
    }
    if (!m_front || (m_format.double_buffer && !m_back) ||
        (targets.depth_stencil != graphics::PixelFormat::Unknown && !m_depth_stencil)) {
        std::fprintf(stderr, "[vitreo] opengl32: the backend refused a %ux%u drawable\n", width, height);
        m_back.reset();
        m_front.reset();
        m_depth_stencil.reset();
        m_width = m_height = 0;
        return;
    }
    // The initial contents are undefined (spec 4.2); start from black, with
    // alpha 1 where the format has no alpha (spec 4.3.2), depth 1, stencil 0.
    const std::array<float, 4> black{0.0f, 0.0f, 0.0f, m_format.alpha_bits != 0 ? 0.0f : 1.0f};
    (void)m_front->clear_render_target(black);
    if (m_back) (void)m_back->clear_render_target(black);
    if (m_depth_stencil) {
        (void)m_depth_stencil->clear_depth_stencil_aspects(1.0f, 0, true, m_format.stencil_bits != 0, 0);
    }
    // The swap chain is resized by the next present, outside the lock
    // (MetalSwapchain::resize synchronises with the main thread).
}

// ---- Targets ---------------------------------------------------------------------

graphics::BackendTexture2D* GlDrawable::target(u32 buffer) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return target_locked(buffer);
}

graphics::BackendTexture2D* GlDrawable::target_locked(u32 buffer) {
    // Spec 4.3.2: FRONT and LEFT read the front-left buffer, BACK the back-left.
    switch (buffer) {
    case GL_FRONT: case GL_LEFT: case GL_FRONT_LEFT: return m_front.get();
    case GL_BACK: case GL_BACK_LEFT: return m_back.get();
    default: return nullptr;   // right buffers (no stereo) and AUXi (no aux) do not exist
    }
}

u32 GlDrawable::draw_targets(u32 draw_buffer, std::array<graphics::BackendTexture2D*, 2>& out) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return draw_targets_locked(draw_buffer, out);
}

u32 GlDrawable::draw_targets_locked(u32 draw_buffer, std::array<graphics::BackendTexture2D*, 2>& out) {
    out = {};
    const auto set = gl_buffers_named(draw_buffer);
    if (!set) return 0;   // GL_NONE
    u32 n = 0;
    // No stereo and no aux buffers: only the left buffers exist.
    if (set->back_left && m_back) out[n++] = m_back.get();
    if (set->front_left && m_front) out[n++] = m_front.get();
    return n;
}

graphics::BackendTexture2D* GlDrawable::depth_stencil() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_depth_stencil.get();
}

// ---- The open pass ---------------------------------------------------------------

GlPassLayout GlDrawable::pass_layout(u32 draw_buffer) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    GlPassLayout l;
    std::array<graphics::BackendTexture2D*, 2> t{};
    l.color_count = draw_targets_locked(draw_buffer, t);
    l.depth_format = m_depth_stencil ? m_depth_stencil->desc().pixel_format : graphics::PixelFormat::Unknown;
    l.has_stencil = m_depth_stencil && m_format.stencil_bits != 0;
    l.width = m_width;
    l.height = m_height;
    return l;
}

graphics::BackendRenderPass* GlDrawable::open_pass(u32 draw_buffer) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return open_pass_locked(draw_buffer);
}

graphics::BackendRenderPass* GlDrawable::open_pass_locked(u32 draw_buffer) {
    std::array<graphics::BackendTexture2D*, 2> t{};
    const u32 n = draw_targets_locked(draw_buffer, t);
    if (m_pass && (n != m_pass_target_count || t != m_pass_targets)) end_pass_locked();
    if (m_pass) return m_pass.get();
    if (n == 0 && !m_depth_stencil) return nullptr;
    graphics::BackendRenderPassDesc desc{};
    for (u32 i = 0; i < n; ++i) desc.color_targets[i] = t[i];
    desc.preserve_color_contents = true;   // GL keeps what earlier work left there
    desc.depth_stencil_target = m_depth_stencil.get();
    desc.clear_depth = false;
    m_pass = m_device.create_render_pass_standalone(desc);
    if (m_pass && !m_pass->valid()) m_pass.reset();
    if (!m_pass) {
        static std::atomic<bool> logged{false};
        gl_log_once(logged, "the backend refused the drawable's render pass");
        return nullptr;
    }
    m_pass_targets = t;
    m_pass_target_count = n;
    m_pending_draws = 0;
    ++m_passes_opened;
    return m_pass.get();
}

bool GlDrawable::record_draw(u32 draw_buffer, const graphics::BackendDrawCall& call) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    graphics::BackendRenderPass* pass = open_pass_locked(draw_buffer);
    if (pass == nullptr || !pass->draw(call)) return false;
    ++m_pending_draws;
    return true;
}

bool GlDrawable::pass_open() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_pass != nullptr;
}

u32 GlDrawable::pending_draws() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_pass ? m_pending_draws : 0;
}

u32 GlDrawable::passes_opened() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_passes_opened;
}

void GlDrawable::end_pass() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    end_pass_locked();
}

void GlDrawable::end_pass_locked() {
    flush_pending_batch_locked();
    if (!m_pass) return;
    (void)m_pass->end();
    m_pass.reset();
    m_pass_targets = {};
    m_pass_target_count = 0;
    m_pending_draws = 0;
}

// ---- Pending batch ---------------------------------------------------------------

void GlDrawable::set_pending_batch(GlPendingBatch* batch) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_pending_batch == batch) return;
    flush_pending_batch_locked();
    m_pending_batch = batch;
}

void GlDrawable::drop_pending_batch(GlPendingBatch* batch) noexcept {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_pending_batch == batch) m_pending_batch = nullptr;
}

void GlDrawable::flush_pending_batch() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    flush_pending_batch_locked();
}

void GlDrawable::flush_pending_batch_locked() {
    GlPendingBatch* batch = m_pending_batch;
    if (batch == nullptr) return;
    m_pending_batch = nullptr;   // detached first: its draws open/end passes without recursing
    batch->flush_into(*this);
}

// ---- Coordinates -----------------------------------------------------------------

std::array<int, 4> GlDrawable::window_rect_to_target(i32 x, i32 y, i32 width, i32 height) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    const i64 w = m_width, h = m_height;
    const i64 x0 = std::clamp<i64>(x, 0, w), x1 = std::clamp<i64>(i64{x} + width, 0, w);
    const i64 y0 = std::clamp<i64>(y, 0, h), y1 = std::clamp<i64>(i64{y} + height, 0, h);
    if (x1 <= x0 || y1 <= y0) return {0, 0, 0, 0};
    // Window row y is target row h - 1 - y: the GL rectangle's top edge y1 is
    // the target's top h - y1.
    return {static_cast<int>(x0), static_cast<int>(h - y1), static_cast<int>(x1), static_cast<int>(h - y0)};
}

std::array<float, 6> GlDrawable::viewport_to_target(i32 x, i32 y, i32 width, i32 height,
                                                    double depth_near, double depth_far) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Spec 2.10.1: window x = (xd + 1) w/2 + x, y = (yd + 1) h/2 + y with y up.
    // The backend's origin is the top-left with NDC y = +1 at the top of the
    // viewport, so the same mapping is a viewport whose top is h_d - (y + h).
    const float top = static_cast<float>(static_cast<i64>(m_height) - (i64{y} + height));
    return {static_cast<float>(x), top, static_cast<float>(width), static_cast<float>(height),
            static_cast<float>(depth_near), static_cast<float>(depth_far)};
}

// ---- glClear ---------------------------------------------------------------------

void GlDrawable::clear(const GlClearParams& p) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // Primitives issued before the clear are drawn before it.
    flush_pending_batch_locked();
    if (m_width == 0 || m_height == 0) return;
    std::array<graphics::BackendTexture2D*, 2> t{};
    const u32 n = draw_targets_locked(p.draw_buffer, t);
    const u32 channels = color_channels();
    u32 color_mask = 0;
    for (u32 i = 0; i < 4; ++i) color_mask |= p.color_mask[i] ? (1u << i) : 0u;
    color_mask &= channels;
    const u32 stencil_all = (1u << m_format.stencil_bits) - 1u;
    // A buffer the format lacks, or one every mask bit protects, is not
    // touched (spec 4.2.3: the masks apply; no depth / stencil buffer: no effect).
    const bool color = p.color && n != 0 && color_mask != 0;
    const bool depth = p.depth && m_depth_stencil && m_format.depth_bits != 0 && p.depth_mask;
    const bool stencil = p.stencil && m_depth_stencil && m_format.stencil_bits != 0 &&
                         (p.stencil_mask & stencil_all) != 0;
    if (!color && !depth && !stencil) return;

    // Spec 4.1.2: the scissor test applies. A box covering the whole window is
    // no restriction; an empty one leaves nothing to clear.
    bool whole = true;
    if (p.scissor) {
        const auto r = window_rect_to_target(p.scissor_box[0], p.scissor_box[1], p.scissor_box[2],
                                             p.scissor_box[3]);
        if (r[2] <= r[0] || r[3] <= r[1]) return;
        whole = r[0] == 0 && r[1] == 0 && static_cast<u32>(r[2]) == m_width &&
                static_cast<u32>(r[3]) == m_height;
    }
    // Design 2: a load-action clear only when nothing restricts it and no
    // draws are pending in the open pass (which a clear outside it would
    // reorder); otherwise a quad at the clear values inside the pass.
    const bool full_masks = (!color || color_mask == channels) &&
                            (!stencil || (p.stencil_mask & stencil_all) == stencil_all);
    if (whole && full_masks && (!m_pass || m_pending_draws == 0)) {
        end_pass_locked();
        if (color) {
            std::array<float, 4> c = p.color_value;
            if (m_format.alpha_bits == 0) c[3] = 1.0f;
            for (u32 i = 0; i < n; ++i) (void)t[i]->clear_render_target(c);
        }
        if (depth || stencil) {
            (void)m_depth_stencil->clear_depth_stencil_aspects(
                p.depth_value, static_cast<u8>(p.stencil_value & stencil_all), depth, stencil, 0);
        }
        return;
    }
    (void)draw_clear_quad_locked(p, color, depth, stencil);
}

graphics::BackendPipeline* GlDrawable::clear_pipeline_locked(u32 color_count, u32 write_mask) {
    const u32 key = (color_count << 8) | write_mask;
    auto& slot = m_clear_pipelines[key];
    if (slot) return slot.get();
    graphics::BackendPipelineDesc desc{};
    desc.vertex_shader_msl = clear_vertex_msl();
    desc.fragment_shader_msl = clear_fragment_msl(color_count);
    desc.vertex_entry_point = "gl_clear_vertex";
    desc.fragment_entry_point = "gl_clear_fragment";
    desc.color_format = color_count != 0 ? graphics::PixelFormat::BGRA8Unorm : graphics::PixelFormat::Unknown;
    for (u32 i = 0; i < color_count; ++i) desc.color_formats[i] = graphics::PixelFormat::BGRA8Unorm;
    desc.color_write_mask = write_mask;
    desc.color_write_masks.fill(write_mask);
    desc.depth_attachment = m_depth_stencil != nullptr;
    desc.depth_format = m_depth_stencil ? m_depth_stencil->desc().pixel_format : graphics::PixelFormat::Unknown;
    const u64 depth_code = static_cast<u64>(desc.depth_format);
    desc.cache_key = 0x474C434C52000000ull ^ (depth_code << 16) ^ key;   // "GLCLR"
    slot = m_device.create_pipeline(desc);
    if (slot && !slot->valid()) slot.reset();
    return slot.get();
}

bool GlDrawable::draw_clear_quad_locked(const GlClearParams& p, bool color, bool depth, bool stencil) {
    static std::atomic<bool> logged{false};
    graphics::BackendRenderPass* pass = open_pass_locked(p.draw_buffer);
    if (pass == nullptr) return false;
    u32 write_mask = 0;
    if (color) {
        for (u32 i = 0; i < 4; ++i) write_mask |= p.color_mask[i] ? (1u << i) : 0u;
        write_mask &= color_channels();
    }
    graphics::BackendPipeline* pipeline = clear_pipeline_locked(m_pass_target_count, write_mask);
    if (pipeline == nullptr) {
        gl_log_once(logged, "the backend refused the glClear pipeline");
        return false;
    }
    const std::array<float, 4> depth_constants{p.depth_value, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> color_constants = p.color_value;
    graphics::BackendDrawCall call{};
    call.pipeline = pipeline;
    call.topology = graphics::BackendPrimitiveTopology::TriangleStrip;
    call.vertex_count = 4;
    call.viewport = {0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    if (p.scissor) {
        call.scissor_enabled = true;
        call.scissor = window_rect_to_target(p.scissor_box[0], p.scissor_box[1], p.scissor_box[2],
                                             p.scissor_box[3]);
    }
    call.vertex_constants = depth_constants.data();
    call.vertex_constants_size = sizeof(depth_constants);
    call.fragment_constants = color_constants.data();
    call.fragment_constants_size = sizeof(color_constants);
    call.cull_mode = graphics::BackendCullMode::None;
    // Depth: written unconditionally (compare Always) when cleared.
    call.depth_test_enabled = depth;
    call.depth_write_enabled = depth;
    call.depth_compare = graphics::BackendCompare::Always;
    // Stencil: every pixel replaced with the value under the write mask.
    if (stencil) {
        const u32 all = (1u << m_format.stencil_bits) - 1u;
        call.stencil_test_enabled = true;
        call.stencil_read_mask = 0xFF;
        call.stencil_write_mask = static_cast<u8>(p.stencil_mask & all);
        call.stencil_reference = p.stencil_value & all;
        graphics::BackendStencilFaceDesc face{};
        face.stencil_fail = graphics::BackendStencilOperation::Keep;
        face.depth_fail = graphics::BackendStencilOperation::Replace;
        face.pass = graphics::BackendStencilOperation::Replace;
        face.compare = graphics::BackendCompare::Always;
        call.front_stencil = face;
        call.back_stencil = face;
    }
    if (!pass->draw(call)) {
        gl_log_once(logged, "the backend refused the glClear quad");
        return false;
    }
    ++m_pending_draws;
    return true;
}

// ---- Presentation ----------------------------------------------------------------

void GlDrawable::post_present_locked() {
    if (!m_front || m_window_destroyed) return;
    if (!m_swap_chain && m_swap_chain_tried) return;   // the window has no presentation view
    // Latest wins: an older request still waiting is the frame this one
    // replaces (final rereview, N1).
    m_pending_present = PresentRequest{m_front, m_width, m_height};
}

void GlDrawable::run_presents() {
    for (;;) {
        {
            // Never under mutex(): a present may wait for the main thread.
            std::unique_lock<std::mutex> presenting(m_present_mutex, std::try_to_lock);
            // Another thread is presenting this window: it takes the request
            // just posted before it lets go (the loop below), so this thread
            // returns at once (it may be the main thread that one waits for).
            if (!presenting.owns_lock()) return;
            for (;;) {
                std::optional<PresentRequest> request;
                {
                    std::lock_guard<std::recursive_mutex> lock(m_mutex);
                    request = std::move(m_pending_present);
                    m_pending_present.reset();
                    if (request && m_window_destroyed) request.reset();
                    // swap() must not recycle the texture shown now as the
                    // back buffer while it is being presented.
                    m_presenting_source = request ? request->source : nullptr;
                }
                if (!request) break;
                present_one(*request);
                std::lock_guard<std::recursive_mutex> lock(m_mutex);
                m_presenting_source.reset();
            }
        }
        // A request posted after the slot was found empty but before the
        // present lock was released found the lock held and left it to us.
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_pending_present) return;
    }
}

void GlDrawable::present_one(const PresentRequest& request) {
    static std::atomic<bool> no_view{false};
    std::shared_ptr<graphics::BackendSwapChain> chain;
    GlSwapChainFactory factory;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (m_window_destroyed) return;
        chain = m_swap_chain;
        if (!chain && !m_swap_chain_tried) {
            m_swap_chain_tried = true;   // made once, by this present
            factory = m_factory;
        }
    }
    if (!chain && factory) {
        // The first present makes the swap chain (MetalSwapchain's constructor
        // synchronises with the main thread), then installs it unless the
        // window was destroyed meanwhile.
        std::shared_ptr<graphics::BackendSwapChain> made = factory(m_hwnd, request.width, request.height);
        if (made && !made->valid()) made.reset();
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (!m_window_destroyed && !m_swap_chain) m_swap_chain = made;
            chain = m_window_destroyed ? nullptr : m_swap_chain;
        }
        // An uninstalled chain (the window died meanwhile) is released here,
        // with no drawable lock held.
        made.reset();
        if (!chain) gl_log_once(no_view, "the GL window has no presentation view; nothing is shown");
    }
    if (!chain) return;
    if (chain->desc().width != request.width || chain->desc().height != request.height) {
        (void)chain->resize(request.width, request.height, graphics::PixelFormat::BGRA8Unorm, 2);
    }
    (void)chain->present_texture(request.source.get(), kPresentInterval, 0);
}

bool GlDrawable::swap() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_format.double_buffer) return false;
        end_pass_locked();
        std::vector<std::array<i32, 4>> hints;
        hints.swap(m_swap_hints);   // every swap clears them (hint-next-swap-blue)
        if (!m_back || !m_front) return true;   // a zero-sized window has nothing to show
        bool copied = false;
        if (!hints.empty()) {
            copied = true;
            for (const auto& h : hints) {
                const std::array<int, 4> r = window_rect_to_target(h[0], h[1], h[2], h[3]);
                if (r[2] <= r[0] || r[3] <= r[1]) continue;   // outside the window
                copied = m_device.copy_texture_2d_region(m_front.get(), static_cast<unsigned>(r[0]),
                                                         static_cast<unsigned>(r[1]), m_back.get(),
                                                         static_cast<unsigned>(r[0]), static_cast<unsigned>(r[1]),
                                                         static_cast<unsigned>(r[2] - r[0]),
                                                         static_cast<unsigned>(r[3] - r[1])) &&
                         copied;
            }
            // The backend could not copy a region: show the whole back buffer.
        }
        if (!copied) {
            if (m_presenting_source && m_presenting_source == m_front) {
                // Another thread is presenting the front buffer this exchange
                // would make the back buffer, which the next frame is drawn
                // into. The back buffer becomes a copy of it instead
                // (PFD_SWAP_EXCHANGE: the back holds the old front), and the
                // presented texture lives until its present ends.
                std::shared_ptr<graphics::BackendTexture2D> copy = make_color(m_device, m_width, m_height);
                if (copy && m_device.copy_texture_2d_region(copy.get(), 0, 0, m_front.get(), 0, 0, m_width,
                                                            m_height)) {
                    m_front = std::move(copy);
                }
            }
            std::swap(m_back, m_front);
        }
        // The request is posted under the same lock as the exchange, so a
        // waiting request never names a texture that is now the back buffer.
        post_present_locked();
    }
    run_presents();
    return true;
}

void GlDrawable::add_swap_hint(i32 x, i32 y, i32 width, i32 height) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    // A single-buffered window never uses a hint: glFlush, glFinish and
    // SwapBuffers all show the whole buffer however many are added
    // (hint-single-*, 100000 adds without an error), and a window's pixel
    // format never changes, so nothing is kept.
    if (!m_format.double_buffer) return;
    m_swap_hints.push_back({x, y, width, height});
}

void GlDrawable::flush() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        end_pass_locked();
        m_device.flush();
        if (!m_format.double_buffer) post_present_locked();
    }
    run_presents();
}

void GlDrawable::finish() {
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        end_pass_locked();
        if (!m_format.double_buffer) post_present_locked();
    }
    run_presents();
    (void)m_device.wait_idle();
}

// ---- Readback --------------------------------------------------------------------

bool GlDrawable::read_color(u32 buffer, std::vector<u8>& bgra) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    end_pass_locked();
    graphics::BackendTexture2D* t = target_locked(buffer);
    if (t == nullptr) return false;
    bgra.assign(static_cast<usize>(m_width) * m_height * 4u, 0);
    return t->readback(0, bgra.data(), m_width * 4u);
}

bool GlDrawable::read_depth(std::vector<float>& depth) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    end_pass_locked();
    if (!m_depth_stencil) return false;
    const usize count = static_cast<usize>(m_width) * m_height;
    depth.assign(count, 0.0f);
    if (m_depth_stencil->desc().pixel_format == graphics::PixelFormat::D16Unorm) {
        std::vector<u16> raw(count, 0);
        if (!m_depth_stencil->readback(0, raw.data(), m_width * 2u)) return false;
        for (usize i = 0; i < count; ++i) depth[i] = static_cast<float>(raw[i]) / 65535.0f;
        return true;
    }
    return m_depth_stencil->readback_depth_stencil_plane(0, 0, false, depth.data(), m_width * 4u);
}

bool GlDrawable::read_stencil(std::vector<u8>& stencil) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    end_pass_locked();
    if (!m_depth_stencil || m_format.stencil_bits == 0) return false;
    stencil.assign(static_cast<usize>(m_width) * m_height, 0);
    return m_depth_stencil->readback_depth_stencil_plane(0, 0, true, stencil.data(), m_width);
}

}  // namespace vitreo::winapi::gl
