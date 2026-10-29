#include "riched20.hpp"
#include "winapi/core/marshaler.hpp"

#include <cstdio>

namespace vitreo::winapi {

namespace {

// HRESULT DllGetVersion(DLLVERSIONINFO*)
// Installers sometimes probe this after LoadLibrary. Report a Win2k-era
// riched20 so version checks accept the module.
void stub_DllGetVersion(Marshaler& m) {
    GuestAddr info = m.arg_ptr(0);
    if (info == 0) {
        m.set_return(static_cast<u64>(0x80004003ull)); // E_POINTER
        return;
    }
    auto cb_res = m.read_u32(info);
    const u32 cb = cb_res.is_ok() ? cb_res.value() : 0;
    if (cb < 20) {
        m.set_return(static_cast<u64>(0x80070057ull)); // E_INVALIDARG
        return;
    }
    // DLLVERSIONINFO: cbSize, dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformID
    (void)m.write_u32(info + 4, 5);   // major (riched20 5.x)
    (void)m.write_u32(info + 8, 30);  // minor
    (void)m.write_u32(info + 12, 0);  // build
    (void)m.write_u32(info + 16, 2);  // DLLVER_PLATFORM_WINDOWS
    VITREO_TRACE("[riched20] DllGetVersion -> S_OK (5.30)\n");
    m.set_return(0); // S_OK
}

} // namespace

DllExports create_riched20_thunks() {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "DllGetVersion", 0,
        [](Marshaler& m) { stub_DllGetVersion(m); },
        4
    });
    return exports;
}

} // namespace vitreo::winapi
