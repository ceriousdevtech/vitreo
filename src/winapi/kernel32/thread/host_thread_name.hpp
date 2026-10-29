// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file host_thread_name.hpp
/// @brief Naming the calling host thread after a guest thread description.
///
/// A guest thread runs on a host thread, and `sample`, xctrace and Instruments
/// label host threads by name. Every way a guest names a thread
/// (SetThreadDescription, the MSVC 0x406D1388 THREADNAME_INFO exception) funnels
/// through here so the UTF-16 -> UTF-8 conversion and the macOS length limit
/// exist once.

#include <vitreo/common.hpp>

#include <string>
#include <string_view>

namespace vitreo::winapi {

/// Longest host thread name macOS keeps, excluding the terminating NUL:
/// MAXTHREADNAMESIZE (64, <mach/thread_info.h>) - 1, the same bound as
/// PROC_SELFSET_THREADNAME_SIZE in <sys/proc_info.h>.
inline constexpr usize HOST_THREAD_NAME_MAX_BYTES = 63;

/// The host thread name for a guest description: UTF-8, ending at the first
/// NUL, cut on a code-point boundary to HOST_THREAD_NAME_MAX_BYTES so a
/// multi-byte character is dropped whole rather than split into invalid UTF-8.
/// An unpaired surrogate (legal in a Windows description, not in UTF-8)
/// becomes U+FFFD.
[[nodiscard]] std::string host_thread_name_from_utf16(
    std::u16string_view description);

/// Name the CALLING host thread. macOS can name only the calling thread
/// (pthread_setname_np takes no thread), so a caller must have established
/// that the guest thread being named is the one it is running on. An empty
/// description clears the name. Returns 0 or the pthread_setname_np errno.
int name_calling_host_thread(std::u16string_view description);

} // namespace vitreo::winapi
