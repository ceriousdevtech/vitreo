/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

#include <vitreo/diagnostics.hpp>

/// @file reattach_detached_module.cpp
/// @brief ModuleManager::reattach_detached_module
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

bool ModuleManager::reattach_detached_module(ModuleInfo& info) {
    // Only a module whose last reference was released needs anything here.
    // A missing entry point does not exempt it: Windows still unmaps and
    // re-maps an entry-point-less DLL, so the image must be refreshed even
    // when there is no DllMain to call afterwards.
    if (info.ref_count != 0 || !info.is_pe_backed()) {
        return true;
    }

    // Windows unmaps a DLL when FreeLibrary takes its reference count to zero,
    // so the LoadLibrary that takes it back to one maps a *fresh* image: every
    // writable section holds its on-disk initial values again and the guest CRT
    // initializes from scratch. Re-running DLL_PROCESS_ATTACH over the old
    // image instead hands the new attach whatever the previous
    // DLL_PROCESS_DETACH left behind. The CRT freed and zeroed its lowio
    // __pioinfo table during detach, and the re-attach then dereferenced
    // that NULL table inside initialize_stdio_handles_nolock.
    //
    // Re-map from disk at the same base before attaching. Keeping the base is
    // what lets any HMODULE the guest already cached stay meaningful, and the
    // range is free because it was just unmapped.
    //
    // A module registered without going through the PE loader (a synthetic
    // module, or one registered directly) has no section descriptors and no
    // file to re-read, so it cannot be re-mapped. Those keep the older
    // approximation of attaching over the existing image: it is not what
    // Windows does, but it is strictly better than failing a load whose image
    // is still perfectly valid.
    const bool can_remap =
        (m_memory != nullptr && info.mapped_image_template != nullptr) ||
        (m_pe_loader != nullptr && m_vfs != nullptr && m_memory != nullptr &&
         !info.mapped_sections.empty() && !info.path.empty());
    if (can_remap) {
        if (!remap_detached_image(info)) {
            fprintf(stderr,
                    "[module] re-attach ABORTED for \"%s\": could not re-map a "
                    "clean image\n",
                    info.name.c_str());
            return false;
        }
    } else if (module_diag_enabled()) {
        fprintf(stderr,
                "[module] re-attaching \"%s\" over its EXISTING image (no "
                "mapping info to re-map); DLL_PROCESS_ATTACH will see state "
                "left by the previous DLL_PROCESS_DETACH\n",
                info.name.c_str());
    }

    if (!publish_loader_entry(info)) {
        return false;
    }

    // A DLL with no entry point has no DllMain to notify; the refreshed image
    // above is the whole of its reload.
    if (!m_guest_call || info.entry_point == 0) {
        return true;
    }

    constexpr u64 DLL_PROCESS_ATTACH = 1;
    u64 args[3] = {
        static_cast<u64>(info.base_address),
        DLL_PROCESS_ATTACH,
        0,
    };
    if (module_diag_enabled()) {
        fprintf(stderr,
                "[module] re-attaching \"%s\" @ 0x%llx (%s)\n",
                info.name.c_str(),
                static_cast<unsigned long long>(info.base_address),
                can_remap ? "re-mapped clean image" : "existing image");
    }
    const u64 rc = m_guest_call(info.entry_point, args, 3, info.bitness == 64);
    if (rc == 0) {
        fprintf(stderr, "[module] re-attach DllMain FAILED for \"%s\"\n",
                info.name.c_str());
        unpublish_loader_entry(info);
        return false;
    }
    info.dllmain_called = true;
    return true;
}

} // namespace vitreo::winapi
