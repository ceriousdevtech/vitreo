// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file setupapi.hpp
/// @brief SetupAPI device-information sets and the cfgmgr32 devnode tree.
///
/// One device tree backs both DLLs: the root devnode HTREE\ROOT\0 and, when
/// the host has a Metal device, Vitreo's root-enumerated display adapter
/// ROOT\VITREO_METAL\0000 with its two device interfaces. DEVINST handles
/// are process-wide and handed out in first-reference order, as cfgmgr32
/// does (s7_cfgmgr_probe: the first CM_Locate_DevNode(NULL) returns 1).
/// Behaviour follows tools/oracle_probe/s7_setupapi_probe.expected and
/// s7_cfgmgr_probe.expected (measured non-elevated, which is how guests run).

#include "winapi/core/thunk_registry.hpp"

#include <functional>
#include <string>

namespace vitreo { namespace capsule { class Vfs; } }

namespace vitreo::winapi {

class RegistryThunks;
class GuestElevationState;

namespace file_compression {
constexpr u32 NONE = 0;
constexpr u32 WINLZA = 1;
constexpr u32 MSZIP = 2;
constexpr u32 NTCAB = 3;
} // namespace file_compression

/// Public SP_COPY_* values from setupapi.h (Windows SDK).
namespace sp_copy {
constexpr u32 DELETESOURCE = 0x00000001u;
constexpr u32 REPLACEONLY = 0x00000002u;
constexpr u32 NOOVERWRITE = 0x00000008u;
constexpr u32 NODECOMP = 0x00000010u;
constexpr u32 SOURCE_ABSOLUTE = 0x00000040u;
constexpr u32 SOURCEPATH_ABSOLUTE = 0x00000080u;
constexpr u32 FORCE_NOOVERWRITE = 0x00001000u;
} // namespace sp_copy

/// SetupDecompressOrCopyFile: copy `source` to `target`, expanding SZDD when
/// needed. A null `compression_type` searches compressed-form names (last
/// character `_` then `$`) per
/// https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdecompressorcopyfilea
[[nodiscard]] u32 setup_decompress_or_copy_file(
    capsule::Vfs& vfs,
    const std::string& source_file_name,
    const std::string& target_file_name,
    const u32* compression_type);

/// SetupInstallFile (no-INF path): copy `source_file` to `destination_name`.
/// Without SP_COPY_NODECOMP, a directory dest receives the uncompressed form
/// of the source name (SZDD lastchar; otherwise the source leaf). With
/// SP_COPY_NODECOMP the dest leaf is the source leaf.
/// https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupinstallfilea
[[nodiscard]] u32 setup_install_file(
    capsule::Vfs& vfs,
    const std::string& source_file,
    const std::string& source_path_root,
    const std::string& destination_name,
    u32 copy_style);

/// What the INF install, file-queue and cabinet APIs need from the running
/// process, wired after registration (run_callbacks_runtime): calling the
/// guest's PSP_FILE_CALLBACK (WINAPI, 4 arguments) and a guest scratch region
/// for the notification records and strings that callback is handed.
struct SetupapiGuestServices {
    std::function<u64(GuestAddr function, const u64* args, usize count,
                      bool is_64bit)> call;
    std::function<GuestAddr(u32 size)> reserve;
};

/// Build the SetupAPI exports. The default display-name provider queries the
/// real process-wide Metal device; tests may inject a deterministic provider
/// without claiming host hardware (an empty name means "no display device").
/// `vfs` backs the file-copy APIs and SetupCopyOEMInf/SetupGetInfDriverStore-
/// Location path checks; `registry` backs the class/device registry keys;
/// `elevation` decides the administrator-only paths (device creation,
/// property writes, write access to device keys); `services` backs the
/// callbacks of SetupInstallFromInfSection, SetupCommitFileQueue and
/// SetupIterateCabinet (without it those fail rather than skip the callback).
[[nodiscard]] DllExports create_setupapi_thunks(
    std::function<std::string()> display_name_provider = {},
    capsule::Vfs* vfs = nullptr,
    RegistryThunks* registry = nullptr,
    const GuestElevationState* elevation = nullptr,
    const SetupapiGuestServices* services = nullptr);

/// cfgmgr32.dll: the CM_* devnode API over the same device tree. setupapi.dll
/// re-exports (forwards) the subset of these names that the real setupapi.dll
/// exports.
[[nodiscard]] DllExports create_cfgmgr32_thunks(
    std::function<std::string()> display_name_provider = {},
    RegistryThunks* registry = nullptr,
    const GuestElevationState* elevation = nullptr);

} // namespace vitreo::winapi
