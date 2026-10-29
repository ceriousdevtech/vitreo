// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration.cpp
/// @brief Export table for ntdll's extended surface.
///
/// Composition only: maps export names to the handlers in ntdll_ext.cpp and
/// records each one's stdcall byte count.
///
/// ntdll is the layer kernel32 is written on top of, which is why the handlers
/// it names live in a nested `ntdll` namespace — see ntdll_internal.hpp.
///
/// Split out of ntdll_ext.cpp, which was 3,278 lines.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// The table itself is split into 11 ordered parts, ntdll_registration_*.cpp,
/// declared in ntdll_registration_parts.hpp. make_ntdll_ext_exports below
/// appends them in table order; that order is guest-visible.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"

namespace vitreo::winapi {

// =============================================================================
// Export Factory
// =============================================================================

DllExports make_ntdll_ext_exports(NtdllExtManager& mgr) {
    DllExports exports;
    u16 ordinal = 100; // Start after core ntdll ordinals

    append_context_memory_exports(mgr, exports, ordinal);
    append_loader_function_table_exports(mgr, exports, ordinal);
    append_object_query_exports(mgr, exports, ordinal);
    append_thread_wait_exports(mgr, exports, ordinal);
    append_rtl_memory_compression_exports(mgr, exports, ordinal);
    append_file_exports(mgr, exports, ordinal);
    append_rtl_unwind_version_exports(mgr, exports, ordinal);
    append_native_services_exports(mgr, exports, ordinal);
    append_thread_control_exports(mgr, exports, ordinal);
    append_key_object_exports(mgr, exports, ordinal);
    append_debug_slist_thread_start_exports(mgr, exports, ordinal);

    return exports;
}

} // namespace vitreo::winapi
