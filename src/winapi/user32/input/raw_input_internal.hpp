// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file raw_input_internal.hpp
/// @brief Private to winapi/user32/input/raw_input_*.cpp: the
///        VITREO_INPUT_DIAG switch the raw-input files share. It stays in an
///        anonymous namespace: each including TU gets its own internal-linkage
///        copy, as the one TU had before, so no symbol is added to the
///        library. Split out of the former mixed USER32 stubs file as
///        verbatim code motion.

#include <cstdlib>

namespace vitreo::winapi {

namespace {
bool raw_input_diag() {
    static const bool on = std::getenv("VITREO_INPUT_DIAG") != nullptr;
    return on;
}
} // namespace

} // namespace vitreo::winapi
