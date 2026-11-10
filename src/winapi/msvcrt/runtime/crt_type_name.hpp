// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_type_name.hpp
/// @brief msvcrt.dll type_info::name(): the readable form of an RTTI type
///        encoding.
///
/// type_info keeps the decorated type (".?AVFoo@@", ".PAH", ...) after its
/// vftable pointer and cache cell. name() undecorates everything after the
/// leading '.' as a data type, the way msvcrt's built-in undecorator does in
/// type-only mode, trims trailing blanks, and caches the result.
///
/// The output text -- spacing, "__ptr64", the "??" markers a truncated
/// encoding produces, the constructs it does not understand ("$$Q", "_S",
/// "W8", a top-level 'Y') falling back to the decorated text -- is measured,
/// not recalled: tools/oracle_probe/crt_legacy_exports_probe.c records 220
/// encodings on Windows (both bitnesses print the same text) and the test
/// replays every one.

#include <string>
#include <string_view>

namespace vitreo::winapi {

/// The text msvcrt's type_info::name() returns for the decorated name stored
/// in type_info::_M_d_name (including its leading '.').
[[nodiscard]] std::string msvcrt_type_info_name(std::string_view decorated);

} // namespace vitreo::winapi
