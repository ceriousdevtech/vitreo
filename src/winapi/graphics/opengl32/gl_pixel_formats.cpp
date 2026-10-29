// SPDX-License-Identifier: LGPL-2.1-only

/// @file gl_pixel_formats.cpp
/// @brief The ICD pixel-format list and ChoosePixelFormat's selection rule.

#include "winapi/graphics/opengl32/gl_pixel_formats.hpp"

#include <array>

namespace vitreo::winapi::gl {
namespace {

// Depth/stencil combinations the backend can create. 24-bit depth is the
// D32Float_S8Uint attachment D3D9 uses for D3DFMT_D24S8 (precision at least
// 24 bits); 16-bit depth is D16Unorm (D3DFMT_D16), which has no stencil.
struct DepthStencil {
    u8 depth;
    u8 stencil;
};
constexpr DepthStencil kDepthStencils[] = {{24, 8}, {24, 0}, {16, 0}, {0, 0}};

std::vector<GlPixelFormat> build_formats() {
    std::vector<GlPixelFormat> formats;
    // Order follows the measured list (wgl_contract_probe64.expected pf-list):
    // alpha-less 32-bit first (idx 1-4), then alpha 8 (idx 5-8); within each,
    // single buffered before double buffered, deeper depth first.
    for (const u8 alpha : {u8{0}, u8{8}}) {
        for (const bool db : {false, true}) {
            for (const DepthStencil ds : kDepthStencils) {
                GlPixelFormat f{};
                // A double-buffered format's SwapBuffers exchanges back and
                // front (Design 3; gl_drawable.cpp), which PFD_SWAP_EXCHANGE
                // states: the back buffer then holds the old front.
                f.flags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_SUPPORT_COMPOSITION |
                          (db ? PFD_DOUBLEBUFFER | PFD_SWAP_EXCHANGE : 0u);
                f.color_bits = 32;
                f.red_bits = f.green_bits = f.blue_bits = 8;
                f.alpha_bits = alpha;
                f.depth_bits = ds.depth;
                f.stencil_bits = ds.stencil;
                f.double_buffer = db;
                formats.push_back(f);
            }
        }
    }
    return formats;
}

}  // namespace

const std::vector<GlPixelFormat>& gl_pixel_formats() {
    static const std::vector<GlPixelFormat> formats = build_formats();
    return formats;
}

const GlPixelFormat* gl_pixel_format(i32 index) {
    const auto& formats = gl_pixel_formats();
    if (index < 1 || static_cast<usize>(index) > formats.size()) return nullptr;
    return &formats[static_cast<usize>(index) - 1];
}

i32 choose_gl_pixel_format(const PixelFormatRequest& r) {
    const auto& formats = gl_pixel_formats();
    // wgl_contract_probe64.expected header, "ChoosePixelFormat selection
    // rule": lexicographic key, smallest wins, ties to the lowest index.
    const bool double_buffer_dontcare = (r.flags & PFD_DOUBLEBUFFER_DONTCARE) != 0;
    const int wanted_depth = (r.flags & PFD_DEPTH_DONTCARE) ? 0 : r.depth_bits;
    // A requested colour depth outside {4, 8, 16, 24, 32} counts as 32.
    const bool known_color = r.color_bits == 4 || r.color_bits == 8 || r.color_bits == 16 ||
                             r.color_bits == 24 || r.color_bits == 32;
    const int wanted_color = known_color ? r.color_bits : 32;

    std::array<int, 8> best_key{};
    i32 best = 0;
    for (usize i = 0; i < formats.size(); ++i) {
        const GlPixelFormat& f = formats[i];
        // 1. Requested DRAW_TO_BITMAP / SUPPORT_GDI the format lacks.
        const int absent_bitmap_gdi =
            (((r.flags & PFD_DRAW_TO_BITMAP) != 0) && !(f.flags & PFD_DRAW_TO_BITMAP)) +
            (((r.flags & 0x10u) != 0) && !(f.flags & 0x10u));
        // 2. Double-buffer mismatch, skipped under DOUBLEBUFFER_DONTCARE.
        const int db_mismatch =
            double_buffer_dontcare ? 0 : (((r.flags & PFD_DOUBLEBUFFER) != 0) != f.double_buffer);
        // 3. DRAW_TO_WINDOW requested but absent.
        const int window_absent = ((r.flags & PFD_DRAW_TO_WINDOW) != 0) && !(f.flags & PFD_DRAW_TO_WINDOW);
        // 4. Four equal-weight mismatches: colour, accumulation (exact, only
        //    when requested; no format has any), pixel type (all RGBA), alpha
        //    (only when requested).
        const int mismatches = (f.color_bits != wanted_color) + (r.accum_bits != 0) +
                               (r.pixel_type != PFD_TYPE_RGBA) +
                               (r.alpha_bits != 0 && f.alpha_bits == 0);
        // 5. Depth: formats reaching the request first, smallest first; when
        //    none reaches it, the largest.
        const bool depth_reaches = f.depth_bits >= wanted_depth;
        const int depth_class = depth_reaches ? 0 : 1;
        const int depth_order = depth_reaches ? f.depth_bits : -static_cast<int>(f.depth_bits);
        // Stencil is unobservable on Windows (all 36 formats carry 8). This
        // list varies it, so it is ranked like depth, after depth.
        const bool stencil_reaches = f.stencil_bits >= r.stencil_bits;
        const int stencil_order = (stencil_reaches ? 0 : 1000) +
                                  (stencil_reaches ? f.stencil_bits : -static_cast<int>(f.stencil_bits));
        // 6. Lowest index: the strict < below keeps the earlier format on ties.
        const std::array<int, 8> key{absent_bitmap_gdi, db_mismatch, window_absent, mismatches,
                                     depth_class,       depth_order, stencil_order, 0};
        if (best == 0 || key < best_key) {
            best_key = key;
            best = static_cast<i32>(i) + 1;
        }
    }
    return best;
}

std::array<u8, kPixelFormatDescriptorSize> gl_pixel_format_descriptor(const GlPixelFormat& f) {
    std::array<u8, kPixelFormatDescriptorSize> d{};
    d[0] = kPixelFormatDescriptorSize;  // nSize
    d[2] = 1;                           // nVersion
    for (int i = 0; i < 4; ++i) d[4 + i] = static_cast<u8>(f.flags >> (8 * i));
    d[8] = PFD_TYPE_RGBA;
    d[9] = f.color_bits;
    // wgl_contract_probe64.expected pf-list idx 1 and 5: shifts 16/8/0/0.
    d[10] = f.red_bits;    d[11] = 16;
    d[12] = f.green_bits;  d[13] = 8;
    d[14] = f.blue_bits;   d[15] = 0;
    d[16] = f.alpha_bits;  d[17] = 0;
    // d[18..22]: cAccumBits and the per-channel accumulation bits, all 0.
    d[23] = f.depth_bits;
    d[24] = f.stencil_bits;
    // d[25] cAuxBuffers 0, d[26] iLayerType 0 (PFD_MAIN_PLANE), masks 0.
    return d;
}

GlFramebufferConfig gl_framebuffer_config(const GlPixelFormat& f) {
    GlFramebufferConfig c;
    c.red_bits = f.red_bits;
    c.green_bits = f.green_bits;
    c.blue_bits = f.blue_bits;
    c.alpha_bits = f.alpha_bits;
    // gl11_state_probe64.expected get-GL_INDEX_BITS = 8 on a 32-bit RGBA
    // format; every format here is 32-bit.
    c.index_bits = 8;
    c.depth_bits = f.depth_bits;
    c.stencil_bits = f.stencil_bits;
    c.accum_red_bits = c.accum_green_bits = c.accum_blue_bits = c.accum_alpha_bits = 0;
    c.aux_buffers = 0;
    c.double_buffer = f.double_buffer;
    c.stereo = false;
    return c;
}

GlBackendTargets gl_backend_targets(const GlPixelFormat& f) {
    GlBackendTargets t{graphics::PixelFormat::BGRA8Unorm, graphics::PixelFormat::Unknown};
    if (f.depth_bits == 16) t.depth_stencil = graphics::PixelFormat::D16Unorm;
    else if (f.depth_bits == 24) t.depth_stencil = graphics::PixelFormat::D32Float_S8Uint;
    return t;
}

}  // namespace vitreo::winapi::gl
