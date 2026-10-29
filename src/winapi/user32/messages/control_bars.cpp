// SPDX-License-Identifier: LGPL-2.1-only

/// @file control_bars.cpp
/// @brief Rebar, toolbar and statusbar native controls.

#include "winapi/user32/user32_internal.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "winapi/user32/messages/message_send_internal.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include "windowing/model/diag_flags.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/user32/messages/cross_process_send.hpp"
#include "ipc/hwnd_registry.hpp"
#include "ipc/process_registry.hpp"
#include <unistd.h>
#include <algorithm>
#include <pthread.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

void MessageSendManager::adjust_common_control_size(windowing::HWND hwnd,
                                                     i32& width, i32& height) const {
    auto* win = m_hwnd_mgr.find(hwnd);
    if (!win || (win->style & 4u)) return; // CCS_NORESIZE
    std::lock_guard lock(m_control_state_mutex);
    auto band = m_rebar_state.find(hwnd);
    if (band == m_rebar_state.end()) return;
    const i32 extent = (band->second.style & 8u) ? 0 : band->second.row_height;
    if (win->style & 0x80u) width = extent; // CCS_VERT
    else height = extent;
}

bool MessageSendManager::insert_rebar_child(windowing::HWND rebar, windowing::HWND child, i32 height) {
    auto* band = m_hwnd_mgr.find(rebar);
    auto* parent = band ? m_hwnd_mgr.find(band->parent) : nullptr;
    if (!band || !parent || !m_hwnd_mgr.find(child) || height <= 0 || height > 65535) return false;
    {
        std::lock_guard lock(m_control_state_mutex);
        if (m_rebar_state[rebar].band_count != 0) return false;
    }
    // Same-window messages belong to its receiving thread. Other HWNDs may
    // insert into this table while Cocoa attaches the child.
    if (!m_bridge.rebar_attach_child(rebar, child)) return false;
    {
        std::lock_guard lock(m_control_state_mutex);
        auto& state = m_rebar_state[rebar];
        state.band_count = 1;
        state.row_height = height;
    }
    const i32 width = parent->client_rect.right - parent->client_rect.left;
    band->rect = {0, 0, width, height};
    band->client_rect = band->rect;
    m_bridge.set_control_frame(rebar, band->rect);
    return true;
}

bool MessageSendManager::dispatch_rebar_control(Marshaler& m, windowing::HWND hwnd,
                                               u32 msg, u64 wParam, u64 lParam) {
    auto* win = window_data(hwnd);
    if (!win) return false;
    auto name = win->control_class_name();
    for (auto& c : name) c = std::towlower(c);
    if (name != L"rebarwindow32") return false;
    RebarState band;
    {
        std::lock_guard lock(m_control_state_mutex);
        band = m_rebar_state[hwnd];
    }
    if (msg == 0x410) { // RB_IDTOINDEX
        m.set_return(band.band_count && band.id == wParam ? 0ull : ~0ull);
        return true;
    }
    const bool insert = msg == 0x40a || msg == 0x401;
    const bool get = msg == 0x41c || msg == 0x405;
    const bool set = msg == 0x40b || msg == 0x406;
    if (!insert && !get && !set) return false;
    const bool wide = m.cpu_is_64bit();
    const u32 size = m.read_u32(lParam).value_or(0);
    const u32 mask = m.read_u32(lParam + 4).value_or(0);
    if (size < (wide ? 80u : 56u) ||
        (!insert && (!band.band_count || wParam != 0))) { m.set_return(0u); return true; }
    const u32 child_offset = wide ? 40 : 32;
    const u32 min_offset = wide ? 48 : 36;
    const u32 id_offset = wide ? 72 : 52;
    if (get) {
        if (mask & 1) (void)m.write_u32(lParam + 8, band.style);
        if (mask & 0x10) {
            if (wide) (void)m.write_u64(lParam + child_offset, band.child);
            else (void)m.write_u32(lParam + child_offset, band.child);
        }
        if (mask & 0x20) {
            (void)m.write_u32(lParam + min_offset, band.min_width);
            (void)m.write_u32(lParam + min_offset + 4, band.min_height);
        }
        if (mask & 0x40) (void)m.write_u32(lParam + min_offset + 8, band.width);
        if (mask & 0x100) (void)m.write_u32(lParam + id_offset, band.id);
        m.set_return(1u); return true;
    }
    if (insert) {
        if ((mask & 0x30u) != 0x30u || (wParam != 0 && static_cast<u32>(wParam) != 0xffffffffu)) {
            m.set_return(0u); return true;
        }
        const u64 child = wide ? m.read_u64(lParam + child_offset).value_or(0) : m.read_u32(lParam + child_offset).value_or(0);
        const u32 height = m.read_u32(lParam + min_offset + 4).value_or(0);
        if (height > 65531 || !insert_rebar_child(hwnd, child, height + 4)) { m.set_return(0u); return true; }
        {
            std::lock_guard lock(m_control_state_mutex);
            band = m_rebar_state[hwnd];
        }
        band.child = child;
    }
    if (mask & 1) band.style = m.read_u32(lParam + 8).value_or(0);
    if (mask & 0x20) {
        band.min_width = m.read_u32(lParam + min_offset).value_or(0);
        band.min_height = m.read_u32(lParam + min_offset + 4).value_or(0);
    }
    if (mask & 0x40) band.width = m.read_u32(lParam + min_offset + 8).value_or(0);
    if (mask & 0x100) band.id = m.read_u32(lParam + id_offset).value_or(0);
    {
        std::lock_guard lock(m_control_state_mutex);
        m_rebar_state[hwnd] = band;
    }
    if (insert && m_wndproc_callback) {
        // A changed band height must notify the parent before it lays out
        // neighboring controls. Give nested notifications separate storage.
        const GuestAddr nm = m.memory().find_free_gap(0x3f000000, 4096);
        if (nm && m.memory().map_region(nm, 4096, MemoryProtection::ReadWrite).is_ok()) {
            if (wide) {
                (void)m.write_u64(nm, hwnd);
                (void)m.write_u64(nm + 8, win->ctrl_id);
                (void)m.write_u32(nm + 16, static_cast<u32>(-831));
            } else {
                (void)m.write_u32(nm, hwnd);
                (void)m.write_u32(nm + 4, win->ctrl_id);
                (void)m.write_u32(nm + 8, static_cast<u32>(-831));
            }
            (void)send_message(win->parent, windowing::WM::NOTIFY, win->ctrl_id, nm);
            (void)m.memory().unmap_region(nm, 4096);
        }
    }
    m.set_return(1u);
    return true;
}

bool try_handle_toolbar(Marshaler& m, MessageSendManager& msm,
                        windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam) {
    auto* win = msm.window_data(hwnd);
    if (!win) return false;
    auto name = win->control_class_name();
    for (auto& c : name) c = std::towlower(c);
    if (name != L"toolbarwindow32") return false;
    if (msg == 0x413) { // TB_ADDBITMAP: explicit guest pointer widths.
        const bool wide = m.cpu_is_64bit();
        const u64 instance = wide ? m.read_u64(lParam).value_or(0) : m.read_u32(lParam).value_or(0);
        const u64 id = wide ? m.read_u64(lParam + 8).value_or(0) : m.read_u32(lParam + 4).value_or(0);
        u32 bitmap = instance == 0 ? static_cast<u32>(id) : 0;
        if (instance && id <= 0xffff) {
            if (auto dir = try_get_resource_dir_from_module(m.memory(), instance)) {
                auto resource = windowing::find_resource(m.memory(), dir->image_base,
                    dir->rsrc_rva, dir->rsrc_size, windowing::RT::BITMAP, static_cast<u16>(id));
                if (resource.data && resource.size) {
                    auto bmp = dib_to_bmp_file(resource.data, resource.size);
                    // Toolbar bitmap resources use the standard button-face
                    // palette mapping. Do not infer a mask from corner pixels.
                    if (bmp.size() >= 54) {
                        auto word = [&](usize at) { return static_cast<u32>(bmp[at]) | (static_cast<u32>(bmp[at + 1]) << 8); };
                        auto dword = [&](usize at) { return word(at) | (word(at + 2) << 16); };
                        const u32 bits = word(28), header = dword(14), offset = dword(10);
                        if (bits <= 8 && header >= 40 && offset <= bmp.size()) {
                            for (usize i = 14 + header; i + 3 < offset; i += 4)
                                if (bmp[i] == 192 && bmp[i + 1] == 192 && bmp[i + 2] == 192)
                                    bmp[i] = bmp[i + 1] = bmp[i + 2] = 240; // COLOR_BTNFACE
                        }
                    }
                    bitmap = msm.bridge().register_bitmap_bmp(bmp);
                }
            }
        }
        m.set_return(static_cast<u64>(static_cast<i64>(msm.bridge().toolbar_add_bitmap(hwnd, bitmap, wParam))));
        return true;
    }
    if (msg == 0x444 || msg == 0x414 || msg == 0x443 || msg == 0x415) {
        const bool append = msg == 0x444 || msg == 0x414;
        const u32 count = append ? static_cast<u32>(wParam) : 1;
        if (count > 4096 || (count && !lParam)) { m.set_return(0u); return true; }
        const bool wide = m.cpu_is_64bit();
        const u32 stride = wide ? 32 : 20;
        std::vector<windowing::CocoaBridge::ToolbarButton> buttons;
        for (u32 i = 0; i < count; ++i) {
            const GuestAddr ptr = lParam + i * stride;
            auto image = m.read_u32(ptr); auto command = m.read_u32(ptr + 4);
            auto state = m.read_u8(ptr + 8); auto style = m.read_u8(ptr + 9);
            if (image.is_err() || command.is_err() || state.is_err() || style.is_err()) { m.set_return(0u); return true; }
            windowing::CocoaBridge::ToolbarButton button;
            button.image = static_cast<i32>(image.value()); button.command = command.value();
            button.state = state.value(); button.style = style.value();
            button.data = wide ? m.read_u64(ptr + 16).value_or(0) : m.read_u32(ptr + 12).value_or(0);
            const u64 text = wide ? m.read_u64(ptr + 24).value_or(0) : m.read_u32(ptr + 16).value_or(0);
            button.string_ref = text;
            if (text > 0xffff && text != (wide ? ~0ull : 0xffffffffull)) {
                auto string = (msg == 0x444 || msg == 0x443) ? m.read_wide_string(text) : m.read_ansi_string(text);
                if (string.is_ok()) button.text = message_send_utf8_to_wstring(string.value());
            }
            buttons.push_back(std::move(button));
        }
        m.set_return(msm.bridge().toolbar_insert_buttons(hwnd, append ? -1 : static_cast<i32>(wParam), buttons) ? 1u : 0u);
        return true;
    }
    if (msg == 0x401 || msg == 0x402 || msg == 0x404) { // enable, check, hide
        const u8 mask = msg == 0x401 ? 4 : (msg == 0x402 ? 1 : 8);
        m.set_return(msm.bridge().toolbar_set_state(hwnd, wParam, mask, (lParam & 0xffff) != 0) ? 1u : 0u);
        return true;
    }
    if (msg == 0x41d) { // TB_GETITEMRECT
        const auto buttons = msm.bridge().toolbar_buttons(hwnd);
        if (wParam >= buttons.size() || !lParam) { m.set_return(0u); return true; }
        const u64 size = msm.send_message(hwnd, TB_GETBUTTONSIZE, 0, 0);
        const i32 width = size & 0xffff, height = (size >> 16) & 0xffff;
        i32 left = 0;
        for (usize i = 0; i < wParam; ++i) if (!(buttons[i].state & 8))
            left += (buttons[i].style & 1) ? std::max(buttons[i].image, 8) : width;
        const auto& button = buttons[wParam];
        const i32 right = left + ((button.style & 1) ? std::max(button.image, 8) : width);
        (void)m.write_u32(lParam, left); (void)m.write_u32(lParam + 4, 2);
        (void)m.write_u32(lParam + 8, right); (void)m.write_u32(lParam + 12, height + 2);
        m.set_return(1u); return true;
    }
    if (msg == 0x417) { // TB_GETBUTTON
        const auto buttons = msm.bridge().toolbar_buttons(hwnd);
        if (wParam >= buttons.size() || !lParam) { m.set_return(0u); return true; }
        const auto& button = buttons[wParam];
        const bool wide = m.cpu_is_64bit();
        bool ok = m.write_u32(lParam, button.image).is_ok() &&
                  m.write_u32(lParam + 4, button.command).is_ok() &&
                  m.write_u8(lParam + 8, button.state).is_ok() &&
                  m.write_u8(lParam + 9, button.style).is_ok();
        if (wide) ok = ok && m.write_u64(lParam + 16, button.data).is_ok() && m.write_u64(lParam + 24, button.string_ref).is_ok();
        else ok = ok && m.write_u32(lParam + 12, button.data).is_ok() && m.write_u32(lParam + 16, button.string_ref).is_ok();
        m.set_return(ok ? 1u : 0u); return true;
    }
    if (msg == 0x418) { m.set_return(msm.bridge().toolbar_buttons(hwnd).size()); return true; }
    if (msg == 0x412) { // TB_GETSTATE
        for (const auto& button : msm.bridge().toolbar_buttons(hwnd)) if (button.command == wParam) {
            m.set_return(button.state); return true;
        }
        m.set_return(~0ull); return true;
    }
    return false;
}

bool try_handle_statusbar(Marshaler& m, MessageSendManager& msm,
                          windowing::HWND hwnd, u32 msg, u64 wParam, u64 lParam) {
    auto* win = msm.window_data(hwnd);
    if (!win) return false;
    auto name = win->control_class_name();
    for (auto& c : name) c = std::towlower(c);
    if (name != L"msctls_statusbar32") return false;
    auto& bridge = msm.bridge();
    if (msg == windowing::WM::SIZE) {
        if (auto* parent = msm.window_data(win->parent)) {
            const i32 width = parent->client_rect.right - parent->client_rect.left;
            const i32 height = parent->client_rect.bottom - parent->client_rect.top;
            win->rect = {0, std::max(0, height - 22), width, height};
            win->client_rect = {0, 0, width, 22};
            bridge.set_control_frame(hwnd, win->rect);
            bridge.status_set_parts(hwnd, bridge.status_get_parts(hwnd));
        }
        m.set_return(0u);
        return true;
    }
    if (msg == 0x404) { // SB_SETPARTS
        std::vector<i32> edges;
        if (wParam == 0 || wParam > 256 || !lParam) { m.set_return(0u); return true; }
        for (u32 i = 0; i < wParam; ++i) {
            auto edge = m.read_u32(lParam + i * 4);
            if (edge.is_err()) { m.set_return(0u); return true; }
            edges.push_back(static_cast<i32>(edge.value()));
        }
        m.set_return(bridge.status_set_parts(hwnd, edges) ? 1u : 0u);
        return true;
    }
    if (msg == 0x40b || msg == 0x401) { // SB_SETTEXTW/A, regular text parts
        if (wParam & 0x1000u) { m.set_return(0u); return true; } // owner draw needs WM_DRAWITEM
        auto text = msg == 0x40b ? m.read_wide_string(lParam) : m.read_ansi_string(lParam);
        m.set_return(text.is_ok() && bridge.status_set_text(hwnd, wParam & 0xffu,
                     message_send_utf8_to_wstring(text.value())) ? 1u : 0u);
        return true;
    }
    if (msg == 0x406) { // SB_GETPARTS
        auto edges = bridge.status_get_parts(hwnd);
        if (lParam) for (usize i = 0; i < std::min<usize>(wParam, edges.size()); ++i)
            (void)m.write_u32(lParam + i * 4, edges[i]);
        m.set_return(static_cast<u32>(edges.size()));
        return true;
    }
    if (msg == 0x40a) { // SB_GETRECT
        auto edges = bridge.status_get_parts(hwnd);
        if (wParam >= edges.size() || !lParam) { m.set_return(0u); return true; }
        i32 left = wParam ? edges[wParam - 1] : 0;
        i32 right = edges[wParam] < 0 ? win->client_rect.right : edges[wParam];
        (void)m.write_u32(lParam, left); (void)m.write_u32(lParam + 4, 0);
        (void)m.write_u32(lParam + 8, right); (void)m.write_u32(lParam + 12, win->client_rect.bottom);
        m.set_return(1u);
        return true;
    }
    return false;
}

} // namespace vitreo::winapi
