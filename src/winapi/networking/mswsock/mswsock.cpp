/// @file mswsock.cpp
/// @brief Implementation of mswsock.dll exports

#include "mswsock.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/networking/ws2_32/ws2_32.hpp"

namespace vitreo::winapi {

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

static void impl_GetSocketErrorMessageW(Marshaler& m,
                                         MswsockManager& /*mgr*/) {
    VITREO_TRACE("[mswsock] GetSocketErrorMessageW stub → 0\n");
    m.set_return(0);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_mswsock_thunks(MswsockManager& mgr, Ws2Manager& ws2) {
    DllExports exports;
    exports.entries.reserve(3);

    // GetSocketErrorMessageW (stdcall, stdcall_bytes=0 for stub)
    exports.entries.push_back(ExportEntry{
        "GetSocketErrorMessageW", 1,
        [&mgr](Marshaler& m) { impl_GetSocketErrorMessageW(m, mgr); },
        0
    });

    // AcceptEx / GetAcceptExSockaddrs — same bodies as ws2_32. Callers
    // delay-load these from mswsock by name; WSAIoctl returns the ws2_32
    // copies. Sharing the implementation keeps both paths consistent.
    exports.entries.push_back(ExportEntry{
        "AcceptEx", 0,
        [&ws2](Marshaler& m) { ws2_AcceptEx(m, ws2); },
        32
    });
    exports.entries.push_back(ExportEntry{
        "GetAcceptExSockaddrs", 0,
        [&ws2](Marshaler& m) { ws2_GetAcceptExSockaddrs(m, ws2); },
        32
    });

    return exports;
}

} // namespace vitreo::winapi
