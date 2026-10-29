// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file sspicli.hpp
/// @brief SspiCli.dll — untrusted LSA connection and GetUserNameEx.
///
/// On Windows secur32.dll forwards its Lsa* and GetUserNameEx* exports to
/// sspicli.dll, so both DLLs expose one implementation. Vitreo's LSA has no
/// authentication packages (no MSV1_0 / Kerberos / Negotiate), so every package
/// lookup reports STATUS_NO_SUCH_PACKAGE exactly as Windows does for a package
/// it does not have (tools/oracle_probe/sspi_secur32_probe.expected).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <string>

namespace vitreo::winapi {

class Marshaler;

/// Owner object kept for the sspicli.dll registration; the LSA state is
/// process-wide and shared with secur32.dll.
class SspiCliManager {
public:
    SspiCliManager() = default;
    ~SspiCliManager() = default;

    SspiCliManager(const SspiCliManager&) = delete;
    SspiCliManager& operator=(const SspiCliManager&) = delete;
};

/// Create SspiCli.dll export thunks.
DllExports create_sspicli_thunks(SspiCliManager& mgr);

/// Append the exports secur32.dll forwards to sspicli.dll on Windows
/// (GetUserNameExA/W and the Lsa* connection functions).
void append_sspicli_forwarded_exports(DllExports& exports);

/// Append secur32.dll's own secext.h computer-name export
/// (GetComputerObjectNameA/W; Windows does not forward it to sspicli).
void append_secur32_computer_object_name_exports(DllExports& exports);

/// The NameSamCompatible name ("<computer>\\<user>") Vitreo reports. The
/// parts match GetComputerNameW ("VITREO") and GetUserNameW ("VitUser").
[[nodiscard]] const char* sspi_sam_compatible_name();

/// Print "[vitreo] UNIMPLEMENTED <what>" to stderr once per distinct `what`.
void sspi_unimplemented_once(const std::string& what);

} // namespace vitreo::winapi
