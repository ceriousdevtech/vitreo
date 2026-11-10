// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file host_address_wait.hpp
/// @brief Futex-style wait/wake on a host address, on every supported macOS.
///
/// Vitreo's deployment target is macOS 13.0, but the public
/// os_sync_wait_on_address / os_sync_wake_by_address family exists only from
/// macOS 14.4. Below that the weak-linked symbols resolve to NULL and the
/// first call jumps to address 0 (first seen as a SIGSEGV in ReleaseMutex on
/// a macOS 13.7 Intel host).
///
/// These wrappers take os_sync's parameters and keep its result contract:
///   wait  >= 0 woken (or the value differed), -1 with errno EINTR, EFAULT,
///         EINVAL (bad size/address/flags, zero timeout) or ETIMEDOUT;
///   wake   0 woke someone, -1 with errno ENOENT when nobody was waiting.
/// On macOS 14.4+ they call os_sync directly. Earlier they use the kernel's
/// __ulock_wait2 / __ulock_wake (xnu bsd/sys/ulock.h, 64-bit and timeout-ns
/// forms since macOS 11), the primitive os_sync is a thin layer over and the
/// one libc++ std::atomic::wait uses on Apple platforms.

#include <os/clock.h>
#include <os/os_sync_wait_on_address.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

extern "C" {
int __ulock_wait2(uint32_t operation, void* addr, uint64_t value,
                  uint64_t timeout_ns, uint64_t value2);
int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);
}

namespace vitreo::platform {

namespace address_wait_detail {

// xnu bsd/sys/ulock.h
inline constexpr uint32_t kUlCompareAndWait         = 1;
inline constexpr uint32_t kUlCompareAndWaitShared   = 3;
inline constexpr uint32_t kUlCompareAndWait64       = 5;
inline constexpr uint32_t kUlCompareAndWait64Shared = 6;
inline constexpr uint32_t kUlfWakeAll               = 0x00000100;

// OS_SYNC_WAIT_ON_ADDRESS_SHARED / OS_SYNC_WAKE_BY_ADDRESS_SHARED (both 1 in
// the SDK). Spelled numerically: the named constants carry a 14.4
// availability attribute even though they are plain enum values.
inline constexpr uint32_t kSharedFlag               = 0x00000001;

/// The ulock operation for a word of @p size, or 0 for a size os_sync
/// rejects. Waiters and wakers must agree on it, exactly as os_sync requires
/// matching size and SHARED flags on both sides.
inline uint32_t operation(std::size_t size, bool shared) noexcept {
    if (size == 4) return shared ? kUlCompareAndWaitShared : kUlCompareAndWait;
    if (size == 8) return shared ? kUlCompareAndWait64Shared : kUlCompareAndWait64;
    return 0;
}

inline bool valid_address(const void* addr, std::size_t size) noexcept {
    return addr != nullptr &&
           (reinterpret_cast<uintptr_t>(addr) & (size - 1)) == 0;
}

inline int fail(int error) noexcept {
    errno = error;
    return -1;
}

inline int ulock_wait(void* addr, uint64_t value, std::size_t size,
                      os_sync_wait_on_address_flags_t flags,
                      uint64_t timeout_ns) noexcept {
    const uint32_t bits = static_cast<uint32_t>(flags);
    const bool shared = (bits & kSharedFlag) != 0;
    if ((bits & ~kSharedFlag) != 0) return fail(EINVAL);
    const uint32_t op = operation(size, shared);
    if (op == 0 || !valid_address(addr, size)) return fail(EINVAL);
    // A 4-byte wait compares only the low half, as os_sync documents.
    if (size == 4) value &= 0xffffffffull;
    return __ulock_wait2(op, addr, value, timeout_ns, 0);
}

inline int ulock_wake(void* addr, std::size_t size,
                      os_sync_wake_by_address_flags_t flags, bool all) noexcept {
    const uint32_t bits = static_cast<uint32_t>(flags);
    const bool shared = (bits & kSharedFlag) != 0;
    if ((bits & ~kSharedFlag) != 0) return fail(EINVAL);
    const uint32_t op = operation(size, shared);
    if (op == 0 || addr == nullptr) return fail(EINVAL);
    return __ulock_wake(op | (all ? kUlfWakeAll : 0u), addr, 0);
}

} // namespace address_wait_detail

/// os_sync_wait_on_address: block while *addr == value, with no timeout.
inline int address_wait(void* addr, uint64_t value, std::size_t size,
                        os_sync_wait_on_address_flags_t flags) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        return os_sync_wait_on_address(addr, value, size, flags);
    }
    // __ulock_wait2 treats a zero timeout as "forever".
    return address_wait_detail::ulock_wait(addr, value, size, flags, 0);
}

/// os_sync_wait_on_address_with_timeout: as address_wait, for at most
/// @p timeout_ns relative nanoseconds. Zero is EINVAL, as in os_sync.
inline int address_wait_with_timeout(void* addr, uint64_t value, std::size_t size,
                                     os_sync_wait_on_address_flags_t flags,
                                     os_clockid_t clockid,
                                     uint64_t timeout_ns) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        return os_sync_wait_on_address_with_timeout(addr, value, size, flags,
                                                    clockid, timeout_ns);
    }
    // The only clock os_sync defines is mach absolute time, whose relative
    // nanoseconds are what __ulock_wait2 takes.
    if (clockid != OS_CLOCK_MACH_ABSOLUTE_TIME || timeout_ns == 0) {
        return address_wait_detail::fail(EINVAL);
    }
    return address_wait_detail::ulock_wait(addr, value, size, flags, timeout_ns);
}

/// os_sync_wake_by_address_any: wake at most one waiter on @p addr.
inline int address_wake_any(void* addr, std::size_t size,
                            os_sync_wake_by_address_flags_t flags) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        return os_sync_wake_by_address_any(addr, size, flags);
    }
    return address_wait_detail::ulock_wake(addr, size, flags, false);
}

/// os_sync_wake_by_address_all: wake every waiter on @p addr.
inline int address_wake_all(void* addr, std::size_t size,
                            os_sync_wake_by_address_flags_t flags) noexcept {
    if (__builtin_available(macOS 14.4, *)) {
        return os_sync_wake_by_address_all(addr, size, flags);
    }
    return address_wait_detail::ulock_wake(addr, size, flags, true);
}

} // namespace vitreo::platform
