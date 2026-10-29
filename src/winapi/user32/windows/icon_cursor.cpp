// SPDX-License-Identifier: LGPL-2.1-only

/// @file icon_cursor.cpp
/// @brief Icon and cursor APIs.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/user32_ext.hpp"
#include "windowing/model/diag_flags.hpp"
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

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// ----- Icon/Cursor APIs -----
//
// Icons and cursors are one USER object type (Learn: ICONINFO). The image of
// each lives in ResourceManager as IconPlanes; every export here reads or
// writes those planes, and GDI draws them. Behaviour is measured on Windows 11
// by tools/oracle_probe/s3gdi_icon_probe.

namespace {

constexpr u32 kErrorInvalidCursorHandle = 1402;
constexpr u32 kErrorCallNotImplemented = 120;
// SM_CXICON / SM_CYICON / SM_CXCURSOR / SM_CYCURSOR at 96 dpi
// (s3gdi_icon_probe "DrawIcon ... extent 32x32").
constexpr u32 kDefaultIconSize = 32;

/// VITREO_INPUT_DIAG also covers cursor CREATION, not just SetCursor: the
/// question "does the guest supply this shape at all" cannot be answered from
/// the SetCursor stream alone.
bool cursor_diag() {
    const bool on = windowing::diag::input_diag();
    return on;
}

/// The stock system images carry Windows artwork Vitreo does not have; say
/// so once per export instead of answering with a made-up bitmap.
void note_stock_image(const char* api) {
    static std::atomic<u32> logged{0};
    const u32 bit = static_cast<u32>(std::hash<std::string_view>{}(api) & 31u);
    if ((logged.fetch_or(1u << bit) & (1u << bit)) == 0) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!%s: stock system icon/cursor "
                             "artwork (IDI_*/IDC_*)\n", api);
    }
}

/// A colour bitmap's planes as CreateIconIndirect reads it.
std::vector<u8> mask_bits(const GdiManager::BitmapRgbaCopy& mask) {
    std::vector<u8> bits(static_cast<usize>(mask.width) * static_cast<usize>(mask.height));
    for (usize i = 0; i < bits.size(); ++i) {
        const u8* p = mask.rgba.data() + i * 4u;
        // A monochrome bitmap holds pure black/white; any other bitmap is
        // read as the nearer of the two.
        const i32 to_white = (255 - p[0]) * (255 - p[0]) + (255 - p[1]) * (255 - p[1]) +
                             (255 - p[2]) * (255 - p[2]);
        const i32 to_black = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
        bits[i] = to_white < to_black ? 1 : 0;
    }
    return bits;
}

/// Monochrome version of colour planes (LR_MONOCHROME): XOR set where the
/// colour is nearer white.
IconPlanes to_monochrome(const IconPlanes& in) {
    if (in.monochrome()) return in;
    IconPlanes out = in;
    const usize n = static_cast<usize>(in.width) * in.height;
    out.mask.resize(n * 2u);
    for (usize i = 0; i < n; ++i) {
        const u8* p = in.color.data() + i * 4u;
        const i32 to_white = (255 - p[0]) * (255 - p[0]) + (255 - p[1]) * (255 - p[1]) +
                             (255 - p[2]) * (255 - p[2]);
        const i32 to_black = p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
        out.mask[n + i] = to_white < to_black ? 1 : 0;
    }
    out.color.clear();
    return out;
}

}  // namespace

void User32ExtManager::impl_CopyIcon(Marshaler& m) {
    const u32 icon = static_cast<u32>(m.arg(0));
    if (m_resource_mgr == nullptr) { m.set_return(0); return; }
    const auto kind = m_resource_mgr->icon_kind(icon);
    if (kind == ResourceManager::IconKind::None) {
        set_last_error(kErrorInvalidCursorHandle);
        m.set_return(0);
        return;
    }
    auto planes = m_resource_mgr->icon_planes(icon);
    if (!planes) {
        // A stock image has no planes here; its copy is the same immutable
        // shared object (DestroyIcon on it is TRUE and harmless, as on Windows).
        m.set_return(icon);
        return;
    }
    planes->res_id = 0;
    planes->module_name.clear();
    planes->res_name.clear();
    const u32 copy = m_resource_mgr->create_icon_object(std::move(*planes));
    if (cursor_diag()) {
        std::fprintf(stderr, "[input] CopyIcon 0x%x -> 0x%x\n", icon, copy);
    }
    m.set_return(copy);
}

void User32ExtManager::impl_GetIconInfo(Marshaler& m) {
    const u32 icon = static_cast<u32>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    if (m_resource_mgr == nullptr || m_gdi_mgr == nullptr || out == 0) {
        m.set_return(0);
        return;
    }
    const auto planes = m_resource_mgr->icon_planes(icon);
    if (!planes) {
        // A destroyed or bogus handle: FALSE, last error untouched
        // ("GetIconInfo(destroyed)=0 err=57005").
        if (m_resource_mgr->icon_kind(icon) == ResourceManager::IconKind::StockSystem) {
            note_stock_image("GetIconInfo");
            set_last_error(kErrorCallNotImplemented);
        }
        m.set_return(0);
        return;
    }
    // hbmMask is a 1bpp DDB (twice as tall for a monochrome image) and
    // hbmColor a 32bpp DDB that keeps the alpha byte; both new, the caller's
    // to delete (s3gdi_icon_probe "alpha GetIconInfo").
    const i32 w = static_cast<i32>(planes->width);
    const i32 h = static_cast<i32>(planes->height);
    const Handle mask = m_gdi_mgr->create_mono_bitmap(w, planes->monochrome() ? h * 2 : h, planes->mask);
    const Handle color = planes->monochrome() ? Handle{0}
                                              : m_gdi_mgr->create_bitmap_from_rgba(w, h, planes->color);
    const bool is64 = m.cpu_is_64bit();
    (void)m.write_u32(out + 0, planes->is_icon ? 1u : 0u);
    (void)m.write_u32(out + 4, planes->hotspot_x);
    (void)m.write_u32(out + 8, planes->hotspot_y);
    if (is64) {
        (void)m.write_u32(out + 12, 0);
        (void)m.memory().write_u64(out + 16, mask);
        (void)m.memory().write_u64(out + 24, color);
    } else {
        (void)m.write_u32(out + 12, mask);
        (void)m.write_u32(out + 16, color);
    }
    m.set_return(1u);
}

void User32ExtManager::impl_GetIconInfoEx(Marshaler& m, bool wide) {
    // ICONINFOEX: cbSize must be the structure's size, 1080/1068 (W) or
    // 560/548 (A) on x64/x86 (SDK winuser.h; "GetIconInfoExW(cbSize=8)=0
    // err=87").
    const u32 icon = static_cast<u32>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    const bool is64 = m.cpu_is_64bit();
    const u32 want = wide ? (is64 ? 1080u : 1068u) : (is64 ? 560u : 548u);
    if (out == 0 || m.read_u32(out).value_or(0) != want) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_resource_mgr == nullptr || m_gdi_mgr == nullptr) { m.set_return(0); return; }
    const auto planes = m_resource_mgr->icon_planes(icon);
    if (!planes) {
        if (m_resource_mgr->icon_kind(icon) == ResourceManager::IconKind::StockSystem) {
            note_stock_image(wide ? "GetIconInfoExW" : "GetIconInfoExA");
            set_last_error(kErrorCallNotImplemented);
        }
        m.set_return(0);
        return;
    }
    const i32 w = static_cast<i32>(planes->width);
    const i32 h = static_cast<i32>(planes->height);
    const Handle mask = m_gdi_mgr->create_mono_bitmap(w, planes->monochrome() ? h * 2 : h, planes->mask);
    const Handle color = planes->monochrome() ? Handle{0}
                                              : m_gdi_mgr->create_bitmap_from_rgba(w, h, planes->color);
    (void)m.write_u32(out + 4, planes->is_icon ? 1u : 0u);
    (void)m.write_u32(out + 8, planes->hotspot_x);
    (void)m.write_u32(out + 12, planes->hotspot_y);
    GuestAddr res_id = 0;
    if (is64) {
        (void)m.memory().write_u64(out + 16, mask);
        (void)m.memory().write_u64(out + 24, color);
        res_id = out + 32;
    } else {
        (void)m.write_u32(out + 16, mask);
        (void)m.write_u32(out + 20, color);
        res_id = out + 24;
    }
    (void)m.write_u16(res_id, planes->res_id);
    const GuestAddr mod = res_id + 2;
    const GuestAddr res = mod + (wide ? 520u : 260u);
    auto put = [&](GuestAddr at, const std::u16string& text) {
        for (u32 i = 0; i < 260; ++i) {
            const char16_t c = i < text.size() ? text[i] : u'\0';
            if (wide) (void)m.write_u16(at + i * 2, static_cast<u16>(c));
            else (void)m.write_u8(at + i, c < 0x80 ? static_cast<u8>(c) : u8{'?'});
            if (c == u'\0') break;
        }
    };
    put(mod, planes->module_name);
    put(res, planes->res_name);
    m.set_return(1u);
}

void User32ExtManager::impl_DrawIconEx(Marshaler& m) {
    // BOOL DrawIconEx(hdc, x, y, hIcon, cx, cy, istepIfAniCur, hbrFlickerFreeDraw, diFlags)
    constexpr u32 DI_DEFAULTSIZE = 0x8;
    const Handle hdc = static_cast<Handle>(m.arg(0));
    const i32 x = static_cast<i32>(m.arg32(1));
    const i32 y = static_cast<i32>(m.arg32(2));
    const u32 icon = static_cast<u32>(m.arg(3));
    i32 cx = static_cast<i32>(m.arg32(4));
    i32 cy = static_cast<i32>(m.arg32(5));
    (void)m.arg32(6);  // istepIfAniCur: animated cursors draw their first frame
    const Handle brush = static_cast<Handle>(m.arg(7));
    const u32 flags = m.arg32(8);
    if (m_resource_mgr == nullptr || m_gdi_mgr == nullptr) { m.set_return(0); return; }
    const auto kind = m_resource_mgr->icon_kind(icon);
    if (kind == ResourceManager::IconKind::None) {
        set_last_error(kErrorInvalidCursorHandle);  // "DrawIconEx NULL icon": 1402
        m.set_return(0);
        return;
    }
    const auto planes = m_resource_mgr->icon_planes(icon);
    if (!planes) {
        note_stock_image("DrawIconEx");
        set_last_error(kErrorCallNotImplemented);
        m.set_return(0);
        return;
    }
    if (cx == 0) cx = (flags & DI_DEFAULTSIZE) ? static_cast<i32>(kDefaultIconSize) : static_cast<i32>(planes->width);
    if (cy == 0) cy = (flags & DI_DEFAULTSIZE) ? static_cast<i32>(kDefaultIconSize) : static_cast<i32>(planes->height);
    GdiManager::IconDraw draw;
    draw.width = planes->width;
    draw.height = planes->height;
    draw.color = planes->monochrome() ? nullptr : planes->color.data();
    draw.mask = planes->mask.data();
    draw.alpha = planes->has_alpha();
    u32 error = 0;
    const bool ok = m_gdi_mgr->draw_icon(hdc, x, y, cx, cy, draw, flags, brush, &error);
    if (!ok && error != 0) set_last_error(error);
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_CopyImage(Marshaler& m) {
    // HANDLE CopyImage(HANDLE h, UINT type, int cx, int cy, UINT flags)
    constexpr u32 IMAGE_BITMAP = 0, IMAGE_ICON = 1, IMAGE_CURSOR = 2;
    constexpr u32 LR_MONOCHROME = 0x1, LR_COPYRETURNORG = 0x4, LR_COPYDELETEORG = 0x8;
    constexpr u32 LR_DEFAULTSIZE = 0x40;
    const u32 h = static_cast<u32>(m.arg(0));
    const u32 type = m.arg32(1);
    i32 cx = static_cast<i32>(m.arg32(2));
    i32 cy = static_cast<i32>(m.arg32(3));
    const u32 flags = m.arg32(4);
    if (type == IMAGE_BITMAP) {
        if (m_gdi_mgr == nullptr) { m.set_return(0); return; }
        u32 error = 0;
        const Handle out = m_gdi_mgr->copy_bitmap(static_cast<Handle>(h), cx, cy, flags, &error);
        if (out == 0 && error != 0) set_last_error(error);
        m.set_return(out);
        return;
    }
    if (type != IMAGE_ICON && type != IMAGE_CURSOR) {
        // "CopyImage type 7": NULL, ERROR_INVALID_PARAMETER. Enhanced
        // metafiles (type 3) have no object model in Vitreo's GDI.
        if (type == 3) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!CopyImage: IMAGE_ENHMETAFILE\n");
            }
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_resource_mgr == nullptr) { m.set_return(0); return; }
    if (m_resource_mgr->icon_kind(h) == ResourceManager::IconKind::None) {
        set_last_error(kErrorInvalidCursorHandle);
        m.set_return(0);
        return;
    }
    auto planes = m_resource_mgr->icon_planes(h);
    if (!planes) {
        note_stock_image("CopyImage");
        set_last_error(kErrorCallNotImplemented);
        m.set_return(0);
        return;
    }
    if (cx == 0) cx = (flags & LR_DEFAULTSIZE) ? static_cast<i32>(kDefaultIconSize) : static_cast<i32>(planes->width);
    if (cy == 0) cy = (flags & LR_DEFAULTSIZE) ? static_cast<i32>(kDefaultIconSize) : static_cast<i32>(planes->height);
    const bool same = static_cast<u32>(cx) == planes->width && static_cast<u32>(cy) == planes->height;
    // "CopyImage RETURNORG same=1": an unchanged size answers the original.
    if ((flags & LR_COPYRETURNORG) != 0 && same && (flags & LR_MONOCHROME) == 0) {
        m.set_return(h);
        return;
    }
    IconPlanes copy = same ? *planes : scale_icon_planes(*planes, static_cast<u32>(cx), static_cast<u32>(cy));
    if (!same) {
        if (copy.is_icon) {
            copy.hotspot_x = copy.width / 2;
            copy.hotspot_y = copy.height / 2;
        } else {
            copy.hotspot_x = planes->hotspot_x * copy.width / planes->width;
            copy.hotspot_y = planes->hotspot_y * copy.height / planes->height;
        }
    }
    if ((flags & LR_MONOCHROME) != 0) copy = to_monochrome(copy);
    const u32 out = m_resource_mgr->create_icon_object(std::move(copy));
    if (out != 0 && (flags & LR_COPYDELETEORG) != 0) {
        (void)m_resource_mgr->destroy_icon_object(h);
    }
    m.set_return(out);
}

void User32ExtManager::impl_DrawIcon(Marshaler& m) {
    // DrawIcon(hdc, x, y, hIcon) = DrawIconEx(hdc, x, y, hIcon, 0, 0, 0,
    // NULL, DI_NORMAL | DI_COMPAT | DI_DEFAULTSIZE): SM_CXICON x SM_CYICON.
    const Handle hdc = static_cast<Handle>(m.arg(0));
    const i32 x = static_cast<i32>(m.arg32(1));
    const i32 y = static_cast<i32>(m.arg32(2));
    const u32 icon = static_cast<u32>(m.arg(3));
    if (m_resource_mgr == nullptr || m_gdi_mgr == nullptr) { m.set_return(0); return; }
    if (m_resource_mgr->icon_kind(icon) == ResourceManager::IconKind::None) {
        set_last_error(kErrorInvalidCursorHandle);
        m.set_return(0);
        return;
    }
    const auto planes = m_resource_mgr->icon_planes(icon);
    if (!planes) {
        note_stock_image("DrawIcon");
        set_last_error(kErrorCallNotImplemented);
        m.set_return(0);
        return;
    }
    GdiManager::IconDraw draw;
    draw.width = planes->width;
    draw.height = planes->height;
    draw.color = planes->monochrome() ? nullptr : planes->color.data();
    draw.mask = planes->mask.data();
    draw.alpha = planes->has_alpha();
    u32 error = 0;
    const bool ok = m_gdi_mgr->draw_icon(hdc, x, y, static_cast<i32>(kDefaultIconSize),
                                         static_cast<i32>(kDefaultIconSize), draw, 0x3, 0, &error);
    if (!ok && error != 0) set_last_error(error);
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_DestroyIcon(Marshaler& m) {
    // DestroyIcon and DestroyCursor destroy either kind; a second destroy is
    // FALSE with ERROR_INVALID_CURSOR_HANDLE, a shared/stock image is TRUE
    // and survives (s3gdi_icon_probe "# Destroy").
    const u32 icon = static_cast<u32>(m.arg(0));
    const bool ok = m_resource_mgr != nullptr && m_resource_mgr->destroy_icon_object(icon);
    if (!ok && m_resource_mgr != nullptr && m_resource_mgr->icon_was_destroyed(icon)) {
        set_last_error(kErrorInvalidCursorHandle);
    }
    m.set_return(ok ? 1u : 0u);
}

void User32ExtManager::impl_CreateIconFromResource(Marshaler& m) {
    // CreateIconFromResource(presbits, size, fIcon, ver) =
    // CreateIconFromResourceEx(..., 0, 0, LR_DEFAULTSIZE | LR_SHARED).
    create_icon_from_resource(m, 0, 0, 0x40);
}

void User32ExtManager::impl_CreateIconFromResourceEx(Marshaler& m) {
    create_icon_from_resource(m, static_cast<i32>(m.arg32(4)), static_cast<i32>(m.arg32(5)),
                              m.arg32(6));
}

void User32ExtManager::create_icon_from_resource(Marshaler& m, i32 cx, i32 cy, u32 flags) {
    constexpr u32 LR_MONOCHROME = 0x1;
    constexpr u32 LR_DEFAULTSIZE = 0x40;
    const GuestAddr bits = m.arg_ptr(0);
    const u32 size = m.arg32(1);
    const bool is_icon = m.arg32(2) != 0;
    const u32 version = m.arg32(3);
    if (m_resource_mgr == nullptr || bits == 0 || size == 0) {
        m.set_return(0);
        return;
    }
    std::vector<u8> blob(size);
    if (m.memory().read_bytes(bits, blob.data(), size).is_err()) {
        m.set_return(0);
        return;
    }
    // An animated cursor arrives here as a whole RIFF/ACON file. Windows
    // accepts one; a caller whose animated cursor fails falls back to a stock
    // shape such as IDC_WAIT.
    if (auto ani = windowing::decode_ani_cursor(blob.data(), size); !ani.frames.empty()) {
        const u32 handle = m_resource_mgr->create_animated_cursor_rgba(
            ani.hotspot_x, ani.hotspot_y, ani.width, ani.height, std::move(ani.frames),
            std::move(ani.delays_ms));
        if (cursor_diag()) {
            std::fprintf(stderr, "[input] CreateIconFromResource animated -> 0x%x\n", handle);
        }
        m.set_return(handle);
        return;
    }
    // Learn: dwVer is 0x00020000 or 0x00030000 ("fromres version 2 -> ok").
    if (version < 0x00020000u || version > 0x00030000u) {
        m.set_return(0);
        return;
    }
    if (cx == 0 && (flags & LR_DEFAULTSIZE) != 0) cx = static_cast<i32>(kDefaultIconSize);
    if (cy == 0 && (flags & LR_DEFAULTSIZE) != 0) cy = static_cast<i32>(kDefaultIconSize);
    auto planes = decode_icon_image(blob, is_icon, static_cast<u32>(std::max(cx, 0)),
                                    static_cast<u32>(std::max(cy, 0)));
    if (!planes) {
        // A truncated or foreign buffer: NULL, last error untouched.
        if (cursor_diag()) {
            std::fprintf(stderr, "[input] CreateIconFromResource FAILED size=%u\n", size);
        }
        m.set_return(0);
        return;
    }
    if ((flags & LR_MONOCHROME) != 0) *planes = to_monochrome(*planes);
    const u32 handle = m_resource_mgr->create_icon_object(std::move(*planes));
    if (cursor_diag()) {
        std::fprintf(stderr, "[input] CreateIconFromResource %s -> 0x%x\n",
                     is_icon ? "icon" : "cursor", handle);
    }
    m.set_return(handle);
}

void User32ExtManager::impl_CreateIconIndirect(Marshaler& m) {
    const GuestAddr info = m.arg_ptr(0);
    if (info == 0 || m_resource_mgr == nullptr || m_gdi_mgr == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const bool is64 = m.cpu_is_64bit();
    const u32 ficon = m.memory().read_u32(info + 0).value_or(1);
    const u32 hot_x = m.memory().read_u32(info + 4).value_or(0);
    const u32 hot_y = m.memory().read_u32(info + 8).value_or(0);
    const Handle hbm_mask = static_cast<Handle>(
        is64 ? m.memory().read_u64(info + 16).value_or(0) : m.memory().read_u32(info + 12).value_or(0));
    const Handle hbm_color = static_cast<Handle>(
        is64 ? m.memory().read_u64(info + 24).value_or(0) : m.memory().read_u32(info + 16).value_or(0));

    // A missing or bogus mask: NULL, last error untouched (s3gdi_icon_probe
    // "no mask", "bogus mask"). The caller keeps its bitmaps.
    GdiManager::BitmapRgbaCopy mask{};
    if (hbm_mask == 0 || !m_gdi_mgr->copy_bitmap_rgba(hbm_mask, mask)) {
        m.set_return(0);
        return;
    }
    IconPlanes planes;
    planes.is_icon = ficon != 0;
    planes.mask = mask_bits(mask);
    if (hbm_color != 0) {
        GdiManager::BitmapRgbaCopy color{};
        if (!m_gdi_mgr->copy_bitmap_rgba(hbm_color, color)) {
            m.set_return(0);
            return;
        }
        // "mask 8x8 colour 4x4": ERROR_INVALID_PARAMETER.
        if (color.width != mask.width || color.height != mask.height) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        planes.width = static_cast<u32>(color.width);
        planes.height = static_cast<u32>(color.height);
        planes.color = std::move(color.rgba);
        // Only a 32bpp colour bitmap carries an alpha byte.
        if (color.bpp != 32) {
            for (usize i = 3; i < planes.color.size(); i += 4) planes.color[i] = 0;
        }
    } else {
        // Monochrome: the mask holds AND rows then XOR rows.
        if (mask.height < 2 || (mask.height & 1) != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        planes.width = static_cast<u32>(mask.width);
        planes.height = static_cast<u32>(mask.height / 2);
    }
    // Learn (ICONINFO): an icon's hotspot is always its centre.
    planes.hotspot_x = planes.is_icon ? planes.width / 2 : hot_x;
    planes.hotspot_y = planes.is_icon ? planes.height / 2 : hot_y;
    const u32 handle = m_resource_mgr->create_icon_object(std::move(planes));
    VITREO_TRACE("[u32ext] CreateIconIndirect %s mask=0x%x color=0x%x -> 0x%x\n",
                 ficon ? "icon" : "cursor", hbm_mask, hbm_color, handle);
    if (cursor_diag()) {
        std::fprintf(stderr, "[input] CreateIconIndirect %s hot=%u,%u -> 0x%x\n",
                     ficon ? "icon" : "cursor", hot_x, hot_y, handle);
    }
    m.set_return(handle);
}

void User32ExtManager::impl_CreateCursor(Marshaler& m) {
    // HCURSOR CreateCursor(hInst, xHotSpot, yHotSpot, nWidth, nHeight,
    //                      pvANDPlane, pvXORPlane): monochrome planes with
    // WORD-aligned rows, top row first (Learn: CreateCursor).
    (void)m.arg(0);
    const i32 hotspot_x = static_cast<i32>(m.arg32(1));
    const i32 hotspot_y = static_cast<i32>(m.arg32(2));
    const i32 width = static_cast<i32>(m.arg32(3));
    const i32 height = static_cast<i32>(m.arg32(4));
    const GuestAddr and_ptr = m.arg_ptr(5);
    const GuestAddr xor_ptr = m.arg_ptr(6);
    if (m_resource_mgr == nullptr || width <= 0 || height <= 0 || width > 256 || height > 256 ||
        and_ptr == 0 || xor_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u32 stride = ((static_cast<u32>(width) + 15u) / 16u) * 2u;
    const u32 bytes = stride * static_cast<u32>(height);
    std::vector<u8> and_bits(bytes);
    std::vector<u8> xor_bits(bytes);
    if (m.memory().read_bytes(and_ptr, and_bits.data(), bytes).is_err() ||
        m.memory().read_bytes(xor_ptr, xor_bits.data(), bytes).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    IconPlanes planes;
    planes.is_icon = false;
    planes.width = static_cast<u32>(width);
    planes.height = static_cast<u32>(height);
    planes.hotspot_x = static_cast<u32>(hotspot_x);
    planes.hotspot_y = static_cast<u32>(hotspot_y);
    planes.mask.resize(static_cast<usize>(width) * height * 2u);
    for (i32 y = 0; y < height; ++y) {
        for (i32 x = 0; x < width; ++x) {
            const usize byte = static_cast<usize>(y) * stride + static_cast<usize>(x) / 8u;
            const u8 bit = static_cast<u8>(0x80u >> (x % 8));
            planes.mask[static_cast<usize>(y) * width + x] = (and_bits[byte] & bit) ? 1 : 0;
            planes.mask[static_cast<usize>(height + y) * width + x] = (xor_bits[byte] & bit) ? 1 : 0;
        }
    }
    const u32 handle = m_resource_mgr->create_icon_object(std::move(planes));
    VITREO_TRACE("[u32ext] CreateCursor %dx%d -> 0x%x\n", width, height, handle);
    if (cursor_diag()) {
        std::fprintf(stderr, "[input] CreateCursor %dx%d hot=%d,%d -> 0x%x\n",
                     width, height, hotspot_x, hotspot_y, handle);
    }
    m.set_return(handle);
}

void User32ExtManager::impl_DestroyCursor(Marshaler& m) {
    impl_DestroyIcon(m);
}

} // namespace vitreo::winapi
