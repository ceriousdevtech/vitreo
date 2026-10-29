// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file autorelease_scope.hpp
/// @brief An Objective-C autorelease pool for one C++ scope.
///
/// A guest thread is a host thread Vitreo created, and nothing on it ever
/// pushes an autorelease pool. Apple's contract ("Using Autorelease Pool
/// Blocks", Advanced Memory Management Programming Guide): a thread that is
/// not the main thread must create its own pool, and a long-lived one must
/// drain it periodically, or every autoreleased object it touches lives until
/// the thread exits. Metal autoreleases on its allocation and descriptor
/// paths, so anything that reaches Metal, AppKit or Foundation on a guest
/// thread must run inside one of these. Every guest->host API crossing opens
/// one around its handler (runtime/execution: dispatch_slow_api_thunk,
/// run_direct_thunk, the native-service and native32 dispatchers), so an
/// export body needs none of its own; host-side entry points that a non-guest
/// thread can reach (DxgiManager::ApiLock) still open their own.
///
/// The scope is the C equivalent of `@autoreleasepool { }` (the same runtime
/// entry points clang emits for it), so plain C++ entry points can open one.
/// Scopes nest; each releases exactly what was autoreleased inside it.

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* context);

namespace vitreo::graphics {

class AutoreleaseScope {
public:
    AutoreleaseScope() noexcept : m_context(objc_autoreleasePoolPush()) {}
    ~AutoreleaseScope() { objc_autoreleasePoolPop(m_context); }
    AutoreleaseScope(const AutoreleaseScope&) = delete;
    AutoreleaseScope& operator=(const AutoreleaseScope&) = delete;

private:
    void* m_context;
};

}  // namespace vitreo::graphics
