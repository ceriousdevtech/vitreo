// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dialog_internal.hpp
/// @brief Shared helpers for dialog.cpp translation-unit splits.
///
/// diag_logs_enabled / dib_to_bmp_file are defined in user32_ext.cpp (also
/// declared in user32_internal.hpp). The marshal_* / wstring_to_utf8 symbols
/// were file-local to dialog.cpp and are promoted here so banner cuts can close.
/// This header deliberately does not include user32_internal.hpp: dialog.cpp
/// still carries file-local utf8_to_wstring with a different contract than
/// the user32_ext copy.

#include "core/memory/guest_memory.hpp"
#include "windowing/contracts/control_host.hpp"
#include "windowing/resources/dialog_template.hpp"

#include <string>
#include <vector>

namespace vitreo::winapi {

/// VITREO_ENABLE_DIAG_LOGS gating. Defined in user32_ext.cpp.
[[nodiscard]] bool diag_logs_enabled() noexcept;

/// Wrap an RT_BITMAP DIB in a BITMAPFILEHEADER. Defined in user32_ext.cpp.
[[nodiscard]] std::vector<u8> dib_to_bmp_file(const u8* dib, u32 dib_size);

/// Marshal a Cocoa tree-view notify into a guest NMHDR/NMTREEVIEW scratch buffer.
[[nodiscard]] GuestAddr marshal_modal_tree_notify(
    GuestMemory& memory,
    const windowing::TreeNotify& notify);

/// Marshal a Cocoa tab-control notify marker into a guest NMHDR scratch buffer.
[[nodiscard]] GuestAddr marshal_modal_tab_notify(
    GuestMemory& memory, u64 marker, u32 control_id);

/// Narrow a macOS UTF-32 wstring to UTF-8 (wchar_t is 4 bytes on this host).
[[nodiscard]] std::string wstring_to_utf8(const std::wstring& ws);

/// Probe guest memory for a dialog template at `template_addr`.
[[nodiscard]] bool try_parse_dialog_from_guest_memory(
    GuestMemory& memory,
    GuestAddr template_addr,
    windowing::DialogTemplate& out_tmpl);

} // namespace vitreo::winapi
