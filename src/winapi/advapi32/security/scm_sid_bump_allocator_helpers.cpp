// SPDX-License-Identifier: LGPL-2.1-only

/// @file security.cpp
/// @brief advapi32 security stubs implementation (WIN32-062)

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"

#include <cstdio>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <stdlib.h> // arc4random_buf
#include <unistd.h>
#include <pwd.h>
#include <cerrno>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <set>
#include <variant>
#include <CommonCrypto/CommonDigest.h>

#ifdef __APPLE__
#include <os/log.h>
#include <sys/xattr.h>
#endif

/// @file scm_sid_bump_allocator_helpers.cpp
/// @brief SCM / SID bump-allocator helpers
///
/// Closed cut from security.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// SCM / SID bump-allocator helpers
// =============================================================================



GuestAddr SecurityStubs::alloc_sid_memory(u32 size) {
    return alloc_local_memory(size);
}

GuestAddr SecurityStubs::alloc_local_memory(u32 size) {
    if (m_local_memory != nullptr) {
        return m_local_memory->global_alloc(0, size);
    }
    GuestAddr addr = SID_SCRATCH_BASE + m_sid_alloc_offset;
    m_sid_alloc_offset += (size + 3) & ~3u; // align to 4 bytes
    return addr;
}

void SecurityStubs::free_local_memory(GuestAddr address) {
    if (m_local_memory != nullptr) {
        (void)m_local_memory->global_free(address);
    }
}

u64 SecurityStubs::register_trace_provider() {
    std::lock_guard lock(m_trace_mutex);
    const u64 handle = m_next_trace_handle++;
    m_trace_handles.insert(handle);
    return handle;
}

bool SecurityStubs::unregister_trace_provider(u64 handle) {
    std::lock_guard lock(m_trace_mutex);
    return m_trace_handles.erase(handle) != 0;
}

u64 SecurityStubs::open_trace_consumer() {
    std::lock_guard lock(m_trace_mutex);
    const u64 handle = m_next_trace_consumer++;
    m_trace_consumers.insert(handle);
    return handle;
}

bool SecurityStubs::close_trace_consumer(u64 handle) {
    std::lock_guard lock(m_trace_mutex);
    return m_trace_consumers.erase(handle) != 0;
}

bool SecurityStubs::is_trace_consumer(u64 handle) const {
    std::lock_guard lock(m_trace_mutex);
    return m_trace_consumers.contains(handle);
}

bool SecurityStubs::is_trace_provider_registered(u64 handle) const {
    std::lock_guard lock(m_trace_mutex);
    return m_trace_handles.contains(handle);
}

} // namespace vitreo::winapi
