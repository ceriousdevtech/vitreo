// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file wer.hpp
/// @brief wer.dll — Windows Error Reporting.
///
/// Every export of the real wer.dll exists (wer_export_table.inc, generated
/// by tools/gen_wer_exports.py). The application exclusion list
/// (WerAddExcludedApplication / WerRemoveExcludedApplication) is implemented
/// on the capsule registry. Error reports, the report store, wait-chain
/// traversal and the undocumented Werp* internals fail at the call with the
/// documented failure value and a one-time UNIMPLEMENTED line
/// (.docs/contracts/wer.md).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <string>
#include <vector>

namespace vitreo::capsule {
class Registry;
}

namespace vitreo::winapi {

/// Build the wer.dll thunk table over @p registry.
DllExports create_wer_thunks(capsule::Registry& registry);

/// Build the faultrep.dll thunk table over @p registry (errorrep.h: ReportFault,
/// AddERExcludedApplicationA/W). The undocumented internal exports of the real
/// DLL (ReportHang, ReportFaultToQueue, ...) are not provided.
DllExports create_faultrep_thunks(capsule::Registry& registry);

/// Every wer.dll export name, in ordinal order.
std::vector<std::string> wer_export_names();

namespace wer {

constexpr u32 S_OK_ = 0;
constexpr u32 E_FAIL_ = 0x80004005u;
constexpr u32 E_INVALIDARG_ = 0x80070057u;
constexpr u32 E_ACCESSDENIED_ = 0x80070005u;

/// WerAddExcludedApplication: exclude @p exe_name (its file-name part) from
/// error reporting for the current user, or all users when @p all_users.
u32 add_excluded_application(capsule::Registry& registry, const std::wstring& exe_name, bool all_users);
/// WerRemoveExcludedApplication: E_FAIL when the name is not on the list.
u32 remove_excluded_application(capsule::Registry& registry, const std::wstring& exe_name, bool all_users);

}  // namespace wer

}  // namespace vitreo::winapi
