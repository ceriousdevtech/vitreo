/// @file uiautomationcore.cpp
/// @brief UIAutomationCore.dll implementation.

#include "uiautomationcore.hpp"
#include "winapi/core/marshaler.hpp"

#include <atomic>
#include <cstdio>

namespace vitreo::winapi {

namespace {
constexpr u32 S_OK_HR      = 0x00000000;
constexpr u32 E_NOTIMPL_HR = 0x80004001;
} // namespace

DllExports create_uiautomationcore_thunks() {
    DllExports exports;

    // BOOL UiaClientsAreListening(void) — 0 args, stdcall_bytes=0.
    //
    // The gate the provider checks before doing any automation work. Nothing
    // is attached under Vitreo, so FALSE is both honest and the answer that
    // lets a well-behaved app skip the rest of its automation path entirely.
    exports.entries.push_back({"UiaClientsAreListening", 0,
        [](Marshaler& m) {
            m.set_return(0);
        }, 0});

    // HRESULT UiaRaiseAutomationEvent(IRawElementProviderSimple *pProvider,
    //                                 EVENTID id)
    // 2 args, stdcall_bytes=8.
    //
    // With no listening clients this is a genuine no-op on Windows too, and it
    // returns S_OK — there is no event to deliver and nothing failed. Reporting
    // an error here would be the fake answer.
    exports.entries.push_back({"UiaRaiseAutomationEvent", 0,
        [](Marshaler& m) {
            const GuestAddr provider = m.arg_ptr(0);
            const u32 event_id = static_cast<u32>(m.arg(1));
            if (provider == 0) {
                // Measured (s1win_dwm_probe "-- uia"): E_INVALIDARG.
                m.set_return(0x80070057u);
                return;
            }
            VITREO_TRACE("[uiautomation] UiaRaiseAutomationEvent(provider=0x%llx"
                         " id=%u) -> S_OK (no clients listening)\n",
                         static_cast<unsigned long long>(provider), event_id);
            m.set_return(S_OK_HR);
        }, 8});

    // HRESULT UiaRaiseAutomationPropertyChangedEvent(
    //     IRawElementProviderSimple *pProvider, PROPERTYID id,
    //     VARIANT oldValue, VARIANT newValue)
    // VARIANT is passed *by value* and is 16 bytes on x86, so the stack
    // footprint is 4 + 4 + 16 + 16 = 40 bytes, not 16. Getting this wrong is
    // precisely the leak that corrupted the caller's frame.
    exports.entries.push_back({"UiaRaiseAutomationPropertyChangedEvent", 0,
        [](Marshaler& m) {
            const GuestAddr provider = m.arg_ptr(0);
            const u32 prop_id = static_cast<u32>(m.arg(1));
            VITREO_TRACE("[uiautomation] UiaRaiseAutomationPropertyChangedEvent("
                         "provider=0x%llx prop=%u) -> S_OK (no clients"
                         " listening)\n",
                         static_cast<unsigned long long>(provider), prop_id);
            m.set_return(S_OK_HR);
        }, 40});

    // HRESULT UiaRaiseNotificationEvent(IRawElementProviderSimple* provider,
    //     NotificationKind, NotificationProcessing, BSTR displayString,
    //     BSTR activityId)          (UIAutomationCoreApi.h, SDK 10.0.26100)
    // 5 args, stdcall_bytes=20.
    //
    // Measured on Windows 11 (tools/oracle_probe/gfx_media_exports_probe
    // .expected): a NULL provider is E_INVALIDARG; any other call is S_OK --
    // including out-of-range kinds and NULL strings -- and the provider is
    // never called back. With no client listening under Vitreo there is no
    // one to deliver the notification to, so S_OK is the whole contract.
    exports.entries.push_back({"UiaRaiseNotificationEvent", 0,
        [](Marshaler& m) {
            const GuestAddr provider = m.arg_ptr(0);
            if (provider == 0) {
                m.set_return(0x80070057u);  // E_INVALIDARG
                return;
            }
            VITREO_TRACE("[uiautomation] UiaRaiseNotificationEvent(provider=0x%llx"
                         " kind=%u processing=%u) -> S_OK (no clients listening)\n",
                         static_cast<unsigned long long>(provider), m.arg32(1),
                         m.arg32(2));
            m.set_return(S_OK_HR);
        }, 20});

    // HRESULT UiaHostProviderFromHwnd(HWND hwnd,
    //                                 IRawElementProviderSimple **ppProvider)
    // 2 args, stdcall_bytes=8.
    //
    // Vitreo implements no automation provider, so there is no object to hand
    // back. Fail honestly and NULL the out-parameter rather than returning
    // S_OK with a pointer the caller would then dereference.
    exports.entries.push_back({"UiaHostProviderFromHwnd", 0,
        [](Marshaler& m) {
            // Measured (s1win_dwm_probe "-- uia"): NULL ppProvider is
            // E_INVALIDARG; a real window gets a host provider (S_OK). The
            // provider object (IRawElementProviderSimple over the HWND's
            // MSAA/Win32 facts) is not implemented.
            const auto hwnd = static_cast<u32>(m.arg(0));
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) {
                m.set_return(0x80070057u);
                return;
            }
            (void)m.write_ptr(out, 0);
            static std::atomic_flag reported = ATOMIC_FLAG_INIT;
            if (!reported.test_and_set()) {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED uiautomationcore!UiaHostProviderFromHwnd: "
                             "no host provider object\n");
            }
            VITREO_TRACE("[uiautomation] UiaHostProviderFromHwnd(hwnd=0x%x)"
                         " -> E_NOTIMPL (no provider)\n", hwnd);
            m.set_return(E_NOTIMPL_HR);
        }, 8});

    // LRESULT UiaReturnRawElementProvider(HWND hwnd, WPARAM wParam,
    //     LPARAM lParam, IRawElementProviderSimple *el)
    // 4 args, stdcall_bytes=16.
    //
    // The provider's WM_GETOBJECT reply. Returning 0 is the documented "I am
    // not supplying an object" answer, which is the truth here.
    exports.entries.push_back({"UiaReturnRawElementProvider", 0,
        [](Marshaler& m) {
            const auto hwnd = static_cast<u32>(m.arg(0));
            VITREO_TRACE("[uiautomation] UiaReturnRawElementProvider(hwnd=0x%x)"
                         " -> 0 (no provider)\n", hwnd);
            m.set_return(0);
        }, 16});

    return exports;
}

} // namespace vitreo::winapi
