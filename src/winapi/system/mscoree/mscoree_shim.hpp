// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
/// @file mscoree_shim.hpp
/// @brief mscoree.dll, the operating system's .NET runtime shim.
///
/// Since Windows 8, mscoree.dll is an OS component, not part of any .NET
/// Framework. It does no runtime selection of its own: it locates the shim
/// IMPLEMENTATION, mscoreei.dll, which the .NET Framework 4 installer puts in
/// the Framework directory, hands it two callbacks, and forwards every export
/// to mscoreei's export of the same name. Runtime selection
/// (<supportedRuntime>, useLegacyV2RuntimeActivationPolicy, the image's
/// metadata version), ICLRMetaHost and the legacy CorBindTo* APIs all live in
/// mscoreei.dll, which is Microsoft's own code running in the capsule.
///
/// Measured on the oracle (tests/oracle_data/cor_shim_locate_probe{32,64}
/// .expected, and the in-box DLL's export table and code):
///  - mscoreei.dll is <InstallRoot>v4.0.<build>\mscoreei.dll. InstallRoot is
///    the COMPlus_InstallRoot environment variable when set, else the
///    registry value, which on Windows is %windir%\Microsoft.NET\Framework\
///    in the 32-bit view and ...\Framework64\ in the 64-bit view. A
///    v4.0.99999 directory next to v4.0.30319 is not picked: the build comes
///    from HKLM\SOFTWARE\Microsoft\.NETFramework\Policy\v4.0 (value names
///    are build numbers). COMPlus_Version does not move it.
///  - The handshake: RegisterShimImplCallback(cleanup, runtime_string) when
///    mscoreei exports it, else RegisterShimImplCleanupCallback(cleanup);
///    with neither the shim frees mscoreei and fails. SetShellShimInstance
///    (absent from the 4.0 mscoreei) receives the shim's HINSTANCE.
///  - Each export forwards to mscoreei's same-named export; ordinal 24
///    (unnamed) to mscoreei's ordinal 24. When mscoreei is missing, failed
///    to bind, has run its cleanup callback, or lacks the export, the
///    export returns CLR_E_SHIM_RUNTIMEEXPORT (0x80131701).
///  - Ordinal 142 (unnamed) is `jmp [__imp_GetVersionExW]`.
///  - _CorImageUnloading does nothing (`ret`); _CorValidateImage is the
///    shim's own code (Learn: _CorValidateImage).
///
/// The shim takes the per-view InstallRoot Windows ships (above) for the
/// calling process's bitness, after the COMPlus_InstallRoot override; the
/// capsule's OS defaults seed the same value in each registry view for
/// mscoreei.dll and clr.dll, which read it themselves.

#pragma once

#include <vitreo/types.hpp>

#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::mscoree {

/// corerror.h: CLR_E_SHIM_RUNTIMEEXPORT, "Failed to find a required export
/// in the runtime" -- what every forwarding export returns without one.
inline constexpr u32 CLR_E_SHIM_RUNTIMEEXPORT = 0x80131701u;
/// winerror.h E_FAIL (Learn _CorValidateImage lists it as a return value).
inline constexpr u32 E_FAIL_HR = 0x80004005u;

/// One export of the in-box mscoree.dll (mscoree_exports.inc).
struct ShimExport {
    u16 ordinal;
    const char* name;        ///< "" for an ordinal-only export
    u16 x86_stdcall_bytes;   ///< what the x86 export's RET pops
};

/// The in-box export table for one bitness, in export-directory order.
[[nodiscard]] std::span<const ShimExport> shim_exports(bool is_64bit);

/// What the shim does with one export (from the in-box DLL's code).
enum class ExportRole : u8 {
    ForwardByName,      ///< mscoreei's same-named export
    ForwardByOrdinal,   ///< mscoreei's export with the same ordinal (24)
    GetVersionExW,      ///< ordinal 142: kernel32!GetVersionExW
    ImageUnloading,     ///< _CorImageUnloading: returns
    ValidateImage,      ///< _CorValidateImage: the shim's own code
};
[[nodiscard]] ExportRole export_role(const ShimExport& e);

/// The guest-process operations the shim performs. Each is a call of the
/// guest-visible kernel32 export the in-box shim imports (or a capsule
/// registry read), so the side effects -- DllMain, the loader's search,
/// last-error -- are the real ones.
class ShimHost {
public:
    virtual ~ShimHost() = default;
    [[nodiscard]] virtual bool is_64bit() const = 0;
    /// GetEnvironmentVariableW; nullopt when unset.
    [[nodiscard]] virtual std::optional<std::u16string> environment_variable(std::u16string_view name) = 0;
    /// GetWindowsDirectoryW.
    [[nodiscard]] virtual std::u16string windows_directory() = 0;
    /// Value names under HKLM\SOFTWARE\Microsoft\.NETFramework\Policy\v4.0.
    [[nodiscard]] virtual std::vector<std::u16string> policy_v4_builds() = 0;
    /// LoadLibraryW(path); 0 on failure.
    [[nodiscard]] virtual u64 load_library(const std::u16string& path) = 0;
    virtual void free_library(u64 module) = 0;
    [[nodiscard]] virtual u64 get_proc_address(u64 module, const char* name) = 0;
    [[nodiscard]] virtual u64 get_proc_address_ordinal(u64 module, u16 ordinal) = 0;
    /// Call a guest stdcall function.
    virtual u64 call(u64 fn, const std::vector<u64>& args) = 0;
    /// The shim's own HINSTANCE (GetModuleHandleW(L"mscoree.dll")).
    [[nodiscard]] virtual u64 shim_instance() = 0;
};

/// <InstallRoot>v4.0.<build>\mscoreei.dll, or nullopt when Policy\v4.0
/// names no build.
[[nodiscard]] std::optional<std::u16string> implementation_path(ShimHost& host);

/// The shim's binding to mscoreei.dll; one per process.
class MscoreeShim {
public:
    /// @p cleanup_cb / @p runtime_string_cb: guest addresses of the two
    /// callbacks the shim registers (void WINAPI(void), void WINAPI(LPCWSTR)).
    MscoreeShim(u64 cleanup_cb, u64 runtime_string_cb)
        : cleanup_cb_(cleanup_cb), runtime_string_cb_(runtime_string_cb) {}

    /// The guest address @p e forwards to, binding mscoreei.dll on first
    /// use; 0 when the export must fail with CLR_E_SHIM_RUNTIMEEXPORT.
    [[nodiscard]] u64 forward_target(ShimHost& host, const ShimExport& e);

    /// The cleanup callback: mscoreei is going away; every later forward fails.
    void on_cleanup();
    /// The runtime-string callback (the in-box shim keeps a copy).
    void on_runtime_string(std::u16string value);

    enum class State : u8 { Unbound, Bound, Failed, CleanedUp };
    [[nodiscard]] State state() const;

private:
    void bind(ShimHost& host);

    mutable std::recursive_mutex mutex_;
    State state_{State::Unbound};
    u64 impl_{0};
    u64 cleanup_cb_;
    u64 runtime_string_cb_;
    std::u16string runtime_string_;
    std::unordered_map<u16, u64> targets_;  // by export ordinal
};

}  // namespace vitreo::winapi::mscoree
