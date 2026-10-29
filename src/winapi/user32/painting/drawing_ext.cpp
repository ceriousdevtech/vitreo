// SPDX-License-Identifier: LGPL-2.1-only

/// @file drawing_ext.cpp
/// @brief Drawing APIs banner.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/painting/frame_drawing.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/painting/sys_colors.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"
#include "capsule/vfs/path_encoding.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ----- Drawing APIs -----

void User32ExtManager::impl_GetSysColorBrush(Marshaler& m) {
    u32 nIndex = static_cast<u32>(m.arg(0));
    // sys_color_brush_probe: an undefined index is NULL, last error untouched.
    if (nIndex >= sys_colors::kCount) {
        m.set_return(0u);
        return;
    }
    u32 h = static_cast<u32>(sys_colors::kSysBrushBase) | nIndex;
    VITREO_TRACE("[u32ext] GetSysColorBrush index=%u -> 0x%x\n", nIndex, h);
    m.set_return(h);
}

// ----- DrawState -----
//
// Every rule below is a row of tools/oracle_probe/draw_state_probe.expected
// (Windows, x86 == x64).

namespace {

constexpr u32 DST_COMPLEX = 0x0;
constexpr u32 DST_TEXT = 0x1;
constexpr u32 DST_PREFIXTEXT = 0x2;
constexpr u32 DST_ICON = 0x3;
constexpr u32 DST_BITMAP = 0x4;
constexpr u32 DSS_UNION = 0x10;
constexpr u32 DSS_DISABLED = 0x20;
constexpr u32 DSS_MONO = 0x80;
constexpr u32 DSS_HIDEPREFIX = 0x200;
constexpr u32 DSS_PREFIXONLY = 0x400;
constexpr u32 DSS_RIGHT = 0x8000;
constexpr u32 kColorBtnShadow = 16;     // COLOR_3DSHADOW
constexpr u32 kColorBtnHighlight = 20;  // COLOR_3DHILIGHT
constexpr u32 kSrcCopy = 0x00CC0020;
constexpr u32 DT_RIGHT_ = 0x2;
constexpr u32 DT_SINGLELINE_ = 0x20;
constexpr u32 DT_NOCLIP_ = 0x100;
constexpr u32 DT_NOPREFIX_ = 0x800;
constexpr u32 DT_HIDEPREFIX_ = 0x100000;
constexpr u32 DT_PREFIXONLY_ = 0x200000;

/// An image to draw: its colour pixels (RGBA, top-down) and which pixels are
/// foreground for the DSS_MONO / DSS_DISABLED / DSS_UNION transforms.
struct StateImage {
    i32 width = 0;
    i32 height = 0;
    std::vector<u8> rgba;
    std::vector<u8> mask;  // 1 = foreground
};

void put_colorref(u8* px, u32 colorref) {
    px[0] = static_cast<u8>(colorref & 0xFF);
    px[1] = static_cast<u8>((colorref >> 8) & 0xFF);
    px[2] = static_cast<u8>((colorref >> 16) & 0xFF);
    px[3] = 0xFF;
}

bool is_white(const u8* px) { return px[0] == 0xFF && px[1] == 0xFF && px[2] == 0xFF; }

/// Copy `w` x `h` pixels at logical (x, y) of `hdc` out as RGBA.
bool read_dc_region(GdiManager& gdi, Handle hdc, i32 x, i32 y, i32 w, i32 h,
                    std::vector<u8>& out) {
    const Handle mem = gdi.create_compatible_dc(hdc);
    const Handle bmp = gdi.create_compatible_bitmap(hdc, w, h);
    if (mem == 0 || bmp == 0) {
        if (mem != 0) (void)gdi.delete_dc(mem);
        if (bmp != 0) (void)gdi.delete_object(bmp);
        return false;
    }
    const Handle old = gdi.select_object(mem, bmp);
    (void)gdi.bit_blt(mem, 0, 0, w, h, hdc, x, y, kSrcCopy);
    (void)gdi.select_object(mem, old);
    GdiManager::BitmapRgbaCopy copy;
    const bool ok = gdi.copy_bitmap_rgba(bmp, copy) && copy.width == w &&
                    copy.height == h;
    if (ok) out = std::move(copy.rgba);
    (void)gdi.delete_dc(mem);
    (void)gdi.delete_object(bmp);
    return ok;
}

/// SRCCOPY `rgba` (w x h) to logical (x, y) of `hdc`.
bool write_dc_region(GdiManager& gdi, Handle hdc, i32 x, i32 y, i32 w, i32 h,
                     const std::vector<u8>& rgba) {
    const Handle bmp = gdi.create_bitmap_from_rgba(w, h, rgba);
    const Handle mem = gdi.create_compatible_dc(hdc);
    if (bmp == 0 || mem == 0) {
        if (mem != 0) (void)gdi.delete_dc(mem);
        if (bmp != 0) (void)gdi.delete_object(bmp);
        return false;
    }
    const Handle old = gdi.select_object(mem, bmp);
    const bool ok = gdi.bit_blt(hdc, x, y, w, h, mem, 0, 0, kSrcCopy);
    (void)gdi.select_object(mem, old);
    (void)gdi.delete_dc(mem);
    (void)gdi.delete_object(bmp);
    return ok;
}

}  // namespace

void User32ExtManager::impl_DrawStateW(Marshaler& m) { draw_state_impl(m, true); }
void User32ExtManager::impl_DrawStateA(Marshaler& m) { draw_state_impl(m, false); }

void User32ExtManager::draw_state_impl(Marshaler& m, bool wide) {
    // BOOL DrawState(HDC, HBRUSH hbrFore, DRAWSTATEPROC, LPARAM lData,
    //                WPARAM wData, int x, int y, int cx, int cy, UINT uFlags)
    const Handle hdc = static_cast<Handle>(m.arg(0));
    const u64 brush = m.arg(1);
    const GuestAddr callback = m.arg_ptr(2);
    const u64 ldata = m.arg(3);
    const u64 wdata = m.arg(4);
    const i32 x = static_cast<i32>(m.arg32(5));
    const i32 y = static_cast<i32>(m.arg32(6));
    i32 cx = static_cast<i32>(m.arg32(7));
    i32 cy = static_cast<i32>(m.arg32(8));
    const u32 flags = m.arg32(9);
    const u32 type = flags & 0xF;
    const u32 transform = flags & (DSS_UNION | DSS_DISABLED | DSS_MONO);

    // bad_type5 / bad_type15: FALSE, last error untouched.
    if (type > DST_BITMAP) {
        m.set_return(0u);
        return;
    }
    // null_hdc: ERROR_INVALID_HANDLE is set, yet the call reports success.
    if (hdc == 0) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(1u);
        return;
    }
    if (m_gdi_mgr == nullptr) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0u);
        return;
    }
    GdiManager& gdi = *m_gdi_mgr;
    if (transform != 0 && transform != DSS_UNION && transform != DSS_DISABLED &&
        transform != DSS_MONO) {
        // Combinations of the transforms were not measured.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!DrawState: "
                                 "combined state flags 0x%x\n", transform);
        }
        m.set_return(0u);
        return;
    }
    if (type == DST_ICON) {
        // Icons draw through DrawIconEx, which Vitreo does not implement yet.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!DrawState: DST_ICON\n");
        }
        m.set_return(0u);
        return;
    }

    // The foreground colour for DSS_MONO / DSS_UNION: hbrFore, or black when
    // it is NULL (bitmap32_mono_nullbrush*, bitmap32_union*; the DC's text
    // colour does not matter).
    u32 fore = 0x000000;
    if (brush != 0) {
        u32 c = 0;
        if (const auto sys = sys_colors::from_brush(brush)) {
            fore = *sys;  // COLOR_xxx + 1 or GetSysColorBrush
        } else if (gdi.lookup_brush_color(static_cast<Handle>(brush), &c)) {
            fore = c;
        }
    }

    StateImage image;
    bool result = true;
    if (type == DST_BITMAP) {
        if (ldata == 0) {  // null_bitmap
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0u);
            return;
        }
        GdiManager::BitmapRgbaCopy bitmap;
        if (!gdi.copy_bitmap_rgba(static_cast<Handle>(ldata), bitmap) ||
            bitmap.width <= 0 || bitmap.height <= 0) {
            m.set_return(0u);  // bogus_bitmap: FALSE, error untouched
            return;
        }
        // cx/cy of 0 mean the bitmap's size; a larger value draws only the
        // bitmap and a smaller one clips it (bitmap32_cx0 / cx4cy2 / cx9cy6).
        // wData, the callback and DSS_RIGHT are ignored.
        const i32 w = std::min(cx != 0 ? cx : bitmap.width, bitmap.width);
        const i32 h = std::min(cy != 0 ? cy : bitmap.height, bitmap.height);
        if (w <= 0 || h <= 0) {
            m.set_return(1u);
            return;
        }
        image.width = w;
        image.height = h;
        image.rgba.assign(static_cast<usize>(w) * h * 4, 0);
        image.mask.assign(static_cast<usize>(w) * h, 1);
        const bool mono = bitmap.bpp == 1;
        const u32 text = gdi.get_text_color(hdc);
        const u32 bk = gdi.get_bk_color(hdc);
        for (i32 row = 0; row < h; ++row) {
            for (i32 col = 0; col < w; ++col) {
                const u8* src = bitmap.rgba.data() +
                    (static_cast<usize>(row) * bitmap.width + col) * 4;
                u8* dst = image.rgba.data() + (static_cast<usize>(row) * w + col) * 4;
                if (mono) {
                    // A 1bpp source draws 0 bits in the text colour and 1 bits
                    // in the background colour (mono_cx0_textblue_bkgreen);
                    // only 0 bits are foreground for the transforms
                    // (mono_mono_red, mono_disabled, mono_union).
                    const bool one = is_white(src);
                    put_colorref(dst, one ? bk : text);
                    image.mask[static_cast<usize>(row) * w + col] = one ? 0 : 1;
                } else {
                    // A colour image is foreground everywhere, white and black
                    // pixels alike (white_pixel_mono_red, black_pixel_*).
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 0xFF;
                }
            }
        }
    } else if (type == DST_COMPLEX) {
        // complex_cx0: an empty extent never calls the callback.
        if (cx == 0 || cy == 0) {
            m.set_return(1u);
            return;
        }
        if (callback == 0 || !m_guest_call) {
            // Windows calls through the NULL pointer and faults; there is no
            // defined result to reproduce.
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        if (transform == 0) {
            // complex_true / complex_false / complex_overdraw: the callback
            // draws straight into the target DC through a viewport origin
            // moved to (x, y), unclipped; its return value is the result.
            i32 ox = 0;
            i32 oy = 0;
            const bool have_org = gdi.get_viewport_org(hdc, ox, oy);
            if (have_org) (void)gdi.set_viewport_org(hdc, ox + x, oy + y);
            const u64 args[5] = {static_cast<u64>(hdc), ldata, wdata,
                                 static_cast<u64>(static_cast<u32>(cx)),
                                 static_cast<u64>(static_cast<u32>(cy))};
            const u64 r = m_guest_call(callback, args, 5, m.cpu_is_64bit());
            if (have_org) (void)gdi.set_viewport_org(hdc, ox, oy);
            m.set_return(static_cast<u32>(r) != 0 ? 1u : 0u);
            return;
        }
        // complex_mono_red: with a transform the callback draws into an
        // offscreen DC; what it changes from the white fill is foreground.
        const Handle mem = gdi.create_compatible_dc(hdc);
        const Handle bmp = gdi.create_compatible_bitmap(hdc, cx, cy);
        if (mem == 0 || bmp == 0) {
            if (mem != 0) (void)gdi.delete_dc(mem);
            if (bmp != 0) (void)gdi.delete_object(bmp);
            m.set_return(0u);
            return;
        }
        const Handle old = gdi.select_object(mem, bmp);
        (void)gdi.fill_rect_gdi(mem, 0, 0, cx, cy, 0xFFFFFF);
        const u64 args[5] = {static_cast<u64>(mem), ldata, wdata,
                             static_cast<u64>(static_cast<u32>(cx)),
                             static_cast<u64>(static_cast<u32>(cy))};
        result = static_cast<u32>(m_guest_call(callback, args, 5, m.cpu_is_64bit())) != 0;
        (void)gdi.select_object(mem, old);
        GdiManager::BitmapRgbaCopy drawn;
        const bool ok = gdi.copy_bitmap_rgba(bmp, drawn) && drawn.width == cx &&
                        drawn.height == cy;
        (void)gdi.delete_dc(mem);
        (void)gdi.delete_object(bmp);
        if (!ok) {
            m.set_return(0u);
            return;
        }
        image.width = cx;
        image.height = cy;
        image.rgba = std::move(drawn.rgba);
        image.mask.assign(static_cast<usize>(cx) * cy, 0);
        for (usize i = 0; i < image.mask.size(); ++i) {
            image.mask[i] = is_white(image.rgba.data() + i * 4) ? 0 : 1;
        }
    } else {
        // DST_TEXT / DST_PREFIXTEXT: lData is the string, wData its length
        // (0 = NUL-terminated; text_len2). The callback is not used (text_cb).
        std::string text;
        const GuestAddr text_ptr = static_cast<GuestAddr>(ldata);
        const usize count = static_cast<usize>(wdata);
        if (text_ptr != 0) {
            auto r = wide ? m.read_wide_string(text_ptr, count)
                          : m.read_ansi_string(text_ptr, count);
            if (r.is_ok()) text = wide ? r.value() : capsule::ansi_to_utf8(r.value());
        }
        if (text.empty()) {
            m.set_return(1u);  // text_empty: nothing drawn, error untouched
            return;
        }
        set_last_error(0);  // text_null_cb / text_cb / prefixtext: err=0
        u32 format = DT_SINGLELINE_ | DT_NOCLIP_;
        if (type == DST_TEXT) format |= DT_NOPREFIX_;
        if ((flags & DSS_HIDEPREFIX) != 0) format |= DT_HIDEPREFIX_;
        if ((flags & DSS_PREFIXONLY) != 0) format |= DT_PREFIXONLY_;
        if ((flags & DSS_RIGHT) != 0) format |= DT_RIGHT_;
        i32 tw = 0;
        i32 th = 0;
        gdi.get_text_extent(hdc, text, tw, th);
        const i32 w = cx != 0 ? cx : tw;
        const i32 h = cy != 0 ? cy : th;
        if (transform == 0) {
            i32 left = x, top = y, right = x + w, bottom = y + h;
            (void)gdi.draw_text(hdc, text, left, top, right, bottom, format);
            m.set_return(1u);
            return;
        }
        if (w <= 0 || h <= 0) {
            m.set_return(1u);
            return;
        }
        // The glyph coverage is the foreground: render black on white offscreen
        // in the DC's own font.
        const Handle mem = gdi.create_compatible_dc(hdc);
        const Handle bmp = gdi.create_compatible_bitmap(hdc, w, h);
        if (mem == 0 || bmp == 0) {
            if (mem != 0) (void)gdi.delete_dc(mem);
            if (bmp != 0) (void)gdi.delete_object(bmp);
            m.set_return(0u);
            return;
        }
        const Handle old_bmp = gdi.select_object(mem, bmp);
        const Handle font = gdi.selected_font(hdc);
        const Handle old_font = font != 0 ? gdi.select_object(mem, font) : Handle{0};
        (void)gdi.fill_rect_gdi(mem, 0, 0, w, h, 0xFFFFFF);
        (void)gdi.set_text_color(mem, 0x000000);
        (void)gdi.set_bk_mode(mem, 1 /* TRANSPARENT */);
        i32 left = 0, top = 0, right = w, bottom = h;
        (void)gdi.draw_text(mem, text, left, top, right, bottom, format);
        if (font != 0) (void)gdi.select_object(mem, old_font);
        (void)gdi.select_object(mem, old_bmp);
        GdiManager::BitmapRgbaCopy drawn;
        const bool ok = gdi.copy_bitmap_rgba(bmp, drawn) && drawn.width == w &&
                        drawn.height == h;
        (void)gdi.delete_dc(mem);
        (void)gdi.delete_object(bmp);
        if (!ok) {
            m.set_return(0u);
            return;
        }
        image.width = w;
        image.height = h;
        image.rgba = std::move(drawn.rgba);
        image.mask.assign(static_cast<usize>(w) * h, 0);
        for (usize i = 0; i < image.mask.size(); ++i) {
            image.mask[i] = is_white(image.rgba.data() + i * 4) ? 0 : 1;
        }
    }

    // DSS_NORMAL: the image as it is.
    if (transform == 0) {
        (void)write_dc_region(gdi, hdc, x, y, image.width, image.height, image.rgba);
        m.set_return(result ? 1u : 0u);
        return;
    }

    // The transforms paint colours through the mask onto what is already
    // there. DSS_DISABLED reaches one pixel further right and down.
    const i32 ow = image.width + (transform == DSS_DISABLED ? 1 : 0);
    const i32 oh = image.height + (transform == DSS_DISABLED ? 1 : 0);
    std::vector<u8> out;
    if (!read_dc_region(gdi, hdc, x, y, ow, oh, out)) {
        m.set_return(0u);
        return;
    }
    const auto fg = [&](i32 col, i32 row) {
        return col >= 0 && row >= 0 && col < image.width && row < image.height &&
               image.mask[static_cast<usize>(row) * image.width + col] != 0;
    };
    for (i32 row = 0; row < oh; ++row) {
        for (i32 col = 0; col < ow; ++col) {
            u8* px = out.data() + (static_cast<usize>(row) * ow + col) * 4;
            if (transform == DSS_MONO) {
                if (fg(col, row)) put_colorref(px, fore);
            } else if (transform == DSS_UNION) {
                // A checkerboard anchored at the image's own top-left
                // (bitmap32_union / _even / _brushorg).
                if (fg(col, row) && ((col + row) & 1) == 0) put_colorref(px, fore);
            } else {
                // Embossed: the highlight one pixel down-right, then the
                // shadow on top (bitmap32_disabled, mono_disabled).
                if (fg(col, row)) {
                    put_colorref(px, sys_color(kColorBtnShadow));
                } else if (fg(col - 1, row - 1)) {
                    put_colorref(px, sys_color(kColorBtnHighlight));
                }
            }
        }
    }
    (void)write_dc_region(gdi, hdc, x, y, ow, oh, out);
    m.set_return(result ? 1u : 0u);
}

void User32ExtManager::impl_DrawTextExA(Marshaler& m) {
    draw_text_ex_impl(m, /*wide=*/false);
}

void User32ExtManager::impl_DrawTextExW(Marshaler& m) {
    draw_text_ex_impl(m, /*wide=*/true);
}

void User32ExtManager::draw_text_ex_impl(Marshaler& m, bool wide) {
    // int DrawTextEx(HDC, LPTSTR, int, LPRECT, UINT, LPDRAWTEXTPARAMS)
    GdiManager* gdi = m_gdi_mgr != nullptr ? m_gdi_mgr : (m_paint_mgr != nullptr ? m_paint_mgr->gdi() : nullptr);
    PaintingManager::marshal_draw_text(gdi, m, wide, /*ex=*/true);
}

void User32ExtManager::impl_TabbedTextOutA(Marshaler& m) {
    impl_TabbedTextOutW(m);
}

void User32ExtManager::impl_TabbedTextOutW(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    (void)m.arg_ptr(3); (void)m.arg(4); (void)m.arg(5);
    (void)m.arg_ptr(6); (void)m.arg(7);
    u32 result = 8u | (16u << 16); // MAKELONG(width=8, height=16)
    VITREO_TRACE("[u32ext] TabbedTextOutW -> 0x%x\n", result);
    m.set_return(result);
}

void User32ExtManager::impl_GrayStringA(Marshaler& m) {
    gray_string_impl(m, /*wide=*/false);
}

void User32ExtManager::impl_GrayStringW(Marshaler& m) {
    gray_string_impl(m, /*wide=*/true);
}

void User32ExtManager::gray_string_impl(Marshaler& m, bool wide) {
    // BOOL GrayString(HDC, HBRUSH, GRAYSTRINGPROC, LPARAM, int, int, int, int, int)
    const u64 hdc = m.arg(0);
    (void)m.arg(1); // hBrush
    const GuestAddr output_func = m.arg_ptr(2);
    const GuestAddr lp_data = m.arg_ptr(3);
    i32 n_count = static_cast<i32>(m.arg(4));
    const i32 x = static_cast<i32>(m.arg(5));
    const i32 y = static_cast<i32>(m.arg(6));
    (void)m.arg(7); // nWidth
    (void)m.arg(8); // nHeight

    if (output_func != 0) {
        if (!m_guest_call) {
            VITREO_TRACE("[u32ext] GrayString%s callback with no guest_call -> TRUE\n",
                         wide ? "W" : "A");
            m.set_return(1u);
            return;
        }
        u64 args[3] = {
            hdc,
            static_cast<u64>(lp_data),
            static_cast<u64>(static_cast<u32>(n_count)),
        };
        const u64 cb_ret = m_guest_call(output_func, args, 3, m.cpu_is_64bit());
        m.set_return(cb_ret != 0 ? 1u : 0u);
        return;
    }

    std::string text;
    if (lp_data != 0) {
        const usize maxc = (n_count > 0) ? static_cast<usize>(n_count) : 0;
        if (wide) {
            auto r = m.read_wide_string(lp_data, maxc);
            if (r.is_ok()) {
                text = r.value();
            }
        } else {
            auto r = m.read_ansi_string(lp_data, maxc);
            if (r.is_ok()) {
                text = r.value();
            }
        }
    }

    bool ok = true;
    if (m_gdi_mgr) {
        ok = m_gdi_mgr->text_out(static_cast<Handle>(hdc), x, y, text);
    }
    VITREO_TRACE("[u32ext] GrayString%s hdc=0x%llx -> %s\n",
                 wide ? "W" : "A",
                 static_cast<unsigned long long>(hdc),
                 ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_DrawEdge(Marshaler& m) {
    // BOOL DrawEdge(HDC, LPRECT, UINT edge, UINT grfFlags): frame_drawing.cpp.
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr rect = m.arg_ptr(1);
    Rect rc{};
    if (m_gdi_mgr == nullptr || rect == 0 ||
        m.memory().read_bytes(rect, reinterpret_cast<u8*>(&rc), sizeof(rc)).is_err()) {
        m.set_return(0u);
        return;
    }
    const bool ok = user_draw::draw_edge(*m_gdi_mgr, hdc, rc, m.arg32(2), m.arg32(3));
    if (ok) (void)m.memory().write_bytes(rect, reinterpret_cast<const u8*>(&rc), sizeof(rc));
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_DrawFrameControl(Marshaler& m) {
    // BOOL DrawFrameControl(HDC, LPRECT, UINT type, UINT state): frame_drawing.cpp.
    const auto hdc = static_cast<Handle>(m.arg(0));
    const GuestAddr rect = m.arg_ptr(1);
    Rect rc{};
    if (m_gdi_mgr == nullptr || rect == 0 ||
        m.memory().read_bytes(rect, reinterpret_cast<u8*>(&rc), sizeof(rc)).is_err()) {
        m.set_return(0u);
        return;
    }
    m.set_return(user_draw::draw_frame_control(*m_gdi_mgr, hdc, rc, m.arg32(2), m.arg32(3)) ? 1u : 0u);
}

void User32ExtManager::impl_GetWindowDC(Marshaler& m) {
    u32 hwnd = static_cast<u32>(m.arg(0));
    if (m_gdi_mgr) {
        auto h = m_gdi_mgr->get_dc(static_cast<Handle>(hwnd));
        VITREO_TRACE("[u32ext] GetWindowDC hwnd=0x%x -> hdc=0x%x\n", hwnd, h);
        m.set_return(static_cast<u64>(h));
    } else {
        VITREO_TRACE("[u32ext] GetWindowDC hwnd=0x%x -> 0xDC000001 (stub)\n", hwnd);
        m.set_return(0xDC000001u);
    }
}

} // namespace vitreo::winapi
