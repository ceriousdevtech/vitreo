// SPDX-License-Identifier: LGPL-2.1-only

/// @file gdi32_registration.cpp
/// @brief Export table for gdi32.
///
/// Composition only: maps export names to GdiManager handlers and records each
/// one's stdcall byte count. No drawing behavior lives here — every handler
/// forwards to the manager or is a small lambda belonging to the export it
/// registers.
///
/// Split out of gdi32.cpp, which was 6,497 lines and 43% registration. The
/// drawing code — BitBlt, PatBlt, DIB sections, Core Text rendering — is left
/// where it belongs, and adding an export no longer touches it.
///
/// The export table itself was later split by section: the entries live in
/// the sibling gdi32_*_exports.cpp files, each exposing one
/// add_gdi32_*_exports registrar declared in
/// gdi32_registration_internal.hpp. This file only calls them, in the order
/// the entries were always pushed.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.

#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/graphics/gdi32/gdi32_registration_internal.hpp"
#include "graphics/contracts/backend_device.hpp"

#include "winapi/core/host_gui_pump.hpp"

#include <functional>
#include <string>
#include <utility>

namespace vitreo::winapi {

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports make_gdi32_exports(
    GdiManager& mgr,
    std::function<std::string()> display_adapter_name_provider) {
    DllExports exports;
    if (!display_adapter_name_provider) {
        display_adapter_name_provider = [] {
            const auto* backend = graphics::active_backend_device();
            return (backend != nullptr && backend->available())
                ? backend->backend_device_name()
                : std::string{};
        };
    }

    // Order is the export-table order (thunk addresses, dump_thunks): do not
    // reorder or regroup these calls.
    add_gdi32_d3dkmt_exports(exports, display_adapter_name_provider);
    add_gdi32_dc_object_exports(exports, mgr);
    add_gdi32_drawing_exports(exports, mgr);
    add_gdi32_dc_state_palette_exports(exports, mgr);
    add_gdi32_viewport_region_exports(exports, mgr);
    add_gdi32_shape_region_exports(exports, mgr);
    add_gdi32_dc_attribute_exports(exports, mgr);
    add_gdi32_metafile_text_query_exports(exports, mgr);
    add_gdi32_text_font_exports(exports, mgr);
    add_gdi32_path_print_exports(exports, mgr);

    for (auto& entry : exports.entries) {
        auto handler = std::move(entry.handler);
        if (!handler) continue;
        entry.handler = [&mgr, handler = std::move(handler)](Marshaler& m) {
            {
                GdiManager::ApiLock api_lock(mgr);
                handler(m);
            }
            // Service native presentation during drawing loops that retain a
            // DC and never dispatch messages. This does not pump guest input.
            pump_host_gui();
        };
    }
    return exports;
}

}  // namespace vitreo::winapi
