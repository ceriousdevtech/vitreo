#pragma once

/// @file user32_errors.hpp
/// @brief USER error codes (winerror.h, "ERROR_INVALID_WINDOW_HANDLE" block)
/// that kernel32/error.hpp does not carry.
///
/// Values checked against the SDK's shared/winerror.h (10.0.26100.0) on
/// Windows; the S1 windowing probes observe them from Windows 11
/// (tools/oracle_probe/s1win_*_probe.expected), e.g. ERROR_TLW_WITH_WSCHILD
/// from CreateWindowEx(WS_CHILD, parent NULL).

#include <vitreo/common.hpp>

namespace vitreo::winapi::user32_errors {

constexpr u32 ERROR_TLW_WITH_WSCHILD         = 1406;
constexpr u32 ERROR_CANNOT_FIND_WND_CLASS    = 1407;
constexpr u32 ERROR_WINDOW_OF_OTHER_THREAD   = 1408;
constexpr u32 ERROR_CLASS_HAS_WINDOWS        = 1412;
constexpr u32 ERROR_INVALID_INDEX            = 1413;
constexpr u32 ERROR_CHILD_WINDOW_MENU        = 1436;
constexpr u32 ERROR_CONTROL_ID_NOT_FOUND     = 1421;
constexpr u32 ERROR_HWNDS_HAVE_DIFF_PARENT   = 1441;
constexpr u32 ERROR_NOT_CHILD_WINDOW         = 1442;
constexpr u32 ERROR_INVALID_GW_COMMAND       = 1443;
constexpr u32 ERROR_DC_NOT_FOUND             = 1425;
constexpr u32 ERROR_INVALID_SHOWWIN_COMMAND  = 1449;
constexpr u32 ERROR_INVALID_MONITOR_HANDLE   = 1461;

} // namespace vitreo::winapi::user32_errors
