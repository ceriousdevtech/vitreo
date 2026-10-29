#include "core/pe/imports/api_set_resolver.hpp"

#include <cctype>

namespace vitreo::pe {

std::string_view resolve_api_set(std::string_view dll_name) {
    // PE import tables keep the DLL's on-disk casing (VCRUNTIME140.dll,
    // API-MS-Win-CRT-Runtime-l1-1-0.dll). Match case-insensitively; on a miss
    // return the original view so companion-PE loads keep the IAT name.
    char lower[128];
    if (dll_name.size() >= sizeof(lower)) return dll_name;
    for (size_t i = 0; i < dll_name.size(); ++i) {
        lower[i] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(dll_name[i])));
    }
    const std::string_view name(lower, dll_name.size());

    // Microsoft Learn, "Windows API sets": only names beginning with api-
    // or ext- identify API-set contracts. Physical DLLs (including versioned
    // CRT redistributables) retain their own image, exports, globals and
    // initializer. A DLL containing a contract-looking substring is still
    // an ordinary file. Keep the canonical spelling of our direct UCRT host.
    if (name == "ucrtbase.dll") return "ucrtbase.dll";
    if (!name.starts_with("api-") && !name.starts_with("ext-")) return dll_name;

    // Exact, observed contracts take precedence over legacy family fallbacks.
    // Compare without the optional .dll suffix (API-set loader contract).
    static constexpr struct {
        std::string_view contract;
        std::string_view host;
    } observed_hosts[] = {
#include "api_set_observed_hosts.inc"
    };
    const auto contract = name.ends_with(".dll") ? name.substr(0, name.size() - 4) : name;
    for (const auto& route : observed_hosts) {
        if (contract == route.contract.substr(0, route.contract.size() - 4))
            return route.host;
    }

    // UCRT API sets retain their UCRT identity. FILE is opaque in UCRT and is
    // not layout-compatible with legacy MSVCRT's public struct _iobuf, so
    // collapsing these contracts loses guest-visible ABI provenance.
    if (name.find("api-ms-win-crt-") != std::string_view::npos) {
        return "ucrtbase.dll";
    }

    // vcruntime140*.dll is NOT aliased. It ships in the Visual C++
    // redistributable (installed to System32 or app-local), not with Windows,
    // so it is a real guest module found by the DLL search order. Serving it
    // from msvcrt.dll dropped 36 of vcruntime140's 71 exports and all 3 of
    // vcruntime140_1's (__CxxFrameHandler4), so every x64 C++ exception
    // through FH4 code hit a missing-export stub.

    if (name.find("api-ms-win-shcore-") != std::string_view::npos) {
        return "shcore.dll";
    }

    // Downlevel Shell32 contracts expose shell APIs such as
    // CommandLineToArgvW. Keep this ahead of the generic api-ms-win rule so
    // runtime LoadLibrary/GetProcAddress probes resolve against shell32.
    if (name.find("api-ms-win-downlevel-shell32-") != std::string_view::npos) {
        return "shell32.dll";
    }

    // SystemFunction036 (RtlGenRandom) is exported by advapi32.  This
    // contract must be routed before the generic api-ms-win fallback or
    // runtime LoadLibrary/GetProcAddress probes look for it in kernel32.
    if (name.find("api-ms-win-security-systemfunctions-") !=
        std::string_view::npos) {
        return "advapi32.dll";
    }

    // DeriveCapabilitySidsFromName and related security-base contracts live
    // in KernelBase on Windows; Vitreo implements them on advapi32. Route
    // before the generic api-ms-win → kernel32 fallback so a sandboxed
    // GetModuleHandle/GetProcAddress probes resolve the real
    // export.
    if (name.find("api-ms-win-security-base-") !=
        std::string_view::npos) {
        return "advapi32.dll";
    }

    // Service Control Manager contracts are hosted by sechost.dll on
    // Windows; Vitreo implements the SCM on advapi32 (which re-exports
    // sechost). api-ms-win-core-registry-l2 (RegCreateKeyTransactedW and
    // the other advapi32-only registry calls) resolves to advapi32.dll.
    if (name.find("api-ms-win-service-") != std::string_view::npos ||
        name.find("api-ms-win-core-registry-l2-") != std::string_view::npos) {
        return "advapi32.dll";
    }

    // ETW contracts. On Windows api-ms-win-eventing-provider-l1-1-0 forwards
    // to advapi32/ntdll; Vitreo implements EventRegister and friends on
    // advapi32. Without this the generic api-ms-win rule sends the contract
    // to kernel32, LoadLibrary succeeds with the kernel32 handle, and the
    // subsequent GetProcAddress("EventRegister") returns NULL. A managed
    // runtime binds that NULL as a P/Invoke target and then calls it, killing
    // the guest with STATUS_ILLEGAL_INSTRUCTION.
    if (name.find("api-ms-win-eventing-") != std::string_view::npos ||
        name.find("api-ms-win-eventlog-") != std::string_view::npos) {
        return "advapi32.dll";
    }

    // PowerBase contracts expose CallNtPowerInformation from PowrProf.dll.
    // Power-setting contracts (PowerGetActiveScheme, PowerReadACValue,
    // PowerReadDCValue) are the same DLL. Keep both ahead of the generic
    // api-ms-win fallback so delay-load GetProcAddress probes do not
    // incorrectly search kernel32.
    if (name.find("api-ms-win-power-base-") != std::string_view::npos ||
        name.find("api-ms-win-power-setting-") != std::string_view::npos) {
        return "powrprof.dll";
    }

    // Multimedia timer contracts (timeGetTime / timeBeginPeriod) live in
    // winmm.dll. The generic api-ms-win rule would incorrectly send them to
    // kernel32, so delay-load probes would see ERROR_PROC_NOT_FOUND.
    if (name.find("api-ms-win-mm-time-") != std::string_view::npos) {
        return "winmm.dll";
    }

    // GetSystemMetrics / SystemParametersInfoW live on user32. The generic
    // api-ms-win / ext-ms- fallback would send this contract to kernel32,
    // where the exports become fail-thunks that return 0xFFFFFFFF and turn
    // metric-sized allocations into std::bad_alloc.
    if (name.find("api-ms-win-ntuser-") != std::string_view::npos ||
        name.find("ext-ms-win-ntuser-") != std::string_view::npos) {
        return "user32.dll";
    }

    // D3DKMT ordinal contracts (ext-ms-win-dx-d3dkmt-dxcore-l1-1-*). Keep the
    // import identity so ordinals resolve against the registered API-set
    // modules, not kernel32 (gdi32 only re-exports them by name under
    // different ordinals).
    if (name.find("ext-ms-win-dx-d3dkmt-") != std::string_view::npos) {
        return dll_name;
    }

    if (name.find("api-ms-win-core-") != std::string_view::npos ||
        name.find("api-ms-win-") != std::string_view::npos) {
        // WinRT API sets map to combase.dll (stub implementations)
        if (name.find("winrt") != std::string_view::npos) {
            return "combase.dll";
        }
        if (name.find("rtlsupport") != std::string_view::npos) {
            return "ntdll.dll";
        }
        return "kernel32.dll";
    }

    if (name.find("ext-ms-") != std::string_view::npos) {
        return "kernel32.dll";
    }

    return dll_name;
}

} // namespace vitreo::pe
