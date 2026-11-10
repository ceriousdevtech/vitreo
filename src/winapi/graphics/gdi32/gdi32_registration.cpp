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
#include "random_rgn_contract.inc"

// =============================================================================
// Thunk Registration
// =============================================================================

DllExports make_gdi32_exports(
    GdiManager& mgr,
    std::function<std::string()> display_adapter_name_provider, bool guest_is_64bit,
    std::function<GdiManager::SystemRegionState()> system_region_state_provider) {
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
    add_gdi32_path_print_exports(exports, mgr,guest_is_64bit);

    for (auto& entry : exports.entries) {
        if(entry.name=="SelectClipRgn" || entry.name=="ExtSelectClipRgn" ||
           entry.name=="ExcludeClipRect" || entry.name=="SelectClipPath") {
            const auto name=entry.name;
            entry.handler=[&mgr,name,provider=system_region_state_provider](Marshaler& m) {
                const Handle dc=static_cast<Handle>(m.arg(0));GdiManager::SystemRegionState scene;
                if(provider && mgr.window_from_dc(dc)!=0)scene=provider();
                i32 result=0;
                if(name=="SelectClipRgn")result=mgr.select_clip_rgn(dc,static_cast<Handle>(m.arg(1)),&scene);
                else if(name=="ExtSelectClipRgn")result=mgr.ext_select_clip_rgn(dc,static_cast<Handle>(m.arg(1)),static_cast<i32>(m.arg32(2)),&scene);
                else if(name=="ExcludeClipRect")result=mgr.exclude_clip_rect(dc,static_cast<i32>(m.arg32(1)),static_cast<i32>(m.arg32(2)),static_cast<i32>(m.arg32(3)),static_cast<i32>(m.arg32(4)),&scene);
                else result=mgr.select_clip_path(dc,static_cast<i32>(m.arg32(1)),&scene)?1:0;
                m.set_return(static_cast<u32>(result));pump_host_gui();
            };
            continue;
        }
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
    // Shape snapshots must precede the GDI lock: USER32 never holds its state
    // lock across a GDI call. This method owns its own GDI API lock.
    exports.entries.push_back({"GetRandomRgn",
        guest_is_64bit ? random_rgn_contract::ordinal64 : random_rgn_contract::ordinal32,
        [&mgr,provider=std::move(system_region_state_provider)](Marshaler& m) {
            GdiManager::SystemRegionState scene;
            const Handle dc=static_cast<Handle>(m.arg(0));
            if (provider && m.arg32(2)==random_rgn_contract::SYSRGN && mgr.window_from_dc(dc)!=0) scene=provider();
            m.set_return(static_cast<u32>(mgr.get_random_region(dc,
                static_cast<Handle>(m.arg(1)),static_cast<i32>(m.arg32(2)),
                m.cpu_is_64bit(),&scene)));
            pump_host_gui();
        },random_rgn_contract::stdcall_bytes});
    exports.entries.push_back({"GetMetaRgn",guest_is_64bit?
        random_rgn_contract::GetMetaRgn_ordinal64:random_rgn_contract::GetMetaRgn_ordinal32,
        [&mgr](Marshaler& m) {m.set_return(static_cast<u32>(mgr.get_random_region(
            static_cast<Handle>(m.arg(0)),static_cast<Handle>(m.arg(1)),
            random_rgn_contract::METARGN,m.cpu_is_64bit())));},random_rgn_contract::GetMetaRgn_stdcall_bytes});
    exports.entries.push_back({"SetMetaRgn",guest_is_64bit?
        random_rgn_contract::SetMetaRgn_ordinal64:random_rgn_contract::SetMetaRgn_ordinal32,
        [&mgr](Marshaler& m) {m.set_return(static_cast<u32>(mgr.set_meta_region(
            static_cast<Handle>(m.arg(0)))));},random_rgn_contract::SetMetaRgn_stdcall_bytes});
    return exports;
}

}  // namespace vitreo::winapi
