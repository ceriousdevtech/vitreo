// SPDX-License-Identifier: LGPL-2.1-only
/// @file wsock32.cpp
/// @brief Implementation of WSOCK32.dll API stubs

#include "wsock32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Wsock32Manager Implementation
// =============================================================================

u32 Wsock32Manager::wsa_startup(Marshaler& m, u16 version_requested,
                                 GuestAddr data_ptr) {
    if (data_ptr) {
        // Zero 400 bytes of WSADATA
        std::vector<u8> zeroes(400, 0);
        (void)m.memory().write_bytes(data_ptr,
                                     reinterpret_cast<const Byte*>(zeroes.data()), 400);
        // wVersion (offset 0, u16)
        (void)m.write_u16(data_ptr, version_requested);
        // wHighVersion (offset 2, u16)
        (void)m.write_u16(data_ptr + 2, 0x0202);
    }
    return 0; // success
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_WSAStartup(Marshaler& m, Wsock32Manager& mgr) {
    u16 version = static_cast<u16>(m.arg(0));
    GuestAddr data_ptr = m.arg_ptr(1);
    u32 rc = mgr.wsa_startup(m, version, data_ptr);
    m.set_return(rc);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_wsock32_thunks(Wsock32Manager& mgr) {
    DllExports exports;
    exports.entries.reserve(4);

    // WSAStartup ordinal: automation_export_ordinals_probe.{x86,x64}.expected
    exports.entries.push_back(ExportEntry{
        "WSAStartup", 115,
        [&mgr](Marshaler& m) { impl_WSAStartup(m, mgr); },
        8
    });

    // int WSACancelBlockingCall(void)
    //
    // A Winsock 1.1 leftover: it cancelled the one blocking call the calling
    // thread had outstanding under the old pseudo-blocking hook model.
    // Vitreo's sockets block in the host kernel, so there is never a
    // Vitreo-managed blocking call to cancel — which is also the case on
    // Windows whenever no such call is in progress, and WSAEINVAL is what it
    // reports then. (Winsock 2 removed the function entirely for the same
    // reason.)
    exports.entries.push_back(ExportEntry{
        "WSACancelBlockingCall", 113,
        [](Marshaler& m) {
            constexpr u32 WSAEINVAL_VALUE = 10022;
            set_last_error(WSAEINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);  // SOCKET_ERROR
        },
        0
    });

    // Note: the process-facing wsock32 table is create_wsock32_thunks(Ws2Manager&)
    // in ws2_32_registration.cpp (AcceptEx @1141, GetAcceptExSockaddrs @1142).
    // This Wsock32Manager factory exists for focused unit tests of WSAStartup.

    return exports;
}

} // namespace vitreo::winapi
