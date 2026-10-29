// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file os_version.hpp
/// @brief The Windows version Vitreo presents, in one place.
///
/// Windows 10 version 2004 (10.0.19041), the build Vitreo's synthetic system
/// modules carry in their version resources. Every source that reports the
/// running OS gives these numbers, as on Windows
/// (tools/oracle_probe/os_version_sources_probe.expected): RtlGetVersion,
/// RtlGetNtVersionNumbers, KUSER_SHARED_DATA, the PEB's OS fields and the
/// CurrentVersion registry key. Only GetVersionEx reports less to a process
/// whose manifest does not declare Windows 10.

#include <vitreo/types.hpp>

namespace vitreo::pe {

inline constexpr u32 kOsMajorVersion = 10;
inline constexpr u32 kOsMinorVersion = 0;
inline constexpr u32 kOsBuildNumber = 19041;
inline constexpr u32 kOsPlatformId = 2;  // VER_PLATFORM_WIN32_NT
// OSVERSIONINFOEX extras of a Windows client: no service pack,
// VER_SUITE_SINGLEUSERTS, VER_NT_WORKSTATION (verify_version_info_probe:
// real_sp=0.0 real_suite=0x100 real_product=1).
inline constexpr u16 kOsServicePackMajor = 0;
inline constexpr u16 kOsServicePackMinor = 0;
inline constexpr u16 kOsSuiteMask = 0x0100;
inline constexpr u8 kOsProductType = 1;

} // namespace vitreo::pe
