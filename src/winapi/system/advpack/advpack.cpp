// SPDX-License-Identifier: LGPL-2.1-only
/// @file advpack.cpp
/// @brief advpack.dll exports. See advpack.hpp for scope.

#include "winapi/system/advpack/advpack.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

DllExports create_advpack_thunks(const GuestElevationState* elevation) {
    DllExports exports;

    // BOOL WINAPI IsNTAdmin(DWORD dwReserved, DWORD* lpdwReserved)
    //
    // advpub.h (Windows SDK). Both parameters are reserved and unused. TRUE
    // when the calling process's token holds the Administrators group
    // enabled, i.e. the process is elevated; a UAC-filtered token is not an
    // administrator. Measured (tests/winapi/advpack/oracle/isnt_probe.c):
    // same answer as shell32!IsUserAnAdmin, the last error and
    // *lpdwReserved are left untouched. 2 args, stdcall.
    exports.entries.push_back(ExportEntry{
        "IsNTAdmin", 43,
        [elevation](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            const bool admin = elevation != nullptr && elevation->is_elevated();
            VITREO_TRACE("[advpack] IsNTAdmin -> %s\n", admin ? "TRUE" : "FALSE");
            m.set_return(admin ? 1u : 0u);
        },
        8});

    return exports;
}

} // namespace vitreo::winapi
