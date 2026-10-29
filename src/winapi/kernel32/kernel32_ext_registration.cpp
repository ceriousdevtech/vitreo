// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration.cpp
/// @brief Export table for the extended kernel32 surface.
///
/// Composition only: this file maps export names to Kernel32ExtManager
/// handlers and records each one's stdcall byte count. It contains no API
/// behavior — every handler either forwards to a method on the manager or is
/// a small lambda whose logic belongs to the export it registers.
///
/// Split out of kernel32_ext.cpp, which was 16,469 lines. Registration and
/// implementation change for different reasons and at different rates, so
/// keeping them apart means adding an export no longer touches the same
/// translation unit as changing one's behavior.
///
/// The table itself is split into nine ordered parts, one translation unit
/// each (kernel32_ext_registration_*.cpp, declared in
/// kernel32_ext_registration_parts.hpp). The parts are contiguous slices of the
/// original table and create_kernel32_ext_thunks calls them in table order.
/// That order is load-bearing: a name registered twice resolves last-wins
/// (GlobalAddAtomW, GlobalFindAtomW, GlobalDeleteAtom and GlobalGetAtomNameW
/// appear in two parts), and thunk addresses are assigned in registration
/// order.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_registration_parts.hpp"

namespace vitreo::winapi {

DllExports create_kernel32_ext_thunks(Kernel32ExtManager& mgr) {
    DllExports exports;
    exports.entries.reserve(100);

    // Table order; see the file comment before changing it.
    append_unwind_path_exports(mgr, exports);
    append_crt_thread_exports(mgr, exports);
    append_handle_resource_exports(mgr, exports);
    append_topology_legacy_exports(mgr, exports);
    append_process_misc_exports(mgr, exports);
    append_additional_win32_exports(mgr, exports);
    append_wave_forwarder_exports(mgr, exports);
    append_cef_wer_exports(mgr, exports);
    append_atom_comm_exports(mgr, exports);

    return exports;
}

} // namespace vitreo::winapi
