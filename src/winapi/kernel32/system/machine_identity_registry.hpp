// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file machine_identity_registry.hpp
/// @brief The registry's view of the machine name and the system environment.
///
/// Every Windows installation has HKLM\SYSTEM\CurrentControlSet\Control\
/// ComputerName\{ComputerName,ActiveComputerName}, the host name under
/// Services\Tcpip\Parameters and the system variables under Control\Session
/// Manager\Environment. Software that fingerprints the machine reads them
/// directly instead of calling GetComputerName / GetEnvironmentVariable, so
/// they must agree with what those APIs report.

#include <string>

namespace vitreo::capsule {
class Registry;
}

namespace vitreo::winapi {

class EnvironmentManager;

/// Publishes the machine name (as GetComputerName reports it) under
/// ComputerName and Tcpip\Parameters, and the system environment under
/// Session Manager\Environment with the names and value types Windows uses
/// (tools/oracle_probe/machine_identity_registry.expected). The processor
/// variables follow @p environment, which describes this host; the other
/// system variables are created only when absent, so guest edits persist.
void publish_machine_identity(capsule::Registry& registry, const std::string& computer_name,
                              const EnvironmentManager& environment);

} // namespace vitreo::winapi
