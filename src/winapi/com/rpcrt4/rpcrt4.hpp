#pragma once

/// @file rpcrt4.hpp
/// @brief rpcrt4.dll UUID APIs.
///
/// Only the UUID generation/formatting surface is implemented. Callers mint
/// correlation identifiers with UuidCreate and UuidToStringA before entering
/// their main loop. A missing-export stub returns 0xFFFFFFFF (a nonzero
/// RPC_STATUS) without writing the out-parameters and pops 0 stdcall bytes.

#include <vitreo/common.hpp>
#include "winapi/com/rpcrt4/ndr_proxy.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <array>

namespace vitreo::winapi {

class GlobalLocalManager;
class ComTaskMemoryManager;

// RPC_STATUS values (winerror.h). RPC_S_OK is plain success.
namespace rpc_status {
constexpr u32 RPC_S_OK              = 0;
constexpr u32 RPC_S_INVALID_ARG     = 87;    // ERROR_INVALID_PARAMETER
constexpr u32 RPC_S_OUT_OF_MEMORY   = 14;    // ERROR_OUTOFMEMORY
constexpr u32 RPC_S_INVALID_STRING_UUID = 1705;
}

/// Size of a UUID/GUID as the guest sees it: DWORD + WORD + WORD + BYTE[8].
constexpr u32 kUuidSize = 16;

/// Length of the canonical hyphenated UUID string, excluding the NUL.
/// RPC's UuidToString* emits lowercase and *no* enclosing braces.
constexpr u32 kUuidStringLen = 36;

/// Generate UUID bytes in the guest GUID structure's in-memory order.
///
/// Shared by UuidCreate and CoCreateGuid so both APIs use one process-wide
/// entropy source and preserve the same version/variant contract.
std::array<u8, kUuidSize> generate_uuid_v4();

/// Create rpcrt4.dll export thunks.
/// @param heap    Backs UuidToString* allocations so RpcStringFree* can release
///                them; the pair must share one allocator for the free to be real.
/// @param task_memory Backs NdrOleAllocate/NdrOleFree (CoTaskMemAlloc wrappers).
/// @param proxies Serves the NdrDll* proxy/stub self-registration entry points.
DllExports create_rpcrt4_thunks(GlobalLocalManager& heap,
                                ComTaskMemoryManager& task_memory,
                                NdrProxyRegistrar& proxies);

} // namespace vitreo::winapi
