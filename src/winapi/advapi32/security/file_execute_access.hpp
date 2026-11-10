// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <vitreo/common.hpp>
#include <string>
namespace vitreo::capsule { class Vfs; }
namespace vitreo::winapi {
class SecurityStubs;
// Authorize the actual calling token, including restricting/deny-only SIDs,
// file generic mappings, mandatory labels, and directory traversal. Returns
// a Win32 error; this helper does not alter thread last-error state.
[[nodiscard]] u32 check_file_execute_access(SecurityStubs&, capsule::Vfs&,
                                           const std::string& absolute_path);
}
