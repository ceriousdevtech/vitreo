// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog_helpers.cpp
/// @brief Promoted helpers shared across dialog.cpp banner cuts.

#include "winapi/user32/dialogs/dialog_internal.hpp"
#include <array>

namespace vitreo::winapi {

GuestAddr marshal_modal_tree_notify(
    GuestMemory& memory,
    const windowing::TreeNotify& notify) {
    static GuestAddr scratch = 0;
    if (scratch == 0 || !memory.is_mapped(scratch, 160)) {
        scratch = memory.find_free_gap(static_cast<GuestAddr>(0x3D000000), 4096);
        if (scratch == 0 ||
            memory.map_region(scratch, 4096, MemoryProtection::ReadWrite).is_err()) {
            scratch = 0;
            return 0;
        }
    }

    constexpr u32 tvn_sel_changed_w = 0xFFFFFE6Eu;
    constexpr u32 tvn_item_expanding_a = 0xFFFFFE6Bu;
    constexpr u32 tve_expand = 0x0002u;
    constexpr u32 tve_collapse = 0x0001u;
    constexpr u32 tvif_text_state_handle_param = 0xDu;
    const bool item_expanding = notify.kind !=
        windowing::TreeNotify::Kind::Selection;
    const u32 action = notify.kind ==
        windowing::TreeNotify::Kind::Expanding
            ? tve_expand
            : (notify.kind == windowing::TreeNotify::Kind::Collapsing
                ? tve_collapse : 0u);
    for (u32 offset = 0; offset < 160; offset += 8) {
        (void)memory.write_u64(scratch + offset, 0);
    }
    (void)memory.write_u32(scratch + 0, static_cast<u32>(notify.ctrl));
    (void)memory.write_u32(scratch + 4, notify.ctrl_id);
    (void)memory.write_u32(scratch + 8,
                           item_expanding ? tvn_item_expanding_a : tvn_sel_changed_w);
    (void)memory.write_u32(scratch + 12, action);
    (void)memory.write_u32(scratch + 56, tvif_text_state_handle_param);
    (void)memory.write_u32(scratch + 60, static_cast<u32>(notify.h_item));
    (void)memory.write_u32(scratch + 64, 2u);
    (void)memory.write_u32(scratch + 92, static_cast<u32>(notify.lparam));
    return scratch;
}

GuestAddr marshal_modal_tab_notify(
    GuestMemory& memory, u64 marker, u32 control_id) {
    windowing::HWND control = windowing::NULL_HWND;
    bool selection_changing = false;
    if (!windowing::decode_tab_notify_lparam(
            marker, &control, &selection_changing)) {
        return 0;
    }

    static GuestAddr scratch = 0;
    if (scratch == 0 || !memory.is_mapped(scratch, 12)) {
        scratch = memory.find_free_gap(
            static_cast<GuestAddr>(0x3D001000), 4096);
        if (scratch == 0 ||
            memory.map_region(
                scratch, 4096, MemoryProtection::ReadWrite).is_err()) {
            scratch = 0;
            return 0;
        }
    }

    constexpr u32 tcn_sel_change = 0xFFFFFDD9u;
    constexpr u32 tcn_sel_changing = 0xFFFFFDD8u;
    (void)memory.write_u32(scratch + 0, static_cast<u32>(control));
    (void)memory.write_u32(scratch + 4, control_id);
    (void)memory.write_u32(
        scratch + 8,
        selection_changing ? tcn_sel_changing : tcn_sel_change);
    return scratch;
}

std::string wstring_to_utf8(const std::wstring& ws) {
    std::string result;
    result.reserve(ws.size() * 2);
    for (wchar_t wc : ws) {
        auto cp = static_cast<uint32_t>(wc);
        if (cp < 0x80) {
            result.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return result;
}

bool try_parse_dialog_from_guest_memory(
    GuestMemory& memory,
    GuestAddr template_addr,
    windowing::DialogTemplate& out_tmpl
) {
    // Probe common template sizes. Smaller reads are more likely to stay
    // within mapped pages while still covering typical installer dialogs.
    constexpr std::array<usize, 5> kProbeSizes = {
        2048, 4096, 8192, 16384, 32768
    };

    for (usize size : kProbeSizes) {
        std::vector<u8> raw(size);
        if (!memory.read_bytes(template_addr,
                               reinterpret_cast<Byte*>(raw.data()),
                               raw.size()).is_ok()) {
            continue;
        }
        auto parsed = windowing::parse_dialog_template(raw.data(), raw.size());
        if (parsed.is_ok()) {
            out_tmpl = std::move(parsed.value());
            return true;
        }
    }

    return false;
}

} // namespace vitreo::winapi
