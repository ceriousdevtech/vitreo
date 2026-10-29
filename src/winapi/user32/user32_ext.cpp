// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/user32/classes/system_classes.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/user32/windows/kbd_layout.hpp"
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
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string_view>
#include <cstdlib>

#include <array>
#include <atomic>
#include <cctype>
#include <optional>
#include <vector>

#include <utility>

namespace vitreo::winapi {

bool diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

// Dialog children keep WindowData::wndproc == 0 to mean "host/Cocoa control
// proc" (not a guest function). GetWindowLong(GWL_WNDPROC) for a system class
// (ListBox, Edit, …) returns the corresponding Def*Proc. CallWindowProc of
// that value must still dispatch the control message; returning 0 makes
// CallWindowProc a no-op.
// kNativeControlWndProc lives in user32_internal.hpp.

[[nodiscard]] bool is_native_host_control(
    const windowing::WindowData& win) noexcept {
    if (win.wndproc != 0) {
        return false;
    }
    // Native class identity is independent of the caller's ASCII spelling.
    // Windows native_control_case_probe{32,64} also checks that Unicode
    // lookalikes (dotless i / long s) do not alias these ASCII class names.
    static constexpr std::wstring_view classes[] = {
        L"Button", L"Edit", L"Static", L"ListBox", L"ComboBox", L"ScrollBar",
        L"SysListView32", L"SysTreeView32", L"SysTabControl32", L"msctls_progress32",
        L"msctls_trackbar32", L"RichEdit20A", L"RichEdit20W", L"RichEdit50W", L"SysLink"
    };
    const auto fold = [](wchar_t ch) {
        return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
    };
    const std::wstring_view name(win.class_name);
    for (const auto candidate : classes) {
        if (name.size() != candidate.size()) continue;
        bool same = true;
        for (usize i = 0; i < name.size(); ++i) {
            if (fold(name[i]) != fold(candidate[i])) { same = false; break; }
        }
        if (same) return true;
    }
    return false;
}

namespace {
/// Set once while user32's thunks are registered; read from guest threads.
std::atomic<u64> g_native_control_wndproc{0};
} // namespace

void set_native_control_wndproc_address(u64 address) noexcept {
    g_native_control_wndproc.store(address, std::memory_order_release);
}

[[nodiscard]] u64 native_control_wndproc_address() noexcept {
    const u64 addr = g_native_control_wndproc.load(std::memory_order_acquire);
    return addr != 0 ? addr : kNativeControlWndProc;
}

[[nodiscard]] u64 effective_wndproc(
    const windowing::WindowData& win) noexcept {
    if (win.wndproc != 0) {
        return win.wndproc;
    }
    return is_native_host_control(win) ?
        (find_system_class(win.class_name) ? system_class_wndproc(win.class_name, !win.unicode) : native_control_wndproc_address()) : 0;
}

[[nodiscard]] bool is_native_control_wndproc(u64 proc) noexcept {
    // The bare constant stays recognised: a guest may still be holding one
    // handed out before the thunk address was known.
    return find_system_class_by_wndproc(proc) || proc == 0 || proc == kNativeControlWndProc ||
           proc == g_native_control_wndproc.load(std::memory_order_acquire);
}



void User32ExtManager::set_native_control_wndproc(u64 address) noexcept {
    set_native_control_wndproc_address(address);
}

void User32ExtManager::set_dialog_manager(DialogManager* mgr) {
    m_dialog_mgr = mgr;
    if (m_dialog_mgr != nullptr) {
        m_dialog_mgr->set_creation_hook(
            [this](windowing::HWND hwnd, u32 style) {
                return dispatch_cbt_create_hook(hwnd, style);
            });
    }
}


void User32ExtManager::impl_GetCaretBlinkTime(Marshaler& m) {
    const u32 milliseconds =
        m_caret_blink_time_ms.load(std::memory_order_relaxed);
    VITREO_TRACE("[user32_ext] GetCaretBlinkTime -> %u ms\n", milliseconds);
    m.set_return(milliseconds);
}

void User32ExtManager::impl_SetCaretBlinkTime(Marshaler& m) {
    const u32 milliseconds = static_cast<u32>(m.arg(0));
    m_caret_blink_time_ms.store(milliseconds, std::memory_order_relaxed);
    VITREO_TRACE("[user32_ext] SetCaretBlinkTime(%u) -> TRUE\n",
                 milliseconds);
    m.set_return(1);
}

namespace {




static inline u16 read_u16_le(const u8* p) {
    return static_cast<u16>(p[0] | (static_cast<u16>(p[1]) << 8));
}

static inline u32 read_u32_le(const u8* p) {
    return static_cast<u32>(p[0] |
                            (static_cast<u32>(p[1]) << 8) |
                            (static_cast<u32>(p[2]) << 16) |
                            (static_cast<u32>(p[3]) << 24));
}


/// Screen position of the window's outer top-left (GetWindowRect origin).
static windowing::Point origin_to_desktop(windowing::HwndManager& mgr, windowing::HWND hwnd) {
    windowing::Point out{0, 0};
    windowing::HWND cur = hwnd;
    bool is_target = true;

    while (cur != windowing::HWND_DESKTOP && cur != windowing::NULL_HWND) {
        const auto* w = mgr.find(cur);
        if (!w) {
            break;
        }
        out.x += w->rect.left;
        out.y += w->rect.top;
        // A child rect is relative to its parent's client area, not the
        // parent's outer frame. Every ancestor above the target therefore
        // contributes its WM_NCCALCSIZE-derived client origin.
        if (!is_target) {
            out.x += w->client_offset.x;
            out.y += w->client_offset.y;
        }
        is_target = false;
        cur = w->parent;
    }
    return out;
}


} // namespace

std::string make_prop_key(Marshaler& m, GuestAddr str_or_atom, bool ansi) {
    if (str_or_atom == 0) {
        return {};
    }
    if (str_or_atom < 0x10000) {
        return "A:" + std::to_string(static_cast<u32>(str_or_atom));
    }
    auto s = ansi ? m.read_ansi_string(str_or_atom, 1024)
                  : m.read_wide_string(str_or_atom, 1024);
    if (s.is_err()) {
        return {};
    }
    // Property names are atoms, so string names compare case-insensitively
    // (measured, s1win_api_probe getpropw_case).
    std::wstring upper = utf8_to_wstring(s.value());
    for (auto& ch : upper) ch = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(ch)));
    std::string key = "S:" + wstring_to_utf8(upper);
    if (std::getenv("VITREO_WIN_DIAG")) {
        std::fprintf(stderr, "[user32-prop] encoding=%c key=\"%s\"\n",
                     ansi ? 'A' : 'W', key.c_str());
    }
    return key;
}

[[nodiscard]] u64 user32_env_u64_or(const char* name, u64 fallback) {
    const char* v = std::getenv(name);
    if (!v || !*v) {
        return fallback;
    }
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) {
        return fallback;
    }
    return static_cast<u64>(parsed);
}


// Reachable from user32_ext_registration.cpp, which is why these seven sit
// outside the anonymous namespace above rather than inside it with the rest of
// this file's helpers. Declared in user32_internal.hpp — private to user32, not
// published. They keep their definitions here because each one still calls the
// file-local helpers immediately above (read_u16_le, origin_to_desktop, …); the
// header shares the declaration, not the implementation.

/// Expand a client RECT in guest memory to the window rect that would contain
/// it, shared by AdjustWindowRect and AdjustWindowRectEx.
///
/// The frame metrics match GetSystemMetrics and compute_nc_insets, so
/// CreateWindowEx of the adjusted outer size yields the client area the
/// caller passed. AdjustWindowRect and AdjustWindowRectEx both expand the
/// RECT by the non-client insets; leaving the RECT untouched makes the
/// frame consume part of the requested client.
///
/// @return false if the rect could not be read/written.
bool adjust_window_rect(GuestMemory& memory, GuestAddr rp, u32 style,
                               bool has_menu, u32 ex_style) {
    if (rp == 0) return false;

    i32 left   = static_cast<i32>(memory.read_u32(rp).value_or(0));
    i32 top    = static_cast<i32>(memory.read_u32(rp + 4).value_or(0));
    i32 right  = static_cast<i32>(memory.read_u32(rp + 8).value_or(0));
    i32 bottom = static_cast<i32>(memory.read_u32(rp + 12).value_or(0));

    const auto in = nc_metrics::insets(style, ex_style, has_menu, false);
    left -= in.left;
    top -= in.top;
    right += in.right;
    bottom += in.bottom;

    return memory.write_u32(rp,      static_cast<u32>(left)).is_ok() &&
           memory.write_u32(rp + 4,  static_cast<u32>(top)).is_ok() &&
           memory.write_u32(rp + 8,  static_cast<u32>(right)).is_ok() &&
           memory.write_u32(rp + 12, static_cast<u32>(bottom)).is_ok();
}

/// Shared translation behind MapVirtualKey{A,W,ExA,ExW}: the US layout
/// Vitreo reports (HKL 0x04090409), as measured on Windows (kbd_layout.hpp).
/// All four entry points return the same mapping. 0 for an invalid map type
/// (the thunks also set ERROR_INVALID_PARAMETER).
u32 map_virtual_key(u32 uCode, u32 uMapType) noexcept {
    u32 result = 0;
    (void)kbd_us::map_virtual_key(uCode, uMapType, result);
    return result;
}

std::wstring utf8_to_wstring(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            result.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x1F) << 6) |
                         (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            result.push_back(wc);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x0F) << 12) |
                         ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(s[i + 2]) & 0x3F));
            result.push_back(wc);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = (static_cast<uint32_t>(c & 0x07) << 18) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          static_cast<uint32_t>(static_cast<unsigned char>(s[i + 3]) & 0x3F);
            result.push_back(static_cast<wchar_t>(cp));
            i += 4;
        } else {
            result.push_back(L'?');
            ++i;
        }
    }
    return result;
}

[[nodiscard]] Result<std::vector<u8>, capsule::VfsError> read_all_bytes(
    capsule::Vfs& vfs,
    std::string_view win_path
) {
    auto h = vfs.open(win_path,
                      capsule::AccessMode::Read,
                      capsule::ShareMode::Read,
                      capsule::CreationDisposition::OpenExisting);
    if (h.is_err()) {
        return Result<std::vector<u8>, capsule::VfsError>::err(h.error());
    }

    std::vector<u8> data;
    if (auto st = vfs.stat(win_path); st.is_ok()) {
        data.reserve(static_cast<usize>(st.value().size));
    }

    std::array<u8, 64 * 1024> buf{};
    for (;;) {
        auto chunk = vfs.read(h.value(), std::span<u8>(buf.data(), buf.size()));
        if (chunk.is_err()) {
            (void)vfs.close(h.value());
            return Result<std::vector<u8>, capsule::VfsError>::err(chunk.error());
        }
        if (chunk.value() == 0) {
            break;
        }
        data.insert(data.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(chunk.value()));
    }

    (void)vfs.close(h.value());
    return Result<std::vector<u8>, capsule::VfsError>::ok(std::move(data));
}

std::optional<ResourceDirInfo> try_get_resource_dir_from_module(
    const GuestMemory& memory,
    GuestAddr module_base) {

    if (module_base == 0) {
        return std::nullopt;
    }

    vitreo::pe::DosHeader dos{};
    if (!memory.read_bytes(module_base, reinterpret_cast<Byte*>(&dos), sizeof(dos)).is_ok()) {
        return std::nullopt;
    }
    if (dos.e_magic != vitreo::pe::DOS_MAGIC) {
        return std::nullopt;
    }

    const GuestAddr nt_base = module_base + static_cast<GuestAddr>(dos.e_lfanew);
    auto sig = memory.read_u32(nt_base);
    if (!sig.is_ok() || sig.value() != vitreo::pe::PE_SIGNATURE) {
        return std::nullopt;
    }

    vitreo::pe::CoffHeader coff{};
    const GuestAddr coff_addr = nt_base + 4;
    if (!memory.read_bytes(coff_addr, reinterpret_cast<Byte*>(&coff), sizeof(coff)).is_ok()) {
        return std::nullopt;
    }

    const GuestAddr opt_addr = coff_addr + sizeof(vitreo::pe::CoffHeader);
    auto magic_rd = memory.read_u16(opt_addr);
    if (!magic_rd.is_ok()) {
        return std::nullopt;
    }
    const u16 magic = magic_rd.value();

    vitreo::pe::DataDirectory rsrc{};

    if (magic == vitreo::pe::PE32_MAGIC) {
        vitreo::pe::OptionalHeader32 opt{};
        if (!memory.read_bytes(opt_addr, reinterpret_cast<Byte*>(&opt), sizeof(opt)).is_ok()) {
            return std::nullopt;
        }
        if (opt.NumberOfRvaAndSizes <= vitreo::pe::DIR_RESOURCE) {
            return std::nullopt;
        }
        rsrc = opt.DataDirectory[vitreo::pe::DIR_RESOURCE];
    } else if (magic == vitreo::pe::PE32PLUS_MAGIC) {
        vitreo::pe::OptionalHeader64 opt{};
        if (!memory.read_bytes(opt_addr, reinterpret_cast<Byte*>(&opt), sizeof(opt)).is_ok()) {
            return std::nullopt;
        }
        if (opt.NumberOfRvaAndSizes <= vitreo::pe::DIR_RESOURCE) {
            return std::nullopt;
        }
        rsrc = opt.DataDirectory[vitreo::pe::DIR_RESOURCE];
    } else {
        return std::nullopt;
    }

    if (rsrc.VirtualAddress == 0 || rsrc.Size == 0) {
        return std::nullopt;
    }

    return ResourceDirInfo{module_base, rsrc.VirtualAddress, rsrc.Size};
}

std::vector<u8> dib_to_bmp_file(const u8* dib, u32 dib_size) {
    // RT_BITMAP resources store a DIB: header + palette + pixels.
    // NSImage expects a complete BMP file, so synthesize a BITMAPFILEHEADER.
    // Supports both BITMAPCOREHEADER (12 bytes, OS/2) and
    // BITMAPINFOHEADER (40+ bytes, Win3.x).
    if (!dib || dib_size < 12) {
        return {};
    }

    u32 header_size = read_u32_le(dib + 0);
    if (header_size < 12 || header_size > dib_size) {
        return {};
    }

    u16 bit_count = 0;
    u32 palette_size = 0;

    if (header_size == 12) {
        // BITMAPCOREHEADER: bcSize(4) + bcWidth(2) + bcHeight(2) + bcPlanes(2) + bcBitCount(2) = 12
        bit_count = read_u16_le(dib + 10);
        u32 colors = 0;
        if (bit_count > 0 && bit_count <= 8) {
            colors = 1u << bit_count;
        }
        // BITMAPCOREHEADER uses 3-byte RGBTRIPLE palette entries (not 4-byte RGBQUAD)
        palette_size = colors * 3u;
    } else {
        // BITMAPINFOHEADER (40) or larger (V4=108, V5=124, etc.)
        if (header_size < 40) {
            return {};
        }
        // Offset 14: biBitCount
        if (header_size >= 16) {
            bit_count = read_u16_le(dib + 14);
        }
        // Offset 32: biClrUsed
        u32 clr_used = 0;
        if (header_size >= 36) {
            clr_used = read_u32_le(dib + 32);
        }
        u32 colors = 0;
        if (clr_used != 0) {
            colors = clr_used;
        } else if (bit_count > 0 && bit_count <= 8) {
            colors = 1u << bit_count;
        }
        palette_size = colors * 4u;
    }

    u32 off_bits = 14u + header_size + palette_size;
    u32 file_size = 14u + dib_size;
    if (off_bits > file_size) {
        off_bits = 14u + header_size;
    }

    std::vector<u8> out;
    out.resize(file_size);

    // BITMAPFILEHEADER
    out[0] = 'B';
    out[1] = 'M';
    // bfSize
    out[2] = static_cast<u8>(file_size & 0xFF);
    out[3] = static_cast<u8>((file_size >> 8) & 0xFF);
    out[4] = static_cast<u8>((file_size >> 16) & 0xFF);
    out[5] = static_cast<u8>((file_size >> 24) & 0xFF);
    // bfReserved1/bfReserved2
    out[6] = out[7] = out[8] = out[9] = 0;
    // bfOffBits
    out[10] = static_cast<u8>(off_bits & 0xFF);
    out[11] = static_cast<u8>((off_bits >> 8) & 0xFF);
    out[12] = static_cast<u8>((off_bits >> 16) & 0xFF);
    out[13] = static_cast<u8>((off_bits >> 24) & 0xFF);

    std::memcpy(out.data() + 14, dib, dib_size);
    return out;
}

/// Screen position of the client-area top-left (ClientToScreen / MapWindowPoints).
/// Matches compute_nc_insets / GetSystemMetrics frame metrics.
windowing::Point client_origin_to_desktop(windowing::HwndManager& mgr,
                                                 windowing::HWND hwnd) {
    windowing::Point out = origin_to_desktop(mgr, hwnd);
    if (hwnd == windowing::HWND_DESKTOP || hwnd == windowing::NULL_HWND) {
        return out;
    }
    const auto* w = mgr.find(hwnd);
    if (!w) {
        return out;
    }
    out.x += w->client_offset.x;
    out.y += w->client_offset.y;
    return out;
}


User32ExtManager::User32ExtManager(GuestMemory& memory,
                                   capsule::Vfs& vfs,
                                   windowing::HwndManager& hwnd_mgr,
                                   WindowClassManager& class_mgr,
                                   windowing::User32Host& host,
                                   windowing::MessageQueue& queue,
                                   std::function<std::string()>
                                       display_adapter_name_provider)
    : m_memory(memory)
    , m_vfs(vfs)
    , m_hwnd_mgr(hwnd_mgr)
    , m_class_mgr(class_mgr)
    , m_host(host)
    , m_queue(queue)
    , m_display_adapter_name_provider(
          display_adapter_name_provider
              ? std::move(display_adapter_name_provider)
              : std::function<std::string()>{[] {
                    const auto* backend = graphics::active_backend_device();
                    return (backend != nullptr && backend->available())
                        ? backend->backend_device_name()
                        : std::string{};
                }})
{
    // CreateProcess(AsUser) STARTUPINFO.lpDesktop is forwarded as this env var
    // (see kernel32 CreateProcessCommon). Attach before any USER calls.
    if (const char* startup_desktop = std::getenv("VITREO_STARTUP_DESKTOP");
        startup_desktop != nullptr && startup_desktop[0] != '\0') {
        apply_startup_desktop(startup_desktop);
    }

    // Keep GetDesktopWindow/GetWindowRect consistent with the actual primary
    // display.  The desktop HWND is a permanent HwndManager entry and is the
    // root for all top-level windows.
    if (windowing::WindowData* desktop =
            m_hwnd_mgr.find(windowing::HWND_DESKTOP)) {
        i32 screen_w = 1920;
        i32 screen_h = 1080;
        m_host.get_screen_size(screen_w, screen_h);
        desktop->rect = {0, 0, screen_w, screen_h};
        desktop->client_rect = desktop->rect;
        VITREO_TRACE("[user32_ext] desktop hwnd=0x%x size=%dx%d\n",
                     windowing::HWND_DESKTOP, screen_w, screen_h);
    }

    // Register native resize callback so that when the user drags the window
    // border, the HwndManager's WindowData stays in sync with the actual size.
    m_host.set_native_resize_callback(
        [this](windowing::HWND hwnd, i32 client_w, i32 client_h) {
            windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
            if (!win) return false;
            if (hwnd == m_virtual_fullscreen_hwnd &&
                m_virtual_display_width > 0 &&
                m_virtual_display_height > 0) {
                // The host window covers the physical macOS display, while a
                // legacy exclusive-mode guest observes the logical resolution
                // selected through ChangeDisplaySettings.
                client_w = m_virtual_display_width;
                client_h = m_virtual_display_height;
            }
            if (m_msgsend_mgr && win->wndproc != 0 && m_windowpos_scratch != 0) {
                queue_native_action(hwnd, win->owner_thread_id,
                                    NativeActionKind::Resize, client_w, client_h);
                return true;
            }
            // Update stored client rect
            win->client_rect = {0, 0, client_w, client_h};
            // Update outer rect: keep origin, compute new size from client + non-client area.
            // Must match compute_nc_insets / AdjustWindowRect (full WS_CAPTION bits;
            // WS_EX_TOOLWINDOW → SM_CYSMCAPTION).
            i32 border_w = 0, border_h = 0;
            if (win->client_drawn_frame) {
                // The guest consumed all non-client insets.
            } else if (win->style & windowing::WS::THICKFRAME) {
                border_w = 8; border_h = 8;
            } else if (win->style & windowing::WS::DLGFRAME) {
                border_w = 4; border_h = 4;
            } else if (win->style & windowing::WS::BORDER) {
                border_w = 1; border_h = 1;
            }
            constexpr u32 WS_CAPTION_MASK = windowing::WS::CAPTION;
            constexpr u32 WS_EX_TOOLWINDOW = 0x00000080u;
            i32 caption_h = 0;
            if (!win->client_drawn_frame && (win->style & WS_CAPTION_MASK) == WS_CAPTION_MASK) {
                caption_h = (win->ex_style & WS_EX_TOOLWINDOW) ? 22 : 30;
            }
            i32 menu_h = !win->client_drawn_frame && win->has_menu ? 20 : 0;
            win->rect.right  = win->rect.left + client_w + 2 * border_w;
            win->rect.bottom = win->rect.top  + client_h + 2 * border_h + caption_h + menu_h;
            if (!win->minimized && !win->maximized) {
                win->normal_rect = win->rect;
                win->normal_client_rect = win->client_rect;
            }
            // A native resize changes the Windows client area. Record that
            // damage as an update region; do not queue WM_PAINT directly.
            // GetMessage/PeekMessage generate WM_PAINT only after higher-
            // priority posted messages (notably WM_SIZE) have drained.
            if (m_paint_mgr) {
                m_paint_mgr->invalidate_rect(
                    static_cast<u64>(hwnd), nullptr, false);
            }

            // Republish the new client size to the cross-process HWND
            // registry. A guest-driven resize reaches the registry through
            // apply_set_window_pos (geometry.cpp); a NATIVE resize -- the
            // user dragging the macOS window border -- never called
            // SetWindowPos, so without this the registry keeps the size the
            // window had when it was created.
            //
            // That stale entry is what peer processes size their presentation
            // view from: run.cpp passes the REGISTERED client size (never the
            // DXGI surface size, deliberately, so a 1x1 popup cannot expand to
            // cover siblings) to grow_hosted_peer_child_to_present_size.
            //
            // MEASURED 2026-09-09 on Cerious AASM: dragging the corner grew
            // the NSWindow and the guest correctly built new swapchains at the
            // new size, but 887 consecutive presents all reported
            // view_bounds=1008x722 and the grow path logged zero times,
            // because it compared against a registry frozen at the creation
            // size. The window grew and the content stayed painted in the
            // top-left at its original size.
            // Identity travels with the geometry; see the note in
            // geometry.cpp. A record first published from a resize would
            // otherwise never carry a class name for FindWindow to match.
            ipc::hwnd_registry_publish(
                static_cast<u32>(hwnd),
                win->client_rect.right - win->client_rect.left,
                win->client_rect.bottom - win->client_rect.top,
                win->owner_thread_id,
                wstring_to_utf8(win->class_name),
                wstring_to_utf8(win->title));

            VITREO_TRACE( "[user32_ext] native resize callback hwnd=0x%x client=%dx%d\n",
                         hwnd, client_w, client_h);
            return false;
        });

    // Learn (Moving and Sizing Windows / WM_WINDOWPOSCHANGED): a window the
    // user moves gets WM_WINDOWPOSCHANGING / WM_WINDOWPOSCHANGED / WM_MOVE and
    // its new position from GetWindowRect, and raises LOCATIONCHANGE. AppKit
    // runs the drag itself (begin_native_window_drag), so the guest learns
    // the new origin here. Without it every screen-relative calculation the
    // guest makes (popup placement from GetWindowRect / ClientToScreen) used
    // the position the window had before the drag.
    m_host.set_native_move_callback(
        [this](windowing::HWND hwnd, i32 client_x, i32 client_y) {
            windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
            if (!win) return;
            const i32 x = client_x - win->client_offset.x;
            const i32 y = client_y - win->client_offset.y;
            queue_native_action(hwnd, win->owner_thread_id,
                                NativeActionKind::Move, x, y);
        });

    m_host.set_native_move_size_callback(
        [this](windowing::HWND hwnd, bool entering) {
            const auto* win = m_hwnd_mgr.find(hwnd);
            if (!win) return;
            queue_native_action(hwnd, win->owner_thread_id,
                entering ? NativeActionKind::EnterMoveSize : NativeActionKind::ExitMoveSize);
        });

    m_host.set_native_show_state_callback(
        [this](windowing::HWND hwnd, bool minimized, bool maximized) {
            windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
            if (!win) return;

            // A Dock restore or a native minimize (the title-bar button,
            // Cmd-M) is the user acting on the window through the shell, as
            // a taskbar click is on Windows: the window gets WM_SYSCOMMAND
            // SC_RESTORE / SC_MINIMIZE and DefWindowProc carries it out
            // through ShowWindow, which restores the parked rectangle and
            // sends the WM_WINDOWPOSCHANGED / WM_SIZE a restore produces.
            // The guest's own ShowWindow updates its state before asking
            // the host, so only a shell action arrives with the states apart.
            constexpr i32 SC_MINIMIZE = 0xF020;
            constexpr i32 SC_RESTORE = 0xF120;
            if (!minimized && win->minimized) {
                queue_native_action(hwnd, win->owner_thread_id,
                                    NativeActionKind::SysCommand, SC_RESTORE);
                return;
            }
            if (minimized && !win->minimized) {
                queue_native_action(hwnd, win->owner_thread_id,
                                    NativeActionKind::SysCommand, SC_MINIMIZE);
                return;
            }

            if ((minimized || maximized) &&
                !win->minimized && !win->maximized) {
                win->normal_rect = win->rect;
                win->normal_client_rect = win->client_rect;
            }
            if (minimized && win->maximized) {
                win->restore_to_maximized = true;
            }

            win->minimized = minimized;
            win->maximized = !minimized && maximized;
            win->style &= ~(windowing::WS::MINIMIZE |
                            windowing::WS::MAXIMIZE);
            if (win->minimized) {
                win->style |= windowing::WS::MINIMIZE;
            } else if (win->maximized) {
                win->style |= windowing::WS::MAXIMIZE;
            } else {
                win->restore_to_maximized = false;
            }

            VITREO_TRACE(
                "[user32_ext] native show-state callback hwnd=0x%x "
                "minimized=%u maximized=%u\n",
                hwnd, win->minimized ? 1u : 0u,
                win->maximized ? 1u : 0u);
        });

    m_host.set_native_hit_test_callback(
        [this](windowing::HWND hwnd, i32 screen_x, i32 screen_y) -> i32 {
            const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
            if (win == nullptr || win->wndproc == 0) {
                return 1; // HTCLIENT
            }
            return local_hit_test(hwnd, screen_x, screen_y);
        });
}


void User32ExtManager::queue_native_action(windowing::HWND hwnd, u32 thread_id,
    NativeActionKind kind, i32 first, i32 second) {
    bool wake = false;
    {
        std::lock_guard lock(m_native_resize_mutex);
        wake = std::none_of(m_pending_native_actions.begin(), m_pending_native_actions.end(),
            [hwnd](const auto& action) { return action.hwnd == hwnd; });
        const bool geometry = kind == NativeActionKind::Resize || kind == NativeActionKind::Move;
        if (geometry && !m_pending_native_actions.empty()) {
            auto& last = m_pending_native_actions.back();
            if (last.hwnd == hwnd && last.thread_id == thread_id && last.kind == kind) {
                last.first = first;
                last.second = second;
                return;
            }
        }
        m_pending_native_actions.push_back({hwnd, thread_id, kind, first, second});
    }
    if (wake) {
        windowing::MSG message{};
        message.hwnd = hwnd; // WM_NULL wakes the owner without guest side effects.
        m_queue.post(message);
    }
}

void User32ExtManager::drain_pending_native_resizes(u32 thread_id) {
    static thread_local bool draining = false;
    if (draining) return;
    std::vector<PendingNativeAction> pending;
    {
        std::lock_guard lock(m_native_resize_mutex);
        for (auto it = m_pending_native_actions.begin(); it != m_pending_native_actions.end();) {
            if (it->thread_id == thread_id) {
                pending.push_back(*it);
                it = m_pending_native_actions.erase(it);
            } else ++it;
        }
    }
    struct Guard { bool& value; ~Guard() { value = false; } } guard{draining};
    draining = true;
    // Generated from the SDK winuser.h. Learn defines once-per-gesture
    // messages with unused parameters and OBJID_WINDOW / CHILDID_SELF events.
    constexpr u32 enter_message = 0x0231;
    constexpr u32 exit_message = 0x0232;
    constexpr u32 start_event = 0x000A;
    constexpr u32 end_event = 0x000B;
    for (const auto& action : pending) {
        const auto hwnd = action.hwnd;
        const auto* win = m_hwnd_mgr.find(hwnd);
        if (!win || win->owner_thread_id != thread_id) continue;
        if (action.kind == NativeActionKind::EnterMoveSize ||
            action.kind == NativeActionKind::ExitMoveSize) {
            const bool entering = action.kind == NativeActionKind::EnterMoveSize;
            if (m_msgsend_mgr)
                (void)m_msgsend_mgr->send_message(hwnd, entering ? enter_message : exit_message, 0, 0);
            notify_win_event(entering ? start_event : end_event, hwnd);
        } else if (action.kind == NativeActionKind::SysCommand) {
            constexpr u32 syscommand_message = 0x0112;  // WM_SYSCOMMAND
            if (m_msgsend_mgr) {
                (void)m_msgsend_mgr->send_message(hwnd, syscommand_message,
                                                  static_cast<u64>(static_cast<u32>(action.first)), 0);
            }
        } else if (action.kind == NativeActionKind::Resize) {
            if (win->client_rect.width() == action.first && win->client_rect.height() == action.second) continue;
            const i32 nonclient_w = win->rect.width() - win->client_rect.width();
            const i32 nonclient_h = win->rect.height() - win->client_rect.height();
            (void)apply_set_window_pos(hwnd, windowing::HWND_TOP, 0, 0,
                action.first + nonclient_w, action.second + nonclient_h,
                0x0002u | 0x0004u | 0x0010u, true, true);
        } else {
            if (win->rect.left == action.first && win->rect.top == action.second) continue;
            m_applying_native_move = hwnd;
            (void)apply_set_window_pos(hwnd, windowing::HWND_TOP,
                action.first, action.second, 0, 0, 0x0001u | 0x0004u | 0x0010u);
            m_applying_native_move = windowing::NULL_HWND;
        }
    }
}

i32 User32ExtManager::local_hit_test(windowing::HWND hwnd, i32 screen_x,
                                     i32 screen_y) {
    if (m_msgsend_mgr == nullptr) return 1; // HTCLIENT
    const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr || win->wndproc == 0) return 1; // HTCLIENT
    const u64 lparam = (static_cast<u64>(static_cast<u16>(screen_y)) << 16) |
                       static_cast<u16>(screen_x);
    return static_cast<i32>(
        m_msgsend_mgr->send_message(hwnd, windowing::WM::NCHITTEST, 0, lparam));
}

void User32ExtManager::impl_WaitForInputIdle(Marshaler& m) {
    if (m_msgloop_mgr == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0xFFFFFFFFu);  // WAIT_FAILED
        return;
    }
    // HANDLE is pointer-sized; DWORD timeout is the stack low dword on x64.
    m.set_return(m_msgloop_mgr->wait_for_input_idle(m.arg(0), m.arg32(1)));
}

} // namespace vitreo::winapi
