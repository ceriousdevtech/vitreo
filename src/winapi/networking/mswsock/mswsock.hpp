#pragma once

/// @file mswsock.hpp
/// @brief mswsock.dll API stubs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;
class Ws2Manager;

// =============================================================================
// MswsockManager
// =============================================================================

class MswsockManager {
public:
    MswsockManager() = default;
    ~MswsockManager() = default;

    MswsockManager(const MswsockManager&) = delete;
    MswsockManager& operator=(const MswsockManager&) = delete;
};

/// Create mswsock.dll export thunks.
///
/// AcceptEx / GetAcceptExSockaddrs share the ws2_32 implementation. Callers
/// delay-load them from mswsock by name while WSAIoctl retrieves them from
/// ws2_32.
DllExports create_mswsock_thunks(MswsockManager& mgr, Ws2Manager& ws2);

} // namespace vitreo::winapi
