// SPDX-License-Identifier: LGPL-2.1-only
/// @file com_wait_run.cpp
/// @brief CoWaitForMultipleHandles and OleRun.
///
/// Learn (CoWaitForMultipleHandles, COWAIT_FLAGS, OleRun) plus winoracle
/// measurements, x86 and x64 identical (tools/oracle_probe/
/// cowait_olerun_probe.c, tests/oracle_data/cowait_olerun_probe*.expected):
///
/// CoWaitForMultipleHandles(dwFlags, dwTimeout, cHandles, pHandles, lpdwindex)
///   - lpdwindex NULL: E_INVALIDARG, nothing written. pHandles NULL or a flag
///     outside COWAIT_FLAGS (0x1F) is E_INVALIDARG and cHandles 0 is
///     RPC_E_NO_SYNC; these two write 0 to *lpdwindex.
///   - No apartment or the MTA: the wait is WaitForMultipleObjectsEx(bWaitAll =
///     COWAIT_WAITALL, bAlertable = COWAIT_ALERTABLE); the other flags have no
///     effect. *lpdwindex receives the wait result (index, WAIT_ABANDONED_0+i,
///     WAIT_IO_COMPLETION, WAIT_TIMEOUT or WAIT_FAILED). A signal is S_OK and a
///     timeout RPC_S_CALLPENDING; every other result returns
///     HRESULT_FROM_WIN32(GetLastError()) with the error the thread already had
///     (Learn: the result "can be nondeterministic" for abandoned mutexes and
///     APCs; call SetLastError(ERROR_SUCCESS) first).
///   - STA: COM's modal wait, MsgWaitForMultipleObjectsEx that delivers only
///     cross-thread sent messages; posted, thread and paint messages stay
///     queued for every flag combination measured, including
///     COWAIT_DISPATCH_WINDOW_MESSAGES | COWAIT_DISPATCH_CALLS (0x18).
///     COWAIT_DISPATCH_WINDOW_MESSAGES without COWAIT_DISPATCH_CALLS is
///     CO_E_NOT_SUPPORTED. A signalled handle, an abandoned mutex
///     (WAIT_ABANDONED_0+i) and an APC (WAIT_IO_COMPLETION) are S_OK with the
///     wait result in *lpdwindex; a timeout and a failed wait (an invalid
///     handle) are RPC_S_CALLPENDING with *lpdwindex left as it was.
///   - STA with COWAIT_INPUTAVAILABLE while posted input is queued: Windows did
///     not return at all (the measured wait outlived its timeout), contrary to
///     Learn's "return S_OK if input exists". Vitreo fails that one case
///     loudly instead of reproducing either.
///
/// OleRun(pUnknown)
///   - NULL: E_INVALIDARG. No IRunnableObject: S_OK. Otherwise the result of
///     IRunnableObject::Run(NULL), verbatim (S_FALSE and failures included),
///     with the interface released. COM need not be initialized.

#include "com_core.hpp"
#include "com_core_internal.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <chrono>
#include <initializer_list>
#include <vector>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi {

namespace {

constexpr u32 RPC_S_CALLPENDING = 0x80010115u;
constexpr u32 RPC_E_NO_SYNC = 0x80010120u;
constexpr u32 CO_E_NOT_SUPPORTED = 0x80004021u;
constexpr u32 E_NOTIMPL_HR = 0x80004001u;
constexpr u32 E_UNEXPECTED_HR = 0x8000FFFFu;

// combaseapi.h COWAIT_FLAGS.
constexpr u32 COWAIT_WAITALL = 0x1;
constexpr u32 COWAIT_ALERTABLE = 0x2;
constexpr u32 COWAIT_INPUTAVAILABLE = 0x4;
constexpr u32 COWAIT_DISPATCH_CALLS = 0x8;
constexpr u32 COWAIT_DISPATCH_WINDOW_MESSAGES = 0x10;
constexpr u32 COWAIT_ALL_FLAGS = 0x1F;

// winuser.h.
constexpr u32 MWMO_WAITALL = 0x1;
constexpr u32 MWMO_ALERTABLE = 0x2;
constexpr u32 QS_SENDMESSAGE = 0x40;
constexpr u32 PM_NOREMOVE = 0x0;
constexpr u32 PM_QS_SENDMESSAGE = QS_SENDMESSAGE << 16;

// winbase.h wait results.
constexpr u32 WAIT_OBJECT_0 = 0x0;
constexpr u32 WAIT_ABANDONED_0 = 0x80;
constexpr u32 WAIT_IO_COMPLETION = 0xC0;
constexpr u32 WAIT_TIMEOUT = 0x102;
constexpr u32 INFINITE = 0xFFFFFFFFu;

/// HRESULT_FROM_WIN32 (winerror.h).
[[nodiscard]] u32 hresult_from_win32(u32 error) {
    return static_cast<i32>(error) <= 0 ? error : ((error & 0xFFFFu) | 0x80070000u);
}

/// IRunnableObject, {00000126-0000-0000-C000-000000000046} (objidl.h).
constexpr Guid IID_IRUNNABLEOBJECT{
    0x00000126, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/// Per-thread guest scratch for out-parameters of the guest calls below:
/// [0,16) an IID, [16,24) an interface out-pointer, [32,80) an x64 MSG.
constexpr GuestAddr kScratchBytes = 96;
std::mutex g_scratch_mutex;
std::unordered_map<GuestAddr, GuestAddr> g_scratch_by_teb;

} // namespace

GuestAddr ComCoreManager::wait_run_scratch(Marshaler& m) {
    const GuestAddr teb = guest_teb(m);
    std::lock_guard lock(g_scratch_mutex);
    auto it = g_scratch_by_teb.find(teb);
    if (it != g_scratch_by_teb.end()) return it->second;
    const GuestAddr block = reserve_com_scratch(kScratchBytes);
    if (block != 0) g_scratch_by_teb.emplace(teb, block);
    return block;
}

u32 ComCoreManager::call_export(Marshaler& m, const char* dll, const char* name,
                                std::initializer_list<u64> args, bool& ok) {
    ok = false;
    if (m_thunks == nullptr || !m_guest_call) return 0;
    const auto address = m_thunks->get_thunk_address(dll, name);
    if (address.is_err()) return 0;
    std::vector<u64> values(args);
    ok = true;
    return static_cast<u32>(
        m_guest_call(address.value(), values.data(), values.size(), m.cpu_is_64bit()));
}

void ComCoreManager::impl_CoWaitForMultipleHandles(Marshaler& m) {
    const u32 flags = static_cast<u32>(m.arg32(0));
    const u32 timeout = static_cast<u32>(m.arg32(1));
    const u32 count = static_cast<u32>(m.arg32(2));
    const GuestAddr handles = m.arg_ptr(3);
    const GuestAddr index_out = m.arg_ptr(4);

    if (index_out == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    // Only these two failures clear the index; every other outcome writes
    // the wait result or leaves it as the caller had it.
    if (handles == 0 || (flags & ~COWAIT_ALL_FLAGS) != 0) {
        (void)m.write_u32(index_out, 0);
        m.set_return(com::E_INVALIDARG);
        return;
    }
    if (count == 0) {
        (void)m.write_u32(index_out, 0);
        m.set_return(RPC_E_NO_SYNC);
        return;
    }

    bool sta = false;
    {
        const GuestAddr teb = guest_teb(m);
        std::lock_guard lock(m_com_mutex);
        auto apt = m_apartments.find(teb);
        sta = apt != m_apartments.end() && apt->second.init_count != 0 && apt->second.sta;
    }

    bool ok = false;
    if (!sta) {
        const u32 result = call_export(
            m, "kernel32.dll", "WaitForMultipleObjectsEx",
            {count, handles, (flags & COWAIT_WAITALL) ? 1u : 0u, timeout,
             (flags & COWAIT_ALERTABLE) ? 1u : 0u},
            ok);
        if (!ok) {
            m.set_return(E_UNEXPECTED_HR);
            return;
        }
        (void)m.write_u32(index_out, result);
        if (result < WAIT_OBJECT_0 + count) {
            m.set_return(com::S_OK);
        } else if (result == WAIT_TIMEOUT) {
            m.set_return(RPC_S_CALLPENDING);
        } else {
            m.set_return(hresult_from_win32(get_last_error()));
        }
        return;
    }

    if ((flags & COWAIT_DISPATCH_WINDOW_MESSAGES) != 0 &&
        (flags & COWAIT_DISPATCH_CALLS) == 0) {
        m.set_return(CO_E_NOT_SUPPORTED);
        return;
    }
    const GuestAddr scratch = wait_run_scratch(m);
    if (scratch == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    const GuestAddr msg = scratch + 32;
    if ((flags & COWAIT_INPUTAVAILABLE) != 0) {
        const u32 queued = call_export(m, "user32.dll", "PeekMessageW",
                                       {msg, 0u, 0u, 0u, PM_NOREMOVE}, ok);
        if (ok && queued != 0) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED ole32!CoWaitForMultipleHandles: "
                             "COWAIT_INPUTAVAILABLE in an STA with input queued\n");
            });
            m.set_return(E_NOTIMPL_HR);
            return;
        }
    }

    const u32 mwmo = ((flags & COWAIT_WAITALL) ? MWMO_WAITALL : 0u) |
                     ((flags & COWAIT_ALERTABLE) ? MWMO_ALERTABLE : 0u);
    const bool infinite = timeout == INFINITE;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(infinite ? 0 : timeout);
    for (;;) {
        u32 left = INFINITE;
        if (!infinite) {
            const auto now = std::chrono::steady_clock::now();
            left = now >= deadline
                       ? 0u
                       : static_cast<u32>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              deadline - now)
                                              .count());
        }
        const u32 result = call_export(m, "user32.dll", "MsgWaitForMultipleObjectsEx",
                                       {count, handles, left, QS_SENDMESSAGE, mwmo}, ok);
        if (!ok) {
            m.set_return(E_UNEXPECTED_HR);
            return;
        }
        if (result == WAIT_OBJECT_0 + count) {
            // A cross-thread sent message: retrieving with PM_QS_SENDMESSAGE
            // delivers it to its window procedure and removes nothing else.
            (void)call_export(m, "user32.dll", "PeekMessageW",
                              {msg, 0u, 0u, 0u, PM_NOREMOVE | PM_QS_SENDMESSAGE}, ok);
            if (!infinite && left == 0) {
                m.set_return(RPC_S_CALLPENDING);
                return;
            }
            continue;
        }
        if (result < WAIT_OBJECT_0 + count ||
            (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + count) ||
            result == WAIT_IO_COMPLETION) {
            (void)m.write_u32(index_out, result);
            m.set_return(com::S_OK);
            return;
        }
        // WAIT_TIMEOUT, or WAIT_FAILED (an invalid handle, last error set by
        // the wait): RPC_S_CALLPENDING with the caller's index untouched.
        m.set_return(RPC_S_CALLPENDING);
        return;
    }
}

void ComCoreManager::impl_OleRun(Marshaler& m) {
    const GuestAddr unknown = m.arg_ptr(0);
    if (unknown == 0) {
        m.set_return(com::E_INVALIDARG);
        return;
    }
    const GuestAddr scratch = wait_run_scratch(m);
    if (scratch == 0 || !m_guest_call) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    const GuestAddr iid = scratch;
    const GuestAddr out = scratch + 16;
    if (m.write_struct(iid, IID_IRUNNABLEOBJECT).is_err() || m.write_ptr(out, 0).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    if (guest_query_interface(m, unknown, iid, out) != com::S_OK) {
        m.set_return(com::S_OK);
        return;
    }
    const GuestAddr runnable = static_cast<GuestAddr>(m.read_ptr(out).value_or(0));
    if (runnable == 0) {
        m.set_return(com::S_OK);
        return;
    }
    // IRunnableObject: GetRunningClass 3, Run 4 (objidl.h).
    const GuestAddr pointer = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr vtbl = static_cast<GuestAddr>(m.read_ptr(runnable).value_or(0));
    const GuestAddr run = vtbl == 0 ? 0 : static_cast<GuestAddr>(
                                              m.read_ptr(vtbl + 4 * pointer).value_or(0));
    u32 hr = E_UNEXPECTED_HR;
    if (run != 0) {
        const u64 args[2] = {runnable, 0};
        hr = static_cast<u32>(m_guest_call(run, args, 2, m.cpu_is_64bit()));
    }
    guest_iunknown_release(m, runnable);
    m.set_return(hr);
}

} // namespace vitreo::winapi
