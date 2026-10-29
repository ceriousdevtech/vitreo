// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file kernel32_ntdll_forwarded.hpp
/// @brief kernel32 exports that Windows forwards to ntdll and that no other
///        kernel32 family owns (kernel32_ntdll_forwarders.inc lists them all).
///
/// register_core_dlls copies each one into ntdll under its forwarder name,
/// so both names run one implementation. Contracts:
/// tools/oracle_probe/k32_ntdll_forwarder_probe.c and
/// tools/oracle_probe/vectored_continue_probe.c (x64 and x86).
///
/// The undocumented and thinly documented ones are measured by
/// tools/oracle_probe/k32_ntdll_misc_forwarder_probe.c (LogUnexpectedCodepath,
/// __misaligned_access, __chkstk, _local_unwind) and
/// tools/oracle_probe/delayload_probe.c (ResolveDelayLoadedAPI,
/// ResolveDelayLoadsFromDll). _local_unwind is RtlUnwind's entry with its
/// last two arguments forced to NULL, so register_core_dlls builds it from
/// kernel32's RtlUnwind.

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Kernel32ExtManager;
class ModuleManager;

/// The exports for a guest of the given bitness: names that exist in only
/// one kernel32 (InterlockedCompareExchange64 on x86, RtlIsEcCode and
/// __C_specific_handler on x64) are absent from the other.
/// @p modules loads and resolves delay imports; @p k32 supplies the guest-call
/// hook the delay-load failure hooks run through.
[[nodiscard]] DllExports make_kernel32_ntdll_forwarded_exports(bool guest_is_64bit,
                                                               ModuleManager& modules,
                                                               Kernel32ExtManager& k32);

}  // namespace vitreo::winapi
