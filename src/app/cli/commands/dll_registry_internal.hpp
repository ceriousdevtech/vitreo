// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Private surface shared by the dll_registry*.cpp parts

#pragma once

/// @file dll_registry_internal.hpp
/// @brief Shared surface between dll_registry.cpp and the parts split out of it.
///
/// Private to the DLL registry. These were file-local helpers of
/// dll_registry.cpp, and the `register_*_dlls` parts were its static
/// functions; this header exists so the parts can still see each other, not
/// to publish them. The public entry point stays `register_all_dlls` in
/// dll_registry.hpp.

#include <vitreo/common.hpp>

#include <cstdio>
#include <string>
#include <unordered_set>

#include "runtime/process/runtime_context.hpp"

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cli {

/// Reserve a Vitreo-internal scratch region.
///
/// The fixed bases these DLLs prefer live in ordinary guest address space, and
/// a guest can claim them first — SafeDisc's unpacker reserves 0x5E000000+1MB
/// before Vitreo registers opengl32/glu32/crypt32, which silently left their
/// scratch unmapped because every call site discarded the result. Fall back to
/// any free range and report the address actually used.
[[nodiscard]] inline vitreo::GuestAddr reserve_scratch_region(
    auto& vmm, const char* name, vitreo::GuestAddr preferred, vitreo::u32 size) {
    constexpr vitreo::u32 kCommitReserve = 0x3000;  // MEM_COMMIT | MEM_RESERVE
    constexpr vitreo::u32 kReadWrite     = 0x04;    // PAGE_READWRITE
    if (vitreo::GuestAddr got = vmm.virtual_alloc(preferred, size,
                                                  kCommitReserve, kReadWrite);
        got != 0) {
        return got;
    }
    // Preferred range is taken by the guest: let the allocator choose.
    const vitreo::GuestAddr relocated =
        vmm.virtual_alloc(0, size, kCommitReserve, kReadWrite);
    if (relocated == 0) {
        std::fprintf(stderr,
                     "[dll-registry] %s scratch reservation FAILED (%u bytes); "
                     "that DLL's guest-visible buffers are unmapped\n",
                     name, size);
        return 0;
    }
    VITREO_TRACE("[dll-registry] %s scratch relocated 0x%llx -> 0x%llx "
                 "(guest holds the preferred range)\n",
                 name, static_cast<unsigned long long>(preferred),
                 static_cast<unsigned long long>(relocated));
    return relocated;
}

/// Append all entries from @p src into @p dst
inline void merge_exports(winapi::DllExports& dst, winapi::DllExports src) {
    for (auto& e : src.entries) {
        dst.entries.push_back(std::move(e));
    }
}

/// Give KernelBase every entry of @p src that the real KernelBase exports.
/// Defined in dll_registry_core.cpp, where the contract is described.
void collect_kernelbase_surface(winapi::DllExports& kernelbase,
                                const winapi::DllExports& src,
                                std::unordered_set<std::string>& seen);

// The DLL registration parts, grouped by the file that defines them.
// register_all_dlls (dll_registry.cpp) runs them in its own order.

// dll_registry_core.cpp
void register_core_dlls(RuntimeContext& ctx, winapi::DllExports& kernelbase,
                        std::unordered_set<std::string>& kernelbase_seen,
                        bool guest_is_64bit);

// dll_registry_crt.cpp
void register_crt_dlls(RuntimeContext& ctx, bool guest_is_64bit);
/// mscoree.dll, the OS .NET runtime shim (dll_registry_mscoree.cpp).
void register_mscoree_dll(RuntimeContext& ctx, bool guest_is_64bit);

// dll_registry_advapi_ui.cpp
void register_advapi_rpc_dlls(RuntimeContext& ctx,
                              winapi::DllExports& kernelbase,
                              std::unordered_set<std::string>& kernelbase_seen);
void register_ui_core_dlls(RuntimeContext& ctx, bool guest_is_64bit,
                           winapi::DllExports& kernelbase,
                           std::unordered_set<std::string>& kernelbase_seen);

// dll_registry_shell_ole.cpp
void register_shell_ole_dlls(RuntimeContext& ctx,
                             winapi::DllExports& kernelbase,
                             std::unordered_set<std::string>& kernelbase_seen);
void register_shell_support_dlls(RuntimeContext& ctx);

// dll_registry_ui_controls.cpp
void register_ui_controls_dlls(RuntimeContext& ctx, bool guest_is_64bit);
void register_media_foundation_dcomp_dlls(RuntimeContext& ctx);

// dll_registry_winsock_crypto.cpp
void register_winsock_oleaut_crypto_dlls(RuntimeContext& ctx);

// dll_registry_input_legacy_graphics.cpp
void register_input_and_legacy_graphics_dlls(RuntimeContext& ctx,
                                             GuestMemory& memory);

// dll_registry_system_services.cpp
void register_system_services_dlls(RuntimeContext& ctx);
void register_printing_network_power_dlls(RuntimeContext& ctx);

// dll_registry_dwrite.cpp
void register_dwrite_dlls(RuntimeContext& ctx);

// dll_registry_trust_profile_tpm.cpp
void register_trust_profile_tpm_dlls(RuntimeContext& ctx);

// dll_registry_graphics_dxgi.cpp
void register_dxgi_d3d_dlls(RuntimeContext& ctx);

// dll_registry_perf_diag.cpp
void register_mmcss_qos_probe_dlls(RuntimeContext& ctx);
void register_perf_event_dlls(RuntimeContext& ctx);
void register_device_diagnostic_dlls(RuntimeContext& ctx, GuestMemory& memory);

// dll_registry_combase.cpp
void register_combase_dlls(RuntimeContext& ctx);

// dll_registry_system_programs.cpp
void register_system_program_dll(RuntimeContext& ctx);
/// This process's Windows Installer custom-action host and installer-service
/// connector (dll_registry_system_programs.cpp).
void install_msi_process_host(RuntimeContext& ctx, GuestMemory& memory, bool is_64bit);

} // namespace vitreo::cli
