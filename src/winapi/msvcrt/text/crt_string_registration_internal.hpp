#pragma once
/// @file crt_string_registration_internal.hpp
/// @brief Private surface between create_crt_string_thunks and its section
///        registrars.
///
/// create_crt_string_thunks was one 2,600-line function that pushed every msvcrt
/// string export in turn. It is now an orchestrator that calls one registrar per
/// contiguous run of entries, in the order the entries were always pushed:
/// DllExports::entries order decides each thunk's address and the order
/// dump_thunks prints, both recorded by the export inventory, so the call order
/// in create_crt_string_thunks must not change.
///
/// Each registrar takes exactly the objects its handlers captured from
/// create_crt_string_thunks (mgr, mem_mgr, vfs); the captures themselves are
/// unchanged. Private to msvcrt/text -- not part of any public surface.

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class CrtMemoryManager;
class CrtStringManager;

/// _itoa_s / _itow_s (crt_string_locale_registration.cpp).
void add_crt_itoa_s_exports(DllExports& exports, CrtMemoryManager& mem_mgr);

/// _create_locale / setlocale / _wsetlocale (crt_string_locale_registration.cpp).
void add_crt_locale_name_exports(DllExports& exports, CrtMemoryManager& mem_mgr);

/// strpbrk / _mbspbrk / _mbstrlen / strcspn / strspn / wcspbrk
/// (crt_string_search_registration.cpp).
void add_crt_charset_search_exports(DllExports& exports);

/// strtol family, ato* / _wto* wrappers and wcstombs
/// (crt_string_integer_registration.cpp).
void add_crt_integer_conversion_exports(DllExports& exports,
                                        CrtMemoryManager& mem_mgr);

/// _splitpath family and _fullpath (crt_path.cpp).
void add_crt_path_exports(DllExports& exports, CrtMemoryManager& mem_mgr,
                          capsule::Vfs& vfs);

/// _stricmp / _strnicmp / _wcsicmp / _wcsnicmp / _mbscmp / _strdup / _wcsdup
/// (crt_string_compare_registration.cpp).
void add_crt_string_compare_exports(DllExports& exports, CrtStringManager& mgr,
                                    CrtMemoryManager& mem_mgr);

/// memset / memcpy / memmove / memcmp / strlen / wcslen / strcpy / strcat
/// (crt_string_memory_registration.cpp).
void add_crt_memory_op_exports(DllExports& exports, CrtStringManager& mgr);

/// Narrow <ctype.h>, strstr, wcscpy, _itoa and friends
/// (crt_string_classic_registration.cpp).
void add_crt_classic_string_exports(DllExports& exports);

/// isw* / towlower / towupper (crt_wctype.cpp).
void add_crt_wide_ctype_exports(DllExports& exports);

/// strcmp / strncmp / strchr / strrchr / strncpy / strncat and the _mbs* pair
/// (crt_string_ansi_registration.cpp).
void add_crt_ansi_string_exports(DllExports& exports, CrtMemoryManager& mem_mgr);

/// wmem* / wcs* / mbrtowc / wcrtomb / mbsrtowcs (crt_string_wide_registration.cpp).
void add_crt_wide_string_exports(DllExports& exports);

/// memchr / strtok / strtok_s / strcoll / strxfrm / wcscoll / wcsxfrm
/// (crt_string_token_registration.cpp).
void add_crt_token_collate_exports(DllExports& exports, CrtMemoryManager& mem_mgr);

/// mbstowcs / mbtowc / wctomb / wcstombs_s / btowc / _isctype / iswctype
/// (crt_string_charset_conversion_registration.cpp).
void add_crt_charset_conversion_exports(DllExports& exports,
                                        CrtMemoryManager& mem_mgr);

/// _atoflt / _fcvt / _ecvt (crt_string_fcvt_registration.cpp).
void add_crt_fcvt_exports(DllExports& exports, CrtMemoryManager& mem_mgr);

} // namespace vitreo::winapi
