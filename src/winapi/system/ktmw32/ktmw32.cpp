// SPDX-License-Identifier: LGPL-2.1-only
/// @file ktmw32.cpp
/// @brief KtmW32.dll Kernel Transaction Manager stubs.
///
/// Vitreo does not implement KTM. CreateTransaction returns
/// INVALID_HANDLE_VALUE with ERROR_RM_NOT_ACTIVE so well-behaved callers
/// (a bootstrap updater, MSI's optional KTM path, and similar callers) detect the
/// absence of a transaction manager and fall through to their
/// non-transactional code paths.
///
/// Why not return a fake success handle and passthrough the *Transacted
/// file APIs? Returning a success handle steered the caller into the path
/// that consumes a real transaction handle. That path is not exercised on
/// Windows when KTM is unavailable, and it depends on additional KTM
/// behaviour this DLL does not implement. The caller then faults. Return an
/// explicit failure instead.

#include "ktmw32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {

namespace {

// Win32 INVALID_HANDLE_VALUE (-1) \u2014 sign-extended to 64 bits.
constexpr u64 INVALID_HANDLE_VALUE_U = 0xFFFFFFFFFFFFFFFFULL;
// ERROR_RM_NOT_ACTIVE \u2014 "The resource manager is not available."
constexpr u32 ERROR_RM_NOT_ACTIVE = 6700;

} // namespace

DllExports create_ktmw32_thunks() {
    DllExports exports;
    exports.entries.reserve(4);

    // HANDLE CreateTransaction(LPSECURITY_ATTRIBUTES, LPGUID UOW,
    //                          DWORD CreateOptions, DWORD IsolationLevel,
    //                          DWORD IsolationFlags, DWORD Timeout,
    //                          LPWSTR Description)
    // 7 args, stdcall_bytes=28
    exports.entries.push_back(ExportEntry{
        "CreateTransaction", 0,
        [](Marshaler& m) {
            for (u32 i = 0; i < 7; ++i) (void)m.arg(i);
            VITREO_TRACE("[ktmw32] CreateTransaction -> INVALID_HANDLE_VALUE "
                         "(KTM not implemented; caller should fall back)\n");
            set_last_error(ERROR_RM_NOT_ACTIVE);
            m.set_return(INVALID_HANDLE_VALUE_U);
        },
        28
    });

    // BOOL CommitTransaction(HANDLE TransactionHandle)
    // 1 arg, stdcall_bytes=4 \u2014 fail; we never hand out real handles.
    exports.entries.push_back(ExportEntry{
        "CommitTransaction", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            VITREO_TRACE("[ktmw32] CommitTransaction -> FALSE (no KTM)\n");
            set_last_error(ERROR_RM_NOT_ACTIVE);
            m.set_return(0);
        },
        4
    });

    // BOOL RollbackTransaction(HANDLE TransactionHandle)
    // 1 arg, stdcall_bytes=4 \u2014 fail.
    exports.entries.push_back(ExportEntry{
        "RollbackTransaction", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            VITREO_TRACE("[ktmw32] RollbackTransaction -> FALSE (no KTM)\n");
            set_last_error(ERROR_RM_NOT_ACTIVE);
            m.set_return(0);
        },
        4
    });

    // BOOL GetTransactionInformation(HANDLE, PDWORD Outcome, PDWORD,
    //                                 PDWORD, PDWORD, DWORD, LPWSTR)
    // 7 args, stdcall_bytes=28 \u2014 fail.
    exports.entries.push_back(ExportEntry{
        "GetTransactionInformation", 0,
        [](Marshaler& m) {
            for (u32 i = 0; i < 7; ++i) (void)m.arg(i);
            VITREO_TRACE("[ktmw32] GetTransactionInformation -> FALSE (no KTM)\n");
            set_last_error(ERROR_RM_NOT_ACTIVE);
            m.set_return(0);
        },
        28
    });

    return exports;
}

} // namespace vitreo::winapi
