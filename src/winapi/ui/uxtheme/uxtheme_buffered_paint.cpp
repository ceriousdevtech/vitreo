/// @file uxtheme_buffered_paint.cpp
/// @brief Buffered paint and buffered animation.
///
/// Measured on Windows 11 (uxtheme_aero_probe "bp" and "banim" lines):
///  - the buffer is a 32 bpp DIB section for every 32-bit format (also
///    BPBF_COMPATIBLEBITMAP, whose bits GetBufferedPaintBits refuses with
///    E_FAIL) and a 1 bpp top-down DIB, its width rounded up to 32 pixels,
///    for BPBF_TOPDOWNMONODIB;
///  - the paint DC draws in the target's coordinates: its window origin is
///    the target rectangle's top-left, MM_TEXT, viewport origin 0,0;
///  - BeginBufferedPaint works without BufferedPaintInit; every failure
///    returns NULL with *phdc = NULL and the last error 0 (a NULL phdc
///    leaves the last error alone); BP_PAINTPARAMS.cbSize must be its size;
///  - BufferedPaintSetAlpha is E_FAIL for the compatible and mono formats;
///    a NULL or ended handle is E_FAIL everywhere except EndBufferedPaint
///    (E_INVALIDARG).

#include "winapi/ui/uxtheme/uxtheme_internal.hpp"

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <algorithm>
#include <cmath>

namespace vitreo::winapi::uxtheme {

namespace {

constexpr u32 BPBF_COMPATIBLEBITMAP = 0, BPBF_DIB = 1, BPBF_TOPDOWNDIB = 2, BPBF_TOPDOWNMONODIB = 3;
constexpr u32 BPPF_ERASE = 1, BPPF_NOCLIP = 2;

struct PaintBuffer {
    u64 target = 0;
    i32 rect[4]{};
    u32 format = 0;
    u32 flags = 0;
    bool has_exclude = false;
    i32 exclude[4]{};
    bool has_blend = false;
    u32 blend = 0;  ///< BLENDFUNCTION bytes
    Handle dc = 0;
    Handle bitmap = 0;
    Handle old_bitmap = 0;
    GuestAddr bits = 0;
    i32 width = 0;
    i32 height = 0;
};

struct Animation {
    u64 from = 0;  ///< paint-buffer handles (from may be 0)
    u64 to = 0;
};

struct BufferedPaintState {
    std::mutex mu;
    std::unordered_map<u64, PaintBuffer> buffers;
    std::unordered_map<u64, Animation> animations;
    u64 next = 0x00B00004u;
    std::unordered_map<u32, u32> inits;
};

BufferedPaintState& state() {
    static BufferedPaintState s;
    return s;
}

/// BeginBufferedPaint's validation and buffer creation; 0 on failure.
u64 begin_paint(Marshaler& m, UxThemeModule& mod, u64 target, GuestAddr rect_ptr, u32 format, GuestAddr params) {
    GdiManager* gdi = mod.services().gdi;
    if (gdi == nullptr || target == 0 || !gdi->has_dc(static_cast<Handle>(target))) return 0;
    PaintBuffer b;
    b.target = target;
    if (!read_rect(m, rect_ptr, b.rect)) return 0;
    if (b.rect[2] <= b.rect[0] || b.rect[3] <= b.rect[1]) return 0;
    if (format > BPBF_TOPDOWNMONODIB) return 0;
    b.format = format;
    if (params != 0) {
        const bool x64 = m.cpu_is_64bit();
        if (m.read_u32(params).value_or(0) != (x64 ? 24u : 16u)) return 0;
        b.flags = m.read_u32(params + 4).value_or(0);
        const GuestAddr exclude = x64 ? m.read_u64(params + 8).value_or(0) : m.read_u32(params + 8).value_or(0);
        const GuestAddr blend = x64 ? m.read_u64(params + 16).value_or(0) : m.read_u32(params + 12).value_or(0);
        b.has_exclude = read_rect(m, exclude, b.exclude);
        if (blend != 0) {
            b.has_blend = true;
            b.blend = m.read_u32(blend).value_or(0);
        }
    }
    b.width = b.rect[2] - b.rect[0];
    b.height = b.rect[3] - b.rect[1];
    const bool mono = format == BPBF_TOPDOWNMONODIB;
    const i32 bw = mono ? (b.width + 31) / 32 * 32 : b.width;
    const bool bottom_up = format == BPBF_COMPATIBLEBITMAP || format == BPBF_DIB;
    b.bitmap = gdi->create_runtime_dib_section(bw, b.height, bottom_up, mono ? 1 : 32, &b.bits);
    if (b.bitmap == 0) return 0;
    b.width = bw;
    b.dc = gdi->create_compatible_dc(static_cast<Handle>(target));
    if (b.dc == 0) {
        (void)gdi->delete_object(b.bitmap);
        return 0;
    }
    b.old_bitmap = gdi->select_object(b.dc, b.bitmap);
    (void)gdi->set_window_org(b.dc, b.rect[0], b.rect[1]);
    if (b.flags & BPPF_ERASE) {
        (void)gdi->with_device_surface(b.dc, [](GdiManager::DeviceSurface& s) {
            std::fill(s.rgba, s.rgba + static_cast<usize>(s.width) * static_cast<usize>(s.height) * 4u, u8{0});
        });
    }
    auto& st = state();
    std::lock_guard lock(st.mu);
    const u64 h = st.next;
    st.next += 4;
    st.buffers[h] = b;
    return h;
}

void destroy(GdiManager& gdi, const PaintBuffer& b) {
    (void)gdi.select_object(b.dc, b.old_bitmap);
    (void)gdi.delete_dc(b.dc);
    (void)gdi.delete_object(b.bitmap);
}

/// Copy the buffer to its target (EndBufferedPaint's fUpdateTarget).
void update_target(GdiManager& gdi, const PaintBuffer& b) {
    const Handle target = static_cast<Handle>(b.target);
    ClipRect saved;
    const bool have_clip = gdi.get_dc_clip(target, saved);
    if (b.flags & BPPF_NOCLIP) gdi.set_dc_clip(target, ClipRect{});
    if (b.has_exclude) (void)gdi.exclude_clip_rect(target, b.exclude[0], b.exclude[1], b.exclude[2], b.exclude[3]);
    const i32 w = b.rect[2] - b.rect[0], h = b.rect[3] - b.rect[1];
    if (b.has_blend) {
        // BLENDFUNCTION: BlendOp, BlendFlags, SourceConstantAlpha, AlphaFormat (AC_SRC_ALPHA = 1).
        (void)gdi.alpha_blend(target, b.rect[0], b.rect[1], w, h, b.dc, b.rect[0], b.rect[1], w, h,
                              static_cast<u8>(b.blend >> 16), ((b.blend >> 24) & 1u) != 0);
    } else {
        constexpr u32 SRCCOPY = 0x00CC0020u;
        (void)gdi.bit_blt(target, b.rect[0], b.rect[1], w, h, b.dc, b.rect[0], b.rect[1], SRCCOPY);
    }
    if (have_clip) gdi.set_dc_clip(target, saved);
}

std::optional<PaintBuffer> find(u64 h) {
    auto& st = state();
    std::lock_guard lock(st.mu);
    const auto it = st.buffers.find(h);
    if (it == st.buffers.end()) return std::nullopt;
    return it->second;
}

/// Run @p fn over the buffer pixels inside @p rect_ptr (logical; NULL = all).
template <typename Fn>
void for_pixels(Marshaler& m, GdiManager& gdi, const PaintBuffer& b, GuestAddr rect_ptr, Fn&& fn) {
    i32 r[4] = {b.rect[0], b.rect[1], b.rect[0] + b.width, b.rect[3]};
    if (rect_ptr != 0) (void)read_rect(m, rect_ptr, r);
    (void)gdi.with_device_surface(b.dc, [&](GdiManager::DeviceSurface& s) {
        const i32 l = std::max(0, static_cast<i32>(std::lround(s.xform[0] * r[0] + s.xform[4])));
        const i32 t = std::max(0, static_cast<i32>(std::lround(s.xform[3] * r[1] + s.xform[5])));
        const i32 rr = std::min(s.width, static_cast<i32>(std::lround(s.xform[0] * r[2] + s.xform[4])));
        const i32 bb = std::min(s.height, static_cast<i32>(std::lround(s.xform[3] * r[3] + s.xform[5])));
        for (i32 y = t; y < bb; ++y)
            for (i32 x = l; x < rr; ++x) fn(s.rgba + (static_cast<usize>(y) * static_cast<usize>(s.width) + x) * 4u);
    });
}

}  // namespace

void add_buffered_paint_exports(DllExports& exports, UxThemeModule& mod_ref) {
    UxThemeModule* mod = &mod_ref;
    auto add = [&](const char* name, u16 ordinal, u16 stdcall, std::function<void(Marshaler&)> fn) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(fn), stdcall});
    };

    // HRESULT BufferedPaintInit(void) / BufferedPaintUnInit(void): per thread,
    // balanced; an extra UnInit is E_FAIL.
    add("BufferedPaintInit", 38, 0, [](Marshaler& m) {
        auto& st = state();
        std::lock_guard lock(st.mu);
        st.inits[ThreadManager::get_current_thread_id_static()] += 1;
        m.set_return(S_OK);
    });
    add("BufferedPaintUnInit", 42, 0, [](Marshaler& m) {
        auto& st = state();
        std::lock_guard lock(st.mu);
        const auto it = st.inits.find(ThreadManager::get_current_thread_id_static());
        if (it == st.inits.end() || it->second == 0) {
            m.set_return(E_FAIL);
            return;
        }
        if (--it->second == 0) st.inits.erase(it);
        m.set_return(S_OK);
    });

    // HPAINTBUFFER BeginBufferedPaint(HDC, const RECT*, BP_BUFFERFORMAT, BP_PAINTPARAMS*, HDC* phdc)
    add("BeginBufferedPaint", 6, 20, [mod](Marshaler& m) {
        const GuestAddr phdc = m.arg_ptr(4);
        if (phdc == 0) {
            m.set_return(0);
            return;
        }
        const u64 h = begin_paint(m, *mod, m.arg(0), m.arg_ptr(1), static_cast<u32>(m.arg(2)), m.arg_ptr(3));
        const auto b = h ? find(h) : std::nullopt;
        (void)m.write_ptr(phdc, b ? b->dc : 0);
        set_last_error(0);
        m.set_return(h);
    });

    // HRESULT EndBufferedPaint(HPAINTBUFFER, BOOL fUpdateTarget)
    add("EndBufferedPaint", 129, 8, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        PaintBuffer b;
        {
            auto& st = state();
            std::lock_guard lock(st.mu);
            const auto it = st.buffers.find(h);
            if (it == st.buffers.end()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            b = it->second;
            st.buffers.erase(it);
        }
        GdiManager* gdi = mod->services().gdi;
        if (gdi != nullptr) {
            if (m.arg(1) != 0) update_target(*gdi, b);
            destroy(*gdi, b);
        }
        m.set_return(S_OK);
    });

    // HRESULT GetBufferedPaintBits(HPAINTBUFFER, RGBQUAD** ppbBuffer, int* pcxRow)
    add("GetBufferedPaintBits", 146, 12, [](Marshaler& m) {
        const auto b = find(m.arg(0));
        if (!b || b->format == BPBF_COMPATIBLEBITMAP) {
            m.set_return(E_FAIL);
            return;
        }
        const GuestAddr pp = m.arg_ptr(1), prow = m.arg_ptr(2);
        if (pp == 0 || prow == 0) {
            m.set_return(E_POINTER);
            return;
        }
        (void)m.write_ptr(pp, b->bits);
        (void)m.write_u32(prow, static_cast<u32>(b->width));
        m.set_return(S_OK);
    });
    // HDC GetBufferedPaintDC(HPAINTBUFFER) / GetBufferedPaintTargetDC(HPAINTBUFFER)
    add("GetBufferedPaintDC", 147, 4, [](Marshaler& m) {
        const auto b = find(m.arg(0));
        m.set_return(b ? b->dc : 0);
    });
    add("GetBufferedPaintTargetDC", 148, 4, [](Marshaler& m) {
        const auto b = find(m.arg(0));
        m.set_return(b ? b->target : 0);
    });
    // HRESULT GetBufferedPaintTargetRect(HPAINTBUFFER, RECT*)
    add("GetBufferedPaintTargetRect", 149, 8, [](Marshaler& m) {
        const auto b = find(m.arg(0));
        if (!b) {
            m.set_return(E_FAIL);
            return;
        }
        if (!write_rect(m, m.arg_ptr(1), b->rect)) {
            m.set_return(E_POINTER);
            return;
        }
        m.set_return(S_OK);
    });
    // HRESULT BufferedPaintClear(HPAINTBUFFER, const RECT*): ARGB = 0.
    add("BufferedPaintClear", 37, 8, [mod](Marshaler& m) {
        const auto b = find(m.arg(0));
        if (!b || mod->services().gdi == nullptr) {
            m.set_return(E_FAIL);
            return;
        }
        for_pixels(m, *mod->services().gdi, *b, m.arg_ptr(1), [](u8* p) { p[0] = p[1] = p[2] = p[3] = 0; });
        m.set_return(S_OK);
    });
    // HRESULT BufferedPaintSetAlpha(HPAINTBUFFER, const RECT*, BYTE alpha)
    add("BufferedPaintSetAlpha", 40, 12, [mod](Marshaler& m) {
        const auto b = find(m.arg(0));
        if (!b || mod->services().gdi == nullptr || b->format == BPBF_COMPATIBLEBITMAP ||
            b->format == BPBF_TOPDOWNMONODIB) {
            m.set_return(E_FAIL);
            return;
        }
        const u8 alpha = static_cast<u8>(m.arg(2));
        for_pixels(m, *mod->services().gdi, *b, m.arg_ptr(1), [alpha](u8* p) { p[3] = alpha; });
        m.set_return(S_OK);
    });

    // HANIMATIONBUFFER BeginBufferedAnimation(HWND, HDC hdcTarget, const RECT*,
    //     BP_BUFFERFORMAT, BP_PAINTPARAMS*, BP_ANIMATIONPARAMS*, HDC* phdcFrom, HDC* phdcTo)
    add("BeginBufferedAnimation", 5, 32, [mod](Marshaler& m) {
        const GuestAddr anim = m.arg_ptr(5), pfrom = m.arg_ptr(6), pto = m.arg_ptr(7);
        auto fail = [&] {
            if (pfrom) (void)m.write_ptr(pfrom, 0);
            if (pto) (void)m.write_ptr(pto, 0);
            set_last_error(0);
            m.set_return(0);
        };
        // BP_ANIMATIONPARAMS {cbSize, dwFlags, style, dwDuration}: 16 bytes.
        if (anim == 0 || m.read_u32(anim).value_or(0) != 16u) return fail();
        const u32 style = m.read_u32(anim + 8).value_or(0), duration = m.read_u32(anim + 12).value_or(0);
        if (style > 3) return fail();  // BPAS_NONE / LINEAR / CUBIC / SINE
        Animation a;
        const bool crossfade = style != 0 && duration != 0;
        a.to = begin_paint(m, *mod, m.arg(1), m.arg_ptr(2), static_cast<u32>(m.arg(3)), m.arg_ptr(4));
        if (a.to == 0) return fail();
        if (crossfade) {
            a.from = begin_paint(m, *mod, m.arg(1), m.arg_ptr(2), static_cast<u32>(m.arg(3)), m.arg_ptr(4));
            unimplemented_once("BeginBufferedAnimation: cross-fade frames (the final frame is painted)");
        }
        const auto to = find(a.to);
        const auto from = a.from ? find(a.from) : std::nullopt;
        if (pfrom) (void)m.write_ptr(pfrom, from ? from->dc : 0);
        if (pto) (void)m.write_ptr(pto, to ? to->dc : 0);
        auto& st = state();
        u64 h;
        {
            std::lock_guard lock(st.mu);
            h = st.next;
            st.next += 4;
            st.animations[h] = a;
        }
        set_last_error(0);
        m.set_return(h);
    });
    // HRESULT EndBufferedAnimation(HANIMATIONBUFFER, BOOL fUpdateTarget)
    add("EndBufferedAnimation", 114, 8, [mod](Marshaler& m) {
        Animation a;
        std::optional<PaintBuffer> from, to;
        {
            auto& st = state();
            std::lock_guard lock(st.mu);
            const auto it = st.animations.find(m.arg(0));
            if (it == st.animations.end()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            a = it->second;
            st.animations.erase(it);
            if (auto f = st.buffers.find(a.from); f != st.buffers.end()) { from = f->second; st.buffers.erase(f); }
            if (auto t = st.buffers.find(a.to); t != st.buffers.end()) { to = t->second; st.buffers.erase(t); }
        }
        GdiManager* gdi = mod->services().gdi;
        if (gdi != nullptr) {
            if (to && m.arg(1) != 0) update_target(*gdi, *to);
            if (from) destroy(*gdi, *from);
            if (to) destroy(*gdi, *to);
        }
        m.set_return(S_OK);
    });
    // BOOL BufferedPaintRenderAnimation(HWND, HDC): no animation is ever in
    // flight between frames, so there is nothing to render.
    add("BufferedPaintRenderAnimation", 39, 8, [](Marshaler& m) { m.set_return(0); });
    // HRESULT BufferedPaintStopAllAnimations(HWND): S_OK for any window (measured, NULL too).
    add("BufferedPaintStopAllAnimations", 41, 4, [](Marshaler& m) { m.set_return(S_OK); });
}

}  // namespace vitreo::winapi::uxtheme
