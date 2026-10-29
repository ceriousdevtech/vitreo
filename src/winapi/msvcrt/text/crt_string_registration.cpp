// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_string_registration.cpp
/// @brief Export table for msvcrt's string and formatting surface.
///
/// Composition only: maps export names to handlers and records each one's
/// calling convention. msvcrt is cdecl, not stdcall, so most entries pop zero
/// bytes and the guest cleans its own stack -- the byte counts here are not the
/// same kind of claim they are in kernel32's table.
///
/// Split out of crt_string.cpp, which was 3,433 lines and four-fifths this
/// table. The table itself was later split by section: the entries live in the
/// sibling *_registration.cpp files (plus crt_wctype.cpp and crt_path.cpp),
/// each exposing one add_crt_*_exports registrar declared in
/// crt_string_registration_internal.hpp. This file only calls them, in the
/// order the entries were always pushed.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.

#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/msvcrt/text/crt_string_registration_internal.hpp"

namespace vitreo::winapi {

DllExports create_crt_string_thunks(CrtStringManager& mgr,
                                    CrtMemoryManager& mem_mgr,
                                    capsule::Vfs& vfs) {
    DllExports exports;

    // Order is the export-table order (thunk addresses, dump_thunks): do not
    // reorder or regroup these calls.
    add_crt_itoa_s_exports(exports, mem_mgr);
    add_crt_locale_name_exports(exports, mem_mgr);
    add_crt_charset_search_exports(exports);
    add_crt_integer_conversion_exports(exports, mem_mgr);
    add_crt_path_exports(exports, mem_mgr, vfs);
    add_crt_string_compare_exports(exports, mgr, mem_mgr);
    add_crt_memory_op_exports(exports, mgr);
    add_crt_classic_string_exports(exports);
    add_crt_wide_ctype_exports(exports);
    add_crt_ansi_string_exports(exports, mem_mgr);
    add_crt_wide_string_exports(exports);
    add_crt_token_collate_exports(exports, mem_mgr);
    add_crt_charset_conversion_exports(exports, mem_mgr);
    add_crt_fcvt_exports(exports, mem_mgr);

    add_crt_secure_string_exports(exports, mem_mgr);
    add_crt_strtod_exports(exports, mem_mgr);
    return exports;
}

} // namespace vitreo::winapi
