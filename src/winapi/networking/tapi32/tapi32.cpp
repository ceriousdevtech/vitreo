#include "tapi32.hpp"

#include "winapi/core/marshaler.hpp"

#include <utility>
#include <vitreo/diagnostics.hpp>

namespace vitreo::winapi {
namespace {

// [MS-TRP] LINEERR_Constants
constexpr u32 LINEERR_INVALPOINTER = 0x80000035u;
constexpr u32 LINEERR_NODRIVER = 0x80000043u;
constexpr u32 LINEERR_UNINITIALIZED = 0x80000050u;

void return_lineerr(Marshaler& m, u32 err) {
    m.set_return(static_cast<u64>(err));
}

void fail_uninitialized(Marshaler& m, u32 argc) {
    for (u32 i = 0; i < argc; ++i) {
        (void)m.arg(i);
    }
    VITREO_TRACE("[tapi32] line* -> LINEERR_UNINITIALIZED\n");
    return_lineerr(m, LINEERR_UNINITIALIZED);
}

void impl_lineInitialize(Marshaler& m) {
    // LONG lineInitialize(LPHLINEAPP, HINSTANCE, LINECALLBACK, LPCSTR, LPDWORD)
    const GuestAddr app = m.arg_ptr(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    const GuestAddr num_devs = m.arg_ptr(4);
    if (app == 0 || num_devs == 0) {
        return_lineerr(m, LINEERR_INVALPOINTER);
        return;
    }
    // No telephony service provider is installed. Do not invent a line
    // device or a usage handle.
    VITREO_TRACE("[tapi32] lineInitialize -> LINEERR_NODRIVER\n");
    return_lineerr(m, LINEERR_NODRIVER);
}

} // namespace

DllExports create_tapi32_thunks() {
    DllExports exports;
    const auto add = [&](const char* name, u32 argc, auto handler) {
        exports.entries.push_back(ExportEntry{
            name, 0, std::move(handler), static_cast<u16>(argc * 4u)});
    };

    add("lineInitialize", 5, impl_lineInitialize);
    add("lineShutdown", 1, [](Marshaler& m) { fail_uninitialized(m, 1); });
    add("lineNegotiateAPIVersion", 6,
        [](Marshaler& m) { fail_uninitialized(m, 6); });
    add("lineGetDevCaps", 5, [](Marshaler& m) { fail_uninitialized(m, 5); });
    add("lineOpen", 9, [](Marshaler& m) { fail_uninitialized(m, 9); });
    add("lineClose", 1, [](Marshaler& m) { fail_uninitialized(m, 1); });
    add("lineGetID", 6, [](Marshaler& m) { fail_uninitialized(m, 6); });
    add("lineMakeCall", 5, [](Marshaler& m) { fail_uninitialized(m, 5); });
    add("lineAnswer", 3, [](Marshaler& m) { fail_uninitialized(m, 3); });
    add("lineDrop", 3, [](Marshaler& m) { fail_uninitialized(m, 3); });
    add("lineDeallocateCall", 1, [](Marshaler& m) { fail_uninitialized(m, 1); });
    add("lineGetCallStatus", 2, [](Marshaler& m) { fail_uninitialized(m, 2); });
    add("lineTranslateAddress", 7,
        [](Marshaler& m) { fail_uninitialized(m, 7); });
    return exports;
}

} // namespace vitreo::winapi
