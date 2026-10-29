// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_core_thunks_internal.hpp
/// @brief The ordered slices of create_com_core_thunks(). Each appends its
/// ole32 thunk entries to `exports`; the driver in com_core_thunks.cpp calls
/// them in the original registration order, so entry order is unchanged.

#include "com_core.hpp"

namespace vitreo::winapi {

/// CoInitialize through CoCreateInstance: apartment lifecycle and class
/// creation.
void append_com_lifecycle_thunks(DllExports& exports, ComCoreManager& mgr);

/// Remote-proxy IUnknown/IDispatch methods, IServerSecurity and the
/// Vitreo_RemoteProxy_Invoke_N slots.
void append_com_remote_proxy_thunks(DllExports& exports, ComCoreManager& mgr);

/// ShellLink and IGlobalOptions vtable methods.
void append_com_object_vtable_thunks(DllExports& exports, ComCoreManager& mgr);

/// ole32 completions: FTM, agile references, PROPVARIANT copy, HGLOBAL
/// streams, object context, IComThreadingInfo.
void append_com_completion_thunks(DllExports& exports, ComCoreManager& mgr);

/// INetworkListManager, INetworkConnection and INetwork vtable methods.
void append_com_network_thunks(DllExports& exports, ComCoreManager& mgr);

/// MMDevice enumerator, endpoint and audio-client vtable methods plus the
/// WASAPI service exports.
void append_com_audio_thunks(DllExports& exports, ComCoreManager& mgr);

/// IShellLink and IPersistFile vtable methods.
void append_com_shelllink_thunks(DllExports& exports, ComCoreManager& mgr);

/// SHCreateMemStream IStream vtable methods.
void append_com_memory_stream_thunks(DllExports& exports, ComCoreManager& mgr);

/// CoGetMalloc and the IMalloc vtable, task memory, OLE init, CoCreateGuid
/// and library loading.
void append_com_runtime_thunks(DllExports& exports, ComCoreManager& mgr);

/// CLSIDFromString, IIDFromString, StringFromGUID2, StringFromCLSID and
/// StringFromIID.
void append_com_guid_string_thunks(DllExports& exports, ComCoreManager& mgr);

/// Additional OLE32 exports: ROT, monikers, ProgID lookup, class-object
/// registration, marshal helpers, security and PropVariantClear.
void append_com_ole_stub_thunks(DllExports& exports, ComCoreManager& mgr);

} // namespace vitreo::winapi
