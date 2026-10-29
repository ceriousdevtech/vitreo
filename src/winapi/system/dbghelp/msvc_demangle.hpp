// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file msvc_demangle.hpp
/// @brief UnDecorateSymbolName's core: MSVC decorated-name -> readable text.
///
/// Guests reach this through dbghelp when they build a crash report or a stack
/// trace, so an empty result turns every frame into a blank line. It was a
/// stub returning 0; real Windows renders "?func@@YAHXZ" as
/// "int __cdecl func(void)".
///
/// This covers the shapes that actually show up in a stack trace -- free
/// functions, namespace-scoped data, member functions, and the common builtin
/// and pointer types. Anything it cannot parse degrades to the readable
/// qualified name rather than to nothing, which is what a crash report needs.

#include <vitreo/common.hpp>

#include <string>

namespace vitreo::winapi {

/// UNDNAME_* flags this understands. Others are accepted and ignored.
constexpr u32 kUndnameNameOnly = 0x1000;

/// Demangle @p mangled. A name with no leading '?' is not decorated and is
/// returned unchanged, matching Windows. Returns an empty string only when
/// the input is empty.
[[nodiscard]] std::string msvc_undecorate(const std::string& mangled, u32 flags);

} // namespace vitreo::winapi
