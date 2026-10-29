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

/// @file fire_tls_callbacks.cpp
/// @brief ModuleManager::fire_tls_callbacks
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

void ModuleManager::fire_tls_callbacks(GuestAddr image_base,
                                       u32 bitness,
                                       GuestAddr tls_dir_va,
                                       u64 reason,
                                       const char* dbg_path) {
    if (!m_guest_call || !m_memory || tls_dir_va == 0) return;

    // IMAGE_TLS_DIRECTORY{32,64} layout: StartAddressOfRawData,
    // EndAddressOfRawData, AddressOfIndex, AddressOfCallbacks.
    // 32-bit: callbacks pointer @ +12; 64-bit: callbacks pointer @ +24.
    u64 callbacks_va = 0;
    if (bitness == 64) {
        auto r = m_memory->read_u64(tls_dir_va + 24);
        if (r.is_err()) return;
        callbacks_va = r.value();
    } else {
        auto r = m_memory->read_u32(tls_dir_va + 12);
        if (r.is_err()) return;
        callbacks_va = r.value();
    }
    if (callbacks_va == 0) return;

    constexpr usize MAX_TLS_CALLBACKS = 64;
    for (usize i = 0; i < MAX_TLS_CALLBACKS; ++i) {
        const GuestAddr entry_va = callbacks_va + i * (bitness == 64 ? 8 : 4);
        GuestAddr cb = 0;
        if (bitness == 64) {
            const auto callback = m_memory->read_u64(entry_va);
            if (callback.is_err()) break;
            cb = callback.value();
        } else {
            const auto callback = m_memory->read_u32(entry_va);
            if (callback.is_err()) break;
            cb = callback.value();
        }
        if (cb == 0) break;

        if (module_diag_enabled()) {
            std::array<u8, 16> bytes{};
            const auto read = m_memory->read_bytes(
                cb, bytes.data(), bytes.size());
            std::fprintf(stderr,
                         "[loader] TLS callback [%zu]=0x%llx bytes=%s",
                         i, static_cast<unsigned long long>(cb),
                         read.is_ok() ? "" : "<unreadable>");
            if (read.is_ok()) {
                for (u8 byte : bytes) {
                    std::fprintf(stderr, "%02x", byte);
                }
            }
            std::fputc('\n', stderr);
        }

        u64 args[3] = { static_cast<u64>(image_base), reason, 0 };
        VITREO_TRACE("[module] TLS callback %zu @ 0x%llx for \"%s\"\n",
                     i, static_cast<unsigned long long>(cb),
                     dbg_path ? dbg_path : "<unknown>");
        (void)m_guest_call(cb, args, 3, bitness == 64);
    }
}

} // namespace vitreo::winapi
