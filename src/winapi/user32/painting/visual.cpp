// SPDX-License-Identifier: LGPL-2.1-only

/// @file visual.cpp
/// @brief System colors, cursors, LoadImage/LoadBitmap, and MessageBoxIndirect.
///
/// System colors, cursors, and resource-backed image loaders: GetSysColor,
/// SystemParametersInfoW, SetCursor, ReleaseDC, and LoadImage/LoadBitmap.
/// MessageBoxIndirectW lives here because it was already filed under the
/// same banner.
///
/// Closed cut from user32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/user32/user32_ext.hpp"
#include "windowing/model/diag_flags.hpp"
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

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>

#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

enum class OemBitmapLookup { Found, Retired, Unknown };

/// LoadBitmap / LoadImage(IMAGE_BITMAP) with a NULL hInstance and an integer
/// id name an OEM bitmap, never the exe's own resources
/// (tools/oracle_probe/obm_bitmaps_probe): an OBM_* id Windows provides
/// yields that bitmap, a retired OBM_OLD_* id NULL with the last error
/// untouched, any other id NULL with ERROR_RESOURCE_NAME_NOT_FOUND. The host
/// draws the bitmap with its own controls; ids it cannot draw yet fail as
/// unknown ids do, loudly.
OemBitmapLookup oem_bitmap(windowing::BitmapHost& host, u32 id, u32& handle) {
    handle = 0;
    // winuser.h OBM_OLD_* ids that Windows 10/11 no longer loads.
    constexpr u16 kRetired[] = {32755, 32756, 32757, 32762, 32763, 32764, 32765, 32767};
    for (const u16 retired : kRetired) {
        if (retired == id) return OemBitmapLookup::Retired;
    }
    // winuser.h OBM_* ids Windows loads: OBM_LFARROWI (32734) .. OBM_CLOSE
    // (32754), OBM_BTNCORNERS, OBM_CHECKBOXES, OBM_CHECK, OBM_BTSIZE, OBM_SIZE.
    const bool provided = (id >= 32734 && id <= 32754) || (id >= 32758 && id <= 32761) || id == 32766;
    if (!provided) return OemBitmapLookup::Unknown;
    handle = host.register_oem_bitmap(id);
    if (handle != 0) return OemBitmapLookup::Found;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!LoadBitmap: OEM bitmap %u (only OBM_CHECKBOXES is drawn)\n", id);
    }
    return OemBitmapLookup::Unknown;
}

}  // namespace

// ---------- GDI / visual ----------

u32 User32ExtManager::sys_color(u32 index) const {
    return sys_colors::get(index);
}

void User32ExtManager::impl_GetSysColor(Marshaler& m) {
    u32 nIndex = static_cast<u32>(m.arg(0));
    // sys_color_brush_probe: an undefined index reads 0 with
    // ERROR_INVALID_PARAMETER.
    if (nIndex >= sys_colors::kCount) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 color = sys_color(nIndex);
    VITREO_TRACE( "[user32_ext] GetSysColor(%u) → 0x%06X\n", nIndex, color);
    m.set_return(color);
}

u32 User32ExtManager::bitmap_miss_error(GuestAddr module_base) const {
    // ERROR_RESOURCE_TYPE_NOT_FOUND when the module has no bitmaps at all,
    // ERROR_RESOURCE_NAME_NOT_FOUND when it has others.
    auto info = try_get_resource_dir_from_module(m_memory, module_base);
    if (!info) return 1813u;
    const auto names = windowing::enumerate_resource_names(m_memory, info->image_base, info->rsrc_rva,
                                                           info->rsrc_size, windowing::RT::BITMAP, false, nullptr, 0);
    return names.empty() ? 1813u : 1814u;
}

void User32ExtManager::impl_LoadBitmapW(Marshaler& m) {
    // LoadBitmapW(hInstance, lpBitmapName) is equivalent to
    // LoadImageW(hInstance, lpBitmapName, IMAGE_BITMAP, 0, 0, 0)
    u64 hInst = m.arg(0);
    GuestAddr name = m.arg_ptr(1);

    // IMAGE_BITMAP = 0
    // cx/cy = 0 (use resource size)
    // fuLoad = 0
    u32 handle = 0;
    if (hInst == 0 && name < 0x10000) {
        const auto lookup = oem_bitmap(m_host, static_cast<u32>(name), handle);
        if (lookup == OemBitmapLookup::Unknown) set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
        VITREO_TRACE("[user32_ext] LoadBitmapW(hInst=0x0, oem=%llu) -> 0x%x\n",
                     static_cast<unsigned long long>(name), handle);
        m.set_return(handle);
        return;
    }
    GuestAddr module_base = static_cast<GuestAddr>(hInst);
    if (module_base == 0) {
        module_base = m_rsrc_image_base;
    }

    if (module_base != 0 && name != 0) {
        if (name < 0x10000) {
            // Integer resource ID
            u16 res_id = static_cast<u16>(name & 0xFFFF);
            if (auto info = try_get_resource_dir_from_module(m_memory, module_base)) {
                auto res = windowing::find_resource(
                    m_memory, info->image_base, info->rsrc_rva, info->rsrc_size,
                    windowing::RT::BITMAP, res_id);
                if (res.data && res.size > 0) {
                    auto bmp_file = dib_to_bmp_file(res.data, res.size);
                    if (!bmp_file.empty()) {
                        handle = m_host.register_bitmap_bmp(bmp_file);
                    }
                }
            }
        } else {
            // String resource name - read it and look up by name
            auto str_res = m.read_wide_string(name, 256);
            std::string str_name = str_res.is_ok() ? str_res.value() : "";
            
            if (!str_name.empty()) {
                // Convert to UTF-16LE for resource lookup
                std::u16string u16name;
                for (char c : str_name) {
                    u16name.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
                }
                
                if (auto info = try_get_resource_dir_from_module(m_memory, module_base)) {
                    auto res = windowing::find_resource_by_string_name(
                        m_memory, info->image_base, info->rsrc_rva, info->rsrc_size,
                        windowing::RT::BITMAP, u16name.data(), static_cast<u16>(u16name.size()));
                    if (res.data && res.size > 0) {
                        auto bmp_file = dib_to_bmp_file(res.data, res.size);
                        if (!bmp_file.empty()) {
                            handle = m_host.register_bitmap_bmp(bmp_file);
                        }
                    }
                }
            }
            
            if (handle == 0) set_last_error(bitmap_miss_error(module_base));  // "LoadBitmapW(NOSUCH) err=1813"
            VITREO_TRACE( "[user32_ext] LoadBitmapW(hInst=0x%llx, name=\"%s\") -> 0x%x\n",
                         static_cast<unsigned long long>(hInst),
                         str_name.c_str(),
                         handle);
            m.set_return(handle);
            return;
        }
    }

    if (handle == 0) set_last_error(bitmap_miss_error(module_base));
    VITREO_TRACE( "[user32_ext] LoadBitmapW(hInst=0x%llx, name=0x%llx) -> 0x%x\n",
                 static_cast<unsigned long long>(hInst),
                 static_cast<unsigned long long>(name),
                 handle);
    m.set_return(handle);
}

void User32ExtManager::impl_LoadImageW(Marshaler& m) {
    u64 hInst = m.arg(0);
    GuestAddr name = m.arg_ptr(1);
    u32 type = static_cast<u32>(m.arg(2));
    u32 cx = static_cast<u32>(m.arg(3));
    u32 cy = static_cast<u32>(m.arg(4));
    u32 fuLoad = static_cast<u32>(m.arg(5));

    constexpr u32 kFileNotFound = 2;  // "LoadImageW(missing file)=NULL err=2"
    // A string name for a module resource, upper-cased as FindResource does.
    auto read_name = [&](GuestAddr p) {
        std::u16string text;
        for (u32 i = 0; i < 256; ++i) {
            const auto r = m.read_u16(p + i * 2);
            if (r.is_err() || r.value() == 0) break;
            u32 unit = r.value();
            if (unit >= 'a' && unit <= 'z') unit -= 32;
            text.push_back(static_cast<char16_t>(unit));
        }
        return text;
    };
    if (type == 2 && m_resource_mgr != nullptr) {  // IMAGE_CURSOR
        constexpr u32 LR_LOADFROMFILE = 0x0010;
        u32 cursor = 0;
        if (fuLoad & LR_LOADFROMFILE) {
            auto path = m.read_wide_string(name, 4096);
            auto bytes = read_all_bytes(m_vfs, path.value_or(""));
            if (bytes.is_err() || bytes.value().empty()) {
                set_last_error(kFileNotFound);
                m.set_return(0u);
                return;
            }
            const auto& data = bytes.value();
            const u32 want = cx != 0 ? cx : 32;
            if (data.size() >= 12 && std::memcmp(data.data(), "RIFF", 4) == 0) {
                auto ani = windowing::decode_ani_cursor(data.data(), static_cast<u32>(data.size()));
                if (!ani.frames.empty()) {
                    cursor = m_resource_mgr->create_animated_cursor_rgba(ani.hotspot_x, ani.hotspot_y, ani.width,
                                                                         ani.height, std::move(ani.frames),
                                                                         std::move(ani.delays_ms));
                }
            } else {
                auto cur = windowing::decode_cur_file(data.data(), static_cast<u32>(data.size()), want);
                if (!cur.rgba.empty()) {
                    cursor = m_resource_mgr->create_cursor_rgba(cur.hotspot_x, cur.hotspot_y, cur.width, cur.height,
                                                                std::move(cur.rgba));
                }
            }
            if (cursor == 0) set_last_error(error_codes::ERROR_INVALID_DATA);
            if (windowing::diag::input_diag()) {
                // Keep the file: callers commonly write a temporary .cur and
                // delete it right after this call.
                char copy_path[96];
                std::snprintf(copy_path, sizeof(copy_path), "/tmp/vitreo-cursor-%d-0x%x.cur",
                              static_cast<int>(::getpid()), cursor);
                if (std::FILE* f = std::fopen(copy_path, "wb")) {
                    std::fwrite(data.data(), 1, data.size(), f);
                    std::fclose(f);
                }
                std::fprintf(stderr, "[input] LoadImageW cursor file bytes=%zu cx=%u cy=%u fuLoad=0x%x -> 0x%x (%s)\n",
                             data.size(), cx, cy, fuLoad, cursor, copy_path);
            }
        } else if (name != 0 && name < 0x10000) {
            // OCR_* system cursors share the IDC_* numbers ("LoadImageW(IDC_ARROW,
            // shared) same=1" as LoadCursor).
            cursor = m_resource_mgr->load_cursor(static_cast<GuestAddr>(hInst), static_cast<u32>(name));
            if (cursor == 0) set_last_error(m_resource_mgr->resource_miss_error(static_cast<GuestAddr>(hInst), false));
        } else if (name != 0) {
            cursor = m_resource_mgr->load_cursor_named(static_cast<GuestAddr>(hInst), read_name(name));
            if (cursor == 0) set_last_error(m_resource_mgr->resource_miss_error(static_cast<GuestAddr>(hInst), false));
        } else {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        }
        m.set_return(cursor);
        return;
    }

    // IMAGE_BITMAP = 0, IMAGE_ICON = 1, IMAGE_CURSOR = 2
    if (type == 1) {
        constexpr u32 LR_LOADFROMFILE = 0x0010;
        constexpr u32 LR_SHARED = 0x8000;
        const bool shared = (fuLoad & LR_SHARED) != 0;

        if (name == 0 || m_resource_mgr == nullptr) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }

        if (hInst == 0 && (fuLoad & LR_LOADFROMFILE) == 0) {
            const u32 icon = name < 0x10000 && shared
                ? m_resource_mgr->load_icon(0, static_cast<u32>(name))
                : 0;
            if (icon == 0) {
                set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
            }
            m.set_return(icon);
            return;
        }

        std::vector<u8> ico_data;
        std::optional<ResourceManager::IconSourceKey> shared_source;
        std::string source_description;
        if ((fuLoad & LR_LOADFROMFILE) != 0) {
            if (hInst != 0 || name < 0x10000) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto path = m.read_wide_string(name, 4096);
            source_description = path.value_or("");
            if (!source_description.empty()) {
                auto bytes = read_all_bytes(m_vfs, source_description);
                if (bytes.is_ok()) {
                    ico_data = std::move(bytes.value());
                }
            }
            if (ico_data.empty()) {
                set_last_error(kFileNotFound);
                m.set_return(0u);
                return;
            }
        } else if (name < 0x10000 && hInst != 0) {
            const GuestAddr module_base = static_cast<GuestAddr>(hInst);
            const u16 resource_id = static_cast<u16>(name);
            source_description = "module resource " +
                std::to_string(resource_id);
            if (auto info =
                    try_get_resource_dir_from_module(m_memory, module_base)) {
                ico_data = windowing::extract_icon_by_group_id(
                    m_memory, info->image_base, info->rsrc_rva,
                    info->rsrc_size, resource_id);
            }
            if (shared) {
                shared_source = ResourceManager::IconSourceKey{
                    module_base, resource_id};
            }
        } else {
            const std::u16string group = read_name(name);
            source_description = "named module icon";
            if (auto info = try_get_resource_dir_from_module(m_memory, static_cast<GuestAddr>(hInst))) {
                ico_data = windowing::extract_icon_by_group_name(m_memory, info->image_base, info->rsrc_rva,
                                                                 info->rsrc_size, group.data(),
                                                                 static_cast<u16>(group.size()));
            }
        }

        const u32 icon = m_resource_mgr->register_icon_image(
            std::move(ico_data), shared_source,
            static_cast<i32>(cx), static_cast<i32>(cy));
        if (icon == 0) {
            set_last_error(m_resource_mgr->resource_miss_error(static_cast<GuestAddr>(hInst), true));
        }
        VITREO_TRACE(
            "[user32_ext] LoadImageW(%s type=ICON cx=%d cy=%d "
            "fu=0x%x) -> 0x%x\n",
            source_description.c_str(), static_cast<i32>(cx),
            static_cast<i32>(cy), fuLoad, icon);
        m.set_return(icon);
        return;
    }
    if (type != 0) {
        VITREO_TRACE(
                     "[user32_ext] LoadImageW: unsupported type=%u (hInst=0x%llx name=0x%llx cx=%u cy=%u fu=0x%x)\n",
                     type,
                     static_cast<unsigned long long>(hInst),
                     static_cast<unsigned long long>(name),
                     cx, cy, fuLoad);
        m.set_return(0);
        return;
    }

    // We currently only support MAKEINTRESOURCE-style loads.
    if (name == 0) {
        m.set_return(0);
        return;
    }

    constexpr u32 LR_LOADFROMFILE = 0x0010;

    bool is_resource_id = (name < 0x10000);
    if (!is_resource_id) {
        auto s = m.read_wide_string(name, 4096);
        std::string win_path = s.is_ok() ? s.value() : "";

        if ((fuLoad & LR_LOADFROMFILE) != 0 && !win_path.empty()) {
            u32 handle = 0;
            auto bytes = read_all_bytes(m_vfs, win_path);
            if (bytes.is_ok() && !bytes.value().empty()) {
                handle = m_host.register_bitmap_bmp(bytes.value());
                if (handle == 0) set_last_error(error_codes::ERROR_INVALID_DATA);
            } else {
                set_last_error(kFileNotFound);  // "LoadImageA(missing bmp)=NULL err=2"
                VITREO_TRACE(
                             "[user32_ext] LoadImageW: LR_LOADFROMFILE read failed for '%s'\n",
                             win_path.c_str());
            }

            VITREO_TRACE(
                         "[user32_ext] LoadImageW(file=\"%s\" type=BITMAP cx=%u cy=%u fu=0x%x) -> 0x%x\n",
                         win_path.c_str(), cx, cy, fuLoad, handle);
            m.set_return(handle);
            return;
        }

        // Named resource in module (non-file path)
        GuestAddr module_base = static_cast<GuestAddr>(hInst);
        if (module_base == 0) {
            module_base = m_rsrc_image_base;
        }
        if (!win_path.empty() && module_base != 0) {
            u32 handle = 0;
            std::u16string u16name;
            u16name.reserve(win_path.size());
            for (char c : win_path) {
                u16name.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
            }

            if (auto info = try_get_resource_dir_from_module(m_memory, module_base)) {
                auto res = windowing::find_resource_by_string_name(
                    m_memory, info->image_base, info->rsrc_rva, info->rsrc_size,
                    windowing::RT::BITMAP, u16name.data(), static_cast<u16>(u16name.size()));
                if (res.data && res.size > 0) {
                    auto bmp_file = dib_to_bmp_file(res.data, res.size);
                    if (!bmp_file.empty()) {
                        handle = m_host.register_bitmap_bmp(bmp_file);
                    }
                }
            }

            VITREO_TRACE(
                         "[user32_ext] LoadImageW(named_res=\"%s\" hInst=0x%llx type=BITMAP cx=%u cy=%u fu=0x%x) -> 0x%x\n",
                         win_path.c_str(),
                         static_cast<unsigned long long>(hInst),
                         cx, cy, fuLoad, handle);
            m.set_return(handle);
            return;
        }

        VITREO_TRACE(
                     "[user32_ext] LoadImageW: unsupported name pointer '%s' (hInst=0x%llx cx=%u cy=%u fu=0x%x)\n",
                     win_path.empty() ? "<err>" : win_path.c_str(),
                     static_cast<unsigned long long>(hInst),
                     cx, cy, fuLoad);
        m.set_return(0);
        return;
    }

    u16 res_id = static_cast<u16>(name & 0xFFFF);
    // A NULL hInstance names an OEM image (Learn: LoadImage), never the exe.
    if (hInst == 0) {
        u32 handle = 0;
        const auto lookup = oem_bitmap(m_host, res_id, handle);
        if (lookup == OemBitmapLookup::Unknown) set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
        VITREO_TRACE("[user32_ext] LoadImageW(hInst=0x0 oem=%u type=BITMAP) -> 0x%x\n",
                     static_cast<unsigned>(res_id), handle);
        m.set_return(handle);
        return;
    }
    GuestAddr module_base = static_cast<GuestAddr>(hInst);
    if (module_base == 0) {
        module_base = m_rsrc_image_base;
    }

    u32 handle = 0;
    if (auto info = try_get_resource_dir_from_module(m_memory, module_base)) {
        auto res = windowing::find_resource(
            m_memory, info->image_base, info->rsrc_rva, info->rsrc_size,
            windowing::RT::BITMAP, res_id);

        if (res.data && res.size > 0) {
            auto bmp_file = dib_to_bmp_file(res.data, res.size);
            if (!bmp_file.empty()) {
                handle = m_host.register_bitmap_bmp(bmp_file);
            }
        }
    }

    VITREO_TRACE(
                 "[user32_ext] LoadImageW(hInst=0x%llx res_id=%u type=BITMAP cx=%u cy=%u fu=0x%x) -> 0x%x\n",
                 static_cast<unsigned long long>(hInst),
                 static_cast<unsigned>(res_id),
                 cx, cy, fuLoad, handle);
    m.set_return(handle);
}

void User32ExtManager::impl_SetCursor(Marshaler& m) {
    // HCURSOR SetCursor(HCURSOR hCursor)
    //
    // A NULL cursor removes it from the screen — that is the documented way to
    // hide it, including from WM_SETCURSOR when the window captures the mouse
    // or draws its own pointer. Ignoring the argument leaves the macOS cursor
    // floating over the guest's own.
    const auto cursor = static_cast<u32>(m.arg(0));
    const u32 previous = m_current_cursor;
    apply_cursor(cursor);
    const bool input_diag = windowing::diag::input_diag();
    if (input_diag) {
        std::fprintf(stderr, "[input] SetCursor cursor=0x%x previous=0x%x\n",
                     cursor, previous);
        // Which guest code chose this cursor: the first few NULL and non-NULL
        // settings list the code-looking values on the guest stack so the
        // deciding frame can be symbolized offline against module exports.
        static int dumped_null = 0;
        static int dumped_shape = 0;
        int& dumped = cursor == 0 ? dumped_null : dumped_shape;
        if (dumped < 3 && m.has_cpu()) {
            ++dumped;
            const bool is64 = m.cpu_is_64bit();
            const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
            const u32 slot = is64 ? 8u : 4u;
            std::fprintf(stderr, "[input]   SetCursor(0x%x) stack sp=0x%llx:",
                         cursor, static_cast<unsigned long long>(sp));
            for (u32 i = 0; i < 256; ++i) {
                const GuestAddr at = sp + static_cast<GuestAddr>(i) * slot;
                const u64 v = is64 ? m.read_u64(at).value_or(0)
                                   : m.read_u32(at).value_or(0);
                const bool code_like =
                    is64 ? (v >= 0x7ff000000000ull && v < 0x800000000000ull)
                         : (v >= 0x00400000ull && v < 0x80000000ull);
                if (code_like) {
                    std::fprintf(stderr, " 0x%llx",
                                 static_cast<unsigned long long>(v));
                }
            }
            std::fprintf(stderr, "\n");
        }
    }
    m.set_return(previous);
}

namespace {
/// Push one decoded cursor to the host, animated when it has frames.
[[nodiscard]] bool install_cursor_image(windowing::User32Host& host,
                                        const ResourceManager::CursorImage& image) {
    if (windowing::diag::input_diag()) {
        usize opaque = 0;
        for (usize i = 3; i < image.rgba.size(); i += 4) opaque += image.rgba[i] != 0;
        std::fprintf(stderr,
                     "[input] install cursor %ux%u hot=%u,%u frames=%zu "
                     "opaque_pixels=%zu/%zu\n",
                     image.width, image.height, image.hotspot_x, image.hotspot_y,
                     image.frames.size(), opaque, image.rgba.size() / 4u);
    }
    // An animated cursor is one shape to the guest -- SetCursor names it once
    // and the frames advance on their own -- so the fallback when the host
    // cannot animate is frame 0, not failure.
    if (image.frames.size() > 1 &&
        host.set_animated_cursor_rgba(image.width, image.height,
                                      image.hotspot_x, image.hotspot_y,
                                      image.frames, image.frame_delays_ms)) {
        return true;
    }
    return host.set_custom_cursor_rgba(image.width, image.height,
                                       image.hotspot_x, image.hotspot_y,
                                       image.rgba.data(), image.rgba.size());
}
} // namespace

void User32ExtManager::apply_cursor(u32 hcursor) {
    const u32 previous = m_current_cursor;
    m_current_cursor = hcursor;

    auto reinstall_last_custom = [this]() -> bool {
        if (m_last_custom_cursor == 0 || m_resource_mgr == nullptr) {
            return false;
        }
        // Already the installed shape: nothing to push, and pushing anyway
        // costs a main-thread round trip per call.
        if (m_installed_custom_cursor == m_last_custom_cursor) {
            return true;
        }
        auto image = m_resource_mgr->cursor_image(m_last_custom_cursor);
        if (!image || image->is_system || image->rgba.empty()) {
            return false;
        }
        const bool ok = install_cursor_image(m_host, *image);
        if (ok) m_installed_custom_cursor = m_last_custom_cursor;
        return ok;
    };

    // Learn SetCursor: return immediately when the handle is unchanged.
    if (hcursor != previous && hcursor != 0 && m_resource_mgr != nullptr) {
        if (auto image = m_resource_mgr->cursor_image(hcursor)) {
            if (image->is_system) {
                // A stock arrow from LoadCursor / DefWindowProc / WM_SETCURSOR
                // must not replace a custom cursor installed for a software
                // pointer. That replacement happens when a loading screen
                // appears and exclusive fullscreen starts.
                if ((image->system_id == idc::IDC_ARROW || hcursor == 1) &&
                    reinstall_last_custom()) {
                    // keep game shape
                } else {
                    m_host.set_system_cursor(image->system_id);
                    m_installed_custom_cursor = 0;
                }
                if (windowing::diag::input_diag()) {
                    std::fprintf(stderr,
                                 "[input] apply_cursor 0x%x system id=%u\n",
                                 hcursor, image->system_id);
                }
            } else if (!image->rgba.empty()) {
                const bool ok = install_cursor_image(m_host, *image);
                if (!ok) {
                    std::fprintf(stderr,
                                 "[input] SetCursor 0x%x custom %ux%u "
                                 "host install failed\n",
                                 hcursor, image->width, image->height);
                } else {
                    m_last_custom_cursor = hcursor;
                    m_installed_custom_cursor = hcursor;
                }
            }
        } else {
            std::fprintf(stderr,
                         "[input] SetCursor 0x%x has no cursor image\n",
                         hcursor);
        }
    }
    // Learn ShowCursor: the cursor is displayed only while the display
    // count is >= 0. SetCursor(NULL) after a custom image is the software
    // cursor path: keep the last custom shape.
    if (hcursor == 0 && m_last_custom_cursor != 0 &&
        m_cursor_display_count >= 0) {
        (void)reinstall_last_custom();
        set_native_cursor_visible(true);
        return;
    }
    set_native_cursor_visible(hcursor != 0 && m_cursor_display_count >= 0);
}

void User32ExtManager::set_native_cursor_visible(bool visible) {
    if (windowing::diag::input_diag()) {
        std::fprintf(stderr,
                     "[input] native cursor visible=%u current=0x%x "
                     "count=%d last_custom=0x%x installed_custom=0x%x\n",
                     visible ? 1u : 0u, m_current_cursor,
                     m_cursor_display_count, m_last_custom_cursor,
                     m_installed_custom_cursor);
    }
    m_host.set_cursor_visible(visible);
}


void User32ExtManager::impl_ReleaseDC(Marshaler& m) {
    auto hwnd = static_cast<u64>(m.arg(0));
    auto hdc  = static_cast<u64>(m.arg(1));
    // 1 when a window or screen DC was released, 0 for a DC released
    // already or never handed out ("ReleaseDC=1 again=0").
    const i32 released =
        m_gdi_mgr ? m_gdi_mgr->release_dc(static_cast<Handle>(hwnd), static_cast<Handle>(hdc)) : 0;
    m.set_return(static_cast<u32>(released));
}

// SystemParametersInfoW: desktop/system_parameters.cpp.

void User32ExtManager::impl_MessageBoxIndirectW(Marshaler& m) {
    // int MessageBoxIndirectW(const MSGBOXPARAMSW*). The same box as
    // MessageBoxW with the owner, text, caption and style taken from the
    // structure (s2msg_msgbox_probe indirect_*: identical validation and
    // results; cbSize is not checked). MSGBOXPARAMSW:
    //   x86: cbSize@0 hwndOwner@4 hInstance@8 lpszText@12 lpszCaption@16
    //        dwStyle@20
    //   x64: cbSize@0 hwndOwner@8 hInstance@16 lpszText@24 lpszCaption@32
    //        dwStyle@40
    // lpszText / lpszCaption may be string-resource identifiers in
    // hInstance (Learn MSGBOXPARAMSW). MB_USERICON's lpszIcon only picks the
    // picture, and lpfnMsgBoxCallback only runs for MB_HELP's Help button,
    // which the host alert does not offer.
    const GuestAddr params = m.arg_ptr(0);
    const bool is64 = m.cpu_is_64bit();
    const auto ptr_at = [&](u32 x86_off, u32 x64_off) -> u64 {
        return is64 ? m.read_u64(params + x64_off).value_or(0)
                    : m.read_u32(params + x86_off).value_or(0);
    };
    if (params == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    const auto owner = static_cast<windowing::HWND>(ptr_at(4, 8));
    const u64 instance = ptr_at(8, 16);
    const u64 text_ptr = ptr_at(12, 24);
    const u64 caption_ptr = ptr_at(16, 32);
    const u32 style = m.read_u32(params + (is64 ? 40 : 20)).value_or(0);

    const auto read_text = [&](u64 value, const std::string& fallback) -> std::string {
        if (value == 0) return fallback;
        if (value <= 0xFFFF) {
            // A string resource of hInstance. Staged below the guest stack
            // pointer and copied out before anything can run guest code.
            if (m_resource_mgr == nullptr || !m.has_cpu()) return fallback;
            const GuestAddr scratch =
                (static_cast<GuestAddr>(m.cpu()->gpr.rsp) - 0x1000u) & ~GuestAddr{0xF};
            const i32 len = m_resource_mgr->load_string(
                static_cast<GuestAddr>(instance), static_cast<u32>(value), scratch, 1024);
            if (len <= 0) return fallback;
            auto res = m.read_wide_string(scratch);
            return res.is_ok() ? res.value() : fallback;
        }
        auto res = m.read_wide_string(static_cast<GuestAddr>(value));
        return res.is_ok() ? res.value() : fallback;
    };
    // Learn MessageBox: NULL lpText is empty, NULL lpCaption is "Error".
    const std::string text = read_text(text_ptr, "");
    const std::string caption = read_text(caption_ptr, "Error");
    std::fprintf(stderr, "[msgbox] MessageBoxIndirectW: [%s] '%s' style=0x%x owner=0x%x\n",
                 caption.c_str(), text.c_str(), style, static_cast<unsigned>(owner));
    if (m_resource_mgr == nullptr) {
        // No box can be shown; answering a button nobody pressed would
        // confirm whatever the application asked (an uninstaller's "Yes").
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!MessageBoxIndirectW: "
                             "no message box host is attached\n");
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0u);
        return;
    }
    const i32 result = m_resource_mgr->message_box(owner, caption, text, style);
    m.set_return(static_cast<u64>(static_cast<u32>(result)));
}

} // namespace vitreo::winapi
