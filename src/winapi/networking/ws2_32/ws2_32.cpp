/// @file ws2_32.cpp
/// @brief Remaining thin Winsock thunks (error getters and WSASocketA).

#include "ws2_32.hpp"
#include "winapi/networking/ws2_32/ws2_32_internal.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

void ws2::impl_WSAGetLastError(Marshaler& m, Ws2Manager& mgr) {
    // WSAGetLastError() → stored last error
    u32 err = mgr.last_error();
    VITREO_TRACE( "[ws2_32] WSAGetLastError() -> %u\n", err);
    m.set_return(static_cast<u64>(err));
}

void ws2::impl_WSASetLastError(Marshaler& m, Ws2Manager& mgr) {
    // WSASetLastError(error) → void
    u32 err = static_cast<u32>(m.arg(0));
    VITREO_TRACE( "[ws2_32] WSASetLastError(%u)\n", err);
    mgr.set_last_error(err);
    m.set_return(0);
}

void ws2::impl_WSASocketA(Marshaler& m, Ws2Manager& mgr) {
    // Same contract as WSASocketW (ANSI vs Unicode only affects lpProtocolInfo
    // string fields we do not read for FROM_PROTOCOL_INFO).
    impl_WSASocketW(m, mgr);
}

} // namespace vitreo::winapi
