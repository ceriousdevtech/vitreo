// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_io_ext.cpp
/// @brief Overlapped I/O and completion ports.
///
/// FlushFileBuffers, the overlapped read/write forms, CreateIoCompletionPort,
/// GetQueuedCompletionStatus and the notification-mode controls. file.cpp owns
/// synchronous file access; this is the asynchronous surface layered over it.
///
/// Its one tie back to kernel32_ext.cpp is iocp_diag_log — the VITREO_IOCP_DIAG
/// tracer, which timestamps each line and locks stderr so completions arriving
/// on several threads stay readable. Declared in kernel32_ext_internal.hpp.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "ipc/process_handle.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

} // namespace vitreo::winapi
